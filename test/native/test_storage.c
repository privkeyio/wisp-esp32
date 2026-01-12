#include <stdio.h>
#include <string.h>
#include "test_fixtures.h"

typedef enum {
    STORAGE_OK = 0,
    STORAGE_ERR_NOT_INITIALIZED,
    STORAGE_ERR_FULL,
    STORAGE_ERR_DUPLICATE,
    STORAGE_ERR_NOT_FOUND,
    STORAGE_ERR_IO,
    STORAGE_ERR_NO_MEM,
    STORAGE_ERR_SERIALIZE
} storage_error_t;

#define STORAGE_MAX_EVENTS         100
#define STORAGE_FLAG_DELETED       0x01

typedef struct __attribute__((packed)) {
    uint8_t  event_id[32];
    uint32_t created_at;
    uint32_t expires_at;
    uint32_t file_index;
    uint16_t kind;
    uint8_t  pubkey_prefix[4];
    uint8_t  flags;
    uint8_t  reserved;
} storage_index_entry_t;

typedef struct {
    uint32_t total_events;
    uint32_t total_bytes;
    uint32_t free_bytes;
    uint32_t oldest_event_ts;
    uint32_t newest_event_ts;
} storage_stats_t;

typedef struct storage_engine {
    storage_index_entry_t index[STORAGE_MAX_EVENTS];
    nostr_event *events[STORAGE_MAX_EVENTS];
    uint16_t index_count;
    uint32_t next_file_index;
    SemaphoreHandle_t lock;
    bool initialized;
    uint32_t default_ttl_sec;
} storage_engine_t;

static int64_t g_mock_now = 0;

static esp_err_t storage_init(storage_engine_t *engine, uint32_t default_ttl_sec) {
    memset(engine, 0, sizeof(storage_engine_t));
    engine->lock = xSemaphoreCreateMutex();
    if (!engine->lock) return ESP_FAIL;
    engine->default_ttl_sec = default_ttl_sec;
    engine->initialized = true;
    return ESP_OK;
}

static void storage_destroy(storage_engine_t *engine) {
    if (!engine) return;
    for (int i = 0; i < STORAGE_MAX_EVENTS; i++) {
        if (engine->events[i]) {
            fixture_free_event(engine->events[i]);
            engine->events[i] = NULL;
        }
    }
    if (engine->lock) {
        vSemaphoreDelete(engine->lock);
        engine->lock = NULL;
    }
    engine->initialized = false;
}

static int find_event_index(storage_engine_t *engine, const uint8_t event_id[32]) {
    for (int i = 0; i < engine->index_count; i++) {
        if (!(engine->index[i].flags & STORAGE_FLAG_DELETED) &&
            memcmp(engine->index[i].event_id, event_id, 32) == 0) {
            return i;
        }
    }
    return -1;
}

static bool storage_event_exists(storage_engine_t *engine, const uint8_t event_id[32]) {
    xSemaphoreTake(engine->lock, portMAX_DELAY);
    int idx = find_event_index(engine, event_id);
    xSemaphoreGive(engine->lock);
    return idx >= 0;
}

static storage_error_t storage_save_event(storage_engine_t *engine, const nostr_event *event) {
    if (!engine->initialized) return STORAGE_ERR_NOT_INITIALIZED;

    xSemaphoreTake(engine->lock, portMAX_DELAY);

    if (find_event_index(engine, event->id) >= 0) {
        xSemaphoreGive(engine->lock);
        return STORAGE_ERR_DUPLICATE;
    }

    int slot = -1;
    for (int i = 0; i < engine->index_count; i++) {
        if (engine->index[i].flags & STORAGE_FLAG_DELETED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (engine->index_count >= STORAGE_MAX_EVENTS) {
            xSemaphoreGive(engine->lock);
            return STORAGE_ERR_FULL;
        }
        slot = engine->index_count++;
    }

    nostr_event *copy = (nostr_event*)calloc(1, sizeof(nostr_event));
    if (!copy) {
        xSemaphoreGive(engine->lock);
        return STORAGE_ERR_NO_MEM;
    }
    memcpy(copy, event, sizeof(nostr_event));
    if (event->content) {
        copy->content = strdup(event->content);
    }

    memcpy(engine->index[slot].event_id, event->id, 32);
    engine->index[slot].created_at = (uint32_t)event->created_at;
    engine->index[slot].expires_at = engine->default_ttl_sec > 0 ?
        (uint32_t)event->created_at + engine->default_ttl_sec : 0;
    engine->index[slot].kind = event->kind;
    memcpy(engine->index[slot].pubkey_prefix, event->pubkey.data, 4);
    engine->index[slot].flags = 0;
    engine->index[slot].file_index = engine->next_file_index++;

    engine->events[slot] = copy;

    xSemaphoreGive(engine->lock);
    return STORAGE_OK;
}

static storage_error_t storage_delete_event(storage_engine_t *engine, const uint8_t event_id[32]) {
    xSemaphoreTake(engine->lock, portMAX_DELAY);

    int idx = find_event_index(engine, event_id);
    if (idx < 0) {
        xSemaphoreGive(engine->lock);
        return STORAGE_ERR_NOT_FOUND;
    }

    engine->index[idx].flags |= STORAGE_FLAG_DELETED;
    if (engine->events[idx]) {
        fixture_free_event(engine->events[idx]);
        engine->events[idx] = NULL;
    }

    xSemaphoreGive(engine->lock);
    return STORAGE_OK;
}

static storage_error_t storage_query_events(storage_engine_t *engine,
                                            const nostr_filter_t *filter,
                                            nostr_event ***results,
                                            uint16_t *count,
                                            uint16_t limit) {
    xSemaphoreTake(engine->lock, portMAX_DELAY);

    nostr_event **res = (nostr_event**)calloc(limit, sizeof(nostr_event*));
    if (!res) {
        xSemaphoreGive(engine->lock);
        return STORAGE_ERR_NO_MEM;
    }

    uint16_t matched = 0;
    for (int i = 0; i < engine->index_count && matched < limit; i++) {
        storage_index_entry_t *entry = &engine->index[i];
        if (entry->flags & STORAGE_FLAG_DELETED) continue;
        if (!engine->events[i]) continue;

        if (filter->kinds_count > 0) {
            bool kind_match = false;
            for (size_t k = 0; k < filter->kinds_count && !kind_match; k++) {
                kind_match = (filter->kinds[k] == entry->kind);
            }
            if (!kind_match) continue;
        }
        if (filter->since > 0 && entry->created_at < (uint32_t)filter->since) continue;
        if (filter->until > 0 && entry->created_at > (uint32_t)filter->until) continue;

        res[matched++] = engine->events[i];
    }

    *results = res;
    *count = matched;

    xSemaphoreGive(engine->lock);
    return STORAGE_OK;
}

static void storage_free_query_results(nostr_event **results, uint16_t count) {
    (void)count;
    free(results);
}

static int storage_purge_expired(storage_engine_t *engine) {
    xSemaphoreTake(engine->lock, portMAX_DELAY);

    int purged = 0;
    uint32_t now = (uint32_t)g_mock_now;

    for (int i = 0; i < engine->index_count; i++) {
        if (engine->index[i].flags & STORAGE_FLAG_DELETED) continue;
        if (engine->index[i].expires_at > 0 && engine->index[i].expires_at <= now) {
            engine->index[i].flags |= STORAGE_FLAG_DELETED;
            if (engine->events[i]) {
                fixture_free_event(engine->events[i]);
                engine->events[i] = NULL;
            }
            purged++;
        }
    }

    xSemaphoreGive(engine->lock);
    return purged;
}

static storage_engine_t g_storage;

void setUp(void) {
    g_mock_now = 1700000000;
    storage_init(&g_storage, 86400);
}

void tearDown(void) {
    storage_destroy(&g_storage);
}

void test_storage_save_and_retrieve(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 60);

    TEST_ASSERT_EQUAL(STORAGE_OK, storage_save_event(&g_storage, event));
    TEST_ASSERT_TRUE(storage_event_exists(&g_storage, event->id));

    fixture_free_event(event);
}

void test_storage_rejects_duplicate(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 60);

    TEST_ASSERT_EQUAL(STORAGE_OK, storage_save_event(&g_storage, event));
    TEST_ASSERT_EQUAL(STORAGE_ERR_DUPLICATE, storage_save_event(&g_storage, event));

    fixture_free_event(event);
}

void test_storage_query_by_kind(void) {
    for (int k = 0; k < 5; k++) {
        nostr_event *event = fixture_create_event(k, g_mock_now - 60);
        storage_save_event(&g_storage, event);
        fixture_free_event(event);
    }

    nostr_filter_t filter = fixture_kinds_filter(1);
    nostr_event **results;
    uint16_t count;

    TEST_ASSERT_EQUAL(STORAGE_OK, storage_query_events(&g_storage, &filter, &results, &count, 100));
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(1, results[0]->kind);

    storage_free_query_results(results, count);
    nostr_filter_free(&filter);
}

void test_storage_query_multiple_kinds(void) {
    for (int k = 0; k < 5; k++) {
        nostr_event *event = fixture_create_event(k, g_mock_now - 60);
        storage_save_event(&g_storage, event);
        fixture_free_event(event);
    }

    nostr_filter_t filter = {0};
    filter.kinds = (int32_t*)malloc(2 * sizeof(int32_t));
    filter.kinds[0] = 1;
    filter.kinds[1] = 3;
    filter.kinds_count = 2;

    nostr_event **results;
    uint16_t count;

    TEST_ASSERT_EQUAL(STORAGE_OK, storage_query_events(&g_storage, &filter, &results, &count, 100));
    TEST_ASSERT_EQUAL(2, count);

    storage_free_query_results(results, count);
    nostr_filter_free(&filter);
}

void test_storage_query_time_range(void) {
    for (int i = 0; i < 5; i++) {
        nostr_event *event = fixture_create_event(1, g_mock_now - 1000 + i * 100);
        storage_save_event(&g_storage, event);
        fixture_free_event(event);
    }

    nostr_filter_t filter = fixture_time_filter(g_mock_now - 800, g_mock_now - 600);
    nostr_event **results;
    uint16_t count;

    TEST_ASSERT_EQUAL(STORAGE_OK, storage_query_events(&g_storage, &filter, &results, &count, 100));
    TEST_ASSERT_EQUAL(3, count);

    storage_free_query_results(results, count);
    nostr_filter_free(&filter);
}

void test_storage_delete_event(void) {
    nostr_event *event = fixture_create_event(1, g_mock_now - 60);

    storage_save_event(&g_storage, event);
    TEST_ASSERT_TRUE(storage_event_exists(&g_storage, event->id));

    TEST_ASSERT_EQUAL(STORAGE_OK, storage_delete_event(&g_storage, event->id));
    TEST_ASSERT_FALSE(storage_event_exists(&g_storage, event->id));

    fixture_free_event(event);
}

void test_storage_delete_nonexistent(void) {
    uint8_t fake_id[32] = {0};
    TEST_ASSERT_EQUAL(STORAGE_ERR_NOT_FOUND, storage_delete_event(&g_storage, fake_id));
}

void test_storage_purge_expired(void) {
    nostr_event *event1 = fixture_create_event(1, g_mock_now - 100000);
    nostr_event *event2 = fixture_create_event(1, g_mock_now - 60);

    storage_save_event(&g_storage, event1);
    storage_save_event(&g_storage, event2);

    g_mock_now += 50000;
    int purged = storage_purge_expired(&g_storage);

    TEST_ASSERT_EQUAL(1, purged);
    TEST_ASSERT_FALSE(storage_event_exists(&g_storage, event1->id));
    TEST_ASSERT_TRUE(storage_event_exists(&g_storage, event2->id));

    fixture_free_event(event1);
    fixture_free_event(event2);
}

void test_storage_limit_results(void) {
    for (int i = 0; i < 10; i++) {
        nostr_event *event = fixture_create_event(1, g_mock_now - 60 + i);
        storage_save_event(&g_storage, event);
        fixture_free_event(event);
    }

    nostr_filter_t filter = {0};
    nostr_event **results;
    uint16_t count;

    TEST_ASSERT_EQUAL(STORAGE_OK, storage_query_events(&g_storage, &filter, &results, &count, 5));
    TEST_ASSERT_EQUAL(5, count);

    storage_free_query_results(results, count);
}

void test_storage_full(void) {
    for (int i = 0; i < STORAGE_MAX_EVENTS; i++) {
        nostr_event *event = fixture_create_event(1, g_mock_now - 60 + i);
        TEST_ASSERT_EQUAL(STORAGE_OK, storage_save_event(&g_storage, event));
        fixture_free_event(event);
    }

    nostr_event *overflow = fixture_create_event(1, g_mock_now);
    TEST_ASSERT_EQUAL(STORAGE_ERR_FULL, storage_save_event(&g_storage, overflow));
    fixture_free_event(overflow);
}

void test_storage_reuses_deleted_slots(void) {
    for (int i = 0; i < STORAGE_MAX_EVENTS; i++) {
        nostr_event *event = fixture_create_event(1, g_mock_now - 60 + i);
        storage_save_event(&g_storage, event);
        fixture_free_event(event);
    }

    nostr_filter_t filter = {0};
    nostr_event **results;
    uint16_t count;
    storage_query_events(&g_storage, &filter, &results, &count, 1);

    uint8_t id_to_delete[32];
    memcpy(id_to_delete, results[0]->id, 32);
    storage_free_query_results(results, count);

    storage_delete_event(&g_storage, id_to_delete);

    nostr_event *new_event = fixture_create_event(1, g_mock_now);
    TEST_ASSERT_EQUAL(STORAGE_OK, storage_save_event(&g_storage, new_event));
    fixture_free_event(new_event);
}

int main(void) {
    printf("=== Storage Tests ===\n");
#ifdef HAVE_UNITY
    UNITY_BEGIN();
    RUN_TEST(test_storage_save_and_retrieve);
    RUN_TEST(test_storage_rejects_duplicate);
    RUN_TEST(test_storage_query_by_kind);
    RUN_TEST(test_storage_query_multiple_kinds);
    RUN_TEST(test_storage_query_time_range);
    RUN_TEST(test_storage_delete_event);
    RUN_TEST(test_storage_delete_nonexistent);
    RUN_TEST(test_storage_purge_expired);
    RUN_TEST(test_storage_limit_results);
    RUN_TEST(test_storage_full);
    RUN_TEST(test_storage_reuses_deleted_slots);
    return UNITY_END();
#else
    setUp();
    RUN_TEST(test_storage_save_and_retrieve);
    tearDown(); setUp();
    RUN_TEST(test_storage_rejects_duplicate);
    tearDown(); setUp();
    RUN_TEST(test_storage_query_by_kind);
    tearDown(); setUp();
    RUN_TEST(test_storage_query_multiple_kinds);
    tearDown(); setUp();
    RUN_TEST(test_storage_query_time_range);
    tearDown(); setUp();
    RUN_TEST(test_storage_delete_event);
    tearDown(); setUp();
    RUN_TEST(test_storage_delete_nonexistent);
    tearDown(); setUp();
    RUN_TEST(test_storage_purge_expired);
    tearDown(); setUp();
    RUN_TEST(test_storage_limit_results);
    tearDown(); setUp();
    RUN_TEST(test_storage_full);
    tearDown(); setUp();
    RUN_TEST(test_storage_reuses_deleted_slots);
    tearDown();
    printf("\n=== All tests passed ===\n");
    return 0;
#endif
}
