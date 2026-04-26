#include "psu_backend.h"
#include "psu_modbus_rtu.h"

#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "psu_riden";

#define REG_MODEL    0x0000
#define REG_V_SET    0x0008
#define REG_I_SET    0x0009
#define REG_V_OUT    0x000A
#define REG_I_OUT    0x000B
#define REG_OUTPUT   0x0012

static const struct {
    uint16_t    id;
    const char *name;
    float       i_scale;   // raw_register / i_scale = amps
    float       i_max;
} RD_MODELS[] = {
    { 60062, "RD6006",  1000.0f,  6.0f },
    { 60065, "RD6006P", 1000.0f,  6.0f },
    { 60121, "RD6012",   100.0f, 12.0f },
    { 60125, "RD6012P",  100.0f, 12.0f },
    { 60181, "RD6018",   100.0f, 18.0f },
    { 60241, "RD6024",   100.0f, 24.0f },
};
#define RD_MODELS_N (sizeof(RD_MODELS) / sizeof(RD_MODELS[0]))

static float s_i_scale_div = 1000.0f;   // local cache; also published

static esp_err_t riden_detect(void)
{
    uint16_t model = 0;
    esp_err_t e = psu_modbus_rtu_read_holding(psu_driver_priv_get_slave(),
                                              REG_MODEL, 1, &model);
    psu_driver_priv_note_txn_result(e);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "model detect failed; falling back to RD6006 scale");
        s_i_scale_div = 1000.0f;
        psu_driver_priv_publish_model(0, "unknown", 1000.0f, 6.0f);
        return e;
    }

    const char *name = "unknown";
    float scale = 1000.0f, i_max = 6.0f;
    for (size_t i = 0; i < RD_MODELS_N; i++) {
        if (RD_MODELS[i].id == model) {
            name  = RD_MODELS[i].name;
            scale = RD_MODELS[i].i_scale;
            i_max = RD_MODELS[i].i_max;
            break;
        }
    }
    s_i_scale_div = scale;
    psu_driver_priv_publish_model(model, name, scale, i_max);
    ESP_LOGI(TAG, "detected model %u (%s, I scale = ÷%.0f)",
             model, name, (double)scale);
    return ESP_OK;
}

static esp_err_t riden_poll(void)
{
    uint8_t slave = psu_driver_priv_get_slave();

    uint16_t r[4] = {0};
    esp_err_t e = psu_modbus_rtu_read_holding(slave, REG_V_SET, 4, r);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) {
        psu_driver_priv_publish_v_set(r[0] / 100.0f);
        psu_driver_priv_publish_i_set(r[1] / s_i_scale_div);
        psu_driver_priv_publish_v_out(r[2] / 100.0f);
        psu_driver_priv_publish_i_out(r[3] / s_i_scale_div);
    }

    uint16_t o = 0;
    e = psu_modbus_rtu_read_holding(slave, REG_OUTPUT, 1, &o);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_output(o != 0);

    return ESP_OK;
}

static esp_err_t riden_set_voltage(float v)
{
    if (v < 0.0f)  v = 0.0f;
    if (v > 60.0f) v = 60.0f;
    uint16_t raw = (uint16_t)(v * 100.0f + 0.5f);
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(), REG_V_SET, raw);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_v_set(raw / 100.0f);
    return e;
}

static esp_err_t riden_set_current(float i)
{
    if (i < 0.0f) i = 0.0f;
    float div = s_i_scale_div;
    if (div < 1.0f) div = 1000.0f;   // pre-detect fallback
    uint16_t raw = (uint16_t)(i * div + 0.5f);
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(), REG_I_SET, raw);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_i_set(raw / div);
    return e;
}

static esp_err_t riden_set_output(bool on)
{
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(), REG_OUTPUT, on ? 1 : 0);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_output(on);
    return e;
}

const psu_backend_t psu_backend_riden = {
    .name         = "riden",
    .default_baud = 115200,   // factory default per spec
    .detect       = riden_detect,
    .poll         = riden_poll,
    .set_voltage  = riden_set_voltage,
    .set_current  = riden_set_current,
    .set_output   = riden_set_output,
};
