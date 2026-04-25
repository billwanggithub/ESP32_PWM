# GPIO IO + Power Switch — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a software-controllable power-switch GPIO and 16 user-configurable GPIO pins (Group A input-default, Group B output-default) reachable from the Web dashboard, USB HID, USB CDC, and the UART CLI — all funneled through the existing `control_task` queue so the single-handler-multiple-frontend invariant is preserved.

**Architecture:** A new `components/gpio_io/` component owns the 16-pin atomic state table, per-pin esp_timer one-shot pulses, and a 20 Hz input polling task. Every transport translates its frame into a `ctrl_cmd_t`, posts to `ctrl_cmd_queue`, and `control_task` is the only caller of the `gpio_io_*` API. The dashboard adds two new panels (Power, GPIO) and extends Settings with a global pulse-width input. NVS persists only `pulse_width_ms`; power-switch state and per-pin mode/level reset to Kconfig defaults each boot for safety.

**Tech Stack:** ESP-IDF v6.0, FreeRTOS, esp_driver_gpio, esp_timer, NVS, cJSON, TinyUSB HID/CDC, vanilla JS dashboard with `data-i18n` keys.

**Spec:** [docs/superpowers/specs/2026-04-25-gpio-io-and-power-switch-design.md](../specs/2026-04-25-gpio-io-and-power-switch-design.md)

---

## File map

**Created:**
- `components/gpio_io/CMakeLists.txt`
- `components/gpio_io/include/gpio_io.h`
- `components/gpio_io/gpio_io.c`

**Modified:**
- `main/Kconfig.projbuild` — Kconfig defaults for power switch + 16 GPIO pin numbers + default pulse width
- `main/CMakeLists.txt` — adds `gpio_io` to REQUIRES
- `main/app_main.c` — calls `gpio_io_init()`, posts boot `CTRL_CMD_POWER_SET(0)`, registers new CLI commands, extends `status` command output
- `main/control_task.c` — handles 5 new `ctrl_cmd_kind_t` values
- `components/app_api/include/app_api.h` — adds 5 new `ctrl_cmd_kind_t` values + payload union members
- `components/usb_composite/include/usb_protocol.h` — adds HID report 0x04 + CDC ops 0x30..0x34
- `components/usb_composite/usb_descriptors.c` — adds report-id 0x04 OUT to descriptor; updates `_Static_assert` byte count
- `components/usb_composite/usb_composite.c` — updates `HID_REPORT_DESC_SIZE`
- `components/usb_composite/usb_hid_task.c` — handles report id 0x04 in `tud_hid_set_report_cb`; CDC status response gains GPIO tail
- `components/usb_composite/usb_cdc_task.c` — handles new ops 0x30..0x34
- `components/usb_composite/CMakeLists.txt` — adds `gpio_io` to REQUIRES
- `components/net_dashboard/ws_handler.c` — handles 5 new WS message types; status frame gains `gpio[]`, `power`, `pulse_width_ms` fields
- `components/net_dashboard/net_dashboard.c` — `device_info` JSON gains GPIO pin numbers
- `components/net_dashboard/CMakeLists.txt` — adds `gpio_io` to REQUIRES
- `components/net_dashboard/web/index.html` — Power panel + GPIO panel; pulse-width section in Settings
- `components/net_dashboard/web/app.js` — GPIO/power UI logic, telemetry handling, i18n strings
- `components/net_dashboard/web/app.css` — GPIO/power panel styling

---

## Phase ordering and "always-buildable" rule

Each task's commit must compile and not regress existing functionality. The order is:

1. **Tasks 1–3:** new component scaffold + Kconfig + boot-default (skeleton only — no transports)
2. **Tasks 4–5:** input-mode + pull configuration (still skeleton-tested via CLI)
3. **Tasks 6–7:** output-mode + pulse with esp_timer (still skeleton-tested via CLI)
4. **Task 8:** power switch
5. **Tasks 9–10:** CLI commands + status extension
6. **Tasks 11–12:** WebSocket frontend + telemetry extension
7. **Tasks 13–15:** Dashboard UI (HTML, JS, CSS) — i18n included
8. **Tasks 16–17:** USB HID frontend + descriptor edit
9. **Task 18:** USB CDC frontend + status payload tail
10. **Task 19:** NVS persistence of pulse-width
11. **Task 20:** End-to-end smoke verification on hardware

Tasks 1–10 are pure C and verifiable on UART. Tasks 11–18 add transport surfaces one at a time. Each commit boots; each commit can be reverted independently.

---

## sdkconfig trap reminder (CLAUDE.md)

**After Task 2 (when `Kconfig.projbuild` gains new symbols), the engineer MUST run:**

```
del sdkconfig
idf.py fullclean
idf.py build
```

Not doing this means the new Kconfig defaults are silently ignored and the
build runs with whatever `sdkconfig` had before. The first verification step
of Task 2 explicitly performs this dance.

---

## Task 1: Scaffold the `gpio_io` component (header + empty .c + CMakeLists)

**Files:**
- Create: `components/gpio_io/CMakeLists.txt`
- Create: `components/gpio_io/include/gpio_io.h`
- Create: `components/gpio_io/gpio_io.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create `components/gpio_io/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS        "gpio_io.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES    app_api
                esp_driver_gpio
                esp_timer
                nvs_flash
                log
)
```

- [ ] **Step 2: Create `components/gpio_io/include/gpio_io.h`**

```c
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_IO_PIN_COUNT 16

typedef enum {
    GPIO_IO_MODE_INPUT_PULLDOWN = 0,
    GPIO_IO_MODE_INPUT_PULLUP   = 1,
    GPIO_IO_MODE_INPUT_FLOATING = 2,
    GPIO_IO_MODE_OUTPUT         = 3,
} gpio_io_mode_t;

typedef struct {
    gpio_io_mode_t mode;
    bool           level;     // input: last sampled; output: driven
    bool           pulsing;   // true while a one-shot pulse is in flight
} gpio_io_state_t;

esp_err_t gpio_io_init(void);

esp_err_t gpio_io_set_mode (uint8_t idx, gpio_io_mode_t mode);
esp_err_t gpio_io_set_level(uint8_t idx, bool level);
esp_err_t gpio_io_pulse    (uint8_t idx, uint32_t width_ms);

void      gpio_io_get_state(uint8_t idx, gpio_io_state_t *out);
void      gpio_io_get_all  (gpio_io_state_t out[GPIO_IO_PIN_COUNT]);

esp_err_t gpio_io_set_power(bool on);
bool      gpio_io_get_power(void);

uint32_t  gpio_io_get_pulse_width_ms(void);
esp_err_t gpio_io_set_pulse_width_ms(uint32_t width_ms);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Create stub `components/gpio_io/gpio_io.c`**

```c
#include "gpio_io.h"

#include "esp_log.h"

static const char *TAG = "gpio_io";

esp_err_t gpio_io_init(void)            { ESP_LOGI(TAG, "gpio_io stub init"); return ESP_OK; }

esp_err_t gpio_io_set_mode (uint8_t idx, gpio_io_mode_t mode)  { (void)idx; (void)mode;  return ESP_ERR_NOT_SUPPORTED; }
esp_err_t gpio_io_set_level(uint8_t idx, bool level)           { (void)idx; (void)level; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t gpio_io_pulse    (uint8_t idx, uint32_t width_ms)    { (void)idx; (void)width_ms; return ESP_ERR_NOT_SUPPORTED; }

void gpio_io_get_state(uint8_t idx, gpio_io_state_t *out) { (void)idx; if (out) *out = (gpio_io_state_t){0}; }
void gpio_io_get_all  (gpio_io_state_t out[GPIO_IO_PIN_COUNT])
{
    if (!out) return;
    for (int i = 0; i < GPIO_IO_PIN_COUNT; i++) out[i] = (gpio_io_state_t){0};
}

esp_err_t gpio_io_set_power(bool on)         { (void)on;  return ESP_ERR_NOT_SUPPORTED; }
bool      gpio_io_get_power(void)            { return false; }

uint32_t  gpio_io_get_pulse_width_ms(void)              { return 100; }
esp_err_t gpio_io_set_pulse_width_ms(uint32_t width_ms) { (void)width_ms; return ESP_ERR_NOT_SUPPORTED; }
```

- [ ] **Step 4: Add `gpio_io` to `main/CMakeLists.txt`**

Find the existing `idf_component_register(...)` block and add `gpio_io` to its `REQUIRES` list.

```cmake
# main/CMakeLists.txt — add gpio_io to REQUIRES
idf_component_register(
    SRCS "app_main.c" "control_task.c"
    INCLUDE_DIRS "."
    REQUIRES app_api pwm_gen rpm_cap usb_composite net_dashboard ota_core nvs_flash
             driver console esp_console argtable3
             gpio_io
)
```

(Verify the existing REQUIRES list before editing; preserve any other entries already present.)

- [ ] **Step 5: Build to verify the empty component links**

Run: `idf.py build`
Expected: clean build, no link errors. The stub functions are unused so far.

- [ ] **Step 6: Commit**

```bash
git add components/gpio_io main/CMakeLists.txt
git commit -m "feat(gpio_io): scaffold component (stub API)"
```

---

## Task 2: Add Kconfig defaults for the 17 GPIO pins + pulse width + active-low flag

**Files:**
- Modify: `main/Kconfig.projbuild`

- [ ] **Step 1: Append new config entries inside the `Fan-TestKit App` menu**

Open `main/Kconfig.projbuild`. Before the closing `endmenu`, add:

```
    config APP_POWER_SWITCH_GPIO
        int "Power-switch GPIO"
        default 21

    config APP_POWER_SWITCH_ACTIVE_LOW
        bool "Power-switch is active-low (low-trigger relay module)"
        default y

    config APP_GPIO_GROUP_A_PIN_0
        int "Group A pin 0 GPIO"
        default 7
    config APP_GPIO_GROUP_A_PIN_1
        int "Group A pin 1 GPIO"
        default 15
    config APP_GPIO_GROUP_A_PIN_2
        int "Group A pin 2 GPIO"
        default 16
    config APP_GPIO_GROUP_A_PIN_3
        int "Group A pin 3 GPIO"
        default 17
    config APP_GPIO_GROUP_A_PIN_4
        int "Group A pin 4 GPIO"
        default 18
    config APP_GPIO_GROUP_A_PIN_5
        int "Group A pin 5 GPIO"
        default 8
    config APP_GPIO_GROUP_A_PIN_6
        int "Group A pin 6 GPIO"
        default 9
    config APP_GPIO_GROUP_A_PIN_7
        int "Group A pin 7 GPIO"
        default 10

    config APP_GPIO_GROUP_B_PIN_0
        int "Group B pin 0 GPIO"
        default 11
    config APP_GPIO_GROUP_B_PIN_1
        int "Group B pin 1 GPIO"
        default 12
    config APP_GPIO_GROUP_B_PIN_2
        int "Group B pin 2 GPIO"
        default 13
    config APP_GPIO_GROUP_B_PIN_3
        int "Group B pin 3 GPIO"
        default 14
    config APP_GPIO_GROUP_B_PIN_4
        int "Group B pin 4 GPIO"
        default 1
    config APP_GPIO_GROUP_B_PIN_5
        int "Group B pin 5 GPIO"
        default 2
    config APP_GPIO_GROUP_B_PIN_6
        int "Group B pin 6 GPIO"
        default 42
    config APP_GPIO_GROUP_B_PIN_7
        int "Group B pin 7 GPIO"
        default 41

    config APP_DEFAULT_PULSE_WIDTH_MS
        int "Default GPIO pulse width (ms) on first boot before NVS is set"
        default 100
        range 1 10000
```

- [ ] **Step 2: Force sdkconfig regeneration (CLAUDE.md sdkconfig trap)**

```
del sdkconfig
idf.py fullclean
idf.py build
```

Expected: clean build. Verify the new symbols took effect:

```
findstr CONFIG_APP_POWER_SWITCH_GPIO sdkconfig
findstr CONFIG_APP_GPIO_GROUP_A_PIN_0 sdkconfig
findstr CONFIG_APP_DEFAULT_PULSE_WIDTH_MS sdkconfig
```

Expected output:
```
CONFIG_APP_POWER_SWITCH_GPIO=21
CONFIG_APP_GPIO_GROUP_A_PIN_0=7
CONFIG_APP_DEFAULT_PULSE_WIDTH_MS=100
```

- [ ] **Step 3: Commit**

```bash
git add main/Kconfig.projbuild
git commit -m "feat(kconfig): add power-switch + 16 GPIO pin defaults"
```

---

## Task 3: Implement the 16-pin state table + boot-time mode application

**Files:**
- Modify: `components/gpio_io/gpio_io.c`

- [ ] **Step 1: Replace the stub with the real state table + init**

```c
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
#define MAKE_STATE(m,l,p) ((uint8_t)((m & 0x03) | ((l & 0x01) << 2) | ((p & 0x01) << 3)))

// power switch
static _Atomic uint8_t s_power;     // 0 = OFF, 1 = ON

// global pulse width (kept in RAM; persisted via NVS in a later task).
static _Atomic uint32_t s_pulse_width_ms;

// per-pin one-shot timers (allocated on first OUTPUT-mode set).
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

// ---- public API ------------------------------------------------------------

esp_err_t gpio_io_init(void)
{
    s_pulse_width_ms = CONFIG_APP_DEFAULT_PULSE_WIDTH_MS;
    s_power = 0;

    // Group A (idx 0..7) → input pull-down; Group B (idx 8..15) → output low.
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
        s_state[i] = MAKE_STATE(m, 0, 0);
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

// remaining stubs (set_mode/set_level/pulse/set_power/get_power) stay until
// the next tasks fill them in.
esp_err_t gpio_io_set_mode (uint8_t idx, gpio_io_mode_t mode)  { (void)idx; (void)mode;  return ESP_ERR_NOT_SUPPORTED; }
esp_err_t gpio_io_set_level(uint8_t idx, bool level)           { (void)idx; (void)level; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t gpio_io_pulse    (uint8_t idx, uint32_t width_ms)    { (void)idx; (void)width_ms; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t gpio_io_set_power(bool on)                            { (void)on; return ESP_ERR_NOT_SUPPORTED; }
bool      gpio_io_get_power(void)                               { return false; }
```

- [ ] **Step 2: Wire `gpio_io_init()` into `app_main`**

Open `main/app_main.c`. Add `#include "gpio_io.h"` near the existing `#include "ota_core.h"`. Add a call to `gpio_io_init()` just **after** `ESP_ERROR_CHECK(rpm_cap_init(&rpm_cfg));` and **before** `ESP_ERROR_CHECK(ota_core_init());`:

```c
    ESP_ERROR_CHECK(rpm_cap_init(&rpm_cfg));

    ESP_ERROR_CHECK(gpio_io_init());

    ESP_ERROR_CHECK(ota_core_init());
```

- [ ] **Step 3: Build + flash + verify boot logs**

```
idf.py build
idf.py -p COM24 flash monitor
```

Expected boot log lines (among others):
```
I (xxx) gpio_io: gpio_io ready: 8 inputs (pull-down), 8 outputs (low)
```

If a `gpio_config` fails for any pin (typically because the pin number doesn't exist on the variant), the boot will halt at `ESP_ERROR_CHECK` — fix the offending Kconfig pin and retry.

- [ ] **Step 4: Multimeter sanity check**

Probe each Group B pin (J1-17..J1-20, J2-3..J2-6) — all should read 0 V (low). Probe each Group A pin (J1-7..J1-16, omitting the 4/5/6/13/14 already used) — all should read 0 V (pull-down + no driver).

- [ ] **Step 5: Commit**

```bash
git add components/gpio_io/gpio_io.c main/app_main.c
git commit -m "feat(gpio_io): boot 16 pins to Kconfig defaults (A=input-PD, B=output-low)"
```

---

## Task 4: Implement `gpio_io_set_mode` (no pulsing concerns yet)

**Files:**
- Modify: `components/gpio_io/gpio_io.c`

- [ ] **Step 1: Replace `gpio_io_set_mode` stub with real implementation**

```c
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
```

- [ ] **Step 2: Build to verify**

Run: `idf.py build`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add components/gpio_io/gpio_io.c
git commit -m "feat(gpio_io): implement set_mode (rejects during pulse)"
```

---

## Task 5: Add the input-polling task (samples input pins @ 20 Hz)

**Files:**
- Modify: `components/gpio_io/gpio_io.c`

- [ ] **Step 1: Add a poll task and start it from `gpio_io_init`**

Insert above `gpio_io_init` (after the helper definitions):

```c
static void gpio_io_poll_task(void *arg)
{
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
            if (ns != s) atomic_store_explicit(&s_state[i], ns, memory_order_relaxed);
        }
    }
}
```

At the bottom of `gpio_io_init`, before `return ESP_OK;`, add:

```c
    BaseType_t ok = xTaskCreate(gpio_io_poll_task, "gpio_io_poll", 2048, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "poll task create failed");
        return ESP_ERR_NO_MEM;
    }
```

- [ ] **Step 2: Build, flash, verify**

```
idf.py build
idf.py -p COM24 flash monitor
```

Probe a Group A pin (e.g. GPIO7 / J1-7) with a 3.3 V jumper. After ≤ 50 ms the
pin's stored state will flip — visible by adding a temporary one-shot log
inside the loop OR (preferred) wait for Task 9 below where the `status` CLI
prints the table.

For now, just verify the boot still completes and the new task name appears
in `top` output if you run it manually.

- [ ] **Step 3: Commit**

```bash
git add components/gpio_io/gpio_io.c
git commit -m "feat(gpio_io): add 20 Hz input polling task"
```

---

## Task 6: Implement `gpio_io_set_level` for output pins

**Files:**
- Modify: `components/gpio_io/gpio_io.c`

- [ ] **Step 1: Replace `gpio_io_set_level` stub**

```c
esp_err_t gpio_io_set_level(uint8_t idx, bool level)
{
    if (idx >= GPIO_IO_PIN_COUNT) return ESP_ERR_INVALID_ARG;
    uint8_t s = atomic_load_explicit(&s_state[idx], memory_order_relaxed);
    if (STATE_MODE(s) != GPIO_IO_MODE_OUTPUT) return ESP_ERR_INVALID_STATE;
    if (STATE_PULSING(s))                     return ESP_ERR_INVALID_STATE;

    gpio_set_level(s_pins[idx], level ? 1 : 0);
    atomic_store_explicit(&s_state[idx],
                          MAKE_STATE(GPIO_IO_MODE_OUTPUT, level ? 1 : 0, 0),
                          memory_order_relaxed);
    return ESP_OK;
}
```

- [ ] **Step 2: Build to verify**

Run: `idf.py build`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add components/gpio_io/gpio_io.c
git commit -m "feat(gpio_io): implement set_level (output mode only)"
```

---

## Task 7: Implement `gpio_io_pulse` with esp_timer one-shot

**Files:**
- Modify: `components/gpio_io/gpio_io.c`

- [ ] **Step 1: Add the timer callback and pulse implementation**

Above `gpio_io_init` (or near the top after the helpers), add:

```c
static void pulse_done_cb(void *arg)
{
    uintptr_t idx_u = (uintptr_t)arg;
    uint8_t idx = (uint8_t)idx_u;
    if (idx >= GPIO_IO_PIN_COUNT) return;

    uint8_t s = atomic_load_explicit(&s_state[idx], memory_order_relaxed);
    if (STATE_MODE(s) != GPIO_IO_MODE_OUTPUT) return;

    bool restore = STATE_LEVEL(s);    // restore to the pre-pulse driven level
    gpio_set_level(s_pins[idx], restore ? 1 : 0);
    atomic_store_explicit(&s_state[idx],
                          MAKE_STATE(GPIO_IO_MODE_OUTPUT, restore ? 1 : 0, 0),
                          memory_order_relaxed);
}
```

Replace the `gpio_io_pulse` stub:

```c
esp_err_t gpio_io_pulse(uint8_t idx, uint32_t width_ms)
{
    if (idx >= GPIO_IO_PIN_COUNT) return ESP_ERR_INVALID_ARG;

    uint8_t s = atomic_load_explicit(&s_state[idx], memory_order_relaxed);
    if (STATE_MODE(s) != GPIO_IO_MODE_OUTPUT) return ESP_ERR_INVALID_STATE;
    if (STATE_PULSING(s))                     return ESP_ERR_INVALID_STATE;

    if (width_ms < 1)     width_ms = 1;
    if (width_ms > 10000) width_ms = 10000;

    if (!s_pulse_timer[idx]) {
        const esp_timer_create_args_t args = {
            .callback        = pulse_done_cb,
            .arg             = (void *)(uintptr_t)idx,
            .dispatch_method = ESP_TIMER_TASK,
            .name            = "gpio_io_pulse",
        };
        esp_err_t ce = esp_timer_create(&args, &s_pulse_timer[idx]);
        if (ce != ESP_OK) return ce;
    }

    bool idle  = STATE_LEVEL(s);
    bool burst = !idle;
    gpio_set_level(s_pins[idx], burst ? 1 : 0);
    // Mark pulsing BEFORE arming the timer so the callback can never see
    // pulsing=false (which would short-circuit the restore).
    atomic_store_explicit(&s_state[idx],
                          MAKE_STATE(GPIO_IO_MODE_OUTPUT, idle ? 1 : 0, 1),
                          memory_order_relaxed);

    esp_err_t te = esp_timer_start_once(s_pulse_timer[idx], (uint64_t)width_ms * 1000);
    if (te != ESP_OK) {
        // Synchronous restore on the failure path so the pin is never left
        // mid-pulse and the next telemetry frame shows pulsing=false.
        gpio_set_level(s_pins[idx], idle ? 1 : 0);
        atomic_store_explicit(&s_state[idx],
                              MAKE_STATE(GPIO_IO_MODE_OUTPUT, idle ? 1 : 0, 0),
                              memory_order_relaxed);
        return te;
    }
    return ESP_OK;
}
```

- [ ] **Step 2: Build**

Run: `idf.py build`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add components/gpio_io/gpio_io.c
git commit -m "feat(gpio_io): implement one-shot idle-inverted pulse via esp_timer"
```

---

## Task 8: Implement the power switch (active-low aware)

**Files:**
- Modify: `components/gpio_io/gpio_io.c`

- [ ] **Step 1: Replace the power-switch stubs**

Add a helper near the top of the file:

```c
static int power_pin_level_for(bool on)
{
#if CONFIG_APP_POWER_SWITCH_ACTIVE_LOW
    return on ? 0 : 1;
#else
    return on ? 1 : 0;
#endif
}
```

In `gpio_io_init`, just after the for-loop that initialises the 16 GPIO pins,
configure the power-switch pin and drive it OFF:

```c
    {
        gpio_config_t cfg = {
            .pin_bit_mask    = 1ULL << CONFIG_APP_POWER_SWITCH_GPIO,
            .mode            = GPIO_MODE_OUTPUT,
            .pull_up_en      = GPIO_PULLUP_DISABLE,
            .pull_down_en    = GPIO_PULLDOWN_DISABLE,
            .intr_type       = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&cfg);
        if (e != ESP_OK) return e;
        gpio_set_level(CONFIG_APP_POWER_SWITCH_GPIO, power_pin_level_for(false));
        atomic_store_explicit(&s_power, 0, memory_order_relaxed);
    }
```

Replace the two stubs:

```c
esp_err_t gpio_io_set_power(bool on)
{
    gpio_set_level(CONFIG_APP_POWER_SWITCH_GPIO, power_pin_level_for(on));
    atomic_store_explicit(&s_power, on ? 1 : 0, memory_order_relaxed);
    ESP_LOGI(TAG, "power switch %s", on ? "ON" : "OFF");
    return ESP_OK;
}

bool gpio_io_get_power(void)
{
    return atomic_load_explicit(&s_power, memory_order_relaxed) != 0;
}
```

- [ ] **Step 2: Build, flash, verify with multimeter**

```
idf.py build
idf.py -p COM24 flash monitor
```

Multimeter on GPIO21 (J2-17). With `APP_POWER_SWITCH_ACTIVE_LOW=y` (default),
expect 3.3 V on boot (OFF → high). To verify the active-low *meaning* (rather
than just the wire), wait until Task 9's CLI is in place; for now just
confirm the pin reads 3.3 V at boot.

- [ ] **Step 3: Commit**

```bash
git add components/gpio_io/gpio_io.c
git commit -m "feat(gpio_io): power switch with build-time active-low polarity"
```

---

## Task 9: Extend `app_api.h` with new ctrl_cmd kinds

**Files:**
- Modify: `components/app_api/include/app_api.h`

- [ ] **Step 1: Add 5 new enum values + 5 new union members**

Open `components/app_api/include/app_api.h`. Replace the `ctrl_cmd_kind_t` enum and the `ctrl_cmd_t` struct's union with:

```c
typedef enum {
    CTRL_CMD_SET_PWM,
    CTRL_CMD_SET_RPM_PARAMS,
    CTRL_CMD_SET_RPM_TIMEOUT,
    CTRL_CMD_OTA_BEGIN,
    CTRL_CMD_OTA_CHUNK,
    CTRL_CMD_OTA_END,
    CTRL_CMD_GPIO_SET_MODE,
    CTRL_CMD_GPIO_SET_LEVEL,
    CTRL_CMD_GPIO_PULSE,
    CTRL_CMD_POWER_SET,
    CTRL_CMD_PULSE_WIDTH_SET,
} ctrl_cmd_kind_t;

typedef struct {
    ctrl_cmd_kind_t kind;
    union {
        struct { uint32_t freq_hz; float duty_pct; }   set_pwm;
        struct { uint8_t pole; uint16_t mavg; }        set_rpm_params;
        struct { uint32_t timeout_us; }                set_rpm_timeout;
        struct { uint8_t  idx; uint8_t  mode; }        gpio_set_mode;
        struct { uint8_t  idx; uint8_t  level; }       gpio_set_level;
        struct { uint8_t  idx; uint32_t width_ms; }    gpio_pulse;
        struct { uint8_t  on; }                        power_set;
        struct { uint32_t width_ms; }                  pulse_width_set;
    };
} ctrl_cmd_t;
```

- [ ] **Step 2: Build to verify the existing code still compiles**

Run: `idf.py build`
Expected: clean build (existing usages still match).

- [ ] **Step 3: Commit**

```bash
git add components/app_api/include/app_api.h
git commit -m "feat(app_api): add GPIO + power + pulse-width ctrl_cmd kinds"
```

---

## Task 10: Wire ctrl_cmds into `control_task` + add CLI commands

**Files:**
- Modify: `main/control_task.c`
- Modify: `main/app_main.c`

- [ ] **Step 1: Handle the 5 new kinds in `control_task.c`**

Open `main/control_task.c`. Add the include near the top:

```c
#include "gpio_io.h"
```

In the switch inside `control_task`, add 5 new cases before the closing brace:

```c
        case CTRL_CMD_GPIO_SET_MODE:
            gpio_io_set_mode(cmd.gpio_set_mode.idx,
                             (gpio_io_mode_t)cmd.gpio_set_mode.mode);
            break;
        case CTRL_CMD_GPIO_SET_LEVEL:
            gpio_io_set_level(cmd.gpio_set_level.idx,
                              cmd.gpio_set_level.level != 0);
            break;
        case CTRL_CMD_GPIO_PULSE:
            gpio_io_pulse(cmd.gpio_pulse.idx, cmd.gpio_pulse.width_ms);
            break;
        case CTRL_CMD_POWER_SET:
            gpio_io_set_power(cmd.power_set.on != 0);
            break;
        case CTRL_CMD_PULSE_WIDTH_SET:
            gpio_io_set_pulse_width_ms(cmd.pulse_width_set.width_ms);
            break;
```

- [ ] **Step 2: Add CLI commands in `app_main.c`**

Open `main/app_main.c`. Add `#include "gpio_io.h"` near the existing includes (idempotent if already added in Task 3).

Add command argtables and handlers near the existing CLI functions (after `cmd_status` is fine):

```c
// ---- CLI: gpio_mode <idx> <mode> -------------------------------------------

static struct {
    struct arg_int *idx;
    struct arg_str *mode;
    struct arg_end *end;
} s_gpio_mode_args;

static int cmd_gpio_mode(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_gpio_mode_args);
    if (n != 0) { arg_print_errors(stderr, s_gpio_mode_args.end, argv[0]); return 1; }
    const char *m = s_gpio_mode_args.mode->sval[0];
    uint8_t mode;
    if      (strcmp(m, "i_pd") == 0) mode = 0;
    else if (strcmp(m, "i_pu") == 0) mode = 1;
    else if (strcmp(m, "i_fl") == 0) mode = 2;
    else if (strcmp(m, "o")    == 0) mode = 3;
    else { printf("mode must be i_pd | i_pu | i_fl | o\n"); return 1; }
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_GPIO_SET_MODE,
        .gpio_set_mode = { .idx = (uint8_t)s_gpio_mode_args.idx->ival[0], .mode = mode },
    };
    return control_task_post(&c, pdMS_TO_TICKS(100)) == ESP_OK ? 0 : 1;
}

// ---- CLI: gpio_set <idx> <0|1> ---------------------------------------------

static struct {
    struct arg_int *idx;
    struct arg_int *level;
    struct arg_end *end;
} s_gpio_set_args;

static int cmd_gpio_set(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_gpio_set_args);
    if (n != 0) { arg_print_errors(stderr, s_gpio_set_args.end, argv[0]); return 1; }
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_GPIO_SET_LEVEL,
        .gpio_set_level = {
            .idx   = (uint8_t)s_gpio_set_args.idx->ival[0],
            .level = (uint8_t)(s_gpio_set_args.level->ival[0] ? 1 : 0),
        },
    };
    return control_task_post(&c, pdMS_TO_TICKS(100)) == ESP_OK ? 0 : 1;
}

// ---- CLI: gpio_pulse <idx> [width_ms] --------------------------------------

static struct {
    struct arg_int *idx;
    struct arg_int *width;
    struct arg_end *end;
} s_gpio_pulse_args;

static int cmd_gpio_pulse(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_gpio_pulse_args);
    if (n != 0) { arg_print_errors(stderr, s_gpio_pulse_args.end, argv[0]); return 1; }
    uint32_t w = (s_gpio_pulse_args.width->count > 0)
                 ? (uint32_t)s_gpio_pulse_args.width->ival[0]
                 : gpio_io_get_pulse_width_ms();
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_GPIO_PULSE,
        .gpio_pulse = { .idx = (uint8_t)s_gpio_pulse_args.idx->ival[0], .width_ms = w },
    };
    return control_task_post(&c, pdMS_TO_TICKS(100)) == ESP_OK ? 0 : 1;
}

// ---- CLI: power <0|1> ------------------------------------------------------

static struct {
    struct arg_int *on;
    struct arg_end *end;
} s_power_args;

static int cmd_power(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_power_args);
    if (n != 0) { arg_print_errors(stderr, s_power_args.end, argv[0]); return 1; }
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_POWER_SET,
        .power_set = { .on = (uint8_t)(s_power_args.on->ival[0] ? 1 : 0) },
    };
    return control_task_post(&c, pdMS_TO_TICKS(100)) == ESP_OK ? 0 : 1;
}
```

Extend `cmd_status` to print GPIO + power state:

```c
static int cmd_status(int argc, char **argv)
{
    uint32_t f; float d;
    control_task_get_pwm(&f, &d);
    float rpm = rpm_cap_get_latest();
    printf("pwm  freq=%lu Hz  duty=%.2f %%  (duty resolution %u bits)\n",
           (unsigned long)f, d, pwm_gen_duty_resolution_bits(f));
    printf("rpm  latest=%.2f\n", rpm);

    static const char *mode_str[] = { "i_pd", "i_pu", "i_fl", "o" };
    gpio_io_state_t st[GPIO_IO_PIN_COUNT];
    gpio_io_get_all(st);
    printf("power %s\n", gpio_io_get_power() ? "on" : "off");
    printf("gpio  ");
    for (int i = 0; i < GPIO_IO_PIN_COUNT; i++) {
        const char *grp = (i < 8) ? "A" : "B";
        int slot = (i < 8) ? (i + 1) : (i - 7);
        printf("%s%d=%s:%d%s ", grp, slot, mode_str[st[i].mode],
               st[i].level ? 1 : 0, st[i].pulsing ? "*" : "");
    }
    printf("(pulse_width=%lums)\n", (unsigned long)gpio_io_get_pulse_width_ms());
    return 0;
}
```

In `register_commands()`, register the 4 new commands. Place them after the `rpm_timeout` registration:

```c
    s_gpio_mode_args.idx  = arg_int1(NULL, NULL, "<idx>",  "GPIO index 0..15");
    s_gpio_mode_args.mode = arg_str1(NULL, NULL, "<mode>", "i_pd|i_pu|i_fl|o");
    s_gpio_mode_args.end  = arg_end(2);
    const esp_console_cmd_t gm_cmd = { .command = "gpio_mode", .help = "set GPIO mode",
        .hint = NULL, .func = cmd_gpio_mode, .argtable = &s_gpio_mode_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&gm_cmd));

    s_gpio_set_args.idx   = arg_int1(NULL, NULL, "<idx>",   "GPIO index 0..15");
    s_gpio_set_args.level = arg_int1(NULL, NULL, "<level>", "0 or 1");
    s_gpio_set_args.end   = arg_end(2);
    const esp_console_cmd_t gs_cmd = { .command = "gpio_set", .help = "set GPIO output level",
        .hint = NULL, .func = cmd_gpio_set, .argtable = &s_gpio_set_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&gs_cmd));

    s_gpio_pulse_args.idx   = arg_int1(NULL, NULL, "<idx>",          "GPIO index 0..15");
    s_gpio_pulse_args.width = arg_int0(NULL, NULL, "[width_ms]",     "pulse width override (default global)");
    s_gpio_pulse_args.end   = arg_end(2);
    const esp_console_cmd_t gp_cmd = { .command = "gpio_pulse", .help = "one-shot idle-inverted pulse",
        .hint = NULL, .func = cmd_gpio_pulse, .argtable = &s_gpio_pulse_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&gp_cmd));

    s_power_args.on  = arg_int1(NULL, NULL, "<on>", "1 = ON, 0 = OFF");
    s_power_args.end = arg_end(1);
    const esp_console_cmd_t pw_cmd = { .command = "power", .help = "power switch ON/OFF",
        .hint = NULL, .func = cmd_power, .argtable = &s_power_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pw_cmd));
```

Also add the boot-default queue post (just after the existing PWM boot-default block):

```c
    {
        ctrl_cmd_t boot_pwr = {
            .kind = CTRL_CMD_POWER_SET,
            .power_set = { .on = 0 },
        };
        control_task_post(&boot_pwr, pdMS_TO_TICKS(100));
    }
```

- [ ] **Step 2: Build, flash, verify on UART**

```
idf.py build
idf.py -p COM24 flash monitor
```

Wait for `fan-testkit>` prompt. Run:

```
status
```

Expected output (one line per category):
```
pwm  freq=10000 Hz  duty=0.00 %  (duty resolution X bits)
rpm  latest=0.00
power off
gpio  A1=i_pd:0 A2=i_pd:0 A3=i_pd:0 ... B8=o:0 (pulse_width=100ms)
```

Then exercise:

```
gpio_mode 0 o          # A1 → output
gpio_set 0 1           # drive A1 high
status                 # A1 should now show o:1
gpio_pulse 0 200       # 200 ms low pulse (idle high → low → high)
status                 # while pulsing, will show o:1* briefly
power 1                # multimeter on GPIO21 → 0 V (active-low ON)
power 0                # → 3.3 V
gpio_mode 0 i_pd       # A1 back to input
```

For each, scope or DMM-confirm the GPIO pin reflects the change.

- [ ] **Step 3: Commit**

```bash
git add main/control_task.c main/app_main.c
git commit -m "feat(cli): wire gpio + power ctrl_cmds + CLI commands + status output"
```

---

## Task 11: Extend WebSocket handler — accept new client→device messages

**Files:**
- Modify: `components/net_dashboard/ws_handler.c`
- Modify: `components/net_dashboard/CMakeLists.txt`

- [ ] **Step 1: Add `gpio_io` to net_dashboard's REQUIRES**

Open `components/net_dashboard/CMakeLists.txt`. Add `gpio_io` to the existing `REQUIRES` list (right after `rpm_cap` is fine).

```cmake
    REQUIRES    esp_http_server
                esp_wifi
                esp_netif
                esp_event
                nvs_flash
                espressif__cjson
                app_api
                pwm_gen
                rpm_cap
                gpio_io
                ota_core
                esp_driver_gpio
                lwip
                espressif__mdns
```

- [ ] **Step 2: Extend `handle_json` in `ws_handler.c`**

Open `components/net_dashboard/ws_handler.c`. Add the include near the top:

```c
#include "gpio_io.h"
```

Inside `handle_json`, add new branches before the existing `factory_reset` branch:

```c
    } else if (strcmp(type_j->valuestring, "set_gpio_mode") == 0) {
        const cJSON *idx  = cJSON_GetObjectItem(root, "idx");
        const cJSON *mode = cJSON_GetObjectItem(root, "mode");
        if (!cJSON_IsNumber(idx) || !cJSON_IsString(mode)) return;
        const char *m = mode->valuestring;
        uint8_t mv;
        if      (strcmp(m, "input_pulldown") == 0) mv = 0;
        else if (strcmp(m, "input_pullup")   == 0) mv = 1;
        else if (strcmp(m, "input_floating") == 0) mv = 2;
        else if (strcmp(m, "output")         == 0) mv = 3;
        else return;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_SET_MODE,
            .gpio_set_mode = { .idx = (uint8_t)idx->valuedouble, .mode = mv },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_gpio_level") == 0) {
        const cJSON *idx   = cJSON_GetObjectItem(root, "idx");
        const cJSON *level = cJSON_GetObjectItem(root, "level");
        if (!cJSON_IsNumber(idx) || !cJSON_IsNumber(level)) return;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_SET_LEVEL,
            .gpio_set_level = {
                .idx   = (uint8_t)idx->valuedouble,
                .level = (level->valuedouble != 0) ? 1u : 0u,
            },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "pulse_gpio") == 0) {
        const cJSON *idx = cJSON_GetObjectItem(root, "idx");
        const cJSON *w   = cJSON_GetObjectItem(root, "width_ms");
        if (!cJSON_IsNumber(idx)) return;
        uint32_t width = cJSON_IsNumber(w) ? (uint32_t)w->valuedouble
                                           : gpio_io_get_pulse_width_ms();
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_PULSE,
            .gpio_pulse = { .idx = (uint8_t)idx->valuedouble, .width_ms = width },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_power") == 0) {
        const cJSON *on = cJSON_GetObjectItem(root, "on");
        if (!cJSON_IsBool(on) && !cJSON_IsNumber(on)) return;
        bool b = cJSON_IsBool(on) ? cJSON_IsTrue(on) : (on->valuedouble != 0);
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_POWER_SET,
            .power_set = { .on = b ? 1u : 0u },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_pulse_width") == 0) {
        const cJSON *w = cJSON_GetObjectItem(root, "width_ms");
        if (!cJSON_IsNumber(w)) return;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_PULSE_WIDTH_SET,
            .pulse_width_set = { .width_ms = (uint32_t)w->valuedouble },
        };
        control_task_post(&c, 0);
```

(The existing `} else if (strcmp(type_j->valuestring, "factory_reset") ...` follows, unchanged.)

- [ ] **Step 3: Build**

Run: `idf.py build`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add components/net_dashboard/ws_handler.c components/net_dashboard/CMakeLists.txt
git commit -m "feat(ws): accept set_gpio_mode/level, pulse_gpio, set_power, set_pulse_width"
```

---

## Task 12: Extend telemetry frame — add `gpio[]`, `power`, `pulse_width_ms`

**Files:**
- Modify: `components/net_dashboard/ws_handler.c`

- [ ] **Step 1: Replace the telemetry-task body**

Find the existing `telemetry_task` in `ws_handler.c`. Replace the inner loop body:

```c
static void telemetry_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(50);  // 20 Hz to the browser
    char payload[768];
    static const char *mode_short[] = { "i_pd", "i_pu", "i_fl", "o" };
    while (true) {
        vTaskDelayUntil(&last, period);
        if (!s_httpd_for_telemetry) continue;

        uint32_t f; float d;
        control_task_get_pwm(&f, &d);
        float rpm = rpm_cap_get_latest();
        gpio_io_state_t st[GPIO_IO_PIN_COUNT];
        gpio_io_get_all(st);
        bool power = gpio_io_get_power();
        uint32_t pw = gpio_io_get_pulse_width_ms();

        int64_t ts = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;

        int n = snprintf(payload, sizeof(payload),
                "{\"type\":\"status\",\"freq\":%lu,\"duty\":%.2f,\"rpm\":%.2f,\"ts\":%" PRId64
                ",\"power\":%d,\"pulse_width_ms\":%lu,\"gpio\":[",
                (unsigned long)f, d, rpm, ts, power ? 1 : 0, (unsigned long)pw);

        for (int i = 0; i < GPIO_IO_PIN_COUNT && n < (int)sizeof(payload); i++) {
            n += snprintf(payload + n, sizeof(payload) - n,
                          "%s{\"m\":\"%s\",\"v\":%d,\"p\":%d}",
                          (i == 0) ? "" : ",",
                          mode_short[st[i].mode],
                          st[i].level ? 1 : 0,
                          st[i].pulsing ? 1 : 0);
        }
        if (n < (int)sizeof(payload)) n += snprintf(payload + n, sizeof(payload) - n, "]}");

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_client_fds[i] != 0) ws_send_json_to(s_client_fds[i], payload);
        }
    }
}
```

- [ ] **Step 2: Build**

Run: `idf.py build`
Expected: clean build (no new includes needed; `gpio_io.h` was added in Task 11).

- [ ] **Step 3: Manual verify (browser DevTools)**

Flash, open `http://fan-testkit.local`, open DevTools → Network → WS → frames. Each `status` frame should now look like:

```json
{
  "type":"status",
  "freq":10000, "duty":0.00, "rpm":0.00, "ts":12345,
  "power":0, "pulse_width_ms":100,
  "gpio":[{"m":"i_pd","v":0,"p":0}, ... 16 entries ...]
}
```

- [ ] **Step 4: Commit**

```bash
git add components/net_dashboard/ws_handler.c
git commit -m "feat(ws): include gpio[], power, pulse_width_ms in 20 Hz telemetry"
```

---

## Task 13: Extend `device_info` JSON with the 17 new GPIO pin numbers

**Files:**
- Modify: `components/net_dashboard/net_dashboard.c`

- [ ] **Step 1: Append GPIO pin numbers to `device_info_get`**

Open `components/net_dashboard/net_dashboard.c`. In `device_info_get`, after the existing `cJSON_AddNumberToObject(pins, "status_led", CONFIG_APP_STATUS_LED_GPIO);`, add:

```c
    cJSON_AddNumberToObject(pins, "power_switch", CONFIG_APP_POWER_SWITCH_GPIO);
    cJSON *gpio_a = cJSON_AddArrayToObject(pins, "group_a");
    cJSON *gpio_b = cJSON_AddArrayToObject(pins, "group_b");
    if (gpio_a) {
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_0));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_1));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_2));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_3));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_4));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_5));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_6));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_7));
    }
    if (gpio_b) {
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_0));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_1));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_2));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_3));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_4));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_5));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_6));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_7));
    }
```

- [ ] **Step 2: Build, flash**

```
idf.py build
idf.py -p COM24 flash
```

Open `http://fan-testkit.local/api/device_info`. Verify response contains:

```json
{
  "pins":{
    "pwm":4,"trigger":5,"rpm":6,"status_led":48,
    "power_switch":21,
    "group_a":[7,15,16,17,18,8,9,10],
    "group_b":[11,12,13,14,1,2,42,41]
  },
  ...
}
```

- [ ] **Step 3: Commit**

```bash
git add components/net_dashboard/net_dashboard.c
git commit -m "feat(device_info): expose power_switch + group_a/group_b GPIO pin numbers"
```

---

## Task 14: Dashboard HTML — add Power panel, GPIO panel, Settings pulse-width

**Files:**
- Modify: `components/net_dashboard/web/index.html`

- [ ] **Step 1: Add the Power panel and the GPIO panel**

Open `components/net_dashboard/web/index.html`. Locate the closing `</section>` of the **Frequency panel** (the panel with `id="freq-panel"`). Insert these two new sections right after it, before the existing OTA panel:

```html
  <!-- ============== Power switch panel ============== -->
  <section class="panel power-panel">
    <h2 data-i18n="power_switch">Power Switch</h2>
    <div class="row">
      <span class="label" data-i18n="power_state">State</span>
      <button id="power_btn" class="big-toggle" type="button" data-on="0">
        <span class="ico">⏻</span>
        <span class="txt" data-i18n="power_off">OFF</span>
      </button>
    </div>
  </section>

  <!-- ============== GPIO panel ============== -->
  <section class="panel gpio-panel">
    <h2 data-i18n="gpio">GPIO</h2>

    <div class="gpio-group">
      <h3 data-i18n="group_a">Group A</h3>
      <div class="gpio-rows" id="gpio-group-a"></div>
    </div>

    <div class="gpio-group">
      <h3 data-i18n="group_b">Group B</h3>
      <div class="gpio-rows" id="gpio-group-b"></div>
    </div>
  </section>
```

- [ ] **Step 2: Add the Settings pulse-width section**

Inside the existing Settings `<details>` block, after the `Step sizes` section's closing `</div>`, append:

```html
    <div class="settings-section">
      <h3 data-i18n="gpio_output">GPIO output</h3>
      <div class="settings-grid">
        <label for="pulse-width-ms" data-i18n="pulse_width_label">Pulse width (ms)</label>
        <input id="pulse-width-ms" type="number" min="1" max="10000" value="100" />
      </div>
      <button id="apply_pulse_width" class="btn-primary" data-i18n="apply_pulse_width">Apply</button>
    </div>
```

- [ ] **Step 3: Add GPIO/power references to the Help panel**

In the Help panel's `<ul id="help-pins">`, add at the bottom:

```html
      <li><span data-i18n="help_pin_power">Power-switch:</span> GPIO<span data-pin="power_switch">?</span></li>
      <li><span data-i18n="help_pin_group_a">GPIO Group A:</span> <span data-pin-list="group_a">?</span></li>
      <li><span data-i18n="help_pin_group_b">GPIO Group B:</span> <span data-pin-list="group_b">?</span></li>
```

After the existing `<h3 data-i18n="help_reset_h">Factory Reset</h3>` heading and its paragraph, add a new help section:

```html
    <h3 data-i18n="help_gpio_h">GPIO &amp; Power</h3>
    <p data-i18n="help_gpio_p">Group A defaults to input (pull-down) and Group B defaults to output (low). Each pin can be flipped between input and output at runtime; outputs support level toggle and a one-shot pulse of configurable width. Power switch toggles a GPIO whose polarity (active-high / active-low) is set at build time via Kconfig.</p>
```

- [ ] **Step 4: Verify HTML is valid**

`idf.py build` (the EMBED_TXTFILES will pick up the changed HTML). Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add components/net_dashboard/web/index.html
git commit -m "feat(dashboard): power + GPIO panel skeletons + settings pulse-width row"
```

---

## Task 15: Dashboard JS — i18n strings, render rows, telemetry handling

**Files:**
- Modify: `components/net_dashboard/web/app.js`

- [ ] **Step 1: Add new i18n keys to all three languages**

Open `components/net_dashboard/web/app.js`. In the `I18N` object, add these keys to **each** of `en`, `zh-Hant`, `zh-Hans`:

```js
// inside I18N.en:
      power_switch: 'Power Switch',
      power_state: 'State',
      power_on: 'ON',
      power_off: 'OFF',
      gpio: 'GPIO',
      group_a: 'Group A',
      group_b: 'Group B',
      gpio_input_pulldown: 'input + pull-down',
      gpio_input_pullup: 'input + pull-up',
      gpio_input_floating: 'input + floating',
      gpio_output: 'output',
      gpio_pulse_btn: 'Pulse',
      gpio_pulsing: 'Pulsing…',
      gpio_value_label: 'value:',
      gpio_output_section: 'GPIO output',
      pulse_width_label: 'Pulse width (ms)',
      apply_pulse_width: 'Apply',
      help_pin_power: 'Power-switch:',
      help_pin_group_a: 'GPIO Group A:',
      help_pin_group_b: 'GPIO Group B:',
      help_gpio_h: 'GPIO & Power',
      help_gpio_p: 'Group A defaults to input (pull-down) and Group B defaults to output (low). Each pin can be flipped between input and output at runtime; outputs support level toggle and a one-shot pulse of configurable width. Power switch toggles a GPIO whose polarity (active-high / active-low) is set at build time via Kconfig.',
```

```js
// inside I18N['zh-Hant']:
      power_switch: '電源開關',
      power_state: '狀態',
      power_on: '開',
      power_off: '關',
      gpio: 'GPIO',
      group_a: 'A 組',
      group_b: 'B 組',
      gpio_input_pulldown: '輸入 + 下拉',
      gpio_input_pullup: '輸入 + 上拉',
      gpio_input_floating: '輸入 + 浮接',
      gpio_output: '輸出',
      gpio_pulse_btn: '脈衝',
      gpio_pulsing: '脈衝中…',
      gpio_value_label: '值：',
      gpio_output_section: 'GPIO 輸出',
      pulse_width_label: '脈衝寬度 (ms)',
      apply_pulse_width: '套用',
      help_pin_power: '電源開關：',
      help_pin_group_a: 'GPIO A 組：',
      help_pin_group_b: 'GPIO B 組：',
      help_gpio_h: 'GPIO 與電源',
      help_gpio_p: 'A 組預設為輸入（下拉），B 組預設為輸出（低）。每隻 pin 可在執行時切換輸入 / 輸出；輸出模式支援切換準位以及單發脈衝（脈衝寬度可設）。電源開關控制一隻 GPIO，其 active-high / active-low 由 Kconfig 編譯時決定。',
```

```js
// inside I18N['zh-Hans']:
      power_switch: '电源开关',
      power_state: '状态',
      power_on: '开',
      power_off: '关',
      gpio: 'GPIO',
      group_a: 'A 组',
      group_b: 'B 组',
      gpio_input_pulldown: '输入 + 下拉',
      gpio_input_pullup: '输入 + 上拉',
      gpio_input_floating: '输入 + 浮接',
      gpio_output: '输出',
      gpio_pulse_btn: '脉冲',
      gpio_pulsing: '脉冲中…',
      gpio_value_label: '值：',
      gpio_output_section: 'GPIO 输出',
      pulse_width_label: '脉冲宽度 (ms)',
      apply_pulse_width: '应用',
      help_pin_power: '电源开关：',
      help_pin_group_a: 'GPIO A 组：',
      help_pin_group_b: 'GPIO B 组：',
      help_gpio_h: 'GPIO 与电源',
      help_gpio_p: 'A 组默认为输入（下拉），B 组默认为输出（低）。每个 pin 可在运行时切换输入 / 输出；输出模式支持切换电平以及单发脉冲（脉冲宽度可设）。电源开关控制一个 GPIO，其 active-high / active-low 由 Kconfig 编译时决定。',
```

- [ ] **Step 2: Render GPIO rows after `loadDeviceInfo` resolves**

Inside `loadDeviceInfo`, after the existing `data-pin` block, add:

```js
      // power-switch pin number
      const powerPinEl = document.querySelector('[data-pin="power_switch"]');
      if (powerPinEl) {
        const v = info.pins ? info.pins.power_switch : undefined;
        powerPinEl.textContent = (v ?? '?').toString();
      }
      // group A / B pin lists rendered as comma-separated
      document.querySelectorAll('[data-pin-list]').forEach(el => {
        const arr = info.pins ? info.pins[el.dataset.pinList] : null;
        el.textContent = Array.isArray(arr) ? arr.map(n => `GPIO${n}`).join(', ') : '?';
      });
      // build GPIO rows (need group_a / group_b pin numbers from device_info)
      if (info.pins) buildGpioRows(info.pins.group_a || [], info.pins.group_b || []);
```

After the existing `setRpmFromDevice` function, add this builder + GPIO state plumbing:

```js
  // ---------- GPIO panel ----------
  const MODE_LABELS = ['gpio_input_pulldown','gpio_input_pullup','gpio_input_floating','gpio_output'];
  const MODE_SHORT_TO_INT = { i_pd:0, i_pu:1, i_fl:2, o:3 };
  const MODE_INT_TO_WIRE = ['input_pulldown','input_pullup','input_floating','output'];

  function buildGpioRows(groupAPins, groupBPins) {
    const groupARoot = document.getElementById('gpio-group-a');
    const groupBRoot = document.getElementById('gpio-group-b');
    if (!groupARoot || !groupBRoot) return;
    [...groupAPins, ...groupBPins].forEach((gpioNum, idx) => {
      const grp = idx < 8 ? 'A' : 'B';
      const slot = idx < 8 ? (idx + 1) : (idx - 7);
      const row = document.createElement('div');
      row.className = 'gpio-row';
      row.dataset.idx = String(idx);
      row.innerHTML = `
        <span class="gpio-name">${grp}${slot}</span>
        <span class="gpio-pinnum">GPIO${gpioNum}</span>
        <select class="gpio-mode">
          <option value="input_pulldown" data-i18n="gpio_input_pulldown">input + pull-down</option>
          <option value="input_pullup"   data-i18n="gpio_input_pullup">input + pull-up</option>
          <option value="input_floating" data-i18n="gpio_input_floating">input + floating</option>
          <option value="output"         data-i18n="gpio_output">output</option>
        </select>
        <span class="gpio-tail">
          <span class="gpio-input-tail">
            <span data-i18n="gpio_value_label">value:</span>
            <span class="gpio-value">0</span>
          </span>
          <span class="gpio-output-tail" hidden>
            <label class="gpio-toggle">
              <input type="checkbox" class="gpio-level" />
              <span class="gpio-level-text">0</span>
            </label>
            <button type="button" class="gpio-pulse" data-i18n="gpio_pulse_btn">Pulse</button>
          </span>
        </span>
      `;
      ((idx < 8) ? groupARoot : groupBRoot).appendChild(row);

      const modeSel = row.querySelector('.gpio-mode');
      modeSel.addEventListener('change', () => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: 'set_gpio_mode', idx, mode: modeSel.value }));
        }
      });

      const levelEl = row.querySelector('.gpio-level');
      levelEl.addEventListener('change', () => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: 'set_gpio_level', idx, level: levelEl.checked ? 1 : 0 }));
        }
      });

      const pulseBtn = row.querySelector('.gpio-pulse');
      pulseBtn.addEventListener('click', () => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: 'pulse_gpio', idx }));
        }
      });
    });
    // Re-apply current language to the freshly-rendered rows.
    const sel = document.getElementById('lang-select');
    if (sel) applyLang(sel.value);
  }

  function setGpioFromDevice(arr) {
    if (!Array.isArray(arr)) return;
    arr.forEach((entry, idx) => {
      const row = document.querySelector(`.gpio-row[data-idx="${idx}"]`);
      if (!row) return;
      const modeInt = MODE_SHORT_TO_INT[entry.m];
      if (modeInt === undefined) return;
      const wireMode = MODE_INT_TO_WIRE[modeInt];
      const modeSel = row.querySelector('.gpio-mode');
      // Don't fight a focused mode dropdown.
      if (document.activeElement !== modeSel && modeSel.value !== wireMode) {
        modeSel.value = wireMode;
      }
      const isOutput = (modeInt === 3);
      row.querySelector('.gpio-input-tail').hidden = isOutput;
      row.querySelector('.gpio-output-tail').hidden = !isOutput;
      row.querySelector('.gpio-value').textContent = entry.v ? '1' : '0';
      const levelEl = row.querySelector('.gpio-level');
      const levelTxt = row.querySelector('.gpio-level-text');
      if (document.activeElement !== levelEl) levelEl.checked = !!entry.v;
      levelTxt.textContent = entry.v ? '1' : '0';
      const pulsing = !!entry.p;
      row.classList.toggle('pulsing', pulsing);
      const pulseBtn = row.querySelector('.gpio-pulse');
      pulseBtn.disabled = pulsing;
      levelEl.disabled  = pulsing;
      modeSel.disabled  = pulsing;
      pulseBtn.textContent = pulsing ? t('gpio_pulsing') : t('gpio_pulse_btn');
    });
  }

  // ---------- Power switch ----------
  const powerBtn = document.getElementById('power_btn');
  function setPowerFromDevice(on) {
    powerBtn.dataset.on = on ? '1' : '0';
    powerBtn.classList.toggle('on', !!on);
    powerBtn.querySelector('.txt').textContent = on ? t('power_on') : t('power_off');
  }
  powerBtn.addEventListener('click', () => {
    const want = powerBtn.dataset.on === '1' ? 0 : 1;
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'set_power', on: want === 1 }));
    }
  });

  // ---------- Pulse-width settings ----------
  const pulseWidthEl = document.getElementById('pulse-width-ms');
  let pulseWidthFromDeviceArmed = false;
  function setPulseWidthFromDevice(ms) {
    if (!pulseWidthEl) return;
    if (document.activeElement === pulseWidthEl) return;
    if (!pulseWidthFromDeviceArmed) {
      pulseWidthEl.value = String(ms);
      pulseWidthFromDeviceArmed = true;
    }
  }
  document.getElementById('apply_pulse_width').addEventListener('click', () => {
    const n = parseInt(pulseWidthEl.value, 10);
    if (!isFinite(n) || n < 1 || n > 10000) {
      pulseWidthEl.value = '100';
      return;
    }
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'set_pulse_width', width_ms: n }));
    }
  });
```

Note: `applyLang` already iterates `[data-i18n]` on every render — both static
HTML and dynamically-built rows are covered after `applyLang` is re-invoked
in `buildGpioRows`. Define a `t(key)` helper near the top of the IIFE if one
doesn't exist:

```js
  // (place just after I18N is defined)
  let currentLang = 'en';
  function t(key) {
    return (I18N[currentLang] && I18N[currentLang][key]) || I18N.en[key] || key;
  }
```

Modify the existing `applyLang(lang)` to update `currentLang = lang;` at the top.

- [ ] **Step 2: Extend the WS message dispatcher**

Find the `ws.addEventListener('message', ...)` block. Inside the `if (msg.type === 'status')` branch, append:

```js
        if (Array.isArray(msg.gpio)) setGpioFromDevice(msg.gpio);
        if (typeof msg.power === 'number') setPowerFromDevice(msg.power);
        if (typeof msg.pulse_width_ms === 'number') setPulseWidthFromDevice(msg.pulse_width_ms);
```

- [ ] **Step 3: Build, flash, hard-refresh browser**

```
idf.py build
idf.py -p COM24 flash
```

In the browser, hard-refresh (Ctrl+Shift+R). Open Help panel and confirm the
new pin entries render. Open Settings panel and confirm the GPIO output
section is present. Confirm the Power Switch panel and GPIO panel exist
between Frequency and OTA. Click Group A row's mode dropdown — verify the
options are translated when language is switched.

- [ ] **Step 4: Commit**

```bash
git add components/net_dashboard/web/app.js
git commit -m "feat(dashboard): GPIO/power UI logic + 3-language i18n strings"
```

---

## Task 16: Dashboard CSS — power button, GPIO rows, pulsing greyout

**Files:**
- Modify: `components/net_dashboard/web/app.css`

- [ ] **Step 1: Append GPIO + power styling**

Append at the end of `components/net_dashboard/web/app.css`:

```css
/* ---------- Power switch ---------- */
.big-toggle {
  display: inline-flex;
  align-items: center;
  gap: 0.5em;
  font-size: 1.4em;
  padding: 0.5em 1.2em;
  background: var(--surface-2, #2a2a2a);
  color: var(--fg, #e6e6e6);
  border: 1px solid var(--border, #3a3a3a);
  border-radius: 0.5em;
  cursor: pointer;
}
.big-toggle .ico { font-size: 1.5em; }
.big-toggle.on  { background: var(--danger, #c62828); color: #fff; border-color: var(--danger, #c62828); }

/* ---------- GPIO panel ---------- */
.gpio-panel .gpio-group { margin-top: 0.8em; }
.gpio-panel h3 { margin: 0 0 0.4em 0; font-size: 1em; opacity: 0.8; }

.gpio-rows {
  display: grid;
  grid-template-columns: 1fr;
  gap: 0.4em;
}
.gpio-row {
  display: grid;
  grid-template-columns: 2.4em 4.4em 12em 1fr;
  align-items: center;
  gap: 0.6em;
  padding: 0.3em 0.5em;
  background: var(--surface-2, #1f1f1f);
  border: 1px solid var(--border, #2f2f2f);
  border-radius: 0.4em;
}
.gpio-row.pulsing { opacity: 0.55; }
.gpio-row .gpio-name { font-weight: 600; }
.gpio-row .gpio-pinnum { opacity: 0.7; font-family: ui-monospace, monospace; }
.gpio-row .gpio-mode { width: 100%; }
.gpio-row .gpio-tail { display: flex; align-items: center; gap: 0.6em; }
.gpio-row .gpio-input-tail,
.gpio-row .gpio-output-tail {
  display: inline-flex;
  align-items: center;
  gap: 0.4em;
}
.gpio-row .gpio-output-tail[hidden],
.gpio-row .gpio-input-tail[hidden] { display: none; }
.gpio-row .gpio-value {
  font-family: ui-monospace, monospace;
  min-width: 1ch;
}
.gpio-row .gpio-toggle {
  display: inline-flex;
  align-items: center;
  gap: 0.3em;
}
.gpio-row .gpio-pulse { padding: 0.2em 0.6em; }
.gpio-row .gpio-pulse:disabled,
.gpio-row .gpio-level:disabled,
.gpio-row .gpio-mode:disabled { cursor: not-allowed; }

/* Narrow viewport: stack columns */
@media (max-width: 560px) {
  .gpio-row {
    grid-template-columns: 2.4em 1fr;
    grid-auto-rows: auto;
  }
  .gpio-row .gpio-mode  { grid-column: 1 / -1; }
  .gpio-row .gpio-tail  { grid-column: 1 / -1; }
  .gpio-row .gpio-pinnum { grid-column: 2; }
}
```

- [ ] **Step 2: Build, flash, hard-refresh, eyeball**

```
idf.py build
idf.py -p COM24 flash
```

Hard-refresh the browser. Visually check:
- Power button has a clear OFF style; click → flips to ON with a coloured fill.
- GPIO rows align in a grid; A1..A8 then B1..B8.
- Switch one Group A row's mode to `output` — toggle + Pulse button appear.
- Click Pulse — row briefly fades while `pulsing` is true.

- [ ] **Step 3: Commit**

```bash
git add components/net_dashboard/web/app.css
git commit -m "style(dashboard): power button + GPIO rows + pulse greyout"
```

---

## Task 17: HID descriptor — add report id 0x04 OUT (4 bytes)

**Files:**
- Modify: `components/usb_composite/usb_descriptors.c`
- Modify: `components/usb_composite/usb_composite.c`
- Modify: `components/usb_composite/include/usb_protocol.h`
- Modify: `components/usb_composite/usb_hid_task.c`
- Modify: `components/usb_composite/CMakeLists.txt`

- [ ] **Step 1: Add gpio_io to usb_composite REQUIRES**

Open `components/usb_composite/CMakeLists.txt`, add `gpio_io` to its REQUIRES list (near the existing `app_api` line).

- [ ] **Step 2: Add new HID + CDC constants to `usb_protocol.h`**

Open `components/usb_composite/include/usb_protocol.h`. Add **before** the closing `#ifdef __cplusplus`:

```c
// ---- GPIO IO + power switch (HID) ------------------------------------------

#define USB_HID_REPORT_GPIO          0x04   // OUT, 4 B (op, b1, b2, b3)

// op codes inside report 0x04 payload byte 0
#define USB_HID_GPIO_OP_SET_MODE     0x01   // payload: idx, mode, _
#define USB_HID_GPIO_OP_SET_LEVEL    0x02   // payload: idx, level, _
#define USB_HID_GPIO_OP_PULSE        0x03   // payload: idx, width_lo, width_hi
#define USB_HID_GPIO_OP_POWER        0x04   // payload: on, _, _

// ---- GPIO IO + power switch (CDC SLIP) -------------------------------------

#define USB_CDC_OP_GPIO_SET_MODE     0x30   // payload: idx, mode
#define USB_CDC_OP_GPIO_SET_LEVEL    0x31   // payload: idx, level
#define USB_CDC_OP_GPIO_PULSE        0x32   // payload: idx, width_lo, width_hi
#define USB_CDC_OP_POWER             0x33   // payload: on
#define USB_CDC_OP_PULSE_WIDTH_SET   0x34   // payload: width_lo, width_hi
```

- [ ] **Step 3: Extend the HID report descriptor**

Open `components/usb_composite/usb_descriptors.c`. In the `enum`, add `REPORT_ID_GPIO = 0x04` between FACTORY_RESET (0x03) and STATUS (0x10).

In the `usb_hid_report_descriptor[]` array, insert this block **between** the existing 0x03 OUT entry and the 0x10 IN entry:

```c
    // 0x04 OUT GPIO/power: 4 bytes
    0x85, REPORT_ID_GPIO,
    0x09, 0x05,
    0x75, 0x08, 0x95, 4,
    0x91, 0x02,
```

This adds 10 bytes to the descriptor (the new entry is `0x85,id, 0x09,usage, 0x75,8, 0x95,4, 0x91,0x02` = 10 bytes). Update the `_Static_assert`:

```c
_Static_assert(sizeof(usb_hid_report_descriptor) == 73,
               "HID_REPORT_DESC_SIZE in usb_composite.c must match this size");
```

If `idf.py build` reports a mismatch (e.g. complains about `73`), the actual
size produced by the toolchain is the source of truth — change the assert AND
the macro in the next step to whatever `sizeof` returns. **Do not silently
let the assert pass with a different number than the macro.**

- [ ] **Step 4: Update `HID_REPORT_DESC_SIZE` macro in `usb_composite.c`**

Open `components/usb_composite/usb_composite.c`. Find the line `#define HID_REPORT_DESC_SIZE 63` (around line 49) and change the literal to match whatever the assert in step 3 settled on (e.g. `73`).

- [ ] **Step 5: Handle the new report id in `usb_hid_task.c`**

Open `components/usb_composite/usb_hid_task.c`. Add the include:

```c
#include "gpio_io.h"
```

In `tud_hid_set_report_cb`, add a new case before `default:`:

```c
    case USB_HID_REPORT_GPIO: {
        if (bufsize < 4) return;
        uint8_t op = buffer[0];
        switch (op) {
        case USB_HID_GPIO_OP_SET_MODE: {
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_GPIO_SET_MODE,
                .gpio_set_mode = { .idx = buffer[1], .mode = buffer[2] },
            };
            control_task_post(&c, 0);
        } break;
        case USB_HID_GPIO_OP_SET_LEVEL: {
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_GPIO_SET_LEVEL,
                .gpio_set_level = { .idx = buffer[1], .level = buffer[2] ? 1u : 0u },
            };
            control_task_post(&c, 0);
        } break;
        case USB_HID_GPIO_OP_PULSE: {
            uint16_t width = (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8);
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_GPIO_PULSE,
                .gpio_pulse = { .idx = buffer[1], .width_ms = width },
            };
            control_task_post(&c, 0);
        } break;
        case USB_HID_GPIO_OP_POWER: {
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_POWER_SET,
                .power_set = { .on = buffer[1] ? 1u : 0u },
            };
            control_task_post(&c, 0);
        } break;
        default:
            ESP_LOGW(TAG, "unknown gpio op 0x%02x", op);
            break;
        }
    } break;
```

- [ ] **Step 6: Build to confirm descriptor size matches**

Run: `idf.py build`

If a `_Static_assert` failure appears, note the actual size reported, then update both the `_Static_assert` literal **and** the `HID_REPORT_DESC_SIZE` macro to match. Re-run `idf.py build` until clean.

- [ ] **Step 7: Flash and verify HID still enumerates**

```
idf.py -p COM24 flash monitor
```

On the host PC, check Device Manager / `lsusb` to confirm the HID interface still enumerates without errors. Send a HID report id 0x04 with op 0x04 and `on=1` (e.g. via the existing PC tool or an ad-hoc Python `hid` script) and verify the power switch toggles.

- [ ] **Step 8: Commit**

```bash
git add components/usb_composite/include/usb_protocol.h \
        components/usb_composite/usb_descriptors.c \
        components/usb_composite/usb_composite.c \
        components/usb_composite/usb_hid_task.c \
        components/usb_composite/CMakeLists.txt
git commit -m "feat(usb_hid): add report id 0x04 for GPIO + power ops"
```

---

## Task 18: CDC SLIP frontend — handle 0x30..0x34; status response GPIO tail

**Files:**
- Modify: `components/usb_composite/usb_cdc_task.c`

- [ ] **Step 1: Handle the 5 new CDC ops in `handle_frame`**

Open `components/usb_composite/usb_cdc_task.c`. Add the includes:

```c
#include "app_api.h"
#include "gpio_io.h"
```

In `handle_frame`, add new cases between `USB_CDC_OP_OTA_END` and `USB_CDC_OP_FACTORY_RESET`:

```c
    case USB_CDC_OP_GPIO_SET_MODE: {
        if (plen < 2) break;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_SET_MODE,
            .gpio_set_mode = { .idx = payload[0], .mode = payload[1] },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_GPIO_SET_LEVEL: {
        if (plen < 2) break;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_SET_LEVEL,
            .gpio_set_level = { .idx = payload[0], .level = payload[1] ? 1u : 0u },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_GPIO_PULSE: {
        if (plen < 3) break;
        uint16_t width = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_PULSE,
            .gpio_pulse = { .idx = payload[0], .width_ms = width },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_POWER: {
        if (plen < 1) break;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_POWER_SET,
            .power_set = { .on = payload[0] ? 1u : 0u },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_PULSE_WIDTH_SET: {
        if (plen < 2) break;
        uint16_t w = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_PULSE_WIDTH_SET,
            .pulse_width_set = { .width_ms = w },
        };
        control_task_post(&c, 0);
    } break;
```

- [ ] **Step 2: Build**

Run: `idf.py build`
Expected: clean build.

- [ ] **Step 3: Manual verify with Python `pyserial`**

Use a small Python script that opens the CDC com port and sends:

```python
import serial
s = serial.Serial('COM25', 115200)
END = 0xC0
# power ON via op 0x33, payload 0x01
s.write(bytes([END, 0x33, 0x01, END]))
```

Verify the power switch flips (multimeter on GPIO21).

- [ ] **Step 4: Commit**

```bash
git add components/usb_composite/usb_cdc_task.c
git commit -m "feat(usb_cdc): handle GPIO/power/pulse-width SLIP ops 0x30..0x34"
```

---

## Task 19: NVS persistence for `pulse_width_ms`

**Files:**
- Modify: `components/gpio_io/gpio_io.c`

- [ ] **Step 1: Load on init, save on set**

Add at the top of `components/gpio_io/gpio_io.c` (after the existing `static const char *TAG = ...`):

```c
#define NVS_NAMESPACE  "gpio_io"
#define NVS_KEY_PULSE  "pulse_ms"

static void load_pulse_width_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint32_t v = 0;
    if (nvs_get_u32(h, NVS_KEY_PULSE, &v) == ESP_OK && v >= 1 && v <= 10000) {
        atomic_store_explicit(&s_pulse_width_ms, v, memory_order_relaxed);
        ESP_LOGI(TAG, "pulse width loaded from NVS: %lu ms", (unsigned long)v);
    }
    nvs_close(h);
}

static void save_pulse_width_to_nvs(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, NVS_KEY_PULSE, v);
    nvs_commit(h);
    nvs_close(h);
}
```

In `gpio_io_init`, after the existing `s_pulse_width_ms = CONFIG_APP_DEFAULT_PULSE_WIDTH_MS;` line, add:

```c
    load_pulse_width_from_nvs();
```

In `gpio_io_set_pulse_width_ms`, after the existing `atomic_store_explicit` line, add:

```c
    save_pulse_width_to_nvs(width_ms);
```

- [ ] **Step 2: Build, flash, verify persistence**

```
idf.py build
idf.py -p COM24 flash monitor
```

In the dashboard, change pulse width to 250 ms, click Apply. Press Ctrl+]
to exit monitor, then run `idf.py -p COM24 monitor` again (or just press
RST) — boot log should show:

```
I (xxx) gpio_io: pulse width loaded from NVS: 250 ms
```

The dashboard, when it reconnects, should also show 250 in the Settings
input.

- [ ] **Step 3: Commit**

```bash
git add components/gpio_io/gpio_io.c
git commit -m "feat(gpio_io): persist pulse_width_ms in NVS namespace gpio_io"
```

---

## Task 20: End-to-end smoke verification on hardware

**Files:** none (verification only)

- [ ] **Step 1: Force a clean build to catch any missed sdkconfig entry**

```
del sdkconfig
idf.py fullclean
idf.py build
idf.py -p COM24 flash monitor
```

Expected: clean boot. `gpio_io: gpio_io ready` log appears.

- [ ] **Step 2: CLI smoke**

At `fan-testkit>`:

```
status
```
Expected: power off; A1..A8 i_pd:0; B1..B8 o:0; pulse_width=100ms.

```
gpio_mode 0 o
gpio_set 0 1
status
```
Expected: A1=o:1. Multimeter on GPIO7 → 3.3 V.

```
gpio_pulse 0 200
```
Scope GPIO7 → expect 200 ms low pulse (idle high → low → high).

```
power 1
```
Multimeter on GPIO21 → 0 V (active-low ON).
```
power 0
```
Multimeter on GPIO21 → 3.3 V.

- [ ] **Step 3: Web dashboard smoke**

Open `http://fan-testkit.local`. Verify:
- Power Switch panel is between Frequency and OTA.
- GPIO panel below it shows Group A (8 rows) and Group B (8 rows).
- Help panel lists power-switch GPIO + group_a / group_b pin numbers.
- Settings → GPIO output shows `Pulse width [100] ms`.

Click power button → multimeter on GPIO21 reflects active-low semantics.
Toggle B1 to high → multimeter on GPIO11 → 3.3 V.
Click Pulse on B1 → scope shows 100 ms pulse (idle low → high → low); row
greys out for 100 ms with "Pulsing…" label.
Set Pulse width to 250 ms, Apply, click Pulse on B1 → scope shows 250 ms.
Wire a 3.3 V jumper to GPIO7 (A1) → A1 row's value flips to 1 within ≤ 50 ms.

- [ ] **Step 4: Language smoke**

Switch language dropdown to 繁體中文 and 简体中文 — every label in the new
panels updates. Inspect the static help panel and dynamic GPIO rows.

- [ ] **Step 5: Persistence smoke**

Set pulse-width to 250, Apply. Press the RST button on the board. After
reboot, dashboard should show 250 (loaded from NVS).

Toggle power ON. Press RST. After reboot, power should be **OFF**
(intentional non-persistence).

- [ ] **Step 6: Regression smoke**

- PWM still works: set 1 kHz / 50 % via dashboard → scope GPIO4.
- RPM still works: connect a tach signal to GPIO6 → RPM panel reads.
- Factory reset still works: hold BOOT for 3 s → board reboots into
  Fan-TestKit-setup SoftAP.
- OTA still works: upload a bin via dashboard → device flashes + reboots.

- [ ] **Step 7: Commit a record of verification (optional, for traceability)**

If any docs file (e.g. HANDOFF.md) tracks per-feature verification, append a
one-liner:

```
- 2026-04-25: GPIO IO + power switch verified end-to-end on COM24.
```

```bash
git add HANDOFF.md
git commit -m "docs(handoff): mark GPIO IO + power switch verified"
```

(Skip Step 7 if the project doesn't track verification this way.)

---

## Summary

After Task 20 the system has:

- A `gpio_io` component owning 16-pin atomic state, pulse timers, and a 20 Hz poll task.
- 5 new `ctrl_cmd_kind_t` values plumbed through control_task.
- New CLI commands: `gpio_mode`, `gpio_set`, `gpio_pulse`, `power`; extended `status`.
- New WS messages: `set_gpio_mode`, `set_gpio_level`, `pulse_gpio`, `set_power`, `set_pulse_width`.
- Telemetry frame extended with `gpio[]`, `power`, `pulse_width_ms`.
- Dashboard Power panel + GPIO panel + Settings pulse-width section, fully i18n'd in en / zh-Hant / zh-Hans, with pulsing-greyout UI.
- HID report id `0x04` for GPIO/power ops; descriptor lockstep maintained via `_Static_assert`.
- CDC SLIP ops `0x30..0x34` for GPIO/power/pulse-width.
- NVS persistence of pulse width in namespace `gpio_io`; everything else resets to Kconfig defaults each boot.

All changes preserve the project's two architecture invariants: **single logical handler with multiple transport frontends**, and **components never depend on `main`**.
