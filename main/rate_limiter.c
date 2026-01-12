#include "rate_limiter.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "rate_limiter";

void rate_limiter_init(rate_limiter_t *rl, const rate_config_t *config)
{
    memset(rl, 0, sizeof(rate_limiter_t));
    rl->lock = xSemaphoreCreateMutex();
    if (config) {
        memcpy(&rl->config, config, sizeof(rate_config_t));
    } else {
        rl->config.events_per_minute = 30;
        rl->config.reqs_per_minute = 60;
    }
}

void rate_limiter_destroy(rate_limiter_t *rl)
{
    if (!rl) return;
    if (rl->lock) {
        vSemaphoreDelete(rl->lock);
        rl->lock = NULL;
    }
}

static rate_bucket_t* get_bucket(rate_limiter_t *rl, int fd)
{
    for (int i = 0; i < RATE_LIMITER_MAX_BUCKETS; i++) {
        if (rl->buckets[i].active && rl->buckets[i].fd == fd) {
            return &rl->buckets[i];
        }
    }
    for (int i = 0; i < RATE_LIMITER_MAX_BUCKETS; i++) {
        if (!rl->buckets[i].active) {
            rl->buckets[i].fd = fd;
            rl->buckets[i].active = true;
            rl->buckets[i].event_count = 0;
            rl->buckets[i].req_count = 0;
            rl->buckets[i].window_start = esp_timer_get_time() / 1000000;
            return &rl->buckets[i];
        }
    }
    return NULL;
}

bool rate_limiter_check(rate_limiter_t *rl, int fd, rate_type_t type)
{
    xSemaphoreTake(rl->lock, portMAX_DELAY);

    rate_bucket_t *bucket = get_bucket(rl, fd);
    if (!bucket) {
        xSemaphoreGive(rl->lock);
        return false;
    }

    uint32_t now = esp_timer_get_time() / 1000000;

    if (now - bucket->window_start >= 60) {
        bucket->event_count = 0;
        bucket->req_count = 0;
        bucket->window_start = now;
    }

    bool allowed = true;
    if (type == RATE_TYPE_EVENT) {
        if (bucket->event_count >= rl->config.events_per_minute) {
            ESP_LOGW(TAG, "Rate limited: fd=%d events=%d", fd, bucket->event_count);
            allowed = false;
        } else {
            bucket->event_count++;
        }
    } else {
        if (bucket->req_count >= rl->config.reqs_per_minute) {
            ESP_LOGW(TAG, "Rate limited: fd=%d reqs=%d", fd, bucket->req_count);
            allowed = false;
        } else {
            bucket->req_count++;
        }
    }

    xSemaphoreGive(rl->lock);
    return allowed;
}

void rate_limiter_reset(rate_limiter_t *rl, int fd)
{
    xSemaphoreTake(rl->lock, portMAX_DELAY);
    for (int i = 0; i < RATE_LIMITER_MAX_BUCKETS; i++) {
        if (rl->buckets[i].active && rl->buckets[i].fd == fd) {
            rl->buckets[i].active = false;
            break;
        }
    }
    xSemaphoreGive(rl->lock);
}
