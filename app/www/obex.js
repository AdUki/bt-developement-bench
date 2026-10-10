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
}

const refreshObex = () => api('/obex').then(renderObex).catch(e => setText($('ob-msg'), e.message));

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

registerTab('obex', {
  topics: () => ['obex'],
  show() { refreshObex(); obDevices(); },
  onMessage(topic, data) {
    if (currentTab === 'obex' && topic === 'obex') renderObex(data);
  },
});
setupObex();
