# PSU Multi-Driver Support (WZ5005 + XY-SK120)

**Date:** 2026-04-26
**Branch (target):** `feature/psu-multi-driver` (to be cut from current
`feature/psu-modbus-rtu` head once design is approved)
**Supersedes (extends):** `docs/superpowers/specs/2026-04-25-psu-modbus-rtu-design.md`

## Summary

Add support for two additional bench PSUs alongside the already-shipped
Riden RD60xx integration:

- **XY-SK120** — Modbus-RTU on a different register map. Same wire
  framing as Riden, mostly mechanical change.
- **WZ5005** — custom 20-byte fixed-length frames with a sum-mod-256
  checksum. Different wire framing from Riden/XY-SK120.

A single **`psu_family`** runtime selector (NVS-persisted, dashboard +
CLI editable, default `riden`) decides which backend owns UART1 for the
current boot. Only one PSU is active at a time.

The public API the rest of the firmware calls
(`psu_*_set_voltage / set_current / set_output / set_slave_addr` and
`psu_*_get_telemetry / get_model_name / get_i_max`) stays semantically
the same — **the names get renamed from `psu_modbus_*` to `psu_driver_*`**
because "modbus" no longer describes half the backends.

## Why this matters

The current `components/psu_modbus/` is hard-wired to Modbus-RTU + the
Riden register map. Two project goals make that brittle:

1. The bench is being expanded with a WZ5005 unit, which uses a custom
   non-Modbus protocol. Keeping the "modbus" name on a component that
   dispatches to a sum-checksum protocol would mislead anyone reading
   the code in 6 months.
2. XY-SK120 is the third PSU planned. Its wire framing is identical to
   Riden's; only the register map differs. If we delay extracting a
   shared Modbus-RTU helper until XY-SK120 lands, that work becomes a
   retrofit through code that's already grown around the helpers.

Doing the rename + helper extraction now (driven by WZ5005's forcing
function) means XY-SK120 lands later as ~80 lines of register table +
family-enum entry, with no further restructuring.

## Non-goals

- **Wire-protocol surface stays unchanged.** No new HID report bytes,
  no new CDC ops, no new WS JSON fields beyond a single `psu_family`
  string in the existing `psu` block. The PC host tool keeps working
  unmodified.
- **No widening of telemetry beyond Riden parity** — v_set / i_set /
  v_out / i_out / output_on / link_ok only. WZ5005 + XY-SK120 expose
  power / energy / temperature / OVP / OCP at the protocol level; we
  ignore those for v1 to keep the wire contract frozen.
- **No PSU-family setpoint over HID/CDC.** Family is a configuration
  decision (closer to Wi-Fi creds than to "set voltage") and gets the
  NVS + dashboard + CLI surface only. Same posture as `psu_slave`
  selection.
- **No automatic detection.** User explicitly picks the family. Detect
  attempts on a custom-checksum protocol risk false-positives on noise.

## Background — what we know about each protocol

### Riden RD60xx (already implemented)

Modbus-RTU, FC 0x03 (read holding) + FC 0x06 (write single). Register
map in `psu_modbus.c:25-30`:

```text
REG_MODEL  = 0x0000   (60062=RD6006, 60121=RD6012, 60181=RD6018, ...)
REG_V_SET  = 0x0008   (raw / 100  = volts)
REG_I_SET  = 0x0009   (raw / scale = amps; scale = 100 or 1000 by model)
REG_V_OUT  = 0x000A
REG_I_OUT  = 0x000B
REG_OUTPUT = 0x0012
```

Default UART: 19200-8N1. Slave addr range 1..247.

### XY-SK120 (new, Modbus-RTU)

Reference: <https://github.com/csvke/XY-SK120-Modbus-RTU-TTL>
(`lib/XY-SKxxx/XY-SKxxx.h`).

Same wire framing as Riden. Different register map:

```text
REG_V_SET  = 0x0000   (raw / 100  = volts, 2 decimals)
REG_I_SET  = 0x0001   (raw / 1000 = amps,  3 decimals)
REG_V_OUT  = 0x0002
REG_I_OUT  = 0x0003
REG_OUTPUT = 0x0012   (same offset as Riden — coincidence)
REG_MODEL  = 0x0016   (XY-SK120 returns 22873)
REG_SLAVE  = 0x0018   (read/write, 0..247)
```

Default UART: **115200**-8N1 (vs Riden's 19200). Slave addr range 0..247.
**i_max = 5.0 A** (XY-SK120 is rated 5 A continuous; XY-SK80 sibling is
8 A but we're only adding the one model in v1).

### WZ5005 (new, custom protocol)

References: `docs/WZ5005数控电源说明书.pdf` (manufacturer manual, has
gaps) + <https://github.com/kordian-kowalski/wz5005-control/blob/main/protocol.md>
(community reverse-engineering, also has TODOs).

20-byte fixed-length frame on the wire:

```text
[0]    sync header   = 0xAA  (constant)
[1]    device addr   = 1..255
[2]    command       = 0x20..0x2C
[3..18] command args / response payload, zero-padded
[19]   checksum      = sum(buf[0..18]) mod 256
```

Verified the checksum against both example frames in the
kordian-kowalski reference (the "remote mode" frame and the
"set voltage to 5V" frame both compute 0xCC and 0x6A respectively under
sum-mod-256). Confirmed.

The "Set voltage to 5V" example in the reference encodes 5V as
`0x13 0x88` (= 5000) at bytes 3..4 → **scale is millivolts (÷1000)**, big-endian. The
manual's other multi-byte fields are described as "high byte / next-highest
byte / next-lower byte / low byte" → big-endian read order across the frame.

Op codes used in v1 (subset of the full op-code list):

```text
0x20  Set operating mode    (arg byte 0: 0=manual, 1=remote)
0x21  Set device address    (arg byte 0: new addr)
0x22  Set output on/off     (arg byte 0: 0=off, 1=on)
0x23  Read output state     (resp byte 0: out_on, byte 1: cv/cc, byte 2: abnormal)
0x2B  Read voltage/current/protection block (returns V_SET / I_SET / OVP / OCP /
                                              V_OUT / I_OUT in big-endian pairs)
0x2C  Write voltage/current/protection block (mirror of 0x2B)
```

Default UART: 19200-8N1 (matches existing). Slave addr range 1..255.

**Known unknowns** (will iterate against real hardware once on bench):

- The exact byte offsets inside the 0x2B/0x2C payload for V_OUT / I_OUT
  vs setpoints are inferred from the manual's table layout and the
  "Set voltage to 5V" example, not independently verified.
- Whether `0x23` byte 1 (CV/CC mode) actually populates on a unit not
  in remote mode.
- Whether the device requires `0x20 1` (enter remote mode) before any
  write, or if writes work in manual mode too.

These are end-to-end testable: a wrong frame layout produces persistent
timeouts, `link_ok` stays false, dashboard shows "PSU offline." Same
posture as the original Riden integration — no boot-time self-check.

## Component layout

```text
components/psu_driver/                        (renamed from psu_modbus/)
  CMakeLists.txt                              (SRCS list grows; REQUIRES unchanged)
  Kconfig.projbuild                           (renamed APP_PSU_* keys kept; new defaults)
  include/
    psu_driver.h                              (renamed from psu_modbus.h; same public API)
    psu_modbus_rtu.h                          (NEW — shared between psu_riden + psu_xy_sk120)
    psu_backend.h                             (NEW — internal vtable, not exported to other components)
  psu_driver.c                                (NEW — public API, family dispatch, common atomic state, NVS,
                                                psu_task; replaces psu_modbus.c top half)
  psu_modbus_rtu.c                            (NEW — CRC-16, FC 0x03/0x06 builders, psu_txn primitive;
                                                lifted from current psu_modbus.c, no logic change)
  psu_riden.c                                 (NEW — Riden register map + RD_MODELS table; uses
                                                psu_modbus_rtu helpers; replaces psu_modbus.c bottom half)
  psu_xy_sk120.c                              (NEW — XY-SK120 register map + model id 22873;
                                                uses same psu_modbus_rtu helpers)
  psu_wz5005.c                                (NEW — 20-byte custom-frame builder + sum checksum +
                                                op-code dispatch; standalone, no Modbus dep)
```

The existing `components/psu_modbus/psu_modbus.c` is **deleted** in this
work — every line of it migrates to one of the four new `.c` files
above. No "compatibility shim" or temporary `psu_modbus.h` redirect.

### Internal vtable (psu_backend.h)

```c
typedef struct psu_backend {
    const char *name;                       // "riden" / "xy_sk120" / "wz5005"
    esp_err_t (*init)(void);                // backend-specific UART config (baud)
    esp_err_t (*detect)(uint16_t *raw_id);  // sets s_model_id, s_i_scale, s_i_max
    esp_err_t (*poll)(void);                // 5 Hz tick: read v/i set+out, output state, write atomics
    esp_err_t (*set_voltage)(float v);
    esp_err_t (*set_current)(float i);
    esp_err_t (*set_output)(bool on);
    // No set_slave — slave addr selection is family-agnostic, lives in psu_driver.c
} psu_backend_t;

extern const psu_backend_t psu_backend_riden;
extern const psu_backend_t psu_backend_xy_sk120;
extern const psu_backend_t psu_backend_wz5005;
```

`psu_driver.c` holds:

- A `static const psu_backend_t *s_backend = &psu_backend_riden;` resolved
  once at `psu_driver_init()` time from the NVS `psu_family` key
  (default `riden`).
- The atomic-published telemetry state (`s_v_set_bits` etc.) — backends
  write to it via `psu_driver_publish_*()` helpers exposed in
  `psu_backend.h`.
- The polling task. Backends supply `poll()`; the task is shared.
- The UART mutex. Backends call `psu_modbus_rtu_txn()` (Modbus
  family) or roll their own `psu_wz5005_txn()` (which still uses the
  same shared mutex).
- NVS load/save for both `slave_addr` and the new `psu_family` key.

### Public API (psu_driver.h)

Drop-in identical to today's `psu_modbus.h` semantically; only the
prefix changes:

```c
typedef struct {
    float    v_set, i_set, v_out, i_out;
    bool     output_on, link_ok;
    uint16_t model_id;
    float    i_scale_div;
} psu_driver_telemetry_t;

esp_err_t  psu_driver_init(void);
esp_err_t  psu_driver_start(void);
esp_err_t  psu_driver_set_voltage(float v);
esp_err_t  psu_driver_set_current(float i);
esp_err_t  psu_driver_set_output(bool on);
uint8_t    psu_driver_get_slave_addr(void);
esp_err_t  psu_driver_set_slave_addr(uint8_t addr);   // 1..247 (Modbus) or 1..255 (WZ5005)
void       psu_driver_get_telemetry(psu_driver_telemetry_t *out);
const char *psu_driver_get_model_name(void);
float      psu_driver_get_i_max(void);

// New:
const char *psu_driver_get_family(void);              // "riden" | "xy_sk120" | "wz5005"
esp_err_t   psu_driver_set_family(const char *name);  // NVS-persists; effective on NEXT boot
```

`set_family` is **boot-effective**, not hot-swappable. Switching family
mid-boot would mean tearing down the UART driver, swapping baud (Riden
19200 / WZ5005 19200 / XY-SK120 115200), reconfiguring backend state —
doable but risky, and the user changes PSU model rarely (bench setup,
not per-test). Keeping it boot-effective is one config-write +
`esp_restart()`, mirroring how `factory_reset` works.

The dashboard surfaces this with: dropdown change → POST `set_psu_family`
WS message → device persists + ACKs → user reboots manually OR an
autoreboot toggle (default off — explicit reboot button on the panel).

## Slave address range — the off-by-zero gotcha

| Family    | Min | Max | Notes |
|-----------|-----|-----|-------|
| Riden     | 1   | 247 | Modbus standard |
| XY-SK120  | 1   | 247 | Reference allows 0..247; we cap at 1 to match Riden + spec |
| WZ5005    | 1   | 255 | Manual: 1..255; reference: 0x00..0xFF (0 disallowed for safety) |

`psu_driver_set_slave_addr()` validates against the active family's
range. Setting slave to 248 with `riden` returns `ESP_ERR_INVALID_ARG`;
same call with `wz5005` succeeds.

## File-level rename surface

This refactor renames symbols **across components**. All callers must
move atomically in the same commit:

| File | Edit |
|------|------|
| `components/psu_modbus/` | **deleted** (replaced by `components/psu_driver/`) |
| `main/CMakeLists.txt` | `psu_modbus` → `psu_driver` in REQUIRES |
| `main/app_main.c` | `#include "psu_modbus.h"` → `psu_driver.h`; `psu_modbus_*` → `psu_driver_*` (4 sites) |
| `main/control_task.c` | same include + symbol rename (4 sites) |
| `components/usb_composite/CMakeLists.txt` | `psu_modbus` → `psu_driver` in REQUIRES |
| `components/usb_composite/usb_cdc_task.c` | include + telemetry call rename |
| `components/net_dashboard/CMakeLists.txt` | `psu_modbus` → `psu_driver` in REQUIRES |
| `components/net_dashboard/net_dashboard.c` | include + telemetry calls |
| `components/net_dashboard/ws_handler.c` | include + telemetry calls + new `set_psu_family` op + `family` field on the `psu` JSON block |
| `components/net_dashboard/web/index.html` + `app.js` + `app.css` | dashboard dropdown for family + reboot button |
| `CLAUDE.md` | "PSU Modbus-RTU master" section title + namespace name + "5th controllable subsystem" diagram update + invariant note about UART1 baud being family-dependent |
| `main/Kconfig.projbuild` | `APP_PSU_*` keys keep their names but `APP_PSU_UART_BAUD` default becomes "depends on default family" — see Kconfig section below |

Every grep hit in the codebase for `psu_modbus` is rewritten in this
single commit. The plan step that does this rename is mechanical (one
multi-file find/replace), but the test pass after it must be done on
hardware — symbol renames sometimes mask integration regressions a
build-only check misses.

## Kconfig changes

```kconfig
# (existing)
config APP_PSU_UART_TX_GPIO         # unchanged
config APP_PSU_UART_RX_GPIO         # unchanged
config APP_PSU_SLAVE_DEFAULT        # unchanged

# (new)
choice APP_PSU_DEFAULT_FAMILY
    prompt "Default PSU family (used when NVS key absent)"
    default APP_PSU_DEFAULT_FAMILY_RIDEN
config APP_PSU_DEFAULT_FAMILY_RIDEN
    bool "Riden RD60xx (Modbus-RTU)"
config APP_PSU_DEFAULT_FAMILY_XY_SK120
    bool "XY-SK120 (Modbus-RTU)"
config APP_PSU_DEFAULT_FAMILY_WZ5005
    bool "WZ5005 (custom 20-byte protocol)"
endchoice

# (changed)
config APP_PSU_UART_BAUD
    int "UART1 baud (matches the PSU's panel-set rate)"
    default 19200 if APP_PSU_DEFAULT_FAMILY_RIDEN
    default 115200 if APP_PSU_DEFAULT_FAMILY_XY_SK120
    default 19200 if APP_PSU_DEFAULT_FAMILY_WZ5005
```

Note: the baud is a **defaults-only** Kconfig — the runtime
`psu_family` switch does NOT auto-change baud, because the PSU's own
panel decides the baud and the user must match it. Switching family
in NVS therefore means: (1) set the new family, (2) reboot, (3) if
the family change implies a different default baud, the user must
either rebuild OR manually set `APP_PSU_UART_BAUD` to match what the
new PSU is configured for. We log a warning if `link_ok` doesn't
recover within 5 s after a family change.

## CLI surface

Two new CLI commands (CLI is not a wire-contract surface):

```text
psu_family                 # prints current family
psu_family <name>          # NVS-set family; warns "reboot to apply"
                           #   <name> ∈ {riden, xy_sk120, wz5005}
```

`psu_slave` and the existing PSU CLI (`psu_v / psu_i / psu_out /
psu_status`) work unchanged — they delegate through `psu_driver_*`
which routes to the active backend.

## WebSocket / dashboard surface

The existing `psu` block in the 20 Hz status frame gains one field:

```jsonc
{
  "type": "status",
  "psu": {
    "v_set": 5.0,  "i_set": 1.5,  "v_out": 4.99, "i_out": 1.48,
    "output_on": true, "link_ok": true,
    "model_name": "XY-SK120",
    "i_max": 5.0,
    "slave": 1,
    "family": "xy_sk120"           // NEW
  }
}
```

New client→device WS op:

```jsonc
{ "type": "set_psu_family", "family": "wz5005" }
```

→ device replies `{"type":"ack","op":"set_psu_family","ok":true}` and
persists. UI then prompts the user to reboot (or offers an auto-reboot
button that calls the existing soft-reboot path).

The PSU panel HTML/JS gains:

- A "Family" `<select>` populated from `family`, sending `set_psu_family`
  on commit.
- A "Reboot to apply" button shown when the live `family` differs from
  what's been queued in NVS (the device exposes both via a tiny
  `family_pending` field for one telemetry frame after the set).

## Module wiring (per-backend specifics)

### psu_riden.c

Drops in unchanged from current `psu_modbus.c`. The `RD_MODELS` table
moves here. `psu_riden_init()` configures UART1 for 19200; `detect()`
issues `read_holding(0x0000, 1)`; `poll()` reads `0x0008..0x000B`
(4 regs) + `0x0012` (1 reg) per cycle.

### psu_xy_sk120.c

Same shape as `psu_riden.c`. Constants:

```c
#define XY_SK120_MODEL_ID  22873
#define XY_SK120_I_MAX     5.0f
#define XY_SK120_I_SCALE   1000.0f   // ÷1000, 3 decimals
#define XY_SK120_V_SCALE   100.0f    // ÷100, 2 decimals
#define XY_SK120_BAUD      115200
```

`detect()` issues `read_holding(0x0016, 1)`; expects `22873`. `poll()`
reads `0x0000..0x0003` (4 regs) + `0x0012` (1 reg) per cycle.

XY-SK120 has only one model id we care about, so no model table — a
single equality check covers it. If a future XY-SK80 / XY-SK60 lands,
expand to a table.

### psu_wz5005.c

Self-contained. Helpers:

```c
static size_t  wz5005_build_frame(uint8_t *out, uint8_t addr, uint8_t op,
                                  const uint8_t *args, size_t arg_len);
                                  // pads + appends sum-mod-256 checksum,
                                  // always writes 20 bytes
static esp_err_t wz5005_txn(uint8_t op, const uint8_t *args, size_t arg_len,
                            uint8_t *resp_payload, size_t expected_payload_bytes);
                                  // grabs the shared psu_driver UART mutex;
                                  // expects 20-byte response;
                                  // verifies checksum; copies bytes [3..18]
                                  // up to expected_payload_bytes into resp_payload
```

`detect()` issues op `0x24` (factory info), reads the model byte at
response[3], populates `s_model_id` with that single byte (the WZ5005
manual doesn't enumerate model bytes — for v1 we set `model_name =
"WZ5005"` whenever a non-zero detect response comes back).

`poll()` issues op `0x2B` (read voltage/current/protection block) once
per cycle and op `0x23` once per cycle (output state). Two transactions
per 5 Hz cycle, same pattern as Riden.

`set_voltage(v)` / `set_current(i)` / `set_output(on)` use op `0x2C`
(write block) — the manual indicates 0x2C is a "set" of the same block
0x2B reads, so we read-modify-write: read current full block via 0x2B,
mutate the relevant pair of bytes, write back via 0x2C. Cost: each
setpoint write is two transactions instead of one. If the read fails
(timeout / bad checksum), the write is aborted and the error is
returned to the caller — we never write a frame built from
zero-initialised bytes.

**Alternative considered:** cache the last-read block locally to avoid
the read-side of read-modify-write. Rejected for v1 — the bench dial
on the WZ5005 front panel can change setpoints without our knowledge,
so a stale cached block would clobber a user-driven change. The 5 Hz
poll already reads the block; setpoint writes happen rarely enough
that the extra transaction is invisible (~10 ms at 19200).

`set_slave_addr(addr)` issues op `0x21` to the **current** slave addr
first, then updates the NVS-stored addr to the new value (matching
Riden's pattern: address change is a wire write + NVS update).

### psu_modbus_rtu.c (shared)

Verbatim lift of these symbols from current `psu_modbus.c`:

- `modbus_crc16(buf, len)`
- `build_read_holding(out, slave, addr, n)` (FC 0x03 builder)
- `build_write_single(out, slave, addr, val)` (FC 0x06 builder)
- `verify_crc(buf, len)`
- `psu_modbus_rtu_txn(req, req_len, resp, expect_len)` — wraps the
  UART mutex / write / read / CRC-verify sequence; same logic as
  today's `psu_txn`, but the mutex pointer is supplied by `psu_driver.c`.

Plus convenience wrappers:

```c
esp_err_t psu_modbus_rtu_read_holding(uint8_t slave, uint16_t addr,
                                      uint16_t n, uint16_t *out_regs);
esp_err_t psu_modbus_rtu_write_single(uint8_t slave, uint16_t addr,
                                      uint16_t val);
```

These take `slave` as a parameter because the active slave addr lives
in `psu_driver.c`, not the helper. (Today's `psu_modbus.c` reaches into
its own `s_slave_addr` global; we hoist it out.)

## Boot sequence

```text
app_main()
 └── psu_driver_init()
      ├── load NVS keys: psu_family (default → Kconfig), slave_addr
      ├── resolve s_backend = psu_backend_<family>
      ├── s_backend->init()    // configures UART1 with backend's baud
      │                        // creates s_uart_mutex if first call
      └── log: "PSU driver: family=xy_sk120 baud=115200 slave=1"
 └── psu_driver_start()
      └── creates psu_task (priority 4, 4 KB stack)
           └── psu_task_fn():
                ├── s_backend->detect()        // tries once at task entry
                ├── loop @ 5 Hz:
                │    ├── re-detect if model_id==0 && link_ok==1
                │    ├── s_backend->poll()    // backend-specific reads
                │    └── vTaskDelayUntil
```

The atomic-publish state (`s_v_set_bits` etc.) is owned by
`psu_driver.c`; backends call publish helpers
(`psu_driver_publish_v_set(float)` etc.) declared in `psu_backend.h`.
This keeps the bit-pun + `memory_order_relaxed` discipline in one
place and prevents three backends from drifting on the publish
strategy.

## Concurrency invariants (preserved)

- One UART mutex (`s_uart_mutex` in `psu_driver.c`), shared by all
  backends. Same lock everyone — control_task setpoint writes can't
  interleave with psu_task polls.
- Atomic publish of telemetry (bit-punned floats, relaxed ordering) —
  unchanged.
- `note_txn_result(e)` link-tracking — moves into `psu_driver.c`,
  called by all backends after every UART transaction.
- 5-fail-then-flip threshold for `link_ok` — unchanged.

## Test plan (manual, on hardware)

Each scenario must pass before merge:

1. **Riden regression** — set `psu_family=riden`, reboot, point at
   RD6006 on UART1 @ 19200. Existing dashboard panel works: V/I sliders,
   output toggle, slave change, telemetry. No regression vs current
   `feature/psu-modbus-rtu` head.
2. **XY-SK120 fresh** — set `psu_family=xy_sk120`, reboot, point at
   XY-SK120 @ 115200 (panel-set baud must match). Detect reads
   model id 22873 → `model_name = "XY-SK120"`, `i_max = 5.0`. Set
   voltage to 5.00, observe v_out follows. Set output on/off. Slave
   change persists.
3. **WZ5005 fresh** — set `psu_family=wz5005`, reboot, point at
   WZ5005 @ 19200. Detect → `model_name = "WZ5005"`. Set voltage
   to 5.00 and 12.34, observe front panel display matches (within
   the ÷1000 scale). Toggle output. Read v_out matches actual
   multimeter reading on the supply terminals. Slave change to 5,
   then 1, persists.
4. **Family switch round-trip** — XY-SK120 active → set family
   `wz5005` → reboot → confirm WZ5005 talks → set family `riden`
   → reboot → confirm Riden talks. NVS preserves across power cycles.
5. **Wrong-family graceful failure** — set `psu_family=wz5005`
   while wired to a Riden. Boot logs "PSU offline" within 1 s
   (5 fails × ~200 ms). Dashboard shows offline indicator. No
   crash, no UART driver lock-up, family switch back to `riden`
   recovers without any other intervention.
6. **CLI escape hatch** — over USB-UART console:
   `psu_family wz5005` → log says "reboot to apply" + NVS persists.
   `psu_family` (no arg) prints current.
7. **Build-only XY-SK120 stub** *(if hardware unavailable at
   merge time)* — confirm compile + the model id detect path
   exits cleanly when the supply is silent (link_ok stays false,
   no crash).
8. **WZ5005 checksum vector** — synthetic test (one-shot dev-only
   gtest-style call inside `app_main` behind a `#if 0`): build the
   "set voltage to 5V" frame from the kordian-kowalski reference,
   confirm bytes 0..18 + checksum match the reference exactly. This
   is a manual sanity check before first hardware contact, not a
   shipped self-test (per CLAUDE.md "no boot-time CRC self-check").

## Risks and open questions

- **WZ5005 0x2B/0x2C exact byte layout.** The manual's table and the
  community reference both leave gaps (the reference marks 0x2B/0x2C
  as TODO). Plan: write `psu_wz5005.c` against best-guess layout
  (V_SET hi/lo at bytes 3..4, I_SET hi/lo at 5..6, V_OUT/I_OUT at
  9..12, OVP/OCP at 7..8 — to be confirmed against scope captures
  on first bench contact). If the layout is wrong, only setpoint /
  telemetry values are wrong; no crash, link_ok stays true, value
  drift is visible immediately.
- **WZ5005 remote-mode requirement.** Some custom-protocol PSUs
  reject writes unless `0x20 1` (enter remote mode) is sent first.
  Plan: `psu_wz5005_init()` sends `0x20 1` on init, ignores any
  ACK/NAK (best-effort). If `set_voltage` returns a `0xC0` (invalid
  command) or `0xB0` (cannot execute) consistently, we know remote
  mode wasn't entered and need to retry.
- **XY-SK120 baud default = 115200 conflicts with Riden's 19200.**
  If a user runs Riden first, sets XY-SK120 in NVS, reboots, but
  forgets to reflash with the new Kconfig default — the UART
  baud stays at the compiled-in 19200 and XY-SK120 won't link. Plan:
  on `link_ok=false` for >10 s after a fresh boot, log a one-shot
  warning recommending the user verify `APP_PSU_UART_BAUD` matches
  the supply's panel-set baud.
- **Naming.** `psu_driver` is generic; if there's ever a need to
  distinguish multiple PSUs running simultaneously (multi-channel
  bench), the singleton pattern would have to grow. Out of scope
  for v1 — UART1 hardware constraint makes simultaneous multi-PSU
  impossible without extra UART pins anyway.
- **CLAUDE.md drift.** The "PSU Modbus-RTU master" section needs to
  become "PSU driver (multi-family)" — easy to forget. Listed
  explicitly in the rename table above.

## Out-of-scope follow-ups (track separately, not in this work)

- OVP / OCP setpoints in dashboard — all three families expose them,
  but the wire contract gets bigger and the host PC tool needs an
  update. Defer until there's a concrete use case.
- Power / energy / capacity telemetry — all three families have it,
  no UI for it yet, no host-tool consumer.
- XY-SK80 / XY-SK60 / RD6024 — additional models within already-
  supported families. Cheap once the family backend exists; just
  register-table entries.
- Hot-swap family without reboot — not worth the UART
  reconfigure complexity; users change PSUs rarely.

## Appendix — Verified WZ5005 frame samples

From kordian-kowalski/wz5005-control/protocol.md, hand-checked under
sum-mod-256:

```text
"Enter remote mode" (op 0x20, arg=1, addr=1):
  AA 01 20 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 CC
  sum(AA..00) = 170+1+32+1 = 204 = 0xCC ✓

"Set voltage to 5V" (op 0x2C, addr=1):
  AA 01 2C 13 88 12 AB 01 F4 00 04 00 00 00 42 00 00 00 00 6A
  sum = 170+1+44+19+136+18+171+1+244+4+66 = 874 → 874 mod 256 = 106 = 0x6A ✓
  bytes 3..4 = 0x1388 = 5000 → V_SET = 5.000 V (÷1000, 3 decimals)
```

These two vectors anchor the checksum implementation. If a
future-implementer's `wz5005_build_frame` produces different bytes
for these inputs, the implementation is wrong.
