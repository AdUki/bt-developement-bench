'use strict';
// Listen: the console as a music player rather than a bench. One "now playing" for whichever
// source is playing — a connected phone (its AVRCP player: track, cover, controls, the link's
// volume, its now-playing list and its library) or the board itself (internet radio, or tracks
// from a UPnP media server as a playlist the board plays to the end) — with an output to play to:
// a speaker the board has, or this browser. The bench views (Audio) stay for everything else.
//
// The board's playlist lives in the daemon (a URL stream's playlist): closing the page does not
// stop the music, and another phone opening the page sees the same queue.

const pl = {
  media: null,         // the "media" topic: players, transports
  streams: [],         // the "audio.streams" topic
  endpoints: null,     // the board's outputs
  src: lsGet('btbench.pl.src') || '',   // 'board' or a player's path; '' = whichever plays
  boardId: null,       // the stream this view started
  lib: lsGet('btbench.pl.lib') || '',  // phone | radio | upnp; '' = the phone's when there is one
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
  server: 'M4 4h16v6H4zM4 14h16v6H4zM8 7h.01M8 17h.01',
  phone: 'M8 3h8a1 1 0 011 1v16a1 1 0 01-1 1H8a1 1 0 01-1-1V4a1 1 0 011-1zM11 18h2',
  folder: 'M4 6a2 2 0 012-2h4l2 2h6a2 2 0 012 2v10a2 2 0 01-2 2H6a2 2 0 01-2-2z',
  plus: 'M12 5v14M5 12h14',
  remove: 'M6 6l12 12M18 6L6 18',
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

// The board's stream this view shows: the one it started if it still runs, else any URL stream.
function boardStream() {
  const live = s => s.state === 'running' || s.state === 'starting';
  const urls = pl.streams.filter(s => s.source.type === 'url' && live(s));
  return urls.find(s => s.id === pl.boardId) || urls[0] || null;
}
// The players worth showing: BlueZ also makes an empty one for a phone's LE Audio media control
// service (mcp_gmcs: no name, never a track), which would be a second, dead chip for the phone. An
// AVRCP player is always one: right after pairing, before the phone has played anything, it has no
// name and no track yet, and its play button is how the music gets started from here.
const phones = () => (pl.media ? pl.media.players : []).filter(p => p.name || p.browsable || (p.track && p.track.title) ||
  /\/avrcp\//.test(p.path));

// The source on screen: the one chosen if it is still there, else the board if it plays, else a
// phone that plays, else any phone.
function current() {
  const b = boardStream();
  const ps = phones();
  if (pl.src === 'board' && b) return {kind: 'board', s: b};
  const chosen = ps.find(p => p.path === pl.src);
  if (chosen) return {kind: 'phone', p: chosen};
  if (b) return {kind: 'board', s: b};
  const playing = ps.find(p => p.status === 'playing');
  if (playing || ps[0]) return {kind: 'phone', p: playing || ps[0]};
  return {kind: 'none'};
}

// ---------------------------------------------------------------- now playing

function renderNowPlaying() {
  const c = current();
  const b = boardStream();
  // Source chips: every phone, and the board when it plays.
  // A chip names the device and the player in it: one phone can have several (Samsung Music,
  // Spotify), and the device's name alone would make them look the same.
  const chips = phones().map(p => ({key: p.path, label: (p.device_name || devName(p.address)) + (p.name ? ' · ' + p.name : ''),
    icon: PL_SVG.bt, on: c.kind === 'phone' && c.p.path === p.path}));
  if (b) chips.push({key: 'board', label: 'This board', icon: PL_SVG.radio, on: c.kind === 'board'});
  setHTML($('np-src'), chips.length > 1 ? chips.map(x =>
    `<button class="chip${x.on ? ' on' : ''}" data-src="${esc(x.key)}">${svg(x.icon)}${esc(x.label)}</button>`).join('') : '');

  let artSrc = '', artLoading = false, ph = PL_SVG.note;
  let art = svg(ph, 'ph'), title = 'Nothing playing', sub = '', sub2 = '', playing = false, pos = 0, dur = 0;
  let can = {prev: false, next: false, play: false, stop: false};
  if (c.kind === 'phone') {
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
      coverConnect(p);
      // The phone sends a cover's handle only once its image channel is up: a few seconds after
      // a track starts (or the board connects). Shown as loading for that long, then as no cover.
      const k = p.path + '|' + (t.title || '');
      if (!pl.artWait[k]) pl.artWait[k] = Date.now();
      artLoading = Date.now() - pl.artWait[k] < 12000;
    }
    can = {prev: true, next: true, play: true, stop: true};
  } else if (c.kind === 'board') {
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
  }
  // An image: loading until it arrived (or failed: then the note stays).
  if (artSrc) {
    art = svg(ph, 'ph') + `<img alt="" src="${esc(artSrc)}" onload="npArtDone(this, true)" onerror="npArtDone(this, false)"${pl.artDone[artSrc] === true ? '' : ' hidden'}>`;
    artLoading = pl.artDone[artSrc] === undefined;
  }
  setHTML($('np-art'), art);
  $('np-art').classList.toggle('loading', artLoading);
  setText($('np-title'), title);
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
    (c.kind === 'phone' ? modeButtons('np-btn', btRepeat(c.p.repeat), c.p.shuffle == null ? null : c.p.shuffle !== 'off')
      : c.kind === 'board' && (c.s.source.playlist || []).length > 1 ? modeButtons('np-btn', boardRepeat(c.s.source.repeat), !!c.s.source.shuffle) : '') +
    (status ? pill(status, playing ? 'good' : status === 'paused' ? 'warn' : '') : '') + '</div>');
  // The library page's bar: the track, and play/pause and next, to skip without leaving the list.
  $('np-mini').hidden = c.kind === 'none';
  setText($('np-mini-title'), title);
  setText($('np-mini-sub'), sub || sub2);
  setHTML($('np-mini-ctl'), btn(playing ? 'pause' : 'play', playing ? PL_SVG.pause : PL_SVG.play, can.play) + btn('next', PL_SVG.next, can.next));
  setHTML($('np-from'), npFrom(c));
  renderTime(pos, dur);
  renderVolume(c);
  setMediaSession();
  setText($('np-hear'), pl.hearBusy ? 'Connecting…' : pl.hearId ? 'Stop listening here' : 'Listen in this browser');
  $('np-hear').disabled = !!pl.hearBusy;
  // The output only means something for what the board plays.
  $('np-out-row').hidden = c.kind === 'phone';
  $('np-hear').hidden = c.kind !== 'phone';
  renderQueue(c);
}

// Where the sound comes from, said above the title: a Bluetooth device playing into the board (and
// whether its audio actually streams in, or only its player is known), or the board itself playing
// a station or a media server's tracks, and to which output.
function npFrom(c) {
  const tag = (icon, cls, text) => `<span class="np-tag ${cls}">${svg(icon)}${text}</span>`;
  if (c.kind === 'phone') {
    const ts = (pl.media ? pl.media.transports : []).filter(t => t.address === c.p.address && /^(110b|2bc9)$/i.test(t.uuid));
    const live = ts.some(t => t.state === 'active');
    return tag(PL_SVG.bt, 'bt', `Bluetooth · from ${esc(c.p.device_name || devName(c.p.address))}`) +
      `<span class="np-state ${live ? 'good-text' : ''}">${live ? 'streaming into the board' : ts.length ? 'connected, not streaming' : 'remote control only, no audio link'}</span>`;
  }
  if (c.kind === 'board') {
    const s = c.s, list = s.source.playlist || [];
    const item = list[s.track] || {};
    const what = list.length > 1 || item.duration_ms || item.duration_s ? 'Media server' : 'Internet radio';
    return tag(what === 'Media server' ? PL_SVG.server : PL_SVG.radio, 'local', `On this board · ${what}`) +
      `<span class="np-state">→ ${s.sink ? esc(outLabel(s.sink.target)) : 'this browser'}</span>`;
  }
  return '';
}

// Repeat or shuffle pressed: the phone's player's setting, or the board's playlist's.
function npMode(mode) {
  const c = current();
  if (c.kind === 'phone') {
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
// one, else the stream's gain; in this browser, the browser's.
function volumeTarget(c) {
  const ts = pl.media ? pl.media.transports : [];
  if (c.kind === 'phone') {
    const t = ts.find(x => x.path === c.p.transport);
    return t && t.volume != null ? {kind: 'transport', path: t.path, value: Math.round(t.volume / 127 * 100)} : null;
  }
  if (c.kind === 'board') {
    const s = c.s;
    if (!s.sink) return pl.hearId === s.id ? {kind: 'browser', value: Math.round(npAudio.volume * 100)} : null;
    const ep = pl.endpoints && pl.endpoints.sinks.find(e => e.id === s.sink.target);
    // A device can have a transport each way (A2DP sink and source): the one the board sends on is
    // its "Audio Source" side — the other is what the device would send the board.
    const same = x => ep && ep.address && x.address && x.address.toUpperCase() === ep.address.toUpperCase() && x.volume != null;
    const t = ts.find(x => same(x) && /source/i.test(x.profile || '')) || ts.find(same);
    if (t) return {kind: 'transport', path: t.path, value: Math.round(t.volume / 127 * 100)};
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
  if (v.kind === 'transport') put('/media/transport', {path: v.path, volume: Math.round(value / 100 * 127)}).catch(fail);
  else if (v.kind === 'browser') npAudio.volume = value / 100;
  // 0..100 as -60..0 dB on a square-law curve: the slider's middle is about half as loud.
  else put(`/audio/streams/${v.id}`, {gain_db: value <= 0 ? -60 : Math.max(-60, 40 * Math.log10(value / 100))}).catch(fail);
}

function npAction(a) {
  const c = current();
  if (c.kind === 'phone') return playerAction(c.p.path, a);
  if (c.kind !== 'board') return;
  const s = c.s;
  if (a === 'stop') {
    hearStop();
    return del(`/audio/streams/${s.id}`).catch(fail);
  }
  const body = a === 'play' ? {paused: false} : a === 'pause' ? {paused: true} : {[a]: true};
  // Heard here: the element pauses with the board, and plays again from a fresh connection (what
  // it buffered while paused is stale silence).
  if (pl.hearId === s.id) {
    if (a === 'pause') npAudio.pause();
    if (a === 'play') hearConnect();
  }
  put(`/audio/streams/${s.id}`, body).catch(fail);
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
    if (list.length > 1) {
      rows = list.map((it, i) => `<div class="tr${i === s.track ? ' now' : ''}" data-track="${i}">
          <span class="tr-n">${i === s.track ? svg(PL_SVG.play) : i + 1}</span>
          <span class="tr-t">${esc(it.title || it.url)}</span></div>`).join('');
      note = `${list.length} tracks`;
    } else {
      note = 'A radio stream';
    }
  } else if (c.kind === 'phone') {
    // The phone's now-playing list: read when the track changes, and retried a little later after
    // a failure (a phone answers browsing slowly, or not at all, for a while after it connects).
    // Not re-read on a timer: every read moves the phone's browsing from the library's folder to
    // Now Playing, which drops the items BlueZ holds for the other, and on some phones (an S25)
    // costs 10 s during which the library waits.
    const key = c.p.path + '|' + ((c.p.track || {}).title || '');
    const due = pl.queueKey !== key ? 0 : pl.queueOk ? Infinity : 8000;
    if (c.p.browsable && !pl.queueBusy && Date.now() - pl.queueAt > due) {
      pl.queueBusy = true;
      pl.queueAt = Date.now();
      post('/media/players/browse', {path: c.p.path, folder: c.p.path + '/NowPlaying'})
        .then(r => {
          const items = r.items || [];
          // An empty answer right after a track change is the phone rebuilding its list (AVRCP
          // "UIDs changed": BlueZ drops the old items meanwhile). The list on screen stays, and
          // the phone is asked again in a few seconds instead of the queue going blank.
          if (!items.length && pl.queueItems.length && pl.queuePlayer === c.p.path) {
            pl.queueKey = key;
            pl.queueOk = false;
            return;
          }
          pl.queueItems = items;
          pl.queuePlayer = c.p.path;
          pl.queueKey = key;
          pl.queueOk = true;
        })
        .catch(() => { pl.queueKey = key; pl.queueOk = false; })
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
    note = c.p.browsable ? '' : 'This player does not share its list';
  }
  setText($('np-q-note'), note);
  const waiting = c.kind === 'phone' && c.p.browsable && pl.queueBusy && !pl.queueItems.length;
  setHTML($('np-queue'), rows || (waiting ? '<div class="empty-line loading-line"><span class="spin"></span>Getting the phone’s list…</div>'
    : `<div class="empty-line">${c.kind === 'none' ? 'Pick something from the library.' : 'Nothing queued.'}</div>`));
}

// ---------------------------------------------------------------- the library

function renderLibTabs() {
  const tabs = [['radio', 'Radio'], ['upnp', 'Media servers']];
  if (phones().some(p => p.browsable)) tabs.unshift(['phone', 'Phone']);
  if (!tabs.some(t => t[0] === pl.lib)) pl.lib = tabs[0][0];  // the phone's first, when browsable
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
  if (kind === 'radio') {
    setHTML($('np-crumbs'), '');
    api('/audio/radio').then(renderStations)
      .catch(e => setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`));
  } else if (kind === 'upnp') {
    pl.upnpServer = null;
    upnpServers(false);
  } else if (phone) {
    phoneFolder(phone, phone.path + '/Filesystem', 'Files');
  }
}

// The stations, each with a remove, and under them a station to add by its URL. The board keeps the
// whole list (PUT /audio/radio), the one Audio's "Edit stations" edits too.
function renderStations(r) {
  pl.libItems = r.stations.map(s => ({kind: 'station', title: s.name, url: s.url}));
  renderLib('<div class="empty-line">No stations yet.</div>');
  $('np-lib').insertAdjacentHTML('beforeend', `<form class="radio-add row wrap" id="np-radio-add">
    <input type="url" name="url" class="grow" placeholder="Stream URL (MP3, AAC, .pls, .m3u)" required>
    <input type="text" name="name" placeholder="Name (optional)" maxlength="120">
    <button class="small-btn">${svg(PL_SVG.plus)} Add station</button></form>`);
}

// The list as the board has it now, changed by `edit`, saved, and shown.
function stationsSave(edit) {
  return api('/audio/radio').then(r => put('/audio/radio', {stations: edit(r.stations)}))
    .then(r => { if (pl.lib === 'radio') renderStations(r); return r; });
}

function stationAdd(f) {
  const url = f.url.value.trim(), name = f.name.value.trim();
  if (!url) return;
  stationsSave(list => list.concat([{name: name || url, url}])).then(() => toast(`Added ${name || url}`)).catch(fail);
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
  setHTML($('np-lib'), `<div class="empty-line loading-line"><span class="spin"></span>${search ? 'Searching the network…' : 'Loading…'}</div>`);
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
  setHTML($('np-lib'), '<div class="empty-line loading-line"><span class="spin"></span>Loading…</div>');
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
    : `<div class="empty-line loading-line lib-more"><span class="spin"></span>${pl.libItems.length.toLocaleString()}${of} shown, loading more…</div>`);
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
  setHTML($('np-lib'), '<div class="empty-line loading-line"><span class="spin"></span>Loading…</div>');
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

function renderLib(empty) {
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
      <div class="row"><button class="primary small-btn" data-lib-all="play">${svg(PL_SVG.play)} Play all</button>
        <button class="small-btn" data-lib-all="add" title="Add every track to the queue">${svg(PL_SVG.plus)} Add all</button></div></div></div>`;
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
    setHTML($('np-lib'), head + (libRowsHTML(items, 0, inAlbum) || empty || '<div class="empty-line">Empty.</div>'));
  }
  libMoreLine();
}

const libIcons = () => ({station: PL_SVG.radio, server: PL_SVG.folder, folder: PL_SVG.folder, pfolder: PL_SVG.folder, track: PL_SVG.note, ptrack: PL_SVG.note});

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
    // Inside an album the tracks show their number (the cover is at the top); elsewhere their cover.
    const lead = it.kind === 'track' && inAlbum && it.track != null ? `<span class="tr-n">${it.track}</span>`
      : `<span class="tr-n tr-art">${libArt(it, icon[it.kind])}</span>`;
    return `<div class="tr lib" data-lib-i="${i}">
      ${lead}
      <span class="tr-t">${esc(it.title)}${it.sub && !(inAlbum && it.kind === 'track') ? `<span class="muted">${esc(it.sub)}</span>` : ''}</span>
      ${it.format ? `<span class="tr-fmt">${esc(it.format)}</span>` : ''}
      ${it.duration_s ? `<span class="tr-d">${fmtMs(it.duration_s * 1000)}</span>` : ''}
      ${add ? `<button class="icon-btn" data-lib-add="${i}" title="Add to the queue" aria-label="Add to the queue">${svg(PL_SVG.plus)}</button>` : ''}
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

// Play every track of the folder, or add them all to the queue.
function libAll(add) {
  const tracks = pl.libItems.filter(x => x.kind === 'track');
  if (!tracks.length) return;
  const items = tracks.map(x => ({url: x.url, title: x.title, art: x.art, duration_s: x.duration_s, artist: x.artist, album: x.album, rate: x.rate}));
  const b = boardStream();
  if (add && b) {
    items.forEach(t => { pl.art[t.url] = t; });
    return put(`/audio/streams/${b.id}`, {enqueue: items.map(playlistItem)})
      .then(() => toast(`Added ${items.length} tracks to the queue`)).catch(fail);
  }
  playOnBoard(items);
}

// A library row: open a folder, or play — a station on its own, a track with the rest of its
// folder after it (as the queue), a phone's item on the phone.
function libClick(i, add) {
  const it = pl.libItems[i];
  if (!it) return;
  if (it.kind === 'server') return upnpFolder(it.id, '0', it.title);
  if (it.kind === 'folder') return upnpFolder(pl.upnpServer, it.id, it.title, true, it);
  if (it.kind === 'pfolder') return phoneFolder(phones().find(p => p.path === it.player), it.path, it.title);
  if (it.kind === 'ptrack') {
    if (!it.playable) return;
    return post('/media/items', {path: it.path, action: add ? 'add' : 'play'})
      .then(() => { toast(add ? 'Added to the phone’s queue' : 'Playing on the phone'); pl.src = it.player; pl.queueAt = 0; }).catch(fail);
  }
  if (it.kind === 'station') return playOnBoard([{url: it.url, title: it.title}]);
  const tracks = pl.libItems.filter(x => x.kind === 'track');
  const from = tracks.indexOf(it);
  const items = (add ? [it] : tracks.slice(from)).map(x => ({url: x.url, title: x.title, art: x.art, duration_s: x.duration_s,
    artist: x.artist, album: x.album, rate: x.rate}));
  // "+" on a track while the board plays: onto the end of its queue.
  const b = boardStream();
  if (add && b) {
    items.forEach(t => { pl.art[t.url] = t; });
    return put(`/audio/streams/${b.id}`, {enqueue: items.map(playlistItem)})
      .then(() => toast('Added to the queue')).catch(fail);
  }
  playOnBoard(items);
}

// A track as the board's playlist keeps it: what the board's player tells a car or speaker over
// AVRCP (artist, album, length) goes along; the cover stays here (pl.art), by URL.
const playlistItem = it => ({url: it.url, title: it.title, artist: it.artist || '', album: it.album || '',
  duration_ms: it.duration_s ? Math.round(it.duration_s * 1000) : null});

// What the board plays from the library: one stream with the tracks as its playlist, to the
// output chosen. The art a UPnP server gave is kept here by URL (the daemon does not keep it).
function playOnBoard(items) {
  items.forEach(it => { pl.art[it.url] = it; });
  const out = $('np-out').value;
  const listen = out === 'browser';
  const sink = listen ? null : out === 'default' ? {type: 'pipewire', target: ''} : {type: out.split(':')[0], target: out.slice(out.indexOf(':') + 1)};
  const old = boardStream();
  const go = () => {
    return post('/audio/streams', {source: {type: 'url', url: items[0].url, title: items[0].title, playlist: items.map(playlistItem)},
      sink, rate: streamRate(items[0].rate), channels: 2, label: items.length > 1 ? items[0].title + ' and ' + (items.length - 1) + ' more' : items[0].title})
      .then(s => {
        pl.boardId = s.id;
        pl.src = 'board';
        lsSet('btbench.pl.src', 'board');
        if (listen) hear(s.id);
        refreshPlayerStreams();
      }).catch(fail);
  };
  // One thing at a time from here: what the board played before stops.
  if (old) {
    hearStop();
    del(`/audio/streams/${old.id}`).catch(() => {}).then(go);
  } else {
    go();
  }
}
pl.art = {};

// The rate the board decodes a track at: its own, in the family PipeWire's graph runs at (44.1 or
// 48 kHz) — the decoder resampling a 44.1 kHz MP3 to 48 kHz costs a Zero W three times the CPU of
// decoding it. Unknown (a radio station): 44.1 kHz, what nearly every MP3 stream is.
const streamRate = r => r && r % 48000 === 0 ? 48000 : 44100;

// The art and duration of the board's tracks, from what the library said about them.
function decorate(s) {
  (s.source.playlist || []).forEach(it => Object.assign(it, pl.art[it.url] ? {art: pl.art[it.url].art, duration_s: pl.art[it.url].duration_s} : {}));
  return s;
}

// ---------------------------------------------------------------- outputs

const outLabel = id => {
  const e = pl.endpoints && pl.endpoints.sinks.find(x => x.id === id);
  return e ? (e.address ? devName(e.address) : e.label) : id ? id : 'the default output';
};

function renderOutputs(ep) {
  pl.endpoints = ep;
  const opts = [{value: 'browser', label: 'This browser'}];
  if (ep.backend === 'pipewire') opts.push({value: 'default', label: 'The board’s default output'});
  const kind = ep.backend === 'alsa' ? 'alsa' : 'pipewire';
  ep.sinks.forEach(e => opts.push({value: `${kind}:${e.id}`, label: e.address ? `${devName(e.address)} (Bluetooth)` : e.label}));
  fillSelect($('np-out'), opts, idle($('np-out')) ? (lsGet('btbench.pl.out') || 'browser') : null);
  renderNowPlaying();  // "Playing on …" names the output now that its label is known
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
    pl.src = b.dataset.src;
    lsSet('btbench.pl.src', pl.src);
    pl.queueKey = '';
    renderNowPlaying();
  };
  $('np-vol').oninput = e => {
    clearTimeout(pl.volT);
    // A slider sends many values a second: at most one request each 150 ms.
    pl.volT = setTimeout(() => setVolume(parseInt(e.target.value, 10)), 150);
  };
  $('np-out').onchange = e => lsSet('btbench.pl.out', e.target.value);
  $('np-hear').onclick = () => {
    const c = current();
    if (c.kind !== 'phone' || pl.hearBusy) return;
    // A toggle: the second press stops it.
    if (pl.hearId) {
      const id = pl.hearId;
      hearStop();
      renderNowPlaying();
      return del(`/audio/streams/${id}`).catch(() => {});
    }
    pl.hearBusy = true;
    renderNowPlaying();
    listenToDevice(c.p.address, s => hear(s.id)).finally(() => { pl.hearBusy = false; renderNowPlaying(); });
  };
  $('np-queue').onclick = e => {
    const r = e.target.closest('.tr');
    if (!r) return;
    const c = current();
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
  $('np-lib').onclick = e => {
    if (e.target.closest('[data-lib-search]')) return upnpServers(true);
    if (e.target.closest('[data-lib-more]')) return libMoreLoad();
    const all = e.target.closest('[data-lib-all]');
    if (all) return libAll(all.dataset.libAll === 'add');
    const add = e.target.closest('[data-lib-add]');
    if (add) return libClick(parseInt(add.dataset.libAdd, 10), true);
    const rm = e.target.closest('[data-lib-del]');
    if (rm) return stationRemove(parseInt(rm.dataset.libDel, 10));
    const r = e.target.closest('[data-lib-i]');
    if (r) libClick(parseInt(r.dataset.libI, 10), false);
  };
  $('np-lib').onsubmit = e => {
    e.preventDefault();
    if (e.target.id === 'np-radio-add') stationAdd(e.target);
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
  topics: () => ['media', 'audio.streams'],
  show() {
    // The library opens once the players are known: a browsable phone's is the first tab.
    // A folder still open from before (Back to this view, or a look at another tab) stays as it was
    // left, with its Back entries; a phone's only while the phone is still there.
    const kept = () => pl.libPath.length && pl.libItems.length && (pl.libPath[0].parent || {}).lib === pl.lib &&
      (pl.lib !== 'phone' || phones().some(x => x.browsable));
    api('/media').then(m => { pl.media = m; notePlayers(m.players); renderNowPlaying(); renderLibTabs(); if (!kept()) openLib(pl.lib); })
      .catch(() => { renderLibTabs(); if (!kept()) openLib(pl.lib); });
    api('/bluetooth').then(b => { btState = b; renderNowPlaying(); }).catch(() => {});
    api('/audio/endpoints').then(renderOutputs).catch(() => {});
    refreshPlayerStreams();
    clearInterval(pl.tick);
    // The progress bar moves between the phone's updates.
    pl.tick = setInterval(() => {
      if (++pl.ticks % 6 === 0) hearCheck();  // every 3 s
      const c = current();
      if (c.kind === 'phone' && c.p.status === 'playing') renderTime(playerPosition(c.p), (c.p.track || {}).duration_ms || 0);
    }, 500);
  },
  hide() { clearInterval(pl.tick); pl.tick = null; },
  onMessage(topic, data) {
    if (currentTab !== 'player') return;
    if (topic === 'media') {
      const hadLib = phones().some(p => p.browsable);
      pl.media = data;
      notePlayers(data.players);
      if (hadLib !== phones().some(p => p.browsable)) renderLibTabs();
      if (!(document.activeElement && document.activeElement.id === 'np-vol')) renderNowPlaying();
    } else if (topic === 'audio.streams') {
      pl.streams = data.streams.map(decorate);
      noteFailure();
      if (!(document.activeElement && document.activeElement.id === 'np-vol')) renderNowPlaying();
    }
  },
});
setupPlayer();
