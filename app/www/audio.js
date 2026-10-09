'use strict';
// Audio: which stack owns Bluetooth audio (btbench-audio), and what BlueZ's media objects say —
// the same in every mode, since endpoints and transports are BlueZ's own.

const AUDIO_MODES = ['pipewire', 'bluealsa', 'none'];

function renderAudio(s) {
  setText($('au-mode'), s.mode);
  setClass($('au-mode'), 'pill good');
  setHTML($('au-buttons'), (s.modes || AUDIO_MODES).map(m =>
    `<button data-mode="${esc(m)}"${m === s.mode ? ' disabled' : ''}>${esc(m)}</button>`).join(' '));
  setText($('au-services'), Object.entries(s.services || {}).map(([k, v]) => `${k}: ${v}`).join(' · '));
}

function refreshAudio() {
  return api('/audio').then(s => { renderAudio(s); setText($('au-msg'), ''); }).catch(e => {
    setText($('au-mode'), e.status === 503 ? 'not here' : 'error');
    setHTML($('au-buttons'), '');
    // On a PC the target scripts are not installed; say so plainly rather than as an error.
    setText($('au-msg'), e.status === 503 ? 'The audio-mode switch runs on the board only (btbench-audio is not installed on this machine).' : e.message);
  });
}

function codecCell(c) {
  return `<b>${esc(c.name)}</b> <span class="small">${esc(c.summary)}</span>`;
}

function renderMedia(m) {
  setHTML($('au-transports'), m.transports.length ? m.transports.map(t => `<div class="card dev">
      <div class="head"><span><b>${esc(t.profile || t.uuid)}</b> with <span class="mono">${esc(t.address)}</span></span>
        ${pill(t.state, t.state === 'active' ? 'good' : t.state === 'pending' ? 'warn' : '')}</div>
      <div class="small">${codecCell(t.codec)}</div>
      <div class="kv"><span>Configuration</span><span class="mono">${esc(t.configuration)}</span>
        <span>Delay</span><span>${t.delay_ms != null ? t.delay_ms.toFixed(1) + ' ms' : '—'}</span>
        <span>Path</span><span class="mono small">${esc(t.path)}</span></div>
      ${t.volume != null ? `<label class="row">Volume <input type="range" min="0" max="127" value="${t.volume}" data-path="${esc(t.path)}" class="grow">
        <span class="mono">${t.volume}</span></label>` : ''}
    </div>`).join('') : '<div class="small muted">No transport: nothing is configured to stream.</div>');
  setHTML($('au-endpoints'), m.endpoints.length ? `<div class="scroll-x"><table class="tbl">
      <tr><th>Device</th><th>Role</th><th>Codec</th><th>Capabilities</th><th>Delay rep.</th></tr>
      ${m.endpoints.map(e => `<tr><td class="mono">${esc(e.address)}</td><td>${esc(e.role || e.uuid)}</td>
        <td>${esc(e.codec.name)}</td><td class="small">${esc(e.codec.summary)} <span class="mono muted">${esc(e.capabilities)}</span></td>
        <td>${e.delay_reporting ? 'yes' : 'no'}</td></tr>`).join('')}</table></div>`
    : '<div class="small muted">No remote endpoints (connect an A2DP device).</div>');
  setHTML($('au-players'), m.players.length ? m.players.map(p => `<div class="card dev">
      <div class="head"><span><b>${esc(p.name || 'Player')}</b> on <span class="mono">${esc(p.address)}</span></span>${pill(p.status)}</div>
      <div class="small">${esc([p.track.title, p.track.artist, p.track.album].filter(Boolean).join(' — ') || 'no track')}
        <span class="mono muted">${Math.round(p.position_ms / 1000)} s${p.track.duration_ms ? ' / ' + Math.round(p.track.duration_ms / 1000) + ' s' : ''}</span></div>
    </div>`).join('') : '<div class="small muted">No AVRCP players.</div>');
}

function refreshMedia() { return api('/media').then(renderMedia).catch(fail); }

function setupAudio() {
  $('au-buttons').onclick = e => {
    const b = e.target.closest('button[data-mode]');
    if (!b) return;
    const mode = b.dataset.mode;
    const restart = $('au-restart-bt').checked;
    if (!confirm(`Switch the audio stack to ${mode}? Audio connections are dropped${restart ? ' and bluetoothd restarts' : ''}.`)) return;
    setText($('au-msg'), `switching to ${mode}…`);
    put('/audio', {mode, restart_bt: restart}).then(s => { renderAudio(s); setText($('au-msg'), `now ${s.mode}`); })
      .catch(e2 => { setText($('au-msg'), e2.message); refreshAudio(); });
  };
  // Sent as the slider settles, not on every pixel of a drag.
  $('au-transports').onchange = e => {
    const r = e.target.closest('input[type=range]');
    if (r) put('/media/transport', {path: r.dataset.path, volume: parseInt(r.value, 10)}).catch(fail);
  };
}

registerTab('audio', {
  topics: () => ['media'],
  show() { refreshAudio(); refreshMedia(); },
  onMessage(topic, data) {
    // Not under a volume drag: the rebuild would take the slider from under the pointer.
    const ae = document.activeElement;
    if (topic === 'media' && currentTab === 'audio' && !(ae && ae.type === 'range')) renderMedia(data);
  },
});
setupAudio();
