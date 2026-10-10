'use strict';
// System → Logs: one viewer for the journal (a unit, or the kernel) and btbench-logd's files in
// /tmp/logs (each program's own format). Lines are coloured by priority and by program, and what
// people look for in a Bluetooth log stands out: addresses, D-Bus object paths, source locations,
// hex, failures. The filter narrows what is shown, in the browser, over what was loaded: words
// (all of them), !word to hide, /regex/, "a phrase", and a lowest priority. Following a source
// keeps adding to it: the journal over the "journal" topic, a file by asking for what was
// appended (?from=) every second and a half.

const LG_KEEP = 5000;   // lines kept in the page
const LG_SHOW = 2000;   // lines drawn: the newest matches (a log of thousands is read from its end)

const lg = {
  src: lsGet('btbench.lg.src') || 'j:bluetooth.service',
  entries: [],          // {ts, prio, who, pid, msg} (journal) or {prio, msg, file: true}
  units: [], files: [],
  follow: false, gen: 0,
  fileAt: 0, fileTimer: null, rest: '',
  prio: Number(lsGet('btbench.lg.prio')) || 7,
  filter: null, pending: false,
};

// ---------------------------------------------------------------- sources

function logsUnits(units) {
  if (!units.length || lg.units.length) return;
  lg.units = units;
  logsSources();
}

function logsFiles(files) {
  const names = files.filter(f => !f.link && /\.log(\.1)?$/.test(f.name)).map(f => f.name);
  if (names.join() === lg.files.join()) return;
  lg.files = names;
  logsSources();
}

function logsSources() {
  const opt = (v, l) => `<option value="${esc(v)}"${v === lg.src ? ' selected' : ''}>${esc(l)}</option>`;
  setHTML($('lg-src'), `<optgroup label="Journal">${lg.units.map(u => opt('j:' + u, u.replace(/\.service$/, ''))).join('')}
      ${opt('j:kernel', 'kernel')}</optgroup>
    ${lg.files.length ? `<optgroup label="/tmp/logs">${lg.files.map(f => opt('f:' + f, f)).join('')}</optgroup>` : ''}`);
  if ($('lg-src').value !== lg.src && $('lg-src').options.length) $('lg-src').value = lg.src;
}

const lgUnit = () => lg.src.slice(2);
const lgIsFile = () => lg.src.startsWith('f:');

// Opens a source (a select change, or System's shortcuts: kernel logs, a file's name).
function logsOpen(src, follow) {
  lg.src = src;
  lsSet('btbench.lg.src', src);
  logsSources();
  if (follow != null) $('lg-follow').checked = follow;
  logsLoad();
}

function logsLoad() {
  const gen = ++lg.gen;
  clearInterval(lg.fileTimer);
  lg.fileTimer = null;
  lg.entries = [];
  lg.rest = '';
  setText($('lg-err'), '');
  setHTML($('lg-view'), '<div class="muted loading-line"><span class="spin"></span>Loading…</div>');
  // The journal is read entry by entry on the board: about 3 ms each on a Zero W.
  setText($('lg-count'), 'loading…');
  const n = $('lg-lines').value;
  if (lgIsFile()) {
    fetch(`/api/system/logs/${encodeURIComponent(lgUnit())}?tail=${n}`).then(async r => {
      if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.statusText);
      const text = await r.text();
      if (gen !== lg.gen) return;
      lg.fileAt = Number(r.headers.get('X-Size')) || 0;
      lgFileText(text, false);
      logsFollow();
    }).catch(e => { if (gen === lg.gen) lgError(e); });
  } else {
    api(`/system/journal?unit=${encodeURIComponent(lgUnit())}&lines=${n}`).then(r => {
      if (gen !== lg.gen) return;
      r.entries.forEach(e => lg.entries.push(lgFromJournal(e)));
      lgRender();
      logsFollow();
    }).catch(e => { if (gen === lg.gen) lgError(e); });
  }
}

function lgError(e) {
  setHTML($('lg-view'), '');
  setText($('lg-err'), e.message);
  setText($('lg-count'), '');
}

// Follow: the journal through the topic (the board follows one unit for everyone), a file by
// polling for what was appended.
function logsFollow() {
  lg.follow = $('lg-follow').checked;
  clearInterval(lg.fileTimer);
  lg.fileTimer = null;
  if (lg.follow && !lgIsFile()) put('/system/journal', {unit: lgUnit()}).catch(fail);
  if (lg.follow && lgIsFile()) {
    const gen = lg.gen;
    lg.fileTimer = setInterval(() => {
      if (currentTab !== 'system' || gen !== lg.gen) return;
      fetch(`/api/system/logs/${encodeURIComponent(lgUnit())}?from=${lg.fileAt}`).then(async r => {
        if (!r.ok || gen !== lg.gen) return;
        const text = await r.text();
        lg.fileAt = Number(r.headers.get('X-Size')) || lg.fileAt;
        // Rotated (btbench-logd renames it to .1 and starts afresh): what came before stays.
        if (r.headers.get('X-Reset')) lg.entries.push({prio: 5, msg: '— the file was rotated —', note: true});
        if (text) lgFileText(text, true);
      }).catch(() => {});
    }, 1500);
  }
  updateTopics();
}

function logsTopics() {
  return lg.follow && !lgIsFile() && currentTab === 'system' ? ['journal'] : [];
}

function logsShow() {
  if (!lg.entries.length) logsLoad();
}

function logsJournal(e) {
  if (!lg.follow || lgIsFile()) return;
  const u = lgUnit();
  // The topic follows the board's one unit: a message for another (someone else changed it)
  // does not belong here.
  if (u !== 'kernel' && e.unit && e.unit !== u) return;
  lgAdd(lgFromJournal(e));
}

// ---------------------------------------------------------------- lines

const lgFromJournal = e => ({ts: e.ts, prio: e.prio, who: e.ident || e.unit, pid: e.pid, msg: e.msg});

// A line of a btbench-logd file, in its program's own format (app/logd/format.cpp): the priority
// is read back from where that format puts it.
function lgPrio(line) {
  let m = /^\[([TDIWE])\]\[/.exec(line);                                      // PipeWire
  if (m) return {T: 7, D: 7, I: 6, W: 4, E: 3}[m[1]];
  m = /^([DIMWEC]) \d\d:\d\d:\d\d\./.exec(line);                               // WirePlumber
  if (m) return {D: 7, I: 6, M: 5, W: 4, E: 3, C: 2}[m[1]];
  m = /\b(DEBUG|INFO|NOTICE|WARN|WARNING|ERROR|FATAL|CRIT)\b/.exec(line.slice(0, 80));  // bluez, nsdk
  if (m) return {DEBUG: 7, INFO: 6, NOTICE: 5, WARN: 4, WARNING: 4, ERROR: 3, FATAL: 2, CRIT: 2}[m[1]];
  return 6;
}

function lgFileText(text, append) {
  const all = lg.rest + text;
  const lines = all.split('\n');
  lg.rest = lines.pop();  // a line still being written
  for (const l of lines) if (l) lg.entries.push({prio: lgPrio(l), msg: l, file: true});
  if (!append) lgRender();
  else lgAdd(null);
}

function lgAdd(e) {
  if (e) lg.entries.push(e);
  if (lg.entries.length > LG_KEEP) lg.entries.splice(0, lg.entries.length - LG_KEEP);
  // Redrawn at most once a frame, however many lines arrive in it.
  if (lg.pending) return;
  lg.pending = true;
  requestAnimationFrame(() => { lg.pending = false; lgRender(true); });
}

// ---------------------------------------------------------------- the filter

// Words must all appear (case-insensitive), !word must not, /re/ is a regex (its own flags, "i"
// by default), "a phrase" is one word with spaces.
function lgParse(src) {
  const terms = [];
  const re = /(!?)(?:"([^"]*)"|\/((?:\\.|[^/])+)\/([gimsuy]*)|(\S+))/g;
  let m;
  while ((m = re.exec(src))) {
    const neg = m[1] === '!';
    if (m[3] != null) {
      try {
        terms.push({neg, re: new RegExp(m[3], m[4].includes('i') || !/[A-Z]/.test(m[3]) ? 'i' + m[4].replace('i', '') : m[4])});
      } catch (e) {
        return {error: `bad regex /${m[3]}/: ${e.message}`};
      }
    } else {
      const w = (m[2] != null ? m[2] : m[5]).toLowerCase();
      if (w) terms.push({neg, word: w});
    }
  }
  return {terms};
}

function lgText(e) {
  return e.file ? e.msg : `${e.who || ''}${e.pid ? '[' + e.pid + ']' : ''}: ${e.msg}`;
}

function lgMatch(e, f) {
  if (e.prio > lg.prio && !e.note) return false;
  if (!f || !f.terms.length) return true;
  const t = lgText(e), low = t.toLowerCase();
  for (const term of f.terms) {
    const hit = term.re ? term.re.test(t) : low.includes(term.word);
    if (hit === term.neg) return false;
  }
  return true;
}

// ---------------------------------------------------------------- colour

// The things worth seeing at a glance, in one pass: each match becomes a span, the rest is
// escaped text. Order matters where they overlap (a source location before a plain word).
const LG_TOKENS = new RegExp([
  '(?<addr>\\b(?:[0-9A-Fa-f]{2}[:_]){5}[0-9A-Fa-f]{2}\\b)',
  '(?<path>/org/bluez/[\\w/]+|/org/freedesktop/[\\w/]+)',
  // file.c:123, file.c:123:func:, and bluetoothd's file.c:func() (a debug line's location).
  '(?<src>(?:/|\\b)[\\w./+-]+\\.(?:c|cc|cpp|h|lua):(?:\\d+(?::\\w+(?:\\(\\))?:?)?|\\w+\\(\\))|\\b\\w+\\(\\)(?=[ :]))',
  '(?<uuid>\\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\b)',
  '(?<hex>\\b0x[0-9a-fA-F]+\\b)',
  '(?<bad>\\b(?:[Ee]rror|ERROR|[Ff]ail(?:ed|ure|s)?|FAIL(?:ED)?|[Tt]imed? ?out|[Rr]efused|[Dd]enied|[Rr]ejected|[Uu]nable|[Cc]annot|[Ii]nvalid|xrun|[Uu]nderrun|[Oo]verrun|[Cc]rash(?:ed)?|[Ss]egfault)\\b)',
  '(?<warn>\\b(?:[Ww]arning|WARN(?:ING)?|[Dd]isconnect(?:ed)?|[Ll]ost|[Dd]ropp?(?:ed)?|[Rr]etry(?:ing)?)\\b)',
  '(?<good>\\b(?:[Cc]onnected|[Ss]uccess(?:ful)?|[Rr]eady|[Ss]tarted|[Pp]aired|[Ee]stablished)\\b)',
].join('|'), 'g');

function lgColour(text, marks) {
  let out = '', at = 0;
  LG_TOKENS.lastIndex = 0;
  for (let m; (m = LG_TOKENS.exec(text));) {
    if (!m[0]) { LG_TOKENS.lastIndex++; continue; }
    const kind = Object.keys(m.groups).find(k => m.groups[k] != null);
    // A packaged build's source path ("/usr/src/debug/bluez5/5.87+git/src/device.c") is shown as
    // the path in the source tree, as btbench-logd writes it; the whole one is in the tooltip.
    const shown = kind === 'src' ? m[0].replace(/^\/usr\/src\/debug\/[^/]+\/[^/]+\//, '') : m[0];
    out += lgMark(text.slice(at, m.index), marks) +
      `<span class="lg-${kind}"${shown !== m[0] ? ` title="${esc(m[0])}"` : ''}>${lgMark(shown, marks)}</span>`;
    at = m.index + m[0].length;
  }
  return out + lgMark(text.slice(at), marks);
}

// The filter's words and regexes, highlighted where they matched.
function lgMark(s, marks) {
  if (!s) return '';
  if (!marks) return esc(s);
  const ranges = [];
  for (const mk of marks) {
    if (mk.word) {
      const low = s.toLowerCase();
      for (let i = low.indexOf(mk.word); i >= 0; i = low.indexOf(mk.word, i + mk.word.length)) ranges.push([i, i + mk.word.length]);
    } else {
      const re = new RegExp(mk.re.source, mk.re.flags.includes('g') ? mk.re.flags : mk.re.flags + 'g');
      for (let m; (m = re.exec(s));) {
        if (!m[0]) { re.lastIndex++; continue; }
        ranges.push([m.index, m.index + m[0].length]);
      }
    }
  }
  if (!ranges.length) return esc(s);
  ranges.sort((a, b) => a[0] - b[0]);
  let out = '', at = 0;
  for (const [a, b] of ranges) {
    if (a < at) continue;
    out += esc(s.slice(at, a)) + '<mark>' + esc(s.slice(a, b)) + '</mark>';
    at = b;
  }
  return out + esc(s.slice(at));
}

// A program's name in its own colour: the same name always gets the same one.
function lgWhoClass(who) {
  let h = 0;
  for (let i = 0; i < who.length; i++) h = (h * 31 + who.charCodeAt(i)) | 0;
  return 'lg-who c' + (Math.abs(h) % 8);
}

// A btbench-logd line split into its parts (app/logd/format.cpp), each drawn like a journal
// line's: the time quiet, the program in its colour, the level as a tag, then the message.
const LG_FORMATS = [
  // bluez: bluetoothd[13077]: 20220309 12:06:48.280 DEBUG src/adapter.c:adapter_service_insert() ...
  [/^(bluetoothd)(\[\d+\]): (\d{8} [\d:.]+) ([A-Z]+) (.*)$/, m => ({who: m[1], pid: m[2], ts: m[3], lvl: m[4], msg: m[5]})],
  // nsdk: 20220403 11:35:59.169 [778.781] INFO Networking: Enter ... (system.log: "ident: " first)
  [/^(\d{8} [\d:.]+) (\[[\d.]+\]) ([A-Z]+) (?:([\w@.-]+(?:\[\d+\])?): )?(.*)$/, m => ({ts: m[1], pid: m[2], lvl: m[3], who: m[4], msg: m[5]})],
  // PipeWire: [I][10:44:56.111404] pw.conf      | [          conf.c:  423 conf_load()] ...
  [/^\[([A-Z])\]\[([\d:.]+)\] (\S+)\s*\| (.*)$/, m => ({lvl: m[1], ts: m[2], who: m[3], msg: m[4]})],
  // WirePlumber: I 16:45:19.152172    m-lua-scripting config.c:104:load_file: ...
  [/^([DIMWEC]) ([\d:.]+) +(\S+) (.*)$/, m => ({lvl: m[1], ts: m[2], who: m[3], msg: m[4]})],
];

function lgFileLine(line, marks) {
  for (const [re, f] of LG_FORMATS) {
    const m = re.exec(line);
    if (!m) continue;
    const p = f(m);
    return `<span class="lg-ts">${lgMark(p.ts, marks)}</span> ` +
      (p.who ? `<span class="${lgWhoClass(p.who)}">${lgMark(p.who, marks)}</span>` : '') +
      (p.pid ? `<span class="lg-pid">${lgMark(p.pid, marks)}</span>` : '') +
      (p.lvl ? ` <span class="lg-lvl">${esc(p.lvl)}</span> ` : ' ') + lgColour(p.msg, marks);
  }
  return lgColour(line, marks);
}

const LG_PRIO = ['emerg', 'alert', 'crit', 'err', 'warning', 'notice', 'info', 'debug'];

function lgLineHTML(e, marks) {
  const cls = `lg-line p${Math.min(7, Math.max(0, e.prio))}${e.note ? ' lg-note' : ''}`;
  if (e.file) return `<div class="${cls}">${lgFileLine(e.msg, marks)}</div>`;
  const t = new Date(e.ts);
  const ms = String(t.getMilliseconds()).padStart(3, '0');
  return `<div class="${cls}" title="${LG_PRIO[e.prio] || e.prio}"><span class="lg-ts">${fmtTime(e.ts)}.${ms}</span> ` +
    `<span class="${lgWhoClass(e.who || '')}">${esc(e.who || '')}</span>${e.pid ? `<span class="lg-pid">[${e.pid}]</span>` : ''} ` +
    `${lgColour(e.msg, marks)}</div>`;
}

// ---------------------------------------------------------------- drawing

function lgRender(keepPlace) {
  if (!lg.entries.length && $('lg-count').textContent === 'loading…') return;
  const view = $('lg-view');
  const atEnd = !keepPlace || view.scrollTop + view.clientHeight >= view.scrollHeight - 8;
  const f = lg.filter;
  const shown = [];
  for (let i = lg.entries.length - 1; i >= 0 && shown.length < LG_SHOW; i--) {
    if (lgMatch(lg.entries[i], f)) shown.push(lg.entries[i]);
  }
  shown.reverse();
  const marks = f && f.terms.filter(t => !t.neg);
  setHTML(view, shown.length ? shown.map(e => lgLineHTML(e, marks && marks.length ? marks : null)).join('')
    : `<div class="muted">${lg.entries.length ? 'No line matches the filter.' : 'Nothing logged.'}</div>`);
  const all = lg.entries.length;
  const filtered = (f && f.terms.length) || lg.prio < 7;
  setText($('lg-count'), filtered ? `${fmtNum(shown.length)}${shown.length === LG_SHOW ? '+' : ''} of ${fmtNum(all)} lines`
    : `${fmtNum(all)} lines${all > LG_SHOW ? ` (the newest ${fmtNum(LG_SHOW)} shown)` : ''}`);
  if (atEnd) view.scrollTop = view.scrollHeight;
}

const fmtNum = n => Number(n).toLocaleString();

function lgSetPrio(p) {
  lg.prio = p;
  lsSet('btbench.lg.prio', String(p));
  for (const b of $('lg-prio').querySelectorAll('[data-prio]')) b.classList.toggle('on', Number(b.dataset.prio) === p);
}

function setupLogs() {
  lgSetPrio(lg.prio);
  $('lg-src').onchange = e => logsOpen(e.target.value);
  $('lg-lines').onchange = logsLoad;
  $('lg-reload').onclick = logsLoad;
  $('lg-clear').onclick = () => { lg.entries = []; lgRender(); };
  $('lg-follow').onchange = logsFollow;
  $('lg-wrap').onchange = e => $('lg-view').classList.toggle('wrap', e.target.checked);
  $('lg-prio').onclick = e => {
    const b = e.target.closest('[data-prio]');
    if (!b) return;
    lgSetPrio(Number(b.dataset.prio));
    lgRender();
  };
  let t = null;
  $('lg-filter').value = lsGet('btbench.lg.filter') || '';
  const apply = () => {
    const src = $('lg-filter').value;
    lsSet('btbench.lg.filter', src);
    const f = lgParse(src);
    setText($('lg-err'), f.error || '');
    $('lg-filter').classList.toggle('bad', !!f.error);
    if (f.error) return;
    lg.filter = f;
    lgRender();
  };
  // As you type, a moment after the last key: a few thousand lines redraw in a few ms.
  $('lg-filter').oninput = () => { clearTimeout(t); t = setTimeout(apply, 150); };
  $('lg-filter').onkeydown = e => { if (e.key === 'Escape') { $('lg-filter').value = ''; apply(); } };
  apply();
}

setupLogs();
