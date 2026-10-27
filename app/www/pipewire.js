'use strict';
// Audio → PipeWire: the audio server itself. Its services (on, off, restart), its clock settings,
// the default sink and source, every node of its graph — state, format, latency, what drives it,
// its volume and mute, what it is linked to, its raw props folded — the Bluetooth cards with their
// codec pickers, and pw-top's timing while the view is open. The graph is one pw-dump
// (GET /api/audio/pipewire/graph), read when the view opens, after each of its own changes and
// when the board's audio links or streams change: there is no topic for it, and no timer.

const pw = {graph: null, open: {}, linksKey: null, streamsKey: null, top: null};

// ---------------------------------------------------------------- services

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

// ---------------------------------------------------------------- the graph

const refreshGraph = fresh => api('/audio/pipewire/graph' + (fresh ? '?fresh=1' : '')).then(renderGraph)
  .catch(e => ldFail(e, $('pw-nodes'), $('pw-clock'), $('pw-defaults')));
const refreshCards = () => api('/audio/devices').then(r => { au.devices = r; renderCards(); })
  .catch(e => ldFail(e, $('pw-cards')));

// A setter's answer is the graph after it.
function pwSet(path, body) {
  return put('/audio/pipewire/' + path, body).then(renderGraph).catch(e => { fail(e); refreshGraph(true); });
}

const KIND_HEAD = {sink: 'Sinks', source: 'Sources', duplex: 'Duplex', stream_out: 'Playing streams',
  stream_in: 'Recording streams', driver: 'Drivers', other: 'Other'};
const nodeName = (g, id) => { const n = g.nodes.find(x => x.id === id); return n ? n.description : `node ${id}`; };
const fmtFormat = f => !f ? '' : [f.format, f.rate ? f.rate / 1000 + ' kHz' : '', f.channels ? f.channels + ' ch' : ''].filter(Boolean).join(' · ');

function renderGraph(g) {
  pw.graph = g;
  setText($('pw-graph-note'), g.error || '');
  renderClock(g);
  renderDefaults(g);
  renderNodes(g);
}

// The clock: what PipeWire's settings ask for, each a field of its own; empty is PipeWire's own
// value (Default). What the graph runs at is pw-top's, said above them.
const CLOCK_KEYS = [['force_quantum', 'Force quantum'], ['force_rate', 'Force rate'], ['quantum', 'Quantum'],
  ['min_quantum', 'Min quantum'], ['max_quantum', 'Max quantum'], ['rate', 'Rate']];

function renderClock(g) {
  const c = g.clock;
  if (holdingIn($('pw-clock'))) return;
  if (!c) { setHTML($('pw-clock'), `<div class="small muted">${esc(g.error || 'PipeWire has no settings metadata')}</div>`); return; }
  const off = !g.tools['pw-metadata'] ? ' disabled' : '';
  const run = runningClock();
  setHTML($('pw-clock'), (run ? `<div class="small muted pw-run">Running: ${esc(run)}</div>` : '') + `<div class="grid pw-clock">
    ${CLOCK_KEYS.map(([k, label]) => `<label>${label} <span class="row"><input type="number" class="grow" data-clock="${k}" min="0"
      value="${c[k] ? c[k] : ''}" placeholder="default"${off}><button class="small-btn" data-clock-default="${k}"${off || !c[k] ? ' disabled' : ''}>Default</button></span></label>`).join('')}
    <label class="wide">Allowed rates <span class="row"><input type="text" class="grow mono" data-clock="allowed_rates"
      value="${esc((c.allowed_rates || []).join(', '))}" placeholder="the configured ones"${off}><button class="small-btn" data-clock-default="allowed_rates"${off || !(c.allowed_rates || []).length ? ' disabled' : ''}>Default</button></span></label>
  </div>`);
}

// The quantum and rate the running drivers cycle at, from pw-top's last table.
function runningClock() {
  const ds = pw.top && pw.top.drivers ? pw.top.drivers.filter(d => /[RT]/.test(d.state) && d.quantum != null) : [];
  return ds.map(d => `${d.quantum}/${d.rate} (${d.name})`).join(', ');
}

function renderDefaults(g) {
  if (holdingIn($('pw-defaults'))) return;
  const pick = (cls, key) => {
    const ns = g.nodes.filter(n => n.can_default && n.media_class.startsWith(cls));
    const cur = g.defaults[key], chosen = g.defaults['configured_' + key];
    const gone = chosen && chosen !== cur ? `<div class="small muted">Chosen: <span class="mono">${esc(chosen)}</span>, not there now</div>` : '';
    return `<label>Default ${key} <select data-default="${key}"${g.tools.wpctl ? '' : ' disabled'}>
      ${ns.some(n => n.name === cur) ? '' : `<option value="">${esc(cur || 'none')}</option>`}${ns.map(n => `<option value="${n.id}"${n.name === cur ? ' selected' : ''}>${esc(n.description)}</option>`).join('')}
      </select></label>${gone}`;
  };
  setHTML($('pw-defaults'), `<div class="grid">${pick('Audio/Sink', 'sink')}${pick('Audio/Source', 'source')}</div>`);
}

function renderNodes(g) {
  if (holdingIn($('pw-nodes'))) return;
  const out = {}, inn = {};
  g.links.forEach(l => { (out[l.output_node] = out[l.output_node] || []).push(l); (inn[l.input_node] = inn[l.input_node] || []).push(l); });
  const linkLine = (l, to) => {
    const ch = l.ports.map(p => p.channel).filter(Boolean).join(' ');
    return `<div class="pw-link${l.state === 'active' ? '' : ' muted'}">${to ? '→' : '←'} ${esc(nodeName(g, to ? l.input_node : l.output_node))}` +
      `${ch ? ` <span class="mono small">${esc(ch)}</span>` : ''}${l.state === 'active' ? '' : ' ' + pill(l.state, l.state === 'error' ? 'bad' : '')}${l.passive ? ' <span class="small muted">passive</span>' : ''}</div>`;
  };
  let html = '', kind = '';
  for (const n of g.nodes) {
    if (n.kind !== kind) { kind = n.kind; html += `<div class="in-head small muted">${KIND_HEAD[kind] || kind}</div>`; }
    const v = n.volume;
    const facts = [n.media_class, fmtFormat(n.format), n.latency_ms != null ? `latency ${n.latency_ms} ms` : '',
      n.driving ? 'drives the graph' : n.driver_id != null ? 'driven by ' + nodeName(g, n.driver_id) : '',
      n.application ? n.application.name : '', n.bluetooth && n.bluetooth.codec ? n.bluetooth.codec.toUpperCase() : ''].filter(Boolean).join(' · ');
    const key = 'n:' + n.name;
    html += `<div class="pw-node${n.state === 'running' ? '' : ' idle'}">
      <div class="pw-node-head">${pill(n.state, n.state === 'running' ? 'good' : n.state === 'error' ? 'bad' : '')}
        <b>${esc(n.description)}</b>${n.default ? ' <span class="pill good">default</span>' : ''}
        <span class="mono small muted">${n.id} · ${esc(n.name)}</span></div>
      <div class="small muted">${esc(facts)}${n.error ? ` <span class="bad-text">${esc(n.error)}</span>` : ''}</div>
      ${v ? `<div class="row small pw-vol"><button class="small-btn${v.mute ? ' on' : ''}" data-mute="${n.id}" aria-pressed="${v.mute}">${v.mute ? 'Muted' : 'Mute'}</button>
        <input type="range" class="grow" min="0" max="1.5" step="0.01" value="${v.volume}" data-vol="${n.id}" aria-label="Volume">
        <span class="mono pw-vol-v">${Math.round(v.volume * 100)}%</span></div>` : ''}
      ${(out[n.id] || []).map(l => linkLine(l, true)).join('')}${(inn[n.id] || []).map(l => linkLine(l, false)).join('')}
      <details class="more-fields" data-key="${esc(key)}"${pw.open[key] ? ' open' : ''}><summary>Props</summary>
        <div class="kv small">${Object.entries(n.props || {}).map(([k, x]) => `<span class="mono">${esc(k)}</span><span class="mono">${esc(typeof x === 'string' ? x : JSON.stringify(x))}</span>`).join('')}</div>
      </details></div>`;
  }
  setHTML($('pw-nodes'), html || `<div class="small muted">${esc(g.error || 'No nodes')}</div>`);
}

function renderCards() {
  if (holdingIn($('pw-cards'))) return;
  const ds = (au.devices && au.devices.devices) || [];
  setHTML($('pw-cards'), ds.length ? ds.map(d => `<div class="row wrap pw-card"><span class="grow"><b>${esc(devName(d.address) !== d.address ? devName(d.address) : d.label)}</b>
      <span class="mono small muted">${esc(d.address)} · ${esc(d.name)}</span></span>${codecSelect(d)}</div>`).join('')
    : `<div class="small muted">${esc((au.devices && au.devices.error) || 'No Bluetooth device has a PipeWire card')}</div>`);
}

const holdingIn = el => {
  const ae = document.activeElement;
  return !!ae && el.contains(ae) && (ae.type === 'range' || ae.type === 'number' || ae.type === 'text' || ae.tagName === 'SELECT');
};

// ---------------------------------------------------------------- PipeWire's timing (pw-top)

// The drivers, running ones first, each with its followers under it.
function renderPipewire(t) {
  pw.top = t;
  if (!t.drivers) {
    setText($('pw-note'), t.error ? 'pw-top: ' + t.error : 'waiting for pw-top…');
    return;
  }
  setText($('pw-note'), `${fmtTime(t.ts)}${t.xruns ? ` · ${t.xruns} xrun${t.xruns === 1 ? '' : 's'} this second` : ''}`);
  const run = runningClock(), runEl = document.querySelector('#pw-clock .pw-run');
  if (runEl) setText(runEl, run ? 'Running: ' + run : '');
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

// ---------------------------------------------------------------- wiring

function setupPipewire() {
  // The whole stack on or off, kept across reboots (btbench-audio on|off).
  $('au-on').onchange = e => {
    const on = e.target.checked;
    if (!on && !confirm('Switch the audio stack off? Audio links are dropped, and it stays off after a reboot until switched on here.')) {
      e.target.checked = true;
      return;
    }
    e.target.disabled = true;
    setText($('au-msg'), on ? 'starting PipeWire…' : 'stopping PipeWire…');
    put('/audio', {enabled: on}).then(s => { renderAudio(s); setText($('au-msg'), ''); refreshGraph(true); refreshCards(); })
      .catch(err => { setText($('au-msg'), err.message); refreshAudio(); })
      .finally(() => { e.target.disabled = false; });
  };
  $('au-restart').onclick = () => {
    const restart = $('au-restart-bt').checked;
    if (!confirm(`Restart PipeWire and WirePlumber${restart ? ' and bluetoothd' : ''}? Audio connections are dropped.`)) return;
    $('au-restart').disabled = true;
    setText($('au-msg'), 'restarting…');
    post('/audio/restart', {restart_bt: restart}).then(s => { renderAudio(s); setText($('au-msg'), 'restarted'); refreshGraph(true); refreshCards(); })
      .catch(e2 => { setText($('au-msg'), e2.message); refreshAudio(); })
      .finally(() => { $('au-restart').disabled = false; });
  };
  $('pw-reload').onclick = () => { refreshGraph(true); refreshCards(); };

  const panel = $('pipewire');
  panel.addEventListener('input', e => {
    const t = e.target;
    if (t.dataset.vol != null) setText(t.parentElement.querySelector('.pw-vol-v'), Math.round(parseFloat(t.value) * 100) + '%');
  });
  panel.addEventListener('change', e => {
    const t = e.target;
    if (t.dataset.vol != null) pwSet(`nodes/${t.dataset.vol}/volume`, {volume: parseFloat(t.value)});
    else if (t.dataset.default != null && t.value) pwSet('default', {node: parseInt(t.value, 10)});
    else if (t.dataset.clock === 'allowed_rates') {
      const rates = t.value.split(/[\s,]+/).filter(Boolean).map(Number);
      pwSet('clock', {allowed_rates: rates.length ? rates : null});
    } else if (t.dataset.clock != null) {
      const v = parseInt(t.value, 10);
      pwSet('clock', {[t.dataset.clock]: v > 0 ? v : null});
    } else if (t.matches('select.cd-pick')) pickCodec(t, () => { refreshGraph(true); refreshCards(); });
    else return;
    // Let go of: the view redraws again (it waits while a field has the focus).
    t.blur();
  });
  panel.addEventListener('click', e => {
    const m = e.target.closest('[data-mute]');
    if (m) return pwSet(`nodes/${m.dataset.mute}/mute`, {mute: 'toggle'});
    const d = e.target.closest('[data-clock-default]');
    if (d) pwSet('clock', {[d.dataset.clockDefault]: null});
  });
  panel.addEventListener('toggle', e => {
    if (e.target.dataset && e.target.dataset.key) pw.open[e.target.dataset.key] = e.target.open;
  }, true);
}

registerTab('pipewire', {
  // media and the streams only say when the graph changed: a link or a stream that came or went.
  topics: () => ['pipewire', 'media', 'audio.streams'],
  show() {
    refreshAudio();
    refreshGraph(false);
    refreshCards();
  },
  onMessage(topic, data) {
    // Graphs can keep the pipewire topic while this view is not on screen: no table to redraw then.
    if (currentTab !== 'pipewire') return;
    if (topic === 'pipewire') return renderPipewire(data);
    if (topic !== 'media' && topic !== 'audio.streams') return;
    const key = topic === 'media' ? data.transports.map(t => t.path + t.state + t.configuration).join()
      : data.streams.map(s => s.id + s.state + (s.sinks || []).length).join();
    const was = topic === 'media' ? pw.linksKey : pw.streamsKey;
    if (key === was) return;
    if (topic === 'media') pw.linksKey = key; else pw.streamsKey = key;
    if (was === null) return;  // the first message only says how things are
    clearTimeout(pw.timer);
    pw.timer = setTimeout(() => { refreshGraph(true); if (topic === 'media') refreshCards(); }, 1500);
  },
});
setupPipewire();
