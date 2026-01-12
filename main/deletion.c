#include "deletion.h"
#include "storage_engine.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "deletion";

static int delete_by_event_ids(storage_engine_t *storage,
                               const nostr_deletion_request_t *request)
{
    int deleted = 0;

    for (size_t i = 0; i < request->event_ids_count; i++) {
        uint8_t event_id[32];
        if (nostr_hex_to_bytes(request->event_ids[i], 64, event_id, 32) != NOSTR_RELAY_OK) {
            continue;
        }

        nostr_event *target = storage_get_event(storage, event_id);
        if (!target) {
            continue;
        }

        if (nostr_deletion_authorized(request, target)) {
            if (storage_delete_event(storage, event_id) == STORAGE_OK) {
                deleted++;
                ESP_LOGI(TAG, "Deleted event: %.16s...", request->event_ids[i]);
            }
        } else {
            ESP_LOGW(TAG, "Unauthorized deletion: %.16s...", request->event_ids[i]);
        }

        nostr_event_destroy(target);
    }

    return deleted;
}

static int delete_by_addresses(storage_engine_t *storage,
                               const nostr_deletion_request_t *request,
                               int64_t delete_before)
{
    int deleted = 0;

    for (size_t i = 0; i < request->addresses_count; i++) {
        const char *addr = request->addresses[i];
        int32_t kind;
        char pubkey[65];
        char d_tag[256] = "";

        if (sscanf(addr, "%" SCNd32 ":%64[^:]:%255s", &kind, pubkey, d_tag) < 2) {
            continue;
        }

        if (strcmp(pubkey, request->pubkey) != 0) {
            ESP_LOGW(TAG, "Unauthorized address deletion: %s", addr);
            continue;
        }

        nostr_filter_t filter = {0};
        filter.kinds = malloc(sizeof(int32_t));
        filter.authors = malloc(sizeof(char *));
        if (!filter.kinds || !filter.authors) {
            free(filter.kinds);
            free(filter.authors);
            continue;
        }
        filter.kinds[0] = kind;
        filter.kinds_count = 1;
        filter.authors[0] = pubkey;
        filter.authors_count = 1;
        filter.until = delete_before;
        filter.limit = 100;

        nostr_event **events = NULL;
        uint16_t count = 0;

        if (storage_query_events(storage, &filter, &events, &count, 100) == STORAGE_OK && events) {
            for (uint16_t e = 0; e < count; e++) {
                const char *event_d = nostr_event_get_d_tag(events[e]);
                if (!event_d) event_d = "";

                if (strcmp(event_d, d_tag) == 0) {
                    if (storage_delete_event(storage, events[e]->id) == STORAGE_OK) {
                        deleted++;
                        ESP_LOGI(TAG, "Deleted addressable: %s", addr);
                    }
                }
            }
            storage_free_query_results(events, count);
        }

        free(filter.kinds);
        free(filter.authors);
    }

    return deleted;
}

static int delete_by_kinds(storage_engine_t *storage,
                           const nostr_event *delete_event)
{
    int deleted = 0;
    int32_t kinds[32];
    size_t kinds_count = 0;

    for (size_t i = 0; i < delete_event->tags_count && kinds_count < 32; i++) {
        nostr_tag *tag = &delete_event->tags[i];
        if (tag->count >= 2 && strcmp(tag->values[0], "k") == 0) {
            kinds[kinds_count++] = atoi(tag->values[1]);
        }
    }

    if (kinds_count == 0) {
        return 0;
    }

    char pubkey_hex[65];
    nostr_bytes_to_hex(delete_event->pubkey.data, 32, pubkey_hex);

    for (size_t k = 0; k < kinds_count; k++) {
        nostr_filter_t filter = {0};
        filter.kinds = &kinds[k];
        filter.kinds_count = 1;
        filter.authors = malloc(sizeof(char *));
        if (!filter.authors) continue;
        filter.authors[0] = pubkey_hex;
        filter.authors_count = 1;
        filter.until = delete_event->created_at;
        filter.limit = 500;

        nostr_event **events = NULL;
        uint16_t count = 0;

        if (storage_query_events(storage, &filter, &events, &count, 500) == STORAGE_OK && events) {
            int kind_deleted = 0;
            for (uint16_t e = 0; e < count; e++) {
                if (storage_delete_event(storage, events[e]->id) == STORAGE_OK) {
                    kind_deleted++;
                    deleted++;
                }
            }
            if (kind_deleted > 0) {
                ESP_LOGI(TAG, "Deleted %u events of kind %" PRId32, kind_deleted, kinds[k]);
            }
            storage_free_query_results(events, count);
        }

        free(filter.authors);
    }

    return deleted;
}

int deletion_process(storage_engine_t *storage, const nostr_event *delete_event)
{
    if (!storage || !delete_event || delete_event->kind != NOSTR_KIND_DELETION) {
        return 0;
    }

    nostr_deletion_request_t request;
    if (nostr_deletion_parse(delete_event, &request) != NOSTR_RELAY_OK) {
        ESP_LOGW(TAG, "Failed to parse deletion request");
        return 0;
    }

    int deleted = 0;

    deleted += delete_by_event_ids(storage, &request);
    deleted += delete_by_addresses(storage, &request, delete_event->created_at);
    deleted += delete_by_kinds(storage, delete_event);

    nostr_deletion_free(&request);

    return deleted;
}
