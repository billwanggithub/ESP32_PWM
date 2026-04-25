#include "psu_modbus.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "psu_modbus";

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

// ---- Public API stubs (filled in Tasks 5-8) -------------------------------

esp_err_t psu_modbus_init(void)            { ESP_LOGI(TAG, "init (stub)");  return ESP_OK; }
esp_err_t psu_modbus_start(void)           { ESP_LOGI(TAG, "start (stub)"); return ESP_OK; }
esp_err_t psu_modbus_set_voltage(float v)  { (void)v; return ESP_OK; }
esp_err_t psu_modbus_set_current(float i)  { (void)i; return ESP_OK; }
esp_err_t psu_modbus_set_output(bool on)   { (void)on; return ESP_OK; }
uint8_t   psu_modbus_get_slave_addr(void)  { return 1; }
esp_err_t psu_modbus_set_slave_addr(uint8_t a) { (void)a; return ESP_OK; }

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
