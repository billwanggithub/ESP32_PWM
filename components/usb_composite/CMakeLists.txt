idf_component_register(
    SRCS        "usb_composite.c"
                "usb_hid_task.c"
                "usb_cdc_task.c"
                "usb_descriptors.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES    esp_tinyusb
                driver
                esp_timer
                log
                app_api
                pwm_gen
                rpm_cap
                ota_core
                # HID/CDC factory-reset frontends call net_dashboard_factory_reset().
                # Keeps the reprovision flow in a single place so all three transports
                # share one restart path.
                net_dashboard
                # v6.0: freertos/ringbuf.h moved out of freertos transitive
                # closure; usb_cdc_task.c uses RingbufHandle_t for SLIP frame
                # buffering, so explicit dep is now required.
                esp_ringbuf
    # HID/CDC callbacks (tud_hid_descriptor_report_cb, tud_hid_*_cb) satisfy
    # weak references from TinyUSB core archives that appear earlier on the
    # link line; WHOLE_ARCHIVE forces our objects in so the symbols resolve.
    WHOLE_ARCHIVE
)
