#include "psu_backend.h"
#include "esp_log.h"

static const char *TAG = "psu_wz5005";

static esp_err_t stub_detect(void)
{
    ESP_LOGW(TAG, "wz5005 backend not yet implemented (Task B5)");
    psu_driver_priv_publish_model(0, "WZ5005", 1000.0f, 5.0f);
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t stub_poll(void)         { return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_voltage(float v) { (void)v; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_current(float i) { (void)i; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_output(bool on)  { (void)on; return ESP_ERR_NOT_SUPPORTED; }

const psu_backend_t psu_backend_wz5005 = {
    .name = "wz5005", .default_baud = 19200,
    .detect = stub_detect, .poll = stub_poll,
    .set_voltage = stub_set_voltage,
    .set_current = stub_set_current,
    .set_output  = stub_set_output,
};
