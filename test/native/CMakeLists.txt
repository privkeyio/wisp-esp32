cmake_minimum_required(VERSION 3.10)
project(wisp_native_tests C)

set(CMAKE_C_STANDARD 99)

set(CJSON_DIR ${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON)

add_executable(test_router
    test_router.c
    ${CJSON_DIR}/cJSON.c
)

target_include_directories(test_router PRIVATE ${CJSON_DIR})

add_executable(test_sub_manager test_sub_manager.c)

add_executable(test_broadcaster
    test_broadcaster.c
    ${CJSON_DIR}/cJSON.c
)
target_include_directories(test_broadcaster PRIVATE ${CJSON_DIR})
