'use strict';
// Capture: the always-on btmon ring (btbench-btsnoop.service) in /data/btsnoop, and a packet
// viewer for its files. Files download as they are — btmon -r and Wireshark both read btsnoop —
// and "analyze" is btmon -a on the board. "view" opens the file in the viewer: a filter, a
// virtualised packet table, the decoded fields and hex of one packet, and a graph strip of the
// whole capture (docs/monitor.md "Packet view").
//
// The board indexes a file once and answers every request within a time budget. A request that
// could not finish (a big file being indexed, a text filter decoding summaries) says
// "complete": false; the viewer then asks again shortly and the totals grow. Everything is
// wrapped in one function: the tab files share the page's global scope.

(() => {
  let capState = null;
  let capPoll = null;

  // ------------------------------------------------------------ the ring

  function renderCapture(s) {
    capState = s;
    setText($('cap-state'), s.running ? 'recording' : s.state || 'stopped');
    setClass($('cap-state'), 'pill ' + (s.running ? 'good' : ''));
    setText($('cap-toggle'), s.running ? 'Stop' : 'Start');
    setText($('cap-dir'), `${s.unit} → ${s.dir}`);
    setHTML($('cap-files'), s.files.length ? `<tr><th>file</th><th>size</th><th>last written</th><th></th></tr>` +
      s.files.map(f => `<tr${f.name === cv.name ? ' class="sel"' : ''}><td class="mono">${esc(f.name)}${f.active ? ' ' + pill('writing', 'good') : ''}</td>
        <td>${fmtBytes(f.size)}</td><td>${fmtDate(f.mtime)}</td>
        <td><button class="small-btn" data-view="${esc(f.name)}">view</button>
          <a href="/api/capture/files/${encodeURIComponent(f.name)}" download>download</a>
          <button class="small-btn" data-an="${esc(f.name)}">analyze</button>
          ${f.active ? '' : `<button class="small-btn danger" data-rm="${esc(f.name)}">${armed['cap' + f.name] ? 'really?' : 'delete'}</button>`}</td></tr>`).join('')
      : '<tr><td class="muted">No captures.</td></tr>');
    const open = s.files.find(f => f.name === cv.name);
    if (cv.name && open) {
      cv.active = !!open.active;
      $('cv-follow-wrap').hidden = !cv.active;
      if (!cv.active && cv.follow) setFollow(false);
    }
  }

  const refreshCapture = () => api('/capture').then(s => { setText($('cap-msg'), ''); renderCapture(s); })
    .catch(e => setText($('cap-msg'), e.message));

  // ------------------------------------------------------------ the viewer: state

  const ROW = 22;     // px; fixed, so only the rows on screen exist in the page
  const PAGE = 200;   // rows per request
  const KEEP = 3000;  // rows kept in memory around the viewport

  // A few filters people reach for. Clicking one replaces the filter but keeps a time range.
  const CHIPS = [
    ['all', ''],
    ['HCI cmd/evt', 'type:cmd|evt !evt:0x13'],
    ['L2CAP sig', 'cid:1|5'],
    ['ATT', 'proto:att'],
    ['SMP', 'proto:smp'],
    ['AVDTP', 'proto:avdtp'],
    ['media', 'proto:rtp'],
    ['AVRCP', 'proto:avctp'],
    ['RFCOMM', 'proto:rfcomm'],
    ['errors', 'is:err'],
    ['marks + logs', 'type:log'],
    ['slow TX', 'lat>20'],
  ];

  const cv = {
    name: null, active: false, filter: '', gen: 0,
    rows: new Map(),       // match ordinal → packet
    total: 0, complete: true, info: null, inflight: false, error: '',
    selIdx: -1, selN: null, detail: null, hexSpans: [],
    graph: null, conn: '', graphGen: 0, drag: null, pendingSel: null,
    progressTimer: null, follow: false, followTimer: null,
  };

  const enc = encodeURIComponent;
  const base = () => `/capture/files/${enc(cv.name)}`;
  const tTerms = f => f.split(/\s+/).filter(w => /^t(>=|<=|>|<|=)/.test(w));
  const nonT = f => f.split(/\s+/).filter(w => w && !/^t(>=|<=|>|<|=)/.test(w)).join(' ');
  const fmtT = t => t == null ? '' : t.toFixed(6);
  const fmtN = n => Number(n).toLocaleString();

  // ------------------------------------------------------------ opening a file

  function openView(name) {
    cv.name = name;
    const f = capState && capState.files.find(x => x.name === name);
    cv.active = !!(f && f.active);
    $('cv').hidden = false;
    $('cv-follow-wrap').hidden = !cv.active;
    setText($('cv-title'), name);
    setFollow(false);
    cv.selIdx = -1;
    cv.selN = null;
    cv.conn = '';
    clearDetail();
    applyFilter($('cv-filter').value.trim(), false);
    if (capState) renderCapture(capState);
    $('cv').scrollIntoView({behavior: 'smooth', block: 'start'});
  }

  function closeView() {
    cv.name = null;
    cv.gen++;
    setFollow(false);
    clearTimeout(cv.progressTimer);
    $('cv').hidden = true;
    if (capState) renderCapture(capState);
  }

  // A new filter: drop the cached rows and, if a packet was selected, come back to it (or the
  // match after it) in the new list.
  function applyFilter(f, keepSel = true) {
    cv.filter = f;
    $('cv-filter').value = f;
    cv.gen++;
    cv.rows.clear();
    cv.total = 0;
    cv.complete = false;
    cv.error = '';
    cv.inflight = false;
    clearTimeout(cv.progressTimer);
    renderChips();
    $('cv-spacer').style.height = '0px';
    $('cv-body').scrollTop = 0;
    if (keepSel && cv.selN) jump({at_n: cv.selN}, true);
    else {
      cv.selIdx = -1;
      fetchRows(0);
    }
    fetchGraph();
  }

  function renderChips() {
    const cur = nonT(cv.filter);
    setHTML($('cv-chips'), CHIPS.map(([label, f]) =>
      `<button class="small-btn cv-chip${cur === f ? ' on' : ''}" data-chip="${esc(f)}">${esc(label)}</button>`).join('') +
      (tTerms(cv.filter).length ? '<button class="small-btn cv-chip on" data-unzoom="1">whole capture ✕</button>' : ''));
  }

  // ------------------------------------------------------------ fetching rows

  function query(extra) {
    return `${base()}/packets?filter=${enc(cv.filter)}&count=${PAGE}${extra}`;
  }

  function took(r) {
    cv.total = r.total;
    cv.complete = r.complete;
    cv.info = r;
    cv.error = '';
    $('cv-spacer').style.height = (cv.total * ROW) + 'px';
    r.packets.forEach((p, i) => cv.rows.set(r.start + i, p));
    if (cv.rows.size > KEEP) trimRows();
    renderStatus();
    // A row the keyboard moved to before it was loaded is selected when it arrives.
    if (cv.pendingSel != null && cv.rows.has(cv.pendingSel)) {
      const i = cv.pendingSel;
      cv.pendingSel = null;
      select(i, cv.rows.get(i));
    }
    // Not done yet (indexing a big file, or a text filter decoding): ask again, which advances
    // the scan; the rows already shown stay valid, matches are only ever appended.
    clearTimeout(cv.progressTimer);
    if (!r.complete) cv.progressTimer = setTimeout(() => { cv.inflight = false; fetchVisible(true); }, 150);
    else if (cv.graph && !cv.graph.complete) fetchGraph();
  }

  function fetchRows(start, force) {
    if (!cv.name || (cv.inflight && !force)) return;
    cv.inflight = true;
    const gen = cv.gen;
    api(query(`&start=${Math.max(0, start)}`)).then(r => {
      if (gen !== cv.gen) return;
      cv.inflight = false;
      took(r);
      renderRows();
    }).catch(e => {
      if (gen !== cv.gen) return;
      cv.inflight = false;
      showError(e);
    });
  }

  // Jump to a time (a click on the graph) or a frame (keeping the selection across filters).
  function jump(at, selectIt) {
    if (!cv.name) return;
    const gen = ++cv.gen;
    cv.inflight = true;
    const q = at.at_t != null ? `&at_t=${at.at_t}` : `&at_n=${at.at_n}`;
    api(query(q)).then(r => {
      if (gen !== cv.gen) return;
      cv.inflight = false;
      took(r);
      const body = $('cv-body');
      body.scrollTop = Math.max(0, r.start * ROW - body.clientHeight / 3);
      const p = r.packets[0];
      if (p && (selectIt || at.at_t != null) && (at.at_n == null || p.n >= at.at_n)) select(r.start, p);
      else cv.selIdx = -1;
      renderRows();
    }).catch(e => {
      if (gen !== cv.gen) return;
      cv.inflight = false;
      showError(e);
    });
  }

  function trimRows() {
    const first = Math.floor($('cv-body').scrollTop / ROW);
    for (const k of cv.rows.keys()) {
      if (k < first - KEEP / 2 || k > first + KEEP / 2) cv.rows.delete(k);
    }
  }

  function visibleRange() {
    const body = $('cv-body');
    const first = Math.max(0, Math.floor(body.scrollTop / ROW) - 5);
    const last = Math.min(cv.total, first + Math.ceil(body.clientHeight / ROW) + 10);
    return [first, last];
  }

  // Whatever is on screen and not loaded yet, from its first missing row.
  function fetchVisible(force) {
    const [first, last] = visibleRange();
    for (let i = first; i < last; i++) {
      if (!cv.rows.has(i)) return fetchRows(i - (i % 50), force);
    }
    if (force || !cv.complete) fetchRows(cv.complete ? first : Math.max(0, last - PAGE), true);
  }

  // At most once a frame however fast the scroll events come; the timer covers a browser that
  // throttles animation frames (a background window, a headless one).
  let rowsQueued = false;
  function queueRows() {
    if (rowsQueued) return;
    rowsQueued = true;
    const run = () => {
      if (!rowsQueued) return;
      rowsQueued = false;
      renderRows();
    };
    requestAnimationFrame(run);
    setTimeout(run, 100);
  }

  function renderRows() {
    const [first, last] = visibleRange();
    let html = '';
    let missing = false;
    for (let i = first; i < last; i++) {
      const p = cv.rows.get(i);
      if (!p) {
        missing = true;
        html += `<div class="cv-row cv-wait"><span>…</span></div>`;
        continue;
      }
      const cls = ['cv-row', 't-' + p.type, p.dir ? 'd-' + p.dir : '', p.err ? 'err' : '', p.mark ? 'mark' : '',
        i === cv.selIdx ? 'sel' : ''].join(' ');
      html += `<div class="${cls}" data-i="${i}" title="${esc(fmtTime(p.ts_ms))}.${String(Math.floor((p.ts_ms % 1000) * 1000)).padStart(6, '0').slice(0, 6)}">` +
        `<span>${p.n}</span><span>${fmtT(p.t)}</span><span class="cv-dir cv-c-d">${p.dir}</span>` +
        `<span class="cv-c-h">${p.handle == null ? '' : p.handle}</span><span>${p.proto || p.type}</span>` +
        `<span class="cv-c-l">${p.len}</span><span class="cv-sum">${esc(p.summary)}${p.lat_ms != null ? ` <i class="muted">${p.lat_ms} ms</i>` : ''}</span></div>`;
    }
    const rows = $('cv-rows');
    rows.style.transform = `translateY(${first * ROW}px)`;
    setHTML(rows, html || (cv.complete && !cv.error ? '<div class="cv-empty muted">No packets match.</div>' : ''));
    if (missing) fetchVisible(false);
    drawGraph();
  }

  function renderStatus() {
    const r = cv.info;
    if (!r) return;
    const bits = [cv.filter ? `${fmtN(cv.total)} matching of ${fmtN(r.packets_in_file)} packets`
      : `${fmtN(r.packets_in_file)} packets`];
    if (r.indexed_bytes < r.file_size && !r.complete) {
      bits.push(`indexing ${Math.floor(100 * r.indexed_bytes / Math.max(1, r.file_size))}%…`);
    } else if (!r.complete) {
      bits.push(`searching… ${Math.floor(100 * r.scanned / Math.max(1, r.packets_in_file))}%`);
    }
    if (r.datalink !== 2001) bits.push(r.datalink === 1002 ? 'H4 capture' : 'HCI capture');
    setText($('cv-status'), bits.join(' · '));
    setClass($('cv-status'), 'small ' + (r.complete ? 'muted' : 'warn-text'));
  }

  function showError(e) {
    cv.error = e.message;
    setText($('cv-status'), e.message);
    setClass($('cv-status'), 'small bad-text');
    if (e.status === 400) {
      cv.total = 0;
      $('cv-spacer').style.height = '0px';
      setHTML($('cv-rows'), '');
    }
  }

  // ------------------------------------------------------------ selection and detail

  function select(i, p) {
    cv.selIdx = i;
    cv.selN = p.n;
    queueRows();
    const gen = cv.gen, name = cv.name;
    api(`${base()}/packets/${p.n}`).then(d => {
      if (name !== cv.name || cv.selN !== p.n) return;
      renderDetail(d);
    }).catch(e => { if (gen === cv.gen) setHTML($('cv-fields'), `<div class="bad-text small">${esc(e.message)}</div>`); });
    drawGraph();
  }

  function clearDetail() {
    cv.detail = null;
    cv.hexSpans = [];
    setHTML($('cv-fields'), '<div class="muted small">Select a packet: click a row, or move with ↑ ↓.</div>');
    setHTML($('cv-hex'), '');
  }

  function fieldsHTML(list) {
    return (list || []).map(f => {
      const rng = f.len ? ` data-off="${f.off}" data-len="${f.len}"` : '';
      const val = f.value ? ` <span class="cv-v">${esc(f.value)}</span>` : '';
      if (f.children) {
        return `<details open><summary${rng}><b>${esc(f.name)}</b>${val}</summary>${fieldsHTML(f.children)}</details>`;
      }
      return `<div class="cv-f"${rng}><span class="muted">${esc(f.name)}</span>${val}</div>`;
    }).join('');
  }

  function hexHTML(hex) {
    const n = hex.length / 2;
    let out = '';
    for (let off = 0; off < n; off += 16) {
      let h = '', a = '';
      for (let i = off; i < off + 16; i++) {
        if (i < n) {
          const b = parseInt(hex.substr(i * 2, 2), 16);
          h += `<span data-o="${i}">${hex.substr(i * 2, 2)}</span>` + (i % 8 === 7 ? '  ' : ' ');
          a += b >= 0x20 && b < 0x7f ? esc(String.fromCharCode(b)) : '.';
        } else {
          h += '   ' + (i % 8 === 7 ? ' ' : '');
        }
      }
      out += `<span class="muted">${off.toString(16).padStart(4, '0')}</span>  ${h} <span class="muted">${a}</span>\n`;
    }
    return out;
  }

  function renderDetail(d) {
    cv.detail = d;
    const head = `<div class="cv-dhead"><b>#${d.n}</b> ${esc(d.summary)}</div>`;
    setHTML($('cv-fields'), head + fieldsHTML(d.fields));
    setHTML($('cv-hex'), d.hex ? hexHTML(d.hex) : '<span class="muted">(no bytes)</span>');
    cv.hexSpans = Array.from($('cv-hex').querySelectorAll('[data-o]'));
  }

  // Hovering a field lights up its bytes in the hex dump.
  function highlight(off, len) {
    cv.hexSpans.forEach((s, i) => s.classList.toggle('hl', off != null && i >= off && i < off + len));
  }

  function moveSel(delta) {
    if (!cv.total) return;
    let i = cv.selIdx < 0 ? 0 : Math.max(0, Math.min(cv.total - 1, cv.selIdx + delta));
    const body = $('cv-body');
    const top = i * ROW;
    if (top < body.scrollTop) body.scrollTop = top;
    else if (top + ROW > body.scrollTop + body.clientHeight) body.scrollTop = top + ROW - body.clientHeight;
    const p = cv.rows.get(i);
    if (p) select(i, p);
    else {
      cv.selIdx = i;  // loads with the next render; selected when it arrives
      cv.pendingSel = i;
      queueRows();
    }
  }

  // ------------------------------------------------------------ the graph strip

  function fetchGraph() {
    if (!cv.name) return;
    const gen = ++cv.graphGen;
    // About one bucket per 2 px: finer buckets would only draw as noise.
    const points = Math.max(50, Math.round(($('cv-graph').clientWidth || 1000) / 2));
    api(`${base()}/graph?filter=${enc(cv.filter)}&points=${points}${cv.conn ? '&conn=' + enc(cv.conn) : ''}`).then(g => {
      if (gen !== cv.graphGen) return;
      cv.graph = g;
      const sel = g.conn ? `${g.conn.index}:${g.conn.handle}` : '';
      setHTML($('cv-conn'), g.conns.length ? g.conns.map(c => {
        const k = `${c.index}:${c.handle}`;
        return `<option value="${k}"${k === sel ? ' selected' : ''}>hci${c.index}/${c.handle} ${esc(c.type)} ${esc(c.peer)} · ${fmtBytes(c.tx_bytes + c.rx_bytes)}</option>`;
      }).join('') : '<option value="">no connections</option>');
      setText($('cv-gscale'), `${g.bucket_ms >= 1000 ? g.bucket_ms / 1000 + ' s' : g.bucket_ms + ' ms'} buckets · ${g.from.toFixed(3)}–${g.to.toFixed(3)} s`);
      drawGraph();
      if (!g.complete) setTimeout(() => { if (gen === cv.graphGen) fetchGraph(); }, 400);
    }).catch(() => {
      if (gen !== cv.graphGen) return;
      cv.graph = null;
      drawGraph();
    });
  }

  const cssv = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

  function graphCtx(c) {
    const dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth, h = c.clientHeight;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
    }
    const g = c.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    return {g, w, h};
  }

  // Layout: packets per bucket (the filtered set) as bars on top; the chosen connection's TX/RX
  // throughput (left axis) and TX latency p50/p95 (right axis) below; time along the bottom.
  const GL = 46, GR = 44, BAR_H = 44, GAP = 8, AX = 16;

  function gx(t) {
    const g = cv.graph, c = $('cv-graph');
    const end = g.from + g.buckets * g.bucket_ms / 1000;
    return GL + (t - g.from) / (end - g.from) * (c.clientWidth - GL - GR);
  }
  function tAt(x) {
    const g = cv.graph, c = $('cv-graph');
    const end = g.from + g.buckets * g.bucket_ms / 1000;
    return g.from + Math.max(0, Math.min(1, (x - GL) / (c.clientWidth - GL - GR))) * (end - g.from);
  }

  function niceMax(v) {
    if (!(v > 0)) return 1;
    const p = Math.pow(10, Math.floor(Math.log10(v)));
    for (const m of [1, 2, 2.5, 5, 10]) if (v <= m * p) return m * p;
    return 10 * p;
  }

  function drawGraph() {
    const c = $('cv-graph');
    if (!c || $('cv').hidden || !c.clientWidth) return;
    const {g: x, w, h} = graphCtx(c);
    const G = cv.graph;
    x.font = '11px system-ui, sans-serif';
    x.fillStyle = cssv('--muted');
    if (!G || !G.buckets) {
      x.fillText(cv.name ? 'loading…' : '', GL, 20);
      return;
    }
    const pw = w - GL - GR;
    const bw = pw / G.buckets;
    const top2 = BAR_H + GAP, h2 = h - top2 - AX;
    const line = cssv('--line');

    // Bars: packets per bucket.
    const pk = G.filtered.pkts;
    const pmax = niceMax(Math.max(1, ...pk));
    x.fillStyle = cssv('--accent');
    x.globalAlpha = 0.55;
    for (let i = 0; i < pk.length; i++) {
      if (!pk[i]) continue;
      const bh = Math.max(1, pk[i] / pmax * (BAR_H - 4));
      x.fillRect(GL + i * bw, BAR_H - bh, Math.max(1, bw - (bw > 3 ? 1 : 0)), bh);
    }
    x.globalAlpha = 1;
    x.fillStyle = cssv('--muted');
    x.textAlign = 'right';
    x.fillText(fmtN(pmax), GL - 4, 10);
    x.fillText('pkts', GL - 4, BAR_H - 2);

    x.strokeStyle = line;
    x.lineWidth = 1;
    x.beginPath();
    x.moveTo(GL, BAR_H + 0.5); x.lineTo(w - GR, BAR_H + 0.5);
    x.moveTo(GL, top2 + h2 + 0.5); x.lineTo(w - GR, top2 + h2 + 0.5);
    x.stroke();

    // Throughput and latency of one connection.
    const C = G.conn;
    if (C) {
      const bmax = niceMax(Math.max(1000, ...C.tx_bps, ...C.rx_bps));
      const lvals = C.lat_p95.filter(v => v != null);
      const lmax = niceMax(Math.max(1, ...lvals));
      const series = (arr, max, color, dots) => {
        x.strokeStyle = color;
        x.fillStyle = color;
        x.lineWidth = 1.5;
        x.beginPath();
        let pen = false;
        for (let i = 0; i < arr.length; i++) {
          const v = arr[i];
          if (v == null) { pen = false; continue; }
          const px = GL + (i + 0.5) * bw, py = top2 + h2 - v / max * (h2 - 4);
          if (dots && bw > 4) x.fillRect(px - 1, py - 1, 2, 2);
          if (pen) x.lineTo(px, py); else x.moveTo(px, py);
          pen = true;
        }
        x.stroke();
      };
      series(C.rx_bps, bmax, cssv('--rx'));
      series(C.tx_bps, bmax, cssv('--tx'));
      series(C.lat_p50, lmax, cssv('--p50'), true);
      series(C.lat_p95, lmax, cssv('--p95'), true);
      x.fillStyle = cssv('--muted');
      x.textAlign = 'right';
      x.fillText(fmtKbps(bmax), GL - 4, top2 + 10);
      x.fillText('kbit/s', GL - 4, top2 + 22);
      x.textAlign = 'left';
      x.fillText(lmax + ' ms', w - GR + 4, top2 + 10);
      x.fillText('p50/95', w - GR + 4, top2 + 22);
    } else {
      x.fillStyle = cssv('--muted');
      x.textAlign = 'left';
      x.fillText('no connection data in this capture', GL + 6, top2 + h2 / 2);
    }

    // Time axis: about one label per 90 px.
    const end = G.from + G.buckets * G.bucket_ms / 1000;
    const step = niceMax((end - G.from) / Math.max(1, Math.floor(pw / 90)));
    x.fillStyle = cssv('--muted');
    x.textAlign = 'center';
    for (let t = Math.ceil(G.from / step) * step; t <= end + 1e-9; t += step) {
      const px = gx(t);
      x.fillText((step < 1 ? t.toFixed(Math.min(3, -Math.floor(Math.log10(step)))) : t.toFixed(0)) + ' s', px, h - 3);
      x.strokeStyle = line;
      x.beginPath(); x.moveTo(px + 0.5, top2 + h2); x.lineTo(px + 0.5, top2 + h2 + 3); x.stroke();
    }

    // The rows on screen as a band, the selected packet as a line.
    const [first, last] = visibleRange();
    const a = cv.rows.get(first), b = cv.rows.get(Math.max(first, last - 1));
    if (a && b) {
      x.fillStyle = cssv('--fg');
      x.globalAlpha = 0.07;
      const xa = gx(a.t), xb = Math.max(xa + 2, gx(b.t));
      x.fillRect(xa, 0, xb - xa, h - AX);
      x.globalAlpha = 1;
    }
    const sp = cv.selIdx >= 0 && cv.rows.get(cv.selIdx);
    if (sp && sp.t >= G.from && sp.t <= end) {
      x.strokeStyle = cssv('--fg');
      x.lineWidth = 1;
      x.beginPath(); x.moveTo(Math.round(gx(sp.t)) + 0.5, 0); x.lineTo(Math.round(gx(sp.t)) + 0.5, h - AX); x.stroke();
    }
    if (cv.drag && Math.abs(cv.drag.x1 - cv.drag.x0) > 3) {
      x.fillStyle = cssv('--accent');
      x.globalAlpha = 0.18;
      x.fillRect(Math.min(cv.drag.x0, cv.drag.x1), 0, Math.abs(cv.drag.x1 - cv.drag.x0), h - AX);
      x.globalAlpha = 1;
    }
  }

  function readout(px) {
    const G = cv.graph;
    if (!G || px == null) return setText($('cv-readout'), '');
    const i = Math.floor((px - GL) / ((($('cv-graph').clientWidth) - GL - GR) / G.buckets));
    if (i < 0 || i >= G.buckets) return setText($('cv-readout'), '');
    const t = G.from + i * G.bucket_ms / 1000;
    const bits = [`${t.toFixed(3)} s`, `${fmtN(G.filtered.pkts[i])} pkts`];
    const C = G.conn;
    if (C) {
      bits.push(`TX ${fmtKbps(C.tx_bps[i])} RX ${fmtKbps(C.rx_bps[i])} kbit/s`);
      if (C.lat_p95[i] != null) bits.push(`lat p50 ${C.lat_p50[i]} p95 ${C.lat_p95[i]} max ${C.lat_max[i]} ms`);
    }
    setText($('cv-readout'), bits.join(' · '));
  }

  // A drag selects a time range (it becomes t>= t< terms of the filter); a click jumps the table.
  function setupGraph() {
    const c = $('cv-graph');
    const px = e => e.clientX - c.getBoundingClientRect().left;
    c.addEventListener('pointerdown', e => {
      if (!cv.graph) return;
      c.setPointerCapture(e.pointerId);
      cv.drag = {x0: px(e), x1: px(e)};
    });
    c.addEventListener('pointermove', e => {
      readout(px(e));
      if (!cv.drag) return;
      cv.drag.x1 = px(e);
      drawGraph();
    });
    c.addEventListener('pointerleave', () => readout(null));
    c.addEventListener('pointerup', e => {
      if (!cv.drag) return;
      const d = cv.drag;
      cv.drag = null;
      d.x1 = px(e);
      if (Math.abs(d.x1 - d.x0) <= 3) {
        jump({at_t: Math.max(0, tAt(d.x0)).toFixed(6)});
        drawGraph();
        return;
      }
      const a = tAt(Math.min(d.x0, d.x1)), b = tAt(Math.max(d.x0, d.x1));
      const rest = nonT(cv.filter);
      applyFilter(`${rest ? rest + ' ' : ''}t>=${a.toFixed(3)} t<${b.toFixed(3)}`.trim());
    });
    c.addEventListener('pointercancel', () => { cv.drag = null; drawGraph(); });
  }

  // ------------------------------------------------------------ follow (the file being written)

  function setFollow(on) {
    cv.follow = on;
    $('cv-follow').checked = on;
    clearInterval(cv.followTimer);
    cv.followTimer = null;
    if (!on) return;
    let ticks = 0;
    cv.followTimer = setInterval(() => {
      if (!cv.name || cv.inflight) return;
      const body = $('cv-body');
      const atEnd = body.scrollTop + body.clientHeight >= body.scrollHeight - ROW * 2;
      const gen = cv.gen;
      cv.inflight = true;
      api(query(`&start=${Math.max(0, cv.total - 20)}`)).then(r => {
        if (gen !== cv.gen) return;
        cv.inflight = false;
        took(r);
        if (atEnd) body.scrollTop = body.scrollHeight;
        renderRows();
      }).catch(e => { cv.inflight = false; showError(e); });
      if (++ticks % 3 === 0) fetchGraph();
    }, 2000);
  }

  // ------------------------------------------------------------ wiring

  function setupCapture() {
    $('cap-toggle').onclick = () => put('/capture', {running: !(capState && capState.running)})
      .then(refreshCapture).catch(fail);
    $('cap-files').onclick = e => {
      const view = e.target.closest('button[data-view]');
      const an = e.target.closest('button[data-an]');
      const rm = e.target.closest('button[data-rm]');
      if (view) {
        openView(view.dataset.view);
      } else if (an) {
        const name = an.dataset.an;
        $('cap-analyze').hidden = false;
        setText($('cap-an-title'), `btmon -a ${name}`);
        setText($('cap-an-text'), 'analyzing… (a 16 MiB file takes a while on the Zero W)');
        apiText(`/capture/files/${encodeURIComponent(name)}/analyze`)
          .then(t => setText($('cap-an-text'), t)).catch(e2 => setText($('cap-an-text'), e2.message));
      } else if (rm) {
        const name = rm.dataset.rm;
        if (!twoClick('cap' + name, () => capState && renderCapture(capState))) return;
        if (name === cv.name) closeView();
        del(`/capture/files/${encodeURIComponent(name)}`).then(refreshCapture).catch(fail);
      }
    };
    $('cap-an-close').onclick = () => { $('cap-analyze').hidden = true; };

    $('cv-close').onclick = closeView;
    $('cv-filter').addEventListener('keydown', e => {
      if (e.key === 'Enter') applyFilter($('cv-filter').value.trim());
      if (e.key === 'Escape') { $('cv-filter').value = cv.filter; $('cv-filter').blur(); }
    });
    $('cv-apply').onclick = () => applyFilter($('cv-filter').value.trim());
    $('cv-chips').onclick = e => {
      const b = e.target.closest('button');
      if (!b) return;
      if (b.dataset.unzoom) return applyFilter(nonT(cv.filter));
      const t = tTerms(cv.filter).join(' ');
      applyFilter(`${b.dataset.chip}${t ? ' ' + t : ''}`.trim());
    };
    $('cv-conn').onchange = e => { cv.conn = e.target.value; fetchGraph(); };
    $('cv-follow').onchange = e => setFollow(e.target.checked);

    const body = $('cv-body');
    body.addEventListener('scroll', queueRows, {passive: true});
    $('cv-rows').onclick = e => {
      const r = e.target.closest('.cv-row[data-i]');
      if (!r) return;
      const i = Number(r.dataset.i);
      const p = cv.rows.get(i);
      if (p) select(i, p);
      $('cv-table').focus({preventScroll: true});
    };
    $('cv-table').addEventListener('keydown', e => {
      const page = Math.max(1, Math.floor(body.clientHeight / ROW) - 1);
      const moves = {ArrowDown: 1, ArrowUp: -1, PageDown: page, PageUp: -page, Home: -1e12, End: 1e12};
      if (!(e.key in moves)) return;
      e.preventDefault();
      moveSel(moves[e.key]);
    });
    const fields = $('cv-fields');
    fields.addEventListener('mouseover', e => {
      const f = e.target.closest('[data-off]');
      highlight(f ? Number(f.dataset.off) : null, f ? Number(f.dataset.len) : 0);
    });
    fields.addEventListener('mouseleave', () => highlight(null, 0));
    setupGraph();
    window.addEventListener('resize', () => { if (cv.name) { queueRows(); } });
    clearDetail();
    renderChips();
  }

  registerTab('capture', {
    show() {
      refreshCapture();
      clearInterval(capPoll);
      capPoll = setInterval(refreshCapture, 5000);
      if (cv.name) {
        cv.inflight = false;
        if (!cv.complete) fetchVisible(true);
        queueRows();
        fetchGraph();
      }
    },
    hide() {
      clearInterval(capPoll);
      capPoll = null;
      setFollow(false);
      clearTimeout(cv.progressTimer);
    },
  });
  setupCapture();
})();
