'use strict';
// Audio: the board as a player and a bench at once. The board plays one input at a time — its own
// music (internet radio, tracks from a UPnP media server as a playlist the board plays to the end,
// a test signal) or what one Bluetooth device sends it (a phone's A2DP and its AVRCP player; an LE
// Audio source; an Auracast broadcast, or a left and a right one as one stereo input; a call's
// audio over HFP) — on any number of outputs at once: the board's sound card, Bluetooth speakers
// and earbuds, its Auracast broadcasts, and this browser. The input and the outputs are the
// daemon's route (/api/audio/route, audio/route.h), the same for every browser; the board's
// playlist lives there too, so closing the page does not stop the music.
//
// One tab, three views: Now playing with what plays next (here), Route (route.js: the inputs and
// the outputs, each with its link, codec and volume), Library (library.js). LE Audio (leaudio.js) and
// PipeWire (pipewire.js) are tabs of their own in the same area. This file holds what they share:
// the state, what plays, the outputs and how a switch of one is followed, hearing it here, and the
// device helpers the rest of the console uses too (devName, the codec picker, the link words).

const au = {
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
  cover: {},     // a player's cover-art (BIP) channel: {state, at}, see coverConnect
  playerClock: {},  // a player's position as last heard: {pos, at, status}
  devices: null, // GET /api/audio/devices: PipeWire's Bluetooth cards, the codec picker's
  local: null,   // GET /api/media/local-player: the board's own player
  art: {},       // a track's URL → what the library said of it (cover, length), see decorate
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
  au.artDone[src] = ok;
  img.hidden = !ok;
  img.parentNode.classList.remove('loading');
  const base = src.replace(/&r=\d+$/, '');
  const n = (au.artTries[base] || 0) + 1;
  if (!ok && base.startsWith('/api/media/players/cover') && n <= 4) {
    au.artTries[base] = n;
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
const routeStream = () => au.route && au.route.stream ? au.streams.find(s => s.id === au.route.stream) || null : null;
// The board's own stream that is loaded, playing or paused while a device is the input.
const loadedStream = () => au.route && au.route.local ? au.streams.find(s => s.id === au.route.local) || null : null;
const routeOuts = () => (au.route ? au.route.outputs : []).map(sinkOut);

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
const phones = () => (au.media ? au.media.players : []).filter(p => p.name || p.browsable || (p.track && p.track.title) ||
  /\/avrcp\//.test(p.path));
const isMcp = p => /\/mcp_/.test(p.path);  // BlueZ's mcp_gmcs/playerN, mcp_mcs/playerN

// Bluetooth audio coming into the board with no player of the device's to show for it, each
// shown the way a phone's is (`input` set): an Auracast broadcast in range — every one BlueZ has
// read, received or not, so one can be picked and play and stop receive it — and a device that
// streams to the board with no media control (an LE Audio source). A broadcaster's BISes are one.
const sameAddr = (a, b) => !!a && !!b && a.toUpperCase() === b.toUpperCase();
function inputs() {
  const ts = au.media ? au.media.transports : [], ps = phones();
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
  const ts = au.media ? au.media.transports : [];
  const srcs = au.endpoints ? au.endpoints.sources.filter(e => e.kind === 'bluetooth') : [];
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
const isRouted = r => au.route && au.route.input.type === 'bluetooth' &&
  au.route.input.devices.some(d => sameAddr(d.address, r.address) && (!d.kind || d.kind === r.kind));

// The source on screen: the route's input. A device with several players (Spotify and the phone's
// own music app) shows the one picked, else the one playing.
function current() {
  const r = au.route;
  if (!r) return {kind: 'none'};
  if (r.input.type === 'local') {
    const s = routeStream();
    return s ? {kind: 'board', s} : {kind: 'none'};
  }
  if (r.input.type !== 'bluetooth' || !r.input.devices.length) return {kind: 'none'};
  const d = r.input.devices[0];
  if (d.kind !== 'broadcast' && d.kind !== 'hfp') {
    const ps = phones().filter(p => sameAddr(p.address, d.address) || (inputSet(d.address) || []).some(m => sameAddr(m, p.address)));
    const p = ps.find(x => x.path === au.players[d.address.toUpperCase()]) || ps.find(x => x.status === 'playing') || ps[0];
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
  const e = au.endpoints && au.endpoints.sources.find(s => s.set_leader && (s.members || []).some(m => sameAddr(m, address)));
  return e ? e.members : null;
}

// The stream the output row acts on: the route's.
const outStream = () => routeStream();



// ---------------------------------------------------------------- now playing

function renderNowPlaying() {
  const c = current();
  // The input is picked in Route; here, a device with several players (Spotify and the
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
      artSrc = base + (au.artTries[base] ? '&r=' + au.artTries[base] : '');
    } else if (p.obex_port != null) {
      coverConnect(p, renderNowPlaying);
      // A cover's handle comes with the track info once the image channel is open. Loading while
      // it opens, and for a few seconds after for a track that came before it (a phone sends the
      // handle a little later); a track that came while it was open and has none has no cover,
      // and neither has any while it cannot be opened.
      const k = p.path + '|' + (t.title || '');
      if (!au.artWait[k]) au.artWait[k] = Date.now();
      const ch = au.cover[p.path] || {state: 'asking'};
      artLoading = ch.state === 'asking' ? Date.now() - au.artWait[k] < 12000
        : ch.state === 'open' && au.artWait[k] < ch.at && Date.now() - ch.at < 5000;
      if (artLoading && !au.artTimer) au.artTimer = setTimeout(() => { au.artTimer = null; renderNowPlaying(); }, 1000);
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
    art = svg(ph, 'ph') + `<img alt="" src="${esc(artSrc)}" onload="npArtDone(this, true)" onerror="npArtDone(this, false)"${au.artDone[artSrc] === true ? '' : ' hidden'}>`;
    artLoading = au.artDone[artSrc] === undefined;
  }
  // Until /media answers, "Nothing playing" would be a guess: the cover and the title wait instead.
  const waiting = c.kind === 'none' && !au.media && !au.mediaFailed;
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
  // The bar under Route and Library: the track, and play/pause and next, to skip without
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
  renderRoute();
}

// Which way the music flows, said above the title the same way for both: into the board from a
// Bluetooth device (and whether its audio actually streams in, or only its player is known), or out
// of the board from a station, a media server's tracks or a test signal; then the path, from the
// source through the board to where it plays.
function npFrom(c) {
  const tag = (icon, cls, text) => `<span class="np-tag ${cls}">${svg(icon)}${text}</span>`;
  const flow = parts => `<span class="np-flow">${parts.map(esc).join(svg(PL_SVG.arrow))}</span>`;
  const lower = n => n.replace(/^The /, 'the ');
  const outs = s => (s ? streamOuts(s).map(v => lower(outName(v))) : []).concat(s && au.hearId === s.id ? ['this browser'] : []);
  if (c.kind === 'phone') {
    const ts = c.p.input ? c.p.transports : (au.media ? au.media.transports : []).filter(t => t.address === c.p.address && /^(110b|2bc9)$/i.test(t.uuid));
    const on = ts.some(t => t.state === 'active' || t.state === 'broadcasting');
    const s = routeStream(), inp = au.route.input;
    // What the board does with it: records it (the capture) and plays that on the route's outputs.
    const to = routeOuts().map(v => lower(outName(v))).concat(s && au.hearId === s.id ? ['this browser'] : []);
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
  const ts = au.media ? au.media.transports : [];
  if (c.kind === 'phone') {
    const t = ts.find(x => x.path === c.p.transport);
    return t && t.volume != null ? {kind: 'transport', path: t.path, max: transportVolMax(t), value: Math.round(t.volume / transportVolMax(t) * 100)} : null;
  }
  if (c.kind === 'board') {
    const s = c.s;
    const outs = streamOuts(s);
    if (!outs.length) return au.hearId === s.id ? {kind: 'browser', value: Math.round(npAudio.volume * 100)} : null;
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
  au.vol = v;
  $('np-vol-row').hidden = !v;
  if (v && idle($('np-vol'))) $('np-vol').value = Math.min(100, v.value);
}

function setVolume(value) {
  const v = au.vol;
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
    if (a === 'play') return receiveBroadcast(c.p, true).then(() => pickInput(au.route.input));
    return receiveBroadcast(c.p, false).then(() => pickInput({type: 'none'}));
  }
  if (c.kind === 'phone') return playerAction(c.p.path, a);
  if (c.kind !== 'board') return;
  const s = c.s;
  if (a === 'stop') {
    if (au.hearId === s.id) hearStop();
    return del(`/audio/streams/${s.id}`).then(refreshPlayerStreams).catch(fail);
  }
  if (s.source.type === 'capture' || (!isUrl(s) && a !== 'play' && a !== 'pause')) return;  // tracks: a URL stream's
  const body = a === 'play' ? {paused: false} : a === 'pause' ? {paused: true} : {[a]: true};
  // Heard here: the element pauses with the board, and plays again from a fresh connection (what
  // it buffered while paused is stale silence).
  if (au.hearId === s.id) {
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
    .catch(fail).finally(() => { api('/media').then(m => { au.media = m; renderNowPlaying(); }).catch(() => {}); });
}


// ---------------------------------------------------------------- hearing it in this browser

// What the board plays is heard in this browser through one <audio> element, not Web Audio fed by
// a script: a media element keeps playing with the page in the background (Android stops a page's
// scripts seconds after the home button), it buffers a few seconds against Wi-Fi hiccups, and with
// the Media Session it puts title, art and the transport keys on the lock screen. The spectrum taps
// it only once asked for (hearAnalyser).
const npAudio = new Audio();
npAudio.preload = 'none';
au.hearId = null;

function hear(id) {
  au.hearId = id;
  hearConnect();
  setMediaSession();
}

function hearConnect() {
  if (!au.hearId) return;
  au.heardAt = Date.now();
  npAudio.src = `/api/audio/streams/${au.hearId}/listen?c=${Date.now()}`;
  npAudio.play().catch(e => toast('This browser would not play: ' + e.message + ' (tap play)'));
}

function hearStop() {
  au.hearId = null;
  npAudio.pause();
  npAudio.removeAttribute('src');
  npAudio.load();
  if ('mediaSession' in navigator) navigator.mediaSession.playbackState = 'none';
}

// The connection dropped (the board restarted the stream, Wi-Fi went away) or the page was in the
// background long enough for the browser to let it go: connect again if the stream still plays.
function hearCheck() {
  if (!au.hearId) return;
  const s = au.streams.find(x => x.id === au.hearId);
  if (!s || (s.state !== 'running' && s.state !== 'starting')) { hearStop(); renderNowPlaying(); return; }
  if (s.paused) return;
  const stuck = npAudio.paused || npAudio.ended || npAudio.error || npAudio.readyState < 2;
  if (stuck && Date.now() - au.heardAt > 4000) hearConnect();
}
['error', 'ended', 'stalled'].forEach(ev => npAudio.addEventListener(ev, () => setTimeout(hearCheck, 1000)));
// "This browser is connecting…" turns into playing.
['playing', 'waiting'].forEach(ev => npAudio.addEventListener(ev, () => { if (currentTab === 'audio') renderNowPlaying(); }));
// Back from the background — shown again, thawed after the browser froze the page (Page
// Lifecycle "resume"), restored from the back/forward cache, or focused: look at once.
const hearBack = () => { au.heardAt = 0; hearCheck(); };
document.addEventListener('visibilitychange', () => { if (!document.hidden) hearBack(); });
document.addEventListener('resume', hearBack);
window.addEventListener('pageshow', hearBack);
window.addEventListener('focus', hearBack);

// The lock screen and the notification: what plays, and the keys, which act on the board.
function setMediaSession() {
  if (!('mediaSession' in navigator) || !au.hearId) return;
  const c = current();
  const title = $('np-title').textContent, artist = $('np-sub').textContent;
  const img = $('np-art').querySelector('img');
  const key = title + '|' + artist + '|' + (img ? img.src : '');
  if (au.msKey !== key) {
    au.msKey = key;
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
    const due = au.queueKey !== key ? 0 : au.queueOk ? Infinity : au.queueEmpty === 1 ? 2000 : au.queueEmpty ? 6000 : 8000;
    if (c.p.browsable && !au.queueBusy && Date.now() - au.queueAt > due) {
      au.queueBusy = true;
      au.queueAt = Date.now();
      post('/media/players/browse', {path: c.p.path, folder: c.p.path + '/NowPlaying'})
        .then(r => {
          const items = r.items || [];
          // An empty answer right after a track change can be the phone rebuilding its list (AVRCP
          // "UIDs changed": BlueZ drops the old items meanwhile), or the new list not there yet.
          // For a track of a list (it has a number) what is on screen stays, and the phone is
          // asked again 2 s later and 6 s after that; empty a third time, it is empty. A track
          // with no number (a test tone after a playlist) has no list.
          const numbered = ((c.p.track || {}).track_number || 0) > 0;
          au.queueEmpty = au.queueKey === key ? au.queueEmpty || 0 : 0;
          if (!items.length && numbered && au.queueEmpty < 2) {
            au.queueKey = key;
            au.queueEmpty++;
            au.queueOk = false;
            setTimeout(renderNowPlaying, au.queueEmpty === 1 ? 2100 : 6100);
            return;
          }
          au.queueItems = items;
          au.queuePlayer = c.p.path;
          au.queueKey = key;
          au.queueOk = true;
        })
        .catch(() => {
          if (au.queueKey !== key) au.queueEmpty = 0;
          au.queueKey = key;
          au.queueOk = false;
          setTimeout(renderNowPlaying, 8100);
        })
        .finally(() => { au.queueBusy = false; renderNowPlaying(); });
    }
    const cur = (c.p.track || {}).title;
    rows = au.queueItems.map((it, i) => {
      const t = it.title || (it.metadata && it.metadata.title) || it.name;
      const now = t && t === cur;
      return `<div class="tr${now ? ' now' : ''}" data-item="${esc(it.path)}">
        <span class="tr-n">${now ? svg(PL_SVG.play) : i + 1}</span>
        <span class="tr-t">${esc(t)}${it.artist ? ` <span class="muted">${esc(it.artist)}</span>` : ''}</span></div>`;
    }).join('');
    note = c.p.browsable || c.p.input ? '' : 'This player does not share its list';
  }
  setText($('np-q-note'), note);
  $('np-q-card').hidden = c.kind === 'none';
  const waiting = c.kind === 'phone' && c.p.browsable && au.queueBusy && !au.queueItems.length;
  setHTML($('np-queue'), rows || (waiting ? '<div class="empty-line loading-line"><span class="dots"></span>Getting the phone’s list…</div>'
    : `<div class="empty-line">${c.kind === 'none' ? 'Nothing playing: pick an input in Route, or something in Library.' : 'Nothing queued.'}</div>`));
}


// The rate the board decodes a track at: its own, in the family PipeWire's graph runs at (44.1 or
// 48 kHz) — the decoder resampling a 44.1 kHz MP3 to 48 kHz costs a Zero W three times the CPU of
// decoding it. Unknown (a radio station): 44.1 kHz, what nearly every MP3 stream is.
const streamRate = r => r && r % 48000 === 0 ? 48000 : 44100;

// The art and duration of the board's tracks, from what the library said about them.
function decorate(s) {
  (s.source.playlist || []).forEach(it => Object.assign(it, au.art[it.url] ? {art: it.art || au.art[it.url].art, duration_s: au.art[it.url].duration_s} : {}));
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
au.listen = lsGet('btbench.pl.here') === '1';
au.sw = null;  // a switch: {value, add, seq, phase: asking|removing|starting|linking|done|failed, at, error}

const outSink = v => v === 'none' || v === 'browser' ? null : v === 'default' ? {type: 'pipewire', target: ''}
  : {type: v.split(':')[0], target: v.slice(v.indexOf(':') + 1)};
// A value the route has keeps its sink as the route has it (a broadcast's left or right channel).
const outSinks = vs => vs.map(v => (au.route ? au.route.outputs : []).find(k => sinkOut(k) === v) || outSink(v)).filter(Boolean);
const sinkOut = k => k.target ? `${k.type}:${k.target}` : 'default';
// A stream's outputs, as values (a stream from before there were several has one "sink").
const streamOuts = s => (s.sinks || (s.sink ? [s.sink] : [])).map(sinkOut);
// The outputs after `v` was tapped with `cur` on: each one is added or taken off by itself.
function nextOuts(cur, v) {
  if (v === 'none') return [];
  return cur.includes(v) ? cur.filter(x => x !== v) : cur.filter(x => x !== 'none').concat(v);
}
const outEndpoint = (v) => au.endpoints && au.endpoints.sinks.find(e => v.endsWith(':' + e.id));
function outName(v) {
  if (v === 'none') return 'No board output';
  if (v === BCAST) return 'Auracast';
  if (v === 'browser') return 'This browser';
  if (v === 'default') return 'The board’s default output';
  const e = outEndpoint(v);
  if (e && isBcast(e)) return 'Auracast';
  if (e) return e.address ? devName(e.address) : e.label;
  // A Bluetooth sink PipeWire does not have (now): its device, by name.
  const bt = /bluez_output\.([0-9A-F]{2}(?:_[0-9A-F]{2}){5})/i.exec(v);
  return bt ? devName(bt[1].replace(/_/g, ':')) : v.slice(v.indexOf(':') + 1);
}
// The board's transport that carries a Bluetooth output's audio: its "Audio Source" side (a device
// can have one each way, see volumeTarget).
function outTransport(v) {
  const e = outEndpoint(v);
  if (!e || !e.address || !au.media) return null;
  const same = x => x.address && x.address.toUpperCase() === e.address.toUpperCase();
  return au.media.transports.find(x => same(x) && /source/i.test(x.profile || '')) || null;
}
// A stream's sink status for output `v`: {state, error, …}, or null.
const outStatusOf = (b, v) => (b && b.sinks_status || []).find(k => sinkOut(k) === v) || null;

function renderOutputs(ep) {
  au.endpoints = ep;
  renderNowPlaying();  // "Playing on …" names the output now that its label is known
}

// Auracast as an output: one button for the board's one broadcast, whose BISes (one, or a left and a
// right) are each a sink of the route. Tapped with none, a broadcast is started; tapped off, its
// sinks leave the outputs and the daemon ends it.
const BCAST = 'auracast';
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

// The outputs, in one list: this browser, the board's own sound cards, the Bluetooth speakers and
// earbuds (a coordinated set as one: PipeWire's node of the set, its members' own nodes left out),
// then the board's Auracast broadcast (one to start when there is none). The loopback card is left
// out: nobody hears it. An output the input rules out is shown off and cannot be tapped
// (outBlocked). Route shows them as rows.
function outOptions() {
  const ep = au.endpoints;
  const opts = [{value: 'browser', name: 'This browser', sub: 'listen here', icon: PL_SVG.browser, rank: 0}];
  const kind = ep && ep.backend === 'alsa' ? 'alsa' : 'pipewire';
  const input = au.route ? au.route.input : null;
  const sinks = (ep ? ep.sinks : []).filter(e => e.kind !== 'loopback');
  sinks.forEach(e => {
    if (e.set && !e.set_leader) return;
    if (isBcast(e)) return;
    const v = `${kind}:${e.id}`;
    const t = e.address ? outTransport(v) : null;
    const bcast = isBcast(e);
    const bt = e.kind === 'bluetooth' && !bcast;
    opts.push({value: v, name: e.set_leader ? ((e.members || []).length === 1 ? devName(e.members[0]) : e.label) : e.address ? devName(e.address) : e.label,
      sub: e.set_leader ? ((e.members || []).length > 1 ? `${e.members.length} devices, one each channel` : 'left + right') : e.address ? (t && t.codec ? t.codec.name : 'Bluetooth')
        : bcast ? 'Auracast' : 'the board’s sound card',
      icon: bt || bcast ? PL_SVG.bt : PL_SVG.speaker, rank: bt ? 2 : bcast ? 3 : 1, off: outBlocked(e, input)});
  });
  if (bcastAdapter() || sinks.some(isBcast)) {
    opts.push({value: BCAST, name: 'Auracast', sub: '', icon: PL_SVG.bt, rank: 3, off: outBlocked(null, input)});
  }
  return opts.sort((a, b) => a.rank - b.rank);
}
// The adapter that can broadcast (its broadcast endpoint registered by PipeWire), or null.
const bcastAdapter = () => (au.la ? au.la.adapters : []).find(a => a.broadcaster && a.pac) || null;

// When this view first saw an output's Bluetooth link in a state other than streaming while its
// sink plays (stream id|value → {state, at}). A link idle for LINK_MS failed: nobody acquired it, or
// BlueZ's Acquire failed (right after a device connected that is common). One pending is being
// acquired, which BlueZ gives up on after 25 s (seen on the bench): failed only past ACQUIRE_MS.
const LINK_MS = 8000, ACQUIRE_MS = 30000;
const linkSeen = {};
function linkLate(b, v, state) {
  const k = b.id + '|' + v;
  if (!state || state === 'active') { delete linkSeen[k]; return false; }
  if (!linkSeen[k] || linkSeen[k].state !== state) linkSeen[k] = {state, at: Date.now()};
  return Date.now() - linkSeen[k].at > (state === 'pending' ? ACQUIRE_MS : LINK_MS);
}

// What one of stream `b`'s outputs does, for its button: 'on' (it plays, and its Bluetooth link
// streams), 'busy' (opening, or its link starting) or 'bad' (it would not open, takes no audio, or
// its link did not stream).
function sinkState(b, v) {
  const ss = outStatusOf(b, v);
  const seq = b.sink_seq != null ? b.sink_seq : (b.sink_status || {}).seq;
  if (!ss) return b.sink_req != null && seq < b.sink_req ? 'busy' : 'bad';
  if (ss.state === 'starting') return 'busy';
  if (ss.state !== 'playing') return 'bad';
  // An A2DP or LE Audio output whose link BlueZ has not shown yet is waited for like an idle one
  // (HFP has no transport of BlueZ's to wait for).
  const e = outEndpoint(v), t = outTransport(v);
  const link = t ? t.state : e && e.address && /^(a2dp|bap)/.test(e.profile || '') ? 'none' : null;
  return linkLate(b, v, link) ? 'bad' : link && link !== 'active' ? 'busy' : 'on';
}

// An output's state, for its row in Route: lit only once it plays — while it opens and its link
// starts it shows the dots, and one that failed is not lit (tapping it again is the retry). With
// nothing playing (`b` null) an output on is only chosen, and is lit. The switch `sw` this browser
// asked for shows the dots until the board answered it.
function outState(v, cur, b, sw) {
  if (sw && sw.value === v && sw.phase === 'failed') return 'bad';
  if (sw && sw.value === v && sw.phase !== 'done') return 'busy';
  if (!(v === 'none' ? !cur.length : cur.includes(v))) return '';
  if (v === 'browser') return au.hearId && (npAudio.paused || npAudio.readyState < 3) ? 'busy' : 'on';
  if (!b || v === 'none') return 'on';
  const vs = v === BCAST ? routeOuts().filter(isBcastOut) : [v];
  const st = vs.map(x => sinkState(b, x));
  return st.includes('bad') ? 'bad' : st.includes('busy') ? 'busy' : 'on';
}

// The outputs on: the route's, and this browser when it listens.
const isBcastOut = v => { const e = outEndpoint(v); return !!e && isBcast(e); };
const curOuts = () => {
  const vs = routeOuts();
  return vs.filter(v => !isBcastOut(v)).concat(vs.some(isBcastOut) ? [BCAST] : [], au.listen ? ['browser'] : []);
};

// The switch under way followed (swAdvance), and the outputs drawn again (Route's rows).
function renderOuts() {
  const b = outStream();
  swAdvance(au.sw, b);
  // A switch that failed is over once its output plays after all (put back by another browser,
  // or by the board when the device came again).
  if (au.sw && au.sw.phase === 'failed' && b && curOuts().includes(au.sw.value) && outState(au.sw.value, curOuts(), b, null) === 'on') au.sw = null;
  // One this browser turned on that failed is taken off the outputs again: not lit is off, and the
  // next tap is the retry.
  const sw = au.sw;
  if (sw && sw.phase === 'failed' && sw.add && !sw.dropped && sw.value !== BCAST && routeOuts().includes(sw.value)) {
    sw.dropped = true;
    put('/audio/route', {outputs: outSinks(routeOuts().filter(x => x !== sw.value))}).then(setRoute).catch(() => refreshRoute());
  }
  if (currentTab === 'audio' && panes.audio === 'route' && !holding()) renderRouteOutputs();
}

// One of a stream's outputs in words, from its sink status `k`, for its row in Route: {cls, text}.
function sinkLine(k, b) {
  const v = sinkOut(k);
  if (k.state === 'starting') return {cls: 'busy', text: 'opening…'};
  if (k.state === 'failed') return {cls: 'bad', text: 'lost: ' + outReason(k.error)};
  const t = outTransport(v);
  if (k.state === 'stalled') return {cls: 'bad', text: 'takes no audio' + (t ? `, link ${t.state}` : '')};
  if (t && t.state !== 'active') {
    return b && linkLate(b, v, t.state) ? {cls: 'bad', text: `link did not stream (${t.state}): tap to retry`}
      : {cls: 'warn', text: `waiting for its link (${t.state})`};
  }
  const codec = t && t.codec ? `${t.codec.name}${t.codec.rate ? ' ' + t.codec.rate / 1000 + ' kHz' : ''}` : '';
  return {cls: 'good', text: 'playing' + (codec ? ' · ' + codec : '')};
}

// PipeWire's words for an output that is not there.
const outReason = e => /target not found/.test(e || '') ? 'it is not connected (no such output in PipeWire)' : (e || 'no reason given');

// The switch `sw` under way on stream `b`, moved on by what the stream says: once its sink_seq
// answers our request (seq) the pump has opened the output added (or failed to) or let the one
// taken off go; a Bluetooth one is done when its link streams (in au.media).
function swAdvance(sw, b) {
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
    const t = outTransport(sw.value);
    // A Bluetooth link takes a moment to start streaming; one that does not failed (linkLate: idle
    // for LINK_MS, or still being acquired past ACQUIRE_MS).
    if (t && linkLate(b, sw.value, t.state)) {
      sw.phase = 'failed';
      sw.error = `its Bluetooth link did not start streaming (it is ${t.state})`;
      return;
    }
    sw.phase = t && t.state !== 'active' ? 'linking' : 'done';
    return;
    return;
  }
  sw.phase = 'done';  // not among its sinks: nothing to follow
}

// An output tapped: added to the route's outputs or taken off them, and so to what plays now.
// "This browser" is this browser listening or not.
function pickOut(v) {
  if (v === 'browser') return pickListen(!au.listen);
  if (!au.route) return;
  if (v === BCAST) {
    const outs = routeOuts();
    if (outs.some(isBcastOut)) return put('/audio/route', {outputs: outSinks(outs.filter(x => !isBcastOut(x)))}).then(setRoute).catch(fail);
    // One that is not an output any more is being ended.
    if (au.endpoints && au.endpoints.sinks.some(isBcast)) return;
    return startBroadcastOut();
  }
  const b = outStream();
  // One on that failed (not lit) is tried again: taken off and put back, so the board opens it
  // anew (a sink that stalls or whose link did not stream is still open, and would be kept).
  const retry = !!b && routeOuts().includes(v) && outState(v, curOuts(), b, au.sw) === 'bad';
  const next = retry ? routeOuts() : nextOuts(routeOuts(), v);
  if (next.length > MAX_OUTS) { toast(`The board plays to at most ${MAX_OUTS} outputs at once.`); return; }
  const add = next.includes(v);
  const sw = b ? {value: v, add, phase: 'asking', at: Date.now(), seq: null} : null;
  au.sw = sw;
  au.route.outputs = outSinks(next);
  delete linkSeen[(b || {}).id + '|' + v];
  renderOuts();
  (retry ? put('/audio/route', {outputs: outSinks(next.filter(x => x !== v))}) : Promise.resolve())
    .then(() => put('/audio/route', {outputs: outSinks(next)})).then(r => {
    const u = r.updated;
    delete r.updated;
    au.route = r;
    if (sw && au.sw === sw) {
      if (u) {
        sw.seq = u.sink_req;
        sw.phase = add ? 'starting' : 'removing';
        const i = au.streams.findIndex(x => x.id === u.id);
        if (i >= 0) au.streams[i] = decorate(u);
      } else {
        sw.phase = 'done';
      }
    }
    renderNowPlaying();
  }).catch(e => {
    if (sw && au.sw === sw) { sw.phase = 'failed'; sw.error = e.message; }
    refreshRoute();
  });
  // The switch's result comes with the streams topic; this view asks too, in case it is slow.
  if (sw) [400, 1200, 2500, 5000, 9000].forEach(t => setTimeout(() => { if (au.sw === sw && sw.phase !== 'done') refreshPlayerStreams(); }, t));
}

// Auracast tapped while the board broadcasts nothing: a stereo broadcast of BAP's 48_2_1 (LE
// Audio's default) is started, which the daemon makes one of the route's outputs; tapped off
// like any output, the daemon ends it. Its settings, or a second broadcast, are the LE Audio view's.
function startBroadcastOut() {
  const a = bcastAdapter();
  if (!a || (au.sw && au.sw.phase !== 'done' && au.sw.phase !== 'failed')) return;
  const sw = au.sw = {value: BCAST, add: true, phase: 'broadcast', at: Date.now(), seq: null};
  renderOuts();
  post('/le-audio/broadcasts', {adapter: a.adapter, preset: '48_2_1', channels: 2}).then(r => {
    if (r.output_error) throw new Error(r.output_error);
    return Promise.all([api('/audio/endpoints?fresh=1').then(ep => { au.endpoints = ep; }), refreshLa(), refreshRoute()]);
  }).then(() => { if (au.sw === sw) au.sw = null; renderNowPlaying(); })
    .catch(e => { if (au.sw === sw) { sw.phase = 'failed'; sw.error = e.message; } renderOuts(); });
}

// "This browser" on or off: whatever the route plays is heard here, or not, and the choice is this
// browser's (it stays for the next input).
function pickListen(on) {
  au.listen = on;
  lsSet('btbench.pl.here', on ? '1' : '0');
  followRoute();
  renderNowPlaying();
}

// This browser follows the route: listening, it hears the route's stream, and a new input (or the
// same one recorded anew) is heard in its place.
function followRoute() {
  const id = au.route ? au.route.stream : null;
  if (au.hearId && au.hearId !== id) hearStop();
  if (au.listen && id && au.hearId !== id) hear(id);
  if (!au.listen && au.hearId) hearStop();
}

function setRoute(r) {
  delete r.updated;
  au.route = r;
  followRoute();
  renderNowPlaying();
}
const refreshRoute = () => api('/audio/route').then(setRoute).catch(() => {});
// Which adapter can broadcast and receive, for Auracast in the outputs and the inputs; asked again on
// "bt" (a scan starting or ending, a broadcaster found), at most once a second.
function refreshLa() {
  return api('/le-audio').then(s => { au.la = s; renderNowPlaying(); }).catch(() => {});
}

// The outputs come and go with Bluetooth: asked again whenever the board's audio links change,
// and once more a moment later (PipeWire makes a speaker's sink a little after BlueZ's transport).
function outLinksChanged(m) {
  const key = (m.transports || []).map(t => t.path).sort().join(',');
  if (key === au.linksKey) return;
  au.linksKey = key;
  const ask = () => api('/audio/endpoints?fresh=1').then(renderOutputs).catch(() => {});
  ask();
  refreshLa();
  setTimeout(ask, 2000);
}

function refreshPlayerStreams() {
  return api('/audio/streams').then(r => { au.streams = r.streams.map(decorate); noteFailure(); renderNowPlaying(); }).catch(() => {});
}

// What this view started and the board could not play (a decoder missing, a server refusing):
// said once, with the board's reason — otherwise the page would only fall back to "Nothing
// playing" without a word.
function noteFailure() {
  const s = au.streams.find(x => x.id === au.boardId);
  if (!s || s.state !== 'failed' || au.failedShown === s.id) return;
  au.failedShown = s.id;
  const why = /mpg123/.test(s.error) && /\.(flac|wav|m4a|aac|ogg|opus)\b/i.test((s.source.playlist[s.track] || {}).url || s.source.url)
    ? 'the board has no decoder for this format (mpg123 plays MP3 only; other formats need ffmpeg in the image, BTBENCH_FFMPEG)'
    : s.error.replace(/\[\/usr\/src\/debug\/[^\]]*\]\s*/g, '');
  toast('Could not play “' + (s.source.title || 'this') + '”: ' + why);
}


// ---------------------------------------------------------------- devices and players

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

// A device's name: from the device list, else from what the media state says about it (a view
// that shows players gets the name with them, without waiting for the device list).
const devName = addr => {
  const d = typeof btState !== 'undefined' && btState && btState.devices.find(x => x.address === addr);
  if (d) return d.alias;
  const m = au.media;
  const o = m && m.players.concat(m.transports).find(x => x.address === addr && x.device_name);
  return o ? o.device_name : addr;
};

// The cover-art (BIP) channel to a player's device: the phone sends image handles only while one
// is open, so it is opened once per connection (a player that goes away is forgotten, see
// notePlayers), again 10 s after a failure; the next track info then carries a handle.
// au.cover[path] is {state: asking | open | failed, at}: `at` is when it opened, 0 when it was
// open already (a player BlueZ made again); `changed` is called when the answer comes.
function coverConnect(p, changed) {
  const c = au.cover[p.path];
  if (!c || (c.state === 'failed' && Date.now() - c.at > 10000)) {
    const e = au.cover[p.path] = {state: 'asking', at: Date.now()};
    const done = (state, at) => { e.state = state; e.at = at; if (changed) changed(); };
    fetch('/api/media/players/cover?path=' + encodeURIComponent(p.path))
      .then(r => r.ok ? r.json().catch(() => ({})) : Promise.reject())
      .then(j => done('open', j.channel === 'open' ? 0 : Date.now()), () => done('failed', Date.now()));
  }
}

function playerAction(path, action) {
  return post('/media/players/control', {path, action}).catch(fail);
}

// ---- the codec picker: PipeWire's profile of a Bluetooth device is its codec choice

const CD_KINDS = [['a2dp', 'Send (A2DP)'], ['a2dp-duplex', 'Both ways (A2DP duplex)'], ['gateway', 'Receive A2DP / HFP gateway'],
  ['headset', 'Headset (HFP/HSP)'], ['bap', 'LE Audio (BAP)'], ['other', 'Other'], ['off', 'Off']];

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

// The cards, for the codec pickers in Route and PipeWire: read when those are shown and when a
// device's links change (onMessage), not with every track position.
const refreshCodecs = () => api('/audio/devices').then(r => { au.devices = r; renderRoute(); })
  .catch(e => { au.devices = {devices: [], error: e.message}; renderRoute(); });
const cardOf = address => ((au.devices && au.devices.devices) || []).find(d => sameAddr(d.address, address)) || null;

// A codec picked (Route, PipeWire): the link renegotiates, and the outputs follow (a switch makes
// the device's node anew).
function pickCodec(sel, then) {
  sel.disabled = true;
  put(`/audio/devices/${sel.dataset.id}/profile`, {profile: parseInt(sel.value, 10)})
    .then(d => { if (d.switch_error) toast(d.switch_error); })
    .catch(fail)
    .finally(() => {
      sel.disabled = false;
      refreshCodecs();
      api('/audio/endpoints?fresh=1').then(renderOutputs).catch(() => {});
      if (then) then();
    });
}

// ---- a Bluetooth link in words

function codecCell(c) {
  return `<b>${esc(c.name)}</b> <span class="small">${esc(c.summary)}</span>`;
}
// The board's own role on a transport is its UUID: Audio Sink (or a sink PAC, or a broadcast it
// syncs to: Basic Audio Announcement) means the board receives from the device.
const receives = t => /^(110b|2bc9|1851)$/i.test(t.uuid);
// What kind of link a transport is, in words: BlueZ's UUID names are the board's PAC or role.
const LINK_KIND = {'110a': 'A2DP', '110b': 'A2DP', '2bc9': 'LE Audio · CIS', '2bcb': 'LE Audio · CIS',
  '1851': 'Auracast · BIS', '1852': 'Auracast · BIS'};
const linkKind = t => LINK_KIND[(t.uuid || '').toLowerCase()] || t.profile || t.uuid;

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

// ---- the spectrum of what this browser hears

// The <audio> element goes through Web Audio only once someone opens the spectrum: until then it
// plays as a plain media element, which keeps playing with the page in the background (see
// npAudio). From then on, for this page, an analyser sits between it and the speakers.
function hearAnalyser() {
  if (au.hearCtx) return au.hearCtx;
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return null;
  const ctx = new AC();
  const analyser = ctx.createAnalyser();
  analyser.fftSize = 2048;
  ctx.createMediaElementSource(npAudio).connect(analyser);
  analyser.connect(ctx.destination);
  au.hearCtx = {ctx, analyser};
  return au.hearCtx;
}

// One requestAnimationFrame loop while the spectrum is open and on screen.
function drawHearing() {
  if (drawHearing.on) return;
  drawHearing.on = true;
  const frame = () => {
    const cv = document.querySelector('#rt-outputs .rt-spec');
    const open = cv && cv.closest('details').open && currentTab === 'audio' && panes.audio === 'route';
    if (!open || !au.hearCtx) { drawHearing.on = false; return; }
    if (au.hearCtx.ctx.state === 'suspended') au.hearCtx.ctx.resume().catch(() => {});
    drawSpectrum(cv, au.hearCtx);
    const b = npAudio.buffered;
    const ahead = b.length ? b.end(b.length - 1) - npAudio.currentTime : 0;
    setText(document.querySelector('#rt-outputs .rt-buf'), au.hearId ? `${(ahead * 1000).toFixed(0)} ms buffered` : '');
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}

// Log frequency axis 20 Hz .. Nyquist, -100..0 dB. `l`: {ctx, analyser}.
function drawSpectrum(cv, l) {
  const dpr = window.devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight;
  if (!w) return;
  if (cv.width !== Math.round(w * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
  const g = cv.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  const bins = new Float32Array(l.analyser.frequencyBinCount);
  l.analyser.getFloatFrequencyData(bins);
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

// ---------------------------------------------------------------- wiring

function setupAudio() {
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
    if (p) au.players[p.address.toUpperCase()] = p.path;
    au.queueKey = '';
    renderNowPlaying();
  };
  $('np-vol').oninput = e => {
    clearTimeout(au.volT);
    // A slider sends many values a second: at most one request each 150 ms.
    au.volT = setTimeout(() => setVolume(parseInt(e.target.value, 10)), 150);
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
}

// Not while a slider, a picker or a number of the view is held: the rebuild would take it from
// under the pointer. Route lets go of them once they changed (setupRoute).
const holding = () => {
  const ae = document.activeElement;
  return !!ae && ae.closest && !!ae.closest('#audio') && (ae.type === 'range' || ae.type === 'number' || ae.tagName === 'SELECT');
};

registerTab('audio', {
  topics: () => ['media', 'audio.streams', 'audio.route', 'bt'],
  show() {
    // The library opens once the players are known: a browsable phone's is the first tab.
    // A folder still open from before (Back to this view, or a look at another tab) stays as it was
    // left, with its Back entries; a phone's only while the phone is still there.
    const kept = () => au.libPath.length && au.libItems.length && (au.libPath[0].parent || {}).lib === au.lib &&
      (au.lib !== 'phone' || phones().some(x => x.browsable));
    api('/media').then(m => { au.media = m; au.linksKey = (m.transports || []).map(t => t.path).sort().join(','); notePlayers(m.players); renderNowPlaying(); renderLibTabs(); if (!kept()) openLib(au.lib); })
      .catch(() => { au.mediaFailed = true; renderNowPlaying(); renderLibTabs(); if (!kept()) openLib(au.lib); });
    api('/bluetooth').then(b => { btState = b; renderNowPlaying(); }).catch(() => {});
    api('/audio/endpoints').then(renderOutputs).catch(() => { au.endpointsFailed = true; renderOuts(); });
    refreshLa();
    refreshPlayerStreams();
    refreshRoute();
    if (panes.audio === 'route') routeShown();
    clearInterval(au.tick);
    // The progress bar moves between the phone's updates.
    au.tick = setInterval(() => {
      if (++au.ticks % 6 === 0) hearCheck();  // every 3 s
      const c = current();
      if (c.kind === 'phone' && c.p.status === 'playing') renderTime(playerPosition(c.p), (c.p.track || {}).duration_ms || 0);
    }, 500);
  },
  hide() {
    clearInterval(au.tick);
    au.tick = null;
    routeHidden();
  },
  pane(p) {
    if (currentTab !== 'audio') return;
    // HFP's and a set's nodes are only in the endpoints, which follow the transports otherwise;
    // the codec pickers are read when they are shown.
    if (p === 'route') {
      api('/audio/endpoints?fresh=1').then(renderOutputs).catch(() => {});
      routeShown();
    } else {
      routeHidden();
    }
  },
  onMessage(topic, data) {
    if (currentTab !== 'audio') return;
    if (topic === 'media') {
      const hadLib = phones().some(p => p.browsable);
      au.media = data;
      notePlayers(data.players);
      outLinksChanged(data);
      if (hadLib !== phones().some(p => p.browsable)) renderLibTabs();
      // The codecs change when a device's links do, not with every track position.
      const key = data.transports.map(t => t.path + t.state + t.configuration).join();
      if (key !== au.cdKey && panes.audio === 'route') {
        au.cdKey = key;
        clearTimeout(au.cdTimer);
        au.cdTimer = setTimeout(refreshCodecs, 1500);
      }
      if (!holding()) renderNowPlaying();
    } else if (topic === 'bt') {
      // "bt" carries every RSSI change during a scan.
      if (!au.laT) au.laT = setTimeout(() => { au.laT = null; refreshLa(); }, 1000);
    } else if (topic === 'audio.route') {
      au.route = data;
      followRoute();
      if (!holding()) renderNowPlaying();
    } else if (topic === 'audio.streams') {
      au.streams = data.streams.map(decorate);
      noteFailure();
      if (!holding()) renderNowPlaying();
      renderRouteMeters();
    }
  },
});
setupAudio();
