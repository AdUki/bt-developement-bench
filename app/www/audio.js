'use strict';
// Audio: which stack owns Bluetooth audio (btbench-audio); the AVRCP players of connected phones
// (controls, metadata, browsing); streams — a test signal, a radio or UPnP URL, or one device's
// audio — played into a sink and/or listened to here; and BlueZ's transports and endpoints.
//
// Listening is plain fetch() of the stream's endless WAV, scheduled into Web Audio: no media
// element (which buffers seconds and cannot say how much), and the AudioContext is made in the
// click that asked for it, so autoplay rules let it play.

const AUDIO_MODES = ['pipewire', 'bluealsa', 'none'];
const au = {endpoints: null, media: null, streams: [], cards: {}, listeners: {}, playerClock: {},
  tick: null, radio: [], upnp: {server: null, path: []}};

function renderAudio(s) {
  setText($('au-mode'), s.mode);
  setClass($('au-mode'), 'pill good');
  setHTML($('au-buttons'), (s.modes || AUDIO_MODES).map(m =>
    `<button data-mode="${esc(m)}"${m === s.mode ? ' disabled' : ''}>${esc(m)}</button>`).join(' '));
  setText($('au-services'), Object.entries(s.services || {}).map(([k, v]) => `${k}: ${v}`).join(' · '));
}

function refreshAudio() {
  return api('/audio').then(s => { renderAudio(s); setText($('au-msg'), ''); }).catch(e => {
    setText($('au-mode'), e.status === 503 ? 'not here' : 'error');
    setHTML($('au-buttons'), '');
    // On a PC the target scripts are not installed; say so plainly rather than as an error.
    setText($('au-msg'), e.status === 503 ? 'The audio-mode switch runs on the board only (btbench-audio is not installed on this machine).' : e.message);
  });
}

// ---------------------------------------------------------------- endpoints

const epLabel = e => `${e.label}${e.codec ? ' · ' + e.codec : ''}${e.default ? ' (default)' : ''}`;


function renderEndpoints(ep) {
  au.endpoints = ep;
  const t = ep.tools || {};
  const missing = ['pw-cat', 'mpg123', 'ffmpeg'].filter(k => !t[k]);
  setText($('st-backend'), `${ep.backend === 'pipewire' ? 'PipeWire' : ep.backend === 'alsa' ? 'ALSA / BlueALSA' : 'no audio stack'}` +
    `${ep.error ? ' · ' + ep.error : ''}${missing.length ? ' · not installed: ' + missing.join(', ') : ''}`);
  const sinkKind = ep.backend === 'alsa' ? 'alsa' : 'pipewire';
  const sinks = [{value: '', label: 'nowhere (listen only)'}];
  if (ep.backend === 'pipewire') sinks.push({value: 'pipewire:', label: 'the default sink'});
  ep.sinks.forEach(e => sinks.push({value: `${sinkKind}:${e.id}`, label: epLabel(e)}));
  fillSelect($('st-sink'), sinks, idle($('st-sink')) ? lsGet('btbench.au.sink') : null);
  const caps = ep.sources.map(e => ({value: `${e.backend}:${e.id}`, label: epLabel(e)}));
  if (ep.backend === 'pipewire') caps.unshift({value: 'pipewire:', label: 'the default source'});
  fillSelect($('st-capture'), caps);
  fillSelect($('st-monitor'), ep.backend === 'pipewire' ? ep.sinks.map(e => ({value: e.id, label: epLabel(e)})) : []);
}

function refreshEndpoints(fresh) {
  return api('/audio/endpoints' + (fresh ? '?fresh=1' : '')).then(renderEndpoints)
    .catch(e => setText($('st-backend'), e.message));
}

// The sink chosen in the form, as a stream's sink: null (listen only), or {type, target}.
function chosenSink() {
  const v = $('st-sink').value;
  if (!v) return null;
  const i = v.indexOf(':');
  return {type: v.slice(0, i), target: v.slice(i + 1)};
}

// ---------------------------------------------------------------- streams

function sourceFromForm() {
  const type = $('st-src').value;
  const num = id => parseFloat($(id).value);
  switch (type) {
    case 'tone': {
      const r = $('st-freq-r').value.trim();
      return {type, freq: num('st-freq'), freq_right: r ? parseFloat(r) : null, level_db: num('st-level')};
    }
    case 'sweep':
      return {type, from: num('st-from'), to: num('st-to'), seconds: num('st-secs'), log: $('st-log').checked,
        repeat: $('st-repeat').checked, level_db: num('st-level')};
    case 'noise': return {type, color: $('st-color').value, level_db: num('st-level')};
    case 'url': return {type, url: $('st-url').value.trim(), decoder: $('st-decoder').value};
    case 'capture': {
      const v = $('st-capture').value, i = v.indexOf(':');
      return {type, backend: v.slice(0, i) || 'pipewire', target: v.slice(i + 1),
        title: $('st-capture').selectedOptions[0] ? $('st-capture').selectedOptions[0].textContent : ''};
    }
    case 'monitor':
      return {type: 'capture', backend: 'pipewire', target: $('st-monitor').value, monitor: true,
        title: 'what ' + ($('st-monitor').selectedOptions[0] ? $('st-monitor').selectedOptions[0].textContent : 'it') + ' plays'};
    default: return {type};
  }
}

function startStream(source, sink, listen, extra) {
  const body = Object.assign({source, sink, rate: parseInt($('st-rate').value, 10),
    channels: parseInt($('st-ch').value, 10), gain_db: parseFloat($('st-gain').value) || 0}, extra || {});
  // The AudioContext must come from the click itself, before any await.
  const ctx = listen ? newAudioContext() : null;
  setText($('st-msg'), 'starting…');
  return post('/audio/streams', body).then(s => {
    setText($('st-msg'), '');
    upsertStream(s);
    if (listen) startListening(s.id, ctx);
    return s;
  }).catch(e => {
    if (ctx) ctx.close();
    setText($('st-msg'), e.message);
    throw e;
  });
}

const STATE_CLS = {running: 'good', starting: 'warn', failed: 'bad', ended: '', stopped: ''};

// A level bar from -60 dBFS (empty) to 0 (full); the peak is the thin mark.
function meterHTML(lv, ch) {
  const n = Math.max(1, (lv && lv.rms_db.length) || ch || 1);
  let h = '';
  for (let c = 0; c < n; c++) {
    const rms = lv && lv.rms_db[c] != null ? lv.rms_db[c] : -120;
    const pk = lv && lv.peak_db[c] != null ? lv.peak_db[c] : -120;
    const w = v => Math.max(0, Math.min(100, (v + 60) / 60 * 100)).toFixed(1);
    h += `<div class="meter${pk > -1 ? ' clip' : pk > -6 ? ' hot' : ''}"><i style="width:${w(rms)}%"></i><b style="left:${w(pk)}%"></b>` +
      `<span>${n > 1 ? (c ? 'R ' : 'L ') : ''}${pk > -119 ? pk.toFixed(1) : '—'} dB</span></div>`;
  }
  return h;
}

function streamCard(s) {
  const el = document.createElement('div');
  el.className = 'card dev stream';
  el.dataset.id = s.id;
  el.innerHTML = `<div class="head"><span><b class="st-label"></b> <span class="pill st-state"></span></span>
      <span class="acts"><button class="small-btn st-listen">listen</button><button class="small-btn danger st-stop">stop</button></span></div>
    <div class="small st-meta"></div>
    <div class="st-meters"></div>
    <div class="row wrap small">
      <label class="row">gain <input type="range" class="st-gain grow" min="-60" max="20" step="0.5"> <span class="mono st-gain-v"></span></label>
      ${s.source.type === 'tone' ? `<label class="row">Hz <input type="number" class="st-freq narrow" min="1"></label>` : ''}
      ${s.source.type === 'tone' || s.source.type === 'noise' ? `<label class="row">level <input type="number" class="st-level narrow" min="-96" max="0"> dB</label>` : ''}
    </div>
    <div class="st-listening small" hidden>
      <div class="row wrap"><span class="st-buf mono"></span>
        <label class="row">volume <input type="range" class="st-vol" min="0" max="1" step="0.01" value="1"></label></div>
      <canvas class="graph short st-spec"></canvas>
    </div>
    <div class="small bad-text st-err"></div>`;
  return el;
}

function upsertStream(s) {
  let el = au.cards[s.id];
  if (!el) {
    el = au.cards[s.id] = streamCard(s);
    $('st-list').prepend(el);
  }
  const q = c => el.querySelector(c);
  setText(q('.st-label'), s.label);
  setText(q('.st-state'), s.state);
  setClass(q('.st-state'), 'pill st-state ' + (STATE_CLS[s.state] || ''));
  const meta = [];
  if (s.meta.title) meta.push('♪ ' + s.meta.title);
  if (s.meta.name) meta.push(s.meta.name);
  meta.push(`${s.rate} Hz · ${s.channels === 1 ? 'mono' : 'stereo'} · ${fmtMs(s.seconds * 1000)}`);
  if (s.listeners) meta.push(`${s.listeners} listening`);
  setText(q('.st-meta'), meta.join(' · '));
  setHTML(q('.st-meters'), s.state === 'running' ? meterHTML(s.level, s.channels) : '');
  setText(q('.st-err'), s.error || '');
  const g = q('.st-gain');
  if (idle(g)) g.value = s.gain_db;
  setText(q('.st-gain-v'), `${(+s.gain_db).toFixed(1)} dB`);
  const f = q('.st-freq');
  if (f && idle(f)) f.value = s.source.freq;
  const lv = q('.st-level');
  if (lv && idle(lv)) lv.value = s.source.level_db;
  const live = s.state === 'running' || s.state === 'starting';
  q('.st-listen').hidden = !live;
  setText(q('.st-listen'), au.listeners[s.id] ? 'mute here' : 'listen');
  setText(q('.st-stop'), live ? 'stop' : 'remove');
  if (!live && au.listeners[s.id]) stopListening(s.id);
}

function renderStreams(list) {
  au.streams = list.streams;
  const ids = new Set(list.streams.map(s => String(s.id)));
  for (const id of Object.keys(au.cards)) {
    if (!ids.has(id)) {
      au.cards[id].remove();
      delete au.cards[id];
      stopListening(id);
    }
  }
  // Oldest first into prepend(): newest ends on top.
  list.streams.slice().reverse().forEach(upsertStream);
}

const refreshStreams = () => api('/audio/streams').then(renderStreams).catch(fail);

// ---------------------------------------------------------------- listening (Web Audio)

function newAudioContext() {
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) { toast('This browser has no Web Audio'); return null; }
  return new AC({latencyHint: 'playback'});
}

// One listening stream: reads the WAV, turns each ~100 ms of samples into an AudioBuffer and
// schedules it right after the previous one, keeping about a third of a second queued. A gap
// (Wi-Fi) re-primes that cushion; more than two seconds queued (a background tab let it pile
// up) drops blocks to catch up.
class Listen {
  constructor(id, ctx) {
    this.id = id;
    this.ctx = ctx;
    this.abort = new AbortController();
    this.gain = ctx.createGain();
    this.analyser = ctx.createAnalyser();
    this.analyser.fftSize = 2048;
    this.gain.connect(this.analyser);
    this.analyser.connect(ctx.destination);
    this.next = 0;
    this.dropped = 0;
    this.underruns = 0;
  }

  async run() {
    const r = await fetch(`/api/audio/streams/${this.id}/listen`, {signal: this.abort.signal});
    if (!r.ok) {
      let msg = r.statusText;
      try { msg = (await r.json()).error || msg; } catch (e) { /* not JSON */ }
      throw new Error(msg);
    }
    const reader = r.body.getReader();
    let head = new Uint8Array(0);
    let rate = 0, ch = 0, rest = new Uint8Array(0);
    // The header: RIFF, then chunks until "data" (fmt gives the rate and the channels).
    while (!rate) {
      const {value, done} = await reader.read();
      if (done) return;
      head = concat(head, value);
      const p = parseWavHeader(head);
      if (p) { rate = p.rate; ch = p.channels; rest = head.subarray(p.dataAt); }
    }
    this.rate = rate;
    this.ch = ch;
    const block = Math.round(rate / 10) * ch * 2;  // bytes in ~100 ms
    let pend = rest;
    for (;;) {
      while (pend.length >= block) {
        this.schedule(pend.subarray(0, block));
        pend = pend.subarray(block);
      }
      const {value, done} = await reader.read();
      if (done) break;
      pend = concat(pend, value);
    }
  }

  schedule(bytes) {
    const frames = bytes.length / (2 * this.ch);
    const buf = this.ctx.createBuffer(this.ch, frames, this.rate);
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
    for (let c = 0; c < this.ch; c++) {
      const out = buf.getChannelData(c);
      for (let i = 0; i < frames; i++) out[i] = dv.getInt16((i * this.ch + c) * 2, true) / 32768;
    }
    const now = this.ctx.currentTime;
    if (this.next < now + 0.02) {
      if (this.next) this.underruns++;
      this.next = now + 0.3;
    } else if (this.next > now + 2) {
      this.dropped++;
      return;
    }
    const src = this.ctx.createBufferSource();
    src.buffer = buf;
    src.connect(this.gain);
    src.start(this.next);
    this.next += buf.duration;
  }

  buffered() { return Math.max(0, this.next - this.ctx.currentTime); }

  stop() {
    this.abort.abort();
    this.ctx.close().catch(() => {});
  }
}

function concat(a, b) {
  const c = new Uint8Array(a.length + b.length);
  c.set(a);
  c.set(b, a.length);
  return c;
}

function parseWavHeader(h) {
  if (h.length < 12) return null;
  const dv = new DataView(h.buffer, h.byteOffset, h.length);
  const tag = o => String.fromCharCode(h[o], h[o + 1], h[o + 2], h[o + 3]);
  if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') throw new Error('not a WAV stream');
  let pos = 12, rate = 0, channels = 0;
  while (pos + 8 <= h.length) {
    const id = tag(pos), len = dv.getUint32(pos + 4, true);
    if (id === 'fmt ' && pos + 16 <= h.length) {
      channels = dv.getUint16(pos + 10, true);
      rate = dv.getUint32(pos + 12, true);
    }
    if (id === 'data') return rate ? {rate, channels, dataAt: pos + 8} : null;
    pos += 8 + len + (len & 1);
  }
  return null;
}

function startListening(id, ctx) {
  if (au.listeners[id]) return;
  ctx = ctx || newAudioContext();
  if (!ctx) return;
  const l = new Listen(id, ctx);
  au.listeners[id] = l;
  const el = au.cards[id];
  if (el) el.querySelector('.st-listening').hidden = false;
  l.run().catch(e => { if (e.name !== 'AbortError') toast('listen: ' + e.message); })
    .finally(() => stopListening(id));
  drawListening();
  const s = au.streams.find(x => x.id === id);
  if (s) upsertStream(s);
}

function stopListening(id) {
  const l = au.listeners[id];
  if (!l) return;
  delete au.listeners[id];
  l.stop();
  const el = au.cards[id];
  if (el) {
    el.querySelector('.st-listening').hidden = true;
    setText(el.querySelector('.st-listen'), 'listen');
  }
}

// The spectrum of what is being heard, while anything is: one requestAnimationFrame loop for all.
function drawListening() {
  if (drawListening.on) return;
  drawListening.on = true;
  const frame = () => {
    const ids = Object.keys(au.listeners);
    if (!ids.length || currentTab !== 'audio') { drawListening.on = false; return; }
    for (const id of ids) {
      const l = au.listeners[id], el = au.cards[id];
      if (!el) continue;
      setText(el.querySelector('.st-buf'), `${(l.buffered() * 1000).toFixed(0)} ms buffered` +
        (l.underruns ? ` · ${l.underruns} gaps` : '') + (l.dropped ? ` · ${l.dropped} blocks dropped` : ''));
      drawSpectrum(el.querySelector('.st-spec'), l);
    }
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}

function drawSpectrum(cv, l) {
  const dpr = window.devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight;
  if (!w) return;
  if (cv.width !== Math.round(w * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
  const g = cv.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  const bins = new Float32Array(l.analyser.frequencyBinCount);
  l.analyser.getFloatFrequencyData(bins);
  // Log frequency axis 20 Hz .. Nyquist, -100..0 dB.
  const ny = l.ctx.sampleRate / 2, lo = Math.log(20), hi = Math.log(ny);
  const style = getComputedStyle(document.documentElement);
  g.strokeStyle = style.getPropertyValue('--accent').trim();
  g.lineWidth = 1.2;
  g.beginPath();
  for (let i = 1; i < bins.length; i++) {
    const f = i * ny / bins.length;
    if (f < 20) continue;
    const x = (Math.log(f) - lo) / (hi - lo) * w;
    const y = h - Math.max(0, Math.min(1, (bins[i] + 100) / 100)) * (h - 4);
    if (i === 1) g.moveTo(x, y); else g.lineTo(x, y);
  }
  g.stroke();
  g.fillStyle = style.getPropertyValue('--muted').trim();
  g.font = '10px system-ui';
  for (const f of [100, 1000, 10000]) {
    if (f >= ny) continue;
    const x = (Math.log(f) - lo) / (hi - lo) * w;
    g.fillText(f >= 1000 ? f / 1000 + 'k' : String(f), x + 2, h - 3);
  }
}

// ---------------------------------------------------------------- AVRCP players

const fmtMs = ms => {
  const s = Math.max(0, Math.floor(ms / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
};

// BlueZ updates Position when the phone says (on play, pause, seek, and now and then); between
// those the console counts on from the moment it heard it.
function playerPosition(p) {
  const c = au.playerClock[p.path];
  if (!c) return p.position_ms;
  let pos = c.pos;
  if (p.status === 'playing') pos += Date.now() - c.at;
  else if (p.status === 'forward-seek') pos += (Date.now() - c.at) * 4;
  else if (p.status === 'reverse-seek') pos -= (Date.now() - c.at) * 4;
  const d = p.track.duration_ms;
  return Math.max(0, d ? Math.min(pos, d) : pos);
}

function notePlayers(players) {
  for (const p of players) {
    const c = au.playerClock[p.path];
    if (!c || c.pos !== p.position_ms || c.status !== p.status) au.playerClock[p.path] = {pos: p.position_ms, at: Date.now(), status: p.status};
  }
}

function selectHTML(cls, path, cur, options) {
  if (cur == null) return '';
  return `<select class="${cls}" data-path="${esc(path)}">${options.map(o =>
    `<option${o === cur ? ' selected' : ''}>${esc(o)}</option>`).join('')}</select>`;
}

function playerCard(p, transports) {
  const t = p.track || {};
  const tr = p.transport && transports.find(x => x.path === p.transport);
  const d = t.duration_ms || 0;
  const num = t.track_number ? `track ${t.track_number}${t.number_of_tracks ? ' of ' + t.number_of_tracks : ''}` : '';
  const btn = (a, label, title) => `<button class="pl-btn" data-a="${a}" data-path="${esc(p.path)}" title="${title}">${label}</button>`;
  const playing = p.status === 'playing';
  return `<div class="card dev player" data-path="${esc(p.path)}">
    <div class="head"><span><b>${esc(p.name || 'Player')}</b> on <span class="mono">${esc(p.address)}</span>
      ${p.type ? `<span class="small muted">${esc(p.type)}${p.subtype ? ' / ' + esc(p.subtype) : ''}</span>` : ''}</span>
      ${pill(p.status || 'unknown', playing ? 'good' : p.status === 'paused' ? 'warn' : '')}</div>
    <div class="pl-track">
      <div class="pl-title">${esc(t.title || 'no track')}</div>
      <div class="small">${esc([t.artist, t.album].filter(Boolean).join(' — '))}</div>
      <div class="small muted">${esc([t.genre, num].filter(Boolean).join(' · '))}</div>
    </div>
    <div class="pl-progress"><i style="width:${d ? (playerPosition(p) / d * 100).toFixed(2) : 0}%"></i></div>
    <div class="small mono pl-time">${fmtMs(playerPosition(p))}${d ? ' / ' + fmtMs(d) : ''}</div>
    <div class="row wrap pl-controls">
      ${btn('previous', '⏮', 'previous')}${btn('rewind', '⏪', 'rewind (hold)')}
      ${playing ? btn('pause', '⏸', 'pause') : btn('play', '▶', 'play')}
      ${btn('stop', '⏹', 'stop')}${btn('fast-forward', '⏩', 'fast forward (hold)')}${btn('next', '⏭', 'next')}
      ${p.repeat != null ? `<label class="row small">repeat ${selectHTML('pl-repeat', p.path, p.repeat, ['off', 'singletrack', 'alltracks', 'group'])}</label>` : ''}
      ${p.shuffle != null ? `<label class="row small">shuffle ${selectHTML('pl-shuffle', p.path, p.shuffle, ['off', 'alltracks', 'group'])}</label>` : ''}
      <button class="small-btn pl-listen" data-address="${esc(p.address)}" title="Hear what this device sends the board">listen</button>
      ${p.browsable ? `<button class="small-btn pl-browse" data-path="${esc(p.path)}">browse</button>` : ''}
    </div>
    ${tr && tr.volume != null ? `<label class="row small">volume <input type="range" min="0" max="127" value="${tr.volume}" data-path="${esc(tr.path)}" class="grow pl-vol">
      <span class="mono">${tr.volume}</span></label>` : ''}
    ${p.items && p.items.length ? `<div class="pl-items small">${p.items.map(i => `<div class="row between">
        <span>${esc(i.metadata && i.metadata.title || i.name)}${i.metadata && i.metadata.artist ? ' <span class="muted">— ' + esc(i.metadata.artist) + '</span>' : ''}
          ${i.type === 'folder' ? '<span class="muted">(folder)</span>' : ''}</span>
        <span>${i.playable ? `<button class="small-btn pl-item" data-a="play" data-path="${esc(i.path)}">play</button>` : ''}
          ${i.type === 'folder' ? `<button class="small-btn pl-folder" data-player="${esc(p.path)}" data-path="${esc(i.path)}">open</button>` : ''}</span></div>`).join('')}</div>` : ''}
  </div>`;
}

function renderPlayers(m) {
  notePlayers(m.players);
  // Not while a select is open or a slider held: the rebuild would close it under the pointer.
  const ae = document.activeElement;
  if (ae && $('au-players').contains(ae) && (ae.tagName === 'SELECT' || ae.type === 'range')) return;
  setHTML($('au-players'), m.players.length ? m.players.map(p => playerCard(p, m.transports)).join('')
    : '<div class="small muted">No AVRCP players: connect a phone (or anything that plays) to the board as its audio sink.</div>');
}

// The progress bars move between updates.
function tickPlayers() {
  if (!au.media) return;
  for (const p of au.media.players) {
    if (p.status !== 'playing' && !String(p.status).endsWith('seek')) continue;
    const el = $('au-players').querySelector(`.player[data-path="${CSS.escape(p.path)}"]`);
    if (!el) continue;
    const d = p.track.duration_ms, pos = playerPosition(p);
    const bar = el.querySelector('.pl-progress i');
    if (bar && d) bar.style.width = (pos / d * 100).toFixed(2) + '%';
    setText(el.querySelector('.pl-time'), `${fmtMs(pos)}${d ? ' / ' + fmtMs(d) : ''}`);
  }
}

function playerAction(path, action) {
  return post('/media/players/control', {path, action}).catch(fail);
}

// Hear what a phone plays into the board: its capture endpoint, by address.
function listenToDevice(address) {
  const ep = au.endpoints;
  const src = ep && ep.sources.find(e => e.address && e.address.toUpperCase() === address.toUpperCase());
  if (!src) {
    toast(`No capture endpoint for ${address}: is it streaming to the board (A2DP source → the board as sink)?`);
    refreshEndpoints(true);
    return;
  }
  startStream({type: 'capture', backend: src.backend, target: src.id, title: src.label}, null, true,
    {rate: src.rate || 48000}).catch(() => {});
}

// ---------------------------------------------------------------- media (BlueZ)

function codecCell(c) {
  return `<b>${esc(c.name)}</b> <span class="small">${esc(c.summary)}</span>`;
}

function renderMedia(m) {
  au.media = m;
  renderPlayers(m);
  setHTML($('au-transports'), m.transports.length ? m.transports.map(t => `<div class="card dev">
      <div class="head"><span><b>${esc(t.profile || t.uuid)}</b> with <span class="mono">${esc(t.address)}</span></span>
        ${pill(t.state, t.state === 'active' ? 'good' : t.state === 'pending' ? 'warn' : '')}</div>
      <div class="small">${codecCell(t.codec)}</div>
      <div class="kv"><span>Configuration</span><span class="mono">${esc(t.configuration)}</span>
        <span>Delay</span><span>${t.delay_ms != null ? t.delay_ms.toFixed(1) + ' ms' : '—'}</span>
        <span>Path</span><span class="mono small">${esc(t.path)}</span></div>
      ${t.volume != null ? `<label class="row">Volume <input type="range" min="0" max="127" value="${t.volume}" data-path="${esc(t.path)}" class="grow">
        <span class="mono">${t.volume}</span></label>` : ''}
    </div>`).join('') : '<div class="small muted">No transport: nothing is configured to stream.</div>');
  setHTML($('au-endpoints'), m.endpoints.length ? `<div class="scroll-x"><table class="tbl">
      <tr><th>Device</th><th>Role</th><th>Codec</th><th>Capabilities</th><th>Delay rep.</th></tr>
      ${m.endpoints.map(e => `<tr><td class="mono">${esc(e.address)}</td><td>${esc(e.role || e.uuid)}</td>
        <td>${esc(e.codec.name)}</td><td class="small">${esc(e.codec.summary)} <span class="mono muted">${esc(e.capabilities)}</span></td>
        <td>${e.delay_reporting ? 'yes' : 'no'}</td></tr>`).join('')}</table></div>`
    : '<div class="small muted">No remote endpoints (connect an A2DP device).</div>');
}

function refreshMedia() { return api('/media').then(renderMedia).catch(fail); }

// ---------------------------------------------------------------- radio

function renderRadio(r) {
  au.radio = r.stations;
  setHTML($('rd-list'), r.stations.length ? r.stations.map((s, i) => `<div class="row between radio-row">
      <span>${esc(s.name)} <span class="muted mono">${esc(s.url.replace(/^https?:\/\//, ''))}</span></span>
      <span><button class="small-btn" data-play="${i}">play</button><button class="small-btn" data-hear="${i}">listen</button></span></div>`).join('')
    : '<span class="muted">No stations.</span>');
}

function playUrl(url, title, listen) {
  return startStream({type: 'url', url, title}, chosenSink(), listen).catch(() => {});
}

// ---------------------------------------------------------------- UPnP

function renderUpnpServers(r) {
  setHTML($('up-servers'), r.servers.length ? r.servers.map(s => `<div class="row between">
      <span><b>${esc(s.name)}</b> <span class="muted">${esc([s.manufacturer, s.model].filter(Boolean).join(' '))}</span></span>
      <button class="small-btn" data-server="${esc(s.id)}" data-name="${esc(s.name)}">browse</button></div>`).join('')
    : `<span class="muted">${r.searched_ms ? 'No media servers answered' + (r.error ? ': ' + esc(r.error) : '.') : 'Not searched yet.'}</span>`);
}

function browseUpnp(server, object, title) {
  const u = au.upnp;
  if (server !== u.server) { u.server = server; u.path = []; }
  const at = u.path.findIndex(p => p.id === object);
  if (at >= 0) u.path = u.path.slice(0, at + 1); else u.path.push({id: object, title});
  setHTML($('up-path'), u.path.map((p, i) => `<a href="#" data-crumb="${i}">${esc(p.title)}</a>`).join(' / '));
  setHTML($('up-items'), '<span class="muted">browsing…</span>');
  api(`/audio/upnp/browse?server=${encodeURIComponent(server)}&object=${encodeURIComponent(object)}&count=200`).then(r => {
    au.upnp.items = r.items;
    setHTML($('up-items'), (r.containers.map(c => `<div class="row between"><span>📁 ${esc(c.title)}${c.child_count != null ? ` <span class="muted">(${c.child_count})</span>` : ''}</span>
        <button class="small-btn" data-open="${esc(c.id)}" data-title="${esc(c.title)}">open</button></div>`).join('') +
      r.items.map((it, i) => `<div class="row between"><span>${esc(it.title)}${it.artist ? ' <span class="muted">— ' + esc(it.artist) + '</span>' : ''}
          <span class="muted mono">${esc(it.mime || '')}${it.duration_s ? ' ' + fmtMs(it.duration_s * 1000) : ''}</span></span>
        <span>${it.url ? `<button class="small-btn" data-item="${i}">play</button><button class="small-btn" data-item-hear="${i}">listen</button>` : ''}</span></div>`).join('')) ||
      '<span class="muted">Empty.</span>') + (r.total > r.returned ? `<div class="muted">${r.returned} of ${r.total} shown</div>` : '');
  }).catch(e => setHTML($('up-items'), `<span class="bad-text">${esc(e.message)}</span>`));
}

// ---------------------------------------------------------------- setup

function showSourceFields() {
  const t = $('st-src').value;
  document.querySelectorAll('#st-new [data-for]').forEach(el => {
    el.hidden = !el.dataset.for.split(' ').includes(t);
  });
  setText($('st-level-kind'), t === 'noise' ? 'Level (dBFS, RMS)' : 'Level (dBFS, peak)');
  if (t === 'noise' && $('st-level').value === '-12') $('st-level').value = '-20';
}

function setupAudio() {
  $('au-buttons').onclick = e => {
    const b = e.target.closest('button[data-mode]');
    if (!b) return;
    const mode = b.dataset.mode;
    const restart = $('au-restart-bt').checked;
    if (!confirm(`Switch the audio stack to ${mode}? Audio connections are dropped${restart ? ' and bluetoothd restarts' : ''}.`)) return;
    setText($('au-msg'), `switching to ${mode}…`);
    put('/audio', {mode, restart_bt: restart}).then(s => { renderAudio(s); setText($('au-msg'), `now ${s.mode}`); refreshEndpoints(true); })
      .catch(e2 => { setText($('au-msg'), e2.message); refreshAudio(); });
  };
  // Sent as the slider settles, not on every pixel of a drag.
  const volume = e => {
    const r = e.target.closest('input[type=range][data-path]');
    if (r) put('/media/transport', {path: r.dataset.path, volume: parseInt(r.value, 10)}).catch(fail);
  };
  $('au-transports').onchange = volume;

  // Players: buttons, hold-to-seek, settings, browsing.
  const pl = $('au-players');
  pl.onchange = e => {
    volume(e);
    const s = e.target.closest('select[data-path]');
    if (s) put('/media/players', {path: s.dataset.path, [s.classList.contains('pl-repeat') ? 'repeat' : 'shuffle']: s.value}).catch(fail);
  };
  pl.onclick = e => {
    const b = e.target.closest('.pl-btn');
    if (b && b.dataset.a !== 'fast-forward' && b.dataset.a !== 'rewind') playerAction(b.dataset.path, b.dataset.a);
    const ls = e.target.closest('.pl-listen');
    if (ls) listenToDevice(ls.dataset.address);
    const br = e.target.closest('.pl-browse');
    if (br) post('/media/players/browse', {path: br.dataset.path}).then(refreshMedia).catch(fail);
    const fo = e.target.closest('.pl-folder');
    if (fo) post('/media/players/browse', {path: fo.dataset.player, folder: fo.dataset.path}).then(refreshMedia).catch(fail);
    const it = e.target.closest('.pl-item');
    if (it) post('/media/items', {path: it.dataset.path, action: 'play'}).catch(fail);
  };
  // Seeking runs while the button is held, as on a remote: pressed starts it, released resumes play.
  pl.onpointerdown = e => {
    const b = e.target.closest('.pl-btn');
    if (!b || (b.dataset.a !== 'fast-forward' && b.dataset.a !== 'rewind')) return;
    playerAction(b.dataset.path, b.dataset.a);
    const up = () => { window.removeEventListener('pointerup', up); playerAction(b.dataset.path, 'play'); };
    window.addEventListener('pointerup', up);
  };

  // The stream form.
  $('st-src').onchange = showSourceFields;
  showSourceFields();
  $('st-sink').onchange = () => lsSet('btbench.au.sink', $('st-sink').value);
  $('st-refresh').onclick = () => refreshEndpoints(true);
  $('st-start').onclick = () => {
    const src = sourceFromForm();
    const sink = chosenSink();
    const listen = $('st-listen').checked;
    if (!sink && !listen) { setText($('st-msg'), 'Choose where to play it, or tick "listen in this browser".'); return; }
    startStream(src, sink, listen).catch(() => {});
  };

  // The stream cards.
  const list = $('st-list');
  list.onclick = e => {
    const card = e.target.closest('.stream');
    if (!card) return;
    const id = parseInt(card.dataset.id, 10);
    if (e.target.closest('.st-stop')) {
      stopListening(id);
      const live = (au.streams.find(s => s.id === id) || {}).state;
      del(`/audio/streams/${id}`).then(() => {
        if (live !== 'running' && live !== 'starting') { au.cards[id] && au.cards[id].remove(); delete au.cards[id]; }
        refreshStreams();
      }).catch(fail);
    } else if (e.target.closest('.st-listen')) {
      if (au.listeners[id]) stopListening(id); else startListening(id);
    }
  };
  list.oninput = e => {
    const card = e.target.closest('.stream');
    if (!card) return;
    if (e.target.classList.contains('st-vol')) {
      const l = au.listeners[card.dataset.id];
      if (l) l.gain.gain.value = parseFloat(e.target.value);
    } else if (e.target.classList.contains('st-gain')) {
      setText(card.querySelector('.st-gain-v'), `${parseFloat(e.target.value).toFixed(1)} dB`);
    }
  };
  list.onchange = e => {
    const card = e.target.closest('.stream');
    if (!card) return;
    const id = card.dataset.id;
    let body = null;
    if (e.target.classList.contains('st-gain')) body = {gain_db: parseFloat(e.target.value)};
    else if (e.target.classList.contains('st-freq')) body = {source: {freq: parseFloat(e.target.value)}};
    else if (e.target.classList.contains('st-level')) body = {source: {level_db: parseFloat(e.target.value)}};
    if (body) put(`/audio/streams/${id}`, body).then(upsertStream).catch(fail);
  };

  // Radio.
  $('rd-list').onclick = e => {
    const b = e.target.closest('button');
    if (!b) return;
    const s = au.radio[parseInt(b.dataset.play != null ? b.dataset.play : b.dataset.hear, 10)];
    if (!s) return;
    if (b.dataset.play != null && !chosenSink()) { toast('Choose a sink in "Play to" above (or use listen).'); return; }
    playUrl(s.url, s.name, b.dataset.hear != null || $('st-listen').checked);
  };
  $('rd-edit').onclick = () => {
    $('rd-text').value = au.radio.map(s => `${s.name} | ${s.url}`).join('\n');
    $('rd-editor').hidden = false;
  };
  $('rd-cancel').onclick = () => { $('rd-editor').hidden = true; };
  $('rd-save').onclick = () => {
    const stations = $('rd-text').value.split('\n').map(l => l.trim()).filter(Boolean).map(l => {
      const i = l.lastIndexOf('|');
      return i < 0 ? {name: l, url: l} : {name: l.slice(0, i).trim(), url: l.slice(i + 1).trim()};
    });
    put('/audio/radio', {stations}).then(r => { renderRadio(r); $('rd-editor').hidden = true; }).catch(fail);
  };

  // UPnP.
  $('up-search').onclick = () => {
    setHTML($('up-servers'), '<span class="muted">searching…</span>');
    api('/audio/upnp/servers?search=1').then(renderUpnpServers).catch(e => setHTML($('up-servers'), `<span class="bad-text">${esc(e.message)}</span>`));
  };
  $('up-servers').onclick = e => {
    const b = e.target.closest('button[data-server]');
    if (b) browseUpnp(b.dataset.server, '0', b.dataset.name);
  };
  $('up-path').onclick = e => {
    const a = e.target.closest('a[data-crumb]');
    if (!a) return;
    e.preventDefault();
    const p = au.upnp.path[parseInt(a.dataset.crumb, 10)];
    browseUpnp(au.upnp.server, p.id, p.title);
  };
  $('up-items').onclick = e => {
    const o = e.target.closest('button[data-open]');
    if (o) return browseUpnp(au.upnp.server, o.dataset.open, o.dataset.title);
    const p = e.target.closest('button[data-item], button[data-item-hear]');
    if (!p) return;
    const hear = p.dataset.itemHear != null;
    const it = au.upnp.items[parseInt(hear ? p.dataset.itemHear : p.dataset.item, 10)];
    if (!hear && !chosenSink()) { toast('Choose a sink in "Play to" above (or use listen).'); return; }
    playUrl(it.url, [it.artist, it.title].filter(Boolean).join(' — '), hear || $('st-listen').checked);
  };
}

registerTab('audio', {
  topics: () => ['media', 'audio.streams'],
  show() {
    refreshAudio();
    refreshMedia();
    refreshEndpoints(false);
    refreshStreams();
    api('/audio/radio').then(renderRadio).catch(() => {});
    api('/audio/upnp/servers').then(renderUpnpServers).catch(() => {});
    clearInterval(au.tick);
    au.tick = setInterval(tickPlayers, 500);
    if (Object.keys(au.listeners).length) drawListening();
  },
  hide() {
    // Listening goes on in the background (it is what someone asked for); only the visuals stop.
    clearInterval(au.tick);
    au.tick = null;
  },
  onMessage(topic, data) {
    if (currentTab !== 'audio') return;
    if (topic === 'media') {
      // Not under a volume drag: the rebuild would take the slider from under the pointer.
      const ae = document.activeElement;
      if (!(ae && ae.type === 'range' && !$('au-players').contains(ae))) renderMedia(data);
    } else if (topic === 'audio.streams') {
      renderStreams(data);
    }
  },
});
setupAudio();
