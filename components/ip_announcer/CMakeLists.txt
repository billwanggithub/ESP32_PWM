idf_component_register(
    SRCS         "ip_announcer.c"
    INCLUDE_DIRS "include"
    REQUIRES     log
                 nvs_flash
                 esp_event
                 esp_netif
                 esp_timer
                 app_api
)
