#include "broadcaster.h"
#include "relay_core.h"
#include "router.h"
#include "sub_manager.h"

#include "esp_log.h"

static const char *TAG = "broadcaster";

void broadcaster_fanout(relay_ctx_t *ctx, const nostr_event *event)
{
    if (!ctx || !ctx->sub_manager) {
        return;
    }

    sub_match_result_t matches;
    sub_manager_match(ctx->sub_manager, event, &matches);

    if (matches.count == 0) {
        ESP_LOGD(TAG, "No subscribers for event kind=%d", event->kind);
        return;
    }

    ESP_LOGD(TAG, "Broadcasting event kind=%d to %d subscriptions",
             event->kind, matches.count);

    for (uint8_t i = 0; i < matches.count; i++) {
        sub_match_entry_t *entry = &matches.matches[i];
        router_send_event(ctx, entry->conn_fd, entry->sub_id, event);
        ESP_LOGD(TAG, "Sent to sub=%s fd=%d", entry->sub_id, entry->conn_fd);
    }

    ESP_LOGD(TAG, "Broadcast complete: %d subscriptions", matches.count);
}
