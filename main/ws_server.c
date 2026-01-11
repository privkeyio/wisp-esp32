#include "ws_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *TAG = "ws_server";
static ws_message_cb_t g_message_callback = NULL;
static ws_server_t *g_server = NULL;

static const char *NIP11_INFO =
    "{"
    "\"name\":\"ESP32 Ephemeral Relay\","
    "\"description\":\"Minimal Nostr relay with 21-day TTL\","
    "\"supported_nips\":[1,11,40],"
    "\"software\":\"wisp-esp32\","
    "\"version\":\"0.1.0\","
    "\"limitation\":{"
        "\"max_message_length\":65536,"
        "\"max_subscriptions\":8,"
        "\"max_filters\":4,"
        "\"max_event_tags\":100,"
        "\"auth_required\":false,"
        "\"payment_required\":false"
    "},"
    "\"retention\":[{\"kinds\":[0,1,2,3,4,5,6,7],\"time\":1814400}]"
    "}";

static ws_connection_t* find_free_slot(ws_server_t *server)
{
    for (int i = 0; i < WS_MAX_CONNECTIONS; i++) {
        if (!server->connections[i].active) {
            return &server->connections[i];
        }
    }
    return NULL;
}

static ws_connection_t* find_connection_by_fd(ws_server_t *server, int fd)
{
    for (int i = 0; i < WS_MAX_CONNECTIONS; i++) {
        if (server->connections[i].active && server->connections[i].fd == fd) {
            return &server->connections[i];
        }
    }
    return NULL;
}

ws_connection_t* ws_server_get_connection(ws_server_t *server, int fd)
{
    xSemaphoreTake(server->lock, portMAX_DELAY);
    ws_connection_t *conn = find_connection_by_fd(server, fd);
    xSemaphoreGive(server->lock);
    return conn;
}

static void get_client_ip(int fd, char *ip_buf, size_t buf_len)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, ip_buf, buf_len);
    } else {
        strncpy(ip_buf, "unknown", buf_len - 1);
        ip_buf[buf_len - 1] = '\0';
    }
}

static esp_err_t on_open(httpd_handle_t hd, int sockfd)
{
    if (!g_server) return ESP_FAIL;

    xSemaphoreTake(g_server->lock, portMAX_DELAY);

    if (g_server->connection_count >= WS_MAX_CONNECTIONS) {
        xSemaphoreGive(g_server->lock);
        ESP_LOGW(TAG, "Connection rejected - max connections reached");
        return ESP_FAIL;
    }

    ws_connection_t *conn = find_free_slot(g_server);
    if (conn) {
        conn->fd = sockfd;
        conn->active = true;
        conn->connected_at = esp_timer_get_time() / 1000000;
        conn->last_activity = conn->connected_at;
        get_client_ip(sockfd, conn->remote_ip, sizeof(conn->remote_ip));
        conn->events_this_minute = 0;
        conn->reqs_this_minute = 0;
        conn->rate_window_start = conn->connected_at;
        g_server->connection_count++;
        ESP_LOGI(TAG, "New connection from %s (fd=%d, total=%d)",
                 conn->remote_ip, sockfd, g_server->connection_count);
    }

    xSemaphoreGive(g_server->lock);
    return ESP_OK;
}

static void on_close(httpd_handle_t hd, int sockfd)
{
    if (!g_server) return;

    xSemaphoreTake(g_server->lock, portMAX_DELAY);

    ws_connection_t *conn = find_connection_by_fd(g_server, sockfd);
    if (conn) {
        ESP_LOGI(TAG, "Connection closed (fd=%d, ip=%s)", sockfd, conn->remote_ip);
        memset(conn, 0, sizeof(ws_connection_t));
        g_server->connection_count--;
    }

    xSemaphoreGive(g_server->lock);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        char upgrade[16] = {0};
        if (httpd_req_get_hdr_value_str(req, "Upgrade", upgrade, sizeof(upgrade)) != ESP_OK ||
            strcasecmp(upgrade, "websocket") != 0) {
            char accept[64] = {0};
            if (httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept)) == ESP_OK &&
                strstr(accept, "application/nostr+json")) {
                httpd_resp_set_type(req, "application/nostr+json");
            } else {
                httpd_resp_set_type(req, "application/json");
            }
            return httpd_resp_send(req, NIP11_INFO, strlen(NIP11_INFO));
        }
        ESP_LOGD(TAG, "WebSocket handshake completed");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get frame len: %d", ret);
        return ret;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK;
    }

    if (ws_pkt.len > WS_MAX_FRAME_SIZE) {
        ESP_LOGW(TAG, "Frame too large: %d bytes", ws_pkt.len);
        return ESP_FAIL;
    }

    ws_pkt.payload = malloc(ws_pkt.len + 1);
    if (!ws_pkt.payload) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes", ws_pkt.len);
        return ESP_ERR_NO_MEM;
    }

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to receive frame: %d", ret);
        free(ws_pkt.payload);
        return ret;
    }

    ((char *)ws_pkt.payload)[ws_pkt.len] = '\0';

    int fd = httpd_req_to_sockfd(req);
    ws_connection_t *conn = ws_server_get_connection(g_server, fd);
    if (conn) {
        conn->last_activity = esp_timer_get_time() / 1000000;
    }

    switch (ws_pkt.type) {
        case HTTPD_WS_TYPE_TEXT:
            ESP_LOGD(TAG, "Received %d bytes from fd=%d", ws_pkt.len, fd);
            if (g_message_callback) {
                g_message_callback(fd, (char *)ws_pkt.payload, ws_pkt.len);
            }
            break;

        case HTTPD_WS_TYPE_PING:
            ws_pkt.type = HTTPD_WS_TYPE_PONG;
            httpd_ws_send_frame(req, &ws_pkt);
            break;

        case HTTPD_WS_TYPE_CLOSE:
            break;

        default:
            break;
    }

    free(ws_pkt.payload);
    return ESP_OK;
}

typedef struct {
    httpd_handle_t hd;
    int fd;
    char *data;
    size_t len;
} async_send_arg_t;

static void ws_async_send(void *arg)
{
    async_send_arg_t *a = (async_send_arg_t *)arg;

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)a->data,
        .len = a->len,
    };

    esp_err_t ret = httpd_ws_send_frame_async(a->hd, a->fd, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Async send failed to fd=%d: %d", a->fd, ret);
    }

    free(a->data);
    free(a);
}

esp_err_t ws_server_init(ws_server_t *server, uint16_t port, ws_message_cb_t on_message)
{
    memset(server, 0, sizeof(ws_server_t));
    server->lock = xSemaphoreCreateMutex();
    if (!server->lock) {
        return ESP_ERR_NO_MEM;
    }

    g_server = server;
    g_message_callback = on_message;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = port + 1;
    config.max_open_sockets = WS_MAX_CONNECTIONS;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.open_fn = on_open;
    config.close_fn = on_close;

    esp_err_t ret = httpd_start(&server->server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %d", ret);
        vSemaphoreDelete(server->lock);
        return ret;
    }

    httpd_uri_t ws_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };

    ret = httpd_register_uri_handler(server->server, &ws_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WS handler: %d", ret);
        httpd_stop(server->server);
        vSemaphoreDelete(server->lock);
        return ret;
    }

    ESP_LOGI(TAG, "WebSocket server started on port %d", port);
    return ESP_OK;
}

void ws_server_stop(ws_server_t *server)
{
    if (server->server) {
        httpd_stop(server->server);
        server->server = NULL;
    }
    if (server->lock) {
        vSemaphoreDelete(server->lock);
        server->lock = NULL;
    }
    g_server = NULL;
    g_message_callback = NULL;
}

esp_err_t ws_server_send(ws_server_t *server, int fd, const char *data, size_t len)
{
    if (!server->server) return ESP_ERR_INVALID_STATE;

    async_send_arg_t *arg = malloc(sizeof(async_send_arg_t));
    if (!arg) return ESP_ERR_NO_MEM;

    arg->hd = server->server;
    arg->fd = fd;
    arg->data = malloc(len);
    if (!arg->data) {
        free(arg);
        return ESP_ERR_NO_MEM;
    }
    memcpy(arg->data, data, len);
    arg->len = len;

    return httpd_queue_work(server->server, ws_async_send, arg);
}

esp_err_t ws_server_broadcast(ws_server_t *server, const char *data, size_t len)
{
    xSemaphoreTake(server->lock, portMAX_DELAY);

    for (int i = 0; i < WS_MAX_CONNECTIONS; i++) {
        if (server->connections[i].active) {
            ws_server_send(server, server->connections[i].fd, data, len);
        }
    }

    xSemaphoreGive(server->lock);
    return ESP_OK;
}

void ws_server_close_connection(ws_server_t *server, int fd)
{
    httpd_sess_trigger_close(server->server, fd);
}
