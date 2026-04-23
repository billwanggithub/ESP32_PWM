#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start a UDP:53 listener on the SoftAP netif. Every A-record query is
// answered with 192.168.4.1 (the AP gateway). This is what makes
// Android's captive-portal detector fire and auto-open a browser.
// Safe to call only while WIFI_MODE_AP or WIFI_MODE_APSTA is active.
esp_err_t dns_hijack_start(void);

// Stop the DNS task and close the socket. Blocks up to ~500ms waiting
// for the task to exit its recv loop.
void dns_hijack_stop(void);

#ifdef __cplusplus
}
#endif
