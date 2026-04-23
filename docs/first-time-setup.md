# First-Time Setup — Fan-TestKit (YD-ESP32-S3) 板子

給剛拿到板子的新人看的 onboarding guide。第一次從 unbox 到在 browser
上操作 PWM dashboard，全部步驟走一遍。目標是 5 ~ 10 分鐘搞定。

**你需要準備：**

- YD-ESP32-S3-COREBOARD V1.4 板子一塊
- USB-C 線一條 (**只要接 USB1 埠供電**；USB2 先不用)
- 一支 Android phone (iOS 沒 validate 過，但 protocol 相同，理論上可以)
- 家裡的 Wi-Fi SSID + password (**必須 2.4 GHz**；ESP32-S3 不支援 5 GHz)


## Quick-Start

整個流程就 3 個 step：

```
1. 板子通電 → 會開一個 open AP "Fan-TestKit-setup"
       │
       ▼
2. 手機連上那個 AP → browser 打開任何 URL → 自動跳到 setup page
       │  填 home Wi-Fi SSID + password → Connect
       ▼
3. Success page 會顯示 raw IP (例如 http://192.168.1.47/)
   → 手機切回 home Wi-Fi → 點那個 IP → dashboard 出現
```

底下每一步拆開講。


## Step 1 — 接電，確認板子開 SoftAP

1. 拿 USB-C 線接板子的 **USB1 埠** (靠近 CH343P chip 那顆，有 UART
   auto-reset 電路的那個)。
2. 插上電腦或充電器之後，**onboard WS2812 LED (GPIO48) 會亮**，
   代表 firmware 跑起來了。
3. 等 3 ~ 5 秒，板子會開一個 open (沒密碼) 的 SoftAP，SSID 是
   **`Fan-TestKit-setup`**。

**怎麼確認 SoftAP 有開？** 手機 Wi-Fi 設定裡掃一下，應該看得到
`Fan-TestKit-setup`。如果看不到：

- 等再久一點 (某些板子 boot 比較慢，到 10 秒都算正常)。
- 確認板子有通電 (LED 有亮)。
- 拔掉 USB 重插，看 LED 會不會重亮。

> **Note**：如果板子之前已經 provision 過 (NVS 裡有 Wi-Fi 記錄)，它
> **不會**開 SoftAP，會直接連上次記得那個 Wi-Fi。要重新觸發
> provisioning 必須先 factory reset — 看這份文件最後的
> **[Factory reset](#factory-reset--重新進入-provisioning)** 那一節。


## Step 2 — 手機連 SoftAP、用 browser provision

### 2.1 連上 `Fan-TestKit-setup`

在手機 Wi-Fi 設定點 `Fan-TestKit-setup`。它是 open network，不用密碼，直接
連。連上之後手機會顯示「此網路可能沒有網際網路連線」或類似警告 — **這正常**，
因為 SoftAP 確實不通外網。**不要**選「切換網路」或「離開」，保持連著就對了。

### 2.2 打開 browser，進 setup page

**兩種情況：**

**(a) 自動跳 (lucky path)** — 有些 Android 版本 (stock Android 11+ 比較
會跳) 會出現「登入此 Wi-Fi 網路 / Sign in to Wi-Fi network」的
notification，點它就直接進 setup page。

**(b) 沒跳、或者 Samsung One UI 的手機 (常見情況)** — 這種就手動進：
打開 Chrome 或任何 browser，在網址列**隨便打一個 URL**
(例如 `http://example.com` 或 `http://google.com` 都可以)，device 的
DNS hijack 會把你 redirect 到 setup page。

> **Why 需要手動？** Samsung 的 One UI 跟部分電信商 ROM 把 HTTP
> captive-portal probe 關掉了 (改成 HTTPS-only detection)，所以手機
> 的「Sign in to Wi-Fi network」notification 不會跳。這**沒有辦法
> 在 firmware 端修**，只能叫 user 手動打 URL。細節見 `HANDOFF.md`
> 的 Samsung 段。

### 2.3 填 home Wi-Fi credentials

Setup page 長這樣：

```
Fan-TestKit Setup
───────────────────────────────────────
Choose your home Wi-Fi and enter the
password. The device will then switch
off this setup network and join your
Wi-Fi.

Wi-Fi network
┌──────────────────────────────────┐
│ MyHome_2.4G  (-56 dBm)        ▼ │
└──────────────────────────────────┘

Password
┌──────────────────────────────────┐
│ ●●●●●●●●                         │
└──────────────────────────────────┘

┌──────────────────────────────────┐
│            Connect                │
└──────────────────────────────────┘
```

- **Wi-Fi network** dropdown：device 自己 scan 出來的周遭 AP 清單，
  訊號強度 (dBm) 越大越靠近 0 越好 (-50 dBm 強，-80 dBm 弱)。
- 如果清單裡沒看到你家 Wi-Fi，下拉到最底選 **`Other...`**，會多出一個
  手動輸入 SSID 的 text field。
- **Password** field 大小寫要對，藏 SSID 也支援。
- 點 **Connect** 之後 button 會變成 `Connecting... (up to 20 s)`。

Device 這時候在背景做：
1. 切到 APSTA 模式 (SoftAP 繼續開著，手機不會被踢掉)。
2. STA 去連你填的 Wi-Fi。
3. 20 秒內拿到 IP → 成功；失敗就丟 error 回來 setup page 讓你重填。

### 2.4 密碼打錯？

看到紅字 `Failed: auth_failed` 就是密碼錯 (或 SSID 錯、或 AP 不是
2.4 GHz)。不用 reboot 板子，setup page 會自己 re-enable form，改
一下 password 重點 Connect 就好。


## Step 3 — Success page，跳到 dashboard

連線成功後會自動跳到 success page：

```
Setup complete [OK]
───────────────────────────────────────
Reconnect your phone to your home Wi-Fi,
then tap either link:

┌──────────────────────────────────┐
│ http://fan-testkit.local/          │
└──────────────────────────────────┘
(try this first)

┌──────────────────────────────────┐
│ http://192.168.1.47/             │
└──────────────────────────────────┘
(fallback if .local names do not
 resolve on your phone)
```

**兩個連結差在哪：**

- **`http://fan-testkit.local/`** — mDNS hostname。桌面 browser 在
  Windows (要裝 Bonjour) / macOS / Linux (要 Avahi) 都 OK。但
  **Chrome on Android 不解 `.local`** (Chromium 長期 open issue)，所以
  手機上多半沒用。
- **`http://192.168.1.47/`** (你的實際 IP 會不一樣) — 直接 raw IP，
  **這個手機、電腦都會通**。建議手機就用這條。

### 3.1 切換手機回 home Wi-Fi

- **離開** `Fan-TestKit-setup` (device 自己會在大約 25 秒後關掉 AP，但
  手機不會自動切換)。
- 手機 Wi-Fi 設定切回你家的 Wi-Fi。
- 點 success page 上的 raw IP 連結 (或直接在 browser 網址列打那個 IP)。
- PWM dashboard 應該出現，可以調頻率、duty、看 RPM。

> **小提醒**：raw IP 記下來 (拍照、抄便條都行)。板子下次 boot 會
> 用同一組 credentials 自動連 home Wi-Fi，但 DHCP 有可能配不同 IP。
> 如果想要 stable address，就用 `fan-testkit.local` (桌面 browser) 或
> 去 router 設 DHCP reservation。


## Troubleshooting

### 板子一直 stuck 在「download mode」，LED 亮但沒開 SoftAP

Symptom：serial monitor 看到 `rst:0x1 (POWERON),boot:0x0
(DOWNLOAD(USB/UART0)) waiting for download`。

這是 YD-ESP32-S3 的 **CH343 DTR/RTS auto-program 陷阱** — 某些 CH343
driver 打開 serial port 時會讓 GPIO0 被拉低，導致板子 boot 進 download
mode 不是 run mode。**拔掉 USB、等 2 秒、再插**通常就好 (因為沒有
host 開著 port)。如果你有開 serial monitor，用支援「強制 DTR=1,
RTS=1」的工具 (例如 串口調試助手) 或用 `idf.py -p COMn monitor --no-reset`。
詳見 `CLAUDE.md` 的 "CH343 DTR/RTS auto-program trap" 那段。

### 手機連 `Fan-TestKit-setup` 之後 browser 沒自動跳

Samsung 跟某些電信 ROM 的已知限制 (上面 Step 2.2 有解釋)。
**Workaround**：手動打開 browser，網址列隨便打一個 `http://` 開頭的
URL (`http://example.com` 都可以)，DNS hijack 會把你 redirect 到
setup page。**不要打 `https://`** — TLS 沒辦法 hijack，會直接 error。

### Setup page 的 SSID dropdown 卡在「Scanning...」

Device scan 失敗 (偶爾 driver glitch)。下拉選 `Other...`、手動打 SSID
就好。

### 連 success page 上的 `fan-testkit.local` 連不到

多半是你的 OS 沒有 mDNS resolver：

- **Windows**：要裝 Bonjour Print Services (Apple 出的，免費)，
  不然 `.local` 不會解。
- **Android (Chrome)**：不支援，直接用 raw IP。
- **Linux**：裝 `avahi-daemon` 跟 `libnss-mdns`。
- **macOS / iOS**：內建 Bonjour，應該直接通。

如果確定 resolver 裝了還是不通，用 raw IP 就對。

### Dashboard 打開後 WebSocket 沒連上 / live status 不動

- 檢查 browser console 有沒有 WS error。
- 確認板子跟手機/電腦**在同一個 LAN 的同一個 subnet** (有些 guest
  Wi-Fi 會隔離 client-to-client traffic，mDNS 跟 raw IP 都打不到)。
- Reload dashboard 一次。

### USB2 (native USB) 插了 PC 不認

兩個可能：

1. **USB-JTAG / USB-OTG jumper 沒焊對**。想用 TinyUSB composite
   (HID + CDC)，必須把 `USB-OTG` 那個 0 Ω jumper 橋起來。板上預設
   多半是 `USB-JTAG`。用電烙鐵換過。
2. **driver 沒裝**。Windows 10+ 會自動 enum `USB Composite Device`
   跟 `USB 序列裝置 (COMx)`，裝置管理員裡應該看得到。看不到就是
   jumper 沒橋。

USB2 跟 Wi-Fi provisioning 無關 (provisioning 只用 Wi-Fi)，但很多新
人第一次就踩這個。


## Factory reset — 重新進入 provisioning

板子一旦 provision 過，會記在 NVS，下次 boot 直接連舊的 Wi-Fi。如果
換環境 (換家 / 換公司 / 測試) 要重新 provision，有 **4 種** 方式都會
擦掉 credentials 並重開 SoftAP：

1. **BOOT 按鈕長按 ≥ 3 秒** (板子上唯一那顆 BOOT 按鈕，接 GPIO0)。
   最快、不用接 USB、不用手機。
2. **Web dashboard** — 已經在 dashboard 的話，按 "Factory Reset"
   button，會跳 confirm dialog，按 OK。
3. **USB HID report ID 0x03 + magic byte 0xA5** — host 工具 path，
   一般 user 用不到。
4. **USB CDC op 0x20 + magic byte 0xA5** — 同上，host 工具 path。

任一種觸發後，板子會 200 ms 內 reboot，boot 起來沒 credentials，就會
進 Step 1 的 SoftAP 流程。


## 更進階的資料

- `README.md` — feature overview、build 指令、Wi-Fi / USB / OTA 全貌
- `CLAUDE.md` — architecture invariants、task topology、PWM band-
  cross trap、CH343 auto-program trap
- `HANDOFF.md` — 遺留 issue、Samsung captive-portal 限制的完整
  bisect 記錄、Secure Boot 重開的 TODO
- `docs/superpowers/specs/2026-04-22-softap-captive-portal-design.md`
  — provisioning flow 的原始 design spec
- `docs/YD-ESP32-S3-SCH-V1.4.pdf` — 板子 schematic (Q1/Q2 DTR/RTS
  auto-program 電路在 page 1)
