#include "psu_modbus.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "driver/uart.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "psu_modbus";

#define PSU_UART_PORT     UART_NUM_1
#define PSU_RX_BUF_SIZE   256
#define PSU_TX_BUF_SIZE   256

#define NVS_NAMESPACE     "psu_modbus"
#define NVS_KEY_SLAVE     "slave_addr"

static _Atomic uint8_t s_slave_addr;

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
    uart_flush_input(PSU_UART_PORT);

    int written = uart_write_bytes(PSU_UART_PORT, (const char *)req, req_len);
    if (written != (int)req_len) return ESP_ERR_INVALID_STATE;
    esp_err_t e = uart_wait_tx_done(PSU_UART_PORT, pdMS_TO_TICKS(50));
    if (e != ESP_OK) return e;

    int got = uart_read_bytes(PSU_UART_PORT, resp, expect_len,
                              pdMS_TO_TICKS(PSU_TXN_TIMEOUT_MS));
    // Inter-frame gap before the next transaction.
    vTaskDelay(pdMS_TO_TICKS(PSU_INTERFRAME_MS));

    if (got <= 0) return ESP_ERR_TIMEOUT;
    if ((size_t)got < expect_len) {
        // Could be exception response: 5 bytes [slave][fc|0x80][exc][crc][crc]
        if (got >= 5 && (resp[1] & 0x80)) {
            if (verify_crc(resp, 5)) {
                ESP_LOGW(TAG, "modbus exception: fc=0x%02X exc=0x%02X",
                         resp[1] & 0x7F, resp[2]);
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        return ESP_ERR_TIMEOUT;
    }
    if (!verify_crc(resp, expect_len)) return ESP_ERR_INVALID_CRC;
    if (resp[0] != req[0])             return ESP_ERR_INVALID_RESPONSE;   // wrong slave echo
    if ((resp[1] & 0x7F) != (req[1] & 0x7F)) return ESP_ERR_INVALID_RESPONSE;
    if (resp[1] & 0x80)                return ESP_ERR_INVALID_RESPONSE;   // exception
    return ESP_OK;
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

// ---- Public API (Tasks 5-8) -----------------------------------------------

esp_err_t psu_modbus_init(void)
{
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
esp_err_t psu_modbus_start(void)           { ESP_LOGI(TAG, "start (stub)"); return ESP_OK; }
esp_err_t psu_modbus_set_voltage(float v)  { (void)v; return ESP_OK; }
esp_err_t psu_modbus_set_current(float i)  { (void)i; return ESP_OK; }
esp_err_t psu_modbus_set_output(bool on)   { (void)on; return ESP_OK; }
uint8_t psu_modbus_get_slave_addr(void)
{
    return atomic_load_explicit(&s_slave_addr, memory_order_relaxed);
}

esp_err_t psu_modbus_set_slave_addr(uint8_t addr)
{
    if (addr < 1 || addr > 247) return ESP_ERR_INVALID_ARG;
    atomic_store_explicit(&s_slave_addr, addr, memory_order_relaxed);
    save_slave_addr_to_nvs(addr);
    ESP_LOGI(TAG, "slave addr set to %u (NVS)", addr);
    return ESP_OK;
}

void psu_modbus_get_telemetry(psu_modbus_telemetry_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

const char *psu_modbus_get_model_name(void) { return "unknown"; }

// ---- Compile-time CRC sanity ----------------------------------------------
// Modbus FAQ canonical request {01 03 00 08 00 05} → CRC 0x0944. modbus_crc16
// isn't constexpr-friendly in C99, so we run the check at startup via a
// constructor. Trap (intentional crash) on mismatch — this is a wire-protocol
// invariant; if it ever fails the firmware should not run.
__attribute__((constructor))
static void modbus_crc16_self_check(void)
{
    static const uint8_t v[6] = {0x01, 0x03, 0x00, 0x08, 0x00, 0x05};
    uint16_t got = modbus_crc16(v, 6);
    if (got != 0x0944) {
        // ESP_LOG isn't ready yet at constructor time. __builtin_trap halts
        // and leaves a clean PC for backtrace.
        __builtin_trap();
    }
}
