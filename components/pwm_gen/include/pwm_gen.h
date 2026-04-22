#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t pwm_gpio;
    gpio_num_t trigger_gpio;
} pwm_gen_config_t;

esp_err_t pwm_gen_init(const pwm_gen_config_t *cfg);

// Glitch-free update within a resolution band. Latches at the next period
// boundary (TEZ). Valid freq range is 5 Hz .. 1_000_000 Hz. Crossing the
// HI↔LO band boundary at 152↔153 Hz causes a brief (~tens of µs) output
// discontinuity while the MCPWM timer is reconfigured.
// Returns ESP_ERR_INVALID_ARG if freq_hz is out of range or duty_pct is out
// of [0, 100].
esp_err_t pwm_gen_set(uint32_t freq_hz, float duty_pct);

void pwm_gen_get(uint32_t *freq_hz, float *duty_pct);

// Effective duty resolution (bits) at the given frequency, accounting for
// the 2-band dynamic resolution table (HI=10 MHz, LO=320 kHz).
uint8_t pwm_gen_duty_resolution_bits(uint32_t freq_hz);

#ifdef __cplusplus
}
#endif
