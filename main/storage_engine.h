#ifndef STORAGE_ENGINE_H
#define STORAGE_ENGINE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nostr_relay_protocol.h"

#define STORAGE_MAX_EVENTS         5000
#define STORAGE_MAX_EVENT_SIZE     8192
#define STORAGE_INDEX_ENTRIES      5000
#define STORAGE_PARTITION_LABEL    "storage"

typedef enum {
    STORAGE_OK = 0,
    STORAGE_ERR_NOT_INITIALIZED,
    STORAGE_ERR_FULL,
    STORAGE_ERR_DUPLICATE,
    STORAGE_ERR_NOT_FOUND,
    STORAGE_ERR_IO,
    STORAGE_ERR_NO_MEM,
    STORAGE_ERR_SERIALIZE
} storage_error_t;

#define STORAGE_FLAG_DELETED  0x01

typedef struct __attribute__((packed)) {
    uint8_t  event_id[32];
    uint32_t created_at;
    uint32_t expires_at;
    uint16_t kind;
    uint8_t  pubkey_prefix[4];
    uint16_t file_index;
    uint8_t  flags;
    uint8_t  reserved;
} storage_index_entry_t;

typedef struct {
    uint32_t total_events;
    uint32_t total_bytes;
    uint32_t free_bytes;
    uint32_t oldest_event_ts;
    uint32_t newest_event_ts;
} storage_stats_t;

typedef struct storage_engine {
    storage_index_entry_t *index;
    uint16_t index_count;
    uint16_t next_file_index;
    SemaphoreHandle_t lock;
    bool initialized;
    char mount_point[16];
    uint32_t default_ttl_sec;
} storage_engine_t;

esp_err_t storage_init(storage_engine_t *engine, uint32_t default_ttl_sec);
void storage_destroy(storage_engine_t *engine);

storage_error_t storage_save_event(storage_engine_t *engine, const nostr_event *event);

storage_error_t storage_query_events(storage_engine_t *engine,
                                     const nostr_filter_t *filter,
                                     nostr_event ***results,
                                     uint16_t *count,
                                     uint16_t limit);

void storage_free_query_results(nostr_event **results, uint16_t count);

bool storage_event_exists(storage_engine_t *engine, const uint8_t event_id[32]);

storage_error_t storage_delete_event(storage_engine_t *engine, const uint8_t event_id[32]);

int storage_purge_expired(storage_engine_t *engine);

int storage_compact_index(storage_engine_t *engine);

void storage_get_stats(storage_engine_t *engine, storage_stats_t *stats);

void storage_start_cleanup_task(storage_engine_t *engine);

#endif
