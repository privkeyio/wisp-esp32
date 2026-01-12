#ifndef TEST_FIXTURES_H
#define TEST_FIXTURES_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_UNITY
#include "unity.h"
#else
#include <assert.h>
#define TEST_ASSERT(x) assert(x)
#define TEST_ASSERT_TRUE(x) assert(x)
#define TEST_ASSERT_FALSE(x) assert(!(x))
#define TEST_ASSERT_EQUAL(e, a) assert((e) == (a))
#define TEST_ASSERT_NOT_EQUAL(e, a) assert((e) != (a))
#define TEST_ASSERT_NULL(x) assert((x) == NULL)
#define TEST_ASSERT_NOT_NULL(x) assert((x) != NULL)
#define TEST_ASSERT_EQUAL_STRING(e, a) assert(strcmp((e), (a)) == 0)
#define TEST_ASSERT_EQUAL_MEMORY(e, a, len) assert(memcmp((e), (a), (len)) == 0)
#define TEST_FAIL_MESSAGE(msg) do { fprintf(stderr, "FAIL: %s\n", msg); assert(0); } while(0)
#define RUN_TEST(func) do { printf("Running " #func "... "); func(); printf("PASS\n"); } while(0)
#define UNITY_BEGIN() ((void)0)
#define UNITY_END() (0)
#endif

#define portMAX_DELAY 0xFFFFFFFFUL
typedef void* SemaphoreHandle_t;
typedef void* TaskHandle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void*)1; }
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }

#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)

#define NOSTR_ID_SIZE 32
#define NOSTR_SIG_SIZE 64

typedef struct { uint8_t data[32]; } nostr_key;
typedef struct nostr_tag_arena nostr_tag_arena;
typedef struct { char** values; size_t values_count; } nostr_tag;

typedef struct nostr_event {
    uint8_t id[NOSTR_ID_SIZE];
    nostr_key pubkey;
    int64_t created_at;
    uint16_t kind;
    nostr_tag* tags;
    size_t tags_count;
    char* content;
    uint8_t sig[NOSTR_SIG_SIZE];
    nostr_tag_arena* tag_arena;
} nostr_event;

typedef struct { char tag_name; char** values; size_t values_count; } nostr_generic_tag_filter_t;

typedef struct {
    char** ids; size_t ids_count;
    char** authors; size_t authors_count;
    int32_t* kinds; size_t kinds_count;
    char** e_tags; size_t e_tags_count;
    char** p_tags; size_t p_tags_count;
    nostr_generic_tag_filter_t* generic_tags; size_t generic_tags_count;
    int64_t since;
    int64_t until;
    int32_t limit;
} nostr_filter_t;

typedef enum {
    NOSTR_RELAY_OK = 0,
    NOSTR_RELAY_ERR_MEMORY,
    NOSTR_RELAY_ERR_TOO_MANY_FILTERS,
    NOSTR_RELAY_ERR_INVALID_SUBSCRIPTION_ID,
    NOSTR_RELAY_ERR_MISSING_FIELD,
    NOSTR_RELAY_ERR_INVALID_ID,
    NOSTR_RELAY_ERR_INVALID_PUBKEY,
    NOSTR_RELAY_ERR_INVALID_CREATED_AT,
    NOSTR_RELAY_ERR_INVALID_KIND,
    NOSTR_RELAY_ERR_INVALID_TAGS,
    NOSTR_RELAY_ERR_INVALID_CONTENT,
    NOSTR_RELAY_ERR_ID_MISMATCH,
    NOSTR_RELAY_ERR_SIG_MISMATCH,
    NOSTR_RELAY_ERR_INVALID_SIG,
    NOSTR_RELAY_ERR_FUTURE_EVENT,
    NOSTR_RELAY_ERR_EXPIRED_EVENT,
    NOSTR_RELAY_ERR_INVALID_JSON,
} nostr_relay_error_t;

#define NOSTR_OK_PREFIX_DUPLICATE "duplicate:"
#define NOSTR_OK_PREFIX_POW "pow:"
#define NOSTR_OK_PREFIX_BLOCKED "blocked:"
#define NOSTR_OK_PREFIX_INVALID "invalid:"

#define FREE_STRING_ARRAY(arr, count) do { \
    for (size_t i = 0; i < (count); i++) free((arr)[i]); \
    free(arr); \
} while(0)

static inline void nostr_filter_free(nostr_filter_t* f) {
    if (!f) return;
    FREE_STRING_ARRAY(f->ids, f->ids_count);
    FREE_STRING_ARRAY(f->authors, f->authors_count);
    free(f->kinds);
    FREE_STRING_ARRAY(f->e_tags, f->e_tags_count);
    FREE_STRING_ARRAY(f->p_tags, f->p_tags_count);
    if (f->generic_tags) {
        for (size_t i = 0; i < f->generic_tags_count; i++) {
            FREE_STRING_ARRAY(f->generic_tags[i].values, f->generic_tags[i].values_count);
        }
        free(f->generic_tags);
    }
    memset(f, 0, sizeof(nostr_filter_t));
}

static inline void fill_random_bytes(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(rand() & 0xFF);
}

static inline nostr_event* fixture_create_event(uint16_t kind, int64_t created_at) {
    nostr_event* e = (nostr_event*)calloc(1, sizeof(nostr_event));
    if (!e) return NULL;
    e->kind = kind;
    e->created_at = created_at;
    fill_random_bytes(e->id, 32);
    fill_random_bytes(e->pubkey.data, 32);
    fill_random_bytes(e->sig, 64);
    return e;
}

static inline void fixture_free_event(nostr_event* e) {
    if (!e) return;
    free(e->content);
    free(e);
}

static inline nostr_filter_t fixture_kinds_filter(int32_t kind) {
    nostr_filter_t f = {0};
    f.kinds = (int32_t*)malloc(sizeof(int32_t));
    if (!f.kinds) {
        return f;
    }
    f.kinds[0] = kind;
    f.kinds_count = 1;
    return f;
}

static inline nostr_filter_t fixture_time_filter(int64_t since, int64_t until) {
    nostr_filter_t f = {0};
    f.since = since;
    f.until = until;
    return f;
}

static inline nostr_filter_t fixture_author_filter(const uint8_t pubkey[32]) {
    nostr_filter_t f = {0};
    f.authors = (char**)malloc(sizeof(char*));
    if (!f.authors) {
        return f;
    }
    f.authors[0] = (char*)malloc(65);
    if (!f.authors[0]) {
        free(f.authors);
        f.authors = NULL;
        return f;
    }
    for (int i = 0; i < 32; i++) {
        sprintf(&f.authors[0][i*2], "%02x", pubkey[i]);
    }
    f.authors_count = 1;
    return f;
}

static inline int64_t fixture_now(void) {
    return (int64_t)time(NULL);
}

#endif
