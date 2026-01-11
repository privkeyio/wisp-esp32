#include "esp_log.h"

#include "relay_core.h"
#include "router.h"

static const char *TAG = "handlers";

int handle_event(relay_ctx_t *ctx, int conn_fd, nostr_event *event)
{
    ESP_LOGI(TAG, "EVENT: kind=%d fd=%d", event->kind, conn_fd);

    nostr_validation_result_t result;
    nostr_relay_error_t err = nostr_event_validate_full(event, ctx->config.max_future_sec, &result);
    if (err != NOSTR_RELAY_OK) {
        ESP_LOGW(TAG, "Validation failed: %s", result.error_message);
        return err;
    }

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
