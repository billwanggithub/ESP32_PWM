# Hand-off — ESP32-S3 PWM + RPM firmware

Date: 2026-04-22 (晚間，IDF v6.0 migration)
Branch: `chore/migrate-idf-v6.0` (off `feat/pwm-1hz-floor`)
Working dir: `D:\github\ESP32_PWM`
IDF: `C:\esp\v6.0\esp-idf` (was `C:\Espressif\frameworks\esp-idf-v5.5.1`)

## 2026-04-22 — provisioning migration: BLE → SoftAP captive portal

Dropped `espressif/network_provisioning` + BLE (NimBLE) + PoP-based
protocomm SECURITY_1. Replaced with in-project SoftAP + captive portal
flow (components/net_dashboard/captive_portal.c, dns_hijack.c,
mdns_svc.c). Saves ~60 KB flash, removes the Espressif BLE Provisioning
Android app requirement.

User flow: on boot no creds → AP `ESP32-PWM-setup` + DNS hijack (every
query → 192.168.4.1) + catch-all HTTP 200 with meta-refresh + RFC 8908
`Link: rel="captive-portal"` header. After submit + STA connect, success
page shows `esp32-pwm.local` and raw DHCP IP. AP torn down ~25 s later.
Factory reset path (`prov_clear_credentials` → `esp_wifi_restore`)
unchanged for callers.

Hardware validation on 2026-04-22 (Samsung phone, One UI):

- PC (Windows with Bonjour): `http://esp32-pwm.local/` resolves, dashboard
  loads.
- Samsung phone, STA side: raw-IP link works; `.local` mDNS does NOT
  resolve (Chrome on Android doesn't do mDNS — long-standing Chromium
  limitation). The success page intentionally shows the raw IP for
  exactly this fallback.
- Samsung phone, captive-portal auto-popup: **does not fire on this
  phone**. Tried 302, 200 + HTML body, RFC 8908 Link header, POST/HEAD
  catch-all, and a port-443 accept+close helper — none made Samsung
  raise the "Sign in to Wi-Fi network" UI. The phone issues exactly
  one malformed HTTP probe (~15 s after DHCP) and gives up silently.
  Suspected cause: Samsung OneUI / carrier policy disables HTTP probe
  fallback and uses HTTPS-only detection.
- Samsung phone, manual flow: works — join AP, open browser, navigate
  to anything (DNS hijack redirects to `192.168.4.1`), setup page loads,
  submit credentials, success page renders, done.

Open items:

- iOS not validated. Protocol identical, and iOS captive-portal detection
  is historically more reliable than Android's; likely works out of the
  box, but confirm before shipping to iOS users.
- STA failure doesn't fall back to AP at runtime — user must factory-reset
  to re-enter setup. Acceptable per spec (Q4 in brainstorming).
- Auto-popup on Samsung is **not fixable in firmware**. If we want a
  guaranteed auto-open experience, the options are: (a) ship a
  companion Android app that uses `NsdManager` to find the device after
  provisioning, or (b) print a QR code on the device label that points
  to the setup URL so the user scans instead of relying on captive
  detection. Both are out of scope for this branch.

## 2026-04-22 晚 — factory reset / re-provisioning 四路打通 (commit `7ef24d6`)

之前沒有 runtime 路徑可以清 Wi-Fi credentials，要重新 provision 一定得
`idf.py erase-flash`。現在四種 transport 都能觸發，共用
`net_dashboard_factory_reset()` 當單一 entry point:

1. **Web UI** — dashboard 新增紅色 "Factory reset" 按鈕 + JS `confirm()`
   dialog；WS 送 `{type:"factory_reset"}`，收到 ack 後 device 自己 reboot。
2. **USB HID** — report id `0x03`，1 byte payload，magic `0xA5`。HID
   descriptor 從 53 → 63 bytes，`_Static_assert` 守著。
3. **USB CDC** — SLIP op `0x20` (H→D) + `0x21` (D→H ack)，同樣 magic
   `0xA5` guard。
4. **BOOT 按鈕 (GPIO0)** — 長按 ≥3 秒觸發，50 ms poll cadence。GPIO0 是
   strapping pin 但 runtime 可以當一般 input 用。

核心實作在 `components/net_dashboard/net_dashboard.c` 的
`net_dashboard_factory_reset()` + `factory_reset_task` (200 ms delay 讓
ack frame flush)；wipe 用 `network_prov_mgr_reset_wifi_provisioning()`；
低階 wipe function 叫 `prov_clear_credentials()` 擺在 `prov_internal.h`
(component-private)，public API 才是 `net_dashboard_factory_reset()`。

CLAUDE.md 有完整 section ("Factory reset / reprovision")。README 有
user-facing 使用說明。

Cross-component coupling: `usb_composite` REQUIRES `net_dashboard`
(就像之前已經 REQUIRES `ota_core` 一樣)；`net_dashboard` REQUIRES
`esp_driver_gpio` 做 BOOT button watcher。

## ESP-IDF v6.0 migration — done & verified

Migrated whole project up from v5.5.1 to v6.0. Hardware verification
passed on real board: PWM band cross OK at multiple freqs (10/100/1000/
1000000 Hz), boot log clean, Wi-Fi reconnect from NVS, USB composite
enumerated, BLE provisioning still works.

### Source / config changes applied

- `main/idf_component.yml` — IDF constraint `>=5.5.0` → `>=6.0.0`. Added
  `espressif/network_provisioning ^1.2.0` (replaces removed built-in
  `wifi_provisioning`). cJSON pulled in transitively as
  `espressif/cjson` (was IDF built-in `json`).
- `components/net_dashboard/CMakeLists.txt` — REQUIRES `wifi_provisioning`
  → `espressif__network_provisioning`; `json` → `espressif__cjson`.
- `components/net_dashboard/provisioning.c` — header `wifi_provisioning/*`
  → `network_provisioning/*`; symbol prefix `wifi_prov_*` →
  `network_prov_*`; event ID `WIFI_PROV_CRED_SUCCESS/FAIL` →
  `NETWORK_PROV_WIFI_CRED_SUCCESS/FAIL` (note `WIFI_` infix because the
  new component supports both Wi-Fi and Thread); `WIFI_PROV_END` →
  `NETWORK_PROV_END` (no infix — generic event).
- `components/usb_composite/CMakeLists.txt` — added `esp_ringbuf` to
  REQUIRES; `freertos/ringbuf.h` no longer comes via the freertos
  umbrella in v6.0.
- `components/rpm_cap/rpm_cap.c` — removed `.flags.pull_up` from
  `mcpwm_capture_channel_config_t` (field deleted in v6.0); replaced
  with explicit `gpio_set_pull_mode()` call before
  `mcpwm_new_capture_channel()`.
- `components/pwm_gen/pwm_gen.c` — LO band `resolution_hz` 320 kHz →
  625 kHz, `freq_min` 5 → 10. v6.0 changed
  `MCPWM_GROUP_CLOCK_DEFAULT_PRESCALE` from 2 to 1 (group clock 80 MHz
  → 160 MHz), so the v5.5 LO band no longer fits in the auto-resolver's
  range without forcing a `group prescale conflict`. Floor moves up
  from 5 Hz to 10 Hz; spec impact accepted. CLAUDE.md "PWM glitch-free
  update mechanism" section has the full math.
- `main/CMakeLists.txt` — added `esp_driver_uart` to REQUIRES; v6.0
  split the monolithic `driver` component into per-peripheral pieces
  and `driver/uart.h` no longer comes via the umbrella.
- `sdkconfig.defaults` —
  - `CONFIG_NETWORK_PROV_NETWORK_TYPE_WIFI=y` (unmask Wi-Fi event IDs in
    network_provisioning)
  - `CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1=y` (v6.0 changed
    protocomm default away from sec1; we keep PoP-based BLE
    provisioning so we need the enum re-exposed)
  - `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` + sibling `=n` setters (workaround
    for v6.0 MCPWM band-cross bug — see Open issues below)
- Comment in `usb_composite.c` about esp_tinyusb 1.7.x's HID
  limitation still holds; descriptor logic unchanged.

### Tooling

- ESP-IDF v6.0 installed to `C:\esp\v6.0\esp-idf` via
  `install.ps1 esp32s3`. Old v5.5.1 still present at
  `C:\Espressif\frameworks\esp-idf-v5.5.1` for fallback (don't activate
  both at once — Python venv collision).
- Desktop shortcut **"ESP-IDF 6.0 PWM Project"** opens new PowerShell
  with v6.0 env active, cwd at `D:\github\ESP32_PWM`. Created via
  WScript.Shell COM in user `Desktop`.
- PowerShell profile alias `esp6` (and `esp6 pwm`) added to
  `D:\Documents\WindowsPowerShell\Microsoft.PowerShell_profile.ps1`.

## 🟡 OPEN ISSUE — v6.0 MCPWM band-cross workaround not understood

### Symptom

After v6.0 migration, the first HI band → LO band cross outputs the PWM
at 16× the requested frequency (period correct, prescale not actually
latched). E.g. `pwm 100 50` → scope shows 1.6 kHz instead of 100 Hz.
Driver's `mcpwm_set_prescale` returns OK; computed values are correct
on paper (`group_prescale=1, timer_prescale=256, resolution=625kHz,
period=6250`).

Same bug pattern as the v5.5 STOP→START latch issue we already fixed in
`reconfigure_for_band()` lines 219-235, but our existing fix is no
longer sufficient under v6.0.

### Workaround (currently applied)

Two pieces, both required:

1. `sdkconfig.defaults`: `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` so the
   driver's `LOGD` calls compile in.
2. `pwm_gen_init()` calls `esp_log_level_set("mcpwm", ESP_LOG_DEBUG)`
   so those compiled-in calls actually fire.

Bisected: removing **either** piece reproduces the bug. Both must be
present.

### Suspected (not confirmed) cause

The microseconds spent in driver `LOGD` argument formatting introduce
enough delay between MCPWM register writes for some shadow/latch
register to settle. Without the delay, the next register access reads
stale state. We did NOT confirm this with logic-analyser on the MCPWM
register bus.

### Why we shipped without root cause

Migration was already at the end of a long session; further bisect
would need a minimal repro project (no Wi-Fi/USB) and probably hardware
register tracing. The workaround is non-invasive (cost ~few KB of LOGD
format strings) and explicit at both the source-code site
(`pwm_gen.c:165`) and the Kconfig (`sdkconfig.defaults` LOG_MAXIMUM
section).

### Investigation hooks for next session

- Reproduce on minimal project: just `pwm_gen` + REPL, strip Wi-Fi/USB.
  Confirms it's not interaction with another subsystem.
- If it reproduces, read `/c/esp/v6.0/esp-idf/components/esp_driver_mcpwm/src/`
  changelog vs v5.5.1. Look for the `mcpwm_hal_timer_reset` addition
  (line 105 of `mcpwm_timer.c`) — that's a v6.0-only call that wasn't in
  v5.5; might be the root cause.
- Try replacing the LOGD-trick workaround with explicit
  `esp_rom_delay_us(N)` between specific register writes inside
  `reconfigure_for_band`. Minimum N that fixes it tells you the latch
  window.
- File against ESP-IDF GitHub if root cause is confirmed driver bug.

### Verification recipe

After any pwm_gen.c change, verify on scope:

```text
pwm 100 50    # scope GPIO4 → must read 100 Hz, not 1.6 kHz
pwm 1000 50   # scope GPIO4 → must read 1000 Hz
pwm 100 50    # scope GPIO4 → must read 100 Hz again
```

Each `pwm` REPL command should also print
`D (...) mcpwm: ... module calc prescale:N` line in monitor — if those
lines are missing, the workaround is broken.

---

> 以下 sections 是 v5.5.1 時代的歷史記錄（bring-up status, Bug 1-4
> chronicle），保留給之後考古用。新 session 應該優先看上面的 v6.0
> migration section，那才是 current 狀態。

Design spec lives at
`C:\Users\billw\.claude\plans\read-the-project-plan-pure-eagle.md`.

## Bring-up status — ✅ 軟體端全部驗證，Secure Boot 待重開 (v5.5.1 era)

Chip: ESP32-S3 (QFN56 rev v0.2), 16 MB flash, 8 MB octal PSRAM
(MAC `d0:cf:13:19:a3:70`), plain-text eFuse (未燒 SB/FE key).

驗證通過的路徑:

- **Boot**: ROM → bootloader → app，完整到 `main_task: Calling app_main()`
- **PSRAM**: 8 MB octal PSRAM @ 80 MHz init OK，heap allocator 正確接上
- **Wi-Fi STA**: 透過 BLE provisioning 註冊 SSID 成功，DHCP 拿到 IP
- **BLE (NimBLE)**: `wifi_prov_scheme_ble` advertising `ESP32-PWM`，PoP
  `abcd1234`，provisioning 完畢 `FREE_BTDM` 釋放 controller
- **HTTP + WebSocket**: `http://<ip>/` 吐出 dashboard，`/ws` 101 Switching
  Protocols，`set_pwm` / `set_rpm` JSON → `ctrl_cmd_queue` → `control_task`
  → sub-systems 全部驗證
- **PWM**: `mcpwm_new_timer` init @ 1 kHz 0% duty OK；dashboard Apply 真的
  改到 `control_task: pwm set: 1000 Hz, 50.00%`
- **RPM capture**: `mcpwm_new_cap_timer` init OK，GPIO6 timeout sentinel
  正常運作（無訊號 → 0 RPM）
- **TinyUSB composite**: `usb_comp: usb composite started (HID IF0 + CDC
  IF1/IF2)`，Windows Device Manager 看到 `USB Composite Device` →
  `HID-compliant vendor-defined device` + `USB 序列裝置 (COM8)`，
  VID/PID `0x303a:0x4005`

未驗證:

- ⬜ PWM GPIO4 實際波形（接 scope 量 1 kHz 50% square wave）
- ⬜ RPM capture 真的有資料進來（GPIO6 接訊號源）
- ⬜ Change-trigger pulse @ GPIO5
- ⬜ POST /ota（用 dashboard 上傳新 fw）
- ⬜ USB HID/CDC 實際流量（只確認 enumerate；沒跑 HID report 或 CDC
  SLIP OTA frame）
- ⬜ Secure Boot V2 / Flash Encryption（**目前關閉**，見 Bug 1）

## 本次 session 解掉的 bug（四個都已 fix + commit + push）

### Bug 1 — Secure Boot V2 + Flash Encryption 第一次 boot boot loop

**Symptom**: `rst:0x3 (RTC_SW_SYS_RST)` infinite loop，bootloader banner
都沒印出來就掛，`Saved PC:0x403cdb0a` = `process_segment` at
`esp_image_format.c:622`。

**Root cause**: 不確定是 SB 跟 FE 的 chicken-and-egg 互動、還是單一 feature
在全新 eFuse 上第一次 boot 就有問題。細節需要分開測試 (SB only / FE only)
來定位。

**Workaround**（Plan A）: `sdkconfig.defaults` 把
`CONFIG_SECURE_BOOT` 跟 `CONFIG_SECURE_FLASH_ENC_ENABLED` 都設 `n`。
**現階段 eFuse 還沒燒（boot loop 發生在 digest burn 之前），所以 Plan A
是 safe 的；一旦 SB 或 FE 曾經成功 boot 過一次 eFuse 就不能回頭**，
`docs/release-hardening.md` 要搭配更新。

**下一步**: 先在 eFuse 純淨的板子上一次開一個 feature 測（先 SB only、
再 FE only、最後 combined），找到真正 root cause 再決定怎麼重啟 security。

### Bug 2 — `mcpwm_new_timer(82): invalid period ticks`

**Symptom**: app 過了 `pwm_gen_init()` 就 abort，`ESP_ERR_INVALID_ARG`。

**Root cause**: MCPWM timer counter 是 16-bit
(`MCPWM_LL_MAX_COUNT_VALUE = 0x10000`)，但我們原本設
`resolution_hz = 160 MHz`，1 kHz 就算出 `period_ticks = 160000` 直接超界。

**Fix**: `components/pwm_gen/pwm_gen.c` 改成 `resolution_hz = 10 MHz`，
freq 支援範圍 153 Hz ~ 1 MHz（10 MHz / 65535 ≈ 153），duty resolution
從低頻 16 bits 到 1 MHz 3.3 bits 線性變化。`pwm_gen_duty_resolution_bits()`
目前回傳值跟實際情況對應得上。

### Bug 3 — Dashboard JS trailing NUL

**Symptom**: `app.js:60 Uncaught SyntaxError: Invalid or unexpected token`
→ JS 不 load → WebSocket 不連 → Apply 按鈕沒反應、RPM 不 update。

**Root cause**: `EMBED_TXTFILES` 在 binary 尾端塞一個 `\0` 讓 C 能當
string 用，但 `_end` symbol 指在 NUL 之後。`serve_embedded` 送
`end - start` bytes 結果 browser 拿到 `app.js\0`，JS parser 炸掉。

**Fix**: `components/net_dashboard/net_dashboard.c` 的 `serve_embedded`
送 `end - start - 1`。HTML/CSS 同樣受益，只是 browser 對 CSS/HTML 的
trailing NUL 比較寬容沒炸。

上述三個 fix 在 commit `e905b56` (`fix: 打通 hardware bring-up 三個
boot/runtime bug`)。

### Bug 4 — TinyUSB composite descriptor 不枚舉

**Symptom**:

<!-- markdownlint-disable MD004 MD032 -->
```text
E (981) tusb_desc: tinyusb_set_descriptors(183): Configuration descriptor must be provided for this device
E (990) TinyUSB: tinyusb_driver_install(90): Descriptors config failed
W (1002) app: USB composite init failed: ESP_ERR_INVALID_ARG
```
<!-- markdownlint-enable MD004 MD032 -->

**Root cause**: `esp_tinyusb 1.7.x` 的 default config descriptor 只 cover
CDC/MSC/NCM/VENDOR，**不處理 HID**。只要 `CFG_TUD_HID > 0`，
`tinyusb_set_descriptors` 就會強制要求 user 提供
`configuration_descriptor`，否則 install 整個被 reject。CLAUDE.md 原本
寫的「1.x auto-generates HID+CDC composite descriptors from Kconfig」
是**錯的**，已更正。

**Fix**: `components/usb_composite/usb_composite.c` 手寫 composite config
descriptor，layout:

```text
IF0          HID (1 IN EP @ 0x81, 16-byte packet, 10 ms polling)
IF1 + IF2    CDC (notif IN @ 0x82, bulk OUT @ 0x03, bulk IN @ 0x83)
```

`TUD_HID_DESCRIPTOR()` 要 compile-time report desc length，寫死
`HID_REPORT_DESC_SIZE = 53` 並在 `usb_descriptors.c` 加
`_Static_assert(sizeof(usb_hid_report_descriptor) == 53)` 綁住。

這個 fix 在 commit `be4a9ad` (`fix(usb): 提供 composite config descriptor
讓 HID+CDC 枚舉`)。

App 本身被設計成 USB init 失敗 graceful degradation — 即使 Bug 4 沒
解，其他 subsystem 照跑。這個 graceful pattern 保留著，有新 USB 問題
時不會把整個 app 拖下來。

## Environment housekeeping 本次 session 變更

- `credential.helper` (global): `wincred` → `manager`
  （要改回：`git config --global credential.helper wincred`）
- `gh` CLI active account: `billwang-gmt-project` → `billwanggithub`
  （要切回：`gh auth switch --user billwang-gmt-project`）
- Remote `origin` 指向 `https://github.com/billwanggithub/ESP32_PWM.git`
  （不變，只是第一次 push force-push 過，原本 remote 的孤立
  `Initial commit` 被蓋掉）

## 下一步

全部「必做」都已完成並 push 上 main。以下是後續 polish/驗證項目，
沒有嚴格順序。

1. 硬體量測 PWM / RPM 實際波形（scope 接 GPIO4 看 1 kHz 50% square wave；
   GPIO5 看 change trigger pulse；GPIO6 餵訊號源看 RPM capture 真的有
   讀到）。
2. USB HID / CDC 實際流量測試：Windows 端用 `hidapi` 或 `HidHide` 送
   `set_pwm` report 給 `0x303a:0x4005`；COM8 打開看 CDC log mirror；
   寫最小 Python SLIP client 送 OTA frame。
3. POST /ota 驗證：dashboard 上傳新 firmware 看 `ota_writer_task` 接收 +
   重啟。
4. 分開啟用 Secure Boot V2 / Flash Encryption 做 bisect，找到 Bug 1 的
   真正 root cause。步驟已記在 `docs/release-hardening.md` 的新 Stage
   0.5。
5. WS2812 @ GPIO48 加上 `led_strip` 做 status LED（idle / provisioning /
   wifi-connected / ota-in-progress 各一個顏色）。

## Board-specific reminders

- USB2 的 0 Ω jumper 必須在 **USB-OTG**（TinyUSB 才能接手 D-/D+；
  `USB-JTAG` 位置下 D-/D+ 被 USB-JTAG peripheral 搶走，TinyUSB 會
  install OK 但 host 看不到枚舉）。UART console 一直在 USB1 不受
  jumper 影響。
- GPIO 19/20 保留給 USB，絕對不要分給 PWM 或 capture。
- GPIO 0/3/45/46 是 strapping pin，避免當成 drive high/low output。

## Language preference

本次 session 已經改回 晶晶體 (中英混搭)。Commit title 保持 English-first
（git log 可讀性），body 走晶晶體。參考
`C:\Users\billw\.claude\memory\feedback_language.md`。
