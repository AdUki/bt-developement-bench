'use strict';
// OBEX: BlueZ's obexd as a tester — client sessions to a device (Object Push, File Transfer,
// Phonebook, Messages, AVRCP cover art) with each profile's operations, the transfers they start,
// what the board's own OBEX server receives (and whether to accept it), and the files on the board
// that are sent or were received. The state arrives on the "obex" topic while the tab is open.

const ob = {state: null, sessions: {}, results: {}};

const obFolder = (name, files, del) => files.length ? files.map(f => `<div class="file-row">
    <span><a href="/api/obex/files/${name}/${encodeURIComponent(f.name)}" download="${esc(f.name)}">${esc(f.name)}</a>
      <span class="muted">${fmtBytes(f.size)}</span></span>
    ${del ? `<button class="small-btn danger" data-del="${name}/${esc(f.name)}">${armed['ob' + name + f.name] ? 'Really?' : 'Delete'}</button>` : ''}</div>`).join('')
  : '<span class="muted">Empty</span>';

function renderObex(s) {
  ob.state = s;
  loaded($('ob-bar'));
  setText($('ob-state'), s.running ? 'running' : 'stopped');
  $('ob-state').title = s.running ? (s.managed ? 'Started by btbenchd' : 'Started by something else') : '';
  setClass($('ob-state'), 'pill ' + (s.running ? 'good' : ''));
  setText($('ob-toggle'), s.running ? (s.managed ? 'Stop' : 'Stop (not ours)') : 'Start');
  $('ob-toggle').disabled = s.running && !s.managed;
  if (idle($('ob-policy'))) $('ob-policy').value = s.agent.policy;
  setText($('ob-info'), s.running ? (s.agent.registered ? 'Devices can push files to the board and browse it over FTP.' : 'The agent is not registered: pushes to the board are refused.')
    : 'Starts by itself with the first session. While it runs, devices can push files to the board.');
  $('ob-info').title = s.root ? 'Files live in ' + s.root : '';
  const r = s.request;
  $('ob-request').hidden = !r;
  if (r && $('ob-request').dataset.id !== String(r.id)) {
    $('ob-request').dataset.id = r.id;
    $('ob-request').innerHTML = `<b>${esc(r.device || 'A device')}</b> pushes <b>${esc(r.name)}</b> (${fmtBytes(r.size)}).
      <div class="row wrap" style="margin-top:0.4rem">save as <input id="ob-req-name" value="${esc(r.name)}">
      <button class="primary" data-ans="yes">Accept</button><button data-ans="no">Reject</button>
      <span class="small muted">${r.expires_s} s left</span></div>`;
  }
  // Sessions: a card each, built once and kept (their results and inputs survive updates).
  const ids = new Set(s.sessions.map(x => x.id));
  for (const id of Object.keys(ob.sessions)) if (!ids.has(id)) { ob.sessions[id].remove(); delete ob.sessions[id]; }
  for (const x of s.sessions) if (!ob.sessions[x.id]) $('ob-sessions').prepend(ob.sessions[x.id] = sessionCard(x));
  setHTML($('ob-transfers'), s.transfers.length ? `<tr><th>File</th><th>Status</th><th>Progress</th><th>Size</th><th></th></tr>` +
    s.transfers.map(t => {
      const pct = t.size ? Math.min(100, t.transferred / t.size * 100) : (t.status === 'complete' ? 100 : 0);
      const live = t.status === 'active' || t.status === 'queued' || t.status === 'suspended';
      const file = t.filename ? t.filename.split('/').pop() : '';
      const where = t.filename && t.filename.includes('/obex/in/') ? `<a href="/api/obex/files/in/${encodeURIComponent(file)}" download>${esc(file)}</a>` : esc(file);
      return `<tr title="${esc(t.id)}"><td>${esc(t.name || file)}${t.direction === 'in' ? ' ' + pill('incoming', 'warn') : ''}<div class="small muted">${where}</div></td>
        <td>${pill(t.status, t.status === 'complete' ? 'good' : t.status === 'error' ? 'bad' : 'warn')}</td>
        <td><div class="progress"><i style="width:${pct.toFixed(0)}%"></i></div></td>
        <td>${t.size != null ? fmtBytes(t.size) : '—'}</td>
        <td>${live ? `<button class="small-btn" data-tr="${esc(t.id)}" data-act="${t.status === 'suspended' ? 'resume' : 'suspend'}">${t.status === 'suspended' ? 'Resume' : 'Pause'}</button>
          <button class="small-btn danger" data-tr="${esc(t.id)}" data-act="cancel">Cancel</button>` : ''}</td></tr>`;
    }).join('') : '<tr><td class="muted empty">No transfers yet: they show here as sessions send and pull files.</td></tr>');
  setHTML($('ob-f-out'), obFolder('out', s.files.out, true));
  setHTML($('ob-f-in'), obFolder('in', s.files.in, true));
  setHTML($('ob-f-shared'), obFolder('shared', s.files.shared, true));
  // The out/ pickers in the session cards follow the folder.
  const outOpts = s.files.out.map(f => ({value: f.name, label: f.name}));
  document.querySelectorAll('#ob-sessions select.ob-outfile').forEach(sel =>
    fillSelect(sel, outOpts.length ? outOpts : [{value: '', label: '(upload a file to out/ first)'}]));
  renderShare();
}

const refreshObex = () => api('/obex').then(renderObex).catch(e => {
  setText($('ob-msg'), e.message);
  if ($('ob-state').classList.contains('ld')) {
    setText($('ob-state'), 'error');
    setClass($('ob-state'), 'pill bad');
  }
  loaded($('ob-info'));
  ldFail(e, $('sh-recv'), $('ob-transfers'), $('ob-f-out'), $('ob-f-in'), $('ob-f-shared'));
});

// ---------------------------------------------------------------- session cards

const OPS = {
  opp: `<select class="ob-outfile"></select><button data-op="send">Send file</button>
        <button data-op="pull_card">Pull business card</button><button data-op="exchange">Exchange cards</button>`,
  ftp: `<button data-op="list">List</button><button data-op="cd" data-name="..">Up a folder</button><button data-op="cd" data-name="/">Root</button>
        <input class="ob-arg narrow" placeholder="folder"><button data-op="mkdir">New folder</button>
        <select class="ob-outfile"></select><button data-op="put">Put file</button>`,
  pbap: `<select class="ob-loc"><option value="int">phone</option><option value="sim1">SIM</option></select>
         <select class="ob-pb"><option value="pb">contacts</option><option value="ich">incoming calls</option>
           <option value="och">outgoing calls</option><option value="mch">missed calls</option><option value="cch">all calls</option>
           <option value="fav">favourites</option><option value="spd">speed dial</option></select>
         <button data-op="select">Select</button><button data-op="size">Size</button><button data-op="list">List</button>
         <button data-op="pull_all">Pull all (vCard 3.0)</button>
         <input class="ob-arg narrow" placeholder="name"><button data-op="search">Search</button>`,
  map: `<button data-op="folder" data-name="">Root</button><button data-op="folder" data-name="telecom/msg">telecom/msg</button>
        <button data-op="list_folders">Folders</button>
        <input class="ob-arg narrow" placeholder="inbox"><button data-op="list">Messages</button>`,
  'bip-avrcp': `<input class="ob-arg narrow" placeholder="image handle">
        <button data-op="properties">Properties</button><button data-op="thumbnail">Thumbnail</button><button data-op="get">Image</button>`,
};

const TARGET_NAMES = {opp: 'Object Push', ftp: 'File Transfer', pbap: 'Phonebook', map: 'Messages', 'bip-avrcp': 'Cover art (BIP)'};

// A cover-art session's handle box starts with the image of the track the device plays now.
function prefillHandle(el, address) {
  api('/media').then(m => {
    const p = m.players.find(pl => pl.address === address && pl.track && pl.track.img_handle);
    const box = el.querySelector('.ob-arg');
    if (p && box && !box.value) box.value = p.track.img_handle;
  }).catch(() => {});
}

function sessionCard(x) {
  const el = document.createElement('div');
  el.className = 'card ob-session';
  el.dataset.id = x.id;
  el.dataset.target = x.target;
  const via = x.channel != null ? 'channel ' + x.channel : x.psm != null ? 'PSM ' + x.psm : '';
  el.innerHTML = `<div class="card-head"><h3>${esc(TARGET_NAMES[x.target] || x.target)}
      <span class="small muted" title="${esc(x.id + (via ? ' · ' + via : ''))}">with ${esc(typeof devName === 'function' ? devName(x.destination) : x.destination)}</span></h3>
      <button class="small-btn danger" data-close="1">Close</button></div>
    <div class="ops">${OPS[x.target] || ''}</div>
    <div class="ob-path small muted"></div>
    <div class="result small"></div>`;
  if (x.target === 'bip-avrcp') prefillHandle(el, x.destination);
  return el;
}

function showResult(el, html) { el.querySelector('.result').innerHTML = html; }

function renderOpResult(el, op, r) {
  const t = el.dataset.target;
  if (r.entries) {
    // An FTP folder: folders open, files download to in/.
    showResult(el, `<table class="tbl">${r.entries.map(e => `<tr><td>${e.type === 'folder' ? '📁' : '📄'} ${esc(e.name)}</td>
      <td>${e.size != null ? fmtBytes(e.size) : ''}</td><td class="small muted">${esc(e.modified || '')}</td>
      <td>${e.type === 'folder' ? `<button class="small-btn" data-op="cd" data-name="${esc(e.name)}">open</button>`
        : `<button class="small-btn" data-op="get" data-name="${esc(e.name)}">get</button>`}
        <button class="small-btn danger" data-op="delete" data-name="${esc(e.name)}">delete</button></td></tr>`).join('') ||
      '<tr><td class="muted">empty folder</td></tr>'}</table>`);
  } else if (r.folders) {
    showResult(el, r.folders.map(f => `<button class="small-btn" data-op="folder" data-name="${esc(f.name)}" data-rel="1">📁 ${esc(f.name)}</button>`).join(' ') || '<span class="muted">no folders</span>');
  } else if (r.vcards) {
    showResult(el, `<table class="tbl">${r.vcards.map(v => `<tr><td class="mono">${esc(v.vcard)}</td><td>${esc(v.name)}</td>
      <td><button class="small-btn" data-op="pull" data-vcard="${esc(v.vcard)}">pull</button></td></tr>`).join('') || '<tr><td class="muted">none</td></tr>'}</table>`);
  } else if (r.messages) {
    showResult(el, `<table class="tbl"><tr><th>from</th><th>subject</th><th>time</th><th>type</th><th></th></tr>${r.messages.map(m => `<tr>
      <td>${esc(m.sender || m.senderaddress || '')}</td><td>${m.read === false ? '<b>' + esc(m.subject || '') + '</b>' : esc(m.subject || '')}</td>
      <td class="small">${esc(m.timestamp || '')}</td><td class="small">${esc(m.type || '')}</td>
      <td><button class="small-btn" data-op="get" data-message="${esc(m.path)}">get</button></td></tr>`).join('') || '<tr><td class="muted">no messages</td></tr>'}</table>`);
  } else if (r.properties) {
    showResult(el, `<pre class="log">${esc(JSON.stringify(r.properties, null, 1))}</pre>`);
  } else if (r.size != null && op === 'size') {
    showResult(el, `${r.size} entries`);
  } else if (r.status) {
    // A transfer: it shows in Transfers; a finished image is shown here too.
    showResult(el, `started ${esc(r.id)} (${esc(r.name || r.filename.split('/').pop())}) — see Transfers`);
    if (t === 'bip-avrcp') {
      // Shown once the transfer has finished, or its failure (a handle the phone does not know
      // ends in an error, not in an image).
      const file = r.filename.split('/').pop();
      const started = Date.now();
      const wait = () => {
        const tr = ob.state && ob.state.transfers.find(x => x.id === r.id);
        if (tr && tr.status === 'complete')
          return showResult(el, `<img class="cover" style="float:none" src="/api/obex/files/in/${encodeURIComponent(file)}?t=${Date.now()}" alt="cover"> ${esc(file)} (${fmtBytes(tr.transferred)})`);
        if (tr && tr.status === 'error') return showResult(el, `<span class="bad-text">the phone did not send the image (is the handle current?)</span>`);
        if (Date.now() - started > 20000) return showResult(el, '<span class="bad-text">no image after 20 s</span>');
        setTimeout(() => refreshObex().then(wait), 700);
      };
      wait();
    }
  } else {
    showResult(el, '<span class="good-text">ok</span>');
    if (t === 'ftp' && (op === 'cd' || op === 'mkdir' || op === 'delete')) runOp(el, {op: 'list'});
  }
}

function runOp(el, body) {
  showResult(el, '<span class="muted">…</span>');
  return post(`/obex/sessions/${el.dataset.id}`, body).then(r => {
    if (el.dataset.target === 'ftp' && body.op === 'cd') {
      const p = ob.results[el.dataset.id] || [];
      if (body.name === '/') p.length = 0; else if (body.name === '..') p.pop(); else p.push(body.name);
      ob.results[el.dataset.id] = p;
      el.querySelector('.ob-path').textContent = '/' + p.join('/');
    }
    renderOpResult(el, body.op, r);
  }).catch(e => showResult(el, `<span class="bad-text">${esc(e.message)}</span>`));
}

function obDevices() {
  api('/bluetooth').then(b => {
    const devs = b.devices.filter(d => d.paired || d.connected);
    fillSelect($('ob-dev'), devs.length ? devs.map(d => ({value: d.address, label: `${d.alias} (${d.address})${d.connected ? '' : ' — not connected'}`}))
      : [{value: '', label: 'no paired device'}]);
  }).catch(() => fillSelect($('ob-dev'), [{value: '', label: 'Bluetooth is not running'}]));
}

function uploadTo(folder, input) {
  const files = [...input.files];
  input.value = '';
  Promise.all(files.map(f => fetch(`/api/obex/files/${folder}?name=${encodeURIComponent(f.name)}`, {method: 'POST', body: f, headers: {'Content-Type': 'application/octet-stream'}})
    .then(r => r.ok ? r.json() : r.json().then(j => { throw new Error(j.error || r.statusText); }))))
    .then(refreshObex).catch(fail);
}

function setupObex() {
  $('ob-toggle').onclick = () => put('/obex', {running: !(ob.state && ob.state.running)}).then(renderObex).catch(fail);
  $('ob-policy').onchange = () => put('/obex', {policy: $('ob-policy').value}).then(renderObex).catch(fail);
  $('ob-request').onclick = e => {
    const b = e.target.closest('button[data-ans]');
    if (!b) return;
    post('/obex/request', {id: parseInt($('ob-request').dataset.id, 10), accept: b.dataset.ans === 'yes',
      name: $('ob-req-name') ? $('ob-req-name').value.trim() : ''}).then(refreshObex).catch(fail);
  };
  $('ob-connect').onclick = () => {
    const address = $('ob-dev').value;
    if (!address) return toast('Pair a device first (Devices)');
    const target = $('ob-target').value;
    const body = {address, target};
    const ch = parseInt($('ob-chan').value, 10);
    if (ch) body[target === 'bip-avrcp' || ch > 30 ? 'psm' : 'channel'] = ch;
    setText($('ob-msg'), `connecting ${target} to ${address}…`);
    post('/obex/sessions', body).then(() => { setText($('ob-msg'), ''); refreshObex(); })
      .catch(e => setText($('ob-msg'), e.message));
  };
  $('ob-sessions').onclick = e => {
    const el = e.target.closest('.ob-session');
    if (!el) return;
    if (e.target.closest('[data-close]')) {
      del(`/obex/sessions/${el.dataset.id}`).then(refreshObex).catch(fail);
      return;
    }
    const b = e.target.closest('button[data-op]');
    if (!b) return;
    const body = {op: b.dataset.op};
    const arg = el.querySelector('.ob-arg');
    const outfile = el.querySelector('.ob-outfile');
    if (b.dataset.name != null) body.name = b.dataset.name;
    else if (arg && arg.value.trim() && ['cd', 'mkdir', 'list', 'folder'].includes(body.op)) body.name = arg.value.trim();
    if (b.dataset.vcard) body.vcard = b.dataset.vcard;
    if (b.dataset.message) body.message = b.dataset.message;
    if (['send', 'put', 'exchange'].includes(body.op)) body.file = outfile && outfile.value;
    if (el.dataset.target === 'pbap') {
      if (body.op === 'select') { body.location = el.querySelector('.ob-loc').value; body.phonebook = el.querySelector('.ob-pb').value; }
      if (body.op === 'pull_all') body.format = 'vcard30';
      if (body.op === 'search') { body.field = 'name'; body.value = arg.value.trim(); }
    }
    if (el.dataset.target === 'bip-avrcp') body.handle = arg.value.trim();
    runOp(el, body);
  };
  $('ob-transfers').onclick = e => {
    const b = e.target.closest('button[data-tr]');
    if (b) post(`/obex/transfers/${b.dataset.tr}`, {action: b.dataset.act}).then(refreshObex).catch(fail);
  };
  document.querySelectorAll('#obex [id^=ob-f-]').forEach(box => {
    box.onclick = e => {
      const b = e.target.closest('button[data-del]');
      if (!b) return;
      const [folder, name] = [b.dataset.del.split('/')[0], b.dataset.del.slice(b.dataset.del.indexOf('/') + 1)];
      if (!twoClick('ob' + folder + name, () => ob.state && renderObex(ob.state))) return;
      del(`/obex/files/${folder}/${encodeURIComponent(name)}`).then(refreshObex).catch(fail);
    };
  });
  $('ob-up-out').onchange = e => uploadTo('out', e.target);
  $('ob-up-shared').onchange = e => uploadTo('shared', e.target);
}


// ---------------------------------------------------------------- Share

// Files the way a phone shares them: choose files, choose a device, Send — the board uploads them
// to out/, opens an Object Push session to the device and pushes them one by one, with the
// device's answer (it may ask its user first) and the progress of each. Received files are a
// gallery of in/, images as thumbnails; a push that waits for an answer is asked here.
const sh = {picked: [], dev: lsGet('btbench.sh.dev') || '', jobs: [], busy: false};

const SH_ICON = {
  phone: 'M8 3h8a1 1 0 011 1v16a1 1 0 01-1 1H8a1 1 0 01-1-1V4a1 1 0 011-1zM11 18h2',
  computer: 'M4 5h16v10H4zM2 19h20',
  audio: 'M4 14v-2a8 8 0 0116 0v2M4 14h3v6H4zM17 14h3v6h-3z',
  other: 'M7 7l10 10-5 5V2l5 5L7 17',
  file: 'M6 2h8l4 4v16H6zM14 2v4h4',
  image: 'M4 5h16v14H4zM4 15l4-4 4 4 3-3 5 5M15 9h.01',
  contact: 'M5 4h14v16H5zM12 11a2.5 2.5 0 100-5 2.5 2.5 0 000 5zM8 17a4 4 0 018 0',
  music: 'M9 18V6l10-2v12M9 18a2 2 0 11-4 0 2 2 0 014 0zM19 16a2 2 0 11-4 0 2 2 0 014 0z',
  video: 'M4 6h12v12H4zM16 10l5-3v10l-5-3',
  check: 'M5 12l5 5 9-10',
};
const shSvg = d => `<svg viewBox="0 0 24 24" aria-hidden="true"><path d="${d}"/></svg>`;
const shKind = n => /\.(jpe?g|png|gif|webp|bmp)$/i.test(n) ? 'image' : /\.vcf$/i.test(n) ? 'contact'
  : /\.(mp3|m4a|aac|flac|wav|ogg|opus)$/i.test(n) ? 'music' : /\.(mp4|mkv|mov|3gp|webm)$/i.test(n) ? 'video' : 'file';
const shDevIcon = d => /phone/.test(d.icon || '') || d.class_major === 'phone' ? 'phone'
  : /computer/.test(d.icon || '') || d.class_major === 'computer' ? 'computer' : /audio|headset|headphone/.test(d.icon || '') ? 'audio' : 'other';
// How long ago, by the board's clock (now: its now_ms; one without a network may be hours off).
const shAgo = (ms, now) => {
  const s = Math.max(0, ((now || Date.now()) - ms) / 1000);
  return s < 60 ? 'just now' : s < 3600 ? Math.floor(s / 60) + ' min ago' : s < 86400 ? Math.floor(s / 3600) + ' h ago' : Math.floor(s / 86400) + ' d ago';
};
// A device takes files when it offers Object Push (0x1105).
const shCanTake = d => d.uuids.some(u => u.uuid === '1105');

function renderShareDevices() {
  const b = btState;
  if (!b) return;  // still on the way: not "no paired device"
  const devs = b.devices.filter(d => d.paired);
  if (!devs.some(d => d.address === sh.dev && shCanTake(d))) sh.dev = (devs.find(d => d.connected && shCanTake(d)) || devs.find(shCanTake) || {}).address || '';
  setHTML($('sh-devs'), devs.length ? devs.map(d => {
    const ok = shCanTake(d);
    return `<button class="sh-dev${d.address === sh.dev ? ' on' : ''}" data-addr="${d.address}"${ok ? '' : ' disabled title="It offers no Object Push: it cannot receive files"'}>
      <span class="sh-dev-i">${shSvg(SH_ICON[shDevIcon(d)])}</span><b>${esc(d.alias)}</b>
      <span class="small muted">${ok ? (d.connected ? 'connected' : 'paired') : 'takes no files'}</span></button>`;
  }).join('') : '<div class="small muted">No paired device. Pair one in Devices, then it shows here.</div>');
  $('sh-send').disabled = sh.busy || !sh.picked.length || !sh.dev;
}

function renderSharePicked() {
  setHTML($('sh-picked'), sh.picked.map((f, i) => `<div class="sh-file"><span class="sh-fi">${shSvg(SH_ICON[shKind(f.name)])}</span>
    <span class="grow">${esc(f.name)} <span class="muted small">${fmtBytes(f.size)}</span></span>
    <button class="icon-btn" data-unpick="${i}" title="Leave it out" aria-label="Leave it out"${sh.busy ? ' disabled' : ''}>×</button></div>`).join(''));
  $('sh-drop').classList.toggle('has', sh.picked.length > 0);
  renderShareDevices();
}

function renderShareJobs() {
  const tr = id => ob.state && ob.state.transfers.find(t => t.id === id);
  setHTML($('sh-jobs'), sh.jobs.map(j => {
    const t = j.tid ? tr(j.tid) : null;
    let pct = j.phase === 'upload' ? j.up : 0, note = '';
    if (t) {
      pct = t.size ? Math.min(100, t.transferred / t.size * 100) : t.status === 'complete' ? 100 : 0;
      if (t.status === 'complete') j.phase = 'done';
      else if (t.status === 'error') { j.phase = 'failed'; j.err = j.err || 'refused or interrupted'; }
      else j.phase = t.transferred > 0 ? 'sending' : 'waiting';
    }
    note = {upload: 'preparing…', session: 'connecting…', waiting: `waiting for ${esc(j.to)} to accept…`, sending: `${pct.toFixed(0)} %`,
      done: 'sent', failed: esc(j.err || 'failed'), queued: 'queued'}[j.phase] || '';
    if (j.phase === 'done') pct = 100;
    return `<div class="sh-job ${j.phase}"><span class="sh-fi">${shSvg(SH_ICON[j.phase === 'done' ? 'check' : shKind(j.name)])}</span>
      <div class="grow"><div class="row between"><span>${esc(j.name)}</span><span class="small ${j.phase === 'failed' ? 'bad-text' : 'muted'}">${note}</span></div>
      <div class="progress"><i style="width:${pct.toFixed(1)}%"></i></div></div></div>`;
  }).join(''));
}

function renderShareRecv(s) {
  loaded($('sh-recv-card'));
  $('sh-on').checked = !!s.running;
  if (idle($('sh-policy'))) $('sh-policy').value = s.agent.policy;
  const name = btState && btState.adapter ? btState.adapter.alias : 'the board';
  setText($('sh-recv-note'), s.running ? `Others send to “${name}”.` : 'Off: devices cannot send files to the board.');
  const r = s.request;
  setHTML($('sh-incoming'), r ? `<div class="sh-ask"><span class="sh-fi">${shSvg(SH_ICON[shKind(r.name)])}</span>
      <div class="grow"><b>${esc(r.device || 'A device')}</b> wants to send <b>${esc(r.name)}</b> <span class="muted">(${fmtBytes(r.size)})</span></div>
      <button class="primary" data-sh-ans="yes" data-id="${r.id}">Accept</button><button data-sh-ans="no" data-id="${r.id}">Decline</button></div>` : '');
  const files = s.files.in;
  setHTML($('sh-recv'), files.length ? files.map(f => {
    const url = `/api/obex/files/in/${encodeURIComponent(f.name)}`;
    const k = shKind(f.name);
    return `<div class="sh-item"><a class="sh-thumb" href="${url}" target="_blank" rel="noopener">${k === 'image'
      ? `<img loading="lazy" alt="" src="${url}?t=${f.mtime}">` : shSvg(SH_ICON[k])}</a>
      <div class="sh-meta"><span class="sh-name" title="${esc(f.name)}">${esc(f.name)}</span>
        <span class="small muted">${fmtBytes(f.size)}${f.mtime ? ' · ' + shAgo(f.mtime, s.now_ms) : ''}</span></div>
      <div class="sh-acts"><a class="small-btn btn-link" href="${url}" download="${esc(f.name)}">Download</a>
        <button class="small-btn danger" data-sh-del="${esc(f.name)}">${armed['shin' + f.name] ? 'Really?' : 'Delete'}</button></div></div>`;
  }).join('') : '<div class="sh-empty">Nothing received yet. Send a photo from a paired phone to the board: it shows up here.</div>');
}

function renderShare() {
  if (!ob.state) return;
  renderShareRecv(ob.state);
  renderShareJobs();
  renderShareDevices();
}

// One file to out/, with the upload's progress (fetch has none for a request body).
function shUpload(job, f) {
  return new Promise((ok, bad) => {
    const x = new XMLHttpRequest();
    x.open('POST', `/api/obex/files/out?name=${encodeURIComponent(f.name)}`);
    x.setRequestHeader('Content-Type', 'application/octet-stream');
    x.upload.onprogress = e => { if (e.lengthComputable) { job.up = e.loaded / e.total * 100; renderShareJobs(); } };
    x.onload = () => {
      if (x.status < 300) return ok();
      let m = x.statusText;
      try { m = JSON.parse(x.responseText).error || m; } catch (e) { /* not JSON */ }
      bad(new Error(m));
    };
    x.onerror = () => bad(new Error('upload failed'));
    x.send(f);
  });
}

// Waits for a transfer to end, from the "obex" frames (renderObex keeps ob.state). One that
// vanishes after it was seen (obexd restarted), or never shows, has failed; a device that sits on
// the question is given ten minutes.
function shDone(tid) {
  const until = Date.now() + 600000;
  let seen = false, since = Date.now();
  return new Promise(ok => {
    const look = () => {
      const t = ob.state && ob.state.transfers.find(x => x.id === tid);
      if (t && (t.status === 'complete' || t.status === 'error')) return ok(t.status);
      if (t) { seen = true; since = Date.now(); }
      if ((seen || Date.now() - since > 15000) && !t && Date.now() - since > 3000) return ok('error');
      if (Date.now() > until) return ok('error');
      setTimeout(look, 500);
    };
    look();
  });
}

async function shSend() {
  const d = btState && btState.devices.find(x => x.address === sh.dev);
  if (!d || !sh.picked.length) return;
  sh.busy = true;
  lsSet('btbench.sh.dev', sh.dev);
  const files = sh.picked;
  sh.picked = [];
  sh.jobs = files.map(f => ({name: f.name, to: d.alias, phase: 'queued', up: 0}));
  renderSharePicked();
  renderShareJobs();
  setText($('sh-msg'), `Sending to ${d.alias}…`);
  let sid = null;
  try {
    for (let i = 0; i < files.length; i++) {
      sh.jobs[i].phase = 'upload';
      renderShareJobs();
      await shUpload(sh.jobs[i], files[i]);
    }
    sh.jobs.forEach(j => { j.phase = 'session'; });
    renderShareJobs();
    sid = (await post('/obex/sessions', {address: d.address, target: 'opp'})).id;
    let sent = 0;
    for (const j of sh.jobs) {
      try {
        const t = await post(`/obex/sessions/${sid}`, {op: 'send', file: j.name});
        j.tid = t.id;
        j.phase = 'waiting';
        renderShareJobs();
        if (await shDone(t.id) === 'complete') sent++;
      } catch (e) { j.phase = 'failed'; j.err = e.message; }
      renderShareJobs();
    }
    setText($('sh-msg'), sent === sh.jobs.length ? `Sent ${sent === 1 ? 'it' : 'all ' + sent} to ${d.alias}.` : `${sent} of ${sh.jobs.length} sent.`);
  } catch (e) {
    sh.jobs.forEach(j => { if (j.phase !== 'done') { j.phase = 'failed'; j.err = j.err || e.message; } });
    renderShareJobs();
    setText($('sh-msg'), e.message);
  } finally {
    if (sid) del(`/obex/sessions/${sid}`).catch(() => {});
    sh.busy = false;
    renderSharePicked();
  }
}

function setupShare() {
  const add = list => { if (!sh.busy) { sh.picked = sh.picked.concat([...list]); renderSharePicked(); } };
  $('sh-files').onchange = e => { add(e.target.files); e.target.value = ''; };
  const drop = $('sh-drop');
  drop.onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); $('sh-files').click(); } };
  ['dragenter', 'dragover'].forEach(ev => drop.addEventListener(ev, e => { e.preventDefault(); drop.classList.add('over'); }));
  ['dragleave', 'drop'].forEach(ev => drop.addEventListener(ev, e => { e.preventDefault(); drop.classList.remove('over'); }));
  drop.addEventListener('drop', e => add(e.dataTransfer.files));
  $('sh-picked').onclick = e => {
    const b = e.target.closest('[data-unpick]');
    if (b) { sh.picked.splice(parseInt(b.dataset.unpick, 10), 1); renderSharePicked(); }
  };
  $('sh-devs').onclick = e => {
    const b = e.target.closest('.sh-dev');
    if (b && !b.disabled) { sh.dev = b.dataset.addr; renderShareDevices(); }
  };
  $('sh-send').onclick = shSend;
  $('sh-on').onchange = e => put('/obex', {running: e.target.checked}).then(renderObex).catch(fail);
  $('sh-policy').onchange = e => put('/obex', {policy: e.target.value}).then(renderObex).catch(fail);
  $('sh-incoming').onclick = e => {
    const b = e.target.closest('[data-sh-ans]');
    if (b) post('/obex/request', {id: parseInt(b.dataset.id, 10), accept: b.dataset.shAns === 'yes', name: ''}).then(refreshObex).catch(fail);
  };
  $('sh-recv').onclick = e => {
    const b = e.target.closest('[data-sh-del]');
    if (!b) return;
    const n = b.dataset.shDel;
    if (!twoClick('shin' + n, renderShare)) return;
    del(`/obex/files/in/${encodeURIComponent(n)}`).then(refreshObex).catch(fail);
  };
}

registerTab('obex', {
  topics: () => ['obex', 'bt'],
  show() {
    refreshObex();
    obDevices();
    api('/bluetooth').then(b => { btState = b; renderShare(); }).catch(e => ldFail(e, $('sh-devs')));
  },
  onMessage(topic, data) {
    if (currentTab !== 'obex') return;
    if (topic === 'obex') renderObex(data);
    if (topic === 'bt' && panes.obex === 'share') renderShareDevices();
  },
});
setupObex();
setupShare();
