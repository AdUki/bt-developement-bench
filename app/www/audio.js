'use strict';
// Audio: which stack owns Bluetooth audio (btbench-audio); the AVRCP players of connected phones
// (controls, metadata, browsing); streams — a test signal, a radio or UPnP URL, or one device's
// audio — played into a sink and/or listened to here; and BlueZ's transports and endpoints.
//
// Listening is plain fetch() of the stream's endless WAV, scheduled into Web Audio: no media
// element (which buffers seconds and cannot say how much), and the AudioContext is made in the
// click that asked for it, so autoplay rules let it play.

const AUDIO_MODES = ['pipewire', 'none'];
const au = {browse: {}, endpoints: null, media: null, streams: [], cards: {}, listeners: {}, playerClock: {},
  tick: null, radio: [], upnp: {server: null, path: []}};

function renderAudio(s) {
  setText($('au-mode'), s.mode);
  setClass($('au-mode'), 'pill good');
  setHTML($('au-buttons'), (s.modes || AUDIO_MODES).map(m =>
    `<button data-mode="${esc(m)}"${m === s.mode ? ' disabled' : ''}>${esc(m)}</button>`).join(''));
  setHTML($('au-services'), Object.entries(s.services || {}).map(([k, v]) =>
    pill(k.replace(/\.service$/, ''), v === 'active' ? 'good' : v === 'failed' ? 'bad' : '')).join(' '));
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
  // Without PipeWire the only endpoints are the loopback card's, played to with aplay.
  setText($('st-backend'), `${ep.backend === 'pipewire' ? 'PipeWire' : ep.backend === 'alsa' ? 'ALSA (loopback card only)' : 'no audio stack'}` +
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

function startStream(source, sink, listen, extra, ctxGiven) {
  const body = Object.assign({source, sink, rate: parseInt($('st-rate').value, 10),
    channels: parseInt($('st-ch').value, 10), gain_db: parseFloat($('st-gain').value) || 0}, extra || {});
  // The AudioContext must come from the click itself, before any await (a caller that waits
  // first makes it in its click and passes it).
  const ctx = listen ? (ctxGiven || newAudioContext()) : null;
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
      <span class="acts"><button class="small-btn st-listen">Listen</button><button class="small-btn danger st-stop">Stop</button></span></div>
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
  setText(q('.st-listen'), au.listeners[s.id] ? 'Mute here' : 'Listen');
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
    setText(el.querySelector('.st-listen'), 'Listen');
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

// ---------------------------------------------------------------- codec picker

const CD_KINDS = [['a2dp', 'Send (A2DP)'], ['a2dp-duplex', 'Both ways (A2DP duplex)'], ['gateway', 'Receive A2DP / HFP gateway'],
  ['headset', 'Headset (HFP/HSP)'], ['bap', 'LE Audio (BAP)'], ['other', 'Other'], ['off', 'Off']];

function renderCodecs(r) {
  au.devices = r;
  if (!r.devices.length) {
    setHTML($('cd-list'), `<div class="small muted">${esc(r.error || 'No Bluetooth audio device connected.')}</div>`);
    return;
  }
  // Not while a picker is open: the rebuild would close it.
  const ae = document.activeElement;
  if (ae && $('cd-list').contains(ae) && ae.tagName === 'SELECT') return;
  setHTML($('cd-list'), r.devices.map(d => {
    const a = d.active;
    const groups = CD_KINDS.map(([k, label]) => {
      const ps = d.profiles.filter(p => p.kind === k);
      if (!ps.length) return '';
      return `<optgroup label="${esc(label)}">${ps.map(p => `<option value="${p.index}"${a && a.index === p.index ? ' selected' : ''}${p.available ? '' : ' disabled'}>
        ${p.codec ? esc(p.codec) + ' (' + esc(p.name) + ')' : esc(p.name)}</option>`).join('')}</optgroup>`;
    }).join('');
    return `<div class="row wrap codec-row"><b>${esc(d.label)}</b> <span class="mono small muted">${esc(d.address)}</span>
      ${a ? pill(a.codec || a.name, a.kind === 'off' ? '' : 'good') : ''}
      <select class="cd-pick grow" data-id="${d.id}">${groups}</select></div>`;
  }).join(''));
}

const refreshCodecs = () => api('/audio/devices').then(renderCodecs).catch(e => setHTML($('cd-list'), `<span class="bad-text">${esc(e.message)}</span>`));

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

// Transport icons, drawn rather than emoji: emoji render in colour and at different sizes on
// every phone.
const PL_ICON = {
  previous: 'M18 6l-8.5 6L18 18zM6 6h2v12H6z', next: 'M6 6l8.5 6L6 18zM16 6h2v12h-2z',
  rewind: 'M20 6l-8 6 8 6zM12 6l-8 6 8 6z', 'fast-forward': 'M4 6l8 6-8 6zM12 6l8 6-8 6z',
  play: 'M8 5v14l11-7z', pause: 'M7 5h4v14H7zM13 5h4v14h-4z', stop: 'M6 6h12v12H6z',
};
const NOTE_SVG = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M9 18V6l10-2v12M9 18a2 2 0 11-4 0 2 2 0 014 0zM19 16a2 2 0 11-4 0 2 2 0 014 0z"/></svg>';
const devName = addr => {
  const d = typeof btState !== 'undefined' && btState && btState.devices.find(x => x.address === addr);
  return d ? d.alias : addr;
};

// A player as a now-playing screen: cover, track, the transport row and the volume. Repeat,
// shuffle and the browser are a click away (kept open across the rebuilds every update causes).
function playerCard(p, transports) {
  const t = p.track || {};
  const tr = p.transport && transports.find(x => x.path === p.transport);
  const d = t.duration_ms || 0;
  const num = t.track_number ? `track ${t.track_number}${t.number_of_tracks ? ' of ' + t.number_of_tracks : ''}` : '';
  const btn = (a, title, cls) => `<button class="pl-btn ${cls || ''}" data-a="${a}" data-path="${esc(p.path)}" title="${title}" aria-label="${title}">
    <svg viewBox="0 0 24 24"><path d="${PL_ICON[a]}"/></svg></button>`;
  const playing = p.status === 'playing';
  const art = p.obex_port != null && t.img_handle
    ? `<img alt="cover art" src="/api/media/players/cover?path=${encodeURIComponent(p.path)}&h=${esc(t.img_handle)}" onerror="coverRetry(this)">`
    : (p.obex_port != null ? coverConnect(p) : '') + NOTE_SVG;
  const extras = p.repeat != null || p.shuffle != null || p.browsable;
  au.plOpen = au.plOpen || {};
  return `<div class="card player" data-path="${esc(p.path)}">
    <div class="pl-art">${art}</div>
    <div class="head"><span class="pl-who">${esc(p.name || 'Player')} on ${esc(devName(p.address))}${p.type ? ' · ' + esc(p.type) : ''}</span>
      ${pill(p.status || 'unknown', playing ? 'good' : p.status === 'paused' ? 'warn' : '')}</div>
    <div class="pl-title">${esc(t.title || 'Nothing playing')}</div>
    <div class="small">${esc([t.artist, t.album].filter(Boolean).join(' — '))}</div>
    <div class="small muted">${esc([t.genre, num].filter(Boolean).join(' · '))}</div>
    <div class="pl-progress"><i style="width:${d ? (playerPosition(p) / d * 100).toFixed(2) : 0}%"></i></div>
    <div class="pl-time mono">${fmtMs(playerPosition(p))}${d ? ' / ' + fmtMs(d) : ''}</div>
    <div class="pl-controls">
      ${btn('previous', 'Previous')}${btn('rewind', 'Rewind (hold)')}
      ${btn('play', 'Play')}${btn('pause', 'Pause')}
      ${btn('fast-forward', 'Fast forward (hold)')}${btn('next', 'Next')}${btn('stop', 'Stop')}
      <span class="spacer"></span>
      <button class="small-btn pl-listen" data-address="${esc(p.address)}" title="Hear what this device sends the board">Listen here</button>
    </div>
    ${tr && tr.volume != null ? `<label class="row small pl-vol-row"><span class="muted">Volume</span> <input type="range" min="0" max="127" value="${tr.volume}" data-path="${esc(tr.path)}" class="grow pl-vol">
      <span class="mono">${tr.volume}</span></label>` : ''}
    ${extras ? `<details class="more-fields pl-more" data-path="${esc(p.path)}"${au.plOpen[p.path] ? ' open' : ''}>
      <summary>${p.browsable ? 'Browse, repeat and shuffle' : 'Repeat and shuffle'}</summary>
      <div class="row small" style="margin-top:.5rem">
        ${p.repeat != null ? `<label class="row">Repeat ${selectHTML('pl-repeat', p.path, p.repeat, ['off', 'singletrack', 'alltracks', 'group'])}</label>` : ''}
        ${p.shuffle != null ? `<label class="row">Shuffle ${selectHTML('pl-shuffle', p.path, p.shuffle, ['off', 'alltracks', 'group'])}</label>` : ''}
      </div>
      ${p.browsable ? browserHTML(p) : ''}
    </details>` : ''}
  </div>`;
}

// AVRCP browsing: the player's folders (now playing, its file system, search results), kept per
// player across the card rebuilds every media update causes.
function browserHTML(p) {
  const b = au.browse[p.path];
  const items = b ? b.items : (p.items || []).map(i => Object.assign({title: i.metadata && i.metadata.title, artist: i.metadata && i.metadata.artist}, i));
  const nav = `<div class="row wrap small pl-browser-nav">
      <button class="small-btn pl-nav" data-path="${esc(p.path)}" data-folder="${esc(p.path)}/NowPlaying">Now playing</button>
      <button class="small-btn pl-nav" data-path="${esc(p.path)}" data-folder="${esc(p.path)}/Filesystem">Files</button>
      ${b && b.stack && b.stack.length > 1 ? `<button class="small-btn pl-back" data-path="${esc(p.path)}">Back</button>` : ''}
      ${p.searchable ? `<input class="pl-q narrow" data-path="${esc(p.path)}" placeholder="Search"><button class="small-btn pl-search" data-path="${esc(p.path)}">Search</button>` : ''}
      <span class="muted mono">${esc(b ? b.folder : (p.folder ? p.folder.name : ''))}</span></div>`;
  return `<div class="pl-items small">${nav}${items.length ? items.map(i => `<div class="row between">
      <span>${i.type === 'folder' ? '📁 ' : ''}${esc(i.title || i.name)}${i.artist ? ' <span class="muted">— ' + esc(i.artist) + '</span>' : ''}</span>
      <span>${i.type === 'folder' ? `<button class="small-btn pl-open" data-path="${esc(p.path)}" data-folder="${esc(i.path)}">open</button>` : ''}
        ${i.playable ? `<button class="small-btn pl-item" data-a="play" data-path="${esc(i.path)}">play</button>
          <button class="small-btn pl-item" data-a="add" data-path="${esc(i.path)}">add</button>` : ''}</span></div>`).join('')
    : '<div class="muted">nothing listed</div>'}</div>`;
}

function browse(player, folder, push) {
  const b = au.browse[player] || (au.browse[player] = {stack: [], items: [], folder: ''});
  return post('/media/players/browse', folder ? {path: player, folder} : {path: player}).then(r => {
    if (push !== false) b.stack.push(folder);
    b.items = r.items;
    b.folder = r.folder || folder || '';
    if (au.media) renderPlayers(au.media);
  }).catch(fail);
}

// The first image of a player can be asked for while its BIP channel is still opening: try once
// more a few seconds later before giving up on it.
function coverRetry(img) {
  if (img.dataset.retried) { img.hidden = true; return; }
  img.dataset.retried = '1';
  setTimeout(() => { img.src = img.src.replace(/&r=\d+$/, '') + '&r=' + Date.now(); }, 3000);
}

// A player with cover art but no image handle yet: the phone sends handles only while a BIP
// channel is open, so open one (once per player); the next track info then carries a handle.
function coverConnect(p) {
  au.coverAsked = au.coverAsked || {};
  if (!au.coverAsked[p.path]) {
    au.coverAsked[p.path] = true;
    fetch('/api/media/players/cover?path=' + encodeURIComponent(p.path)).catch(() => {});
  }
  return '';
}

function renderPlayers(m) {
  notePlayers(m.players);
  // Not while a select is open or a slider held: the rebuild would close it under the pointer.
  const ae = document.activeElement;
  if (ae && $('au-players').contains(ae) && (ae.tagName === 'SELECT' || ae.type === 'range' || ae.classList.contains('pl-q'))) return;
  setHTML($('au-players'), m.players.length ? m.players.map(p => playerCard(p, m.transports)).join('')
    : '<div class="card empty-card"><h3>No player</h3><p class="hint">Connect a phone, or anything that plays, with the board as its speaker: its AVRCP player shows here with the track, cover art and controls.</p></div>');
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

// ---------------------------------------------------------------- the board's own player

// What a car or speaker the board plays to sees over AVRCP: the board's player (bench_player.h),
// its track and its library, which such a device can browse. The library is browsed here from the
// tree the state carries; playing from here is the same as playing from the remote.
function localPlayerCard(s) {
  const lib = s.library || [];
  const byId = id => lib.find(i => i.id === id);
  const t = s.track || {};
  const playing = s.status === 'playing';
  const btn = (a, title, cls) => `<button class="pl-btn lp-a ${cls || ''}" data-a="${a}" title="${title}" aria-label="${title}">
    <svg viewBox="0 0 24 24"><path d="${PL_ICON[a]}"/></svg></button>`;
  const folder = au.lpFolder == null ? null : au.lpFolder;
  const entries = lib.filter(i => (i.parent == null ? null : i.parent) === folder);
  const cur = s.track ? s.track.id : null;
  const path = [];
  for (let f = folder == null ? null : byId(folder); f; f = f.parent == null ? null : byId(f.parent)) path.unshift(f.name);
  const reg = (s.registered || []).map(a => a.split('/').pop()).join(', ');
  const num = t.Number ? `track ${t.Number}${t.NumberOfTracks ? ' of ' + t.NumberOfTracks : ''}` : '';
  return `<div class="card player lp">
    <div class="pl-art">${NOTE_SVG}</div>
    <div class="head"><span class="pl-who">This board's player · what a car or speaker sees</span>
      ${pill(s.enabled ? s.status : 'off', playing ? 'good' : s.status === 'paused' ? 'warn' : '')}</div>
    <div class="pl-title">${esc(t.Title || 'Nothing playing')}</div>
    <div class="small">${esc([t.Album, num].filter(Boolean).join(' · '))}</div>
    <div class="small muted">${s.enabled ? (reg ? 'Offered on ' + esc(reg) : 'Waiting for BlueZ') : 'Not offered to devices'}${s.sink ? ' · plays to ' + esc(s.sink) : ''}</div>
    ${s.error ? `<div class="small bad">${esc(s.error)}</div>` : ''}
    <div class="pl-controls">
      ${btn('previous', 'Previous')}${btn('play', 'Play')}${btn('pause', 'Pause')}${btn('next', 'Next')}${btn('stop', 'Stop')}
      <span class="spacer"></span>
      <label class="row small"><input type="checkbox" id="lp-enabled"${s.enabled ? ' checked' : ''}> Offer it</label>
    </div>
    <details class="more-fields pl-more" id="lp-more"${au.lpOpen ? ' open' : ''}>
      <summary>Library and now playing</summary>
      <div class="pl-items small">
        <div class="row wrap small pl-browser-nav">
          ${folder != null ? '<button class="small-btn lp-up">Up</button>' : ''}
          <span class="muted mono">/${esc(path.join('/'))}</span></div>
        ${entries.map(i => `<div class="row between">
          <span>${i.type === 'folder' ? '📁 ' : ''}${i.id === cur ? '<b>' + esc(i.name) + '</b>' : esc(i.name)}</span>
          <span>${i.type === 'folder' ? `<button class="small-btn lp-open" data-id="${i.id}">open</button>` : ''}
            <button class="small-btn lp-play" data-id="${i.id}">play</button>
            <button class="small-btn lp-add" data-id="${i.id}">add</button></span></div>`).join('')}
        <div class="muted" style="margin-top:.5rem">Now playing</div>
        ${(s.queue || []).length ? s.queue.map((q, n) => `<div class="row between"><span>${s.position && n === s.position.index ? '<b>' + esc(q.name) + '</b>' : esc(q.name)}</span></div>`).join('')
          : '<div class="muted">empty: play something, here or from the remote</div>'}
      </div>
    </details>
  </div>`;
}

function refreshLocalPlayer() {
  return api('/media/local-player').then(s => {
    au.local = s;
    const ae = document.activeElement;
    if (ae && $('au-local').contains(ae) && ae.type === 'checkbox') return;
    setHTML($('au-local'), localPlayerCard(s));
  }).catch(() => setHTML($('au-local'), ''));
}

function localPlayerPost(body) {
  return post('/media/local-player', body).then(s => {
    au.local = s;
    setHTML($('au-local'), localPlayerCard(s));
  }).catch(fail);
}

function playerAction(path, action) {
  return post('/media/players/control', {path, action}).catch(fail);
}

// Hear what a phone plays into the board: its capture endpoint, by address. A phone that is
// connected but not streaming (its transport idle — it fell back to its own speaker after a
// reconnect, say) has no capture node: it is asked over AVRCP to play again, which makes it open
// the stream, and the node is waited for (a few seconds) before giving up.
// `onStream`: the caller plays the stream itself (the Listen view's media element) instead of
// Web Audio here.
function listenToDevice(address, onStream) {
  const find = ep => ep && ep.sources.find(e => e.address && e.address.toUpperCase() === address.toUpperCase());
  // The AudioContext comes from the click itself, before any wait.
  const ctx = onStream ? {close() {}} : newAudioContext();
  if (!ctx) return;
  const go = src => startStream({type: 'capture', backend: src.backend, target: src.id, title: src.label}, null, !onStream,
    {rate: src.rate || 48000}, onStream ? null : ctx).then(s => { if (onStream) onStream(s); }).catch(() => {});
  const src = find(au.endpoints);
  if (src) return go(src);
  const m = au.media || (typeof pl !== 'undefined' ? pl.media : null);
  const player = m && m.players.find(p => p.address.toUpperCase() === address.toUpperCase());
  const transport = m && m.transports.find(t => t.address.toUpperCase() === address.toUpperCase());
  if (!transport) {
    ctx.close();
    toast('The phone sends no audio to the board: connect it with the board as its speaker (A2DP).');
    return;
  }
  toast('Asking the phone to stream to the board…');
  const nudge = player ? (player.status === 'playing'
    ? post('/media/players/control', {path: player.path, action: 'pause'}).then(() => new Promise(r => setTimeout(r, 800)))
    : Promise.resolve()).then(() => post('/media/players/control', {path: player.path, action: 'play'})) : Promise.resolve();
  const deadline = Date.now() + 7000;
  const poll = () => api('/audio/endpoints?fresh=1').then(ep => {
    renderEndpoints(ep);
    const s = find(ep);
    if (s) return go(s);
    if (Date.now() < deadline) return new Promise(r => setTimeout(r, 800)).then(poll);
    ctx.close();
    toast('The phone did not start streaming to the board. On the phone, choose btbench as the audio output.');
  });
  nudge.catch(() => {}).then(poll).catch(e => { ctx.close(); fail(e); });
}

// ---------------------------------------------------------------- media (BlueZ)

function codecCell(c) {
  return `<b>${esc(c.name)}</b> <span class="small">${esc(c.summary)}</span>`;
}

function renderMedia(m) {
  au.media = m;
  renderPlayers(m);
  // The codec in words up front; the raw configuration bytes and the object path, which only a
  // bug report needs, behind "Details".
  setHTML($('au-transports'), m.transports.length ? m.transports.map(t => `<div class="card">
      <div class="card-head"><h3>${esc(t.profile || t.uuid)} <span class="small muted">with ${esc(devName(t.address))}</span></h3>
        ${pill(t.state, t.state === 'active' ? 'good' : t.state === 'pending' ? 'warn' : '')}</div>
      <div class="small">${codecCell(t.codec)}${t.delay_ms != null ? ` <span class="muted">· delay ${t.delay_ms.toFixed(1)} ms</span>` : ''}</div>
      ${t.volume != null ? `<label class="row small pl-vol-row"><span class="muted">Volume</span> <input type="range" min="0" max="127" value="${t.volume}" data-path="${esc(t.path)}" class="grow">
        <span class="mono">${t.volume}</span></label>` : ''}
      <details class="more-fields"><summary>Details</summary>
        <div class="kv" style="margin-top:.4rem"><span>Configuration</span><span class="mono">${esc(t.configuration)}</span>
          <span>Device</span><span class="mono">${esc(t.address)}</span>
          <span>Path</span><span class="mono">${esc(t.path)}</span></div></details>
    </div>`).join('') : '<div class="card"><p class="hint" style="margin:0">No transport: nothing is set up to stream. A2DP devices get one when they connect.</p></div>');
  setHTML($('au-endpoints'), m.endpoints.length ? `<div class="card flush"><div class="scroll-x"><table class="tbl">
      <tr><th>Device</th><th>Role</th><th>Codec</th><th>Capabilities</th><th>Delay reports</th></tr>
      ${m.endpoints.map(e => `<tr><td>${esc(devName(e.address))}</td><td>${esc(e.role || e.uuid)}</td>
        <td><b>${esc(e.codec.name)}</b></td><td class="wrap small" title="${esc(e.capabilities)}">${esc(e.codec.summary)}</td>
        <td>${e.delay_reporting ? 'yes' : 'no'}</td></tr>`).join('')}</table></div></div>`
    : '<div class="card"><p class="hint" style="margin:0">No remote endpoints: connect an A2DP device.</p></div>');
}

// AVDTP browsing: ask a device for its stream endpoints over a signalling channel of our own.
function probeAvdtp() {
  const addr = $('av-dev').value;
  if (!addr) return;
  setHTML($('av-result'), '<span class="muted">Probing: paging the device…</span>');
  post(`/media/avdtp/${addr}`).then(r => setHTML($('av-result'), `<div class="small muted" style="margin-bottom:.4rem">${r.seps.length} stream endpoints in ${r.took_ms} ms</div>
    <div class="scroll-x"><table class="tbl"><tr><th>SEID</th><th>role</th><th>media</th><th>in use</th><th>codec</th><th>capabilities</th></tr>
    ${r.seps.map(sp => `<tr><td>${sp.seid}</td><td>${esc(sp.role)}</td><td>${esc(sp.media_type)}</td><td>${sp.in_use ? pill('in use', 'warn') : ''}</td>
      <td>${sp.codec ? `<b>${esc(sp.codec.name)}</b> <span class="small">${esc(sp.codec.summary || '')}</span>` : esc(sp.error || '—')}</td>
      <td class="wrap small" title="${esc(((sp.capabilities || []).find(c => c.category === 7) || {}).hex || '')}">${(sp.capabilities || []).filter(c => c.category !== 7).map(c => esc(c.name) + (c.type ? ' (' + esc(c.type) + ')' : '')).join(', ')}</td></tr>`).join('')}</table></div>`))
    .catch(e => setHTML($('av-result'), `<span class="bad-text">${esc(e.message)}</span>`));
}

function avdtpDevices() {
  api('/bluetooth').then(b => { if (!btState) btState = b; return b; }).then(b => fillSelect($('av-dev'), b.devices.filter(d => d.paired || d.connected || d.rssi != null)
    .map(d => ({value: d.address, label: `${d.alias} (${d.address})${d.connected ? ' — connected' : ''}`})))).catch(() => {});
}

function refreshMedia() { return api('/media').then(renderMedia).catch(fail); }

// ---------------------------------------------------------------- radio

function renderRadio(r) {
  au.radio = r.stations;
  setHTML($('rd-list'), r.stations.length ? r.stations.map((s, i) => `<div class="row between radio-row">
      <span title="${esc(s.url)}">${esc(s.name)}</span>
      <span class="acts"><button class="small-btn" data-play="${i}" title="Play to the output chosen above">Play</button><button class="small-btn" data-hear="${i}" title="Listen in this browser">Listen</button></span></div>`).join('')
    : '<span class="muted">No stations: Edit to add some.</span>');
}

function playUrl(url, title, listen) {
  return startStream({type: 'url', url, title}, chosenSink(), listen).catch(() => {});
}

// ---------------------------------------------------------------- UPnP

function renderUpnpServers(r) {
  setHTML($('up-servers'), r.servers.length ? r.servers.map(s => `<div class="row between">
      <span><b>${esc(s.name)}</b> <span class="muted">${esc([s.manufacturer, s.model].filter(Boolean).join(' '))}</span></span>
      <button class="small-btn" data-server="${esc(s.id)}" data-name="${esc(s.name)}">Browse</button></div>`).join('')
    : `<span class="muted">${r.searched_ms ? 'No media servers answered' + (r.error ? ': ' + esc(r.error) : '.') : 'Search the LAN to find DLNA / UPnP servers.'}</span>`);
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
        <button class="small-btn" data-open="${esc(c.id)}" data-title="${esc(c.title)}">Open</button></div>`).join('') +
      r.items.map((it, i) => `<div class="row between"><span>${esc(it.title)}${it.artist ? ' <span class="muted">— ' + esc(it.artist) + '</span>' : ''}
          <span class="muted mono">${esc(it.mime || '')}${it.duration_s ? ' ' + fmtMs(it.duration_s * 1000) : ''}</span></span>
        <span>${it.url ? `<button class="small-btn" data-item="${i}">Play</button><button class="small-btn" data-item-hear="${i}">Listen</button>` : ''}</span></div>`).join('')) ||
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

  // The board's player.
  const lp = $('au-local');
  lp.addEventListener('toggle', e => { if (e.target.id === 'lp-more') au.lpOpen = e.target.open; }, true);
  lp.addEventListener('change', e => {
    if (e.target.id === 'lp-enabled')
      put('/media/local-player', {enabled: e.target.checked}).then(s => { au.local = s; setHTML(lp, localPlayerCard(s)); }).catch(fail);
  });
  lp.addEventListener('click', e => {
    const b = e.target.closest('button');
    if (!b) return;
    const id = b.dataset.id != null ? Number(b.dataset.id) : null;
    if (b.classList.contains('lp-a')) localPlayerPost({action: b.dataset.a});
    else if (b.classList.contains('lp-play')) localPlayerPost({play: id});
    else if (b.classList.contains('lp-add')) localPlayerPost({enqueue: id});
    else if (b.classList.contains('lp-open')) { au.lpFolder = id; setHTML(lp, localPlayerCard(au.local)); }
    else if (b.classList.contains('lp-up')) {
      const f = au.local.library.find(i => i.id === au.lpFolder);
      au.lpFolder = f && f.parent != null ? f.parent : null;
      setHTML(lp, localPlayerCard(au.local));
    }
  });

  // Players: buttons, hold-to-seek, settings, browsing.
  const pl = $('au-players');
  // "toggle" does not bubble: caught on the way down, it says which players have their extras open.
  pl.addEventListener('toggle', e => {
    if (e.target.classList.contains('pl-more')) au.plOpen[e.target.dataset.path] = e.target.open;
  }, true);
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
    const nav = e.target.closest('.pl-nav, .pl-open');
    if (nav) {
      if (nav.classList.contains('pl-nav')) au.browse[nav.dataset.path] = {stack: [], items: [], folder: ''};
      browse(nav.dataset.path, nav.dataset.folder);
    }
    const back = e.target.closest('.pl-back');
    if (back) {
      const b = au.browse[back.dataset.path];
      b.stack.pop();
      browse(back.dataset.path, b.stack[b.stack.length - 1], false);
    }
    const sr = e.target.closest('.pl-search');
    if (sr) {
      const q = $('au-players').querySelector(`.pl-q[data-path="${CSS.escape(sr.dataset.path)}"]`);
      post('/media/players/search', {path: sr.dataset.path, value: q ? q.value : ''}).then(r => {
        const b = au.browse[sr.dataset.path] || (au.browse[sr.dataset.path] = {stack: []});
        b.stack.push(r.result);
        b.items = r.items;
        b.folder = r.folder;
        if (au.media) renderPlayers(au.media);
      }).catch(fail);
    }
    const it = e.target.closest('.pl-item');
    if (it) post('/media/items', {path: it.dataset.path, action: it.dataset.a}).then(() => toast(it.dataset.a === 'add' ? 'added to now playing' : 'playing')).catch(fail);
  };
  // Seeking runs while the button is held, as on a remote: pressed starts it, released resumes play.
  pl.onpointerdown = e => {
    const b = e.target.closest('.pl-btn');
    if (!b || (b.dataset.a !== 'fast-forward' && b.dataset.a !== 'rewind')) return;
    playerAction(b.dataset.path, b.dataset.a);
    const up = () => { window.removeEventListener('pointerup', up); playerAction(b.dataset.path, 'play'); };
    window.addEventListener('pointerup', up);
  };

  $('av-probe').onclick = probeAvdtp;

  // The codec picker.
  $('cd-refresh').onclick = refreshCodecs;
  $('cd-list').onchange = e => {
    const sel = e.target.closest('select.cd-pick');
    if (!sel) return;
    sel.disabled = true;
    put(`/audio/devices/${sel.dataset.id}/profile`, {profile: parseInt(sel.value, 10)})
      .then(d => { if (d.switch_error) toast(d.switch_error); refreshCodecs(); refreshEndpoints(true); })
      .catch(err => { fail(err); refreshCodecs(); })
      .finally(() => { sel.disabled = false; });
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
    if (!sink && !listen) { setText($('st-msg'), 'Choose an output in "Play to", or turn on "Also listen in this browser".'); return; }
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
    if (b.dataset.play != null && !chosenSink()) { toast('Choose an output in "Play to" above first, or use Listen.'); return; }
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
    if (!hear && !chosenSink()) { toast('Choose an output in "Play to" above first, or use Listen.'); return; }
    playUrl(it.url, [it.artist, it.title].filter(Boolean).join(' — '), hear || $('st-listen').checked);
  };
}

// ---------------------------------------------------------------- PipeWire's graph (pw-top)

// The drivers, running ones first, each with its followers under it.
function renderPipewire(t) {
  if (!t.drivers) {
    setText($('pw-note'), t.error ? 'pw-top: ' + t.error : 'waiting for pw-top…');
    return;
  }
  setText($('pw-note'), `${fmtTime(t.ts)}${t.xruns ? ` · ${t.xruns} xrun${t.xruns === 1 ? '' : 's'} this second` : ''}`);
  const on = st => /[RITt]/.test(st);
  const stName = {R: 'running', I: 'idle', S: 'suspended', C: 'creating', E: 'error', T: 'running', t: 'starting'};
  const load = q => q == null ? '' : `<span class="pw-load${q > 0.8 ? ' bad' : q > 0.5 ? ' warn' : ''}"><i style="width:${Math.min(100, Math.round(q * 100))}%"></i></span> ${Math.round(q * 100)}%`;
  const ms = v => v == null ? '' : v < 1 ? `${(v * 1000).toFixed(0)} µs` : `${v.toFixed(2)} ms`;
  const errCell = n => `<td class="num${n.xruns ? ' bad-text' : n.err ? ' warn-text' : ' muted'}" title="${n.err} since pw-top started">${n.xruns ? '+' + n.xruns : n.err || ''}</td>`;
  const drivers = [...t.drivers].sort((a, b) => on(b.state) - on(a.state));
  const rows = drivers.map(d => {
    const head = `<tr class="${on(d.state) ? '' : 'muted'}"><td>${pill(stName[d.state] || d.state, d.state === 'E' ? 'bad' : on(d.state) ? 'good' : '')}</td>
      <td class="mono"><b>${esc(d.name)}</b></td><td class="mono">${d.quantum != null ? `${d.quantum}/${d.rate}` : ''}</td>
      <td class="num">${d.period_ms != null ? d.period_ms + ' ms' : ''}</td><td>${load(d.busy_q)}</td>
      <td class="num">${ms(d.busy_ms)}</td><td class="num">${ms(d.wait_ms)}</td>${errCell(d)}<td class="mono small">${esc(d.format || '')}</td></tr>`;
    const fol = (d.followers || []).map(f => `<tr class="pw-fol${on(f.state) ? '' : ' muted'}"><td></td>
      <td class="mono">↳ ${esc(f.name)}</td><td></td><td class="num" title="the latency it reports">${f.latency_ms != null ? f.latency_ms + ' ms' : ''}</td>
      <td>${load(f.busy_q)}</td><td class="num">${ms(f.busy_ms)}</td><td class="num">${ms(f.wait_ms)}</td>${errCell(f)}<td class="mono small">${esc(f.format || '')}</td></tr>`).join('');
    return head + fol;
  }).join('');
  setHTML($('pw-graph'), `<table class="tbl compact pw-tbl"><tr><th></th><th>Node</th><th>quantum</th><th class="num">period</th><th>load</th>
    <th class="num">busy</th><th class="num">wait</th><th class="num">xruns</th><th>format</th></tr>${rows}</table>`);
}

registerTab('audio', {
  topics: () => ['media', 'audio.streams'].concat(panes.audio === 'stack' ? ['pipewire'] : []),
  show() {
    refreshAudio();
    refreshMedia();
    refreshEndpoints(false);
    refreshCodecs();
    avdtpDevices();
    refreshStreams();
    api('/audio/radio').then(renderRadio).catch(() => {});
    api('/audio/upnp/servers').then(renderUpnpServers).catch(() => {});
    clearInterval(au.tick);
    au.tick = setInterval(tickPlayers, 500);
    // The board's player has no topic: asked every two seconds while the players are shown.
    refreshLocalPlayer();
    clearInterval(au.lpTick);
    au.lpTick = setInterval(() => { if (panes.audio === 'players' && !document.hidden) refreshLocalPlayer(); }, 2000);
    if (Object.keys(au.listeners).length) drawListening();
  },
  hide() {
    // Listening goes on in the background (it is what someone asked for); only the visuals stop.
    clearInterval(au.tick);
    au.tick = null;
    clearInterval(au.lpTick);
    au.lpTick = null;
  },
  onMessage(topic, data) {
    if (topic === 'pipewire') return renderPipewire(data);
    if (currentTab !== 'audio') return;
    if (topic === 'media') {
      // The codec list changes when a device's transports do, not with every track position.
      const key = data.transports.map(t => t.path + t.state + t.configuration).join();
      if (key !== au.cdKey) {
        au.cdKey = key;
        clearTimeout(au.cdTimer);
        au.cdTimer = setTimeout(refreshCodecs, 1500);
      }
      // Not under a volume drag: the rebuild would take the slider from under the pointer.
      const ae = document.activeElement;
      if (!(ae && ae.type === 'range' && !$('au-players').contains(ae))) renderMedia(data);
    } else if (topic === 'audio.streams') {
      renderStreams(data);
    }
  },
});
setupAudio();
