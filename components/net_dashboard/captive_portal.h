#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fired when the user submits /save_wifi. Callback receives null-terminated
// ssid and password. Return ESP_OK if the credentials drove a successful
// STA connect; otherwise the captive portal will keep the form open and
// show the error string (err_msg) to the user.
//
// The callback is invoked from the httpd thread and MUST block until it
// knows the outcome (success or failure), because the HTTP response to
// POST /save_wifi is sent after the callback returns.
typedef struct {
    char        ip[16];      // dotted-quad, filled on success
    const char *mdns;        // e.g. "fan-testkit.local" — filled on success
    char        err_msg[64]; // filled on failure
} captive_portal_result_t;

typedef esp_err_t (*captive_portal_creds_cb_t)(const char *ssid,
                                               const char *password,
                                               captive_portal_result_t *out);

// Start the HTTP server on port 80 and register handlers. Does NOT start
// the DNS hijack (that's separate — see dns_hijack.h).
esp_err_t captive_portal_start(captive_portal_creds_cb_t cb);

// Stop the HTTP server.
void captive_portal_stop(void);

#ifdef __cplusplus
}
#endif
