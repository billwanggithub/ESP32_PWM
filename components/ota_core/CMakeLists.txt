idf_component_register(
    SRCS        "ota_core.c"
    INCLUDE_DIRS "include"
    REQUIRES    app_update
                esp_http_server
                esp_partition
                esp_timer
                log
)
