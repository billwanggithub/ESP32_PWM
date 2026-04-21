#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialises TinyUSB as a composite HID+CDC device, redirects ESP_LOG to CDC,
// and starts the HID and CDC worker tasks. Must be called after Wi-Fi/nvs init
// so logs produced during those steps are still visible on UART0 (USB1).
esp_err_t usb_composite_start(void);

#ifdef __cplusplus
}
#endif
