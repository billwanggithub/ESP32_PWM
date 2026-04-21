# Hand-off — ESP32-S3 PWM + RPM firmware

Date: 2026-04-22 (updated 01:30)
Branch: `main` — pushed to `github.com/billwanggithub/ESP32_PWM` @ `779d95b`
Working dir: `D:\github\ESP32_PWM`
IDF: `C:\Espressif\frameworks\esp-idf-v5.5.1`

Design spec lives at
`C:\Users\billw\.claude\plans\read-the-project-plan-pure-eagle.md`.

## Bring-up status — ✅ 板子跑起來了，但 Secure Boot / TinyUSB 尚未

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

未驗證:

- ⬜ PWM GPIO4 實際波形（接 scope 量 1 kHz 50% square wave）
- ⬜ RPM capture 真的有資料進來（GPIO6 接訊號源）
- ⬜ Change-trigger pulse @ GPIO5
- ⬜ POST /ota（用 dashboard 上傳新 fw）
- ⬜ TinyUSB HID + CDC 枚舉（**尚未 work**，見下）
- ⬜ Secure Boot V2 / Flash Encryption（**目前關閉**，見下）

## 本次 session 解掉的 bug（三個都已 fix，尚未 commit）

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

## TinyUSB composite descriptor 問題（未 fix）

Boot log:

```text
E (981) tusb_desc: tinyusb_set_descriptors(183): Configuration descriptor must be provided for this device
E (990) TinyUSB: tinyusb_driver_install(90): Descriptors config failed
W (1002) app: USB composite init failed: ESP_ERR_INVALID_ARG
```

`esp_tinyusb 1.7.6` **不會**自動 generate composite config descriptor
給 HID+CDC，`usb_composite.c:34` 的
`.configuration_descriptor = NULL` 被 reject。要手寫 IAD + HID interface
+ CDC interface 的 config descriptor bytes（~40 bytes）。

App 本身被設計成 USB init 失敗 graceful degradation，其他 subsystem
（Wi-Fi / BLE / HTTP / PWM / RPM）照常跑，只是 USB HID/CDC 目前不 work。

## Environment housekeeping 本次 session 變更

- `credential.helper` (global): `wincred` → `manager`
  （要改回：`git config --global credential.helper wincred`）
- `gh` CLI active account: `billwang-gmt-project` → `billwanggithub`
  （要切回：`gh auth switch --user billwang-gmt-project`）
- Remote `origin` 指向 `https://github.com/billwanggithub/ESP32_PWM.git`
  （不變，只是第一次 push force-push 過，原本 remote 的孤立
  `Initial commit` 被蓋掉）

## 下一步

**必做**:

1. Commit Bug 1/2/3 的 fix + 這份 HANDOFF。
2. 回頭處理 TinyUSB composite descriptor —— 手寫 config descriptor。

**選做（之後）**:

1. 硬體量測 PWM / RPM 實際波形。
2. WS2812 @ GPIO48 加上 `led_strip` 做 status LED。
3. 分開啟用 Secure Boot / Flash Encryption，找到 Bug 1 的真正 root cause。
4. `docs/release-hardening.md` Step 0 改寫成「先確認 non-secure build 能跑」。

## Board-specific reminders

- USB2 的 0 Ω jumper 要在 **USB-OTG**（TinyUSB 才能接手 D-/D+）；
  UART console 一直在 USB1 不受影響。目前 USB 還沒 work 所以 jumper
  設定尚未真正被測試。
- GPIO 19/20 保留給 USB，絕對不要分給 PWM 或 capture。
- GPIO 0/3/45/46 是 strapping pin，避免當成 drive high/low output。

## Language preference

本次 session 已經改回 晶晶體 (中英混搭)。Commit title 保持 English-first
（git log 可讀性），body 走晶晶體。參考
`C:\Users\billw\.claude\memory\feedback_language.md`。
