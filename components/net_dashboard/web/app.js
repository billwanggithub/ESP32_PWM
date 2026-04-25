(() => {
  const ws = new WebSocket(`ws://${location.host}/ws`);

  // ---------- shared helpers ----------
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

  // ---------- i18n ----------
  const I18N = {
    en: {
      app_title: 'Fan-TestKit Dashboard',
      language: 'Language',
      settings: 'Settings',
      rpm: 'RPM',
      pole_count: 'Pole count',
      avg_window: 'Avg window',
      timeout_us: 'Timeout (µs)',
      apply_rpm: 'Apply RPM',
      step_sizes: 'Step sizes',
      duty_step: 'Duty step (%)',
      freq_step: 'Frequency step (Hz)',
      step_hint: 'Step sizes apply immediately to the slider step on the Duty / Frequency panels.',
      live: 'Live',
      chart: 'Chart',
      time: 'time',
      duty: 'Duty',
      frequency: 'Frequency',
      fine: 'Fine',
      presets: 'Presets',
      duty_res_label: 'duty resolution at this freq:',
      bits: 'bits',
      firmware_update: 'Firmware Update',
      upload_reboot: 'Upload & reboot',
      factory_reset: 'Factory Reset',
      factory_reset_warn: 'Clears stored Wi-Fi credentials and reboots. The device will open the Fan-TestKit-setup SoftAP for re-provisioning.',
      reset_wifi_btn: 'Reset Wi-Fi & restart',
      help: 'Help',
      help_pins_h: 'Pin assignments',
      help_pin_pwm: 'PWM output:',
      help_pin_trigger: 'Change-trigger output:',
      help_pin_rpm: 'RPM capture input:',
      help_pin_led: 'Status LED (WS2812):',
      help_pwm_h: 'Duty / Frequency',
      help_pwm_p: 'Drives the PWM-output pin. Frequency range <span data-info="freq_min">?</span>&nbsp;Hz to <span data-info="freq_max">?</span>&nbsp;Hz. Duty resolution decreases at high frequencies — the live "duty resolution at this freq" readout shows the current bit count.',
      help_rpm_h: 'RPM',
      help_rpm_p: 'Reads tach edges on the RPM-input pin. <em>RPM = edges/sec × 60 × 2 / pole count.</em> RPM tunables and slider step sizes live in the Settings panel.',
      help_ota_h: 'Firmware Update',
      help_ota_p: 'Pick a <code>.bin</code> and click Upload. The device flashes to the inactive OTA slot and reboots into it.',
      help_reset_h: 'Factory Reset',
      help_reset_p: 'Clears stored Wi-Fi credentials and reboots. Same effect as holding the BOOT button for ≥ 3 seconds.',
      help_dev_notes: 'Developer notes',
      help_dev_notes_p: 'The same reset is also reachable via USB HID report <code>0x03</code> with magic byte <code>0xA5</code>, or USB CDC op <code>0x20</code> with magic byte <code>0xA5</code>. All four entry points land on <code>net_dashboard_factory_reset()</code>.',
      help_wifi_h: 'Wi-Fi setup fallback',
      help_wifi_p: 'If credentials are missing or wrong, the device opens an open SoftAP named <code>Fan-TestKit-setup</code>. Connect from a phone — the captive-portal page lets you enter SSID/password. After success the dashboard is reachable at <code>fan-testkit.local</code> or the assigned IP.',
      factory_confirm: 'Factory reset: clear Wi-Fi credentials and reboot?\n\nThe device will disconnect and open the Fan-TestKit-setup SoftAP for re-provisioning.',
      factory_sending: 'Sending…',
      factory_acked: 'Device acknowledged — rebooting…',
      ota_accepted: 'OTA accepted; device will reboot.',
      ota_failed: 'OTA failed: ',
      power_switch: 'Power Switch',
      power_state: 'State',
      power_on: 'ON',
      power_off: 'OFF',
      gpio: 'GPIO',
      group_a: 'Group A',
      group_b: 'Group B',
      gpio_input_pulldown: 'input + pull-down',
      gpio_input_pullup: 'input + pull-up',
      gpio_input_floating: 'input + floating',
      gpio_output: 'output',
      gpio_pulse_btn: 'Pulse',
      gpio_pulsing: 'Pulsing…',
      gpio_value_label: 'value:',
      gpio_output_section: 'GPIO output',
      pulse_width_label: 'Pulse width (ms)',
      apply_pulse_width: 'Apply',
      help_pin_power: 'Power-switch:',
      help_pin_group_a: 'GPIO Group A:',
      help_pin_group_b: 'GPIO Group B:',
      help_gpio_h: 'GPIO & Power',
      help_gpio_p: 'Group A defaults to input (pull-down) and Group B defaults to output (low). Each pin can be flipped between input and output at runtime; outputs support level toggle and a one-shot pulse of configurable width. Power switch toggles a GPIO whose polarity (active-high / active-low) is set at build time via Kconfig.',
    },
    'zh-Hant': {
      app_title: 'Fan-TestKit 儀表板',
      language: '語言',
      settings: '設定',
      rpm: '轉速',
      pole_count: '極數',
      avg_window: '平均視窗',
      timeout_us: '逾時 (µs)',
      apply_rpm: '套用轉速設定',
      step_sizes: '步進量',
      duty_step: '工作週期步進 (%)',
      freq_step: '頻率步進 (Hz)',
      step_hint: '步進量會立即套用到工作週期 / 頻率面板的滑桿。',
      live: '即時',
      chart: '圖表',
      time: '時間',
      duty: '工作週期',
      frequency: '頻率',
      fine: '微調',
      presets: '預設值',
      duty_res_label: '此頻率的工作週期解析度：',
      bits: '位元',
      firmware_update: '韌體更新',
      upload_reboot: '上傳並重開機',
      factory_reset: '回復出廠設定',
      factory_reset_warn: '清除已儲存的 Wi-Fi 帳密並重開機。裝置會開啟 Fan-TestKit-setup SoftAP 供重新配網。',
      reset_wifi_btn: '重設 Wi-Fi 並重開機',
      help: '說明',
      help_pins_h: '腳位配置',
      help_pin_pwm: 'PWM 輸出：',
      help_pin_trigger: '變更觸發輸出：',
      help_pin_rpm: '轉速輸入：',
      help_pin_led: '狀態 LED (WS2812)：',
      help_pwm_h: '工作週期 / 頻率',
      help_pwm_p: '驅動 PWM 輸出腳。頻率範圍 <span data-info="freq_min">?</span>&nbsp;Hz 至 <span data-info="freq_max">?</span>&nbsp;Hz。高頻時工作週期解析度會降低 — 即時顯示的「duty resolution at this freq」會顯示目前位元數。',
      help_rpm_h: '轉速',
      help_rpm_p: '讀取轉速輸入腳的 tach 邊緣。<em>RPM = 邊緣數/秒 × 60 × 2 / 極數。</em>轉速調整參數與滑桿步進量都在「設定」面板。',
      help_ota_h: '韌體更新',
      help_ota_p: '選一個 <code>.bin</code> 然後按上傳。裝置會把韌體寫入待用 OTA slot 後重開機切過去。',
      help_reset_h: '回復出廠設定',
      help_reset_p: '清除已儲存的 Wi-Fi 帳密並重開機。等同按住 BOOT 按鈕 ≥ 3 秒。',
      help_dev_notes: '開發者備註',
      help_dev_notes_p: '同樣的 reset 也可以透過 USB HID report <code>0x03</code> + magic byte <code>0xA5</code>，或 USB CDC op <code>0x20</code> + magic byte <code>0xA5</code> 觸發。四個入口最後都會 land 在 <code>net_dashboard_factory_reset()</code>。',
      help_wifi_h: 'Wi-Fi 設定 fallback',
      help_wifi_p: '若帳密遺失或錯誤，裝置會開啟名為 <code>Fan-TestKit-setup</code> 的開放 SoftAP。用手機連上 — captive portal 頁面會讓你輸入 SSID / password。成功後 dashboard 可以從 <code>fan-testkit.local</code> 或分配到的 IP 連入。',
      factory_confirm: '回復出廠設定：清除 Wi-Fi 帳密並重開機？\n\n裝置會中斷連線並開啟 Fan-TestKit-setup SoftAP 供重新配網。',
      factory_sending: '傳送中…',
      factory_acked: '裝置已確認 — 重開機中…',
      ota_accepted: 'OTA 已接受；裝置即將重開機。',
      ota_failed: 'OTA 失敗：',
      power_switch: '電源開關',
      power_state: '狀態',
      power_on: '開',
      power_off: '關',
      gpio: 'GPIO',
      group_a: 'A 組',
      group_b: 'B 組',
      gpio_input_pulldown: '輸入 + 下拉',
      gpio_input_pullup: '輸入 + 上拉',
      gpio_input_floating: '輸入 + 浮接',
      gpio_output: '輸出',
      gpio_pulse_btn: '脈衝',
      gpio_pulsing: '脈衝中…',
      gpio_value_label: '值：',
      gpio_output_section: 'GPIO 輸出',
      pulse_width_label: '脈衝寬度 (ms)',
      apply_pulse_width: '套用',
      help_pin_power: '電源開關：',
      help_pin_group_a: 'GPIO A 組：',
      help_pin_group_b: 'GPIO B 組：',
      help_gpio_h: 'GPIO 與電源',
      help_gpio_p: 'A 組預設為輸入（下拉），B 組預設為輸出（低）。每隻 pin 可在執行時切換輸入 / 輸出；輸出模式支援切換準位以及單發脈衝（脈衝寬度可設）。電源開關控制一隻 GPIO，其 active-high / active-low 由 Kconfig 編譯時決定。',
    },
    'zh-Hans': {
      app_title: 'Fan-TestKit 仪表板',
      language: '语言',
      settings: '设置',
      rpm: '转速',
      pole_count: '极数',
      avg_window: '平均窗口',
      timeout_us: '超时 (µs)',
      apply_rpm: '应用转速设置',
      step_sizes: '步进量',
      duty_step: '占空比步进 (%)',
      freq_step: '频率步进 (Hz)',
      step_hint: '步进量会立即应用到占空比 / 频率面板的滑块。',
      live: '实时',
      chart: '图表',
      time: '时间',
      duty: '占空比',
      frequency: '频率',
      fine: '微调',
      presets: '预设值',
      duty_res_label: '当前频率的占空比分辨率：',
      bits: '位',
      firmware_update: '固件更新',
      upload_reboot: '上传并重启',
      factory_reset: '恢复出厂设置',
      factory_reset_warn: '清除已保存的 Wi-Fi 凭据并重启。设备会开启 Fan-TestKit-setup SoftAP 供重新配网。',
      reset_wifi_btn: '重置 Wi-Fi 并重启',
      help: '帮助',
      help_pins_h: '引脚分配',
      help_pin_pwm: 'PWM 输出：',
      help_pin_trigger: '变更触发输出：',
      help_pin_rpm: '转速输入：',
      help_pin_led: '状态 LED (WS2812)：',
      help_pwm_h: '占空比 / 频率',
      help_pwm_p: '驱动 PWM 输出引脚。频率范围 <span data-info="freq_min">?</span>&nbsp;Hz 至 <span data-info="freq_max">?</span>&nbsp;Hz。高频时占空比分辨率会降低 — 实时显示的「duty resolution at this freq」会显示当前位数。',
      help_rpm_h: '转速',
      help_rpm_p: '读取转速输入引脚的 tach 边沿。<em>RPM = 边沿数/秒 × 60 × 2 / 极数。</em>转速调整参数与滑块步进量都在「设置」面板。',
      help_ota_h: '固件更新',
      help_ota_p: '选一个 <code>.bin</code> 然后按上传。设备会把固件写入待用 OTA slot 后重启切过去。',
      help_reset_h: '恢复出厂设置',
      help_reset_p: '清除已保存的 Wi-Fi 凭据并重启。等同按住 BOOT 按钮 ≥ 3 秒。',
      help_dev_notes: '开发者备注',
      help_dev_notes_p: '同样的 reset 也可以通过 USB HID report <code>0x03</code> + magic byte <code>0xA5</code>，或 USB CDC op <code>0x20</code> + magic byte <code>0xA5</code> 触发。四个入口最后都会 land 在 <code>net_dashboard_factory_reset()</code>。',
      help_wifi_h: 'Wi-Fi 配网 fallback',
      help_wifi_p: '若凭据丢失或错误，设备会开启名为 <code>Fan-TestKit-setup</code> 的开放 SoftAP。用手机连上 — captive portal 页面会让你输入 SSID / password。成功后 dashboard 可以从 <code>fan-testkit.local</code> 或分配到的 IP 连入。',
      factory_confirm: '恢复出厂设置：清除 Wi-Fi 凭据并重启？\n\n设备会断开连接并开启 Fan-TestKit-setup SoftAP 供重新配网。',
      factory_sending: '发送中…',
      factory_acked: '设备已确认 — 重启中…',
      ota_accepted: 'OTA 已接受；设备即将重启。',
      ota_failed: 'OTA 失败：',
      power_switch: '电源开关',
      power_state: '状态',
      power_on: '开',
      power_off: '关',
      gpio: 'GPIO',
      group_a: 'A 组',
      group_b: 'B 组',
      gpio_input_pulldown: '输入 + 下拉',
      gpio_input_pullup: '输入 + 上拉',
      gpio_input_floating: '输入 + 浮接',
      gpio_output: '输出',
      gpio_pulse_btn: '脉冲',
      gpio_pulsing: '脉冲中…',
      gpio_value_label: '值：',
      gpio_output_section: 'GPIO 输出',
      pulse_width_label: '脉冲宽度 (ms)',
      apply_pulse_width: '应用',
      help_pin_power: '电源开关：',
      help_pin_group_a: 'GPIO A 组：',
      help_pin_group_b: 'GPIO B 组：',
      help_gpio_h: 'GPIO 与电源',
      help_gpio_p: 'A 组默认为输入（下拉），B 组默认为输出（低）。每个 pin 可在运行时切换输入 / 输出；输出模式支持切换电平以及单发脉冲（脉冲宽度可设）。电源开关控制一个 GPIO，其 active-high / active-low 由 Kconfig 编译时决定。',
    },
  };

  const LANG_STORAGE_KEY = 'fan-testkit:lang';
  let currentLang = 'en';

  function t(key) {
    return (I18N[currentLang] && I18N[currentLang][key]) || I18N.en[key] || key;
  }

  function applyLang(lang) {
    if (!I18N[lang]) lang = 'en';
    currentLang = lang;
    document.documentElement.lang = lang;
    document.querySelectorAll('[data-i18n]').forEach(el => {
      const key = el.dataset.i18n;
      const value = t(key);
      // Strings containing markup (<em>, <code>, data-info spans) round-trip
      // through innerHTML; plain ones use textContent so any user-controlled
      // strings stay safely escaped. Translation source is in this file only,
      // so the HTML side of the fork is trusted.
      if (/[<&]/.test(value)) {
        el.innerHTML = value;
        // After innerHTML overwrite, re-fill any [data-info] placeholders
        // the helper would normally populate at load time.
        el.querySelectorAll('[data-info]').forEach(infoEl => {
          if (deviceInfoMap && deviceInfoMap[infoEl.dataset.info] !== undefined) {
            infoEl.textContent = deviceInfoMap[infoEl.dataset.info];
          }
        });
      } else {
        el.textContent = value;
      }
    });
    try { localStorage.setItem(LANG_STORAGE_KEY, lang); } catch (e) { /* ignore */ }
  }

  let deviceInfoMap = null;

  // ---------- Device info (Help block, fetched once) ----------
  async function loadDeviceInfo() {
    try {
      const r = await fetch('/api/device_info');
      if (!r.ok) throw new Error(r.status);
      const info = await r.json();
      document.querySelectorAll('[data-pin]').forEach(el => {
        const v = info.pins ? info.pins[el.dataset.pin] : undefined;
        el.textContent = (v ?? '?').toString();
      });
      // group A / B pin lists rendered as comma-separated
      document.querySelectorAll('[data-pin-list]').forEach(el => {
        const arr = info.pins ? info.pins[el.dataset.pinList] : null;
        el.textContent = Array.isArray(arr) ? arr.map(n => `GPIO${n}`).join(', ') : '?';
      });
      // build GPIO rows (need group_a / group_b pin numbers from device_info)
      if (info.pins) buildGpioRows(info.pins.group_a || [], info.pins.group_b || []);
      deviceInfoMap = { freq_min: info.freq_hz_min, freq_max: info.freq_hz_max };
      document.querySelectorAll('[data-info]').forEach(el => {
        const v = deviceInfoMap[el.dataset.info];
        el.textContent = (v ?? '?').toString();
      });
      // Apply device boot defaults to the Settings inputs the first time we
      // reach them — keeps the dashboard in lockstep with Kconfig defaults.
      if (info.defaults) {
        const poleSel = document.getElementById('pole');
        if (poleSel && info.defaults.pole_count !== undefined) {
          const want = String(info.defaults.pole_count);
          // If the device default isn't one of the dropdown options, leave
          // the dropdown at its HTML default rather than silently coercing.
          if ([...poleSel.options].some(o => o.value === want)) poleSel.value = want;
        }
        const mavgEl = document.getElementById('mavg');
        if (mavgEl && info.defaults.mavg_count !== undefined) mavgEl.value = info.defaults.mavg_count;
        const toEl = document.getElementById('timeout_us');
        if (toEl && info.defaults.rpm_timeout_us !== undefined) toEl.value = info.defaults.rpm_timeout_us;
      }
    } catch (e) {
      // Help block stays usable with '?' placeholders. Other features unaffected.
    }
  }
  // Defer-safe: today the <script> is at end of <body> so the DOM is ready,
  // but guard against a future move into <head> that would race the queries.
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', loadDeviceInfo);
  } else {
    loadDeviceInfo();
  }

  // Language switcher — restore preference, wire <select>, apply default English.
  (() => {
    const sel = document.getElementById('lang-select');
    let stored = null;
    try { stored = localStorage.getItem(LANG_STORAGE_KEY); } catch (e) { /* ignore */ }
    const initial = (stored && I18N[stored]) ? stored : 'en';
    if (sel) sel.value = initial;
    applyLang(initial);
    if (sel) sel.addEventListener('change', () => applyLang(sel.value));
  })();

  // Single source of truth for "what's the other axis?" is each panel's
  // current value (kept in sync with device telemetry via setFromDevice).
  // No browser-side cache — eliminates the staleness window where a stale
  // freq=0 would poison a duty-only commit before the user ever changed freq.
  function sendPwm(freq, duty) {
    if (ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({ type: 'set_pwm', freq, duty }));
  }

  // mirrors components/pwm_gen/pwm_gen.c band table (v6.0: LO is 625 kHz)
  function dutyResolutionBits(freqHz) {
    const RES_HI = 10_000_000, RES_LO = 625_000;
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

    // Presets — load from storage or defaults; render slots; wire Apply + edit
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
      btn.textContent = 'Apply';
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
  // Forward-declared so each panel's onCommit can read the other panel's
  // current value at commit time (closure binds the let at call time, not at
  // declaration time). This keeps "the other axis" in lockstep with whatever
  // the device most recently reported via setFromDevice.
  let dutyPanel;
  let freqPanel;

  const freqResbits = document.getElementById('freq-resbits');

  dutyPanel = makePanel({
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
    onCommit: (v) => sendPwm(freqPanel.getValue(), v),
  });

  freqPanel = makePanel({
    kind: 'freq',
    axis: 'freq',
    range: { min: 10, max: 1_000_000 },
    defaultValue: 10000,
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
    onCommit: (v) => sendPwm(v, dutyPanel.getValue()),
    onValueChanged: (v) => {
      freqResbits.textContent = String(dutyResolutionBits(v));
    },
  });

  // ---------- GPIO panel ----------
  const MODE_LABELS = ['gpio_input_pulldown','gpio_input_pullup','gpio_input_floating','gpio_output'];
  const MODE_SHORT_TO_INT = { i_pd:0, i_pu:1, i_fl:2, o:3 };
  const MODE_INT_TO_WIRE = ['input_pulldown','input_pullup','input_floating','output'];

  function buildGpioRows(groupAPins, groupBPins) {
    const groupARoot = document.getElementById('gpio-group-a');
    const groupBRoot = document.getElementById('gpio-group-b');
    if (!groupARoot || !groupBRoot) return;
    [...groupAPins, ...groupBPins].forEach((gpioNum, idx) => {
      const grp = idx < groupAPins.length ? 'A' : 'B';
      const slot = idx < groupAPins.length ? (idx + 1) : (idx - groupAPins.length + 1);
      const row = document.createElement('div');
      row.className = 'gpio-row';
      row.dataset.idx = String(idx);
      row.innerHTML = `
        <span class="gpio-name">${grp}${slot}</span>
        <span class="gpio-pinnum">GPIO${gpioNum}</span>
        <select class="gpio-mode">
          <option value="input_pulldown" data-i18n="gpio_input_pulldown">input + pull-down</option>
          <option value="input_pullup"   data-i18n="gpio_input_pullup">input + pull-up</option>
          <option value="input_floating" data-i18n="gpio_input_floating">input + floating</option>
          <option value="output"         data-i18n="gpio_output">output</option>
        </select>
        <span class="gpio-tail">
          <span class="gpio-input-tail">
            <span data-i18n="gpio_value_label">value:</span>
            <span class="gpio-value">0</span>
          </span>
          <span class="gpio-output-tail" hidden>
            <label class="gpio-toggle">
              <input type="checkbox" class="gpio-level" />
              <span class="gpio-level-text">0</span>
            </label>
            <button type="button" class="gpio-pulse" data-i18n="gpio_pulse_btn">Pulse</button>
          </span>
        </span>
      `;
      (idx < groupAPins.length ? groupARoot : groupBRoot).appendChild(row);

      const modeSel = row.querySelector('.gpio-mode');
      modeSel.addEventListener('change', () => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: 'set_gpio_mode', idx, mode: modeSel.value }));
        }
      });

      const levelEl = row.querySelector('.gpio-level');
      levelEl.addEventListener('change', () => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: 'set_gpio_level', idx, level: levelEl.checked ? 1 : 0 }));
        }
      });

      const pulseBtn = row.querySelector('.gpio-pulse');
      pulseBtn.addEventListener('click', () => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: 'pulse_gpio', idx }));
        }
      });
    });
    // Re-apply current language to the freshly-rendered rows.
    const sel = document.getElementById('lang-select');
    if (sel) applyLang(sel.value);
  }

  function setGpioFromDevice(arr) {
    if (!Array.isArray(arr)) return;
    arr.forEach((entry, idx) => {
      const row = document.querySelector(`.gpio-row[data-idx="${idx}"]`);
      if (!row) return;
      const modeInt = MODE_SHORT_TO_INT[entry.m];
      if (modeInt === undefined) return;
      const wireMode = MODE_INT_TO_WIRE[modeInt];
      const modeSel = row.querySelector('.gpio-mode');
      // Don't fight a focused mode dropdown.
      if (document.activeElement !== modeSel && modeSel.value !== wireMode) {
        modeSel.value = wireMode;
      }
      const isOutput = (modeInt === 3);
      row.querySelector('.gpio-input-tail').hidden = isOutput;
      row.querySelector('.gpio-output-tail').hidden = !isOutput;
      row.querySelector('.gpio-value').textContent = entry.v ? '1' : '0';
      const levelEl = row.querySelector('.gpio-level');
      const levelTxt = row.querySelector('.gpio-level-text');
      if (document.activeElement !== levelEl) levelEl.checked = !!entry.v;
      levelTxt.textContent = entry.v ? '1' : '0';
      const pulsing = !!entry.p;
      row.classList.toggle('pulsing', pulsing);
      const pulseBtn = row.querySelector('.gpio-pulse');
      pulseBtn.disabled = pulsing;
      levelEl.disabled  = pulsing;
      modeSel.disabled  = pulsing;
      pulseBtn.textContent = pulsing ? t('gpio_pulsing') : t('gpio_pulse_btn');
    });
  }

  // ---------- Power switch ----------
  const powerBtn = document.getElementById('power_btn');
  function setPowerFromDevice(on) {
    if (!powerBtn) return;
    powerBtn.dataset.on = on ? '1' : '0';
    powerBtn.classList.toggle('on', !!on);
    const txtEl = powerBtn.querySelector('.txt');
    if (txtEl) txtEl.textContent = on ? t('power_on') : t('power_off');
  }
  if (powerBtn) {
    powerBtn.addEventListener('click', () => {
      const want = powerBtn.dataset.on === '1' ? 0 : 1;
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'set_power', on: want === 1 }));
      }
    });
  }

  // ---------- Pulse-width settings ----------
  const pulseWidthEl = document.getElementById('pulse-width-ms');
  function setPulseWidthFromDevice(ms) {
    if (!pulseWidthEl) return;
    // Same focus-protection pattern the freq/duty readouts use: only the
    // currently-typing user gets to keep their value; otherwise mirror the
    // device. This means a CLI/HID/CDC change to pulse_width_ms is reflected
    // here on the next telemetry tick.
    if (document.activeElement === pulseWidthEl) return;
    if (pulseWidthEl.value !== String(ms)) pulseWidthEl.value = String(ms);
  }
  const applyPulseBtn = document.getElementById('apply_pulse_width');
  if (applyPulseBtn) {
    applyPulseBtn.addEventListener('click', () => {
      if (!pulseWidthEl) return;
      const n = parseInt(pulseWidthEl.value, 10);
      if (!isFinite(n) || n < 1 || n > 10000) {
        pulseWidthEl.value = '100';
        return;
      }
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'set_pulse_width', width_ms: n }));
      }
    });
  }

  // ---------- WebSocket dispatch ----------
  ws.addEventListener('message', (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.type === 'status') {
        // setFromDevice updates each panel's `current` (read by getValue()),
        // so the next onCommit on the *other* axis automatically picks up
        // the latest device value. No separate cache to keep coherent.
        if (typeof msg.freq === 'number') freqPanel.setFromDevice(msg.freq);
        if (typeof msg.duty === 'number') dutyPanel.setFromDevice(msg.duty);
        if (typeof msg.rpm === 'number') setRpmFromDevice(msg.rpm);
        if (Array.isArray(msg.gpio)) setGpioFromDevice(msg.gpio);
        if (typeof msg.power === 'number') setPowerFromDevice(msg.power);
        if (typeof msg.pulse_width_ms === 'number') setPulseWidthFromDevice(msg.pulse_width_ms);
      } else if (msg.type === 'ack' && msg.op === 'factory_reset') {
        const fs = document.getElementById('factory_reset_status');
        if (fs) fs.textContent = t('factory_acked');
      }
    } catch (e) { /* ignore non-JSON / partial frames */ }
  });

  // ---------- RPM apply (existing contract) ----------
  // Inputs (#pole, #mavg, #timeout_us) now live in the unified Settings panel,
  // but the IDs and WS contract are unchanged.
  document.getElementById('apply_rpm').addEventListener('click', () => {
    const pole = parseInt(document.getElementById('pole').value, 10);
    const mavg = parseInt(document.getElementById('mavg').value, 10);
    const timeout_us = parseInt(document.getElementById('timeout_us').value, 10);
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'set_rpm', pole, mavg, timeout_us }));
    }
  });

  // ---------- OTA ----------
  document.getElementById('upload').addEventListener('click', async () => {
    const f = document.getElementById('fwfile').files[0];
    if (!f) return;
    const prog = document.getElementById('otaprog');
    const r = await fetch('/ota', { method: 'POST', body: f });
    if (r.ok) { prog.value = 100; alert(t('ota_accepted')); }
    else     { alert(t('ota_failed') + r.status); }
  });

  // ---------- Factory reset ----------
  const factoryBtn = document.getElementById('factory_reset');
  const factoryStatus = document.getElementById('factory_reset_status');
  factoryBtn.addEventListener('click', () => {
    const ok = confirm(t('factory_confirm'));
    if (!ok) return;
    factoryBtn.disabled = true;
    factoryStatus.textContent = t('factory_sending');
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'factory_reset' }));
    }
  });
})();
