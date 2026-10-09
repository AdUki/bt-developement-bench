'use strict';
// The console's core: API helpers, the tabs, the one WebSocket and the pairing modal. Each tab
// lives in its own file and registers itself with registerTab(); only the tab on screen gets its
// topics and runs its polls, because the board is a single-core Zero W and a console left open in
// a background tab must cost it nothing.

const api = (p, opt) => fetch('/api' + p, opt).then(async r => {
  const text = await r.text();
  let body = {};
  try { body = text ? JSON.parse(text) : {}; } catch (e) { body = {error: text}; }
  if (!r.ok) {
    const err = new Error(body.error || r.statusText);
    err.status = r.status;
    err.body = body;
    throw err;
  }
  return body;
});
const jsonReq = method => (p, obj) => api(p, {
  method, headers: {'Content-Type': 'application/json'},
  body: obj === undefined ? undefined : JSON.stringify(obj),
});
const put = jsonReq('PUT');
const post = jsonReq('POST');
const del = p => api(p, {method: 'DELETE'});
const apiText = p => fetch('/api' + p).then(async r => {
  const t = await r.text();
  if (!r.ok) {
    let msg = t;
    try { msg = JSON.parse(t).error || t; } catch (e) { /* plain text */ }
    throw new Error(msg);
  }
  return t;
});

const $ = id => document.getElementById(id);

// Everything a device or a tool says (a name, a GATT string, a journal line) is someone else's
// text going into markup: escape it.
const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g,
  c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));

// Periodic updates write only what changed: rewriting an element with the same content still
// relayouts the page, and Chrome closes an open <select> when it does.
function setHTML(el, html) {
  if (el && el._html !== html) {
    el._html = html;
    el.innerHTML = html;
  }
}
const setText = (el, t) => { if (el && el.textContent !== t) el.textContent = t; };
const setClass = (el, c) => { if (el && el.className !== c) el.className = c; };
// Inputs are updated from the daemon only while the user is not in them.
const idle = el => document.activeElement !== el;

// localStorage throws, not just returns null, when the origin has storage blocked.
const lsGet = k => { try { return localStorage.getItem(k); } catch (e) { return null; } };
const lsSet = (k, v) => { try { localStorage.setItem(k, v); } catch (e) { /* blocked */ } };

// A non-modal error strip: alert() would steal focus at exactly the moment someone is watching.
function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => t.classList.remove('show'), 4000);
}
const fail = e => toast(e.message || String(e));

const fmtBytes = n => n >= 1048576 ? (n / 1048576).toFixed(1) + ' MiB'
  : n >= 1024 ? (n / 1024).toFixed(1) + ' KiB' : n + ' B';
const fmtKbps = bps => (bps / 1000).toFixed(bps >= 100000 ? 0 : 1);
const fmtTime = ms => new Date(ms).toLocaleTimeString([], {hour12: false});
const fmtDate = ms => new Date(ms).toLocaleString([], {hour12: false});
const fmtUptime = s => {
  s = Math.floor(s);
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + 'd ' : '') + h + 'h ' + m + 'm';
};
const pill = (text, cls) => `<span class="pill ${cls || ''}">${esc(text)}</span>`;

// Two clicks for anything that cannot be undone from here (forget a device, delete a capture):
// the first arms the button for three seconds.
const armed = {};
function twoClick(key, rerender) {
  if (armed[key]) {
    clearTimeout(armed[key]);
    delete armed[key];
    return true;
  }
  armed[key] = setTimeout(() => { delete armed[key]; rerender(); }, 3000);
  rerender();
  return false;
}

// Logs (notifications, events, the journal, a job's output) keep a bounded tail.
const LOG_MAX = 200;  // lines kept in each log: a 10 Hz notifier must not grow the page forever

function logLine(el, line) {
  const lines = (el._lines = el._lines || []);
  lines.push(line);
  if (lines.length > LOG_MAX) lines.splice(0, lines.length - LOG_MAX);
  // The log is rewritten at most once a frame, however many lines arrived in it.
  if (!el._pending) {
    el._pending = true;
    requestAnimationFrame(() => {
      el._pending = false;
      const atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 4;
      el.textContent = el._lines.join('\n');
      if (atEnd) el.scrollTop = el.scrollHeight;
    });
  }
}
function logClear(el) {
  el._lines = [];
  el.textContent = '';
}

// ---------------------------------------------------------------- tabs

const tabs = {};
let currentTab = null;

// def: {topics(): [...], show(), hide(), onMessage(topic, data)}; all optional.
function registerTab(name, def) { tabs[name] = def; }

function showTab(name) {
  if (!tabs[name] && !$(name)) name = 'devices';
  if (currentTab === name) return;
  const old = tabs[currentTab];
  if (old && old.hide) old.hide();
  currentTab = name;
  document.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t.dataset.tab === name));
  document.querySelectorAll('.panel').forEach(p => p.classList.toggle('active', p.id === name));
  lsSet('btbench.tab', name);
  const t = tabs[name];
  if (t && t.show) t.show();
  updateTopics();
}

document.querySelectorAll('[data-tab]').forEach(el => el.addEventListener('click', e => {
  e.preventDefault();
  history.replaceState(null, '', '#' + el.dataset.tab);
  showTab(el.dataset.tab);
}));

// ---------------------------------------------------------------- the WebSocket

// One socket for the whole console. What it carries is what the visible tab asks for, plus the
// pairing question and the health line in the header, which matter on every tab.
const ALWAYS = ['bt.request', 'system'];
let ws = null;
let wsBackoff = 500;
let wsTopics = '';

function wantedTopics() {
  const t = tabs[currentTab];
  return [...new Set(ALWAYS.concat(t && t.topics ? t.topics() : []))];
}

function updateTopics() {
  const list = wantedTopics();
  const key = list.join(',');
  if (key === wsTopics) return;
  wsTopics = key;
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({topics: list}));
}

function wsPill(text, cls) {
  setText($('pill-ws'), text);
  setClass($('pill-ws'), 'pill ' + cls);
}

function connectWs() {
  const list = wantedTopics();
  wsTopics = list.join(',');
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/api/ws?topics=${encodeURIComponent(wsTopics)}`);
  ws.onopen = () => {
    wsBackoff = 500;
    wsPill('live', 'good');
    // The tab may have changed while the socket was connecting.
    const now = wantedTopics().join(',');
    if (now !== wsTopics) { wsTopics = ''; updateTopics(); }
    // What happened while the socket was down is not replayed: read it fresh.
    refreshRequest();
    const t = tabs[currentTab];
    if (t && t.show) t.show();
  };
  ws.onmessage = ev => {
    let m;
    try { m = JSON.parse(ev.data); } catch (e) { return; }
    dispatch(m.topic, m.data);
  };
  ws.onclose = () => {
    wsPill('offline', 'bad');
    ws = null;
    setTimeout(connectWs, wsBackoff);
    wsBackoff = Math.min(wsBackoff * 2, 10000);
  };
}

function dispatch(topic, data) {
  if (topic === 'bt.request') renderRequest(data);
  else if (topic === 'system') renderHealthPills(data);
  else if (topic === 'bt') {
    renderBtPill(data);
    renderRequest(data.request);
  }
  // Every tab hears what came; each ignores what is not its own.
  for (const name in tabs) {
    if (tabs[name].onMessage) tabs[name].onMessage(topic, data);
  }
}

// ---------------------------------------------------------------- header

function renderBtPill(b) {
  const a = b && b.adapter;
  let t = 'bluetooth unavailable', c = 'bad';
  if (b && b.available && a) {
    t = a.name + (a.powered ? (a.discovering ? ' · scanning' : a.discoverable ? ' · visible' : ' · on') : ' · off');
    c = a.powered ? (a.discovering ? 'warn' : 'good') : '';
  }
  setText($('pill-bt'), t);
  setClass($('pill-bt'), 'pill ' + c);
}

function renderHealthPills(h) {
  if (!h) return;
  if (h.hostname) setText($('hostname'), h.hostname);
  const p = $('pill-power');
  const bits = [];
  let cls = '';
  if (h.power && h.power.available && (h.power.under_voltage || h.power.seen)) {
    bits.push(h.power.under_voltage ? 'UNDERVOLTAGE' : 'undervoltage seen');
    cls = 'bad';
  }
  if (h.temp_c != null) {
    bits.push(h.temp_c.toFixed(0) + ' °C');
    if (!cls && h.temp_c >= 75) cls = 'warn';
  }
  bits.push('cpu ' + (h.cpu_pct || 0).toFixed(0) + '%');
  p.hidden = false;
  setText(p, bits.join(' · '));
  setClass(p, 'pill ' + cls);
}

// ---------------------------------------------------------------- the agent's question

// BlueZ asks one thing at a time; the modal shows it on whichever tab is open, with the time
// left before BlueZ gives up. Built once per request so a passkey half typed is not wiped by the
// next update.
let request = null;
let requestTimer = null;
const answered = new Set();

function refreshRequest() {
  api('/bluetooth/request').then(renderRequest).catch(() => { /* no Bluetooth: no questions */ });
}

function renderRequest(r) {
  if (r && answered.has(r.id)) r = null;
  if (!r) {
    request = null;
    $('modal').hidden = true;
    clearInterval(requestTimer);
    return;
  }
  const deadline = Date.now() + r.expires_s * 1000;
  if (request && request.id === r.id) {
    request.deadline = deadline;
    return;
  }
  request = Object.assign({}, r, {deadline});
  const who = `<b>${esc(r.name)}</b> <span class="mono small muted">${esc(r.address)}</span>`;
  const code = r.passkey ? `<div class="code">${esc(r.passkey)}</div>` : '';
  let body, acts;
  const yesNo = (yes, no) => `<button class="primary" data-a="yes">${yes}</button><button data-a="no">${no}</button>`;
  switch (r.kind) {
    case 'confirm':
      body = `${who} wants to pair. Does it show the same code?${code}`;
      acts = yesNo('Same code: pair', 'Reject');
      break;
    case 'authorize':
      body = `${who} wants to pair (just works).`;
      acts = yesNo('Allow', 'Reject');
      break;
    case 'service':
      body = `${who} wants to use <b>${esc(r.uuid_name || r.uuid)}</b> <span class="mono small muted">${esc(r.uuid)}</span>.`;
      acts = yesNo('Allow', 'Reject');
      break;
    case 'pin':
      body = `${who} asks for a PIN code.`;
      acts = '<input id="modal-val" maxlength="16" placeholder="0000">' + yesNo('Send', 'Reject');
      break;
    case 'passkey':
      body = `${who} shows a six-digit passkey. Type it here:`;
      acts = '<input id="modal-val" maxlength="6" inputmode="numeric" placeholder="123456">' + yesNo('Send', 'Reject');
      break;
    default:  // display
      body = `Type this code on ${who}, then press Enter there.${code}`;
      acts = '<button data-a="yes">Dismiss</button>';
  }
  $('modal-title').textContent = r.kind === 'service' ? 'Profile authorization' : 'Pairing';
  $('modal-body').innerHTML = body;
  $('modal-actions').innerHTML = acts;
  $('modal').hidden = false;
  const v = $('modal-val');
  if (v) v.focus();
  $('modal-actions').querySelectorAll('button[data-a]').forEach(b => {
    b.onclick = () => {
      const req = {id: r.id, accept: b.dataset.a === 'yes'};
      if (v && req.accept) {
        if (r.kind === 'pin') req.pin = v.value.trim();
        else req.passkey = v.value.trim();
      }
      post('/bluetooth/request', req).then(() => {
        answered.add(r.id);
        renderRequest(null);
      }).catch(fail);
    };
  });
  clearInterval(requestTimer);
  const tick = () => {
    if (!request) return;
    const left = Math.max(0, Math.round((request.deadline - Date.now()) / 1000));
    $('modal-left').textContent = `${left} s left before BlueZ gives up`;
    if (left === 0) renderRequest(null);
  };
  tick();
  requestTimer = setInterval(tick, 1000);
}

// ---------------------------------------------------------------- start

function startConsole() {
  api('/system/health').then(renderHealthPills).catch(() => {});
  api('/bluetooth').then(renderBtPill).catch(() => {});
  refreshRequest();
  const want = location.hash.slice(1) || lsGet('btbench.tab') || 'devices';
  showTab(want);
  connectWs();
}
