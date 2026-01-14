#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
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

#define MEM_MONITOR_INTERVAL_MS 60000
#define MEM_MONITOR_STACK_SIZE  2048
#define WATCHDOG_TIMEOUT_MS     30000

static void memory_monitor_task(void *arg)
{
    (void)arg;

    while (1) {
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t min_heap = esp_get_minimum_free_heap_size();
        uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

        ESP_LOGI(TAG, "Free heap: %lu, min: %lu, internal: %lu",
                 (unsigned long)free_heap, (unsigned long)min_heap,
                 (unsigned long)free_internal);

        if (free_heap < 50000) {
            ESP_LOGW(TAG, "Low memory warning: %lu bytes free", (unsigned long)free_heap);
        }

        vTaskDelay(pdMS_TO_TICKS(MEM_MONITOR_INTERVAL_MS));
    }
}

static void init_watchdog(void)
{
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_init(&wdt_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Watchdog initialized (%d ms timeout)", WATCHDOG_TIMEOUT_MS);
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "Watchdog already initialized");
    } else {
        ESP_LOGW(TAG, "Failed to init watchdog: %s", esp_err_to_name(err));
    }
}

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

static void cleanup_relay_resources(bool cleanup_rate_limiter, bool cleanup_storage, bool cleanup_sub_manager)
{
    if (cleanup_rate_limiter && g_relay_ctx.rate_limiter) {
        rate_limiter_destroy(&g_rate_limiter);
        g_relay_ctx.rate_limiter = NULL;
    }
    if (cleanup_storage && g_relay_ctx.storage) {
        storage_destroy(&g_storage);
        g_relay_ctx.storage = NULL;
    }
    if (cleanup_sub_manager && g_relay_ctx.sub_manager) {
        sub_manager_destroy(&g_sub_manager);
        g_relay_ctx.sub_manager = NULL;
    }
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
        cleanup_relay_resources(false, false, true);
        return;
    }
    g_relay_ctx.storage = &g_storage;

    if (storage_start_cleanup_task(&g_storage) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start storage cleanup task");
        cleanup_relay_resources(false, true, true);
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
        cleanup_relay_resources(true, true, true);
        return;
    }
    ws_server_set_disconnect_cb(on_ws_disconnect);

    ESP_LOGI(TAG, "Relay listening on ws://" IPSTR ":%d",
             IP2STR(&event->ip_info.ip), g_relay_ctx.config.port);
}

static void init_sntp(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    int retry = 0;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 15) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (retry < 15) {
        ESP_LOGI(TAG, "NTP synced (epoch: %lu)", (unsigned long)time(NULL));
    } else {
        ESP_LOGW(TAG, "NTP sync timeout");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        init_sntp();
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

    init_watchdog();

    nostr_init();

    TaskHandle_t mem_mon_handle = NULL;
    BaseType_t task_ret = xTaskCreate(memory_monitor_task, "mem_mon", MEM_MONITOR_STACK_SIZE, NULL, 1, &mem_mon_handle);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create mem_mon task (stack=%d)", MEM_MONITOR_STACK_SIZE);
    }

    wifi_init_sta();
}
