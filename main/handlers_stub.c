#include "broadcaster.h"
#include "relay_core.h"
#include "router.h"
#include "storage_engine.h"
#include "sub_manager.h"
#include "validator.h"

#include "esp_log.h"

static const char *TAG = "handlers";

int handle_event(relay_ctx_t *ctx, int conn_fd, nostr_event *event)
{
    validator_config_t config = {
        .max_event_age_sec = ctx->config.max_event_age_sec,
        .max_future_sec = ctx->config.max_future_sec,
        .min_pow_difficulty = 0,
        .check_duplicates = true,
    };

    validation_result_t result = validator_check_event(event, &config, ctx->storage);
    if (result != VALIDATION_OK) {
        ESP_LOGW(TAG, "Validation failed: %s", validator_result_string(result));
        return validator_result_to_relay_error(result);
    }

    bool ephemeral = nostr_kind_is_ephemeral(event->kind);

    if (!ephemeral && ctx->storage) {
        storage_error_t store_result = storage_save_event(ctx->storage, event);
        if (store_result != STORAGE_OK && store_result != STORAGE_ERR_DUPLICATE) {
            ESP_LOGE(TAG, "Storage failed: %d", store_result);
            return NOSTR_RELAY_ERR_STORAGE;
        }
    }

    ESP_LOGI(TAG, "EVENT: kind=%d fd=%d ephemeral=%d", event->kind, conn_fd, ephemeral);

    broadcaster_fanout(ctx, event);

    return NOSTR_RELAY_OK;
}

void handle_req(relay_ctx_t *ctx, int conn_fd, router_req_t *req)
{
    ESP_LOGI(TAG, "REQ: sub=%s filters=%zu fd=%d", req->sub_id, req->filter_count, conn_fd);

    if (!ctx->sub_manager) {
        router_send_eose(ctx, conn_fd, req->sub_id);
        return;
    }

    nostr_relay_error_t err = sub_manager_add(ctx->sub_manager, conn_fd,
                                              req->sub_id, req->filters,
                                              req->filter_count);
    if (err != NOSTR_RELAY_OK) {
        router_send_closed(ctx, conn_fd, req->sub_id, "error: too many subscriptions");
        return;
    }

    if (ctx->storage) {
        uint8_t sent_ids[100][32];
        uint16_t sent_count = 0;

        for (size_t i = 0; i < req->filter_count; i++) {
            if (req->filters[i].limit == 0) {
                continue;
            }

            nostr_event **events = NULL;
            uint16_t event_count = 0;
            uint16_t limit = req->filters[i].limit;

            storage_error_t query_result = storage_query_events(ctx->storage,
                                                                 &req->filters[i],
                                                                 &events,
                                                                 &event_count,
                                                                 limit);
            if (query_result == STORAGE_OK && events) {
                for (uint16_t e = 0; e < event_count; e++) {
                    bool duplicate = false;
                    for (uint16_t s = 0; s < sent_count; s++) {
                        if (memcmp(sent_ids[s], events[e]->id, 32) == 0) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        router_send_event(ctx, conn_fd, req->sub_id, events[e]);
                        if (sent_count < 100) {
                            memcpy(sent_ids[sent_count++], events[e]->id, 32);
                        }
                    }
                }
                storage_free_query_results(events, event_count);
            }
        }
    }

    router_send_eose(ctx, conn_fd, req->sub_id);
}

int handle_close(relay_ctx_t *ctx, int conn_fd, const char *sub_id)
{
    ESP_LOGI(TAG, "CLOSE: sub=%s fd=%d", sub_id, conn_fd);

    if (ctx->sub_manager) {
        return sub_manager_remove(ctx->sub_manager, conn_fd, sub_id);
    }
    return NOSTR_RELAY_OK;
}
