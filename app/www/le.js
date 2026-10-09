'use strict';
// LE: what advertisers in range say, a GATT explorer over the BlueZ client, LE advertising from a
// form, and a local GATT server from JSON. The device list comes from the same "bt" frames the
// Devices tab uses; notifications and the server's events have topics of their own.

// LE devices are the ones that advertise something: a random address, data, or flags.
const isLe = d => d.address_type === 'random' || d.advertising_flags != null ||
  Object.keys(d.manufacturer_data).length || Object.keys(d.service_data).length;

function leOnBt(b) {
  if (currentTab !== 'le' || !b) return;
  const les = b.devices.filter(isLe);
  setHTML($('le-adv-view'), les.length ? les.map(d => `<div class="card dev">
      <div class="head"><span class="name">${esc(d.alias || d.address)}</span>
        <span class="mono small muted">${d.address} ${esc(d.address_type)}</span></div>
      <div class="kv">
        <span>RSSI</span><span>${d.rssi != null ? d.rssi + ' dBm' : '—'}${d.tx_power != null ? ' · tx ' + d.tx_power + ' dBm' : ''}</span>
        <span>Flags</span><span class="mono">${esc(d.advertising_flags || '—')}</span>
        <span>Appearance</span><span>${d.appearance ? esc(d.appearance + ' ' + d.appearance_name) : '—'}</span>
        <span>UUIDs</span><span>${d.uuids.map(u => `${esc(u.uuid)} ${esc(u.name)}`).join(', ') || '—'}</span>
        ${Object.entries(d.manufacturer_data).map(([k, v]) => `<span>Mfr ${esc(k)}</span><span class="mono">${esc(v)}</span>`).join('')}
        ${Object.entries(d.service_data).map(([k, v]) => `<span>Svc ${esc(k)}</span><span class="mono">${esc(v)}</span>`).join('')}
      </div></div>`).join('')
    : '<div class="muted small">None seen: scan with transport le or auto on the Devices tab.</div>');

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
  setText($('gx-state'), cur ? (cur.connected ? (cur.services_resolved ? 'connected, services resolved' : 'connected, resolving…')
    : 'not connected') + (cur.busy ? ' · ' + cur.busy : '') + (cur.error ? ' · ' + cur.error : '') : '');
  setText($('gx-connect'), cur && cur.connected ? 'Disconnect' : 'Connect');
  // A device that just resolved its services gets its tree without another click.
  if (cur && cur.services_resolved && gx.addr === cur.address && !gx.loaded) loadGatt();
}

// ---------------------------------------------------------------- GATT explorer

const gx = {addr: '', loaded: false, tree: null};

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
      ${esc(d.name || d.uuid)} <span class="mono">${esc(d.value || '')}</span> ${d.text ? '“' + esc(d.text) + '”' : ''}
      <button class="small-btn" data-g="read" data-h="${d.handle}">read</button></div>`).join('');
  return `<div class="gatt-chr">
    <div><span class="mono">${h}</span> <b>${esc(c.name || '')}</b> <span class="mono small muted">${esc(c.uuid)}</span>
      <span class="small muted">${esc(f.join(', '))}</span></div>
    <div class="small">value <span class="mono" id="gv-${c.handle}">${esc(c.value == null ? '—' : c.value || '(empty)')}</span>
      <span id="gt-${c.handle}">${c.text ? '“' + esc(c.text) + '”' : ''}</span></div>
    <div class="acts">${acts.join('')}</div>${descs}</div>`;
}

function renderGatt() {
  const t = gx.tree;
  if (!t) return setHTML($('gx-tree'), '');
  setHTML($('gx-tree'), t.services.length ? t.services.map(s => `<div class="gatt-svc">
      <div class="title"><span class="mono">0x${s.handle.toString(16).padStart(4, '0')}</span> ${esc(s.name || 'Service')}
        <span class="mono small muted">${esc(s.uuid)}</span>${s.primary ? '' : ' <span class="small muted">secondary</span>'}</div>
      ${s.characteristics.map(charRow).join('')}</div>`).join('')
    : `<div class="small muted">${t.connected ? 'No services (yet).' : 'Connect the device to resolve its services.'}</div>`);
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
      if (t) t.textContent = r.text ? '“' + r.text + '”' : '';
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
    post(`${base}/notify`, {on}).then(() => {
      const c = findChar(Number(handle));
      if (c) c.notifying = on;
      renderGatt();
    }).catch(fail);
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
    setText($('adv-mgr'), m ? `controller: ${m.active_instances} active, ${m.supported_instances} free · includes ${m.supported_includes.join(', ')}` : 'no LE advertising manager on this adapter');
    setHTML($('adv-list'), r.instances.map(i => {
      const s = i.spec;
      const what = [s.type, s.local_name && `“${s.local_name}”`, s.service_uuids.length && s.service_uuids.join(' '),
        Object.keys(s.manufacturer_data).length && 'mfr ' + Object.entries(s.manufacturer_data).map(e => e.join(':')).join(' '),
        s.timeout_s && `timeout ${s.timeout_s} s`].filter(Boolean).join(' · ');
      const cls = i.state === 'active' ? 'good' : i.state === 'failed' ? 'bad' : 'warn';
      return `<div class="card dev"><div class="head"><span>#${i.id} ${esc(what)}</span>${pill(i.state, cls)}</div>
        ${i.error ? `<div class="small bad-text">${esc(i.error)}</div>` : ''}
        <div class="acts"><button data-adv="${i.id}" class="danger">Remove</button></div></div>`;
    }).join('') || '<div class="small muted">Not advertising.</div>');
  }).catch(e => setText($('adv-mgr'), e.message));
}

// ---------------------------------------------------------------- local GATT server

function renderServer(s) {
  setText($('gs-state'), s.state);
  setClass($('gs-state'), 'pill ' + (s.state === 'registered' ? 'good' : s.state === 'failed' ? 'bad' : ''));
  setText($('gs-err'), s.error || '');
  setHTML($('gs-values'), s.values.length ? `<div class="kv">${s.values.map(v =>
    `<span class="mono">${esc(v.uuid)}</span><span class="mono">${esc(v.value || '(empty)')}${v.text ? ' “' + esc(v.text) + '”' : ''}${v.notifying ? ' · notifying' : ''}</span>`).join('')}</div>`
    : '<div class="small muted">No application.</div>');
  if (s.app && !$('gs-json').value.trim()) $('gs-json').value = JSON.stringify(s.app, null, 2);
}
const refreshServer = () => api('/le/gatt-server').then(renderServer).catch(fail);

// ---------------------------------------------------------------- wiring

let lePoll = null;

function setupLe() {
  $('gx-load').onclick = loadGatt;
  $('gx-dev').onchange = () => { gx.addr = $('gx-dev').value; gx.tree = null; gx.loaded = false; renderGatt(); loadGatt(); };
  $('gx-connect').onclick = () => {
    const d = btState && btState.devices.find(x => x.address === $('gx-dev').value);
    if (!d) return;
    gx.addr = d.address;
    gx.loaded = false;
    post(`/bluetooth/devices/${d.address}/${d.connected ? 'disconnect' : 'connect'}`).catch(fail);
  };
  $('gx-clear').onclick = () => logClear($('gx-log'));
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
        if (v) v.textContent = d.value;
        const c = findChar(d.handle);
        if (c) c.value = d.value;
      }
    } else if (topic === 'gatt.server') {
      logLine($('gs-log'), `${fmtTime(Date.now())} ${d.op} ${d.uuid}${d.value != null ? ' = ' + d.value : ''}${d.device ? ' by ' + d.device : ''}`);
    }
  },
});
setupLe();
