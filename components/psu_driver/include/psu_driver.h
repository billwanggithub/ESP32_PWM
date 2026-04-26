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
    uint16_t model_id;       // raw model register; 0 if not yet detected
    float    i_scale_div;    // 100.0 or 1000.0; valid only when model_id != 0
} psu_driver_telemetry_t;

esp_err_t psu_driver_init(void);    // brings up UART, loads slave addr + family from NVS
esp_err_t psu_driver_start(void);   // creates psu_task, fires model-detect read

esp_err_t psu_driver_set_voltage(float v);
esp_err_t psu_driver_set_current(float i);
esp_err_t psu_driver_set_output(bool on);

uint8_t   psu_driver_get_slave_addr(void);
esp_err_t psu_driver_set_slave_addr(uint8_t addr);   // 1..255 (Modbus families clamp internally)

void psu_driver_get_telemetry(psu_driver_telemetry_t *out);

// Convenience for device_info JSON.
const char *psu_driver_get_model_name(void);   // "RD6006" | "XY-SK120" | "WZ5005" | "unknown"
float       psu_driver_get_i_max(void);        // model-aware A; 6.0 if not yet detected

const char *psu_driver_get_family(void);                 // active backend name
esp_err_t   psu_driver_set_family(const char *name);     // NVS-set; effective on reboot

#ifdef __cplusplus
}
#endif
