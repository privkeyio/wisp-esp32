#include "storage_engine.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "storage";

#define INDEX_NVS_NAMESPACE "nostr_idx"
#define EVENTS_DIR "/littlefs/events"

static void get_event_path(const uint8_t event_id[32], uint32_t file_index,
                           char *path, size_t len)
{
    char id_hex[33];
    nostr_bytes_to_hex(event_id, 16, id_hex);
    snprintf(path, len, EVENTS_DIR "/%02x/%s_%08" PRIx32 ".bin",
             event_id[0], id_hex, file_index);
}

static storage_index_entry_t *find_index_entry(storage_engine_t *engine,
                                               const uint8_t event_id[32])
{
    for (uint16_t i = 0; i < engine->index_count; i++) {
        if (memcmp(engine->index[i].event_id, event_id, 32) == 0 &&
            !(engine->index[i].flags & STORAGE_FLAG_DELETED)) {
            return &engine->index[i];
        }
    }
    return NULL;
}

static int save_index_to_nvs(storage_engine_t *engine)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(INDEX_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %d", err);
        return STORAGE_ERR_IO;
    }

    err = nvs_set_u16(nvs, "count", engine->index_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set count: %d", err);
        nvs_close(nvs);
        return STORAGE_ERR_IO;
    }

    err = nvs_set_u32(nvs, "next_idx", engine->next_file_index);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set next_idx: %d", err);
        nvs_close(nvs);
        return STORAGE_ERR_IO;
    }

    const uint16_t chunk_size = 50;
    uint16_t num_chunks = (engine->index_count + chunk_size - 1) / chunk_size;
    if (engine->index_count == 0) num_chunks = 0;

    for (uint16_t i = 0; i < engine->index_count; i += chunk_size) {
        char key[16];
        snprintf(key, sizeof(key), "idx_%u", i / chunk_size);
        uint16_t entries = engine->index_count - i;
        if (entries > chunk_size) entries = chunk_size;
        err = nvs_set_blob(nvs, key, &engine->index[i],
                           entries * sizeof(storage_index_entry_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set blob %s: %d", key, err);
            nvs_close(nvs);
            return STORAGE_ERR_IO;
        }
    }

    for (uint16_t chunk = num_chunks; chunk < 100; chunk++) {
        char key[16];
        snprintf(key, sizeof(key), "idx_%u", chunk);
        err = nvs_erase_key(nvs, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) break;
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to erase stale chunk %s: %d", key, err);
        }
    }

    err = nvs_commit(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %d", err);
        nvs_close(nvs);
        return STORAGE_ERR_IO;
    }

    nvs_close(nvs);
    return STORAGE_OK;
}

static int load_index_from_nvs(storage_engine_t *engine)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(INDEX_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No existing index found");
        return STORAGE_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %d", err);
        return STORAGE_ERR_IO;
    }

    err = nvs_get_u16(nvs, "count", &engine->index_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get count: %d", err);
        nvs_close(nvs);
        return STORAGE_ERR_IO;
    }

    err = nvs_get_u32(nvs, "next_idx", &engine->next_file_index);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get next_idx: %d", err);
        nvs_close(nvs);
        return STORAGE_ERR_IO;
    }

    const uint16_t chunk_size = 50;
    for (uint16_t i = 0; i < engine->index_count; i += chunk_size) {
        char key[16];
        snprintf(key, sizeof(key), "idx_%u", i / chunk_size);
        uint16_t entries = engine->index_count - i;
        if (entries > chunk_size) entries = chunk_size;
        size_t expected_len = entries * sizeof(storage_index_entry_t);
        size_t len = expected_len;
        err = nvs_get_blob(nvs, key, &engine->index[i], &len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get blob %s: %d", key, err);
            nvs_close(nvs);
            return STORAGE_ERR_IO;
        }
        if (len != expected_len) {
            ESP_LOGE(TAG, "Blob %s size mismatch: got %zu, expected %zu", key, len, expected_len);
            nvs_close(nvs);
            return STORAGE_ERR_IO;
        }
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Loaded %" PRIu16 " index entries", engine->index_count);
    return STORAGE_OK;
}

esp_err_t storage_init(storage_engine_t *engine, uint32_t default_ttl_sec)
{
    memset(engine, 0, sizeof(storage_engine_t));
    engine->default_ttl_sec = default_ttl_sec;
    strcpy(engine->mount_point, "/littlefs");

    engine->lock = xSemaphoreCreateMutex();
    if (!engine->lock) {
        return ESP_ERR_NO_MEM;
    }

    engine->index = heap_caps_calloc(STORAGE_INDEX_ENTRIES,
                                     sizeof(storage_index_entry_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!engine->index) {
        engine->index = calloc(STORAGE_INDEX_ENTRIES,
                               sizeof(storage_index_entry_t));
    }
    if (!engine->index) {
        vSemaphoreDelete(engine->lock);
        return ESP_ERR_NO_MEM;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = STORAGE_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS: %d", ret);
        free(engine->index);
        vSemaphoreDelete(engine->lock);
        return ret;
    }

    mkdir(EVENTS_DIR, 0755);

    for (int i = 0; i < 256; i++) {
        char subdir[64];
        snprintf(subdir, sizeof(subdir), EVENTS_DIR "/%02x", i);
        mkdir(subdir, 0755);
    }

    int load_err = load_index_from_nvs(engine);
    if (load_err != STORAGE_OK) {
        ESP_LOGW(TAG, "Failed to load index, starting fresh");
        engine->index_count = 0;
        engine->next_file_index = 0;
    }

    engine->initialized = true;

    size_t total, used;
    esp_littlefs_info(STORAGE_PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "Storage initialized: %" PRIu16 " events, %zu/%zu bytes used",
             engine->index_count, used, total);

    return ESP_OK;
}

void storage_destroy(storage_engine_t *engine)
{
    if (!engine->initialized) return;

    if (engine->cleanup_task) {
        engine->cleanup_stop = true;
        while (engine->cleanup_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    save_index_to_nvs(engine);
    esp_vfs_littlefs_unregister(STORAGE_PARTITION_LABEL);

    if (engine->index) {
        free(engine->index);
        engine->index = NULL;
    }
    if (engine->lock) {
        vSemaphoreDelete(engine->lock);
        engine->lock = NULL;
    }
    engine->initialized = false;
}

storage_error_t storage_save_event(storage_engine_t *engine, const nostr_event *event)
{
    if (!engine->initialized) return STORAGE_ERR_NOT_INITIALIZED;

    xSemaphoreTake(engine->lock, portMAX_DELAY);

    if (find_index_entry(engine, event->id)) {
        xSemaphoreGive(engine->lock);
        return STORAGE_ERR_DUPLICATE;
    }

    if (engine->index_count >= STORAGE_INDEX_ENTRIES) {
        xSemaphoreGive(engine->lock);
        ESP_LOGW(TAG, "Storage full");
        return STORAGE_ERR_FULL;
    }

    char *json = malloc(STORAGE_MAX_EVENT_SIZE);
    if (!json) {
        xSemaphoreGive(engine->lock);
        return STORAGE_ERR_NO_MEM;
    }

    size_t json_len;
    nostr_relay_error_t ser_err = nostr_event_serialize(event, json, STORAGE_MAX_EVENT_SIZE, &json_len);
    if (ser_err != NOSTR_RELAY_OK) {
        free(json);
        xSemaphoreGive(engine->lock);
        return STORAGE_ERR_SERIALIZE;
    }

    char path[128];
    get_event_path(event->id, engine->next_file_index, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        char dir[64];
        snprintf(dir, sizeof(dir), EVENTS_DIR "/%02x", event->id[0]);
        mkdir(dir, 0755);
        f = fopen(path, "wb");
    }
    if (!f) {
        free(json);
        xSemaphoreGive(engine->lock);
        ESP_LOGE(TAG, "Failed to create file: %s", path);
        return STORAGE_ERR_IO;
    }

    size_t written = fwrite(json, 1, json_len, f);
    fclose(f);
    free(json);

    if (written != json_len) {
        ESP_LOGE(TAG, "Short write to %s: %zu/%zu bytes", path, written, json_len);
        unlink(path);
        xSemaphoreGive(engine->lock);
        return STORAGE_ERR_IO;
    }

    storage_index_entry_t *entry = &engine->index[engine->index_count];
    memcpy(entry->event_id, event->id, 32);
    entry->created_at = (uint32_t)event->created_at;
    entry->kind = event->kind;
    memcpy(entry->pubkey_prefix, event->pubkey.data, 4);
    entry->file_index = engine->next_file_index;
    entry->flags = 0;

    uint32_t now = (uint32_t)time(NULL);
    entry->expires_at = now + engine->default_ttl_sec;

    int64_t nip40_exp = nostr_event_get_expiration(event);
    if (nip40_exp > 0 && (uint32_t)nip40_exp < entry->expires_at) {
        entry->expires_at = (uint32_t)nip40_exp;
    }

    engine->index_count++;
    engine->next_file_index++;

    if (engine->index_count % 10 == 0) {
        save_index_to_nvs(engine);
    }

    xSemaphoreGive(engine->lock);

    ESP_LOGD(TAG, "Stored event: kind=%" PRIu16 ", expires=%" PRIu32, event->kind, entry->expires_at);
    return STORAGE_OK;
}

bool storage_event_exists(storage_engine_t *engine, const uint8_t event_id[32])
{
    if (!engine->initialized) return false;

    xSemaphoreTake(engine->lock, portMAX_DELAY);
    bool exists = (find_index_entry(engine, event_id) != NULL);
    xSemaphoreGive(engine->lock);

    return exists;
}

static nostr_event *load_event_from_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > STORAGE_MAX_EVENT_SIZE) {
        fclose(f);
        return NULL;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return NULL;
    }

    size_t bytes_read = fread(json, 1, size, f);
    if (bytes_read != (size_t)size) {
        if (ferror(f)) {
            ESP_LOGE(TAG, "Read error on %s", path);
        }
        free(json);
        fclose(f);
        return NULL;
    }
    json[bytes_read] = '\0';
    fclose(f);

    nostr_event *event = NULL;
    nostr_event_parse(json, bytes_read, &event);
    free(json);

    return event;
}

static bool index_matches_filter(const storage_index_entry_t *entry,
                                 const nostr_filter_t *filter)
{
    if (filter->since > 0 && entry->created_at < (uint32_t)filter->since) return false;
    if (filter->until > 0 && entry->created_at > (uint32_t)filter->until) return false;

    if (filter->kinds_count > 0) {
        bool found = false;
        for (size_t k = 0; k < filter->kinds_count; k++) {
            if (filter->kinds[k] == entry->kind) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    if (filter->ids_count > 0) {
        bool found = false;
        for (size_t k = 0; k < filter->ids_count; k++) {
            uint8_t id_bytes[32];
            if (nostr_hex_to_bytes(filter->ids[k], 64, id_bytes, 32) == NOSTR_RELAY_OK) {
                if (memcmp(id_bytes, entry->event_id, 32) == 0) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) return false;
    }

    if (filter->authors_count > 0) {
        bool found = false;
        for (size_t k = 0; k < filter->authors_count; k++) {
            size_t prefix_len = strlen(filter->authors[k]);
            if (prefix_len >= 8) {
                uint8_t prefix_bytes[4];
                if (nostr_hex_to_bytes(filter->authors[k], 8, prefix_bytes, 4) == NOSTR_RELAY_OK) {
                    if (memcmp(prefix_bytes, entry->pubkey_prefix, 4) == 0) {
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) return false;
    }

    return true;
}

static void mark_entry_expired(storage_index_entry_t *entry)
{
    char path[128];
    get_event_path(entry->event_id, entry->file_index, path, sizeof(path));
    unlink(path);
    entry->flags |= STORAGE_FLAG_DELETED;
}

storage_error_t storage_query_events(storage_engine_t *engine,
                                     const nostr_filter_t *filter,
                                     nostr_event ***results,
                                     uint16_t *count,
                                     uint16_t limit)
{
    if (!engine->initialized) return STORAGE_ERR_NOT_INITIALIZED;

    *results = NULL;
    *count = 0;

    if (limit > 500) limit = 500;

    nostr_event **events = calloc(limit, sizeof(nostr_event *));
    if (!events) return STORAGE_ERR_NO_MEM;

    xSemaphoreTake(engine->lock, portMAX_DELAY);

    uint32_t now = (uint32_t)time(NULL);

    for (int i = engine->index_count - 1; i >= 0 && *count < limit; i--) {
        storage_index_entry_t *entry = &engine->index[i];

        if (entry->flags & STORAGE_FLAG_DELETED) continue;

        if (entry->expires_at > 0 && entry->expires_at < now) {
            mark_entry_expired(entry);
            continue;
        }

        if (!index_matches_filter(entry, filter)) continue;

        char path[128];
        get_event_path(entry->event_id, entry->file_index, path, sizeof(path));
        nostr_event *event = load_event_from_file(path);

        if (event && nostr_filter_matches(filter, event)) {
            events[*count] = event;
            (*count)++;
        } else if (event) {
            nostr_event_destroy(event);
        }
    }

    xSemaphoreGive(engine->lock);

    *results = events;
    ESP_LOGD(TAG, "Query returned %" PRIu16 " events", *count);
    return STORAGE_OK;
}

void storage_free_query_results(nostr_event **results, uint16_t count)
{
    if (!results) return;
    for (uint16_t i = 0; i < count; i++) {
        if (results[i]) {
            nostr_event_destroy(results[i]);
        }
    }
    free(results);
}

int storage_purge_expired(storage_engine_t *engine)
{
    if (!engine->initialized) return 0;

    xSemaphoreTake(engine->lock, portMAX_DELAY);

    uint32_t now = (uint32_t)time(NULL);
    int purged = 0;

    for (uint16_t i = 0; i < engine->index_count; i++) {
        storage_index_entry_t *entry = &engine->index[i];

        if (entry->flags & STORAGE_FLAG_DELETED) continue;

        if (entry->expires_at > 0 && entry->expires_at < now) {
            mark_entry_expired(entry);
            purged++;
        }
    }

    if (purged > 0) {
        save_index_to_nvs(engine);
        ESP_LOGI(TAG, "Purged %d expired events", purged);
    }

    xSemaphoreGive(engine->lock);
    return purged;
}

int storage_compact_index(storage_engine_t *engine)
{
    if (!engine->initialized) return 0;

    xSemaphoreTake(engine->lock, portMAX_DELAY);

    uint16_t write_idx = 0;
    int compacted = 0;

    for (uint16_t read_idx = 0; read_idx < engine->index_count; read_idx++) {
        if (!(engine->index[read_idx].flags & STORAGE_FLAG_DELETED)) {
            if (write_idx != read_idx) {
                memcpy(&engine->index[write_idx], &engine->index[read_idx],
                       sizeof(storage_index_entry_t));
            }
            write_idx++;
        } else {
            compacted++;
        }
    }

    if (compacted > 0) {
        engine->index_count = write_idx;
        save_index_to_nvs(engine);
        ESP_LOGI(TAG, "Compacted index: removed %d entries, %" PRIu16 " remaining",
                 compacted, engine->index_count);
    }

    xSemaphoreGive(engine->lock);
    return compacted;
}

storage_error_t storage_delete_event(storage_engine_t *engine, const uint8_t event_id[32])
{
    if (!engine->initialized) return STORAGE_ERR_NOT_INITIALIZED;

    xSemaphoreTake(engine->lock, portMAX_DELAY);

    storage_index_entry_t *entry = find_index_entry(engine, event_id);
    if (!entry) {
        xSemaphoreGive(engine->lock);
        return STORAGE_ERR_NOT_FOUND;
    }

    char path[128];
    get_event_path(entry->event_id, entry->file_index, path, sizeof(path));
    unlink(path);

    entry->flags |= STORAGE_FLAG_DELETED;
    save_index_to_nvs(engine);

    xSemaphoreGive(engine->lock);
    return STORAGE_OK;
}

nostr_event *storage_get_event(storage_engine_t *engine, const uint8_t event_id[32])
{
    if (!engine->initialized) return NULL;

    xSemaphoreTake(engine->lock, portMAX_DELAY);

    storage_index_entry_t *entry = find_index_entry(engine, event_id);
    if (!entry) {
        xSemaphoreGive(engine->lock);
        return NULL;
    }

    char path[128];
    get_event_path(entry->event_id, entry->file_index, path, sizeof(path));
    nostr_event *event = load_event_from_file(path);

    xSemaphoreGive(engine->lock);
    return event;
}

void storage_get_stats(storage_engine_t *engine, storage_stats_t *stats)
{
    memset(stats, 0, sizeof(storage_stats_t));
    if (!engine->initialized) return;

    xSemaphoreTake(engine->lock, portMAX_DELAY);

    uint32_t now = (uint32_t)time(NULL);
    stats->oldest_event_ts = UINT32_MAX;

    for (uint16_t i = 0; i < engine->index_count; i++) {
        if (engine->index[i].flags & STORAGE_FLAG_DELETED) continue;
        if (engine->index[i].expires_at > 0 && engine->index[i].expires_at < now) continue;

        stats->total_events++;
        if (engine->index[i].created_at < stats->oldest_event_ts) {
            stats->oldest_event_ts = engine->index[i].created_at;
        }
        if (engine->index[i].created_at > stats->newest_event_ts) {
            stats->newest_event_ts = engine->index[i].created_at;
        }
    }

    if (stats->total_events == 0) {
        stats->oldest_event_ts = 0;
    }

    size_t total, used;
    esp_littlefs_info(STORAGE_PARTITION_LABEL, &total, &used);
    stats->total_bytes = total;
    stats->free_bytes = total - used;

    xSemaphoreGive(engine->lock);
}

static void storage_cleanup_task(void *arg)
{
    storage_engine_t *engine = (storage_engine_t *)arg;
    int cycles_since_compact = 0;

    while (!engine->cleanup_stop) {
        for (int i = 0; i < 60 && !engine->cleanup_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (engine->cleanup_stop) break;

        storage_purge_expired(engine);
        cycles_since_compact++;

        if (cycles_since_compact >= 10) {
            storage_compact_index(engine);
            cycles_since_compact = 0;
        }
    }

    engine->cleanup_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t storage_start_cleanup_task(storage_engine_t *engine)
{
    engine->cleanup_stop = false;
    BaseType_t ret = xTaskCreate(storage_cleanup_task, "storage_cleanup", 4096,
                                 engine, 2, &engine->cleanup_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cleanup task");
        engine->cleanup_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
