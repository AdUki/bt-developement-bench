'use strict';
// Kernel: the good/test/prev slots of btbench-kernel. A trial boots the test kernel once (U-Boot
// one-shot): a panic, a hang (watchdog) or a power cycle comes back to the good one. Committing
// makes the test kernel the good one.

function renderKernel(s) {
  const slot = (name, x) => x ? `<span>${name}</span><span>${x.present === false ? '—' : esc(x.version || '?')}</span>` : '';
  setHTML($('k-card'), `<div class="row between"><h3>Kernel</h3>${pill('running ' + s.slot, s.slot === 'test' ? 'warn' : 'good')}</div>
    <div class="kv">
      <span>Running</span><span class="mono">${esc(s.running)}</span>
      <span>Method</span><span>${esc(s.method)}${s.pending_try ? ' · a trial boot is pending' : ''}</span>
      ${slot('Good', s.good)}${slot('Test', s.test)}${slot('Previous', s.prev)}
      ${s.op ? `<span>Last action</span><span>${esc(s.op.op)}: ${s.op.running ? 'running…' : s.op.ok ? 'done' : 'failed: ' + esc(s.op.error)}</span>` : ''}
    </div>`);
  $('k-try').disabled = !(s.test && s.test.present);
  $('k-commit').disabled = !(s.test && s.test.present);
  $('k-rollback').disabled = !(s.prev && s.prev.present);
}

const refreshKernel = () => api('/kernel').then(s => { setText($('k-msg'), ''); renderKernel(s); }).catch(e => {
  setHTML($('k-card'), '<h3>Kernel</h3>');
  ['k-try', 'k-commit', 'k-rollback'].forEach(id => { $(id).disabled = true; });
  setText($('k-msg'), e.status === 503 ? 'Kernel trials run on the board only (btbench-kernel is not installed on this machine).' : e.message);
});

function setupKernel() {
  $('k-try').onclick = () => {
    if (!confirm('Reboot into the test kernel once? The board goes away for a minute; if the test kernel fails it comes back on the good one.')) return;
    post('/kernel/try').then(() => setText($('k-msg'), 'Rebooting into the test kernel… this page reconnects by itself.')).catch(fail);
  };
  $('k-commit').onclick = () => {
    if (!confirm('Make the test kernel the good one? The current good kernel becomes "previous".')) return;
    post('/kernel/commit').then(renderKernel).catch(fail);
  };
  $('k-rollback').onclick = () => {
    if (!confirm('Put the previous kernel back as the good one?')) return;
    post('/kernel/rollback').then(renderKernel).catch(fail);
  };
}

registerTab('kernel', {show: refreshKernel});
setupKernel();
