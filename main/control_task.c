#include "app_api.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "pwm_gen.h"
#include "rpm_cap.h"

static const char *TAG = "control";

static QueueHandle_t     s_cmd_q;
static TaskHandle_t      s_task;
static _Atomic uint32_t  s_freq_hz;
static _Atomic uint32_t  s_duty_bits;   // float bit-punned

static inline void publish_pwm(uint32_t f, float d)
{
    uint32_t bits;
    memcpy(&bits, &d, sizeof(bits));
    atomic_store_explicit(&s_freq_hz,   f,    memory_order_relaxed);
    atomic_store_explicit(&s_duty_bits, bits, memory_order_relaxed);
}

void control_task_get_pwm(uint32_t *freq_hz, float *duty_pct)
{
    uint32_t f = atomic_load_explicit(&s_freq_hz,   memory_order_relaxed);
    uint32_t b = atomic_load_explicit(&s_duty_bits, memory_order_relaxed);
    float d; memcpy(&d, &b, sizeof(d));
    if (freq_hz)  *freq_hz  = f;
    if (duty_pct) *duty_pct = d;
}

static void control_task(void *arg)
{
    ctrl_cmd_t cmd;
    while (true) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.kind) {
        case CTRL_CMD_SET_PWM: {
            esp_err_t e = pwm_gen_set(cmd.set_pwm.freq_hz, cmd.set_pwm.duty_pct);
            if (e == ESP_OK) {
                publish_pwm(cmd.set_pwm.freq_hz, cmd.set_pwm.duty_pct);
                ESP_LOGI(TAG, "pwm set: %lu Hz, %.2f%%",
                         (unsigned long)cmd.set_pwm.freq_hz, cmd.set_pwm.duty_pct);
            } else {
                ESP_LOGW(TAG, "pwm set failed: %s", esp_err_to_name(e));
            }
        } break;
        case CTRL_CMD_SET_RPM_PARAMS:
            rpm_cap_set_params(cmd.set_rpm_params.pole, cmd.set_rpm_params.mavg);
            ESP_LOGI(TAG, "rpm params: pole=%u mavg=%u",
                     cmd.set_rpm_params.pole, cmd.set_rpm_params.mavg);
            break;
        case CTRL_CMD_SET_RPM_TIMEOUT:
            rpm_cap_set_timeout(cmd.set_rpm_timeout.timeout_us);
            ESP_LOGI(TAG, "rpm timeout: %lu us",
                     (unsigned long)cmd.set_rpm_timeout.timeout_us);
            break;
        case CTRL_CMD_OTA_BEGIN:
        case CTRL_CMD_OTA_CHUNK:
        case CTRL_CMD_OTA_END:
            // Wired in Phase 5. For now, silently acknowledge so senders don't block.
            ESP_LOGW(TAG, "OTA command received but ota_core not wired yet");
            break;
        }
    }
}

esp_err_t control_task_post(const ctrl_cmd_t *cmd, TickType_t to)
{
    if (!cmd || !s_cmd_q) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_cmd_q, cmd, to) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t control_task_start(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    s_cmd_q = xQueueCreate(16, sizeof(ctrl_cmd_t));
    if (!s_cmd_q) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreatePinnedToCore(
        control_task, "control", 4096, NULL, 6, &s_task, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
