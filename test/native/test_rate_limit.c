#include <stdio.h>
#include <string.h>
#include "test_fixtures.h"

#define RATE_LIMITER_MAX_BUCKETS 16

typedef enum {
    RATE_TYPE_EVENT,
    RATE_TYPE_REQ,
} rate_type_t;

typedef struct {
    uint16_t events_per_minute;
    uint16_t reqs_per_minute;
} rate_config_t;

typedef struct {
    int      fd;
    uint16_t event_count;
    uint16_t req_count;
    uint32_t window_start;
    bool     active;
} rate_bucket_t;

typedef struct rate_limiter {
    rate_config_t config;
    rate_bucket_t buckets[RATE_LIMITER_MAX_BUCKETS];
    SemaphoreHandle_t lock;
} rate_limiter_t;

static uint32_t g_mock_time = 0;

static uint32_t mock_get_time(void) {
    return g_mock_time;
}

static void rate_limiter_init(rate_limiter_t *rl, const rate_config_t *config) {
    memset(rl, 0, sizeof(rate_limiter_t));
    rl->lock = xSemaphoreCreateMutex();
    if (config) {
        memcpy(&rl->config, config, sizeof(rate_config_t));
    } else {
        rl->config.events_per_minute = 30;
        rl->config.reqs_per_minute = 60;
    }
}

static rate_bucket_t* get_bucket(rate_limiter_t *rl, int fd) {
    for (int i = 0; i < RATE_LIMITER_MAX_BUCKETS; i++) {
        if (rl->buckets[i].active && rl->buckets[i].fd == fd) {
            return &rl->buckets[i];
        }
    }
    for (int i = 0; i < RATE_LIMITER_MAX_BUCKETS; i++) {
        if (!rl->buckets[i].active) {
            rl->buckets[i].fd = fd;
            rl->buckets[i].active = true;
            rl->buckets[i].event_count = 0;
            rl->buckets[i].req_count = 0;
            rl->buckets[i].window_start = mock_get_time();
            return &rl->buckets[i];
        }
    }
    return NULL;
}

static bool rate_limiter_check(rate_limiter_t *rl, int fd, rate_type_t type) {
    xSemaphoreTake(rl->lock, portMAX_DELAY);

    rate_bucket_t *bucket = get_bucket(rl, fd);
    if (!bucket) {
        xSemaphoreGive(rl->lock);
        return false;
    }

    uint32_t now = mock_get_time();

    if (now - bucket->window_start >= 60) {
        bucket->event_count = 0;
        bucket->req_count = 0;
        bucket->window_start = now;
    }

    bool allowed = true;
    if (type == RATE_TYPE_EVENT) {
        if (bucket->event_count >= rl->config.events_per_minute) {
            allowed = false;
        } else {
            bucket->event_count++;
        }
    } else {
        if (bucket->req_count >= rl->config.reqs_per_minute) {
            allowed = false;
        } else {
            bucket->req_count++;
        }
    }

    xSemaphoreGive(rl->lock);
    return allowed;
}

static void rate_limiter_reset(rate_limiter_t *rl, int fd) {
    xSemaphoreTake(rl->lock, portMAX_DELAY);
    for (int i = 0; i < RATE_LIMITER_MAX_BUCKETS; i++) {
        if (rl->buckets[i].active && rl->buckets[i].fd == fd) {
            rl->buckets[i].active = false;
            break;
        }
    }
    xSemaphoreGive(rl->lock);
}

static rate_limiter_t g_rl;

void setUp(void) {
    g_mock_time = 1000;
    rate_config_t config = { .events_per_minute = 30, .reqs_per_minute = 60 };
    rate_limiter_init(&g_rl, &config);
}

void tearDown(void) {
    if (g_rl.lock) {
        vSemaphoreDelete(g_rl.lock);
        g_rl.lock = NULL;
    }
}

void test_rate_limiter_allows_initial_events(void) {
    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));
    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));
    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));
}

void test_rate_limiter_allows_initial_reqs(void) {
    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_REQ));
    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_REQ));
    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_REQ));
}

void test_rate_limiter_blocks_excess_events(void) {
    for (int i = 0; i < 30; i++) {
        TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));
    }
    TEST_ASSERT_FALSE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));
    TEST_ASSERT_FALSE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));
}

void test_rate_limiter_blocks_excess_reqs(void) {
    for (int i = 0; i < 60; i++) {
        TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_REQ));
    }
    TEST_ASSERT_FALSE(rate_limiter_check(&g_rl, 1, RATE_TYPE_REQ));
    TEST_ASSERT_FALSE(rate_limiter_check(&g_rl, 1, RATE_TYPE_REQ));
}

void test_rate_limiter_separate_counters(void) {
    for (int i = 0; i < 30; i++) {
        rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT);
    }
    TEST_ASSERT_FALSE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));
    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_REQ));
}

void test_rate_limiter_per_connection(void) {
    for (int i = 0; i < 30; i++) {
        rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT);
    }
    TEST_ASSERT_FALSE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));
    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 2, RATE_TYPE_EVENT));
}

void test_rate_limiter_window_reset(void) {
    for (int i = 0; i < 30; i++) {
        rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT);
    }
    TEST_ASSERT_FALSE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));

    g_mock_time += 60;
    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));
}

void test_rate_limiter_reset_clears_bucket(void) {
    for (int i = 0; i < 30; i++) {
        rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT);
    }
    TEST_ASSERT_FALSE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));

    rate_limiter_reset(&g_rl, 1);

    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 1, RATE_TYPE_EVENT));
}

void test_rate_limiter_max_buckets(void) {
    for (int i = 0; i < RATE_LIMITER_MAX_BUCKETS; i++) {
        TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, i + 100, RATE_TYPE_EVENT));
    }
    TEST_ASSERT_FALSE(rate_limiter_check(&g_rl, 200, RATE_TYPE_EVENT));
}

void test_rate_limiter_bucket_reuse(void) {
    for (int i = 0; i < RATE_LIMITER_MAX_BUCKETS; i++) {
        rate_limiter_check(&g_rl, i + 100, RATE_TYPE_EVENT);
    }
    TEST_ASSERT_FALSE(rate_limiter_check(&g_rl, 200, RATE_TYPE_EVENT));

    rate_limiter_reset(&g_rl, 100);

    TEST_ASSERT_TRUE(rate_limiter_check(&g_rl, 200, RATE_TYPE_EVENT));
}

void test_rate_limiter_default_config(void) {
    rate_limiter_t rl2;
    rate_limiter_init(&rl2, NULL);

    TEST_ASSERT_EQUAL(30, rl2.config.events_per_minute);
    TEST_ASSERT_EQUAL(60, rl2.config.reqs_per_minute);

    vSemaphoreDelete(rl2.lock);
}

void test_rate_limiter_custom_config(void) {
    rate_limiter_t rl2;
    rate_config_t config = { .events_per_minute = 10, .reqs_per_minute = 20 };
    rate_limiter_init(&rl2, &config);

    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(rate_limiter_check(&rl2, 1, RATE_TYPE_EVENT));
    }
    TEST_ASSERT_FALSE(rate_limiter_check(&rl2, 1, RATE_TYPE_EVENT));

    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_TRUE(rate_limiter_check(&rl2, 1, RATE_TYPE_REQ));
    }
    TEST_ASSERT_FALSE(rate_limiter_check(&rl2, 1, RATE_TYPE_REQ));

    vSemaphoreDelete(rl2.lock);
}

int main(void) {
    printf("=== Rate Limiter Tests ===\n");
#ifdef HAVE_UNITY
    UNITY_BEGIN();
    RUN_TEST(test_rate_limiter_allows_initial_events);
    RUN_TEST(test_rate_limiter_allows_initial_reqs);
    RUN_TEST(test_rate_limiter_blocks_excess_events);
    RUN_TEST(test_rate_limiter_blocks_excess_reqs);
    RUN_TEST(test_rate_limiter_separate_counters);
    RUN_TEST(test_rate_limiter_per_connection);
    RUN_TEST(test_rate_limiter_window_reset);
    RUN_TEST(test_rate_limiter_reset_clears_bucket);
    RUN_TEST(test_rate_limiter_max_buckets);
    RUN_TEST(test_rate_limiter_bucket_reuse);
    RUN_TEST(test_rate_limiter_default_config);
    RUN_TEST(test_rate_limiter_custom_config);
    return UNITY_END();
#else
    setUp();
    RUN_TEST(test_rate_limiter_allows_initial_events);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_allows_initial_reqs);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_blocks_excess_events);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_blocks_excess_reqs);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_separate_counters);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_per_connection);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_window_reset);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_reset_clears_bucket);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_max_buckets);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_bucket_reuse);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_default_config);
    tearDown(); setUp();
    RUN_TEST(test_rate_limiter_custom_config);
    tearDown();
    printf("\n=== All tests passed ===\n");
    return 0;
#endif
}
