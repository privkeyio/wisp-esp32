#include "esp_log.h"

#include "relay_core.h"
#include "router.h"
#include "sub_manager.h"
#include "validator.h"

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

    ESP_LOGI(TAG, "EVENT: kind=%d fd=%d", event->kind, conn_fd);
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
