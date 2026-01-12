#ifndef ROUTER_H
#define ROUTER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "nostr_relay_protocol.h"

typedef struct relay_ctx relay_ctx_t;

typedef enum {
    ROUTER_MSG_EVENT,
    ROUTER_MSG_REQ,
    ROUTER_MSG_CLOSE,
    ROUTER_MSG_AUTH,
    ROUTER_MSG_UNKNOWN,
    ROUTER_MSG_INVALID
} router_msg_type_t;

#define ROUTER_MAX_FILTERS 4
#define ROUTER_MAX_SUB_ID  64

typedef struct {
    char sub_id[ROUTER_MAX_SUB_ID + 1];
    nostr_filter_t *filters;
    size_t filter_count;
} router_req_t;

typedef struct {
    char sub_id[ROUTER_MAX_SUB_ID + 1];
} router_close_t;

typedef struct {
    router_msg_type_t type;
    union {
        nostr_event *event;
        router_req_t req;
        router_close_t close;
    } data;
} router_msg_t;

nostr_relay_error_t router_parse(const char *json, size_t len, router_msg_t *out);

void router_msg_free(router_msg_t *msg);

void router_dispatch(relay_ctx_t *ctx, int conn_fd, router_msg_t *msg);

esp_err_t router_send_notice(relay_ctx_t *ctx, int conn_fd, const char *message);

esp_err_t router_send_ok(relay_ctx_t *ctx, int conn_fd, const char *event_id_hex,
                         bool accepted, const char *message);

esp_err_t router_send_eose(relay_ctx_t *ctx, int conn_fd, const char *sub_id);

esp_err_t router_send_closed(relay_ctx_t *ctx, int conn_fd, const char *sub_id,
                             const char *message);

esp_err_t router_send_event(relay_ctx_t *ctx, int conn_fd, const char *sub_id,
                            const nostr_event *event);

#endif
