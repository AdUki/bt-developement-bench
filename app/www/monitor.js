'use strict';
// Monitor: what the HCI monitor (app/src/hci) measures, per connection and per L2CAP channel —
// throughput, TX latency (HCI send → Number Of Completed Packets), in-flight packets against the
// controller's credits, and the AVDTP stream. Stats arrive on "hci.stats" once a second; the
// graphs read /api/hci/history for the selected connection at the same pace, only while visible.
// Below the events, the live packet list: the monitor's packets as they happen, filtered on the
// board with the Capture viewer's filter language.

const mon = {sel: null, stats: null, hist: null, lat: null, poll: null, latPoll: null, lastSeq: 0};

const linkKey = c => `${c.index}:${c.handle}`;
const latTxt = l => l && l.n ? `${l.p50} / ${l.p95} / ${l.max}` : '—';

function renderStats(s) {
  mon.stats = s;
  setText($('mon-msg'), s.source && !s.source.available ? 'The HCI monitor is not reading (see the daemon log; it needs CAP_NET_RAW).' : '');
  if (s.source) {
    setText($('mon-source'), s.source.kind === 'replay'
      ? `replay of ${s.source.file}${s.source.done ? ' (done)' : ''} · ${s.source.packets} packets`
      : `live · ${s.source.packets} packets${s.source.kernel_drops ? ' · ' + s.source.kernel_drops + ' dropped by the kernel' : ''}`);
  }
  setText($('mon-adapters'), (s.adapters || []).map(a =>
    `${a.name} ${a.addr} · ACL ${a.acl_mtu}×${a.acl_pkts} · LE ${a.le_mtu}×${a.le_pkts}${a.iso_pkts ? ' · ISO ' + a.iso_mtu + '×' + a.iso_pkts : ''}`).join('  |  '));
  const conns = s.conns || [];
  if (!mon.sel && conns.length) mon.sel = linkKey(conns.find(c => c.connected) || conns[0]);
  setHTML($('mon-conns'), conns.length ? `<tr><th>hci/handle</th><th>type</th><th>peer</th><th>TX kbit/s</th><th>RX kbit/s</th>
      <th>lat p50/p95/max ms</th><th>in flight / credits</th><th>RTP lost</th></tr>` +
    conns.map(c => {
      const media = (c.channels || []).find(ch => ch.avdtp && ch.avdtp.role === 'media');
      return `<tr class="click${linkKey(c) === mon.sel ? ' sel' : ''}" data-k="${linkKey(c)}">
        <td class="mono">hci${c.index}/${c.handle}${c.connected ? '' : ' ✕'}</td><td>${esc(c.type)}</td><td class="mono">${esc(c.peer)}</td>
        <td>${fmtKbps(c.tx_bps)}</td><td>${fmtKbps(c.rx_bps)}</td><td>${latTxt(c.lat_ms)}</td>
        <td>${c.in_flight} / ${c.credits}</td><td>${media ? media.avdtp.rtp_lost_total : '—'}</td></tr>`;
    }).join('') : '<tr><td class="muted empty">No connections seen since the monitor started.</td></tr>');
  renderDetail();
}

function selectedConn() {
  return mon.stats && (mon.stats.conns || []).find(c => linkKey(c) === mon.sel);
}

function renderDetail() {
  const c = selectedConn();
  $('mon-detail').hidden = !c;
  if (!c) return;
  setText($('mon-title'), `hci${c.index} handle ${c.handle} · ${c.type} · ${c.peer}${c.role ? ' · ' + c.role : ''}`);
  setText($('mon-now'), `TX ${fmtKbps(c.tx_bps)} RX ${fmtKbps(c.rx_bps)} kbit/s · in flight ${c.in_flight}/${c.credits} (max ${c.in_flight_max}, credits min ${c.credits_min})`);
  setHTML($('mon-channels'), (c.channels || []).length ? `<h4>Channels</h4><div class="scroll-x"><table class="tbl">
      <tr><th>CID</th><th>PSM</th><th>name</th><th>TX kbit/s</th><th>RX kbit/s</th><th>lat p50/p95/max</th><th>AVDTP</th></tr>
      ${c.channels.map(ch => {
        const a = ch.avdtp;
        let av = '—';
        if (a && a.role === 'media') {
          av = `${esc(a.state)} · ${esc(a.codec)} ${esc(a.config)} · RTP ${a.rtp_pkts}/s lost ${a.rtp_lost} (total ${a.rtp_lost_total})` +
            `${a.rtp_jitter_ms != null ? ' · jitter ' + a.rtp_jitter_ms + ' ms' : ''}${a.frames_per_packet ? ' · ' + a.frames_per_packet + ' fr/pkt' : ''}`;
        } else if (a && a.streams) {
          av = a.streams.map(st => `SEID ${st.lseid}→${st.rseid} ${esc(st.state)} ${esc(st.codec)}`).join('; ') || 'signalling';
        }
        return `<tr><td class="mono">${ch.scid}/${ch.dcid}</td><td>${ch.psm || ''}</td><td>${esc(ch.name)}${ch.open ? '' : ' (closed)'}</td>
          <td>${fmtKbps(ch.tx_bps)}</td><td>${fmtKbps(ch.rx_bps)}</td><td>${latTxt(ch.lat_ms)}</td><td class="small">${av}</td></tr>`;
      }).join('')}</table></div>` : '');
}

// ---------------------------------------------------------------- graphs

function css(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

function canvasCtx(cv) {
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth, h = cv.clientHeight;
  if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
    cv.width = Math.round(w * dpr);
    cv.height = Math.round(h * dpr);
  }
  const g = cv.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  return {g, w, h};
}

// Series share one x axis (the history's timestamps) and one y scale from 0 to the max.
function plot(cv, t, series, unitScale) {
  const {g, w, h} = canvasCtx(cv);
  if (!t || t.length < 2) {
    g.fillStyle = css('--muted');
    g.fillText('no data yet', 8, 16);
    return;
  }
  let max = 0;
  for (const s of series) for (const v of s.data) if (v != null && isFinite(v)) max = Math.max(max, v * (s.scale || unitScale));
  max = max > 0 ? max * 1.1 : 1;
  const t0 = t[0], t1 = t[t.length - 1] || t0 + 1;
  const x = v => 4 + (w - 8) * (v - t0) / Math.max(1, t1 - t0);
  const y = v => h - 14 - (h - 22) * v / max;
  g.strokeStyle = css('--line');
  g.beginPath(); g.moveTo(0, y(0)); g.lineTo(w, y(0)); g.stroke();
  g.fillStyle = css('--muted');
  g.font = '11px system-ui';
  g.fillText(max.toFixed(max < 10 ? 1 : 0), 4, 11);
  g.fillText(`${Math.round((t1 - t0) / 1000)} s`, w - 34, h - 2);
  for (const s of series) {
    g.strokeStyle = css(s.color);
    g.lineWidth = 1.5;
    g.beginPath();
    let pen = false;
    s.data.forEach((v, i) => {
      if (v == null || !isFinite(v)) { pen = false; return; }
      const px = x(t[i]), py = y(v * (s.scale || unitScale));
      if (pen) g.lineTo(px, py); else g.moveTo(px, py);
      pen = true;
    });
    g.stroke();
  }
}

function drawHist(lat) {
  const {g, w, h} = canvasCtx($('mon-hist'));
  if (!lat || !lat.counts) return;
  // Up to the last non-empty bucket, at least 20 ms, so a tidy link's spike is not a sliver.
  let last = 20;
  lat.counts.forEach((n, i) => { if (n) last = Math.max(last, i + 1); });
  const counts = lat.counts.slice(0, last);
  const max = Math.max(1, ...counts);
  const bw = (w - 8) / counts.length;
  g.fillStyle = css('--p50');
  counts.forEach((n, i) => {
    const bh = (h - 16) * n / max;
    g.fillRect(4 + i * bw, h - 14 - bh, Math.max(1, bw - 1), bh);
  });
  g.fillStyle = css('--muted');
  g.font = '11px system-ui';
  g.fillText('0', 4, h - 2);
  g.fillText(`${last} ms${lat.overflow ? ' (+' + lat.overflow + ' beyond)' : ''}`, w - 90, h - 2);
}

function pollHistory() {
  const c = selectedConn();
  if (!c) return;
  api(`/hci/history?index=${c.index}&handle=${c.handle}&seconds=120`).then(hs => {
    mon.hist = hs;
    plot($('mon-tput'), hs.t, [{data: hs.tx_bps, color: '--tx'}, {data: hs.rx_bps, color: '--rx'}], 0.001);
    plot($('mon-lat'), hs.t, [{data: hs.lat_p50, color: '--p50'}, {data: hs.lat_p95, color: '--p95'},
      {data: hs.in_flight, color: '--fl'}], 1);
  }).catch(() => { /* the connection went: the next stats frame says so */ });
}

function pollLatency() {
  const c = selectedConn();
  if (c) api(`/hci/latency?index=${c.index}&handle=${c.handle}`).then(drawHist).catch(() => {});
}

// ---------------------------------------------------------------- events

function eventLine(e) {
  return `${fmtTime(e.ts)} hci${e.index}${e.handle != null ? '/' + e.handle : ''} ${e.kind}: ${e.text}`;
}

function loadEvents() {
  api(`/hci/events?since=0`).then(evs => {
    logClear($('mon-events'));
    evs.forEach(e => { logLine($('mon-events'), eventLine(e)); mon.lastSeq = Math.max(mon.lastSeq, e.seq); });
  }).catch(e => setText($('mon-msg'), e.message));
}

function setupMonitor() {
  $('mon-conns').onclick = e => {
    const tr = e.target.closest('tr[data-k]');
    if (!tr) return;
    mon.sel = tr.dataset.k;
    if (mon.stats) renderStats(mon.stats);
    pollHistory();
    pollLatency();
  };
  const mark = () => {
    const text = $('mon-mark-text').value.trim() || 'mark';
    post('/hci/mark', {text}).then(r => {
      if (!r.logged) toast('Mark kept in the events, but the kernel did not take it' + (r.error ? ': ' + r.error : ''));
      $('mon-mark-text').value = '';
    }).catch(fail);
  };
  $('mon-mark').onclick = mark;
  $('mon-mark-text').onkeydown = e => { if (e.key === 'Enter') mark(); };
  $('mon-ws-cmd').textContent = `curl -sN http://${location.host}/api/hci/live.pcap | wireshark -k -i -`;
  window.addEventListener('resize', () => { if (currentTab === 'monitor') { pollHistory(); pollLatency(); } });
}

// ---------------------------------------------------------------- live packets

// GET /api/hci/live is a stream of NDJSON lines: packets in the Capture viewer's shape plus
// "seq", and {"status": {...}} lines with the stream's counters. The filter is the stream's own:
// changing it reopens the stream with since=0, so the board re-filters the packets it still holds
// (the last 2048). The stream is open only while this tab is on screen, the page is visible and
// the view is not paused — the board then classifies nothing at all.
const livePkts = (() => {
  const MAXROWS = 1000;  // rows kept in the table; the board's ring holds more for the detail
  const ROW = 22;        // px, as the Capture viewer's rows
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
  const lp = {
    filter: lsGet('btbench.lp.filter') || 'type:cmd|evt !evt:0x13', paused: false, abort: null,
    gen: 0, lastSeq: -1, shown: 0, dropped: 0, skipped: 0, base: null, error: '', sel: null,
    hexSpans: [], retry: null, connected: false, held: [],
  };

  const want = () => currentTab === 'packets' && !document.hidden && !lp.paused;

  function renderStatus() {
    const bits = [];
    if (lp.error) bits.push(lp.error);
    else if (lp.paused) bits.push('paused');
    else if (lp.connected) bits.push('live');
    else bits.push('connecting…');
    bits.push(`${lp.shown.toLocaleString()} shown` + (lp.scanned ? ` of ${lp.scanned.toLocaleString()} seen` : ''));
    if (lp.dropped) bits.push(`${lp.dropped.toLocaleString()} dropped`);
    if (lp.skipped) bits.push(`${lp.skipped.toLocaleString()} skipped`);
    if (lp.held.length) bits.push(`${lp.held.length.toLocaleString()} new rows held: scroll to the bottom to follow`);
    setText($('lp-status'), bits.join(' · '));
    setClass($('lp-status'), 'small ' + (lp.error ? 'bad-text' : lp.dropped || lp.skipped ? 'warn-text' : 'muted'));
    setText($('lp-run'), lp.paused ? 'Resume' : 'Pause');
  }

  function renderChips() {
    setHTML($('lp-chips'), CHIPS.map(([label, f]) =>
      `<button class="small-btn cv-chip${lp.filter === f ? ' on' : ''}" data-chip="${esc(f)}">${esc(label)}</button>`).join(''));
  }

  const atBottom = el => el.scrollTop + el.clientHeight >= el.scrollHeight - ROW * 2;

  function rowHTML(p) {
    const ms = String(Math.floor(p.ts_ms % 1000)).padStart(3, '0');
    const cls = ['cv-row', 't-' + p.type, p.dir ? 'd-' + p.dir : '', p.err ? 'err' : '', p.mark ? 'mark' : '',
      p.seq === lp.sel ? 'sel' : ''].join(' ');
    return `<div class="${cls}" data-seq="${p.seq}"><span>${p.seq}</span><span>${fmtTime(p.ts_ms)}.${ms}</span>` +
      `<span class="cv-dir cv-c-d">${p.dir}</span><span class="cv-c-h">${p.handle == null ? '' : p.handle}</span>` +
      `<span>${p.proto || p.type}</span><span class="cv-c-l">${p.len}</span>` +
      `<span class="cv-sum">${esc(p.summary)}${p.lat_ms != null ? ` <i class="muted">${p.lat_ms} ms</i>` : ''}</span></div>`;
  }
  const noteHTML = text => `<div class="cv-row cv-wait lp-note"><span></span><span>${esc(text)}</span></div>`;

  // Rows arrive in batches (the board sends one every 150 ms at most): one DOM insertion each.
  // While the reader is scrolled up, new rows are held back instead of added: at A2DP rates the
  // rows being read would otherwise be trimmed from the top within a second or two.
  function append(rows) {
    if (!rows.length) return;
    if (lp.held.length || !atBottom($('lp-body'))) {
      lp.held.push(...rows);
      if (lp.held.length > MAXROWS) lp.held.splice(0, lp.held.length - MAXROWS);
      return;
    }
    insert(rows);
  }

  function insert(rows) {
    const body = $('lp-body');
    const empty = body.querySelector('.cv-empty');
    if (empty) empty.remove();
    body.insertAdjacentHTML('beforeend', rows.join(''));
    let extra = body.childElementCount - MAXROWS;
    while (extra-- > 0) body.firstElementChild.remove();
    body.scrollTop = body.scrollHeight;
  }

  function onScroll() {
    if (lp.held.length && atBottom($('lp-body'))) {
      const rows = lp.held;
      lp.held = [];
      insert(rows);
    }
    renderStatus();
  }

  function onLines(items) {
    const rows = [];
    for (const o of items) {
      if (o.status) {
        const s = o.status;
        // The counters are the stream's: a reopened stream starts from 0, so add what grew.
        if (lp.base == null) lp.base = {dropped: 0, skipped: 0};
        const nd = s.dropped - lp.base.dropped, ns = s.skipped - lp.base.skipped;
        if (nd > 0 || ns > 0) {
          rows.push(noteHTML([nd > 0 ? `${nd.toLocaleString()} packets dropped (the page fell behind)` : '',
            ns > 0 ? `${ns.toLocaleString()} matching packets skipped (too many to show; the newest are)` : '']
            .filter(Boolean).join(' · ')));
          lp.dropped += Math.max(0, nd);
          lp.skipped += Math.max(0, ns);
        }
        lp.base = {dropped: s.dropped, skipped: s.skipped};
        // How much went past the filter: "0 shown" then reads as a filter that matches nothing,
        // not as a stream that is stuck.
        if (s.scanned != null) lp.scanned = s.scanned;
        continue;
      }
      lp.lastSeq = Math.max(lp.lastSeq, o.seq);
      lp.shown++;
      rows.push(rowHTML(o));
    }
    append(rows);
    renderStatus();
  }

  function stop() {
    lp.gen++;
    clearTimeout(lp.retry);
    lp.retry = null;
    if (lp.abort) lp.abort.abort();
    lp.abort = null;
    lp.connected = false;
  }

  // (Re)opens the stream if it should be open and is not. since: where to pick up — the last
  // packet seen, or 0 to re-filter everything the board still holds.
  function start(since) {
    if (!want() || lp.abort) return;
    const gen = ++lp.gen;
    const ctl = new AbortController();
    lp.abort = ctl;
    lp.base = null;
    lp.error = '';
    const q = `filter=${encodeURIComponent(lp.filter)}${since != null && since >= 0 ? '&since=' + since : ''}`;
    renderStatus();
    fetch(`/api/hci/live?${q}`, {signal: ctl.signal, cache: 'no-store'}).then(async r => {
      if (!r.ok) {
        let msg = r.statusText;
        try { msg = (await r.json()).error || msg; } catch (e) { /* not JSON */ }
        const err = new Error(msg);
        err.status = r.status;
        throw err;
      }
      lp.connected = true;
      renderStatus();
      const reader = r.body.getReader();
      const dec = new TextDecoder();
      let buf = '';
      for (;;) {
        const {value, done} = await reader.read();
        if (done || gen !== lp.gen) break;
        buf += dec.decode(value, {stream: true});
        const items = [];
        let nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, nl);
          buf = buf.slice(nl + 1);
          if (line) try { items.push(JSON.parse(line)); } catch (e) { /* a cut line: skip */ }
        }
        if (items.length) onLines(items);
      }
      throw new Error('the stream ended');
    }).catch(e => {
      if (gen !== lp.gen) return;  // stopped on purpose
      lp.abort = null;
      lp.connected = false;
      lp.error = e.message;
      renderStatus();
      // A bad filter stays wrong; anything else (the daemon restarting, too many streams) is
      // retried, picking up where the stream stopped.
      if (e.status !== 400) lp.retry = setTimeout(() => { lp.error = ''; start(lp.lastSeq); }, 3000);
    });
  }

  function restart(since) {
    stop();
    start(since);
  }

  function clear() {
    // Rows are appended outside setHTML, so its memo of the content is dropped too.
    const body = $('lp-body');
    body.innerHTML = '<div class="cv-empty muted small">Waiting for packets…</div>';
    body._html = null;
    lp.held = [];
    lp.shown = lp.dropped = lp.skipped = lp.scanned = 0;
  }

  function applyFilter(f) {
    lp.filter = f.trim();
    $('lp-filter').value = lp.filter;
    lsSet('btbench.lp.filter', lp.filter);
    renderChips();
    clear();
    lp.lastSeq = -1;
    restart(0);
  }

  function setPaused(on) {
    lp.paused = on;
    if (on) {
      stop();
      append([noteHTML('paused')]);
    } else {
      start(lp.lastSeq);
    }
    renderStatus();
  }

  // ---- the detail pane (the Capture viewer's layout)

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

  function clearDetail() {
    lp.hexSpans = [];
    setHTML($('lp-fields'), '<div class="muted small">Click a row for its decoded fields and bytes.</div>');
    setHTML($('lp-hex'), '');
  }

  function select(seq) {
    lp.sel = seq;
    $('lp-body').querySelectorAll('.cv-row.sel').forEach(r => r.classList.remove('sel'));
    const row = $('lp-body').querySelector(`.cv-row[data-seq="${seq}"]`);
    if (row) row.classList.add('sel');
    api(`/hci/live/${seq}`).then(d => {
      if (lp.sel !== seq) return;
      setHTML($('lp-fields'), `<div class="cv-dhead"><b>#${d.seq}</b> ${esc(d.summary)}</div>` + fieldsHTML(d.fields));
      setHTML($('lp-hex'), d.hex ? hexHTML(d.hex) : '<span class="muted">(no bytes)</span>');
      lp.hexSpans = Array.from($('lp-hex').querySelectorAll('[data-o]'));
    }).catch(e => {
      if (lp.sel !== seq) return;
      setHTML($('lp-fields'), `<div class="bad-text small">${esc(e.message)}</div>`);
      setHTML($('lp-hex'), '');
    });
  }

  // The Capture viewer on the file btmon is writing, with this filter. Its frame numbers are the
  // file's, not these sequence numbers.
  function openInCapture() {
    api('/capture').then(s => {
      const f = (s.files || []).find(x => x.active) || (s.files || [])[0];
      if (!f) return toast('No capture file: the btsnoop ring is not recording');
      $('cv-filter').value = lp.filter;
      go('capture');
      const open = tries => {
        const b = Array.from(document.querySelectorAll('#cap-files button[data-view]')).find(x => x.dataset.view === f.name);
        if (b) b.click();
        else if (tries > 0) setTimeout(() => open(tries - 1), 200);
      };
      open(20);
    }).catch(fail);
  }

  function setup() {
    $('lp-filter').value = lp.filter;
    $('lp-filter').addEventListener('keydown', e => {
      if (e.key === 'Enter') applyFilter($('lp-filter').value);
      if (e.key === 'Escape') { $('lp-filter').value = lp.filter; $('lp-filter').blur(); }
    });
    $('lp-apply').onclick = () => applyFilter($('lp-filter').value);
    $('lp-chips').onclick = e => {
      const b = e.target.closest('button[data-chip]');
      if (b) applyFilter(b.dataset.chip);
    };
    $('lp-run').onclick = () => setPaused(!lp.paused);
    $('lp-clear').onclick = () => { clear(); renderStatus(); };
    $('lp-cap').onclick = openInCapture;
    $('lp-body').onclick = e => {
      const r = e.target.closest('.cv-row[data-seq]');
      if (r) select(Number(r.dataset.seq));
    };
    $('lp-body').addEventListener('scroll', onScroll, {passive: true});
    const fields = $('lp-fields');
    const hl = (off, len) => lp.hexSpans.forEach((s, i) => s.classList.toggle('hl', off != null && i >= off && i < off + len));
    fields.addEventListener('mouseover', e => {
      const f = e.target.closest('[data-off]');
      hl(f ? Number(f.dataset.off) : null, f ? Number(f.dataset.len) : 0);
    });
    fields.addEventListener('mouseleave', () => hl(null, 0));
    // A browser tab in the background is "hidden" too: nothing may run for it.
    document.addEventListener('visibilitychange', () => {
      if (document.hidden) stop();
      else start(lp.lastSeq);
      renderStatus();
    });
    renderChips();
    clear();
    clearDetail();
    renderStatus();
  }

  return {
    setup,
    // show() also runs again when the WebSocket reconnects: start() does nothing when open.
    show() { start(lp.lastSeq); },
    hide() { stop(); renderStatus(); },
  };
})();

registerTab('monitor', {
  topics: () => ['hci.stats', 'hci.event'],
  show() {
    api('/hci/stats').then(renderStats).catch(e => setText($('mon-msg'), e.message));
    loadEvents();
    clearInterval(mon.poll);
    clearInterval(mon.latPoll);
    mon.poll = setInterval(pollHistory, 1000);
    mon.latPoll = setInterval(pollLatency, 5000);
    setTimeout(() => { pollHistory(); pollLatency(); }, 300);
  },
  hide() {
    clearInterval(mon.poll);
    clearInterval(mon.latPoll);
    mon.poll = mon.latPoll = null;
  },
  onMessage(topic, d) {
    if (currentTab !== 'monitor') return;
    if (topic === 'hci.stats') renderStats(d);
    else if (topic === 'hci.event' && d.seq > mon.lastSeq) {
      mon.lastSeq = d.seq;
      logLine($('mon-events'), eventLine(d));
    }
  },
});
// Live packets are a view of their own: the stream runs only while it is on screen.
registerTab('packets', {show: livePkts.show, hide: livePkts.hide});
setupMonitor();
livePkts.setup();
