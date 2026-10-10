'use strict';
// Devices: the adapter, the agent's settings, discovery and the device list. Everything here is a
// request followed by the next "bt" frame: the daemon's bus thread does the D-Bus work, and a slow
// operation (a pairing waiting on a phone) shows up as the device's "busy", then its result.

let btState = null;
const devOpen = new Set();  // addresses whose details are expanded

function btApply(b) {
  btState = b;
  if (currentTab === 'devices') renderDevices();
  if (typeof leOnBt === 'function') leOnBt(b);
}

// Whether the adapter's BlueZ takes a discovery filter key (GetDiscoveryFilters); unknown (an
// empty list) counts as yes, and BlueZ says no itself if it is not.
function scSupports(key) {
  const f = btState && btState.discovery && btState.discovery.filters;
  return !f || !f.length || f.includes(key);
}

function renderScanOptions(b) {
  const f = (b.discovery && b.discovery.filters) || [];
  document.querySelectorAll('#sc-opts [data-filter]').forEach(el => {
    const keys = el.dataset.filter.split(' ');
    const ok = !f.length || keys.some(k => f.includes(k));
    if (el.disabled !== !ok && el.id !== 'sc-auto') el.disabled = !ok;
  });
  setText($('sc-note'), f.length ? 'This adapter takes: ' + f.join(', ') + '.' : '');
  $('sc-sig-val').disabled = !$('sc-sig').value || $('sc-sig').disabled;
  if (!scSupports('AutoConnect') || !$('sc-pattern').value.trim()) $('sc-auto').disabled = true;
  const cur = b.discovery && b.discovery.on && b.discovery.ours ? b.discovery.filter : null;
  const parts = cur ? [cur.transport === 'auto' ? 'dual' : cur.transport === 'le' ? 'LE' : 'BR/EDR',
    cur.rssi != null ? 'RSSI ≥ ' + cur.rssi : '', cur.pathloss != null ? 'path loss ≤ ' + cur.pathloss : '',
    cur.uuids.length ? cur.uuids.join(', ') : '', cur.pattern != null ? '“' + cur.pattern + '…”' : ''].filter(Boolean) : [];
  return parts.join(' · ');
}

function renderAdapter(b) {
  const a = b.adapter;
  setText($('ad-msg'), b.error || '');
  setText($('ad-name'), a ? `${a.name} ${a.address}` : '');
  const on = !!(b.available && a);
  ['ad-powered', 'ad-discoverable', 'ad-pairable', 'ad-disc-timeout', 'ad-alias', 'ag-policy', 'ag-cap',
    'sc-toggle'].forEach(id => { $(id).disabled = !on; });
  if (a) {
    if (idle($('ad-powered'))) $('ad-powered').checked = a.powered;
    if (idle($('ad-discoverable'))) $('ad-discoverable').checked = a.discoverable;
    if (idle($('ad-pairable'))) $('ad-pairable').checked = a.pairable;
    if (idle($('ad-disc-timeout'))) $('ad-disc-timeout').value = a.discoverable_timeout_s;
    if (idle($('ad-alias'))) $('ad-alias').value = a.alias;
  }
  if (b.agent) {
    if (idle($('ag-policy'))) $('ag-policy').value = b.agent.policy;
    if (idle($('ag-cap'))) $('ag-cap').value = b.agent.capability;
  }
  const d = b.discovery || {};
  setText($('sc-toggle'), d.on ? 'Stop' : 'Scan');
  const how = renderScanOptions(b);
  setText($('sc-state'), d.on
    ? `Scanning${how ? ' (' + how + ')' : ''}${d.until_s != null ? ', ' + d.until_s + ' s left' : ''}${d.ours ? '' : ' (started elsewhere)'}`
    : '');
  setClass($('sc-toggle'), d.on ? '' : 'primary');
}

const short = u => u.length > 8 ? u.slice(0, 8) + '…' : u;

// RSSI as bars: -55 dBm and better is four, below -85 one.
function sigBars(rssi) {
  const n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
  return `<span class="sig s${n}" aria-hidden="true"><i></i><i></i><i></i><i></i></span>`;
}

// The transports the device was seen on (BlueZ's bearers), lit where it is connected.
const BEARER_NAME = {le: 'LE', bredr: 'BR/EDR'};
function bearerChips(d) {
  const b = d.bearers || {};
  return ['bredr', 'le'].filter(k => b[k]).map(k =>
    `<span class="tag${b[k].connected ? ' good' : ''}" title="${BEARER_NAME[k]}: ${b[k].connected ? 'connected' : 'not connected'}${b[k].bonded ? ', bonded' : b[k].paired ? ', paired' : ''}">${BEARER_NAME[k]}</span>`).join('');
}

// Each bearer of a device: its state and a connect or disconnect for that transport alone, then
// which one a dual-mode device connects on first, and why it last went down.
function bearersHTML(d) {
  const a = d.address;
  const b = d.bearers || {};
  const rows = ['bredr', 'le'].filter(k => b[k]).map(k => {
    const x = b[k];
    const st = [x.connected ? '<span class="good-text">connected</span>' : 'not connected',
      x.bonded ? 'bonded' : x.paired ? 'paired' : 'not paired',
      x.connectable === true ? 'connectable' : x.connectable === false ? 'not connectable' : ''].filter(Boolean).join(' · ');
    const act = x.connected ? 'disconnect' : 'connect';
    return `<span class="b-name">${BEARER_NAME[k]}</span><span class="small">${st}</span>
      <button class="small-btn" data-act="bearer-${act}" data-bearer="${k}" data-addr="${a}"${d.busy ? ' disabled' : ''}>${x.connected ? 'Disconnect' : 'Connect'} ${BEARER_NAME[k]}</button>`;
  }).join('');
  const pref = d.preferred_bearer != null ? `<label class="row small">Connect first on
      <select class="dev-pref" data-addr="${a}">${[['last-used', 'the last used'], ['last-seen', 'the last seen'], ['bredr', 'BR/EDR'], ['le', 'LE']]
        .map(([v, t]) => `<option value="${v}"${v === d.preferred_bearer ? ' selected' : ''}>${t}</option>`).join('')}</select>
      <span class="muted">(takes effect once disconnected)</span></label>` : '';
  const ld = d.last_disconnect;
  const last = ld ? `<div class="small muted">Last disconnect: ${BEARER_NAME[ld.bearer] || ld.bearer}, ${esc(ld.reason)}${ld.message ? ' (' + esc(ld.message) + ')' : ''}, ${new Date(ld.at_ms).toLocaleTimeString()}</div>` : '';
  if (!rows && !pref && !last) return '';
  return `<div class="bearers">${rows}</div>${pref}${last}`;
}

// The live link of a connected device (btmgmt conn-info): asked every two seconds while its
// details are open, since BlueZ's RSSI comes from scanning only.
const devLink = {};  // address → {bredr, le} or {error}
let devLinkTimer = null;
function linkHTML(d) {
  const l = devLink[d.address];
  if (!d.connected) return '';
  if (!l) return '<span class="muted">reading…</span>';
  if (l.error) return `<span class="muted">${esc(l.error)}</span>`;
  return ['bredr', 'le'].filter(k => l[k]).map(k => {
    const x = l[k];
    if (x.error) return `<div>${BEARER_NAME[k]}: <span class="muted">${esc(x.error)}</span></div>`;
    // BR/EDR's RSSI is relative to the controller's golden receive range (0: inside it).
    const rssi = x.rssi == null ? 'RSSI unknown'
      : x.rssi_relative ? (x.rssi === 0 ? 'RSSI in the golden range' : `RSSI ${x.rssi > 0 ? '+' : ''}${x.rssi} dB ${x.rssi > 0 ? 'above' : 'below'} the golden range`)
      : sigBars(x.rssi) + ' RSSI ' + x.rssi + ' dBm';
    return `<div>${BEARER_NAME[k]}: ${rssi}${x.tx_power != null ? ', TX power ' + x.tx_power + ' dBm' : ''}${x.max_tx_power != null ? ' (max ' + x.max_tx_power + ')' : ''}</div>`;
  }).join('') || '<span class="muted">no connected bearer</span>';
}
function pollLinks() {
  const open = btState ? btState.devices.filter(d => devOpen.has(d.address) && d.connected) : [];
  if (!open.length || currentTab !== 'devices' || document.hidden) return;
  Promise.all(open.map(d => api(`/bluetooth/devices/${d.address}/link`)
    .then(r => { devLink[d.address] = r; }, e => { devLink[d.address] = {error: e.message || String(e)}; })))
    .then(() => renderDevices());
}
const tag = (text, cls) => `<span class="tag ${cls || ''}">${esc(text)}</span>`;

// One row a device: a state LED, its name with what it is, and the one action its state calls for
// (pair, connect or disconnect). The rest — trust, remove, profiles, a PIN, where else to take it —
// waits behind a click on the row, since it is wanted for one device at a time.
function deviceRow(d) {
  const a = d.address;
  const busy = !!d.busy;
  const dis = busy ? ' disabled' : '';
  const open = devOpen.has(a);
  const btn = (act, label, cls) => `<button data-act="${act}" data-addr="${a}" class="${cls || ''}"${dis}>${label}</button>`;
  let main;
  if (busy && d.busy === 'pairing') main = `<button data-act="cancel-pairing" data-addr="${a}">Cancel pairing</button>`;
  else if (d.connected) main = btn('disconnect', 'Disconnect');
  else if (!d.paired) main = btn('pair', 'Pair', 'primary');
  else main = btn('connect', 'Connect', 'primary');
  const kind = d.appearance_name || d.class_major || '';
  const led = busy ? 'busy' : d.connected ? 'on' : d.paired ? 'paired' : '';
  const sub = [
    `<span class="mono">${a}</span>`,
    kind ? `<span>${esc(kind)}</span>` : '',
    d.connected ? '<span class="good-text">connected</span>' : d.paired ? `<span>${d.bonded ? 'bonded' : 'paired'}</span>` : '',
    d.rssi != null ? `<span title="RSSI when last seen in a scan">${sigBars(d.rssi)} ${d.rssi} dBm</span>` : '',
    bearerChips(d),
  ].join('');
  let body = '';
  if (open) {
    const uuids = d.uuids.map(u => `<div>${esc(u.name)} <span class="mono muted">${esc(short(u.uuid))}</span></div>`).join('');
    const md = Object.entries(d.manufacturer_data).map(([k, v]) => `<div class="mono">${esc(k)}: ${esc(v)}</div>`).join('');
    const sd = Object.entries(d.service_data).map(([k, v]) => `<div class="mono">${esc(short(k))}: ${esc(v)}</div>`).join('');
    // Only what the device actually reported: a column of dashes says nothing.
    const kv = [
      ['Name', d.name && d.name !== d.alias ? esc(d.name) : ''],
      ['Class', d.class ? esc(d.class_major || '') + ` <span class="mono muted">${esc(d.class)}</span>` : ''],
      ['Appearance', d.appearance ? esc(d.appearance_name || '') + ` <span class="mono muted">${esc(d.appearance)}</span>` : ''],
      ['Link', linkHTML(d)],
      ['TX power', d.tx_power != null ? d.tx_power + ' dBm (advertised)' : ''],
      ['Advertising flags', d.advertising_flags ? `<span class="mono">${esc(d.advertising_flags)}</span>` : ''],
      ['Services', uuids],
      ['Manufacturer data', md],
      ['Service data', sd],
      ['Adapter', esc(d.adapter)],
    ].filter(r => r[1]).map(r => `<span>${r[0]}</span><span>${r[1]}</span>`).join('');
    const tags = [
      d.trusted ? tag('trusted', 'good') : '',
      d.blocked ? tag('blocked', 'bad') : '',
      d.address_type === 'random' ? tag('LE random address') : '',
      d.legacy_pairing ? tag('legacy pairing') : '',
      d.connected && !d.services_resolved ? tag('resolving services', 'warn') : '',
    ].join(' ');
    const dl = 'dev-uuids-' + a.replace(/:/g, '');
    body = `<div class="rw-body">
      ${tags ? `<div class="row wrap" style="margin-bottom:.5rem">${tags}</div>` : ''}
      ${bearersHTML(d)}
      <div class="kv">${kv}</div>
      <div class="acts">
        ${d.trusted ? btn('untrust', 'Untrust') : btn('trust', 'Trust')}
        <button data-act="remove" data-addr="${a}" class="danger">${armed['rm' + a] ? 'Really remove?' : 'Remove'}</button>
        <span class="spacer"></span>
        <button class="small-btn" data-act="goto-gatt" data-addr="${a}">GATT explorer</button>
        <button class="small-btn" data-act="goto-obex" data-addr="${a}">Send files</button>
        <button class="small-btn" data-act="goto-avdtp" data-addr="${a}">AVDTP probe</button>
      </div>
      <details class="more-fields">
        <summary>One profile, or a legacy PIN</summary>
        <div class="row wrap" style="margin-top:.5rem">
          <input class="prof-uuid mono" data-addr="${a}" placeholder="profile UUID, e.g. 110b" list="${dl}">
          <datalist id="${dl}">${d.uuids.map(u => `<option value="${esc(u.uuid)}">${esc(u.name)}</option>`).join('')}</datalist>
          ${btn('connect-profile', 'Connect')}${btn('disconnect-profile', 'Disconnect')}
          <input class="pair-pin" data-addr="${a}" placeholder="PIN for a legacy pair" maxlength="16">
          ${btn('pair', 'Pair with PIN')}
        </div>
      </details>
    </div>`;
  }
  return `<div class="rw click${open ? ' open' : ''}">
    <div class="rw-main" data-act="toggle" data-addr="${a}">
      <span class="rw-led ${led}"></span>
      <div class="rw-title"><b>${esc(d.alias || d.address)}</b><span class="rw-sub">${sub}</span></div>
      <div class="rw-acts">${main}<span class="chev"></span></div>
    </div>
    ${busy ? `<div class="rw-note warn-text">${esc(d.busy)}…</div>` : ''}
    ${d.error ? `<div class="rw-note bad-text">${esc(d.error)}</div>` : ''}
    ${body}
  </div>`;
}

// Opens another view on this device: its select is filled from the next "bt" frame, so wait for
// the option to exist, then choose it the way a person would (its change handler runs).
function openOnDevice(view, selId, addr) {
  go(view);
  let tries = 0;
  const pick = () => {
    const sel = $(selId);
    if ([...sel.options].some(o => o.value === addr)) {
      sel.value = addr;
      sel.dispatchEvent(new Event('change'));
    } else if (++tries < 20) setTimeout(pick, 150);
  };
  pick();
}

function renderDevices() {
  const b = btState;
  if (!b) return;
  renderAdapter(b);
  // Never under the user's typing: a rebuild would take the text and the focus with it.
  const ae = document.activeElement;
  if (ae && (ae.classList.contains('prof-uuid') || ae.classList.contains('pair-pin') || ae.classList.contains('dev-pref'))) return;
  const f = $('dev-filter').value.trim().toLowerCase();
  const list = b.devices.filter(d => !f || (d.alias + ' ' + d.address).toLowerCase().includes(f));
  setHTML($('dev-list'), list.length ? list.map(deviceRow).join('')
    : `<div class="empty">${b.available ? (f ? 'No device matches the filter.' : 'No devices yet. Press Scan to find some, or make the board visible and pair from the other side.')
      : esc(b.error || 'Bluetooth is not available.')}</div>`);
}

function refreshBt() { return api('/bluetooth').then(btApply).catch(fail); }

function deviceAction(addr, act, el) {
  const path = `/bluetooth/devices/${addr}`;
  const uuidOf = () => {
    const i = document.querySelector(`input.prof-uuid[data-addr="${addr}"]`);
    return i ? i.value.trim() : '';
  };
  let req;
  if (act === 'goto-gatt') return openOnDevice('le/explore', 'gx-dev', addr);
  if (act === 'goto-obex') return openOnDevice('obex/sessions', 'ob-dev', addr);
  if (act === 'goto-avdtp') return openOnDevice('audio/stack', 'av-dev', addr);
  if (act === 'toggle') {
    if (devOpen.has(addr)) devOpen.delete(addr); else devOpen.add(addr);
    renderDevices();
    return;
  } else if (act === 'remove') {
    if (!twoClick('rm' + addr, renderDevices)) return;
    req = del(path);
  } else if (act === 'pair') {
    const pin = document.querySelector(`input.pair-pin[data-addr="${addr}"]`);
    req = post(`${path}/pair`, pin && pin.value.trim() ? {pin: pin.value.trim()} : undefined);
  } else if (act === 'bearer-connect' || act === 'bearer-disconnect') {
    req = post(`${path}/${act.split('-')[1]}`, {bearer: el.dataset.bearer});
  } else if (act === 'connect-profile' || act === 'disconnect-profile') {
    const u = uuidOf();
    if (!u) return toast('Type a profile UUID first (e.g. 110b for A2DP sink)');
    req = post(`${path}/${act.split('-')[0]}`, {uuid: u});
  } else {
    req = post(`${path}/${act}`);
  }
  req.then(() => setTimeout(refreshBt, 200)).catch(fail);
}

function setupDevices() {
  const send = body => put('/bluetooth', body).then(btApply).catch(e => { fail(e); refreshBt(); });
  $('ad-powered').onchange = e => send({powered: e.target.checked});
  $('ad-discoverable').onchange = e => send({discoverable: e.target.checked});
  $('ad-pairable').onchange = e => send({pairable: e.target.checked});
  $('ad-disc-timeout').onchange = e => send({discoverable_timeout_s: Math.max(0, parseInt(e.target.value, 10) || 0)});
  $('ad-alias').onchange = e => send({alias: e.target.value});
  $('ag-policy').onchange = e => send({agent: e.target.value});
  $('ag-cap').onchange = e => send({agent_capability: e.target.value});
  // The scan's options, kept in this browser between visits.
  const SC_IDS = ['sc-transport', 'sc-secs', 'sc-sig', 'sc-sig-val', 'sc-uuids', 'sc-pattern', 'sc-dup', 'sc-disc', 'sc-auto'];
  try {
    const saved = JSON.parse(localStorage.getItem('btbench.scan') || '{}');
    for (const id of SC_IDS) if (id in saved) { const el = $(id); if (el.type === 'checkbox') el.checked = saved[id]; else el.value = saved[id]; }
  } catch (e) { /* no storage: the defaults */ }
  const scSync = () => {
    const sig = $('sc-sig').value;
    $('sc-sig-val').disabled = !sig || $('sc-sig').disabled;
    setText($('sc-sig-label'), sig === 'pathloss' ? 'Path loss (dB, 0..137)' : sig === 'rssi' ? 'RSSI (dBm, -127..20)' : 'Threshold');
    $('sc-sig-val').placeholder = sig === 'pathloss' ? '70' : '-70';
    // Auto-connect works on what the pattern matches, so it needs one.
    const noAuto = !scSupports('AutoConnect');
    $('sc-auto').disabled = noAuto || $('sc-pattern').value.trim() === '';
    if ($('sc-auto').disabled) $('sc-auto').checked = false;
    try {
      const o = {};
      for (const id of SC_IDS) { const el = $(id); o[id] = el.type === 'checkbox' ? el.checked : el.value; }
      localStorage.setItem('btbench.scan', JSON.stringify(o));
    } catch (e) { /* not kept */ }
  };
  $('sc-opts').addEventListener('input', scSync);
  $('sc-opts').addEventListener('change', scSync);
  scSync();
  $('sc-toggle').onclick = () => {
    const on = !(btState && btState.discovery && btState.discovery.on);
    const body = {on, transport: $('sc-transport').value, seconds: parseInt($('sc-secs').value, 10) || 0,
      duplicate_data: $('sc-dup').checked};
    const sig = $('sc-sig').value, v = $('sc-sig-val').value.trim();
    if (sig && v !== '') body[sig] = parseInt(v, 10);
    const uuids = $('sc-uuids').value.split(/[\s,]+/).filter(Boolean);
    if (uuids.length) body.uuids = uuids;
    const pat = $('sc-pattern').value.trim();
    if (pat) body.pattern = pat;
    if ($('sc-disc').checked && scSupports('Discoverable')) body.discoverable = true;
    if ($('sc-auto').checked && !$('sc-auto').disabled) body.auto_connect = true;
    post('/bluetooth/scan', body).then(() => setTimeout(refreshBt, 300)).catch(fail);
  };
  $('dev-list').addEventListener('change', e => {
    if (e.target.classList.contains('dev-pref'))
      put(`/bluetooth/devices/${e.target.dataset.addr}`, {preferred_bearer: e.target.value})
        .then(() => setTimeout(refreshBt, 200)).catch(e2 => { fail(e2); refreshBt(); });
  });
  setInterval(pollLinks, 2000);
  $('dev-filter').oninput = renderDevices;
  $('dev-list').onclick = e => {
    const b = e.target.closest('[data-act]');
    if (b && !b.disabled && b.tagName !== 'SELECT') deviceAction(b.dataset.addr, b.dataset.act, b);
  };
}

registerTab('devices', {
  topics: () => ['bt'],
  show: refreshBt,
  onMessage: (topic, data) => { if (topic === 'bt') btApply(data); },
});
setupDevices();
