'use strict';
// Audio, four views. Bluetooth: each connected audio device — which way audio goes on it and in
// what codec (BlueZ's transports), the PipeWire profile that is its codec choice, its stream
// endpoints and the AVDTP probe. Players: AVRCP — the phones' players (controls, metadata,
// browsing) and the board's own player. Streams: what the board plays itself — a test signal, a
// device's audio, a radio or URL — into an output and/or listened to here. PipeWire: the services
// and the graph.
//
// Listening is plain fetch() of the stream's endless WAV, scheduled into Web Audio: no media
// element (which buffers seconds and cannot say how much), and the AudioContext is made in the
// click that asked for it, so autoplay rules let it play.

const au = {adOpen: {}, browse: {}, cover: {}, endpoints: null, media: null, streams: [], cards: {}, listeners: {}, playerClock: {}, sw: {},
  tick: null, radio: []};

function renderAudio(s) {
  loaded($('au-card'));
  setHTML($('au-services'), Object.entries(s.services || {}).map(([k, v]) =>
    pill(k.replace(/\.service$/, ''), v === 'active' ? 'good' : v === 'failed' ? 'bad' : '')).join(' '));
  const on = s.enabled !== false;
  if (idle($('au-on'))) $('au-on').checked = on;
  $('au-off-hint').hidden = on;
  $('au-restart').disabled = !on;
}

function refreshAudio() {
  return api('/audio').then(s => { renderAudio(s); setText($('au-msg'), ''); }).catch(e => {
    loaded($('au-card'));
    loaded($('au-services'));
    $('au-restart').disabled = e.status === 503;
    // On a PC the target scripts are not installed; say so plainly rather than as an error.
    setText($('au-msg'), e.status === 503 ? 'Restarting PipeWire works on the board only (btbench-audio is not installed on this machine).' : e.message);
  });
}

// ---------------------------------------------------------------- endpoints

const epLabel = e => `${e.label}${e.codec ? ' · ' + e.codec : ''}${e.default ? ' (default)' : ''}`;


function renderEndpoints(ep) {
  au.endpoints = ep;
  const t = ep.tools || {};
  const missing = ['pw-cat', 'mpg123', 'ffmpeg'].filter(k => !t[k]);
  // Without PipeWire the only endpoints are the loopback card's, played to with aplay.
  setText($('st-backend'), `${ep.backend === 'pipewire' ? 'PipeWire' : ep.backend === 'alsa' ? 'ALSA (loopback card only)' : 'PipeWire is not running'}` +
    `${ep.error ? ' · ' + ep.error : ''}${missing.length ? ' · not installed: ' + missing.join(', ') : ''}`);
  // With nothing chosen before, the first connected speaker or car is where things play, else
  // the board's sound card (the loopback card).
  if (!au.outs) {
    const first = ep.sinks.find(e => e.kind === 'bluetooth' && !(e.set && !e.set_leader)) || ep.sinks[0];
    au.outs = first ? [`${ep.backend === 'alsa' ? 'alsa' : 'pipewire'}:${first.id}`] : [];
  }
  renderNewOuts();
  const caps = ep.sources.map(e => ({value: `${e.backend}:${e.id}`, label: epLabel(e)}));
  if (ep.backend === 'pipewire') caps.unshift({value: 'pipewire:', label: 'the default source'});
  fillSelect($('st-capture'), caps);
  fillSelect($('st-monitor'), ep.backend === 'pipewire' ? ep.sinks.map(e => ({value: e.id, label: epLabel(e)})) : []);
  au.streams.forEach(upsertStream);  // each running stream's own output picker
}

// The outputs new streams start on, as Listen's values (player.js outSink): from before there
// were several, the one select's value ("" nowhere; "pipewire:", the default output that is no
// longer offered, nowhere too). Null until
// the endpoints are known, when the first Bluetooth sink is taken.
au.outs = (() => {
  try {
    const v = JSON.parse(lsGet('btbench.au.outs') || 'null');
    if (Array.isArray(v)) return v.filter(x => x !== 'default' && x !== 'none');
  } catch (e) { /* the old choice */ }
  const one = lsGet('btbench.au.sink');
  return one == null ? null : one === '' || one === 'pipewire:' ? [] : [one];
})();

// The form's outputs: Listen's buttons, each one on or off by itself.
function renderNewOuts() {
  setHTML($('st-sinks'), outButtonsHTML(outOptions(false, au), au.outs || [], null, au));
  renderOutCodec();
}

function pickNewOut(v) {
  const next = nextOuts(au.outs || [], v);
  if (next.length > MAX_OUTS) { toast(`A stream plays to at most ${MAX_OUTS} outputs.`); return; }
  au.outs = next;
  lsSet('btbench.au.outs', JSON.stringify(next));
  renderNewOuts();
}

// An output tapped in a stream's card: added to it or taken off it at once, the switch followed as
// Listen follows its own (au.sw by stream, player.js swAdvance) until the output takes the audio
// or fails.
function pickStreamOut(id, v) {
  const s = au.streams.find(x => x.id === id);
  if (!s) return;
  const next = nextOuts(streamOuts(s), v);
  if (next.length > MAX_OUTS) { toast(`A stream plays to at most ${MAX_OUTS} outputs.`); return; }
  const add = next.includes(v);
  const sw = {value: v, add, phase: 'asking', at: Date.now(), seq: null};
  au.sw[id] = sw;
  const redraw = () => { const x = au.streams.find(y => y.id === id); if (x) upsertStream(x); };
  redraw();
  put(`/audio/streams/${id}`, {sinks: outSinks(next)}).then(r => {
    if (au.sw[id] !== sw) return;
    sw.seq = r.sink_req;
    sw.phase = add ? 'starting' : 'removing';
    upsertStream(r);
  }).catch(e => {
    if (au.sw[id] !== sw) return;
    sw.phase = 'failed';
    sw.error = e.message;
    redraw();
  });
  // The result comes with the streams topic; asked too, in case it is slow.
  [400, 1200, 2500, 5000, 9000].forEach(t => setTimeout(() => { if (au.sw[id] === sw && sw.phase !== 'done') refreshStreams(); }, t));
}

function refreshEndpoints(fresh) {
  return api('/audio/endpoints' + (fresh ? '?fresh=1' : '')).then(renderEndpoints)
    .catch(e => { ldFail(e, $('st-sinks')); setText($('st-backend'), e.message); });
}

// The codecs of the chosen outputs that are Bluetooth devices: the same picker as in Players, so
// trying a codec does not mean leaving what plays.
function renderOutCodec() {
  const ep = au.endpoints;
  const ds = (au.outs || []).map(v => ep && ep.sinks.find(k => v.endsWith(':' + k.id)))
    .map(e => e && e.address && au.devices && au.devices.devices.find(x => x.address === e.address)).filter(Boolean);
  $('st-codec-l').hidden = !ds.length;
  const ae = document.activeElement;
  if (ae && $('st-codec').contains(ae)) return;
  setHTML($('st-codec'), ds.map(d => `<span class="row">${ds.length > 1 ? esc(devName(d.address)) + ' ' : ''}${codecSelect(d)}</span>`).join(''));
}

// The sinks chosen in the form, as a stream's: [] (listen only), or [{type, target}, ...].
const chosenSinks = () => outSinks(au.outs || []);

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

function startStream(source, sinks, listen, extra, ctxGiven) {
  const body = Object.assign({source, sinks: sinks || [], rate: parseInt($('st-rate').value, 10),
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
    if (source.type === 'url') toast(e.message);  // played from a station or a server, far from st-msg
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
      <span class="acts">${s.source.type === 'url' ? `<button class="small-btn st-prev" hidden>Previous</button><button class="small-btn st-pause">Pause</button><button class="small-btn st-next" hidden>Next</button>` : ''}<button class="small-btn st-listen">Listen</button><button class="small-btn danger st-stop">Stop</button></span></div>
    <div class="small st-meta"></div>
    <div class="st-meters"></div>
    <div class="row wrap small">
      <label class="row">gain <input type="range" class="st-gain grow" min="-60" max="20" step="0.5"> <span class="mono st-gain-v"></span></label>
      ${s.source.type === 'tone' ? `<label class="row">Hz <input type="number" class="st-freq narrow" min="1"></label>` : ''}
      ${s.source.type === 'tone' || s.source.type === 'noise' ? `<label class="row">level <input type="number" class="st-level narrow" min="-96" max="0"> dB</label>` : ''}
    </div>
    <div class="np-outs st-outs" role="group" aria-label="Where this stream plays"></div>
    <div class="np-out-status st-out-status small" role="status" aria-live="polite"></div>
    <div class="st-listening small" hidden>
      <div class="row wrap"><span class="st-buf mono"></span>
        <label class="row">volume <input type="range" class="st-vol" min="0" max="1" step="0.01" value="1"></label></div>
      <canvas class="graph short st-spec"></canvas>
    </div>
    <div class="small bad-text st-err"></div>`;
  return el;
}

function upsertStream(s) {
  const at = au.streams.findIndex(x => x.id === s.id);
  if (at >= 0) au.streams[at] = s;  // a PUT's answer: Pause must know it paused
  let el = au.cards[s.id];
  if (!el) {
    el = au.cards[s.id] = streamCard(s);
    $('st-list').prepend(el);
    $('st-empty').hidden = true;
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
  (s.sinks_status || []).forEach(k => { if (k.state !== 'playing') meta.push(`${outName(sinkOut(k), au)} ${k.state}`); });
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
  // Its own outputs, Listen's buttons: each added or taken off at once (the source and the others
  // carry on). One not there any more (a speaker gone) stays, greyed, until taken off.
  // What the board does about them, in Listen's words (player.js outLine): a switch tapped
  // followed until the output takes the audio (and a Bluetooth link streams), or why not.
  const sw = au.sw[s.id];
  swAdvance(sw, s, au);
  q('.st-outs').hidden = !live;
  if (live) setHTML(q('.st-outs'), outButtonsHTML(outOptions(false, au), streamOuts(s), sw, au));
  const line = outLine(s, sw && sw.phase !== 'done' ? sw : null, au) || {cls: s.listeners ? 'good' : '',
    html: s.listeners ? `No board output: ${s.listeners} browser${s.listeners > 1 ? 's' : ''} listening.`
      : 'No board output and nobody listening: it plays on until stopped.'};
  q('.st-out-status').hidden = !live;
  if (live) outStatusHTML(q('.st-out-status'), line);
  q('.st-listen').hidden = !live;
  if (q('.st-pause')) {
    const n = (s.source.playlist || []).length;
    q('.st-pause').hidden = !live;
    setText(q('.st-pause'), s.paused ? 'Resume' : 'Pause');
    q('.st-prev').hidden = q('.st-next').hidden = !live || n < 2;
    q('.st-prev').disabled = s.track <= 0;
    q('.st-next').disabled = s.track >= n - 1;
    if (n > 1) meta.unshift(`track ${s.track + 1} of ${n}`), setText(q('.st-meta'), meta.join(' · '));
  }
  setText(q('.st-listen'), au.listeners[s.id] ? 'Mute here' : 'Listen');
  setText(q('.st-stop'), live ? 'Stop' : 'Remove');
  if (!live && au.listeners[s.id]) stopListening(s.id);
}

function renderStreams(list) {
  au.streams = list.streams;
  const ids = new Set(list.streams.map(s => String(s.id)));
  for (const id of Object.keys(au.cards)) {
    if (!ids.has(id)) {
      au.cards[id].remove();
      delete au.cards[id];
      delete au.sw[id];
      stopListening(id);
    }
  }
  // Oldest first into prepend(): newest ends on top.
  list.streams.slice().reverse().forEach(upsertStream);
  loaded($('st-list'));
  $('st-empty').hidden = list.streams.length > 0;
}

const refreshStreams = () => api('/audio/streams').then(renderStreams).catch(e => { ldFail(e, $('st-list')); fail(e); });

// ---------------------------------------------------------------- listening (Web Audio)

function newAudioContext() {
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) { toast('This browser has no Web Audio'); return null; }
  return new AC({latencyHint: 'playback'});
}

// One listening stream: reads the WAV, turns each ~100 ms of samples into an AudioBuffer and
// schedules it right after the previous one, keeping about a third of a second queued (played a
// hair faster or slower to stay there). A gap (Wi-Fi) re-primes that cushion; more than two
// seconds queued (a background tab let it pile up) drops blocks to catch up.
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
    // The board's clock and this browser's output differ by up to a few hundred ppm: left alone the
    // cushion drains into a gap every few minutes, or grows. Each block plays a little faster or
    // slower to hold it near 0.3 s (the error averaged over ~1 s of blocks), at most ±0.5%: 9 cents.
    this.err = 0.9 * (this.err || 0) + 0.1 * (this.next - now - 0.3);
    const rate = Math.min(1.005, Math.max(0.995, 1 + 0.02 * this.err));
    const src = this.ctx.createBufferSource();
    src.buffer = buf;
    src.playbackRate.value = rate;
    src.connect(this.gain);
    src.start(this.next);
    this.next += buf.duration / rate;
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
  if ($('st-sinks')) renderOutCodec();
  renderAudioDevices();
}

function codecSelect(d) {
  const a = d.active;
  const groups = CD_KINDS.map(([k, label]) => {
    const ps = d.profiles.filter(p => p.kind === k);
    if (!ps.length) return '';
    return `<optgroup label="${esc(label)}">${ps.map(p => `<option value="${p.index}"${a && a.index === p.index ? ' selected' : ''}${p.available ? '' : ' disabled'}>
      ${p.codec ? esc(p.codec) + ' (' + esc(p.name) + ')' : esc(p.name)}</option>`).join('')}</optgroup>`;
  }).join('');
  return `<select class="cd-pick grow" data-id="${d.id}" aria-label="Codec">${groups}</select>`;
}

const refreshCodecs = () => api('/audio/devices').then(renderCodecs).catch(e => renderCodecs({devices: [], error: e.message}));

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
  for (const path of Object.keys(au.cover)) if (!players.some(p => p.path === path)) delete au.cover[path];
  for (const p of players) {
    const c = au.playerClock[p.path];
    if (!c || c.pos !== p.position_ms || c.status !== p.status) au.playerClock[p.path] = {pos: p.position_ms, at: Date.now(), status: p.status};
  }
}

// Transport icons, drawn rather than emoji: emoji render in colour and at different sizes on
// every phone.
const PL_ICON = {
  previous: 'M18 6l-8.5 6L18 18zM6 6h2v12H6z', next: 'M6 6l8.5 6L6 18zM16 6h2v12h-2z',
  rewind: 'M20 6l-8 6 8 6zM12 6l-8 6 8 6z', 'fast-forward': 'M4 6l8 6-8 6zM12 6l8 6-8 6z',
  play: 'M8 5v14l11-7z', pause: 'M7 5h4v14H7zM13 5h4v14h-4z', stop: 'M6 6h12v12H6z',
};
const NOTE_SVG = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M9 18V6l10-2v12M9 18a2 2 0 11-4 0 2 2 0 014 0zM19 16a2 2 0 11-4 0 2 2 0 014 0z"/></svg>';
// A device's name: from the device list, else from what the media state says about it (a view
// that shows players gets the name with them, without waiting for the device list).
const devName = addr => {
  const d = typeof btState !== 'undefined' && btState && btState.devices.find(x => x.address === addr);
  if (d) return d.alias;
  const m = au.media || (typeof pl !== 'undefined' ? pl.media : null);
  const o = m && m.players.concat(m.transports).find(x => x.address === addr && x.device_name);
  return o ? o.device_name : addr;
};

// A player as a now-playing screen: cover, track, the transport row (with repeat and shuffle when
// the player has them) and the volume. The browser is a click away (kept open across the rebuilds
// every update causes).
function playerCard(p, transports) {
  const t = p.track || {};
  const tr = p.transport && transports.find(x => x.path === p.transport);
  const d = t.duration_ms || 0;
  const num = t.track_number ? `track ${t.track_number}${t.number_of_tracks ? ' of ' + t.number_of_tracks : ''}` : '';
  const btn = (a, title, cls) => `<button class="pl-btn ${cls || ''}" data-a="${a}" data-path="${esc(p.path)}" title="${title}" aria-label="${title}">
    <svg viewBox="0 0 24 24"><path d="${PL_ICON[a]}"/></svg></button>`;
  const playing = p.status === 'playing';
  const art = p.obex_port != null && t.img_handle
    ? `<img alt="cover art" src="/api/media/players/cover?path=${encodeURIComponent(p.path)}&h=${esc(t.img_handle)}&t=${encodeURIComponent([t.title, t.album].join('|'))}" onerror="coverRetry(this)">`
    : (p.obex_port != null ? coverConnect(p) : '') + NOTE_SVG;
  const extras = p.browsable;
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
      ${modeButtons('pl-btn', btRepeat(p.repeat), p.shuffle == null ? null : p.shuffle !== 'off', `data-path="${esc(p.path)}"`)}
      <span class="spacer"></span>
      <button class="small-btn pl-listen" data-address="${esc(p.address)}" title="Hear what this device sends the board">Listen here</button>
    </div>
    ${tr && tr.volume != null ? `<label class="row small pl-vol-row"><span class="muted">Volume</span> <input type="range" min="0" max="127" value="${tr.volume}" data-path="${esc(tr.path)}" class="grow pl-vol">
      <span class="mono">${tr.volume}</span></label>` : ''}
    ${extras ? `<details class="more-fields pl-more" data-path="${esc(p.path)}"${au.plOpen[p.path] ? ' open' : ''}>
      <summary>Browse</summary>
      ${browserHTML(p)}
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
// The cover-art (BIP) channel to a player's device: opened once per connection (a player that
// goes away is forgotten, see notePlayers), again 10 s after a failure. au.cover[path] is
// {state: asking | open | failed, at}: `at` is when it opened, 0 when it was open already (a
// player BlueZ made again); `changed` is called when the answer comes.
function coverConnect(p, changed) {
  const c = au.cover[p.path];
  if (!c || (c.state === 'failed' && Date.now() - c.at > 10000)) {
    const e = au.cover[p.path] = {state: 'asking', at: Date.now()};
    const done = (state, at) => { e.state = state; e.at = at; if (changed) changed(); };
    fetch('/api/media/players/cover?path=' + encodeURIComponent(p.path))
      .then(r => r.ok ? r.json().catch(() => ({})) : Promise.reject())
      .then(j => done('open', j.channel === 'open' ? 0 : Date.now()), () => done('failed', Date.now()));
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
    <div class="small">${esc([t.Artist, t.Album, num].filter(Boolean).join(' · '))}</div>
    <div class="small muted">${s.enabled ? (reg ? 'Offered on ' + esc(reg) : 'Waiting for BlueZ') : 'Not offered to devices'}${s.sink ? ' · plays to ' + esc(s.sink) : ''}</div>
    ${s.error ? `<div class="small bad">${esc(s.error)}</div>` : ''}
    <div class="pl-controls">
      ${btn('previous', 'Previous')}${btn('play', 'Play')}${btn('pause', 'Pause')}${btn('next', 'Next')}${btn('stop', 'Stop')}
      ${s.repeat != null ? modeButtons('pl-btn lp-mode', boardRepeat(s.repeat), !!s.shuffle) : ''}
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

// Hear what a phone plays into the board: PipeWire's capture node of its link, found by address in
// a fresh read of the endpoints (a cached one may be from before the phone connected). A phone that
// is connected but not streaming may have no node yet: it is asked once over AVRCP to play — never
// paused, which would interrupt what someone is listening to — and the node is waited for.
// `onStream`: the caller plays the stream itself (the Listen view's media element) instead of Web
// Audio here. Resolves with the stream, or null when there was nothing to hear (said in a toast).
// A capture of what the device at `address` sends the board: heard in this browser (Web Audio) or,
// with `onStream`, handed to it instead; `sinks` plays it on outputs as well (a tunnel).
function listenToDevice(address, onStream, sinks) {
  const same = a => !!a && a.toUpperCase() === address.toUpperCase();
  // The AudioContext comes from the click itself, before any wait.
  const ctx = onStream ? null : newAudioContext();
  if (!onStream && !ctx) return Promise.resolve(null);
  const m = au.media || (typeof pl !== 'undefined' ? pl.media : null);
  const player = m && m.players.find(p => same(p.address) && /\/avrcp\//.test(p.path));
  if (m && !m.transports.some(t => same(t.address))) {
    if (ctx) ctx.close();
    toast('The phone sends no audio to the board: connect it with the board as its speaker (A2DP).');
    return Promise.resolve(null);
  }
  const deadline = Date.now() + 10000;
  let asked = false, looks = 0;
  // The first look takes the board's answer of the last two seconds: the page has usually just
  // asked (a pw-dump costs a busy Zero W seconds, and a fresh one would wait behind it).
  const look = () => api(looks++ ? '/audio/endpoints?fresh=1' : '/audio/endpoints').then(ep => {
    renderEndpoints(ep);
    const src = ep.sources.find(e => same(e.address));
    if (src) {
      return startStream({type: 'capture', backend: src.backend, target: src.id, title: src.label}, sinks || [], !onStream,
        {rate: src.rate || 48000, channels: 2, gain_db: 0}, ctx);
    }
    if (!asked && player && player.status !== 'playing') {
      asked = true;
      toast('Asking the phone to play…');
      post('/media/players/control', {path: player.path, action: 'play'}).catch(() => {});
    }
    if (Date.now() > deadline) throw new Error('The phone is not streaming to the board: play something on it, with the board as its speaker.');
    return new Promise(r => setTimeout(r, 700)).then(look);
  });
  return look().then(s => { if (onStream) onStream(s); return s; })
    .catch(e => { if (ctx) ctx.close(); toast(e.message); return null; });
}

// ---------------------------------------------------------------- media (BlueZ)

function codecCell(c) {
  return `<b>${esc(c.name)}</b> <span class="small">${esc(c.summary)}</span>`;
}

function renderMedia(m) {
  au.media = m;
  renderPlayers(m);
  renderAudioDevices();
}

// ---------------------------------------------------------------- Bluetooth: one card a device

// The board's own role on a transport is its UUID: Audio Sink (or a sink PAC, or a broadcast it
// syncs to: Basic Audio Announcement) means the board receives from the device.
const receives = t => /^(110b|2bc9|1851)$/i.test(t.uuid);
// What kind of link a transport is, in words: BlueZ's UUID names are the board's PAC or role.
const LINK_KIND = {'110a': 'A2DP', '110b': 'A2DP', '2bc9': 'LE Audio · CIS', '2bcb': 'LE Audio · CIS',
  '1851': 'Auracast · BIS', '1852': 'Auracast · BIS'};
const linkKind = t => LINK_KIND[(t.uuid || '').toLowerCase()] || t.profile || t.uuid;
// AVRCP's absolute volume is 0..127; LE Audio's (the Volume Control Service) 0..255.
const volMax = t => /^110[ab]$/i.test(t.uuid) ? 127 : 255;

// Every device with a transport, a PipeWire card or stream endpoints, by address.
function audioDevices() {
  const m = au.media || {transports: [], endpoints: []};
  const by = new Map();
  const get = a => {
    const k = (a || '').toUpperCase();
    if (!by.has(k)) by.set(k, {address: k, transports: [], endpoints: [], card: null});
    return by.get(k);
  };
  m.transports.forEach(t => get(t.address).transports.push(t));
  ((au.devices && au.devices.devices) || []).forEach(c => { if (c.address) get(c.address).card = c; });
  m.endpoints.forEach(e => get(e.address).endpoints.push(e));
  by.delete('');
  return [...by.values()];
}

function audioDeviceCard(x) {
  const name = devName(x.address) !== x.address ? devName(x.address) : x.card ? x.card.label : x.address;
  const live = x.transports.some(t => t.state === 'active');
  const links = x.transports.slice().sort((a, b) => receives(b) - receives(a)).map(t => `<div class="ad-link">
      <span class="ad-dir"><b>${receives(t) ? 'Receiving' : 'Sending'}</b> <span class="muted small">${esc(linkKind(t))}</span></span>
      <span class="ad-codec">${codecCell(t.codec)}${t.delay_ms != null ? ` <span class="muted small">· delay ${t.delay_ms.toFixed(1)} ms</span>` : ''}</span>
      <span class="ad-state">${pill(t.state === 'active' ? 'streaming' : t.state, t.state === 'active' ? 'good' : t.state === 'pending' ? 'warn' : '')}</span>
      <span class="ad-acts">${/^185[12]$/.test(t.uuid) ? '<a class="small-btn btn-link" href="#leaudio" title="Receiving a broadcast is chosen in Audio → LE Audio">LE Audio</a>'
        : receives(t) ? `<button class="small-btn ad-listen" data-address="${esc(x.address)}" title="Hear what the device sends the board, in this browser">Listen here</button>` : ''}</span>
      ${t.volume != null ? `<label class="row small ad-vol"><span class="muted">Volume</span> <input type="range" min="0" max="${volMax(t)}" value="${t.volume}" data-path="${esc(t.path)}" class="grow">
        <span class="mono">${t.volume}</span></label>` : ''}
    </div>`).join('');
  // Every A2DP endpoint set has SBC (the spec makes it mandatory): one without it was discovered
  // half way — while the device was still registering its codecs — and holds for this connection.
  const a2dp = x.endpoints.filter(e => /^Audio (Sink|Source)$/.test(e.role || ''));
  const noSbc = ['Audio Sink', 'Audio Source'].filter(r => a2dp.some(e => e.role === r) && !a2dp.some(e => e.role === r && e.codec.id === 0));
  const partial = noSbc.length ? `<p class="small warn-text">Its ${noSbc.join(' and ').toLowerCase()} endpoints are listed without SBC, which every A2DP device has: the list was read while the device was still registering its codecs. Disconnect and connect it again for the whole list.</p>` : '';
  const eps = x.endpoints.length ? `<details class="more-fields" data-key="ep-${esc(x.address)}"${au.adOpen['ep-' + x.address] ? ' open' : ''}>
      <summary>Its stream endpoints (${x.endpoints.length})</summary>
      <div class="scroll-x"><table class="tbl"><tr><th>Role</th><th>Codec</th><th>Capabilities</th><th>Delay reports</th></tr>
      ${x.endpoints.map(e => `<tr><td>${esc(e.role || e.uuid)}</td><td><b>${esc(e.codec.name)}</b></td>
        <td class="wrap small" title="${esc(e.capabilities)}">${esc(e.codec.summary)}</td><td>${e.delay_reporting ? 'yes' : 'no'}</td></tr>`).join('')}</table></div>
    </details>` : '';
  const raw = x.transports.length ? `<details class="more-fields" data-key="raw-${esc(x.address)}"${au.adOpen['raw-' + x.address] ? ' open' : ''}><summary>Details</summary>
      <div class="kv" style="margin-top:.4rem">${x.transports.map(t => `<span>${esc(linkKind(t))}</span><span class="mono">${esc(t.configuration)} · ${esc(t.path)}</span>`).join('')}</div></details>` : '';
  return `<div class="card ad-card" data-address="${esc(x.address)}">
    <div class="card-head"><h3>${esc(name)} <span class="mono small muted">${esc(x.address)}</span></h3>
      ${pill(live ? 'streaming' : x.transports.length ? 'idle' : 'connected', live ? 'good' : '')}</div>
    ${links || '<p class="hint">No audio link set up: the device and the board have not agreed on a stream yet.</p>'}
    ${x.card ? `<label class="row wrap small ad-profile"><span class="muted">PipeWire profile and codec</span> ${codecSelect(x.card)}</label>` : ''}
    ${partial}${eps}${raw}
  </div>`;
}

function renderAudioDevices() {
  const el = $('ad-list');
  // Which links a device has comes with /media: without it, every device would look idle.
  if (!el || !au.media) return;
  // Not while a picker is open or a slider held: the rebuild would take it from under the pointer.
  const ae = document.activeElement;
  if (ae && el.contains(ae) && (ae.tagName === 'SELECT' || ae.type === 'range')) return;
  const ds = audioDevices();
  setHTML(el, ds.length ? ds.map(audioDeviceCard).join('')
    : `<div class="card empty-card"><h3>No Bluetooth audio device</h3><p class="hint">${esc((au.devices && au.devices.error) || '')}
        Connect a phone with the board as its speaker, or a speaker or headphones to play to (Devices): each shows here with its audio links and codec.</p></div>`);
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

function refreshMedia() { return api('/media').then(renderMedia).catch(e => { ldFail(e, $('ad-list'), $('au-players')); fail(e); }); }

// ---------------------------------------------------------------- radio

function renderRadio(r) {
  au.radio = r.stations;
  setHTML($('rd-list'), r.stations.length ? r.stations.map((s, i) => `<div class="row between radio-row">
      <span title="${esc(s.url)}">${esc(s.name)}</span>
      <span class="acts"><button class="small-btn" data-play="${i}" title="Play to the output chosen above">Play</button><button class="small-btn" data-hear="${i}" title="Listen in this browser">Listen</button></span></div>`).join('')
    : '<span class="muted">No stations: Edit to add some.</span>');
}

// A URL plays at the rate it most likely is (streamRate, player.js) unless one was chosen under
// Format and gain: decoding to another rate makes the decoder resample, at three times the CPU.
function playUrl(url, title, listen, rate) {
  const extra = $('st-rate').dataset.chosen ? {} : {rate: streamRate(rate)};
  return startStream({type: 'url', url, title, decoder: $('st-decoder').value}, chosenSinks(), listen, extra).catch(() => {});
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
  // The whole stack on or off, kept across reboots (btbench-audio on|off).
  $('au-on').onchange = e => {
    const on = e.target.checked;
    if (!on && !confirm('Switch the audio stack off? Audio links are dropped, and it stays off after a reboot until switched on here.')) {
      e.target.checked = true;
      return;
    }
    e.target.disabled = true;
    setText($('au-msg'), on ? 'starting PipeWire…' : 'stopping PipeWire…');
    put('/audio', {enabled: on}).then(s => { renderAudio(s); setText($('au-msg'), ''); refreshEndpoints(true); })
      .catch(err => { setText($('au-msg'), err.message); refreshAudio(); })
      .finally(() => { e.target.disabled = false; });
  };
  $('au-restart').onclick = () => {
    const restart = $('au-restart-bt').checked;
    if (!confirm(`Restart PipeWire and WirePlumber${restart ? ' and bluetoothd' : ''}? Audio connections are dropped.`)) return;
    $('au-restart').disabled = true;
    setText($('au-msg'), 'restarting…');
    post('/audio/restart', {restart_bt: restart}).then(s => { renderAudio(s); setText($('au-msg'), 'restarted'); refreshEndpoints(true); })
      .catch(e2 => { setText($('au-msg'), e2.message); refreshAudio(); })
      .finally(() => { $('au-restart').disabled = false; });
  };
  // Sent as the slider settles, not on every pixel of a drag.
  const volume = e => {
    const r = e.target.closest('input[type=range][data-path]');
    if (r) put('/media/transport', {path: r.dataset.path, volume: parseInt(r.value, 10)}).catch(fail);
  };
  // The device cards: volume, the codec picker, listening, what is open.
  const ad = $('ad-list');
  ad.addEventListener('change', e => {
    if (e.target.type === 'range') volume(e);
  });
  ad.addEventListener('toggle', e => { if (e.target.dataset && e.target.dataset.key) au.adOpen[e.target.dataset.key] = e.target.open; }, true);
  ad.addEventListener('click', e => {
    const ls = e.target.closest('.ad-listen');
    if (ls) listenToDevice(ls.dataset.address);
  });

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
    else if (b.classList.contains('lp-mode')) {
      const s = au.local;
      put('/media/local-player', b.dataset.mode === 'repeat' ? {repeat: boardRepeatValue(nextRepeat(boardRepeat(s.repeat)))} : {shuffle: !s.shuffle})
        .then(n => { au.local = n; setHTML(lp, localPlayerCard(n)); }).catch(fail);
    }
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
  pl.onchange = volume;
  pl.onclick = e => {
    const b = e.target.closest('.pl-btn');
    const p = b && b.dataset.mode && au.media && au.media.players.find(x => x.path === b.dataset.path);
    if (p) {
      put('/media/players', Object.assign({path: p.path}, b.dataset.mode === 'repeat'
        ? {repeat: btRepeatValue(nextRepeat(btRepeat(p.repeat)))} : {shuffle: p.shuffle === 'off' ? 'alltracks' : 'off'})).catch(fail);
    } else if (b && b.dataset.a && b.dataset.a !== 'fast-forward' && b.dataset.a !== 'rewind') {
      playerAction(b.dataset.path, b.dataset.a);
    }
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

  // The codec picker, on a device's card and next to the output.
  ad.onchange = $('st-codec').onchange = e => {
    const sel = e.target.closest('select.cd-pick');
    if (!sel) return;
    sel.disabled = true;
    put(`/audio/devices/${sel.dataset.id}/profile`, {profile: parseInt(sel.value, 10)})
      .then(d => { if (d.switch_error) toast(d.switch_error); refreshCodecs(); refreshEndpoints(true); })
      .catch(err => { fail(err); refreshCodecs(); })
      .finally(() => { sel.disabled = false; });
  };

  // The stream form.
  $('st-rate').onchange = () => { $('st-rate').dataset.chosen = '1'; };
  $('st-src').onchange = showSourceFields;
  showSourceFields();
  $('st-sinks').onclick = e => {
    const b = e.target.closest('[data-out]');
    if (b) pickNewOut(b.dataset.out);
  };
  $('st-refresh').onclick = () => refreshEndpoints(true);
  $('st-start').onclick = () => {
    const src = sourceFromForm();
    const sinks = chosenSinks();
    const listen = $('st-listen').checked;
    if (!sinks.length && !listen) { setText($('st-msg'), 'Choose an output in "Play to", or turn on "Also listen in this browser".'); return; }
    startStream(src, sinks, listen).catch(() => {});
  };
  const playTyped = () => {
    const url = $('st-url').value.trim();
    if (!url) return $('st-url').focus();
    if (!chosenSinks().length && !$('st-listen').checked) { toast('Choose an output in "Play to" above, or turn on "Also listen in this browser".'); return; }
    playUrl(url, '', $('st-listen').checked);
  };
  $('st-url-play').onclick = playTyped;
  $('st-url').onkeydown = e => { if (e.key === 'Enter') playTyped(); };

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
    } else if (e.target.closest('.st-pause, .st-prev, .st-next')) {
      const b = e.target.closest('button');
      const s = au.streams.find(x => x.id === id) || {};
      const body = b.classList.contains('st-pause') ? {paused: !s.paused} : b.classList.contains('st-next') ? {next: true} : {previous: true};
      put(`/audio/streams/${id}`, body).then(upsertStream).catch(fail);
    } else if (e.target.closest('.st-listen')) {
      if (au.listeners[id]) stopListening(id); else startListening(id);
    } else if (e.target.closest('[data-out]')) {
      pickStreamOut(id, e.target.closest('[data-out]').dataset.out);
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
    if (b.dataset.play != null && !chosenSinks().length) { toast('Choose an output in "Play to" above first, or use Listen.'); return; }
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
      return i < 0 ? {url: l} : {name: l.slice(0, i).trim(), url: l.slice(i + 1).trim()};  // the board names it
    });
    put('/audio/radio', {stations}).then(r => { renderRadio(r); $('rd-editor').hidden = true; }).catch(fail);
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
    api('/audio/radio').then(renderRadio).catch(e => ldFail(e, $('rd-list')));
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
    // Graphs can keep the pipewire topic while Audio is not on screen: no table to redraw then.
    if (topic === 'pipewire') return currentTab === 'audio' && renderPipewire(data);
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
