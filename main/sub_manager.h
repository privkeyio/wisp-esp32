#ifndef SUB_MANAGER_H
#define SUB_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nostr_relay_protocol.h"

#define SUB_MAX_TOTAL         64
#define SUB_MAX_PER_CONN      8
#define SUB_MAX_FILTERS       4
#define SUB_MAX_ID_LEN        64

typedef struct {
    char sub_id[SUB_MAX_ID_LEN + 1];
    int conn_fd;
    nostr_filter_t filters[SUB_MAX_FILTERS];
    uint8_t filter_count;
    uint16_t events_sent;
    bool active;
} subscription_t;

typedef struct sub_manager {
    subscription_t subs[SUB_MAX_TOTAL];
    SemaphoreHandle_t lock;
    uint16_t active_count;
} sub_manager_t;

typedef struct {
    int conn_fd;
    char sub_id[SUB_MAX_ID_LEN + 1];
} sub_match_entry_t;

typedef struct {
    sub_match_entry_t matches[SUB_MAX_TOTAL];
    uint8_t count;
} sub_match_result_t;

esp_err_t sub_manager_init(sub_manager_t *mgr);
void sub_manager_destroy(sub_manager_t *mgr);

nostr_relay_error_t sub_manager_add(sub_manager_t *mgr, int conn_fd,
                                    const char *sub_id,
                                    const nostr_filter_t *filters,
                                    size_t filter_count);

nostr_relay_error_t sub_manager_remove(sub_manager_t *mgr, int conn_fd,
                                       const char *sub_id);

void sub_manager_remove_all(sub_manager_t *mgr, int conn_fd);

void sub_manager_match(sub_manager_t *mgr, const nostr_event *event,
                       sub_match_result_t *result);

uint8_t sub_manager_count(sub_manager_t *mgr, int conn_fd);

#endif
