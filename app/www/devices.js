'use strict';
// Devices: the adapter, the agent's settings, discovery and the device list. Everything here is a
// request followed by the next "bt" frame: the daemon's bus thread does the D-Bus work, and a slow
// operation (a pairing waiting on a phone) shows up as the device's "busy", then its result.

let btState = null;
const devOpen = new Set();  // devKey()s whose details are expanded

// A device is one adapter's: on a board with two adapters, each adapter that has seen a device has
// its own object of it (its own pairing, its own connection). Its key, for a select or the rows
// opened, is "AA:BB:CC:DD:EE:FF@hci1", and every request about it names the adapter.
const devKey = d => d.address + '@' + d.adapter;
const keyAddr = k => (k || '').split('@')[0];
const keyQ = k => (k || '').includes('@') ? '?adapter=' + encodeURIComponent(k.split('@')[1]) : '';
const devByKey = k => (btState && btState.devices.find(d => devKey(d) === k)) || null;
const devUrl = (d, rest = '') => `/bluetooth/devices/${d.address}${rest}?adapter=${encodeURIComponent(d.adapter)}`;

// The adapter the card, the scan and the device list are about: the primary until another is
// picked, remembered in this browser by its address (an hciN can change at the next boot).
let adSel = '';
try { adSel = localStorage.getItem('btbench.adapter') || ''; } catch (e) { /* the primary */ }
function selAdapter(b) {
  const as = (b && b.adapters) || [];
  return as.find(a => a.address === adSel) || (b && b.adapter) || as[0] || null;
}
const adWhat = a => [a.manufacturer_name || (a.manufacturer != null ? 'company ' + a.manufacturer : ''),
  a.version ? 'Bluetooth ' + a.version : '', a.usb ? 'USB' : ''].filter(Boolean).join(' · ');

function btApply(b) {
  btState = b;
  if (currentTab === 'devices') {
    refreshDevAudio(b);
    renderDevices();
  }
  if (typeof leOnBt === 'function') leOnBt(b);
}

// Whether the adapter's BlueZ takes a discovery filter key (GetDiscoveryFilters); unknown (an
// empty list) counts as yes, and BlueZ says no itself if it is not.
function scSupports(key) {
  const a = selAdapter(btState);
  const f = a && a.discovery && a.discovery.filters;
  return !f || !f.length || f.includes(key);
}

function renderScanOptions(b) {
  const a = selAdapter(b);
  const disc = (a && a.discovery) || {};
  const f = disc.filters || [];
  document.querySelectorAll('#sc-opts [data-filter]').forEach(el => {
    const keys = el.dataset.filter.split(' ');
    const ok = !f.length || keys.some(k => f.includes(k));
    if (el.disabled !== !ok && el.id !== 'sc-auto') el.disabled = !ok;
  });
  setText($('sc-note'), f.length ? 'This adapter takes: ' + f.join(', ') + '.' : '');
  $('sc-sig-val').disabled = !$('sc-sig').value || $('sc-sig').disabled;
  if (!scSupports('AutoConnect') || !$('sc-pattern').value.trim()) $('sc-auto').disabled = true;
  const cur = disc.on && disc.ours ? disc.filter : null;
  const parts = cur ? [cur.transport === 'auto' ? 'dual' : cur.transport === 'le' ? 'LE' : 'BR/EDR',
    cur.rssi != null ? 'RSSI ≥ ' + cur.rssi : '', cur.pathloss != null ? 'path loss ≤ ' + cur.pathloss : '',
    cur.uuids.length ? cur.uuids.join(', ') : '', cur.pattern != null ? '“' + cur.pattern + '…”' : ''].filter(Boolean) : [];
  return parts.join(' · ');
}

function renderAdapter(b) {
  const a = selAdapter(b);
  const as = b.adapters || [];
  setText($('ad-msg'), b.error || (b.stale ? 'BlueZ is slow to answer: the devices are as it last listed them.' : ''));
  // Two adapters (the board's own and a USB one): which one this card is about, and which is the
  // primary — the one the advertisements, the GATT server and the HID remote run on.
  $('ad-pick').hidden = as.length < 2;
  if (as.length > 1)
    setHTML($('ad-pick'), as.map(x => `<button class="seg${a && x.address === a.address ? ' on' : ''}" data-ad="${esc(x.address)}"
      title="${esc([x.alias, x.address, adWhat(x), x.primary ? 'primary' : ''].filter(Boolean).join(' · '))}">${esc(x.name)}${x.primary ? ' ★' : ''}</button>`).join(''));
  setText($('ad-name'), a ? [as.length > 1 ? '' : a.name, a.address, adWhat(a)].filter(Boolean).join(' · ') : '');
  setHTML($('ad-role'), !a || as.length < 2 ? ''
    : a.primary ? '<span class="tag good" title="Advertising, the GATT server and the HID remote run on this adapter, and a request that names no adapter goes to it">primary</span>'
    : `<button class="small-btn" data-act="primary" title="Move advertising, the GATT server and the HID remote to ${esc(a.name)}">Make primary</button>`);
  const on = !!(b.available && a);
  ['ad-powered', 'ad-discoverable', 'ad-pairable', 'ad-disc-timeout', 'ad-alias', 'ag-policy', 'ag-cap',
    'ad-reconnect', 'sc-toggle'].forEach(id => { $(id).disabled = !on; });
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
  if (b.reconnect && idle($('ad-reconnect'))) $('ad-reconnect').checked = b.reconnect.on;
  const d = (a && a.discovery) || {};
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
  const a = devKey(d);
  const b = d.bearers || {};
  const rows = ['bredr', 'le'].filter(k => b[k]).map(k => {
    const x = b[k];
    const st = [x.connected ? '<span class="good-text">connected</span>' : 'not connected',
      x.bonded ? 'bonded' : x.paired ? 'paired' : 'not paired',
      x.connectable === true ? 'connectable' : x.connectable === false ? 'not connectable' : ''].filter(Boolean).join(' · ');
    const act = x.connected ? 'disconnect' : 'connect';
    // A connect or disconnect of this transport alone says so here, not under the device's row.
    const here = d.busy && d.busy_on === k;
    const err = !d.busy && d.error && d.error_on === k ? `<span class="b-err small bad-text">${esc(d.error)}</span>` : '';
    return `<span class="b-name">${BEARER_NAME[k]}</span><span class="small">${here ? `<span class="warn-text"><span class="spin"></span>${esc(d.busy)} ${BEARER_NAME[k]}…</span>` : st}</span>
      <button class="small-btn" data-act="bearer-${act}" data-bearer="${k}" data-key="${a}"${d.busy ? ' disabled' : ''}>${x.connected ? 'Disconnect' : 'Connect'} ${BEARER_NAME[k]}</button>${err}`;
  }).join('');
  const pref = d.preferred_bearer != null ? `<label class="row small">Connect first on
      <select class="dev-pref" data-key="${a}">${[['last-used', 'the last used'], ['last-seen', 'the last seen'], ['bredr', 'BR/EDR'], ['le', 'LE']]
        .map(([v, t]) => `<option value="${v}"${v === d.preferred_bearer ? ' selected' : ''}>${t}</option>`).join('')}</select>
      <span class="muted">(takes effect once disconnected)</span></label>` : '';
  const ld = d.last_disconnect;
  const last = ld ? `<div class="small muted">Last disconnect: ${BEARER_NAME[ld.bearer] || ld.bearer}, ${esc(ld.reason)}${ld.message ? ' (' + esc(ld.message) + ')' : ''}, ${new Date(ld.at_ms).toLocaleTimeString()}</div>` : '';
  if (!rows && !pref && !last) return '';
  return `<div class="bearers">${rows}</div>${pref}${last}`;
}

// The live link of a connected device (btmgmt conn-info): asked every two seconds while its
// details are open, since BlueZ's RSSI comes from scanning only.
const devLink = {};  // devKey → {bredr, le} or {error}
let devLinkTimer = null;
function linkHTML(d) {
  const l = devLink[devKey(d)];
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
  const open = btState ? btState.devices.filter(d => devOpen.has(devKey(d)) && d.connected) : [];
  if (!open.length || currentTab !== 'devices' || document.hidden) return;
  Promise.all(open.map(d => api(devUrl(d, '/link'))
    .then(r => { devLink[devKey(d)] = r; }, e => { devLink[devKey(d)] = {error: e.message || String(e)}; })))
    .then(() => renderDevices());
}
const tag = (text, cls) => `<span class="tag ${cls || ''}">${esc(text)}</span>`;

// The profiles a connected device has up: what BlueZ holds for it (media transports, AVRCP, HID,
// PAN, GATT, battery) and, from PipeWire, the audio profile its card is on — the only place HFP
// shows, since PipeWire's own HFP does not go through BlueZ.
const devAudio = {};  // address → PipeWire's device ({active: {kind, codec, name}}), from /audio/devices
let devAudioKey = '';
function refreshDevAudio(b) {
  const key = b.devices.filter(d => d.connected).map(d => d.address + (d.profiles || []).map(p => p.id + p.detail).join()).join();
  if (key === devAudioKey) return;
  devAudioKey = key;
  clearTimeout(refreshDevAudio.t);
  // A moment after the change: PipeWire makes the device's card once BlueZ has its transports.
  refreshDevAudio.t = setTimeout(() => api('/audio/devices').then(r => {
    for (const k in devAudio) delete devAudio[k];
    (r.devices || []).forEach(x => { if (x.address) devAudio[x.address.toUpperCase()] = x; });
    if (currentTab === 'devices') renderDevices();
  }).catch(() => {}), 1200);
}

const PROFILE_KIND = {a2dp: 'A2DP', 'a2dp-duplex': 'A2DP duplex', gateway: 'A2DP / HFP gateway', headset: 'HFP', bap: 'LE Audio'};
function deviceProfiles(d) {
  const ps = (d.profiles || []).map(p => ({name: p.name, detail: p.detail, state: p.state}));
  const pw = devAudio[d.address];
  const act = pw && pw.active;
  // HFP, and an audio profile PipeWire chose that BlueZ has no transport for (yet).
  if (act && act.kind && act.kind !== 'off' && act.kind !== 'other' && !(act.kind !== 'headset' && ps.some(p => p.name === 'A2DP' || p.name === 'LE Audio')))
    ps.push({name: PROFILE_KIND[act.kind] || act.kind, detail: [act.codec, 'PipeWire: ' + act.name].filter(Boolean).join(' · '), state: 'connected'});
  return ps;
}
// In the row: each profile once (a duplex link has two A2DP transports), lit while it streams.
function profileTags(d) {
  if (!d.connected) return '';
  const by = new Map();
  deviceProfiles(d).filter(p => p.name !== 'Battery').forEach(p => {
    const x = by.get(p.name) || {on: false, details: []};
    x.on = x.on || p.state === 'active';
    if (p.detail) x.details.push(p.detail);
    by.set(p.name, x);
  });
  return [...by].map(([name, x]) => `<span class="tag${x.on ? ' good' : ''}" title="${esc(x.details.join('; '))}">${esc(name)}</span>`).join('');
}
// A profile's UUID (busy_on, error_on: a bearer is "le" or "bredr").
const isUuid = s => !!s && s.length > 5;
function profilesHTML(d) {
  const ps = d.connected ? deviceProfiles(d) : [];
  const busy = d.busy && isUuid(d.busy_on) ? `<div class="small warn-text"><span class="spin"></span>${esc(d.busy)} ${esc(uuidLabel(d, d.busy_on))}…</div>` : '';
  const err = !d.busy && d.error && isUuid(d.error_on) ? `<div class="small bad-text">${esc(uuidLabel(d, d.error_on))}: ${esc(d.error)}</div>` : '';
  if (!d.connected) return busy + err;
  if (!ps.length && !busy && !err) return `<div class="small muted">${d.services_resolved ? 'No profile connected.' : 'Resolving services…'}</div>`;
  return `<div class="profiles">${ps.map(p => `<span class="pr-name">${esc(p.name)}</span>
    <span class="small">${esc(p.detail || '')}</span>
    <span>${p.state && p.state !== 'connected' ? pill(p.state === 'active' ? 'streaming' : p.state, p.state === 'active' ? 'good' : p.state === 'pending' ? 'warn' : '') : ''}</span>`).join('')}</div>${busy}${err}`;
}
// A profile's UUID by its name where the device lists it.
const uuidLabel = (d, u) => {
  const full = v => v.length === 4 ? '0000' + v + '-0000-1000-8000-00805f9b34fb' : v;
  const x = d.uuids.find(v => full(v.uuid.toLowerCase()) === u.toLowerCase());
  return x ? x.name : u;
};

// One row a device: a state LED, its name with what it is, and the one action its state calls for
// (pair, connect or disconnect). The rest — trust, remove, profiles, a PIN, where else to take it —
// waits behind a click on the row, since it is wanted for one device at a time.
// A device that is one of this board's own adapters, as the other adapter sees it advertise.
function ownAdapterTag(d) {
  const own = ((btState && btState.adapters) || []).find(x => x.address === d.address);
  return own ? `<span class="tag own" title="One of this board's own adapters, seen by the other one">this board · ${esc(own.name)}</span>` : '';
}

function deviceRow(d) {
  const a = devKey(d);
  const busy = !!d.busy;
  const dis = busy ? ' disabled' : '';
  const open = devOpen.has(a);
  const btn = (act, label, cls) => `<button data-act="${act}" data-key="${a}" class="${cls || ''}"${dis}>${label}</button>`;
  // One button of one width whatever it says, the work in progress included, so the row does not
  // grow a line or move its neighbours while a device connects.
  let main;
  if (busy && d.busy === 'pairing') main = `<button data-act="cancel-pairing" data-key="${a}" class="dev-main">Cancel pairing</button>`;
  else if (busy) main = `<button class="dev-main" disabled><span class="spin"></span>${esc(d.busy.charAt(0).toUpperCase() + d.busy.slice(1))}…</button>`;
  else if (d.connected) main = btn('disconnect', 'Disconnect', 'dev-main');
  else if (!d.paired) main = btn('pair', 'Pair', 'primary dev-main');
  else main = btn('connect', 'Connect', 'primary dev-main');
  const kind = d.appearance_name || d.class_major || '';
  const led = busy ? 'busy' : d.connected ? 'on' : d.paired ? 'paired' : '';
  // What a connect changes comes last, so it does not push the rest around.
  const sub = [
    `<span class="mono">${d.address}</span>`,
    kind ? `<span>${esc(kind)}</span>` : '',
    bearerChips(d),
    d.connected ? '<span class="good-text">connected</span>' : d.paired ? `<span>${d.bonded ? 'bonded' : 'paired'}</span>` : '',
    d.rssi != null && !d.connected ? `<span title="RSSI when last seen in a scan">${sigBars(d.rssi)} ${d.rssi} dBm</span>` : '',
    profileTags(d),
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
    const dl = 'dev-uuids-' + a.replace(/[:@]/g, '');
    body = `<div class="rw-body">
      ${tags ? `<div class="row wrap" style="margin-bottom:.5rem">${tags}</div>` : ''}
      ${bearersHTML(d)}
      ${d.connected || isUuid(d.busy_on) || isUuid(d.error_on) ? `<h4 class="rw-h">Profiles</h4>${profilesHTML(d)}` : ''}
      <div class="kv">${kv}</div>
      <div class="acts">
        ${d.trusted ? btn('untrust', 'Untrust') : btn('trust', 'Trust')}
        <button data-act="remove" data-key="${a}" class="danger">${armed['rm' + a] ? 'Really remove?' : 'Remove'}</button>
        <span class="spacer"></span>
        <button class="small-btn" data-act="goto-gatt" data-key="${a}">GATT explorer</button>
        <button class="small-btn" data-act="goto-obex" data-key="${a}">Send files</button>
        <button class="small-btn" data-act="goto-avdtp" data-key="${a}">AVDTP probe</button>
      </div>
      <details class="more-fields">
        <summary>One profile, or a legacy PIN</summary>
        <div class="row wrap" style="margin-top:.5rem">
          <input class="prof-uuid mono" data-key="${a}" placeholder="profile UUID, e.g. 110b" list="${dl}">
          <datalist id="${dl}">${d.uuids.map(u => `<option value="${esc(u.uuid)}">${esc(u.name)}</option>`).join('')}</datalist>
          ${btn('connect-profile', 'Connect')}${btn('disconnect-profile', 'Disconnect')}
          <input class="pair-pin" data-key="${a}" placeholder="PIN for a legacy pair" maxlength="16">
          ${btn('pair', 'Pair with PIN')}
        </div>
      </details>
    </div>`;
  }
  return `<div class="rw click${open ? ' open' : ''}" data-dev="${a}" data-state="${led}">
    <div class="rw-main" data-act="toggle" data-key="${a}">
      <span class="rw-led ${led}"></span>
      <div class="rw-title"><b>${esc(d.alias || d.address)}</b>${ownAdapterTag(d)}<span class="rw-sub">${sub}</span></div>
      <div class="rw-acts">${main}<span class="chev"></span></div>
    </div>
    ${d.error && (!d.error_on || !open) ? `<div class="rw-note bad-text">${esc(d.error)}</div>` : ''}
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
  // With two adapters, the devices of the one picked: what it has seen, paired and connected.
  const a = (b.adapters || []).length > 1 ? selAdapter(b) : null;
  const list = stableOrder(b.devices.filter(d => (!a || d.adapter === a.name) && (!f || (d.alias + ' ' + d.address).toLowerCase().includes(f))),
    f + '@' + (a ? a.name : ''));
  if (!list.length) {
    setHTML($('dev-list'), `<div class="empty">${b.available ? (f ? 'No device matches the filter.' : `No devices${a ? ' on ' + esc(a.name) : ''} yet. Press Scan to find some, or make the board visible and pair from the other side.`)
      : esc(b.error || 'Bluetooth is not available.')}</div>`);
    return;
  }
  patchRows($('dev-list'), list.map(d => [devKey(d), deviceRow(d)]));
}

// Rows keep the place they were first shown in: a connect, a pairing or a new RSSI does not move
// them under the pointer. A new device goes last; coming back to the view, or another filter or
// adapter, sorts them again (the daemon's order: connected, paired, then by name).
let devOrder = new Map();  // devKey → position
let devOrderFor = null;
function stableOrder(list, view) {
  if (view !== devOrderFor) {
    devOrder = new Map();
    devOrderFor = view;
  }
  const known = list.filter(d => devOrder.has(devKey(d)));
  const fresh = list.filter(d => !devOrder.has(devKey(d)));
  known.sort((x, y) => devOrder.get(devKey(x)) - devOrder.get(devKey(y)));
  const out = known.concat(fresh);
  devOrder = new Map(out.map((d, i) => [devKey(d), i]));
  return out;
}

// Updates the rows in place, [key, html] in order: only a row that changed is rebuilt, the rest
// keep their DOM (and with it hover, focus and text selection). A rebuilt row keeps its open
// <details>, and a change of state (connected, paired, busy) is flashed rather than jumped to.
function patchRows(list, rows) {
  list._html = null;  // setHTML's cache no longer says what is there
  const have = new Map([...list.children].filter(el => el.dataset.dev).map(el => [el.dataset.dev, el]));
  [...list.children].filter(el => !el.dataset.dev).forEach(el => el.remove());
  let at = list.firstElementChild;
  for (const [key, html] of rows) {
    let el = have.get(key);
    have.delete(key);
    if (!el || el._html !== html) {
      const t = document.createElement('template');
      t.innerHTML = html.trim();
      const fresh = t.content.firstElementChild;
      fresh._html = html;
      if (el) {
        const opened = [...el.querySelectorAll('details')].map(x => x.open);
        fresh.querySelectorAll('details').forEach((x, i) => { if (opened[i]) x.open = true; });
        if (el.dataset.state !== fresh.dataset.state) fresh.classList.add('changed');
        el.replaceWith(fresh);
        if (at === el) at = fresh;
      }
      el = fresh;
    }
    if (el !== at) list.insertBefore(el, at);
    else at = at.nextElementSibling;
  }
  have.forEach(el => el.remove());
}

function refreshBt() { return api('/bluetooth').then(btApply).catch(fail); }

function deviceAction(key, act, el) {
  const addr = keyAddr(key);
  const d = devByKey(key);
  const url = (rest = '') => `/bluetooth/devices/${addr}${rest}${keyQ(key)}`;
  const uuidOf = () => {
    const i = document.querySelector(`input.prof-uuid[data-key="${key}"]`);
    return i ? i.value.trim() : '';
  };
  let req;
  if (act === 'goto-gatt') return openOnDevice('le/explore', 'gx-dev', d ? devKey(d) : key);
  if (act === 'goto-obex') return openOnDevice('obex/sessions', 'ob-dev', addr);
  if (act === 'goto-avdtp') return openOnDevice('audio/bt', 'av-dev', addr);
  if (act === 'toggle') {
    if (devOpen.has(key)) devOpen.delete(key); else devOpen.add(key);
    renderDevices();
    return;
  } else if (act === 'remove') {
    if (!twoClick('rm' + key, renderDevices)) return;
    req = del(url());
  } else if (act === 'pair') {
    const pin = document.querySelector(`input.pair-pin[data-key="${key}"]`);
    req = post(url('/pair'), pin && pin.value.trim() ? {pin: pin.value.trim()} : undefined);
  } else if (act === 'bearer-connect' || act === 'bearer-disconnect') {
    req = post(url('/' + act.split('-')[1]), {bearer: el.dataset.bearer});
  } else if (act === 'connect-profile' || act === 'disconnect-profile') {
    const u = uuidOf();
    if (!u) return toast('Type a profile UUID first (e.g. 110b for A2DP sink)');
    req = post(url('/' + act.split('-')[0]), {uuid: u});
  } else {
    req = post(url('/' + act));
  }
  req.then(() => setTimeout(refreshBt, 200)).catch(fail);
}

function setupDevices() {
  // The adapter settings go to the adapter picked; the agent's and the reconnect are the bench's.
  const send = body => {
    const a = selAdapter(btState);
    return put('/bluetooth', a ? Object.assign({adapter: a.address}, body) : body).then(btApply).catch(e => { fail(e); refreshBt(); });
  };
  $('adapter-card').addEventListener('click', e => {
    const seg = e.target.closest('[data-ad]');
    if (seg) {
      adSel = seg.dataset.ad;
      try { localStorage.setItem('btbench.adapter', adSel); } catch (e2) { /* this visit only */ }
      renderDevices();
      return;
    }
    const pr = e.target.closest('[data-act="primary"]');
    if (pr) send({primary: selAdapter(btState).address});
  });
  $('ad-powered').onchange = e => send({powered: e.target.checked});
  $('ad-discoverable').onchange = e => send({discoverable: e.target.checked});
  $('ad-pairable').onchange = e => send({pairable: e.target.checked});
  $('ad-reconnect').onchange = e => send({reconnect: e.target.checked});
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
    const a = selAdapter(btState);
    const on = !(a && a.discovery && a.discovery.on);
    const body = {on, transport: $('sc-transport').value, seconds: parseInt($('sc-secs').value, 10) || 0,
      duplicate_data: $('sc-dup').checked};
    if (a) body.adapter = a.address;
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
      put(`/bluetooth/devices/${keyAddr(e.target.dataset.key)}${keyQ(e.target.dataset.key)}`, {preferred_bearer: e.target.value})
        .then(() => setTimeout(refreshBt, 200)).catch(e2 => { fail(e2); refreshBt(); });
  });
  setInterval(pollLinks, 2000);
  $('dev-filter').oninput = renderDevices;
  $('dev-list').onclick = e => {
    const b = e.target.closest('[data-act]');
    if (b && !b.disabled && b.tagName !== 'SELECT') deviceAction(b.dataset.key, b.dataset.act, b);
  };
}

registerTab('devices', {
  topics: () => ['bt'],
  show() {
    devOrderFor = null;
    return refreshBt();
  },
  onMessage: (topic, data) => { if (topic === 'bt') btApply(data); },
});
setupDevices();
