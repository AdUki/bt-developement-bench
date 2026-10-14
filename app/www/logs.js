'use strict';
// System → Logs: one viewer for the journal (a unit, or the kernel) and btbench-logd's files in
// /tmp/logs. The board does the reading (GET /api/system/logs/view, logview.cpp): both kinds come
// as the same entries — time, priority, program, pid, topic, message — read from the end, filtered
// there so a search covers the whole log, and gzipped. The page colours them: by priority, the
// program in its own colour, and what people look for in a Bluetooth log picked out (addresses,
// D-Bus object paths, source locations, hex, failures), the filter's hits marked.
//
// The filter is words (all of them), !word to hide, /regex/, "a phrase", and a lowest priority.
// "Load older" asks for the page before the oldest shown. Following adds to it as it comes: the
// journal over the "journal" topic, a file by asking for what was appended (?after=) every 1.5 s;
// new lines are filtered here, with the same rules.

const LG_KEEP = 40000;  // entries kept in the page (two pages of the largest size)
const LG_DRAW = 1500;   // entries drawn at a time; scrolling to the top draws the next ones
// The first answer is small so it is on screen at once (0.1–0.3 s on the Zero W); the rest of what
// was asked for follows in bigger chunks, older first, while the newest are already readable.
const LG_FIRST = 1000;
const LG_CHUNK = 5000;

const lg = {
  src: lsGet('btbench.lg.src') || 'j:bluetooth.service',
  entries: [],          // oldest first: {ts, prio, who, pid, tid, topic, msg} (note: a marker line)
  units: [], files: [],
  before: null,         // where older entries start (a cursor or an offset); null: none
  end: null,            // a file: the offset to follow from
  limited: false,       // the board stopped searching (its time budget) before filling the page
  follow: false, gen: 0, busy: false,
  target: 1000,         // how many entries this view is filling up to
  scanned: 0,           // entries the board looked at for them (a search)
  fileTimer: null,
  prio: Number(lsGet('btbench.lg.prio')) || 7,
  q: '', filter: null, pending: false,
  drawn: 0,             // how many of the newest matches are drawn
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

const lgIsFile = () => lg.src.startsWith('f:');

// Opens a source (a select change, or System's shortcuts: kernel logs, a file's name).
function logsOpen(src, follow) {
  lg.src = src;
  lsSet('btbench.lg.src', src);
  logsSources();
  if (follow != null) $('lg-follow').checked = follow;
  logsLoad();
}

const lgUrl = extra => `/system/logs/view?src=${encodeURIComponent(lg.src)}&q=${encodeURIComponent(lg.q)}&prio=${lg.prio}${extra}`;

// The newest page: on opening a source, and whenever the filter or the priority changes (the board
// searches the whole log for it).
function logsLoad() {
  const gen = ++lg.gen;
  clearInterval(lg.fileTimer);
  lg.fileTimer = null;
  lg.entries = [];
  lg.before = lg.end = null;
  lg.limited = false;
  lg.busy = true;
  lg.target = Number($('lg-lines').value);
  lg.scanned = 0;
  lg.took = null;
  setText($('lg-err'), '');
  // What was on screen is not what is coming: the view says it is on the way.
  lgRender(false);
  lg.t0 = performance.now();
  api(lgUrl(`&lines=${Math.min(lg.target, LG_FIRST)}`)).then(r => {
    if (gen !== lg.gen) return;
    lg.busy = false;
    lg.entries = r.entries;
    lg.before = r.before;
    lg.end = r.end;
    lg.limited = r.limited;
    lg.scanned = r.scanned;
    lg.drawn = 0;
    lgRender(false);
    logsFollow();
    lgFill(gen);
  }).catch(e => { if (gen === lg.gen) { lg.busy = false; lgError(e); } });
}

// Older entries until the view holds what was asked for (or the log has no more), a chunk at a
// time. A search the board cut short (its time budget) carries on the same way, so it covers the
// whole log; a new source, filter or reload stops it (gen).
function lgFill(gen) {
  if (gen !== lg.gen || lg.before == null || lg.entries.length >= lg.target) return lgDone(gen);
  lg.busy = true;
  lgStatus();
  const want = Math.min(LG_CHUNK, lg.target - lg.entries.length);
  api(lgUrl(`&lines=${want}&before=${encodeURIComponent(lg.before)}`)).then(r => {
    if (gen !== lg.gen) return;
    lg.busy = false;
    lg.entries = r.entries.concat(lg.entries);
    if (lg.entries.length > LG_KEEP) lg.entries.splice(LG_KEEP);
    lg.before = r.before;
    lg.limited = r.limited;
    lg.scanned += r.scanned;
    // Older lines are kept but drawn only when scrolled up to: the view stays where it is.
    lgRender(true, true);
    lgFill(gen);
  }).catch(e => { if (gen === lg.gen) { lg.busy = false; lgError(e); } });
}

function lgDone(gen) {
  if (gen !== lg.gen) return;
  lg.busy = false;
  lg.took = Math.round(performance.now() - lg.t0);
  lgStatus();
}

// "Load older": as many again, before the oldest held.
function logsOlder() {
  if (lg.before == null || lg.busy) return;
  lg.target = lg.entries.length + Number($('lg-lines').value);
  lg.t0 = performance.now();
  lg.drawn = Math.max(lg.drawn, Math.min(lg.entries.length, LG_KEEP)) + LG_DRAW;
  lgFill(lg.gen);
}

function lgError(e) {
  loaded($('lg-view'));
  setText($('lg-err'), e.message);
  lgStatus('');
}

// Follow: the journal through the topic (the board follows one unit for everyone), a file by
// asking for what was appended.
function logsFollow() {
  lg.follow = $('lg-follow').checked;
  clearInterval(lg.fileTimer);
  lg.fileTimer = null;
  if (lg.follow && !lgIsFile()) put('/system/journal', {unit: lg.src.slice(2)}).catch(fail);
  if (lg.follow && lgIsFile() && lg.end != null) {
    const gen = lg.gen;
    lg.fileTimer = setInterval(() => {
      if (currentTab !== 'system' || gen !== lg.gen || lg.end == null) return;
      api(lgUrl(`&after=${lg.end}`)).then(r => {
        if (gen !== lg.gen) return;
        lg.end = r.end;
        // Rotated (btbench-logd renames it to .1 and starts afresh): what came before stays.
        if (r.reset) lg.entries.push({prio: 5, msg: '— the file was rotated —', note: true});
        r.entries.forEach(e => lg.entries.push(e));
        if (r.entries.length || r.reset) lgAdd(null);
      }).catch(() => {});
    }, 1500);
  }
  updateTopics();
}

function logsTopics() {
  return lg.follow && !lgIsFile() && currentTab === 'system' ? ['journal'] : [];
}

function logsShow() {
  if (!lg.entries.length && !lg.busy) logsLoad();
}

function logsJournal(d) {
  if (!lg.follow || lgIsFile()) return;
  const u = lg.src.slice(2);
  // The topic follows the board's one unit: a message for another (someone else changed it)
  // does not belong here.
  if (u !== 'kernel' && d.unit && d.unit !== u) return;
  const e = {ts: d.ts, prio: d.prio, who: d.ident || d.unit, pid: d.pid, msg: d.msg};
  if (lgMatch(e, lg.filter)) lgAdd(e);
}

function lgAdd(e) {
  if (e) lg.entries.push(e);
  if (lg.entries.length > LG_KEEP) lg.entries.splice(0, lg.entries.length - LG_KEEP);
  // Redrawn at most once a frame, however many lines arrive in it.
  if (lg.pending) return;
  lg.pending = true;
  requestAnimationFrame(() => { lg.pending = false; lgRender(true); });
}

// ---------------------------------------------------------------- the filter (for followed lines)

// The same rules as the board's (logview.cpp LogFilter), for lines that arrive while following, and
// for marking hits.
function lgParse(src) {
  const terms = [];
  const re = /(!?)(?:"([^"]*)"|\/((?:\\.|[^/])+)\/([gimsuy]*)|(\S+))/g;
  let m;
  while ((m = re.exec(src))) {
    const neg = m[1] === '!';
    if (m[3] != null) {
      try {
        const icase = m[4].includes('i') || !/[A-Z]/.test(m[3]);
        terms.push({neg, re: new RegExp(m[3], (icase ? 'i' : '') + m[4].replace('i', ''))});
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

const lgText = e => `${e.who || ''}${e.pid ? '[' + e.pid + ']' : ''}${e.who || e.pid ? ': ' : ''}${e.topic ? e.topic + ' ' : ''}${e.msg}`;

function lgMatch(e, f) {
  if (e.note) return true;
  if (e.prio > lg.prio) return false;
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


const LG_PRIO = ['emerg', 'alert', 'crit', 'err', 'warning', 'notice', 'info', 'debug'];

// A debug build names its source files by where it was built: bluetoothd -d prints
// "/home/…/src/bluez/src/adapter.c:…", a Yocto build "/usr/src/debug/bluez5/5.86/src/adapter.c:…".
// Shown from the source tree on ("src/adapter.c"), which is what is read; the search still sees
// the whole path.
const lgShortSrc = m => m.replace(/(^|[\s(])\/(?:[^\s:]*\/)?(?:src|debug)\/(?:bluez5?|pipewire|wireplumber)(?:-[\d.]+)?\/(?:[\d.]+\/)?(?=[\w-]+\/)/g, '$1');

function lgLineHTML(e, marks) {
  const cls = `lg-line p${Math.min(7, Math.max(0, e.prio))}${e.note ? ' lg-note' : ''}`;
  if (e.note) return `<div class="${cls}">${esc(e.msg)}</div>`;
  let head = '';
  if (e.ts) {
    const ms = String(new Date(e.ts).getMilliseconds()).padStart(3, '0');
    head += `<span class="lg-ts">${fmtTime(e.ts)}.${ms}</span> `;
  }
  if (e.who) head += `<span class="${lgWhoClass(e.who)}">${lgMark(e.who, marks)}</span>`;
  if (e.pid) head += `<span class="lg-pid" title="${esc(e.tid ? 'pid.tid ' + e.tid : 'pid')}">[${e.tid || e.pid}]</span>`;
  if (head && !head.endsWith(' ')) head += ' ';
  if (e.topic) head += `<span class="lg-topic">${lgMark(e.topic, marks)}</span> `;
  return `<div class="${cls}" title="${LG_PRIO[e.prio] || e.prio}">${head}${lgColour(lgShortSrc(e.msg), marks)}</div>`;
}

// ---------------------------------------------------------------- drawing

// The newest `drawn` entries (at least LG_DRAW); scrolling to the top draws more, then a button
// asks the board for older ones.
function lgRender(keepPlace, older) {
  const view = $('lg-view');
  const atEnd = !keepPlace || view.scrollTop + view.clientHeight >= view.scrollHeight - 8;
  const fromBottom = view.scrollHeight - view.scrollTop;
  lg.drawn = Math.max(lg.drawn, LG_DRAW);
  const n = lg.entries.length;
  const first = Math.max(0, n - lg.drawn);
  const marks = lg.filter && lg.filter.terms.filter(t => !t.neg);
  const mk = marks && marks.length ? marks : null;
  let html = '';
  if (first > 0) html += `<div class="lg-more"><button class="small-btn" data-lg="draw">Show ${fmtNum(Math.min(LG_DRAW, first))} more of the ${fmtNum(first)} loaded above</button></div>`;
  else if (lg.before != null && !lg.busy) html += `<div class="lg-more"><button class="small-btn" data-lg="older">Load ${fmtNum(Number($('lg-lines').value))} older</button></div>`;
  for (let i = first; i < n; i++) html += lgLineHTML(lg.entries[i], mk);
  const search = lg.q || lg.prio < 7;
  if (!n) html += lg.busy ? `<div class="muted loading-line"><span class="dots"></span>${search ? 'Searching…' : 'Loading…'}</div>`
    : `<div class="muted">${search ? 'No line matches the filter.' : 'Nothing logged.'}</div>`;
  loaded(view);
  view.innerHTML = html;
  if (atEnd && !older) view.scrollTop = view.scrollHeight;
  else view.scrollTop = view.scrollHeight - fromBottom;  // the same line stays under the eye
  lgStatus();
}

function lgStatus(text) {
  if (text != null && text !== '') return setText($('lg-count'), text);
  const n = lg.entries.length;
  const search = lg.q || lg.prio < 7;
  const what = search ? 'matching lines' : 'lines';
  const back = n && lg.entries[0].ts ? ` since ${fmtTime(lg.entries[0].ts)}` : '';
  if (lg.busy) {
    setText($('lg-count'), `${fmtNum(n)} ${what}${back} · ${search ? `searching (${fmtNum(lg.scanned)} looked at)` : `loading up to ${fmtNum(lg.target)}`}…`);
    return;
  }
  setText($('lg-count'), `${fmtNum(n)} ${what}${back}${lg.before == null ? ' (the whole log)' : ''}` +
    `${search && lg.scanned ? ` · ${fmtNum(lg.scanned)} looked at` : ''}${lg.took != null ? ` · ${lg.took} ms` : ''}`);
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
  $('lg-lines').value = ['200', '1000', '5000', '20000'].includes(lsGet('btbench.lg.lines')) ? lsGet('btbench.lg.lines') : '1000';
  $('lg-lines').onchange = () => { lsSet('btbench.lg.lines', $('lg-lines').value); logsLoad(); };
  $('lg-reload').onclick = logsLoad;
  $('lg-clear').onclick = () => { lg.entries = []; lg.drawn = 0; lgRender(false); };
  $('lg-follow').onchange = logsFollow;
  $('lg-wrap').onchange = e => $('lg-view').classList.toggle('wrap', e.target.checked);
  $('lg-prio').onclick = e => {
    const b = e.target.closest('[data-prio]');
    if (!b) return;
    lgSetPrio(Number(b.dataset.prio));
    logsLoad();
  };
  $('lg-view').onclick = e => {
    const b = e.target.closest('[data-lg]');
    if (!b) return;
    if (b.dataset.lg === 'draw') {
      lg.drawn += LG_DRAW;
      lgRender(true, true);
    } else {
      logsOlder();
    }
  };
  // At the top: the next lines that are loaded but not drawn come by themselves.
  $('lg-view').addEventListener('scroll', () => {
    const v = $('lg-view');
    if (v.scrollTop < 40 && lg.entries.length > lg.drawn) {
      lg.drawn += LG_DRAW;
      lgRender(true, true);
    }
  }, {passive: true});
  let t = null;
  $('lg-filter').value = lsGet('btbench.lg.filter') || '';
  const apply = load => {
    const src = $('lg-filter').value.trim();
    const f = lgParse(src);
    setText($('lg-err'), f.error || '');
    $('lg-filter').classList.toggle('bad', !!f.error);
    if (f.error) return;
    lsSet('btbench.lg.filter', src);
    lg.filter = f;
    if (src === lg.q) return;
    lg.q = src;
    // The board searches the whole log: asked a moment after the last key.
    if (load) logsLoad();
  };
  $('lg-filter').oninput = () => { clearTimeout(t); t = setTimeout(() => apply(true), 350); };
  $('lg-filter').onkeydown = e => {
    if (e.key === 'Enter') { clearTimeout(t); apply(true); }
    if (e.key === 'Escape') { $('lg-filter').value = ''; clearTimeout(t); apply(true); }
  };
  apply(false);
}

setupLogs();
