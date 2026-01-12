#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "nostr.h"
#include "rate_limiter.h"
#include "relay_core.h"
#include "router.h"
#include "storage_engine.h"
#include "sub_manager.h"
#include "ws_server.h"

static const char *TAG = "wisp";
static relay_ctx_t g_relay_ctx;
static sub_manager_t g_sub_manager;
static storage_engine_t g_storage;
static rate_limiter_t g_rate_limiter;

static void on_ws_disconnect(int fd)
{
    sub_manager_remove_all(&g_sub_manager, fd);
    rate_limiter_reset(&g_rate_limiter, fd);
}

static void on_ws_message(int fd, const char *data, size_t len)
{
    if (len == 0 || len > WS_MAX_FRAME_SIZE) {
        ESP_LOGW(TAG, "Invalid length fd=%d: %zu", fd, len);
        return;
    }

    router_msg_t msg;
    nostr_relay_error_t result = router_parse(data, len, &msg);
    if (result != NOSTR_RELAY_OK) {
        router_send_notice(&g_relay_ctx, fd, "error: failed to parse message");
        return;
    }

    router_dispatch(&g_relay_ctx, fd, &msg);
    router_msg_free(&msg);
}

static void start_relay_server(ip_event_got_ip_t *event)
{
    if (ws_server_is_running(&g_relay_ctx.ws_server)) {
        ESP_LOGI(TAG, "WebSocket server already running");
        return;
    }

    g_relay_ctx.config.port = 4869;
    g_relay_ctx.config.max_event_age_sec = 21 * 24 * 60 * 60;
    g_relay_ctx.config.max_subs_per_conn = 8;
    g_relay_ctx.config.max_filters_per_sub = 4;
    g_relay_ctx.config.max_future_sec = 900;

    if (sub_manager_init(&g_sub_manager) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init subscription manager");
        return;
    }
    g_relay_ctx.sub_manager = &g_sub_manager;

    uint32_t default_ttl_sec = g_relay_ctx.config.max_event_age_sec;
    if (storage_init(&g_storage, default_ttl_sec) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init storage engine");
        sub_manager_destroy(&g_sub_manager);
        g_relay_ctx.sub_manager = NULL;
        return;
    }
    g_relay_ctx.storage = &g_storage;
    if (storage_start_cleanup_task(&g_storage) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start storage cleanup task");
        storage_destroy(&g_storage);
        g_relay_ctx.storage = NULL;
        sub_manager_destroy(&g_sub_manager);
        g_relay_ctx.sub_manager = NULL;
        return;
    }

    rate_config_t rate_cfg = {
        .events_per_minute = 30,
        .reqs_per_minute = 60,
    };
    rate_limiter_init(&g_rate_limiter, &rate_cfg);
    g_relay_ctx.rate_limiter = &g_rate_limiter;

    esp_err_t ret = ws_server_init(&g_relay_ctx.ws_server, g_relay_ctx.config.port, on_ws_message);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ws server: %s", esp_err_to_name(ret));
        storage_destroy(&g_storage);
        g_relay_ctx.storage = NULL;
        sub_manager_destroy(&g_sub_manager);
        g_relay_ctx.sub_manager = NULL;
        return;
    }
    ws_server_set_disconnect_cb(on_ws_disconnect);

    ESP_LOGI(TAG, "Relay listening on ws://" IPSTR ":%d",
             IP2STR(&event->ip_info.ip), g_relay_ctx.config.port);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START || event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
                ESP_LOGI(TAG, "Disconnected, reconnecting...");
            }
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_relay_server(event);
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WISP_WIFI_SSID,
            .password = CONFIG_WISP_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA initialized");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Wisp ESP32 Nostr Relay Starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nostr_init();

    wifi_init_sta();
}
