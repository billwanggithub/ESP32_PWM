(() => {
  const ws = new WebSocket(`ws://${location.host}/ws`);
  const rpmSpan = document.getElementById('rpm');
  const appliedSpan = document.getElementById('applied');
  const canvas = document.getElementById('chart');
  const ctx = canvas.getContext('2d');

  const history = [];
  const MAX_POINTS = 300;

  function draw() {
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    if (history.length < 2) return;
    const maxV = Math.max(1, ...history);
    ctx.beginPath();
    history.forEach((v, i) => {
      const x = (i / (MAX_POINTS - 1)) * w;
      const y = h - (v / maxV) * (h - 4) - 2;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.strokeStyle = '#4af'; ctx.lineWidth = 1.5; ctx.stroke();
  }

  ws.addEventListener('message', (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.type === 'status') {
        rpmSpan.textContent = msg.rpm.toFixed(1);
        appliedSpan.textContent = `${msg.freq} Hz, ${msg.duty.toFixed(2)} %`;
        history.push(msg.rpm);
        if (history.length > MAX_POINTS) history.shift();
        draw();
      }
    } catch (e) { /* ignore */ }
  });

  document.getElementById('apply').addEventListener('click', () => {
    const freq = parseInt(document.getElementById('freq').value, 10);
    const duty = parseFloat(document.getElementById('duty').value);
    ws.send(JSON.stringify({ type: 'set_pwm', freq, duty }));
  });

  document.getElementById('apply_rpm').addEventListener('click', () => {
    const pole = parseInt(document.getElementById('pole').value, 10);
    const mavg = parseInt(document.getElementById('mavg').value, 10);
    const timeout_us = parseInt(document.getElementById('timeout_us').value, 10);
    ws.send(JSON.stringify({ type: 'set_rpm', pole, mavg, timeout_us }));
  });

  document.getElementById('upload').addEventListener('click', async () => {
    const f = document.getElementById('fwfile').files[0];
    if (!f) return;
    const prog = document.getElementById('otaprog');
    const r = await fetch('/ota', { method: 'POST', body: f });
    if (r.ok) { prog.value = 100; alert('OTA accepted; device will reboot.'); }
    else     { alert(`OTA failed: ${r.status}`); }
  });
})();
