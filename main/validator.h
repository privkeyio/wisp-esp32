#ifndef VALIDATOR_H
#define VALIDATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "nostr_relay_protocol.h"

typedef enum {
    VALIDATION_OK = 0,
    VALIDATION_ERR_SCHEMA,
    VALIDATION_ERR_ID,
    VALIDATION_ERR_SIG,
    VALIDATION_ERR_EXPIRED,
    VALIDATION_ERR_FUTURE,
    VALIDATION_ERR_DUPLICATE,
    VALIDATION_ERR_POW,
    VALIDATION_ERR_BLOCKED,
    VALIDATION_ERR_TOO_OLD,
} validation_result_t;

typedef struct {
    uint32_t max_event_age_sec;
    int64_t max_future_sec;
    uint8_t min_pow_difficulty;
    bool check_duplicates;
} validator_config_t;

typedef struct storage_engine storage_engine_t;

validation_result_t validator_check_event(
    const nostr_event *event,
    const validator_config_t *config,
    storage_engine_t *storage
);

validation_result_t validator_check_pow(const nostr_event *event, uint8_t min_difficulty);

const char *validator_result_string(validation_result_t result);

const char *validator_result_prefix(validation_result_t result);

nostr_relay_error_t validator_result_to_relay_error(validation_result_t result);

#endif
