idf_component_register(
    SRCS        "psu_modbus.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES    app_api
                esp_driver_uart
                nvs_flash
                log
)
