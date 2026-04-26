#include "pwm_gen.h"

#include <math.h>
#include <string.h>

#include "driver/mcpwm_prelude.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mcpwm_ll.h"
#include "soc/mcpwm_struct.h"
#include "nvs.h"

#define PWM_NVS_NAMESPACE  "pwm_gen"
#define PWM_NVS_KEY_FREQ   "freq_hz"

// MCPWM 的 timer counter 是 16-bit（MCPWM_LL_MAX_COUNT_VALUE = 0x10000），所以
// period_ticks 必須 ∈ [2, 65535]。一個固定 resolution_hz 覆蓋不了 10 Hz ~ 1 MHz，
// 於是用 2-band 動態 resolution：每次 pwm_gen_set() 依 freq 挑適合的 band。
// 同 band 內走 TEZ latch glitch-free 更新，跨 band 需要 teardown→recreate
// timer、有 ~tens of µs 的 output 斷點。
//
// **Critical constraint**: ESP32-S3 MCPWM group 0 share 一個 group prescaler，
// 一旦第一次 new_timer 把 group->prescale committed 之後就不能變。因此 HI / LO
// 兩個 band 的 resolution_hz 必須落在「同一個 group_prescale 下 timer_prescale
// 都 ∈ [1..256]」的範圍內。
//
// **ESP-IDF v6.0 改變**：driver 的 default group_prescale 從 v5.x 的 2 變成 1
// (見 esp_driver_mcpwm/src/mcpwm_private.h:55 MCPWM_GROUP_CLOCK_DEFAULT_PRESCALE)。
// group clock 從 80 MHz 升到 160 MHz，timer_prescale [1..256] 對應 module
// resolution range = 160 MHz ~ 625 kHz。HI 用 10 MHz（timer_prescale=16），
// LO 用 625 kHz（timer_prescale=256），兩個都在範圍內且共用 group_prescale=1。
//
// 為什麼不沿用 v5.x 的 LO=320 kHz？因為 160 MHz / 320 kHz = 500 > 256，driver
// 的 auto-resolver 會嘗試把 group_prescale 改到 2 → group prescale conflict。
// driver 沒提供 public API 可以強制 group_prescale=2 (mcpwm_timer_config_t
// 沒有相關欄位)，第一個 mcpwm_new_timer 是用 HI 10 MHz，160 MHz / 10 MHz = 16
// 落在範圍內，所以 group 就 commit 在 prescale=1 了。
//
//   Band  resolution_hz  freq range       period_ticks    duty bits
//   HI    10 MHz         153 Hz – 1 MHz   10 – 65359      3.3 – 16
//   LO    625 kHz        10 Hz – 152 Hz   4112 – 62500    12 – 16
//
// 1 Hz ~ 9 Hz 需要更低 resolution（resolution < 625 kHz），不可達於目前的
// group_prescale=1 + 16-bit timer 組合。要延伸到 1 Hz 得改用 LEDC peripheral
// (它有獨立的 timer prescaler 跟 div_param fractional divider)，或在 LO band
// 改用 MCPWM group 1（但同顆 GPIO 不能同時掛兩個 group 的 generator，band cross
// 會要 delete+recreate 整條 generator chain，比現有 teardown 還久）。本檔案
// out of scope。
// Range constants live in the public header so other components
// (net_dashboard's /api/device_info) can read them without duplicating literals.
#define PWM_FREQ_MIN_HZ PWM_GEN_FREQ_MIN_HZ
#define PWM_FREQ_MAX_HZ PWM_GEN_FREQ_MAX_HZ

typedef struct {
    uint32_t resolution_hz;
    uint32_t freq_min;   // inclusive lower bound; freq < freq_min falls to next band
} pwm_band_t;

// Ordered by descending resolution. First entry with freq >= freq_min wins.
static const pwm_band_t s_bands[] = {
    { 10000000u, 153u },   // HI
    {   625000u,  10u },   // LO  (v6.0: was 320 kHz / 5 Hz under v5.x)
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

    // Start at a safe known state: 10 kHz, 0% duty. Falls into the HI band.
    const uint32_t init_freq = 10000;
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

    // ESP32-S3 MCPWM `timer_period` and `timer_prescale` live in a shadow
    // register; even with `upmethod=0` (immediate), the active register
    // flush is intermittent on a band-cross teardown→recreate. Direct
    // hardware verification: right after `mcpwm_new_timer` returns, reading
    // `timer_status.timer_value` shows the counter still at the OLD peak
    // (e.g. ~25000) while the shadow shows the NEW peak (e.g. 2000) —
    // shadow ≠ active. The symptom on the pin is `old_resolution /
    // new_shadow_peak`, e.g. a 1 kHz request outputs 62.5 Hz
    // (= 625 kHz / 10000) and a 100 Hz request outputs 1.6 kHz
    // (= 10 MHz / 6250).
    //
    // Force flush by software-syncing the timer to phase=0: that reloads
    // the counter to 0, which is itself a TEZ event, which flushes
    // shadow→active for both prescale and period atomically. Sync input
    // must be enabled for the soft trigger to take effect;
    // `mcpwm_hal_timer_reset` (called inside `mcpwm_new_timer` in v6.0)
    // had explicitly disabled it, so we re-enable, fire, then disable.
    //
    // Replaces an earlier LOGD-induced-delay workaround that used to sit
    // in `pwm_gen_init` (set "mcpwm" to ESP_LOG_DEBUG) plus a
    // STOP_EMPTY→START_NO_STOP dance after `mcpwm_timer_enable`. Both
    // narrowed the race but never closed it under Wi-Fi RX / telemetry
    // load. This soft-sync is the actual fix.
    MCPWM0.timer[0].timer_sync.timer_phase = 0;
    MCPWM0.timer[0].timer_sync.timer_phase_direction = 0;
    MCPWM0.timer[0].timer_sync.timer_synci_en = 1;
    MCPWM0.timer[0].timer_sync.timer_sync_sw = ~MCPWM0.timer[0].timer_sync.timer_sync_sw;
    MCPWM0.timer[0].timer_sync.timer_synci_en = 0;

    err = mcpwm_operator_connect_timer(s_pwm.oper, s_pwm.timer);
    if (err != ESP_OK) { ESP_LOGE(TAG, "oper connect: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_comparator_set_compare_value(s_pwm.cmpr, new_compare);
    if (err != ESP_OK) { ESP_LOGE(TAG, "cmpr set: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_timer_enable(s_pwm.timer);
    if (err != ESP_OK) { ESP_LOGE(TAG, "timer enable: %s", esp_err_to_name(err)); return err; }

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

uint32_t pwm_gen_load_saved_freq(uint32_t fallback_hz)
{
    nvs_handle_t h;
    if (nvs_open(PWM_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return fallback_hz;
    uint32_t f = fallback_hz;
    (void)nvs_get_u32(h, PWM_NVS_KEY_FREQ, &f);
    nvs_close(h);
    if (f < PWM_GEN_FREQ_MIN_HZ || f > PWM_GEN_FREQ_MAX_HZ) return fallback_hz;
    return f;
}

esp_err_t pwm_gen_save_current_freq_to_nvs(void)
{
    uint32_t f; float d;
    pwm_gen_get(&f, &d);
    if (f < PWM_GEN_FREQ_MIN_HZ || f > PWM_GEN_FREQ_MAX_HZ) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t e = nvs_open(PWM_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    esp_err_t es = nvs_set_u32(h, PWM_NVS_KEY_FREQ, f);
    esp_err_t ec = (es == ESP_OK) ? nvs_commit(h) : ESP_OK;
    nvs_close(h);
    if (es != ESP_OK) return es;
    if (ec != ESP_OK) return ec;
    ESP_LOGI(TAG, "saved to NVS: freq_hz=%lu", (unsigned long)f);
    return ESP_OK;
}
