'use strict';
// Capture: the always-on btmon ring (btbench-btsnoop.service) in /data/btsnoop. Files download as
// they are — btmon -r and Wireshark both read btsnoop — and "analyze" is btmon -a on the board.

let capState = null;
let capPoll = null;

function renderCapture(s) {
  capState = s;
  setText($('cap-state'), s.running ? 'recording' : s.state || 'stopped');
  setClass($('cap-state'), 'pill ' + (s.running ? 'good' : ''));
  setText($('cap-toggle'), s.running ? 'Stop' : 'Start');
  setText($('cap-dir'), `${s.unit} → ${s.dir}`);
  setHTML($('cap-files'), s.files.length ? `<tr><th>file</th><th>size</th><th>last written</th><th></th></tr>` +
    s.files.map(f => `<tr><td class="mono">${esc(f.name)}${f.active ? ' ' + pill('writing', 'good') : ''}</td>
      <td>${fmtBytes(f.size)}</td><td>${fmtDate(f.mtime)}</td>
      <td><a href="/api/capture/files/${encodeURIComponent(f.name)}" download>download</a>
        <button class="small-btn" data-an="${esc(f.name)}">analyze</button>
        ${f.active ? '' : `<button class="small-btn danger" data-rm="${esc(f.name)}">${armed['cap' + f.name] ? 'really?' : 'delete'}</button>`}</td></tr>`).join('')
    : '<tr><td class="muted">No captures.</td></tr>');
}

const refreshCapture = () => api('/capture').then(s => { setText($('cap-msg'), ''); renderCapture(s); })
  .catch(e => setText($('cap-msg'), e.message));

function setupCapture() {
  $('cap-toggle').onclick = () => put('/capture', {running: !(capState && capState.running)})
    .then(refreshCapture).catch(fail);
  $('cap-files').onclick = e => {
    const an = e.target.closest('button[data-an]');
    const rm = e.target.closest('button[data-rm]');
    if (an) {
      const name = an.dataset.an;
      $('cap-analyze').hidden = false;
      setText($('cap-an-title'), `btmon -a ${name}`);
      setText($('cap-an-text'), 'analyzing… (a 16 MiB file takes a while on the Zero W)');
      apiText(`/capture/files/${encodeURIComponent(name)}/analyze`)
        .then(t => setText($('cap-an-text'), t)).catch(e2 => setText($('cap-an-text'), e2.message));
    } else if (rm) {
      const name = rm.dataset.rm;
      if (!twoClick('cap' + name, () => capState && renderCapture(capState))) return;
      del(`/capture/files/${encodeURIComponent(name)}`).then(refreshCapture).catch(fail);
    }
  };
  $('cap-an-close').onclick = () => { $('cap-analyze').hidden = true; };
}

registerTab('capture', {
  show() {
    refreshCapture();
    clearInterval(capPoll);
    capPoll = setInterval(refreshCapture, 5000);
  },
  hide() { clearInterval(capPoll); capPoll = null; },
});
setupCapture();
