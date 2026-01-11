#include "esp_log.h"

#include "relay_core.h"
#include "router.h"
#include "validator.h"

static const char *TAG = "handlers";

int handle_event(relay_ctx_t *ctx, int conn_fd, nostr_event *event)
{
    ESP_LOGI(TAG, "EVENT: kind=%d fd=%d", event->kind, conn_fd);

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

    ESP_LOGD(TAG, "Event accepted: kind=%d ephemeral=%d", event->kind, nostr_kind_is_ephemeral(event->kind));

    return NOSTR_RELAY_OK;
}

int handle_req(relay_ctx_t *ctx, int conn_fd, router_req_t *req)
{
    ESP_LOGI(TAG, "REQ: sub=%s filters=%zu fd=%d", req->sub_id, req->filter_count, conn_fd);

    router_send_eose(ctx, conn_fd, req->sub_id);
    return NOSTR_RELAY_OK;
}

int handle_close(relay_ctx_t *ctx, int conn_fd, const char *sub_id)
{
    ESP_LOGI(TAG, "CLOSE: sub=%s fd=%d", sub_id, conn_fd);
    return NOSTR_RELAY_OK;
}
