idf_component_register(
    SRCS        "net_dashboard.c"
                "ws_handler.c"
                "provisioning.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES    esp_http_server
                esp_wifi
                esp_netif
                esp_event
                nvs_flash
                wifi_provisioning
                bt
                json
                app_api
                pwm_gen
                rpm_cap
                ota_core
    EMBED_TXTFILES "web/index.html"
                   "web/app.js"
                   "web/app.css"
)
