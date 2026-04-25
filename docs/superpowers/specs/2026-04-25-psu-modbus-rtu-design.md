# PSU Modbus-RTU Master — Design

**Date:** 2026-04-25
**Topic:** UART-attached programmable DC power supply control via Modbus-RTU
**Target supply:** Riden RD60xx family (RD6006 / RD6012 / RD6018)
**Transport:** UART1 @ 19200-8N1, half-duplex, plain TTL (no RS-485 transceiver)

---

## 1. Goal

Add a 5th controllable subsystem to Fan-TestKit so the user can drive a Riden
RD60xx programmable bench supply from the same dashboard / HID host / CDC stream
/ CLI that already drives PWM, RPM capture, GPIO, and the relay power switch.

Core operations in v1: set output voltage, set current limit, output ON/OFF,
read measured voltage, read measured current. Plus an NVS-persisted Modbus
slave address (default 1).

Out of scope for v1: protection limits (OVP/OCP), CV/CC mode bit, temperature,
energy counters, battery-mode registers — these are a follow-up once v1 ships.

---

## 2. Architectural placement

Adheres to the **two existing project invariants** (see `CLAUDE.md`):

### 2.1 Single logical handler, multiple transport frontends

```text
Wi-Fi WebSocket  ──┐
USB HID 0x05     ──┤
USB CDC 0x40..43 ──┼──► control_task ──► psu_modbus_set_*() ──► UART1 ──► RD60xx
CLI psu_*        ──┘
```

`control_task` gains four new `ctrl_cmd_kind_t` values
(`CTRL_CMD_PSU_SET_VOLTAGE`, `_SET_CURRENT`, `_SET_OUTPUT`, `_SET_SLAVE`). All
frontends translate their wire format and post a `ctrl_cmd_t`. The handler
calls `psu_modbus_set_*()`. There is exactly one logical handler.

Telemetry path: `psu_task` polls the supply at 5 Hz, publishes 5 atomic
`uint32` (bit-punned floats / bools) plus a `link_ok` flag. `telemetry_task`
(20 Hz WS push) reads these atomics into the existing JSON status frame. HID
host gets a parallel push via CDC op `0x44` at 5 Hz.

### 2.2 Components never depend on `main`

New component `psu_modbus` lives at the same layer as `gpio_io` / `pwm_gen` /
`rpm_cap`. Its public header is in `components/psu_modbus/include/`. The four
new `ctrl_cmd_kind_t` enum values and union members go in
`components/app_api/include/app_api.h` so frontends can post commands without
depending on `main`.

---

## 3. Hardware

### 3.1 Pin assignment

| Function     | GPIO | Notes                                              |
|--------------|------|----------------------------------------------------|
| PSU UART TX  | 38   | UART1 TXD — wires to supply RX                     |
| PSU UART RX  | 39   | UART1 RXD — wires to supply TX                     |

GPIO 38 and 39 are unused by the existing design (no peripheral conflict, no
strap-pin hazard). Both are exposed on the board's outer header. Kconfig
options `CONFIG_APP_PSU_UART_TX_GPIO` / `_RX_GPIO` allow override.

GND is shared via the common ground rail (the supply's TTL daughterboard
expects 3.3 V signal levels referenced to a common GND).

### 3.2 UART configuration

| Param      | Value          |
|------------|----------------|
| Port       | UART_NUM_1     |
| Baud       | 19200 (Kconfig overridable, `CONFIG_APP_PSU_UART_BAUD`) |
| Data bits  | 8              |
| Parity     | None           |
| Stop bits  | 1              |
| Flow ctrl  | None           |
| RX buffer  | 256 B          |
| TX buffer  | 256 B          |

UART0 (GPIO 43/44) remains the CH343 console — untouched. UART2 free for
future use.

---

## 4. Modbus-RTU master implementation

### 4.1 Why hand-rolled, not `esp-modbus`

ESP-IDF's `esp-modbus` component covers RTU + TCP, master + slave, with object
dictionaries and serialiser. It's overkill for our 5 registers and 3 function
codes, and adds another component-manager pin to maintain alongside
`esp_tinyusb` / `mdns` / `cjson`. Hand-rolled master is ~150 LoC and easier
to audit. Cost of "we'll add another supply later" is hypothetical — YAGNI.

### 4.2 Function codes used

| FC   | Name                    | Use                                  |
|------|-------------------------|--------------------------------------|
| 0x03 | Read Holding Registers  | Telemetry poll, model detection      |
| 0x06 | Write Single Register   | set V, set I, output on/off          |

(0x10 / 0x17 explicitly not used — every write in v1 is a single 16-bit
register.)

### 4.3 Frame format (RTU)

```text
master → slave : [slave][fc][hi(addr)][lo(addr)][hi(N)][lo(N)][lo(crc)][hi(crc)]
slave  → master: [slave][fc][bytecount][...data...][lo(crc)][hi(crc)]    (FC=0x03)
                 [slave][fc][hi(addr)][lo(addr)][hi(val)][lo(val)][lo(crc)][hi(crc)]  (FC=0x06 echo)
                 [slave][fc|0x80][exception][lo(crc)][hi(crc)]            (error)
```

CRC-16 polynomial 0xA001, init 0xFFFF (Modbus standard).

### 4.4 Riden RD60xx register map

Source: Ruideng's Modbus protocol document; cross-checked against the
open-source `Riden-Modbus` Home Assistant integration.

| Addr  | Name      | Scale         | RW | Notes                                    |
|-------|-----------|---------------|----|------------------------------------------|
| 0x00  | MODEL     | int           | R  | 60062=RD6006, 60121=RD6012, 60181=RD6018 |
| 0x08  | V_SET     | ÷100 → V      | RW | uint16, 0..6000 = 0..60.00 V             |
| 0x09  | I_SET     | ÷N → A        | RW | uint16; N=1000 for RD6006, N=100 for RD6012/6018 |
| 0x0A  | V_OUT     | ÷100 → V      | R  | actual output voltage                    |
| 0x0B  | I_OUT     | ÷N → A        | R  | actual output current (same N as I_SET)  |
| 0x12  | OUTPUT    | 0/1           | RW | 0=off, 1=on                              |

**Model-dependent current scale**: the I-axis divisor differs by model. Read
register 0x00 once at boot, store `s_i_scale = 1000.0f or 100.0f`. Unknown
model logs `WARN` and falls back to ÷1000. Exposed via `psu_modbus_get_model()`
so the dashboard can render correct slider precision.

### 4.5 Timing

| Rule                       | Value at 19200 baud   |
|----------------------------|-----------------------|
| Inter-frame gap (3.5 char) | ~1.8 ms; we use 2 ms  |
| Per-transaction timeout    | 100 ms                |
| Telemetry poll period      | 200 ms (5 Hz)         |
| Setpoint write coalesce    | front-of-queue        |

`uart_wait_tx_done()` after every TX so we know the last byte left the wire
before we try to read. `uart_read_bytes()` with the 100 ms timeout reads the
full expected response length in one shot (Modbus response lengths are known
from the function code and request).

### 4.6 Link health

- Counter `s_consecutive_failures` increments on timeout / CRC mismatch /
  exception response.
- After **5** consecutive failures, `link_ok` flips to false; `WARN` log fires.
- A single successful response resets the counter and flips `link_ok` back to
  true; `INFO` log fires.
- Avoids log spam when the cable is unplugged: only state transitions log.

### 4.7 Setpoint coalescing

Transaction queue depth = 4. Setpoint writes from `control_task` enqueue at
the *front* (priority over telemetry polls). If the queue already holds a
pending write of the *same kind* (`set_v` / `set_i` / `set_out` /
`set_slave`), the new value replaces it in place — fast slider drags
collapse to roughly one transaction per Modbus round trip (~30 ms typical
at 19200 baud). Telemetry polls go to the back. The queue is guarded by a
short mutex (the only shared state between `control_task` and `psu_task`).

A 100 ms transaction timeout combined with one in-flight transaction means
worst-case slider response is 100 ms after the previous transaction
finishes. That is acceptable for a setpoint UI.

### 4.8 Slave address change semantics

`psu_modbus_set_slave_addr()` writes to NVS *only*. It does **not** issue a
Modbus write to change the supply's own address (that would require knowing
the current address to address the write to it, which is the chicken-and-egg
case the user is trying to fix). The user is expected to set the supply's
slave address from the supply's own front panel; the firmware just needs to
match it.

---

## 5. `psu_modbus` component API

```c
// components/psu_modbus/include/psu_modbus.h
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t psu_modbus_init(void);    // brings up UART, loads slave addr from NVS
esp_err_t psu_modbus_start(void);   // creates psu_task, fires model-detect read

// Setpoint writes — single-writer (called from control_task)
esp_err_t psu_modbus_set_voltage(float v);   // 0.00 .. 60.00 V (clamped)
esp_err_t psu_modbus_set_current(float i);   // 0.00 .. model_max A (clamped)
esp_err_t psu_modbus_set_output(bool on);

// Slave address (NVS-persisted, used by next transaction)
uint8_t   psu_modbus_get_slave_addr(void);
esp_err_t psu_modbus_set_slave_addr(uint8_t addr);   // 1..247

// Telemetry — lock-free atomic loads, OK from any task
typedef struct {
    float    v_set;
    float    i_set;
    float    v_out;
    float    i_out;
    bool     output_on;
    bool     link_ok;
    uint16_t model_id;       // raw register 0x00; 0 if not yet detected
    float    i_scale_div;    // 100.0 or 1000.0
} psu_modbus_telemetry_t;

void psu_modbus_get_telemetry(psu_modbus_telemetry_t *out);

#ifdef __cplusplus
}
#endif
```

---

## 6. `app_api` extensions

```c
// components/app_api/include/app_api.h — additions only

typedef enum {
    /* ... existing values ... */
    CTRL_CMD_PSU_SET_VOLTAGE,
    CTRL_CMD_PSU_SET_CURRENT,
    CTRL_CMD_PSU_SET_OUTPUT,
    CTRL_CMD_PSU_SET_SLAVE,
} ctrl_cmd_kind_t;

typedef struct {
    ctrl_cmd_kind_t kind;
    union {
        /* ... existing members ... */
        struct { float v;       } psu_set_voltage;
        struct { float i;       } psu_set_current;
        struct { uint8_t on;    } psu_set_output;
        struct { uint8_t addr;  } psu_set_slave;
    };
} ctrl_cmd_t;
```

---

## 7. `control_task` extensions

Four new switch cases drain the queue and call into `psu_modbus_*`:

```c
case CTRL_CMD_PSU_SET_VOLTAGE: psu_modbus_set_voltage(c.psu_set_voltage.v); break;
case CTRL_CMD_PSU_SET_CURRENT: psu_modbus_set_current(c.psu_set_current.i); break;
case CTRL_CMD_PSU_SET_OUTPUT:  psu_modbus_set_output(c.psu_set_output.on != 0); break;
case CTRL_CMD_PSU_SET_SLAVE:   psu_modbus_set_slave_addr(c.psu_set_slave.addr); break;
```

No boot-time setpoint write. The PSU holds its own state on power loss; the
firmware reflects it via the first telemetry poll. (Different from PWM, where
firmware *defines* the boot setpoint.)

---

## 8. Frontends

### 8.1 CLI (`main/app_main.c`)

```text
psu_v <volts>          # set output voltage
psu_i <amps>           # set current limit
psu_out <0|1>          # output enable
psu_slave <1..247>     # change Modbus slave addr (NVS)
psu_status             # print v_set/i_set/v_out/i_out/on/link/model
```

The existing `status` command also gains a one-line PSU summary so a single
command shows the full system snapshot.

### 8.2 WebSocket JSON

Client → device:

```json
{ "type": "set_psu_voltage", "v": 12.0  }
{ "type": "set_psu_current", "i": 2.5   }
{ "type": "set_psu_output",  "on": true }
{ "type": "set_psu_slave",   "addr": 1  }
```

Device → client (ack pattern unchanged):

```json
{ "type": "ack", "op": "set_psu_voltage", "ok": true }
```

20 Hz status frame extended:

```json
{
  "type": "status",
  /* ... existing fields ... */
  "psu": {
    "v_set": 12.00,  "i_set": 2.500,
    "v_out": 11.98,  "i_out": 1.234,
    "output": true,  "link": true,
    "model": "RD6006", "slave": 1
  }
}
```

### 8.3 HID report 0x05 (OUT, 8 bytes)

```text
byte 0 : op
         0x10 = set_voltage   (bytes 1..4 = float LE)
         0x11 = set_current   (bytes 1..4 = float LE)
         0x12 = set_output    (byte  1 = 0|1)
         0x13 = set_slave     (byte  1 = addr, byte 5 = 0xA5 magic)
bytes 1..7 : op payload (zero-padded)
```

Magic byte 0xA5 on `set_slave` mirrors the factory-reset guard pattern — a
stray zeroed report cannot silently retarget the bus.

HID descriptor grows by 10 bytes (one TLC for report 0x05).
`HID_REPORT_DESC_SIZE` macro updates from 63 → 73 and the
`_Static_assert(sizeof(usb_hid_report_descriptor) == 73)` in
`usb_descriptors.c` catches drift.

### 8.4 CDC SLIP ops

| Op   | Direction | Payload                         | Meaning              |
|------|-----------|---------------------------------|----------------------|
| 0x40 | host→dev  | float LE (4 B)                  | set_psu_voltage      |
| 0x41 | host→dev  | float LE (4 B)                  | set_psu_current      |
| 0x42 | host→dev  | u8 (1 B)                        | set_psu_output       |
| 0x43 | host→dev  | u8 addr + 0xA5 magic (2 B)      | set_psu_slave        |
| 0x44 | dev→host  | 4×float + u8 flags (17 B)       | psu_telemetry @ 5 Hz |

Acks via existing CDC ack op (`0x21`-style) keyed by request op ID.

### 8.5 Dashboard panel

New collapsible "Power Supply" panel above the GPIO panel:

```text
┌─ Power Supply (Riden RD6006, slave 1)  [link ●] [▼] ─────┐
│                                                          │
│  V set [────●────────] 12.00 V    measured 11.98 V       │
│  I set [──●──────────]  2.500 A   measured  1.234 A      │
│                                                          │
│  Output:  [ OFF ] [ ON ]                                  │
│                                                          │
│  ⚙ Settings: slave addr [ 1 ] [Save]                     │
└──────────────────────────────────────────────────────────┘
```

Follows the same panel architecture as the PWM and GPIO panels:

- `commit-on-release` for sliders — no `lastSent` cache.
- `setFromDevice(telemetry)` updates `panel.current` from the WS status frame.
- Slider ranges keyed off `device_info.psu_model_name`.
- Link indicator: green dot when `psu.link === true`, red otherwise.
- i18n strings (zh-Hant / zh-Hans / en) added following existing translation
  patterns; `translation-sync` skill covers verification.

### 8.6 `device_info` HTTP fields

```json
{
  "psu_uart_tx_pin": 38,
  "psu_uart_rx_pin": 39,
  "psu_baud":       19200,
  "psu_model_name": "RD6006",
  "psu_i_max":      6.0,
  "psu_v_max":      60.0
}
```

---

## 9. Persistence (NVS)

- Namespace: `psu_modbus`
- Key: `slave_addr` (uint8, 1..247, default 1)

---

## 10. Boot sequence (`app_main`)

```text
nvs_flash_init
pwm_gen_init
rpm_cap_init
gpio_io_init
psu_modbus_init           ← UART1 up, slave addr from NVS
ota_core_init
control_task_start
psu_modbus_start          ← task runs; first poll fires within 200 ms
boot PWM setpoint
boot power_switch OFF
usb_composite_start
net_dashboard_start
ota_core_mark_current_image_valid
start_console
```

`psu_modbus_start` triggers a one-shot model-detect read of register 0x00.
If it times out, the task still runs and `link_ok` stays false; the dashboard
shows "PSU offline". A successful poll later flips `link_ok` true and refreshes
the panel — supports plug-in-after-boot.

---

## 11. Files touched

```text
NEW   components/psu_modbus/CMakeLists.txt
NEW   components/psu_modbus/include/psu_modbus.h
NEW   components/psu_modbus/psu_modbus.c       (~400 LoC)
EDIT  components/app_api/include/app_api.h
EDIT  main/Kconfig.projbuild                   (PSU_UART_TX/RX/BAUD/SLAVE_DEFAULT)
EDIT  main/idf_component.yml                   (no new components — hand-rolled)
EDIT  main/app_main.c                          (init order + 5 CLI commands)
EDIT  main/control_task.c                      (4 new switch cases)
EDIT  components/net_dashboard/ws_handler.c    (4 new WS msg types + status JSON)
EDIT  components/net_dashboard/device_info.c   (psu_* fields)
EDIT  components/net_dashboard/dashboard/...   (HTML/CSS/JS panel + i18n)
EDIT  components/usb_composite/include/usb_protocol.h  (HID 0x05 + CDC 0x40..0x44)
EDIT  components/usb_composite/usb_descriptors.c       (descriptor + assert)
EDIT  components/usb_composite/usb_composite.c         (HID parse + CDC dispatch)
EDIT  CLAUDE.md                                (UART pin reservation note)
```

---

## 12. Testing strategy

1. **Component-level unit reasoning** (no test harness on this project — same
   as gpio_io shipped). Code-review the CRC-16 against a known-good vector
   (request `01 03 00 08 00 05 [crc]` → CRC = `04 09`).

2. **Hardware-in-the-loop** with a real RD6006:
   - Flash, watch CDC log for "psu_modbus: detected RD6006 (model 60062)".
   - From CLI: `psu_v 5.0`, observe supply display and the `psu_status`
     output. Repeat for `psu_i 1.0`, `psu_out 1`, `psu_out 0`.
   - From dashboard: drag V slider, confirm supply tracks.
   - Unplug PSU UART cable, verify `link_ok` flips false within ~1 s, log
     fires once, dashboard greys out the panel. Plug back in, verify recovery.

3. **No-PSU-attached path**: boot with PSU disconnected, verify firmware
   stays up, telemetry frame reports `psu.link=false`, all other subsystems
   work normally.

4. **CRC error injection** (deferred — not blocking v1): manually corrupt a
   response byte in firmware, verify the failure counter trips and the link
   recovers on the next good response.

---

## 13. Risks and open questions

1. **Bus contention** — only one master on the bus. If the user already has
   the supply paired with the official Riden BLE module / phone app, two
   masters will collide. Document in `CLAUDE.md` that the BLE pairing must
   be removed.

2. **Model-coverage drift** — if Ruideng releases a RD6024 with a different
   I-scale, our detect code falls back to ÷1000 with a WARN. User can
   override via `CONFIG_APP_PSU_I_SCALE_OVERRIDE` at compile time as a
   workaround until firmware adds the model ID.

3. **No protection-limit visibility in v1** — if the user sets I=10 A on
   an RD6006 (max 6 A), the supply silently clamps. v1 firmware doesn't
   read OVP/OCP registers. Mitigated by clamping the dashboard slider to
   `psu_i_max` from `device_info`.

4. **HID descriptor breaking change** — adding report 0x05 changes the
   descriptor bytes the host tool sees. Documented in the host tool's
   spec; bump `usb_protocol.h`'s implicit version (already covered by
   per-op IDs being stable). The `_Static_assert` catches local drift.

---

## 14. Out of scope / follow-ups

- OVP/OCP read & set (registers 0x82, 0x83, 0x60, 0x61).
- Battery / charge mode registers.
- CV/CC mode flag in telemetry.
- Internal/external temperature in telemetry.
- Multi-PSU chaining (Modbus supports it, our v1 hardcodes one slave).
- RS-485 transceiver support (would need DE pin GPIO; out of scope).
- ESP-Modbus migration if requirements outgrow the hand-rolled master.
