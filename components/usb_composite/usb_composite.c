#include "usb_composite.h"

#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

extern const uint8_t usb_hid_report_descriptor[];
extern const size_t  usb_hid_report_descriptor_size;

void usb_hid_task_start(void);
void usb_cdc_task_start(void);

static const char *TAG = "usb_comp";

// esp_tinyusb 1.7.x 的 default config descriptor 只 cover CDC+MSC+NCM+VENDOR，
// **不會** auto-include HID interface。啟用 HID 就必須提供自製 config
// descriptor；否則 tinyusb_set_descriptors 會 reject 整個 install。
//
// Interface layout:
//   IF0          : HID              (1 IN endpoint  @ 0x81)
//   IF1 + IF2    : CDC (control + data) via IAD
//                  control IF1 notif IN @ 0x82
//                  data    IF2 bulk OUT @ 0x03 / IN @ 0x83
//
// String indices（對應 s_string_desc[]）:
//   1 Manufacturer, 2 Product, 3 Serial, 4 HID 介面名, 5 CDC 介面名

enum {
    ITF_NUM_HID = 0,
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_HID_INTERFACE,
    STRID_CDC_INTERFACE,
};

#define EPNUM_HID       0x81
#define EPNUM_CDC_NOTIF 0x82
#define EPNUM_CDC_OUT   0x03
#define EPNUM_CDC_IN    0x83

#define HID_REPORT_DESC_SIZE 63  // usb_descriptors.c:usb_hid_report_descriptor 實際大小。
                                 // 若 report descriptor 修改必須同步 (usb_descriptors.c 的
                                 // _Static_assert 會把兩者綁住，漂移會 compile-fail)。

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),

    // HID @ IF0: boot_protocol=0 (no boot), report desc size, EP IN, packet 16 B, polling 10 ms
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID_INTERFACE, 0, HID_REPORT_DESC_SIZE,
                       EPNUM_HID, 16, 10),

    // CDC ACM @ IF1 (control) + IF2 (data): notif EP 8B, bulk data 64 B
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC_INTERFACE,
                       EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

static const char *s_string_desc[] = {
    (char[]){ 0x09, 0x04 },    // 0: langid = en-US
    "VCC-GND",                 // 1: Manufacturer
    "Fan-TestKit",             // 2: Product
    "0001",                    // 3: Serial
    "Control HID",             // 4: HID interface name
    "Firmware + Log CDC",      // 5: CDC interface name
};

esp_err_t usb_composite_start(void)
{
    const tinyusb_config_t cfg = {
        .device_descriptor        = NULL,    // Kconfig VID/PID 就夠 (dev-mode)
        .string_descriptor        = s_string_desc,
        .string_descriptor_count  = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
        .external_phy             = false,
        .configuration_descriptor = s_configuration_descriptor,
    };
    esp_err_t e = tinyusb_driver_install(&cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "tinyusb install failed: %s", esp_err_to_name(e)); return e; }

    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev  = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
    };
    e = tusb_cdc_acm_init(&acm_cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "cdc_acm init failed: %s", esp_err_to_name(e)); return e; }

    usb_hid_task_start();
    usb_cdc_task_start();

    ESP_LOGI(TAG, "usb composite started (HID IF0 + CDC IF1/IF2)");
    return ESP_OK;
}
