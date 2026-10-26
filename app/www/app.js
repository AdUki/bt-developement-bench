'use strict';
// The console's core: API helpers, the navigation, the one WebSocket and the pairing modal. Each
// tab lives in its own file and registers itself with registerTab(); only the tab on screen gets its
// topics and runs its polls, because the board is a single-core Zero W and a console left open in
// a background tab must cost it nothing — unless Graphs is told to record in the background.

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

// Waiting for the board: a Zero W just booted takes seconds to answer a tab's first read. A box
// marked "ld" shows three dots while it is empty, and its first write (even an empty one: nothing
// to show is an answer too) ends that; a group marked "pending" has its switches and buttons
// greyed out, since what they show until then is the page's default, not the board's state.
// loaded() ends both.
const loaded = el => {
  if (el && (el.classList.contains('ld') || el.classList.contains('pending'))) el.classList.remove('ld', 'pending');
};
// A first read that failed: a box still waiting says why instead of waiting on. A box that has
// shown an answer keeps it (a later refresh failing is the toast's).
function ldFail(e, ...els) {
  const msg = esc(e && e.message || e);
  for (const el of els) {
    if (!el || !el.classList.contains('ld')) continue;
    setHTML(el, el.tagName === 'TABLE' ? `<tr><td class="small bad-text">${msg}</td></tr>` : `<div class="small bad-text">${msg}</div>`);
  }
}

// Periodic updates write only what changed: rewriting an element with the same content still
// relayouts the page, and Chrome closes an open <select> when it does.
function setHTML(el, html) {
  if (!el) return;
  loaded(el);
  if (el._html !== html) {
    el._html = html;
    el.innerHTML = html;
  }
}
const setText = (el, t) => {
  if (!el) return;
  loaded(el);
  if (el.textContent !== t) el.textContent = t;
};
const setClass = (el, c) => { if (el && el.className !== c) el.className = c; };
// Inputs are updated from the daemon only while the user is not in them.
const idle = el => document.activeElement !== el;

// A <select>'s options from [{value, label}], keeping the choice (or `keep`) when it is still one.
function fillSelect(sel, opts, keep) {
  const want = keep != null ? keep : sel.value;
  const html = opts.map(o => `<option value="${esc(o.value)}">${esc(o.label)}</option>`).join('');
  setHTML(sel, html);
  if ([...sel.options].some(o => o.value === want)) sel.value = want;
}

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

// Repeat and shuffle as two toggle buttons, for every player (a phone's over AVRCP, the board's
// own, a playlist the board plays). `repeat` is off | all | one, `shuffle` a bool, either null when
// the player has no such setting (no button then). `cls` is the transport row's button class;
// `attrs` what the row's click handler needs to tell the players apart. A press sets the next
// value: repeat goes off → all → one → off.
const MODE_SVG = {
  repeat: 'M17 2l3 3-3 3M3 11V9a4 4 0 014-4h13M7 22l-3-3 3-3M21 13v2a4 4 0 01-4 4H4',
  shuffle: 'M16 3h5v5M4 20L21 3M21 16v5h-5M15 15l6 6M4 4l5 5',
};
const nextRepeat = r => r === 'off' ? 'all' : r === 'all' ? 'one' : 'off';
function modeButtons(cls, repeat, shuffle, attrs) {
  const b = (mode, on, label, extra) => `<button class="${cls} mode${on ? ' on' : ''}" data-mode="${mode}" ${attrs || ''}
    title="${label}" aria-label="${label}" aria-pressed="${on}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="${MODE_SVG[mode]}"/></svg>${extra || ''}</button>`;
  return (repeat != null ? b('repeat', repeat !== 'off', repeat === 'one' ? 'Repeat this track' : repeat === 'all' ? 'Repeat all' : 'Repeat off',
    repeat === 'one' ? '<b class="mode-one">1</b>' : '') : '') +
    (shuffle != null ? b('shuffle', shuffle, shuffle ? 'Shuffle on' : 'Shuffle off') : '');
}
// BlueZ's MediaPlayer1 values (a phone's player) ↔ the buttons'.
const btRepeat = v => v == null ? null : v === 'singletrack' ? 'one' : v === 'off' ? 'off' : 'all';
const btRepeatValue = r => r === 'one' ? 'singletrack' : r === 'all' ? 'alltracks' : 'off';
// The board's (a stream's playlist, its own player): off | track | all.
const boardRepeat = v => v === 'track' ? 'one' : v === 'all' ? 'all' : 'off';
const boardRepeatValue = r => r === 'one' ? 'track' : r;

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
      logRender(el);
    });
  }
}

// What a log shows. On a desktop a log does not scroll inside the scrolling page: one marked
// data-tail shows only its newest lines (all of them once its card is full screen, or where it
// is the list of a view that fills the window, which scroll there), any other shows them all and
// the page holds them (style.css).
const LOG_DESK = window.matchMedia('(min-width: 901px) and (min-height: 560px)');
function logRender(el) {
  const lines = el._lines || [];
  const scrolls = !LOG_DESK.matches || el.closest('.is-full, .fit-main');
  const tail = !scrolls && el.dataset.tail ? Number(el.dataset.tail) : 0;
  const atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 4;
  el.textContent = (tail && lines.length > tail ? [`… ${lines.length - tail} earlier lines (full screen shows them)`].concat(lines.slice(-tail)) : lines).join('\n');
  if (atEnd) el.scrollTop = el.scrollHeight;
}
LOG_DESK.addEventListener('change', () => document.querySelectorAll('pre.log').forEach(el => { if (el._lines) logRender(el); }));
function logClear(el) {
  el._lines = [];
  el.textContent = '';
}

// ---------------------------------------------------------------- tabs and navigation

const tabs = {};
let currentTab = null;

// def: {topics(): [...], background(): [...], show(), hide(), onMessage(topic, data)}; all optional.
// background() names the topics a tab wants even while another one is on screen: only Graphs
// asks, and only while its recording is switched on, so the samples before an issue are there.
function registerTab(name, def) { tabs[name] = def; }

// The areas in the rail and the views in each. A view is a tab (what owns topics and polls) and
// optionally one pane of its panel: panes only change what is on screen, so moving between the
// panes of one tab costs the board nothing, while a tab of its own (packets) starts and stops its
// stream with it. The hash names the view, "#audio/streams".
const NAV = [
  {id: 'listen', label: 'Listen', icon: 'M12 21a9 9 0 100-18 9 9 0 000 18zM10 8.5v7l6-3.5z', views: [
    {tab: 'player', pane: 'now', label: 'Now playing'},
    {tab: 'player', pane: 'inputs', label: 'Inputs'},
    {tab: 'player', pane: 'library', label: 'Library'},
    {tab: 'player', pane: 'queue', label: 'Up next'}]},
  {id: 'devices', label: 'Devices', icon: 'M7 7l10 10-5 5V2l5 5L7 17', views: [
    {tab: 'devices', label: 'Devices'},
    {tab: 'hid', label: 'Input devices'}]},
  {id: 'audio', label: 'Audio', icon: 'M4 14v-2a8 8 0 0116 0v2M4 14h3v6H4zM17 14h3v6h-3z', views: [
    {tab: 'audio', pane: 'bt', label: 'Bluetooth'},
    {tab: 'leaudio', label: 'LE Audio'},
    {tab: 'audio', pane: 'players', label: 'Players'},
    {tab: 'audio', pane: 'streams', label: 'Streams'},
    {tab: 'audio', pane: 'stack', label: 'PipeWire'}]},
  {id: 'le', label: 'LE', icon: 'M12 13a1 1 0 100-2 1 1 0 000 2zM8.5 8.5a5 5 0 000 7M15.5 8.5a5 5 0 010 7M5.6 5.6a9 9 0 000 12.8M18.4 5.6a9 9 0 010 12.8', views: [
    {tab: 'le', pane: 'explore', label: 'GATT explorer'},
    {tab: 'le', pane: 'adv', label: 'Advertise'},
    {tab: 'le', pane: 'server', label: 'GATT server'},
    {tab: 'remote', label: 'Remote control'}]},
  {id: 'files', label: 'Files', icon: 'M4 6a2 2 0 012-2h4l2 2h6a2 2 0 012 2v10a2 2 0 01-2 2H6a2 2 0 01-2-2zM12 10v6M9 13l3 3 3-3', views: [
    {tab: 'obex', pane: 'share', label: 'Share'},
    {tab: 'obex', pane: 'sessions', label: 'Sessions'},
    {tab: 'obex', pane: 'files', label: 'On the board'}]},
  {id: 'traffic', label: 'Traffic', icon: 'M3 12h4l3-8 4 16 3-8h4', views: [
    {tab: 'monitor', label: 'Links'},
    {tab: 'packets', label: 'Packets'},
    {tab: 'dbus', label: 'D-Bus'},
    {tab: 'logs', label: 'Logs'},
    {tab: 'graphs', label: 'Graphs'},
    {tab: 'capture', label: 'Captures'}]},
  // The board itself: its state and tools first, then what it is set up to be. Setup's settings
  // were an area of their own; one area keeps the rail short on a phone, and the old links
  // (#setup/profiles) still land, since a view is found by its tab.
  {id: 'system', label: 'System', icon: 'M4 5h16v10H4zM9 19h6M12 15v4', views: [
    {tab: 'system', pane: 'overview', label: 'Overview'},
    {tab: 'system', pane: 'tools', label: 'Tools'},
    {tab: 'setup', pane: 'profiles', label: 'Profiles'},
    {tab: 'setup', pane: 'bluetooth', label: 'Bluetooth settings'},
    {tab: 'setup', pane: 'audio', label: 'Audio settings'},
    {tab: 'setup', pane: 'plugins', label: 'Plugins'},
    {tab: 'wifi', label: 'Wi-Fi'},
    {tab: 'kernel', label: 'Kernel'}]},
];
const viewKey = v => v.tab + (v.pane ? '/' + v.pane : '');
const areaOf = tab => NAV.find(a => a.views.some(v => v.tab === tab)) || NAV[0];
const panes = {};  // tab → the pane on screen

const icon = d => `<svg viewBox="0 0 24 24" aria-hidden="true"><path d="${d}"/></svg>`;

function buildNav() {
  $('areas').innerHTML = NAV.map(a => `<a href="#${viewKey(a.views[0])}" class="area" data-area="${a.id}">${icon(a.icon)}<span>${a.label}</span></a>`).join('');
  $('areas').onclick = e => {
    const el = e.target.closest('[data-area]');
    if (!el) return;
    e.preventDefault();
    // An area opens on the view last used in it.
    const a = NAV.find(x => x.id === el.dataset.area);
    go(lsGet('btbench.area.' + a.id) || viewKey(a.views[0]));
  };
  $('views').onclick = e => {
    const el = e.target.closest('[data-view]');
    if (!el) return;
    e.preventDefault();
    go(el.dataset.view);
  };
}

function renderNav() {
  const area = areaOf(currentTab);
  const here = viewKey({tab: currentTab, pane: panes[currentTab]});
  document.querySelectorAll('#areas .area').forEach(el => el.classList.toggle('active', el.dataset.area === area.id));
  setText($('area-title'), area.label);
  setHTML($('views'), area.views.length > 1 ? area.views.map(v =>
    `<a href="#${viewKey(v)}" class="view" data-view="${viewKey(v)}">${esc(v.label)}</a>`).join('') : '');
  $('views').querySelectorAll('.view').forEach(el => el.classList.toggle('active', el.dataset.view === here));
  // On a phone the bar scrolls: System's eight views do not fit, so bring the one shown into sight
  // (the bar only; scrollIntoView would move the page too).
  const bar = $('views'), cur = bar.querySelector('.view.active');
  if (cur && bar.scrollWidth > bar.clientWidth) {
    const l = cur.offsetLeft - bar.offsetLeft, r = l + cur.offsetWidth;
    if (l < bar.scrollLeft || r > bar.scrollLeft + bar.clientWidth) bar.scrollLeft = l - (bar.clientWidth - cur.offsetWidth) / 2;
  }
  lsSet('btbench.area.' + area.id, here);
}

// Views that moved to a tab of their own, so their old links (and an area's remembered view) land.
const MOVED = {'system/logs': 'logs'};

// Shows a view by its key ("audio/streams", "capture"), keeping the hash in step. Each view shown is
// a history entry, so a phone's Back returns to the view before instead of leaving the console;
// `replace` (the first view, a hash typed by hand) takes the current entry's place.
function go(key, replace) {
  key = MOVED[key] || key;
  const [tab, pane] = key.split('/');
  const same = key === location.hash.slice(1);
  showTab(tab, pane);
  const state = {layer: backLayers.length};
  if (replace || same) history.replaceState(state, '', '#' + key);
  else history.pushState(state, '', '#' + key);
}

const viewNow = () => viewKey({tab: currentTab, pane: panes[currentTab]});

// ---------------------------------------------------------------- the back button

// What opens over a view and Back should close first — a card made full screen, a folder opened in
// the library — is a layer: one history entry each, {layer: depth}, with the function that closes
// it. Back pops entries: popstate closes the layers above the entry's depth, then shows the
// entry's view. A layer the page closes itself (its button, Esc, a breadcrumb) takes its entries
// back out of the history with backClose, whose own popstate is then ignored.
const backLayers = [];  // {close}, oldest first
let backSkip = 0;       // popstates of our own history.go(), not the user's

function backOpen(close) {
  const layer = {close};
  backLayers.push(layer);
  history.pushState({layer: backLayers.length}, '', location.hash);
  return layer;
}

// The page closed `layer` itself: it and the layers opened after it leave the stack and the history,
// without their close functions (the page has already put itself in the state it wants).
function backClose(layer) {
  const i = backLayers.indexOf(layer);
  if (i < 0) return;
  const n = backLayers.length - i;
  backLayers.splice(i);
  backSkip++;
  history.go(-n);
}

// The same without touching the history, for a layer closed by going to another view: the view's
// entry is pushed next, and the layer's entries under it are skipped on the way back (popstate).
function backForget(layer) {
  const i = backLayers.indexOf(layer);
  if (i >= 0) backLayers.splice(i);
}

window.addEventListener('popstate', e => {
  if (backSkip) { backSkip--; return; }
  const depth = (e.state && e.state.layer) || 0;
  while (backLayers.length > depth) backLayers.pop().close();
  const key = location.hash.slice(1);
  if (key && key !== viewNow()) showTab(...key.split('/'));
  // An entry of a layer forgotten meanwhile (backForget): nothing of it is left to close, so on to
  // the entry under it.
  if (depth > backLayers.length) {
    backSkip++;
    history.go(backLayers.length - depth);
  }
});

function showPane(name, pane) {
  const panel = $(name);
  const all = panel ? [...panel.querySelectorAll(':scope > .pane')] : [];
  if (!all.length) return;
  if (!all.some(p => p.dataset.pane === pane)) pane = panes[name] || lsGet('btbench.pane.' + name) || all[0].dataset.pane;
  if (!all.some(p => p.dataset.pane === pane)) pane = all[0].dataset.pane;
  panes[name] = pane;
  lsSet('btbench.pane.' + name, pane);
  all.forEach(p => { p.hidden = p.dataset.pane !== pane; });
  const t = tabs[name];
  if (t && t.pane) t.pane(pane);
}

function showTab(name, pane) {
  if (!tabs[name] && !$(name)) name = 'player';
  showPane(name, pane);
  if (fullCard) setFull(fullCard, false, true);
  if (currentTab !== name) {
    const old = tabs[currentTab];
    if (old && old.hide) old.hide();
    currentTab = name;
    document.querySelectorAll('.panel').forEach(p => p.classList.toggle('active', p.id === name));
    lsSet('btbench.tab', name);
    const t = tabs[name];
    if (t && t.show) t.show();
    updateTopics();
    window.scrollTo(0, 0);
  }
  // A pane can want topics of its own (Audio → PipeWire: pipewire), so a pane switch asks too.
  updateTopics();
  renderNav();
  updateFit();
}

// A hash typed or linked by hand (popstate has shown the view already when Back changed it).
window.addEventListener('hashchange', () => {
  const key = location.hash.slice(1);
  if (key && key !== viewNow()) go(key, true);
});

// ---------------------------------------------------------------- the WebSocket

// One socket for the whole console. What it carries is what the visible tab asks for, plus the
// pairing question and the health line in the header, which matter on every tab.
const ALWAYS = ['bt.request', 'system'];
let ws = null;
let wsBackoff = 500;
let wsTopics = '';

function wantedTopics() {
  const t = tabs[currentTab];
  const bg = Object.values(tabs).flatMap(x => x.background ? x.background() : []);
  return [...new Set(ALWAYS.concat(bg, t && t.topics ? t.topics() : []))];
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
    // BlueZ was slow to answer the last read (a Zero W scanning a busy room): what shows is its
    // last answer, not nothing.
    if (b.stale) { t = a.name + ' · slow to answer'; c = 'warn'; }
  }
  setText($('pill-bt'), t);
  setClass($('pill-bt'), 'pill ' + c);
}

function renderHealthPills(h) {
  if (!h) return;
  // The board's name in the tab too: with several boards open, the tabs tell them apart.
  if (h.hostname) { setText($('hostname'), h.hostname); document.title = h.hostname; }
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
  // On a board with two adapters, which of them it asks.
  const many = typeof btState !== 'undefined' && btState && (btState.adapters || []).length > 1;
  const who = `<b>${esc(r.name)}</b> <span class="mono small muted">${esc(r.address)}${many && r.adapter ? ' → ' + esc(r.adapter) : ''}</span>`;
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

// ---------------------------------------------------------------- filter groups

// The groups of a packet filter: "a b || c" is either group (docs/monitor.md "Filter syntax").
const splitGroups = f => f.split(/\s+\|\|\s+/).map(g => g.trim()).filter(Boolean);

// ---------------------------------------------------------------- one scrollbar on a desktop

// A view built around one long list (packets, D-Bus, the log, an open capture) fills the window
// on a desktop: the page stops scrolling and only the list does, instead of a scrolling box inside
// a scrolling page. A view marks itself with class "fit" (its panel, or a pane of it) and the card
// that holds the list with "fit-main"; the CSS does the rest, and only on a screen big enough.
function updateFit() {
  document.body.classList.toggle('fit', !!document.querySelector('.panel.active.fit, .panel.active > .pane.fit:not([hidden])'));
  // Listen on a wide screen: what plays and the library side by side, each column scrolling
  // (style.css; a narrower screen keeps them as two views).
  document.body.classList.toggle('fit-player', currentTab === 'player');
}

// ---------------------------------------------------------------- full screen

// A card marked data-full gets a button that lays it over the whole window, and Esc or the button
// puts it back: the packet tables, the D-Bus and log views, the library and each graph are boxes
// in the page's flow, and reading one wants all the room there is. A resize event follows, for
// the tables that draw only their visible rows and the canvases that draw at their size.
const FULL_SVG = {on: 'M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5', off: 'M9 4v5H4M15 4v5h5M9 20v-5H4M15 20v-5h5'};
let fullCard = null;

// Full screen is a layer (Back puts the card back). `leaving`: put back because another view is
// shown, whose history entry replaces the layer's.
function setFull(card, on, leaving) {
  if (on && fullCard && fullCard !== card) setFull(fullCard, false, leaving);
  if (on && !card._back) card._back = backOpen(() => { card._back = null; setFull(card, false); });
  if (!on && card._back) {
    const layer = card._back;
    card._back = null;
    if (leaving) backForget(layer); else backClose(layer);
  }
  card.classList.toggle('is-full', on);
  document.body.classList.toggle('has-full', on);
  fullCard = on ? card : null;
  const b = card.querySelector(':scope .full-btn');
  if (b) {
    b.innerHTML = `<svg viewBox="0 0 24 24" aria-hidden="true"><path d="${on ? FULL_SVG.off : FULL_SVG.on}"/></svg>`;
    b.title = on ? 'Back to the page (Esc)' : 'Full screen';
    b.setAttribute('aria-pressed', String(on));
  }
  card.querySelectorAll('pre.log').forEach(el => { if (el._lines) logRender(el); });
  window.dispatchEvent(new Event('resize'));
}

function initFull(root) {
  for (const card of (root || document).querySelectorAll('.card[data-full]:not([data-full-ready])')) {
    card.dataset.fullReady = '1';
    const b = document.createElement('button');
    b.className = 'icon-btn full-btn';
    b.type = 'button';
    b.onclick = () => setFull(card, !card.classList.contains('is-full'));
    const head = card.querySelector(':scope > .card-head');
    const acts = head && head.querySelector('.acts');
    // In the head's actions, else at the end of the head or of the title (a graph's), else in the
    // corner.
    const title = !head && card.querySelector(':scope > h4, :scope > h3');
    if (acts) acts.append(b);
    else if (head) head.append(b);
    else if (title) { title.classList.add('with-full'); title.append(b); }
    else { card.classList.add('full-abs'); card.prepend(b); }
    setFull(card, false);
  }
}

document.addEventListener('keydown', e => {
  if (e.key === 'Escape' && fullCard && !document.querySelector('.modal:not([hidden])')) setFull(fullCard, false);
});

function startConsole() {
  initFull();
  api('/system/health').then(renderHealthPills).catch(() => {});
  api('/bluetooth').then(renderBtPill).catch(() => setClass($('pill-bt'), 'pill'));
  refreshRequest();
  buildNav();
  const t = lsGet('btbench.tab') || 'player';
  go(location.hash.slice(1) || (panes[t] ? t + '/' + panes[t] : t), true);
  $('boot').remove();
  connectWs();
}
