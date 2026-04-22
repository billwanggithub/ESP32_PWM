#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- HID Report IDs (wire contract with PC host tool) ----------------------

#define USB_HID_REPORT_SET_PWM        0x01   // OUT, 8 B
#define USB_HID_REPORT_SET_RPM        0x02   // OUT, 7 B
#define USB_HID_REPORT_FACTORY_RESET  0x03   // OUT, 1 B  (magic=0xA5 → clear wifi creds + reboot)
#define USB_HID_REPORT_STATUS         0x10   // IN , 16 B
#define USB_HID_REPORT_ACK            0x11   // IN , 6 B

#define USB_HID_FACTORY_RESET_MAGIC   0xA5

// Payloads are little-endian, naturally packed.
typedef struct __attribute__((packed)) {
    uint32_t freq_hz;
    float    duty_pct;
} usb_hid_set_pwm_t;

typedef struct __attribute__((packed)) {
    uint8_t  pole_count;
    uint16_t moving_avg_count;
    uint32_t timeout_us;
} usb_hid_set_rpm_t;

typedef struct __attribute__((packed)) {
    uint32_t freq_hz;
    float    duty_pct;
    float    rpm;
    uint32_t seq;
} usb_hid_status_t;

typedef struct __attribute__((packed)) {
    uint8_t  cmd_id;
    uint8_t  ok;
    uint32_t ts;
} usb_hid_ack_t;

// ---- CDC SLIP-framed ops ---------------------------------------------------

#define USB_CDC_OP_LOG             0x01   // D→H, UTF-8 text
#define USB_CDC_OP_OTA_BEGIN       0x10   // H→D, total_size + signature_offset
#define USB_CDC_OP_OTA_CHUNK       0x11   // H→D, offset + data[]
#define USB_CDC_OP_OTA_END         0x12   // H→D, crc32
#define USB_CDC_OP_OTA_STATUS      0x1F   // D→H, state + progress + error
#define USB_CDC_OP_FACTORY_RESET   0x20   // H→D, uint8_t magic=0xA5 → wipe wifi creds + reboot
#define USB_CDC_OP_FACTORY_ACK     0x21   // D→H, empty payload

#define USB_CDC_FACTORY_RESET_MAGIC 0xA5

#define USB_CDC_SLIP_END        0xC0
#define USB_CDC_SLIP_ESC        0xDB
#define USB_CDC_SLIP_ESC_END    0xDC
#define USB_CDC_SLIP_ESC_ESC    0xDD

typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t signature_offset;
} usb_cdc_ota_begin_t;

typedef struct __attribute__((packed)) {
    uint32_t offset;
    // followed by variable-length data
} usb_cdc_ota_chunk_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t crc32;
} usb_cdc_ota_end_t;

typedef struct __attribute__((packed)) {
    uint8_t  state;
    uint32_t progress;
    uint8_t  error;
} usb_cdc_ota_status_t;

#ifdef __cplusplus
}
#endif
