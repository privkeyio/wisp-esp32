#include "flash_monitor.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "flash_monitor";

void flash_get_health(const char *partition_label, flash_health_t *health)
{
    memset(health, 0, sizeof(flash_health_t));

    esp_err_t ret = esp_littlefs_info(partition_label,
                                       &health->total_bytes,
                                       &health->used_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LittleFS info: %s", esp_err_to_name(ret));
        return;
    }

    if (health->total_bytes == 0) {
        health->free_bytes = 0;
        health->usage_percent = 0.0f;
    } else {
        health->free_bytes = health->total_bytes - health->used_bytes;
        health->usage_percent = (float)health->used_bytes / health->total_bytes * 100.0f;
    }

    ESP_LOGD(TAG, "Flash: %.1f%% used (%zu/%zu bytes)",
             health->usage_percent, health->used_bytes, health->total_bytes);
}
