'use strict';
// Audio → LE Audio: Auracast both ways (GET /api/le-audio). The board's own broadcasts — a BIG
// each, which goes on air while something plays into its node — and the broadcasts in range, each
// stream (BIS) received on request. What plays goes through Listen's route, as everywhere: a
// broadcast here is turned on or off as one of its outputs, and one received becomes its input.
// States are BlueZ's as they are: a stream that does not sync stays where it got to, and Listen
// again is the retry.

const la = {s: null, streams: [], route: null, poll: null, btT: null, busy: {}, codes: {}, ending: {}};
// The route's outputs, and whether the board's broadcast `node` is one of them.
const laOuts = () => la.route ? la.route.outputs : [];
const laOn = node => laOuts().some(k => k.target === node);
// The route's input is a broadcast received: the board does not broadcast as well (as in Listen).
const laReceiving = () => !!la.route && la.route.input.type === 'bluetooth' && la.route.input.devices.some(d => d.kind === 'broadcast');

// The Front Left, Front Right … bits of an audio location, as the BAP spec names them.
const LOC = ['FL', 'FR', 'FC', 'LFE', 'BL', 'BR', 'FLC', 'FRC', 'BC', 'LFE2', 'SL', 'SR'];
const locName = v => !v ? 'mono' : LOC.filter((_, i) => v & (1 << i)).join('+') || '0x' + v.toString(16);

const laAdapter = () => {
  const ads = (la.s && la.s.adapters) || [];
  const able = ads.filter(a => a.broadcaster || a.receiver);
  const want = $('la-ad').value || lsGet('btbench.la.adapter');
  return able.find(a => a.adapter === want) || able[0] || null;
};

// Octets per channel per frame of each BAP broadcast configuration (BAP table 6.4).
const OCTETS = {'8_1': 26, '8_2': 30, '16_1': 30, '16_2': 40, '24_1': 45, '24_2': 60, '32_1': 60, '32_2': 80,
  '48_1': 75, '48_2': 100, '48_3': 90, '48_4': 120, '48_5': 117, '48_6': 155};
function presetLabel(p) {
  const [rate, frame, qos] = p.split('_');
  const ms = frame % 2 ? 7.5 : 10;
  return `${p} · ${rate} kHz, ${ms} ms, ${Math.round(OCTETS[rate + '_' + frame] * 8 / ms)} kbit/s a channel · ${qos === '1' ? 'low latency' : 'high reliability'}`;
}
const PRESETS = ['48_2_1', '48_2_2', '48_4_1', '48_4_2', '48_6_1', '48_6_2', '48_1_1', '48_1_2', '48_3_1', '48_3_2',
  '48_5_1', '48_5_2', '32_2_1', '32_2_2', '24_2_1', '24_2_2', '16_2_1', '16_2_2', '8_2_1', '8_2_2'];

function qosLine(q, codec) {
  const parts = [];
  if (q.sdu != null && q.interval_us) parts.push(`${q.sdu} B every ${q.interval_us / 1000} ms`);
  if (codec && codec.bitrate) parts.push(`${Math.round(codec.bitrate * (codec.channels || 1) / 1000)} kbit/s`);
  if (q.rtn != null) parts.push(`${q.rtn} retransmissions`);
  if (q.latency_ms != null) parts.push(`≤ ${q.latency_ms} ms`);
  if (q.phy) parts.push(q.phy);
  if (q.presentation_delay_us != null) parts.push(`presentation delay ${q.presentation_delay_us / 1000} ms`);
  return parts.join(' · ');
}

// ---------------------------------------------------------------- the board's broadcasts

function broadcastRow(b) {
  const on = b.state === 'active';
  const st = on ? pill('on air', 'good') : b.state === 'idle' ? pill('ready · off air') : pill(b.state, 'warn');
  const out = b.node && laOn(b.node), blocked = !out && laReceiving();
  // Streams started elsewhere into it (Audio → Streams, bench) are named; Listen's is its input.
  const others = la.streams.filter(s => s.id !== (la.route && la.route.stream) && (s.sinks || []).some(k => k.target === b.node) && s.state !== 'ended');
  const playing = [out ? 'what Listen plays' : '', ...others.map(s => esc(s.label.split(' → ')[0]))].filter(Boolean).join(', ') ||
    `<span class="muted">${!b.node ? 'nothing (PipeWire has no node for it yet)' : blocked ? 'nothing: Listen receives a broadcast' : 'nothing: turn on Play to, here or in Listen'}</span>`;
  return `<div class="rw"><div class="rw-main"><span class="rw-led${on ? ' on' : ''}"></span>
      <div class="rw-title"><b>BIG ${b.big}${b.bis != null && b.bis !== 255 ? ' · BIS ' + b.bis : ''}</b> ${st}${b.encrypted ? ' ' + pill('encrypted') : ''}
        <span class="rw-sub"><span>${esc(b.codec.summary)}${b.codec.channels ? ' · ' + (b.codec.channels === 2 ? 'stereo' : 'mono') : ''}</span><span>${esc(qosLine(b.qos, b.codec))}</span></span></div>
      <div class="rw-acts">${b.node ? `<button data-la-out="${esc(b.node)}" class="${out ? 'on' : ''}" aria-pressed="${!!out}"${blocked ? ' disabled' : ''}
          title="One of Listen's outputs: what Listen plays goes on air">${out ? 'Playing to it' : 'Play to'}</button>` : ''}
        <button data-la-end="${b.big}" class="danger"${la.ending[b.big] ? ' disabled' : ''}>${la.ending[b.big] ? 'going off air…' : 'Stop broadcast'}</button></div></div>
    <div class="rw-note small">Plays: ${playing}</div></div>`;
}

// ---------------------------------------------------------------- broadcasts in range

function streamRow(src, t) {
  const st = t.state === 'active' ? pill('receiving', 'good') : t.state === 'broadcasting' || t.state === 'pending' ? pill('syncing…', 'warn')
    : t.state === 'idle' ? '' : pill(t.state);
  const selected = t.state !== 'idle';
  const busy = la.busy[t.path];
  const code = t.encrypted && !selected ? `<input class="la-code" data-path="${esc(t.path)}" placeholder="Broadcast Code" maxlength="34" value="${esc(la.codes[t.path] || '')}" autocomplete="off">` : '';
  return `<div class="ad-link">
      <span class="ad-dir"><b>BIS ${t.bis != null ? t.bis : '?'}</b> <span class="muted small">${esc(locName(t.location))}</span></span>
      <span class="ad-codec">${t.program_info ? `<b>${esc(t.program_info)}</b> ` : ''}<span class="small">${esc(t.codec.summary)}</span></span>
      <span class="ad-state">${st}${t.encrypted ? ' ' + pill('encrypted') : ''}</span>
      <span class="ad-acts">${code}
        <button class="small-btn" data-la-rx="${esc(t.path)}" data-addr="${esc(src.address)}" data-on="${selected ? 0 : 1}"${busy ? ' disabled' : ''}
          title="${selected ? 'Stop receiving it' : 'Receive it as Listen’s input'}">${busy ? '…' : selected ? 'Stop' : 'Listen'}</button>
</span>
    </div>`;
}

function sourceCard(src) {
  // The signal comes from scanning: without one, a broadcast being received is still heard.
  const rx = src.streams.some(t => t.state === 'active');
  const sig = src.rssi != null ? `${sigBars(src.rssi)} <span class="small muted">${src.rssi} dBm</span>`
    : rx ? '' : '<span class="small muted">not heard now</span>';
  return `<div class="card ad-card">
    <div class="card-head"><h3>${esc(src.name || src.address)} <span class="mono small muted">${esc(src.address)}</span></h3>
      <div class="acts">${sig}</div></div>
    <div class="small muted">${src.broadcast_id ? 'Broadcast ID <span class="mono">' + esc(src.broadcast_id) + '</span>' : ''}${src.paired ? ' · paired with the board' : ''}${src.encrypted ? ' · encrypted' : ''}</div>
    ${src.streams.length ? src.streams.map(t => streamRow(src, t)).join('')
      : '<p class="hint">Its streams are not read yet: BlueZ reads the BASE while the adapter scans. Scan, and stay in range.</p>'}
  </div>`;
}

// ---------------------------------------------------------------- render

function renderLeAudio(s) {
  la.s = s;
  loaded($('la-bar'));
  const able = s.adapters.filter(a => a.broadcaster || a.receiver);
  $('la-ad-l').hidden = able.length < 2;
  fillSelect($('la-ad'), able.map(a => ({value: a.adapter, label: `${a.adapter} · ${a.address}`})), true);
  const a = laAdapter();
  setText($('la-caps'), a ? `${a.adapter} ${a.address} · ${[a.broadcaster && 'broadcasts', a.receiver && 'receives'].filter(Boolean).join(' and ')}`
    : 'no adapter here has LE Audio broadcast (ISO broadcaster or sync receiver)');
  const form = $('la-form');
  form.querySelector('[type=submit]').disabled = !(a && a.broadcaster && a.pac);
  if (a && a.broadcaster && !a.pac) setText($('la-msg'), 'No broadcast endpoint: PipeWire registers it with the LE Audio broadcast source role (System → Profiles).');
  else if ($('la-msg').dataset.sticky !== '1') setText($('la-msg'), '');
  const mine = s.broadcasts.filter(b => !a || b.adapter === a.adapter);
  setHTML($('la-bcasts'), mine.map(broadcastRow).join('') || '<div class="empty">The board broadcasts nothing. Start a broadcast, then play into it: Auracast receivers in range find it and can listen.</div>');
  loaded($('la-bcasts'));
  // Not while a code is being typed: the rebuild would take the field away.
  const ae = document.activeElement;
  if (!(ae && ae.classList.contains('la-code'))) {
    const srcs = s.sources.filter(x => !a || x.adapter === a.adapter);
    setHTML($('la-sources'), srcs.map(sourceCard).join('') || `<div class="card empty-card"><h3>No broadcast found</h3>
      <p class="hint">${a && a.receiver ? 'Scan to find the Auracast broadcasts in range (a TV, a transmitter, a phone sharing its audio, another board).' : 'This adapter cannot receive broadcasts.'}</p></div>`);
    loaded($('la-sources'));
  }
  const scanning = !!(a && a.discovering);
  setText($('la-scan-st'), scanning ? 'scanning' : '');
  setClass($('la-scan-st'), scanning ? 'pill warn' : 'pill');
  $('la-scan-st').hidden = !scanning;
  setText($('la-scan'), scanning ? 'Stop scan' : 'Scan');
  $('la-scan').disabled = !(a && a.receiver);
}

function refreshLeAudio() {
  return Promise.all([api('/le-audio'), api('/audio/streams').catch(() => ({streams: []})), api('/audio/route').catch(() => null)])
    .then(([s, st, r]) => { la.streams = st.streams || []; la.route = r; renderLeAudio(s); })
    .catch(e => { ldFail(e, $('la-bcasts'), $('la-sources')); setText($('la-caps'), e.message); });
}

// ---------------------------------------------------------------- actions

// Stop broadcast: what plays into it stops, then the BIG is cleared once it is off air — PipeWire
// lets it go a few seconds after its last stream (clearing it on air crashed bluetoothd).
function endBroadcast(btn, a) {
  const big = parseInt(btn.dataset.laEnd, 10);
  const b = la.s.broadcasts.find(x => x.big === big && (!a || x.adapter === a.adapter));
  la.ending[big] = true;
  btn.disabled = true;
  setText(btn, 'going off air…');
  // Taken off Listen's outputs (the route's stream plays on to the others), and the streams
  // started elsewhere into it stopped.
  const node = b && b.node;
  const plays = b ? la.streams.filter(s => s.id !== (la.route && la.route.stream) && (s.sinks || []).some(k => k.target === node) && s.state !== 'ended') : [];
  const offRoute = node && laOn(node) ? put('/audio/route', {outputs: laOuts().filter(k => k.target !== node)}) : Promise.resolve();
  const deadline = Date.now() + 20000;
  const clear = () => api('/le-audio').then(s => {
    const now = s.broadcasts.find(x => x.big === big && (!a || x.adapter === a.adapter));
    if (now && now.state !== 'idle') {
      if (Date.now() > deadline) throw new Error('It is still on air: PipeWire did not let it go. Stop what plays into it (Audio → Streams).');
      return new Promise(r => setTimeout(r, 1000)).then(clear);
    }
    return now ? del(`/le-audio/broadcasts/${big}?adapter=${a ? a.adapter : ''}`) : null;
  });
  offRoute.then(() => Promise.all(plays.map(s => del(`/audio/streams/${s.id}`).catch(() => {})))).then(clear)
    .catch(fail).finally(() => { delete la.ending[big]; refreshLeAudio(); });
}

function setupLeAudio() {
  fillSelect($('la-preset'), PRESETS.map(p => ({value: p, label: presetLabel(p)})));
  $('la-preset').value = lsGet('btbench.la.preset') || '48_2_1';
  $('la-ad').onchange = e => { lsSet('btbench.la.adapter', e.target.value); renderLeAudio(la.s); };
  $('la-form').onsubmit = e => {
    e.preventDefault();
    const f = e.target, a = laAdapter();
    lsSet('btbench.la.preset', f.preset.value);
    const body = {adapter: a ? a.adapter : '', preset: f.preset.value, channels: parseInt(f.channels.value, 10),
      code: f.code.value, presentation_delay_us: Math.round((parseFloat(f.pd_ms.value) || 40) * 1000)};
    setText($('la-msg'), 'starting…');
    post('/le-audio/broadcasts', body).then(r => {
      setText($('la-msg'), `BIG ${r.big} is ready: play into it to go on air.`);
      return refreshLeAudio();
    }).catch(err => setText($('la-msg'), err.message));
  };
  $('la-bcasts').onclick = e => {
    const end = e.target.closest('[data-la-end]'), out = e.target.closest('[data-la-out]');
    if (end) endBroadcast(end, laAdapter()); else if (out) {
      const node = out.dataset.laOut;
      const next = laOn(node) ? laOuts().filter(k => k.target !== node) : laOuts().concat([{type: 'pipewire', target: node}]);
      if (next.length > 4) { toast('The board plays to at most 4 outputs at once.'); return; }
      out.disabled = true;
      put('/audio/route', {outputs: next}).then(refreshLeAudio).catch(fail);
    }
  };
  $('la-sources').oninput = e => { if (e.target.classList.contains('la-code')) la.codes[e.target.dataset.path] = e.target.value; };
  $('la-sources').onclick = e => {
    const rx = e.target.closest('[data-la-rx]');
    if (!rx) return;
    // Listen: received, and Listen's input from then on (its outputs keep playing it; the board's
    // own broadcasts go off them, as Listen does). Stop: no longer the input, then let go.
    const path = rx.dataset.laRx, on = rx.dataset.on === '1', addr = rx.dataset.addr;
    const isInput = !!la.route && la.route.input.type === 'bluetooth' && la.route.input.devices.some(d => d.kind === 'broadcast' && d.address.toUpperCase() === addr.toUpperCase());
    const own = la.s.broadcasts.map(b => b.node).filter(Boolean);
    const outs = laOuts().filter(k => !own.includes(k.target));
    la.busy[path] = true;
    renderLeAudio(la.s);
    const receive = () => put('/le-audio/streams', {path, receive: on, code: la.codes[path] || ''});
    (on ? receive().then(() => put('/audio/route', Object.assign({input: {type: 'bluetooth', devices: [{address: addr, kind: 'broadcast'}]}},
      outs.length !== laOuts().length ? {outputs: outs} : {})))
      : (isInput ? put('/audio/route', {input: {type: 'none'}}) : Promise.resolve()).then(receive))
      .catch(fail).finally(() => { delete la.busy[path]; refreshLeAudio(); });
  };
  $('la-scan').onclick = () => {
    const a = laAdapter();
    if (!a) return;
    const on = !a.discovering;
    post('/bluetooth/scan', on ? {on: true, adapter: a.adapter, transport: 'le', seconds: 30} : {on: false, adapter: a.adapter})
      .then(() => setTimeout(refreshLeAudio, 300)).catch(fail);
  };
}

registerTab('leaudio', {
  topics: () => ['bt'],
  show() {
    if (!la.setup) { la.setup = true; setupLeAudio(); }
    refreshLeAudio();
    clearInterval(la.poll);
    // A stream's state (syncing, receiving) and the broadcasts found change on their own; "bt"
    // brings device changes sooner.
    la.poll = setInterval(refreshLeAudio, 2500);
  },
  hide() { clearInterval(la.poll); la.poll = null; },
  onMessage(topic) {
    if (currentTab !== 'leaudio' || topic !== 'bt' || la.btT) return;
    // At most one look a second: "bt" carries every RSSI change during a scan.
    la.btT = setTimeout(() => { la.btT = null; refreshLeAudio(); }, 1000);
  },
});
