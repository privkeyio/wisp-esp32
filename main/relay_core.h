#ifndef RELAY_CORE_H
#define RELAY_CORE_H

#include <stdint.h>

#include "ws_server.h"

typedef struct sub_manager sub_manager_t;
typedef struct storage_engine storage_engine_t;
typedef struct rate_limiter rate_limiter_t;

typedef struct relay_ctx {
    ws_server_t ws_server;
    sub_manager_t *sub_manager;
    storage_engine_t *storage;
    rate_limiter_t *rate_limiter;

    struct {
        uint16_t port;
        uint32_t max_event_age_sec;
        uint8_t max_subs_per_conn;
        uint8_t max_filters_per_sub;
        int64_t max_future_sec;
    } config;
} relay_ctx_t;

#endif
