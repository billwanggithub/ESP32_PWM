# PWM frequency range extension: 1 Hz – 1 MHz

**Status:** Design approved 2026-04-22
**Scope:** `components/pwm_gen` and its callers

## Goal

Extend the supported PWM output frequency range from the current **153 Hz – 1 MHz** down to a new floor of **1 Hz**, while keeping the existing 1 MHz upper end. The generator hardware (ESP32-S3 MCPWM) has only a 16-bit counter and cannot natively span the full 6 decades with a single fixed resolution.

## Problem

Current design (`components/pwm_gen/pwm_gen.c`):

- Fixed `PWM_RESOLUTION_HZ = 10 MHz` (the MCPWM timer prescaler output rate).
- MCPWM counter is 16-bit (`MCPWM_LL_MAX_COUNT_VALUE = 65536`). Silicon limit on ESP32-S3 — no 32-bit mode exists.
- Floor = 10e6 / 65535 ≈ 153 Hz.
- LEDC on ESP32-S3 has only 14-bit duty counter, so it is not a better single-peripheral choice either.

To reach 1 Hz with a 16-bit counter we need a smaller `resolution_hz`, but lowering it globally destroys duty resolution at high frequencies (at 1 MHz a 65 kHz resolution gives `period = 0.065` ticks → impossible).

## Approach

**Dynamic resolution with an explicit 3-band table.** At each `pwm_gen_set()` call, pick the band whose `resolution_hz` gives the best duty resolution while keeping `period_ticks ≤ 65535`. Within a band, updates stay glitch-free via the existing TEZ latch (`update_cmp_on_tez` on the comparator). Crossing a band boundary triggers a teardown-reconfigure-restart of the MCPWM timer — a brief (~tens of µs) output discontinuity is accepted.

### Why explicit bands (not "always max resolution")

An "always maximize resolution per call" policy would cause every frequency change to cross an implicit band, so every freq change would glitch. Explicit bands preserve the glitch-free invariant for the common case (sweeps within a band) and make the behavior predictable and testable.

### Band table

| Band | `resolution_hz` | Freq range       | `period_ticks` range | Duty bits (range) |
|------|-----------------|------------------|----------------------|-------------------|
| HI   | 10 MHz          | 153 Hz – 1 MHz   | 10 – 65359           | 3.3 – 16          |
| MID  | 160 kHz         | 3 Hz – 152 Hz    | 1052 – 53333         | 10 – 16           |
| LO   | 1 kHz           | 1 Hz – 2 Hz      | 500 – 1000           | 9 – 10            |

Boundary rule: a frequency belongs to the **highest-resolution** band that still satisfies `period_ticks ≤ 65535`. Computed with integer arithmetic. Minimum duty resolution across the whole 1 Hz – 1 MHz range is ~3 bits (at 1 MHz, same as today).

## Architecture

### Changes to `components/pwm_gen/pwm_gen.c`

**Static state** — add `resolution_hz` to the struct so the current band is tracked:

```c
static struct {
    bool                 initialised;
    gpio_num_t           pwm_gpio;
    gpio_num_t           trigger_gpio;
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t  oper;
    mcpwm_cmpr_handle_t  cmpr;
    mcpwm_gen_handle_t   gen;
    uint32_t             freq_hz;
    float                duty_pct;
    uint32_t             period_ticks;
    uint32_t             resolution_hz;   // NEW
} s_pwm;
```

Remove the `PWM_RESOLUTION_HZ` macro. Keep `PWM_FREQ_MIN_HZ = 1` and `PWM_FREQ_MAX_HZ = 1_000_000`.

**Band table and picker:**

```c
typedef struct {
    uint32_t resolution_hz;
    uint32_t freq_min;   // inclusive lower bound
} pwm_band_t;

// Ordered by descending resolution. First matching (freq >= freq_min) wins.
static const pwm_band_t s_bands[] = {
    { 10000000u, 153u },   // HI
    {   160000u,   3u },   // MID
    {     1000u,   1u },   // LO
};

static const pwm_band_t *pick_band(uint32_t freq_hz);
```

**`pwm_gen_set(freq_hz, duty_pct)` rewrite:**

1. Validate `freq_hz ∈ [1, 1_000_000]` and `duty_pct ∈ [0, 100]`.
2. `band = pick_band(freq_hz)` — returns non-NULL by construction once validation passed.
3. `new_period = band->resolution_hz / freq_hz` (integer; guaranteed ≤ 65535 by band design).
4. `new_compare = lroundf(duty/100 * new_period)`, clamped to `[0, new_period]`.
5. If `band->resolution_hz == s_pwm.resolution_hz` → **glitch-free path** (current code):
   - `mcpwm_timer_set_period(timer, new_period)`
   - `mcpwm_comparator_set_compare_value(cmpr, new_compare)`
6. Else → **band-crossing path** (new function `reconfigure_for_band()`): teardown + recreate timer with new `resolution_hz`, reconnect oper, re-arm comparator, enable and start.
7. Update `s_pwm` state.
8. Fire trigger-GPIO pulse (updated logic, see below).

**`reconfigure_for_band()`:**

MCPWM v5.5 driver does not expose a way to change `resolution_hz` on a live timer. The supported sequence:

```
mcpwm_timer_start_stop(timer, MCPWM_TIMER_STOP_EMPTY)   // stop at next TEZ
mcpwm_timer_disable(timer)
mcpwm_del_timer(timer)
mcpwm_new_timer(&cfg_with_new_resolution_and_period, &timer)
mcpwm_operator_connect_timer(oper, timer)
mcpwm_comparator_set_compare_value(cmpr, new_compare)
mcpwm_timer_enable(timer)
mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP)
```

Operator, comparator, and generator objects are **not** recreated — they stay connected and retain their generator action config (high on TEZ, low on compare). Only the timer handle is replaced. This is the minimum reconfig needed when `resolution_hz` must change.

During the stop→enable window (~tens of µs), the PWM output stays at whatever level the generator last drove.

**Trigger-GPIO pulse:**

Current formula `pulse_us = 2 + 1_000_000 / freq` with cap at 1000 µs produces a 1-second pulse at 1 Hz (via the current clamp). New rule:

```c
int64_t pulse_us = 2 + 1000000 / (int64_t)freq_hz;
if (pulse_us > 1000) pulse_us = 1000;
if (pulse_us <  200) pulse_us =  200;
```

Gives 1000 µs at 1 Hz, 200 µs at 1 MHz. Both cleanly observable on a scope.

**`pwm_gen_duty_resolution_bits(freq_hz)`:**

Signature unchanged. Implementation updated to call `pick_band()` then compute `bits = floor(log2(resolution_hz / freq_hz))`. Returns 0 for out-of-range inputs.

### Changes to `components/pwm_gen/include/pwm_gen.h`

Update the doc comment on `pwm_gen_set()` to note the new freq range (1 Hz – 1 MHz) and the band-crossing glitch behavior.

### Changes to `components/net_dashboard/web/index.html`

Current: `<input id="freq" type="number" min="1" max="1000000" value="1000" />` — `min="1"` is already correct. No change needed (but verify during testing).

### Changes to `main/app_main.c`

Initial-state call currently starts at 1000 Hz. Unchanged.

### Changes to `CLAUDE.md`

Update the "PWM glitch-free update mechanism" section:

- New freq range: 1 Hz – 1 MHz.
- Replace the fixed-resolution explanation with the 3-band table.
- Document the band-crossing glitch exception to the glitch-free invariant.
- Update the trigger-pulse rule.

### Unchanged

- `components/app_api/include/app_api.h` — `set_pwm.freq_hz` is already `uint32_t`.
- `components/usb_composite/include/usb_protocol.h` — `usb_hid_set_pwm_t.freq_hz` already `uint32_t`.
- WebSocket JSON contract (`{type: "set_pwm", freq, duty}`) unchanged.
- Control task, RPM capture, HID/CDC frontends — all pass `freq_hz` through opaquely.

## Error handling

- `freq_hz == 0` or `freq_hz > 1_000_000` → `ESP_ERR_INVALID_ARG`.
- `duty_pct < 0` or `duty_pct > 100` → `ESP_ERR_INVALID_ARG`.
- MCPWM driver error during band-crossing: log at `ESP_LOGE`, return the driver error code. No rollback — a partial reconfigure has no safe rollback target, and the next successful call will re-establish consistent state. This matches the current code's philosophy.

## Testing

Manual scope-based verification (no firmware unit-test infra):

1. **Sweep** — set 1, 2, 3, 10, 100, 152, 153, 1000, 10000, 100000, 1000000 Hz at duty=50%. Each should produce the correct waveform.
2. **Band-crossing glitch** — sweep 200 → 100 Hz (HI→MID) and 5 → 2 Hz (MID→LO). Observe short output discontinuity on crossing, clean waveform either side.
3. **Glitch-free in-band** — sweep 500 → 600 Hz repeatedly. Expect zero output discontinuity.
4. **Duty resolution UI** — verify displayed bit count: 1 Hz → 9 bits, 100 Hz → 10 bits, 1 kHz → 13 bits, 1 MHz → 3 bits.
5. **Trigger GPIO** — scope the trigger pin during `set_pwm` calls. Pulse width should be between 200 µs and 1000 µs regardless of freq.
6. **Boundary values** — exactly freq=2 and freq=3 (straddles MID↔LO), exactly freq=152 and freq=153 (straddles HI↔MID). Confirm correct band assignment and no duty calculation errors.

## Out of scope

- Sub-Hz frequencies (would require LEDC or a software-timer generator).
- Frequencies above 1 MHz (would require a faster resolution_hz band, but MCPWM's practical max resolution is 160 MHz on S3; more importantly the existing 1 MHz cap is a product requirement).
- Making the band-crossing itself glitch-free (would require custom driver work or a second MCPWM unit for dual-buffered transitions).
- Removing the `PWM_RESOLUTION_HZ` macro usage from comments in other files beyond CLAUDE.md (grep shows none).
