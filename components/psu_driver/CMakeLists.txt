idf_component_register(
    SRCS        "psu_driver.c"
                "psu_modbus_rtu.c"
                "psu_riden.c"
                "psu_xy_sk120.c"
                "psu_wz5005.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES    app_api
                esp_driver_uart
                nvs_flash
                log
)
