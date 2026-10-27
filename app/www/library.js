'use strict';
// Audio → Library: what the board can play of its own, and a browsable phone's own library. The
// radio stations, the media servers on the LAN (UPnP, a page at a time), the test signals, and the
// phone's folders and search: a station or a signal plays on its own, a track with the rest of its
// folder after it as the queue, a phone's item on the phone. What the board plays from here is the
// route's input (POST /api/audio/route/play), on the outputs the route has.

// ---------------------------------------------------------------- the library

// The tabs: the library's sources, a browsable phone's first. With none picked, the first.
function renderLibTabs() {
  const tabs = [['radio', 'Radio'], ['upnp', 'Media servers'], ['signals', 'Test signals']];
  if (phones().some(p => p.browsable)) tabs.unshift(['phone', 'Phone']);
  if (!tabs.some(t => t[0] === au.lib)) au.lib = tabs[0][0];
  setHTML($('np-lib-tabs'), tabs.map(([k, l]) => `<button class="seg${au.lib === k ? ' on' : ''}" data-lib="${k}">${l}</button>`).join(''));
  $('np-search-row').hidden = !(au.lib === 'phone' && phones().some(p => p.searchable));
}

function openLib(kind, chosen) {
  const phone = kind === 'phone' && phones().find(x => x.browsable);
  // The phone's top folder already on its way (the tab shown twice while the phone is slow): wait
  // for that answer rather than queueing a second request behind it.
  if (phone && au.lib === kind && au.libLoading === phone.path + '/Filesystem') return;
  // Whatever the library was loading is no longer wanted (see phoneFolder).
  au.libSeq++;
  au.libLoading = '';
  au.lib = kind;
  if (chosen) lsSet('btbench.pl.lib', kind);  // only a tab picked by hand is remembered
  libDropBack(0);
  au.libPath = [];
  renderLibTabs();
  kind = au.lib;  // a tab gone (an old Inputs or Up next remembered, a phone left): the first one
  if (kind === 'radio') {
    setHTML($('np-crumbs'), '');
    api('/audio/radio').then(renderStations)
      .catch(e => setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`));
  } else if (kind === 'upnp') {
    au.upnpServer = null;
    upnpServers(false);
  } else if (kind === 'signals') {
    renderSignals();
  } else if (phone) {
    phoneFolder(phone, phone.path + '/Filesystem', 'Files');
  }
}

// The stations, each with a remove, and under them a station to add by its URL: the board names it
// (what the stream calls itself). The board keeps the whole list (PUT /audio/radio). Under it, a
// URL played once without keeping it, with the decoder to play it with.
function renderStations(r) {
  au.libItems = r.stations.map(s => ({kind: 'station', title: s.name, url: s.url}));
  renderLib('<div class="empty-line">No stations yet.</div>', `<form class="radio-add row" id="np-radio-add">
    <input type="url" name="url" class="grow" placeholder="Add a station: its stream URL (MP3, AAC, .pls, .m3u)" required>
    <button class="small-btn">${svg(PL_SVG.plus)} Add</button></form>
    <form class="radio-add row" id="np-url-play">
    <input type="url" name="url" class="grow" placeholder="Play a stream or a file by its URL" required>
    <select name="decoder" aria-label="Decoder"><option value="auto">auto</option><option>mpg123</option><option>ffmpeg</option></select>
    <button class="small-btn">${svg(PL_SVG.play)} Play</button></form>`);
}

// A URL played once: at 44.1 kHz, what nearly every stream is (streamRate).
function playUrl(f) {
  const url = f.url.value.trim();
  if (!url) return;
  playLocal({source: {type: 'url', url, title: '', decoder: f.decoder.value}, rate: streamRate(0), channels: 2},
    s => { au.boardId = s.id; });
}

// The list as the board has it now, changed by `edit`, saved, and shown.
function stationsSave(edit) {
  return api('/audio/radio').then(r => put('/audio/radio', {stations: edit(r.stations)}))
    .then(r => { if (au.lib === 'radio') renderStations(r); return r; });
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
  const it = au.libItems[i];
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
  return {items: au.libItems, more: au.libMore, lib: au.lib, server: au.upnpServer, scroll: libScroller().scrollTop};
}

// Back to the list `entry` was opened from (its parent's snapshot).
function libRestore(entry) {
  const i = au.libPath.indexOf(entry);
  if (i < 0 || au.lib !== entry.parent.lib) return;
  const snap = entry.parent;
  au.libPath = au.libPath.slice(0, i);
  au.libSeq++;
  au.libLoading = '';
  au.upnpServer = snap.server;
  au.libItems = snap.items;
  au.libMore = snap.more && Object.assign({}, snap.more, {seq: au.libSeq, busy: false});
  renderCrumbs();
  renderLib();
  libScroller().scrollTop = snap.scroll;
}

// A folder opened from what is shown: remember that, and give Back a way to it.
function libDescend(entry, layered) {
  entry.parent = libSnapshot();
  au.libPath.push(entry);
  if (layered) entry.back = backOpen(() => { entry.back = null; libRestore(entry); });
}

// The library leaves the folders from `from` on by itself (a breadcrumb, another library, a
// search): their history entries go too.
function libDropBack(from) {
  const first = au.libPath.slice(from).find(c => c.back);
  if (!first) return;
  const layer = first.back;
  au.libPath.slice(from).forEach(c => { c.back = null; });
  backClose(layer);
}

function upnpServers(search) {
  au.libMore = null;
  setHTML($('np-crumbs'), '');
  setHTML($('np-lib'), `<div class="empty-line loading-line"><span class="dots"></span>${search ? 'Searching the network…' : 'Loading…'}</div>`);
  api('/audio/upnp/servers' + (search ? '?search=1' : '')).then(r => {
    // None known yet (the board only remembers what a search found): search now.
    if (!search && !r.servers.length) return upnpServers(true);
    au.libItems = r.servers.map(s => ({kind: 'server', title: s.name, id: s.id, thumb: s.icon,
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
  const seq = ++au.libSeq;
  au.upnpServer = server;
  if (push !== false) libDescend({id, title, meta: meta || null}, true);
  const here = au.libPath[au.libPath.length - 1];
  const folderArt = here && here.meta ? here.meta.art : '';
  renderCrumbs();
  setHTML($('np-lib'), '<div class="empty-line loading-line"><span class="dots"></span>Loading…</div>');
  au.libMore = null;
  upnpPage(server, id, 0, folderArt, seq);
}

// One page of the folder from `start`; what is left is in au.libMore until the list's end shows.
// A server that leaves TotalMatches at 0 (allowed when it does not know) has more while it fills
// whole pages.
function upnpPage(server, id, start, folderArt, seq) {
  return api(`/audio/upnp/browse?server=${encodeURIComponent(server)}&object=${encodeURIComponent(id)}&start=${start}&count=${UPNP_PAGE}`).then(r => {
    if (seq !== au.libSeq) return;
    const rows = upnpRows(r, folderArt);
    const next = start + (r.returned || 0);
    const more = r.returned > 0 && (r.total ? next < r.total : r.returned >= UPNP_PAGE);
    au.libMore = more ? {server, id, next, total: r.total || 0, folderArt, seq, busy: false} : null;
    if (start === 0) {
      au.libItems = rows;
      renderLib();
    } else {
      libAppend(rows);
    }
  }).catch(e => {
    if (seq !== au.libSeq) return;
    if (start === 0) return setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`);
    au.libMore = {server, id, next: start, total: 0, folderArt, seq, busy: false, failed: e.message};
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
  const m = au.libMore;
  if (!m || m.seq !== au.libSeq) return;
  const of = m.total ? ` of ${m.total.toLocaleString()}` : '';
  lib.insertAdjacentHTML('beforeend', m.failed
    ? `<div class="empty-line lib-more"><span class="bad-text">${esc(m.failed)}</span> <button class="small-btn" data-lib-more="1">Try again</button></div>`
    : `<div class="empty-line loading-line lib-more"><span class="dots"></span>${au.libItems.length.toLocaleString()}${of} shown, loading more…</div>`);
  if (m.failed || !('IntersectionObserver' in window)) return;
  libMoreSeen = new IntersectionObserver(es => { if (es.some(e => e.isIntersecting)) libMoreLoad(); }, {rootMargin: '600px'});
  libMoreSeen.observe(lib.querySelector('.lib-more'));
}

function libMoreLoad() {
  const m = au.libMore;
  if (!m || m.busy || m.seq !== au.libSeq) return;
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
  if (push !== false) libDescend({id: folder, title}, au.libPath.length > 0);
  renderCrumbs();
  setHTML($('np-lib'), '<div class="empty-line loading-line"><span class="dots"></span>Loading…</div>');
  // Only the last folder asked for is shown: browsing waits its turn on the board, and an earlier
  // answer arriving late must not replace the one opened since.
  const seq = ++au.libSeq;
  au.libLoading = folder;
  post('/media/players/browse', {path: p.path, folder}).then(r => {
    if (seq !== au.libSeq) return;
    au.libItems = (r.items || []).map(i => phoneRow(i, p.path));
    renderLib();
  }).catch(e => { if (seq === au.libSeq) setHTML($('np-lib'), `<div class="empty-line bad-text">${esc(e.message)}</div>`); })
    .finally(() => { if (seq === au.libSeq) au.libLoading = ''; });
}

function renderCrumbs() {
  const root = au.lib === 'upnp' ? '<a href="#" data-crumb="-1">Servers</a>' : '';
  setHTML($('np-crumbs'), root + au.libPath.map((c, i) => `${root || i ? ' / ' : ''}<a href="#" data-crumb="${i}">${esc(c.title)}</a>`).join(''));
}

// A cover over the row's icon: the icon stays when the image cannot be had (a server on another
// network than this browser, a broken link).
const libArt = (it, icon) => `${svg(icon)}${it.thumb || it.art ? `<img alt="" loading="lazy" decoding="async" src="${esc(it.thumb || it.art)}" onerror="this.remove()">` : ''}`;

// `foot` goes under the rows (the stations' add form).
function renderLib(empty, foot) {
  const items = au.libItems;
  const tracks = items.filter(x => x.kind === 'track');
  const here = au.lib === 'upnp' ? au.libPath[au.libPath.length - 1] : null;
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
  au.libView = {grid, inAlbum, tracks: tracks.length > 0};
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
  const from = au.libItems.length;
  au.libItems = au.libItems.concat(rows);
  const v = au.libView || {};
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
  const here = au.libPath[au.libPath.length - 1];
  if (au.libMore && here) return folderPlay(au.upnpServer, here.id, here.title, (here.meta || {}).art || '', add);
  queueTracks(au.libItems.filter(x => x.kind === 'track'), add);
}

// Tracks (library rows) played on the board, or into its queue (`add`: 'next' or 'end') while a
// URL stream plays.
function queueTracks(tracks, add) {
  if (!tracks.length) return;
  const items = tracks.map(x => ({url: x.url, title: x.title, art: x.art, duration_s: x.duration_s, artist: x.artist, album: x.album, rate: x.rate}));
  const b = loadedStream();
  if (add && b && isUrl(b)) {
    items.forEach(t => { au.art[t.url] = t; });
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
  const it = au.libItems[i];
  if (!it) return;
  if (it.kind === 'server') return upnpFolder(it.id, '0', it.title);
  if (it.kind === 'folder') return upnpFolder(au.upnpServer, it.id, it.title, true, it);
  if (it.kind === 'pfolder') return phoneFolder(phones().find(p => p.path === it.player), it.path, it.title);
  if (it.kind === 'ptrack') {
    if (!it.playable) return;
    return post('/media/items', {path: it.path, action: add ? 'add' : 'play'})
      .then(() => {
        toast(add ? 'Added to the phone’s queue' : 'Playing on the phone');
        au.queueAt = 0;
        // Played on the phone, the phone is what the board plays.
        const p = phones().find(x => x.path === it.player);
        if (add || !p) return;
        au.players[p.address.toUpperCase()] = p.path;
        if (!isRouted({address: p.address, kind: 'a2dp'})) pickInput({type: 'bluetooth', devices: [{address: p.address, kind: isMcp(p) ? 'bap' : 'a2dp'}]});
      }).catch(fail);
  }
  if (it.kind === 'station') return playOnBoard([{url: it.url, title: it.title}]);
  if (it.kind === 'signal') return playSignal(it.source);
  const tracks = au.libItems.filter(x => x.kind === 'track');
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
  items.forEach(it => { au.art[it.url] = it; });
  playLocal({source: {type: 'url', url: items[0].url, title: items[0].title, playlist: items.map(playlistItem)},
    rate: streamRate(items[0].rate), channels: 2, label: items.length > 1 ? items[0].title + ' and ' + (items.length - 1) + ' more' : items[0].title},
  s => { au.boardId = s.id; });
}

// The board's own input started (`body` without its outputs): the route plays it on its outputs,
// in the place of whatever the board played of its own and of the device that was the input.
function playLocal(body, started) {
  au.sw = null;
  if (au.route && !au.route.outputs.length && !au.listen) toast('No output is on: turn one on in Route.');
  post('/audio/route/play', body).then(s => {
    if (started) started(s);
    refreshPlayerStreams();
    return refreshRoute();
  }).catch(fail);
}

// ---- Test signals: a tap away, in the place of what the board plays, on its outputs. Under the
// presets, any of them made to measure: a tone (its own frequency on the right channel if wanted),
// a sweep, white, pink or brown noise, silence, and the stream's rate, channels and gain.
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
  au.libItems = SIGNALS.map(x => ({kind: 'signal', title: x.title, sub: x.sub, source: x.source}));
  renderLib('', `<form class="sig-form" id="np-sig-add">
    <div class="grid">
      <label>Signal <select name="type"><option value="tone">Tone</option><option value="sweep">Sweep</option>
        <option value="noise">Noise</option><option value="silence">Silence</option></select></label>
      <label data-for="tone">Frequency (Hz) <input type="number" name="freq" value="1000" min="1" max="24000"></label>
      <label data-for="tone">Right channel (Hz) <input type="number" name="freq_right" min="1" max="24000" placeholder="same as left"></label>
      <label data-for="sweep" hidden>From (Hz) <input type="number" name="from" value="20" min="1" max="24000"></label>
      <label data-for="sweep" hidden>To (Hz) <input type="number" name="to" value="20000" min="1" max="24000"></label>
      <label data-for="sweep" hidden>Seconds <input type="number" name="seconds" value="10" min="0.5" max="600" step="0.5"></label>
      <label data-for="noise" hidden>Colour <select name="color"><option>pink</option><option>white</option><option>brown</option></select></label>
      <label data-for="tone sweep noise"><span class="sig-level">Level (dBFS, peak)</span> <input type="number" name="level" value="-12" min="-96" max="0"></label>
      <label data-for="sweep" class="switch inline" hidden><input type="checkbox" name="log" checked><i></i> Logarithmic</label>
      <label data-for="sweep" class="switch inline" hidden><input type="checkbox" name="repeat" checked><i></i> Repeat</label>
    </div>
    <details class="more-fields"><summary>Format and gain</summary><div class="grid">
      <label>Rate <select name="rate"><option>48000</option><option>44100</option><option>32000</option><option>16000</option><option>8000</option></select></label>
      <label>Channels <select name="channels"><option value="2">stereo</option><option value="1">mono</option></select></label>
      <label>Gain (dB) <input type="number" name="gain" value="0" min="-60" max="20" step="0.5"></label>
    </div></details>
    <div class="form-acts"><button class="primary small-btn">${svg(PL_SVG.play)} Play</button></div></form>`);
}

// The form's fields for the signal picked; noise's level is its RMS, the others' their peak.
function signalFields(f) {
  const t = f.type.value;
  f.querySelectorAll('[data-for]').forEach(el => { el.hidden = !el.dataset.for.split(' ').includes(t); });
  setText(f.querySelector('.sig-level'), t === 'noise' ? 'Level (dBFS, RMS)' : 'Level (dBFS, peak)');
  if (t === 'noise' && f.level.value === '-12') f.level.value = '-20';
}

function signalFromForm(f) {
  const t = f.type.value, num = n => parseFloat(f[n].value);
  if (t === 'tone') return {type: t, freq: num('freq'), freq_right: f.freq_right.value.trim() ? num('freq_right') : null, level_db: num('level')};
  if (t === 'sweep') return {type: t, from: num('from'), to: num('to'), seconds: num('seconds'), log: f.log.checked, repeat: f.repeat.checked, level_db: num('level')};
  if (t === 'noise') return {type: t, color: f.color.value, level_db: num('level')};
  return {type: t};
}

function playSignal(source, format) {
  playLocal(Object.assign({source, rate: 48000, channels: 2}, format || {}), s => { au.boardId = s.id; });
}


// ---------------------------------------------------------------- wiring

function setupLibrary() {
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
    const below = au.libPath[i + 1];
    libDropBack(i + 1);
    if (below && below.parent) return libRestore(below);
    if (i < 0) { au.libPath = []; return upnpServers(false); }
    const c = au.libPath[i];
    au.libPath = au.libPath.slice(0, i + 1);
    if (au.lib === 'upnp') upnpFolder(au.upnpServer, c.id, c.title, false);
    else phoneFolder(phones().find(p => p.browsable), c.id, c.title, false);
  };
  $('np-lib').onclick = e => {
    if (e.target.closest('form')) return;
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
      const f = au.libItems[parseInt(folder.dataset.libFolder, 10)];
      return f && folderPlay(au.upnpServer, f.id, f.title, f.art || '', '');
    }
    const rm = e.target.closest('[data-lib-del]');
    if (rm) return stationRemove(parseInt(rm.dataset.libDel, 10));
    const r = e.target.closest('[data-lib-i]');
    if (r) libClick(parseInt(r.dataset.libI, 10), false);
  };
  $('np-lib').onchange = e => {
    if (e.target.name === 'type' && e.target.form && e.target.form.id === 'np-sig-add') signalFields(e.target.form);
  };
  $('np-lib').onsubmit = e => {
    e.preventDefault();
    const f = e.target;
    if (f.id === 'np-radio-add') stationAdd(f);
    if (f.id === 'np-url-play') playUrl(f);
    if (f.id === 'np-sig-add') {
      playSignal(signalFromForm(f), {rate: parseInt(f.rate.value, 10), channels: parseInt(f.channels.value, 10),
        gain_db: parseFloat(f.gain.value) || 0});
    }
  };
  $('np-search').onkeydown = e => {
    if (e.key !== 'Enter') return;
    const p = phones().find(x => x.searchable);
    if (!p) return;
    post('/media/players/search', {path: p.path, value: e.target.value}).then(r => {
      libDropBack(0);
      au.libPath = [{id: r.folder, title: '“' + e.target.value + '”'}];
      renderCrumbs();
      au.libItems = (r.items || []).map(i => phoneRow(i, p.path));
      renderLib();
    }).catch(fail);
  };
}
setupLibrary();
