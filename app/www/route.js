'use strict';
// Audio → Route: what the board plays and where, as two lists side by side (one under the other on
// a phone). Inputs: the board's own music and every Bluetooth device that sends it audio, one row
// a device and a kind of audio; picking one makes it the route's input. Outputs: this browser, the
// board's sound card, every Bluetooth speaker or pair of earbuds and the board's Auracast
// broadcast, each on or off by itself (audio.js pickOut). Each row
// says its state; under a Bluetooth one are its links that way (kind, codec, delay, state, volume)
// and its codec picker, and folded its stream endpoints and the links' raw configuration. The
// input that plays carries the stream's level meters and gain. Under the outputs, folded, the
// board's own AVRCP player: what a car or speaker the board plays to sees.

au.rtOpen = {};  // the Details folds open, by device and direction

// A device's icon, as in Devices (once the device list is here).
function devSvg(address) {
  const d = btState && btState.devices.find(x => sameAddr(x.address, address));
  return d ? DEV_SVG[devKind(d).kind] : PL_SVG.bt;
}

function renderRoute() {
  if (currentTab !== 'audio' || panes.audio !== 'route' || holding()) return;
  renderInputs();
  renderRouteOutputs();
  renderRouteMeters();
}

// ---------------------------------------------------------------- a Bluetooth device's detail

// What a Bluetooth row shows under it: the device's links that way (`rx`: what it sends the
// board), each with its codec, the delay the sink reports, its state and its volume (AVRCP's
// absolute volume, or LE Audio's Volume Control); its codec picker (PipeWire's profile); and
// folded, its stream endpoints and the links' raw configuration. `extra` goes after the links.
function deviceMore(addresses, rx, extra) {
  const ts = (au.media ? au.media.transports : []).filter(t => addresses.some(a => sameAddr(a, t.address)) && receives(t) === rx);
  const links = ts.map(t => `<div class="ad-link">
      <span class="ad-dir muted small">${esc(linkKind(t))}</span>
      <span class="ad-codec">${t.codec ? codecCell(t.codec) : ''}${t.delay_ms != null ? ` <span class="muted small">· delay ${t.delay_ms.toFixed(1)} ms</span>` : ''}</span>
      <span class="ad-state">${pill(t.state === 'active' ? 'streaming' : t.state, t.state === 'active' ? 'good' : t.state === 'pending' ? 'warn' : '')}</span>
      ${t.volume != null ? `<label class="row small ad-vol"><span class="muted">Volume</span> <input type="range" min="0" max="${transportVolMax(t)}" value="${t.volume}" data-tvol="${esc(t.path)}" class="grow">
        <span class="mono">${t.volume}</span></label>` : ''}
    </div>`).join('');
  const cards = addresses.map(cardOf).filter(Boolean);
  // The profile's codec is what the board sends: on an input it is the link's way back, if any.
  const pick = cards.map(c => `<label class="row wrap small ad-profile"><span class="muted">${rx ? 'Profile' : 'Codec'}${cards.length > 1 ? ' · ' + esc(devName(c.address)) : ''}</span> ${codecSelect(c)}</label>`).join('');
  const eps = (au.media ? au.media.endpoints : []).filter(e => addresses.some(a => sameAddr(a, e.address)));
  // Every A2DP endpoint set has SBC (the spec makes it mandatory): one without it was discovered
  // half way — while the device was still registering its codecs — and holds for this connection.
  const a2dp = eps.filter(e => /^Audio (Sink|Source)$/.test(e.role || ''));
  const noSbc = ['Audio Sink', 'Audio Source'].filter(r => a2dp.some(e => e.role === r) && !a2dp.some(e => e.role === r && e.codec.id === 0));
  const partial = noSbc.length ? `<p class="small warn-text">Its ${noSbc.join(' and ').toLowerCase()} endpoints are listed without SBC, which every A2DP device has: the list was read while the device was still registering its codecs. Disconnect and connect it again for the whole list.</p>` : '';
  const key = addresses.join('+').toUpperCase() + (rx ? ':rx' : ':tx');
  const details = eps.length || ts.length ? `<details class="more-fields" data-key="${esc(key)}"${au.rtOpen[key] ? ' open' : ''}><summary>Details</summary>
      ${eps.length ? `<div class="scroll-x"><table class="tbl"><tr><th>Role</th><th>Codec</th><th>Capabilities</th><th>Delay reports</th></tr>
      ${eps.map(e => `<tr><td>${esc(e.role || e.uuid)}</td><td><b>${esc(e.codec.name)}</b></td>
        <td class="wrap small" title="${esc(e.capabilities)}">${esc(e.codec.summary)}</td><td>${e.delay_reporting ? 'yes' : 'no'}</td></tr>`).join('')}</table></div>` : ''}
      ${ts.length ? `<div class="kv">${ts.map(t => `<span>${esc(linkKind(t))}</span><span class="mono">${esc(t.configuration)} · ${esc(t.path)}</span>`).join('')}</div>` : ''}
    </details>` : '';
  if (!links && !extra && !pick && !partial && !details) return '';
  return `<div class="rt-more">${links}${extra || ''}${pick}${partial}${details}</div>`;
}

// The input stream's controls: its meters (renderRouteMeters fills them, four times a second), its
// gain, and a tone's frequency and a tone's or noise's level, all live.
function streamMore(s) {
  if (!s) return '';
  const src = s.source;
  return `<div class="rt-more">
    <div class="rt-meters"></div>
    <div class="row wrap small rt-ctl">
      <label class="row grow">Gain <input type="range" class="grow" data-st-gain min="-60" max="20" step="0.5" value="${s.gain_db}">
        <span class="mono rt-gain-v">${(+s.gain_db).toFixed(1)} dB</span></label>
      ${src.type === 'tone' ? `<label class="row">Hz <input type="number" class="narrow" data-st-freq min="1" max="24000" value="${src.freq}"></label>` : ''}
      ${src.type === 'tone' || src.type === 'noise' ? `<label class="row">Level <input type="number" class="narrow" data-st-level min="-96" max="0" value="${src.level_db}"> dB</label>` : ''}
      <span class="muted">${s.rate / 1000} kHz · ${s.channels === 1 ? 'mono' : 'stereo'}</span>
    </div></div>`;
}

function renderRouteMeters() {
  const el = document.querySelector('#rt-inputs .rt-meters');
  if (!el) return;
  const s = routeStream();
  setHTML(el, s && s.state === 'running' ? meterHTML(s.level, s.channels) : '');
}

// ---------------------------------------------------------------- inputs

// What the board plays, one at a time — its own music (what the library loaded: a station, the
// queue, a test signal) or one Bluetooth device's audio. A row picked is the route's input; what
// was before goes quiet (a playlist pauses, keeping its queue). A single-channel broadcast can be
// the other channel of the one picked (a left and a right broadcast are one stereo input). How the
// input does — waiting for the device's audio, why it failed — is said in its row. A device that
// stops sending stays the input and plays again when it resumes; one that failed or disconnected
// is not retried by itself, picking it again is the retry. The row is lit only while its audio
// plays: picked, and waiting for the device's audio, it shows the dots, and one that failed or
// stopped sending is not lit. Disconnect takes it off (the route has no input; the device stays
// connected). A phone that is not the input has its player's keys under it.
function renderInputs() {
  const inp = au.route ? au.route.input : {type: 'none', devices: [], state: 'idle'};
  const ls = loadedStream();
  const pick = au.inPick;
  const busyOf = on => on && (pick ? pick.what !== 'none' : inp.state === 'waiting' && !inp.error);
  const stateText = on => !on ? '' : pick && pick.what !== 'none' ? 'connecting…' : inp.state === 'waiting' ? (inp.error ? 'not sending' : 'waiting for its audio…')
    : inp.state === 'failed' ? 'failed' : 'the input';
  const off = `<button class="small-btn" data-in-off${pick ? ' disabled' : ''}>${pick && pick.what === 'none' ? '<span class="dots"></span>' : ''}Disconnect</button>`;
  const row = (attrs, on, icon, title, sub, state, extra, more) => {
    const lit = on && !busyOf(on) && inp.state !== 'failed' && !(inp.state === 'waiting' && inp.error);
    return `<div class="rt-item"><div class="tr lib in-row${lit ? ' now' : ''}${busyOf(on) ? ' busy' : ''}" ${attrs}>
      <span class="tr-n tr-art">${svg(icon)}</span>
      <span class="tr-t">${esc(title)}${sub ? `<span class="muted">${esc(sub)}</span>` : ''}</span>
      ${state ? `<span class="tr-fmt${on && inp.state === 'failed' ? ' bad-text' : ''}">${busyOf(on) ? '<span class="dots"></span>' : ''}${esc(state)}</span>` : ''}${extra || ''}${on ? off : ''}</div>${more || ''}</div>`;
  };
  const err = on => on && !pick && inp.state === 'failed' && inp.error ? `<div class="empty-line bad-text in-err">${esc(inp.error)}</div>` : '';
  // The row picked, until the board answered: shown as the input on the way. Disconnect keeps the
  // input's row as it is, its button showing the dots.
  const to = pick && pick.what !== 'none' ? pick.what : null;
  const local = to ? to === 'local' : inp.type === 'local';
  const playing = routeStream();
  let html = '<div class="in-head small muted">This board</div>' +
    row('data-in="local"', local, ls ? streamKind(ls).icon : PL_SVG.note, ls ? streamTitle(ls) : 'Nothing loaded',
      ls ? streamKind(ls).what + (ls.paused && !local ? ', paused' : '') : 'pick a station, tracks or a test signal in the library',
      local ? stateText(true) : '', '', local && !to ? streamMore(playing) : '');
  const rows = btInputs();
  const pairing = inp.type === 'bluetooth' && inp.devices.length === 1 && inp.devices[0].kind === 'broadcast'
    ? rows.find(r => r.kind === 'broadcast' && sameAddr(r.address, inp.devices[0].address)) : null;
  const rowHTML = (r, i) => {
    const on = to ? to === r.kind + '|' + r.address.toUpperCase() : isRouted(r);
    const sub = [KIND_WORDS[r.kind], r.codec, r.title, r.members && r.members.length > 1 ? `${r.members.length} devices` : '',
      r.side ? (r.side < 0 ? 'left channel' : 'right channel') : '',
      r.players.length ? r.players.map(p => p.name || (isMcp(p) ? 'media control' : 'player')).join(', ') : ''].filter(Boolean).join(' · ');
    // The other channel of the broadcast picked: one of the opposite side.
    const pair = pairing && !on && r.kind === 'broadcast' && r.side && pairing.side && r.side === -pairing.side
      ? `<button class="small-btn" data-in-pair="${i}" title="Play it as the ${r.side < 0 ? 'left' : 'right'} channel of the input">+ as ${r.side < 0 ? 'left' : 'right'}</button>` : '';
    // The input's player is in Players; another phone's keys are here, so it can be started
    // or stopped without making it the input.
    const p = !on && r.players.length ? r.players.find(x => x.status === 'playing') || r.players[0] : null;
    const keys = p ? `<div class="row small rt-player"><span class="muted grow">${esc(p.name || 'Player')} · ${esc(p.status || 'unknown')}${p.track && p.track.title ? ' · ' + esc(p.track.title) : ''}</span>
      ${['previous', 'play', 'pause', 'next'].map(a => `<button class="np-btn minor" data-pa="${a}" data-path="${esc(p.path)}" title="${a}" aria-label="${a}">${svg(PL_SVG[a])}</button>`).join('')}</div>` : '';
    const more = r.kind === 'hfp' ? '' : deviceMore(r.members || [r.address], true, keys);
    return row(`data-in="${i}"`, on, r.kind === 'broadcast' ? PL_SVG.bt : r.kind === 'hfp' ? PL_SVG.phone : devSvg(r.address),
      r.name, sub, on ? stateText(on) : r.state, pair, more + (on && !to ? streamMore(playing) : '')) + err(on);
  };
  const some = bc => rows.map((r, i) => (r.kind === 'broadcast') === bc ? rowHTML(r, i) : '').join('');
  const rx = rxAdapter();
  html += '<div class="in-head small muted">Bluetooth</div>' + (some(false) ||
    `<div class="empty-line">No Bluetooth device sends the board audio: connect one in Devices (the board as its speaker)${rx ? '' : ', or scan for broadcasts in LE Audio'}.</div>`);
  if (rx) {
    // Broadcasters heard whose streams BlueZ has not read yet (it reads the BASE while it scans):
    // listed, not pickable until they have streams. Only while they are heard (an RSSI): BlueZ keeps
    // a device's last announcement after it stops broadcasting, and that one would be "reading"
    // forever.
    const reading = (au.la.sources || []).filter(x => x.rssi != null && !rows.some(r => r.kind === 'broadcast' && sameAddr(r.address, x.address)))
      .map(x => row('', false, PL_SVG.bt, x.name || devName(x.address), 'Auracast', 'reading its streams…')).join('');
    // The dots while it scans, and while a start or stop is on the way.
    const dots = rx.discovering || au.scanBusy ? '<span class="dots"></span>' : '';
    html += `<div class="in-head small muted in-bc-head">Auracast<button class="small-btn" data-in-scan${au.scanBusy ? ' disabled' : ''}>${dots}${rx.discovering ? 'Stop scan' : 'Scan'}</button></div>` +
      (some(true) + reading || `<div class="empty-line">${rx.discovering ? 'Scanning for Auracast broadcasts…' : 'No broadcast found yet: scan for the ones in range.'}</div>`);
  }
  setHTML($('rt-inputs'), html);
  au.inRows = rows;
  renderRouteMeters();
}

// An input picked. A broadcast not received yet is received first (its node comes once the board
// synced to it); a phone that is not playing is asked once to play, never paused later. Until the
// board answered, the row picked shows the dots (au.inPick: "local", "<kind>|<address>" or "none").
function pickInput(input, r) {
  const d = (input.devices || [])[0];
  const what = input.type === 'local' ? 'local' : input.type === 'none' ? 'none' : d ? d.kind + '|' + d.address.toUpperCase() : '';
  const pick = au.inPick = what ? {what} : null;
  const done = () => { if (au.inPick === pick) { au.inPick = null; renderInputs(); } };
  renderInputs();
  const pre = [];
  if (r && r.kind === 'broadcast' && r.state === 'in range') pre.push(receiveBroadcast({transports: r.transports}, true));
  if (r && r.players.length && !r.players.some(p => p.status === 'playing')) {
    const p = r.players.find(x => x.path === au.players[r.address.toUpperCase()]) || r.players[0];
    post('/media/players/control', {path: p.path, action: 'play'}).catch(() => {});
  }
  au.sw = null;
  // An output the new input rules out (the device itself, a broadcast while one is received) is
  // taken off with it.
  const kind = au.endpoints && au.endpoints.backend === 'alsa' ? 'alsa' : 'pipewire';
  const ruled = v => { const e = outEndpoint(v); return e && v.startsWith(kind + ':') && outBlocked(e, input); };
  const outs = routeOuts(), keep = outs.filter(v => !ruled(v));
  const body = keep.length === outs.length ? {input} : {input, outputs: outSinks(keep)};
  return Promise.all(pre).then(() => put('/audio/route', body)).then(r => { done(); setRoute(r); }).catch(e => { done(); fail(e); });
}

function inputClick(e) {
  if (e.target.closest('[data-in-scan]')) { broadcastScan(); return true; }
  if (e.target.closest('[data-in-off]')) { if (!au.inPick) pickInput({type: 'none'}); return true; }
  const pair = e.target.closest('[data-in-pair]');
  if (pair) {
    const r = au.inRows[parseInt(pair.dataset.inPair, 10)];
    if (r) pickInput({type: 'bluetooth', devices: au.route.input.devices.concat([{address: r.address, kind: 'broadcast'}])}, r);
    return true;
  }
  const el = e.target.closest('[data-in]');
  if (!el) return false;
  if (el.dataset.in === 'local') {
    pickInput({type: 'local'});
    return true;
  }
  const r = au.inRows[parseInt(el.dataset.in, 10)];
  if (r) pickInput({type: 'bluetooth', devices: [{address: r.address, kind: r.kind}]}, r);
  return true;
}

// The adapter that can receive broadcasts, or null: the inputs then have an Auracast section.
const rxAdapter = () => (au.la ? au.la.adapters : []).find(a => a.receiver) || null;
// Scan for broadcasts (an LE scan of 30 s on that adapter, as LE Audio does), or stop it.
function broadcastScan() {
  const a = rxAdapter();
  if (!a || au.scanBusy) return;
  const on = !a.discovering;
  const over = () => { au.scanBusy = false; renderInputs(); };
  au.scanBusy = true;
  renderInputs();
  post('/bluetooth/scan', on ? {on: true, adapter: a.adapter, transport: 'le', seconds: 30} : {on: false, adapter: a.adapter})
    .then(refreshLa).then(over).catch(e => { over(); fail(e); });
}

// ---------------------------------------------------------------- outputs

// An output's row, in words: the switch this browser asked for (until the board answered it, or
// how it failed), else what the route's stream says of that sink (audio.js sinkLine), else whether
// it is on. {cls, text}.
function outRowState(x, cur, b) {
  const v = x.value, sw = au.sw;
  if (x.off) return {cls: 'muted', text: x.off};
  if (sw && sw.value === v && sw.phase === 'failed') return {cls: 'bad-text', text: 'failed: ' + outReason(sw.error)};
  if (sw && sw.value === v && sw.phase !== 'done') {
    return {cls: '', text: {broadcast: 'starting…', asking: sw.add ? 'asking…' : 'taking it off…', removing: 'taking it off…',
      starting: 'opening…', linking: 'waiting for its link…'}[sw.phase]};
  }
  if (!cur.includes(v)) return {cls: 'muted', text: x.gone ? 'not connected' : ''};
  if (v === 'browser') return !au.hearId ? {cls: '', text: 'on'} : npAudio.paused || npAudio.readyState < 3 ? {cls: '', text: 'connecting…'} : {cls: 'good-text', text: 'playing'};
  if (!b) return {cls: '', text: 'on'};
  const ks = (b.sinks_status || []).filter(k => v === BCAST ? isBcastOut(sinkOut(k)) : sinkOut(k) === v);
  if (!ks.length) return sinkState(b, v) === 'busy' ? {cls: '', text: 'opening…'} : {cls: 'bad-text', text: 'not playing: tap to retry'};
  const ls = ks.map(k => sinkLine(k, b));
  const l = ls.find(x => x.cls !== 'good') || ls[0];
  return {cls: {good: 'good-text', warn: 'warn-text', bad: 'bad-text'}[l.cls] || '', text: l.text};
}

// This browser's row, when it listens: its volume, and folded the spectrum of what it hears.
function browserMore() {
  return `<div class="rt-more">
    <label class="row small"><span class="muted">Volume</span> <input type="range" class="grow" data-hear-vol min="0" max="100" value="${Math.round(npAudio.volume * 100)}"></label>
    <details class="more-fields rt-spec-fold"${au.specOpen ? ' open' : ''}><summary>Spectrum</summary>
      <span class="rt-buf small mono muted"></span><canvas class="graph short rt-spec"></canvas></details></div>`;
}

// The outputs as rows (outOptions, outState).
function renderRouteOutputs() {
  if (!(au.endpoints || au.endpointsFailed) || !au.route) return;
  const b = outStream(), cur = curOuts(), opts = outOptions();
  cur.forEach(v => { if (!opts.some(x => x.value === v)) opts.push({value: v, name: outName(v), sub: 'not connected', icon: PL_SVG.bt, gone: true}); });
  const rows = opts.map(x => {
    const st = x.off ? '' : outState(x.value, cur, b, au.sw);
    const said = outRowState(x, cur, b);
    const e = x.value.includes(':') ? outEndpoint(x.value) : null;
    const addrs = e ? (e.set_leader ? e.members || [] : e.address ? [e.address] : []) : [];
    const more = x.value === 'browser' ? (cur.includes('browser') ? browserMore() : '')
      : x.value === BCAST ? '<div class="rt-more small"><a href="#leaudio">Its settings are in LE Audio</a></div>'
      : addrs.length ? deviceMore(addrs, false) : '';
    return `<div class="rt-item"><div class="tr lib rt-out${st === 'on' ? ' now' : ''}${st === 'busy' ? ' busy' : ''}${st === 'bad' ? ' bad' : ''}${x.gone ? ' gone' : ''}${x.off ? ' off' : ''}"
        data-out="${esc(x.value)}" role="switch" aria-checked="${cur.includes(x.value)}"${x.off ? ' aria-disabled="true"' : ''}>
      <span class="tr-n tr-art">${st === 'busy' ? '<span class="dots"></span>' : svg(x.icon)}</span>
      <span class="tr-t">${esc(x.name)}${x.sub ? `<span class="muted">${esc(x.sub)}</span>` : ''}</span>
      ${said.text ? `<span class="tr-fmt ${said.cls}">${esc(said.text)}</span>` : ''}
      <span class="rt-sw${cur.includes(x.value) ? ' on' : ''}" aria-hidden="true"><i></i></span></div>${more}</div>`;
  }).join('');
  setHTML($('rt-outputs'), rows);
  if (au.specOpen && cur.includes('browser')) drawHearing();
}

// ---------------------------------------------------------------- the board's own player

// What a car or speaker the board plays to sees over AVRCP: the board's player (bench_player.h),
// its track and its library, which such a device can browse. The library is browsed here from the
// tree the state carries; playing from here is the same as playing from the remote. It has no
// topic: read when its fold opens, after its own buttons, and every two seconds while it is open.
function localPlayerCard(s) {
  const lib = s.library || [];
  const byId = id => lib.find(i => i.id === id);
  const t = s.track || {};
  const playing = s.status === 'playing';
  const btn = (a, title) => `<button class="pl-btn lp-a" data-a="${a}" title="${title}" aria-label="${title}">${svg(PL_SVG[a])}</button>`;
  const folder = au.lpFolder == null ? null : au.lpFolder;
  const entries = lib.filter(i => (i.parent == null ? null : i.parent) === folder);
  const cur = s.track ? s.track.id : null;
  const path = [];
  for (let f = folder == null ? null : byId(folder); f; f = f.parent == null ? null : byId(f.parent)) path.unshift(f.name);
  const reg = (s.registered || []).map(a => a.split('/').pop()).join(', ');
  const num = t.Number ? `track ${t.Number}${t.NumberOfTracks ? ' of ' + t.NumberOfTracks : ''}` : '';
  return `<div class="player lp">
    <div class="pl-art">${svg(PL_SVG.note)}</div>
    <div class="head"><span class="pl-who">What a car or speaker sees</span>
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

function showLocalPlayer(s) {
  au.local = s;
  const ae = document.activeElement;
  if (ae && $('au-local').contains(ae) && ae.type === 'checkbox') return;
  setHTML($('au-local'), localPlayerCard(s));
}

function refreshLocalPlayer() {
  return api('/media/local-player').then(showLocalPlayer).catch(e => setHTML($('au-local'), `<div class="small bad-text">${esc(e.message)}</div>`));
}

function localPlayerPost(body) {
  return post('/media/local-player', body).then(showLocalPlayer).catch(fail);
}

function localPlayerWatch(on) {
  clearInterval(au.lpTick);
  au.lpTick = null;
  if (!on) return;
  refreshLocalPlayer();
  au.lpTick = setInterval(() => { if (!document.hidden) refreshLocalPlayer(); }, 2000);
}

// The route view shown: the codec pickers are read, and the board's player again if its fold is
// open. Left (another view, another tab): nothing of it is asked for any more.
function routeShown() {
  refreshCodecs();
  renderRoute();
  if ($('rt-board-player').open) localPlayerWatch(true);
}
function routeHidden() {
  localPlayerWatch(false);
}

// ---------------------------------------------------------------- wiring

function setupRoute() {
  const cols = $('rt-cols');
  $('rt-inputs').onclick = e => {
    const k = e.target.closest('[data-pa]');
    if (k) return playerAction(k.dataset.path, k.dataset.pa);
    inputClick(e);
  };
  $('rt-outputs').onclick = e => {
    const row = e.target.closest('.tr[data-out]');
    if (row && !row.classList.contains('off')) pickOut(row.dataset.out);
  };
  // Sliders show their value as they move, and are sent once let go.
  cols.addEventListener('input', e => {
    const t = e.target;
    if (t.dataset.tvol != null) setText(t.nextElementSibling, t.value);
    else if (t.dataset.stGain != null) setText(t.parentElement.querySelector('.rt-gain-v'), `${parseFloat(t.value).toFixed(1)} dB`);
    else if (t.dataset.hearVol != null) npAudio.volume = parseInt(t.value, 10) / 100;
  });
  cols.addEventListener('change', e => {
    const t = e.target, s = routeStream();
    if (t.matches('select.cd-pick')) pickCodec(t);
    else if (t.dataset.tvol != null) put('/media/transport', {path: t.dataset.tvol, volume: parseInt(t.value, 10)}).catch(fail);
    else if (t.dataset.stGain != null && s) put(`/audio/streams/${s.id}`, {gain_db: parseFloat(t.value)}).then(refreshPlayerStreams).catch(fail);
    else if (t.dataset.stFreq != null && s) put(`/audio/streams/${s.id}`, {source: {freq: parseFloat(t.value)}}).then(refreshPlayerStreams).catch(fail);
    else if (t.dataset.stLevel != null && s) put(`/audio/streams/${s.id}`, {source: {level_db: parseFloat(t.value)}}).then(refreshPlayerStreams).catch(fail);
    else return;
    // Let go of: the view redraws again (it waits while a slider or a picker has the focus).
    if (t.type === 'range' || t.tagName === 'SELECT') t.blur();
  });
  // "toggle" does not bubble: caught on the way down, it says which folds are open.
  cols.addEventListener('toggle', e => {
    const d = e.target;
    if (d.dataset && d.dataset.key) au.rtOpen[d.dataset.key] = d.open;
    if (d.classList && d.classList.contains('rt-spec-fold')) {
      au.specOpen = d.open;
      if (d.open && hearAnalyser()) drawHearing();
    }
  }, true);

  // The board's player.
  $('rt-board-player').addEventListener('toggle', e => {
    if (e.target.id === 'rt-board-player') localPlayerWatch(e.target.open);
    if (e.target.id === 'lp-more') au.lpOpen = e.target.open;
  }, true);
  const lp = $('au-local');
  lp.addEventListener('change', e => {
    if (e.target.id === 'lp-enabled') put('/media/local-player', {enabled: e.target.checked}).then(showLocalPlayer).catch(fail);
  });
  lp.addEventListener('click', e => {
    const b = e.target.closest('button');
    if (!b || !au.local) return;
    const id = b.dataset.id != null ? Number(b.dataset.id) : null;
    if (b.classList.contains('lp-a')) localPlayerPost({action: b.dataset.a});
    else if (b.classList.contains('lp-mode')) {
      const s = au.local;
      put('/media/local-player', b.dataset.mode === 'repeat' ? {repeat: boardRepeatValue(nextRepeat(boardRepeat(s.repeat)))} : {shuffle: !s.shuffle})
        .then(showLocalPlayer).catch(fail);
    } else if (b.classList.contains('lp-play')) localPlayerPost({play: id});
    else if (b.classList.contains('lp-add')) localPlayerPost({enqueue: id});
    else if (b.classList.contains('lp-open')) { au.lpFolder = id; showLocalPlayer(au.local); }
    else if (b.classList.contains('lp-up')) {
      const f = au.local.library.find(i => i.id === au.lpFolder);
      au.lpFolder = f && f.parent != null ? f.parent : null;
      showLocalPlayer(au.local);
    }
  });
}
setupRoute();
