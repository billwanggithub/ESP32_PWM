# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Hardware target

**YD-ESP32-S3-COREBOARD V1.4** (ESP32-S3-WROOM-1, 16 MB flash, 8 MB octal
PSRAM — the N16R8 variant). Schematic is in `doc/YD-ESP32-S3-SCH-V1.4.pdf`;
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

```
idf.py set-target esp32s3     # once
idf.py build
idf.py -p COM3 encrypted-flash monitor    # first flash uses encrypted-flash
idf.py -p COM3 flash monitor              # subsequent flashes
```

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
`mcpwm_timer_stop` / restart in the update path — that's where glitches
come from. The 1 MHz upper bound is set by the 160 MHz source clock; at
1 MHz duty resolution drops to 7 bits (exposed via
`pwm_gen_duty_resolution_bits()` for the UI).

The "change-trigger output" on a separate GPIO is a software pulse from
`control_task` after the write succeeds — not a hardware sync output. If
jitter on that trigger matters, wire it to an MCPWM ETM event instead.

## Security posture

Secure Boot V2 + Flash Encryption are both enabled by default in
**development mode** (re-flashable key, JTAG available). The one-way
transition to release mode is documented step-by-step in
`docs/release-hardening.md` — every step burns eFuses and is irreversible,
so it's a checklist the human runs, not a script. Don't automate it.

`secure_boot_signing_key.pem` is gitignored; every developer generates
their own with `espsecure.py generate_signing_key --version 2`.

## esp_tinyusb pinning

Pinned to `espressif/esp_tinyusb ~1.7.0` (1.x series) via
`main/idf_component.yml`. The 2.x rewrite requires hand-building the
composite configuration descriptor (`tinyusb_desc_config_t.full_speed_config`).
1.x auto-generates HID+CDC composite descriptors from Kconfig, which is
what this firmware relies on. If you upgrade to 2.x,
`components/usb_composite/usb_composite.c` and `usb_descriptors.c` both
need rewriting, and the 3 Kconfig options for TinyUSB in
`sdkconfig.defaults` change meaning.

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
