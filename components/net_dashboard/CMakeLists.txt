idf_component_register(
    SRCS        "net_dashboard.c"
                "ws_handler.c"
                "provisioning.c"
                "dns_hijack.c"
                "mdns_svc.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES    esp_http_server
                esp_wifi
                esp_netif
                esp_event
                nvs_flash
                espressif__cjson
                app_api
                pwm_gen
                rpm_cap
                ota_core
                # BOOT-button watcher in net_dashboard.c uses gpio_config/
                # gpio_get_level (v6.0 moved these out of the driver umbrella).
                esp_driver_gpio
                # SoftAP captive portal DNS hijack (raw UDP socket).
                lwip
                # mDNS advertisement of esp32-pwm.local after provisioning.
                mdns
    EMBED_TXTFILES "web/index.html"
                   "web/app.js"
                   "web/app.css"
                   "web/setup.html"
                   "web/success.html"
)
