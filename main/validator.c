#include "validator.h"
#include "nostr.h"
#include "esp_log.h"
#include <inttypes.h>

static const char *TAG = "validator";

static int count_leading_zero_bits(const uint8_t *data, size_t len)
{
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0) {
            bits += 8;
        } else {
            uint8_t byte = data[i];
            while ((byte & 0x80) == 0) {
                bits++;
                byte <<= 1;
            }
            break;
        }
    }
    return bits;
}

static validation_result_t map_relay_error(nostr_relay_error_t err)
{
    switch (err) {
        case NOSTR_RELAY_OK:
            return VALIDATION_OK;
        case NOSTR_RELAY_ERR_MISSING_FIELD:
        case NOSTR_RELAY_ERR_INVALID_ID:
        case NOSTR_RELAY_ERR_INVALID_PUBKEY:
        case NOSTR_RELAY_ERR_INVALID_CREATED_AT:
        case NOSTR_RELAY_ERR_INVALID_KIND:
        case NOSTR_RELAY_ERR_INVALID_TAGS:
        case NOSTR_RELAY_ERR_INVALID_CONTENT:
            return VALIDATION_ERR_SCHEMA;
        case NOSTR_RELAY_ERR_ID_MISMATCH:
            return VALIDATION_ERR_ID;
        case NOSTR_RELAY_ERR_SIG_MISMATCH:
        case NOSTR_RELAY_ERR_INVALID_SIG:
            return VALIDATION_ERR_SIG;
        case NOSTR_RELAY_ERR_FUTURE_EVENT:
            return VALIDATION_ERR_FUTURE;
        case NOSTR_RELAY_ERR_EXPIRED_EVENT:
            return VALIDATION_ERR_EXPIRED;
        default:
            return VALIDATION_ERR_SCHEMA;
    }
}

static validation_result_t check_event_age(const nostr_event *event, uint32_t max_age_sec)
{
    if (max_age_sec == 0) {
        return VALIDATION_OK;
    }

    int64_t now = nostr_timestamp_now();
    int64_t age = now - event->created_at;

    if (age > (int64_t)max_age_sec) {
        ESP_LOGD(TAG, "Event too old: age=%lld max=%" PRIu32, (long long)age, max_age_sec);
        return VALIDATION_ERR_TOO_OLD;
    }

    return VALIDATION_OK;
}

validation_result_t validator_check_pow(const nostr_event *event, uint8_t min_difficulty)
{
    if (min_difficulty == 0) {
        return VALIDATION_OK;
    }

    int difficulty = count_leading_zero_bits(event->id, NOSTR_ID_SIZE);
    if (difficulty < min_difficulty) {
        ESP_LOGD(TAG, "Insufficient PoW: %d < %u", difficulty, min_difficulty);
        return VALIDATION_ERR_POW;
    }

    return VALIDATION_OK;
}

static validation_result_t check_duplicate(storage_engine_t *storage)
{
    if (!storage) {
        return VALIDATION_OK;
    }

    return VALIDATION_OK;
}

validation_result_t validator_check_event(
    const nostr_event *event,
    const validator_config_t *config,
    storage_engine_t *storage)
{
    validation_result_t result;
    nostr_validation_result_t libnostr_result;

    nostr_relay_error_t err = nostr_event_validate_full(event, config->max_future_sec, &libnostr_result);
    if (err != NOSTR_RELAY_OK) {
        ESP_LOGD(TAG, "libnostr validation failed: %s", libnostr_result.error_message);
        return map_relay_error(err);
    }

    result = check_event_age(event, config->max_event_age_sec);
    if (result != VALIDATION_OK) {
        return result;
    }

    result = validator_check_pow(event, config->min_pow_difficulty);
    if (result != VALIDATION_OK) {
        return result;
    }

    if (config->check_duplicates && !nostr_kind_is_ephemeral(event->kind)) {
        result = check_duplicate(storage);
        if (result != VALIDATION_OK) {
            return result;
        }
    }

    return VALIDATION_OK;
}

const char *validator_result_string(validation_result_t result)
{
    switch (result) {
        case VALIDATION_OK:
            return "ok";
        case VALIDATION_ERR_SCHEMA:
            return "invalid: missing or malformed fields";
        case VALIDATION_ERR_ID:
            return "invalid: event id does not match";
        case VALIDATION_ERR_SIG:
            return "invalid: signature verification failed";
        case VALIDATION_ERR_EXPIRED:
            return "invalid: event has expired";
        case VALIDATION_ERR_FUTURE:
            return "invalid: created_at too far in future";
        case VALIDATION_ERR_DUPLICATE:
            return "duplicate: event already exists";
        case VALIDATION_ERR_POW:
            return "pow: insufficient proof of work";
        case VALIDATION_ERR_BLOCKED:
            return "blocked: policy violation";
        case VALIDATION_ERR_TOO_OLD:
            return "invalid: event too old";
        default:
            return "error: unknown";
    }
}

const char *validator_result_prefix(validation_result_t result)
{
    switch (result) {
        case VALIDATION_OK:
            return "";
        case VALIDATION_ERR_DUPLICATE:
            return NOSTR_OK_PREFIX_DUPLICATE;
        case VALIDATION_ERR_POW:
            return NOSTR_OK_PREFIX_POW;
        case VALIDATION_ERR_BLOCKED:
            return NOSTR_OK_PREFIX_BLOCKED;
        default:
            return NOSTR_OK_PREFIX_INVALID;
    }
}

nostr_relay_error_t validator_result_to_relay_error(validation_result_t result)
{
    switch (result) {
        case VALIDATION_OK:
            return NOSTR_RELAY_OK;
        case VALIDATION_ERR_SIG:
            return NOSTR_RELAY_ERR_SIG_MISMATCH;
        case VALIDATION_ERR_ID:
            return NOSTR_RELAY_ERR_ID_MISMATCH;
        case VALIDATION_ERR_FUTURE:
            return NOSTR_RELAY_ERR_FUTURE_EVENT;
        case VALIDATION_ERR_EXPIRED:
        case VALIDATION_ERR_TOO_OLD:
            return NOSTR_RELAY_ERR_EXPIRED_EVENT;
        default:
            return NOSTR_RELAY_ERR_INVALID_JSON;
    }
}
