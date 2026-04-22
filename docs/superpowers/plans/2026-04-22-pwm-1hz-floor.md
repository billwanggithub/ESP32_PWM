# PWM 1 Hz Floor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend PWM output frequency range from 153 Hz–1 MHz down to 1 Hz–1 MHz on ESP32-S3 MCPWM.

**Architecture:** Replace the fixed 10 MHz resolution with a 3-band dynamic-resolution table. `pwm_gen_set()` picks the highest-resolution band whose period fits the 16-bit counter. Within a band, updates remain glitch-free via TEZ latch. Band crossings require tearing down and recreating the MCPWM timer (operator/comparator/generator are retained), producing a brief (~tens of µs) output discontinuity only on crossings.

**Tech Stack:** ESP-IDF v5.5.1, ESP32-S3 MCPWM driver (`driver/mcpwm_prelude.h`), FreeRTOS.

**Spec:** `docs/superpowers/specs/2026-04-22-pwm-1hz-floor-design.md`

---

## File Structure

**Modified:**
- `components/pwm_gen/pwm_gen.c` — remove `PWM_RESOLUTION_HZ`, add band table, band picker, band-crossing reconfigure function; rewrite `pwm_gen_set()` and `pwm_gen_duty_resolution_bits()`.
- `components/pwm_gen/include/pwm_gen.h` — doc comment updates.
- `CLAUDE.md` — update the PWM glitch-free update section with the new freq range, band table, and band-crossing exception.

**Verify (no change expected):**
- `components/net_dashboard/web/index.html` — `min="1"` already correct.
- `components/app_api/include/app_api.h`, `components/usb_composite/include/usb_protocol.h`, `components/net_dashboard/ws_handler.c` — wire types are already `uint32_t`, no changes needed.

**No tests added.** This firmware repo has no host-side unit-test harness for the `pwm_gen` component; validation is manual scope-based. Testing steps are included at the end of the plan instead of per task.

---

### Task 1: Add band table, band picker, and tracked `resolution_hz` state

**Files:**
- Modify: `components/pwm_gen/pwm_gen.c` (top of file: macros and static state)

- [ ] **Step 1: Replace the macro block and static struct**

Open `components/pwm_gen/pwm_gen.c`. Replace lines 13–40 (the comment block, macros, and the `s_pwm` struct) with:

```c
// MCPWM 的 timer counter 是 16-bit（MCPWM_LL_MAX_COUNT_VALUE = 0x10000），所以
// period_ticks 必須 ∈ [2, 65535]。一個固定 resolution_hz 覆蓋不了 1 Hz ~ 1 MHz
// 六個 decade，於是用 3-band 動態 resolution：每次 pwm_gen_set() 依 freq
// 挑「能在 16-bit counter 內塞下 period」的最高 resolution。同 band 內走
// TEZ latch glitch-free 更新，跨 band 需要 teardown→recreate timer（oper /
// cmpr / gen 物件保留並重新 connect）、會有 ~tens of µs 的 output 斷點。
//
//   Band  resolution_hz  freq range       period_ticks    duty bits
//   HI    10 MHz         153 Hz – 1 MHz   10 – 65359      3.3 – 16
//   MID   160 kHz        3 Hz – 152 Hz    1052 – 53333    10 – 16
//   LO    1 kHz          1 Hz – 2 Hz      500 – 1000      9 – 10
#define PWM_FREQ_MIN_HZ 1u
#define PWM_FREQ_MAX_HZ 1000000u

typedef struct {
    uint32_t resolution_hz;
    uint32_t freq_min;   // inclusive lower bound; freq < freq_min falls to next band
} pwm_band_t;

// Ordered by descending resolution. First entry with freq >= freq_min wins.
static const pwm_band_t s_bands[] = {
    { 10000000u, 153u },   // HI
    {   160000u,   3u },   // MID
    {     1000u,   1u },   // LO
};

static const char *TAG = "pwm_gen";

static struct {
    bool                      initialised;
    gpio_num_t                pwm_gpio;
    gpio_num_t                trigger_gpio;
    mcpwm_timer_handle_t      timer;
    mcpwm_oper_handle_t       oper;
    mcpwm_cmpr_handle_t       cmpr;
    mcpwm_gen_handle_t        gen;
    uint32_t                  freq_hz;
    float                     duty_pct;
    uint32_t                  period_ticks;
    uint32_t                  resolution_hz;   // current band's resolution
} s_pwm;

static const pwm_band_t *pick_band(uint32_t freq_hz)
{
    for (size_t i = 0; i < sizeof(s_bands) / sizeof(s_bands[0]); ++i) {
        if (freq_hz >= s_bands[i].freq_min) return &s_bands[i];
    }
    return NULL;
}
```

Note: the previous `static const char *TAG = "pwm_gen";` declaration on line 27 is kept in the same relative position (inside the new block). If you prefer to leave `TAG` where it was, that also works — just don't declare it twice.

- [ ] **Step 2: Replace `freq_to_period_ticks` helper**

Replace the old `freq_to_period_ticks()` function (lines 42–46) with a band-aware version:

```c
static inline uint32_t freq_to_period_ticks(uint32_t resolution_hz, uint32_t freq_hz)
{
    if (freq_hz == 0) return 0;
    return resolution_hz / freq_hz;
}
```

- [ ] **Step 3: Compile check**

Run: `idf.py build`

Expected: build fails because `pwm_gen_duty_resolution_bits()`, `pwm_gen_init()`, and `pwm_gen_set()` still reference the old `PWM_RESOLUTION_HZ` macro and the old `freq_to_period_ticks()` signature. That is OK — we fix those in Tasks 2–4.

- [ ] **Step 4: Commit**

```bash
git add components/pwm_gen/pwm_gen.c
git commit -m "refactor(pwm_gen): introduce 3-band resolution table

Add pwm_band_t table and pick_band() helper. Track resolution_hz in
static state so subsequent commits can change resolution per-call.
Does not compile yet — callers updated in follow-ups."
```

---

### Task 2: Update `pwm_gen_duty_resolution_bits()` to use the band picker

**Files:**
- Modify: `components/pwm_gen/pwm_gen.c`

- [ ] **Step 1: Rewrite the function**

Replace the current `pwm_gen_duty_resolution_bits()` (originally lines 48–55) with:

```c
uint8_t pwm_gen_duty_resolution_bits(uint32_t freq_hz)
{
    const pwm_band_t *band = pick_band(freq_hz);
    if (!band) return 0;
    uint32_t period = freq_to_period_ticks(band->resolution_hz, freq_hz);
    if (period < 2) return 0;
    uint8_t bits = 0;
    while ((1u << bits) <= period) bits++;
    return bits ? (uint8_t)(bits - 1) : 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add components/pwm_gen/pwm_gen.c
git commit -m "refactor(pwm_gen): duty-resolution calc follows band picker"
```

---

### Task 3: Update `pwm_gen_init()` for the new 1 kHz starting state

**Files:**
- Modify: `components/pwm_gen/pwm_gen.c`

- [ ] **Step 1: Rewrite the init body**

Replace the current `pwm_gen_init()` (originally lines 57–119) with:

```c
esp_err_t pwm_gen_init(const pwm_gen_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_pwm.initialised) return ESP_ERR_INVALID_STATE;

    memset(&s_pwm, 0, sizeof(s_pwm));
    s_pwm.pwm_gpio     = cfg->pwm_gpio;
    s_pwm.trigger_gpio = cfg->trigger_gpio;

    // Start at a safe known state: 1 kHz, 0% duty. Falls into the HI band.
    const uint32_t init_freq = 1000;
    const pwm_band_t *band = pick_band(init_freq);
    s_pwm.resolution_hz = band->resolution_hz;
    s_pwm.period_ticks  = freq_to_period_ticks(band->resolution_hz, init_freq);
    s_pwm.freq_hz       = init_freq;
    s_pwm.duty_pct      = 0.0f;

    mcpwm_timer_config_t timer_cfg = {
        .group_id      = 0,
        .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = s_pwm.resolution_hz,
        .count_mode    = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks  = s_pwm.period_ticks,
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_cfg, &s_pwm.timer));

    mcpwm_operator_config_t oper_cfg = { .group_id = 0 };
    ESP_ERROR_CHECK(mcpwm_new_operator(&oper_cfg, &s_pwm.oper));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(s_pwm.oper, s_pwm.timer));

    mcpwm_comparator_config_t cmpr_cfg = { .flags.update_cmp_on_tez = true };
    ESP_ERROR_CHECK(mcpwm_new_comparator(s_pwm.oper, &cmpr_cfg, &s_pwm.cmpr));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_pwm.cmpr, 0));

    mcpwm_generator_config_t gen_cfg = { .gen_gpio_num = s_pwm.pwm_gpio };
    ESP_ERROR_CHECK(mcpwm_new_generator(s_pwm.oper, &gen_cfg, &s_pwm.gen));

    // High on timer empty, low when compare hits → standard PWM.
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(
        s_pwm.gen, MCPWM_GEN_TIMER_EVENT_ACTION(
            MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(
        s_pwm.gen, MCPWM_GEN_COMPARE_EVENT_ACTION(
            MCPWM_TIMER_DIRECTION_UP, s_pwm.cmpr, MCPWM_GEN_ACTION_LOW)));

    // Trigger GPIO: plain push-pull, idle low.
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << s_pwm.trigger_gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    gpio_set_level(s_pwm.trigger_gpio, 0);

    ESP_ERROR_CHECK(mcpwm_timer_enable(s_pwm.timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_pwm.timer, MCPWM_TIMER_START_NO_STOP));

    s_pwm.initialised = true;
    ESP_LOGI(TAG, "init ok: pwm_gpio=%d trigger_gpio=%d freq=%lu duty=%.1f%% res=%lu",
             s_pwm.pwm_gpio, s_pwm.trigger_gpio,
             (unsigned long)s_pwm.freq_hz, s_pwm.duty_pct,
             (unsigned long)s_pwm.resolution_hz);
    return ESP_OK;
}
```

- [ ] **Step 2: Commit**

```bash
git add components/pwm_gen/pwm_gen.c
git commit -m "refactor(pwm_gen): init picks starting band for 1 kHz"
```

---

### Task 4: Add the band-crossing reconfigure helper and rewrite `pwm_gen_set()`

**Files:**
- Modify: `components/pwm_gen/pwm_gen.c`

- [ ] **Step 1: Add `reconfigure_for_band()` above `pwm_gen_set()`**

Insert this new function immediately before `pwm_gen_set()`:

```c
// Tear down the current timer and bring up a new one with a different
// resolution_hz / period_ticks combination. Operator / comparator / generator
// objects are retained and reconnected — their action config (high on TEZ,
// low on compare) is preserved. Call site already validated band and period.
static esp_err_t reconfigure_for_band(const pwm_band_t *band,
                                      uint32_t new_period,
                                      uint32_t new_compare)
{
    esp_err_t err;

    err = mcpwm_timer_start_stop(s_pwm.timer, MCPWM_TIMER_STOP_EMPTY);
    if (err != ESP_OK) { ESP_LOGE(TAG, "timer stop: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_timer_disable(s_pwm.timer);
    if (err != ESP_OK) { ESP_LOGE(TAG, "timer disable: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_del_timer(s_pwm.timer);
    if (err != ESP_OK) { ESP_LOGE(TAG, "timer del: %s", esp_err_to_name(err)); return err; }
    s_pwm.timer = NULL;

    mcpwm_timer_config_t timer_cfg = {
        .group_id      = 0,
        .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = band->resolution_hz,
        .count_mode    = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks  = new_period,
    };
    err = mcpwm_new_timer(&timer_cfg, &s_pwm.timer);
    if (err != ESP_OK) { ESP_LOGE(TAG, "timer new: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_operator_connect_timer(s_pwm.oper, s_pwm.timer);
    if (err != ESP_OK) { ESP_LOGE(TAG, "oper connect: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_comparator_set_compare_value(s_pwm.cmpr, new_compare);
    if (err != ESP_OK) { ESP_LOGE(TAG, "cmpr set: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_timer_enable(s_pwm.timer);
    if (err != ESP_OK) { ESP_LOGE(TAG, "timer enable: %s", esp_err_to_name(err)); return err; }

    err = mcpwm_timer_start_stop(s_pwm.timer, MCPWM_TIMER_START_NO_STOP);
    if (err != ESP_OK) { ESP_LOGE(TAG, "timer start: %s", esp_err_to_name(err)); return err; }

    return ESP_OK;
}
```

- [ ] **Step 2: Rewrite `pwm_gen_set()`**

Replace the entire existing `pwm_gen_set()` (originally lines 121–152) with:

```c
esp_err_t pwm_gen_set(uint32_t freq_hz, float duty_pct)
{
    if (!s_pwm.initialised) return ESP_ERR_INVALID_STATE;
    if (freq_hz < PWM_FREQ_MIN_HZ || freq_hz > PWM_FREQ_MAX_HZ) return ESP_ERR_INVALID_ARG;
    if (duty_pct < 0.0f || duty_pct > 100.0f) return ESP_ERR_INVALID_ARG;

    const pwm_band_t *band = pick_band(freq_hz);
    if (!band) return ESP_ERR_INVALID_ARG;   // defensive; validation above should cover it

    uint32_t period = freq_to_period_ticks(band->resolution_hz, freq_hz);
    if (period < 2 || period > 65535) return ESP_ERR_INVALID_ARG;

    uint32_t compare = (uint32_t)lroundf((duty_pct / 100.0f) * (float)period);
    if (compare > period) compare = period;

    if (band->resolution_hz == s_pwm.resolution_hz) {
        // Same band → glitch-free TEZ-latched update.
        esp_err_t err = mcpwm_timer_set_period(s_pwm.timer, period);
        if (err != ESP_OK) return err;
        err = mcpwm_comparator_set_compare_value(s_pwm.cmpr, compare);
        if (err != ESP_OK) return err;
    } else {
        // Band crossing → teardown-reconfigure-restart (brief output glitch).
        esp_err_t err = reconfigure_for_band(band, period, compare);
        if (err != ESP_OK) return err;
    }

    s_pwm.period_ticks  = period;
    s_pwm.freq_hz       = freq_hz;
    s_pwm.duty_pct      = duty_pct;
    s_pwm.resolution_hz = band->resolution_hz;

    // Trigger pulse: a software "settings changed" edge for scope latching.
    // 1 Hz gives 1000 µs; 1 MHz gives 200 µs — both cleanly observable.
    int64_t pulse_us = 2 + 1000000 / (int64_t)freq_hz;
    if (pulse_us > 1000) pulse_us = 1000;
    if (pulse_us <  200) pulse_us =  200;
    gpio_set_level(s_pwm.trigger_gpio, 1);
    esp_rom_delay_us((uint32_t)pulse_us);
    gpio_set_level(s_pwm.trigger_gpio, 0);

    return ESP_OK;
}
```

- [ ] **Step 3: Build**

Run: `idf.py build`
Expected: build succeeds with no errors or warnings from `pwm_gen.c`.

- [ ] **Step 4: Commit**

```bash
git add components/pwm_gen/pwm_gen.c
git commit -m "feat(pwm_gen): extend freq range to 1 Hz via dynamic resolution

pwm_gen_set() now picks a band (HI/MID/LO) from the resolution table.
Same-band updates stay glitch-free via TEZ latch; band-crossings tear
down and recreate the MCPWM timer (operator/comparator/generator are
retained). Trigger-pulse duration clamped to [200us, 1000us]."
```

---

### Task 5: Update the public header comment

**Files:**
- Modify: `components/pwm_gen/include/pwm_gen.h`

- [ ] **Step 1: Update doc comments**

Replace the existing comment block on `pwm_gen_set()` (originally line 18–19) with:

```c
// Glitch-free update within a resolution band. Latches at the next period
// boundary (TEZ). Valid freq range is 1 Hz .. 1_000_000 Hz. Crossing a band
// boundary (freq 2↔3 Hz or 152↔153 Hz) causes a brief (~tens of µs) output
// discontinuity while the MCPWM timer is reconfigured.
// Returns ESP_ERR_INVALID_ARG if freq_hz is out of range or duty_pct is out
// of [0, 100].
```

Also update the `pwm_gen_duty_resolution_bits()` comment (originally line 24) to:

```c
// Effective duty resolution (bits) at the given frequency, accounting for
// the 3-band dynamic resolution table (HI=10 MHz, MID=160 kHz, LO=1 kHz).
```

- [ ] **Step 2: Build**

Run: `idf.py build`
Expected: success.

- [ ] **Step 3: Commit**

```bash
git add components/pwm_gen/include/pwm_gen.h
git commit -m "docs(pwm_gen): document new 1 Hz freq floor and band crossings"
```

---

### Task 6: Update `CLAUDE.md`

**Files:**
- Modify: `CLAUDE.md` (the "PWM glitch-free update mechanism" section)

- [ ] **Step 1: Replace the PWM section**

Locate the `## PWM glitch-free update mechanism` section in `CLAUDE.md` and replace its body (from the heading down to the end of the section, before `## Security posture`) with:

```markdown
## PWM glitch-free update mechanism

`pwm_gen_set()` writes new period and compare values; both latch on
`MCPWM_TIMER_EVENT_EMPTY` (TEZ — timer equals zero) via the
`update_cmp_on_tez` flag on the comparator. **Do not** call
`mcpwm_timer_stop` / restart in the update path for same-band changes —
that's where glitches come from.

Frequency range is **1 Hz ~ 1 MHz**. The 16-bit MCPWM counter
(`MCPWM_LL_MAX_COUNT_VALUE = 0x10000`) cannot span 6 decades with a
single fixed `resolution_hz`, so `pwm_gen.c` defines a 3-band table and
picks a band per call:

| Band | resolution_hz | freq range     | period_ticks   | duty bits |
|------|---------------|----------------|----------------|-----------|
| HI   | 10 MHz        | 153 Hz – 1 MHz | 10 – 65359     | 3.3 – 16  |
| MID  | 160 kHz       | 3 Hz – 152 Hz  | 1052 – 53333   | 10 – 16   |
| LO   | 1 kHz         | 1 Hz – 2 Hz    | 500 – 1000     | 9 – 10    |

**Same-band updates** (e.g. 500 Hz → 600 Hz) stay glitch-free via TEZ.
**Band-crossing updates** (e.g. 200 Hz → 100 Hz) go through
`reconfigure_for_band()`: stop → disable → delete timer → `mcpwm_new_timer`
with the new `resolution_hz` → reconnect operator → re-arm comparator →
enable → start. This produces a brief (~tens of µs) output discontinuity.
Operator, comparator, and generator objects are retained across the
reconfigure so their action config (high on TEZ, low on compare) does
not need to be re-registered.

The actual bit count at any freq is exposed via
`pwm_gen_duty_resolution_bits()` for the UI. If future requirements need
sub-Hz, the MCPWM counter can't stretch further — an LEDC-based second
generator or software timer would be needed.

The "change-trigger output" on a separate GPIO is a software pulse from
`control_task` after the write succeeds — not a hardware sync output.
Pulse width is clamped to [200 µs, 1000 µs] so it's always cleanly
observable regardless of PWM freq. If jitter on that trigger matters,
wire it to an MCPWM ETM event instead.
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(CLAUDE): update PWM section for 1 Hz–1 MHz range"
```

---

### Task 7: Full build and on-device validation

**Files:** (no file edits expected)

- [ ] **Step 1: Clean build**

Run: `idf.py build`
Expected: success, zero errors / zero new warnings in `components/pwm_gen`.

- [ ] **Step 2: Flash to hardware**

Run: `idf.py -p COM3 flash monitor` (substitute your COM port)
Expected: boot logs show `pwm_gen: init ok: … freq=1000 duty=0.0% res=10000000`.

- [ ] **Step 3: Sweep test via CLI**

At the `esp32-pwm>` prompt, run each of:

```
pwm 1 50
pwm 2 50
pwm 3 50
pwm 10 50
pwm 100 50
pwm 152 50
pwm 153 50
pwm 1000 50
pwm 10000 50
pwm 100000 50
pwm 1000000 50
```

For each, check on a scope:
- Square wave present on `CONFIG_APP_PWM_OUTPUT_GPIO` at the commanded frequency.
- Duty cycle ≈ 50% (exact ratio limited by duty bits at that freq).
- Trigger pulse visible on `CONFIG_APP_PWM_TRIGGER_GPIO`, width in 200–1000 µs range.

Run `status` after each to verify reported `freq` and duty-resolution bits match:
- 1 Hz → 9 bits
- 100 Hz → 10 bits
- 1 kHz → 13 bits
- 100 kHz → 6 bits
- 1 MHz → 3 bits

- [ ] **Step 4: Band-crossing observation**

At the scope, set rolling / long-record. Run:

```
pwm 200 50
pwm 100 50
```

Expected: one brief (~tens of µs) output discontinuity on the transition
(output held at last level, then resumes at 100 Hz). Signal is clean both
before and after.

Run:

```
pwm 5 50
pwm 2 50
```

Expected: similar brief discontinuity.

- [ ] **Step 5: Glitch-free in-band test**

Set scope to catch glitches on the PWM output. Run repeatedly:

```
pwm 500 50
pwm 600 50
pwm 500 50
pwm 600 50
```

Expected: no output discontinuity between consecutive commands (both are
in the HI band). Only the period of the square wave changes.

- [ ] **Step 6: Out-of-range validation**

Run:

```
pwm 0 50
pwm 1000001 50
```

Each should print an error (control_task_post returns non-zero) and not
change the output. Previous waveform continues.

- [ ] **Step 7: Dashboard UI sanity**

If Wi-Fi is configured: open the dashboard URL in a browser, set freq=1
with duty=50% via the form, confirm the "applied" text reads
`1 Hz, 50.00 %` and the PWM output matches.

- [ ] **Step 8: Tag the merge**

If all validation passes:

```bash
git log --oneline  # confirm the 6 commits from tasks 1–6 are present
```

No additional commit needed at this task — it's pure validation.

---

## Spec Coverage Check

Items from `docs/superpowers/specs/2026-04-22-pwm-1hz-floor-design.md` and their covering tasks:

- 3-band table with exact `resolution_hz` / `freq_min` values → Task 1
- `pick_band()` picks highest-resolution band satisfying period ≤ 65535 → Task 1
- `s_pwm.resolution_hz` state tracking → Task 1
- `pwm_gen_duty_resolution_bits()` updated for bands → Task 2
- `pwm_gen_init()` uses the band picker for starting state → Task 3
- Same-band glitch-free TEZ path preserved → Task 4 (`pwm_gen_set` branch 1)
- Band-crossing teardown/reconfigure sequence → Task 4 (`reconfigure_for_band`)
- Trigger-pulse clamp to [200 µs, 1000 µs] → Task 4
- Header comment update → Task 5
- CLAUDE.md PWM section update → Task 6
- Manual scope validation including boundary values 2/3 and 152/153 → Task 7 Step 3
- Band-crossing discontinuity observation → Task 7 Step 4
- In-band glitch-free verification → Task 7 Step 5
- Out-of-range validation → Task 7 Step 6
- UI sanity at freq=1 → Task 7 Step 7

No gaps.
