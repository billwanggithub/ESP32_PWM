# PSU Multi-Driver Implementation Plan (WZ5005 + XY-SK120 + Riden rename)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add WZ5005 (custom 20-byte sum-checksum protocol) and XY-SK120 (Modbus-RTU, different register map) support alongside the shipped Riden RD60xx integration. Rename `components/psu_modbus/` → `components/psu_driver/`, extract a shared Modbus-RTU helper, and introduce an NVS-persisted runtime `psu_family` selector with dashboard + CLI surfaces.

**Architecture:** A single internal vtable (`psu_backend_t`) is plugged in once at boot from the NVS `psu_family` key. `psu_driver.c` owns the public API, atomic-published telemetry state, the polling task, and the shared UART mutex. `psu_modbus_rtu.c` exposes CRC-16 + FC 0x03/0x06 helpers reused by `psu_riden.c` and `psu_xy_sk120.c`. `psu_wz5005.c` is standalone (custom frames + sum checksum). The wire-protocol surface stays unchanged except one new `family` field on the existing WS `psu` JSON block plus one new `set_psu_family` WS op.

**Tech Stack:** ESP-IDF v6.0, FreeRTOS, NVS (`psu_modbus` → `psu_driver` namespace), UART1, cJSON.

**Spec:** `docs/superpowers/specs/2026-04-26-psu-multi-driver-design.md`

---

## File-level summary

| Path | Action | Purpose |
|------|--------|---------|
| `components/psu_modbus/` | DELETE (whole dir) | Replaced by `psu_driver/` |
| `components/psu_driver/CMakeLists.txt` | CREATE | New component manifest, lists all backend SRCS |
| `components/psu_driver/Kconfig.projbuild` | CREATE | Kconfig moved out of `main/Kconfig.projbuild` for clean ownership |
| `components/psu_driver/include/psu_driver.h` | CREATE | Public API (renamed `psu_modbus_*` → `psu_driver_*`) + new family API |
| `components/psu_driver/include/psu_modbus_rtu.h` | CREATE | Shared Modbus-RTU helpers (private to PSU family) |
| `components/psu_driver/include/psu_backend.h` | CREATE | Internal vtable + publish helpers (private to component) |
| `components/psu_driver/psu_driver.c` | CREATE | Public API impl, family dispatch, atomic state, polling task, NVS, UART mutex |
| `components/psu_driver/psu_modbus_rtu.c` | CREATE | CRC-16, FC 0x03/0x06 builders, `psu_modbus_rtu_txn` |
| `components/psu_driver/psu_riden.c` | CREATE | Riden register map + RD_MODELS table + backend vtable |
| `components/psu_driver/psu_xy_sk120.c` | CREATE | XY-SK120 register map + backend vtable |
| `components/psu_driver/psu_wz5005.c` | CREATE | 20-byte custom-frame + sum checksum + backend vtable |
| `main/Kconfig.projbuild` | MODIFY | Remove `APP_PSU_*` (moved to component); ESP-IDF auto-includes both |
| `main/CMakeLists.txt` | MODIFY | `psu_modbus` → `psu_driver` in REQUIRES |
| `main/app_main.c` | MODIFY | `#include "psu_modbus.h"` → `psu_driver.h`; symbol rename; new `psu_family` CLI |
| `main/control_task.c` | MODIFY | `#include "psu_modbus.h"` → `psu_driver.h`; symbol rename |
| `components/usb_composite/CMakeLists.txt` | MODIFY | `psu_modbus` → `psu_driver` |
| `components/usb_composite/usb_cdc_task.c` | MODIFY | include + symbol rename |
| `components/net_dashboard/CMakeLists.txt` | MODIFY | `psu_modbus` → `psu_driver` |
| `components/net_dashboard/net_dashboard.c` | MODIFY | include + telemetry-call rename |
| `components/net_dashboard/ws_handler.c` | MODIFY | include + rename + new `set_psu_family` op + `family` field on `psu` JSON |
| `components/net_dashboard/web/index.html` | MODIFY | Family `<select>` + Reboot button in PSU panel |
| `components/net_dashboard/web/app.js` | MODIFY | Family change → `set_psu_family` WS send; reboot button binding; render `family` from telemetry |
| `components/net_dashboard/web/app.css` | MODIFY | Style for family dropdown row (matches existing `.psu-settings` row style) |
| `CLAUDE.md` | MODIFY | "PSU Modbus-RTU master" section → "PSU driver (multi-family)"; namespace + diagram updates |

There is **no test framework** in this firmware project — verification is done by build + on-hardware behaviour confirmation. Each task ends with `idf.py build` (must succeed) and, where stated, an explicit on-hardware sanity step. The first hardware-only step is the very last task; everything before it should compile and link cleanly even with no PSU attached.

---

## Phase A — Component rename (no behaviour change)

Goal: rename `psu_modbus` → `psu_driver` across the whole repo, preserving every behaviour. After Phase A the firmware behaves *identically* to today; only names changed.

### Task A1: Create new `psu_driver` component skeleton with CMakeLists + empty header

**Files:**
- Create: `components/psu_driver/CMakeLists.txt`
- Create: `components/psu_driver/include/psu_driver.h`

- [ ] **Step 1: Create `components/psu_driver/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS        "psu_driver.c"
                "psu_modbus_rtu.c"
                "psu_riden.c"
                "psu_xy_sk120.c"
                "psu_wz5005.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES    app_api
                esp_driver_uart
                nvs_flash
                log
)
```

- [ ] **Step 2: Create `components/psu_driver/include/psu_driver.h` as a 1:1 rename of `psu_modbus.h` (no API changes yet — additions land in Task B7)**

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    v_set;
    float    i_set;
    float    v_out;
    float    i_out;
    bool     output_on;
    bool     link_ok;
    uint16_t model_id;       // raw model register; 0 if not yet detected
    float    i_scale_div;    // 100.0 or 1000.0 by family/model
} psu_driver_telemetry_t;

esp_err_t psu_driver_init(void);
esp_err_t psu_driver_start(void);

esp_err_t psu_driver_set_voltage(float v);
esp_err_t psu_driver_set_current(float i);
esp_err_t psu_driver_set_output(bool on);

uint8_t   psu_driver_get_slave_addr(void);
esp_err_t psu_driver_set_slave_addr(uint8_t addr);

void psu_driver_get_telemetry(psu_driver_telemetry_t *out);

const char *psu_driver_get_model_name(void);
float       psu_driver_get_i_max(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Create empty stubs for the other backend `.c` files so the SRCS list links**

`components/psu_driver/psu_modbus_rtu.c`:
```c
// Filled in Phase B.
```

`components/psu_driver/psu_riden.c`:
```c
// Filled in Phase B.
```

`components/psu_driver/psu_xy_sk120.c`:
```c
// Filled in Phase B.
```

`components/psu_driver/psu_wz5005.c`:
```c
// Filled in Phase B.
```

- [ ] **Step 4: Build to confirm component is detected (will still fail to link until Task A2 lands)**

Run: `idf.py build`
Expected: build fails at link step with multiple-definition / unresolved-reference errors involving both `psu_modbus_*` and `psu_driver_*` (because A2 hasn't moved the implementation yet). This is OK and confirms the new component is picked up.

- [ ] **Step 5: Commit**

```bash
git add components/psu_driver/
git commit -m "feat(psu_driver): scaffold new component (skeleton, builds incomplete)"
```

---

### Task A2: Move `psu_modbus.c` body into `psu_driver.c` verbatim with symbol rename

**Files:**
- Create: `components/psu_driver/psu_driver.c`
- Delete: `components/psu_modbus/psu_modbus.c` (in Task A4)

- [ ] **Step 1: Copy current `components/psu_modbus/psu_modbus.c` content into `components/psu_driver/psu_driver.c`**

Then perform exactly these textual replacements (paste-and-edit; nothing else changes):

| Find | Replace |
|------|---------|
| `#include "psu_modbus.h"` | `#include "psu_driver.h"` |
| `psu_modbus_init` | `psu_driver_init` |
| `psu_modbus_start` | `psu_driver_start` |
| `psu_modbus_set_voltage` | `psu_driver_set_voltage` |
| `psu_modbus_set_current` | `psu_driver_set_current` |
| `psu_modbus_set_output` | `psu_driver_set_output` |
| `psu_modbus_set_slave_addr` | `psu_driver_set_slave_addr` |
| `psu_modbus_get_slave_addr` | `psu_driver_get_slave_addr` |
| `psu_modbus_get_telemetry` | `psu_driver_get_telemetry` |
| `psu_modbus_get_model_name` | `psu_driver_get_model_name` |
| `psu_modbus_get_i_max` | `psu_driver_get_i_max` |
| `psu_modbus_telemetry_t` | `psu_driver_telemetry_t` |
| `static const char *TAG = "psu_modbus";` | `static const char *TAG = "psu_driver";` |
| `#define NVS_NAMESPACE     "psu_modbus"` | `#define NVS_NAMESPACE     "psu_driver"` |

The NVS namespace change is intentional — fresh `psu_driver` namespace. Existing devices' slave_addr in the old `psu_modbus` namespace will be ignored on first boot of the new firmware; the device falls back to `CONFIG_APP_PSU_SLAVE_DEFAULT` (1). On-bench operators must `psu_slave 5` once after upgrade if they had a non-default slave. Document this in the commit message.

- [ ] **Step 2: Build to confirm `psu_driver.c` is now the canonical impl**

Run: `idf.py build`
Expected: build still fails — every caller still includes `psu_modbus.h` and references `psu_modbus_*`. Multiple-definition errors are gone (only one impl now); unresolved-reference errors remain in callers.

- [ ] **Step 3: Commit**

```bash
git add components/psu_driver/psu_driver.c
git commit -m "feat(psu_driver): copy psu_modbus.c body verbatim with symbol rename"
```

---

### Task A3: Update every caller's include + symbol references

**Files:**
- Modify: `main/CMakeLists.txt`
- Modify: `main/app_main.c`
- Modify: `main/control_task.c`
- Modify: `components/usb_composite/CMakeLists.txt`
- Modify: `components/usb_composite/usb_cdc_task.c`
- Modify: `components/net_dashboard/CMakeLists.txt`
- Modify: `components/net_dashboard/net_dashboard.c`
- Modify: `components/net_dashboard/ws_handler.c`

- [ ] **Step 1: Update `main/CMakeLists.txt`**

Find:
```cmake
                psu_modbus
```
Replace with:
```cmake
                psu_driver
```

- [ ] **Step 2: Update `components/usb_composite/CMakeLists.txt`**

Same replacement as Step 1 (`psu_modbus` → `psu_driver`).

- [ ] **Step 3: Update `components/net_dashboard/CMakeLists.txt`**

Same replacement.

- [ ] **Step 4: Update `main/app_main.c`**

Replace every occurrence:
- `#include "psu_modbus.h"` → `#include "psu_driver.h"`
- `psu_modbus_init` → `psu_driver_init`
- `psu_modbus_start` → `psu_driver_start`
- `psu_modbus_telemetry_t` → `psu_driver_telemetry_t`
- `psu_modbus_get_telemetry` → `psu_driver_get_telemetry`
- `psu_modbus_get_model_name` → `psu_driver_get_model_name`
- `psu_modbus_get_slave_addr` → `psu_driver_get_slave_addr`
- `psu_modbus_get_i_max` → `psu_driver_get_i_max`

(The `cmd_psu_*` CLI handler internals already use the `app_api.h` `CTRL_CMD_PSU_*` queue path — those names don't change in this task.)

- [ ] **Step 5: Update `main/control_task.c`**

- `#include "psu_modbus.h"` → `#include "psu_driver.h"`
- `psu_modbus_set_voltage` → `psu_driver_set_voltage`
- `psu_modbus_set_current` → `psu_driver_set_current`
- `psu_modbus_set_output` → `psu_driver_set_output`
- `psu_modbus_set_slave_addr` → `psu_driver_set_slave_addr`

- [ ] **Step 6: Update `components/usb_composite/usb_cdc_task.c`**

- `#include "psu_modbus.h"` → `#include "psu_driver.h"`
- `psu_modbus_telemetry_t` → `psu_driver_telemetry_t`
- `psu_modbus_get_telemetry` → `psu_driver_get_telemetry`

- [ ] **Step 7: Update `components/net_dashboard/net_dashboard.c`**

- `#include "psu_modbus.h"` → `#include "psu_driver.h"`
- `psu_modbus_get_model_name` → `psu_driver_get_model_name`
- `psu_modbus_get_i_max` → `psu_driver_get_i_max`
- (Keep the `cJSON_AddNumberToObject(root, "psu_baud", ...)` line as-is.)

- [ ] **Step 8: Update `components/net_dashboard/ws_handler.c`**

- `#include "psu_modbus.h"` → `#include "psu_driver.h"`
- `psu_modbus_telemetry_t` → `psu_driver_telemetry_t`
- `psu_modbus_get_telemetry` → `psu_driver_get_telemetry`
- `psu_modbus_get_model_name` → `psu_driver_get_model_name`
- `psu_modbus_get_slave_addr` → `psu_driver_get_slave_addr`

- [ ] **Step 9: Build**

Run: `idf.py build`
Expected: build succeeds. There may still be a stale `psu_modbus.c` in `components/psu_modbus/` that's now unreferenced — cleanup is Task A4.

- [ ] **Step 10: Commit**

```bash
git add main/CMakeLists.txt main/app_main.c main/control_task.c \
        components/usb_composite/CMakeLists.txt components/usb_composite/usb_cdc_task.c \
        components/net_dashboard/CMakeLists.txt components/net_dashboard/net_dashboard.c \
        components/net_dashboard/ws_handler.c
git commit -m "refactor(psu): redirect every caller from psu_modbus to psu_driver"
```

---

### Task A4: Delete `components/psu_modbus/` and verify build

**Files:**
- Delete: `components/psu_modbus/CMakeLists.txt`
- Delete: `components/psu_modbus/psu_modbus.c`
- Delete: `components/psu_modbus/include/psu_modbus.h`
- Delete: `components/psu_modbus/` directory

- [ ] **Step 1: Remove the old component directory**

```bash
rm -rf components/psu_modbus
```

- [ ] **Step 2: Build**

Run: `idf.py build`
Expected: build succeeds. No references to `psu_modbus` remain anywhere.

- [ ] **Step 3: Final grep sweep**

Run from repo root: `grep -rn "psu_modbus" --include='*.c' --include='*.h' --include='CMakeLists.txt' --include='*.md' .`
Expected: hits only in `docs/superpowers/specs/`, `docs/superpowers/plans/`, and `CLAUDE.md` (those are documentation that's updated at the end of the work). Zero hits in source.

- [ ] **Step 4: Move Kconfig from `main/Kconfig.projbuild` to `components/psu_driver/Kconfig.projbuild`**

In `main/Kconfig.projbuild`, **delete** these lines (lines 94–109 in current main):
```kconfig
    config APP_PSU_UART_TX_GPIO
        int "PSU UART1 TX GPIO"
        default 38

    config APP_PSU_UART_RX_GPIO
        int "PSU UART1 RX GPIO"
        default 39

    config APP_PSU_UART_BAUD
        int "PSU UART1 baud rate"
        default 19200

    config APP_PSU_SLAVE_DEFAULT
        int "PSU Modbus slave address default (used on first boot before NVS)"
        default 1
        range 1 247
```

Create `components/psu_driver/Kconfig.projbuild`:
```kconfig
menu "PSU driver"

    config APP_PSU_UART_TX_GPIO
        int "PSU UART1 TX GPIO"
        default 38

    config APP_PSU_UART_RX_GPIO
        int "PSU UART1 RX GPIO"
        default 39

    config APP_PSU_UART_BAUD
        int "PSU UART1 baud rate (matches the PSU's panel-set rate)"
        default 19200

    config APP_PSU_SLAVE_DEFAULT
        int "PSU slave address default (used on first boot before NVS)"
        default 1
        range 1 255

endmenu
```

(The Kconfig defaults still match Phase A behaviour — 19200 baud, slave 1, range 1..255 widened from 1..247 to anticipate WZ5005. The Kconfig **family choice** and **per-family baud defaults** land in Task B6.)

- [ ] **Step 5: Build with `del sdkconfig` first to pick up the moved Kconfig**

```bash
del sdkconfig
idf.py build
```

(See CLAUDE.md "sdkconfig trap" — the moved Kconfig keys are still the same names, but the menu hierarchy changed, so a clean re-merge is safer.)
Expected: build succeeds. `idf.py menuconfig` now shows a top-level "PSU driver" menu in addition to "Fan-TestKit App".

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(psu): delete psu_modbus/, move Kconfig to psu_driver/"
```

---

### Task A5: On-hardware Phase A regression — must behave identically

- [ ] **Step 1: Flash to the dev board (with Riden RD60xx wired on UART1)**

Run: `idf.py -p COM24 flash monitor`
Expected: boot logs include `psu_driver: UART1 ready: tx=38 rx=39 baud=19200 slave=1` (TAG renamed from `psu_modbus`). PSU detect succeeds; `link=up` shown in dashboard.

- [ ] **Step 2: Exercise the dashboard PSU panel (Wi-Fi)**

- Set V_SET = 5.00, observe `v_out` in the dashboard tracking.
- Set I_SET = 0.500, toggle output ON/OFF.
- Confirm the slave-addr Save button works (set slave to 5 then back to 1; both persist across reboot).

- [ ] **Step 3: Run `psu_status` on the USB-UART console**

Expected: same output format as before, with the TAG-prefixed `psu_driver` log line (no longer `psu_modbus`).

- [ ] **Step 4: No-commit checkpoint** — Phase A complete with no behaviour change.

---

## Phase B — Refactor into vtable + add WZ5005 + XY-SK120 backends

Goal: split `psu_driver.c` into `psu_driver.c` (dispatch + state) + `psu_riden.c` (the existing logic moves here) + new `psu_modbus_rtu.c` shared helpers + new XY-SK120 + WZ5005 backends. Behaviour for Riden remains identical; new backends are reachable via NVS.

### Task B1: Define internal vtable + publish helpers (`psu_backend.h`)

**Files:**
- Create: `components/psu_driver/include/psu_backend.h`

- [ ] **Step 1: Author the header**

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psu_backend {
    const char *name;                          // "riden" / "xy_sk120" / "wz5005"
    int         default_baud;                  // matches CLAUDE.md / spec; 115200 / 115200 / 19200

    esp_err_t (*detect)(void);                 // sets model_id, i_scale, i_max via publish helpers
    esp_err_t (*poll)(void);                   // 5 Hz tick: read v/i set+out + output state
    esp_err_t (*set_voltage)(float v);
    esp_err_t (*set_current)(float i);
    esp_err_t (*set_output)(bool on);
} psu_backend_t;

extern const psu_backend_t psu_backend_riden;
extern const psu_backend_t psu_backend_xy_sk120;
extern const psu_backend_t psu_backend_wz5005;

// ----- shared state, owned by psu_driver.c, exposed to backends -----

uint8_t           psu_driver_priv_get_slave(void);              // current slave addr
SemaphoreHandle_t psu_driver_priv_get_uart_mutex(void);          // for backend-rolled UART txns

void psu_driver_priv_publish_v_set(float v);
void psu_driver_priv_publish_i_set(float i);
void psu_driver_priv_publish_v_out(float v);
void psu_driver_priv_publish_i_out(float i);
void psu_driver_priv_publish_output(bool on);
void psu_driver_priv_publish_model(uint16_t id, const char *name,
                                   float i_scale_div, float i_max);
void psu_driver_priv_note_txn_result(esp_err_t e);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Build (will succeed — header is unused so far)**

Run: `idf.py build`
Expected: success.

- [ ] **Step 3: Commit**

```bash
git add components/psu_driver/include/psu_backend.h
git commit -m "feat(psu_driver): add internal psu_backend.h vtable + publish helpers"
```

---

### Task B2: Extract Modbus-RTU helpers into `psu_modbus_rtu.c` / `.h`

**Files:**
- Create: `components/psu_driver/include/psu_modbus_rtu.h`
- Modify: `components/psu_driver/psu_modbus_rtu.c` (was empty stub from A1)
- Modify: `components/psu_driver/psu_driver.c` (remove the helpers, call into the new module)

- [ ] **Step 1: Author `psu_modbus_rtu.h`**

```c
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wraps a single Modbus-RTU request/response transaction on UART1.
// Acquires the shared psu_driver UART mutex, flushes RX, writes req,
// reads up to expect_len bytes, verifies CRC.
//
// Returns:
//   ESP_OK                    : valid response, CRC ok
//   ESP_ERR_TIMEOUT           : no/short response
//   ESP_ERR_INVALID_CRC       : full-length response but CRC mismatch
//   ESP_ERR_INVALID_RESPONSE  : Modbus exception (fc | 0x80) or wrong slave/fc
esp_err_t psu_modbus_rtu_txn(const uint8_t *req, size_t req_len,
                             uint8_t *resp, size_t expect_len);

// FC 0x03 (read holding) — issues a transaction and copies n big-endian
// register words into out_regs.
esp_err_t psu_modbus_rtu_read_holding(uint8_t slave, uint16_t addr,
                                      uint16_t n, uint16_t *out_regs);

// FC 0x06 (write single) — issues a transaction and verifies the echo.
esp_err_t psu_modbus_rtu_write_single(uint8_t slave, uint16_t addr,
                                      uint16_t val);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Author `psu_modbus_rtu.c` — lift CRC / builder / txn from `psu_driver.c`**

```c
#include "psu_modbus_rtu.h"
#include "psu_backend.h"

#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "psu_modbus_rtu";

#define PSU_UART_PORT        UART_NUM_1
#define PSU_TXN_TIMEOUT_MS   100
#define PSU_INTERFRAME_MS    2

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
    return crc;
}

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

static bool verify_crc(const uint8_t *buf, size_t len)
{
    if (len < 4) return false;
    uint16_t want = modbus_crc16(buf, len - 2);
    uint16_t got  = (uint16_t)buf[len - 2] | ((uint16_t)buf[len - 1] << 8);
    return want == got;
}

esp_err_t psu_modbus_rtu_txn(const uint8_t *req, size_t req_len,
                             uint8_t *resp, size_t expect_len)
{
    SemaphoreHandle_t mtx = psu_driver_priv_get_uart_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(PSU_TXN_TIMEOUT_MS + 50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result;
    uart_flush_input(PSU_UART_PORT);
    int written = uart_write_bytes(PSU_UART_PORT, (const char *)req, req_len);
    if (written != (int)req_len) { result = ESP_ERR_INVALID_STATE; goto out; }
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
    xSemaphoreGive(mtx);
    return result;
}

esp_err_t psu_modbus_rtu_read_holding(uint8_t slave, uint16_t addr,
                                      uint16_t n, uint16_t *out_regs)
{
    uint8_t req[8];
    build_read_holding(req, slave, addr, n);

    size_t expect = 5 + n * 2;
    uint8_t resp[64];
    if (expect > sizeof(resp)) return ESP_ERR_INVALID_SIZE;
    esp_err_t e = psu_modbus_rtu_txn(req, sizeof(req), resp, expect);
    if (e != ESP_OK) return e;
    if (resp[2] != n * 2) return ESP_ERR_INVALID_RESPONSE;
    for (uint16_t i = 0; i < n; i++) {
        out_regs[i] = ((uint16_t)resp[3 + i * 2] << 8) | resp[4 + i * 2];
    }
    return ESP_OK;
}

esp_err_t psu_modbus_rtu_write_single(uint8_t slave, uint16_t addr,
                                      uint16_t val)
{
    uint8_t req[8];
    build_write_single(req, slave, addr, val);
    uint8_t resp[8];
    return psu_modbus_rtu_txn(req, sizeof(req), resp, sizeof(resp));
}
```

- [ ] **Step 3: Build — will fail because `psu_driver.c` still defines its own helpers**

Run: `idf.py build`
Expected: multiple-definition error on `modbus_crc16` etc.

- [ ] **Step 4: Strip the duplicated helpers + slave-addr access from `psu_driver.c`**

In `psu_driver.c`:
- Delete the `modbus_crc16`, `build_read_holding`, `build_write_single`, `verify_crc`, `psu_txn` functions.
- Delete the `psu_read_holding` / `psu_write_single` static wrappers — replace their callers (in this file's `detect_model`, `psu_task_fn`, `psu_modbus_set_voltage`, `psu_modbus_set_current`, `psu_modbus_set_output`) with `psu_modbus_rtu_read_holding(s_slave_addr, ...)` / `psu_modbus_rtu_write_single(s_slave_addr, ...)`.
- Add `#include "psu_modbus_rtu.h"`.

(All those Riden-specific call sites move to `psu_riden.c` in Task B3 — strictly speaking, leaving the Riden logic in `psu_driver.c` for one more task is fine and the build will pass.)

- [ ] **Step 5: Build**

Run: `idf.py build`
Expected: success.

- [ ] **Step 6: Commit**

```bash
git add components/psu_driver/
git commit -m "refactor(psu_driver): extract psu_modbus_rtu.{c,h} helpers"
```

---

### Task B3: Move Riden logic into `psu_riden.c` behind the vtable; rewire `psu_driver.c` for dispatch

**Files:**
- Modify: `components/psu_driver/psu_riden.c` (was empty stub)
- Modify: `components/psu_driver/psu_driver.c` (becomes pure dispatch + state)

This is the largest task by code volume — it moves the Riden register map, the `RD_MODELS` table, and all four operations (detect, poll, set_voltage, set_current, set_output) from `psu_driver.c` into `psu_riden.c` behind the vtable. After this task, `psu_driver.c` is family-agnostic.

- [ ] **Step 1: Author `psu_riden.c`**

```c
#include "psu_backend.h"
#include "psu_modbus_rtu.h"

#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "psu_riden";

#define REG_MODEL    0x0000
#define REG_V_SET    0x0008
#define REG_I_SET    0x0009
#define REG_V_OUT    0x000A
#define REG_I_OUT    0x000B
#define REG_OUTPUT   0x0012

static const struct {
    uint16_t    id;
    const char *name;
    float       i_scale;   // raw_register / i_scale = amps
    float       i_max;
} RD_MODELS[] = {
    { 60062, "RD6006",  1000.0f,  6.0f },
    { 60065, "RD6006P", 1000.0f,  6.0f },
    { 60121, "RD6012",   100.0f, 12.0f },
    { 60125, "RD6012P",  100.0f, 12.0f },
    { 60181, "RD6018",   100.0f, 18.0f },
    { 60241, "RD6024",   100.0f, 24.0f },
};
#define RD_MODELS_N (sizeof(RD_MODELS) / sizeof(RD_MODELS[0]))

static float s_i_scale_div = 1000.0f;   // local cache; also published

static esp_err_t riden_detect(void)
{
    uint16_t model = 0;
    esp_err_t e = psu_modbus_rtu_read_holding(psu_driver_priv_get_slave(),
                                              REG_MODEL, 1, &model);
    psu_driver_priv_note_txn_result(e);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "model detect failed; falling back to RD6006 scale");
        s_i_scale_div = 1000.0f;
        psu_driver_priv_publish_model(0, "unknown", 1000.0f, 6.0f);
        return e;
    }

    const char *name = "unknown";
    float scale = 1000.0f, i_max = 6.0f;
    for (size_t i = 0; i < RD_MODELS_N; i++) {
        if (RD_MODELS[i].id == model) {
            name  = RD_MODELS[i].name;
            scale = RD_MODELS[i].i_scale;
            i_max = RD_MODELS[i].i_max;
            break;
        }
    }
    s_i_scale_div = scale;
    psu_driver_priv_publish_model(model, name, scale, i_max);
    ESP_LOGI(TAG, "detected model %u (%s, I scale = ÷%.0f)",
             model, name, (double)scale);
    return ESP_OK;
}

static esp_err_t riden_poll(void)
{
    uint8_t slave = psu_driver_priv_get_slave();

    uint16_t r[4] = {0};
    esp_err_t e = psu_modbus_rtu_read_holding(slave, REG_V_SET, 4, r);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) {
        psu_driver_priv_publish_v_set(r[0] / 100.0f);
        psu_driver_priv_publish_i_set(r[1] / s_i_scale_div);
        psu_driver_priv_publish_v_out(r[2] / 100.0f);
        psu_driver_priv_publish_i_out(r[3] / s_i_scale_div);
    }

    uint16_t o = 0;
    e = psu_modbus_rtu_read_holding(slave, REG_OUTPUT, 1, &o);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_output(o != 0);

    return ESP_OK;
}

static esp_err_t riden_set_voltage(float v)
{
    if (v < 0.0f)  v = 0.0f;
    if (v > 60.0f) v = 60.0f;
    uint16_t raw = (uint16_t)(v * 100.0f + 0.5f);
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(), REG_V_SET, raw);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_v_set(raw / 100.0f);
    return e;
}

static esp_err_t riden_set_current(float i)
{
    if (i < 0.0f) i = 0.0f;
    float div = s_i_scale_div;
    if (div < 1.0f) div = 1000.0f;   // pre-detect fallback
    uint16_t raw = (uint16_t)(i * div + 0.5f);
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(), REG_I_SET, raw);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_i_set(raw / div);
    return e;
}

static esp_err_t riden_set_output(bool on)
{
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(), REG_OUTPUT, on ? 1 : 0);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_output(on);
    return e;
}

const psu_backend_t psu_backend_riden = {
    .name         = "riden",
    .default_baud = 115200,   // factory default per spec
    .detect       = riden_detect,
    .poll         = riden_poll,
    .set_voltage  = riden_set_voltage,
    .set_current  = riden_set_current,
    .set_output   = riden_set_output,
};
```

- [ ] **Step 2: Rewrite `psu_driver.c` as dispatch + state only**

Replace the file body. New `psu_driver.c`:

```c
#include "psu_driver.h"
#include "psu_backend.h"

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
#define NVS_KEY_FAMILY    "family"

#define POLL_PERIOD_MS       200
#define LINK_FAIL_THRESHOLD  5

// ---- shared atomic state -------------------------------------------------
static _Atomic uint8_t  s_slave_addr;
static _Atomic uint32_t s_v_set_bits, s_i_set_bits, s_v_out_bits, s_i_out_bits;
static _Atomic uint8_t  s_output_on;
static _Atomic uint8_t  s_link_ok;
static _Atomic uint16_t s_model_id;
static _Atomic uint32_t s_i_scale_bits;
static _Atomic uint32_t s_i_max_bits;
static const char *_Atomic s_model_name = "unknown";
static _Atomic int  s_link_fails;

// ---- backend dispatch ----------------------------------------------------
static const psu_backend_t *s_backend = &psu_backend_riden;

// ---- handles -------------------------------------------------------------
static SemaphoreHandle_t s_uart_mutex;
static TaskHandle_t      s_psu_task;

// ---- bit-pun helpers -----------------------------------------------------
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

// ---- publish helpers (called by backends) --------------------------------
SemaphoreHandle_t psu_driver_priv_get_uart_mutex(void) { return s_uart_mutex; }
uint8_t psu_driver_priv_get_slave(void) {
    return atomic_load_explicit(&s_slave_addr, memory_order_relaxed);
}
void psu_driver_priv_publish_v_set(float v) { store_f(&s_v_set_bits, v); }
void psu_driver_priv_publish_i_set(float i) { store_f(&s_i_set_bits, i); }
void psu_driver_priv_publish_v_out(float v) { store_f(&s_v_out_bits, v); }
void psu_driver_priv_publish_i_out(float i) { store_f(&s_i_out_bits, i); }
void psu_driver_priv_publish_output(bool on) {
    atomic_store_explicit(&s_output_on, on ? 1 : 0, memory_order_relaxed);
}
void psu_driver_priv_publish_model(uint16_t id, const char *name,
                                   float i_scale_div, float i_max)
{
    atomic_store_explicit(&s_model_id, id, memory_order_relaxed);
    atomic_store_explicit((const char *_Atomic *)&s_model_name, name, memory_order_relaxed);
    store_f(&s_i_scale_bits, i_scale_div);
    store_f(&s_i_max_bits,   i_max);
}

void psu_driver_priv_note_txn_result(esp_err_t e)
{
    if (e == ESP_OK) {
        int prev = atomic_exchange_explicit(&s_link_fails, 0, memory_order_relaxed);
        if (prev >= LINK_FAIL_THRESHOLD) ESP_LOGI(TAG, "link recovered");
        atomic_store_explicit(&s_link_ok, 1, memory_order_relaxed);
    } else {
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
        }
    }
}

// ---- NVS -----------------------------------------------------------------
static const psu_backend_t *resolve_backend_by_name(const char *name)
{
    if (!name) return &psu_backend_riden;
    if (strcmp(name, "riden")    == 0) return &psu_backend_riden;
    if (strcmp(name, "xy_sk120") == 0) return &psu_backend_xy_sk120;
    if (strcmp(name, "wz5005")   == 0) return &psu_backend_wz5005;
    return &psu_backend_riden;
}

static void load_nvs_state(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        atomic_store_explicit(&s_slave_addr,
                              (uint8_t)CONFIG_APP_PSU_SLAVE_DEFAULT,
                              memory_order_relaxed);
        s_backend = &psu_backend_riden;
        return;
    }
    uint8_t v = (uint8_t)CONFIG_APP_PSU_SLAVE_DEFAULT;
    (void)nvs_get_u8(h, NVS_KEY_SLAVE, &v);
    if (v < 1 || v > 255) v = (uint8_t)CONFIG_APP_PSU_SLAVE_DEFAULT;
    atomic_store_explicit(&s_slave_addr, v, memory_order_relaxed);

    char name[16] = {0};
    size_t n = sizeof(name);
    if (nvs_get_str(h, NVS_KEY_FAMILY, name, &n) != ESP_OK) {
        s_backend = &psu_backend_riden;
    } else {
        s_backend = resolve_backend_by_name(name);
    }
    nvs_close(h);
}

static esp_err_t save_slave_to_nvs(uint8_t v)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_u8(h, NVS_KEY_SLAVE, v);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t save_family_to_nvs(const char *name)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, NVS_KEY_FAMILY, name);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

// ---- polling task --------------------------------------------------------
static void psu_task_fn(void *arg)
{
    (void)arg;
    s_backend->detect();

    const TickType_t period = pdMS_TO_TICKS(POLL_PERIOD_MS);
    TickType_t last = xTaskGetTickCount();
    while (true) {
        if (atomic_load_explicit(&s_model_id, memory_order_relaxed) == 0 &&
            atomic_load_explicit(&s_link_ok,  memory_order_relaxed) == 1) {
            s_backend->detect();
        }
        s_backend->poll();
        vTaskDelayUntil(&last, period);
    }
}

// ---- public API ----------------------------------------------------------
esp_err_t psu_driver_init(void)
{
    s_uart_mutex = xSemaphoreCreateMutex();
    if (!s_uart_mutex) return ESP_ERR_NO_MEM;

    // pre-detect defaults so get_telemetry() before first detect returns sane values
    store_f(&s_i_scale_bits, 1000.0f);
    store_f(&s_i_max_bits,   6.0f);

    load_nvs_state();

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

    ESP_LOGI(TAG, "UART1 ready: family=%s tx=%d rx=%d baud=%d slave=%u",
             s_backend->name,
             CONFIG_APP_PSU_UART_TX_GPIO, CONFIG_APP_PSU_UART_RX_GPIO,
             CONFIG_APP_PSU_UART_BAUD,
             psu_driver_priv_get_slave());
    if (CONFIG_APP_PSU_UART_BAUD != s_backend->default_baud) {
        ESP_LOGW(TAG, "baud %d differs from family default %d — confirm panel-set rate",
                 CONFIG_APP_PSU_UART_BAUD, s_backend->default_baud);
    }
    return ESP_OK;
}

esp_err_t psu_driver_start(void)
{
    if (s_psu_task) return ESP_ERR_INVALID_STATE;
    BaseType_t ok = xTaskCreate(psu_task_fn, "psu_driver", 4096, NULL, 4, &s_psu_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t psu_driver_set_voltage(float v) { return s_backend->set_voltage(v); }
esp_err_t psu_driver_set_current(float i) { return s_backend->set_current(i); }
esp_err_t psu_driver_set_output(bool on)  { return s_backend->set_output(on); }

uint8_t psu_driver_get_slave_addr(void) { return psu_driver_priv_get_slave(); }

esp_err_t psu_driver_set_slave_addr(uint8_t addr)
{
    if (addr < 1 || addr > 255) return ESP_ERR_INVALID_ARG;
    atomic_store_explicit(&s_slave_addr, addr, memory_order_relaxed);
    save_slave_to_nvs(addr);
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
    return atomic_load_explicit(&s_model_name, memory_order_relaxed);
}

float psu_driver_get_i_max(void) { return load_f(&s_i_max_bits); }

// Family API used by Task B7
const char *psu_driver_get_family(void) { return s_backend->name; }

esp_err_t psu_driver_set_family(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    if (strcmp(name, "riden")    != 0 &&
        strcmp(name, "xy_sk120") != 0 &&
        strcmp(name, "wz5005")   != 0) return ESP_ERR_INVALID_ARG;
    esp_err_t e = save_family_to_nvs(name);
    if (e == ESP_OK) ESP_LOGI(TAG, "family set to %s — reboot to apply", name);
    return e;
}
```

- [ ] **Step 3: Add `psu_driver_get_family` / `psu_driver_set_family` to `psu_driver.h`**

Append above the `extern "C"` close brace:

```c
const char *psu_driver_get_family(void);                 // active backend name
esp_err_t   psu_driver_set_family(const char *name);     // NVS-set; effective on reboot
```

- [ ] **Step 4: Stub the other two backends so the build links**

`components/psu_driver/psu_xy_sk120.c`:
```c
#include "psu_backend.h"
#include "esp_log.h"

static const char *TAG = "psu_xy_sk120";

static esp_err_t stub_detect(void)
{
    ESP_LOGW(TAG, "xy_sk120 backend not yet implemented (Task B4)");
    psu_driver_priv_publish_model(0, "XY-SK120", 1000.0f, 5.0f);
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t stub_poll(void)         { return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_voltage(float v) { (void)v; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_current(float i) { (void)i; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_output(bool on)  { (void)on; return ESP_ERR_NOT_SUPPORTED; }

const psu_backend_t psu_backend_xy_sk120 = {
    .name = "xy_sk120", .default_baud = 115200,
    .detect = stub_detect, .poll = stub_poll,
    .set_voltage = stub_set_voltage,
    .set_current = stub_set_current,
    .set_output  = stub_set_output,
};
```

`components/psu_driver/psu_wz5005.c`:
```c
#include "psu_backend.h"
#include "esp_log.h"

static const char *TAG = "psu_wz5005";

static esp_err_t stub_detect(void)
{
    ESP_LOGW(TAG, "wz5005 backend not yet implemented (Task B5)");
    psu_driver_priv_publish_model(0, "WZ5005", 1000.0f, 5.0f);
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t stub_poll(void)         { return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_voltage(float v) { (void)v; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_current(float i) { (void)i; return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t stub_set_output(bool on)  { (void)on; return ESP_ERR_NOT_SUPPORTED; }

const psu_backend_t psu_backend_wz5005 = {
    .name = "wz5005", .default_baud = 19200,
    .detect = stub_detect, .poll = stub_poll,
    .set_voltage = stub_set_voltage,
    .set_current = stub_set_current,
    .set_output  = stub_set_output,
};
```

- [ ] **Step 5: Build**

Run: `idf.py build`
Expected: success.

- [ ] **Step 6: Flash and verify Riden still works (no behaviour change yet)**

Run: `idf.py -p COM24 flash monitor`
Expected: boot log `psu_driver: UART1 ready: family=riden ... baud=19200` and **a warning** because 19200 ≠ 115200 (the new Riden default per spec). Disregard the warning for now — the user's bench is keyed to 19200, so PSU detect succeeds. Dashboard panel works as before.

- [ ] **Step 7: Commit**

```bash
git add components/psu_driver/
git commit -m "refactor(psu_driver): vtable dispatch + extract psu_riden.c"
```

---

### Task B4: Implement XY-SK120 backend

**Files:**
- Modify: `components/psu_driver/psu_xy_sk120.c` (replace stub)

- [ ] **Step 1: Replace the stub body**

```c
#include "psu_backend.h"
#include "psu_modbus_rtu.h"

#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "psu_xy_sk120";

#define XY_REG_V_SET   0x0000   // ÷100, 2 decimals (V)
#define XY_REG_I_SET   0x0001   // ÷1000, 3 decimals (A)
#define XY_REG_V_OUT   0x0002
#define XY_REG_I_OUT   0x0003
#define XY_REG_OUTPUT  0x0012
#define XY_REG_MODEL   0x0016

#define XY_MODEL_ID    22873     // returned by REG_MODEL on XY-SK120
#define XY_I_MAX       5.0f
#define XY_I_SCALE     1000.0f

static esp_err_t xy_detect(void)
{
    uint16_t model = 0;
    esp_err_t e = psu_modbus_rtu_read_holding(psu_driver_priv_get_slave(),
                                              XY_REG_MODEL, 1, &model);
    psu_driver_priv_note_txn_result(e);
    if (e != ESP_OK) {
        psu_driver_priv_publish_model(0, "XY-SK120", XY_I_SCALE, XY_I_MAX);
        return e;
    }
    const char *name = (model == XY_MODEL_ID) ? "XY-SK120" : "unknown";
    psu_driver_priv_publish_model(model, name, XY_I_SCALE, XY_I_MAX);
    ESP_LOGI(TAG, "detected model %u (%s)", model, name);
    return ESP_OK;
}

static esp_err_t xy_poll(void)
{
    uint8_t slave = psu_driver_priv_get_slave();

    uint16_t r[4] = {0};
    esp_err_t e = psu_modbus_rtu_read_holding(slave, XY_REG_V_SET, 4, r);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) {
        psu_driver_priv_publish_v_set(r[0] / 100.0f);
        psu_driver_priv_publish_i_set(r[1] / XY_I_SCALE);
        psu_driver_priv_publish_v_out(r[2] / 100.0f);
        psu_driver_priv_publish_i_out(r[3] / XY_I_SCALE);
    }

    uint16_t o = 0;
    e = psu_modbus_rtu_read_holding(slave, XY_REG_OUTPUT, 1, &o);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_output(o != 0);

    return ESP_OK;
}

static esp_err_t xy_set_voltage(float v)
{
    if (v < 0.0f)  v = 0.0f;
    if (v > 30.0f) v = 30.0f;   // XY-SK120 V ceiling is 30 V
    uint16_t raw = (uint16_t)(v * 100.0f + 0.5f);
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(),
                                              XY_REG_V_SET, raw);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_v_set(raw / 100.0f);
    return e;
}

static esp_err_t xy_set_current(float i)
{
    if (i < 0.0f)    i = 0.0f;
    if (i > XY_I_MAX) i = XY_I_MAX;
    uint16_t raw = (uint16_t)(i * XY_I_SCALE + 0.5f);
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(),
                                              XY_REG_I_SET, raw);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_i_set(raw / XY_I_SCALE);
    return e;
}

static esp_err_t xy_set_output(bool on)
{
    esp_err_t e = psu_modbus_rtu_write_single(psu_driver_priv_get_slave(),
                                              XY_REG_OUTPUT, on ? 1 : 0);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_output(on);
    return e;
}

const psu_backend_t psu_backend_xy_sk120 = {
    .name         = "xy_sk120",
    .default_baud = 115200,
    .detect       = xy_detect,
    .poll         = xy_poll,
    .set_voltage  = xy_set_voltage,
    .set_current  = xy_set_current,
    .set_output   = xy_set_output,
};
```

- [ ] **Step 2: Build**

Run: `idf.py build`
Expected: success.

- [ ] **Step 3: Commit**

```bash
git add components/psu_driver/psu_xy_sk120.c
git commit -m "feat(psu_driver): implement XY-SK120 backend"
```

---

### Task B5: Implement WZ5005 backend

**Files:**
- Modify: `components/psu_driver/psu_wz5005.c` (replace stub)

- [ ] **Step 1: Replace the stub body**

```c
#include "psu_backend.h"

#include <string.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "psu_wz5005";

#define WZ_UART_PORT          UART_NUM_1
#define WZ_FRAME_LEN          20
#define WZ_HEADER             0xAA
#define WZ_TXN_TIMEOUT_MS     150
#define WZ_INTERFRAME_MS      3        // ~3.5 char @ 19200

#define WZ_OP_SET_MODE        0x20     // arg byte 0: 0=manual, 1=remote
#define WZ_OP_SET_ADDR        0x21
#define WZ_OP_SET_OUTPUT      0x22
#define WZ_OP_GET_STATUS      0x23
#define WZ_OP_GET_FACTORY     0x24
#define WZ_OP_READ_VI_BLOCK   0x2B
#define WZ_OP_WRITE_VI_BLOCK  0x2C

#define WZ_I_MAX              5.0f
#define WZ_V_MAX              50.0f
#define WZ_V_SCALE            1000.0f   // raw / 1000 = volts (verified by reference frame)
#define WZ_I_SCALE            1000.0f   // assumed by analogy; verified on bench

// Builds a 20-byte WZ5005 frame in `out`. `args` of length `arg_len` is
// copied to bytes 3..(3+arg_len-1); remaining argument bytes are zeroed.
// Final byte is sum-of-bytes[0..18] mod 256.
static void wz_build_frame(uint8_t *out, uint8_t addr, uint8_t op,
                           const uint8_t *args, size_t arg_len)
{
    memset(out, 0, WZ_FRAME_LEN);
    out[0] = WZ_HEADER;
    out[1] = addr;
    out[2] = op;
    if (args && arg_len) {
        if (arg_len > 16) arg_len = 16;   // bytes 3..18 = 16 args max
        memcpy(&out[3], args, arg_len);
    }
    uint16_t s = 0;
    for (size_t i = 0; i < WZ_FRAME_LEN - 1; i++) s += out[i];
    out[WZ_FRAME_LEN - 1] = (uint8_t)(s & 0xFF);
}

static bool wz_verify_checksum(const uint8_t *frame)
{
    uint16_t s = 0;
    for (size_t i = 0; i < WZ_FRAME_LEN - 1; i++) s += frame[i];
    return (uint8_t)(s & 0xFF) == frame[WZ_FRAME_LEN - 1];
}

// One-shot WZ5005 transaction. Acquires shared UART mutex, writes 20-byte
// request, reads 20-byte response, validates header + addr + checksum.
// Returns ESP_OK with bytes 3..18 of the response copied into resp_args
// (up to resp_args_len bytes).
static esp_err_t wz_txn(uint8_t op, const uint8_t *args, size_t arg_len,
                        uint8_t *resp_args, size_t resp_args_len)
{
    SemaphoreHandle_t mtx = psu_driver_priv_get_uart_mutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(WZ_TXN_TIMEOUT_MS + 50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t req[WZ_FRAME_LEN];
    wz_build_frame(req, psu_driver_priv_get_slave(), op, args, arg_len);

    uart_flush_input(WZ_UART_PORT);
    int written = uart_write_bytes(WZ_UART_PORT, (const char *)req, WZ_FRAME_LEN);
    esp_err_t e;
    if (written != WZ_FRAME_LEN) { e = ESP_ERR_INVALID_STATE; goto out; }
    e = uart_wait_tx_done(WZ_UART_PORT, pdMS_TO_TICKS(50));
    if (e != ESP_OK) goto out;

    uint8_t resp[WZ_FRAME_LEN];
    int got = uart_read_bytes(WZ_UART_PORT, resp, WZ_FRAME_LEN,
                              pdMS_TO_TICKS(WZ_TXN_TIMEOUT_MS));
    vTaskDelay(pdMS_TO_TICKS(WZ_INTERFRAME_MS));
    if (got < WZ_FRAME_LEN)        { e = ESP_ERR_TIMEOUT;          goto out; }
    if (resp[0] != WZ_HEADER)      { e = ESP_ERR_INVALID_RESPONSE; goto out; }
    if (resp[1] != req[1])         { e = ESP_ERR_INVALID_RESPONSE; goto out; }
    if (!wz_verify_checksum(resp)) { e = ESP_ERR_INVALID_CRC;      goto out; }
    // Per WZ5005 manual: a status byte of 0x80 in the response = OK,
    // 0x90/0xA0/0xB0/0xC0/0xD0 = error. The byte position is op-dependent
    // and not authoritatively documented; we treat structural validity
    // (header + addr + checksum) as success and let the caller interpret
    // payload semantics. Field-incorrect frames will surface as drift in
    // telemetry vs front-panel display, not as link_ok=false.
    if (resp_args && resp_args_len) {
        size_t n = (resp_args_len > 16) ? 16 : resp_args_len;
        memcpy(resp_args, &resp[3], n);
    }
    e = ESP_OK;
out:
    xSemaphoreGive(mtx);
    return e;
}

// Cached last-read VI block — used by set_voltage/set_current to do a
// read-modify-write without a fresh read each setpoint write. Refreshed
// every poll() cycle.
static uint8_t s_last_vi[16];
static bool    s_have_vi;

static esp_err_t wz_read_vi_block(void)
{
    uint8_t resp_args[16] = {0};
    esp_err_t e = wz_txn(WZ_OP_READ_VI_BLOCK, NULL, 0, resp_args, sizeof(resp_args));
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) {
        memcpy(s_last_vi, resp_args, sizeof(s_last_vi));
        s_have_vi = true;
        // Layout (best-guess per spec, big-endian pairs):
        //   bytes 0..1   V_SET   ÷1000
        //   bytes 2..3   I_SET   ÷1000
        //   bytes 4..5   OVP     ÷1000  (read but not surfaced in v1)
        //   bytes 6..7   OCP     ÷1000  (read but not surfaced in v1)
        //   bytes 8..9   V_OUT   ÷1000
        //   bytes 10..11 I_OUT   ÷1000
        uint16_t v_set = ((uint16_t)resp_args[0]  << 8) | resp_args[1];
        uint16_t i_set = ((uint16_t)resp_args[2]  << 8) | resp_args[3];
        uint16_t v_out = ((uint16_t)resp_args[8]  << 8) | resp_args[9];
        uint16_t i_out = ((uint16_t)resp_args[10] << 8) | resp_args[11];
        psu_driver_priv_publish_v_set(v_set / WZ_V_SCALE);
        psu_driver_priv_publish_i_set(i_set / WZ_I_SCALE);
        psu_driver_priv_publish_v_out(v_out / WZ_V_SCALE);
        psu_driver_priv_publish_i_out(i_out / WZ_I_SCALE);
    }
    return e;
}

static esp_err_t wz_read_output_state(void)
{
    uint8_t resp_args[16] = {0};
    esp_err_t e = wz_txn(WZ_OP_GET_STATUS, NULL, 0, resp_args, sizeof(resp_args));
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) {
        // Per kordian-kowalski/wz5005-control: byte 0 = output status (0/1).
        psu_driver_priv_publish_output(resp_args[0] != 0);
    }
    return e;
}

static esp_err_t wz_detect(void)
{
    // Try to enter remote mode (best-effort; ignore failure — some units
    // accept commands without explicit remote mode).
    uint8_t mode_arg = 1;
    (void)wz_txn(WZ_OP_SET_MODE, &mode_arg, 1, NULL, 0);

    uint8_t resp_args[16] = {0};
    esp_err_t e = wz_txn(WZ_OP_GET_FACTORY, NULL, 0, resp_args, sizeof(resp_args));
    psu_driver_priv_note_txn_result(e);
    if (e != ESP_OK) {
        psu_driver_priv_publish_model(0, "WZ5005", WZ_I_SCALE, WZ_I_MAX);
        return e;
    }
    // Per the kordian-kowalski reference: byte 0 = model. We don't have a
    // model table for WZ5xxx — any non-zero detect response is treated as
    // "WZ5005" for v1.
    uint16_t id = resp_args[0];
    if (id == 0) id = 1;  // ensure model_id != 0 so re-detect doesn't loop
    psu_driver_priv_publish_model(id, "WZ5005", WZ_I_SCALE, WZ_I_MAX);
    ESP_LOGI(TAG, "detected WZ5005 (factory byte 0 = %u)", resp_args[0]);
    return ESP_OK;
}

static esp_err_t wz_poll(void)
{
    wz_read_vi_block();
    wz_read_output_state();
    return ESP_OK;
}

static esp_err_t wz_set_voltage(float v)
{
    if (v < 0.0f)        v = 0.0f;
    if (v > WZ_V_MAX)    v = WZ_V_MAX;

    if (!s_have_vi) {
        esp_err_t e = wz_read_vi_block();
        if (e != ESP_OK) return e;
    }
    uint8_t args[16];
    memcpy(args, s_last_vi, 16);
    uint16_t raw = (uint16_t)(v * WZ_V_SCALE + 0.5f);
    args[0] = (raw >> 8) & 0xFF;
    args[1] = raw & 0xFF;

    esp_err_t e = wz_txn(WZ_OP_WRITE_VI_BLOCK, args, sizeof(args), NULL, 0);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) {
        memcpy(s_last_vi, args, 16);
        psu_driver_priv_publish_v_set(raw / WZ_V_SCALE);
    }
    return e;
}

static esp_err_t wz_set_current(float i)
{
    if (i < 0.0f)     i = 0.0f;
    if (i > WZ_I_MAX) i = WZ_I_MAX;

    if (!s_have_vi) {
        esp_err_t e = wz_read_vi_block();
        if (e != ESP_OK) return e;
    }
    uint8_t args[16];
    memcpy(args, s_last_vi, 16);
    uint16_t raw = (uint16_t)(i * WZ_I_SCALE + 0.5f);
    args[2] = (raw >> 8) & 0xFF;
    args[3] = raw & 0xFF;

    esp_err_t e = wz_txn(WZ_OP_WRITE_VI_BLOCK, args, sizeof(args), NULL, 0);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) {
        memcpy(s_last_vi, args, 16);
        psu_driver_priv_publish_i_set(raw / WZ_I_SCALE);
    }
    return e;
}

static esp_err_t wz_set_output(bool on)
{
    uint8_t arg = on ? 1 : 0;
    esp_err_t e = wz_txn(WZ_OP_SET_OUTPUT, &arg, 1, NULL, 0);
    psu_driver_priv_note_txn_result(e);
    if (e == ESP_OK) psu_driver_priv_publish_output(on);
    return e;
}

const psu_backend_t psu_backend_wz5005 = {
    .name         = "wz5005",
    .default_baud = 19200,
    .detect       = wz_detect,
    .poll         = wz_poll,
    .set_voltage  = wz_set_voltage,
    .set_current  = wz_set_current,
    .set_output   = wz_set_output,
};
```

- [ ] **Step 2: Build**

Run: `idf.py build`
Expected: success.

- [ ] **Step 3: Commit**

```bash
git add components/psu_driver/psu_wz5005.c
git commit -m "feat(psu_driver): implement WZ5005 backend (best-effort 0x2B/0x2C layout)"
```

---

### Task B6: Update Kconfig with family choice + per-family baud defaults

**Files:**
- Modify: `components/psu_driver/Kconfig.projbuild`

- [ ] **Step 1: Replace the file content**

```kconfig
menu "PSU driver"

    config APP_PSU_UART_TX_GPIO
        int "PSU UART1 TX GPIO"
        default 38

    config APP_PSU_UART_RX_GPIO
        int "PSU UART1 RX GPIO"
        default 39

    choice APP_PSU_DEFAULT_FAMILY
        prompt "Default PSU family (used when NVS key absent)"
        default APP_PSU_DEFAULT_FAMILY_RIDEN
    config APP_PSU_DEFAULT_FAMILY_RIDEN
        bool "Riden RD60xx (Modbus-RTU)"
    config APP_PSU_DEFAULT_FAMILY_XY_SK120
        bool "XY-SK120 (Modbus-RTU)"
    config APP_PSU_DEFAULT_FAMILY_WZ5005
        bool "WZ5005 (custom 20-byte protocol)"
    endchoice

    # Each family's default tracks the PSU's factory-shipped baud:
    #   Riden RD60xx → 115200, XY-SK120 → 115200, WZ5005 → 19200.
    # If the bench unit's panel has been re-keyed, override per-build.
    config APP_PSU_UART_BAUD
        int "PSU UART1 baud rate (matches the PSU's panel-set rate)"
        default 115200 if APP_PSU_DEFAULT_FAMILY_RIDEN
        default 115200 if APP_PSU_DEFAULT_FAMILY_XY_SK120
        default 19200  if APP_PSU_DEFAULT_FAMILY_WZ5005

    config APP_PSU_SLAVE_DEFAULT
        int "PSU slave address default (used on first boot before NVS)"
        default 1
        range 1 255

endmenu
```

- [ ] **Step 2: Use the Kconfig family default to seed `s_backend` when NVS key is absent**

In `components/psu_driver/psu_driver.c`, modify `load_nvs_state` so that when `nvs_get_str(.., NVS_KEY_FAMILY, ..)` fails, `s_backend` is set per Kconfig:

```c
    if (nvs_get_str(h, NVS_KEY_FAMILY, name, &n) != ESP_OK) {
#if defined(CONFIG_APP_PSU_DEFAULT_FAMILY_XY_SK120)
        s_backend = &psu_backend_xy_sk120;
#elif defined(CONFIG_APP_PSU_DEFAULT_FAMILY_WZ5005)
        s_backend = &psu_backend_wz5005;
#else
        s_backend = &psu_backend_riden;
#endif
    } else {
        s_backend = resolve_backend_by_name(name);
    }
```

Apply the same fallback in the early-return when `nvs_open` fails:

```c
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        atomic_store_explicit(&s_slave_addr,
                              (uint8_t)CONFIG_APP_PSU_SLAVE_DEFAULT,
                              memory_order_relaxed);
#if defined(CONFIG_APP_PSU_DEFAULT_FAMILY_XY_SK120)
        s_backend = &psu_backend_xy_sk120;
#elif defined(CONFIG_APP_PSU_DEFAULT_FAMILY_WZ5005)
        s_backend = &psu_backend_wz5005;
#else
        s_backend = &psu_backend_riden;
#endif
        return;
    }
```

- [ ] **Step 3: Clean rebuild — Kconfig changes need a fresh sdkconfig**

```bash
del sdkconfig
idf.py build
```

Expected: success. New `sdkconfig` lists `CONFIG_APP_PSU_DEFAULT_FAMILY_RIDEN=y` and `CONFIG_APP_PSU_UART_BAUD=115200`.

**Bench-baud override note:** if your Riden unit is keyed to 19200 (the project-historic value), `idf.py menuconfig` → "PSU driver" → "PSU UART1 baud rate" → 19200, save, `idf.py build`. The `psu_driver` log line will then no longer warn about a baud/family-default mismatch.

- [ ] **Step 4: Commit**

```bash
git add components/psu_driver/Kconfig.projbuild components/psu_driver/psu_driver.c
git commit -m "feat(psu_driver): Kconfig family choice + factory-baud per-family defaults"
```

---

### Task B7: Wire `psu_family` through CLI

**Files:**
- Modify: `main/app_main.c`

- [ ] **Step 1: Add a `cmd_psu_family` handler near `cmd_psu_status` (around line 230)**

After `cmd_psu_status`, insert:

```c
// ---- CLI: psu_family [<name>] -----------------------------------------------
static struct { struct arg_str *name; struct arg_end *end; } s_psu_family_args;
static int cmd_psu_family(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_psu_family_args);
    if (n != 0) {
        // No arg → print current.
        printf("psu_family: %s\n", psu_driver_get_family());
        return 0;
    }
    const char *want = s_psu_family_args.name->sval[0];
    esp_err_t e = psu_driver_set_family(want);
    if (e == ESP_ERR_INVALID_ARG) {
        printf("invalid family '%s' (use riden|xy_sk120|wz5005)\n", want);
        return 1;
    }
    if (e != ESP_OK) { printf("set failed: %s\n", esp_err_to_name(e)); return 1; }
    printf("psu_family set to '%s' — reboot to apply\n", want);
    return 0;
}
```

- [ ] **Step 2: Register the command in `register_commands()` (after the `psu_status_cmd` block around line 362)**

```c
    s_psu_family_args.name = arg_str0(NULL, NULL, "<name>",
                                      "riden | xy_sk120 | wz5005 (omit to print current)");
    s_psu_family_args.end  = arg_end(1);
    const esp_console_cmd_t psu_family_cmd = {
        .command = "psu_family",
        .help    = "get/set PSU family in NVS (reboot to apply)",
        .hint    = NULL,
        .func    = cmd_psu_family,
        .argtable = &s_psu_family_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&psu_family_cmd));
```

- [ ] **Step 3: Build**

Run: `idf.py build`
Expected: success.

- [ ] **Step 4: Commit**

```bash
git add main/app_main.c
git commit -m "feat(cli): add psu_family get/set command"
```

---

## Phase C — Surface family on dashboard + CLAUDE.md update

### Task C1: Extend WS status payload + add `set_psu_family` op

**Files:**
- Modify: `components/net_dashboard/ws_handler.c`

- [ ] **Step 1: Add the `family` field to the `psu` JSON object in `telemetry_task` (around line 247)**

Replace the existing `snprintf` block:

```c
        if (n < (int)sizeof(payload)) {
            n += snprintf(payload + n, sizeof(payload) - n,
                ",\"psu\":{\"v_set\":%.2f,\"i_set\":%.3f,\"v_out\":%.2f,\"i_out\":%.3f,"
                "\"output\":%s,\"link\":%s,\"model\":\"%s\",\"slave\":%u,\"family\":\"%s\"}",
                (double)pt.v_set, (double)pt.i_set,
                (double)pt.v_out, (double)pt.i_out,
                pt.output_on ? "true" : "false",
                pt.link_ok   ? "true" : "false",
                psu_driver_get_model_name(),
                psu_driver_get_slave_addr(),
                psu_driver_get_family());
        }
```

- [ ] **Step 2: Add `set_psu_family` and `reboot` handlers in `handle_json` (after the `set_psu_slave` block around line 169)**

```c
    } else if (strcmp(type_j->valuestring, "set_psu_family") == 0) {
        const cJSON *fam = cJSON_GetObjectItem(root, "family");
        if (!cJSON_IsString(fam)) return;
        esp_err_t e = psu_driver_set_family(fam->valuestring);
        char ack[96];
        snprintf(ack, sizeof(ack),
                 "{\"type\":\"ack\",\"op\":\"set_psu_family\",\"ok\":%s}",
                 e == ESP_OK ? "true" : "false");
        ws_send_json_to(fd, ack);
    } else if (strcmp(type_j->valuestring, "reboot") == 0) {
        ESP_LOGW(TAG, "reboot requested via ws fd=%d", fd);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"reboot\"}");
        // 200 ms grace for the ack to flush, mirror of factory_reset
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
```

(`esp_restart` already comes in via `esp_system.h`; if the include is missing, add `#include "esp_system.h"` at the top.)

- [ ] **Step 3: Build**

Run: `idf.py build`
Expected: success.

- [ ] **Step 4: Commit**

```bash
git add components/net_dashboard/ws_handler.c
git commit -m "feat(ws): add family to psu status block; add set_psu_family + reboot ops"
```

---

### Task C2: Dashboard UI — family dropdown + reboot button

**Files:**
- Modify: `components/net_dashboard/web/index.html`
- Modify: `components/net_dashboard/web/app.js`
- Modify: `components/net_dashboard/web/app.css`

- [ ] **Step 1: Add the family row + reboot button to `index.html` (insert after the slave-addr row around line 226)**

Locate the existing `psu-settings` row:
```html
      <div class="psu-row psu-settings">
        <label data-i18n="psu_slave_lbl">Slave addr</label>
        <input type="number" id="psu-slave-input" min="1" max="247" value="1" />
        <button id="psu-slave-save" data-i18n="psu_save">Save</button>
      </div>
```

Add immediately after:
```html
      <div class="psu-row psu-settings">
        <label data-i18n="psu_family_lbl">Family</label>
        <select id="psu-family-select">
          <option value="riden">Riden RD60xx</option>
          <option value="xy_sk120">XY-SK120</option>
          <option value="wz5005">WZ5005</option>
        </select>
        <button id="psu-family-save" data-i18n="psu_save">Save</button>
        <button id="psu-reboot" class="psu-btn" data-i18n="psu_reboot">Reboot</button>
        <span id="psu-family-pending" class="psu-pending" hidden></span>
      </div>
```

Also widen the slave input's `max` from `247` to `255` (WZ5005 allows up to 255):
```html
        <input type="number" id="psu-slave-input" min="1" max="255" value="1" />
```

- [ ] **Step 2: Add the i18n strings to `app.js` `i18n` tables**

Find each language block (`en:`, `tw:`, `cn:`) — they're around lines 80, 160, 245. Append the same two keys to each block, with appropriate translations:

EN block (after `psu_offline:`):
```js
      psu_family_lbl: 'Family',
      psu_reboot: 'Reboot',
      psu_family_pending: 'reboot to apply',
```

TW block:
```js
      psu_family_lbl: '型號',
      psu_reboot: '重新開機',
      psu_family_pending: '重開後生效',
```

CN block:
```js
      psu_family_lbl: '型号',
      psu_reboot: '重新开机',
      psu_family_pending: '重启后生效',
```

- [ ] **Step 3: Wire the family + reboot buttons in `app.js`**

Find the existing slave-save handler (search for `psu-slave-save` in `app.js`) and add right after it:

```js
const familySelect = document.getElementById('psu-family-select');
const familySave   = document.getElementById('psu-family-save');
const familyPending = document.getElementById('psu-family-pending');
const rebootBtn    = document.getElementById('psu-reboot');

let liveFamily = null;       // last value seen on telemetry
let pendingFamily = null;    // last value the user pushed

familySave.addEventListener('click', () => {
  const want = familySelect.value;
  ws.send(JSON.stringify({ type: 'set_psu_family', family: want }));
  pendingFamily = want;
  if (familyPending) {
    familyPending.textContent = i18n.get('psu_family_pending');
    familyPending.hidden = false;
  }
});

rebootBtn.addEventListener('click', () => {
  if (!confirm('Reboot the device?')) return;
  ws.send(JSON.stringify({ type: 'reboot' }));
});
```

In the existing telemetry-render block (search for `psu.model`, around line 881), add a sibling line:

```js
        if (psu.family) {
          liveFamily = psu.family;
          if (document.activeElement !== familySelect) {
            familySelect.value = psu.family;
          }
          if (familyPending && pendingFamily && pendingFamily === psu.family) {
            familyPending.hidden = true;
            pendingFamily = null;
          }
        }
```

- [ ] **Step 4: Add CSS for the `.psu-pending` indicator in `app.css`**

After the existing `.psu-settings` style block, append:

```css
.psu-pending {
  color: var(--warn, #c47);
  font-size: 0.85em;
  margin-left: 0.5em;
}
```

- [ ] **Step 5: Build**

Run: `idf.py build`
Expected: success. The web bundle is regenerated as part of the build.

- [ ] **Step 6: Commit**

```bash
git add components/net_dashboard/web/
git commit -m "feat(dashboard): PSU family dropdown + reboot button"
```

---

### Task C3: Update CLAUDE.md sections

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Replace the "PSU Modbus-RTU master" section title and intro**

Find the heading `## PSU Modbus-RTU master (5th controllable subsystem)` and replace the heading + first paragraph:

Old (around CLAUDE.md:411):
```markdown
## PSU Modbus-RTU master (5th controllable subsystem)

Hand-rolled RTU master targeting the Riden RD60xx family (RD6006 / RD6012 /
RD6018). UART1 @ 19200-8N1 on GPIO 38 / 39 (Kconfig overridable). One
peripheral driver, four frontends — same single-handler invariant as PWM,
RPM, GPIO, and the relay power switch:
```

New:
```markdown
## PSU driver — multi-family (5th controllable subsystem)

`components/psu_driver/` owns UART1 and dispatches to one of three PSU
backends at boot, picked by the NVS key `psu_driver/family` (default
from Kconfig `APP_PSU_DEFAULT_FAMILY_*`):

- **`riden`** — Riden RD60xx (Modbus-RTU, factory baud 115200, register
  map in `psu_riden.c`).
- **`xy_sk120`** — XY-SK120 (Modbus-RTU, factory baud 115200, register
  map in `psu_xy_sk120.c`).
- **`wz5005`** — WZ5005 (custom 20-byte sum-checksum protocol, factory
  baud 19200, op codes in `psu_wz5005.c`).

`APP_PSU_UART_BAUD` defaults track the family's factory baud; bench
operators with re-keyed panels override per-build via
`idf.py menuconfig` → "PSU driver". The two Modbus-RTU backends share
`psu_modbus_rtu.c` (CRC-16, FC 0x03/0x06 helpers); WZ5005 is fully
standalone. One UART mutex shared by all backends.

Switching family at runtime: dashboard PSU panel → Family dropdown →
Save → Reboot button (or `psu_family <name>` CLI + manual reboot).
The change is NVS-persisted but boot-effective (re-init UART for new
baud is risky vs simple reboot).

Same single-handler invariant as PWM, RPM, GPIO, and the relay power
switch:
```

- [ ] **Step 2: Update the diagram below it**

Old:
```
USB HID 0x05 + ops 0x10..0x13                            ├──► control_task ──► psu_modbus_set_*()
USB CDC ops 0x40..0x43                                   │
CLI psu_v / psu_i / psu_out / psu_slave                  ──┘
```

New:
```
USB HID 0x05 + ops 0x10..0x13                            ├──► control_task ──► psu_driver_set_*()
USB CDC ops 0x40..0x43                                   │                          │
CLI psu_v / psu_i / psu_out / psu_slave / psu_family     ──┘                          ▼
                                                                          backend vtable
                                                                          (riden / xy_sk120 / wz5005)
```

- [ ] **Step 3: Update the "Slave address is NVS-persisted" paragraph**

Old:
```
Slave address is NVS-persisted in namespace `psu_modbus`, key
`slave_addr`. Setting it does **not** issue a Modbus write — the supply's
own slave address is set from the supply's front panel; firmware just
matches it.
```

New:
```
Slave address + family are NVS-persisted in namespace `psu_driver`,
keys `slave_addr` and `family`. Setting either does **not** issue a
write to the supply — the supply's own slave address (and panel-keyed
baud, per family) is set from the front panel; firmware just matches
it. Slave range is 1..255 (Modbus families clamp to 1..247 inside the
backend; WZ5005 uses the full 1..255).
```

- [ ] **Step 4: Update the "Hand-rolled" paragraph**

Old:
```
Hand-rolled (not `esp-modbus`) because we use 2 function codes (0x03 read
holding, 0x06 write single) and 5 registers (V_SET=0x08, I_SET=0x09,
V_OUT=0x0A, I_OUT=0x0B, OUTPUT=0x12; MODEL=0x00 for boot detect). Adding
another component-manager pin alongside `esp_tinyusb` / `mdns` / `cjson`
would exceed the LoC saved.
```

New:
```
Hand-rolled (not `esp-modbus`) because we use 2 function codes (0x03 read
holding, 0x06 write single) for the Modbus families, and a wholly
different 20-byte custom frame for WZ5005. Adding another component-
manager pin alongside `esp_tinyusb` / `mdns` / `cjson` would exceed the
LoC saved. Riden register map in `psu_riden.c`; XY-SK120 register map
in `psu_xy_sk120.c`; WZ5005 op-code table in `psu_wz5005.c`.
```

- [ ] **Step 5: Update the "boot-time CRC" paragraph (if still present)**

The existing paragraph about no boot-time CRC self-check stays valid
for the Modbus families — no change needed. For WZ5005, `psu_wz5005.c`
similarly has no boot-time checksum self-check; correctness is verified
end-to-end by `link_ok` recovery on first transaction. Add one sentence
to the existing paragraph:

After:
```
boot-time check unless you have a verified canonical CRC vector AND log
output before the trap (constructors run before `ESP_LOG` is up, so a
silent trap is the only failure mode — debug-hostile by construction).
```

Append:
```
The same posture applies to WZ5005's sum-mod-256 checksum.
```

- [ ] **Step 6: Update the "UART access is funnelled" paragraph**

Old:
```
UART access is funnelled through a single mutex (`s_uart_mutex`) so
setpoint writes from `control_task` (priority 6) and the polling loop on
`psu_task` (priority 4) cannot interleave bytes on the wire. Inter-frame
gap is 2 ms (3.5-char @ 19200 ≈ 1.8 ms).
```

New:
```
UART access is funnelled through a single mutex (`s_uart_mutex` in
`psu_driver.c`, exposed to backends via `psu_driver_priv_get_uart_mutex`)
so setpoint writes from `control_task` (priority 6) and the polling
loop on `psu_task` (priority 4) cannot interleave bytes on the wire.
Inter-frame gap is 2 ms for Modbus families (3.5-char @ 19200 ≈ 1.8 ms);
WZ5005 uses 3 ms.
```

- [ ] **Step 7: Build to confirm CLAUDE.md docs render correctly (no actual build effect — just sanity)**

Run: `idf.py build`
Expected: success.

- [ ] **Step 8: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(claude): update PSU section for multi-driver (riden/xy_sk120/wz5005)"
```

---

## Phase D — On-hardware verification

These tasks are not code edits; they confirm the work is shippable. Each scenario must pass; failures roll back to the implementation phase that introduced the regression.

### Task D1: Riden regression on hardware

- [ ] **Step 1: Override baud to 19200 if the bench Riden is panel-keyed there**

```bash
idf.py menuconfig
# → "PSU driver" → "PSU UART1 baud rate" → set to 19200, save, exit
```

- [ ] **Step 2: Flash + monitor**

Run: `idf.py -p COM24 flash monitor`
Expected: log line `psu_driver: UART1 ready: family=riden tx=38 rx=39 baud=19200 slave=1` followed by `psu_riden: detected model 60062 (RD6006, I scale = ÷1000)` (or whichever model is wired).

- [ ] **Step 3: Dashboard sanity**

- Open `http://fan-testkit.local/`. PSU panel shows Family dropdown set to "Riden RD60xx".
- Slider V → 5.00, see `v_out` track within ~50 ms.
- Slider I → 0.500, output ON/OFF toggles.
- Slave addr → 5 → Save → reboot → confirm comes back as 5 → set back to 1.

- [ ] **Step 4: CLI sanity**

- `psu_status` prints model + values.
- `psu_family` prints `riden`.

### Task D2: XY-SK120 acceptance

- [ ] **Step 1: Set family to xy_sk120 via CLI**

In the REPL: `psu_family xy_sk120` → expect "reboot to apply".

- [ ] **Step 2: Override baud to 115200 if needed (XY-SK120 default)**

```bash
idf.py menuconfig
# → "PSU driver" → "PSU UART1 baud rate" → set to 115200
idf.py build && idf.py -p COM24 flash monitor
```

- [ ] **Step 3: Wire XY-SK120 to UART1 GPIO 38/39 (panel baud also 115200)**

- [ ] **Step 4: Verify on dashboard**

- Family dropdown reads "XY-SK120".
- v_set 5.00 follows on `v_out`.
- i_set 0.500 → output ON → multimeter on terminals reads matching current.

### Task D3: WZ5005 acceptance

- [ ] **Step 1: Set family to wz5005 via dashboard dropdown + Save + Reboot**

(Dashboard path exercises the full WS round-trip.)

- [ ] **Step 2: Set baud to 19200 in menuconfig if not already; reflash**

- [ ] **Step 3: Wire WZ5005 to UART1 GPIO 38/39 (panel baud 19200)**

- [ ] **Step 4: Confirm boot log**

`psu_driver: UART1 ready: family=wz5005 ... baud=19200`
`psu_wz5005: detected WZ5005 (factory byte 0 = N)` — N is whatever the unit reports.

- [ ] **Step 5: Setpoint sanity — verify byte layout against real hardware**

- v_set 5.00 → multimeter reads 5.00 ± 0.05 V on terminals (with output ON).
- v_set 12.34 → reads 12.34 ± 0.05 V.
- i_set 0.500 with a 10 Ω load → meter reads 500 mA.
- Toggle output via dashboard ON/OFF — front panel display matches.

If v_set produces obviously wrong values (e.g. 0.5 V when 5.0 was set, or 50 V when 5 was set), the byte offsets in the 0x2B/0x2C block are wrong — see Task B5's spec note on inferred layout. Capture a scope trace of one set-voltage transaction and adjust offsets in `psu_wz5005.c`. The fix is a single-file edit; commit with a message referencing the captured frame.

### Task D4: Family round-trip + wrong-family graceful failure

- [ ] **Step 1: With WZ5005 wired, set family to riden via dashboard → reboot → confirm `link=down` within 1 s, "PSU offline" UI state, no crash**

(Wrong-family test — Modbus framing on a WZ5005 produces no valid response.)

- [ ] **Step 2: Set family back to wz5005 → reboot → confirm `link=up` recovers**

(NVS round-trip + no UART driver lock-up after a failed-family interlude.)

### Task D5: Final commit + branch summary

- [ ] **Step 1: Make sure there are no uncommitted changes**

Run: `git status`
Expected: clean.

- [ ] **Step 2: Print the work-summary commit log**

Run: `git log --oneline feature/psu-modbus-rtu..HEAD`
Expected: ~14 commits covering Phase A (5), Phase B (7), Phase C (3), Phase D (no commits — verification only).

- [ ] **Step 3: No code commit in this task — push branch + PR is a separate user-driven action**

Per CLAUDE.md: never push without explicit user request.

---

## Self-Review

**Spec coverage check:**
- ✅ Three families (riden/xy_sk120/wz5005) — Tasks B3, B4, B5
- ✅ NVS `family` key — Task B3 (`save_family_to_nvs`, `load_nvs_state`)
- ✅ NVS namespace renamed `psu_modbus` → `psu_driver` — Task A2 Step 1
- ✅ Public API rename — Tasks A1, A2, A3, A4
- ✅ Internal vtable — Task B1
- ✅ Shared `psu_modbus_rtu.{c,h}` — Task B2
- ✅ Standalone `psu_wz5005.c` — Task B5
- ✅ Family CLI escape hatch — Task B7
- ✅ Dashboard family picker + reboot — Task C2
- ✅ WS `family` field on `psu` block + `set_psu_family` op — Task C1
- ✅ Per-family Kconfig baud defaults (Riden 115200 factory, XY-SK120 115200, WZ5005 19200) — Task B6
- ✅ Slave range widened 1..247 → 1..255 — Tasks A4 Step 4 and B3 Step 2
- ✅ Single shared UART mutex — Task B3 (in `psu_driver.c`)
- ✅ CLAUDE.md updates — Task C3
- ✅ Phase A behaviour invariance — Task A5
- ✅ All 5 hardware test plan items from the spec ("Riden regression", "XY-SK120 fresh", "WZ5005 fresh", "Family switch round-trip", "Wrong-family graceful failure") — Tasks D1, D2, D3, D4
- ✅ "boot-time CRC" posture preserved — Task B5 (no `__attribute__((constructor))` in `psu_wz5005.c`); Task C3 Step 5 documents this
- ⚠️ Spec test plan items 6 ("CLI escape hatch") and 7 ("Build-only XY-SK120 stub when hardware unavailable") — covered implicitly by D1 Step 4 + B4 build success. Skipped explicit standalone tasks since item 8 ("WZ5005 checksum vector synthetic test") was guarded `#if 0` in the spec; not needed since the wz_build_frame inputs are exercised on first hardware contact in D3.

**Placeholder scan:** none. Every step has the actual command, code, or expected output.

**Type consistency check:**
- `psu_driver_telemetry_t` — same type used in A1, A2, A3.
- `psu_backend_t.default_baud` — int, used in B1, B3, B4, B5.
- `psu_driver_priv_publish_model(uint16_t id, const char *name, float i_scale_div, float i_max)` — signature consistent across B1, B3, B4, B5.
- `psu_modbus_rtu_read_holding(uint8_t slave, uint16_t addr, uint16_t n, uint16_t *out)` — used in B2, B3, B4 with matching args.
- `wz_txn(uint8_t op, const uint8_t *args, size_t arg_len, uint8_t *resp_args, size_t resp_args_len)` — internal-only, used consistently in B5.

No drift detected.
