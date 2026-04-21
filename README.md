# ESP32-S3 PWM + RPM Capture

Firmware for the YD-ESP32-S3-COREBOARD V1.4 (ESP32-S3-WROOM-1, 16 MB flash,
8 MB octal PSRAM). Generates a glitch-free PWM up to 1 MHz and captures
tachometer RPM with a moving-averaged, configurable-timeout pipeline.
Controllable from an Android phone via a Wi-Fi web dashboard (BLE used only
for provisioning) and from a PC host via USB composite (HID + CDC).

The full design is in `C:\Users\billw\.claude\plans\read-the-project-plan-pure-eagle.md`.
The release-hardening checklist is in `docs/release-hardening.md`.

## Status (2026-04-22)

Working on real hardware as of commit `be4a9ad`:
Wi-Fi provisioning + HTTP dashboard + WebSocket + USB composite
(HID + CDC) + PWM init + RPM init all verified end-to-end.

Secure Boot V2 + Flash Encryption are **currently disabled** —
they caused a boot loop on first power-on with the untouched eFuse set.
Tracking in [HANDOFF.md](HANDOFF.md) Bug 1.

## First-time build (Windows 11)

1. **Install the ESP-IDF Python venv** (one-off, because the current
   environment is Python 3.14 and IDF's venv for it is missing). Open a
   plain `cmd.exe` and run:

   ```bat
   C:\Espressif\frameworks\esp-idf-v5.5.1\install.bat esp32s3
   ```

2. **Activate IDF** in your shell (each new terminal session). Easiest:
   use the Start-menu shortcut **"ESP-IDF 5.5.1 CMD"** that Espressif
   installs — it opens a pre-activated cmd.exe. Otherwise:

   - **cmd.exe**: `C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat`
   - **PowerShell**: `C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1`
   - **Git Bash / MSYS2**: `source /c/Espressif/frameworks/esp-idf-v5.5.1/export.sh`

3. **Set target, build, flash**:

   ```bat
   cd D:\github\ESP32_PWM
   idf.py set-target esp32s3
   idf.py build
   idf.py -p COM3 flash monitor
   ```

   The first build pulls `espressif/esp_tinyusb ~1.7.0` from the
   component registry (see `main/idf_component.yml`). Replace `COM3`
   with what Windows Device Manager assigns to the CH343P bridge on
   USB1.

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
| 48   | Onboard WS2812 RGB status LED           |

All configurable under `idf.py menuconfig` -> *ESP32 PWM App*.

## Interacting with the device

- **UART console** (USB1, CH343P): commands `pwm <freq> <duty>`,
  `rpm_params <pole> <mavg>`, `rpm_timeout <us>`, `status`, `help`.
  Baud rate is 115200.
- **BLE provisioning** (NimBLE): on first boot the device advertises as
  `ESP32-PWM`, PoP = `abcd1234`. Use the Espressif **ESP BLE
  Provisioning** app (Android / iOS) to enter Wi-Fi credentials.
  Credentials persist in NVS; subsequent boots skip provisioning and
  reconnect automatically.
- **Web dashboard** (Wi-Fi): once provisioned and online, browse to
  `http://<device-ip>/`. PWM freq/duty Apply, RPM params, live status
  (20 Hz WebSocket push), OTA upload form all work.
- **USB HID/CDC** (USB2, native USB with jumper on **USB-OTG**): the
  board enumerates as `USB Composite Device` → `HID-compliant
  vendor-defined device` + `USB 序列裝置 (COMx)`. HID report IDs and
  CDC SLIP frame ops are defined in
  `components/usb_composite/include/usb_protocol.h`.

## Repository layout

```
main/                      app_main, control_task, UART CLI, Kconfig
components/
  app_api/                 cross-component ctrl_cmd_t header
  pwm_gen/                 MCPWM generator
  rpm_cap/                 MCPWM capture + converter + averager
  usb_composite/           TinyUSB HID + CDC + log redirect
  net_dashboard/           BLE prov + HTTP + WebSocket + embedded web UI
  ota_core/                shared esp_ota_* writer (Wi-Fi + USB frontends)
docs/
  release-hardening.md     irreversible eFuse checklist
partitions.csv             factory + ota_0 + ota_1 + spiffs + nvs(_keys)
sdkconfig.defaults         target, PSRAM, BT/NimBLE, TinyUSB HID+CDC
                           (Secure Boot + Flash Enc currently off;
                            see HANDOFF.md Bug 1)
```
