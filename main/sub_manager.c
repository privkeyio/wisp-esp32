#include "sub_manager.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "sub_mgr";

static char **copy_string_array(char **src, size_t count)
{
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

static int32_t *copy_int_array(int32_t *src, size_t count)
{
    if (!src || count == 0) return NULL;
    int32_t *dst = malloc(sizeof(int32_t) * count);
    if (!dst) return NULL;
    memcpy(dst, src, sizeof(int32_t) * count);
    return dst;
}

static nostr_generic_tag_filter_t *copy_generic_tags(nostr_generic_tag_filter_t *src, size_t count)
{
    if (!src || count == 0) return NULL;
    nostr_generic_tag_filter_t *dst = calloc(count, sizeof(nostr_generic_tag_filter_t));
    if (!dst) return NULL;
    for (size_t i = 0; i < count; i++) {
        dst[i].tag_name = src[i].tag_name;
        dst[i].values_count = src[i].values_count;
        dst[i].values = copy_string_array(src[i].values, src[i].values_count);
        if (src[i].values_count > 0 && !dst[i].values) {
            for (size_t j = 0; j < i; j++) {
                for (size_t k = 0; k < dst[j].values_count; k++) free(dst[j].values[k]);
                free(dst[j].values);
            }
            free(dst);
            return NULL;
        }
    }
    return dst;
}

static bool filter_copy(nostr_filter_t *dst, const nostr_filter_t *src)
{
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

    dst->e_tags_count = src->e_tags_count;
    dst->e_tags = copy_string_array(src->e_tags, src->e_tags_count);
    if (src->e_tags_count > 0 && !dst->e_tags) goto fail;

    dst->p_tags_count = src->p_tags_count;
    dst->p_tags = copy_string_array(src->p_tags, src->p_tags_count);
    if (src->p_tags_count > 0 && !dst->p_tags) goto fail;

    dst->generic_tags_count = src->generic_tags_count;
    dst->generic_tags = copy_generic_tags(src->generic_tags, src->generic_tags_count);
    if (src->generic_tags_count > 0 && !dst->generic_tags) goto fail;

    dst->since = src->since;
    dst->until = src->until;
    dst->limit = src->limit;
    return true;

fail:
    nostr_filter_free(dst);
    return false;
}

static void free_filters(subscription_t *sub)
{
    for (uint8_t i = 0; i < sub->filter_count; i++) {
        nostr_filter_free(&sub->filters[i]);
    }
    sub->filter_count = 0;
}

static void clear_subscription(subscription_t *sub)
{
    free_filters(sub);
    memset(sub, 0, sizeof(subscription_t));
}

esp_err_t sub_manager_init(sub_manager_t *mgr)
{
    memset(mgr, 0, sizeof(sub_manager_t));
    mgr->lock = xSemaphoreCreateMutex();
    if (!mgr->lock) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Initialized (max=%d, per_conn=%d)", SUB_MAX_TOTAL, SUB_MAX_PER_CONN);
    return ESP_OK;
}

void sub_manager_destroy(sub_manager_t *mgr)
{
    if (!mgr) return;
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (mgr->subs[i].active) {
            clear_subscription(&mgr->subs[i]);
        }
    }
    if (mgr->lock) {
        vSemaphoreDelete(mgr->lock);
        mgr->lock = NULL;
    }
}

static subscription_t *sub_manager_find(sub_manager_t *mgr, int conn_fd, const char *sub_id)
{
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (mgr->subs[i].active &&
            mgr->subs[i].conn_fd == conn_fd &&
            strcmp(mgr->subs[i].sub_id, sub_id) == 0) {
            return &mgr->subs[i];
        }
    }
    return NULL;
}

static subscription_t *find_free_slot(sub_manager_t *mgr)
{
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (!mgr->subs[i].active) return &mgr->subs[i];
    }
    return NULL;
}

static bool copy_filters_to_slot(subscription_t *slot, const nostr_filter_t *filters,
                                 size_t filter_count)
{
    for (size_t i = 0; i < filter_count; i++) {
        if (!filter_copy(&slot->filters[i], &filters[i])) {
            for (size_t j = 0; j < i; j++) {
                nostr_filter_free(&slot->filters[j]);
            }
            return false;
        }
    }
    slot->filter_count = (uint8_t)filter_count;
    return true;
}

nostr_relay_error_t sub_manager_add(sub_manager_t *mgr, int conn_fd,
                                    const char *sub_id,
                                    const nostr_filter_t *filters,
                                    size_t filter_count)
{
    if (filter_count > SUB_MAX_FILTERS) {
        filter_count = SUB_MAX_FILTERS;
    }

    xSemaphoreTake(mgr->lock, portMAX_DELAY);

    subscription_t *existing = sub_manager_find(mgr, conn_fd, sub_id);
    if (existing) {
        free_filters(existing);
        existing->events_sent = 0;
        if (!copy_filters_to_slot(existing, filters, filter_count)) {
            xSemaphoreGive(mgr->lock);
            return NOSTR_RELAY_ERR_MEMORY;
        }
        ESP_LOGD(TAG, "Updated sub=%s fd=%d filters=%zu", sub_id, conn_fd, filter_count);
        xSemaphoreGive(mgr->lock);
        return NOSTR_RELAY_OK;
    }

    uint8_t conn_count = 0;
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (mgr->subs[i].active && mgr->subs[i].conn_fd == conn_fd) {
            conn_count++;
        }
    }
    if (conn_count >= SUB_MAX_PER_CONN) {
        ESP_LOGW(TAG, "Too many subs for fd=%d", conn_fd);
        xSemaphoreGive(mgr->lock);
        return NOSTR_RELAY_ERR_TOO_MANY_FILTERS;
    }

    subscription_t *slot = find_free_slot(mgr);
    if (!slot) {
        ESP_LOGW(TAG, "No free slots");
        xSemaphoreGive(mgr->lock);
        return NOSTR_RELAY_ERR_MEMORY;
    }

    memset(slot, 0, sizeof(subscription_t));
    strncpy(slot->sub_id, sub_id, SUB_MAX_ID_LEN);
    slot->sub_id[SUB_MAX_ID_LEN] = '\0';
    slot->conn_fd = conn_fd;

    if (!copy_filters_to_slot(slot, filters, filter_count)) {
        clear_subscription(slot);
        xSemaphoreGive(mgr->lock);
        return NOSTR_RELAY_ERR_MEMORY;
    }
    slot->active = true;
    mgr->active_count++;

    ESP_LOGI(TAG, "Added sub=%s fd=%d filters=%zu total=%d",
             sub_id, conn_fd, filter_count, mgr->active_count);

    xSemaphoreGive(mgr->lock);
    return NOSTR_RELAY_OK;
}

nostr_relay_error_t sub_manager_remove(sub_manager_t *mgr, int conn_fd, const char *sub_id)
{
    xSemaphoreTake(mgr->lock, portMAX_DELAY);

    subscription_t *sub = sub_manager_find(mgr, conn_fd, sub_id);
    if (!sub) {
        xSemaphoreGive(mgr->lock);
        return NOSTR_RELAY_ERR_INVALID_SUBSCRIPTION_ID;
    }

    clear_subscription(sub);
    mgr->active_count--;
    ESP_LOGD(TAG, "Removed sub=%s fd=%d remaining=%d", sub_id, conn_fd, mgr->active_count);

    xSemaphoreGive(mgr->lock);
    return NOSTR_RELAY_OK;
}

void sub_manager_remove_all(sub_manager_t *mgr, int conn_fd)
{
    xSemaphoreTake(mgr->lock, portMAX_DELAY);

    int removed = 0;
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (mgr->subs[i].active && mgr->subs[i].conn_fd == conn_fd) {
            clear_subscription(&mgr->subs[i]);
            mgr->active_count--;
            removed++;
        }
    }

    if (removed > 0) {
        ESP_LOGI(TAG, "Removed %d subs for fd=%d", removed, conn_fd);
    }

    xSemaphoreGive(mgr->lock);
}

void sub_manager_match(sub_manager_t *mgr, const nostr_event *event,
                       sub_match_result_t *result)
{
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
    ESP_LOGD(TAG, "Event matched %d subs", result->count);
}

uint8_t sub_manager_count(sub_manager_t *mgr, int conn_fd)
{
    uint8_t count = 0;

    xSemaphoreTake(mgr->lock, portMAX_DELAY);
    for (int i = 0; i < SUB_MAX_TOTAL; i++) {
        if (mgr->subs[i].active && mgr->subs[i].conn_fd == conn_fd) {
            count++;
        }
    }
    xSemaphoreGive(mgr->lock);

    return count;
}
