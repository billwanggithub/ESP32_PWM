#include <string.h>

#include "tusb.h"
#include "class/hid/hid_device.h"

// HID report map matching the spec:
//   0x01 OUT: set_pwm        (uint32 freq, float duty)            8 B
//   0x02 OUT: set_rpm_params (uint8 pole, uint16 mavg, uint32 us) 7 B
//   0x10 IN : status         (u32 freq, f32 duty, f32 rpm, u32 seq) 16 B
//   0x11 IN : ack            (u8 cmd_id, u8 ok, u32 ts)            6 B
// We use a single top-level vendor-defined collection with Report IDs.

enum { REPORT_ID_SET_PWM = 0x01, REPORT_ID_SET_RPM = 0x02,
       REPORT_ID_STATUS  = 0x10, REPORT_ID_ACK     = 0x11 };

const uint8_t usb_hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,                 // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,                       // Usage (Vendor Usage 0x01)
    0xA1, 0x01,                       //   Collection (Application)

    // 0x01 OUT set_pwm: 8 bytes
    0x85, REPORT_ID_SET_PWM,
    0x09, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 8,
    0x91, 0x02,

    // 0x02 OUT set_rpm: 7 bytes
    0x85, REPORT_ID_SET_RPM,
    0x09, 0x03,
    0x75, 0x08, 0x95, 7,
    0x91, 0x02,

    // 0x10 IN status: 16 bytes
    0x85, REPORT_ID_STATUS,
    0x09, 0x10,
    0x75, 0x08, 0x95, 16,
    0x81, 0x02,

    // 0x11 IN ack: 6 bytes
    0x85, REPORT_ID_ACK,
    0x09, 0x11,
    0x75, 0x08, 0x95, 6,
    0x81, 0x02,

    0xC0,                             // End Collection
};

const size_t usb_hid_report_descriptor_size = sizeof(usb_hid_report_descriptor);

// TinyUSB expects this specific callback name.
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return usb_hid_report_descriptor;
}
