#include "psu_backend.h"
#include "esp_log.h"

static const char *TAG = "psu_xy_sk120";

static esp_err_t stub_detect(void)
{
    ESP_LOGW(TAG, "xy_sk120 backend not yet implemented (Task B4)");
    psu_driver_priv_publish_model(0, "XY-SK120", 1000.0f, 5.0f);
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t stub_poll(void)         { return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_voltage(float v) { (void)v; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_current(float i) { (void)i; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_output(bool on)  { (void)on; return ESP_ERR_NOT_SUPPORTED; }

const psu_backend_t psu_backend_xy_sk120 = {
    .name = "xy_sk120", .default_baud = 115200,
    .detect = stub_detect, .poll = stub_poll,
    .set_voltage = stub_set_voltage,
    .set_current = stub_set_current,
    .set_output  = stub_set_output,
};
