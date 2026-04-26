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
                gpio_io
                psu_driver
                ui_settings
                ip_announcer
                esp_event
                esp_timer
                console
                driver
                # v6.0 拆 driver 成 per-peripheral components；driver/uart.h
                # 不再透過 driver umbrella 帶進來，要 explicit 加 esp_driver_uart。
                esp_driver_uart
                nvs_flash
)
