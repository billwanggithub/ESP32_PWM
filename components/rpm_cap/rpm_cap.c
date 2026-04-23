#include "rpm_cap.h"

#include <string.h>
#include <stdatomic.h>
#include <inttypes.h>

#include "driver/mcpwm_prelude.h"
#include "driver/gpio.h"   // ESP-IDF v6.0: gpio_set_pull_mode() replaces removed
                            // mcpwm_capture_channel_config_t.flags.pull_up.
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CAP_SRC_CLK_HZ 160000000u
#define FREQ_FIFO_LEN  128
#define RPM_FIFO_LEN   128
#define HISTORY_LEN    256

// ISR edge-rate ceiling. Real fan tachs top out at ~1 kHz (2 pulses/rev ×
// ~30k RPM); 10 kHz gives 10× headroom. Above this the ISR skips the
// expensive esp_timer stop/start + notify path, because at 500 kHz
// loopback the per-edge esp_timer spinlock contention triggers the
// Interrupt WDT (pwm_gen output wired to cap input hits this).
// 100 µs × 160 MHz = 16'000 capture ticks.
#define MIN_EDGE_PERIOD_TICKS  16000u

// A period value equal to rpm_timeout_us (in clock ticks) is our "signal lost"
// sentinel. The converter turns it into 0.0f RPM.
#define SENTINEL_RPM_VALUE 0.0f

static const char *TAG = "rpm_cap";

// ---- SPSC ring buffers (ISR producer → task consumer, task → task) ---------
// Single producer, single consumer, power-of-two capacity so mask works.

typedef struct {
    volatile uint32_t head;      // producer
    volatile uint32_t tail;      // consumer
    uint32_t          mask;
    uint32_t         *buf;
} ring_u32_t;

typedef struct {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t          mask;
    float            *buf;
} ring_f32_t;

static uint32_t  s_freq_buf[FREQ_FIFO_LEN];
static float     s_rpm_buf [RPM_FIFO_LEN];
static ring_u32_t s_freq_fifo = { 0, 0, FREQ_FIFO_LEN - 1, s_freq_buf };
static ring_f32_t s_rpm_fifo  = { 0, 0, RPM_FIFO_LEN  - 1, s_rpm_buf  };

static inline bool ring_u32_push_from_isr(ring_u32_t *r, uint32_t v)
{
    uint32_t head = r->head;
    uint32_t next = (head + 1) & r->mask;
    if (next == r->tail) return false;   // full: drop
    r->buf[head] = v;
    r->head = next;
    return true;
}

static inline bool ring_u32_pop(ring_u32_t *r, uint32_t *out)
{
    uint32_t tail = r->tail;
    if (tail == r->head) return false;
    *out = r->buf[tail];
    r->tail = (tail + 1) & r->mask;
    return true;
}

static inline bool ring_f32_push(ring_f32_t *r, float v)
{
    uint32_t head = r->head;
    uint32_t next = (head + 1) & r->mask;
    if (next == r->tail) return false;
    r->buf[head] = v;
    r->head = next;
    return true;
}

static inline bool ring_f32_pop(ring_f32_t *r, float *out)
{
    uint32_t tail = r->tail;
    if (tail == r->head) return false;
    *out = r->buf[tail];
    r->tail = (tail + 1) & r->mask;
    return true;
}

// ---- Module state -----------------------------------------------------------

static struct {
    bool                         initialised;
    gpio_num_t                   input_gpio;
    mcpwm_cap_timer_handle_t     cap_timer;
    mcpwm_cap_channel_handle_t   cap_chan;
    esp_timer_handle_t           timeout_timer;

    _Atomic uint8_t              pole_count;
    _Atomic uint16_t             moving_avg_count;
    _Atomic uint32_t             rpm_timeout_us;

    TaskHandle_t                 converter_task;
    TaskHandle_t                 averager_task;

    volatile uint32_t            prev_cap_value;
    volatile bool                have_prev;

    // Averaging (owned by averager_task)
    float                        history[HISTORY_LEN];
    uint32_t                     history_head;
    uint32_t                     history_filled;

    float                        mavg_ring[HISTORY_LEN];
    uint32_t                     mavg_head;
    uint32_t                     mavg_filled;
    double                       mavg_sum;

    _Atomic uint32_t             latest_rpm_bits;   // bit-punned float
} s_cap;

static inline void atomic_store_rpm(float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    atomic_store_explicit(&s_cap.latest_rpm_bits, bits, memory_order_relaxed);
}

float rpm_cap_get_latest(void)
{
    uint32_t bits = atomic_load_explicit(&s_cap.latest_rpm_bits, memory_order_relaxed);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

// ---- ISR and timeout --------------------------------------------------------

static bool IRAM_ATTR on_cap_edge(mcpwm_cap_channel_handle_t chan,
                                  const mcpwm_capture_event_data_t *edata,
                                  void *user_ctx)
{
    BaseType_t hpw = pdFALSE;
    uint32_t cap = edata->cap_value;

    // Edge-rate guard: at very high input rates (e.g. pwm output looped
    // back to cap input at 500 kHz), per-edge esp_timer_stop/start_once
    // contends the global esp_timer spinlock badly enough to trip IWDT.
    // Drop edges faster than MIN_EDGE_PERIOD_TICKS without touching the
    // expensive paths; keep prev_cap_value current so the next
    // in-range period is still measured correctly.
    if (s_cap.have_prev) {
        uint32_t period = cap - s_cap.prev_cap_value;  // unsigned wrap safe
        if (period < MIN_EDGE_PERIOD_TICKS) {
            s_cap.prev_cap_value = cap;
            return false;
        }
        ring_u32_push_from_isr(&s_freq_fifo, period);
        vTaskNotifyGiveFromISR(s_cap.converter_task, &hpw);
    }
    s_cap.prev_cap_value = cap;
    s_cap.have_prev = true;

    // Rearm timeout from ISR context.
    uint32_t timeout_us = atomic_load_explicit(&s_cap.rpm_timeout_us, memory_order_relaxed);
    esp_timer_stop(s_cap.timeout_timer);
    esp_timer_start_once(s_cap.timeout_timer, timeout_us);

    return hpw == pdTRUE;
}

static void on_timeout(void *arg)
{
    // No edge within timeout window: publish "signal lost" sentinel.
    // We push the timeout period itself; converter recognises it and emits 0 RPM.
    uint32_t timeout_us = atomic_load_explicit(&s_cap.rpm_timeout_us, memory_order_relaxed);
    uint64_t ticks = ((uint64_t)timeout_us * CAP_SRC_CLK_HZ) / 1000000ull;
    if (ticks == 0 || ticks > UINT32_MAX) ticks = UINT32_MAX;

    // Mark sentinel with MSB set so the converter can distinguish unambiguously.
    uint32_t sentinel = 0x80000000u | (uint32_t)(ticks & 0x7FFFFFFFu);
    ring_u32_push_from_isr(&s_freq_fifo, sentinel);
    BaseType_t hpw = pdFALSE;
    vTaskNotifyGiveFromISR(s_cap.converter_task, &hpw);

    // Reset edge-diff state so next real edge starts a fresh measurement.
    s_cap.have_prev = false;

    if (hpw == pdTRUE) portYIELD_FROM_ISR();
}

// ---- Converter task ---------------------------------------------------------

static void converter_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t raw;
        while (ring_u32_pop(&s_freq_fifo, &raw)) {
            float rpm;
            if (raw & 0x80000000u) {
                rpm = SENTINEL_RPM_VALUE;  // timeout → 0 RPM
            } else {
                float freq_hz = (float)CAP_SRC_CLK_HZ / (float)raw;
                uint8_t pole = atomic_load_explicit(&s_cap.pole_count, memory_order_relaxed);
                if (pole == 0) pole = 1;
                rpm = freq_hz * 60.0f / (float)pole;
            }
            ring_f32_push(&s_rpm_fifo, rpm);
            xTaskNotifyGive(s_cap.averager_task);
        }
    }
}

// ---- Averager task ----------------------------------------------------------

static void averager_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        float rpm;
        while (ring_f32_pop(&s_rpm_fifo, &rpm)) {
            // History ring (for chart drain)
            s_cap.history[s_cap.history_head] = rpm;
            s_cap.history_head = (s_cap.history_head + 1) % HISTORY_LEN;
            if (s_cap.history_filled < HISTORY_LEN) s_cap.history_filled++;

            // Moving average ring, constant-time update
            uint16_t window = atomic_load_explicit(&s_cap.moving_avg_count, memory_order_relaxed);
            if (window == 0) window = 1;
            if (window > HISTORY_LEN) window = HISTORY_LEN;

            if (s_cap.mavg_filled < window) {
                s_cap.mavg_sum += rpm;
                s_cap.mavg_ring[s_cap.mavg_head] = rpm;
                s_cap.mavg_head = (s_cap.mavg_head + 1) % HISTORY_LEN;
                s_cap.mavg_filled++;
            } else {
                uint32_t oldest_idx = (s_cap.mavg_head + HISTORY_LEN - window) % HISTORY_LEN;
                s_cap.mavg_sum -= s_cap.mavg_ring[oldest_idx];
                s_cap.mavg_sum += rpm;
                s_cap.mavg_ring[s_cap.mavg_head] = rpm;
                s_cap.mavg_head = (s_cap.mavg_head + 1) % HISTORY_LEN;
            }

            float avg = (float)(s_cap.mavg_sum / (double)s_cap.mavg_filled);
            atomic_store_rpm(avg);
        }
    }
}

// ---- Public API -------------------------------------------------------------

esp_err_t rpm_cap_set_params(uint8_t pole_count, uint16_t moving_avg_count)
{
    if (pole_count == 0) return ESP_ERR_INVALID_ARG;
    if (moving_avg_count == 0) return ESP_ERR_INVALID_ARG;
    atomic_store_explicit(&s_cap.pole_count, pole_count, memory_order_relaxed);
    atomic_store_explicit(&s_cap.moving_avg_count, moving_avg_count, memory_order_relaxed);
    return ESP_OK;
}

esp_err_t rpm_cap_set_timeout(uint32_t timeout_us)
{
    if (timeout_us < 1000) return ESP_ERR_INVALID_ARG;  // 1 ms floor
    atomic_store_explicit(&s_cap.rpm_timeout_us, timeout_us, memory_order_relaxed);
    return ESP_OK;
}

size_t rpm_cap_drain_history(float *dst, size_t max)
{
    if (!dst || max == 0) return 0;
    size_t n = s_cap.history_filled < max ? s_cap.history_filled : max;
    // Copy oldest-first.
    uint32_t start = (s_cap.history_head + HISTORY_LEN - s_cap.history_filled) % HISTORY_LEN;
    for (size_t i = 0; i < n; i++) {
        dst[i] = s_cap.history[(start + i) % HISTORY_LEN];
    }
    return n;
}

esp_err_t rpm_cap_init(const rpm_cap_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_cap.initialised) return ESP_ERR_INVALID_STATE;

    memset(&s_cap, 0, sizeof(s_cap));
    s_cap.input_gpio = cfg->input_gpio;
    atomic_store_explicit(&s_cap.pole_count,
        cfg->pole_count ? cfg->pole_count : 2, memory_order_relaxed);
    atomic_store_explicit(&s_cap.moving_avg_count,
        cfg->moving_avg_count ? cfg->moving_avg_count : 16, memory_order_relaxed);
    atomic_store_explicit(&s_cap.rpm_timeout_us,
        cfg->rpm_timeout_us ? cfg->rpm_timeout_us : 1000000u, memory_order_relaxed);

    mcpwm_capture_timer_config_t cap_timer_cfg = {
        .group_id   = 0,
        .clk_src    = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&cap_timer_cfg, &s_cap.cap_timer));

    // v6.0 拔掉 capture channel config 上的 .flags.pull_up，改成獨立 GPIO call。
    // 順序: pull mode 必須在 mcpwm_new_capture_channel 之前 set，因為 new_channel
    // 會 take over GPIO matrix routing；之後再改 pull mode 不會被覆蓋（pull
    // 是 pad-level 設定，跟 IO_MUX function selection 正交），但放前面語意比較
    // 清楚 — pin 完全 configure 完才 hand off 給 MCPWM。
    ESP_ERROR_CHECK(gpio_set_pull_mode(s_cap.input_gpio, GPIO_PULLUP_ONLY));

    mcpwm_capture_channel_config_t cap_ch_cfg = {
        .gpio_num  = s_cap.input_gpio,
        .prescale  = 1,
        .flags.pos_edge = true,
        .flags.neg_edge = false,
    };
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(s_cap.cap_timer, &cap_ch_cfg, &s_cap.cap_chan));

    mcpwm_capture_event_callbacks_t cbs = { .on_cap = on_cap_edge };
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(s_cap.cap_chan, &cbs, NULL));

    esp_timer_create_args_t tim_cfg = {
        .callback = on_timeout,
        .name     = "rpm_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tim_cfg, &s_cap.timeout_timer));

    // Tasks first (ISR notifies them); pinned to APP_CPU to keep wifi/httpd on PRO_CPU.
    xTaskCreatePinnedToCore(converter_task, "rpm_conv", 3072, NULL, 5, &s_cap.converter_task, 1);
    xTaskCreatePinnedToCore(averager_task,  "rpm_avg",  3072, NULL, 4, &s_cap.averager_task,  1);

    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(s_cap.cap_chan));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(s_cap.cap_timer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(s_cap.cap_timer));

    // Arm initial timeout so a no-signal startup still publishes 0 RPM.
    uint32_t t = atomic_load_explicit(&s_cap.rpm_timeout_us, memory_order_relaxed);
    esp_timer_start_once(s_cap.timeout_timer, t);

    s_cap.initialised = true;
    ESP_LOGI(TAG, "init ok: gpio=%d pole=%u mavg=%u timeout_us=%" PRIu32,
             s_cap.input_gpio,
             (unsigned)atomic_load(&s_cap.pole_count),
             (unsigned)atomic_load(&s_cap.moving_avg_count),
             atomic_load(&s_cap.rpm_timeout_us));
    return ESP_OK;
}
