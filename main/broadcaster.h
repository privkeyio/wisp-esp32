#ifndef BROADCASTER_H
#define BROADCASTER_H

#include "nostr_relay_protocol.h"

typedef struct relay_ctx relay_ctx_t;

void broadcaster_fanout(relay_ctx_t *ctx, const nostr_event *event);

#endif
