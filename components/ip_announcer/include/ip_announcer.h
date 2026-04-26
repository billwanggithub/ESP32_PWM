#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Settings shape — public so transports / dashboard can populate it.
typedef struct {
    bool        enable;
    char        topic[65];     // 64-char + NUL
    char        server[97];    // hostname-only
    uint8_t     priority;      // 1..5
} ip_announcer_settings_t;

// Telemetry shape — published in 20 Hz WS status frame and HID/CDC mirrors.
typedef enum {
    IP_ANN_STATUS_NEVER = 0,
    IP_ANN_STATUS_OK,
    IP_ANN_STATUS_FAILED,
    IP_ANN_STATUS_DISABLED,
} ip_announcer_status_t;

typedef struct {
    ip_announcer_status_t status;
    int                   last_http_code;       // 0 if never tried
    char                  last_pushed_ip[16];   // dotted-quad, "" if never
    char                  last_err[48];         // human-readable, "" on success
    int64_t               last_attempt_ms;      // esp_timer_get_time / 1000
} ip_announcer_telemetry_t;

// Lifecycle (called from app_main BEFORE net_dashboard_start).
esp_err_t ip_announcer_init(void);

// Settings getters/setters. set persists to NVS.
esp_err_t ip_announcer_get_settings(ip_announcer_settings_t *out);
esp_err_t ip_announcer_set_settings(const ip_announcer_settings_t *in);

// Toggle enable only (used by HID 0x07 op 0x01).
esp_err_t ip_announcer_set_enable(bool enable);

// Read-only telemetry snapshot.
void ip_announcer_get_telemetry(ip_announcer_telemetry_t *out);

// Trigger an immediate push of the current STA IP. Async — returns
// ESP_OK after enqueueing. Implemented in Phase 2.
esp_err_t ip_announcer_test_push(void);

#ifdef __cplusplus
}
#endif
