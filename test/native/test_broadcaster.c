#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cJSON.h"

static void test_serialize_event_message(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("EVENT"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("sub123"));

    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "id", "abc123");
    cJSON_AddStringToObject(event, "pubkey", "pubkey123");
    cJSON_AddNumberToObject(event, "created_at", 1234567890);
    cJSON_AddNumberToObject(event, "kind", 1);
    cJSON_AddItemToObject(event, "tags", cJSON_CreateArray());
    cJSON_AddStringToObject(event, "content", "Hello world");
    cJSON_AddStringToObject(event, "sig", "sig123");
    cJSON_AddItemToArray(arr, event);

    char *str = cJSON_PrintUnformatted(arr);
    assert(str != NULL);
    assert(strstr(str, "\"EVENT\"") != NULL);
    assert(strstr(str, "\"sub123\"") != NULL);
    assert(strstr(str, "\"id\":\"abc123\"") != NULL);
    assert(strstr(str, "\"kind\":1") != NULL);

    free(str);
    cJSON_Delete(arr);
    printf("PASS: serialize EVENT message\n");
}

static int is_ephemeral_kind(int kind) {
    return kind >= 20000 && kind < 30000;
}

static void test_ephemeral_kind_detection(void) {
    assert(is_ephemeral_kind(20000) == 1);
    assert(is_ephemeral_kind(29999) == 1);
    assert(is_ephemeral_kind(30000) == 0);
    assert(is_ephemeral_kind(19999) == 0);
    assert(is_ephemeral_kind(1) == 0);
    assert(is_ephemeral_kind(0) == 0);
    printf("PASS: ephemeral kind detection (20000-29999)\n");
}

static void test_event_message_with_sub_id_variations(void) {
    const char *sub_ids[] = {"a", "test-sub", "sub_with_underscore", "12345"};

    for (int i = 0; i < 4; i++) {
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, cJSON_CreateString("EVENT"));
        cJSON_AddItemToArray(arr, cJSON_CreateString(sub_ids[i]));
        cJSON_AddItemToArray(arr, cJSON_CreateObject());

        char *str = cJSON_PrintUnformatted(arr);
        assert(str != NULL);
        assert(strstr(str, sub_ids[i]) != NULL);

        free(str);
        cJSON_Delete(arr);
    }
    printf("PASS: EVENT with various sub_id formats\n");
}

static void test_broadcast_message_format(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("EVENT"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("feed"));

    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "id", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    cJSON_AddStringToObject(event, "pubkey", "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210");
    cJSON_AddNumberToObject(event, "created_at", 1704067200);
    cJSON_AddNumberToObject(event, "kind", 1);

    cJSON *tags = cJSON_CreateArray();
    cJSON *ptag = cJSON_CreateArray();
    cJSON_AddItemToArray(ptag, cJSON_CreateString("p"));
    cJSON_AddItemToArray(ptag, cJSON_CreateString("abcd1234"));
    cJSON_AddItemToArray(tags, ptag);
    cJSON_AddItemToObject(event, "tags", tags);

    cJSON_AddStringToObject(event, "content", "Test broadcast");
    cJSON_AddStringToObject(event, "sig", "signature");
    cJSON_AddItemToArray(arr, event);

    char *str = cJSON_PrintUnformatted(arr);
    assert(str != NULL);

    cJSON *parsed = cJSON_Parse(str);
    assert(parsed != NULL);
    assert(cJSON_GetArraySize(parsed) == 3);

    cJSON *type = cJSON_GetArrayItem(parsed, 0);
    assert(strcmp(type->valuestring, "EVENT") == 0);

    cJSON *sub_id = cJSON_GetArrayItem(parsed, 1);
    assert(strcmp(sub_id->valuestring, "feed") == 0);

    cJSON *evt = cJSON_GetArrayItem(parsed, 2);
    assert(cJSON_GetObjectItem(evt, "id") != NULL);
    assert(cJSON_GetObjectItem(evt, "tags") != NULL);

    free(str);
    cJSON_Delete(arr);
    cJSON_Delete(parsed);
    printf("PASS: broadcast message format roundtrip\n");
}

int main(void) {
    printf("=== Broadcaster Tests ===\n");

    test_serialize_event_message();
    test_ephemeral_kind_detection();
    test_event_message_with_sub_id_variations();
    test_broadcast_message_format();

    printf("\n=== All broadcaster tests passed ===\n");
    return 0;
}
