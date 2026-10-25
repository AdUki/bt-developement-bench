'use strict';
// Listen: the console as a music player rather than a bench. The board plays one input at a time
// — its own music (internet radio, tracks from a UPnP media server as a playlist the board plays to
// the end, a test signal) or what one Bluetooth device sends it (a phone's A2DP and its AVRCP
// player: track, cover, controls, its now-playing list and library; an LE Audio source; an
// Auracast broadcast, or a left and a right one as one stereo input; a call's audio over HFP) —
// on any number of outputs at once: the board's sound card, Bluetooth speakers and earbuds, its
// Auracast broadcasts, and this browser. The input and the outputs are the daemon's route
// (/api/audio/route, audio/route.h), the same for every browser; Inputs picks the input,
// "Play to" turns each output on or off. Devices that are not the input play nowhere and are left
// alone. The bench views (Audio) stay for everything else.
//
// The board's playlist lives in the daemon (a URL stream's playlist): closing the page does not
// stop the music, and another phone opening the page sees the same queue.

const pl = {
  media: null,         // the "media" topic: players, transports
  mediaFailed: false,  // its first read failed: nothing is on the way any more
  streams: [],         // the "audio.streams" topic
  endpoints: null,     // the board's outputs
  endpointsFailed: false,
  route: null,         // GET /api/audio/route, the "audio.route" topic: the input and the outputs
  players: {},         // a device's address → the player of it picked (it has several)
  boardId: null,       // the stream this view started last
  lib: lsGet('btbench.pl.lib') || '',  // phone | radio | upnp | signals; '' = the first
  libPath: [],         // the library's breadcrumbs: [{id, title}] (UPnP), or folders (phone)
  libItems: [],
  upnpServer: null,
  queueItems: [],      // the phone's now-playing list
  queueAt: 0,          // when it was read
  libMore: null,       // the UPnP folder's pages still to load (upnpPage)
  libView: null,       // how the library is drawn, for the pages appended to it
  libSeq: 0,           // the library's latest request, and the phone folder it is loading
  libLoading: '',
  tick: null,
  ticks: 0,
  artDone: {},   // cover URL → true (shown) / false (failed)
  artTries: {},  // a phone's cover URL → how many times it was asked for again
  artWait: {},   // player|track → when its cover handle was first waited for
};

const PL_SVG = {
  previous: 'M18 6l-8.5 6L18 18zM6 6h2v12H6z', next: 'M6 6l8.5 6L6 18zM16 6h2v12h-2z',
  play: 'M8 5v14l11-7z', pause: 'M7 5h4v14H7zM13 5h4v14h-4z', stop: 'M6 6h12v12H6z',
  vol: 'M4 9v6h4l5 4V5L8 9zM16 8.5a5 5 0 010 7M18.5 6a8.5 8.5 0 010 12',
  note: 'M9 18V6l10-2v12M9 18a2 2 0 11-4 0 2 2 0 014 0zM19 16a2 2 0 11-4 0 2 2 0 014 0z',
  radio: 'M4 10h16v10H4zM7 10l10-5M8 15h.01M12 15h5',
  bt: 'M7 7l10 10-5 5V2l5 5L7 17',
  speaker: 'M6 3h12v18H6zM12 17.5a3 3 0 100-6 3 3 0 000 6zM12 7h.01',
  off: 'M4 4l16 16M9 9v6h4l5 4V14M13 5L11 7',
  server: 'M4 4h16v6H4zM4 14h16v6H4zM8 7h.01M8 17h.01',
  phone: 'M8 3h8a1 1 0 011 1v16a1 1 0 01-1 1H8a1 1 0 01-1-1V4a1 1 0 011-1zM11 18h2',
  folder: 'M4 6a2 2 0 012-2h4l2 2h6a2 2 0 012 2v10a2 2 0 01-2 2H6a2 2 0 01-2-2z',
  plus: 'M12 5v14M5 12h14',
  remove: 'M6 6l12 12M18 6L6 18',
  playnext: 'M4 6h12M4 11h12M4 16h6M14 14v6l5-3z',  // into the queue, right after what plays
  wave: 'M2 12h3l3-7 4 14 4-10 3 3h3',
  mic: 'M12 3a3 3 0 013 3v6a3 3 0 01-6 0V6a3 3 0 013-3zM5 11a7 7 0 0014 0M12 18v3',
  into: 'M12 3v11M7 9l5 5 5-5M4 15v4h16v-4',     // music coming into the board
  outof: 'M12 15V4M7 9l5-5 5 5M4 15v4h16v-4',    // the board playing it out
  arrow: 'M4 12h15M14 7l5 5-5 5',
  browser: 'M3 5h18v12H3zM8 21h8M12 17v4',
};
// A cover image's outcome, by URL: the art box stops showing it is loading, and a failed image
// leaves the placeholder.
// A phone's cover that failed is asked for again a few times, a little later each time: the first
// one of a connection is often asked for while the phone's image channel is still opening.
function npArtDone(img, ok) {
  const src = img.getAttribute('src');
  pl.artDone[src] = ok;
  img.hidden = !ok;
  img.parentNode.classList.remove('loading');
  const base = src.replace(/&r=\d+$/, '');
  const n = (pl.artTries[base] || 0) + 1;
  if (!ok && base.startsWith('/api/media/players/cover') && n <= 4) {
    pl.artTries[base] = n;
    setTimeout(renderNowPlaying, 1500 * n);
  }
}

const svg = (d, cls) => `<svg viewBox="0 0 24 24" class="${cls || ''}" aria-hidden="true"><path d="${d}"/></svg>`;

// ---------------------------------------------------------------- what plays

const live = s => s.state === 'running' || s.state === 'starting';
const isUrl = s => s.source.type === 'url';
const TEST_SIGNALS = ['tone', 'sweep', 'noise', 'silence'];

// The route (GET /api/audio/route, the "audio.route" topic): the one input the board plays and the
// outputs it plays on, the same for every browser. The stream it plays: the board's own (a
// station, tracks, a test signal) or the capture the board made of the device picked.
const routeStream = () => pl.route && pl.route.stream ? pl.streams.find(s => s.id === pl.route.stream) || null : null;
// The board's own stream that is loaded, playing or paused while a device is the input.
const loadedStream = () => pl.route && pl.route.local ? pl.streams.find(s => s.id === pl.route.local) || null : null;
const routeOuts = () => (pl.route ? pl.route.outputs : []).map(sinkOut);

// What a board stream is, for the inputs and above the title.
function streamKind(s) {
  const t = s.source.type, list = s.source.playlist || [];
  if (t === 'url') {
    const item = list[s.track] || {};
    return list.length > 1 || item.duration_ms || item.duration_s ? {what: 'Media server', icon: PL_SVG.server}
      : {what: 'Internet radio', icon: PL_SVG.radio};
  }
  if (TEST_SIGNALS.includes(t)) return {what: 'Test signal', icon: PL_SVG.wave};
  return {what: s.source.monitor ? 'What an output plays' : 'Audio input', icon: PL_SVG.mic};
}
// A test signal in words: "Tone 1000 Hz" and its level.
function signalTitle(src) {
  const hz = f => f >= 1000 && f % 100 === 0 ? `${f / 1000} kHz` : `${f} Hz`;
  if (src.type === 'tone') return 'Tone ' + hz(src.freq) + (src.freq_right && src.freq_right !== src.freq ? ` left, ${hz(src.freq_right)} right` : '');
  if (src.type === 'sweep') return `Sweep ${hz(src.from)} – ${hz(src.to)}`;
  if (src.type === 'noise') return `${(src.color || 'pink').replace(/^./, c => c.toUpperCase())} noise`;
  return 'Silence';
}
const streamTitle = s => TEST_SIGNALS.includes(s.source.type) ? signalTitle(s.source)
  : s.meta.title || s.source.title || (s.source.type === 'url' ? 'Internet radio' : s.label);
// The players worth showing: BlueZ also makes an empty one for a phone's LE Audio media control
// service (mcp_gmcs: no name, never a track), which would be a second, dead one for the phone. An
// AVRCP player is always one: right after pairing, before the phone has played anything, it has no
// name and no track yet, and its play button is how the music gets started from here.
const phones = () => (pl.media ? pl.media.players : []).filter(p => p.name || p.browsable || (p.track && p.track.title) ||
  /\/avrcp\//.test(p.path));
const isMcp = p => /\/mcp_/.test(p.path);  // BlueZ's mcp_gmcs/playerN, mcp_mcs/playerN

// Bluetooth audio coming into the board with no player of the device's to show for it, each
// shown the way a phone's is (`input` set): an Auracast broadcast in range — every one BlueZ has
// read, received or not, so one can be picked and play and stop receive it — and a device that
// streams to the board with no media control (an LE Audio source). A broadcaster's BISes are one.
const sameAddr = (a, b) => !!a && !!b && a.toUpperCase() === b.toUpperCase();
function inputs() {
  const ts = pl.media ? pl.media.transports : [], ps = phones();
  const out = [];
  for (const t of ts) {
    const bcast = /^1851$/i.test(t.uuid);
    if (!bcast && !(/^(110b|2bc9)$/i.test(t.uuid) && t.state !== 'idle' && !ps.some(p => sameAddr(p.address, t.address)))) continue;
    const key = 'in:' + (bcast ? t.device : t.path);
    let x = out.find(o => o.path === key);
    if (!x) {
      x = {path: key, input: true, broadcast: bcast, address: t.address, device_name: t.device_name, name: '', transports: [], track: {}};
      out.push(x);
    }
    x.transports.push(t);
  }
  out.forEach(inputStatus);
  return out;
}
function inputStatus(x) {
  const st = x.transports.map(t => t.state);
  x.receiving = st.some(v => v === 'active' || v === 'broadcasting');
  x.status = x.receiving ? 'playing' : st.includes('pending') ? 'syncing' : x.broadcast ? 'not received' : 'stopped';
  x.transport = x.transports.length ? x.transports[0].path : '';
  x.track = {title: x.transports.map(t => t.program_info).find(Boolean) || ''};
}

// What a source node the board records from is, in the route's words (route.cpp route_kind).
function routeKind(e) {
  if (e.kind !== 'bluetooth') return '';
  if (e.transport) return 'broadcast';
  const p = e.profile || '';
  if (/^a2dp/.test(p)) return 'a2dp';
  if (/^bap/.test(p)) return 'bap';
  if (/^(headset|audio-gateway|hfp|hsp)/.test(p)) return 'hfp';
  return e.codec === 'lc3' ? 'bap' : /^(cvsd|msbc|lc3_swb)$/.test(e.codec) ? 'hfp' : 'a2dp';
}
const KIND_WORDS = {a2dp: 'A2DP', bap: 'LE Audio', broadcast: 'Auracast', hfp: 'Call audio (HFP)'};
// LE Audio's audio locations: a part is the left or the right channel with front left or front
// right and not both.
const sideOf = loc => loc == null ? 0 : (loc & 1) && !(loc & 2) ? -1 : (loc & 2) && !(loc & 1) ? 1 : 0;

// Every Bluetooth input there is, one row a device and a kind of audio (a phone's music and its
// call audio are two): the players (AVRCP, MCP), what the board receives (A2DP and LE Audio
// transports, broadcasts in range) and the nodes it can record (HFP has no transport of BlueZ's).
// A coordinated set (a pair of earbuds) is one row, for PipeWire's node of the set.
function btInputs() {
  const rows = [];
  const row = (address, kind) => {
    let r = rows.find(y => sameAddr(y.address, address) && y.kind === kind);
    if (!r) rows.push(r = {address, kind, players: [], transports: [], source: null, members: null});
    return r;
  };
  const ts = pl.media ? pl.media.transports : [];
  const srcs = pl.endpoints ? pl.endpoints.sources.filter(e => e.kind === 'bluetooth') : [];
  const sets = srcs.filter(e => e.set_leader && e.members && e.members.length);
  const inSet = a => sets.find(e => e.members.some(m => sameAddr(m, a)));
  const addrOf = a => { const s = inSet(a); return s ? s.members[0] : a; };
  phones().forEach(p => row(addrOf(p.address), isMcp(p) ? 'bap' : 'a2dp').players.push(p));
  ts.forEach(t => {
    if (/^1851$/i.test(t.uuid)) row(t.address, 'broadcast').transports.push(t);
    else if (/^110b$/i.test(t.uuid)) row(t.address, 'a2dp').transports.push(t);
    else if (/^2bc9$/i.test(t.uuid)) row(addrOf(t.address), 'bap').transports.push(t);
  });
  srcs.forEach(e => {
    if (e.set && !e.set_leader) return;
    const k = routeKind(e);
    const r = row(e.set_leader ? e.members[0] : e.address, k);
    r.source = r.source || e;
    if (e.set_leader) r.members = e.members;
  });
  rows.forEach(r => {
    const s = inSet(r.address);
    if (s && r.kind === 'bap') { r.members = s.members; r.source = r.source || s; }
    r.name = r.members ? r.members.map(devName).filter((v, i, a) => a.indexOf(v) === i).join(' + ') : devName(r.address);
    if (r.kind === 'broadcast') {
      const x = {transports: r.transports, broadcast: true};
      inputStatus(x);
      r.state = x.status === 'playing' ? 'receiving' : x.status === 'syncing' ? 'syncing' : 'in range';
      r.title = x.track.title;
      const locs = r.transports.map(t => t.location).filter(v => v != null);
      // One channel of a stereo pair: it can be the other half of an input.
      r.side = locs.length === 1 ? sideOf(locs[0]) : 0;
    } else {
      const on = r.transports.some(t => t.state === 'active');
      r.state = on ? 'streaming' : r.source && r.kind === 'hfp' ? 'connected' : r.transports.length || r.source ? 'connected, not streaming'
        : 'remote control only';
      const t = r.transports.find(x => x.codec) || null;
      r.codec = t && t.codec ? t.codec.name : r.source && r.source.codec ? r.source.codec.toUpperCase() : '';
    }
  });
  // The board's own broadcasts are outputs, not inputs.
  return rows.filter(r => r.address);
}
const isRouted = r => pl.route && pl.route.input.type === 'bluetooth' &&
  pl.route.input.devices.some(d => sameAddr(d.address, r.address) && (!d.kind || d.kind === r.kind));

// The source on screen: the route's input. A device with several players (Spotify and the phone's
// own music app) shows the one picked, else the one playing.
function current() {
  const r = pl.route;
  if (!r) return {kind: 'none'};
  if (r.input.type === 'local') {
    const s = routeStream();
    return s ? {kind: 'board', s} : {kind: 'none'};
  }
  if (r.input.type !== 'bluetooth' || !r.input.devices.length) return {kind: 'none'};
  const d = r.input.devices[0];
  if (d.kind !== 'broadcast' && d.kind !== 'hfp') {
    const ps = phones().filter(p => sameAddr(p.address, d.address) || (inputSet(d.address) || []).some(m => sameAddr(m, p.address)));
    const p = ps.find(x => x.path === pl.players[d.address.toUpperCase()]) || ps.find(x => x.status === 'playing') || ps[0];
    if (p) return {kind: 'phone', p, players: ps};
  }
  const x = inputs().find(i => sameAddr(i.address, d.address) && (d.kind !== 'broadcast' || i.broadcast)) ||
    {path: 'in:' + d.address, input: true, broadcast: d.kind === 'broadcast', address: d.address, name: '', transports: [], track: {}};
  if (!x.transports) x.transports = [];
  if (!x.transports.length) inputStatus(x);
  x.hfp = d.kind === 'hfp';
  if (r.input.devices.length > 1) x.also = r.input.devices.slice(1).map(o => o.address);
  return {kind: 'phone', p: x, players: []};
}
function inputSet(address) {
  const e = pl.endpoints && pl.endpoints.sources.find(s => s.set_leader && (s.members || []).some(m => sameAddr(m, address)));
  return e ? e.members : null;
}

// The stream the output row acts on: the route's.
const outStream = () => routeStream();


// ---------------------------------------------------------------- now playing

function renderNowPlaying() {
  const c = current();
  // The input is picked in Inputs; here, a device with several players (Spotify and the
  // phone's own music app, an LE Audio device's media control) has a chip each, named after the
  // player.
  // Two of one name (a board's AVRCP player and its LE Audio media control are both "btbench") say
  // which they are.
  const pname = p => p.name || (isMcp(p) ? 'Media control' : 'Player');
  const twice = p => (c.players || []).filter(x => pname(x) === pname(p)).length > 1;
  const chips = (c.players || []).length > 1 ? c.players.map(p => ({key: p.path, label: pname(p) + (twice(p) ? (isMcp(p) ? ' · LE Audio' : ' · AVRCP') : ''),
    icon: isMcp(p) ? PL_SVG.bt : PL_SVG.phone, on: c.p.path === p.path})) : [];
  setHTML($('np-src'), chips.map(x =>
    `<button class="chip${x.on ? ' on' : ''}" data-src="${esc(x.key)}">${svg(x.icon)}${esc(x.label)}</button>`).join(''));

  let artSrc = '', artLoading = false, ph = PL_SVG.note;
  let art = svg(ph, 'ph'), title = 'Nothing playing', sub = '', sub2 = '', playing = false, pos = 0, dur = 0;
  let can = {prev: false, next: false, play: false, stop: false};
  if (c.kind === 'phone' && c.p.input) {
    // A broadcast or an LE Audio source: what the broadcaster says it plays (its Program_Info), the
    // codec, and play/stop to receive it or stop (nothing to control on the device).
    const p = c.p, t0 = p.transports[0] || {};
    playing = p.receiving || (p.hfp && !!routeStream());
    title = p.track.title || inputLabel(p);
    sub = p.track.title ? inputLabel(p) : p.hfp ? 'Call audio (HFP)' : '';
    sub2 = [t0.codec ? t0.codec.summary : '', p.also ? 'with ' + p.also.map(devName).join(', ') + ' as the other channel' : ''].filter(Boolean).join(' · ');
    ph = PL_SVG.bt;
    art = svg(ph, 'ph');
    can = {prev: false, next: false, play: p.broadcast, stop: p.broadcast && p.status !== 'not received'};
  } else if (c.kind === 'phone') {
    const p = c.p, t = p.track || {};
    playing = p.status === 'playing';
    title = t.title || 'Nothing playing';
    sub = [t.artist, t.album].filter(Boolean).join(' — ');
    sub2 = p.name ? `in ${p.name}` : '';
    pos = playerPosition(p);
    dur = t.duration_ms || 0;
    if (p.obex_port != null && t.img_handle) {
      // The track in the address too: a phone hands out the same handles again on its next
      // connection, for other images.
      const base = `/api/media/players/cover?path=${encodeURIComponent(p.path)}&h=${encodeURIComponent(t.img_handle)}` +
        `&t=${encodeURIComponent([t.title, t.album].join('|'))}`;
      artSrc = base + (pl.artTries[base] ? '&r=' + pl.artTries[base] : '');
    } else if (p.obex_port != null) {
      coverConnect(p, renderNowPlaying);
      // A cover's handle comes with the track info once the image channel is open. Loading while
      // it opens, and for a few seconds after for a track that came before it (a phone sends the
      // handle a little later); a track that came while it was open and has none has no cover,
      // and neither has any while it cannot be opened.
      const k = p.path + '|' + (t.title || '');
      if (!pl.artWait[k]) pl.artWait[k] = Date.now();
      const ch = au.cover[p.path] || {state: 'asking'};
      artLoading = ch.state === 'asking' ? Date.now() - pl.artWait[k] < 12000
        : ch.state === 'open' && pl.artWait[k] < ch.at && Date.now() - ch.at < 5000;
      if (artLoading && !pl.artTimer) pl.artTimer = setTimeout(() => { pl.artTimer = null; renderNowPlaying(); }, 1000);
    }
    can = {prev: true, next: true, play: true, stop: true};
  } else if (c.kind === 'board' && isUrl(c.s)) {
    const s = c.s, list = s.source.playlist || [];
    const item = list[s.track] || {};
    playing = !s.paused && s.state === 'running';
    title = s.meta.title || item.title || s.source.title || 'Internet radio';
    sub = list.length > 1 ? [item.artist, item.album, `track ${s.track + 1} of ${list.length}`].filter(Boolean).join(' — ')
      : (s.meta.name || s.source.title || '');
    // A station: what it says about itself, and the cover (or its logo) it sends with the title.
    sub2 = list.length > 1 || s.meta.description === sub ? '' : s.meta.description || '';
    pos = (s.track_seconds || 0) * 1000;
    dur = item.duration_ms || (item.duration_s || 0) * 1000;
    if (list.length <= 1) ph = PL_SVG.radio;
    artSrc = item.art || (list.length > 1 ? '' : s.meta.art || '');
    art = svg(ph, 'ph');
    // Repeating all, the first track's previous is the last, and the last one's next the first.
    const round = s.source.repeat === 'all' || s.source.shuffle;
    can = {prev: round ? list.length > 1 : s.track > 0, next: round ? list.length > 1 : s.track + 1 < list.length, play: true, stop: true};
  } else if (c.kind === 'board') {
    // A test signal pauses and plays on like a station; a capture (a device's audio the board
    // plays on) has no pause. Neither has tracks.
    const s = c.s, src = s.source;
    playing = s.state === 'running' && !s.paused;
    title = streamTitle(s);
    sub = TEST_SIGNALS.includes(src.type)
      ? [src.type === 'silence' ? '' : `${src.level_db} dBFS ${src.type === 'noise' ? 'RMS' : 'peak'}`,
        src.type === 'sweep' ? `${src.seconds} s${src.log ? ', logarithmic' : ''}${src.repeat ? ', repeating' : ''}` : ''].filter(Boolean).join(' · ')
      : src.monitor ? '' : src.target || 'the default source';
    sub2 = `${s.rate / 1000} kHz · ${s.channels === 1 ? 'mono' : 'stereo'} · ${fmtMs(s.seconds * 1000)}`;
    ph = streamKind(s).icon;
    art = svg(ph, 'ph');
    can = {prev: false, next: false, play: src.type !== 'capture', stop: true};
  }
  // An image: loading until it arrived (or failed: then the note stays).
  if (artSrc) {
    art = svg(ph, 'ph') + `<img alt="" src="${esc(artSrc)}" onload="npArtDone(this, true)" onerror="npArtDone(this, false)"${pl.artDone[artSrc] === true ? '' : ' hidden'}>`;
    artLoading = pl.artDone[artSrc] === undefined;
  }
  // Until /media answers, "Nothing playing" would be a guess: the cover and the title wait instead.
  const waiting = c.kind === 'none' && !pl.media && !pl.mediaFailed;
  setHTML($('np-art'), art);
  $('np-art').classList.toggle('loading', artLoading || waiting);
  $('np-title').classList.toggle('wait', waiting);
  setText($('np-title'), waiting ? '' : title);
  setText($('np-sub'), sub);
  setText($('np-sub2'), sub2);
  $('np').classList.toggle('idle', c.kind === 'none');
  $('np-bar').parentElement.hidden = c.kind === 'none';
  const btn = (a, d, on, cls) => `<button class="np-btn ${cls || ''}" data-np="${a}" title="${a}" aria-label="${a}"${on ? '' : ' disabled'}>${svg(d)}</button>`;
  // Play and pause both, all the buttons alike, and the state said in words: a phone reports its
  // status late or not at all, and buttons that change with it then offer the wrong one.
  const status = c.kind === 'phone' ? (c.p.status || 'unknown') : c.kind === 'board' ? (c.s.paused ? 'paused' : c.s.state === 'running' ? 'playing' : c.s.state) : '';
  // Two rows, so the buttons keep their width on a phone: the transport, then repeat, shuffle and
  // the state.
  setHTML($('np-ctl'), '<div class="np-row">' + btn('previous', PL_SVG.previous, can.prev) +
    btn('play', PL_SVG.play, can.play) + btn('pause', PL_SVG.pause, can.play) +
    btn('next', PL_SVG.next, can.next) + btn('stop', PL_SVG.stop, can.stop) + '</div><div class="np-row">' +
    (c.kind === 'phone' && !c.p.input ? modeButtons('np-btn', btRepeat(c.p.repeat), c.p.shuffle == null ? null : c.p.shuffle !== 'off')
      : c.kind === 'board' && isUrl(c.s) && (c.s.source.playlist || []).length > 1 ? modeButtons('np-btn', boardRepeat(c.s.source.repeat), !!c.s.source.shuffle) : '') +
    (status ? pill(status, playing ? 'good' : status === 'paused' ? 'warn' : '') : '') + '</div>');
  // The bar under Inputs, Library and Up next: the track, and play/pause and next, to skip without
  // leaving the list.
  $('np-mini').hidden = c.kind === 'none';
  setText($('np-mini-title'), title);
  setText($('np-mini-sub'), sub || sub2);
  setHTML($('np-mini-ctl'), btn(playing ? 'pause' : 'play', playing ? PL_SVG.pause : PL_SVG.play, can.play) + btn('next', PL_SVG.next, can.next));
  setHTML($('np-from'), npFrom(c));
  renderTime(pos, dur);
  renderVolume(c);
  setMediaSession();
  renderOuts();
  renderQueue(c);
  if (currentTab === 'player') renderInputs();
}

// Which way the music flows, said above the title the same way for both: into the board from a
// Bluetooth device (and whether its audio actually streams in, or only its player is known), or out
// of the board from a station, a media server's tracks or a test signal; then the path, from the
// source through the board to where it plays.
function npFrom(c) {
  const tag = (icon, cls, text) => `<span class="np-tag ${cls}">${svg(icon)}${text}</span>`;
  const flow = parts => `<span class="np-flow">${parts.map(esc).join(svg(PL_SVG.arrow))}</span>`;
  const lower = n => n.replace(/^The /, 'the ');
  const outs = s => (s ? streamOuts(s).map(v => lower(outName(v))) : []).concat(s && pl.hearId === s.id ? ['this browser'] : []);
  if (c.kind === 'phone') {
    const ts = c.p.input ? c.p.transports : (pl.media ? pl.media.transports : []).filter(t => t.address === c.p.address && /^(110b|2bc9)$/i.test(t.uuid));
    const on = ts.some(t => t.state === 'active' || t.state === 'broadcasting');
    const s = routeStream(), inp = pl.route.input;
    // What the board does with it: records it (the capture) and plays that on the route's outputs.
    const to = routeOuts().map(v => lower(outName(v))).concat(s && pl.hearId === s.id ? ['this browser'] : []);
    const bc = c.p.input && c.p.broadcast;
    const state = inp.state === 'failed' ? inp.error
      : inp.state === 'waiting' ? (inp.error ? 'stopped sending: play brings it back' : bc ? 'syncing to it…' : 'waiting for its audio…')
      : on || s ? (bc ? 'received by the board' : 'streaming into the board')
      : bc ? (c.p.status === 'syncing' ? 'syncing to it…' : 'in range: play receives it')
      : ts.length ? 'connected, not streaming' : 'remote control only, no audio link';
    const cls = inp.state === 'failed' ? 'bad-text' : on || s ? 'good-text' : '';
    return tag(PL_SVG.into, 'bt', bc ? 'Receiving · Auracast' : c.p.hfp ? 'Receiving · Call audio' : 'Receiving · Bluetooth') +
      `<span class="np-state ${cls}">${esc(state)}</span>` +
      flow([inputLabel(c.p) + (c.p.also ? ' + ' + c.p.also.map(devName).join(' + ') : ''), 'this board', to.length ? to.join(' + ') : 'no output']);
  }
  if (c.kind === 'board') {
    const s = c.s, to = outs(s);
    return tag(PL_SVG.outof, 'local', `Playing from the board · ${streamKind(s).what}`) +
      flow(['this board', to.length ? to.join(' + ') : 'no output']);
  }
  return '';
}

// Repeat or shuffle pressed: the phone's player's setting, or the board's playlist's.
function npMode(mode) {
  const c = current();
  if (c.kind === 'phone' && !c.p.input) {
    const p = c.p;
    const body = mode === 'repeat' ? {repeat: btRepeatValue(nextRepeat(btRepeat(p.repeat)))} : {shuffle: p.shuffle === 'off' ? 'alltracks' : 'off'};
    return put('/media/players', Object.assign({path: p.path}, body)).catch(fail);
  }
  if (c.kind === 'board') {
    const src = c.s.source;
    const body = mode === 'repeat' ? {repeat: boardRepeatValue(nextRepeat(boardRepeat(src.repeat)))} : {shuffle: !src.shuffle};
    return put(`/audio/streams/${c.s.id}`, body).then(refreshPlayerStreams).catch(fail);
  }
}

function renderTime(pos, dur) {
  $('np-bar').style.width = dur ? (Math.min(1, pos / dur) * 100).toFixed(2) + '%' : '0%';
  $('np-bar').parentElement.classList.toggle('live', !dur);
  setText($('np-pos'), pos ? fmtMs(pos) : '');
  setText($('np-dur'), dur ? fmtMs(dur) : '');
}

// The volume a person means: the speaker's own (AVRCP absolute volume of the link) when there is
// one, else the stream's gain — and the gain for a stream on several outputs, which moves them all;
// in this browser, the browser's.
function volumeTarget(c) {
  const ts = pl.media ? pl.media.transports : [];
  if (c.kind === 'phone') {
    const t = ts.find(x => x.path === c.p.transport);
    return t && t.volume != null ? {kind: 'transport', path: t.path, max: transportVolMax(t), value: Math.round(t.volume / transportVolMax(t) * 100)} : null;
  }
  if (c.kind === 'board') {
    const s = c.s;
    const outs = streamOuts(s);
    if (!outs.length) return pl.hearId === s.id ? {kind: 'browser', value: Math.round(npAudio.volume * 100)} : null;
    const ep = outs.length === 1 && outEndpoint(outs[0]);
    // A device can have a transport each way (A2DP sink and source): the one the board sends on is
    // its "Audio Source" side — the other is what the device would send the board.
    const same = x => ep && ep.address && x.address && x.address.toUpperCase() === ep.address.toUpperCase() && x.volume != null;
    const t = ts.find(x => same(x) && /source/i.test(x.profile || '')) || ts.find(same);
    if (t) return {kind: 'transport', path: t.path, max: transportVolMax(t), value: Math.round(t.volume / transportVolMax(t) * 100)};
    return {kind: 'gain', id: s.id, value: Math.round(Math.pow(10, s.gain_db / 40) * 100)};
  }
  return null;
}

function renderVolume(c) {
  const v = volumeTarget(c);
  pl.vol = v;
  $('np-vol-row').hidden = !v;
  if (v && idle($('np-vol'))) $('np-vol').value = Math.min(100, v.value);
}

function setVolume(value) {
  const v = pl.vol;
  if (!v) return;
  if (v.kind === 'transport') put('/media/transport', {path: v.path, volume: Math.round(value / 100 * v.max)}).catch(fail);
  else if (v.kind === 'browser') npAudio.volume = value / 100;
  // 0..100 as -60..0 dB on a square-law curve: the slider's middle is about half as loud.
  else put(`/audio/streams/${v.id}`, {gain_db: value <= 0 ? -60 : Math.max(-60, 40 * Math.log10(value / 100))}).catch(fail);
}

function npAction(a) {
  const c = current();
  if (c.kind === 'phone' && c.p.input) {
    if (!c.p.broadcast) return null;
    // Play receives the broadcast again and records it anew (the route does not by itself); stop
    // lets it go, and the board's input is then nothing.
    if (a === 'play') return receiveBroadcast(c.p, true).then(() => pickInput(pl.route.input));
    return receiveBroadcast(c.p, false).then(() => pickInput({type: 'none'}));
  }
  if (c.kind === 'phone') return playerAction(c.p.path, a);
  if (c.kind !== 'board') return;
  const s = c.s;
  if (a === 'stop') {
    if (pl.hearId === s.id) hearStop();
    return del(`/audio/streams/${s.id}`).then(refreshPlayerStreams).catch(fail);
  }
  if (s.source.type === 'capture' || (!isUrl(s) && a !== 'play' && a !== 'pause')) return;  // tracks: a URL stream's
  const body = a === 'play' ? {paused: false} : a === 'pause' ? {paused: true} : {[a]: true};
  // Heard here: the element pauses with the board, and plays again from a fresh connection (what
  // it buffered while paused is stale silence).
  if (pl.hearId === s.id) {
    if (a === 'pause') npAudio.pause();
    if (a === 'play') hearConnect();
  }
  put(`/audio/streams/${s.id}`, body).catch(fail);
}

// A device in words: its name, and for a broadcast what it is.
function inputLabel(p) {
  const name = p.device_name || devName(p.address);
  return p.input && p.broadcast ? 'Auracast · ' + name : name;
}
// AVRCP's absolute volume is 0..127, LE Audio's (the Volume Control Service) 0..255.
const transportVolMax = t => /^110[ab]$/i.test(t.uuid) ? 127 : 255;

// A broadcast received or let go: every BIS of it (Select on each, as Audio → LE Audio does), with
// the Broadcast Code typed there if it is encrypted. Resolves once BlueZ answered.
function receiveBroadcast(p, on) {
  const codes = typeof la !== 'undefined' ? la.codes : {};
  // All its BISes in one request, linked: a BIG is synced once, with the BISes selected by then.
  const [first, ...rest] = p.transports.map(t => t.path);
  if (!first) return Promise.resolve();
  return put('/le-audio/streams', {path: first, links: rest, receive: on, code: codes[first] || ''})
    .catch(fail).finally(() => { api('/media').then(m => { pl.media = m; renderNowPlaying(); }).catch(() => {}); });
}

// ---------------------------------------------------------------- hearing it in this browser

// What this view plays in the browser goes through one <audio> element, not Web Audio: a media
// element keeps playing with the page in the background (Android stops a page's scripts, which
// feed Web Audio, seconds after the home button), it buffers a few seconds against Wi-Fi hiccups,
// and with the Media Session it puts title, art and the transport keys on the lock screen. The
// bench views keep Web Audio, for its low latency and the spectrum.
const npAudio = new Audio();
npAudio.preload = 'none';
pl.hearId = null;

function hear(id) {
  pl.hearId = id;
  hearConnect();
  setMediaSession();
}

function hearConnect() {
  if (!pl.hearId) return;
  pl.heardAt = Date.now();
  npAudio.src = `/api/audio/streams/${pl.hearId}/listen?c=${Date.now()}`;
  npAudio.play().catch(e => toast('This browser would not play: ' + e.message + ' (tap play)'));
}

function hearStop() {
  pl.hearId = null;
  npAudio.pause();
  npAudio.removeAttribute('src');
  npAudio.load();
  if ('mediaSession' in navigator) navigator.mediaSession.playbackState = 'none';
}

// The connection dropped (the board restarted the stream, Wi-Fi went away) or the page was in the
// background long enough for the browser to let it go: connect again if the stream still plays.
function hearCheck() {
  if (!pl.hearId) return;
  const s = pl.streams.find(x => x.id === pl.hearId);
  if (!s || (s.state !== 'running' && s.state !== 'starting')) { hearStop(); renderNowPlaying(); return; }
  if (s.paused) return;
  const stuck = npAudio.paused || npAudio.ended || npAudio.error || npAudio.readyState < 2;
  if (stuck && Date.now() - pl.heardAt > 4000) hearConnect();
}
['error', 'ended', 'stalled'].forEach(ev => npAudio.addEventListener(ev, () => setTimeout(hearCheck, 1000)));
// "This browser is connecting…" turns into playing.
['playing', 'waiting'].forEach(ev => npAudio.addEventListener(ev, () => { if (currentTab === 'player') renderNowPlaying(); }));
// Back from the background — shown again, thawed after the browser froze the page (Page
// Lifecycle "resume"), restored from the back/forward cache, or focused: look at once.
const hearBack = () => { pl.heardAt = 0; hearCheck(); };
document.addEventListener('visibilitychange', () => { if (!document.hidden) hearBack(); });
document.addEventListener('resume', hearBack);
window.addEventListener('pageshow', hearBack);
window.addEventListener('focus', hearBack);

// The lock screen and the notification: what plays, and the keys, which act on the board.
function setMediaSession() {
  if (!('mediaSession' in navigator) || !pl.hearId) return;
  const c = current();
  const title = $('np-title').textContent, artist = $('np-sub').textContent;
  const img = $('np-art').querySelector('img');
  const key = title + '|' + artist + '|' + (img ? img.src : '');
  if (pl.msKey !== key) {
    pl.msKey = key;
    navigator.mediaSession.metadata = new MediaMetadata({title, artist, album: 'btbench',
      artwork: img ? [{src: img.src, sizes: '512x512'}] : []});
  }
  const playing = c.kind === 'board' ? !c.s.paused : c.kind === 'phone' && c.p.status === 'playing';
  navigator.mediaSession.playbackState = playing ? 'playing' : 'paused';
}
if ('mediaSession' in navigator) {
  const on = (a, f) => { try { navigator.mediaSession.setActionHandler(a, f); } catch (e) { /* not this browser */ } };
  on('play', () => npAction('play'));
  on('pause', () => npAction('pause'));
  on('stop', () => npAction('stop'));
  on('nexttrack', () => npAction('next'));
  on('previoustrack', () => npAction('previous'));
}

// ---------------------------------------------------------------- the queue

function renderQueue(c) {
  let rows = '', note = '';
  if (c.kind === 'board') {
    const s = c.s, list = s.source.playlist || [];
    // The list in the order it plays (shuffled too: the board shuffles the list itself), each
    // track but the one playing with a remove.
    if (list.length > 1 || streamKind(s).what === 'Media server') {
      rows = list.map((it, i) => {
        const now = i === s.track, d = it.duration_ms || (it.duration_s || 0) * 1000;
        return `<div class="tr${now ? ' now' : ''}" data-track="${i}">
          <span class="tr-n">${now ? svg(PL_SVG.play) : i + 1}</span>
          <span class="tr-t">${esc(it.title || it.url)}${it.artist ? ` <span class="muted">${esc(it.artist)}</span>` : ''}</span>
          ${d ? `<span class="tr-d">${fmtMs(d)}</span>` : ''}
          ${now || it.id == null ? '<span class="icon-btn" aria-hidden="true"></span>' : `<button class="icon-btn" data-q-del="${it.id}" title="Remove from the queue" aria-label="Remove from the queue">${svg(PL_SVG.remove)}</button>`}</div>`;
      }).join('');
      note = `${list.length} track${list.length > 1 ? 's' : ''}${s.source.shuffle ? ', shuffled' : ''}`;
    } else {
      note = isUrl(s) ? 'A radio stream' : streamKind(s).what;
    }
  } else if (c.kind === 'phone') {
    // The phone's now-playing list: read when the track changes, and retried a little later after
    // a failure (a phone answers browsing slowly, or not at all, for a while after it connects).
    // Read again too when its shuffle changes, or it says its list changed (AVRCP's now playing
    // content changed: another board reshuffling or editing its queue does). Not re-read on a
    // timer: every read moves the phone's browsing from the library's folder to Now Playing,
    // which drops the items BlueZ holds for the other, and on some phones (an S25) costs 10 s
    // during which the library waits.
    const key = [c.p.path, (c.p.track || {}).title || '', c.p.shuffle || '', c.p.playlist_changes || 0].join('|');
    const due = pl.queueKey !== key ? 0 : pl.queueOk ? Infinity : pl.queueEmpty === 1 ? 2000 : pl.queueEmpty ? 6000 : 8000;
    if (c.p.browsable && !pl.queueBusy && Date.now() - pl.queueAt > due) {
      pl.queueBusy = true;
      pl.queueAt = Date.now();
      post('/media/players/browse', {path: c.p.path, folder: c.p.path + '/NowPlaying'})
        .then(r => {
          const items = r.items || [];
          // An empty answer right after a track change can be the phone rebuilding its list (AVRCP
          // "UIDs changed": BlueZ drops the old items meanwhile), or the new list not there yet.
          // For a track of a list (it has a number) what is on screen stays, and the phone is
          // asked again 2 s later and 6 s after that; empty a third time, it is empty. A track
          // with no number (a test tone after a playlist) has no list.
          const numbered = ((c.p.track || {}).track_number || 0) > 0;
          pl.queueEmpty = pl.queueKey === key ? pl.queueEmpty || 0 : 0;
          if (!items.length && numbered && pl.queueEmpty < 2) {
            pl.queueKey = key;
            pl.queueEmpty++;
            pl.queueOk = false;
            setTimeout(renderNowPlaying, pl.queueEmpty === 1 ? 2100 : 6100);
            return;
          }
          pl.queueItems = items;
          pl.queuePlayer = c.p.path;
          pl.queueKey = key;
          pl.queueOk = true;
        })
        .catch(() => {
          if (pl.queueKey !== key) pl.queueEmpty = 0;
          pl.queueKey = key;
          pl.queueOk = false;
          setTimeout(renderNowPlaying, 8100);
        })
        .finally(() => { pl.queueBusy = false; renderNowPlaying(); });
    }
    const cur = (c.p.track || {}).title;
    rows = pl.queueItems.map((it, i) => {
      const t = it.title || (it.metadata && it.metadata.title) || it.name;
      const now = t && t === cur;
      return `<div class="tr${now ? ' now' : ''}" data-item="${esc(it.path)}">
        <span class="tr-n">${now ? svg(PL_SVG.play) : i + 1}</span>
        <span class="tr-t">${esc(t)}${it.artist ? ` <span class="muted">${esc(it.artist)}</span>` : ''}</span></div>`;
    }).join('');
    note = c.p.browsable || c.p.input ? '' : 'This player does not share its list';
  }
  setText($('np-q-note'), note);
  $('np-q-head').hidden = !note;
  const waiting = c.kind === 'phone' && c.p.browsable && pl.queueBusy && !pl.queueItems.length;
  setHTML($('np-queue'), rows || (waiting ? '<div class="empty-line loading-line"><span class="dots"></span>Getting the phone’s list…</div>'
    : `<div class="empty-line">${c.kind === 'none' ? 'Nothing playing: pick an input in Inputs, or something in Library.' : 'Nothing queued.'}</div>`));
}

// ---------------------------------------------------------------- inputs

// A device's icon, as in Devices (once the device list is here).
function devSvg(address) {
  const d = btState && btState.devices.find(x => sameAddr(x.address, address));
  return d ? DEV_SVG[devKind(d).kind] : PL_SVG.bt;
}

// Inputs: what the board plays, one at a time — its own music (what the library loaded:
// a station, the queue, a test signal) or one Bluetooth device's audio. A row picked is the route's
// input; what was before goes quiet (a playlist pauses, keeping its queue). A single-channel
// broadcast can be the other channel of the one picked (a left and a right broadcast are one
// stereo input). How the input does — waiting for the device's audio, why it failed — is said in
// its row. A device that stops sending stays the input and plays again when it resumes; one that
// failed or disconnected is not retried by itself, picking it again is the retry.
function renderInputs() {
  const inp = pl.route ? pl.route.input : {type: 'none', devices: [], state: 'idle'};
  const ls = loadedStream();
  const stateText = on => !on ? '' : inp.state === 'waiting' ? (inp.error ? 'not sending' : 'waiting for its audio…') : inp.state === 'failed' ? 'failed' : 'the input';
  const row = (attrs, on, icon, title, sub, state, extra) => `<div class="tr lib in-row${on ? ' now' : ''}" ${attrs}>
      <span class="tr-n tr-art">${svg(icon)}</span>
      <span class="tr-t">${esc(title)}${sub ? `<span class="muted">${esc(sub)}</span>` : ''}</span>
      ${state ? `<span class="tr-fmt${on && inp.state === 'failed' ? ' bad-text' : ''}">${esc(state)}</span>` : ''}${extra || ''}</div>`;
  const err = on => on && inp.state === 'failed' && inp.error ? `<div class="empty-line bad-text in-err">${esc(inp.error)}</div>` : '';
  const local = inp.type === 'local';
  let html = '<div class="in-head small muted">This board</div>' +
    row('data-in="local"', local, ls ? streamKind(ls).icon : PL_SVG.note, ls ? streamTitle(ls) : 'Nothing loaded',
      ls ? streamKind(ls).what + (ls.paused && !local ? ', paused' : '') : 'pick a station, tracks or a test signal in the library',
      local ? 'the input' : '');
  const rows = btInputs();
  const pairing = inp.type === 'bluetooth' && inp.devices.length === 1 && inp.devices[0].kind === 'broadcast'
    ? rows.find(r => r.kind === 'broadcast' && sameAddr(r.address, inp.devices[0].address)) : null;
  const rowHTML = (r, i) => {
    const on = isRouted(r);
    const sub = [KIND_WORDS[r.kind], r.codec, r.title, r.members && r.members.length > 1 ? `${r.members.length} devices` : '',
      r.side ? (r.side < 0 ? 'left channel' : 'right channel') : '',
      r.players.length ? r.players.map(p => p.name || (isMcp(p) ? 'media control' : 'player')).join(', ') : ''].filter(Boolean).join(' · ');
    // The other channel of the broadcast picked: one of the opposite side.
    const pair = pairing && !on && r.kind === 'broadcast' && r.side && pairing.side && r.side === -pairing.side
      ? `<button class="small-btn" data-in-pair="${i}" title="Play it as the ${r.side < 0 ? 'left' : 'right'} channel of the input">+ as ${r.side < 0 ? 'left' : 'right'}</button>` : '';
    return row(`data-in="${i}"`, on, r.kind === 'broadcast' ? PL_SVG.bt : r.kind === 'hfp' ? PL_SVG.phone : devSvg(r.address),
      r.name, sub, on ? stateText(on) : r.state, pair) + err(on);
  };
  const some = bc => rows.map((r, i) => (r.kind === 'broadcast') === bc ? rowHTML(r, i) : '').join('');
  const rx = rxAdapter();
  html += '<div class="in-head small muted">Bluetooth</div>' + (some(false) ||
    `<div class="empty-line">No Bluetooth device sends the board audio: connect one in Devices (the board as its speaker)${rx ? '' : ', or scan for broadcasts in Audio → LE Audio'}.</div>`);
  if (rx) {
    // Broadcasters heard whose streams BlueZ has not read yet (it reads the BASE while it scans):
    // listed, not pickable until they have streams.
    const reading = (pl.la.sources || []).filter(x => !rows.some(r => r.kind === 'broadcast' && sameAddr(r.address, x.address)))
      .map(x => row('', false, PL_SVG.bt, x.name || devName(x.address), 'Auracast', 'reading its streams…')).join('');
    html += `<div class="in-head small muted in-bc-head">Auracast<button class="small-btn" data-in-scan>${rx.discovering ? 'Stop scan' : 'Scan'}</button></div>` +
      (some(true) + reading || `<div class="empty-line">${rx.discovering ? 'Scanning for Auracast broadcasts…' : 'No broadcast found yet: scan for the ones in range.'}</div>`);
  }
  setHTML($('np-inputs'), html);
  pl.inRows = rows;
}

// An input picked. A broadcast not received yet is received first (its node comes once the board
// synced to it); a phone that is not playing is asked once to play, never paused later.
function pickInput(input, r) {
  const pre = [];
  if (r && r.kind === 'broadcast' && r.state === 'in range') pre.push(receiveBroadcast({transports: r.transports}, true));
  if (r && r.players.length && !r.players.some(p => p.status === 'playing')) {
    const p = r.players.find(x => x.path === pl.players[r.address.toUpperCase()]) || r.players[0];
    post('/media/players/control', {path: p.path, action: 'play'}).catch(() => {});
  }
  pl.sw = null;
  // An output the new input rules out (the device itself, a broadcast while one is received) is
  // taken off with it.
  const kind = pl.endpoints && pl.endpoints.backend === 'alsa' ? 'alsa' : 'pipewire';
  const ruled = v => { const e = outEndpoint(v); return e && v.startsWith(kind + ':') && outBlocked(e, input); };
  const outs = routeOuts(), keep = outs.filter(v => !ruled(v));
  const body = keep.length === outs.length ? {input} : {input, outputs: outSinks(keep)};
  return Promise.all(pre).then(() => put('/audio/route', body)).then(setRoute).catch(fail);
}

function inputClick(e) {
  if (e.target.closest('[data-in-scan]')) { broadcastScan(); return true; }
  const pair = e.target.closest('[data-in-pair]');
  if (pair) {
    const r = pl.inRows[parseInt(pair.dataset.inPair, 10)];
    if (r) pickInput({type: 'bluetooth', devices: pl.route.input.devices.concat([{address: r.address, kind: 'broadcast'}])}, r);
    return true;
  }
  const el = e.target.closest('[data-in]');
  if (!el) return false;
  if (el.dataset.in === 'local') {
    pickInput({type: 'local'});
    return true;
  }
  const r = pl.inRows[parseInt(el.dataset.in, 10)];
  if (r) pickInput({type: 'bluetooth', devices: [{address: r.address, kind: r.kind}]}, r);
  return true;
}

// The adapter that can receive broadcasts, or null: Inputs then has an Auracast section.
const rxAdapter = () => (pl.la ? pl.la.adapters : []).find(a => a.receiver) || null;
// Scan for broadcasts (an LE scan of 30 s on that adapter, as Audio → LE Audio does), or stop it.
function broadcastScan() {
  const a = rxAdapter();
  if (!a) return;
  const on = !a.discovering;
  post('/bluetooth/scan', on ? {on: true, adapter: a.adapter, transport: 'le', seconds: 30} : {on: false, adapter: a.adapter})
    .then(refreshLa).catch(fail);
}

// ---------------------------------------------------------------- the library

// The tabs: the library's sources, a browsable phone's first. With none picked, the first.
function renderLibTabs() {
  const tabs = [['radio', 'Radio'], ['upnp', 'Media servers'], ['signals', 'Test signals']];
  if (phones().some(p => p.browsable)) tabs.unshift(['phone', 'Phone']);
  if (!tabs.some(t => t[0] === pl.lib)) pl.lib = tabs[0][0];
  setHTML($('np-lib-tabs'), tabs.map(([k, l]) => `<button class="seg${pl.lib === k ? ' on' : ''}" data-lib="${k}">${l}</button>`).join(''));
  $('np-search-row').hidden = !(pl.lib === 'phone' && phones().some(p => p.searchable));
}

function openLib(kind, chosen) {
  const phone = kind === 'phone' && phones().find(x => x.browsable);
  // The phone's top folder already on its way (the tab shown twice while the phone is slow): wait
  // for that answer rather than queueing a second request behind it.
  if (phone && pl.lib === kind && pl.libLoading === phone.path + '/Filesystem') return;
  // Whatever the library was loading is no longer wanted (see phoneFolder).
  pl.libSeq++;
  pl.libLoading = '';
  pl.lib = kind;
  if (chosen) lsSet('btbench.pl.lib', kind);  // only a tab picked by hand is remembered
  libDropBack(0);
  pl.libPath = [];
  renderLibTabs();
  kind = pl.lib;  // a tab gone (an old Inputs or Up next remembered, a phone left): the first one
  if (kind === 'radio') {
    setHTML($('np-crumbs'), '');
    api('/audio/radio').then(renderStations)
      .catch(e => setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`));
  } else if (kind === 'upnp') {
    pl.upnpServer = null;
    upnpServers(false);
  } else if (kind === 'signals') {
    renderSignals();
  } else if (phone) {
    phoneFolder(phone, phone.path + '/Filesystem', 'Files');
  }
}

// The stations, each with a remove, and under them a station to add by its URL: the board names it
// (what the stream calls itself). The board keeps the whole list (PUT /audio/radio), the one
// Audio's "Edit stations" edits too.
function renderStations(r) {
  pl.libItems = r.stations.map(s => ({kind: 'station', title: s.name, url: s.url}));
  renderLib('<div class="empty-line">No stations yet.</div>', `<form class="radio-add row" id="np-radio-add">
    <input type="url" name="url" class="grow" placeholder="Add a station: its stream URL (MP3, AAC, .pls, .m3u)" required>
    <button class="small-btn">${svg(PL_SVG.plus)} Add</button></form>`);
}

// The list as the board has it now, changed by `edit`, saved, and shown.
function stationsSave(edit) {
  return api('/audio/radio').then(r => put('/audio/radio', {stations: edit(r.stations)}))
    .then(r => { if (pl.lib === 'radio') renderStations(r); return r; });
}

function stationAdd(f) {
  const url = f.url.value.trim();
  const btn = f.querySelector('button');
  if (!url || btn.disabled) return;
  btn.disabled = true;
  stationsSave(list => list.concat([{url}]))
    .then(r => { const s = r.stations[r.stations.length - 1]; toast(`Added ${s ? s.name : url}`); })
    .catch(e => { btn.disabled = false; fail(e); });
}

function stationRemove(i) {
  const it = pl.libItems[i];
  if (!it || it.kind !== 'station' || !confirm(`Remove ${it.title}?`)) return;
  stationsSave(list => list.filter(s => !(s.url === it.url && s.name === it.title))).catch(fail);
}

// ---- Back in the library: each folder opened is a layer (app.js backOpen), and Back shows the
// list it was opened from as it was left — its rows, the pages already loaded, where it was
// scrolled to — without asking the server again. The top of a library (a phone's root folder, the
// list of media servers) is no layer: Back from there goes to the view before.

// The element the library scrolls in: the page on a phone, a card or the pane in fit mode.
function libScroller() {
  for (let e = $('np-lib'); e && e !== document.body; e = e.parentElement) {
    const o = getComputedStyle(e).overflowY;
    if ((o === 'auto' || o === 'scroll') && e.scrollHeight > e.clientHeight) return e;
  }
  return document.scrollingElement;
}

function libSnapshot() {
  return {items: pl.libItems, more: pl.libMore, lib: pl.lib, server: pl.upnpServer, scroll: libScroller().scrollTop};
}

// Back to the list `entry` was opened from (its parent's snapshot).
function libRestore(entry) {
  const i = pl.libPath.indexOf(entry);
  if (i < 0 || pl.lib !== entry.parent.lib) return;
  const snap = entry.parent;
  pl.libPath = pl.libPath.slice(0, i);
  pl.libSeq++;
  pl.libLoading = '';
  pl.upnpServer = snap.server;
  pl.libItems = snap.items;
  pl.libMore = snap.more && Object.assign({}, snap.more, {seq: pl.libSeq, busy: false});
  renderCrumbs();
  renderLib();
  libScroller().scrollTop = snap.scroll;
}

// A folder opened from what is shown: remember that, and give Back a way to it.
function libDescend(entry, layered) {
  entry.parent = libSnapshot();
  pl.libPath.push(entry);
  if (layered) entry.back = backOpen(() => { entry.back = null; libRestore(entry); });
}

// The library leaves the folders from `from` on by itself (a breadcrumb, another library, a
// search): their history entries go too.
function libDropBack(from) {
  const first = pl.libPath.slice(from).find(c => c.back);
  if (!first) return;
  const layer = first.back;
  pl.libPath.slice(from).forEach(c => { c.back = null; });
  backClose(layer);
}

function upnpServers(search) {
  pl.libMore = null;
  setHTML($('np-crumbs'), '');
  setHTML($('np-lib'), `<div class="empty-line loading-line"><span class="dots"></span>${search ? 'Searching the network…' : 'Loading…'}</div>`);
  api('/audio/upnp/servers' + (search ? '?search=1' : '')).then(r => {
    // None known yet (the board only remembers what a search found): search now.
    if (!search && !r.servers.length) return upnpServers(true);
    pl.libItems = r.servers.map(s => ({kind: 'server', title: s.name, id: s.id, thumb: s.icon,
      sub: [s.manufacturer, s.model].filter(Boolean).join(' ')}));
    renderLib(r.servers.length ? '' : `<div class="empty-line">No media servers found. <button class="small-btn" data-lib-search="1">Search the network</button></div>`);
  }).catch(e => setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`));
}

// What a media server says about a folder's contents, as library rows: albums with their cover,
// artist and year; tracks with their number, cover and format. A track without a cover of its own
// takes its album's (meta: the folder opened, as its row had it).
//
// A big folder (Kodi's artists, thousands of them) comes a page at a time: the first page shows at
// once, and the next is asked for as the end of the list scrolls into view. One page of 200 keeps
// each answer quick for the board to parse and small on its Wi-Fi, which shares the radio with the
// Bluetooth that may be playing.
const UPNP_PAGE = 200;

function upnpRows(r, folderArt) {
  return r.containers.map(c => {
    const album = /album/.test(c.class || '');
    const n = c.child_count != null ? `${c.child_count} ${album ? 'tracks' : 'items'}` : '';
    return {kind: 'folder', title: c.title, id: c.id, album, thumb: c.thumb, art: c.art, artist: c.artist, year: c.year,
      genre: c.genre, count: c.child_count, sub: (album ? [c.artist, c.year, n] : [n]).filter(Boolean).join(' · ')};
  }).concat(r.items.filter(it => it.url).map(it => ({kind: 'track', title: it.title, url: it.url,
    art: it.art || folderArt, thumb: it.thumb || '', track: it.track, year: it.year, genre: it.genre, rate: it.rate,
    sub: [it.artist, it.album].filter(Boolean).join(' — '), duration_s: it.duration_s, artist: it.artist, album: it.album,
    format: upnpFormat(it)})));
}

function upnpFolder(server, id, title, push, meta) {
  const seq = ++pl.libSeq;
  pl.upnpServer = server;
  if (push !== false) libDescend({id, title, meta: meta || null}, true);
  const here = pl.libPath[pl.libPath.length - 1];
  const folderArt = here && here.meta ? here.meta.art : '';
  renderCrumbs();
  setHTML($('np-lib'), '<div class="empty-line loading-line"><span class="dots"></span>Loading…</div>');
  pl.libMore = null;
  upnpPage(server, id, 0, folderArt, seq);
}

// One page of the folder from `start`; what is left is in pl.libMore until the list's end shows.
// A server that leaves TotalMatches at 0 (allowed when it does not know) has more while it fills
// whole pages.
function upnpPage(server, id, start, folderArt, seq) {
  return api(`/audio/upnp/browse?server=${encodeURIComponent(server)}&object=${encodeURIComponent(id)}&start=${start}&count=${UPNP_PAGE}`).then(r => {
    if (seq !== pl.libSeq) return;
    const rows = upnpRows(r, folderArt);
    const next = start + (r.returned || 0);
    const more = r.returned > 0 && (r.total ? next < r.total : r.returned >= UPNP_PAGE);
    pl.libMore = more ? {server, id, next, total: r.total || 0, folderArt, seq, busy: false} : null;
    if (start === 0) {
      pl.libItems = rows;
      renderLib();
    } else {
      libAppend(rows);
    }
  }).catch(e => {
    if (seq !== pl.libSeq) return;
    if (start === 0) return setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`);
    pl.libMore = {server, id, next: start, total: 0, folderArt, seq, busy: false, failed: e.message};
    libMoreLine();
  });
}

// The list's last line while more is to come: it asks for the next page when it scrolls into
// view (an IntersectionObserver, whichever element scrolls), or on a tap after a failure.
let libMoreSeen = null;
function libMoreLine() {
  const lib = $('np-lib');
  const old = lib.querySelector('.lib-more');
  if (old) old.remove();
  if (libMoreSeen) { libMoreSeen.disconnect(); libMoreSeen = null; }
  const m = pl.libMore;
  if (!m || m.seq !== pl.libSeq) return;
  const of = m.total ? ` of ${m.total.toLocaleString()}` : '';
  lib.insertAdjacentHTML('beforeend', m.failed
    ? `<div class="empty-line lib-more"><span class="bad-text">${esc(m.failed)}</span> <button class="small-btn" data-lib-more="1">Try again</button></div>`
    : `<div class="empty-line loading-line lib-more"><span class="dots"></span>${pl.libItems.length.toLocaleString()}${of} shown, loading more…</div>`);
  if (m.failed || !('IntersectionObserver' in window)) return;
  libMoreSeen = new IntersectionObserver(es => { if (es.some(e => e.isIntersecting)) libMoreLoad(); }, {rootMargin: '600px'});
  libMoreSeen.observe(lib.querySelector('.lib-more'));
}

function libMoreLoad() {
  const m = pl.libMore;
  if (!m || m.busy || m.seq !== pl.libSeq) return;
  m.busy = true;
  m.failed = '';
  upnpPage(m.server, m.id, m.next, m.folderArt, m.seq);
}

// "FLAC 24/96": the codec from the MIME type, then bits and kHz where the server gave them.
function upnpFormat(it) {
  const m = (it.mime || '').toLowerCase();
  const codec = /flac/.test(m) ? 'FLAC' : /mpeg|mp3/.test(m) ? 'MP3' : /wav/.test(m) ? 'WAV' : /aiff/.test(m) ? 'AIFF'
    : /dsf|dsd|dff/.test(m) ? 'DSD' : /ogg|vorbis|opus/.test(m) ? 'OGG' : /mp4|m4a|aac/.test(m) ? 'AAC/ALAC' : '';
  const q = it.bits && it.rate ? `${it.bits}/${+(it.rate / 1000).toFixed(1)}` : it.rate ? `${+(it.rate / 1000).toFixed(1)} kHz` : '';
  return [codec, q].filter(Boolean).join(' ');
}

// A phone's item as a library row. A folder's name is its path in the phone's tree
// ("/Filesystem/Favorites"): only the last part is shown.
const phoneRow = (i, player) => ({kind: i.type === 'folder' ? 'pfolder' : 'ptrack',
  title: i.title || (i.metadata && i.metadata.title) || (i.name || '').replace(/^.*\//, ''),
  sub: i.artist || (i.metadata && i.metadata.artist) || '', path: i.path, playable: i.playable, player});

function phoneFolder(p, folder, title, push) {
  if (push !== false) libDescend({id: folder, title}, pl.libPath.length > 0);
  renderCrumbs();
  setHTML($('np-lib'), '<div class="empty-line loading-line"><span class="dots"></span>Loading…</div>');
  // Only the last folder asked for is shown: browsing waits its turn on the board, and an earlier
  // answer arriving late must not replace the one opened since.
  const seq = ++pl.libSeq;
  pl.libLoading = folder;
  post('/media/players/browse', {path: p.path, folder}).then(r => {
    if (seq !== pl.libSeq) return;
    pl.libItems = (r.items || []).map(i => phoneRow(i, p.path));
    renderLib();
  }).catch(e => { if (seq === pl.libSeq) setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`); })
    .finally(() => { if (seq === pl.libSeq) pl.libLoading = ''; });
}

function renderCrumbs() {
  const root = pl.lib === 'upnp' ? '<a href="#" data-crumb="-1">Servers</a>' : '';
  setHTML($('np-crumbs'), root + pl.libPath.map((c, i) => `${root || i ? ' / ' : ''}<a href="#" data-crumb="${i}">${esc(c.title)}</a>`).join(''));
}

// A cover over the row's icon: the icon stays when the image cannot be had (a server on another
// network than this browser, a broken link).
const libArt = (it, icon) => `${svg(icon)}${it.thumb || it.art ? `<img alt="" loading="lazy" decoding="async" src="${esc(it.thumb || it.art)}" onerror="this.remove()">` : ''}`;

// `foot` goes under the rows (the stations' add form).
function renderLib(empty, foot) {
  const items = pl.libItems;
  const tracks = items.filter(x => x.kind === 'track');
  const here = pl.lib === 'upnp' ? pl.libPath[pl.libPath.length - 1] : null;
  const meta = here && here.meta;
  // An album opened: its cover and what it is at the top, the tracks under it by number.
  let head = '';
  if (tracks.length) {
    const total = tracks.reduce((a, t) => a + (t.duration_s || 0), 0);
    const facts = [meta && meta.artist, meta && meta.year, meta && meta.genre,
      `${tracks.length} track${tracks.length > 1 ? 's' : ''}${total ? ', ' + fmtMs(total * 1000) : ''}`].filter(Boolean).join(' · ');
    const cover = meta && (meta.art || meta.thumb) ? `<span class="lib-cover">${libArt({art: meta.art || meta.thumb}, PL_SVG.note)}</span>` : '';
    head = `<div class="lib-head${cover ? ' has-cover' : ''}">${cover}<div class="lib-head-t">${meta ? `<b>${esc(here.title)}</b>` : ''}
      <span class="muted small">${esc(facts)}</span>
      <div class="row"><button class="primary small-btn" data-lib-all="">${svg(PL_SVG.play)} Play all</button>
        <button class="small-btn" data-lib-all="next" title="Every track into the queue, right after what plays">${svg(PL_SVG.playnext)} Play next</button>
        <button class="small-btn" data-lib-all="end" title="Every track at the end of the queue">${svg(PL_SVG.plus)} Add all</button></div></div></div>`;
  }
  const inAlbum = !!(meta && meta.album);
  // A folder of albums with covers is shown as covers.
  const folders = items.filter(x => x.kind === 'folder');
  const grid = !tracks.length && folders.length >= 4 && folders.filter(f => f.album && f.thumb).length >= folders.length / 2;
  // Pages after the first are appended in the same form (libAppend): a grid stays a grid.
  pl.libView = {grid, inAlbum, tracks: tracks.length > 0};
  if (grid) {
    setHTML($('np-lib'), `<div class="lib-grid">${libTiles(items, 0)}</div>`);
  } else {
    setHTML($('np-lib'), head + (libRowsHTML(items, 0, inAlbum) || empty || '<div class="empty-line">Empty.</div>') + (foot || ''));
  }
  libMoreLine();
}

const libIcons = () => ({station: PL_SVG.radio, server: PL_SVG.folder, folder: PL_SVG.folder, pfolder: PL_SVG.folder, track: PL_SVG.note, ptrack: PL_SVG.note,
  signal: PL_SVG.wave});

function libTiles(items, from) {
  const icon = libIcons();
  return items.map((it, k) => `<button class="lib-tile" data-lib-i="${from + k}" title="${esc(it.title)}">
      <span class="lib-tile-art">${libArt(it, icon[it.kind])}</span><b>${esc(it.title)}</b><span class="muted">${esc([it.artist, it.year].filter(Boolean).join(' · '))}</span></button>`).join('');
}

function libRowsHTML(items, from, inAlbum) {
  const icon = libIcons();
  return items.map((it, k) => {
    const i = from + k;
    const open = it.kind === 'server' || it.kind === 'folder' || it.kind === 'pfolder';
    const add = it.kind === 'track' || (it.kind === 'ptrack' && it.playable);
    const btn = (attr, icon, title) => `<button class="icon-btn" ${attr}="${i}" title="${title}" aria-label="${title}">${svg(icon)}</button>`;
    // Inside an album the tracks show their number (the cover is at the top); elsewhere their cover.
    const lead = it.kind === 'track' && inAlbum && it.track != null ? `<span class="tr-n">${it.track}</span>`
      : `<span class="tr-n tr-art">${libArt(it, icon[it.kind])}</span>`;
    return `<div class="tr lib" data-lib-i="${i}">
      ${lead}
      <span class="tr-t">${esc(it.title)}${it.sub && !(inAlbum && it.kind === 'track') ? `<span class="muted">${esc(it.sub)}</span>` : ''}</span>
      ${it.format ? `<span class="tr-fmt">${esc(it.format)}</span>` : ''}
      ${it.duration_s ? `<span class="tr-d">${fmtMs(it.duration_s * 1000)}</span>` : ''}
      ${it.kind === 'folder' ? btn('data-lib-folder', PL_SVG.play, 'Play the whole folder') : ''}
      ${it.kind === 'track' ? btn('data-lib-next', PL_SVG.playnext, 'Play next') : ''}
      ${add ? btn('data-lib-add', PL_SVG.plus, it.kind === 'track' ? 'Add to the end of the queue' : 'Add to the queue') : ''}
      ${it.kind === 'station' ? `<button class="icon-btn" data-lib-del="${i}" title="Remove the station" aria-label="Remove the station">${svg(PL_SVG.remove)}</button>` : ''}
      ${open ? '<span class="chev"></span>' : ''}</div>`;
  }).join('');
}

// The next page under what is shown, without redrawing it: the scroll position and the covers
// already loaded stay. A page that brings the first tracks (or more of an album's) redraws, as the
// album header counts them.
function libAppend(rows) {
  const from = pl.libItems.length;
  pl.libItems = pl.libItems.concat(rows);
  const v = pl.libView || {};
  if (v.tracks || (!v.grid && rows.some(r => r.kind === 'track'))) {
    const lib = $('np-lib'), top = lib.scrollTop, y = window.scrollY;
    renderLib();
    lib.scrollTop = top;
    window.scrollTo(0, y);
    return;
  }
  const lib = $('np-lib');
  const old = lib.querySelector('.lib-more');
  if (old) old.remove();
  if (v.grid) lib.querySelector('.lib-grid').insertAdjacentHTML('beforeend', libTiles(rows, from));
  else lib.insertAdjacentHTML('beforeend', libRowsHTML(rows, from, v.inAlbum));
  libMoreLine();
}

// Play every track of the folder, or put them all in the queue: `add` 'next' right after what
// plays, 'end' at its end. A folder still loading more pages is read to its end first.
function libAll(add) {
  const here = pl.libPath[pl.libPath.length - 1];
  if (pl.libMore && here) return folderPlay(pl.upnpServer, here.id, here.title, (here.meta || {}).art || '', add);
  queueTracks(pl.libItems.filter(x => x.kind === 'track'), add);
}

// Tracks (library rows) played on the board, or into its queue (`add`: 'next' or 'end') while a
// URL stream plays.
function queueTracks(tracks, add) {
  if (!tracks.length) return;
  const items = tracks.map(x => ({url: x.url, title: x.title, art: x.art, duration_s: x.duration_s, artist: x.artist, album: x.album, rate: x.rate}));
  const b = loadedStream();
  if (add && b && isUrl(b)) {
    items.forEach(t => { pl.art[t.url] = t; });
    const what = items.length > 1 ? `${items.length} tracks` : `“${items[0].title}”`;
    return put(`/audio/streams/${b.id}`, {enqueue: items.map(playlistItem), at: add})
      .then(() => { toast(add === 'next' ? `${what} play${items.length > 1 ? '' : 's'} next` : `Added ${what} to the queue`); refreshPlayerStreams(); })
      .catch(fail);
  }
  playOnBoard(items);
}

// Every track of a media server's folder, its subfolders' too (depth first, a few levels down), up
// to what one playlist holds: an artist or a folder of albums plays as one queue. Each track once:
// a server shows the same file under several views (Music → Album, Artist, Genre, All music),
// each with a URL of its own, so a track is known by what it is.
const MAX_PLAYLIST = 500;  // the daemon's kMaxPlaylist
async function folderTracks(server, id, art, out, depth, seen = new Set()) {
  for (let start = 0; out.length < MAX_PLAYLIST;) {
    const r = await api(`/audio/upnp/browse?server=${encodeURIComponent(server)}&object=${encodeURIComponent(id)}&start=${start}&count=${UPNP_PAGE}`);
    const rows = upnpRows(r, art);
    const key = t => [t.title, t.artist, t.album, t.duration_s, t.format].join('|');
    rows.filter(x => x.kind === 'track' && !seen.has(key(x))).slice(0, MAX_PLAYLIST - out.length)
      .forEach(t => { seen.add(key(t)); out.push(t); });
    if (depth < 3) {
      for (const f of rows.filter(x => x.kind === 'folder')) {
        if (out.length >= MAX_PLAYLIST) break;
        await folderTracks(server, f.id, f.art || art, out, depth + 1, seen);
      }
    }
    start += r.returned || 0;
    if (!(r.returned > 0 && (r.total ? start < r.total : r.returned >= UPNP_PAGE))) break;
  }
  return out;
}

function folderPlay(server, id, title, art, add) {
  toast(`Getting the tracks of ${title}…`);
  folderTracks(server, id, art, [], 0)
    .then(ts => ts.length ? queueTracks(ts, add) : toast(`${title} has no tracks to play`))
    .catch(fail);
}

// A library row: open a folder, or play — a station on its own, a track with the rest of its
// folder after it (as the queue), a phone's item on the phone. `add` ('next' or 'end') puts a
// track in the queue instead.
function libClick(i, add) {
  const it = pl.libItems[i];
  if (!it) return;
  if (it.kind === 'server') return upnpFolder(it.id, '0', it.title);
  if (it.kind === 'folder') return upnpFolder(pl.upnpServer, it.id, it.title, true, it);
  if (it.kind === 'pfolder') return phoneFolder(phones().find(p => p.path === it.player), it.path, it.title);
  if (it.kind === 'ptrack') {
    if (!it.playable) return;
    return post('/media/items', {path: it.path, action: add ? 'add' : 'play'})
      .then(() => {
        toast(add ? 'Added to the phone’s queue' : 'Playing on the phone');
        pl.queueAt = 0;
        // Played on the phone, the phone is what the board plays.
        const p = phones().find(x => x.path === it.player);
        if (add || !p) return;
        pl.players[p.address.toUpperCase()] = p.path;
        if (!isRouted({address: p.address, kind: 'a2dp'})) pickInput({type: 'bluetooth', devices: [{address: p.address, kind: isMcp(p) ? 'bap' : 'a2dp'}]});
      }).catch(fail);
  }
  if (it.kind === 'station') return playOnBoard([{url: it.url, title: it.title}]);
  if (it.kind === 'signal') return playSignal(it.source);
  const tracks = pl.libItems.filter(x => x.kind === 'track');
  queueTracks(add ? [it] : tracks.slice(tracks.indexOf(it)), add);
}

// A track as the board's playlist keeps it: what the board's player tells a car or speaker over
// AVRCP (artist, album, length, the cover) goes along.
const playlistItem = it => ({url: it.url, title: it.title, artist: it.artist || '', album: it.album || '',
  duration_ms: it.duration_s ? Math.round(it.duration_s * 1000) : null, art: it.art || ''});

// What the board plays from the library: one stream with the tracks as its playlist, the route's
// input in the place of what was (playLocal). What a UPnP server said of a track is kept here by
// URL too (its length in seconds, for the progress bar).
function playOnBoard(items) {
  items.forEach(it => { pl.art[it.url] = it; });
  playLocal({source: {type: 'url', url: items[0].url, title: items[0].title, playlist: items.map(playlistItem)},
    rate: streamRate(items[0].rate), channels: 2, label: items.length > 1 ? items[0].title + ' and ' + (items.length - 1) + ' more' : items[0].title},
  s => { pl.boardId = s.id; });
}

// The board's own input started (`body` without its outputs): the route plays it on its outputs,
// in the place of whatever the board played of its own and of the device that was the input.
function playLocal(body, started) {
  pl.sw = null;
  if (pl.route && !pl.route.outputs.length && !pl.listen) toast('Nothing is on in “Play to”: turn on where it plays.');
  post('/audio/route/play', body).then(s => {
    if (started) started(s);
    refreshPlayerStreams();
    return refreshRoute();
  }).catch(fail);
}
pl.art = {};

// ---- Test signals: the ones Audio → Streams makes, a tap away, in the place of what the board
// plays, on its outputs (or those chosen, and in this browser if ticked).
const SIGNALS = [
  {title: 'Tone 1 kHz', sub: '−12 dBFS · the reference', source: {type: 'tone', freq: 1000, level_db: -12}},
  {title: 'Tone 440 Hz', sub: '−12 dBFS · A4', source: {type: 'tone', freq: 440, level_db: -12}},
  {title: 'Tone 100 Hz', sub: '−12 dBFS · bass', source: {type: 'tone', freq: 100, level_db: -12}},
  {title: 'Left / right', sub: '1 kHz left, 2 kHz right · which channel is which', source: {type: 'tone', freq: 1000, freq_right: 2000, level_db: -12}},
  {title: 'Sweep 20 Hz – 20 kHz', sub: '10 s, logarithmic, repeating', source: {type: 'sweep', from: 20, to: 20000, seconds: 10, log: true, repeat: true, level_db: -12}},
  {title: 'Pink noise', sub: '−20 dBFS RMS', source: {type: 'noise', color: 'pink', level_db: -20}},
  {title: 'White noise', sub: '−20 dBFS RMS', source: {type: 'noise', color: 'white', level_db: -20}},
  {title: 'Silence', sub: 'keeps a link streaming, with nothing in it', source: {type: 'silence'}},
];

function renderSignals() {
  setHTML($('np-crumbs'), '');
  pl.libItems = SIGNALS.map(x => ({kind: 'signal', title: x.title, sub: x.sub, source: x.source}));
  renderLib('', `<form class="sig-add row" id="np-sig-add">
    <label class="row">Tone <input type="number" name="freq" class="narrow" value="1000" min="1" max="24000" required> Hz</label>
    <label class="row"><input type="number" name="level" class="narrow" value="-12" min="-96" max="0" required> dBFS</label>
    <button class="small-btn">${svg(PL_SVG.play)} Play</button>
    <a href="#audio/streams" class="small">More in Audio → Streams</a></form>`);
}

function playSignal(source) {
  playLocal({source, rate: 48000, channels: 2}, s => { pl.boardId = s.id; });
}

// The rate the board decodes a track at: its own, in the family PipeWire's graph runs at (44.1 or
// 48 kHz) — the decoder resampling a 44.1 kHz MP3 to 48 kHz costs a Zero W three times the CPU of
// decoding it. Unknown (a radio station): 44.1 kHz, what nearly every MP3 stream is.
const streamRate = r => r && r % 48000 === 0 ? 48000 : 44100;

// The art and duration of the board's tracks, from what the library said about them.
function decorate(s) {
  (s.source.playlist || []).forEach(it => Object.assign(it, pl.art[it.url] ? {art: it.art || pl.art[it.url].art, duration_s: pl.art[it.url].duration_s} : {}));
  return s;
}

// ---------------------------------------------------------------- outputs

// Where the input plays: the route's outputs, each on or off by itself — the board's own sound
// card, any Bluetooth speaker or pair of earbuds, the board's Auracast broadcasts —
// and this browser, which is this browser's own choice (it listens to the route's stream). They
// are the route's, in the daemon: every browser sees the same, and they stay when the input
// changes. Tapped while something plays, an output is added to the route's stream or taken off it
// at once (PUT /api/audio/route {outputs} → the stream's sinks: the decoder and the other outputs
// carry on). Values: "<backend>:<endpoint id>", "browser"; "default" (PipeWire's default sink) only
// for a stream started elsewhere on it. The line under the buttons follows a switch until the
// output takes the audio (and a Bluetooth link streams it), or says why it did not.
const MAX_OUTS = 4;  // the daemon's AudioEngine::kMaxSinks
pl.listen = lsGet('btbench.pl.here') === '1';
pl.sw = null;  // a switch: {value, add, seq, phase: asking|removing|starting|linking|done|failed, at, error}

const outSink = v => v === 'none' || v === 'browser' ? null : v === 'default' ? {type: 'pipewire', target: ''}
  : {type: v.split(':')[0], target: v.slice(v.indexOf(':') + 1)};
const outSinks = vs => vs.map(outSink).filter(Boolean);
const sinkOut = k => k.target ? `${k.type}:${k.target}` : 'default';
// A stream's outputs, as values (a stream from before there were several has one "sink").
const streamOuts = s => (s.sinks || (s.sink ? [s.sink] : [])).map(sinkOut);
// The outputs after `v` was tapped with `cur` on: each one is added or taken off by itself.
function nextOuts(cur, v) {
  if (v === 'none') return [];
  return cur.includes(v) ? cur.filter(x => x !== v) : cur.filter(x => x !== 'none').concat(v);
}
// These take the view whose outputs and links they look at, `o` = {endpoints, media}: Listen's
// (pl, the default) or Audio's (au) — Audio → Streams shows a stream's outputs with the same
// buttons and follows their switches with the same words (outLine).
const outEndpoint = (v, o = pl) => o.endpoints && o.endpoints.sinks.find(e => v.endsWith(':' + e.id));
function outName(v, o = pl) {
  if (v === 'none') return 'No board output';
  if (v === NEW_BCAST) return 'Auracast';
  if (v === 'browser') return 'This browser';
  if (v === 'default') return 'The board’s default output';
  const e = outEndpoint(v, o);
  if (e) return e.address ? devName(e.address) : e.label;
  // A Bluetooth sink PipeWire does not have (now): its device, by name.
  const bt = /bluez_output\.([0-9A-F]{2}(?:_[0-9A-F]{2}){5})/i.exec(v);
  return bt ? devName(bt[1].replace(/_/g, ':')) : v.slice(v.indexOf(':') + 1);
}
// An output's name after the start of a sentence ("Playing on the board’s sound card").
const outInSentence = (v, o = pl) => outName(v, o).replace(/^The /, 'the ');
// The board's transport that carries a Bluetooth output's audio: its "Audio Source" side (a device
// can have one each way, see volumeTarget).
function outTransport(v, o = pl) {
  const e = outEndpoint(v, o);
  if (!e || !e.address || !o.media) return null;
  const same = x => x.address && x.address.toUpperCase() === e.address.toUpperCase();
  return o.media.transports.find(x => same(x) && /source/i.test(x.profile || '')) || null;
}
// A stream's sink status for output `v`: {state, error, …}, or null.
const outStatusOf = (b, v) => (b && b.sinks_status || []).find(k => sinkOut(k) === v) || null;

function renderOutputs(ep) {
  pl.endpoints = ep;
  renderNowPlaying();  // "Playing on …" names the output now that its label is known
}

// Auracast as an output while the board broadcasts nothing yet: tapped, a broadcast is started.
const NEW_BCAST = 'auracast:new';
const isBcast = e => !!e.transport && !e.address;

// Why sink `e` (an endpoint; null for a broadcast not started yet) cannot be on while `input` is
// the route's, or '': a device the input comes from would be sent its own audio back, and the
// board does not broadcast while it receives a broadcast.
function outBlocked(e, input) {
  const ds = input && input.type === 'bluetooth' ? input.devices : [];
  if (!e || isBcast(e)) return ds.some(d => d.kind === 'broadcast') ? 'receiving a broadcast' : '';
  const addrs = e.set_leader ? (e.members || []) : e.address ? [e.address] : [];
  const from = a => ds.some(d => sameAddr(d.address, a) || (inputSet(d.address) || []).some(m => sameAddr(m, a)));
  return addrs.some(from) ? 'the input' : '';
}

// The buttons, in one list: this browser (Listen only), the board's own sound cards, the
// Bluetooth speakers and earbuds (a coordinated set as one: PipeWire's node of the set, its
// members' own nodes left out), then the board's Auracast broadcasts (in Listen, one to start when
// there is none). The loopback card is left out: nobody hears it. In Listen an output the input
// rules out is shown off and cannot be tapped (outBlocked).
function outOptions(browser, o = pl) {
  const ep = o.endpoints;
  const opts = browser ? [{value: 'browser', name: 'This browser', sub: 'listen here', icon: PL_SVG.browser, rank: 0}] : [];
  const kind = ep && ep.backend === 'alsa' ? 'alsa' : 'pipewire';
  const input = o === pl && pl.route ? pl.route.input : null;
  const sinks = (ep ? ep.sinks : []).filter(e => e.kind !== 'loopback');
  sinks.forEach(e => {
    if (e.set && !e.set_leader) return;
    const v = `${kind}:${e.id}`;
    const t = e.address ? outTransport(v, o) : null;
    const bcast = isBcast(e);
    const bt = e.kind === 'bluetooth' && !bcast;
    opts.push({value: v, name: e.set_leader ? ((e.members || []).length === 1 ? devName(e.members[0]) : e.label) : e.address ? devName(e.address) : e.label,
      sub: e.set_leader ? ((e.members || []).length > 1 ? `${e.members.length} devices, one each channel` : 'left + right') : e.address ? (t && t.codec ? t.codec.name : 'Bluetooth')
        : bcast ? 'Auracast · off ends it' : 'the board’s sound card',
      icon: bt || bcast ? PL_SVG.bt : PL_SVG.speaker, rank: bt ? 2 : bcast ? 3 : 1, off: outBlocked(e, input)});
  });
  if (o === pl && bcastAdapter() && !sinks.some(isBcast)) {
    opts.push({value: NEW_BCAST, name: 'Auracast', sub: 'start a broadcast', icon: PL_SVG.bt, rank: 3, off: outBlocked(null, input)});
  }
  return opts.sort((a, b) => a.rank - b.rank);
}
// The adapter that can broadcast (its broadcast endpoint registered by PipeWire), or null.
const bcastAdapter = () => (pl.la ? pl.la.adapters : []).find(a => a.broadcaster && a.pac) || null;

// The buttons for outputs `cur` on, with the switch `sw` under way: each one on or off by itself;
// one on that is not there any more (a speaker gone) stays, greyed.
function outButtonsHTML(opts, cur, sw, o = pl) {
  cur.forEach(v => { if (!opts.some(x => x.value === v)) opts.push({value: v, name: outName(v, o), sub: 'not connected', icon: PL_SVG.bt, gone: true}); });
  return opts.map(x => {
    const busy = sw && sw.phase !== 'failed' && sw.phase !== 'done' && sw.value === x.value;
    const bad = sw && sw.phase === 'failed' && sw.value === x.value;
    const on = !busy && !bad && !x.off && (x.value === 'none' ? !cur.length : cur.includes(x.value));
    const cls = 'np-o' + (on ? ' on' : '') + (busy ? ' busy' : '') + (bad ? ' bad' : '') + (x.gone ? ' gone' : '');
    const sub = x.off || x.sub;
    return `<button class="${cls}" data-out="${esc(x.value)}" aria-pressed="${on}"${x.off ? ' disabled' : ''}>` +
      (busy ? '<span class="dots"></span>' : svg(x.icon)) + `<b>${esc(x.name)}</b>${sub ? `<small>${esc(sub)}</small>` : ''}</button>`;
  }).join('');
}

// The outputs on: the route's, and this browser when it listens.
const curOuts = () => routeOuts().concat(pl.listen ? ['browser'] : []);

function renderOuts() {
  const b = outStream();
  swAdvance(pl.sw, b);
  // Not before the board's outputs and the route are known: the buttons would show nothing lit.
  if ((pl.endpoints || pl.endpointsFailed) && pl.route) setHTML($('np-outs'), outButtonsHTML(outOptions(true), curOuts(), pl.sw));
  // A good line (everything plays where it should) is said for a few seconds and then cleared:
  // the lit buttons say it from there. Anything else stays, and a change says it again.
  const st = outStatus(b), key = st.cls + st.html;
  if (key !== pl.statusKey) {
    pl.statusKey = key;
    pl.statusAt = Date.now();
    if (st.cls === 'good') setTimeout(renderOuts, OUT_STATUS_MS + 100);
  }
  const quiet = st.cls === 'good' && Date.now() - pl.statusAt > OUT_STATUS_MS;
  outStatusHTML($('np-out-status'), quiet ? {cls: '', html: ''} : st);
  $('np-out-status').classList.toggle('quiet', quiet);
}
const OUT_STATUS_MS = 5000;

// A status line ({cls, html}) into its element: a dot in the state's colour, or a spinner.
function outStatusHTML(el, st) {
  setClass(el, el.className.replace(/\s*\b(good|warn|bad|busy)\b/g, '') + (st.cls ? ' ' + st.cls : ''));
  setHTML(el, (st.cls === 'busy' ? '<span class="dots"></span>' : '<span class="dot"></span>') + `<span>${st.html}</span>`);
}

const andList = xs => xs.length < 2 ? xs.join('') : xs.slice(0, -1).join(', ') + ' and ' + xs[xs.length - 1];

// One of a stream's outputs in words, from its sink status `k`: {cls, html}, and for one that
// plays `what` (its name and codec), which outLine says of all of them at once.
function sinkLine(k, o) {
  const v = sinkOut(k), name = `<b>${esc(outName(v, o))}</b>`, mid = `<b>${esc(outInSentence(v, o))}</b>`;
  if (k.state === 'starting') return {cls: 'busy', html: `Opening ${mid}…`};
  if (k.state === 'failed') return {cls: 'bad', html: `Lost ${mid}: ${esc(outReason(k.error))}.`};
  const t = outTransport(v, o);
  if (k.state === 'stalled') {
    return {cls: 'bad', html: `${name} takes no audio` + (t ? ` (its Bluetooth link is ${esc(t.state)})` : '') + '.'};
  }
  if (t && t.state !== 'active') return {cls: 'warn', html: `${name} takes the audio, but its Bluetooth link is ${esc(t.state)}, not streaming.`};
  const codec = t && t.codec ? ` · ${esc(t.codec.name)}${t.codec.rate ? ' ' + t.codec.rate / 1000 + ' kHz' : ''}` : '';
  return {cls: 'good', what: mid + codec};
}

// What the board is doing about a stream's outputs, in words: the switch `sw` under way (or how it
// failed), else what its sinks' statuses say — the ones playing in one sentence, then each of the
// others; null when it has no output and lost none. Listen adds what this browser does; Audio →
// Streams shows it as it is.
function outLine(b, sw, o = pl) {
  const name = v => `<b>${esc(outInSentence(v, o))}</b>`;
  if (sw && sw.phase === 'failed') return {cls: 'bad', html: `Could not play on ${name(sw.value)}: ${esc(outReason(sw.error))}.`};
  if (sw && sw.phase !== 'done') {
    return {cls: 'busy', html: {broadcast: 'Starting an Auracast broadcast…', asking: sw.add ? `Asking the board to play on ${name(sw.value)}…` : `Taking ${name(sw.value)} off…`,
      removing: `Taking ${name(sw.value)} off…`, starting: `Opening ${name(sw.value)}…`,
      linking: `<b>${esc(outName(sw.value, o))}</b> takes the audio; waiting for its Bluetooth link to stream…`}[sw.phase]};
  }
  if (!b) return null;
  const lines = (b.sinks_status || []).map(k => sinkLine(k, o));
  if (!lines.length) return null;
  const good = lines.filter(l => l.cls === 'good'), rest = lines.filter(l => l.cls !== 'good');
  const worst = ['bad', 'busy', 'warn'].find(c => rest.some(l => l.cls === c));
  return {cls: !worst ? 'good' : worst === 'bad' && good.length ? 'warn' : worst,
    html: [good.length ? `Playing on ${andList(good.map(l => l.what))}.` : ''].concat(rest.map(l => l.html)).filter(Boolean).join(' ')};
}

// What the board is doing about the outputs shown in Listen, in words: outLine, and this browser.
function outStatus(b) {
  const sw = pl.sw;
  const here = b && pl.hearId === b.id;
  const connecting = here && (npAudio.paused || npAudio.readyState < 3);
  const browser = here ? (connecting ? ' This browser is connecting…' : ' Also in this browser.') : '';
  if (sw && sw.phase === 'failed') return {cls: 'bad', html: outLine(b, sw).html + (here ? ' Still playing in this browser.' : '')};
  if (sw && sw.phase !== 'done') return outLine(b, sw);
  if (!pl.route) return {cls: '', html: ''};
  if (!b) {
    const to = routeOuts().map(v => `<b>${esc(outInSentence(v))}</b>`).concat(pl.listen ? ['this browser'] : []);
    return {cls: '', html: to.length ? `What plays goes to ${andList(to)}.` : 'Nothing is on: turn on where the board plays.'};
  }
  const line = outLine(b, null);
  if (line) return line.cls === 'good' || line.cls === 'warn' ? {cls: line.cls, html: line.html + browser} : line;
  if (here) return {cls: connecting ? 'busy' : 'good', html: connecting ? 'Connecting this browser…' : 'Playing in this browser.'};
  const others = b.listeners;
  return {cls: '', html: others ? `Playing to ${others} other browser${others > 1 ? 's' : ''} only.`
    : 'Playing, to no output: turn one on.'};
}

// PipeWire's words for an output that is not there.
const outReason = e => /target not found/.test(e || '') ? 'it is not connected (no such output in PipeWire)' : (e || 'no reason given');

// The switch `sw` under way on stream `b`, moved on by what the stream says: once its sink_seq
// answers our request (seq) the pump has opened the output added (or failed to) or let the one
// taken off go; a Bluetooth one is done when its link streams (in o.media).
function swAdvance(sw, b, o = pl) {
  // A broadcast being started is followed by startBroadcastOut, not by the stream.
  if (!sw || sw.phase === 'done' || sw.phase === 'failed' || sw.phase === 'broadcast') return;
  const late = Date.now() - sw.at;
  if (!b) {
    if (sw.phase === 'asking' && late < 15000) return;
    sw.phase = 'failed';
    sw.error = 'the board stopped playing';
    return;
  }
  if (sw.seq == null) { if (late > 10000) { sw.phase = 'failed'; sw.error = 'the board did not answer'; } return; }
  const seq = b.sink_seq != null ? b.sink_seq : (b.sink_status || {}).seq;
  if (seq < sw.seq) { if (late > 10000) { sw.phase = 'failed'; sw.error = 'the board did not switch within 10 s'; } return; }
  if (!sw.add) { sw.phase = 'done'; return; }
  const ss = outStatusOf(b, sw.value) || {};
  if (ss.state === 'failed' || ss.state === 'stalled') { sw.phase = 'failed'; sw.error = ss.error; return; }
  if (ss.state === 'starting') {
    sw.phase = 'starting';
    if (late > 10000) { sw.phase = 'failed'; sw.error = 'it took no audio within 10 s'; }
    return;
  }
  if (ss.state === 'playing') {
    const t = outTransport(sw.value, o);
    // A Bluetooth link takes a moment to start streaming; past 8 s the steady line says how it is.
    sw.phase = t && t.state !== 'active' && late < 8000 ? 'linking' : 'done';
    return;
  }
  sw.phase = 'done';  // not among its sinks: nothing to follow
}

// An output tapped: added to the route's outputs or taken off them, and so to what plays now.
// "This browser" is this browser listening or not.
function pickOut(v) {
  if (v === 'browser') return pickListen(!pl.listen);
  if (!pl.route) return;
  if (v === NEW_BCAST) return startBroadcastOut();
  const next = nextOuts(routeOuts(), v);
  if (next.length > MAX_OUTS) { toast(`The board plays to at most ${MAX_OUTS} outputs at once.`); return; }
  const b = outStream(), add = next.includes(v);
  const sw = b ? {value: v, add, phase: 'asking', at: Date.now(), seq: null} : null;
  pl.sw = sw;
  pl.route.outputs = outSinks(next);
  renderOuts();
  put('/audio/route', {outputs: outSinks(next)}).then(r => {
    const u = r.updated;
    delete r.updated;
    pl.route = r;
    if (sw && pl.sw === sw) {
      if (u) {
        sw.seq = u.sink_req;
        sw.phase = add ? 'starting' : 'removing';
        const i = pl.streams.findIndex(x => x.id === u.id);
        if (i >= 0) pl.streams[i] = decorate(u);
      } else {
        sw.phase = 'done';
      }
    }
    renderNowPlaying();
  }).catch(e => {
    if (sw && pl.sw === sw) { sw.phase = 'failed'; sw.error = e.message; }
    refreshRoute();
  });
  // The switch's result comes with the streams topic; this view asks too, in case it is slow.
  if (sw) [400, 1200, 2500, 5000, 9000].forEach(t => setTimeout(() => { if (pl.sw === sw && sw.phase !== 'done') refreshPlayerStreams(); }, t));
}

// Auracast tapped while the board broadcasts nothing: a stereo broadcast of BAP's 48_2_1 (Audio →
// LE Audio's default) is started, which the daemon makes one of the route's outputs; tapped off
// like any output, the daemon ends it. Its settings, or a second broadcast, are Audio → LE Audio's.
function startBroadcastOut() {
  const a = bcastAdapter();
  if (!a || (pl.sw && pl.sw.phase !== 'done' && pl.sw.phase !== 'failed')) return;
  const sw = pl.sw = {value: NEW_BCAST, add: true, phase: 'broadcast', at: Date.now(), seq: null};
  renderOuts();
  post('/le-audio/broadcasts', {adapter: a.adapter, preset: '48_2_1', channels: 2}).then(r => {
    if (r.output_error) throw new Error(r.output_error);
    return Promise.all([api('/audio/endpoints?fresh=1').then(ep => { pl.endpoints = ep; }), refreshLa(), refreshRoute()]);
  }).then(() => { if (pl.sw === sw) pl.sw = null; renderNowPlaying(); })
    .catch(e => { if (pl.sw === sw) { sw.phase = 'failed'; sw.error = e.message; } renderOuts(); });
}

// "This browser" on or off: whatever the route plays is heard here, or not, and the choice is this
// browser's (it stays for the next input).
function pickListen(on) {
  pl.listen = on;
  lsSet('btbench.pl.here', on ? '1' : '0');
  followRoute();
  renderNowPlaying();
}

// This browser follows the route: listening, it hears the route's stream, and a new input (or the
// same one recorded anew) is heard in its place.
function followRoute() {
  const id = pl.route ? pl.route.stream : null;
  if (pl.hearId && pl.hearId !== id) hearStop();
  if (pl.listen && id && pl.hearId !== id) hear(id);
  if (!pl.listen && pl.hearId) hearStop();
}

function setRoute(r) {
  delete r.updated;
  pl.route = r;
  followRoute();
  renderNowPlaying();
}
const refreshRoute = () => api('/audio/route').then(setRoute).catch(() => {});
// Which adapter can broadcast and receive, for Auracast in Play to and Inputs. While it scans, it
// is asked every 3 s: the broadcasters found come with it.
function refreshLa() {
  return api('/le-audio').then(s => {
    pl.la = s;
    renderNowPlaying();
    clearTimeout(pl.laT);
    if (currentTab === 'player' && s.adapters.some(a => a.discovering)) pl.laT = setTimeout(refreshLa, 3000);
  }).catch(() => {});
}

// The outputs come and go with Bluetooth: asked again whenever the board's audio links change,
// and once more a moment later (PipeWire makes a speaker's sink a little after BlueZ's transport).
function outLinksChanged(m) {
  const key = (m.transports || []).map(t => t.path).sort().join(',');
  if (key === pl.linksKey) return;
  pl.linksKey = key;
  const ask = () => api('/audio/endpoints?fresh=1').then(renderOutputs).catch(() => {});
  ask();
  refreshLa();
  setTimeout(ask, 2000);
}

function refreshPlayerStreams() {
  return api('/audio/streams').then(r => { pl.streams = r.streams.map(decorate); noteFailure(); renderNowPlaying(); }).catch(() => {});
}

// What this view started and the board could not play (a decoder missing, a server refusing):
// said once, with the board's reason — otherwise the page would only fall back to "Nothing
// playing" without a word.
function noteFailure() {
  const s = pl.streams.find(x => x.id === pl.boardId);
  if (!s || s.state !== 'failed' || pl.failedShown === s.id) return;
  pl.failedShown = s.id;
  const why = /mpg123/.test(s.error) && /\.(flac|wav|m4a|aac|ogg|opus)\b/i.test((s.source.playlist[s.track] || {}).url || s.source.url)
    ? 'the board has no decoder for this format (mpg123 plays MP3 only; other formats need ffmpeg in the image, BTBENCH_FFMPEG)'
    : s.error.replace(/\[\/usr\/src\/debug\/[^\]]*\]\s*/g, '');
  toast('Could not play “' + (s.source.title || 'this') + '”: ' + why);
}

// ---------------------------------------------------------------- wiring

function setupPlayer() {
  $('np-ctl').onclick = e => {
    const b = e.target.closest('[data-np]');
    if (b && !b.disabled) npAction(b.dataset.np);
    const m = e.target.closest('[data-mode]');
    if (m) npMode(m.dataset.mode);
  };
  $('np-mini-ctl').onclick = e => {
    const b = e.target.closest('[data-np]');
    if (b && !b.disabled) npAction(b.dataset.np);
  };
  $('np-src').onclick = e => {
    const b = e.target.closest('[data-src]');
    if (!b) return;
    const p = phones().find(x => x.path === b.dataset.src);
    if (p) pl.players[p.address.toUpperCase()] = p.path;
    pl.queueKey = '';
    renderNowPlaying();
  };
  $('np-vol').oninput = e => {
    clearTimeout(pl.volT);
    // A slider sends many values a second: at most one request each 150 ms.
    pl.volT = setTimeout(() => setVolume(parseInt(e.target.value, 10)), 150);
  };
  $('np-outs').onclick = e => {
    const b = e.target.closest('[data-out]');
    if (b) pickOut(b.dataset.out);
  };
  $('np-queue').onclick = e => {
    const c = current();
    const del = e.target.closest('[data-q-del]');
    if (del) {
      if (c.kind === 'board') put(`/audio/streams/${c.s.id}`, {remove: parseInt(del.dataset.qDel, 10)}).then(refreshPlayerStreams).catch(fail);
      return;
    }
    const r = e.target.closest('.tr');
    if (!r) return;
    if (r.dataset.track != null && c.kind === 'board') put(`/audio/streams/${c.s.id}`, {track: parseInt(r.dataset.track, 10), paused: false}).catch(fail);
    if (r.dataset.item && c.kind === 'phone') post('/media/items', {path: r.dataset.item, action: 'play'}).catch(fail);
  };
  $('np-lib-tabs').onclick = e => {
    const b = e.target.closest('[data-lib]');
    if (b) openLib(b.dataset.lib, true);
  };
  $('np-crumbs').onclick = e => {
    const a = e.target.closest('[data-crumb]');
    if (!a) return;
    e.preventDefault();
    const i = parseInt(a.dataset.crumb, 10);
    // A folder above this one: shown as it was left, like Back to it (the one shown: loaded again).
    const below = pl.libPath[i + 1];
    libDropBack(i + 1);
    if (below && below.parent) return libRestore(below);
    if (i < 0) { pl.libPath = []; return upnpServers(false); }
    const c = pl.libPath[i];
    pl.libPath = pl.libPath.slice(0, i + 1);
    if (pl.lib === 'upnp') upnpFolder(pl.upnpServer, c.id, c.title, false);
    else phoneFolder(phones().find(p => p.browsable), c.id, c.title, false);
  };
  $('np-inputs').onclick = inputClick;
  $('np-lib').onclick = e => {
    if (e.target.closest('[data-lib-search]')) return upnpServers(true);
    if (e.target.closest('[data-lib-more]')) return libMoreLoad();
    const all = e.target.closest('[data-lib-all]');
    if (all) return libAll(all.dataset.libAll);
    const add = e.target.closest('[data-lib-add]');
    if (add) return libClick(parseInt(add.dataset.libAdd, 10), 'end');
    const next = e.target.closest('[data-lib-next]');
    if (next) return libClick(parseInt(next.dataset.libNext, 10), 'next');
    const folder = e.target.closest('[data-lib-folder]');
    if (folder) {
      const f = pl.libItems[parseInt(folder.dataset.libFolder, 10)];
      return f && folderPlay(pl.upnpServer, f.id, f.title, f.art || '', '');
    }
    const rm = e.target.closest('[data-lib-del]');
    if (rm) return stationRemove(parseInt(rm.dataset.libDel, 10));
    const r = e.target.closest('[data-lib-i]');
    if (r) libClick(parseInt(r.dataset.libI, 10), false);
  };
  $('np-lib').onsubmit = e => {
    e.preventDefault();
    if (e.target.id === 'np-radio-add') stationAdd(e.target);
    if (e.target.id === 'np-sig-add') {
      const f = e.target, freq = parseFloat(f.freq.value), level = parseFloat(f.level.value);
      if (freq > 0 && level <= 0) playSignal({type: 'tone', freq, level_db: level});
    }
  };
  $('np-search').onkeydown = e => {
    if (e.key !== 'Enter') return;
    const p = phones().find(x => x.searchable);
    if (!p) return;
    post('/media/players/search', {path: p.path, value: e.target.value}).then(r => {
      libDropBack(0);
      pl.libPath = [{id: r.folder, title: '“' + e.target.value + '”'}];
      renderCrumbs();
      pl.libItems = (r.items || []).map(i => phoneRow(i, p.path));
      renderLib();
    }).catch(fail);
  };
}

registerTab('player', {
  topics: () => ['media', 'audio.streams', 'audio.route'],
  show() {
    // The library opens once the players are known: a browsable phone's is the first tab.
    // A folder still open from before (Back to this view, or a look at another tab) stays as it was
    // left, with its Back entries; a phone's only while the phone is still there.
    const kept = () => pl.libPath.length && pl.libItems.length && (pl.libPath[0].parent || {}).lib === pl.lib &&
      (pl.lib !== 'phone' || phones().some(x => x.browsable));
    api('/media').then(m => { pl.media = m; pl.linksKey = (m.transports || []).map(t => t.path).sort().join(','); notePlayers(m.players); renderNowPlaying(); renderLibTabs(); if (!kept()) openLib(pl.lib); })
      .catch(() => { pl.mediaFailed = true; renderNowPlaying(); renderLibTabs(); if (!kept()) openLib(pl.lib); });
    api('/bluetooth').then(b => { btState = b; renderNowPlaying(); }).catch(() => {});
    api('/audio/endpoints').then(renderOutputs).catch(() => { pl.endpointsFailed = true; renderOuts(); });
    refreshLa();
    refreshPlayerStreams();
    refreshRoute();
    clearInterval(pl.tick);
    // The progress bar moves between the phone's updates.
    pl.tick = setInterval(() => {
      if (++pl.ticks % 6 === 0) hearCheck();  // every 3 s
      const c = current();
      if (c.kind === 'phone' && c.p.status === 'playing') renderTime(playerPosition(c.p), (c.p.track || {}).duration_ms || 0);
    }, 500);
  },
  hide() { clearInterval(pl.tick); pl.tick = null; },
  pane(p) {
    // HFP's and a set's nodes are only in the endpoints, which follow the transports otherwise.
    if (p === 'inputs' && currentTab === 'player') api('/audio/endpoints?fresh=1').then(renderOutputs).catch(() => {});
  },
  onMessage(topic, data) {
    if (currentTab !== 'player') return;
    if (topic === 'media') {
      const hadLib = phones().some(p => p.browsable);
      pl.media = data;
      notePlayers(data.players);
      outLinksChanged(data);
      if (hadLib !== phones().some(p => p.browsable)) renderLibTabs();
      if (!(document.activeElement && document.activeElement.id === 'np-vol')) renderNowPlaying();
    } else if (topic === 'audio.route') {
      pl.route = data;
      followRoute();
      if (!(document.activeElement && document.activeElement.id === 'np-vol')) renderNowPlaying();
    } else if (topic === 'audio.streams') {
      pl.streams = data.streams.map(decorate);
      noteFailure();
      if (!(document.activeElement && document.activeElement.id === 'np-vol')) renderNowPlaying();
    }
  },
});
setupPlayer();
