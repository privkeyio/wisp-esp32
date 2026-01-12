#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

#define portMAX_DELAY 0xFFFFFFFFUL
typedef void* SemaphoreHandle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM -1

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void*)1; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }

#define ESP_LOGI(tag, fmt, ...) printf("[I] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) printf("[W] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E] " fmt "\n", ##__VA_ARGS__)

typedef enum {
    NOSTR_RELAY_OK = 0,
    NOSTR_RELAY_ERR_MEMORY,
    NOSTR_RELAY_ERR_TOO_MANY_FILTERS,
    NOSTR_RELAY_ERR_INVALID_SUBSCRIPTION_ID,
} nostr_relay_error_t;

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

typedef struct {
    uint8_t id[32];
    struct { uint8_t data[32]; } pubkey;
    int64_t created_at;
    uint16_t kind;
    void* tags;
    size_t tags_count;
    char* content;
    uint8_t sig[64];
    void* tag_arena;
} nostr_event;

static void nostr_filter_free(nostr_filter_t* f) {
    if (!f) return;
    for (size_t i = 0; i < f->ids_count; i++) free(f->ids[i]);
    free(f->ids);
    for (size_t i = 0; i < f->authors_count; i++) free(f->authors[i]);
    free(f->authors);
    free(f->kinds);
    for (size_t i = 0; i < f->e_tags_count; i++) free(f->e_tags[i]);
    free(f->e_tags);
    for (size_t i = 0; i < f->p_tags_count; i++) free(f->p_tags[i]);
    free(f->p_tags);
    if (f->generic_tags) {
        for (size_t i = 0; i < f->generic_tags_count; i++) {
            for (size_t j = 0; j < f->generic_tags[i].values_count; j++)
                free(f->generic_tags[i].values[j]);
            free(f->generic_tags[i].values);
        }
        free(f->generic_tags);
    }
    memset(f, 0, sizeof(nostr_filter_t));
}

static bool nostr_filters_match(const nostr_filter_t* filters, size_t count, const nostr_event* event) {
    for (size_t i = 0; i < count; i++) {
        const nostr_filter_t* f = &filters[i];
        if (f->kinds_count > 0) {
            bool found = false;
            for (size_t k = 0; k < f->kinds_count; k++) {
                if (f->kinds[k] == event->kind) { found = true; break; }
            }
            if (!found) continue;
        }
        if (f->since > 0 && event->created_at < f->since) continue;
        if (f->until > 0 && event->created_at > f->until) continue;
        return true;
    }
    return count == 0;
}

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

static char **copy_string_array(char **src, size_t count) {
    if (!src || count == 0) return NULL;
    char **dst = malloc(sizeof(char *) * count);
    if (!dst) return NULL;
    for (size_t i = 0; i < count; i++) {
        dst[i] = strdup(src[i]);
        if (!dst[i]) {
            for (size_t j = 0; j < i; j++) free(dst[j]);
            free(dst);
            return NULL;
        }
    }
    return dst;
}

static int32_t *copy_int_array(int32_t *src, size_t count) {
    if (!src || count == 0) return NULL;
    int32_t *dst = malloc(sizeof(int32_t) * count);
    if (!dst) return NULL;
    memcpy(dst, src, sizeof(int32_t) * count);
    return dst;
}

static bool filter_copy(nostr_filter_t *dst, const nostr_filter_t *src) {
    memset(dst, 0, sizeof(nostr_filter_t));
    dst->ids_count = src->ids_count;
    dst->ids = copy_string_array(src->ids, src->ids_count);
    if (src->ids_count > 0 && !dst->ids) goto fail;
    dst->authors_count = src->authors_count;
    dst->authors = copy_string_array(src->authors, src->authors_count);
    if (src->authors_count > 0 && !dst->authors) goto fail;
    dst->kinds_count = src->kinds_count;
    dst->kinds = copy_int_array(src->kinds, src->kinds_count);
    if (src->kinds_count > 0 && !dst->kinds) goto fail;
    dst->since = src->since;
    dst->until = src->until;
    dst->limit = src->limit;
    return true;
fail:
    nostr_filter_free(dst);
    return false;
}

static void clear_subscription(subscription_t *sub) {
    for (uint8_t i = 0; i < sub->filter_count; i++) {
        nostr_filter_free(&sub->filters[i]);
    }
    memset(sub, 0, sizeof(subscription_t));
}

static esp_err_t sub_manager_init(sub_manager_t *mgr) {
    memset(mgr, 0, sizeof(sub_manager_t));
    mgr->lock = xSemaphoreCreateMutex();
    if (!mgr->lock) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static void sub_manager_destroy(sub_manager_t *mgr) {
    if (!mgr) return;
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (mgr->subs[i].active) clear_subscription(&mgr->subs[i]);
    }
    if (mgr->lock) { vSemaphoreDelete(mgr->lock); mgr->lock = NULL; }
}

static subscription_t *sub_manager_find(sub_manager_t *mgr, int conn_fd, const char *sub_id) {
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (mgr->subs[i].active && mgr->subs[i].conn_fd == conn_fd &&
            strcmp(mgr->subs[i].sub_id, sub_id) == 0) {
            return &mgr->subs[i];
        }
    }
    return NULL;
}

static subscription_t *find_free_slot(sub_manager_t *mgr) {
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (!mgr->subs[i].active) return &mgr->subs[i];
    }
    return NULL;
}

static nostr_relay_error_t sub_manager_add(sub_manager_t *mgr, int conn_fd, const char *sub_id,
                                           const nostr_filter_t *filters, size_t filter_count) {
    if (filter_count > SUB_MAX_FILTERS) filter_count = SUB_MAX_FILTERS;
    xSemaphoreTake(mgr->lock, portMAX_DELAY);
    subscription_t *existing = sub_manager_find(mgr, conn_fd, sub_id);
    if (existing) {
        for (uint8_t i = 0; i < existing->filter_count; i++) nostr_filter_free(&existing->filters[i]);
        existing->filter_count = 0;
        for (size_t i = 0; i < filter_count; i++) {
            if (!filter_copy(&existing->filters[i], &filters[i])) {
                for (size_t j = 0; j < i; j++) nostr_filter_free(&existing->filters[j]);
                existing->filter_count = 0;
                xSemaphoreGive(mgr->lock);
                return NOSTR_RELAY_ERR_MEMORY;
            }
        }
        existing->filter_count = (uint8_t)filter_count;
        existing->events_sent = 0;
        xSemaphoreGive(mgr->lock);
        return NOSTR_RELAY_OK;
    }
    uint8_t conn_count = 0;
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (mgr->subs[i].active && mgr->subs[i].conn_fd == conn_fd) conn_count++;
    }
    if (conn_count >= SUB_MAX_PER_CONN) {
        xSemaphoreGive(mgr->lock);
        return NOSTR_RELAY_ERR_TOO_MANY_FILTERS;
    }
    subscription_t *slot = find_free_slot(mgr);
    if (!slot) {
        xSemaphoreGive(mgr->lock);
        return NOSTR_RELAY_ERR_MEMORY;
    }
    memset(slot, 0, sizeof(subscription_t));
    strncpy(slot->sub_id, sub_id, SUB_MAX_ID_LEN);
    slot->conn_fd = conn_fd;
    for (size_t i = 0; i < filter_count; i++) {
        if (!filter_copy(&slot->filters[i], &filters[i])) {
            clear_subscription(slot);
            xSemaphoreGive(mgr->lock);
            return NOSTR_RELAY_ERR_MEMORY;
        }
    }
    slot->filter_count = (uint8_t)filter_count;
    slot->active = true;
    mgr->active_count++;
    xSemaphoreGive(mgr->lock);
    return NOSTR_RELAY_OK;
}

static nostr_relay_error_t sub_manager_remove(sub_manager_t *mgr, int conn_fd, const char *sub_id) {
    xSemaphoreTake(mgr->lock, portMAX_DELAY);
    subscription_t *sub = sub_manager_find(mgr, conn_fd, sub_id);
    if (!sub) { xSemaphoreGive(mgr->lock); return NOSTR_RELAY_ERR_INVALID_SUBSCRIPTION_ID; }
    clear_subscription(sub);
    mgr->active_count--;
    xSemaphoreGive(mgr->lock);
    return NOSTR_RELAY_OK;
}

static void sub_manager_remove_all(sub_manager_t *mgr, int conn_fd) {
    xSemaphoreTake(mgr->lock, portMAX_DELAY);
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (mgr->subs[i].active && mgr->subs[i].conn_fd == conn_fd) {
            clear_subscription(&mgr->subs[i]);
            mgr->active_count--;
        }
    }
    xSemaphoreGive(mgr->lock);
}

static void sub_manager_match(sub_manager_t *mgr, const nostr_event *event, sub_match_result_t *result) {
    result->count = 0;
    xSemaphoreTake(mgr->lock, portMAX_DELAY);
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        subscription_t *sub = &mgr->subs[i];
        if (!sub->active) continue;
        if (nostr_filters_match(sub->filters, sub->filter_count, event)) {
            sub_match_entry_t *entry = &result->matches[result->count++];
            entry->conn_fd = sub->conn_fd;
            memcpy(entry->sub_id, sub->sub_id, sizeof(entry->sub_id));
        }
    }
    xSemaphoreGive(mgr->lock);
}

static uint8_t sub_manager_count(sub_manager_t *mgr, int conn_fd) {
    uint8_t count = 0;
    xSemaphoreTake(mgr->lock, portMAX_DELAY);
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (mgr->subs[i].active && mgr->subs[i].conn_fd == conn_fd) count++;
    }
    xSemaphoreGive(mgr->lock);
    return count;
}

static nostr_filter_t make_kinds_filter(int32_t kind) {
    nostr_filter_t f = {0};
    f.kinds = malloc(sizeof(int32_t));
    f.kinds[0] = kind;
    f.kinds_count = 1;
    return f;
}

static nostr_filter_t make_time_filter(int64_t since, int64_t until) {
    nostr_filter_t f = {0};
    f.since = since;
    f.until = until;
    return f;
}

static nostr_event make_event(uint16_t kind, int64_t created_at) {
    nostr_event e = {0};
    e.kind = kind;
    e.created_at = created_at;
    return e;
}

static void test_init_destroy(void) {
    sub_manager_t mgr;
    assert(sub_manager_init(&mgr) == ESP_OK);
    assert(mgr.lock != NULL);
    assert(mgr.active_count == 0);
    sub_manager_destroy(&mgr);
    assert(mgr.lock == NULL);
    printf("PASS: init/destroy\n");
}

static void test_add_subscription(void) {
    sub_manager_t mgr;
    sub_manager_init(&mgr);
    nostr_filter_t filter = make_kinds_filter(1);
    nostr_relay_error_t err = sub_manager_add(&mgr, 5, "test_sub", &filter, 1);
    assert(err == NOSTR_RELAY_OK);
    assert(mgr.active_count == 1);
    subscription_t *sub = sub_manager_find(&mgr, 5, "test_sub");
    assert(sub != NULL);
    assert(sub->conn_fd == 5);
    assert(strcmp(sub->sub_id, "test_sub") == 0);
    assert(sub->filter_count == 1);
    nostr_filter_free(&filter);
    sub_manager_destroy(&mgr);
    printf("PASS: add subscription\n");
}

static void test_update_subscription(void) {
    sub_manager_t mgr;
    sub_manager_init(&mgr);
    nostr_filter_t f1 = make_kinds_filter(1);
    nostr_filter_t f2 = make_kinds_filter(0);
    sub_manager_add(&mgr, 5, "test_sub", &f1, 1);
    assert(mgr.active_count == 1);
    sub_manager_add(&mgr, 5, "test_sub", &f2, 1);
    assert(mgr.active_count == 1);
    subscription_t *sub = sub_manager_find(&mgr, 5, "test_sub");
    assert(sub != NULL);
    assert(sub->filters[0].kinds[0] == 0);
    nostr_filter_free(&f1);
    nostr_filter_free(&f2);
    sub_manager_destroy(&mgr);
    printf("PASS: update subscription (same sub_id replaces)\n");
}

static void test_remove_subscription(void) {
    sub_manager_t mgr;
    sub_manager_init(&mgr);
    nostr_filter_t filter = make_kinds_filter(1);
    sub_manager_add(&mgr, 5, "test_sub", &filter, 1);
    nostr_relay_error_t err = sub_manager_remove(&mgr, 5, "test_sub");
    assert(err == NOSTR_RELAY_OK);
    assert(mgr.active_count == 0);
    err = sub_manager_remove(&mgr, 5, "test_sub");
    assert(err == NOSTR_RELAY_ERR_INVALID_SUBSCRIPTION_ID);
    nostr_filter_free(&filter);
    sub_manager_destroy(&mgr);
    printf("PASS: remove subscription\n");
}

static void test_per_connection_limit(void) {
    sub_manager_t mgr;
    sub_manager_init(&mgr);
    nostr_filter_t filter = make_kinds_filter(1);
    for (int i = 0; i < SUB_MAX_PER_CONN; i++) {
        char sub_id[32];
        snprintf(sub_id, sizeof(sub_id), "sub_%d", i);
        nostr_relay_error_t err = sub_manager_add(&mgr, 5, sub_id, &filter, 1);
        assert(err == NOSTR_RELAY_OK);
    }
    assert(mgr.active_count == SUB_MAX_PER_CONN);
    nostr_relay_error_t err = sub_manager_add(&mgr, 5, "sub_overflow", &filter, 1);
    assert(err == NOSTR_RELAY_ERR_TOO_MANY_FILTERS);
    err = sub_manager_add(&mgr, 6, "other_conn", &filter, 1);
    assert(err == NOSTR_RELAY_OK);
    assert(mgr.active_count == SUB_MAX_PER_CONN + 1);
    nostr_filter_free(&filter);
    sub_manager_destroy(&mgr);
    printf("PASS: per-connection limit (max %d)\n", SUB_MAX_PER_CONN);
}

static void test_remove_all_for_connection(void) {
    sub_manager_t mgr;
    sub_manager_init(&mgr);
    nostr_filter_t filter = make_kinds_filter(1);
    sub_manager_add(&mgr, 5, "sub1", &filter, 1);
    sub_manager_add(&mgr, 5, "sub2", &filter, 1);
    sub_manager_add(&mgr, 6, "sub3", &filter, 1);
    assert(mgr.active_count == 3);
    sub_manager_remove_all(&mgr, 5);
    assert(mgr.active_count == 1);
    assert(sub_manager_find(&mgr, 5, "sub1") == NULL);
    assert(sub_manager_find(&mgr, 5, "sub2") == NULL);
    assert(sub_manager_find(&mgr, 6, "sub3") != NULL);
    nostr_filter_free(&filter);
    sub_manager_destroy(&mgr);
    printf("PASS: remove all for connection (disconnect cleanup)\n");
}

static void test_count_for_connection(void) {
    sub_manager_t mgr;
    sub_manager_init(&mgr);
    nostr_filter_t filter = make_kinds_filter(1);
    sub_manager_add(&mgr, 5, "sub1", &filter, 1);
    sub_manager_add(&mgr, 5, "sub2", &filter, 1);
    sub_manager_add(&mgr, 6, "sub3", &filter, 1);
    assert(sub_manager_count(&mgr, 5) == 2);
    assert(sub_manager_count(&mgr, 6) == 1);
    assert(sub_manager_count(&mgr, 7) == 0);
    nostr_filter_free(&filter);
    sub_manager_destroy(&mgr);
    printf("PASS: count for connection\n");
}

static void test_filter_matching_kinds(void) {
    sub_manager_t mgr;
    sub_manager_init(&mgr);
    nostr_filter_t filter = make_kinds_filter(1);
    sub_manager_add(&mgr, 5, "kind1_sub", &filter, 1);
    nostr_filter_free(&filter);
    nostr_event event1 = make_event(1, 1000);
    nostr_event event0 = make_event(0, 1000);
    sub_match_result_t result;
    sub_manager_match(&mgr, &event1, &result);
    assert(result.count == 1);
    assert(strcmp(result.matches[0].sub_id, "kind1_sub") == 0);
    sub_manager_match(&mgr, &event0, &result);
    assert(result.count == 0);
    sub_manager_destroy(&mgr);
    printf("PASS: filter matching by kind\n");
}

static void test_multiple_filters_or_logic(void) {
    sub_manager_t mgr;
    sub_manager_init(&mgr);
    nostr_filter_t filters[2];
    filters[0] = make_kinds_filter(1);
    filters[1] = make_kinds_filter(0);
    sub_manager_add(&mgr, 5, "multi_filter", filters, 2);
    nostr_filter_free(&filters[0]);
    nostr_filter_free(&filters[1]);
    nostr_event event0 = make_event(0, 1000);
    nostr_event event1 = make_event(1, 1000);
    nostr_event event3 = make_event(3, 1000);
    sub_match_result_t result;
    sub_manager_match(&mgr, &event0, &result);
    assert(result.count == 1);
    sub_manager_match(&mgr, &event1, &result);
    assert(result.count == 1);
    sub_manager_match(&mgr, &event3, &result);
    assert(result.count == 0);
    sub_manager_destroy(&mgr);
    printf("PASS: multiple filters OR'd together\n");
}

static void test_filter_since_until(void) {
    sub_manager_t mgr;
    sub_manager_init(&mgr);
    nostr_filter_t filter = make_time_filter(1000, 2000);
    sub_manager_add(&mgr, 5, "time_filter", &filter, 1);
    nostr_filter_free(&filter);
    nostr_event event_in_range = make_event(1, 1500);
    nostr_event event_too_old = make_event(1, 500);
    nostr_event event_too_new = make_event(1, 3000);
    sub_match_result_t result;
    sub_manager_match(&mgr, &event_in_range, &result);
    assert(result.count == 1);
    sub_manager_match(&mgr, &event_too_old, &result);
    assert(result.count == 0);
    sub_manager_match(&mgr, &event_too_new, &result);
    assert(result.count == 0);
    sub_manager_destroy(&mgr);
    printf("PASS: filter since/until\n");
}

static void test_total_limit(void) {
    sub_manager_t mgr;
    sub_manager_init(&mgr);
    nostr_filter_t filter = make_kinds_filter(1);
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        char sub_id[32];
        snprintf(sub_id, sizeof(sub_id), "sub_%d", i);
        int conn_fd = 100 + (i / SUB_MAX_PER_CONN);
        nostr_relay_error_t err = sub_manager_add(&mgr, conn_fd, sub_id, &filter, 1);
        assert(err == NOSTR_RELAY_OK);
    }
    assert(mgr.active_count == SUB_MAX_TOTAL);
    nostr_relay_error_t err = sub_manager_add(&mgr, 999, "overflow", &filter, 1);
    assert(err == NOSTR_RELAY_ERR_MEMORY);
    assert(mgr.active_count == SUB_MAX_TOTAL);
    nostr_filter_free(&filter);
    sub_manager_destroy(&mgr);
    printf("PASS: total limit (max %d)\n", SUB_MAX_TOTAL);
}

int main(void) {
    printf("=== Subscription Manager Tests ===\n");
    test_init_destroy();
    test_add_subscription();
    test_update_subscription();
    test_remove_subscription();
    test_per_connection_limit();
    test_remove_all_for_connection();
    test_count_for_connection();
    test_filter_matching_kinds();
    test_multiple_filters_or_logic();
    test_filter_since_until();
    test_total_limit();
    printf("\n=== All tests passed ===\n");
    return 0;
}
