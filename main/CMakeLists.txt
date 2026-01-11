idf_component_register(
    SRCS "main.c" "ws_server.c" "router.c" "handlers_stub.c" "validator.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi nvs_flash esp_timer
    PRIV_REQUIRES libnostr-c noscrypt
)
