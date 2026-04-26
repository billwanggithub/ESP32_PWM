#include "ui_settings.h"

#include <string.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "ui_settings";

#define NVS_NAMESPACE   "ui_settings"
#define NVS_KEY_DUTY    "duty_step"
#define NVS_KEY_FREQ    "freq_step"

#define DEFAULT_DUTY_STEP   1.0f
#define DEFAULT_FREQ_STEP   100u

// Atomic-bit-punned float, same trick as control_task / rpm_cap.
static _Atomic uint32_t s_duty_bits;
static _Atomic uint16_t s_freq_step;

static inline void store_duty_f(float v)
{
    uint32_t b; memcpy(&b, &v, sizeof(b));
    atomic_store_explicit(&s_duty_bits, b, memory_order_relaxed);
}
static inline float load_duty_f(void)
{
    uint32_t b = atomic_load_explicit(&s_duty_bits, memory_order_relaxed);
    float v; memcpy(&v, &b, sizeof(v));
    return v;
}

esp_err_t ui_settings_init(void)
{
    store_duty_f(DEFAULT_DUTY_STEP);
    atomic_store_explicit(&s_freq_step, DEFAULT_FREQ_STEP, memory_order_relaxed);

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open ro failed: %s", esp_err_to_name(e));
        return ESP_OK;
    }
    float    d = DEFAULT_DUTY_STEP;
    uint16_t f = DEFAULT_FREQ_STEP;
    size_t sz = sizeof(d);
    if (nvs_get_blob(h, NVS_KEY_DUTY, &d, &sz) == ESP_OK && sz == sizeof(d) && d > 0.0f) {
        store_duty_f(d);
    }
    if (nvs_get_u16(h, NVS_KEY_FREQ, &f) == ESP_OK && f > 0) {
        atomic_store_explicit(&s_freq_step, f, memory_order_relaxed);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "init: duty_step=%.2f freq_step=%u", load_duty_f(),
             (unsigned)atomic_load(&s_freq_step));
    return ESP_OK;
}

void ui_settings_get_steps(float *duty_step, uint16_t *freq_step)
{
    if (duty_step) *duty_step = load_duty_f();
    if (freq_step) *freq_step = atomic_load_explicit(&s_freq_step, memory_order_relaxed);
}

esp_err_t ui_settings_save_steps(float duty_step, uint16_t freq_step)
{
    if (duty_step <= 0.0f) return ESP_ERR_INVALID_ARG;
    if (freq_step == 0)    return ESP_ERR_INVALID_ARG;

    store_duty_f(duty_step);
    atomic_store_explicit(&s_freq_step, freq_step, memory_order_relaxed);

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    esp_err_t e1 = nvs_set_blob(h, NVS_KEY_DUTY, &duty_step, sizeof(duty_step));
    esp_err_t e2 = nvs_set_u16 (h, NVS_KEY_FREQ, freq_step);
    esp_err_t ec = (e1 == ESP_OK && e2 == ESP_OK) ? nvs_commit(h) : ESP_OK;
    nvs_close(h);
    if (e1 != ESP_OK) return e1;
    if (e2 != ESP_OK) return e2;
    if (ec != ESP_OK) return ec;
    ESP_LOGI(TAG, "saved: duty_step=%.2f freq_step=%u", duty_step, (unsigned)freq_step);
    return ESP_OK;
}
