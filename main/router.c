#include <string.h>

#include "esp_log.h"

#include "relay_core.h"
#include "router.h"
#include "ws_server.h"

static const char *TAG = "router";

#define ROUTER_SEND_BUF_SIZE 512

nostr_relay_error_t router_parse(const char *json, size_t len, router_msg_t *out)
{
    memset(out, 0, sizeof(router_msg_t));
    out->type = ROUTER_MSG_INVALID;

    nostr_client_msg_t msg;
    nostr_relay_error_t result = nostr_client_msg_parse(json, len, &msg);
    if (result != NOSTR_RELAY_OK) {
        ESP_LOGW(TAG, "Parse failed: %d", result);
        return result;
    }

    switch (msg.type) {
        case NOSTR_CLIENT_MSG_EVENT:
            out->type = ROUTER_MSG_EVENT;
            out->data.event = msg.data.event.event;
            msg.data.event.event = NULL;
            break;

        case NOSTR_CLIENT_MSG_REQ:
            out->type = ROUTER_MSG_REQ;
            strncpy(out->data.req.sub_id, msg.data.req.subscription_id, ROUTER_MAX_SUB_ID);
            out->data.req.sub_id[ROUTER_MAX_SUB_ID] = '\0';
            out->data.req.filters = msg.data.req.filters;
            out->data.req.filter_count = msg.data.req.filters_count;
            msg.data.req.filters = NULL;
            msg.data.req.filters_count = 0;
            break;

        case NOSTR_CLIENT_MSG_CLOSE:
            out->type = ROUTER_MSG_CLOSE;
            strncpy(out->data.close.sub_id, msg.data.close.subscription_id, ROUTER_MAX_SUB_ID);
            out->data.close.sub_id[ROUTER_MAX_SUB_ID] = '\0';
            break;

        case NOSTR_CLIENT_MSG_AUTH:
            out->type = ROUTER_MSG_AUTH;
            break;

        default:
            out->type = ROUTER_MSG_UNKNOWN;
            break;
    }

    nostr_client_msg_free(&msg);
    return NOSTR_RELAY_OK;
}

void router_msg_free(router_msg_t *msg)
{
    if (!msg) return;

    switch (msg->type) {
        case ROUTER_MSG_EVENT:
            if (msg->data.event) {
                nostr_event_destroy(msg->data.event);
                msg->data.event = NULL;
            }
            break;

        case ROUTER_MSG_REQ:
            if (msg->data.req.filters) {
                for (size_t i = 0; i < msg->data.req.filter_count; i++) {
                    nostr_filter_free(&msg->data.req.filters[i]);
                }
                nostr_free(msg->data.req.filters);
                msg->data.req.filters = NULL;
                msg->data.req.filter_count = 0;
            }
            break;

        default:
            break;
    }
}

static esp_err_t send_relay_msg(relay_ctx_t *ctx, int conn_fd, nostr_relay_msg_t *relay_msg)
{
    char buf[ROUTER_SEND_BUF_SIZE];
    size_t out_len;
    nostr_relay_error_t err = nostr_relay_msg_serialize(relay_msg, buf, sizeof(buf), &out_len);
    if (err != NOSTR_RELAY_OK) {
        ESP_LOGE(TAG, "Serialize failed: %d", err);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t send_err = ws_server_send(&ctx->ws_server, conn_fd, buf, out_len);
    if (send_err != ESP_OK) {
        ESP_LOGW(TAG, "Send failed fd=%d: %d", conn_fd, send_err);
    }
    return send_err;
}

esp_err_t router_send_notice(relay_ctx_t *ctx, int conn_fd, const char *message)
{
    nostr_relay_msg_t msg;
    nostr_relay_msg_notice(&msg, message);
    return send_relay_msg(ctx, conn_fd, &msg);
}

esp_err_t router_send_ok(relay_ctx_t *ctx, int conn_fd, const char *event_id_hex,
                         bool accepted, const char *message)
{
    nostr_relay_msg_t msg;
    nostr_relay_msg_ok(&msg, event_id_hex, accepted, message ? message : "");
    return send_relay_msg(ctx, conn_fd, &msg);
}

esp_err_t router_send_eose(relay_ctx_t *ctx, int conn_fd, const char *sub_id)
{
    nostr_relay_msg_t msg;
    nostr_relay_msg_eose(&msg, sub_id);
    return send_relay_msg(ctx, conn_fd, &msg);
}

esp_err_t router_send_closed(relay_ctx_t *ctx, int conn_fd, const char *sub_id,
                             const char *message)
{
    nostr_relay_msg_t msg;
    nostr_relay_msg_closed(&msg, sub_id, message ? message : "");
    return send_relay_msg(ctx, conn_fd, &msg);
}

#define ROUTER_EVENT_BUF_SIZE 16384

esp_err_t router_send_event(relay_ctx_t *ctx, int conn_fd, const char *sub_id,
                            const nostr_event *event)
{
    nostr_relay_msg_t msg;
    nostr_relay_msg_event(&msg, sub_id, event);

    char *buf = malloc(ROUTER_EVENT_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate event buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t out_len;
    nostr_relay_error_t err = nostr_relay_msg_serialize(&msg, buf, ROUTER_EVENT_BUF_SIZE, &out_len);
    if (err != NOSTR_RELAY_OK) {
        ESP_LOGE(TAG, "Serialize event failed: %d", err);
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t send_err = ws_server_send(&ctx->ws_server, conn_fd, buf, out_len);
    if (send_err != ESP_OK) {
        ESP_LOGW(TAG, "Send event failed fd=%d: %d", conn_fd, send_err);
    }
    free(buf);
    return send_err;
}

extern int handle_event(relay_ctx_t *ctx, int conn_fd, nostr_event *event);
extern void handle_req(relay_ctx_t *ctx, int conn_fd, router_req_t *req);
extern int handle_close(relay_ctx_t *ctx, int conn_fd, const char *sub_id);

static const char *get_event_rejection_message(nostr_relay_error_t err)
{
    switch (err) {
        case NOSTR_RELAY_ERR_INVALID_SIG:
        case NOSTR_RELAY_ERR_SIG_MISMATCH:
            return NOSTR_OK_PREFIX_INVALID "bad signature";

        case NOSTR_RELAY_ERR_INVALID_ID:
        case NOSTR_RELAY_ERR_ID_MISMATCH:
            return NOSTR_OK_PREFIX_INVALID "bad event id";

        case NOSTR_RELAY_ERR_FUTURE_EVENT:
            return NOSTR_OK_PREFIX_INVALID "event too far in future";

        case NOSTR_RELAY_ERR_EXPIRED_EVENT:
            return NOSTR_OK_PREFIX_INVALID "event expired";

        case NOSTR_RELAY_ERR_STORAGE:
            return NOSTR_OK_PREFIX_ERROR "could not save event";

        default:
            return NOSTR_OK_PREFIX_ERROR "internal error";
    }
}

void router_dispatch(relay_ctx_t *ctx, int conn_fd, router_msg_t *msg)
{
    switch (msg->type) {
        case ROUTER_MSG_EVENT: {
            nostr_event *event = msg->data.event;
            ESP_LOGD(TAG, "EVENT fd=%d kind=%d", conn_fd, event->kind);

            int result = handle_event(ctx, conn_fd, event);

            char id_hex[65];
            nostr_event_get_id_hex(event, id_hex);

            bool accepted = (result == NOSTR_RELAY_OK);
            const char *message = accepted ? "" : get_event_rejection_message(result);
            router_send_ok(ctx, conn_fd, id_hex, accepted, message);
            break;
        }

        case ROUTER_MSG_REQ: {
            router_req_t *req = &msg->data.req;
            ESP_LOGD(TAG, "REQ fd=%d sub=%s filters=%zu", conn_fd, req->sub_id, req->filter_count);

            size_t sub_len = strlen(req->sub_id);
            if (sub_len == 0 || sub_len > ROUTER_MAX_SUB_ID) {
                router_send_closed(ctx, conn_fd, req->sub_id, "error: invalid subscription id");
                break;
            }

            if (req->filter_count > ROUTER_MAX_FILTERS) {
                router_send_closed(ctx, conn_fd, req->sub_id, "error: too many filters");
                break;
            }

            handle_req(ctx, conn_fd, req);
            break;
        }

        case ROUTER_MSG_CLOSE: {
            const char *sub_id = msg->data.close.sub_id;
            ESP_LOGD(TAG, "CLOSE fd=%d sub=%s", conn_fd, sub_id);

            int result = handle_close(ctx, conn_fd, sub_id);
            if (result == NOSTR_RELAY_OK) {
                router_send_closed(ctx, conn_fd, sub_id, "");
            }
            break;
        }

        case ROUTER_MSG_AUTH:
            router_send_notice(ctx, conn_fd, "AUTH not implemented");
            break;

        case ROUTER_MSG_UNKNOWN:
            router_send_notice(ctx, conn_fd, "unknown message type");
            break;

        case ROUTER_MSG_INVALID:
            router_send_notice(ctx, conn_fd, "invalid message format");
            break;
    }
}
