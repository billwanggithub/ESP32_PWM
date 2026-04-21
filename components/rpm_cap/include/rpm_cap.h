#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t input_gpio;
    uint8_t    pole_count;
    uint16_t   moving_avg_count;
    uint32_t   rpm_timeout_us;  // 0 = use default (1'000'000)
} rpm_cap_config_t;

esp_err_t rpm_cap_init(const rpm_cap_config_t *cfg);
esp_err_t rpm_cap_set_params(uint8_t pole_count, uint16_t moving_avg_count);
esp_err_t rpm_cap_set_timeout(uint32_t timeout_us);

float  rpm_cap_get_latest(void);
size_t rpm_cap_drain_history(float *dst, size_t max);

#ifdef __cplusplus
}
#endif
