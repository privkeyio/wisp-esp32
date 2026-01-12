#include <stdio.h>
#include <string.h>
#include "test_fixtures.h"

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex) {
    for (size_t i = 0; i < len; i++) {
        sprintf(&hex[i*2], "%02x", bytes[i]);
    }
}

static bool matches_hex_prefix(const uint8_t *bytes, char **prefixes, size_t count) {
    char hex[65];
    bytes_to_hex(bytes, 32, hex);
    for (size_t i = 0; i < count; i++) {
        if (strncmp(hex, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

static bool matches_kind(int32_t *kinds, size_t count, uint16_t event_kind) {
    for (size_t i = 0; i < count; i++) {
        if (kinds[i] == (int32_t)event_kind) return true;
    }
    return false;
}

static bool nostr_filter_matches(const nostr_filter_t *filter, const nostr_event *event) {
    if (!filter || !event) return false;

    if (filter->kinds_count > 0 && !matches_kind(filter->kinds, filter->kinds_count, event->kind)) {
        return false;
    }
    if (filter->authors_count > 0 && !matches_hex_prefix(event->pubkey.data, filter->authors, filter->authors_count)) {
        return false;
    }
    if (filter->ids_count > 0 && !matches_hex_prefix(event->id, filter->ids, filter->ids_count)) {
        return false;
    }
    if (filter->since > 0 && event->created_at < filter->since) {
        return false;
    }
    if (filter->until > 0 && event->created_at > filter->until) {
        return false;
    }
    return true;
}

static bool nostr_filters_match(const nostr_filter_t *filters, size_t count, const nostr_event *event) {
    if (!filters || count == 0 || !event) return false;
    for (size_t i = 0; i < count; i++) {
        if (nostr_filter_matches(&filters[i], event)) {
            return true;
        }
    }
    return false;
}

void setUp(void) {}
void tearDown(void) {}

void test_filter_matches_kind(void) {
    nostr_filter_t filter = {0};
    filter.kinds = (int32_t*)malloc(2 * sizeof(int32_t));
    filter.kinds[0] = 1;
    filter.kinds[1] = 3;
    filter.kinds_count = 2;

    nostr_event *event = fixture_create_event(1, 1000);
    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->kind = 3;
    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->kind = 5;
    TEST_ASSERT_FALSE(nostr_filter_matches(&filter, event));

    fixture_free_event(event);
    nostr_filter_free(&filter);
}

void test_filter_matches_author(void) {
    nostr_event *event = fixture_create_event(1, 1000);
    nostr_filter_t filter = fixture_author_filter(event->pubkey.data);

    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->pubkey.data[0] ^= 0xFF;
    TEST_ASSERT_FALSE(nostr_filter_matches(&filter, event));

    fixture_free_event(event);
    nostr_filter_free(&filter);
}

void test_filter_matches_author_prefix(void) {
    nostr_event *event = fixture_create_event(1, 1000);

    nostr_filter_t filter = {0};
    filter.authors = (char**)malloc(sizeof(char*));
    filter.authors[0] = (char*)malloc(9);
    sprintf(filter.authors[0], "%02x%02x%02x%02x",
            event->pubkey.data[0], event->pubkey.data[1],
            event->pubkey.data[2], event->pubkey.data[3]);
    filter.authors_count = 1;

    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    fixture_free_event(event);
    nostr_filter_free(&filter);
}

void test_filter_matches_time_range(void) {
    nostr_filter_t filter = fixture_time_filter(1000, 2000);
    nostr_event *event = fixture_create_event(1, 1500);

    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->created_at = 500;
    TEST_ASSERT_FALSE(nostr_filter_matches(&filter, event));

    event->created_at = 2500;
    TEST_ASSERT_FALSE(nostr_filter_matches(&filter, event));

    event->created_at = 1000;
    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->created_at = 2000;
    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    fixture_free_event(event);
    nostr_filter_free(&filter);
}

void test_filter_since_only(void) {
    nostr_filter_t filter = {0};
    filter.since = 1000;
    nostr_event *event = fixture_create_event(1, 1500);

    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->created_at = 500;
    TEST_ASSERT_FALSE(nostr_filter_matches(&filter, event));

    fixture_free_event(event);
}

void test_filter_until_only(void) {
    nostr_filter_t filter = {0};
    filter.until = 2000;
    nostr_event *event = fixture_create_event(1, 1500);

    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->created_at = 2500;
    TEST_ASSERT_FALSE(nostr_filter_matches(&filter, event));

    fixture_free_event(event);
}

void test_empty_filter_matches_all(void) {
    nostr_filter_t filter = {0};
    nostr_event *event = fixture_create_event(1, 12345);

    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->kind = 0;
    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->kind = 30000;
    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    fixture_free_event(event);
}

void test_multiple_filters_or_logic(void) {
    nostr_filter_t filters[2];
    filters[0] = fixture_kinds_filter(1);
    filters[1] = fixture_kinds_filter(0);

    nostr_event *event = fixture_create_event(0, 1000);
    TEST_ASSERT_TRUE(nostr_filters_match(filters, 2, event));

    event->kind = 1;
    TEST_ASSERT_TRUE(nostr_filters_match(filters, 2, event));

    event->kind = 3;
    TEST_ASSERT_FALSE(nostr_filters_match(filters, 2, event));

    fixture_free_event(event);
    nostr_filter_free(&filters[0]);
    nostr_filter_free(&filters[1]);
}

void test_filter_combined_conditions(void) {
    nostr_event *event = fixture_create_event(1, 1500);

    nostr_filter_t filter = {0};
    filter.kinds = (int32_t*)malloc(sizeof(int32_t));
    filter.kinds[0] = 1;
    filter.kinds_count = 1;
    filter.since = 1000;
    filter.until = 2000;

    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->kind = 0;
    TEST_ASSERT_FALSE(nostr_filter_matches(&filter, event));

    event->kind = 1;
    event->created_at = 500;
    TEST_ASSERT_FALSE(nostr_filter_matches(&filter, event));

    fixture_free_event(event);
    nostr_filter_free(&filter);
}

void test_filter_null_handling(void) {
    nostr_filter_t filter = {0};
    nostr_event *event = fixture_create_event(1, 1000);

    TEST_ASSERT_FALSE(nostr_filter_matches(NULL, event));
    TEST_ASSERT_FALSE(nostr_filter_matches(&filter, NULL));
    TEST_ASSERT_FALSE(nostr_filter_matches(NULL, NULL));

    TEST_ASSERT_FALSE(nostr_filters_match(NULL, 0, event));
    TEST_ASSERT_FALSE(nostr_filters_match(&filter, 0, event));

    fixture_free_event(event);
}

void test_filter_matches_id_prefix(void) {
    nostr_event *event = fixture_create_event(1, 1000);

    nostr_filter_t filter = {0};
    filter.ids = (char**)malloc(sizeof(char*));
    filter.ids[0] = (char*)malloc(9);
    sprintf(filter.ids[0], "%02x%02x%02x%02x",
            event->id[0], event->id[1], event->id[2], event->id[3]);
    filter.ids_count = 1;

    TEST_ASSERT_TRUE(nostr_filter_matches(&filter, event));

    event->id[0] ^= 0xFF;
    TEST_ASSERT_FALSE(nostr_filter_matches(&filter, event));

    fixture_free_event(event);
    nostr_filter_free(&filter);
}

int main(void) {
    printf("=== Filter Matching Tests ===\n");
#ifdef HAVE_UNITY
    UNITY_BEGIN();
    RUN_TEST(test_filter_matches_kind);
    RUN_TEST(test_filter_matches_author);
    RUN_TEST(test_filter_matches_author_prefix);
    RUN_TEST(test_filter_matches_time_range);
    RUN_TEST(test_filter_since_only);
    RUN_TEST(test_filter_until_only);
    RUN_TEST(test_empty_filter_matches_all);
    RUN_TEST(test_multiple_filters_or_logic);
    RUN_TEST(test_filter_combined_conditions);
    RUN_TEST(test_filter_null_handling);
    RUN_TEST(test_filter_matches_id_prefix);
    return UNITY_END();
#else
    setUp();
    RUN_TEST(test_filter_matches_kind);
    setUp();
    RUN_TEST(test_filter_matches_author);
    setUp();
    RUN_TEST(test_filter_matches_author_prefix);
    setUp();
    RUN_TEST(test_filter_matches_time_range);
    setUp();
    RUN_TEST(test_filter_since_only);
    setUp();
    RUN_TEST(test_filter_until_only);
    setUp();
    RUN_TEST(test_empty_filter_matches_all);
    setUp();
    RUN_TEST(test_multiple_filters_or_logic);
    setUp();
    RUN_TEST(test_filter_combined_conditions);
    setUp();
    RUN_TEST(test_filter_null_handling);
    setUp();
    RUN_TEST(test_filter_matches_id_prefix);
    printf("\n=== All tests passed ===\n");
    return 0;
#endif
}
