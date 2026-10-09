'use strict';
// Wi-Fi: the btbench-wifi state machine. Self-contained on purpose: /wifi loads this file alone
// (with style.css) as the captive page a phone shows in AP mode, where nothing else of the console
// is needed and every request costs. In the console it is mounted into the Wi-Fi tab.
//
// Adding a network or changing the mode may take down the link the page came over (the setup
// AP): the daemon answers at once and does it in the background, so the page says what will happen
// rather than waiting for an answer that cannot arrive.

function mountWifi(root, opts) {
  opts = opts || {};
  const e = s => String(s == null ? '' : s).replace(/[&<>"']/g,
    c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
  const call = (method, path, body) => fetch('/api/wifi' + path, {
    method, headers: body ? {'Content-Type': 'application/json'} : {}, body: body ? JSON.stringify(body) : undefined,
  }).then(async r => {
    let j = {};
    try { j = await r.json(); } catch (x) { /* empty */ }
    if (!r.ok) { const err = new Error(j.error || r.statusText); err.status = r.status; throw err; }
    return j;
  });

  root.innerHTML = `
    <div class="card"><div class="row between"><h3>Wi-Fi</h3><span class="pill" data-w="state">…</span></div>
      <div class="kv small" data-w="kv"></div>
      <div class="small" data-w="msg"></div>
      <div class="row wrap" style="margin-top:.6rem">Mode
        <button data-mode="auto">auto</button><button data-mode="sta">client</button>
        <button data-mode="ap">setup AP</button><button data-mode="off">off</button></div></div>
    <div class="card"><div class="row between"><h3>Networks in range</h3><button data-w="scan">Scan</button></div>
      <div data-w="scanlist" class="small muted">Press Scan.</div></div>
    <div class="card"><h3>Add a network</h3>
      <form data-w="add" class="grid">
        <label>SSID <input name="ssid" required maxlength="32" autocomplete="off"></label>
        <label>Password <input name="psk" type="password" maxlength="64" placeholder="empty for an open network"></label>
        <div class="row"><button class="primary" type="submit">Join</button></div>
      </form></div>
    <div class="card"><h3>Saved networks</h3><div data-w="saved" class="small"></div></div>`;
  const q = n => root.querySelector(`[data-w="${n}"]`);
  const say = (t, bad) => { q('msg').textContent = t; q('msg').className = 'small ' + (bad ? 'bad-text' : ''); };

  let last = '';
  function status() {
    return call('GET', '').then(s => {
      const key = JSON.stringify(s);
      if (key === last) return;
      last = key;
      const st = q('state');
      st.textContent = s.state + (s.policy && s.policy !== 'auto' ? ` (${s.policy})` : '');
      st.className = 'pill ' + (s.state === 'sta' ? 'good' : s.state === 'ap' ? 'warn' : s.state === 'connecting' ? 'warn' : '');
      const rows = [['Network', s.ssid || '—'], ['Address', s.ip || '—']];
      if (s.ap) rows.push(['Setup AP', `${s.ap.ssid} ${s.ap.ip}`]);
      if (s.last_error) rows.push(['Last error', s.last_error]);
      if (s.op) rows.push(['Last change', `${s.op.op}: ${s.op.running ? 'running…' : s.op.ok ? 'done' : 'failed: ' + s.op.error}`]);
      q('kv').innerHTML = rows.map(r => `<span>${e(r[0])}</span><span>${e(r[1])}</span>`).join('');
      q('saved').innerHTML = (s.networks || []).map(n => `<div class="net"><span class="ssid">${e(n)}</span>
        <button class="small-btn danger" data-rm="${e(n)}">remove</button></div>`).join('') || '<span class="muted">none</span>';
      root.querySelectorAll('button[data-mode]').forEach(b => { b.disabled = b.dataset.mode === s.policy; });
    }).catch(err => {
      q('state').textContent = err.status === 503 ? 'not here' : 'error';
      say(err.status === 503 ? 'Wi-Fi is managed on the board only (btbench-wifi is not installed on this machine).' : err.message, err.status !== 503);
    });
  }

  q('scan').onclick = () => {
    q('scanlist').textContent = 'scanning…';
    call('GET', '/scan').then(list => {
      list.sort((a, b) => b.signal_dbm - a.signal_dbm);
      q('scanlist').innerHTML = list.map(n => `<div class="net"><span class="ssid">${e(n.ssid || '(hidden)')}</span>
        <span class="muted">${n.signal_dbm} dBm · ${e(n.security)}</span>
        <button class="small-btn" data-pick="${e(n.ssid)}">use</button></div>`).join('') || 'nothing in range';
    }).catch(err => { q('scanlist').textContent = err.message; });
  };
  q('scanlist').onclick = ev => {
    const b = ev.target.closest('button[data-pick]');
    if (b) {
      q('add').elements.ssid.value = b.dataset.pick;
      q('add').elements.psk.focus();
    }
  };
  q('add').onsubmit = ev => {
    ev.preventDefault();
    const f = ev.target;
    const body = {ssid: f.elements.ssid.value};
    if (f.elements.psk.value) body.psk = f.elements.psk.value;
    call('POST', '/networks', body).then(() => {
      f.elements.psk.value = '';
      say(`Joining ${body.ssid}. If this page was reached over the setup AP, it will drop now: ` +
        `reconnect your device to ${body.ssid} and open the board by its name (.local). ` +
        `If joining fails the board comes back as the setup AP.`);
    }).catch(err => say(err.message, true));
  };
  root.addEventListener('click', ev => {
    const m = ev.target.closest('button[data-mode]');
    const rm = ev.target.closest('button[data-rm]');
    if (m) {
      const mode = m.dataset.mode;
      if (mode !== 'auto' && !confirm(`Switch Wi-Fi to ${mode}? The board may drop off this network.`)) return;
      call('PUT', '/mode', {mode}).then(() => say(`Switching to ${mode}…`)).catch(err => say(err.message, true));
    } else if (rm) {
      if (!confirm(`Forget ${rm.dataset.rm}?`)) return;
      call('DELETE', '/networks?ssid=' + encodeURIComponent(rm.dataset.rm)).then(() => { last = ''; status(); })
        .catch(err => say(err.message, true));
    }
  });

  let timer = null;
  return {
    start() { status(); clearInterval(timer); timer = setInterval(status, opts.period || 3000); },
    stop() { clearInterval(timer); timer = null; },
  };
}

// In the console: a tab like the others. On /wifi there is no registerTab, and wifi.html mounts it.
if (typeof registerTab === 'function') {
  let wifi = null;
  registerTab('wifi', {
    show() {
      if (!wifi) wifi = mountWifi(document.getElementById('wifi-root'));
      wifi.start();
    },
    hide() { if (wifi) wifi.stop(); },
  });
}
