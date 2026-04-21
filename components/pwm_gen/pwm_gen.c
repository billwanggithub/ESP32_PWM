#include "pwm_gen.h"

#include <math.h>
#include <string.h>

#include "driver/mcpwm_prelude.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PWM_SRC_CLK_HZ 160000000u

static const char *TAG = "pwm_gen";

static struct {
    bool                      initialised;
    gpio_num_t                pwm_gpio;
    gpio_num_t                trigger_gpio;
    mcpwm_timer_handle_t      timer;
    mcpwm_oper_handle_t       oper;
    mcpwm_cmpr_handle_t       cmpr;
    mcpwm_gen_handle_t        gen;
    uint32_t                  freq_hz;
    float                     duty_pct;
    uint32_t                  period_ticks;
} s_pwm;

static inline uint32_t freq_to_period_ticks(uint32_t freq_hz)
{
    if (freq_hz == 0) return 0;
    return PWM_SRC_CLK_HZ / freq_hz;
}

uint8_t pwm_gen_duty_resolution_bits(uint32_t freq_hz)
{
    uint32_t period = freq_to_period_ticks(freq_hz);
    if (period < 2) return 0;
    uint8_t bits = 0;
    while ((1u << bits) <= period) bits++;
    return bits ? (uint8_t)(bits - 1) : 0;
}

esp_err_t pwm_gen_init(const pwm_gen_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_pwm.initialised) return ESP_ERR_INVALID_STATE;

    memset(&s_pwm, 0, sizeof(s_pwm));
    s_pwm.pwm_gpio     = cfg->pwm_gpio;
    s_pwm.trigger_gpio = cfg->trigger_gpio;

    // Start at a safe known state: 1 kHz, 0% duty.
    const uint32_t init_freq = 1000;
    s_pwm.period_ticks = freq_to_period_ticks(init_freq);
    s_pwm.freq_hz      = init_freq;
    s_pwm.duty_pct     = 0.0f;

    mcpwm_timer_config_t timer_cfg = {
        .group_id      = 0,
        .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = PWM_SRC_CLK_HZ,
        .count_mode    = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks  = s_pwm.period_ticks,
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_cfg, &s_pwm.timer));

    mcpwm_operator_config_t oper_cfg = { .group_id = 0 };
    ESP_ERROR_CHECK(mcpwm_new_operator(&oper_cfg, &s_pwm.oper));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(s_pwm.oper, s_pwm.timer));

    mcpwm_comparator_config_t cmpr_cfg = { .flags.update_cmp_on_tez = true };
    ESP_ERROR_CHECK(mcpwm_new_comparator(s_pwm.oper, &cmpr_cfg, &s_pwm.cmpr));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_pwm.cmpr, 0));

    mcpwm_generator_config_t gen_cfg = { .gen_gpio_num = s_pwm.pwm_gpio };
    ESP_ERROR_CHECK(mcpwm_new_generator(s_pwm.oper, &gen_cfg, &s_pwm.gen));

    // High on timer empty, low when compare hits → standard PWM.
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(
        s_pwm.gen, MCPWM_GEN_TIMER_EVENT_ACTION(
            MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(
        s_pwm.gen, MCPWM_GEN_COMPARE_EVENT_ACTION(
            MCPWM_TIMER_DIRECTION_UP, s_pwm.cmpr, MCPWM_GEN_ACTION_LOW)));

    // Trigger GPIO: plain push-pull, idle low.
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << s_pwm.trigger_gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    gpio_set_level(s_pwm.trigger_gpio, 0);

    ESP_ERROR_CHECK(mcpwm_timer_enable(s_pwm.timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_pwm.timer, MCPWM_TIMER_START_NO_STOP));

    s_pwm.initialised = true;
    ESP_LOGI(TAG, "init ok: pwm_gpio=%d trigger_gpio=%d freq=%lu duty=%.1f%%",
             s_pwm.pwm_gpio, s_pwm.trigger_gpio,
             (unsigned long)s_pwm.freq_hz, s_pwm.duty_pct);
    return ESP_OK;
}

esp_err_t pwm_gen_set(uint32_t freq_hz, float duty_pct)
{
    if (!s_pwm.initialised) return ESP_ERR_INVALID_STATE;
    if (freq_hz == 0 || freq_hz > 1000000u) return ESP_ERR_INVALID_ARG;
    if (duty_pct < 0.0f || duty_pct > 100.0f) return ESP_ERR_INVALID_ARG;

    uint32_t period = freq_to_period_ticks(freq_hz);
    if (period < 2) return ESP_ERR_INVALID_ARG;

    uint32_t compare = (uint32_t)lroundf((duty_pct / 100.0f) * (float)period);
    if (compare > period) compare = period;

    esp_err_t err = mcpwm_timer_set_period(s_pwm.timer, period);
    if (err != ESP_OK) return err;
    err = mcpwm_comparator_set_compare_value(s_pwm.cmpr, compare);
    if (err != ESP_OK) return err;

    s_pwm.period_ticks = period;
    s_pwm.freq_hz      = freq_hz;
    s_pwm.duty_pct     = duty_pct;

    // Trigger pulse: hold for one period + a small margin so the scope latches it.
    // The new settings are already latched at the next TEZ; a short software pulse
    // here is the "settings changed" edge the user's system can observe.
    int64_t pulse_us = 2 + (1000000 / (int64_t)freq_hz);
    if (pulse_us > 1000) pulse_us = 1000;
    gpio_set_level(s_pwm.trigger_gpio, 1);
    esp_rom_delay_us((uint32_t)pulse_us);
    gpio_set_level(s_pwm.trigger_gpio, 0);

    return ESP_OK;
}

void pwm_gen_get(uint32_t *freq_hz, float *duty_pct)
{
    if (freq_hz)  *freq_hz  = s_pwm.freq_hz;
    if (duty_pct) *duty_pct = s_pwm.duty_pct;
}
