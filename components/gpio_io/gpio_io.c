#include "gpio_io.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "sdkconfig.h"

static const char *TAG = "gpio_io";

// ---- pin map (Kconfig-resolved) --------------------------------------------

static const int s_pins[GPIO_IO_PIN_COUNT] = {
    CONFIG_APP_GPIO_GROUP_A_PIN_0, CONFIG_APP_GPIO_GROUP_A_PIN_1,
    CONFIG_APP_GPIO_GROUP_A_PIN_2, CONFIG_APP_GPIO_GROUP_A_PIN_3,
    CONFIG_APP_GPIO_GROUP_A_PIN_4, CONFIG_APP_GPIO_GROUP_A_PIN_5,
    CONFIG_APP_GPIO_GROUP_A_PIN_6, CONFIG_APP_GPIO_GROUP_A_PIN_7,
    CONFIG_APP_GPIO_GROUP_B_PIN_0, CONFIG_APP_GPIO_GROUP_B_PIN_1,
    CONFIG_APP_GPIO_GROUP_B_PIN_2, CONFIG_APP_GPIO_GROUP_B_PIN_3,
    CONFIG_APP_GPIO_GROUP_B_PIN_4, CONFIG_APP_GPIO_GROUP_B_PIN_5,
    CONFIG_APP_GPIO_GROUP_B_PIN_6, CONFIG_APP_GPIO_GROUP_B_PIN_7,
};

// State word per pin (4 bits used, 4 reserved).
//   bits 0-1: mode (gpio_io_mode_t)
//   bit  2:   level
//   bit  3:   pulsing
static _Atomic uint8_t s_state[GPIO_IO_PIN_COUNT];

#define STATE_MODE(s)     ((gpio_io_mode_t)((s) & 0x03))
#define STATE_LEVEL(s)    (((s) >> 2) & 0x01)
#define STATE_PULSING(s)  (((s) >> 3) & 0x01)
#define MAKE_STATE(m,l,p) ((uint8_t)(((m) & 0x03) | (((l) & 0x01) << 2) | (((p) & 0x01) << 3)))

static _Atomic uint8_t  s_power;
static _Atomic uint32_t s_pulse_width_ms;

// Per-pin one-shot timer handles; created lazily in gpio_io_pulse (Task 7).
// Declared now to keep the file shape stable across tasks.
__attribute__((unused))
static esp_timer_handle_t s_pulse_timer[GPIO_IO_PIN_COUNT];

// ---- helpers ---------------------------------------------------------------

static esp_err_t apply_mode_to_hw(uint8_t idx, gpio_io_mode_t mode)
{
    int pin = s_pins[idx];
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    switch (mode) {
    case GPIO_IO_MODE_INPUT_PULLDOWN:
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
        break;
    case GPIO_IO_MODE_INPUT_PULLUP:
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        break;
    case GPIO_IO_MODE_INPUT_FLOATING:
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        break;
    case GPIO_IO_MODE_OUTPUT:
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return gpio_config(&cfg);
}

// ---- input poll task (20 Hz) -----------------------------------------------

static void gpio_io_poll_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(50);   // 20 Hz
    TickType_t last = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&last, period);
        for (int i = 0; i < GPIO_IO_PIN_COUNT; i++) {
            uint8_t s = atomic_load_explicit(&s_state[i], memory_order_relaxed);
            gpio_io_mode_t m = STATE_MODE(s);
            if (m == GPIO_IO_MODE_OUTPUT) continue;
            int level = gpio_get_level(s_pins[i]);
            uint8_t ns = MAKE_STATE(m, level, STATE_PULSING(s));
            if (ns == s) continue;
            // CAS guards the cross-core race: control_task on core 0 may flip
            // the pin to OUTPUT (or pulse end-callback may clear pulsing)
            // between the load above and this store. If the word changed, drop
            // this sample — the next tick re-evaluates from fresh state.
            uint8_t expected = s;
            (void)atomic_compare_exchange_strong_explicit(
                &s_state[i], &expected, ns,
                memory_order_relaxed, memory_order_relaxed);
        }
    }
}

// ---- public API ------------------------------------------------------------

esp_err_t gpio_io_init(void)
{
    atomic_store_explicit(&s_pulse_width_ms,
                          CONFIG_APP_DEFAULT_PULSE_WIDTH_MS,
                          memory_order_relaxed);
    atomic_store_explicit(&s_power, 0, memory_order_relaxed);

    for (int i = 0; i < GPIO_IO_PIN_COUNT; i++) {
        gpio_io_mode_t m = (i < 8) ? GPIO_IO_MODE_INPUT_PULLDOWN
                                   : GPIO_IO_MODE_OUTPUT;
        esp_err_t e = apply_mode_to_hw(i, m);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config(idx %d, gpio %d) failed: %s",
                     i, s_pins[i], esp_err_to_name(e));
            return e;
        }
        if (m == GPIO_IO_MODE_OUTPUT) {
            gpio_set_level(s_pins[i], 0);
        }
        atomic_store_explicit(&s_state[i], MAKE_STATE(m, 0, 0),
                              memory_order_relaxed);
    }

    BaseType_t ok = xTaskCreate(gpio_io_poll_task, "gpio_io_poll", 2048, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "poll task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "gpio_io ready: 8 inputs (pull-down), 8 outputs (low)");
    return ESP_OK;
}

void gpio_io_get_state(uint8_t idx, gpio_io_state_t *out)
{
    if (!out || idx >= GPIO_IO_PIN_COUNT) return;
    uint8_t s = atomic_load_explicit(&s_state[idx], memory_order_relaxed);
    out->mode    = STATE_MODE(s);
    out->level   = STATE_LEVEL(s);
    out->pulsing = STATE_PULSING(s);
}

void gpio_io_get_all(gpio_io_state_t out[GPIO_IO_PIN_COUNT])
{
    if (!out) return;
    for (int i = 0; i < GPIO_IO_PIN_COUNT; i++) gpio_io_get_state(i, &out[i]);
}

uint32_t gpio_io_get_pulse_width_ms(void)
{
    return atomic_load_explicit(&s_pulse_width_ms, memory_order_relaxed);
}

esp_err_t gpio_io_set_pulse_width_ms(uint32_t width_ms)
{
    if (width_ms < 1)     width_ms = 1;
    if (width_ms > 10000) width_ms = 10000;
    atomic_store_explicit(&s_pulse_width_ms, width_ms, memory_order_relaxed);
    return ESP_OK;
}

// remaining stubs (set_level/pulse/set_power/get_power) stay until the
// next tasks fill them in. Single-writer invariant: every gpio_io_set_*
// and gpio_io_pulse call is funnelled through control_task, so loads of
// s_state[] in this file see no concurrent writers. Pulse end-callback
// only clears pulsing — never starts a new pulse.
esp_err_t gpio_io_set_mode(uint8_t idx, gpio_io_mode_t mode)
{
    if (idx >= GPIO_IO_PIN_COUNT) return ESP_ERR_INVALID_ARG;
    if (mode > GPIO_IO_MODE_OUTPUT) return ESP_ERR_INVALID_ARG;

    // Reject mode change while a pulse is in flight on this pin.
    uint8_t cur = atomic_load_explicit(&s_state[idx], memory_order_relaxed);
    if (STATE_PULSING(cur)) return ESP_ERR_INVALID_STATE;

    esp_err_t e = apply_mode_to_hw(idx, mode);
    if (e != ESP_OK) return e;

    bool level = 0;
    if (mode == GPIO_IO_MODE_OUTPUT) {
        // Preserve the previously driven level if we were already output;
        // otherwise default to 0.
        level = (STATE_MODE(cur) == GPIO_IO_MODE_OUTPUT) ? STATE_LEVEL(cur) : 0;
        gpio_set_level(s_pins[idx], level);
    }
    atomic_store_explicit(&s_state[idx], MAKE_STATE(mode, level, 0),
                          memory_order_relaxed);
    return ESP_OK;
}
esp_err_t gpio_io_set_level(uint8_t idx, bool level)           { (void)idx; (void)level; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t gpio_io_pulse    (uint8_t idx, uint32_t width_ms)    { (void)idx; (void)width_ms; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t gpio_io_set_power(bool on)                            { (void)on; return ESP_ERR_NOT_SUPPORTED; }
bool      gpio_io_get_power(void)                               { return false; }
