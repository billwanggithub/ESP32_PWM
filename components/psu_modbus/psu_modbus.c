#include "psu_modbus.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "psu_modbus";

esp_err_t psu_modbus_init(void)            { ESP_LOGI(TAG, "init (stub)");  return ESP_OK; }
esp_err_t psu_modbus_start(void)           { ESP_LOGI(TAG, "start (stub)"); return ESP_OK; }
esp_err_t psu_modbus_set_voltage(float v)  { (void)v; return ESP_OK; }
esp_err_t psu_modbus_set_current(float i)  { (void)i; return ESP_OK; }
esp_err_t psu_modbus_set_output(bool on)   { (void)on; return ESP_OK; }
uint8_t   psu_modbus_get_slave_addr(void)  { return 1; }
esp_err_t psu_modbus_set_slave_addr(uint8_t a) { (void)a; return ESP_OK; }

void psu_modbus_get_telemetry(psu_modbus_telemetry_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

const char *psu_modbus_get_model_name(void) { return "unknown"; }
