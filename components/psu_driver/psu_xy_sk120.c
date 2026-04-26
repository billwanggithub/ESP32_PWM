#include "psu_backend.h"
#include "psu_modbus_rtu.h"

#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "psu_xy_sk120";

#define XY_REG_V_SET   0x0000   // ÷100, 2 decimals (V)
#define XY_REG_I_SET   0x0001   // ÷1000, 3 decimals (A)
#define XY_REG_V_OUT   0x0002
#define XY_REG_I_OUT   0x0003
#define XY_REG_OUTPUT  0x0012
#define XY_REG_MODEL   0x0016

#define XY_MODEL_ID    22873     // returned by REG_MODEL on XY-SK120
#define XY_I_MAX       5.0f
#define XY_I_SCALE     1000.0f

static esp_err_t xy_detect(void)
{
    uint16_t model = 0;
    esp_err_t e = psu_modbus_rtu_read_holding(psu_driver_priv_get_slave(),
                                              XY_REG_MODEL, 1, &model);
    psu_driver_priv_note_txn_result(e);
    if (e != ESP_OK) {
        psu_driver_priv_publish_model(0, "XY-SK120", XY_I_SCALE, XY_I_MAX);
        return e;
    }
    const char *name = (model == XY_MODEL_ID) ? "XY-SK120" : "unknown";
    psu_driver_priv_publish_model(model, name, XY_I_SCALE, XY_I_MAX);
    ESP_LOGI(TAG, "detected model %u (%s)", model, name);
    return ESP_OK;
}

static esp_err_t xy_poll(void)
{
    uint8_t slave = psu_driver_priv_get_slave();

    uint16_t r[4] = {0};
    esp_err_t e = psu_modbus_rtu_read_holding(slave, XY_REG_V_SET, 4, r);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) {
        psu_driver_priv_publish_v_set(r[0] / 100.0f);
        psu_driver_priv_publish_i_set(r[1] / XY_I_SCALE);
        psu_driver_priv_publish_v_out(r[2] / 100.0f);
        psu_driver_priv_publish_i_out(r[3] / XY_I_SCALE);
    }

    uint16_t o = 0;
    e = psu_modbus_rtu_read_holding(slave, XY_REG_OUTPUT, 1, &o);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_output(o != 0);

    return ESP_OK;
}

static esp_err_t xy_set_voltage(float v)
{
    if (v < 0.0f)  v = 0.0f;
    if (v > 30.0f) v = 30.0f;   // XY-SK120 V ceiling is 30 V
    uint16_t raw = (uint16_t)(v * 100.0f + 0.5f);
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(),
                                              XY_REG_V_SET, raw);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_v_set(raw / 100.0f);
    return e;
}

static esp_err_t xy_set_current(float i)
{
    if (i < 0.0f)    i = 0.0f;
    if (i > XY_I_MAX) i = XY_I_MAX;
    uint16_t raw = (uint16_t)(i * XY_I_SCALE + 0.5f);
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(),
                                              XY_REG_I_SET, raw);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_i_set(raw / XY_I_SCALE);
    return e;
}

static esp_err_t xy_set_output(bool on)
{
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(),
                                              XY_REG_OUTPUT, on ? 1 : 0);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_output(on);
    return e;
}

const psu_backend_t psu_backend_xy_sk120 = {
    .name         = "xy_sk120",
    .default_baud = 115200,
    .detect       = xy_detect,
    .poll         = xy_poll,
    .set_voltage  = xy_set_voltage,
    .set_current  = xy_set_current,
    .set_output   = xy_set_output,
};
