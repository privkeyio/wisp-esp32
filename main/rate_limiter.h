#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define RATE_LIMITER_MAX_BUCKETS 16

typedef enum {
    RATE_TYPE_EVENT,
    RATE_TYPE_REQ,
} rate_type_t;

typedef struct {
    uint16_t events_per_minute;
    uint16_t reqs_per_minute;
} rate_config_t;

typedef struct {
    int      fd;
    uint16_t event_count;
    uint16_t req_count;
    uint32_t window_start;
    bool     active;
} rate_bucket_t;

typedef struct rate_limiter {
    rate_config_t config;
    rate_bucket_t buckets[RATE_LIMITER_MAX_BUCKETS];
    SemaphoreHandle_t lock;
} rate_limiter_t;

void rate_limiter_init(rate_limiter_t *rl, const rate_config_t *config);
bool rate_limiter_check(rate_limiter_t *rl, int fd, rate_type_t type);
void rate_limiter_reset(rate_limiter_t *rl, int fd);

#endif
