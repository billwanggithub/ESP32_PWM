#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runs BLE Wi-Fi provisioning if NVS has no credentials, else connects.
// After Wi-Fi associates, starts the HTTP server and WebSocket endpoint.
esp_err_t net_dashboard_start(void);

// Clear stored Wi-Fi credentials and restart. Does not return on success.
// Exposed here so non-network transports (USB CDC, USB HID, BOOT button) can
// trigger the same reprovision flow — every transport frontend calls this
// single entry point.
void net_dashboard_factory_reset(void);

#ifdef __cplusplus
}
#endif
