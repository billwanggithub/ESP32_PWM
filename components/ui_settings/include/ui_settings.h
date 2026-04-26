#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Live values default to (1.0%, 100 Hz). Loaded from NVS on first call to
// ui_settings_init(); subsequent ui_settings_get_steps() calls read the
// in-memory cache (no NVS hit per access — telemetry runs at 20 Hz).
esp_err_t ui_settings_init(void);

void ui_settings_get_steps(float *duty_step, uint16_t *freq_step);
esp_err_t ui_settings_save_steps(float duty_step, uint16_t freq_step);

#ifdef __cplusplus
}
#endif
