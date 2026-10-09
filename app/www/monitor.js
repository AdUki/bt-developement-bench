'use strict';
// Monitor: what the HCI monitor (app/src/hci) measures, per connection and per L2CAP channel —
// throughput, TX latency (HCI send → Number Of Completed Packets), in-flight packets against the
// controller's credits, and the AVDTP stream. Stats arrive on "hci.stats" once a second; the
// graphs read /api/hci/history for the selected connection at the same pace, only while visible.

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
    }).join('') : '<tr><td class="muted">No connections seen since the monitor started.</td></tr>');
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
setupMonitor();
