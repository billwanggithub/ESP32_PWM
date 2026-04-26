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
      help_pin_power: 'Power-switch:',
      help_pin_group_a: 'GPIO Group A:',
      help_pin_group_b: 'GPIO Group B:',
      help_gpio_h: 'GPIO & Power',
      help_gpio_p: 'Group A defaults to input (pull-down) and Group B defaults to output (low). Each pin can be flipped between input and output at runtime; outputs support level toggle and a one-shot pulse of configurable width. Power switch toggles a GPIO whose polarity (active-high / active-low) is set at build time via Kconfig.',
      psu_title: 'Power Supply',
      psu_v_set: 'V set',
      psu_i_set: 'I set',
      psu_output: 'Output',
      psu_measured: 'measured',
      psu_off: 'OFF',
      psu_on: 'ON',
      psu_slave_lbl: 'Slave addr',
      psu_save: 'Save',
      psu_offline: 'PSU offline',
      psu_family_lbl: 'Family',
      psu_reboot: 'Reboot',
      psu_family_pending: 'reboot to apply',
      save: 'Save',
      announcer_h: 'IP Announcer (ntfy.sh)',
      announcer_enable: 'Enable IP push notifications',
      announcer_topic: 'Topic:',
      announcer_server: 'Server:',
      announcer_priority: 'Priority:',
      announcer_test: 'Send test now',
      announcer_save: 'Save',
      announcer_random: '🎲 Random',
      announcer_status_never: 'Never pushed.',
      announcer_status_ok: 'Pushed {ip}',
      announcer_status_failed: 'Failed: {err}',
      announcer_status_disabled: 'Disabled.',
      announcer_subscribe_h: '📲 Subscribe on phone:',
      announcer_open_in_app: 'Open in ntfy app',
      announcer_banner_placeholder: 'Topic looks like a placeholder — change it before enabling push.',
      announcer_help_h: 'IP Announcer (ntfy.sh push notifications)',
      announcer_help_p: 'When enabled, the device pushes its IP to ntfy.sh on every Wi-Fi connection. Install the ntfy app on your phone, subscribe to your topic, and tap the notification to open the dashboard. The topic name acts as a password — anyone with it can read your IP. Use a long, random topic.',
    },
    'zh-Hant': {
      app_title: 'Fan-TestKit 儀表板',
      language: '語言',
      settings: '設定',
      rpm: '轉速',
      pole_count: '極數',
      avg_window: '平均視窗',
      timeout_us: '逾時 (µs)',
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
      help_pin_power: '電源開關：',
      help_pin_group_a: 'GPIO A 組：',
      help_pin_group_b: 'GPIO B 組：',
      help_gpio_h: 'GPIO 與電源',
      help_gpio_p: 'A 組預設為輸入（下拉），B 組預設為輸出（低）。每隻 pin 可在執行時切換輸入 / 輸出；輸出模式支援切換準位以及單發脈衝（脈衝寬度可設）。電源開關控制一隻 GPIO，其 active-high / active-low 由 Kconfig 編譯時決定。',
      psu_title: '電源供應器',
      psu_v_set: '電壓設定',
      psu_i_set: '電流設定',
      psu_output: '輸出',
      psu_measured: '實測',
      psu_off: '關',
      psu_on: '開',
      psu_slave_lbl: '從機位址',
      psu_save: '儲存',
      psu_offline: 'PSU 離線',
      psu_family_lbl: '型號',
      psu_reboot: '重新開機',
      psu_family_pending: '重開後生效',
      save: '儲存',
      announcer_h: 'IP 通知 (ntfy.sh)',
      announcer_enable: '啟用 IP 推播',
      announcer_topic: 'Topic:',
      announcer_server: '伺服器:',
      announcer_priority: '優先順序:',
      announcer_test: '立即測試',
      announcer_save: '儲存',
      announcer_random: '🎲 隨機',
      announcer_status_never: '尚未推送過。',
      announcer_status_ok: '已推送 {ip}',
      announcer_status_failed: '失敗：{err}',
      announcer_status_disabled: '已停用。',
      announcer_subscribe_h: '📲 在手機上訂閱：',
      announcer_open_in_app: '在 ntfy app 開啟',
      announcer_banner_placeholder: 'Topic 看起來像 placeholder — 啟用推播前請先修改。',
      announcer_help_h: 'IP 通知 (ntfy.sh 推播)',
      announcer_help_p: '啟用後，每次 Wi-Fi 連線會把 IP 推送到 ntfy.sh。手機裝 ntfy app、訂閱你的 topic、點通知就能直接開 dashboard。Topic 等同密碼 — 任何知道它的人都能看到你推送的 IP，請用夠長夠隨機的字串。',
    },
    'zh-Hans': {
      app_title: 'Fan-TestKit 仪表板',
      language: '语言',
      settings: '设置',
      rpm: '转速',
      pole_count: '极数',
      avg_window: '平均窗口',
      timeout_us: '超时 (µs)',
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
      help_pin_power: '电源开关：',
      help_pin_group_a: 'GPIO A 组：',
      help_pin_group_b: 'GPIO B 组：',
      help_gpio_h: 'GPIO 与电源',
      help_gpio_p: 'A 组默认为输入（下拉），B 组默认为输出（低）。每个 pin 可在运行时切换输入 / 输出；输出模式支持切换电平以及单发脉冲（脉冲宽度可设）。电源开关控制一个 GPIO，其 active-high / active-low 由 Kconfig 编译时决定。',
      psu_title: '电源供应器',
      psu_v_set: '电压设定',
      psu_i_set: '电流设定',
      psu_output: '输出',
      psu_measured: '实测',
      psu_off: '关',
      psu_on: '开',
      psu_slave_lbl: '从机地址',
      psu_save: '保存',
      psu_offline: 'PSU 离线',
      psu_family_lbl: '型号',
      psu_reboot: '重新开机',
      psu_family_pending: '重启后生效',
      save: '保存',
      announcer_h: 'IP 通知 (ntfy.sh)',
      announcer_enable: '启用 IP 推送',
      announcer_topic: 'Topic:',
      announcer_server: '服务器:',
      announcer_priority: '优先级:',
      announcer_test: '立即测试',
      announcer_save: '保存',
      announcer_random: '🎲 随机',
      announcer_status_never: '尚未推送过。',
      announcer_status_ok: '已推送 {ip}',
      announcer_status_failed: '失败：{err}',
      announcer_status_disabled: '已停用。',
      announcer_subscribe_h: '📲 在手机上订阅：',
      announcer_open_in_app: '在 ntfy app 打开',
      announcer_banner_placeholder: 'Topic 看起来像占位符 — 启用推送前请先修改。',
      announcer_help_h: 'IP 通知 (ntfy.sh 推送)',
      announcer_help_p: '启用后，每次 Wi-Fi 连接会把 IP 推送到 ntfy.sh。手机装 ntfy app、订阅你的 topic、点通知就能直接打开 dashboard。Topic 等同密码 — 任何知道它的人都能看到你推送的 IP，请用够长够随机的字符串。',
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
      psuPanel.setRanges(info);
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
      sliderToValue, valueToSlider, formatReadout,
      onCommit, onValueChanged,
    } = opts;

    const root = document.getElementById(`${kind}-panel`);
    const readout = document.getElementById(`${kind}-readout`);
    const slider = document.getElementById(`${kind}-slider`);
    const stepInput = document.getElementById(`${kind}-step`);
    const finetuneRoot = document.getElementById(`${kind}-finetune`);
    const presetsRoot = document.getElementById(`${kind}-presets`);

    let current = defaultValue;

    // Pointer/keyboard interaction tracking for the slider.
    // Why: `document.activeElement === slider` is unreliable for mouse drag —
    // some browsers don't focus range inputs on pointerdown, so telemetry
    // would overwrite slider.value mid-drag and yank the thumb back.
    let interacting = false;

    // Optimistic UI gate. Commit-to-echo latency is 50–200 ms (WS RTT +
    // ctrl_cmd_queue drain + pwm_gen_set + next 20 Hz telemetry tick). The
    // 1–4 telemetry frames in that window still carry the OLD device value,
    // which would write the slider/readout back to the old value and cause
    // visible flicker between old and new — especially right after release
    // when the cursor is still on the slider. Suppress fromDevice writes
    // until either the device echoes a matching value or a timeout elapses
    // (covers the case where firmware clamps/rejects our commit and we'd
    // otherwise wait forever).
    let pendingValue = null;
    let pendingDeadline = 0;
    const PENDING_TIMEOUT_MS = 600;
    const PENDING_EPSILON = 0.05;  // duty=0.1% step, freq matches at integer

    // Step input wiring — local-only: updates slider.step immediately so the
    // user sees their typed value reflected. Persisting to device requires the
    // explicit Save button (outside makePanel). Steps from the server arrive
    // via applyStepFromServer() which writes stepInput.value + slider.step
    // directly.
    stepInput.addEventListener('change', () => {
      const n = parseFloat(stepInput.value);
      if (!isFinite(n) || n <= 0) {
        stepInput.value = slider.step || 1;
        return;
      }
      slider.step = String(n);
    });

    function setLocal(value, { commit = false, fromDevice = false } = {}) {
      const v = clamp(value, range.min, range.max);

      if (fromDevice) {
        // While a local commit is pending, ignore stale echoes that don't
        // match yet — they'd flicker the UI back to the old value. Accept
        // the moment the device echoes ≈ our committed value, OR after the
        // timeout (firmware may have clamped/rejected — accept reality).
        if (pendingValue !== null) {
          const now = performance.now();
          if (Math.abs(v - pendingValue) <= PENDING_EPSILON || now >= pendingDeadline) {
            pendingValue = null;
          } else {
            return; // suppress: don't touch current, slider, or readout
          }
        }
        // Don't fight an active drag either.
        if (interacting || document.activeElement === slider) return;
      }

      current = v;
      readout.value = formatReadout(v);
      slider.value = String(valueToSlider(v));
      if (onValueChanged) onValueChanged(v);

      if (commit && !fromDevice) {
        pendingValue = v;
        pendingDeadline = performance.now() + PENDING_TIMEOUT_MS;
        onCommit(v);
      }
    }

    // Slider — fires on release (via commitFromSlider below), drag uses
    // input for live readout. pointerdown + keydown/keyup + blur cover
    // mouse + keyboard. Touch events (touchstart/touchend/touchcancel)
    // are listened separately because mobile browsers (notably iOS
    // Safari) don't always fire matching pointer events for range
    // inputs, and may fire pointercancel mid-drag if the gesture is
    // ambiguous. We deliberately do NOT clear `interacting` on
    // pointercancel — that meant "browser hijacked the gesture" and
    // would reopen the suppression gate while the user is still touching.
    slider.addEventListener('pointerdown',  () => { interacting = true; });
    slider.addEventListener('keydown',      () => { interacting = true; });
    slider.addEventListener('keyup',        () => { interacting = false; });
    slider.addEventListener('blur',         () => { interacting = false; });
    slider.addEventListener('touchstart',   () => { interacting = true; }, { passive: true });
    slider.addEventListener('touchcancel',  () => { interacting = false; }, { passive: true });
    // pointerup + touchend clear `interacting` via commitFromSlider below.
    slider.addEventListener('input', () => {
      const v = sliderToValue(parseFloat(slider.value));
      readout.value = formatReadout(v);
      current = clamp(v, range.min, range.max);
      if (onValueChanged) onValueChanged(current);
    });

    // Commit on release. iOS Safari fires `change` on range inputs
    // inconsistently — sometimes not at all if the touch is interpreted
    // as a tap. So we also commit on pointerup and touchend. The
    // pendingValue gate dedupes back-to-back commits with the same value.
    const commitFromSlider = () => {
      interacting = false;
      const v = sliderToValue(parseFloat(slider.value));
      setLocal(v, { commit: true });
    };
    slider.addEventListener('change',    commitFromSlider);
    slider.addEventListener('pointerup', commitFromSlider);
    slider.addEventListener('touchend',  commitFromSlider);

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
  const Y_AXIS_MIN = 500;
  const Y_AXIS_STEP = 500;
  const Y_AXIS_HEADROOM = 1.1;
  let yAxisMax = Y_AXIS_MIN;
  let chartSized = false;

  function updateYTicks() {
    const yTicks = document.getElementById('y-ticks');
    if (!yTicks) return;
    const spans = yTicks.querySelectorAll('span');
    const labels = [yAxisMax, yAxisMax * 0.75, yAxisMax * 0.5, yAxisMax * 0.25, 0];
    labels.forEach((v, i) => { if (spans[i]) spans[i].textContent = String(Math.round(v)); });
  }

  function autoScaleYAxis() {
    let visibleMax = 0;
    for (const pt of rpmHistory) if (pt.rpm > visibleMax) visibleMax = pt.rpm;
    const target = Math.max(
      Y_AXIS_MIN,
      Math.ceil((visibleMax * Y_AXIS_HEADROOM) / Y_AXIS_STEP) * Y_AXIS_STEP,
    );
    if (target !== yAxisMax) {
      yAxisMax = target;
      updateYTicks();
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
    autoScaleYAxis();
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
  if (pulseWidthEl) {
    pulseWidthEl.addEventListener('change', () => {
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

  // ---------- Power Supply panel ----------
  const psuPanel = (() => {
    const panelEl    = document.getElementById('psu-panel');
    const modelEl    = document.getElementById('psu-model');
    const slaveEl    = document.getElementById('psu-slave');
    const linkEl     = document.getElementById('psu-link');
    const vSlider    = document.getElementById('psu-v-slider');
    const vInput     = document.getElementById('psu-v-input');
    const vOutEl     = document.getElementById('psu-v-out');
    const iSlider    = document.getElementById('psu-i-slider');
    const iInput     = document.getElementById('psu-i-input');
    const iOutEl     = document.getElementById('psu-i-out');
    const outOffBtn  = document.getElementById('psu-out-off');
    const outOnBtn   = document.getElementById('psu-out-on');
    const slaveInput = document.getElementById('psu-slave-input');
    const slaveBtn   = document.getElementById('psu-slave-save');
    const familySelect  = document.getElementById('psu-family-select');
    const familySave    = document.getElementById('psu-family-save');
    const familyPending = document.getElementById('psu-family-pending');
    const rebootBtn     = document.getElementById('psu-reboot');

    let liveFamily = null;       // last value seen on telemetry
    let pendingFamily = null;    // last value the user pushed

    if (!panelEl) {
      // HTML not present — defensive no-op so older builds don't crash.
      return { setRanges() {}, setFromDevice() {} };
    }

    const send = (msg) => {
      if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
    };

    const commitV = () => send({ type: 'set_psu_voltage', v: parseFloat(vInput.value) });
    const commitI = () => send({ type: 'set_psu_current', i: parseFloat(iInput.value) });

    // Slider drag → mirror to number input live (no send); commit on release/change.
    vSlider.addEventListener('input',  () => { vInput.value = vSlider.value; });
    vSlider.addEventListener('change', commitV);
    vInput .addEventListener('change', () => { vSlider.value = vInput.value; commitV(); });

    iSlider.addEventListener('input',  () => { iInput.value = iSlider.value; });
    iSlider.addEventListener('change', commitI);
    iInput .addEventListener('change', () => { iSlider.value = iInput.value; commitI(); });

    outOffBtn.addEventListener('click', () => send({ type: 'set_psu_output', on: false }));
    outOnBtn .addEventListener('click', () => send({ type: 'set_psu_output', on: true  }));

    slaveBtn.addEventListener('click', () => {
      const a = parseInt(slaveInput.value, 10);
      if (!(a >= 1 && a <= 255)) return;
      send({ type: 'set_psu_slave', addr: a });
    });

    if (familySave) {
      familySave.addEventListener('click', () => {
        const want = familySelect.value;
        send({ type: 'set_psu_family', family: want });
        pendingFamily = want;
        if (familyPending) {
          familyPending.textContent = t('psu_family_pending');
          familyPending.hidden = false;
        }
      });
    }

    if (rebootBtn) {
      rebootBtn.addEventListener('click', () => {
        if (!confirm('Reboot the device?')) return;
        send({ type: 'reboot' });
      });
    }

    // Don't yank the slider out from under the user during interaction.
    let userInteractingV = false, userInteractingI = false;
    [vSlider, vInput].forEach(el => {
      el.addEventListener('focus', () => userInteractingV = true);
      el.addEventListener('blur',  () => userInteractingV = false);
    });
    [iSlider, iInput].forEach(el => {
      el.addEventListener('focus', () => userInteractingI = true);
      el.addEventListener('blur',  () => userInteractingI = false);
    });

    return {
      setRanges(info) {
        if (typeof info.psu_v_max === 'number') {
          vSlider.max = info.psu_v_max;
          vInput.max  = info.psu_v_max;
        }
        if (typeof info.psu_i_max === 'number') {
          iSlider.max = info.psu_i_max;
          iInput.max  = info.psu_i_max;
        }
        if (info.psu_model_name) modelEl.textContent = info.psu_model_name;
      },
      setFromDevice(psu) {
        if (!psu) return;
        modelEl.textContent  = psu.model || '—';
        slaveEl.textContent  = (psu.slave != null) ? psu.slave : '—';
        linkEl.dataset.state = psu.link ? 'up' : 'down';
        panelEl.classList.toggle('psu-offline', !psu.link);
        if (!userInteractingV) {
          vSlider.value = psu.v_set;
          vInput.value  = psu.v_set;
        }
        if (!userInteractingI) {
          iSlider.value = psu.i_set;
          iInput.value  = psu.i_set;
        }
        vOutEl.textContent = (+psu.v_out).toFixed(2);
        iOutEl.textContent = (+psu.i_out).toFixed(3);
        outOnBtn .classList.toggle('active',  psu.output);
        outOffBtn.classList.toggle('active', !psu.output);
        if (psu.family) {
          liveFamily = psu.family;
          if (familySelect && document.activeElement !== familySelect) {
            familySelect.value = psu.family;
          }
          if (familyPending && pendingFamily && pendingFamily === psu.family) {
            familyPending.hidden = true;
            pendingFamily = null;
          }
        }
      },
    };
  })();

  // ---------- Step sizes: server-driven apply + Save button ----------
  // Step values arrive in the WS status frame as msg.ui.duty_step / freq_step.
  // This function updates both inputs and the slider.step properties so the
  // UI immediately reflects the device-persisted values after (re)connect.
  function applyStepFromServer(uiBlock) {
    if (!uiBlock) return;
    if (typeof uiBlock.duty_step === 'number' && uiBlock.duty_step > 0) {
      const dutyInput  = document.getElementById('duty-step');
      const dutySlider = document.getElementById('duty-slider');
      if (dutyInput)  dutyInput.value  = uiBlock.duty_step;
      if (dutySlider) dutySlider.step  = String(uiBlock.duty_step);
    }
    if (typeof uiBlock.freq_step === 'number' && uiBlock.freq_step > 0) {
      const freqInput  = document.getElementById('freq-step');
      const freqSlider = document.getElementById('freq-slider');
      if (freqInput)  freqInput.value  = uiBlock.freq_step;
      if (freqSlider) freqSlider.step  = String(uiBlock.freq_step);
    }
  }

  // Unified Save — commits ALL NVS-backed settings in one click.
  // Sends four save commands back-to-back; ack-handler shows aggregate
  // success only after all four acks return ok=true.
  (() => {
    const btn      = document.getElementById('save-all-btn');
    const statusEl = document.getElementById('save-all-status');
    if (!btn) return;
    let pending = 0;
    let allOk   = true;
    function setStatus(text) {
      if (!statusEl) return;
      statusEl.textContent = text;
      if (text) setTimeout(() => { if (statusEl.textContent === text) statusEl.textContent = ''; }, 3000);
    }
    btn.addEventListener('click', () => {
      if (ws.readyState !== WebSocket.OPEN) {
        setStatus('Not connected');
        return;
      }
      const duty = parseFloat(document.getElementById('duty-step').value);
      const freq = parseInt(document.getElementById('freq-step').value, 10);
      if (!(duty > 0) || !(freq > 0)) {
        setStatus('Invalid step sizes');
        return;
      }
      pending = 4;
      allOk   = true;
      setStatus('Saving...');
      ws.send(JSON.stringify({ type: 'save_rpm_params' }));
      ws.send(JSON.stringify({ type: 'save_rpm_timeout' }));
      ws.send(JSON.stringify({ type: 'save_pwm_freq' }));
      ws.send(JSON.stringify({ type: 'save_ui_steps', duty_step: duty, freq_step: freq }));
    });
    // Expose for the ack-handler to count down.
    window.__saveAllAck = (op, ok) => {
      if (pending <= 0) return;
      if (!ok) allOk = false;
      pending--;
      if (pending === 0) setStatus(allOk ? 'Saved' : 'Failed');
    };
  })();

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
        if (msg.psu) psuPanel.setFromDevice(msg.psu);
        applyStepFromServer(msg.ui);
        // announcer block from status frame
        if (msg.announcer) {
          applyAnnouncerStatus(msg.announcer);
        }
      } else if (msg.type === 'ack') {
        if (msg.op === 'factory_reset') {
          const fs = document.getElementById('factory_reset_status');
          if (fs) fs.textContent = t('factory_acked');
        } else if (msg.op === 'save_rpm_params' || msg.op === 'save_rpm_timeout' ||
                   msg.op === 'save_pwm_freq'   || msg.op === 'save_ui_steps') {
          if (window.__saveAllAck) window.__saveAllAck(msg.op, !!msg.ok);
        }
      }
    } catch (e) { /* ignore non-JSON / partial frames */ }
  });

  // ---------- RPM apply on change ----------
  // Inputs (#pole, #mavg, #timeout_us) live in the unified Settings panel.
  // Each value applies live (set_rpm) on change; persistence happens via
  // the unified Save button below.
  function sendSetRpm() {
    if (ws.readyState !== WebSocket.OPEN) return;
    const pole = parseInt(document.getElementById('pole').value, 10);
    const mavg = parseInt(document.getElementById('mavg').value, 10);
    const timeout_us = parseInt(document.getElementById('timeout_us').value, 10);
    ws.send(JSON.stringify({ type: 'set_rpm', pole, mavg, timeout_us }));
  }
  document.getElementById('pole').addEventListener('change', sendSetRpm);
  document.getElementById('mavg').addEventListener('change', sendSetRpm);
  document.getElementById('timeout_us').addEventListener('change', sendSetRpm);

  // ---------- OTA ----------
  document.getElementById('upload').addEventListener('click', async () => {
    const f = document.getElementById('fwfile').files[0];
    if (!f) return;
    const prog = document.getElementById('otaprog');
    const r = await fetch('/ota', { method: 'POST', body: f });
    if (r.ok) { prog.value = 100; alert(t('ota_accepted')); }
    else     { alert(t('ota_failed') + r.status); }
  });

  // ---------- IP Announcer panel ----------
  // Mirror of topic_is_safe() in components/ip_announcer/ip_announcer_push.c —
  // keep the rules in sync (len < 16, CHANGE-ME-*, fan-testkit-CHANGE*).
  function announcerTopicLooksSafe(topic) {
    if (!topic) return false;
    if (topic.length < 16) return false;
    const lower = topic.toLowerCase();
    if (lower.startsWith('change-me-')) return false;
    if (lower.startsWith('fan-testkit-change')) return false;
    return true;
  }

  function applyAnnouncerStatus(a) {
    // Only repopulate inputs when the user is NOT actively editing them.
    const topicInput = document.getElementById('announcer-topic');
    const serverInput = document.getElementById('announcer-server');
    const priorityInput = document.getElementById('announcer-priority');
    const enableInput = document.getElementById('announcer-enable');
    if (document.activeElement !== topicInput) topicInput.value = a.topic || '';
    if (document.activeElement !== serverInput) serverInput.value = a.server || 'ntfy.sh';
    if (document.activeElement !== priorityInput) priorityInput.value = String(a.priority || 3);
    enableInput.checked = !!a.enable;

    const banner = document.getElementById('announcer-banner');
    banner.hidden = announcerTopicLooksSafe(a.topic);

    const statusEl = document.getElementById('announcer-status');
    if (a.status === 'ok') {
      statusEl.textContent = t('announcer_status_ok')
        .replace('{ip}', a.last_pushed_ip || '?');
      statusEl.style.color = 'green';
    } else if (a.status === 'failed') {
      statusEl.textContent = t('announcer_status_failed')
        .replace('{err}', a.last_err || '?');
      statusEl.style.color = 'red';
    } else if (a.status === 'disabled') {
      statusEl.textContent = t('announcer_status_disabled');
      statusEl.style.color = '';
    } else {
      statusEl.textContent = t('announcer_status_never');
      statusEl.style.color = '';
    }

    const dl = document.getElementById('announcer-deeplink');
    dl.href = `ntfy://${a.server || 'ntfy.sh'}/${a.topic || ''}?subscribe=1`;

    // Disable Test button when topic is unsafe.
    document.getElementById('announcer-test').disabled =
      !announcerTopicLooksSafe(a.topic);
  }

  document.getElementById('announcer-randomize').addEventListener('click', () => {
    const alphabet = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
    let tok = '';
    const arr = new Uint32Array(32);
    crypto.getRandomValues(arr);
    for (let i = 0; i < 32; i++) tok += alphabet[arr[i] % alphabet.length];
    document.getElementById('announcer-topic').value = `fan-testkit-${tok}`;
  });

  document.getElementById('announcer-save').addEventListener('click', () => {
    if (ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({
      type: 'set_announcer',
      enable: document.getElementById('announcer-enable').checked,
      topic: document.getElementById('announcer-topic').value.trim(),
      server: document.getElementById('announcer-server').value.trim() || 'ntfy.sh',
      priority: parseInt(document.getElementById('announcer-priority').value, 10) || 3,
    }));
  });

  document.getElementById('announcer-test').addEventListener('click', () => {
    if (ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({ type: 'test_announcer' }));
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
