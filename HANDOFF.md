# Hand-off — ESP32-S3 PWM + RPM firmware

Date: 2026-04-22 (updated 01:55)
Branch: `main` — pushed to `github.com/billwanggithub/ESP32_PWM` @ `be4a9ad`
Working dir: `D:\github\ESP32_PWM`
IDF: `C:\Espressif\frameworks\esp-idf-v5.5.1`

Design spec lives at
`C:\Users\billw\.claude\plans\read-the-project-plan-pure-eagle.md`.

## Bring-up status — ✅ 軟體端全部驗證，Secure Boot 待重開

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
