#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    v_set;
    float    i_set;
    float    v_out;
    float    i_out;
    bool     output_on;
    bool     link_ok;
    uint16_t model_id;       // raw register 0x00; 0 if not yet detected
    float    i_scale_div;    // 100.0 or 1000.0; valid only when model_id != 0
} psu_modbus_telemetry_t;

esp_err_t psu_modbus_init(void);    // brings up UART, loads slave addr from NVS
esp_err_t psu_modbus_start(void);   // creates psu_task, fires model-detect read

esp_err_t psu_modbus_set_voltage(float v);
esp_err_t psu_modbus_set_current(float i);
esp_err_t psu_modbus_set_output(bool on);

uint8_t   psu_modbus_get_slave_addr(void);
esp_err_t psu_modbus_set_slave_addr(uint8_t addr);   // 1..247

void psu_modbus_get_telemetry(psu_modbus_telemetry_t *out);

// Convenience for device_info JSON.
const char *psu_modbus_get_model_name(void);   // "RD6006" | "RD6012" | ... | "unknown"

#ifdef __cplusplus
}
#endif
