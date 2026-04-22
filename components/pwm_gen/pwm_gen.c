#include "pwm_gen.h"

#include <math.h>
#include <string.h>

#include "driver/mcpwm_prelude.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// MCPWM 的 timer counter 是 16-bit（MCPWM_LL_MAX_COUNT_VALUE = 0x10000），所以
// period_ticks 必須 ∈ [2, 65535]。一個固定 resolution_hz 覆蓋不了 5 Hz ~ 1 MHz，
// 於是用 2-band 動態 resolution：每次 pwm_gen_set() 依 freq 挑適合的 band。
// 同 band 內走 TEZ latch glitch-free 更新，跨 band 需要 teardown→recreate
// timer、有 ~tens of µs 的 output 斷點。
//
// **Critical constraint**: ESP32-S3 MCPWM group 0 share 一個 group prescaler，
// 一旦第一次 new_timer 把 group->prescale committed 之後就不能變。因此 HI / LO
// 兩個 band 的 resolution_hz 必須落在「同一個 group_prescale 下 timer_prescale
// 都 ∈ [1..256]」的範圍內。driver 的 default group_prescale=2（group clock =
// 80 MHz），timer_prescale 範圍 [1..256] 對應 module resolution = 80 MHz ~
// 312.5 kHz。HI 用 10 MHz（timer_prescale=8），LO 用 320 kHz（timer_prescale=250），
// 兩個都在範圍內且共用 group_prescale=2。
//
//   Band  resolution_hz  freq range       period_ticks    duty bits
//   HI    10 MHz         153 Hz – 1 MHz   10 – 65359      3.3 – 16
//   LO    320 kHz        5 Hz – 152 Hz    2105 – 64000    11 – 16
//
// 1 Hz ~ 4 Hz 需要更低 resolution（resolution ≤ 65 kHz），那會超出 group_prescale=2
// 允許的 timer_prescale 範圍，會 trigger "group prescale conflict" error。要延伸到
// 1 Hz 得改用 LEDC peripheral 或第二個 MCPWM group（本檔案 out of scope）。
#define PWM_FREQ_MIN_HZ 5u
#define PWM_FREQ_MAX_HZ 1000000u

typedef struct {
    uint32_t resolution_hz;
    uint32_t freq_min;   // inclusive lower bound; freq < freq_min falls to next band
} pwm_band_t;

// Ordered by descending resolution. First entry with freq >= freq_min wins.
static const pwm_band_t s_bands[] = {
    { 10000000u, 153u },   // HI
    {   320000u,   5u },   // LO
};

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
    uint32_t                  resolution_hz;
} s_pwm;

static const pwm_band_t *pick_band(uint32_t freq_hz)
{
    for (size_t i = 0; i < sizeof(s_bands) / sizeof(s_bands[0]); ++i) {
        if (freq_hz >= s_bands[i].freq_min) return &s_bands[i];
    }
    return NULL;
}

static inline uint32_t freq_to_period_ticks(uint32_t resolution_hz, uint32_t freq_hz)
{
    if (freq_hz == 0) return 0;
    return resolution_hz / freq_hz;
}

uint8_t pwm_gen_duty_resolution_bits(uint32_t freq_hz)
{
    const pwm_band_t *band = pick_band(freq_hz);
    if (!band) return 0;
    uint32_t period = freq_to_period_ticks(band->resolution_hz, freq_hz);
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

    // Start at a safe known state: 1 kHz, 0% duty. Falls into the HI band.
    const uint32_t init_freq = 1000;
    const pwm_band_t *band = pick_band(init_freq);
    s_pwm.resolution_hz = band->resolution_hz;
    s_pwm.period_ticks  = freq_to_period_ticks(band->resolution_hz, init_freq);
    s_pwm.freq_hz       = init_freq;
    s_pwm.duty_pct      = 0.0f;

    mcpwm_timer_config_t timer_cfg = {
        .group_id      = 0,
        .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = s_pwm.resolution_hz,
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
    ESP_LOGI(TAG, "init ok: pwm_gpio=%d trigger_gpio=%d freq=%lu duty=%.1f%% res=%lu",
             s_pwm.pwm_gpio, s_pwm.trigger_gpio,
             (unsigned long)s_pwm.freq_hz, s_pwm.duty_pct,
             (unsigned long)s_pwm.resolution_hz);
    return ESP_OK;
}

// Swap the current timer for a new one at a different resolution_hz.
// Operator / comparator / generator objects are retained; only the timer
// handle is replaced. ESP32-S3 MCPWM group 0 has ONE shared prescaler for
// all timers in the group, so we cannot hold two timers with different
// resolution_hz values at once — the old timer must be fully deleted before
// a new one with a different resolution can be created. This produces a
// brief (~tens of µs) output discontinuity during the swap.
static esp_err_t reconfigure_for_band(const pwm_band_t *band,
                                      uint32_t new_period,
                                      uint32_t new_compare)
{
    esp_err_t err;

    // Stop and tear down the old timer first. All three steps must succeed
    // in order for the group prescaler to be released, freeing it for the
    // new resolution. If anything fails here, log and bail — s_pwm.timer is
    // still valid so the output keeps running on the old timer.
    err = mcpwm_timer_start_stop(s_pwm.timer, MCPWM_TIMER_STOP_EMPTY);
    if (err != ESP_OK) { ESP_LOGE(TAG, "old timer stop: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_timer_disable(s_pwm.timer);
    if (err != ESP_OK) { ESP_LOGE(TAG, "old timer disable: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_del_timer(s_pwm.timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "old timer del: %s", esp_err_to_name(err));
        return err;
    }
    // Old timer is gone. s_pwm.timer is a dangling pointer until we install
    // the new one below. No other task can call pwm_gen_set() concurrently —
    // control_task serializes everything — but if the new_timer step fails we
    // must zero out s_pwm.timer to prevent subsequent use-after-free.
    s_pwm.timer = NULL;

    mcpwm_timer_config_t timer_cfg = {
        .group_id      = 0,
        .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = band->resolution_hz,
        .count_mode    = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks  = new_period,
    };
    err = mcpwm_new_timer(&timer_cfg, &s_pwm.timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new timer: %s", esp_err_to_name(err));
        s_pwm.timer = NULL;
        return err;
    }

    err = mcpwm_operator_connect_timer(s_pwm.oper, s_pwm.timer);
    if (err != ESP_OK) { ESP_LOGE(TAG, "oper connect: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_comparator_set_compare_value(s_pwm.cmpr, new_compare);
    if (err != ESP_OK) { ESP_LOGE(TAG, "cmpr set: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_timer_enable(s_pwm.timer);
    if (err != ESP_OK) { ESP_LOGE(TAG, "timer enable: %s", esp_err_to_name(err)); return err; }

    // ESP32-S3 MCPWM hardware quirk: the timer_prescale register only takes
    // effect on a genuine stop→start edge. Since the old timer occupied this
    // same hardware slot and left timer_start=START_NO_STOP (mode=2) in the
    // register, writing START_NO_STOP again does not re-latch the prescale.
    // Explicitly issue STOP_EMPTY (0) first, then START_NO_STOP (2), so the
    // hardware sees the 0→2 transition and loads the new prescale.
    err = mcpwm_timer_start_stop(s_pwm.timer, MCPWM_TIMER_STOP_EMPTY);
    if (err != ESP_OK) { ESP_LOGE(TAG, "timer stop (latch): %s", esp_err_to_name(err)); return err; }

    err = mcpwm_timer_start_stop(s_pwm.timer, MCPWM_TIMER_START_NO_STOP);
    if (err != ESP_OK) { ESP_LOGE(TAG, "timer start: %s", esp_err_to_name(err)); return err; }

    return ESP_OK;
}

esp_err_t pwm_gen_set(uint32_t freq_hz, float duty_pct)
{
    if (!s_pwm.initialised) return ESP_ERR_INVALID_STATE;
    if (!s_pwm.timer) return ESP_ERR_INVALID_STATE;  // previous reconfigure failed midway
    if (freq_hz < PWM_FREQ_MIN_HZ || freq_hz > PWM_FREQ_MAX_HZ) return ESP_ERR_INVALID_ARG;
    if (duty_pct < 0.0f || duty_pct > 100.0f) return ESP_ERR_INVALID_ARG;

    const pwm_band_t *band = pick_band(freq_hz);
    if (!band) return ESP_ERR_INVALID_ARG;

    uint32_t period = freq_to_period_ticks(band->resolution_hz, freq_hz);
    if (period < 2 || period > 65535) return ESP_ERR_INVALID_ARG;

    uint32_t compare = (uint32_t)lroundf((duty_pct / 100.0f) * (float)period);
    if (compare > period) compare = period;

    if (band->resolution_hz == s_pwm.resolution_hz) {
        // Same band → glitch-free TEZ-latched update.
        esp_err_t err = mcpwm_timer_set_period(s_pwm.timer, period);
        if (err != ESP_OK) return err;
        err = mcpwm_comparator_set_compare_value(s_pwm.cmpr, compare);
        if (err != ESP_OK) return err;
    } else {
        // Band crossing → teardown-reconfigure-restart (brief output glitch).
        esp_err_t err = reconfigure_for_band(band, period, compare);
        if (err != ESP_OK) return err;
    }

    s_pwm.period_ticks  = period;
    s_pwm.freq_hz       = freq_hz;
    s_pwm.duty_pct      = duty_pct;
    s_pwm.resolution_hz = band->resolution_hz;

    // Trigger pulse: a software "settings changed" edge for scope latching.
    // 1 Hz gives 1000 µs; 1 MHz gives 200 µs — both cleanly observable.
    int64_t pulse_us = 2 + 1000000 / (int64_t)freq_hz;
    if (pulse_us > 1000) pulse_us = 1000;
    if (pulse_us <  200) pulse_us =  200;
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
