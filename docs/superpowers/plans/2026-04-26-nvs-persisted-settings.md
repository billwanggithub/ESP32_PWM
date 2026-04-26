# NVS-Persisted Runtime Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist user-tunable settings (RPM pole/mavg/timeout, PWM frequency, dashboard slider step sizes) to NVS so they survive reboot, with explicit Save reachable from all four transports (WebSocket, USB HID, USB CDC, CLI).

**Architecture:**
- **Per-component NVS namespaces** matching the established `psu_driver` pattern (`psu_driver.c:114-171`). Each component owns its own NVS load/save and exposes `*_save_to_nvs()` functions; load happens inside `*_init()` and overrides `cfg` defaults if NVS keys exist.
- **Boot policy**: PWM frequency restores from NVS; PWM duty **always boots to 0** (operator must explicitly turn output on after boot — this matches the existing CLAUDE.md invariant that boot defaults flow through `control_task` as a `CTRL_CMD_SET_PWM` post). No saved-duty concept exists — only freq is stored.
- **Single-handler invariant preserved**: Save commands flow through `control_task` via new `ctrl_cmd_t` kinds, so the wire/HID/CDC/CLI transports remain pure protocol frontends. No Save logic in any transport file.
- **Step sizes** are a new tiny component `ui_settings` because they don't belong to `pwm_gen` (firmware doesn't apply them) and shouldn't bloat `net_dashboard`. The component is just `ui_settings_get_steps()` / `ui_settings_save_steps()` over NVS.

**Tech Stack:** ESP-IDF v6.0, NVS (`nvs.h` / `nvs_flash.h`), FreeRTOS queues, cJSON for WS, esp_console for CLI, TinyUSB HID/CDC.

**Phasing:** Three phases, each ending with a working build that can be flashed and tested. RPM (Phase 1) → PWM freq (Phase 2) → Step sizes (Phase 3). Each phase fully wires all four transports before moving on.

---

## Files Touched (overview)

### Phase 1 — RPM params save
- Modify: `components/rpm_cap/include/rpm_cap.h` — add 2 save fns
- Modify: `components/rpm_cap/rpm_cap.c` — add NVS load + 2 save fns; integrate into `rpm_cap_init`
- Modify: `components/rpm_cap/CMakeLists.txt` — add `nvs_flash` REQUIRES
- Modify: `components/app_api/include/app_api.h` — add 2 ctrl_cmd kinds
- Modify: `main/control_task.c` — handle 2 new cmd kinds
- Modify: `components/net_dashboard/ws_handler.c` — add 2 WS ops
- Modify: `components/usb_composite/include/usb_protocol.h` — add HID op codes inside report 0x02 + CDC ops
- Modify: `components/usb_composite/usb_hid_task.c` — extend report 0x02 parser
- Modify: `components/usb_composite/usb_cdc_*.c` — handle 2 new CDC ops
- Modify: `main/app_main.c` — register 2 new CLI commands

### Phase 2 — PWM freq save (boot duty=0)
- Modify: `components/pwm_gen/include/pwm_gen.h` — add `pwm_gen_save_freq_to_nvs`, `pwm_gen_load_saved_freq`
- Modify: `components/pwm_gen/pwm_gen.c` — add NVS load/save (no auto-load in `pwm_gen_init`; loaded explicitly by `app_main`)
- Modify: `components/pwm_gen/CMakeLists.txt` — add `nvs_flash` REQUIRES
- Modify: `components/app_api/include/app_api.h` — add 1 ctrl_cmd kind (save current freq)
- Modify: `main/control_task.c` — handle save cmd
- Modify: `main/app_main.c` — replace hard-coded `SET_PWM(10000, 0)` boot post with `SET_PWM(load_saved_freq(), 0)`; add CLI command
- Modify: `components/net_dashboard/ws_handler.c` — add WS op
- Modify: `components/usb_composite/include/usb_protocol.h` — add new HID report id 0x06 + CDC op
- Modify: `components/usb_composite/usb_hid_task.c` — handle new report
- Modify: `components/usb_composite/usb_descriptors.c` — add report 0x06 to HID descriptor; bump descriptor size assert
- Modify: `components/usb_composite/usb_cdc_*.c` — handle new CDC op

### Phase 3 — Step sizes save
- Create: `components/ui_settings/CMakeLists.txt`
- Create: `components/ui_settings/include/ui_settings.h`
- Create: `components/ui_settings/ui_settings.c`
- Modify: `components/app_api/include/app_api.h` — add 1 ctrl_cmd kind
- Modify: `main/control_task.c` — handle save cmd
- Modify: `components/net_dashboard/CMakeLists.txt` — add `ui_settings` REQUIRES
- Modify: `components/net_dashboard/ws_handler.c` — add WS op + include step sizes in status frame
- Modify: `components/net_dashboard/web/app.js` — replace `localStorage` step persistence with WS-driven values; add Save button wiring
- Modify: `components/net_dashboard/web/index.html` — add Save button next to step inputs
- Modify: `components/usb_composite/include/usb_protocol.h` — add HID op + CDC op
- Modify: `components/usb_composite/usb_hid_task.c` — handle new HID op
- Modify: `components/usb_composite/usb_cdc_*.c` — handle new CDC op
- Modify: `main/app_main.c` — register CLI; init `ui_settings` before `net_dashboard`

---

## Wire-Protocol Additions (locked before code is written)

These are the contract values used throughout the plan. Defining them up front so every task references the same constants.

### WebSocket ops (client → device)
```json
{"type":"save_rpm_params"}                       // saves current pole + mavg
{"type":"save_rpm_timeout"}                      // saves current timeout
{"type":"save_pwm_freq"}                         // saves current freq (NOT duty)
{"type":"save_ui_steps", "duty_step":1.0, "freq_step":100}  // sets and saves
```

Each generates `{"type":"ack","op":"<op_name>","ok":true|false}`.

### HID report IDs and op codes
- Report `0x02` (existing SET_RPM, OUT 7 B) gets a 1-byte op-code prefix repurposed: existing usage is "set live params", new op code `0xA1` after the payload byte means "save params to NVS", `0xA2` means "save timeout to NVS". Implementation: add a single op byte at end of `usb_hid_set_rpm_t`, default `0x00` = legacy live-set, `0xA1` / `0xA2` are save-without-set. **However**, this changes the existing struct size. To keep the existing PC tool contract intact, allocate a **new** report id `0x06` instead:

```
USB_HID_REPORT_SETTINGS_SAVE  0x06  OUT, 8 B
```

Payload (8 bytes total — pad to match other 8-B reports):
```c
typedef struct __attribute__((packed)) {
    uint8_t  op;          // 0x01..0x04
    uint8_t  pad;
    union {
        struct { uint32_t freq_hz; }                    save_pwm_freq;   // op 0x01
        // op 0x02 (save_rpm_params) and 0x03 (save_rpm_timeout) take no payload
        struct { uint16_t duty_step_x100; uint16_t freq_step; }  save_ui_steps;  // op 0x04
    };
    uint8_t  pad2[2];
} usb_hid_settings_save_t;
```

Op codes inside report 0x06 byte 0:
```
USB_HID_SAVE_OP_PWM_FREQ      0x01
USB_HID_SAVE_OP_RPM_PARAMS    0x02
USB_HID_SAVE_OP_RPM_TIMEOUT   0x03
USB_HID_SAVE_OP_UI_STEPS      0x04
```

### CDC SLIP ops
```
USB_CDC_OP_SAVE_PWM_FREQ      0x50   payload: u32 freq_hz
USB_CDC_OP_SAVE_RPM_PARAMS    0x51   payload: empty (uses current live values)
USB_CDC_OP_SAVE_RPM_TIMEOUT   0x52   payload: empty
USB_CDC_OP_SAVE_UI_STEPS      0x53   payload: float duty_step + uint16_t freq_step
```

### CLI commands
```
save_rpm_params       no args, saves current pole + mavg
save_rpm_timeout      no args, saves current timeout
save_pwm_freq         no args, saves current freq (read from control_task_get_pwm)
save_ui_steps <duty> <freq>   args: float, int — sets and saves
```

### control_task command kinds
```c
CTRL_CMD_SAVE_RPM_PARAMS,        // no payload
CTRL_CMD_SAVE_RPM_TIMEOUT,       // no payload
CTRL_CMD_SAVE_PWM_FREQ,          // no payload (reads current freq from pwm_gen)
CTRL_CMD_SAVE_UI_STEPS,          // payload: float duty_step, uint16_t freq_step
```

### NVS keys
- Namespace `rpm_cap`: `pole` (u8), `mavg` (u16), `timeout_us` (u32)
- Namespace `pwm_gen`: `freq_hz` (u32)
- Namespace `ui_settings`: `duty_step` (blob: float, 4 B) — float NVS values are stored as blobs since NVS has no native float; `freq_step` (u16)

---

## Phase 1 — RPM params save (3 settings × 4 transports)

### Task 1.1: rpm_cap NVS load/save core

**Files:**
- Modify: `components/rpm_cap/CMakeLists.txt`
- Modify: `components/rpm_cap/rpm_cap.c:1-32` (header includes + module statics)
- Modify: `components/rpm_cap/rpm_cap.c:288-351` (rpm_cap_init body)
- Modify: `components/rpm_cap/include/rpm_cap.h`

- [ ] **Step 1: Add `nvs_flash` to rpm_cap REQUIRES**

Edit `components/rpm_cap/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS        "rpm_cap.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES    driver
                esp_driver_mcpwm
                esp_driver_gpio
                esp_timer
                log
                nvs_flash
)
```

- [ ] **Step 2: Add NVS includes and namespace constants to rpm_cap.c**

Insert after the existing includes block (around `rpm_cap.c:13`):
```c
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE      "rpm_cap"
#define NVS_KEY_POLE       "pole"
#define NVS_KEY_MAVG       "mavg"
#define NVS_KEY_TIMEOUT    "timeout_us"
```

- [ ] **Step 3: Add NVS load helper above rpm_cap_init**

Insert directly above `esp_err_t rpm_cap_init` (before line `rpm_cap.c:288`):

```c
// Override cfg defaults with NVS values if present. Keys missing → keep cfg
// defaults silently (first boot path). Read errors other than NOT_FOUND are
// logged but non-fatal — we always have safe defaults.
static void load_nvs_overrides(uint8_t *pole, uint16_t *mavg, uint32_t *timeout_us)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open ro failed: %s", esp_err_to_name(e));
        return;
    }
    uint8_t  v_pole = *pole;
    uint16_t v_mavg = *mavg;
    uint32_t v_to   = *timeout_us;
    if (nvs_get_u8 (h, NVS_KEY_POLE,    &v_pole) == ESP_OK && v_pole != 0) *pole       = v_pole;
    if (nvs_get_u16(h, NVS_KEY_MAVG,    &v_mavg) == ESP_OK && v_mavg != 0) *mavg       = v_mavg;
    if (nvs_get_u32(h, NVS_KEY_TIMEOUT, &v_to)   == ESP_OK && v_to >= 1000) *timeout_us = v_to;
    nvs_close(h);
}
```

- [ ] **Step 4: Wire NVS overrides into rpm_cap_init**

Replace the three `atomic_store_explicit` initialisations at `rpm_cap.c:295-300` with:

```c
    uint8_t  pole       = cfg->pole_count       ? cfg->pole_count       : 2;
    uint16_t mavg       = cfg->moving_avg_count ? cfg->moving_avg_count : 16;
    uint32_t timeout_us = cfg->rpm_timeout_us   ? cfg->rpm_timeout_us   : 1000000u;
    load_nvs_overrides(&pole, &mavg, &timeout_us);
    atomic_store_explicit(&s_cap.pole_count,       pole,       memory_order_relaxed);
    atomic_store_explicit(&s_cap.moving_avg_count, mavg,       memory_order_relaxed);
    atomic_store_explicit(&s_cap.rpm_timeout_us,   timeout_us, memory_order_relaxed);
```

- [ ] **Step 5: Add save functions at end of rpm_cap.c**

Append to `components/rpm_cap/rpm_cap.c` (after `rpm_cap_init`):

```c
esp_err_t rpm_cap_save_params_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    uint8_t  p = atomic_load_explicit(&s_cap.pole_count,       memory_order_relaxed);
    uint16_t m = atomic_load_explicit(&s_cap.moving_avg_count, memory_order_relaxed);
    esp_err_t e1 = nvs_set_u8 (h, NVS_KEY_POLE, p);
    esp_err_t e2 = nvs_set_u16(h, NVS_KEY_MAVG, m);
    if (e1 == ESP_OK && e2 == ESP_OK) nvs_commit(h);
    nvs_close(h);
    if (e1 != ESP_OK) return e1;
    if (e2 != ESP_OK) return e2;
    ESP_LOGI(TAG, "saved to NVS: pole=%u mavg=%u", p, m);
    return ESP_OK;
}

esp_err_t rpm_cap_save_timeout_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    uint32_t t = atomic_load_explicit(&s_cap.rpm_timeout_us, memory_order_relaxed);
    e = nvs_set_u32(h, NVS_KEY_TIMEOUT, t);
    if (e == ESP_OK) nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) ESP_LOGI(TAG, "saved to NVS: timeout_us=%" PRIu32, t);
    return e;
}
```

- [ ] **Step 6: Declare save functions in header**

Edit `components/rpm_cap/include/rpm_cap.h` after `rpm_cap_set_timeout` declaration (line 21):

```c
// Persist current live pole_count + moving_avg_count to NVS. Survives reboot.
esp_err_t rpm_cap_save_params_to_nvs(void);

// Persist current live rpm_timeout_us to NVS. Survives reboot.
esp_err_t rpm_cap_save_timeout_to_nvs(void);
```

- [ ] **Step 7: Build to confirm compile**

Run: `idf.py build`
Expected: success; warning-free for rpm_cap.c.

- [ ] **Step 8: Commit**

```bash
git add components/rpm_cap
git commit -m "feat(rpm_cap): NVS persistence for pole / mavg / timeout

Load NVS overrides at init (silently falls back to cfg defaults if
keys missing). Two new public save fns expose explicit-Save semantics
to upper layers; component manages its own namespace.
"
```

### Task 1.2: control_task new command kinds

**Files:**
- Modify: `components/app_api/include/app_api.h:16-32`
- Modify: `main/control_task.c:42-134`

- [ ] **Step 1: Extend ctrl_cmd_kind_t enum**

Edit `components/app_api/include/app_api.h`. Insert after `CTRL_CMD_SET_RPM_TIMEOUT,` (line 19):

```c
    CTRL_CMD_SAVE_RPM_PARAMS,
    CTRL_CMD_SAVE_RPM_TIMEOUT,
```

(No payload union member needed — these commands take no args.)

- [ ] **Step 2: Handle new commands in control_task**

Edit `main/control_task.c`. Insert after the `CTRL_CMD_SET_RPM_TIMEOUT:` case (after line 65):

```c
        case CTRL_CMD_SAVE_RPM_PARAMS: {
            esp_err_t e = rpm_cap_save_params_to_nvs();
            if (e != ESP_OK) ESP_LOGW(TAG, "save_rpm_params failed: %s", esp_err_to_name(e));
        } break;
        case CTRL_CMD_SAVE_RPM_TIMEOUT: {
            esp_err_t e = rpm_cap_save_timeout_to_nvs();
            if (e != ESP_OK) ESP_LOGW(TAG, "save_rpm_timeout failed: %s", esp_err_to_name(e));
        } break;
```

- [ ] **Step 3: Build and commit**

Run: `idf.py build`
Expected: success.

```bash
git add components/app_api main/control_task.c
git commit -m "feat(control_task): wire SAVE_RPM_PARAMS / SAVE_RPM_TIMEOUT cmds"
```

### Task 1.3: WebSocket transport for RPM save

**Files:**
- Modify: `components/net_dashboard/ws_handler.c:76-95`

- [ ] **Step 1: Add two new WS op handlers**

Insert directly after the `set_rpm` block (after line 95 in `ws_handler.c`, before the next `else if`):

```c
    } else if (strcmp(type_j->valuestring, "save_rpm_params") == 0) {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_PARAMS };
        control_task_post(&c, 0);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"save_rpm_params\",\"ok\":true}");
    } else if (strcmp(type_j->valuestring, "save_rpm_timeout") == 0) {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_TIMEOUT };
        control_task_post(&c, 0);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"save_rpm_timeout\",\"ok\":true}");
```

- [ ] **Step 2: Build and commit**

```bash
idf.py build
git add components/net_dashboard/ws_handler.c
git commit -m "feat(ws): save_rpm_params / save_rpm_timeout WS ops"
```

### Task 1.4: USB CDC transport for RPM save

**Files:**
- Modify: `components/usb_composite/include/usb_protocol.h`
- Modify: `components/usb_composite/usb_cdc_task.c` (contains the CDC op switch)

- [ ] **Step 1: Add CDC op codes to usb_protocol.h**

Append at the end of the CDC ops section (after `USB_CDC_OP_PSU_TELEMETRY` at line 117):

```c
// ---- Settings save (CDC SLIP) ---------------------------------------------

#define USB_CDC_OP_SAVE_PWM_FREQ      0x50   // payload: u32 freq_hz
#define USB_CDC_OP_SAVE_RPM_PARAMS    0x51   // payload: empty
#define USB_CDC_OP_SAVE_RPM_TIMEOUT   0x52   // payload: empty
#define USB_CDC_OP_SAVE_UI_STEPS      0x53   // payload: float duty_step + u16 freq_step
```

- [ ] **Step 2: Add cases for the two RPM save ops**

After the `USB_CDC_OP_PSU_SET_SLAVE` case in the same switch, add:

```c
case USB_CDC_OP_SAVE_RPM_PARAMS: {
    ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_PARAMS };
    control_task_post(&c, 0);
} break;
case USB_CDC_OP_SAVE_RPM_TIMEOUT: {
    ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_TIMEOUT };
    control_task_post(&c, 0);
} break;
```

In `usb_cdc_task.c`, locate the existing `switch (op) { case USB_CDC_OP_PSU_SET_VOLTAGE: ... }` block — that's where these two new cases go (alongside `USB_CDC_OP_PSU_SET_SLAVE`).

- [ ] **Step 3: Build and commit**

```bash
idf.py build
git add components/usb_composite
git commit -m "feat(usb_cdc): save_rpm_params / save_rpm_timeout CDC ops 0x51 / 0x52"
```

### Task 1.5: USB HID transport for RPM save (deferred to Phase 2)

This phase **defers** HID for RPM save. Reason: HID save needs a new report id `0x06` (as designed), and adding an HID report id requires updating `usb_descriptors.c` + the `_Static_assert(sizeof(usb_hid_report_descriptor) == ...)` constant. To avoid two descriptor edits in close succession, all four HID save ops (PWM_FREQ, RPM_PARAMS, RPM_TIMEOUT, UI_STEPS) land together in **Task 2.5** when report 0x06 is introduced. This task is a placeholder.

- [ ] **Step 1: Verify nothing TODO** — no work for this task; HID coverage arrives in Task 2.5.

### Task 1.6: CLI for RPM save

**Files:**
- Modify: `main/app_main.c` — add 2 CLI commands following the `cmd_psu_slave` / register pattern at `app_main.c:215-220, 310`

- [ ] **Step 1: Add command handlers**

Find the existing `cmd_rpm_params` / `cmd_rpm_timeout` handlers in `app_main.c` (search for `cmd_rpm_`). Directly **after** the last RPM-related handler, add:

```c
// ---- CLI: save_rpm_params ---------------------------------------------------
static struct { struct arg_end *end; } s_save_rpm_params_args;
static int cmd_save_rpm_params(int argc, char **argv)
{
    (void)argc; (void)argv;
    ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_PARAMS };
    esp_err_t e = control_task_post(&c, pdMS_TO_TICKS(50));
    if (e != ESP_OK) { printf("post failed: %s\n", esp_err_to_name(e)); return 1; }
    printf("save_rpm_params queued\n");
    return 0;
}

// ---- CLI: save_rpm_timeout --------------------------------------------------
static struct { struct arg_end *end; } s_save_rpm_timeout_args;
static int cmd_save_rpm_timeout(int argc, char **argv)
{
    (void)argc; (void)argv;
    ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_TIMEOUT };
    esp_err_t e = control_task_post(&c, pdMS_TO_TICKS(50));
    if (e != ESP_OK) { printf("post failed: %s\n", esp_err_to_name(e)); return 1; }
    printf("save_rpm_timeout queued\n");
    return 0;
}
```

- [ ] **Step 2: Register the commands**

In the registration block (search for the existing `esp_console_cmd_register(&rt_cmd)` line, around `app_main.c:327`), append:

```c
    s_save_rpm_params_args.end = arg_end(0);
    const esp_console_cmd_t srp_cmd = {
        .command = "save_rpm_params", .help = "persist current pole + mavg to NVS",
        .hint = NULL, .func = cmd_save_rpm_params, .argtable = &s_save_rpm_params_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&srp_cmd));

    s_save_rpm_timeout_args.end = arg_end(0);
    const esp_console_cmd_t srt_cmd = {
        .command = "save_rpm_timeout", .help = "persist current RPM timeout to NVS",
        .hint = NULL, .func = cmd_save_rpm_timeout, .argtable = &s_save_rpm_timeout_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&srt_cmd));
```

- [ ] **Step 3: Build, flash, and verify**

```bash
idf.py build flash monitor
```

Expected on monitor (after Wi-Fi ready):
```
> rpm_params 4 32
> save_rpm_params
save_rpm_params queued
I (...) rpm_cap: saved to NVS: pole=4 mavg=32
```

Reset the board (Ctrl-T Ctrl-R or hardware) and verify the boot log shows the saved values:
```
I (...) rpm_cap: init ok: gpio=... pole=4 mavg=32 timeout_us=1000000
```

- [ ] **Step 4: Commit**

```bash
git add main/app_main.c
git commit -m "feat(cli): save_rpm_params / save_rpm_timeout commands

End of Phase 1 — all four transports for RPM-param save are now in
place. Manually verified via monitor: setpoint changes survive reboot.
"
```

### Phase 1 acceptance

- [ ] All four transports issue Save → log message → reboot → values restored.
- [ ] First-boot path with no NVS keys still uses the configured Kconfig defaults (verify with `idf.py erase-flash flash monitor`).

---

## Phase 2 — PWM frequency save (boot duty=0)

### Task 2.1: pwm_gen NVS save/load API

**Files:**
- Modify: `components/pwm_gen/CMakeLists.txt`
- Modify: `components/pwm_gen/pwm_gen.c` (top of file + new functions)
- Modify: `components/pwm_gen/include/pwm_gen.h`

Important: unlike `rpm_cap`, `pwm_gen_init` does **not** auto-load NVS. Reason: `pwm_gen_init` doesn't take a freq/duty in its config (look at `pwm_gen.h`: only `pwm_gpio` and `trigger_gpio`). The actual setpoint is established by `app_main`'s post-init `CTRL_CMD_SET_PWM` post. Per CLAUDE.md, all setpoint state must flow through that queue. So `app_main` calls `pwm_gen_load_saved_freq()` and substitutes the result into the `SET_PWM(saved_freq, 0)` post.

- [ ] **Step 1: Add `nvs_flash` to pwm_gen REQUIRES**

Edit `components/pwm_gen/CMakeLists.txt`, add `nvs_flash` to the REQUIRES list (preserve all existing REQUIRES; this is purely additive).

- [ ] **Step 2: Add NVS includes and namespace constants to pwm_gen.c**

Insert after the existing `#include` block in `pwm_gen.c`:

```c
#include "nvs.h"
#include "nvs_flash.h"

#define PWM_NVS_NAMESPACE  "pwm_gen"
#define PWM_NVS_KEY_FREQ   "freq_hz"
```

- [ ] **Step 3: Add load and save functions**

Append to `pwm_gen.c` (location: end of file is fine):

```c
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
    e = nvs_set_u32(h, PWM_NVS_KEY_FREQ, f);
    if (e == ESP_OK) nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) ESP_LOGI(TAG, "saved to NVS: freq_hz=%lu", (unsigned long)f);
    return e;
}
```

(If `TAG` isn't already defined in `pwm_gen.c`, look for it; usually `static const char *TAG = "pwm_gen";` is present. If not, add it at the top.)

- [ ] **Step 4: Declare new functions in pwm_gen.h**

Append to `components/pwm_gen/include/pwm_gen.h` before the closing `#ifdef __cplusplus` / `}`:

```c
// Read previously-saved freq from NVS. Returns fallback_hz if the key
// is missing or out of [PWM_GEN_FREQ_MIN_HZ, PWM_GEN_FREQ_MAX_HZ].
uint32_t pwm_gen_load_saved_freq(uint32_t fallback_hz);

// Persist the currently-active freq (read via pwm_gen_get) to NVS.
// Duty is intentionally NOT persisted — boot always starts at duty=0.
esp_err_t pwm_gen_save_current_freq_to_nvs(void);
```

- [ ] **Step 5: Build and commit**

```bash
idf.py build
git add components/pwm_gen
git commit -m "feat(pwm_gen): NVS save/load for freq_hz (no auto-load in init)

Caller responsibility (app_main) to use pwm_gen_load_saved_freq()
when constructing the boot SET_PWM ctrl_cmd. Duty is deliberately
not persisted — all reboots start with output off.
"
```

### Task 2.2: app_main uses saved freq on boot

**Files:**
- Modify: `main/app_main.c` — find the existing `CTRL_CMD_SET_PWM(10000, 0)` post

- [ ] **Step 1: Find the existing boot post**

Run: `grep -n "CTRL_CMD_SET_PWM" main/app_main.c` — locate the post-init block where the boot default is sent through `control_task_post`.

- [ ] **Step 2: Replace the hard-coded 10000 with `pwm_gen_load_saved_freq`**

Wherever the existing post looks like:
```c
ctrl_cmd_t boot = { .kind = CTRL_CMD_SET_PWM, .set_pwm = { .freq_hz = 10000, .duty_pct = 0.0f } };
control_task_post(&boot, 0);
```

Change to:
```c
uint32_t boot_freq = pwm_gen_load_saved_freq(10000);
ctrl_cmd_t boot = { .kind = CTRL_CMD_SET_PWM, .set_pwm = { .freq_hz = boot_freq, .duty_pct = 0.0f } };
control_task_post(&boot, 0);
ESP_LOGI(TAG, "boot pwm: freq=%lu duty=0 (from NVS or fallback)", (unsigned long)boot_freq);
```

The duty stays `0.0f` — this is the spec.

- [ ] **Step 3: Build, flash, verify**

```bash
idf.py build flash monitor
```

Expected log on first boot (no NVS key yet): `boot pwm: freq=10000 duty=0 (from NVS or fallback)`.

- [ ] **Step 4: Commit**

```bash
git add main/app_main.c
git commit -m "feat(boot): restore last-saved PWM freq, always boot duty=0

Falls back to 10 kHz on first boot or invalid NVS data. Duty=0 is
intentional and matches CLAUDE.md's safety posture for unsupervised
restart.
"
```

### Task 2.3: control_task save command

**Files:**
- Modify: `components/app_api/include/app_api.h`
- Modify: `main/control_task.c`

- [ ] **Step 1: Add CTRL_CMD_SAVE_PWM_FREQ to the enum**

In `app_api.h`, after `CTRL_CMD_SAVE_RPM_TIMEOUT,` (added in Phase 1):

```c
    CTRL_CMD_SAVE_PWM_FREQ,
```

- [ ] **Step 2: Handle in control_task**

In `main/control_task.c`, after the `CTRL_CMD_SAVE_RPM_TIMEOUT` case:

```c
        case CTRL_CMD_SAVE_PWM_FREQ: {
            esp_err_t e = pwm_gen_save_current_freq_to_nvs();
            if (e != ESP_OK) ESP_LOGW(TAG, "save_pwm_freq failed: %s", esp_err_to_name(e));
        } break;
```

- [ ] **Step 3: Build and commit**

```bash
idf.py build
git add components/app_api main/control_task.c
git commit -m "feat(control_task): SAVE_PWM_FREQ cmd"
```

### Task 2.4: WebSocket + CDC transports for PWM freq save

**Files:**
- Modify: `components/net_dashboard/ws_handler.c`
- Modify: `components/usb_composite/usb_cdc_rx.c` (or whatever houses the CDC op switch)

- [ ] **Step 1: WS handler**

In `ws_handler.c`, after the `save_rpm_timeout` block from Task 1.3:

```c
    } else if (strcmp(type_j->valuestring, "save_pwm_freq") == 0) {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_PWM_FREQ };
        control_task_post(&c, 0);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"save_pwm_freq\",\"ok\":true}");
```

- [ ] **Step 2: CDC handler**

In the CDC op switch, after the `USB_CDC_OP_SAVE_RPM_TIMEOUT` case:

```c
case USB_CDC_OP_SAVE_PWM_FREQ: {
    ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_PWM_FREQ };
    control_task_post(&c, 0);
} break;
```

(Payload is ignored — save uses the currently-active live freq, not the value carried in the payload. The protocol carries `freq_hz` for symmetry with the HID variant but it's advisory; the device authoritatively uses its own `pwm_gen_get`. This avoids transport-level race conditions where a stale freq is saved.)

- [ ] **Step 3: Build and commit**

```bash
idf.py build
git add components/net_dashboard/ws_handler.c components/usb_composite
git commit -m "feat(ws,cdc): save_pwm_freq op (uses live freq, payload advisory)"
```

### Task 2.5: HID transport for all save ops (introduces report 0x06)

This task introduces a new HID OUT report id `0x06` covering **all four** save ops: PWM_FREQ (0x01), RPM_PARAMS (0x02), RPM_TIMEOUT (0x03), UI_STEPS (0x04). UI_STEPS is wired in Phase 3 but its descriptor entry lands here.

**Files:**
- Modify: `components/usb_composite/include/usb_protocol.h`
- Modify: `components/usb_composite/usb_descriptors.c` (HID report descriptor + `_Static_assert`)
- Modify: `components/usb_composite/usb_composite.c` (HID_REPORT_DESC_SIZE macro)
- Modify: `components/usb_composite/usb_hid_task.c` (parser switch)

- [ ] **Step 1: Add report id and op codes to usb_protocol.h**

Append after the existing PSU HID block (around line 109 in `usb_protocol.h`):

```c
// ---- Settings save (HID) ---------------------------------------------------

#define USB_HID_REPORT_SETTINGS_SAVE  0x06   // OUT, 8 B (op + payload)

#define USB_HID_SAVE_OP_PWM_FREQ      0x01   // payload bytes 1..4 = u32 freq_hz (advisory)
#define USB_HID_SAVE_OP_RPM_PARAMS    0x02   // payload empty
#define USB_HID_SAVE_OP_RPM_TIMEOUT   0x03   // payload empty
#define USB_HID_SAVE_OP_UI_STEPS      0x04   // payload bytes 1..2 = u16 duty_step_x100, 3..4 = u16 freq_step

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  pad0;
    uint32_t u32_payload;        // freq_hz for op 0x01
    uint8_t  pad1[2];
} usb_hid_settings_save_t;

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  pad0;
    uint16_t duty_step_x100;     // 100 = 1.0%, 50 = 0.5%, etc.
    uint16_t freq_step;
    uint8_t  pad1[2];
} usb_hid_settings_save_steps_t;
```

- [ ] **Step 2: Add report 0x06 to HID report descriptor**

Open `components/usb_composite/usb_descriptors.c`. Find the `usb_hid_report_descriptor[]` array. Each existing report id (0x01–0x05) ends with a `HID_USAGE_PAGE_VENDOR` block roughly like:
```c
HID_USAGE_PAGE_N(0xFF00, 2),
HID_USAGE(0x00),
HID_COLLECTION(HID_COLLECTION_LOGICAL),
    HID_REPORT_ID(0x05),
    HID_USAGE(0x01),
    HID_LOGICAL_MIN(0),
    HID_LOGICAL_MAX_N(0xFF, 2),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(8),
    HID_OUTPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
HID_COLLECTION_END,
```

Append a clone with `HID_REPORT_ID(0x06)` and `HID_REPORT_COUNT(8)` (8 bytes payload).

The new collection adds 10 bytes to the descriptor. If the existing `_Static_assert(sizeof(usb_hid_report_descriptor) == 83)` is in place (per CLAUDE.md it's 83 with the PSU report), it must change to **93**.

- [ ] **Step 3: Update HID_REPORT_DESC_SIZE macro**

In `components/usb_composite/usb_composite.c` (around line 49 per CLAUDE.md):

Find:
```c
#define HID_REPORT_DESC_SIZE 83
```

Change to:
```c
#define HID_REPORT_DESC_SIZE 93
```

- [ ] **Step 4: Update _Static_assert in usb_descriptors.c**

Find:
```c
_Static_assert(sizeof(usb_hid_report_descriptor) == 83, ...);
```

Change to:
```c
_Static_assert(sizeof(usb_hid_report_descriptor) == 93, "report-desc size drift; sync HID_REPORT_DESC_SIZE in usb_composite.c");
```

- [ ] **Step 5: Build to verify descriptor sizes match**

Run: `idf.py build`
Expected: success. If size assertion fires, recount the bytes added in Step 2 and adjust 93 to match the new actual size — then update both Step 3 and Step 4 to the same number.

- [ ] **Step 6: Add HID parser case for report 0x06**

In `usb_hid_task.c`, find the existing switch on `report_id` (the one handling `USB_HID_REPORT_SET_PWM`, `USB_HID_REPORT_SET_RPM`, etc.). Add a new case:

```c
case USB_HID_REPORT_SETTINGS_SAVE: {
    if (len < 8) break;
    uint8_t op = buffer[1];   // buffer[0] is the report id, [1] is the op byte
    switch (op) {
    case USB_HID_SAVE_OP_PWM_FREQ: {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_PWM_FREQ };
        control_task_post(&c, 0);
    } break;
    case USB_HID_SAVE_OP_RPM_PARAMS: {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_PARAMS };
        control_task_post(&c, 0);
    } break;
    case USB_HID_SAVE_OP_RPM_TIMEOUT: {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_TIMEOUT };
        control_task_post(&c, 0);
    } break;
    case USB_HID_SAVE_OP_UI_STEPS: {
        // wired in Phase 3 task 3.4
    } break;
    default: break;
    }
} break;
```

(Adjust the indexing of `buffer[]` to match the surrounding code's convention — some code paths skip the report-id byte automatically, others don't. Look at the adjacent cases.)

- [ ] **Step 7: Build, flash, verify with PC tool stub**

```bash
idf.py build flash monitor
```

If a host tool with HID support exists, send a 9-byte report (`0x06` + 8 payload bytes, op=0x02). Otherwise visually inspect the descriptor by attaching a USB descriptor viewer. The flash succeeds and enumerates without TinyUSB errors → descriptor is valid; functional HID test is deferred.

- [ ] **Step 8: Commit**

```bash
git add components/usb_composite
git commit -m "feat(usb_hid): report 0x06 SETTINGS_SAVE (4 ops)

Single HID report id covers save_pwm_freq / save_rpm_params /
save_rpm_timeout / save_ui_steps. UI_STEPS handler stub; full wiring
arrives with Phase 3.

Descriptor grows 83→93 bytes; _Static_assert + HID_REPORT_DESC_SIZE
macro updated in lockstep.
"
```

### Task 2.6: CLI for PWM freq save

**Files:**
- Modify: `main/app_main.c`

- [ ] **Step 1: Add command handler and registration**

Same pattern as Task 1.6. Insert next to the other save commands:

```c
static struct { struct arg_end *end; } s_save_pwm_freq_args;
static int cmd_save_pwm_freq(int argc, char **argv)
{
    (void)argc; (void)argv;
    ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_PWM_FREQ };
    esp_err_t e = control_task_post(&c, pdMS_TO_TICKS(50));
    if (e != ESP_OK) { printf("post failed: %s\n", esp_err_to_name(e)); return 1; }
    printf("save_pwm_freq queued\n");
    return 0;
}
```

In the registration block:
```c
    s_save_pwm_freq_args.end = arg_end(0);
    const esp_console_cmd_t spf_cmd = {
        .command = "save_pwm_freq", .help = "persist current PWM freq to NVS",
        .hint = NULL, .func = cmd_save_pwm_freq, .argtable = &s_save_pwm_freq_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&spf_cmd));
```

- [ ] **Step 2: Build, flash, verify boot persistence**

```bash
idf.py build flash monitor
```

Test:
```
> pwm 5000 30
> save_pwm_freq
save_pwm_freq queued
I (...) pwm_gen: saved to NVS: freq_hz=5000
```

Reboot. Boot log should show:
```
I (...) main: boot pwm: freq=5000 duty=0 (from NVS or fallback)
```

Duty must read 0% in the dashboard / status frame after boot, even though 30% was saved. (We don't save duty.)

- [ ] **Step 3: Commit**

```bash
git add main/app_main.c
git commit -m "feat(cli): save_pwm_freq command

End of Phase 2 — PWM freq survives reboot; duty deliberately does
not. Verified manually: pwm 5000 30 → save → reboot → freq=5000
duty=0.
"
```

### Phase 2 acceptance

- [ ] Verified all four transports issue PWM freq save and survive reboot.
- [ ] Confirmed duty always boots to 0 regardless of saved value.
- [ ] First-boot fallback to 10 kHz works (after `idf.py erase-flash flash`).

---

## Phase 3 — UI step sizes (move localStorage → NVS)

### Task 3.1: ui_settings component scaffold

**Files:**
- Create: `components/ui_settings/CMakeLists.txt`
- Create: `components/ui_settings/include/ui_settings.h`
- Create: `components/ui_settings/ui_settings.c`

- [ ] **Step 1: Create CMakeLists.txt**

`components/ui_settings/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS         "ui_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES     log
                 nvs_flash
)
```

- [ ] **Step 2: Create header**

`components/ui_settings/include/ui_settings.h`:
```c
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Live values default to (1.0%, 100 Hz). Loaded from NVS on first call to
// ui_settings_init(); subsequent ui_settings_get_steps() calls read the
// in-memory cache (no NVS hit per access — telemetry runs at 20 Hz).
esp_err_t ui_settings_init(void);

void ui_settings_get_steps(float *duty_step, uint16_t *freq_step);
esp_err_t ui_settings_save_steps(float duty_step, uint16_t freq_step);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Create implementation**

`components/ui_settings/ui_settings.c`:
```c
#include "ui_settings.h"

#include <string.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ui_settings";

#define NVS_NAMESPACE   "ui_settings"
#define NVS_KEY_DUTY    "duty_step"
#define NVS_KEY_FREQ    "freq_step"

#define DEFAULT_DUTY_STEP   1.0f
#define DEFAULT_FREQ_STEP   100u

// Atomic-bit-punned float, same trick as control_task / rpm_cap.
static _Atomic uint32_t s_duty_bits;
static _Atomic uint16_t s_freq_step;

static inline void store_duty_f(float v)
{
    uint32_t b; memcpy(&b, &v, sizeof(b));
    atomic_store_explicit(&s_duty_bits, b, memory_order_relaxed);
}
static inline float load_duty_f(void)
{
    uint32_t b = atomic_load_explicit(&s_duty_bits, memory_order_relaxed);
    float v; memcpy(&v, &b, sizeof(v));
    return v;
}

esp_err_t ui_settings_init(void)
{
    store_duty_f(DEFAULT_DUTY_STEP);
    atomic_store_explicit(&s_freq_step, DEFAULT_FREQ_STEP, memory_order_relaxed);

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open ro failed: %s", esp_err_to_name(e));
        return ESP_OK;
    }
    float    d = DEFAULT_DUTY_STEP;
    uint16_t f = DEFAULT_FREQ_STEP;
    size_t sz = sizeof(d);
    if (nvs_get_blob(h, NVS_KEY_DUTY, &d, &sz) == ESP_OK && sz == sizeof(d) && d > 0.0f) {
        store_duty_f(d);
    }
    if (nvs_get_u16(h, NVS_KEY_FREQ, &f) == ESP_OK && f > 0) {
        atomic_store_explicit(&s_freq_step, f, memory_order_relaxed);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "init: duty_step=%.2f freq_step=%u", load_duty_f(),
             (unsigned)atomic_load(&s_freq_step));
    return ESP_OK;
}

void ui_settings_get_steps(float *duty_step, uint16_t *freq_step)
{
    if (duty_step) *duty_step = load_duty_f();
    if (freq_step) *freq_step = atomic_load_explicit(&s_freq_step, memory_order_relaxed);
}

esp_err_t ui_settings_save_steps(float duty_step, uint16_t freq_step)
{
    if (duty_step <= 0.0f) return ESP_ERR_INVALID_ARG;
    if (freq_step == 0)    return ESP_ERR_INVALID_ARG;

    store_duty_f(duty_step);
    atomic_store_explicit(&s_freq_step, freq_step, memory_order_relaxed);

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    esp_err_t e1 = nvs_set_blob(h, NVS_KEY_DUTY, &duty_step, sizeof(duty_step));
    esp_err_t e2 = nvs_set_u16 (h, NVS_KEY_FREQ, freq_step);
    if (e1 == ESP_OK && e2 == ESP_OK) nvs_commit(h);
    nvs_close(h);
    if (e1 != ESP_OK) return e1;
    if (e2 != ESP_OK) return e2;
    ESP_LOGI(TAG, "saved: duty_step=%.2f freq_step=%u", duty_step, (unsigned)freq_step);
    return ESP_OK;
}
```

- [ ] **Step 4: Build the new component standalone**

Run: `idf.py build`
Expected: builds clean. (`ui_settings` won't be referenced yet, but the component manager picks up new components automatically.)

- [ ] **Step 5: Commit**

```bash
git add components/ui_settings
git commit -m "feat(ui_settings): new component for slider step sizes (NVS-backed)

Single source of truth for duty_step / freq_step. Loaded once at boot;
get/set are atomic in-memory ops with NVS write only on save.
"
```

### Task 3.2: control_task save command

**Files:**
- Modify: `components/app_api/include/app_api.h`
- Modify: `main/control_task.c`

- [ ] **Step 1: Add CTRL_CMD_SAVE_UI_STEPS to enum and union**

In `app_api.h`, add after `CTRL_CMD_SAVE_PWM_FREQ,`:

```c
    CTRL_CMD_SAVE_UI_STEPS,
```

Inside the `union { ... }` block in `ctrl_cmd_t`, add:

```c
        struct { float duty_step; uint16_t freq_step; }   save_ui_steps;
```

- [ ] **Step 2: Handle in control_task**

In `main/control_task.c`, add `#include "ui_settings.h"` at the top, then add a new case:

```c
        case CTRL_CMD_SAVE_UI_STEPS: {
            esp_err_t e = ui_settings_save_steps(cmd.save_ui_steps.duty_step,
                                                  cmd.save_ui_steps.freq_step);
            if (e != ESP_OK) ESP_LOGW(TAG, "save_ui_steps failed: %s", esp_err_to_name(e));
        } break;
```

- [ ] **Step 3: Update main/CMakeLists.txt to require ui_settings**

Find `main/CMakeLists.txt`. Add `ui_settings` to its `REQUIRES`.

- [ ] **Step 4: Build and commit**

```bash
idf.py build
git add components/app_api main/control_task.c main/CMakeLists.txt
git commit -m "feat(control_task): SAVE_UI_STEPS cmd"
```

### Task 3.3: Init ui_settings + advertise in WS status frame

**Files:**
- Modify: `main/app_main.c`
- Modify: `components/net_dashboard/CMakeLists.txt`
- Modify: `components/net_dashboard/ws_handler.c`

- [ ] **Step 1: Init ui_settings before net_dashboard**

In `main/app_main.c`, find where `net_dashboard_init` (or equivalent HTTP server start) is called. Add `ESP_ERROR_CHECK(ui_settings_init());` immediately above it. Add `#include "ui_settings.h"` to includes.

- [ ] **Step 2: Add ui_settings to net_dashboard REQUIRES**

`components/net_dashboard/CMakeLists.txt` — add `ui_settings` to REQUIRES.

- [ ] **Step 3: Include step sizes in the 20 Hz status frame**

Locate the function that builds the WebSocket status JSON in `ws_handler.c` (search `"type":"status"`). It currently emits a JSON like `{type:"status", freq:..., duty:..., rpm:..., psu:{...}}`. Add a new top-level block:

```c
float duty_step; uint16_t freq_step;
ui_settings_get_steps(&duty_step, &freq_step);
// then in the JSON build:
//   ..., "ui":{"duty_step":<duty_step>,"freq_step":<freq_step>}
```

The exact emission technique (cJSON vs `snprintf` template) must match what the surrounding code uses.

- [ ] **Step 4: Add the WS save op handler**

After the `save_pwm_freq` block (Task 2.4):

```c
    } else if (strcmp(type_j->valuestring, "save_ui_steps") == 0) {
        const cJSON *ds = cJSON_GetObjectItem(root, "duty_step");
        const cJSON *fs = cJSON_GetObjectItem(root, "freq_step");
        if (cJSON_IsNumber(ds) && cJSON_IsNumber(fs)) {
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_SAVE_UI_STEPS,
                .save_ui_steps = {
                    .duty_step = (float)ds->valuedouble,
                    .freq_step = (uint16_t)fs->valuedouble,
                },
            };
            control_task_post(&c, 0);
            ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"save_ui_steps\",\"ok\":true}");
        } else {
            ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"save_ui_steps\",\"ok\":false}");
        }
```

- [ ] **Step 5: Build, flash, verify with browser dev tools**

```bash
idf.py build flash monitor
```

Open the dashboard in a browser → DevTools → WebSocket frames. Each status frame should now contain `"ui":{"duty_step":1.0,"freq_step":100}` (default values until the front-end Save sends new ones).

- [ ] **Step 6: Commit**

```bash
git add main components/net_dashboard
git commit -m "feat(ws): include ui step sizes in status frame; save_ui_steps op"
```

### Task 3.4: Wire HID + CDC for ui_steps save

**Files:**
- Modify: `components/usb_composite/usb_hid_task.c` (fill in the stub from Task 2.5)
- Modify: `components/usb_composite/usb_cdc_*.c`

- [ ] **Step 1: HID UI_STEPS handler**

Replace the empty stub in the `USB_HID_SAVE_OP_UI_STEPS` case (added in Task 2.5):

```c
    case USB_HID_SAVE_OP_UI_STEPS: {
        // payload bytes: [1]=op, [2..3]=duty_step_x100, [4..5]=freq_step
        uint16_t duty_x100, freq_step;
        memcpy(&duty_x100, &buffer[2], 2);
        memcpy(&freq_step, &buffer[4], 2);
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_SAVE_UI_STEPS,
            .save_ui_steps = {
                .duty_step = (float)duty_x100 / 100.0f,
                .freq_step = freq_step,
            },
        };
        control_task_post(&c, 0);
    } break;
```

- [ ] **Step 2: CDC UI_STEPS handler**

In the CDC op switch:

```c
case USB_CDC_OP_SAVE_UI_STEPS: {
    if (payload_len < 6) break;  // 4 B float + 2 B u16
    float    duty_step;
    uint16_t freq_step;
    memcpy(&duty_step, &payload[0], 4);
    memcpy(&freq_step, &payload[4], 2);
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_SAVE_UI_STEPS,
        .save_ui_steps = { .duty_step = duty_step, .freq_step = freq_step },
    };
    control_task_post(&c, 0);
} break;
```

- [ ] **Step 3: Build and commit**

```bash
idf.py build
git add components/usb_composite
git commit -m "feat(usb): HID/CDC handlers for save_ui_steps"
```

### Task 3.5: CLI for ui_steps save

**Files:**
- Modify: `main/app_main.c`

- [ ] **Step 1: Add command handler and registration**

```c
static struct {
    struct arg_dbl *duty;
    struct arg_int *freq;
    struct arg_end *end;
} s_save_ui_steps_args;

static int cmd_save_ui_steps(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_save_ui_steps_args);
    if (n != 0) { arg_print_errors(stderr, s_save_ui_steps_args.end, argv[0]); return 1; }
    float    d = (float)s_save_ui_steps_args.duty->dval[0];
    uint16_t f = (uint16_t)s_save_ui_steps_args.freq->ival[0];
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_SAVE_UI_STEPS,
        .save_ui_steps = { .duty_step = d, .freq_step = f },
    };
    esp_err_t e = control_task_post(&c, pdMS_TO_TICKS(50));
    if (e != ESP_OK) { printf("post failed: %s\n", esp_err_to_name(e)); return 1; }
    printf("save_ui_steps queued\n");
    return 0;
}
```

In the registration block:

```c
    s_save_ui_steps_args.duty = arg_dbl1(NULL, NULL, "<duty_step>", "duty step in percent (e.g. 0.5)");
    s_save_ui_steps_args.freq = arg_int1(NULL, NULL, "<freq_step>", "freq step in Hz");
    s_save_ui_steps_args.end  = arg_end(2);
    const esp_console_cmd_t sus_cmd = {
        .command = "save_ui_steps", .help = "persist slider step sizes to NVS",
        .hint = NULL, .func = cmd_save_ui_steps, .argtable = &s_save_ui_steps_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&sus_cmd));
```

- [ ] **Step 2: Build, flash, verify**

```bash
idf.py build flash monitor
```

Test:
```
> save_ui_steps 0.5 50
save_ui_steps queued
I (...) ui_settings: saved: duty_step=0.50 freq_step=50
```

Reboot, verify:
```
I (...) ui_settings: init: duty_step=0.50 freq_step=50
```

- [ ] **Step 3: Commit**

```bash
git add main/app_main.c
git commit -m "feat(cli): save_ui_steps <duty> <freq>"
```

### Task 3.6: Dashboard front-end — replace localStorage with WS-driven step sizes

**Files:**
- Modify: `components/net_dashboard/web/app.js` (around `app.js:421-444` + `app.js:723, 737`)
- Modify: `components/net_dashboard/web/index.html` (around line 50-53)

- [ ] **Step 1: Add Save button next to step inputs in index.html**

Around line 53 (the `<input id="duty-step">`), the existing markup looks like:
```html
<label for="duty-step" data-i18n="duty_step">Duty step (%)</label>
<input id="duty-step" type="number" step="0.1" min="0.1" value="1.0" />
```

Adapt the parent block (need to read it in the actual file to copy the exact pattern) so both step inputs share a single Save button:

```html
<button id="save-ui-steps-btn" data-i18n="save">Save</button>
<span id="save-ui-steps-status" class="save-status"></span>
```

(Single button saves both duty and freq step at once. Matches the "explicit save" UX agreed in spec.)

- [ ] **Step 2: Replace localStorage hooks with WS-driven values in app.js**

The existing code (around `app.js:421-444`) reads `localStorage.getItem(stepStorageKey)` and writes back on `change`. Replace with:

```javascript
function applyStepFromServer(uiBlock) {
  if (!uiBlock) return;
  if (typeof uiBlock.duty_step === 'number') {
    document.getElementById('duty-step').value = uiBlock.duty_step;
    const dutySlider = document.getElementById('duty-slider');
    if (dutySlider) dutySlider.step = String(uiBlock.duty_step);
  }
  if (typeof uiBlock.freq_step === 'number') {
    document.getElementById('freq-step').value = uiBlock.freq_step;
    const freqSlider = document.getElementById('freq-slider');
    if (freqSlider) freqSlider.step = String(uiBlock.freq_step);
  }
}
```

In the WS message handler (search `'status'` case), call `applyStepFromServer(msg.ui)` after updating freq/duty/rpm.

For the Save button:
```javascript
document.getElementById('save-ui-steps-btn').addEventListener('click', () => {
  const duty = parseFloat(document.getElementById('duty-step').value);
  const freq = parseInt(document.getElementById('freq-step').value, 10);
  if (!(duty > 0) || !(freq > 0)) { /* show error */ return; }
  ws.send(JSON.stringify({ type: 'save_ui_steps', duty_step: duty, freq_step: freq }));
});
```

In the WS message handler, on receiving `{type:"ack",op:"save_ui_steps",ok:true}`, briefly show "Saved" in `#save-ui-steps-status`.

- [ ] **Step 3: Remove all `stepStorageKey` references**

Search `app.js` for `stepStorageKey`, `fan-testkit:duty-step`, `fan-testkit:freq-step`. Remove every read/write of these — the WS status frame is now the source of truth. The constants `'fan-testkit:duty-step'` / `'fan-testkit:freq-step'` and the `stepStorageKey` config field can be deleted.

- [ ] **Step 4: Build + manual test**

```bash
idf.py build flash monitor
```

Open dashboard. Steps display default (1.0% / 100). Change to 0.5% / 50, click Save. Browser status shows "Saved". Reload page → values persist (came back from device). Open dashboard from a different browser/device → same values display (this is the whole point of the change).

- [ ] **Step 5: Commit**

```bash
git add components/net_dashboard/web
git commit -m "feat(dashboard): step sizes saved to device, not browser

Removes localStorage persistence; step sizes now arrive in the WS
status frame and are saved via save_ui_steps op. Cross-browser /
cross-device parity by construction.
"
```

### Phase 3 acceptance

- [ ] Dashboard: change steps → Save → reload → values persist.
- [ ] Different browser: same step values display (no localStorage dependency).
- [ ] CLI: `save_ui_steps 0.5 50` → boot log shows persisted values.
- [ ] HID/CDC: payload-level test deferred to PC tool integration.

---

## End-of-implementation acceptance (all phases)

- [ ] `idf.py erase-flash flash monitor` — first boot uses Kconfig defaults, no NVS noise.
- [ ] All seven Save commands work via WS:
  - `save_rpm_params`, `save_rpm_timeout`, `save_pwm_freq`, `save_ui_steps`
- [ ] All seven Save commands work via CLI:
  - `save_rpm_params`, `save_rpm_timeout`, `save_pwm_freq`, `save_ui_steps <d> <f>`
- [ ] CDC ops 0x50–0x53 dispatch correctly (verify via PC tool or `idf.py monitor` log).
- [ ] HID report 0x06 with op codes 0x01–0x04 dispatch correctly (verify via PC tool).
- [ ] Reboot scenarios:
  - Saved RPM params restore on boot.
  - Saved PWM freq restores; **duty always boots to 0** regardless of saved value.
  - Saved UI step sizes apply to dashboard from any browser.
- [ ] No regressions in existing functionality (set_pwm, set_rpm, factory_reset, PSU, GPIO).

---

## Notes for the engineer

- **NVS partition size**: this plan adds 6 new keys spread across 3 namespaces. Defaults (3 KB NVS) have plenty of room.
- **First-boot path**: every load helper checks `ESP_ERR_NVS_NOT_FOUND` and silently falls back to defaults — no error logs on virgin flash.
- **Why payload is "advisory" for `USB_CDC_OP_SAVE_PWM_FREQ`**: see comment in Task 2.4 Step 2.
- **Why HID gets one combined report id (0x06) instead of four separate reports**: HID descriptor space is finite and every new id costs ~10 bytes of descriptor + a `_Static_assert` update. Combining keeps descriptor churn to one edit.
- **CLAUDE.md invariant preserved**: every Save flows through `control_task` via `ctrl_cmd_t`. No transport file holds Save logic.
- **CLAUDE.md update needed at the end**: brief mention of NVS namespaces + boot policy ("PWM freq restores; duty always 0 on boot") in the appropriate section.
