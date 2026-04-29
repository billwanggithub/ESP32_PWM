# Fan-TestKit (ESP32-S3 PWM + RPM Capture)

Firmware for the YD-ESP32-S3-COREBOARD V1.4 (ESP32-S3-WROOM-1, 16 MB flash,
8 MB octal PSRAM). Generates a glitch-free PWM up to 1 MHz, captures
tachometer RPM with a moving-averaged, configurable-timeout pipeline,
and drives a bench DC PSU (Riden RD60xx, XY-SK120, or WZ5005 — runtime
selectable). Controllable from an Android phone via a Wi-Fi web
dashboard (SoftAP captive portal for first-time setup) and from a PC
host via USB composite (HID + CDC).

The full design is in `C:\Users\billw\.claude\plans\read-the-project-plan-pure-eagle.md`.
The release-hardening checklist is in `docs/release-hardening.md`.

## Status (2026-04-26)

NVS-persisted runtime tunables landed: RPM pole/mavg/timeout, PWM
freq, and dashboard slider step sizes (duty/freq) all survive
reboot via explicit Save commands reachable from all four
transports (WS / HID / CDC / CLI). PWM duty deliberately resets to
0 on boot regardless of saved state — safety invariant for
unsupervised restart. New `components/ui_settings/` component owns
step sizes; dashboard now reads them from the WS status frame so
all browser clients stay in sync. Full design + per-task wiring in
`docs/superpowers/plans/2026-04-26-nvs-persisted-settings.md`; NVS
contract summary in `CLAUDE.md`.

Earlier (2026-04-26): multi-family PSU support landed. `psu_driver/`
dispatches to one of three backends (`riden` / `xy_sk120` /
`wz5005`) at boot, picked from NVS via the dashboard PSU panel's
Family dropdown or the CLI `psu_family <name>`. Hardware
verification still pending — see `HANDOFF.md` for the open D-series
tasks.

Earlier (2026-04-22): migrated to **ESP-IDF v6.0** (was v5.5.1). PWM
band-cross verified on scope; Wi-Fi provisioning, HTTP dashboard,
WebSocket, USB composite (HID + CDC), PWM, RPM all working end-to-end
on hardware.

Secure Boot V2 + Flash Encryption are **currently disabled** —
they caused a boot loop on first power-on with the untouched eFuse set.
Tracking in [HANDOFF.md](HANDOFF.md) Bug 1.

PWM frequency floor is **10 Hz** (was 5 Hz under v5.5.1). v6.0 changed
the MCPWM driver's default group prescaler, raising the LO band
resolution. Details in [CLAUDE.md](CLAUDE.md) "PWM glitch-free update
mechanism".

## First-time build (Windows 11)

1. **Install the ESP-IDF v6.0 Python venv** (one-off). Open a plain
   `cmd.exe` and run:

   ```bat
   C:\esp\v6.0\esp-idf\install.bat esp32s3
   ```

2. **Activate IDF** in your shell (each new terminal session). Easiest:

   - **Desktop shortcut "ESP-IDF 6.0 PWM Project"** — opens new
     PowerShell with env active and cwd at this project.
   - **`esp6 pwm` PowerShell alias** — activates v6.0 + cd to project
     in the current PowerShell window. Defined in your PowerShell
     profile.

   Manual activation if you prefer:

   - **cmd.exe**: `C:\esp\v6.0\esp-idf\export.bat`
   - **PowerShell**: `C:\esp\v6.0\esp-idf\export.ps1`
   - **Git Bash / MSYS2**: `source /c/esp/v6.0/esp-idf/export.sh`

3. **Build, flash**:

   ```bat
   cd D:\github\Fan-TestKit-ESP32
   idf.py build
   idf.py -p COM24 flash monitor
   ```

   Target is locked to esp32s3 in `sdkconfig.defaults` so no
   `set-target` needed. First build pulls managed dependencies
   from the component registry (see `main/idf_component.yml`):
   `espressif/esp_tinyusb`, `espressif/cjson`, `espressif/mdns`,
   plus `espressif/tinyusb` as a transitive dep. Replace `COM24`
   with what Windows Device Manager assigns to the CH343P bridge
   on USB1.

### sdkconfig trap

`idf.py fullclean` wipes `build/` but **keeps `sdkconfig`**. Any change
to `sdkconfig.defaults` for a Kconfig symbol already present in
`sdkconfig` is silently ignored. When modifying partition layout,
Secure Boot, Flash Encryption, or TinyUSB Kconfig run:

```bat
del sdkconfig
idf.py fullclean
idf.py build
```

## Board jumper

The onboard USB2 port routes to either the native USB peripheral (TinyUSB)
or the built-in USB-JTAG. By default the `USB-JTAG` 0 ohm jumper is bridged.
For the HID + CDC composite device to enumerate on the PC, move the bridge
to **USB-OTG**. Logs still appear on UART0 via USB1 regardless.

## Pin map

| GPIO | Role                                    |
|------|-----------------------------------------|
| 4    | PWM output                              |
| 5    | PWM change-trigger (pulse on each set)  |
| 6    | RPM capture input                       |
| 19   | USB D-  (reserved by board)             |
| 20   | USB D+  (reserved by board)             |
| 38   | UART1 TX → PSU RX                       |
| 39   | UART1 RX ← PSU TX                       |
| 48   | Onboard WS2812 RGB status LED           |

PWM/RPM/LED pins configurable under `idf.py menuconfig` →
*Fan-TestKit App*. PSU pins + baud + slave default + family choice
under *PSU driver* (separate menu).

## Interacting with the device

- **UART console** (USB1, CH343P): commands `pwm <freq> <duty>`,
  `rpm_params <pole> <mavg>`, `rpm_timeout <us>`, `psu_v <volts>`,
  `psu_i <amps>`, `psu_out <0|1>`, `psu_slave <addr>`,
  `psu_family <name>`, `psu_status`, `status`, `help`. Baud is 115200.
  Save-to-NVS commands: `save_rpm_params`, `save_rpm_timeout`,
  `save_pwm_freq`, `save_ui_steps <duty> <freq>` — each persists the
  current live value(s) so they survive reboot. Saved PWM freq is
  reapplied at boot; saved duty is **not** a concept (boot duty is
  always 0).
- **SoftAP captive portal** (first-time setup): on a board without
  stored Wi-Fi credentials, the device brings up an open AP named
  `Fan-TestKit-setup`. Join it from your phone and open **any** URL in
  the browser — a DNS hijack redirects everything to the setup page.
  On some Android phones the captive-portal "Sign in to Wi-Fi network"
  notification fires automatically (stock Android 11+ works best);
  Samsung One UI often doesn't fire it and needs a manual browser tap.
  Enter your home SSID + password; on success the page shows both
  `http://fan-testkit.local/` and the assigned raw IP. Credentials
  persist in NVS so subsequent boots skip the AP and go straight to
  STA.
- **Web dashboard** (Wi-Fi, after provisioning): browse to the raw IP
  shown on the success page (e.g. `http://192.168.1.47/`). The mDNS
  name `http://fan-testkit.local/` works in desktop browsers (Windows
  with Bonjour, macOS, Linux with Avahi) but Chrome and most Android
  browsers don't resolve `.local` names — use the raw IP there. PWM
  freq/duty Apply, RPM params, live status (20 Hz WebSocket push),
  OTA upload form all work.
- **USB HID/CDC** (USB2, native USB with jumper on **USB-OTG**): the
  board enumerates as `USB Composite Device` → `HID-compliant
  vendor-defined device` + `USB 序列裝置 (COMx)`. HID report IDs and
  CDC SLIP frame ops are defined in
  `components/usb_composite/include/usb_protocol.h`.
- **IP Announcer (ntfy.sh push)** — opt-in feature that pushes the
  device's IP to your phone via ntfy.sh after every Wi-Fi connection.
  Solves the "Android Chrome can't resolve `fan-testkit.local` on a
  phone hotspot with randomised subnet" problem. Default topic is
  `fan-testkit-gmt-bench` (shared across every board built from this
  repo); install the ntfy app, subscribe to that topic, enable from
  Settings → IP Announcer in the dashboard. Step-by-step verification
  flow below in **First-time IP Announcer setup**. Topic resolution:
  NVS → Kconfig `APP_IP_ANNOUNCER_TOPIC_DEFAULT` (override in
  `sdkconfig.defaults.local` for a different default) → random
  `fan-testkit-<32 chars>` fallback if Kconfig is empty. Topics
  matching `CHANGE-ME-*` or shorter than 16 chars are refused at
  runtime to prevent placeholder leaks.

  **⚠️ Topic priority gotcha:** NVS wins over Kconfig. A board that
  already booted once with an older firmware (random fallback or
  different Kconfig default) has its topic stored in NVS — flashing
  a new firmware with a different `APP_IP_ANNOUNCER_TOPIC_DEFAULT`
  does **not** change the topic on that board, because the boot path
  takes the NVS fast path and never re-reads Kconfig. The board will
  still push, but to its old topic name — your ntfy app subscribed
  to the new default will see nothing. To switch the topic on an
  already-provisioned board, use one of: (a) dashboard Settings →
  IP Announcer → change Topic → Save; (b) USB1 REPL `announcer_set
  <new-topic>`; (c) `idf.py erase-flash` (also wipes Wi-Fi creds —
  full re-provisioning needed afterwards).

## First-time IP Announcer setup

Four-step sanity check, ~5 minutes total. Each step has a clear pass
/ fail signal and the failure mode points at the next step's fix.

### Prerequisites

- ntfy app installed on your phone (Play Store / F-Droid / App Store,
  developer "Philipp C. Heckel")
- Notification permission granted to the ntfy app (Android: Settings
  → Apps → ntfy → Notifications → Allow). **~90 % of "phone never
  rings" reports are this setting.**

### Step 1 — Verify ntfy.sh end-to-end (no board involved)

In the ntfy app, tap **+** and subscribe to `fan-testkit-gmt-bench`
(service `ntfy.sh`, no login needed). Then from any browser open:

```text
https://ntfy.sh/fan-testkit-gmt-bench/publish?message=test1
```

The browser returns a JSON `{"id":"...","time":...,"event":"message",...}`.

**Expected:** phone shows a `test1` notification within ~3 s.

**If not:** check the topic spelling (`fan-testkit-gmt-bench`, all
lowercase, hyphens not underscores), confirm notification permission,
kill-and-restart the ntfy app once. Don't move to Step 2 until this
works — Step 1 isolates the phone-side problem from the board-side
problem.

### Step 2 — Verify the board's HTTPS push (manual trigger)

Connect USB1 (CH343P) and open `idf.py monitor` or PuTTY at 115200.
At the `fan-testkit>` prompt:

```text
fan-testkit> announcer_enable 1
fan-testkit> announcer_test
```

**Expected:** monitor log shows:

```text
I (xxx) ip_announcer: IP 192.168.x.y — enqueueing push
I (xxx) ip_ann_push: push ok: 192.168.x.y -> ntfy ntfy.sh/fan-testkit-gmt-bench (HTTP 200)
```

…and the phone receives a `Fan-TestKit online / IP: 192.168.x.y`
notification simultaneously.

**If `push ok` but no notification:** topic mismatch between phone and
firmware (ntfy app subscribed to a different topic). Re-check Step 1.

**If `push attempt N failed: ...`:** see Step 3.

### Step 3 — Diagnose push failures via `last_err`

```text
fan-testkit> announcer_status
```

Match the printed `last_err` against this table:

| `last_err` content                         | Meaning                  | Fix                                                                                          |
|--------------------------------------------|--------------------------|----------------------------------------------------------------------------------------------|
| `ESP_ERR_HTTP_CONNECT` / `ESP_FAIL`        | No internet              | Wi-Fi reachable but cannot egress port 443. Check router firewall / hotspot data plan.       |
| `ESP_ERR_ESP_TLS_*`                        | TLS handshake failed     | System time not synced → cert validation rejects. Confirm SNTP runs at boot.                 |
| `HTTP 401` / `403`                         | Auth required            | ntfy.sh free tier doesn't usually require this. Check server hostname; consider self-host.   |
| `HTTP 429`                                 | Rate-limited             | Free tier = 5 msg/min, 500/day. Wait 60 s and retry `announcer_test`.                        |
| `HTTP 4xx` (other)                         | Topic format rejected    | Confirm topic has only `[a-zA-Z0-9_-]` characters.                                           |
| `topic placeholder; change before enabling`| Safety guard refused     | Topic is `< 16` chars or starts `CHANGE-ME-*`. Set a longer / non-placeholder topic.         |

### Step 4 — Verify automatic cold-boot push

`announcer_status` should show `enable=1`. Power-cycle the board
(unplug + replug, or press EN). After Wi-Fi reconnects:

**Expected:** phone receives a fresh notification within a few hundred
ms of the board's `IP_EVENT_STA_GOT_IP` log line.

**If only manual `announcer_test` works but cold boot doesn't:**

- Check the monitor log for `ip_announcer: IP ... — enqueueing push`.
  - Missing → `enable` didn't persist. Re-run `announcer_enable 1`
    then `announcer_status` to confirm `enable=1`.
  - Present but `last_status=failed` → see Step 3.

The board re-announces on every cold boot intentionally, even with an
unchanged IP — that's the desired UX (every reboot pings "I'm back").
DHCP renew with the same IP within a single power session is deduped
and does NOT push twice.

## Bench DC PSU (UART1)

The firmware drives a serial-controlled bench PSU on UART1 (GPIO 38
TX, GPIO 39 RX, common GND). Three families are supported and
selectable at runtime — same wiring for all of them, only the
protocol changes:

| Family | Protocol | Factory baud | Slave default |
|--------|----------|--------------|---------------|
| `riden` | Modbus-RTU (RD6006/RD6012/RD6018/RD6024) | 115200 | 1 |
| `xy_sk120` | Modbus-RTU (XY-SK120) | 115200 | 1 |
| `wz5005` | Custom 20-byte sum-checksum | 19200 | 1 |

Workflow:

1. Dashboard PSU panel → **Family** dropdown → pick family → **Save**
   → **Reboot**. (Or CLI: `psu_family wz5005`, then power-cycle / reset.)
2. Wire the PSU: ESP **GPIO 38** (TX) → PSU **RX**; ESP **GPIO 39**
   (RX) ← PSU **TX**; **GND ↔ GND**. Don't connect VCC.
3. Set the PSU's panel baud + slave to match firmware (or override the
   firmware via `idf.py menuconfig` → *PSU driver*).
4. WZ5005 only: panel must be in **COM** mode (not WIFI) — see manual
   section 1.4.2.5 item 3.

Boot log on success: `psu_driver: UART1 ready: family=X ... baud=Y
slave=N` followed by `psu_<family>: detected ...`. Dashboard PSU panel
shows green `link=up` within ~1 s.

Family + slave are NVS-persisted (namespace `psu_driver`). Family change
is **boot-effective** — the dashboard's Reboot button completes the
swap. Hot-swap is intentionally not supported (different baud + framing
per family makes mid-flight swaps risky).

Full wiring guide + failure modes: [docs/Power_Supply_Module.md](docs/Power_Supply_Module.md).

## Factory reset (re-provisioning Wi-Fi)

To forget the currently-stored Wi-Fi credentials and drop the device
back into SoftAP setup mode on next boot, trigger one of these (they
all land on the same core handler, call `esp_wifi_restore()`, then
reboot):

1. **Web dashboard** — on `http://<device-ip>/`, scroll to the
   "Factory reset" panel and click the red button. A `confirm()`
   dialog asks before sending.
2. **BOOT button** — hold the on-board BOOT button for **≥3 seconds**.
   Short presses are ignored so a casual touch during dev work won't
   wipe credentials.
3. **USB HID** — send report id `0x03` with a 1-byte payload `0xA5`
   (magic byte guard).
4. **USB CDC** — send SLIP-framed op `0x20` with a 1-byte payload
   `0xA5`. The device replies with op `0x21` (ack) before restarting.

After restart the device brings up the `Fan-TestKit-setup` open AP; join
it from your phone and the captive portal will pop up automatically.

Manual fallback if the firmware is unreachable (wedged, bricked): on
the desktop ESP-IDF shell, `idf.py -p COMn erase-flash` followed by
a re-flash clears all NVS.

## Repository layout

```text
main/                      app_main, control_task, UART CLI, Kconfig
components/
  app_api/                 cross-component ctrl_cmd_t header
  pwm_gen/                 MCPWM generator + NVS save_freq
  rpm_cap/                 MCPWM capture + converter + averager + NVS save
  gpio_io/                 GPIO IO + relay power switch
  psu_driver/              UART1 PSU dispatcher + 3 backends (riden, xy_sk120, wz5005)
                           shared psu_modbus_rtu helpers (CRC-16 + FC 0x03/0x06)
  ui_settings/             dashboard slider step sizes (NVS-backed; served via WS)
  usb_composite/           TinyUSB HID + CDC + log redirect
  net_dashboard/           SoftAP captive portal + HTTP + WebSocket + mDNS + web UI
  ota_core/                shared esp_ota_* writer (Wi-Fi + USB frontends)
docs/
  Power_Supply_Module.md   PSU wiring + family selection user guide
  release-hardening.md     irreversible eFuse checklist
  superpowers/specs/       design specs (one per feature)
  superpowers/plans/       implementation plans (one per feature)
partitions.csv             factory + ota_0 + ota_1 + spiffs + nvs(_keys)
sdkconfig.defaults         target, PSRAM, TinyUSB HID+CDC, mDNS
                           (Secure Boot + Flash Enc currently off;
                            see HANDOFF.md Bug 1)
```
