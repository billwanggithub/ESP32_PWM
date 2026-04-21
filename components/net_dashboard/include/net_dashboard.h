#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runs BLE Wi-Fi provisioning if NVS has no credentials, else connects.
// After Wi-Fi associates, starts the HTTP server and WebSocket endpoint.
esp_err_t net_dashboard_start(void);

#ifdef __cplusplus
}
#endif
