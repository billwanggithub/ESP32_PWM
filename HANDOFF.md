# Hand-off — ESP32-S3 PWM + RPM firmware

Date: 2026-04-22 (updated 00:31)
Branch: `main` (uncommitted work; nothing pushed yet)
Working dir: `D:\github\ESP32_PWM`
IDF: `C:\Espressif\frameworks\esp-idf-v5.5.1`

Design spec lives at
`C:\Users\billw\.claude\plans\read-the-project-plan-pure-eagle.md`.

## Build status — ✅ 全綠

`idf.py build` 跑完 exit code 0。Artefacts:

- `build/bootloader/bootloader.bin` — 45,056 B signed (`0xB000`, 21% free
  under `check_sizes --offset 0xe000`)
- `build/esp32_pwm.bin` — 1,249,280 B signed (`0x131000`, 40% free in
  2 MB factory partition)
- Partition table: `factory @ 0x20000 / 2M`, `ota_0 @ 0x220000`,
  `ota_1 @ 0x420000`, `nvs_keys @ 0x620000` (encrypted), `spiffs` tail.

## 這次 session 做了什麼（relative to 先前 handoff）

先前的 handoff 描述的「`check_sizes` 在 `--offset 0x8000` 失敗」其實
**已經解掉了** — 那是上上個 build 的症狀，`sdkconfig.defaults` 的修改
早就被新一輪 `del sdkconfig; idf.py fullclean; idf.py build` 吃進去。
誤讀 log 的副作用之一是以為 bootloader 簽名檔名「drift」，但 log 裡
出現的 `secure_boot_signing_key2.pem` 只是 IDF 給的 generic
「要簽額外 key 的話可以這樣」提示文字，實際簽章用的是正確的
`secure_boot_signing_key.pem`。

真正讓 app `.elf` 連不起來的是四個 linker error（在這次 session 修掉）:

### Fix 1 — `tud_cdc_line_state_cb` 多重定義

esp_tinyusb 1.7.6 wrapper 的 `tusb_cdc_acm.c:51` 已經 strong-define 了
這個 callback（會把 line-state change 派送到透過
`tusb_cdc_acm_register_callback()` 註冊的 handler）。我們的
`usb_cdc_task.c:177` 同名 strong define → linker 直接 refuse。

解法：移除我們自己的 `tud_cdc_line_state_cb` 以及 `s_cdc_ready` flag。
TX/RX task 改成直接檢查 `tud_cdc_connected()` / `tud_cdc_available()`，
兩個 TinyUSB API 在 device 斷線時本來就會回 false/0，等於同樣的保護，
只是少一層 shadow state。

### Fix 2 — `tud_hid_descriptor_report_cb` undefined reference

`usb_descriptors.c:52` 有這個 symbol，`libusb_composite.a` 的
`nm --defined-only` 也看得到。問題在 **link order**：TinyUSB core
archive (`libespressif__tinyusb.a`) 排在 `libusb_composite.a` 前面，
靜態 archive 掃描是 left-to-right + on-demand，core archive 被拉進來
時 reference 未解，等掃到 `libusb_composite.a` 時 linker 已經不再
revisit 前面的 undefined。

解法：`components/usb_composite/CMakeLists.txt` 加上 `WHOLE_ARCHIVE`。
這會強制整個 archive 的 object 檔全部進 link，不等 symbol 被 reference
才拉進來，我們的 callback 因此永遠已定義。

### Fix 3 — `wifi_prov_scheme_ble` / `wifi_prov_scheme_ble_event_cb_free_btdm` undefined

`net_dashboard/CMakeLists.txt` REQUIRES 已經寫了 `bt`、
`wifi_provisioning`，但 `CONFIG_BT_ENABLED` 還是 `not set`，所以
wifi_provisioning 的 BLE scheme translation unit 根本沒被編進去。

解法：`sdkconfig.defaults` 新增：

```kconfig
CONFIG_BT_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=n
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_CONTROLLER_ENABLED=y
```

NimBLE 比 Bluedroid 小 ~60 KB flash，而且 provisioning 結束後程式碼
已經用 `WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM` 把 BT controller
記憶體釋放回 heap，runtime cost 趨近零。

### Fix 4 (sdkconfig 連動) — partition choice group 全部顯式標記

先前 session 已經做了，這次只是 `del sdkconfig` 讓它真的生效。
`CONFIG_PARTITION_TABLE_TYPE` 是 Kconfig choice group，`sdkconfig` 如果
stale 會保留 `TWO_OTA=y`，merge 時 last-wins 讓 custom 失效。對策是
在 `sdkconfig.defaults` 把其他 sibling 全部寫成 `=n`（已存在，
第 18-22 行）。

## 下一步 — 建議 user 要做的事

1. **接上板子測試**（還沒做過任何硬體驗證）：

   ```bash
   # USB2 port，jumper 切到 USB-OTG
   idf.py -p COM3 encrypted-flash monitor
   ```

   第一次一定要 `encrypted-flash`（寫入 eFuse flash enc key + 初次加密
   寫入）。之後才能用 `flash`。

2. **驗證重點**：
   - UART console (USB1) 有 boot log 跑出來
   - TinyUSB composite 在 host 上枚舉成 HID + CDC 兩個介面（jumper 要在
     `USB-OTG`；`USB-JTAG` 會被 USB-JTAG peripheral 搶走 D-/D+）
   - BLE advertising name `ESP32-PWM`（用 nRF Connect 或
     ESP BLE Prov app 測；PoP = `abcd1234`）
   - WebSocket 20 Hz push：provision 完連 Wi-Fi 後，瀏覽器打開
     `http://<device-ip>/` 應該看得到 status JSON 動起來

3. **commit 現在的 working tree**（如 user 同意再做）：
   - 主要變更：`usb_cdc_task.c`、`usb_composite/CMakeLists.txt`、
     `sdkconfig.defaults`
   - 第一個 commit 建議訊息：`build: 修掉 link error，打通 HID/CDC/BLE 三條路`

## 現在 on-disk 的狀態

```text
main/                    (沒動 this session)
  app_main.c            nvs → pwm_gen → rpm_cap → ota_core → control_task
                        → usb_composite → net_dashboard → mark-valid → REPL
  control_task.c        dispatcher for ctrl_cmd_queue
  CMakeLists.txt        REQUIRES across all components
  Kconfig.projbuild     APP_* pins (4=PWM, 5=trigger, 6=RPM, 48=LED)
  idf_component.yml     pins espressif/esp_tinyusb ~1.7.0

components/
  app_api/              shared ctrl_cmd_t header (prevents main-dep)
  pwm_gen/              MCPWM gen, glitch-free freq/duty via TEZ update
  rpm_cap/              MCPWM cap + SPSC freq_fifo[128] + converter task
                        + rpm_fifo[128] + averager task + 256-entry history
                        + esp_timer-based timeout (sentinel 0x80000000 → 0 RPM)
  usb_composite/        *modified:* CMakeLists WHOLE_ARCHIVE;
                        *modified:* usb_cdc_task.c 移除 line_state_cb
                        - HID IF0, report IDs 0x01/0x02 OUT, 0x10/0x11 IN @50Hz
                        - CDC IF1, SLIP OTA 0x10/0x11/0x12 + log 0x01
                        - esp_log_set_vprintf → CDC ring-buffer mirror
  net_dashboard/        wifi_provisioning scheme_ble + esp_http_server +
                        WebSocket /ws (JSON 20 Hz) + POST /ota + embedded web/
  ota_core/             single-writer esp_ota_* (mutex-guarded)

docs/release-hardening.md    irreversible eFuse checklist
partitions.csv               16 MB layout, factory@0x20000/2M, ota_0/1 each 2M
sdkconfig.defaults           *modified:* +BT/NimBLE block
README.md                    Win11 first-build steps
```

## 仍然未驗證的 risk（留給 hardware bring-up 時注意）

1. **TinyUSB composite descriptor** — 1.7.x Kconfig 裡沒有
   `TINYUSB_DESC_USE_DEFAULT_*` 類符號（舊文件還在提但 1.7 移除了）；
   理論上 HID + CDC 同時啟用就會 auto-build composite config descriptor。
   若 host 只看到 HID 或只看到 CDC，檢查
   `menuconfig → Component config → TinyUSB Stack → Device → Descriptors`
   看是不是有 `desc` 需要手動指定。
2. **`close_fn` default close 行為** — `ws_on_client_closed` 有顯式
   `close(fd)`；如果 httpd double-close 先看這裡。
3. **First `encrypted-flash` 可能 timeout** — CH343P @ 460800 baud 在
   YD-ESP32-S3 上偶爾會 stall。若失敗把 `CONFIG_ESPTOOLPY_BAUD` 降到
   115200 再試。
4. **WS2812 on GPIO48** — `led_strip` component 還沒被 include；status
   indicator 是計畫但尚未實作，留給 polish phase。

## Board-specific reminders

- USB2 的 0 Ω jumper 要在 **USB-OTG**（TinyUSB 才能接手 D-/D+）；
  UART console 不受影響，一直在 USB1。
- GPIO 19/20 保留給 USB，絕對不要分給 PWM 或 capture。
- GPIO 0/3/45/46 是 strapping pin，避免當成要 drive high/low 的 output。

## Language preference note

本次 session 已經改回 晶晶體 (中英混搭)。Commit title 保持 English-first
（git log 可讀性），body 走晶晶體。參考
`C:\Users\billw\.claude\memory\feedback_language.md`。
