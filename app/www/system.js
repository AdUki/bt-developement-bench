'use strict';
// System: health, versions, the services of the stack, bluetoothd's command line, the journal,
// the jobs runner and reboot. Health rides the "system" topic (1 Hz, already subscribed for the
// header); the rest is read when the tab opens.

let sysInfo = null;
let jobSel = null;       // the job whose output is shown
let journalFollow = false;

function renderHealth(h) {
  if (!h || currentTab !== 'system') return;
  const m = h.mem || {};
  setHTML($('sys-health'), `<div class="kv">
    <span>Host</span><span>${esc(h.hostname)}</span>
    <span>Addresses</span><span>${(h.ips || []).map(ip => `<div class="mono">${esc(ip)}</div>`).join('') || '—'}</span>
    <span>Uptime</span><span>${fmtUptime(h.uptime_s || 0)}</span>
    <span>CPU</span><span>${(h.cpu_pct || 0).toFixed(0)} % · load ${(h.load1 || 0).toFixed(2)}</span>
    <span>Memory</span><span>${Math.round((m.used_kb || 0) / 1024)} of ${Math.round((m.total_kb || 0) / 1024)} MiB
      <div class="bar"><i style="width:${m.total_kb ? (m.used_kb / m.total_kb * 100).toFixed(0) : 0}%"></i></div></span>
    <span>Temperature</span><span>${h.temp_c != null ? h.temp_c.toFixed(1) + ' °C' : '—'}</span>
    <span>Supply</span><span>${!h.power || !h.power.available ? 'not monitored here'
      : h.power.under_voltage ? '<b class="bad-text">UNDERVOLTAGE now</b>'
      : h.power.seen ? '<span class="warn-text">undervoltage seen since start</span>' : '<span class="good-text">ok</span>'}</span>
  </div>`);
}

function renderSystem(s) {
  sysInfo = s;
  renderHealth(s);
  const v = s.versions || {};
  setHTML($('sys-versions'), `<div class="kv">${Object.entries(v).map(([k, x]) =>
    `<span>${esc(k)}</span><span class="mono">${esc(x == null ? '—' : x)}</span>`).join('')}
    ${s.board ? `<span>board</span><span class="mono">${esc(Object.entries(s.board).map(e => e.join('=')).join(' '))}</span>` : ''}</div>`);
  renderServices(s.services || {});
  if (idle($('sys-btargs'))) $('sys-btargs').value = s.bluetoothd_args || '';
  // The journal's unit list: the stack's services, as GET /api/system reports them.
  const units = Object.keys(s.services || {});
  if (units.length && !$('sys-j-unit').options.length) {
    $('sys-j-unit').innerHTML = units.map(u => `<option>${esc(u)}</option>`).join('') + '<option value="kernel">Kernel log</option>';
    $('sys-j-unit').value = 'bluetooth.service';
  }
}

function renderServices(sv) {
  // btbenchd itself is the one not offered: the request would die with it.
  setHTML($('sys-services'), `<div class="svc-list">${Object.entries(sv).map(([u, st]) =>
    `<div class="svc">${pill(u.replace(/\.service$/, ''), st === 'active' ? 'good' : st === 'failed' ? 'bad' : '')}
      <span class="small muted">${esc(st)}</span>
      ${u !== 'btbenchd.service' ? `<button class="small-btn" data-restart="${esc(u)}" title="Restart ${esc(u)}">Restart</button>` : ''}</div>`).join('')}</div>`);
}

const refreshSystem = () => api('/system').then(renderSystem).catch(fail);

// ---------------------------------------------------------------- journal

const journalLine = e => `${fmtTime(e.ts)} ${e.ident || e.unit}${e.pid ? '[' + e.pid + ']' : ''}: ${e.msg}`;

function loadJournal() {
  const unit = $('sys-j-unit').value || 'bluetooth.service';
  api(`/system/journal?unit=${encodeURIComponent(unit)}&lines=200`).then(r => {
    logClear($('sys-journal'));
    r.entries.forEach(e => logLine($('sys-journal'), journalLine(e)));
  }).catch(e => setText($('sys-journal'), e.message));
}

// ---------------------------------------------------------------- jobs

function renderJobs(r) {
  if (!$('job-cmd').options.length) $('job-cmd').innerHTML = r.allowed.map(c => `<option>${esc(c)}</option>`).join('');
  setHTML($('job-list'), r.jobs.slice(0, 8).map(j => `<div class="row wrap small">
      <a href="#" data-job="${j.id}">#${j.id}</a> <span class="mono">${esc(j.cmd + ' ' + j.args.join(' '))}</span>
      ${j.running ? pill('running', 'warn') + ` <button class="small-btn danger" data-kill="${j.id}">kill</button>`
        : pill('exit ' + j.exit + (j.killed ? ' (killed)' : ''), j.exit === 0 ? 'good' : 'bad')}</div>`).join(''));
}
const refreshJobs = () => api('/jobs').then(renderJobs).catch(fail);

function showJob(id) {
  jobSel = id;
  updateTopics();
  api(`/jobs/${id}`).then(j => {
    logClear($('job-out'));
    logLine($('job-out'), `$ ${j.cmd} ${j.args.join(' ')}`);
    j.output.forEach(l => logLine($('job-out'), (l.stream === 'err' ? '! ' : '') + l.line));
    if (!j.running) logLine($('job-out'), `[exit ${j.exit}${j.killed ? ', killed' : ''}]`);
  }).catch(fail);
}

function setupSystem() {
  $('sys-services').onclick = e => {
    const b = e.target.closest('button[data-restart]');
    if (!b || !confirm(`Restart ${b.dataset.restart}?`)) return;
    b.disabled = true;
    post(`/system/services/${encodeURIComponent(b.dataset.restart)}/restart`).then(renderServices).catch(fail)
      .finally(() => { b.disabled = false; });
  };
  $('sys-btargs-save').onclick = () => {
    const args = $('sys-btargs').value.trim();
    const restart = $('sys-btargs-restart').checked;
    setText($('sys-btargs-msg'), 'saving…');
    put('/system/bluetoothd-args', {args, restart}).then(r =>
      setText($('sys-btargs-msg'), `saved "${r.args}"${r.restarted ? ', bluetooth restarted' : ', takes effect at the next bluetooth restart'}`))
      .catch(e => setText($('sys-btargs-msg'), e.message));
  };
  $('sys-j-load').onclick = loadJournal;
  $('sys-j-clear').onclick = () => logClear($('sys-journal'));
  const follow = () => {
    journalFollow = $('sys-j-follow').checked;
    if (journalFollow) put('/system/journal', {unit: $('sys-j-unit').value || 'bluetooth.service'}).catch(fail);
    updateTopics();
  };
  $('sys-j-follow').onchange = follow;
  $('sys-j-unit').onchange = () => { loadJournal(); if (journalFollow) follow(); };
  $('job-start').onclick = () => post('/jobs', {cmd: $('job-cmd').value, args: $('job-args').value})
    .then(j => { refreshJobs(); showJob(j.id); }).catch(fail);
  $('job-list').onclick = e => {
    const a = e.target.closest('[data-job]');
    const k = e.target.closest('[data-kill]');
    if (a) { e.preventDefault(); showJob(Number(a.dataset.job)); }
    if (k) del(`/jobs/${k.dataset.kill}`).then(() => setTimeout(refreshJobs, 300)).catch(fail);
  };
  $('sys-reboot').onclick = () => {
    if (!confirm('Reboot the board?')) return;
    post('/system/reboot').then(() => toast('Rebooting… the console reconnects by itself.')).catch(fail);
  };
}

// ---------------------------------------------------------------- log files (/tmp/logs)

function renderLogFiles(r) {
  setText($('lf-dir'), r.dir);
  setHTML($('lf-list'), r.files.length ? r.files.map(f => f.link
    ? `<div class="file-row"><span class="mono">${esc(f.name)}</span><span class="small muted">→ ${esc(f.link)}</span></div>`
    : `<div class="file-row"><span><a href="/api/system/logs/${encodeURIComponent(f.name)}?tail=2000" target="_blank" rel="noopener" class="mono">${esc(f.name)}</a>
        <span class="small muted">${fmtBytes(f.size)} · ${fmtTime(f.mtime_ms)}</span></span>
      <a class="small-btn btn-link" href="/api/system/logs/${encodeURIComponent(f.name)}?download=1">Download</a></div>`).join('')
    : '<div class="muted small">No log files yet: btbench-logs.service writes them on the board.</div>');
}

const refreshLogFiles = () => api('/system/logs').then(renderLogFiles).catch(() => {});

// ---------------------------------------------------------------- kernel Bluetooth logs

// The kernel's own Bluetooth debug messages (btbench-btdebug), by area. Each area says how many of
// its call sites print; a switch turns all of them on or off.
function renderBtDebug(s) {
  setHTML($('kd-areas'), s.areas.map(a => {
    const on = a.on > 0;
    return `<label class="set-row" title="${a.on} of ${a.total} messages print">
      <span class="set-t"><b>${esc(a.label)}</b><span class="hint">${a.total ? (on ? (a.on === a.total ? 'all ' + a.total + ' on' : a.on + ' of ' + a.total + ' on') : a.total + ' messages') : 'not loaded'}${a.kept ? ' · on from boot' : ''}</span></span>
      <span class="switch"><input type="checkbox" data-kd="${a.name}"${on ? ' checked' : ''}${a.total ? '' : ' disabled'}><i></i></span></label>`;
  }).join(''));
  if (idle($('kd-keep'))) $('kd-keep').checked = s.areas.some(a => a.kept);
  setText($('kd-msg'), '');
}

const refreshBtDebug = () => api('/system/btdebug').then(renderBtDebug).catch(e => {
  setHTML($('kd-areas'), '');
  setText($('kd-msg'), e.status === 503 ? 'Kernel Bluetooth logs are switched on the board only.' : e.message);
});

function setupBtDebug() {
  const on = () => [...document.querySelectorAll('#kd-areas input[data-kd]:checked')].map(i => i.dataset.kd);
  $('kd-areas').onchange = e => {
    const t = e.target.closest('input[data-kd]');
    if (!t) return;
    const body = {[t.checked ? 'on' : 'off']: [t.dataset.kd]};
    // Kept from boot follows what is on, while that is asked for.
    if ($('kd-keep').checked) body.keep = on();
    put('/system/btdebug', body).then(renderBtDebug).catch(e => { fail(e); refreshBtDebug(); });
  };
  $('kd-keep').onchange = e => put('/system/btdebug', {keep: e.target.checked ? on() : []}).then(renderBtDebug).catch(fail);
  $('kd-off').onclick = () => put('/system/btdebug', {off: ['all']}).then(renderBtDebug).catch(fail);
  $('kd-show').onclick = () => {
    $('sys-j-unit').value = 'kernel';
    $('sys-j-follow').checked = true;
    $('sys-j-unit').onchange();
    $('sys-j-follow').onchange();
    $('sys-journal').scrollIntoView({block: 'start', behavior: 'smooth'});
  };
}

registerTab('system', {
  topics: () => [].concat(journalFollow ? ['journal'] : [], jobSel ? ['job.' + jobSel] : []),
  show() {
    refreshSystem().then(() => { if (!$('sys-journal').textContent) loadJournal(); });
    refreshJobs();
    refreshBtDebug();
    refreshLogFiles();
  },
  onMessage(topic, d) {
    if (topic === 'system') renderHealth(d);
    else if (topic === 'journal' && journalFollow) logLine($('sys-journal'), journalLine(d));
    else if (jobSel && topic === 'job.' + jobSel) {
      if ('line' in d) logLine($('job-out'), (d.stream === 'err' ? '! ' : '') + d.line);
      else {
        logLine($('job-out'), `[exit ${d.exit}${d.killed ? ', killed' : ''}]`);
        refreshJobs();
      }
    }
  },
});
setupSystem();
setupBtDebug();
