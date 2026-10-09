'use strict';
// Graphs: live, scrolling time series of everything the bench measures, on one time axis —
// per connection the throughput, TX latency (p50/p95/max), packets in flight against the
// controller's credits, the AVDTP media stream's RTP rate, loss and jitter; the audio streams'
// levels; the board's CPU and temperature. Fed by the WebSocket (hci.stats and system once a
// second, audio.streams four times), backfilled from /api/hci/history when the tab opens, and
// kept in the browser for five minutes. Nothing is asked of the board while the tab is hidden.

const GR_KEEP_MS = 300 * 1000;
const GR_PALETTE = ['#1f6feb', '#1a8f5a', '#8250df', '#cf3a3a', '#b7791f', '#0e8a9b', '#c2185b', '#5d6b00'];
const GR_PALETTE_DARK = ['#4da3ff', '#3ecf8e', '#b48cff', '#ef5b5b', '#f5a623', '#3fc6d6', '#f06292', '#b8c94a'];

// The charts, in page order. A chart shows only once one of its series has data.
const GR_CHARTS = [
  {id: 'tput', title: 'Throughput', unit: 'kbit/s', styles: ['TX', 'RX']},
  {id: 'lat', title: 'TX latency: HCI send → Number Of Completed Packets', unit: 'ms', styles: ['p50', 'p95', 'max']},
  {id: 'flight', title: 'Packets in flight, and the controller\'s free credits', unit: 'packets', styles: ['in flight', 'credits']},
  {id: 'rtp', title: 'AVDTP media: RTP packets and losses per second', unit: '/s', styles: ['packets', 'lost']},
  {id: 'jitter', title: 'AVDTP media: RTP jitter', unit: 'ms'},
  {id: 'audio', title: 'Audio streams: level', unit: 'dBFS', min: -60, max: 0, styles: ['RMS', null, 'peak']},
  {id: 'sys', title: 'Board: CPU and temperature', unit: '% · °C', styles: ['CPU %', 'temperature']},
];

const gr = {series: {}, order: [], skew: null, paused: false, frozenAt: 0, hover: null, hidden: new Set(),
  conns: {}, raf: null, last: 0, backfilled: new Set()};

try { (JSON.parse(lsGet('btbench.gr.hidden-groups') || '[]')).forEach(k => gr.hidden.add(k)); } catch (e) { /* none */ }

const grDark = () => window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
function grColor(i) { const p = grDark() ? GR_PALETTE_DARK : GR_PALETTE; return p[i % p.length]; }

// A series: one line on one chart. `group` (a connection, a stream) picks the colour; `dash` tells
// apart the lines of one group (TX solid, RX dashed; p50, p95, max).
function grSeries(key, def) {
  let s = gr.series[key];
  if (!s) {
    const groups = [...new Set(gr.order.map(k => gr.series[k].group))];
    let gi = groups.indexOf(def.group);
    if (gi < 0) gi = groups.length;
    s = gr.series[key] = Object.assign({key, data: [], colorIndex: gi}, def);
    gr.order.push(key);
  }
  return s;
}

function grPush(key, def, t, v) {
  if (v == null || !isFinite(v)) return;
  const s = grSeries(key, def);
  const d = s.data;
  if (d.length && t <= d[d.length - 1][0]) {
    if (t === d[d.length - 1][0]) d[d.length - 1][1] = v;
    return;
  }
  d.push([t, v]);
  const cut = t - GR_KEEP_MS;
  let i = 0;
  while (i < d.length && d[i][0] < cut) i++;
  if (i) d.splice(0, i);
}

const connKey = c => `hci${c.index}/${c.handle}`;
const connName = c => `${connKey(c)} ${c.type}${c.peer ? ' ' + c.peer : ''}`;

function grFromStats(s) {
  if (!s || !s.conns) return;
  // The board may have no RTC (or not be synced yet): place its samples on this browser's clock.
  gr.skew = Date.now() - s.ts;
  const t = s.ts + gr.skew;
  for (const c of s.conns) {
    if (!c.connected && !gr.conns[connKey(c)]) continue;
    const g = connKey(c), name = connName(c);
    gr.conns[g] = name;
    grPush(g + ':tx', {group: g, chart: 'tput', label: `${name} TX`}, t, c.tx_bps / 1000);
    grPush(g + ':rx', {group: g, chart: 'tput', label: `${name} RX`, dash: [5, 3]}, t, c.rx_bps / 1000);
    if (c.lat_ms && c.lat_ms.n) {
      grPush(g + ':p50', {group: g, chart: 'lat', label: `${name} p50`}, t, c.lat_ms.p50);
      grPush(g + ':p95', {group: g, chart: 'lat', label: `${name} p95`, dash: [5, 3]}, t, c.lat_ms.p95);
      grPush(g + ':max', {group: g, chart: 'lat', label: `${name} max`, dash: [1, 3]}, t, c.lat_ms.max);
    }
    if (c.connected) {
      grPush(g + ':fl', {group: g, chart: 'flight', label: `${name} in flight`}, t, c.in_flight);
      grPush(g + ':cr', {group: g, chart: 'flight', label: `${name} credits`, dash: [5, 3]}, t, c.credits);
    }
    for (const ch of c.channels || []) {
      const a = ch.avdtp;
      if (!a || a.role !== 'media') continue;
      grPush(g + ':rtp', {group: g, chart: 'rtp', label: `${name} RTP packets`}, t, a.rtp_pkts);
      grPush(g + ':lost', {group: g, chart: 'rtp', label: `${name} RTP lost`, dash: [5, 3]}, t, a.rtp_lost);
      if (a.rtp_jitter_ms != null) grPush(g + ':jit', {group: g, chart: 'jitter', label: `${name} jitter`}, t, a.rtp_jitter_ms);
    }
  }
  grConnSelect();
}

function grFromSystem(h) {
  const t = Date.now();
  grPush('sys:cpu', {group: 'board', chart: 'sys', label: 'CPU %'}, t, h.cpu_pct);
  if (h.temp_c != null) grPush('sys:temp', {group: 'board', chart: 'sys', label: 'temperature °C', dash: [5, 3]}, t, h.temp_c);
}

function grFromAudio(list) {
  const t = Date.now();
  for (const s of list.streams || []) {
    if (s.state !== 'running' || !s.level || !s.level.rms_db.length) continue;
    const g = 'stream' + s.id;
    const lbl = `#${s.id} ${s.label}`;
    grPush(g + ':rms', {group: g, chart: 'audio', label: lbl}, t, Math.max(-60, Math.max(...s.level.rms_db)));
    grPush(g + ':pk', {group: g, chart: 'audio', label: lbl + ' peak', dash: [1, 3]}, t, Math.max(-60, Math.max(...s.level.peak_db)));
  }
}

// What the monitor kept from before the tab opened (a point a second, up to five minutes).
function grBackfill(stats) {
  for (const c of (stats.conns || []).filter(x => x.connected)) {
    const g = connKey(c);
    if (gr.backfilled.has(g)) continue;
    gr.backfilled.add(g);
    api(`/hci/history?index=${c.index}&handle=${c.handle}&seconds=300`).then(h => {
      const name = connName(c);
      gr.conns[g] = name;
      const skew = gr.skew != null ? gr.skew : Date.now() - stats.ts;
      const defs = {tx_bps: [':tx', 'tput', 'TX', null, 0.001], rx_bps: [':rx', 'tput', 'RX', [5, 3], 0.001],
        lat_p50: [':p50', 'lat', 'p50', null, 1], lat_p95: [':p95', 'lat', 'p95', [5, 3], 1],
        lat_max: [':max', 'lat', 'max', [1, 3], 1], in_flight: [':fl', 'flight', 'in flight', null, 1]};
      for (const [field, [suffix, chart, what, dash, scale]] of Object.entries(defs)) {
        if (!h[field]) continue;
        const s = grSeries(g + suffix, {group: g, chart, label: `${name} ${what}`, dash});
        const first = s.data.length ? s.data[0][0] : Infinity;
        const pts = [];
        h.t.forEach((tt, i) => {
          const v = h[field][i];
          // Latency is 0 in a window without completions: a gap, not a measurement.
          if (v == null || (chart === 'lat' && !v)) return;
          const at = tt + skew;
          if (at < first) pts.push([at, v * scale]);
        });
        s.data = pts.concat(s.data);
      }
      grConnSelect();
    }).catch(() => { gr.backfilled.delete(g); });
  }
}

function grConnSelect() {
  const sel = $('gr-conn');
  fillSelect(sel, [{value: '', label: 'all'}].concat(Object.entries(gr.conns).map(([k, n]) => ({value: k, label: n}))));
}

// ---------------------------------------------------------------- drawing

function grVisible(s) {
  const only = $('gr-conn').value;
  if (only && s.group.startsWith('hci') && s.group !== only) return false;
  return !gr.hidden.has(s.group);
}

function grNow() { return gr.paused ? gr.frozenAt : Date.now(); }

function grLayout() {
  const box = $('gr-charts');
  for (const c of GR_CHARTS) {
    if (!gr.order.some(k => gr.series[k].chart === c.id && gr.series[k].data.length)) continue;
    if ($('gr-c-' + c.id)) continue;
    const el = document.createElement('div');
    el.className = 'card gr-chart';
    el.id = 'gr-c-' + c.id;
    el.innerHTML = `<h4>${esc(c.title)} <span class="muted">(${esc(c.unit)})</span> <span class="legend muted">${grStyleKey(c.styles)}</span></h4><canvas class="graph"></canvas><div class="gr-tip" hidden></div>`;
    // Keep page order whatever order the data arrived in.
    const after = GR_CHARTS.slice(0, GR_CHARTS.indexOf(c)).reverse().map(x => $('gr-c-' + x.id)).find(Boolean);
    if (after) after.after(el); else box.prepend(el);
    const cv = el.querySelector('canvas');
    cv.onmousemove = e => {
      const r = cv.getBoundingClientRect();
      gr.hover = {chart: c.id, x: e.clientX - r.left};
      grDraw(true);
    };
    cv.onmouseleave = () => { gr.hover = null; grDraw(true); };
  }
}

// One entry a group (a connection, a stream, the board), in its colour; a click hides the group.
// The line styles within a group are told in each chart's title.
function grLegend() {
  const groups = new Map();
  for (const k of gr.order) {
    const s = gr.series[k];
    if (s.data.length && !groups.has(s.group)) groups.set(s.group, s);
  }
  const name = s => s.group === 'board' ? 'board' : s.group.startsWith('stream') ? s.label.replace(/ peak$/, '') : gr.conns[s.group] || s.group;
  setHTML($('gr-legend'), [...groups.values()].map(s => `<span data-k="${esc(s.group)}" class="${gr.hidden.has(s.group) ? 'off' : ''}">` +
    `<i style="background:${grColor(s.colorIndex)}"></i>${esc(name(s))}</span>`).join(''));
}

const GR_DASHES = [null, [5, 3], [1, 3]];
function grStyleKey(styles) {
  if (!styles) return '';
  return styles.map((st, i) => st ? `<svg width="22" height="6"><line x1="0" y1="3" x2="22" y2="3" stroke="currentColor" stroke-width="1.6"` +
    `${GR_DASHES[i] ? ` stroke-dasharray="${GR_DASHES[i].join(' ')}"` : ''}/></svg> ${esc(st)}` : '').filter(Boolean).join(' &nbsp; ');
}

function grDraw(force) {
  if (currentTab !== 'graphs') return;
  grLayout();
  const now = grNow();
  const win = parseInt($('gr-window').value, 10) * 1000;
  const t0 = now - win;
  const style = getComputedStyle(document.documentElement);
  const cssv = n => style.getPropertyValue(n).trim();
  let hoverT = null;
  for (const c of GR_CHARTS) {
    const el = $('gr-c-' + c.id);
    if (!el) continue;
    const cv = el.querySelector('canvas');
    const {g, w, h} = canvasCtx(cv);
    const L = 44, R = 8, T = 8, B = 18;
    const series = gr.order.map(k => gr.series[k]).filter(s => s.chart === c.id && grVisible(s));
    let lo = c.min != null ? c.min : 0, hi = c.max != null ? c.max : 0;
    if (c.max == null) {
      for (const s of series) for (const [t, v] of s.data) if (t >= t0 && v > hi) hi = v;
      hi = hi > 0 ? niceCeil(hi * 1.1) : 1;
    }
    const x = t => L + (w - L - R) * (t - t0) / win;
    const y = v => T + (h - T - B) * (1 - (v - lo) / (hi - lo));
    // Grid: four bands, labelled; time ticks every 10 s / 30 s / 1 min, counting back from now.
    g.strokeStyle = cssv('--line');
    g.fillStyle = cssv('--muted');
    g.font = '10.5px system-ui';
    g.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
      const v = lo + (hi - lo) * i / 4, py = Math.round(y(v)) + 0.5;
      g.beginPath(); g.moveTo(L, py); g.lineTo(w - R, py); g.stroke();
      g.fillText(fmtAxis(v), 2, py + 3.5);
    }
    const step = win <= 30000 ? 5000 : win <= 120000 ? 15000 : 60000;
    for (let back = 0; back <= win; back += step) {
      const px = Math.round(x(now - back)) + 0.5;
      g.beginPath(); g.moveTo(px, T); g.lineTo(px, h - B); g.stroke();
      g.fillText(back ? `-${back >= 60000 ? back / 60000 + ' min' : back / 1000 + ' s'}` : (gr.paused ? 'paused' : 'now'), px - (back ? 14 : 24), h - 4);
    }
    // The lines. A gap of more than 3 s between points is drawn as a gap.
    g.save();
    g.beginPath(); g.rect(L, 0, w - L - R, h); g.clip();
    for (const s of series) {
      g.strokeStyle = grColor(s.colorIndex);
      g.lineWidth = s.dash ? 1.2 : 1.6;
      g.setLineDash(s.dash || []);
      g.beginPath();
      let prev = null;
      for (const [t, v] of s.data) {
        if (t < t0 - 2000) continue;
        const px = x(t), py = y(Math.max(lo, Math.min(hi, v)));
        if (prev != null && t - prev < 3000) g.lineTo(px, py); else g.moveTo(px, py);
        prev = t;
      }
      g.stroke();
    }
    g.restore();
    g.setLineDash([]);
    // The crosshair: the same instant on every chart, the values in the hovered one.
    if (gr.hover) {
      if (hoverT == null) {
        const hc = $('gr-c-' + gr.hover.chart);
        const hw = hc ? hc.querySelector('canvas').clientWidth : w;
        hoverT = t0 + (gr.hover.x - L) / (hw - L - R) * win;
      }
      const px = Math.round(x(hoverT)) + 0.5;
      g.strokeStyle = cssv('--fg');
      g.globalAlpha = 0.4;
      g.beginPath(); g.moveTo(px, T); g.lineTo(px, h - B); g.stroke();
      g.globalAlpha = 1;
    }
    const tip = el.querySelector('.gr-tip');
    if (gr.hover && gr.hover.chart === c.id && hoverT != null) {
      const rows = series.map(s => {
        const p = nearest(s.data, hoverT);
        return p && Math.abs(p[0] - hoverT) < 2500 ? `<div><i style="display:inline-block;width:10px;height:3px;vertical-align:middle;background:${grColor(s.colorIndex)}"></i> ${esc(s.label)}: <b>${fmtVal(p[1])}</b></div>` : '';
      }).join('');
      tip.innerHTML = `<div class="muted">${fmtTime(hoverT)}</div>${rows || '<div class="muted">no samples</div>'}`;
      tip.hidden = false;
      const left = gr.hover.x + 14;
      tip.style.left = Math.min(left, w - tip.offsetWidth - 4) + 'px';
      tip.style.top = '28px';
    } else {
      tip.hidden = true;
    }
  }
  if (force) return;
}

function nearest(d, t) {
  let lo = 0, hi = d.length - 1;
  if (hi < 0) return null;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (d[mid][0] < t) lo = mid + 1; else hi = mid;
  }
  const a = d[lo], b = d[lo - 1];
  return b && Math.abs(b[0] - t) < Math.abs(a[0] - t) ? b : a;
}

function niceCeil(v) {
  const p = Math.pow(10, Math.floor(Math.log10(v)));
  for (const m of [1, 2, 2.5, 5, 10]) if (v <= m * p) return m * p;
  return 10 * p;
}
const fmtAxis = v => Math.abs(v) >= 1000 ? (v / 1000).toFixed(v % 1000 ? 1 : 0) + 'k' : (Math.abs(v) < 10 && v % 1 ? v.toFixed(1) : String(Math.round(v)));
const fmtVal = v => Math.abs(v) >= 100 ? v.toFixed(0) : Math.abs(v) >= 10 ? v.toFixed(1) : v.toFixed(2);

// Scrolls at about 8 frames a second: smooth enough for a 1 Hz feed, cheap for a laptop.
function grLoop() {
  if (currentTab !== 'graphs') { gr.raf = null; return; }
  const t = performance.now();
  if (t - gr.last > 120) {
    gr.last = t;
    if (!gr.paused || gr.hover) grDraw();
  }
  gr.raf = requestAnimationFrame(grLoop);
}

function grCsv() {
  const lines = ['series,time_iso,time_ms,value'];
  for (const k of gr.order) {
    const s = gr.series[k];
    for (const [t, v] of s.data) lines.push(`"${s.label.replace(/"/g, '""')}",${new Date(t).toISOString()},${Math.round(t)},${v}`);
  }
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([lines.join('\n') + '\n'], {type: 'text/csv'}));
  a.download = `btbench-graphs-${new Date().toISOString().replace(/[:.]/g, '-')}.csv`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

function setupGraphs() {
  $('gr-pause').onclick = () => {
    gr.paused = !gr.paused;
    gr.frozenAt = Date.now();
    setText($('gr-pause'), gr.paused ? 'Resume' : 'Pause');
    grDraw();
  };
  $('gr-window').onchange = () => grDraw();
  $('gr-conn').onchange = () => grDraw();
  $('gr-csv').onclick = grCsv;
  $('gr-legend').onclick = e => {
    const sp = e.target.closest('span[data-k]');
    if (!sp) return;
    const k = sp.dataset.k;
    if (gr.hidden.has(k)) gr.hidden.delete(k); else gr.hidden.add(k);
    lsSet('btbench.gr.hidden-groups', JSON.stringify([...gr.hidden]));
    grLegend();
    grDraw();
  };
}

registerTab('graphs', {
  topics: () => ['hci.stats', 'system', 'audio.streams'],
  show() {
    api('/hci/stats').then(s => {
      setText($('gr-msg'), s.source && !s.source.available ? 'The HCI monitor is not reading: no connection graphs (see the daemon log).' : '');
      grFromStats(s);
      grBackfill(s);
    }).catch(e => setText($('gr-msg'), 'HCI monitor: ' + e.message));
    if (!gr.raf) gr.raf = requestAnimationFrame(grLoop);
  },
  hide() {
    if (gr.raf) cancelAnimationFrame(gr.raf);
    gr.raf = null;
  },
  onMessage(topic, d) {
    if (currentTab !== 'graphs') return;
    const before = gr.order.length;
    if (topic === 'hci.stats') { grFromStats(d); grBackfill(d); }
    else if (topic === 'system') grFromSystem(d);
    else if (topic === 'audio.streams') grFromAudio(d);
    if (gr.order.length !== before || topic === 'system') grLegend();
  },
});
setupGraphs();
