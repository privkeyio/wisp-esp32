#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cJSON.h"

static void test_parse_event_format(void) {
    const char *json = "[\"EVENT\",{\"id\":\"abc\",\"pubkey\":\"def\",\"created_at\":123,\"kind\":1,\"tags\":[],\"content\":\"test\",\"sig\":\"ghi\"}]";

    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    assert(cJSON_IsArray(root));
    assert(cJSON_GetArraySize(root) == 2);

    cJSON *type = cJSON_GetArrayItem(root, 0);
    assert(cJSON_IsString(type));
    assert(strcmp(type->valuestring, "EVENT") == 0);

    cJSON *event = cJSON_GetArrayItem(root, 1);
    assert(cJSON_IsObject(event));
    assert(cJSON_GetObjectItem(event, "id") != NULL);
    assert(cJSON_GetObjectItem(event, "kind") != NULL);
    assert(cJSON_GetObjectItem(event, "content") != NULL);

    cJSON_Delete(root);
    printf("PASS: EVENT format parsing\n");
}

static void test_parse_req_format(void) {
    const char *json = "[\"REQ\",\"sub123\",{\"kinds\":[1],\"limit\":10}]";

    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    assert(cJSON_GetArraySize(root) >= 3);

    cJSON *type = cJSON_GetArrayItem(root, 0);
    assert(strcmp(type->valuestring, "REQ") == 0);

    cJSON *sub_id = cJSON_GetArrayItem(root, 1);
    assert(cJSON_IsString(sub_id));
    assert(strcmp(sub_id->valuestring, "sub123") == 0);

    cJSON *filter = cJSON_GetArrayItem(root, 2);
    assert(cJSON_IsObject(filter));

    cJSON_Delete(root);
    printf("PASS: REQ format parsing\n");
}

static void test_parse_req_multiple_filters(void) {
    const char *json = "[\"REQ\",\"multi\",{\"kinds\":[1]},{\"kinds\":[0]},{\"authors\":[\"abc\"]}]";

    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    assert(cJSON_GetArraySize(root) == 5);

    cJSON_Delete(root);
    printf("PASS: REQ multiple filters\n");
}

static void test_parse_close_format(void) {
    const char *json = "[\"CLOSE\",\"sub123\"]";

    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    assert(cJSON_GetArraySize(root) == 2);

    cJSON *type = cJSON_GetArrayItem(root, 0);
    assert(strcmp(type->valuestring, "CLOSE") == 0);

    cJSON *sub_id = cJSON_GetArrayItem(root, 1);
    assert(strcmp(sub_id->valuestring, "sub123") == 0);

    cJSON_Delete(root);
    printf("PASS: CLOSE format parsing\n");
}

static void test_parse_invalid_json(void) {
    const char *json = "not json";
    cJSON *root = cJSON_Parse(json);
    assert(root == NULL);
    printf("PASS: reject invalid JSON\n");
}

static void test_serialize_ok(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("OK"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("eventid123"));
    cJSON_AddItemToArray(arr, cJSON_CreateTrue());
    cJSON_AddItemToArray(arr, cJSON_CreateString(""));

    char *str = cJSON_PrintUnformatted(arr);
    assert(str != NULL);
    assert(strstr(str, "\"OK\"") != NULL);
    assert(strstr(str, "eventid123") != NULL);

    free(str);
    cJSON_Delete(arr);
    printf("PASS: serialize OK\n");
}

static void test_serialize_notice(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("NOTICE"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("error message"));

    char *str = cJSON_PrintUnformatted(arr);
    assert(str != NULL);
    assert(strstr(str, "\"NOTICE\"") != NULL);
    assert(strstr(str, "error message") != NULL);

    free(str);
    cJSON_Delete(arr);
    printf("PASS: serialize NOTICE\n");
}

static void test_serialize_eose(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("EOSE"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("sub123"));

    char *str = cJSON_PrintUnformatted(arr);
    assert(str != NULL);
    assert(strstr(str, "\"EOSE\"") != NULL);
    assert(strstr(str, "sub123") != NULL);

    free(str);
    cJSON_Delete(arr);
    printf("PASS: serialize EOSE\n");
}

static void test_serialize_closed(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("CLOSED"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("sub123"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("subscription ended"));

    char *str = cJSON_PrintUnformatted(arr);
    assert(str != NULL);
    assert(strstr(str, "\"CLOSED\"") != NULL);
    assert(strstr(str, "sub123") != NULL);

    free(str);
    cJSON_Delete(arr);
    printf("PASS: serialize CLOSED\n");
}

int main(void) {
    printf("=== Router JSON Tests ===\n");

    test_parse_event_format();
    test_parse_req_format();
    test_parse_req_multiple_filters();
    test_parse_close_format();
    test_parse_invalid_json();
    test_serialize_ok();
    test_serialize_notice();
    test_serialize_eose();
    test_serialize_closed();

    printf("\n=== All tests passed ===\n");
    return 0;
}
