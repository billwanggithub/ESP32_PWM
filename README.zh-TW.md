# Fan-TestKit (ESP32-S3 PWM + RPM Capture)

YD-ESP32-S3-COREBOARD V1.4 (ESP32-S3-WROOM-1, 16 MB flash, 8 MB octal
PSRAM) 用 firmware。產生 glitch-free 的 PWM 最高 1 MHz、用
moving-average + 可調 timeout 的 pipeline 抓 tachometer RPM、並驅動
bench DC PSU (Riden RD60xx、XY-SK120、WZ5005 — runtime 切換)。可從
Android 手機透過 Wi-Fi web dashboard (第一次設定走 SoftAP captive portal)
控制，也可從 PC host 透過 USB composite (HID + CDC) 控制。

完整的 design 寫在 `C:\Users\billw\.claude\plans\read-the-project-plan-pure-eagle.md`。
Release-hardening checklist 在 `docs/release-hardening.md`。

## 現況 (2026-04-26)

NVS-persisted runtime tunables 已落地：RPM pole / mavg / timeout、PWM
freq、dashboard slider step sizes (duty / freq) 全部都會透過明確的
Save 指令在 reboot 後保留 — 四個 transports (WS / HID / CDC / CLI) 都
有 entry。PWM duty 故意在 boot 強制 reset 成 0，不論 NVS 裡面存了什麼
— 這是 unsupervised restart 的 safety invariant。新的
`components/ui_settings/` component 負責 step sizes；dashboard 現在從
WS status frame 讀回這些值，所以多個 browser client 之間都會同步。
完整 design + 每個 task 的 wiring 寫在
`docs/superpowers/plans/2026-04-26-nvs-persisted-settings.md`；NVS
contract 摘要在 `CLAUDE.md`。

更早 (2026-04-26)：multi-family PSU 支援落地。`psu_driver/` 在 boot
時根據 NVS dispatch 到三個 backend 之一 (`riden` / `xy_sk120` /
`wz5005`)，從 dashboard PSU panel 的 Family dropdown 或 CLI
`psu_family <name>` 選。Hardware verification 還沒做完 — 開放的
D-series tasks 看 `HANDOFF.md`。

更早 (2026-04-22)：升級到 **ESP-IDF v6.0** (從 v5.5.1)。PWM band-cross
已用示波器驗證；Wi-Fi provisioning、HTTP dashboard、WebSocket、USB
composite (HID + CDC)、PWM、RPM 全部都在硬體上 end-to-end 跑通。

Secure Boot V2 + Flash Encryption **目前 disabled** — 在還沒燒過的
eFuse 板子上第一次開機會 boot loop。Tracking 在 [HANDOFF.md](HANDOFF.md)
Bug 1。

PWM frequency floor 是 **10 Hz** (在 v5.5.1 時是 5 Hz)。v6.0 改了
MCPWM driver 的 default group prescaler，把 LO band 的 resolution 拉
高了。細節在 [CLAUDE.md](CLAUDE.md) 的 "PWM glitch-free update mechanism"。

## 第一次 build (Windows 11)

1. **裝 ESP-IDF v6.0 Python venv** (一次就好)。開一個普通的
   `cmd.exe` 跑：

   ```bat
   C:\esp\v6.0\esp-idf\install.bat esp32s3
   ```

2. **每次新開 terminal 都要 activate IDF**。最簡單的方式：

   - **桌面捷徑 "ESP-IDF 6.0 PWM Project"** — 開一個新的 PowerShell，
     env 已 active，cwd 已落在這個 project。
   - **`esp6 pwm` PowerShell alias** — 在當前 PowerShell 視窗 activate
     v6.0 + cd 到 project。定義在你的 PowerShell profile 裡。

   要手動 activate 也可以：

   - **cmd.exe**：`C:\esp\v6.0\esp-idf\export.bat`
   - **PowerShell**：`C:\esp\v6.0\esp-idf\export.ps1`
   - **Git Bash / MSYS2**：`source /c/esp/v6.0/esp-idf/export.sh`

3. **Build、flash**：

   ```bat
   cd D:\github\Fan-TestKit-ESP32
   idf.py build
   idf.py -p COM24 flash monitor
   ```

   Target 在 `sdkconfig.defaults` 裡鎖在 esp32s3，不用再下 `set-target`。
   第一次 build 會從 component registry 拉 managed dependencies (見
   `main/idf_component.yml`)：`espressif/esp_tinyusb`、`espressif/cjson`、
   `espressif/mdns`，加上 transitive dep `espressif/tinyusb`。`COM24`
   要換成 Windows Device Manager 給 USB1 上 CH343P bridge 分到的號碼。

### sdkconfig 陷阱

`idf.py fullclean` 會把 `build/` 砍掉但 **保留 `sdkconfig`**。如果改了
`sdkconfig.defaults` 裡某個 Kconfig symbol，但這個 symbol 已經在
`sdkconfig` 裡有值，build 會默默忽略你的改動。要動 partition layout、
Secure Boot、Flash Encryption、TinyUSB Kconfig 時要：

```bat
del sdkconfig
idf.py fullclean
idf.py build
```

## 板子 jumper

板上 USB2 port 透過一顆 0 ohm jumper 連到 native USB peripheral
(TinyUSB) 或 built-in USB-JTAG，default 橋在 `USB-JTAG` 這側。要在 PC
端讓 HID + CDC composite device 枚舉成功，要把橋移到 **USB-OTG**。
Logs 不論橋哪邊都仍會走 USB1 上的 UART0 出來。

## Pin map

| GPIO | 用途                                    |
|------|-----------------------------------------|
| 4    | PWM 輸出                                |
| 5    | PWM change-trigger (每次 set 時 pulse)  |
| 6    | RPM capture 輸入                        |
| 19   | USB D-  (board 保留)                    |
| 20   | USB D+  (board 保留)                    |
| 38   | UART1 TX → PSU RX                       |
| 39   | UART1 RX ← PSU TX                       |
| 48   | 板載 WS2812 RGB 狀態 LED                |

PWM / RPM / LED 的 pin 可以在 `idf.py menuconfig` → *Fan-TestKit App*
裡改。PSU 的 pin + baud + slave default + family choice 在另一個 menu
*PSU driver* 底下。

## 與裝置互動

- **UART console** (USB1, CH343P)：指令 `pwm <freq> <duty>`、
  `rpm_params <pole> <mavg>`、`rpm_timeout <us>`、`psu_v <volts>`、
  `psu_i <amps>`、`psu_out <0|1>`、`psu_slave <addr>`、
  `psu_family <name>`、`psu_status`、`status`、`help`。Baud 是 115200。
  Save-to-NVS 指令：`save_rpm_params`、`save_rpm_timeout`、
  `save_pwm_freq`、`save_ui_steps <duty> <freq>` — 各自把目前的 live
  值寫進 NVS 讓 reboot 後保留。Saved 的 PWM freq 在 boot 時會重新套
  用；duty **沒有 saved** 這個概念 (boot 一律從 0 開始)。
- **SoftAP captive portal** (第一次設定)：板子上沒存 Wi-Fi 認證時會
  開一個叫 `Fan-TestKit-setup` 的 open AP。手機接上後在瀏覽器打開
  **任何** URL — DNS hijack 會把所有 request 導到 setup 頁。某些
  Android 手機 (stock Android 11+ 最穩) 會自動跳出 "登入 Wi-Fi 網路"
  的 captive-portal 通知；Samsung One UI 通常不會跳，要手動開瀏覽器。
  輸入家裡的 SSID + password；成功後頁面會同時顯示
  `http://fan-testkit.local/` 跟分到的 raw IP。認證會存在 NVS，下次
  boot 直接跳過 AP 進 STA。
- **Web dashboard** (Wi-Fi，provisioning 之後)：在瀏覽器打成功頁顯示
  的 raw IP (例如 `http://192.168.1.47/`)。mDNS name
  `http://fan-testkit.local/` 在 desktop 瀏覽器上 work (Windows 裝
  Bonjour、macOS、Linux 裝 Avahi)，但 Chrome 跟大多數 Android 瀏覽器
  不會解 `.local` name — 那邊用 raw IP。PWM freq / duty Apply、RPM
  params、live status (20 Hz WebSocket push)、OTA 上傳表單都 work。
- **USB HID/CDC** (USB2，jumper 要在 **USB-OTG**)：板子枚舉成
  `USB Composite Device` → `HID-compliant vendor-defined device` +
  `USB 序列裝置 (COMx)`。HID report ID 跟 CDC SLIP frame op 定義在
  `components/usb_composite/include/usb_protocol.h`。
- **IP Announcer (ntfy.sh push)** — opt-in 功能，每次 Wi-Fi 連上就把
  裝置 IP push 到你的手機。解的是 "Android Chrome 在隨機 subnet 的
  手機 hotspot 上不會解 fan-testkit.local" 這個問題。Default topic 是
  `fan-testkit-gmt-bench` (這個 repo build 出來的所有板子共用)；裝
  ntfy app、訂閱該 topic、在 dashboard Settings → IP Announcer 啟用。
  逐步驗證流程在下面 **First-time IP Announcer setup**。Topic 解析
  順序：NVS → Kconfig `APP_IP_ANNOUNCER_TOPIC_DEFAULT` (在
  `sdkconfig.defaults.local` 裡覆寫換 default) → 最後 fallback 是
  random `fan-testkit-<32 chars>` (Kconfig 空才會用)。Topic 比對
  `CHANGE-ME-*` 或短於 16 字元的 runtime 會 refuse，避免 placeholder
  外洩。

  **⚠️ Topic 優先順序陷阱：NVS 贏過 Kconfig。** 已經 boot 過一次
  的板子 (用舊 firmware 的 random fallback 或不同的 Kconfig default)
  topic 已經寫進 NVS — 重新 flash 一個帶不同
  `APP_IP_ANNOUNCER_TOPIC_DEFAULT` 的 firmware 上去並 **不會** 改該
  板子的 topic，因為 boot path 走 NVS fast path 完全不會再讀
  Kconfig。板子還是會 push，但是 push 到舊 topic name — 你 ntfy app
  訂閱新 default 那邊就什麼都收不到。要改一個已 provisioning 過板子
  的 topic 三選一：(a) dashboard Settings → IP Announcer → 改 Topic
  → Save；(b) USB1 REPL `announcer_set <new-topic>`；(c)
  `idf.py erase-flash` (這個會連 Wi-Fi 認證一起清，後面要重新 provisioning)。

## First-time IP Announcer setup

四步 sanity check，全部加起來大概 5 分鐘。每步都有清楚的 pass / fail
訊號，failure mode 直接指向下一步要修哪裡。

### 前置條件

- 手機上裝 ntfy app (Play Store / F-Droid / App Store，作者
  "Philipp C. Heckel")
- 給 ntfy app 通知權限 (Android：設定 → 應用程式 → ntfy → 通知 →
  允許)。**約 90% 的 "手機都不會響" 都是這裡沒給。**

### Step 1 — 先驗 ntfy.sh end-to-end (跟板子無關)

在 ntfy app 點 **+** 訂閱 `fan-testkit-gmt-bench` (服務 `ntfy.sh`，
不用註冊)。然後在隨便一個瀏覽器開：

```text
https://ntfy.sh/fan-testkit-gmt-bench/publish?message=test1
```

瀏覽器會回一個 JSON `{"id":"...","time":...,"event":"message",...}`。

**預期：** 手機在 ~3 秒內出現 `test1` 通知。

**沒收到：** 檢查 topic 拼字 (`fan-testkit-gmt-bench`，全小寫，是
hyphen 不是 underscore)、確認通知權限、kill ntfy app 後重開一次。
Step 1 沒通就先別跳 Step 2 — 這一步是把手機端問題跟板子端問題隔開。

### Step 2 — 驗板子 HTTPS push (手動 trigger)

接 USB1 (CH343P) 開 `idf.py monitor` 或 PuTTY 115200。在
`fan-testkit>` prompt 下：

```text
fan-testkit> announcer_enable 1
fan-testkit> announcer_test
```

**預期：** monitor log 顯示：

```text
I (xxx) ip_announcer: IP 192.168.x.y — enqueueing push
I (xxx) ip_ann_push: push ok: 192.168.x.y -> ntfy ntfy.sh/fan-testkit-gmt-bench (HTTP 200)
```

…而且手機同時收到 `Fan-TestKit online / IP: 192.168.x.y` 通知。

**如果是 `push ok` 但手機沒收到通知：** 手機 app 訂的 topic 跟
firmware 裡的 topic 不一致。回頭重檢 Step 1。

**如果是 `push attempt N failed: ...`：** 看 Step 3。

### Step 3 — 用 `last_err` 診斷 push 失敗

```text
fan-testkit> announcer_status
```

把 `last_err` 對照下表：

| `last_err` 內容                            | 意義                  | 修法                                                                                         |
|--------------------------------------------|--------------------------|----------------------------------------------------------------------------------------------|
| `ESP_ERR_HTTP_CONNECT` / `ESP_FAIL`        | 沒 internet              | Wi-Fi 通但無法走 port 443 出去。檢查 router firewall / 手機 hotspot 流量方案。               |
| `ESP_ERR_ESP_TLS_*`                        | TLS handshake 失敗     | 系統時間沒同步 → cert validation 拒絕。確認 boot 時 SNTP 有跑。                              |
| `HTTP 401` / `403`                         | 需要 auth               | ntfy.sh 免費版通常不需要。檢查 server hostname；考慮自架。                                   |
| `HTTP 429`                                 | 被 rate-limit           | 免費版 = 5 msg/min、500/day。等 60 秒再 `announcer_test`。                                   |
| `HTTP 4xx` (其他)                          | Topic 格式被拒          | 確認 topic 只有 `[a-zA-Z0-9_-]`。                                                            |
| `topic placeholder; change before enabling`| Safety guard 拒絕      | Topic 短於 16 字元或開頭是 `CHANGE-ME-*`。設一個比較長 / 非 placeholder 的 topic。          |

### Step 4 — 驗 cold-boot 自動 push

`announcer_status` 應該秀 `enable=1`。把板子 power-cycle (拔再插，或
按 EN)。Wi-Fi 重連後：

**預期：** 手機在板子 `IP_EVENT_STA_GOT_IP` log 出現後幾百 ms 內收到
新通知。

**如果手動 `announcer_test` work 但 cold boot 不 work：**

- 看 monitor log 有沒有 `ip_announcer: IP ... — enqueueing push`。
  - 沒有 → `enable` 沒寫進 NVS。重跑 `announcer_enable 1` 然後
    `announcer_status` 確認 `enable=1`。
  - 有 但 `last_status=failed` → 看 Step 3。

板子是故意每次 cold boot 都 re-announce 的，就算 IP 沒變也一樣 — 這
就是預期 UX (每次 reboot 都告訴你 "我回來了")。同一次 power session
裡 DHCP renew 拿到同一個 IP 不會 push 第二次 (有 dedup)。

## Bench DC PSU (UART1)

Firmware 在 UART1 (GPIO 38 TX、GPIO 39 RX、共地) 上控一台 serial-
controlled bench PSU。三個 family 都 runtime 切換 — 接線一樣，只有
protocol 換：

| Family | Protocol | 出廠 baud | Slave default |
|--------|----------|--------------|---------------|
| `riden` | Modbus-RTU (RD6006/RD6012/RD6018/RD6024) | 115200 | 1 |
| `xy_sk120` | Modbus-RTU (XY-SK120) | 115200 | 1 |
| `wz5005` | 自訂 20-byte sum-checksum | 19200 | 1 |

流程：

1. Dashboard PSU panel → **Family** dropdown → 選 family → **Save**
   → **Reboot**。(或 CLI：`psu_family wz5005`，然後 power-cycle / reset。)
2. 接 PSU：ESP **GPIO 38** (TX) → PSU **RX**；ESP **GPIO 39** (RX)
   ← PSU **TX**；**GND ↔ GND**。不要接 VCC。
3. PSU 面板 baud + slave 要跟 firmware 對齊 (或 `idf.py menuconfig`
   → *PSU driver* 在 firmware 端覆寫)。
4. WZ5005 限定：面板要在 **COM** mode (不是 WIFI) — 看說明書 1.4.2.5
   第三項。

成功時 boot log：`psu_driver: UART1 ready: family=X ... baud=Y
slave=N` 接著 `psu_<family>: detected ...`。Dashboard PSU panel ~1 秒
內顯示綠色 `link=up`。

Family + slave 是 NVS-persisted (namespace `psu_driver`)。Family 改
動是 **boot-effective** — dashboard 的 Reboot 按鈕完成切換。Hot-swap
故意不支援 (各 family 不同 baud + framing，飛行中切太危險)。

完整接線指南 + failure mode：[docs/Power_Supply_Module.md](docs/Power_Supply_Module.md)。

## Factory reset (Wi-Fi 重新 provisioning)

要忘記目前存的 Wi-Fi 認證讓裝置下次 boot 進 SoftAP setup mode 有四
種方式 (都最終呼叫同一個 core handler — `esp_wifi_restore()` 然後
reboot)：

1. **Web dashboard** — `http://<device-ip>/` 上滑到 "Factory reset"
   panel 按紅色按鈕。會跳 `confirm()` 對話框確認。
2. **BOOT 按鈕** — 按住板上的 BOOT 按鈕 **≥3 秒**。短按會被忽略，
   開發中不小心按到不會把認證清掉。
3. **USB HID** — 送 report id `0x03`，1-byte payload `0xA5`
   (magic byte guard)。
4. **USB CDC** — 送 SLIP-framed op `0x20`，1-byte payload `0xA5`。
   裝置會回 op `0x21` (ack) 然後 restart。

Restart 完之後板子開 `Fan-TestKit-setup` open AP；手機接上去 captive
portal 會自動跳。

如果 firmware 整個失控連這個都摸不到 (例如 brick 了)，desktop
ESP-IDF shell 上 `idf.py -p COMn erase-flash` 然後重 flash 會清掉所
有 NVS。

## 目錄結構

```text
main/                      app_main、control_task、UART CLI、Kconfig
components/
  app_api/                 跨 component 共用的 ctrl_cmd_t header
  pwm_gen/                 MCPWM generator + NVS save_freq
  rpm_cap/                 MCPWM capture + converter + averager + NVS save
  gpio_io/                 GPIO IO + relay 電源開關
  psu_driver/              UART1 PSU dispatcher + 3 個 backend (riden、xy_sk120、wz5005)
                           共用的 psu_modbus_rtu helpers (CRC-16 + FC 0x03/0x06)
  ui_settings/             dashboard slider step sizes (NVS-backed；走 WS 推給前端)
  usb_composite/           TinyUSB HID + CDC + log redirect
  net_dashboard/           SoftAP captive portal + HTTP + WebSocket + mDNS + web UI
  ota_core/                共用的 esp_ota_* writer (Wi-Fi + USB frontends)
docs/
  Power_Supply_Module.md   PSU 接線 + family 選擇使用者手冊
  release-hardening.md     不可逆的 eFuse checklist
  superpowers/specs/       design specs (一個 feature 一份)
  superpowers/plans/       implementation plans (一個 feature 一份)
partitions.csv             factory + ota_0 + ota_1 + spiffs + nvs(_keys)
sdkconfig.defaults         target、PSRAM、TinyUSB HID+CDC、mDNS
                           (Secure Boot + Flash Enc 目前 off；
                            看 HANDOFF.md Bug 1)
```
