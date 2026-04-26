idf_component_register(
    SRCS         "ui_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES     log
                 nvs_flash
)
