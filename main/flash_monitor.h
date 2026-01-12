#ifndef FLASH_MONITOR_H
#define FLASH_MONITOR_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    size_t total_bytes;
    size_t used_bytes;
    size_t free_bytes;
    float usage_percent;
} flash_health_t;

void flash_get_health(const char *partition_label, flash_health_t *health);

#endif
