#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "ws_server.h"

static const char *TAG = "wisp";
static ws_server_t g_ws_server;

static bool is_valid_utf8_text(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
            return false;
        }
        if (c >= 0x80) {
            size_t seq_len = 0;
            if ((c & 0xE0) == 0xC0) seq_len = 2;
            else if ((c & 0xF0) == 0xE0) seq_len = 3;
            else if ((c & 0xF8) == 0xF0) seq_len = 4;
            else return false;
            if (i + seq_len > len) return false;
            for (size_t j = 1; j < seq_len; j++) {
                if ((data[i + j] & 0xC0) != 0x80) return false;
            }
            i += seq_len - 1;
        }
    }
    return true;
}

static void on_ws_message(int fd, const char *data, size_t len)
{
    const char *suffix = len > 128 ? "..." : "";
    ESP_LOGI(TAG, "Message from fd=%d (%zu bytes): %.128s%s", fd, len, data, suffix);

    if (len == 0 || len > WS_MAX_FRAME_SIZE) {
        ESP_LOGW(TAG, "Invalid message length from fd=%d: %zu", fd, len);
        return;
    }

    if (!is_valid_utf8_text(data, len)) {
        ESP_LOGW(TAG, "Non-text/binary data from fd=%d, not echoing", fd);
        return;
    }

    esp_err_t ret = ws_server_send(&g_ws_server, fd, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send to fd=%d: %s (0x%x)", fd, esp_err_to_name(ret), ret);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (ws_server_is_running(&g_ws_server)) {
            ESP_LOGI(TAG, "WebSocket server already running");
            return;
        }
        esp_err_t ret = ws_server_init(&g_ws_server, 4869, on_ws_message);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Relay listening on ws://" IPSTR ":4869",
                     IP2STR(&event->ip_info.ip));
        } else {
            ESP_LOGE(TAG, "Failed to init ws server: %s (0x%x)", esp_err_to_name(ret), ret);
        }
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

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();
}
