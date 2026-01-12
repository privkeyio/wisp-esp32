idf_component_register(
    SRCS "main.c" "ws_server.c" "router.c" "handlers_stub.c" "validator.c" "sub_manager.c" "storage_engine.c" "broadcaster.c" "flash_monitor.c" "rate_limiter.c" "nip11.c" "deletion.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi nvs_flash esp_timer littlefs
    PRIV_REQUIRES libnostr-c noscrypt
)
