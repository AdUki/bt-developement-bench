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
  libSeq: 0,           // the library's latest request, and the phone folder it is loading
  libLoading: '',
  tick: null,
  ticks: 0,
  artDone: {},   // cover URL → true (shown) / false (failed)
  artWait: {},   // player|track → when its cover handle was first waited for
};

const PL_SVG = {
  previous: 'M18 6l-8.5 6L18 18zM6 6h2v12H6z', next: 'M6 6l8.5 6L6 18zM16 6h2v12h-2z',
  play: 'M8 5v14l11-7z', pause: 'M7 5h4v14H7zM13 5h4v14h-4z', stop: 'M6 6h12v12H6z',
  vol: 'M4 9v6h4l5 4V5L8 9zM16 8.5a5 5 0 010 7M18.5 6a8.5 8.5 0 010 12',
  note: 'M9 18V6l10-2v12M9 18a2 2 0 11-4 0 2 2 0 014 0zM19 16a2 2 0 11-4 0 2 2 0 014 0z',
  radio: 'M4 10h16v10H4zM7 10l10-5M8 15h.01M12 15h5',
  phone: 'M8 3h8a1 1 0 011 1v16a1 1 0 01-1 1H8a1 1 0 01-1-1V4a1 1 0 011-1zM11 18h2',
  folder: 'M4 6a2 2 0 012-2h4l2 2h6a2 2 0 012 2v10a2 2 0 01-2 2H6a2 2 0 01-2-2z',
  plus: 'M12 5v14M5 12h14',
};
// A cover image's outcome, by URL: the art box stops showing it is loading, and a failed image
// leaves the placeholder.
function npArtDone(img, ok) {
  pl.artDone[img.getAttribute('src')] = ok;
  img.hidden = !ok;
  img.parentNode.classList.remove('loading');
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
// service (mcp_gmcs: no name, never a track), which would be a second, dead chip for the phone.
const phones = () => (pl.media ? pl.media.players : []).filter(p => p.name || p.browsable || (p.track && p.track.title));

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
  const chips = phones().map(p => ({key: p.path, label: devName(p.address) + (p.name ? ' · ' + p.name : ''),
    icon: PL_SVG.phone, on: c.kind === 'phone' && c.p.path === p.path}));
  if (b) chips.push({key: 'board', label: 'This board', icon: PL_SVG.radio, on: c.kind === 'board'});
  setHTML($('np-src'), chips.length > 1 ? chips.map(x =>
    `<button class="chip${x.on ? ' on' : ''}" data-src="${esc(x.key)}">${svg(x.icon)}${esc(x.label)}</button>`).join('') : '');

  let artSrc = '', artLoading = false;
  let art = svg(PL_SVG.note, 'ph'), title = 'Nothing playing', sub = '', sub2 = '', playing = false, pos = 0, dur = 0;
  let can = {prev: false, next: false, play: false, stop: false};
  if (c.kind === 'phone') {
    const p = c.p, t = p.track || {};
    playing = p.status === 'playing';
    title = t.title || 'Nothing playing';
    sub = [t.artist, t.album].filter(Boolean).join(' — ');
    sub2 = `${p.name || 'Player'} on ${devName(p.address)}`;
    pos = playerPosition(p);
    dur = t.duration_ms || 0;
    if (p.obex_port != null && t.img_handle) {
      artSrc = `/api/media/players/cover?path=${encodeURIComponent(p.path)}&h=${encodeURIComponent(t.img_handle)}`;
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
    sub = list.length > 1 ? `Track ${s.track + 1} of ${list.length}` : (s.meta.name || s.source.title || '');
    sub2 = s.sink ? 'Playing on ' + outLabel(s.sink.target) : 'Playing in this browser';
    pos = (s.track_seconds || 0) * 1000;
    dur = (item.duration_s || 0) * 1000;
    if (item.art) artSrc = item.art;
    else art = svg(list.length > 1 ? PL_SVG.note : PL_SVG.radio, 'ph');
    can = {prev: s.track > 0, next: s.track + 1 < list.length, play: true, stop: true};
  }
  // An image: loading until it arrived (or failed: then the note stays).
  if (artSrc) {
    art = svg(PL_SVG.note, 'ph') + `<img alt="" src="${esc(artSrc)}" onload="npArtDone(this, true)" onerror="npArtDone(this, false)"${pl.artDone[artSrc] === true ? '' : ' hidden'}>`;
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
  setHTML($('np-ctl'), btn('previous', PL_SVG.previous, can.prev) +
    btn('play', PL_SVG.play, can.play) + btn('pause', PL_SVG.pause, can.play) +
    btn('next', PL_SVG.next, can.next) + btn('stop', PL_SVG.stop, can.stop) +
    (status ? pill(status, playing ? 'good' : status === 'paused' ? 'warn' : '') : ''));
  renderTime(pos, dur);
  renderVolume(c);
  setMediaSession();
  setText($('np-hear'), pl.hearId ? 'Stop listening here' : 'Listen in this browser');
  // The output only means something for what the board plays.
  $('np-out-row').hidden = c.kind === 'phone';
  $('np-hear').hidden = c.kind !== 'phone';
  renderQueue(c);
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
  pl.libPath = [];
  renderLibTabs();
  if (kind === 'radio') {
    setHTML($('np-crumbs'), '');
    api('/audio/radio').then(r => {
      pl.libItems = r.stations.map(s => ({kind: 'station', title: s.name, url: s.url}));
      renderLib();
    }).catch(e => setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`));
  } else if (kind === 'upnp') {
    pl.upnpServer = null;
    upnpServers(false);
  } else if (phone) {
    phoneFolder(phone, phone.path + '/Filesystem', 'Files');
  }
}

function upnpServers(search) {
  setHTML($('np-crumbs'), '');
  setHTML($('np-lib'), `<div class="empty-line loading-line"><span class="spin"></span>${search ? 'Searching the network…' : 'Loading…'}</div>`);
  api('/audio/upnp/servers' + (search ? '?search=1' : '')).then(r => {
    // None known yet (the board only remembers what a search found): search now.
    if (!search && !r.servers.length) return upnpServers(true);
    pl.libItems = r.servers.map(s => ({kind: 'server', title: s.name, id: s.id, sub: [s.manufacturer, s.model].filter(Boolean).join(' ')}));
    renderLib(r.servers.length ? '' : `<div class="empty-line">No media servers found. <button class="small-btn" data-lib-search="1">Search the network</button></div>`);
  }).catch(e => setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`));
}

function upnpFolder(server, id, title, push) {
  pl.libSeq++;
  pl.upnpServer = server;
  if (push !== false) pl.libPath.push({id, title});
  renderCrumbs();
  setHTML($('np-lib'), '<div class="empty-line loading-line"><span class="spin"></span>Loading…</div>');
  api(`/audio/upnp/browse?server=${encodeURIComponent(server)}&object=${encodeURIComponent(id)}&count=200`).then(r => {
    pl.libItems = r.containers.map(c => ({kind: 'folder', title: c.title, id: c.id, sub: c.child_count != null ? c.child_count + ' items' : ''}))
      .concat(r.items.filter(it => it.url).map(it => ({kind: 'track', title: it.title, url: it.url, art: it.art,
        sub: [it.artist, it.album].filter(Boolean).join(' — '), duration_s: it.duration_s})));
    renderLib();
  }).catch(e => setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`));
}

// A phone's item as a library row. A folder's name is its path in the phone's tree
// ("/Filesystem/Favorites"): only the last part is shown.
const phoneRow = (i, player) => ({kind: i.type === 'folder' ? 'pfolder' : 'ptrack',
  title: i.title || (i.metadata && i.metadata.title) || (i.name || '').replace(/^.*\//, ''),
  sub: i.artist || (i.metadata && i.metadata.artist) || '', path: i.path, playable: i.playable, player});

function phoneFolder(p, folder, title, push) {
  if (push !== false) pl.libPath.push({id: folder, title});
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

function renderLib(empty) {
  const icon = {station: PL_SVG.radio, server: PL_SVG.folder, folder: PL_SVG.folder, pfolder: PL_SVG.folder, track: PL_SVG.note, ptrack: PL_SVG.note};
  setHTML($('np-lib'), pl.libItems.map((it, i) => {
    const open = it.kind === 'server' || it.kind === 'folder' || it.kind === 'pfolder';
    const add = it.kind === 'track' || (it.kind === 'ptrack' && it.playable);
    return `<div class="tr lib" data-lib-i="${i}">
      <span class="tr-n">${svg(icon[it.kind])}</span>
      <span class="tr-t">${esc(it.title)}${it.sub ? `<span class="muted">${esc(it.sub)}</span>` : ''}</span>
      ${it.duration_s ? `<span class="tr-d">${fmtMs(it.duration_s * 1000)}</span>` : ''}
      ${add ? `<button class="icon-btn" data-lib-add="${i}" title="Add to the queue" aria-label="Add to the queue">${svg(PL_SVG.plus)}</button>` : ''}
      ${open ? '<span class="chev"></span>' : ''}</div>`;
  }).join('') || empty || '<div class="empty-line">Empty.</div>');
}

// A library row: open a folder, or play — a station on its own, a track with the rest of its
// folder after it (as the queue), a phone's item on the phone.
function libClick(i, add) {
  const it = pl.libItems[i];
  if (!it) return;
  if (it.kind === 'server') return upnpFolder(it.id, '0', it.title);
  if (it.kind === 'folder') return upnpFolder(pl.upnpServer, it.id, it.title);
  if (it.kind === 'pfolder') return phoneFolder(phones().find(p => p.path === it.player), it.path, it.title);
  if (it.kind === 'ptrack') {
    if (!it.playable) return;
    return post('/media/items', {path: it.path, action: add ? 'add' : 'play'})
      .then(() => { toast(add ? 'Added to the phone’s queue' : 'Playing on the phone'); pl.src = it.player; pl.queueAt = 0; }).catch(fail);
  }
  if (it.kind === 'station') return playOnBoard([{url: it.url, title: it.title}]);
  const tracks = pl.libItems.filter(x => x.kind === 'track');
  const from = tracks.indexOf(it);
  const items = (add ? [it] : tracks.slice(from)).map(x => ({url: x.url, title: x.title, art: x.art, duration_s: x.duration_s}));
  // "+" on a track while the board plays: onto the end of its queue.
  const b = boardStream();
  if (add && b) {
    items.forEach(t => { pl.art[t.url] = t; });
    return put(`/audio/streams/${b.id}`, {enqueue: items.map(t => ({url: t.url, title: t.title}))})
      .then(() => toast('Added to the queue')).catch(fail);
  }
  playOnBoard(items);
}

// What the board plays from the library: one stream with the tracks as its playlist, to the
// output chosen. The art and durations a UPnP server gave are kept here by URL (the daemon keeps
// only the URL and title).
function playOnBoard(items) {
  items.forEach(it => { pl.art[it.url] = it; });
  const out = $('np-out').value;
  const listen = out === 'browser';
  const sink = listen ? null : out === 'default' ? {type: 'pipewire', target: ''} : {type: out.split(':')[0], target: out.slice(out.indexOf(':') + 1)};
  const old = boardStream();
  const go = () => {
    return post('/audio/streams', {source: {type: 'url', url: items[0].url, title: items[0].title, playlist: items.map(it => ({url: it.url, title: it.title}))},
      sink, rate: 48000, channels: 2, label: items.length > 1 ? items[0].title + ' and ' + (items.length - 1) + ' more' : items[0].title})
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

// The art and duration of the board's tracks, from what the library said about them.
function decorate(s) {
  (s.source.playlist || []).forEach(it => Object.assign(it, pl.art[it.url] ? {art: pl.art[it.url].art, duration_s: pl.art[it.url].duration_s} : {}));
  return s;
}

// ---------------------------------------------------------------- outputs

const outLabel = id => {
  const e = pl.endpoints && pl.endpoints.sinks.find(x => x.id === id);
  return e ? e.label : id ? id : 'the default output';
};

function renderOutputs(ep) {
  pl.endpoints = ep;
  const opts = [{value: 'browser', label: 'This browser'}];
  if (ep.backend === 'pipewire') opts.push({value: 'default', label: 'The board’s default output'});
  const kind = ep.backend === 'alsa' ? 'alsa' : 'pipewire';
  ep.sinks.forEach(e => opts.push({value: `${kind}:${e.id}`, label: e.address ? `${devName(e.address)} (Bluetooth)` : e.label}));
  fillSelect($('np-out'), opts, idle($('np-out')) ? (lsGet('btbench.pl.out') || 'browser') : null);
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
    if (c.kind !== 'phone') return;
    // A toggle: the second press stops it.
    if (pl.hearId) return del(`/audio/streams/${pl.hearId}`).catch(() => {}).then(hearStop);
    listenToDevice(c.p.address, s => hear(s.id));
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
    if (i < 0) { pl.libPath = []; return upnpServers(false); }
    const c = pl.libPath[i];
    pl.libPath = pl.libPath.slice(0, i + 1);
    if (pl.lib === 'upnp') upnpFolder(pl.upnpServer, c.id, c.title, false);
    else phoneFolder(phones().find(p => p.browsable), c.id, c.title, false);
  };
  $('np-lib').onclick = e => {
    if (e.target.closest('[data-lib-search]')) return upnpServers(true);
    const add = e.target.closest('[data-lib-add]');
    if (add) return libClick(parseInt(add.dataset.libAdd, 10), true);
    const r = e.target.closest('[data-lib-i]');
    if (r) libClick(parseInt(r.dataset.libI, 10), false);
  };
  $('np-search').onkeydown = e => {
    if (e.key !== 'Enter') return;
    const p = phones().find(x => x.searchable);
    if (!p) return;
    post('/media/players/search', {path: p.path, value: e.target.value}).then(r => {
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
    api('/media').then(m => { pl.media = m; notePlayers(m.players); renderNowPlaying(); renderLibTabs(); openLib(pl.lib); })
      .catch(() => { renderLibTabs(); openLib(pl.lib); });
    api('/bluetooth').then(b => { if (!btState) btState = b; }).catch(() => {});
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
