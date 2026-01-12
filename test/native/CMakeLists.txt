cmake_minimum_required(VERSION 3.10)
project(wisp_native_tests C)

set(CMAKE_C_STANDARD 99)

set(CJSON_DIR ${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON)
set(UNITY_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../Unity/src)

option(USE_UNITY "Use Unity test framework" OFF)

if(USE_UNITY AND EXISTS ${UNITY_DIR}/unity.c)
    message(STATUS "Unity framework enabled")
    add_compile_definitions(HAVE_UNITY)
    set(UNITY_SRC ${UNITY_DIR}/unity.c)
    set(UNITY_INC ${UNITY_DIR})
else()
    message(STATUS "Unity framework disabled (using assert-based tests)")
    set(UNITY_SRC "")
    set(UNITY_INC "")
endif()

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

add_executable(test_validator
    test_validator.c
    ${UNITY_SRC}
)
target_include_directories(test_validator PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${UNITY_INC})

add_executable(test_filter
    test_filter.c
    ${UNITY_SRC}
)
target_include_directories(test_filter PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${UNITY_INC})

add_executable(test_storage
    test_storage.c
    ${UNITY_SRC}
)
target_include_directories(test_storage PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${UNITY_INC})

add_executable(test_rate_limit
    test_rate_limit.c
    ${UNITY_SRC}
)
target_include_directories(test_rate_limit PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${UNITY_INC})

enable_testing()

add_test(NAME router COMMAND test_router)
add_test(NAME sub_manager COMMAND test_sub_manager)
add_test(NAME broadcaster COMMAND test_broadcaster)
add_test(NAME validator COMMAND test_validator)
add_test(NAME filter COMMAND test_filter)
add_test(NAME storage COMMAND test_storage)
add_test(NAME rate_limit COMMAND test_rate_limit)

add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_router test_sub_manager test_broadcaster test_validator test_filter test_storage test_rate_limit
)
