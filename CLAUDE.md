# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Hardware target

**YD-ESP32-S3-COREBOARD V1.4** (ESP32-S3-WROOM-1, 16 MB flash, 8 MB octal
PSRAM — the N16R8 variant). Schematic is in `docs/YD-ESP32-S3-SCH-V1.4.pdf`;
consult it before allocating new GPIOs. The full design spec lives at
`C:\Users\billw\.claude\plans\read-the-project-plan-pure-eagle.md`.

Two USB-C ports with distinct roles:

- **USB1** — CH343P USB-UART bridge on UART0. Serial console + `idf.py flash`
  auto-reset. Always available for logs regardless of other config.
- **USB2** — native USB D-/D+ on GPIO19/GPIO20. Routes to either the
  USB-JTAG peripheral or TinyUSB via a `USB-JTAG` / `USB-OTG` 0 Ω jumper.
  **TinyUSB composite (HID + CDC) requires the `USB-OTG` jumper to be
  bridged.** Users often hit this on first run.

Pins reserved by hardware: **19, 20** (USB). Strapping pins to avoid for
critical outputs: 0 (BOOT), 3 (USB-JTAG select), 45, 46. Onboard WS2812
RGB LED is on GPIO48.

## Build & flash workflow (Windows)

ESP-IDF v6.0 lives at `C:\esp\v6.0\esp-idf`. (We migrated up from v5.5.1
in 2026-04 — see `HANDOFF.md` for the migration notes.) Activate with
either:

- Desktop shortcut **"ESP-IDF 6.0 PWM Project"** — opens a new PowerShell
  with env active and cwd already at this project. Easiest for daily work.
- `esp6 pwm` PowerShell alias — activates v6.0 in the current PowerShell
  window AND cd's to this project. Defined in
  `D:\Documents\WindowsPowerShell\Microsoft.PowerShell_profile.ps1`.
- `C:\esp\v6.0\esp-idf\export.ps1` (PowerShell) or `export.bat` (cmd) —
  raw activation; no auto-cd. `export.sh` only works in Git Bash/MSYS2.

```bash
idf.py set-target esp32s3     # once (already done; sdkconfig.defaults pins it)
idf.py build
idf.py -p COM24 flash monitor # plain-text flash (current dev build has no Secure Boot)
```

The previous v5.5.1 install at `C:\Espressif\frameworks\esp-idf-v5.5.1`
is still on disk; don't `export` it inside a v6.0 shell — Python venv
collisions. v5.5.1 is reachable via the original installer Start-menu
shortcut **"ESP-IDF 5.5.1 CMD"** if you ever need to fall back.

現階段 `sdkconfig.defaults` 把 `CONFIG_SECURE_BOOT` 跟
`CONFIG_SECURE_FLASH_ENC_ENABLED` 都關掉 (temporary workaround)，所以
**不要** 跑 `encrypted-flash` — 那是 eFuse 已經燒過 flash-enc key 之後
的 re-flash 指令，在全新板子上會直接失敗。Secure Boot 重開的路徑
待做，詳見 `HANDOFF.md`。

### sdkconfig trap (hit repeatedly in this project)

`idf.py fullclean` wipes `build/` but **keeps `sdkconfig`**. Any change to
`sdkconfig.defaults` for a symbol already present in `sdkconfig` is silently
ignored. When modifying partition layout, Secure Boot, Flash Encryption, or
TinyUSB Kconfig:

```
del sdkconfig
idf.py fullclean
idf.py build
```

### Kconfig choice groups must be fully stated

For `choice` groups like `CONFIG_PARTITION_TABLE_TYPE`, setting only the
winning member (`CUSTOM=y`) isn't enough if `sdkconfig` has a stale `=y`
on a sibling (`TWO_OTA=y`). The sdkconfig merge picks "last wins" and
silently uses the built-in default partition table. `sdkconfig.defaults`
explicitly sets all siblings to `=n`; preserve this pattern for any other
choice group you touch.

### CH343 DTR/RTS auto-program trap — chip gets stuck in download mode

Symptom: after a successful flash, the serial monitor shows
`rst:0x1 (POWERON),boot:0x0 (DOWNLOAD(USB/UART0)) waiting for download`
on every reset instead of running the app. BOOT button is not pressed,
flash hashes verify, but the app never runs.

Root cause: the YD-ESP32-S3 has an "Auto program" transistor circuit on
the CH343's DTR/RTS lines (schematic `docs/YD-ESP32-S3-SCH-V1.4.pdf`,
page 1, Q1/Q2). The truth table:

```
DTR  RTS  -->  EN   IO0
 1    1         1    1     <- run mode (idle)
 1    0         0    1     <- reset asserted
 0    1         1    0     <- download mode (GPIO0 low at boot)
```

Some CH343 driver + pySerial combinations leave RTS asserted (RTS=1
while DTR drops to 0 at port close), which pulls GPIO0 low so the chip
boots into download mode on the next reset. `idf.py monitor` re-triggers
this every time it opens the port, so the chip never escapes.

**Fix** — when opening a serial tool to interact with the REPL:

- Use a terminal that lets you **explicitly hold DTR=1 and RTS=1** on
  port open (串口調試助手 / SerialTool works — check both DTR and RTS
  boxes before 打開). PuTTY by default does not toggle these and works.
- Or: `idf.py -p COMn monitor --no-reset` skips the reset-on-open
  sequence.
- Or: physical power cycle (unplug USB, wait 2 s, replug) — boots
  cleanly because no host has the port open yet.

Do **not** try to solve this by removing R5/R7/Q1/Q2 — esptool's auto
program sequence on flash depends on them. Only monitor-open is the
problem.

## Architecture — two invariants that must hold

### 1. Single logical handler, multiple transport frontends

Both **setpoint control** and **firmware update** have exactly one
implementation with two transport frontends:

```
Wi-Fi WebSocket ──┐                             ┌── /ota POST ──┐
                  ├──► control_task (setpoints) │                ├──► ota_writer_task
USB HID reports ──┘                             └── CDC frames ──┘     (esp_ota_*)
```

Frontends translate protocol only. Core logic is written once. A third
transport (e.g. Ethernet) should plug in as a new frontend feeding the same
`ctrl_cmd_queue` / `ota_core_*` APIs — never duplicate the business logic.

### 2. Components never depend on `main`

ESP-IDF's `main` component cannot be a `REQUIRES` dependency. Shared types
like `ctrl_cmd_t` live in `components/app_api/include/app_api.h`. When
adding a new cross-component API, put the header in `app_api/` (or a new
dedicated component) — don't let any component `REQUIRES main`.

## Task topology (FreeRTOS)

```
priority 6  control_task                 owns PWM setpoints, drains ctrl_cmd_queue
priority 5  rpm_converter_task           freq_fifo → period→RPM → rpm_fifo
priority 4  rpm_averager_task            sliding avg → atomic latest_rpm + history
priority 3  httpd (ESP-IDF)              HTTP + WebSocket
priority 3  usb_hid_task                 HID OUT parse; IN @ 50 Hz from latest_rpm
priority 2  usb_cdc_tx/rx                CDC log mirror + SLIP OTA frames
priority 2  telemetry_task               20 Hz WebSocket status push
priority 2  ota_writer_task              single esp_ota_* writer (mutex-guarded)
```

Invariants:

- **Lock-free SPSC ring buffers** (`freq_fifo`, `rpm_fifo`) connect ISR→task
  and task→task. Don't replace with `xQueue` without measuring — the capture
  ISR runs at up to MHz rates.
- `latest_rpm` is an **atomic float bit-punned through `uint32_t`**, relaxed
  ordering. One-sample staleness is acceptable; don't add a mutex.
- **RPM timeout sentinel**: when no edge arrives within `rpm_timeout_us`,
  the timeout callback pushes a period value with `0x80000000` OR'd in.
  The converter task recognises this sentinel bit and emits `0.0 RPM`.
  Default timeout is 1 second. Preserve this mechanism when editing
  `rpm_cap.c`.

## PWM glitch-free update mechanism

`pwm_gen_set()` writes new period and compare values; both latch on
`MCPWM_TIMER_EVENT_EMPTY` (TEZ — timer equals zero) via the
`update_cmp_on_tez` flag on the comparator. **Do not** call
`mcpwm_timer_stop` / restart in the update path for same-band changes —
that's where glitches come from.

Frequency range is **10 Hz ~ 1 MHz**. The 16-bit MCPWM counter
(`MCPWM_LL_MAX_COUNT_VALUE = 0x10000`) cannot span that range with a
single fixed `resolution_hz`, so `pwm_gen.c` defines a 2-band table and
picks a band per call:

| Band | resolution_hz | freq range       | period_ticks   | duty bits |
|------|---------------|------------------|----------------|-----------|
| HI   | 10 MHz        | 153 Hz – 1 MHz   | 10 – 65359     | 3.3 – 16  |
| LO   | 625 kHz       | 10 Hz – 152 Hz   | 4112 – 62500   | 12 – 16   |

**Critical constraint — MCPWM group prescaler is committed on first use
and cannot be changed.** Once `mcpwm_new_timer()` runs the first time,
`group->prescale` is locked. Every subsequent timer in the same group
must share that group_prescale; only the per-timer `timer_prescale`
(range [1..256]) can vary. That gives a max 256× spread of resolutions
within one group.

**ESP-IDF v6.0 changed `MCPWM_GROUP_CLOCK_DEFAULT_PRESCALE` from 2 to
1** (see `esp_driver_mcpwm/src/mcpwm_private.h:55`). Group clock is now
160 MHz (was 80 MHz). At gp=1 the resolution range is 160 MHz down to
160 MHz / 256 = 625 kHz. HI=10 MHz uses timer_prescale=16; LO=625 kHz
uses timer_prescale=256. Both fit within gp=1's range, so cross-band
swaps don't trigger `"group prescale conflict"` errors.

The driver does not expose a public knob to force a different
group_prescale (`mcpwm_timer_config_t` has no such field), and the
auto-resolver in `mcpwm_set_prescale` greedily picks the smallest
group_prescale that satisfies the first call. That locks us at gp=1
forever once HI band's first `new_timer` runs.

1 Hz – 9 Hz cannot be reached at gp=1 + 16-bit counter. Extending
below 10 Hz requires switching the generator to LEDC (separate
peripheral with fractional dividers) or sharing group 1 with a second
operator/generator chain (but only one generator can drive the same
GPIO at a time, so cross-band swaps would need full generator
delete+recreate — slower than the current TEZ-based reconfigure). Both
are bigger refactors.

### v6.0 band-cross workaround (timing-sensitive)

After the v6.0 migration, the first HI→LO band cross emits an output
frequency 16× higher than requested (e.g. 100 Hz outputs as 1.6 kHz —
period correct, but timer_prescale 16 from HI carries over instead of
LO's 256). The fix that consistently works:

1. `sdkconfig.defaults` sets `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` so the
   driver's `LOGD` calls are compiled in (not no-op'd).
2. `pwm_gen_init()` calls `esp_log_level_set("mcpwm", ESP_LOG_DEBUG)`
   so those compiled-in calls actually fire at runtime.

Removing **either** piece reproduces the bug. Suspected cause: the
microseconds spent inside the driver's `LOGD` argument formatting let
some MCPWM register write settle between writes; without that delay,
the next `read` sees stale state. Root cause is **not isolated** —
treated as a known-good workaround for now. Full bisect would need a
minimal repro project (no Wi-Fi/USB) and probably register-level traces.

**If you remove either the Kconfig or the runtime call, scope GPIO4
after a `pwm 100 50` first** to confirm you didn't bring the bug back.

**Same-band updates** (e.g. 500 Hz → 600 Hz) stay glitch-free via TEZ.
**Band-crossing updates** (152 Hz ↔ 153 Hz) go through
`reconfigure_for_band()`: stop → disable → delete timer → `mcpwm_new_timer`
with the new `resolution_hz` → reconnect operator → re-arm comparator →
enable → start. This produces a brief (~tens of µs) output discontinuity.
Operator, comparator, and generator objects are retained across the
reconfigure so their action config (high on TEZ, low on compare) does
not need to be re-registered.

The actual bit count at any freq is exposed via
`pwm_gen_duty_resolution_bits()` for the UI.

The "change-trigger output" on a separate GPIO is a software pulse from
`control_task` after the write succeeds — not a hardware sync output.
Pulse width is clamped to [200 µs, 1000 µs] so it's always cleanly
observable regardless of PWM freq. If jitter on that trigger matters,
wire it to an MCPWM ETM event instead.

## Security posture — 目前 disabled

`sdkconfig.defaults` 現階段把 `CONFIG_SECURE_BOOT` 跟
`CONFIG_SECURE_FLASH_ENC_ENABLED` 都設 `n` (temporary workaround)，因為
全新 eFuse 板子第一次 boot 這兩個 feature 都開時會 boot loop (`Saved PC`
指 `process_segment @ esp_image_format.c:622`)，root cause 待定位。

要加回 security 的順序：先單獨啟用 Secure Boot V2（不開 Flash Enc）確認
能 boot；再單獨啟用 Flash Enc（不開 SB）；兩者都能單獨 work 再 combined。
這個 bisect 還沒做，詳情見 `HANDOFF.md` 的 Bug 1 段。

Release-mode 的 irreversible eFuse checklist 仍在 `docs/release-hardening.md`；
Step 0 要改寫成「先確認 non-secure build 全功能驗證過，再單 feature
啟用 security」— 現在的 Step 0 不對。

`secure_boot_signing_key.pem` 已 gitignored；每個 dev 自己用
`espsecure.py generate_signing_key --version 2` 產。

## Component manager dependencies

`main/idf_component.yml` 上目前 pin 三個 component：

- **`espressif/esp_tinyusb ~1.7.0`** — composite HID + CDC descriptor。
  v6.0 IDF 跟 1.7.x 仍相容（驗證過 build + 枚舉 OK）。
- **`espressif/cjson ^1.7.19`** — v6.0 把 IDF built-in `json` component
  拔掉，cJSON 改 component manager 上 `espressif/cjson`（直接 depend，
  不是 transitive）。`net_dashboard/CMakeLists.txt` 的 REQUIRES 寫
  `espressif__cjson`。
- **`espressif/mdns ^1.8.0`** — mDNS hostname `esp32-pwm.local` 廣告
  用。v6.0 把 built-in `mdns` component 拔掉搬到 component manager，
  所以 `net_dashboard/CMakeLists.txt` 的 REQUIRES 要用 namespaced name
  `espressif__mdns`（不是 v5.x 的 `mdns`）。

Wi-Fi provisioning 現在走 SoftAP + captive portal，不再依賴 BLE 或
任何外部 provisioning component — 板子第一次 boot 沒有 creds 時會開
`ESP32-PWM-setup` 這個 open AP，phone 接上後 Android captive-portal
detector 會自動跳 browser。成功後 success page 同時秀 `esp32-pwm.local`
跟 raw IP。細節見 `components/net_dashboard/provisioning.c` 跟 spec
`docs/superpowers/specs/2026-04-22-softap-captive-portal-design.md`。

### esp_tinyusb 1.7.x 的 HID 限制

1.7.x 的 default config descriptor **不處理 HID**（只 cover
CDC/MSC/NCM/VENDOR）。開 `CFG_TUD_HID > 0` 就必須提供自製
`configuration_descriptor`，否則 `tinyusb_set_descriptors` 會 reject。
`usb_composite.c` 的 `s_configuration_descriptor[]` 就是這個 —— IF0 HID,
IF1+IF2 CDC (via IAD)，EP 0x81 HID IN, 0x82 CDC notif IN, 0x03 CDC OUT,
0x83 CDC IN。

TinyUSB 的 `TUD_HID_DESCRIPTOR()` 要 compile-time report desc length，
所以 `HID_REPORT_DESC_SIZE` 寫死 `53` 並在 `usb_descriptors.c` 用
`_Static_assert(sizeof(usb_hid_report_descriptor) == 53)` 綁住，改 report
descriptor 時 compile error 會立刻提醒你同步更新。

升級到 esp_tinyusb 2.x 時 `tinyusb_config_t` 改用
`full_speed_config / high_speed_config`，上述 descriptor bytes 格式不變，
但 struct field name 要跟著改。

### v6.0 driver split (主要 component REQUIRES 變動)

v5.x 的 `driver` 變成多 per-peripheral components。如果某個 component
include `driver/uart.h` / `driver/gpio.h` / `driver/i2c.h` 之類 header
卻 build error 「no such file」，是 v6.0 抽掉 `driver` umbrella 對應
header 的 transitive include。在 component CMakeLists 加：

| header               | REQUIRES                |
|----------------------|-------------------------|
| `driver/uart.h`      | `esp_driver_uart`       |
| `driver/gpio.h`      | `esp_driver_gpio`       |
| `driver/i2c.h`       | `esp_driver_i2c`        |
| `driver/mcpwm_*.h`   | `esp_driver_mcpwm`      |
| `freertos/ringbuf.h` | `esp_ringbuf`           |

`driver` umbrella 仍存在但只有 i2c/touch_sensor/twai 還掛在底下，其他
都搬走了。

## Wire protocols (host tool contracts)

- **HID report IDs** and **CDC SLIP frame ops** are defined in
  `components/usb_composite/include/usb_protocol.h`. These are the contract
  with the PC host tool (separate project, out of this repo's scope).
  Changing payload shapes is a breaking change.
- **WebSocket JSON** contract: `{type: "set_pwm" | "set_rpm" |
  "factory_reset"}` for client→device, `{type: "status" | "ack" |
  "ota_progress"}` for device→client. Documented inline in
  `components/net_dashboard/ws_handler.c`.

## Factory reset / reprovision (4 transports, 1 core)

Instance of the "single handler, multiple frontends" invariant. Every
reset entry point lands on `net_dashboard_factory_reset()`
(`components/net_dashboard/include/net_dashboard.h`), which wipes
stored Wi-Fi credentials via `prov_clear_credentials()` (calls
`esp_wifi_restore()`) and calls `esp_restart()`. Next boot the device
sees no credentials → SoftAP `ESP32-PWM-setup` opens for captive-portal
provisioning.

```
Web UI confirm      ──┐
USB HID 0x03 + 0xA5 ──┤
USB CDC 0x20 + 0xA5 ──┼──► net_dashboard_factory_reset()
BOOT long-press ≥3s ──┘       ├── prov_clear_credentials()
                              └── esp_restart()
```

Design notes:

- **Magic-byte guard on USB paths** (`USB_HID_FACTORY_RESET_MAGIC =
  0xA5`, `USB_CDC_FACTORY_RESET_MAGIC = 0xA5`) so a stray zeroed
  report can't wipe credentials. Web UI uses a JS `confirm()` dialog;
  BOOT uses a 3-second hold (50 ms poll cadence in
  `boot_button_task`, GPIO0 with internal pull-up).
- **200 ms restart delay** in `factory_reset_task` lets each
  transport's ack frame (WS `{type:"ack",op:"factory_reset"}`, CDC op
  `0x21`, HID report `0x03` silent) flush before the reset interrupts
  the connection.
- **Idempotent** — second call inside the 200 ms window is a no-op
  via a static volatile flag. BOOT long-press triggers spawn exactly
  one reset even if the input glitches.
- **GPIO0 runtime reuse**: GPIO0 is a strapping pin but only sampled
  at reset for BOOT/DOWNLOAD selection. After boot it's a normal
  input that any app can read.
- **Cross-component coupling**: `usb_composite` REQUIRES
  `net_dashboard` so HID/CDC callbacks can reach the reset API.
  Direction is acceptable — `net_dashboard` is the "network state"
  component and factory reset *is* a network-state concern
  (credentials).
- **HID descriptor size**: adding report id `0x03` grew
  `usb_hid_report_descriptor` from 53 → 63 bytes. The
  `_Static_assert(sizeof(...) == 63)` in
  `usb_composite/usb_descriptors.c` and the `HID_REPORT_DESC_SIZE`
  macro in `usb_composite.c:49` keep the two in sync — any future
  descriptor edit triggers a compile error on mismatch.

## Interaction & communication preferences

All responses and commit-message bodies: **晶晶體** (Traditional Chinese +
English code-switching). Code, command names, and technical keywords stay
in English. Commit-message titles can be English-first for git log
readability.

Flowcharts in docs and comments use **ASCII tree format** (`├─` `└─` `│`),
not Mermaid.
