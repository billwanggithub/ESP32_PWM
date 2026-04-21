#pragma once

// Cross-component API for setpoint control. Lives in its own component so
// usb_composite, net_dashboard, and main can share it without any of them
// having to depend on `main` (which ESP-IDF discourages).

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CTRL_CMD_SET_PWM,
    CTRL_CMD_SET_RPM_PARAMS,
    CTRL_CMD_SET_RPM_TIMEOUT,
    CTRL_CMD_OTA_BEGIN,
    CTRL_CMD_OTA_CHUNK,
    CTRL_CMD_OTA_END,
} ctrl_cmd_kind_t;

typedef struct {
    ctrl_cmd_kind_t kind;
    union {
        struct { uint32_t freq_hz; float duty_pct; } set_pwm;
        struct { uint8_t pole; uint16_t mavg; }     set_rpm_params;
        struct { uint32_t timeout_us; }             set_rpm_timeout;
    };
} ctrl_cmd_t;

esp_err_t control_task_start(void);
esp_err_t control_task_post(const ctrl_cmd_t *cmd, TickType_t to);
void      control_task_get_pwm(uint32_t *freq_hz, float *duty_pct);

#ifdef __cplusplus
}
#endif
