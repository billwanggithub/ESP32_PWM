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

ESP-IDF lives at `C:\Espressif\frameworks\esp-idf-v5.5.1`. Activate with
the Start-menu **"ESP-IDF 5.5.1 CMD"** shortcut (easiest), or
`export.bat` / `export.ps1` from cmd/PowerShell. `export.sh` only works
in Git Bash/MSYS2.

```bash
idf.py set-target esp32s3     # once
idf.py build
idf.py -p COM3 flash monitor  # plain-text flash (current dev build has no Secure Boot)
```

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

Frequency range is **1 Hz ~ 1 MHz**. The 16-bit MCPWM counter
(`MCPWM_LL_MAX_COUNT_VALUE = 0x10000`) cannot span 6 decades with a
single fixed `resolution_hz`, so `pwm_gen.c` defines a 3-band table and
picks a band per call:

| Band | resolution_hz | freq range     | period_ticks   | duty bits |
|------|---------------|----------------|----------------|-----------|
| HI   | 10 MHz        | 153 Hz – 1 MHz | 10 – 65359     | 3.3 – 16  |
| MID  | 160 kHz       | 3 Hz – 152 Hz  | 1052 – 53333   | 10 – 16   |
| LO   | 1 kHz         | 1 Hz – 2 Hz    | 500 – 1000     | 9 – 10    |

**Same-band updates** (e.g. 500 Hz → 600 Hz) stay glitch-free via TEZ.
**Band-crossing updates** (e.g. 200 Hz → 100 Hz) go through
`reconfigure_for_band()`: stop → disable → delete timer → `mcpwm_new_timer`
with the new `resolution_hz` → reconnect operator → re-arm comparator →
enable → start. This produces a brief (~tens of µs) output discontinuity.
Operator, comparator, and generator objects are retained across the
reconfigure so their action config (high on TEZ, low on compare) does
not need to be re-registered.

The actual bit count at any freq is exposed via
`pwm_gen_duty_resolution_bits()` for the UI. If future requirements need
sub-Hz, the MCPWM counter can't stretch further — an LEDC-based second
generator or software timer would be needed.

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

## esp_tinyusb pinning + composite descriptor

Pinned to `espressif/esp_tinyusb ~1.7.0` via `main/idf_component.yml`。

**Important**: 1.7.x 的 default config descriptor **不處理 HID**（只 cover
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

## Wire protocols (host tool contracts)

- **HID report IDs** and **CDC SLIP frame ops** are defined in
  `components/usb_composite/include/usb_protocol.h`. These are the contract
  with the PC host tool (separate project, out of this repo's scope).
  Changing payload shapes is a breaking change.
- **WebSocket JSON** contract: `{type: "set_pwm" | "set_rpm" | ...}` for
  client→device, `{type: "status" | "ack" | "ota_progress"}` for
  device→client. Documented inline in `components/net_dashboard/ws_handler.c`.

## Interaction & communication preferences

All responses and commit-message bodies: **晶晶體** (Traditional Chinese +
English code-switching). Code, command names, and technical keywords stay
in English. Commit-message titles can be English-first for git log
readability.

Flowcharts in docs and comments use **ASCII tree format** (`├─` `└─` `│`),
not Mermaid.
