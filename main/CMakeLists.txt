idf_component_register(
    SRCS        "app_main.c"
                "control_task.c"
    INCLUDE_DIRS "."
    REQUIRES    app_api
                pwm_gen
                rpm_cap
                usb_composite
                net_dashboard
                ota_core
                esp_event
                esp_timer
                console
                driver
                nvs_flash
)
