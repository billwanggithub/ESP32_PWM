# Release Hardening Checklist

**These steps are irreversible.** Every bullet burns eFuses on the chip. Do
them only after all other testing is complete, on the exact board you intend
to ship. Read each ESP-IDF reference link before running any command.

- ESP-IDF Flash Encryption — <https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/flash-encryption.html>
- ESP-IDF Secure Boot V2 — <https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html>

## 0. Prerequisites

- Non-secure build has been **flashed and fully verified** on the board
  (Wi-Fi + BLE + HTTP + WebSocket + PWM + RPM + USB HID/CDC all work).
  This is the baseline you're hardening on top of — don't start here
  with a firmware you haven't already observed running end-to-end.
- Single-feature security bring-up must be done first (see the
  dedicated stage 0.5 below). Do **not** jump straight from non-secure
  firmware to "both SB + FE enabled". On a fresh-eFuse YD-ESP32-S3 that
  combination boot-loops in `process_segment` before any bootloader log
  appears — we don't yet know whether SB, FE, or their interaction is
  responsible.
- You have a USB1 serial console available (CH343P → UART0) for
  `esptool.py`.
- The board currently accepts unsigned OTA updates from both Wi-Fi and
  USB CDC (will become signed after step 2).

## 0.5 Bring up Secure Boot and Flash Encryption separately first

Before running any of the irreversible release-mode steps below, work
out which feature(s) trigger the first-boot boot-loop observed on a
plain-eFuse board:

1. On a spare or known-clean board, flip **only** `CONFIG_SECURE_BOOT=y`
   (+ `CONFIG_SECURE_BOOT_V2_ENABLED=y` etc., leave Flash Encryption
   off). Rebuild, flash, confirm the device boots into the app.
2. Reset back to baseline, then flip **only**
   `CONFIG_SECURE_FLASH_ENC_ENABLED=y` +
   `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y` (leave Secure
   Boot off). Rebuild, flash, confirm the device boots.
3. Only if steps 1 and 2 both succeed individually do you combine them
   and proceed to step 1 of the release hardening below.

Each of these sub-steps also burns eFuses — do them on the target board
and only after you're ready to keep that board paired to your signing
key / flash-enc key.

## 1. Generate the production signing key **offline**

On an air-gapped machine:

```bash
espsecure.py generate_signing_key --version 2 production_signing_key.pem
```

Back it up to at least two separate secure locations (hardware token, offline
encrypted drive). **If this key is lost, the device can never receive another
OTA update.** If it is leaked, signed malicious firmware can be installed.

## 2. Replace the dev signing key in this repo

Keep it gitignored (`.gitignore` already covers `*.pem`). Update
`CONFIG_SECURE_BOOT_SIGNING_KEY` if you renamed the file.

## 3. Flip Flash Encryption to release mode

Edit `sdkconfig`:

```kconfig
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=n
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
```

And ensure:

```kconfig
CONFIG_SECURE_BOOT_ALLOW_JTAG=n
```

## 4. Build & flash one more time, *then* burn eFuses

```bash
idf.py build
idf.py encrypted-flash
```

On this flash, the bootloader writes the flash encryption key to the eFuse
block and locks it. **After this step JTAG is disabled forever and the device
can no longer be re-flashed with plaintext images.** OTA and signed
re-flash via `encrypted-flash` still work.

## 5. Burn the secure boot aggregate key (if not already)

Secure Boot V2 burns the public-key-hash eFuse automatically during the
signed-image boot sequence. Verify with:

```bash
espefuse.py summary
```

Look for `SECURE_BOOT_EN = 1` and the burned `KEY*_DIGEST` entries.

## 6. Disable UART bootloader download mode

This prevents someone from dropping the chip into download mode to attempt
plaintext recovery:

```bash
espefuse.py burn_efuse DIS_DOWNLOAD_MODE
```

Irreversible. Do this only after you are confident OTA works. You cannot run
`idf.py flash` on this chip again — only OTA-style updates.

## 7. Disable the USB-JTAG peripheral

The onboard USB2 port defaults to USB-JTAG on this board via the
`USB-JTAG` / `USB-OTG` 0 Ω jumper. Ensure the jumper is set to `USB-OTG`
(for TinyUSB); if you want to also block JTAG at the silicon level:

```bash
espefuse.py burn_efuse DIS_USB_JTAG
```

## 8. Verification

- Power-cycle the board. It must boot normally and run the application.
- `esptool.py read_flash 0x0 0x10000 dump.bin` → `dump.bin` must be
  ciphertext (no recognisable strings).
- Attempt to flash a known-unsigned binary via OTA → must be rejected at
  `esp_ota_end` with `ESP_ERR_OTA_VALIDATE_FAILED`.
- JTAG attach must fail (if disabled).

## 9. Record the outcome

- Signed-key backup locations.
- eFuse summary snapshot (`espefuse.py summary > board-<serial>.txt`).
- Date, firmware git SHA, signing-key SHA-256.

Store this record with the device's audit trail.
