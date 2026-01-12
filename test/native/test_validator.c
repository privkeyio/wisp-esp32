#include <stdio.h>
#include <string.h>
#include "test_fixtures.h"

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

typedef struct {
    bool valid;
    nostr_relay_error_t error_code;
    char error_message[128];
    char error_field[32];
} nostr_validation_result_t;

static int64_t g_mock_now = 0;
static nostr_relay_error_t g_mock_validate_result = NOSTR_RELAY_OK;
static int g_mock_pow_difficulty = 0;

static int64_t nostr_timestamp_now(void) {
    return g_mock_now;
}

static nostr_relay_error_t nostr_event_validate_full(const nostr_event *event, int64_t max_future_sec, nostr_validation_result_t *result) {
    if (!event || !result) {
        if (result) {
            result->valid = false;
            result->error_code = NOSTR_RELAY_ERR_MISSING_FIELD;
        }
        return NOSTR_RELAY_ERR_MISSING_FIELD;
    }
    result->valid = (g_mock_validate_result == NOSTR_RELAY_OK);
    result->error_code = g_mock_validate_result;
    result->error_message[0] = '\0';

    if (max_future_sec > 0 && event->created_at > g_mock_now + max_future_sec) {
        result->valid = false;
        result->error_code = NOSTR_RELAY_ERR_FUTURE_EVENT;
        return NOSTR_RELAY_ERR_FUTURE_EVENT;
    }
    return g_mock_validate_result;
}

static int nostr_nip13_calculate_difficulty(const uint8_t *id) {
    (void)id;
    return g_mock_pow_difficulty;
}

typedef struct storage_engine storage_engine_t;

static validation_result_t map_relay_error(nostr_relay_error_t err) {
    switch (err) {
        case NOSTR_RELAY_OK: return VALIDATION_OK;
        case NOSTR_RELAY_ERR_ID_MISMATCH: return VALIDATION_ERR_ID;
        case NOSTR_RELAY_ERR_SIG_MISMATCH:
        case NOSTR_RELAY_ERR_INVALID_SIG: return VALIDATION_ERR_SIG;
        case NOSTR_RELAY_ERR_FUTURE_EVENT: return VALIDATION_ERR_FUTURE;
        case NOSTR_RELAY_ERR_EXPIRED_EVENT: return VALIDATION_ERR_EXPIRED;
        default: return VALIDATION_ERR_SCHEMA;
    }
}

static validation_result_t check_event_age(const nostr_event *event, uint32_t max_age_sec) {
    if (max_age_sec == 0) return VALIDATION_OK;
    int64_t now = nostr_timestamp_now();
    int64_t age = now - event->created_at;
    if (age > (int64_t)max_age_sec) return VALIDATION_ERR_TOO_OLD;
    return VALIDATION_OK;
}

static validation_result_t validator_check_pow(const nostr_event *event, uint8_t min_difficulty) {
    if (min_difficulty == 0) return VALIDATION_OK;
    int difficulty = nostr_nip13_calculate_difficulty(event->id);
    if (difficulty < min_difficulty) return VALIDATION_ERR_POW;
    return VALIDATION_OK;
}

static validation_result_t validator_check_event(
    const nostr_event *event,
    const validator_config_t *config,
    storage_engine_t *storage) {
    (void)storage;
    validation_result_t result;
    nostr_validation_result_t libnostr_result;

    nostr_relay_error_t err = nostr_event_validate_full(event, config->max_future_sec, &libnostr_result);
    if (err != NOSTR_RELAY_OK) return map_relay_error(err);

    result = check_event_age(event, config->max_event_age_sec);
    if (result != VALIDATION_OK) return result;

    result = validator_check_pow(event, config->min_pow_difficulty);
    if (result != VALIDATION_OK) return result;

    return VALIDATION_OK;
}

static const char *validator_result_string(validation_result_t result) {
    switch (result) {
        case VALIDATION_OK: return "ok";
        case VALIDATION_ERR_SCHEMA: return "invalid: missing or malformed fields";
        case VALIDATION_ERR_ID: return "invalid: event id does not match";
        case VALIDATION_ERR_SIG: return "invalid: signature verification failed";
        case VALIDATION_ERR_EXPIRED: return "invalid: event has expired";
        case VALIDATION_ERR_FUTURE: return "invalid: created_at too far in future";
        case VALIDATION_ERR_DUPLICATE: return "duplicate: event already exists";
        case VALIDATION_ERR_POW: return "pow: insufficient proof of work";
        case VALIDATION_ERR_BLOCKED: return "blocked: policy violation";
        case VALIDATION_ERR_TOO_OLD: return "invalid: event too old";
        default: return "error: unknown";
    }
}

void setUp(void) {
    g_mock_now = 1700000000;
    g_mock_validate_result = NOSTR_RELAY_OK;
    g_mock_pow_difficulty = 0;
}

void tearDown(void) {}

void test_validator_accepts_valid_event(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 60);
    validator_config_t config = {
        .max_event_age_sec = 86400,
        .max_future_sec = 900,
        .check_duplicates = false,
    };
    TEST_ASSERT_EQUAL(VALIDATION_OK, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

void test_validator_rejects_bad_signature(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 60);
    g_mock_validate_result = NOSTR_RELAY_ERR_SIG_MISMATCH;
    validator_config_t config = { .check_duplicates = false };
    TEST_ASSERT_EQUAL(VALIDATION_ERR_SIG, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

void test_validator_rejects_bad_id(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 60);
    g_mock_validate_result = NOSTR_RELAY_ERR_ID_MISMATCH;
    validator_config_t config = { .check_duplicates = false };
    TEST_ASSERT_EQUAL(VALIDATION_ERR_ID, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

void test_validator_rejects_future_event(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now + 3600);
    validator_config_t config = { .max_future_sec = 900 };
    TEST_ASSERT_EQUAL(VALIDATION_ERR_FUTURE, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

void test_validator_accepts_slightly_future_event(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now + 300);
    validator_config_t config = { .max_future_sec = 900 };
    TEST_ASSERT_EQUAL(VALIDATION_OK, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

void test_validator_rejects_too_old_event(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 100000);
    validator_config_t config = {
        .max_event_age_sec = 86400,
        .max_future_sec = 900,
    };
    TEST_ASSERT_EQUAL(VALIDATION_ERR_TOO_OLD, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

void test_validator_allows_old_event_when_disabled(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 100000);
    validator_config_t config = {
        .max_event_age_sec = 0,
        .max_future_sec = 900,
    };
    TEST_ASSERT_EQUAL(VALIDATION_OK, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

void test_validator_rejects_insufficient_pow(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 60);
    g_mock_pow_difficulty = 10;
    validator_config_t config = {
        .min_pow_difficulty = 20,
        .max_future_sec = 900,
    };
    TEST_ASSERT_EQUAL(VALIDATION_ERR_POW, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

void test_validator_accepts_sufficient_pow(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 60);
    g_mock_pow_difficulty = 25;
    validator_config_t config = {
        .min_pow_difficulty = 20,
        .max_future_sec = 900,
    };
    TEST_ASSERT_EQUAL(VALIDATION_OK, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

void test_validator_skips_pow_when_disabled(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 60);
    g_mock_pow_difficulty = 0;
    validator_config_t config = {
        .min_pow_difficulty = 0,
        .max_future_sec = 900,
    };
    TEST_ASSERT_EQUAL(VALIDATION_OK, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

void test_validator_result_strings(void) {
    TEST_ASSERT_EQUAL_STRING("ok", validator_result_string(VALIDATION_OK));
    TEST_ASSERT_EQUAL_STRING("invalid: signature verification failed", validator_result_string(VALIDATION_ERR_SIG));
    TEST_ASSERT_EQUAL_STRING("invalid: event id does not match", validator_result_string(VALIDATION_ERR_ID));
    TEST_ASSERT_EQUAL_STRING("invalid: created_at too far in future", validator_result_string(VALIDATION_ERR_FUTURE));
    TEST_ASSERT_EQUAL_STRING("invalid: event too old", validator_result_string(VALIDATION_ERR_TOO_OLD));
    TEST_ASSERT_EQUAL_STRING("pow: insufficient proof of work", validator_result_string(VALIDATION_ERR_POW));
}

void test_validator_rejects_schema_errors(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 60);
    g_mock_validate_result = NOSTR_RELAY_ERR_INVALID_KIND;
    validator_config_t config = { .check_duplicates = false };
    TEST_ASSERT_EQUAL(VALIDATION_ERR_SCHEMA, validator_check_event(event, &config, NULL));
    fixture_free_event(event);
}

int main(void) {
    printf("=== Validator Tests ===\n");
#ifdef HAVE_UNITY
    UNITY_BEGIN();
    RUN_TEST(test_validator_accepts_valid_event);
    RUN_TEST(test_validator_rejects_bad_signature);
    RUN_TEST(test_validator_rejects_bad_id);
    RUN_TEST(test_validator_rejects_future_event);
    RUN_TEST(test_validator_accepts_slightly_future_event);
    RUN_TEST(test_validator_rejects_too_old_event);
    RUN_TEST(test_validator_allows_old_event_when_disabled);
    RUN_TEST(test_validator_rejects_insufficient_pow);
    RUN_TEST(test_validator_accepts_sufficient_pow);
    RUN_TEST(test_validator_skips_pow_when_disabled);
    RUN_TEST(test_validator_result_strings);
    RUN_TEST(test_validator_rejects_schema_errors);
    return UNITY_END();
#else
    setUp();
    RUN_TEST(test_validator_accepts_valid_event);
    setUp();
    RUN_TEST(test_validator_rejects_bad_signature);
    setUp();
    RUN_TEST(test_validator_rejects_bad_id);
    setUp();
    RUN_TEST(test_validator_rejects_future_event);
    setUp();
    RUN_TEST(test_validator_accepts_slightly_future_event);
    setUp();
    RUN_TEST(test_validator_rejects_too_old_event);
    setUp();
    RUN_TEST(test_validator_allows_old_event_when_disabled);
    setUp();
    RUN_TEST(test_validator_rejects_insufficient_pow);
    setUp();
    RUN_TEST(test_validator_accepts_sufficient_pow);
    setUp();
    RUN_TEST(test_validator_skips_pow_when_disabled);
    setUp();
    RUN_TEST(test_validator_result_strings);
    setUp();
    RUN_TEST(test_validator_rejects_schema_errors);
    printf("\n=== All tests passed ===\n");
    return 0;
#endif
}
