(() => {
  const ws = new WebSocket(`ws://${location.host}/ws`);

  // ---------- shared helpers ----------
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

  const lastSent = { freq: 1000, duty: 0 };
  function sendPwm(freq, duty) {
    if (ws.readyState !== WebSocket.OPEN) return;
    lastSent.freq = freq;
    lastSent.duty = duty;
    ws.send(JSON.stringify({ type: 'set_pwm', freq, duty }));
  }

  // mirrors components/pwm_gen/pwm_gen.c band table
  function dutyResolutionBits(freqHz) {
    const RES_HI = 10_000_000, RES_LO = 320_000;
    const res = freqHz >= 153 ? RES_HI : RES_LO;
    const period = Math.max(1, Math.floor(res / freqHz));
    return Math.max(0, Math.floor(Math.log2(period)));
  }

  // ---------- PWM panel factory ----------
  function makePanel(opts) {
    const {
      kind, axis, range, defaultValue, presetDefaults, presetStorageKey,
      stepStorageKey, sliderToValue, valueToSlider, formatReadout,
      onCommit, onValueChanged,
    } = opts;

    const root = document.getElementById(`${kind}-panel`);
    const readout = document.getElementById(`${kind}-readout`);
    const slider = document.getElementById(`${kind}-slider`);
    const stepInput = document.getElementById(`${kind}-step`);
    const finetuneRoot = document.getElementById(`${kind}-finetune`);
    const presetsRoot = document.getElementById(`${kind}-presets`);

    let current = defaultValue;

    // Step input wiring — controls slider step only
    function loadStep() {
      try {
        const raw = localStorage.getItem(stepStorageKey);
        const n = parseFloat(raw);
        if (!isFinite(n) || n <= 0) return;
        stepInput.value = n;
      } catch (e) { /* ignore */ }
    }
    function applyStepToSlider() {
      const n = parseFloat(stepInput.value);
      if (!isFinite(n) || n <= 0) return;
      slider.step = String(n);
    }
    stepInput.addEventListener('change', () => {
      const n = parseFloat(stepInput.value);
      if (!isFinite(n) || n <= 0) {
        stepInput.value = slider.step || 1;
        return;
      }
      try { localStorage.setItem(stepStorageKey, String(n)); } catch (e) {}
      applyStepToSlider();
    });
    loadStep();
    applyStepToSlider();

    function setLocal(value, { commit = false, fromDevice = false } = {}) {
      const v = clamp(value, range.min, range.max);
      current = v;
      readout.value = formatReadout(v);
      // Preserve focus inside readout: only update slider if it's not being dragged
      if (document.activeElement !== slider) {
        slider.value = String(valueToSlider(v));
      }
      if (onValueChanged) onValueChanged(v);
      if (commit && !fromDevice) onCommit(v);
    }

    // Slider — fires on release (change), drag uses input for live readout
    slider.addEventListener('input', () => {
      const v = sliderToValue(parseFloat(slider.value));
      readout.value = formatReadout(v);
      current = clamp(v, range.min, range.max);
      if (onValueChanged) onValueChanged(current);
    });
    slider.addEventListener('change', () => {
      const v = sliderToValue(parseFloat(slider.value));
      setLocal(v, { commit: true });
    });

    // Readout — commit on Enter or blur
    readout.addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter') readout.blur();
    });
    readout.addEventListener('change', () => {
      const v = parseFloat(readout.value);
      if (!isFinite(v)) {
        readout.value = formatReadout(current);
        return;
      }
      setLocal(v, { commit: true });
    });

    // Fine-tune buttons — fixed deltas, immediate commit
    finetuneRoot.querySelectorAll('button').forEach((btn) => {
      const delta = parseFloat(btn.dataset.delta);
      btn.addEventListener('click', () => {
        setLocal(current + delta, { commit: true });
      });
    });

    // Presets — load from storage or defaults; render slots; wire 套用 + edit
    function loadPresets() {
      try {
        const raw = localStorage.getItem(presetStorageKey);
        const arr = JSON.parse(raw);
        if (Array.isArray(arr) && arr.length === presetDefaults.length
            && arr.every((n) => typeof n === 'number' && isFinite(n))) {
          return arr;
        }
      } catch (e) { /* ignore */ }
      return [...presetDefaults];
    }
    function savePresets(arr) {
      try { localStorage.setItem(presetStorageKey, JSON.stringify(arr)); }
      catch (e) { /* ignore */ }
    }
    const presets = loadPresets();
    presets.forEach((value, i) => {
      const input = document.createElement('input');
      input.type = 'number';
      input.value = String(value);
      input.addEventListener('change', () => {
        const n = parseFloat(input.value);
        if (!isFinite(n)) {
          input.value = String(presets[i]);
          return;
        }
        presets[i] = clamp(n, range.min, range.max);
        input.value = String(presets[i]);
        savePresets(presets);
      });
      const btn = document.createElement('button');
      btn.textContent = '套用';
      btn.addEventListener('click', () => {
        setLocal(presets[i], { commit: true });
      });
      presetsRoot.appendChild(input);
      presetsRoot.appendChild(btn);
    });

    // Initial draw
    setLocal(current, { commit: false, fromDevice: true });

    return {
      setFromDevice(value) {
        // Don't fight a focused readout (user is typing)
        if (document.activeElement === readout) return;
        setLocal(value, { commit: false, fromDevice: true });
      },
      getValue() { return current; },
    };
  }

  // ---------- RPM controller ----------
  const rpmReadout = document.getElementById('rpm');
  const rpmLive = document.getElementById('rpm-live');
  const rpmChartExp = document.getElementById('rpm-chart-expander');
  const canvas = document.getElementById('chart');
  const ctx = canvas.getContext('2d');

  const rpmHistory = [];           // entries: {t, rpm}
  const RPM_WINDOW_MS = 15_000;
  let yAxisMax = 2000;
  let chartSized = false;

  function bumpYAxisIfNeeded(rpm) {
    if (rpm <= yAxisMax) return;
    const next = Math.ceil(rpm / 500) * 500;
    if (next === yAxisMax) return;
    yAxisMax = next;
    // Update visible Y-axis tick labels (5 ticks: max, 3/4, 1/2, 1/4, 0)
    const yTicks = document.getElementById('y-ticks');
    if (yTicks) {
      const spans = yTicks.querySelectorAll('span');
      const labels = [yAxisMax, yAxisMax * 0.75, yAxisMax * 0.5, yAxisMax * 0.25, 0];
      labels.forEach((v, i) => { if (spans[i]) spans[i].textContent = String(Math.round(v)); });
    }
  }

  function ensureChartSize() {
    if (!canvas.clientWidth) return false;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.floor(canvas.clientWidth * dpr);
    canvas.height = Math.floor(140 * dpr);
    chartSized = true;
    return true;
  }

  function drawChart() {
    if (!rpmChartExp.open) return;
    if (!chartSized && !ensureChartSize()) return;
    const w = canvas.width, h = canvas.height;
    const dpr = window.devicePixelRatio || 1;
    ctx.clearRect(0, 0, w, h);

    // gridlines (4 horizontal divisions, 4 vertical)
    const borderColor = getComputedStyle(document.documentElement)
      .getPropertyValue('--border').trim() || '#d4d4d8';
    ctx.strokeStyle = borderColor;
    ctx.lineWidth = dpr;
    for (let i = 1; i < 4; i++) {
      const y = (h * i) / 4;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    }
    for (let i = 1; i < 3; i++) {
      const x = (w * i) / 3;
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
    }

    if (rpmHistory.length < 2) return;

    const greenColor = getComputedStyle(document.documentElement)
      .getPropertyValue('--green').trim() || '#16a34a';
    ctx.strokeStyle = greenColor;
    ctx.lineWidth = 1.6 * dpr;
    ctx.beginPath();
    const now = performance.now();
    rpmHistory.forEach((pt, i) => {
      const ageMs = now - pt.t;
      const x = w * (1 - ageMs / RPM_WINDOW_MS);
      const y = h - (pt.rpm / yAxisMax) * (h - 4 * dpr) - 2 * dpr;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  function pruneHistory() {
    const cutoff = performance.now() - RPM_WINDOW_MS;
    while (rpmHistory.length && rpmHistory[0].t < cutoff) rpmHistory.shift();
  }

  function setRpmFromDevice(rpm) {
    rpmReadout.textContent = Number.isFinite(rpm) ? rpm.toFixed(1) : '—';
    if (!rpmLive.checked) return;
    rpmHistory.push({ t: performance.now(), rpm: Math.max(0, rpm) });
    pruneHistory();
    bumpYAxisIfNeeded(rpm);
    drawChart();
  }

  rpmLive.addEventListener('change', () => {
    if (rpmLive.checked) {
      // Resume from "now" — drop frozen-window data per spec decision (a)
      rpmHistory.length = 0;
    }
  });

  rpmChartExp.addEventListener('toggle', () => {
    if (!rpmChartExp.open) return;
    chartSized = false;
    ensureChartSize();
    drawChart();
  });

  window.addEventListener('resize', () => {
    if (!rpmChartExp.open) return;
    chartSized = false;
    ensureChartSize();
    drawChart();
  });

  // ---------- panel instances ----------
  const dutyPanel = makePanel({
    kind: 'duty',
    axis: 'duty',
    range: { min: 0, max: 100 },
    defaultValue: 0,
    presetDefaults: [0, 10, 25, 50, 75, 100],
    presetStorageKey: 'fan-testkit:duty-presets',
    stepStorageKey: 'fan-testkit:duty-step',
    sliderToValue: (s) => s,
    valueToSlider: (v) => Math.round(v),
    formatReadout: (v) => v.toFixed(1),
    onCommit: (v) => sendPwm(lastSent.freq, v),
  });

  const freqResbits = document.getElementById('freq-resbits');
  const freqPanel = makePanel({
    kind: 'freq',
    axis: 'freq',
    range: { min: 10, max: 1_000_000 },
    defaultValue: 1000,
    presetDefaults: [25, 100, 1000, 5000, 25000, 100000],
    presetStorageKey: 'fan-testkit:freq-presets',
    stepStorageKey: 'fan-testkit:freq-step',
    // log10 mapping: slider 100..600 → freq 10^1..10^6 = 10..1_000_000
    sliderToValue: (s) => {
      const f = Math.pow(10, s / 100);
      return Math.round(f);
    },
    valueToSlider: (v) => clamp(Math.round(Math.log10(Math.max(1, v)) * 100), 100, 600),
    formatReadout: (v) => String(Math.round(v)),
    onCommit: (v) => sendPwm(v, lastSent.duty),
    onValueChanged: (v) => {
      freqResbits.textContent = String(dutyResolutionBits(v));
    },
  });

  // ---------- WebSocket dispatch ----------
  ws.addEventListener('message', (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.type === 'status') {
        if (typeof msg.freq === 'number') freqPanel.setFromDevice(msg.freq);
        if (typeof msg.duty === 'number') dutyPanel.setFromDevice(msg.duty);
        if (typeof msg.rpm === 'number') setRpmFromDevice(msg.rpm);
        // Track whatever the device just told us so the next single-axis
        // commit sends the correct other-axis value.
        if (typeof msg.freq === 'number') lastSent.freq = msg.freq;
        if (typeof msg.duty === 'number') lastSent.duty = msg.duty;
      } else if (msg.type === 'ack' && msg.op === 'factory_reset') {
        const fs = document.getElementById('factory_reset_status');
        if (fs) fs.textContent = 'Device acknowledged — rebooting…';
      }
    } catch (e) { /* ignore non-JSON / partial frames */ }
  });

  // ---------- RPM apply (existing contract) ----------
  document.getElementById('apply_rpm').addEventListener('click', () => {
    const pole = parseInt(document.getElementById('pole').value, 10);
    const mavg = parseInt(document.getElementById('mavg').value, 10);
    const timeout_us = parseInt(document.getElementById('timeout_us').value, 10);
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'set_rpm', pole, mavg, timeout_us }));
    }
    document.getElementById('rpm-settings').open = false;
  });

  // Close RPM 設定 popover when clicking outside it
  document.addEventListener('click', (ev) => {
    const settings = document.getElementById('rpm-settings');
    if (settings.open && !settings.contains(ev.target)) settings.open = false;
  });

  // ---------- OTA ----------
  document.getElementById('upload').addEventListener('click', async () => {
    const f = document.getElementById('fwfile').files[0];
    if (!f) return;
    const prog = document.getElementById('otaprog');
    const r = await fetch('/ota', { method: 'POST', body: f });
    if (r.ok) { prog.value = 100; alert('OTA accepted; device will reboot.'); }
    else     { alert(`OTA failed: ${r.status}`); }
  });

  // ---------- Factory reset ----------
  const factoryBtn = document.getElementById('factory_reset');
  const factoryStatus = document.getElementById('factory_reset_status');
  factoryBtn.addEventListener('click', () => {
    const ok = confirm(
      'Factory reset: clear Wi-Fi credentials and reboot?\n\n' +
      'The device will disconnect and open the Fan-TestKit-setup SoftAP for re-provisioning.'
    );
    if (!ok) return;
    factoryBtn.disabled = true;
    factoryStatus.textContent = 'Sending…';
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'factory_reset' }));
    }
  });
})();
