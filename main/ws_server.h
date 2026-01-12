#ifndef WS_SERVER_H
#define WS_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <arpa/inet.h>
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WS_MAX_CONNECTIONS     8
#define WS_MAX_FRAME_SIZE      65536

typedef struct {
    int fd;
    bool active;
    uint32_t connected_at;
    uint32_t last_activity;
    char remote_ip[INET6_ADDRSTRLEN];
    uint16_t events_this_minute;
    uint16_t reqs_this_minute;
    uint32_t rate_window_start;
} ws_connection_t;

typedef struct {
    httpd_handle_t server;
    ws_connection_t connections[WS_MAX_CONNECTIONS];
    SemaphoreHandle_t lock;
    uint8_t connection_count;
} ws_server_t;

typedef void (*ws_message_cb_t)(int fd, const char *data, size_t len);
typedef void (*ws_disconnect_cb_t)(int fd);

esp_err_t ws_server_init(ws_server_t *server, uint16_t port, ws_message_cb_t on_message);
void ws_server_set_disconnect_cb(ws_disconnect_cb_t cb);
void ws_server_stop(ws_server_t *server);
bool ws_server_is_running(ws_server_t *server);
esp_err_t ws_server_send(ws_server_t *server, int fd, const char *data, size_t len);
esp_err_t ws_server_broadcast(ws_server_t *server, const char *data, size_t len);
void ws_server_close_connection(ws_server_t *server, int fd);

#endif
