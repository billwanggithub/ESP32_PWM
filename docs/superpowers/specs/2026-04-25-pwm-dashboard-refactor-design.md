# PWM Dashboard Refactor — Design

**Date:** 2026-04-25
**Status:** Draft for review (revised after RPM panel mockup feedback)
**Scope:** `components/net_dashboard/web/` only. No firmware logic changes,
no WebSocket contract changes.

**Revision history:**

- v1: PWM panels only, RPM/OTA/factory unchanged structurally.
- v2: RPM panel rebuilt — moved to top of page, `<h2>` title removed,
  config + chart now in collapsed `<details>`, chart gains fixed Y-axis
  (RPM scale) and time-based X-axis, plus a "Live" checkbox that
  freezes the plot only (readout keeps updating).
- v3: RPM 設定 moved inline into the header row as a popover-style
  `<details>` anchored next to the Live checkbox, instead of a full-
  width row below the readout. Saves vertical space; settings only
  appear when the gear is clicked.

## Problem

The current `index.html` PWM panel exposes Duty and Frequency as two raw
number inputs and a single Apply button. Three friction points:

1. **No quick recall.** Setting common values (0/25/50/75/100 % duty;
   100 Hz, 1 kHz, 25 kHz frequency) requires retyping each time.
2. **No fine adjustment affordance.** Stepping by 0.1 %, 1 %, 10 % via the
   number-input arrows requires changing the input's `step` attribute or
   typing — neither works while sweeping.
3. **No spatial sense of the value.** No slider; users can't see where
   they are within the range or sweep across it interactively.

Reference Windows app (screenshot in conversation) shows a richer pattern
with readout, slider, step-size control, fine-tune button grid, and
editable presets per axis. We're porting that pattern to the web
dashboard.

## Goals

- One panel each for Duty and Frequency, both following the same layout
  template (large readout + slider + fine-tune grid + presets).
- Discrete actions (button click, preset 套用, value commit) push to the
  device immediately. Slider drag updates the readout but only sends on
  release.
- Frequency slider spans 10 Hz – 1 MHz on a logarithmic scale with
  decade tick marks (10 / 100 / 1k / 10k / 100k / 1M).
- Surface effective duty resolution from `pwm_gen_duty_resolution_bits()`
  in the frequency panel so users know how many decimals are real.
- Presets are 6 per axis, editable in place, persisted to `localStorage`,
  with hard-coded defaults shipped in the HTML.
- RPM panel restructured: moved to top of page, `<h2>` title removed,
  big readout + Live checkbox always visible, chart and config tucked
  into default-collapsed `<details>`. Chart gains fixed Y-axis
  (RPM scale, auto-rescale up only) and time-based X-axis. Live
  checkbox freezes the rendered plot only — readout keeps updating.
- Light CSS-token introduction so the OTA and factory-reset panels
  don't look bolted-on next to the redesigned PWM and RPM panels.

## Non-Goals

- No firmware changes. `pwm_gen.c`, `ws_handler.c`, the `set_pwm` /
  `status` / `set_rpm` JSON shapes, and the 20 Hz status broadcast all
  stay as-is.
- No NVS-stored presets. Presets are per-browser via `localStorage`;
  factory reset on the device does not affect them.
- No new WebSocket message types. Existing
  `{type:"set_pwm",freq,duty}` and `{type:"set_rpm",pole,mavg,timeout_us}`
  carry every interaction.
- No redesign of the OTA upload or factory-reset panels — only their
  CSS picks up the new shared tokens.
- No PWA / offline / install prompt work.
- No reconciliation of the docs/code drift noted in the Open Questions
  section (firmware-side cleanup, separate task).

## Architecture

### File layout (no new files)

All work lives in three existing files under
`components/net_dashboard/web/`:

```text
index.html   ── markup for the new Duty + Frequency panels
app.css      ── CSS tokens + new panel styles (existing rules retained
                where they still apply to RPM/OTA/factory panels)
app.js       ── new panel controllers (Duty, Frequency); existing
                WebSocket connection + status handler retained
```

`CMakeLists.txt` already embeds these three via `EMBED_TXTFILES`. No
build wiring changes needed.

### Panel template (shared between Duty and Frequency)

Each panel has the same DOM skeleton; controllers differ in how they map
slider position to value and what fine-tune deltas they expose.

```text
┌─ panel ─────────────────────────────────────────────────┐
│  label   readout (big, green, tabular-nums)             │
│                          step: [input] unit             │
│  ┌──────── slider ─────────────────────────────────┐    │
│  └──────────────────────────────────────────────────┘   │
│   tick  tick  tick  tick                                │
│   resolution-bits hint (frequency panel only)           │
│                                                          │
│  ▾ 微調                                                  │
│   ┌──────┬──────┬──────┐                                │
│   │ +δ₁  │ +δ₂  │ +δ₃  │  ← greens                     │
│   ├──────┼──────┼──────┤                                │
│   │ −δ₁  │ −δ₂  │ −δ₃  │  ← reds                       │
│   └──────┴──────┴──────┘                                │
│                                                          │
│  ▾ 預設值                                                │
│   ┌────────┬─────┬────────┬─────┐                       │
│   │ [50.0] │ 套用│ [75.0] │ 套用│                       │
│   └────────┴─────┴────────┴─────┘   (6 slots, 3×2)      │
└─────────────────────────────────────────────────────────┘
```

Per-axis specifics:

|                       | Duty                          | Frequency                                       |
|-----------------------|-------------------------------|-------------------------------------------------|
| Range                 | 0 – 100 %                     | 10 – 1,000,000 Hz                               |
| Slider mapping        | linear, `value` = duty pct    | log10, `value/100` = log10(freq)                |
| Slider HTML range     | `min=0 max=100 step=<step>`   | `min=100 max=600 step=1` (i.e. 10^(value/100))  |
| Step input default    | 1.0                           | 100                                             |
| Fine-tune deltas      | ±0.1 / ±1 / ±10 (fixed)       | ±1 / ±10 / ±100 Hz (fixed)                      |
| Preset defaults       | 0, 10, 25, 50, 75, 100 (%)    | 25, 100, 1000, 5000, 25000, 100000 (Hz)         |
| 微調 default state    | open                          | **collapsed**                                   |
| 預設值 default state  | open                          | **collapsed**                                   |
| Resolution-bits hint  | —                             | shown, updated on freq change                   |

The "step input" sets **only** the slider's `step` attribute, which
controls slider drag-snap and the arrow-key increment when the slider
has focus. It does **not** alter the readout input's step (that stays
fixed: `0.1` for duty, `1` for freq) and does **not** alter the
fine-tune button deltas (those stay fixed too). (User confirmed:
"step size only apply to slider".)

### Readout = committable input

The big green readout is an `<input type="number">` styled to look like
a heading (large, tabular-nums, transparent background). Clicking it
focuses for typing; Enter or blur commits the value (clamped, then sent
via `onCommit`). This preserves the screenshot's visual while keeping
keyboard editing available — no separate "edit" mode toggle. Inbound
status updates write `input.value` directly so they don't visibly
disturb a focused field (browsers preserve cursor position when value
hasn't changed).

### Apply behavior

| Interaction              | When it sends `set_pwm`                  |
|--------------------------|------------------------------------------|
| Slider drag              | only on `change` event (release)         |
| Slider keyboard arrows   | on `change` (each commit)                |
| Fine-tune button click   | immediately                              |
| Preset 套用 button       | immediately                              |
| Number input commit      | on Enter or blur                         |
| Preset slot value edit   | never sends — only updates localStorage  |

The existing `Apply` button is removed. The existing freq/duty number
inputs in the panel header are kept as the readout's primary committable
value (still `<input type="number">`, just restyled and shown as the
big green readout).

The 20 Hz status broadcast from the device is the source of truth; the
`status` handler updates the readout, slider position, and any echoed
state. Local user input speculatively updates the readout immediately so
the slider feels responsive between status frames.

### Validation and clamping

Client-side clamps before sending:

- Duty: `Math.max(0, Math.min(100, parseFloat(v)))`. Reject NaN.
- Frequency: `Math.max(10, Math.min(1_000_000, Math.round(parseFloat(v))))`.
  Reject NaN.
- Step input: must be `> 0`. Default back to last valid step on bad input.

The firmware re-validates in `pwm_gen_set()` and returns an error that
`ws_handler.c` currently silently swallows — no change to that path.

### Resolution-bits hint

`pwm_gen_duty_resolution_bits()` runs on the device and is not exposed
over the WebSocket. To avoid a contract change, the JS replicates the
2-band table inline:

```js
function dutyResolutionBits(freqHz) {
  // mirrors components/pwm_gen/pwm_gen.c
  const RES_HI = 10_000_000, RES_LO = 320_000;
  const res = freqHz >= 153 ? RES_HI : RES_LO;
  const period = Math.floor(res / freqHz);
  return Math.floor(Math.log2(period));
}
```

If the firmware band table ever changes, this value drifts until the JS
is updated. Acceptable trade-off — the hint is informational, not used
to drive sends.

### CSS tokens (added to `app.css`)

```css
:root {
  color-scheme: light dark;
  --accent: #2563eb;
  --green: #16a34a;
  --red: #dc2626;
  --readout: var(--green);
  --border: #d4d4d8;
  --muted: #71717a;
  --bg: #ffffff;
  --bg-soft: #f4f4f5;
  --radius: 10px;
}
@media (prefers-color-scheme: dark) {
  :root {
    --border: #3f3f46;
    --muted: #a1a1aa;
    --bg: #09090b;
    --bg-soft: #18181b;
  }
}
```

The existing `.panel` rule (currently `padding:1em; border:1px solid #8884`)
is rewritten to use `var(--bg)`, `var(--border)`, `var(--radius)`. The
RPM, OTA, and factory-reset panels pick up the new chrome automatically
because they share the `.panel` class.

### Persistence

`localStorage` keys (single namespace prefix):

- `fan-testkit:duty-presets` — JSON array of 6 numbers
- `fan-testkit:freq-presets` — JSON array of 6 numbers
- `fan-testkit:duty-step` — string number
- `fan-testkit:freq-step` — string number

On load, the controller reads the key, validates length and types, and
falls back to baked-in defaults on any parse error or schema mismatch.
Fine-tune accordion open/closed state is **not** persisted — defaults
match the table above on every load (so frequency users always start
with the panel collapsed).

### Page ordering

Top-down sequence:

1. RPM panel (live readout always visible)
2. Duty panel
3. Frequency panel
4. OTA panel
5. Factory-reset panel

Rationale: RPM is the most-watched value during a fan test session;
putting it at the top means the user always sees it without scrolling
even while adjusting Duty / Frequency below.

### RPM panel

The current panel (markup at [components/net_dashboard/web/index.html:26-36](components/net_dashboard/web/index.html#L26-L36)) is restructured. The
WebSocket contract and the `set_rpm` message shape are unchanged.

DOM structure:

```text
┌─ panel ─────────────────────────────────────────────────┐
│   1,234 RPM           [⚙ RPM 設定 ▾]  [☑ Live]          │
│                                  └─ popover ─┐           │
│                                  │ Pole [2]  │           │
│                                  │ Avg  [16] │           │
│                                  │ T/O [1k…] │           │
│                                  │ [Apply]   │           │
│                                  └───────────┘           │
│                                                          │
│   ▸ RPM 圖表  (default collapsed)                        │
│      [Y-axis ticks][canvas chart with gridlines]         │
│                    [X-axis ticks: −15s … now]            │
│                            time                          │
└─────────────────────────────────────────────────────────┘
```

Behavior:

- **Header row**: big readout left, then a flex spacer, then a gear-
  styled `RPM 設定` `<details>` button, then the Live checkbox
  (default checked). Inbound `status` messages always update the
  readout — the Live checkbox does not affect it.
- **RPM 設定 popover**: clicking the gear opens an absolutely-
  positioned `<div class="popover">` anchored to its right edge so it
  doesn't push the chart down. Contains pole pairs, avg window,
  timeout (µs), and the Apply RPM button. Default state: collapsed.
  Closes when the gear is clicked again. Settings only commit on
  Apply RPM (existing `set_rpm` message), not on input change.
- **Plot expander** (collapsed by default): native `<details>`. The
  canvas is only sized/drawn after first open (use the `toggle` event
  to lazy-size). While closed, status updates still feed the in-memory
  `history` ring buffer (so opening the expander shows recent data
  immediately) — only rendering is skipped.
- **Live checkbox**: when **unchecked**, `draw()` is no-op'd (the
  current frame stays on screen) and incoming samples are dropped from
  `history` (not buffered). When **rechecked**, `history` is cleared
  and refills from "now" forward — no backfill of frozen-window data.
  This matches user decision (a) from brainstorming.
- **Y-axis**: fixed scale with ticks at 0 / 500 / 1000 / 1500 / 2000
  RPM. Auto-rescale only when an incoming sample exceeds the current
  max (next power-of-2 multiple of 500); never shrinks during a
  session. Replaces the current `Math.max(1, ...history)` per-frame
  rescale, which causes the trace to "breathe" with every update.
- **X-axis**: time-based. With 50 ms broadcast cadence and
  `MAX_POINTS=300`, the window covers the last 15 s. Tick labels:
  `−15s · −10s · −5s · now`. Update tick text only when the window
  changes (i.e. on resize), not every frame.
- **Axis labels**: "RPM" rotated 90° on Y, "time" centered under X.
- **Gridlines** drawn inside the canvas at the same intervals as the
  axis ticks for visual reference.

## Component breakdown

### `app.js` — three pieces

```text
ws.js-equivalent (already exists, reused as-is):
  connect(), send(), onstatus(payload)

makePanel(opts):
  factory that wires up one panel given:
    rootSelector, kind ("duty"|"freq"),
    range {min, max}, sliderMap (linear|log10),
    fineTuneDeltas [d1, d2, d3],
    presetDefaults [6 numbers],
    storageKeys {presets, step},
    onCommit(value) → sends set_pwm with the other axis's last value
  exposes:
    setFromDevice(value)  — called from status handler
    getValue()            — current displayed value

Top-level wiring:
  const dutyPanel = makePanel({...});
  const freqPanel = makePanel({...});
  ws.onstatus = ({freq, duty, rpm}) => {
    dutyPanel.setFromDevice(duty);
    freqPanel.setFromDevice(freq);
    rpmPanel.update(rpm);  // existing
  };
```

`onCommit` builds the full `set_pwm` payload by combining the local
panel's new value with the *other* panel's currently-displayed value.
This avoids the device receiving stale freq while the user adjusts duty.

### `index.html` — panel markup

Top-level `<body>` order is RPM → Duty → Frequency → OTA → Factory-reset.

- **RPM section** (replaces current lines 26–36): no `<h2>`; header row
  has the big `<span id="rpm">` readout plus the `<label class="live-toggle"><input type="checkbox" id="rpm-live" checked>Live</label>`.
  Two `<details>` (no `open` attribute) follow:
  `RPM 圖表` containing the canvas + axis tick rows, and `RPM 設定`
  containing the existing pole/mavg/timeout inputs and Apply RPM button.
- **PWM sections** (replace current lines 12–24): two
  `<section class="panel pwm-panel">` blocks (Duty, Frequency) per the
  PWM panel template. Use `<details>` for accordions; Duty's two get
  `open`, Frequency's two do not.
- **OTA + Factory-reset** sections stay unchanged structurally; they
  keep their existing IDs and JS handlers, only inherit new chrome
  via `.panel`.

### `app.css` — section additions

- New CSS tokens (above)
- `.panel` rewrite using tokens
- `.pwm-panel` specifics (`.readout-input`, `.row`, `.ticks`,
  `.grid-finetune`, `.preset-grid`, step input)
- RPM panel specifics: `.rpm-readout` (flex header, big number,
  Live checkbox alignment), `.live-toggle`, `.chart-row` (Y-axis
  column + canvas column flex layout), `.yaxis-col` (vertical "RPM"
  label + tick column), `.x-ticks`, `.x-label`, `.rpm-config` grid
- `details > summary` chevron styling, shared
- No changes to OTA progress bar or factory-reset button styles
  beyond inheriting `.panel`'s new chrome

## Error handling

- **WebSocket disconnect**: existing reconnect logic in `app.js` is
  unchanged. While disconnected, controls remain interactive but
  `onCommit` queues nothing (current behavior — sends are dropped).
  Acceptable — users can see the disconnect indicator.
- **Bad localStorage payload**: try/catch around `JSON.parse`; on any
  error, log a warning to console and use baked-in defaults.
- **Out-of-range numeric input**: clamp silently. No toast / no inline
  error — the displayed value snaps to the clamped value, which is its
  own feedback.
- **Slider value crosses the HI↔LO band boundary at 152↔153 Hz**:
  expected; the device emits a brief output discontinuity per
  `pwm_gen.c`. The dashboard does not warn — most users won't trigger
  it intentionally and it's not a UI failure.

## Testing

Manual verification on the dev board (no automated test infrastructure
for the dashboard exists in this project):

1. **Duty panel basics**: load dashboard, drag slider 0→100. Readout
   tracks while dragging; only one `set_pwm` lands on the device on
   release (verify with `idf.py -p COM24 monitor` showing the WS log).
2. **Duty fine-tune**: at duty=50, click +0.1 / +1 / +10 / −10 / −1 /
   −0.1. Each click increments the displayed value and sends one
   message. Final value matches start.
3. **Duty step**: change "步進" to 5; confirm slider keyboard ↑/↓
   moves in 5 % increments. Fine-tune buttons unaffected (still 0.1/1/10).
4. **Duty presets**: edit preset slot from 75 → 73.5; reload page;
   slot still shows 73.5. Click 套用; device receives duty=73.5 with
   current freq. Open browser DevTools → Application → Local Storage
   shows the updated array.
5. **Frequency log slider**: drag end-to-end; readout walks through
   10 → 100 → 1k → 10k → 100k → 1M with each decade taking ~equal
   slider width. Tick labels align under decade boundaries.
6. **Frequency resolution hint**: at 100 Hz shows "16 bits" (or
   whatever the formula returns); at 100 kHz shows ~7 bits. Updates
   live as freq changes.
7. **Frequency fine-tune**: ±1, ±10, ±100. Cross 152↔153 Hz boundary
   intentionally; confirm the readout updates and the device responds
   (the brief discontinuity is expected, not a UI bug).
8. **Cross-axis correctness**: set duty=42 then change frequency via
   slider. Verify the `set_pwm` from the freq panel includes
   `duty=42` (i.e. `onCommit` reads the other axis correctly).
9. **Light/dark mode**: toggle OS color scheme; confirm panel chrome,
   readout color, and fine-tune button colors all switch sensibly.
10. **OTA + factory-reset intact**: OTA upload still works, factory-
    reset confirm still triggers reset. They visually pick up the new
    panel border-radius and padding consistently.
11. **RPM panel placement**: RPM is the topmost panel; big readout
    visible without scrolling. No `<h2>` title above it.
12. **RPM expanders default closed**: on first load (and on reload),
    both `RPM 圖表` and `RPM 設定` are collapsed; clicking each opens
    them with the expected content.
13. **RPM live freeze**: open `RPM 圖表`, watch trace update; uncheck
    "Live"; trace stops updating mid-frame and stays frozen. Big
    readout above keeps changing as device sends new values. Re-check
    "Live" → trace clears history and resumes from the right edge.
14. **RPM Y-axis scale**: at typical fan speeds (~1000–2000 RPM), Y-
    axis labels read 0/500/1000/1500/2000. Spike to a higher RPM
    once → axis re-scales up to next 500-multiple and stays there
    (no shrink during the session). Reload page → axis resets to
    default scale.
15. **RPM X-axis labels**: tick labels read `−15s · −10s · −5s · now`
    (or whatever the window math yields for 300 points × 50 ms);
    labels stable across resize.

End-to-end flush: after points 1–15 pass, open the WS handler logs
during a 30-second sweep session and confirm message rate stays well
under 100 Hz (slider release + button clicks only, no drag floods).

## Open Questions

1. **Docs/code drift on PWM range and LO band resolution**:
   `pwm_gen.h:20` says valid range is "5 Hz .. 1_000_000 Hz" and
   "LO=320 kHz", but `CLAUDE.md` says 10 Hz – 1 MHz and LO=625 kHz.
   The current HTML uses `min="5"`. This spec follows CLAUDE.md
   (10 Hz minimum) for the slider; the duty-resolution formula uses
   320 kHz to match `pwm_gen.c` runtime behavior. Reconciling the
   firmware comments is a separate task — not blocking this refactor.
