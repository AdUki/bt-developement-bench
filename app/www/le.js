'use strict';
// LE: what advertisers in range say, a GATT explorer over the BlueZ client, LE advertising from a
// form, and a local GATT server from JSON. The device list comes from the same "bt" frames the
// Devices view uses; notifications and the server's events have topics of their own.

// LE devices are the ones that advertise something: a random address, data, or flags.
const isLe = d => d.address_type === 'random' || d.advertising_flags != null ||
  Object.keys(d.manufacturer_data).length || Object.keys(d.service_data).length;

function leOnBt(b) {
  if (currentTab !== 'le' || !b) return;
  const les = b.devices.filter(isLe);
  // A row an advertiser: name, signal and what it advertises in words; the raw bytes below.
  setHTML($('le-adv-view'), les.length ? les.slice().sort((x, y) => (y.rssi == null ? -999 : y.rssi) - (x.rssi == null ? -999 : x.rssi)).map(d => {
    const raw = Object.entries(d.manufacturer_data).map(([k, v]) => `<span>Manufacturer ${esc(k)}</span><span class="mono">${esc(v)}</span>`).join('') +
      Object.entries(d.service_data).map(([k, v]) => `<span>Service ${esc(k)}</span><span class="mono">${esc(v)}</span>`).join('') +
      (d.advertising_flags ? `<span>Flags</span><span class="mono">${esc(d.advertising_flags)}</span>` : '');
    return `<div class="rw">
      <div class="rw-main">
        <span class="rw-led${d.connected ? ' on' : ''}"></span>
        <div class="rw-title"><b>${esc(d.alias || d.address)}</b><span class="rw-sub">
          <span class="mono">${d.address}</span>
          ${d.appearance_name ? `<span>${esc(d.appearance_name)}</span>` : ''}
          ${d.uuids.length ? `<span>${esc(d.uuids.map(u => u.name || u.uuid).join(', '))}</span>` : ''}</span></div>
        <div class="rw-acts small">${d.rssi != null ? `<span class="mono">${d.rssi} dBm</span>` : ''}${d.tx_power != null ? `<span class="muted">tx ${d.tx_power}</span>` : ''}</div>
      </div>
      ${raw ? `<div class="rw-body"><div class="kv">${raw}</div></div>` : ''}
    </div>`;
  }).join('')
    : '<div class="empty">No advertisers seen. Scan from Devices (LE, or LE and classic) and they show here.</div>');

  // The explorer offers every device, connected ones first.
  const sel = $('gx-dev');
  const opts = b.devices.slice().sort((x, y) => (y.connected - x.connected))
    .map(d => `<option value="${d.address}">${esc(d.alias || d.address)}${d.connected ? ' · connected' : ''}</option>`).join('');
  if (idle(sel)) {
    const keep = sel.value;
    setHTML(sel, opts);
    if (keep) sel.value = keep;
  }
  // The first time, the explorer follows the selection by itself (a connected device first).
  if (!gx.addr && sel.value) gx.addr = sel.value;
  const cur = b.devices.find(d => d.address === sel.value);
  // A dual-mode device (a speaker, a phone) connected over classic alone has no LE GATT to show:
  // the explorer connects and disconnects its LE bearer, not the whole device.
  const le = gattBearer(cur);
  const on = cur && (le ? cur.bearers.le.connected : cur.connected);
  setText($('gx-state'), cur ? (on ? (cur.services_resolved ? '' : 'Connected, resolving services…')
    : le && cur.connected ? 'Connected over classic only: Connect LE to read its LE services.'
    : 'Not connected: Connect to read its services.') + (cur.busy ? ' · ' + cur.busy : '') + (cur.error ? ' · ' + cur.error : '') : '');
  setText($('gx-connect'), `${on ? 'Disconnect' : 'Connect'}${le ? ' LE' : ''}`);
  // A device that just resolved its services gets its tree without another click.
  if (cur && cur.services_resolved && gx.addr === cur.address && !gx.loaded) loadGatt();
}

// ---------------------------------------------------------------- GATT explorer

const gx = {addr: '', loaded: false, tree: null, open: new Set()};

// "le" for a device seen on both transports, whose LE bearer the explorer drives; null otherwise.
const gattBearer = d => d && d.bearers && d.bearers.le && d.bearers.bredr ? 'le' : null;
const quoted = t => t ? '“' + t + '”' : '';

function charRow(c) {
  const f = c.flags;
  const can = x => f.includes(x);
  const h = '0x' + c.handle.toString(16).padStart(4, '0');
  const acts = [];
  if (can('read')) acts.push(`<button data-g="read" data-h="${c.handle}">Read</button>`);
  if (can('write') || can('write-without-response') || can('reliable-write')) {
    acts.push(`<input class="g-val mono" data-h="${c.handle}" placeholder="hex, or text:…" size="14">`,
      `<select class="g-type" data-h="${c.handle}">${['request', 'command', 'reliable']
        .filter(t => t === 'request' ? can('write') : t === 'command' ? can('write-without-response') : can('reliable-write'))
        .map(t => `<option>${t}</option>`).join('')}</select>`,
      `<button data-g="write" data-h="${c.handle}">Write</button>`);
  }
  if (can('notify') || can('indicate'))
    acts.push(`<button data-g="notify" data-h="${c.handle}" data-on="${c.notifying ? 0 : 1}">${c.notifying ? 'Stop notify' : 'Notify'}</button>`);
  const descs = c.descriptors.map(d => `<div class="gatt-dsc"><span class="mono">0x${d.handle.toString(16).padStart(4, '0')}</span>
      ${esc(d.name || d.uuid)} <span class="mono" id="gv-${d.handle}">${esc(d.value || '')}</span> <span id="gt-${d.handle}">${esc(quoted(d.text))}</span>
      <button class="small-btn" data-g="read" data-h="${d.handle}">Read</button></div>`).join('');
  return `<div class="gatt-chr">
    <div class="info"><div><span class="mono">${h}</span> <b>${esc(c.name || '')}</b> <span class="mono small muted">${esc(c.uuid)}</span>
      <span class="small muted">${esc(f.join(', '))}</span></div>
    <div class="small val"><span class="mono" id="gv-${c.handle}">${esc(c.value == null ? '—' : c.value || '(empty)')}</span>
      <span id="gt-${c.handle}">${esc(quoted(c.text))}</span></div></div>
    <div class="acts">${acts.join('')}</div>${descs}</div>`;
}

function renderGatt() {
  const t = gx.tree;
  if (!t) return setHTML($('gx-tree'), '');
  // A few services open, many folded (an LE Audio device has a dozen): the headers are the map,
  // and what was opened stays open across a reload.
  const open = s => gx.open.has(s.uuid + '@' + s.handle) || t.services.length <= 4;
  setHTML($('gx-tree'), t.services.length ? t.services.map(s => `<details class="gatt-svc" data-key="${esc(s.uuid + '@' + s.handle)}"${open(s) ? ' open' : ''}>
      <summary class="title"><span class="mono">0x${s.handle.toString(16).padStart(4, '0')}</span> ${esc(s.name || 'Service')}
        <span class="mono small muted">${esc(s.uuid)}</span>${s.primary ? '' : ' <span class="small muted">secondary</span>'}
        <span class="small muted count">· ${s.characteristics.length} characteristic${s.characteristics.length === 1 ? '' : 's'}</span></summary>
      ${s.characteristics.map(charRow).join('')}</details>`).join('')
    : `<div class="small muted">${t.connected ? 'No GATT services on this device yet.' : 'Connect the device to resolve its services.'}</div>`);
}

function loadGatt() {
  const addr = $('gx-dev').value;
  if (!addr) return;
  gx.addr = addr;
  api(`/gatt/${addr}`).then(t => {
    gx.tree = t;
    gx.loaded = t.services_resolved;
    renderGatt();
  }).catch(fail);
}

function gattAction(op, handle, btn) {
  const base = `/gatt/${gx.addr}/${handle}`;
  if (op === 'read') {
    api(base).then(r => {
      const v = $('gv-' + handle);
      if (v) v.textContent = r.value || '(empty)';
      const t = $('gt-' + handle);
      if (t) t.textContent = quoted(r.text);
      logLine($('gx-log'), `${fmtTime(Date.now())} read 0x${Number(handle).toString(16)}: ${r.value}${r.text ? ' “' + r.text + '”' : ''}`);
    }).catch(fail);
  } else if (op === 'write') {
    const raw = document.querySelector(`input.g-val[data-h="${handle}"]`).value.trim();
    const type = document.querySelector(`select.g-type[data-h="${handle}"]`).value;
    const body = raw.startsWith('text:') ? {text: raw.slice(5), type} : {value: raw, type};
    put(base, body).then(r => logLine($('gx-log'), `${fmtTime(Date.now())} wrote 0x${Number(handle).toString(16)} (${type}): ${r.value}`))
      .catch(fail);
  } else if (op === 'notify') {
    const on = btn.dataset.on === '1';
    btn.disabled = true;
    // Only this button changes: a re-render would drop what is typed in the write fields.
    post(`${base}/notify`, {on}).then(() => {
      const c = findChar(Number(handle));
      if (c) c.notifying = on;
      btn.dataset.on = on ? '0' : '1';
      btn.textContent = on ? 'Stop notify' : 'Notify';
    }).catch(fail).finally(() => { btn.disabled = false; });
  }
}

function findChar(h) {
  if (!gx.tree) return null;
  for (const s of gx.tree.services) for (const c of s.characteristics) if (c.handle === h) return c;
  return null;
}

// ---------------------------------------------------------------- advertising

const csv = s => s.split(',').map(x => x.trim()).filter(Boolean);
// "0xffff:0102, 76:aa" → {"0xffff":"0102","76":"aa"}
const pairs = s => Object.fromEntries(csv(s).map(p => {
  const i = p.lastIndexOf(':');
  return [p.slice(0, i).trim(), p.slice(i + 1).trim()];
}));

function advBody(form) {
  const v = n => form.elements[n].value.trim();
  const b = {type: v('type'), discoverable: form.elements.discoverable.checked};
  if (v('local_name')) b.local_name = v('local_name');
  if (v('service_uuids')) b.service_uuids = csv(v('service_uuids'));
  if (v('manufacturer_data')) b.manufacturer_data = pairs(v('manufacturer_data'));
  if (v('service_data')) b.service_data = pairs(v('service_data'));
  if (v('appearance')) b.appearance = v('appearance');
  if (v('includes')) b.includes = csv(v('includes'));
  for (const n of ['min_interval_ms', 'max_interval_ms', 'timeout_s']) if (v(n)) b[n] = parseInt(v(n), 10);
  if (v('tx_power')) b.tx_power = parseInt(v('tx_power'), 10);
  return b;
}

function refreshAdv() {
  return api('/le/adv').then(r => {
    const m = r.manager;
    setText($('adv-mgr'), m ? `${m.supported_instances} of ${m.active_instances + m.supported_instances} controller slots free` : 'This adapter cannot advertise');
    $('adv-mgr').title = m ? 'Includes the controller supports: ' + m.supported_includes.join(', ') : '';
    setHTML($('adv-list'), r.instances.map(i => {
      const s = i.spec;
      const what = [s.type, s.service_uuids.length && s.service_uuids.join(' '),
        Object.keys(s.manufacturer_data).length && 'mfr ' + Object.entries(s.manufacturer_data).map(e => e.join(':')).join(' '),
        s.timeout_s && `timeout ${s.timeout_s} s`].filter(Boolean).join(' · ');
      const cls = i.state === 'active' ? 'good' : i.state === 'failed' ? 'bad' : 'warn';
      return `<div class="rw"><div class="rw-main"><span class="rw-led${i.state === 'active' ? ' on' : i.state === 'failed' ? '' : ' busy'}"></span>
          <div class="rw-title"><b>${esc(s.local_name || 'Advertisement ' + i.id)}</b><span class="rw-sub"><span>${esc(what)}</span><span>${esc(i.state)}</span></span></div>
          <div class="rw-acts"><button data-adv="${i.id}" class="danger">Stop</button></div></div>
        ${i.error ? `<div class="rw-note bad-text">${esc(i.error)}</div>` : ''}</div>`;
    }).join('') || '<div class="empty">Not advertising. Fill in the form above and press Advertise.</div>');
  }).catch(e => setText($('adv-mgr'), e.message));
}

// ---------------------------------------------------------------- local GATT server

function renderServer(s) {
  setText($('gs-state'), s.state);
  setClass($('gs-state'), 'pill ' + (s.state === 'registered' ? 'good' : s.state === 'failed' ? 'bad' : ''));
  setText($('gs-err'), s.error || '');
  setHTML($('gs-values'), s.values.length ? `<div class="kv">${s.values.map(v =>
    `<span class="mono">${esc(v.uuid)}</span><span class="mono">${esc(v.value || '(empty)')}${v.text ? ' “' + esc(v.text) + '”' : ''}${v.notifying ? ' · notifying' : ''}</span>`).join('')}</div>`
    : '<div class="muted">Nothing registered: Apply a service definition.</div>');
  if (s.app && !$('gs-json').value.trim()) $('gs-json').value = JSON.stringify(s.app, null, 2);
}
const refreshServer = () => api('/le/gatt-server').then(renderServer).catch(fail);

// ---------------------------------------------------------------- wiring

let lePoll = null;

function setupLe() {
  $('gx-load').onclick = loadGatt;
  $('gx-dev').onchange = () => { gx.addr = $('gx-dev').value; gx.tree = null; gx.loaded = false; gx.open.clear(); renderGatt(); loadGatt(); };
  $('gx-connect').onclick = () => {
    const d = btState && btState.devices.find(x => x.address === $('gx-dev').value);
    if (!d) return;
    gx.addr = d.address;
    gx.loaded = false;
    const le = gattBearer(d);
    const on = le ? d.bearers.le.connected : d.connected;
    post(`/bluetooth/devices/${d.address}/${on ? 'disconnect' : 'connect'}`, le ? {bearer: le} : {}).catch(fail);
  };
  $('gx-clear').onclick = () => logClear($('gx-log'));
  // Remembered per service, so a reload or a notify keeps what was opened.
  $('gx-tree').addEventListener('toggle', e => {
    const k = e.target.dataset && e.target.dataset.key;
    if (k) e.target.open ? gx.open.add(k) : gx.open.delete(k);
  }, true);
  $('gx-tree').onclick = e => {
    const b = e.target.closest('button[data-g]');
    if (b) gattAction(b.dataset.g, b.dataset.h, b);
  };
  $('adv-form').onsubmit = e => {
    e.preventDefault();
    post('/le/adv', advBody(e.target)).then(refreshAdv).catch(e2 => { fail(e2); refreshAdv(); });
  };
  $('adv-list').onclick = e => {
    const b = e.target.closest('button[data-adv]');
    if (b) del(`/le/adv/${b.dataset.adv}`).then(refreshAdv).catch(fail);
  };
  $('gs-example').onclick = () => api('/le/gatt-server/example')
    .then(j => { $('gs-json').value = JSON.stringify(j, null, 2); }).catch(fail);
  $('gs-apply').onclick = () => {
    let j;
    try { j = JSON.parse($('gs-json').value); } catch (e) { return toast('Not JSON: ' + e.message); }
    put('/le/gatt-server', j).then(renderServer).catch(e => { fail(e); if (e.body && e.body.state) renderServer(e.body); });
  };
  $('gs-clear').onclick = () => del('/le/gatt-server').then(refreshServer).catch(fail);
  $('gs-log-clear').onclick = () => logClear($('gs-log'));
}

registerTab('le', {
  topics: () => ['bt', 'gatt.notify', 'gatt.server'],
  show() {
    api('/bluetooth').then(btApply).catch(() => {});
    refreshAdv();
    refreshServer().then(() => {
      if (!$('gs-json').value.trim()) $('gs-example').onclick();
    });
    // Advertisement state changes by itself (a Timeout runs out): a slow poll while visible.
    clearInterval(lePoll);
    lePoll = setInterval(() => { refreshAdv(); refreshServer(); }, 3000);
  },
  hide() { clearInterval(lePoll); lePoll = null; },
  onMessage(topic, d) {
    if (topic === 'gatt.notify') {
      logLine($('gx-log'), `${fmtTime(d.ts)} ${d.address} 0x${d.handle.toString(16).padStart(4, '0')} ${d.uuid}: ${d.value}${d.text ? ' “' + d.text + '”' : ''}`);
      if (d.address === gx.addr) {
        const v = $('gv-' + d.handle);
        if (v) v.textContent = d.value || '(empty)';
        const t = $('gt-' + d.handle);
        if (t) t.textContent = quoted(d.text);
        const c = findChar(d.handle);
        if (c) { c.value = d.value; c.text = d.text; }
      }
    } else if (topic === 'gatt.server') {
      logLine($('gs-log'), `${fmtTime(Date.now())} ${d.op} ${d.uuid}${d.value != null ? ' = ' + d.value : ''}${d.device ? ' by ' + d.device : ''}`);
    }
  },
});
setupLe();
