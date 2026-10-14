'use strict';
// Traffic → D-Bus: BlueZ's D-Bus traffic as btbenchd's monitor sees it (dbus_log.h): every call to
// org.bluez or obexd with its arguments, the reply with the call it answers and its latency, the
// errors, the signals. The board keeps the last 2000 for the page and the whole log in
// /tmp/logs/bluez-dbus.log; this view reads the ring when it opens and then the "bluez.dbus"
// topic, which it holds only while it is on screen.
//
// Filtering is here, on what the page holds: words to match anywhere in a message (program,
// path, member, arguments), !word to hide, the kinds shown, and btbenchd's polling of the object
// tree, hidden unless asked for.

const DB_KEEP = 3000;

const db = {
  entries: [], seq: 0, paused: false, held: [], loaded: false,
  types: new Set(['call', 'return', 'signal', 'error']),
  open: new Set(),  // seq of the messages shown with their arguments in full
};

try {
  const kept = JSON.parse(lsGet('btbench.db.types') || 'null');
  if (Array.isArray(kept)) db.types = new Set(kept);
} catch (e) { /* the default: every kind */ }

const dbPolling = e => e.member === 'GetManagedObjects' || e.call === 'GetManagedObjects';

function dbWords() {
  const words = $('db-filter').value.toLowerCase().split(/\s+/).filter(Boolean);
  return {want: words.filter(w => !w.startsWith('!')), hide: words.filter(w => w.startsWith('!') && w.length > 1).map(w => w.slice(1))};
}

function dbText(e) {
  return [e.type, e.sender, e.sender_name, e.dest, e.dest_name, e.path, e.iface, e.member, e.call,
    e.error_name, e.error, typeof e.args === 'string' ? e.args : JSON.stringify(e.args)].join(' ').toLowerCase();
}

function dbShown(e, f) {
  if (!db.types.has(e.type)) return false;
  if (!$('db-poll').checked && dbPolling(e)) return false;
  if (!f.want.length && !f.hide.length) return true;
  const t = dbText(e);
  return f.want.every(w => t.includes(w)) && !f.hide.some(w => t.includes(w));
}

const dbWho = (name, addr) => name ? (addr && addr !== name ? `${esc(name)}<span class="lg-pid">(${esc(addr)})</span>` : esc(name)) : esc(addr || '');
const dbArgs = a => a == null ? '' : typeof a === 'string' ? a : (Array.isArray(a) && !a.length ? '' : JSON.stringify(a));
// PropertiesChanged, the commonest message, read as what changed: "Device1 RSSI=-58 Connected=true",
// and the properties it only says are stale as "−RSSI".
function dbProps(e) {
  const a = e.args;
  if (e.member !== 'PropertiesChanged' || !Array.isArray(a) || typeof a[0] !== 'string' || typeof a[1] !== 'object') return null;
  const set = Object.entries(a[1] || {}).map(([k, v]) => `${k}=${typeof v === 'string' ? v : JSON.stringify(v)}`);
  const gone = (Array.isArray(a[2]) ? a[2] : []).map(k => '−' + k);
  return `${a[0].replace(/^org\.(bluez|freedesktop\.DBus|mpris\.MediaPlayer2)\./, '')} ${set.concat(gone).join(' ')}`;
}

function dbRow(e) {
  const ms = String(e.ts % 1000).padStart(3, '0');
  const cls = e.type === 'error' ? ' p3' : e.type === 'signal' ? ' db-sig' : e.type === 'return' ? ' db-ret' : '';
  let what;
  if (e.type === 'call' || e.type === 'signal') {
    const iface = (e.iface || '').replace(/^org\.(freedesktop\.DBus|bluez)\./, '');
    what = `<span class="lg-path">${esc(e.path)}</span> <span title="${esc(e.iface)}">${esc(iface)}</span>.<b>${esc(e.member)}</b>`;
  } else {
    what = `<b>${esc(e.call || '?')}</b>${e.latency_ms != null ? ` <span class="${e.latency_ms >= 1000 ? 'lg-warn' : 'lg-ts'}">${e.latency_ms} ms</span>` : ''}` +
      (e.type === 'error' ? ` <span class="lg-bad">${esc(e.error_name)}</span>${e.error ? ': ' + esc(e.error) : ''}` : '');
  }
  const args = dbProps(e) || dbArgs(e.args);
  const full = db.open.has(e.seq);
  const argsHTML = !args ? '' : full
    ? `<pre class="db-full">${esc(typeof e.args === 'string' ? e.args : JSON.stringify(e.args, null, 2))}</pre>`
    : ` <span class="db-args">${esc(args.length > 220 ? args.slice(0, 220) + '…' : args)}</span>`;
  return `<div class="lg-line db-row${cls}${args ? ' db-has' : ''}" data-seq="${e.seq}">` +
    `<span class="lg-ts">${fmtTime(e.ts)}.${ms}</span> <span class="db-type db-${e.type}">${e.type}</span> ` +
    `<span class="lg-who">${dbWho(e.sender_name, e.sender)}</span>${e.dest ? ` → ${dbWho(e.dest_name, e.dest)}` : ''}  ${what}${argsHTML}</div>`;
}

function dbAtBottom() {
  const v = $('db-view');
  return v.scrollHeight - v.scrollTop - v.clientHeight < 40;
}

function dbScroll() {
  if ($('db-follow').checked) $('db-view').scrollTop = $('db-view').scrollHeight;
}

function dbRender() {
  const f = dbWords();
  const shown = db.entries.filter(e => dbShown(e, f));
  // The newest 1500 drawn: the page stays quick on a phone, and the log file has the rest.
  const draw = shown.slice(-1500);
  setHTML($('db-view'), draw.map(dbRow).join('') ||
    `<div class="lg-note">${db.loaded ? (db.entries.length ? 'Nothing matches.' : 'No messages yet: they appear here as BlueZ is used.') : '<span class="dots"></span>Loading…'}</div>`);
  dbScroll();
  dbStatus(shown.length);
}

function dbStatus(shown) {
  const n = shown != null ? shown : db.entries.filter(e => dbShown(e, dbWords())).length;
  setText($('db-status'), `${Number(n).toLocaleString()} of ${Number(db.entries.length).toLocaleString()} messages${db.paused ? ` · paused, ${Number(db.held.length).toLocaleString()} waiting` : ''}`);
}

function dbAdd(list) {
  if (!list.length) return;
  const f = dbWords();
  const follow = dbAtBottom();
  const fresh = list.filter(e => e.seq > db.seq);
  if (!fresh.length) return;
  db.seq = fresh[fresh.length - 1].seq;
  db.entries.push(...fresh);
  const over = db.entries.length - DB_KEEP;
  if (over > 0) db.entries.splice(0, over);
  if (currentTab !== 'dbus') return;
  const view = $('db-view');
  const html = fresh.filter(e => dbShown(e, f)).map(dbRow).join('');
  if (view.querySelector('.lg-note')) return dbRender();
  if (html) view.insertAdjacentHTML('beforeend', html);
  while (view.childElementCount > 1600) view.firstElementChild.remove();
  if (follow) dbScroll();
  dbStatus();
}

function dbLoad() {
  api(`/bluetooth/dbus?since=${db.seq}&limit=1000`).then(r => {
    db.loaded = true;
    if (!r.running && r.error) setText($('db-status'), 'Not watching: ' + r.error);
    dbAdd(r.entries || []);
    dbRender();
  }).catch(e => { db.loaded = true; ldFail(e, $('db-view')); setText($('db-status'), 'D-Bus log: ' + e.message); });
}

function setupDbus() {
  for (const b of $('db-types').querySelectorAll('[data-t]')) b.classList.toggle('on', db.types.has(b.dataset.t));
  $('db-types').onclick = e => {
    const b = e.target.closest('[data-t]');
    if (!b) return;
    if (db.types.has(b.dataset.t)) db.types.delete(b.dataset.t); else db.types.add(b.dataset.t);
    b.classList.toggle('on', db.types.has(b.dataset.t));
    lsSet('btbench.db.types', JSON.stringify([...db.types]));
    dbRender();
  };
  let t = null;
  $('db-filter').oninput = () => { clearTimeout(t); t = setTimeout(dbRender, 200); };
  $('db-poll').onchange = dbRender;
  $('db-follow').onchange = dbScroll;
  $('db-run').onclick = () => {
    db.paused = !db.paused;
    setText($('db-run'), db.paused ? 'Resume' : 'Pause');
    if (!db.paused) {
      const held = db.held;
      db.held = [];
      dbAdd(held);
    }
    dbStatus();
  };
  $('db-clear').onclick = () => {
    db.entries = [];
    db.held = [];
    db.open.clear();
    dbRender();
  };
  // A click on a message with arguments shows them in full (and folds them again).
  $('db-view').onclick = e => {
    const row = e.target.closest('.db-row.db-has');
    if (!row || window.getSelection().toString()) return;
    const seq = Number(row.dataset.seq);
    if (db.open.has(seq)) db.open.delete(seq); else db.open.add(seq);
    const entry = db.entries.find(x => x.seq === seq);
    if (entry) row.outerHTML = dbRow(entry);
  };
}

registerTab('dbus', {
  topics: () => ['bluez.dbus'],
  show() { dbLoad(); },
  onMessage(topic, d) {
    if (topic !== 'bluez.dbus' || currentTab !== 'dbus') return;
    if (db.paused) {
      db.held.push(...(d.entries || []));
      if (db.held.length > DB_KEEP) db.held.splice(0, db.held.length - DB_KEEP);
      return dbStatus();
    }
    dbAdd(d.entries || []);
  },
});
setupDbus();
