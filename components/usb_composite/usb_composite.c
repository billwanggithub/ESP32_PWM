#include "usb_composite.h"

#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

extern const uint8_t usb_hid_report_descriptor[];
extern const size_t  usb_hid_report_descriptor_size;

void usb_hid_task_start(void);
void usb_cdc_task_start(void);

static const char *TAG = "usb_comp";

static const char *s_string_desc[] = {
    (char[]){ 0x09, 0x04 },
    "VCC-GND",
    "ESP32-S3 PWM + RPM",
    "0001",
    "Control HID",
    "Firmware + Log CDC",
};

esp_err_t usb_composite_start(void)
{
    // esp_tinyusb 1.7.x: flat config. NULL fields take defaults from Kconfig
    // (VID/PID, product strings, composite descriptor for HID+CDC). We
    // override string_descriptor only to set manufacturer/product names.
    const tinyusb_config_t cfg = {
        .device_descriptor        = NULL,
        .string_descriptor        = s_string_desc,
        .string_descriptor_count  = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
        .external_phy             = false,
        .configuration_descriptor = NULL,
    };
    esp_err_t e = tinyusb_driver_install(&cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "tinyusb install failed: %s", esp_err_to_name(e)); return e; }

    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev  = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        // rx_unread_buf_sz is deprecated in esp_tinyusb 1.7.x
        // (RX buffer is configured via CONFIG_TINYUSB_CDC_RX_BUFSIZE).
    };
    e = tusb_cdc_acm_init(&acm_cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "cdc_acm init failed: %s", esp_err_to_name(e)); return e; }

    usb_hid_task_start();
    usb_cdc_task_start();

    ESP_LOGI(TAG, "usb composite started (HID + CDC)");
    return ESP_OK;
}
