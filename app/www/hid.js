'use strict';
// HID over GATT both ways. Remote (LE → Remote control): the board as a Bluetooth LE remote — turn
// it on, pair it from a phone, TV or PC like any remote, then press its keys here (held keys stay
// down while the pointer is; a physical keyboard drives the pad while it has focus).
// Input devices (Devices → Input devices): keyboards, remotes and mice connected to the board, what
// their report maps declare, and their key presses live; optionally their media keys drive the
// board's player.

const hid = {remote: null, host: null, poll: null, held: new Set()};

// ---------------------------------------------------------------- the remote

const REMOTE_SVG = {
  power: 'M12 3v8M6.3 6.3a8 8 0 1011.4 0',
  mute: 'M4 9v6h4l5 4V5L8 9zM17 9l4 6M21 9l-4 6',
  up: 'M6 15l6-6 6 6', down: 'M6 9l6 6 6-6', left: 'M15 6l-6 6 6 6', right: 'M9 6l6 6-6 6',
  back: 'M9 14L4 9l5-5M4 9h11a5 5 0 010 10h-3',
  home: 'M4 11l8-7 8 7v9h-5v-6H9v6H4z',
  menu: 'M4 7h16M4 12h16M4 17h16',
  volume_up: 'M12 5v14M5 12h14', volume_down: 'M5 12h14',
  previous: 'M18 6l-8.5 6L18 18zM6 6h2v12H6z', next: 'M6 6l8.5 6L6 18zM16 6h2v12h-2z',
  play_pause: 'M5 5v14l8-7zM14 5h2v14h-2zM18 5h2v14h-2z',
  search: 'M10.5 17a6.5 6.5 0 100-13 6.5 6.5 0 000 13zM20 20l-4.8-4.8',
};
const rkey = (key, label, cls) => `<button class="rk ${cls || ''}" data-key="${key}" title="${label}" aria-label="${label}">
  ${REMOTE_SVG[key] ? `<svg viewBox="0 0 24 24"><path d="${REMOTE_SVG[key]}"/></svg>` : esc(label)}</button>`;

function buildPad() {
  setHTML($('rm-pad'), `
    <div class="rk-row">${rkey('power', 'Power', 'round danger-key')}<span></span>${rkey('mute', 'Mute', 'round')}</div>
    <div class="dpad">
      ${rkey('up', 'Up', 'd-up')}${rkey('left', 'Left', 'd-left')}
      <button class="rk d-ok" data-key="enter" title="OK" aria-label="OK">OK</button>
      ${rkey('right', 'Right', 'd-right')}${rkey('down', 'Down', 'd-down')}
    </div>
    <div class="rk-row">${rkey('back', 'Back', 'round')}${rkey('home', 'Home', 'round')}${rkey('menu', 'Menu', 'round')}</div>
    <div class="rk-row rk-media">${rkey('previous', 'Previous')}${rkey('play_pause', 'Play / pause', 'wide')}${rkey('next', 'Next')}</div>
    <div class="rk-vol">
      <div class="rocker">${rkey('volume_up', 'Volume up')}<span>VOL</span>${rkey('volume_down', 'Volume down')}</div>
      <div class="rocker">${rkey('search', 'Search')}<span>FIND</span></div>
    </div>`);
}

function renderRemote(s) {
  hid.remote = s;
  const on = s.enabled;
  if (idle($('rm-on'))) $('rm-on').checked = on;
  if (idle($('rm-name'))) $('rm-name').value = s.name;
  const sub = s.subscribed && (s.subscribed.consumer || s.subscribed.keyboard);
  const adv = s.advertisement;
  let st = 'off', cls = '';
  if (on && sub) { st = 'connected'; cls = 'good'; }
  else if (on && adv && adv.state === 'active') { st = 'waiting for a host'; cls = 'warn'; }
  else if (on) { st = (adv && adv.state) || s.gatt.state; cls = 'warn'; }
  setText($('rm-state'), st);
  setClass($('rm-state'), 'pill ' + cls);
  setText($('rm-hint'), !on ? 'Turn it on, then pair “' + s.name + '” from the phone, TV or computer’s Bluetooth settings.'
    : sub ? 'A host is listening: the keys below reach it.'
    : 'Advertising as “' + s.name + '”. Pair it from the host’s Bluetooth settings (the board answers the pairing by itself with the agent on automatic).');
  $('rm-pad').classList.toggle('off', !on);
  $('rm-type').disabled = $('rm-send').disabled = !on;
  if (idle($('rm-batt'))) $('rm-batt').value = s.battery;
  setText($('rm-batt-v'), s.battery + ' %');
  setText($('rm-err'), (s.gatt && s.gatt.error) || (adv && adv.error) || '');
}

const refreshRemote = () => api('/le/hid').then(renderRemote).catch(e => setText($('rm-err'), e.message));

function sendKey(key, action) {
  return post('/le/hid/key', {key, action}).catch(e => { if (action !== 'up') fail(e); });
}

// Keys held down: released when the pointer lets go, leaves, or the page loses focus — a stuck
// volume key on a TV is the one thing a remote must never do.
function releaseAll() {
  for (const k of hid.held) sendKey(k, 'up');
  hid.held.clear();
  document.querySelectorAll('.rk.down').forEach(b => b.classList.remove('down'));
}

function setupRemote() {
  buildPad();
  const pad = $('rm-pad');
  pad.addEventListener('pointerdown', e => {
    const b = e.target.closest('.rk');
    if (!b || pad.classList.contains('off')) return;
    e.preventDefault();
    b.setPointerCapture(e.pointerId);
    b.classList.add('down');
    hid.held.add(b.dataset.key);
    sendKey(b.dataset.key, 'down');
  });
  const up = e => {
    const b = e.target.closest('.rk');
    if (!b || !hid.held.has(b.dataset.key)) return;
    b.classList.remove('down');
    hid.held.delete(b.dataset.key);
    sendKey(b.dataset.key, 'up');
  };
  pad.addEventListener('pointerup', up);
  pad.addEventListener('pointercancel', up);
  window.addEventListener('blur', releaseAll);
  // The pad has focus: arrows, Enter, Escape and Backspace are the remote's.
  const KB = {ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right', Enter: 'enter', Escape: 'back',
    Backspace: 'back', ' ': 'play_pause', '+': 'volume_up', '-': 'volume_down', m: 'mute', h: 'home'};
  pad.addEventListener('keydown', e => {
    const k = KB[e.key];
    if (!k || pad.classList.contains('off')) return;
    e.preventDefault();
    if (e.repeat) return;
    hid.held.add(k);
    sendKey(k, 'down');
    const b = pad.querySelector(`[data-key="${k}"]`);
    if (b) b.classList.add('down');
  });
  pad.addEventListener('keyup', e => {
    const k = KB[e.key];
    if (!k || !hid.held.has(k)) return;
    hid.held.delete(k);
    sendKey(k, 'up');
    const b = pad.querySelector(`[data-key="${k}"]`);
    if (b) b.classList.remove('down');
  });
  $('rm-on').onchange = e => {
    setText($('rm-state'), e.target.checked ? 'starting…' : 'stopping…');
    put('/le/hid', {enabled: e.target.checked, name: $('rm-name').value.trim()}).then(renderRemote)
      .catch(err => { fail(err); refreshRemote(); });
  };
  $('rm-name').onchange = () => {
    if (hid.remote && hid.remote.enabled) put('/le/hid', {enabled: true, name: $('rm-name').value.trim()}).then(renderRemote).catch(fail);
  };
  const type = () => {
    const t = $('rm-type').value;
    if (!t) return;
    post('/le/hid/type', {text: t}).then(() => { $('rm-type').value = ''; }).catch(fail);
  };
  $('rm-send').onclick = type;
  $('rm-type').onkeydown = e => { if (e.key === 'Enter') type(); };
  $('rm-batt').onchange = e => put('/le/hid', {battery: parseInt(e.target.value, 10)}).then(renderRemote).catch(fail);
  $('rm-batt').oninput = e => setText($('rm-batt-v'), e.target.value + ' %');
}

registerTab('remote', {
  show() {
    refreshRemote();
    clearInterval(hid.poll);
    // Whether a host subscribed changes on its own: a slow look while the view is open.
    hid.poll = setInterval(refreshRemote, 2000);
  },
  hide() { clearInterval(hid.poll); hid.poll = null; releaseAll(); },
});

// ---------------------------------------------------------------- input devices (the host side)

function renderHost(s) {
  hid.host = s;
  if (idle($('hh-media'))) $('hh-media').checked = s.media_keys;
  setHTML($('hh-devices'), s.devices.length ? s.devices.map(d => {
    const apps = ((d.summary || {}).applications || []).map(a => tag(a.name || `page ${a.page} usage ${a.usage}`)).join(' ');
    const reports = ((d.summary || {}).reports || []).map(r => `<tr><td>${r.id || '—'}</td><td>${r.input_bits}</td><td>${r.output_bits}</td><td>${r.feature_bits}</td></tr>`).join('');
    return `<div class="card">
      <div class="card-head"><h3>${esc(d.address ? devName(d.address) : d.name)} <span class="small muted mono">${esc(d.address)}</span></h3>
        <div class="acts">${apps}</div></div>
      <div class="small muted">${d.nodes.map(n => esc(n.name) + ` <span class="mono">${esc(n.event)}${n.hidraw ? ' · ' + esc(n.hidraw) : ''}</span>`).join('<br>')}</div>
      ${d.report_descriptor ? `<details class="more-fields"><summary>Report map (${d.summary.bytes} bytes)</summary>
        <table class="tbl small" style="margin:.5rem 0"><tr><th>Report</th><th>Input bits</th><th>Output bits</th><th>Feature bits</th></tr>${reports}</table>
        <pre class="log mono">${esc(d.report_descriptor.match(/.{1,2}/g).join(' '))}</pre>
        <div class="small muted">USB IDs ${d.vendor.toString(16).padStart(4, '0')}:${d.product.toString(16).padStart(4, '0')} · ${esc(d.hid)}</div></details>` : ''}
    </div>`;
  }).join('') : `<div class="card empty-card"><h3>No input device connected</h3>
      <p class="hint">Pair a Bluetooth keyboard, remote, mouse or game pad in Devices. BlueZ turns it into an input device on the board, and it shows here with what it sends.</p></div>`);
  if (!hid.logged) {
    hid.logged = true;
    s.recent.forEach(hostEvent);
  }
}

function hostEvent(e) {
  const t = e.type === 'key' ? (e.value === 1 ? 'down' : e.value === 0 ? 'up' : 'repeat') : e.value;
  logLine($('hh-log'), `${fmtTime(e.ts)}  ${devName(e.address)}  ${e.name.padEnd(18)} ${t}`);
  if (e.type === 'key' && e.value === 1) {
    setHTML($('hh-last'), `<span class="keycap">${esc(e.name.replace(/^(KEY|BTN)_/, ''))}</span> <span class="muted small">from ${esc(devName(e.address))}</span>`);
  }
}

const refreshHost = () => api('/hid').then(renderHost).catch(e => toast(e.message));

function setupHost() {
  $('hh-media').onchange = e => put('/hid', {media_keys: e.target.checked}).then(renderHost).catch(fail);
  $('hh-clear').onclick = () => logClear($('hh-log'));
}

registerTab('hid', {
  topics: () => ['hid.input', 'bt'],
  show() {
    hid.logged = false;
    refreshHost();
    api('/bluetooth').then(b => { if (!btState) btState = b; }).catch(() => {});
  },
  onMessage(topic, d) {
    if (currentTab !== 'hid') return;
    if (topic === 'hid.input') hostEvent(d);
    // A device that connects or goes: its input devices come and go with it.
    else if (topic === 'bt') {
      // At most one look each 1.5 s: "bt" also carries every RSSI change during a scan.
      if (!hid.hostT) hid.hostT = setTimeout(() => { hid.hostT = null; refreshHost(); }, 1500);
    }
  },
});

setupRemote();
setupHost();
