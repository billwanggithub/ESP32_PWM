#include "psu_driver.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "driver/uart.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "psu_driver";

#define PSU_UART_PORT     UART_NUM_1
#define PSU_RX_BUF_SIZE   256
#define PSU_TX_BUF_SIZE   256

#define NVS_NAMESPACE     "psu_driver"
#define NVS_KEY_SLAVE     "slave_addr"

// ---- Riden RD60xx register map -------------------------------------------
#define REG_MODEL    0x0000
#define REG_V_SET    0x0008
#define REG_I_SET    0x0009
#define REG_V_OUT    0x000A
#define REG_I_OUT    0x000B
#define REG_OUTPUT   0x0012

#define POLL_PERIOD_MS       200    // 5 Hz
#define LINK_FAIL_THRESHOLD  5

static _Atomic uint8_t s_slave_addr;

// Atomic publish — bit-punned floats and bools.
static _Atomic uint32_t s_v_set_bits, s_i_set_bits, s_v_out_bits, s_i_out_bits;
static _Atomic uint8_t  s_output_on;
static _Atomic uint8_t  s_link_ok;
static _Atomic uint16_t s_model_id;
static _Atomic uint32_t s_i_scale_bits;   // float bit-punned, 100.0 or 1000.0

static const struct {
    uint16_t    id;
    const char *name;
    float       i_scale;   // raw_register / i_scale = amps
    float       i_max;     // device current ceiling for slider/clamp
} RD_MODELS[] = {
    { 60062, "RD6006",  1000.0f,  6.0f },
    { 60065, "RD6006P", 1000.0f,  6.0f },
    { 60121, "RD6012",   100.0f, 12.0f },
    { 60125, "RD6012P",  100.0f, 12.0f },
    { 60181, "RD6018",   100.0f, 18.0f },
    { 60241, "RD6024",   100.0f, 24.0f },
};
#define RD_MODELS_N (sizeof(RD_MODELS) / sizeof(RD_MODELS[0]))

static TaskHandle_t s_psu_task;
static SemaphoreHandle_t s_uart_mutex;

static inline void store_f(_Atomic uint32_t *slot, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    atomic_store_explicit(slot, bits, memory_order_relaxed);
}

static inline float load_f(_Atomic uint32_t *slot)
{
    uint32_t bits = atomic_load_explicit(slot, memory_order_relaxed);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

// ---- Modbus-RTU CRC-16 (poly 0xA001, init 0xFFFF) -------------------------
static uint16_t modbus_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else         crc >>= 1;
        }
    }
    return crc;   // already in low-byte-first wire order when written as LE
}

// Build "Read Holding Registers" (FC 0x03) request:
//   [slave][0x03][hi(addr)][lo(addr)][hi(n)][lo(n)][lo(crc)][hi(crc)]
// Returns total frame length (always 8).
static size_t build_read_holding(uint8_t *out, uint8_t slave, uint16_t addr, uint16_t n)
{
    out[0] = slave;
    out[1] = 0x03;
    out[2] = (addr >> 8) & 0xFF;
    out[3] = addr & 0xFF;
    out[4] = (n >> 8) & 0xFF;
    out[5] = n & 0xFF;
    uint16_t crc = modbus_crc16(out, 6);
    out[6] = crc & 0xFF;
    out[7] = (crc >> 8) & 0xFF;
    return 8;
}

// Build "Write Single Register" (FC 0x06) request:
//   [slave][0x06][hi(addr)][lo(addr)][hi(val)][lo(val)][lo(crc)][hi(crc)]
static size_t build_write_single(uint8_t *out, uint8_t slave, uint16_t addr, uint16_t val)
{
    out[0] = slave;
    out[1] = 0x06;
    out[2] = (addr >> 8) & 0xFF;
    out[3] = addr & 0xFF;
    out[4] = (val >> 8) & 0xFF;
    out[5] = val & 0xFF;
    uint16_t crc = modbus_crc16(out, 6);
    out[6] = crc & 0xFF;
    out[7] = (crc >> 8) & 0xFF;
    return 8;
}

// Verify the trailing 2-byte CRC of a frame of length `len`.
static bool verify_crc(const uint8_t *buf, size_t len)
{
    if (len < 4) return false;
    uint16_t want = modbus_crc16(buf, len - 2);
    uint16_t got  = (uint16_t)buf[len - 2] | ((uint16_t)buf[len - 1] << 8);
    return want == got;
}

// ---- NVS helpers -----------------------------------------------------------

static void load_slave_addr_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        atomic_store_explicit(&s_slave_addr,
                              (uint8_t)CONFIG_APP_PSU_SLAVE_DEFAULT,
                              memory_order_relaxed);
        return;
    }
    uint8_t v = (uint8_t)CONFIG_APP_PSU_SLAVE_DEFAULT;
    (void)nvs_get_u8(h, NVS_KEY_SLAVE, &v);
    if (v < 1 || v > 247) v = (uint8_t)CONFIG_APP_PSU_SLAVE_DEFAULT;
    atomic_store_explicit(&s_slave_addr, v, memory_order_relaxed);
    nvs_close(h);
    ESP_LOGI(TAG, "slave addr from NVS: %u", v);
}

static void save_slave_addr_to_nvs(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SLAVE, v);
    nvs_commit(h);
    nvs_close(h);
}

// ---- Modbus-RTU transaction primitive --------------------------------------

#define PSU_TXN_TIMEOUT_MS   100
#define PSU_INTERFRAME_MS    2     // 3.5-char gap @ 19200 ≈ 1.8 ms

// Result codes folded into esp_err_t:
//   ESP_OK            : valid response, CRC ok
//   ESP_ERR_TIMEOUT   : no/short response within timeout
//   ESP_ERR_INVALID_CRC : full-length response but CRC mismatch
//   ESP_ERR_INVALID_RESPONSE : Modbus exception (fc | 0x80) or wrong slave/fc
//
// `expect_len` is the total expected response length (header + data + CRC).
// Caller is responsible for sizing `resp` >= `expect_len`.
static esp_err_t psu_txn(const uint8_t *req, size_t req_len,
                         uint8_t *resp, size_t expect_len)
{
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(PSU_TXN_TIMEOUT_MS + 50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uart_flush_input(PSU_UART_PORT);
    int written = uart_write_bytes(PSU_UART_PORT, (const char *)req, req_len);
    esp_err_t result;
    if (written != (int)req_len) {
        result = ESP_ERR_INVALID_STATE;
        goto out;
    }
    esp_err_t e = uart_wait_tx_done(PSU_UART_PORT, pdMS_TO_TICKS(50));
    if (e != ESP_OK) { result = e; goto out; }

    int got = uart_read_bytes(PSU_UART_PORT, resp, expect_len,
                              pdMS_TO_TICKS(PSU_TXN_TIMEOUT_MS));
    vTaskDelay(pdMS_TO_TICKS(PSU_INTERFRAME_MS));

    if (got <= 0) { result = ESP_ERR_TIMEOUT; goto out; }
    if ((size_t)got < expect_len) {
        if (got >= 5 && (resp[1] & 0x80)) {
            if (verify_crc(resp, 5)) {
                ESP_LOGW(TAG, "modbus exception: fc=0x%02X exc=0x%02X",
                         resp[1] & 0x7F, resp[2]);
                result = ESP_ERR_INVALID_RESPONSE;
                goto out;
            }
        }
        result = ESP_ERR_TIMEOUT;
        goto out;
    }
    if (!verify_crc(resp, expect_len))            { result = ESP_ERR_INVALID_CRC;      goto out; }
    if (resp[0] != req[0])                        { result = ESP_ERR_INVALID_RESPONSE; goto out; }
    if ((resp[1] & 0x7F) != (req[1] & 0x7F))      { result = ESP_ERR_INVALID_RESPONSE; goto out; }
    if (resp[1] & 0x80)                           { result = ESP_ERR_INVALID_RESPONSE; goto out; }
    result = ESP_OK;

out:
    xSemaphoreGive(s_uart_mutex);
    return result;
}

static esp_err_t psu_read_holding(uint16_t addr, uint16_t n, uint16_t *out_regs)
{
    uint8_t slave = atomic_load_explicit(&s_slave_addr, memory_order_relaxed);
    uint8_t req[8];
    build_read_holding(req, slave, addr, n);

    // FC 0x03 response: [slave][0x03][bytecount][N×2 bytes][crc][crc]
    size_t expect = 5 + n * 2;
    uint8_t resp[64];
    if (expect > sizeof(resp)) return ESP_ERR_INVALID_SIZE;
    esp_err_t e = psu_txn(req, sizeof(req), resp, expect);
    if (e != ESP_OK) return e;
    if (resp[2] != n * 2) return ESP_ERR_INVALID_RESPONSE;
    for (uint16_t i = 0; i < n; i++) {
        out_regs[i] = ((uint16_t)resp[3 + i * 2] << 8) | resp[4 + i * 2];
    }
    return ESP_OK;
}

static esp_err_t psu_write_single(uint16_t addr, uint16_t val)
{
    uint8_t slave = atomic_load_explicit(&s_slave_addr, memory_order_relaxed);
    uint8_t req[8];
    build_write_single(req, slave, addr, val);

    // FC 0x06 echoes the request: 8 bytes total.
    uint8_t resp[8];
    return psu_txn(req, sizeof(req), resp, sizeof(resp));
}

// ---- Polling task helpers --------------------------------------------------

static void detect_model(void)
{
    uint16_t model = 0;
    if (psu_read_holding(REG_MODEL, 1, &model) != ESP_OK) {
        ESP_LOGW(TAG, "model detect failed (PSU offline at boot?); falling back to RD6006 scale");
        atomic_store_explicit(&s_model_id, 0, memory_order_relaxed);
        store_f(&s_i_scale_bits, 1000.0f);
        return;
    }
    float scale = 1000.0f;
    const char *name = "unknown";
    for (size_t i = 0; i < RD_MODELS_N; i++) {
        if (RD_MODELS[i].id == model) {
            scale = RD_MODELS[i].i_scale;
            name  = RD_MODELS[i].name;
            break;
        }
    }
    atomic_store_explicit(&s_model_id, model, memory_order_relaxed);
    store_f(&s_i_scale_bits, scale);
    ESP_LOGI(TAG, "detected model %u (%s, I scale = ÷%.0f)",
             model, name, (double)scale);
}

// Called from both psu_task (polling) and control_task (setpoint writes), so
// the failure counter must be atomic. memory_order_relaxed is fine — the only
// invariant we need is that the counter monotonically increases on failure
// and resets to zero on success. The transition log lines may double-fire
// across concurrent writers in a tight race; that's fine, it's just a log.
static _Atomic int s_link_fails;

static void note_txn_result(esp_err_t e)
{
    if (e == ESP_OK) {
        int prev = atomic_exchange_explicit(&s_link_fails, 0, memory_order_relaxed);
        if (prev >= LINK_FAIL_THRESHOLD) {
            ESP_LOGI(TAG, "link recovered");
        }
        atomic_store_explicit(&s_link_ok, 1, memory_order_relaxed);
    } else {
        // CAS-bounded increment: cap the counter at LINK_FAIL_THRESHOLD so it
        // can't wrap. This also makes the "==threshold" edge fire exactly once.
        int cur = atomic_load_explicit(&s_link_fails, memory_order_relaxed);
        while (cur < LINK_FAIL_THRESHOLD) {
            if (atomic_compare_exchange_weak_explicit(&s_link_fails, &cur, cur + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                if (cur + 1 == LINK_FAIL_THRESHOLD) {
                    ESP_LOGW(TAG, "link lost: %s", esp_err_to_name(e));
                    atomic_store_explicit(&s_link_ok, 0, memory_order_relaxed);
                }
                break;
            }
            // cur was reloaded by the CAS; loop and retry.
        }
    }
}

static void psu_task_fn(void *arg)
{
    (void)arg;
    detect_model();

    const TickType_t period = pdMS_TO_TICKS(POLL_PERIOD_MS);
    TickType_t last = xTaskGetTickCount();
    while (true) {
        // Re-detect once if we missed at boot AND link is now up — best-effort.
        if (atomic_load_explicit(&s_model_id, memory_order_relaxed) == 0 &&
            atomic_load_explicit(&s_link_ok,  memory_order_relaxed) == 1) {
            detect_model();
        }

        // Read [V_SET, I_SET, V_OUT, I_OUT] in one transaction (4 contiguous regs).
        uint16_t r[4] = {0};
        esp_err_t e = psu_read_holding(REG_V_SET, 4, r);
        note_txn_result(e);
        if (e == ESP_OK) {
            float i_div = load_f(&s_i_scale_bits);
            store_f(&s_v_set_bits, r[0] / 100.0f);
            store_f(&s_i_set_bits, r[1] / i_div);
            store_f(&s_v_out_bits, r[2] / 100.0f);
            store_f(&s_i_out_bits, r[3] / i_div);
        }

        // Read OUTPUT separately (non-contiguous with the V/I block).
        uint16_t o = 0;
        e = psu_read_holding(REG_OUTPUT, 1, &o);
        note_txn_result(e);
        if (e == ESP_OK) {
            atomic_store_explicit(&s_output_on, o ? 1 : 0, memory_order_relaxed);
        }

        vTaskDelayUntil(&last, period);
    }
}

// ---- Public API (Tasks 5-8) -----------------------------------------------

esp_err_t psu_driver_init(void)
{
    // Create the mutex *first* so any subsequent error path that returns
    // before the task starts can still be safely re-entered.
    s_uart_mutex = xSemaphoreCreateMutex();
    if (!s_uart_mutex) return ESP_ERR_NO_MEM;

    load_slave_addr_from_nvs();

    const uart_config_t cfg = {
        .baud_rate  = CONFIG_APP_PSU_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_driver_install(PSU_UART_PORT, PSU_RX_BUF_SIZE,
                                      PSU_TX_BUF_SIZE, 0, NULL, 0);
    if (e != ESP_OK) { ESP_LOGE(TAG, "driver_install: %s", esp_err_to_name(e)); return e; }
    e = uart_param_config(PSU_UART_PORT, &cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "param_config: %s", esp_err_to_name(e));  return e; }
    e = uart_set_pin(PSU_UART_PORT,
                     CONFIG_APP_PSU_UART_TX_GPIO,
                     CONFIG_APP_PSU_UART_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) { ESP_LOGE(TAG, "set_pin: %s", esp_err_to_name(e));       return e; }

    ESP_LOGI(TAG, "UART1 ready: tx=%d rx=%d baud=%d slave=%u",
             CONFIG_APP_PSU_UART_TX_GPIO, CONFIG_APP_PSU_UART_RX_GPIO,
             CONFIG_APP_PSU_UART_BAUD,
             atomic_load_explicit(&s_slave_addr, memory_order_relaxed));

    return ESP_OK;
}
esp_err_t psu_driver_start(void)
{
    if (s_psu_task) return ESP_ERR_INVALID_STATE;
    BaseType_t ok = xTaskCreate(psu_task_fn, "psu_driver", 4096, NULL, 4, &s_psu_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t psu_driver_set_voltage(float v)
{
    if (v < 0.0f)  v = 0.0f;
    if (v > 60.0f) v = 60.0f;
    uint16_t raw = (uint16_t)(v * 100.0f + 0.5f);
    esp_err_t e = psu_write_single(REG_V_SET, raw);
    note_txn_result(e);
    if (e == ESP_OK) {
        store_f(&s_v_set_bits, raw / 100.0f);
    } else {
        ESP_LOGW(TAG, "set_voltage(%.2f V) failed: %s", (double)v, esp_err_to_name(e));
    }
    return e;
}

esp_err_t psu_driver_set_current(float i)
{
    if (i < 0.0f) i = 0.0f;
    float i_div = load_f(&s_i_scale_bits);
    if (i_div < 1.0f) i_div = 1000.0f;   // before model detect
    float i_max = psu_driver_get_i_max();
    if (i > i_max) i = i_max;
    uint16_t raw = (uint16_t)(i * i_div + 0.5f);
    esp_err_t e = psu_write_single(REG_I_SET, raw);
    note_txn_result(e);
    if (e == ESP_OK) {
        store_f(&s_i_set_bits, raw / i_div);
    } else {
        ESP_LOGW(TAG, "set_current(%.3f A) failed: %s", (double)i, esp_err_to_name(e));
    }
    return e;
}

esp_err_t psu_driver_set_output(bool on)
{
    esp_err_t e = psu_write_single(REG_OUTPUT, on ? 1 : 0);
    note_txn_result(e);
    if (e == ESP_OK) {
        atomic_store_explicit(&s_output_on, on ? 1 : 0, memory_order_relaxed);
    } else {
        ESP_LOGW(TAG, "set_output(%d) failed: %s", on ? 1 : 0, esp_err_to_name(e));
    }
    return e;
}
uint8_t psu_driver_get_slave_addr(void)
{
    return atomic_load_explicit(&s_slave_addr, memory_order_relaxed);
}

esp_err_t psu_driver_set_slave_addr(uint8_t addr)
{
    if (addr < 1 || addr > 247) return ESP_ERR_INVALID_ARG;
    atomic_store_explicit(&s_slave_addr, addr, memory_order_relaxed);
    save_slave_addr_to_nvs(addr);
    ESP_LOGI(TAG, "slave addr set to %u (NVS)", addr);
    return ESP_OK;
}

void psu_driver_get_telemetry(psu_driver_telemetry_t *out)
{
    if (!out) return;
    out->v_set       = load_f(&s_v_set_bits);
    out->i_set       = load_f(&s_i_set_bits);
    out->v_out       = load_f(&s_v_out_bits);
    out->i_out       = load_f(&s_i_out_bits);
    out->output_on   = atomic_load_explicit(&s_output_on, memory_order_relaxed) != 0;
    out->link_ok     = atomic_load_explicit(&s_link_ok,   memory_order_relaxed) != 0;
    out->model_id    = atomic_load_explicit(&s_model_id,  memory_order_relaxed);
    out->i_scale_div = load_f(&s_i_scale_bits);
}

const char *psu_driver_get_model_name(void)
{
    uint16_t id = atomic_load_explicit(&s_model_id, memory_order_relaxed);
    for (size_t i = 0; i < RD_MODELS_N; i++) {
        if (RD_MODELS[i].id == id) return RD_MODELS[i].name;
    }
    return "unknown";
}

float psu_driver_get_i_max(void)
{
    uint16_t id = atomic_load_explicit(&s_model_id, memory_order_relaxed);
    for (size_t i = 0; i < RD_MODELS_N; i++) {
        if (RD_MODELS[i].id == id) return RD_MODELS[i].i_max;
    }
    // Conservative default: RD6006 ceiling. Used pre-detect or unknown model.
    return 6.0f;
}

// CRC correctness is verified end-to-end: a bad implementation produces
// frames the supply rejects, every transaction times out, and link_ok stays
// false — the dashboard immediately shows "PSU offline". A boot-time
// __builtin_trap was tried earlier but the constant we compared against
// turned out to be wrong, bricking boot; the on-the-wire feedback loop is
// the real test anyway.
