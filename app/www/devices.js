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
  setText($('sc-state'), d.on
    ? `scanning (${d.filter ? d.filter.transport : 'auto'})${d.until_s != null ? ', ' + d.until_s + ' s left' : ''}${d.ours ? '' : ' — started elsewhere'}`
    : '');
}

const short = u => u.length > 8 ? u.slice(0, 8) + '…' : u;

function deviceCard(d) {
  const a = d.address;
  const busy = !!d.busy;
  const dis = busy ? ' disabled' : '';
  const btn = (act, label, cls) => `<button data-act="${act}" data-addr="${a}" class="${cls || ''}"${dis}>${label}</button>`;
  const pills = [
    d.connected ? pill('connected', 'good') : '',
    d.paired ? pill(d.bonded ? 'bonded' : 'paired', 'good') : pill('not paired'),
    d.trusted ? pill('trusted') : '',
    d.blocked ? pill('blocked', 'bad') : '',
    d.address_type === 'random' ? pill('LE random') : '',
    d.rssi != null ? pill(d.rssi + ' dBm') : '',
  ].join('');
  const acts = [];
  if (busy && d.busy === 'pairing') acts.push(`<button data-act="cancel-pairing" data-addr="${a}">Cancel pairing</button>`);
  if (!d.paired) acts.push(btn('pair', 'Pair', 'primary'));
  acts.push(d.connected ? btn('disconnect', 'Disconnect') : btn('connect', 'Connect'));
  acts.push(d.trusted ? btn('untrust', 'Untrust') : btn('trust', 'Trust'));
  acts.push(`<button data-act="remove" data-addr="${a}" class="danger">${armed['rm' + a] ? 'Really remove?' : 'Remove'}</button>`);
  let details = '';
  if (devOpen.has(a)) {
    const uuids = d.uuids.map(u => `<div><span class="mono">${esc(short(u.uuid))}</span> ${esc(u.name)}</div>`).join('') || '—';
    const md = Object.entries(d.manufacturer_data).map(([k, v]) => `<div class="mono">${esc(k)}: ${esc(v)}</div>`).join('') || '—';
    const sd = Object.entries(d.service_data).map(([k, v]) => `<div class="mono">${esc(short(k))}: ${esc(v)}</div>`).join('') || '—';
    details = `<div class="details">
      <div class="kv">
        <span>Name</span><span>${esc(d.name || '—')}</span>
        <span>Adapter</span><span>${esc(d.adapter)}</span>
        <span>Class</span><span>${d.class ? esc(d.class + ' ' + d.class_major) : '—'}</span>
        <span>Appearance</span><span>${d.appearance ? esc(d.appearance + ' ' + d.appearance_name) : '—'}</span>
        <span>TX power</span><span>${d.tx_power != null ? d.tx_power + ' dBm' : '—'}</span>
        <span>Adv flags</span><span class="mono">${esc(d.advertising_flags || '—')}</span>
        <span>Legacy pairing</span><span>${d.legacy_pairing ? 'yes' : 'no'}</span>
        <span>Services resolved</span><span>${d.services_resolved ? 'yes' : 'no'}</span>
        <span>UUIDs</span><span>${uuids}</span>
        <span>Manufacturer data</span><span>${md}</span>
        <span>Service data</span><span>${sd}</span>
      </div>
      <div class="row wrap" style="margin-top:.5rem">
        <input class="prof-uuid mono" data-addr="${a}" placeholder="profile UUID, e.g. 110b" list="dev-uuids-${a.replace(/:/g, '')}">
        <datalist id="dev-uuids-${a.replace(/:/g, '')}">${d.uuids.map(u => `<option value="${esc(u.uuid)}">${esc(u.name)}</option>`).join('')}</datalist>
        ${btn('connect-profile', 'Connect profile')}${btn('disconnect-profile', 'Disconnect profile')}
        <input class="pair-pin" data-addr="${a}" placeholder="PIN for a legacy pair" maxlength="16">
      </div>
    </div>`;
  }
  return `<div class="card dev">
    <div class="head"><span class="name" data-act="toggle" data-addr="${a}">${devOpen.has(a) ? '▾' : '▸'} ${esc(d.alias || d.address)}</span>
      <span class="mono small muted">${a}</span></div>
    <div class="meta">${pills}</div>
    ${busy ? `<div class="small warn-text">${esc(d.busy)}…</div>` : ''}
    ${d.error ? `<div class="small bad-text">${esc(d.error)}</div>` : ''}
    <div class="acts">${acts.join('')}</div>
    ${details}
  </div>`;
}

function renderDevices() {
  const b = btState;
  if (!b) return;
  renderAdapter(b);
  // Never under the user's typing: a rebuild would take the text and the focus with it.
  const ae = document.activeElement;
  if (ae && (ae.classList.contains('prof-uuid') || ae.classList.contains('pair-pin'))) return;
  const f = $('dev-filter').value.trim().toLowerCase();
  const list = b.devices.filter(d => !f || (d.alias + ' ' + d.address).toLowerCase().includes(f));
  setHTML($('dev-list'), list.length ? list.map(deviceCard).join('')
    : `<div class="muted small">${b.available ? 'No devices yet: scan.' : esc(b.error || 'Bluetooth is not available.')}</div>`);
}

function refreshBt() { return api('/bluetooth').then(btApply).catch(fail); }

function deviceAction(addr, act) {
  const path = `/bluetooth/devices/${addr}`;
  const uuidOf = () => {
    const i = document.querySelector(`input.prof-uuid[data-addr="${addr}"]`);
    return i ? i.value.trim() : '';
  };
  let req;
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
  $('sc-toggle').onclick = () => {
    const on = !(btState && btState.discovery && btState.discovery.on);
    const body = {on, transport: $('sc-transport').value, seconds: parseInt($('sc-secs').value, 10) || 0};
    const r = $('sc-rssi').value.trim();
    if (r) body.rssi = parseInt(r, 10);
    post('/bluetooth/scan', body).then(() => setTimeout(refreshBt, 300)).catch(fail);
  };
  $('dev-filter').oninput = renderDevices;
  $('dev-list').onclick = e => {
    const b = e.target.closest('[data-act]');
    if (b && !b.disabled) deviceAction(b.dataset.addr, b.dataset.act);
  };
}

registerTab('devices', {
  topics: () => ['bt'],
  show: refreshBt,
  onMessage: (topic, data) => { if (topic === 'bt') btApply(data); },
});
setupDevices();
