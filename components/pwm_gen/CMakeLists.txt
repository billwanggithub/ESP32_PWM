idf_component_register(
    SRCS        "pwm_gen.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES    driver
                esp_driver_mcpwm
                esp_driver_gpio
                esp_timer
                log
)
