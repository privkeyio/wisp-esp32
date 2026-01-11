idf_component_register(
    SRCS "main.c" "ws_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi nvs_flash esp_timer
    PRIV_REQUIRES libnostr-c noscrypt
)
