'use strict';
// Setup: profiles on and off, BlueZ's plugins, main.conf settings (Bluetooth) and WirePlumber's
// (Audio). Changes collect in a draft; the bar at the bottom says what applying them restarts
// (bluetoothd, WirePlumber, obexd) — a restart drops every connection, so it is never a side
// effect of flipping one switch.

const su = {state: null, draft: {profiles: {}, plugins: {}, settings: {}}};

const draftCount = () => Object.keys(su.draft.profiles).length + Object.keys(su.draft.plugins).length + Object.keys(su.draft.settings).length;

// What a draft change would restart, by the layer it is in.
function draftRestarts() {
  const s = su.state;
  const r = new Set();
  for (const id of Object.keys(su.draft.profiles)) {
    const p = s.profiles.find(x => x.id === id);
    (p ? p.layers : []).forEach(l => r.add({bluetoothd: 'bluetooth', pipewire: 'WirePlumber', obexd: 'obexd', btbenchd: ''}[l]));
  }
  if (Object.keys(su.draft.plugins).length) r.add('bluetooth');
  for (const k of Object.keys(su.draft.settings)) r.add(/^(Monitor|Device|Settings)\./.test(k) ? 'WirePlumber' : 'bluetooth');
  r.delete('');
  return [...r];
}

const cur = (kind, key, base) => (key in su.draft[kind] ? su.draft[kind][key] : base);

function renderSetup() {
  const s = su.state;
  if (!s) return;
  // Profiles, by group.
  const groups = [...new Set(s.profiles.map(p => p.group))];
  setHTML($('su-profiles'), groups.map(g => `<div class="card"><h3>${esc(g)}</h3>
    ${s.profiles.filter(p => p.group === g).map(p => {
      const on = cur('profiles', p.id, p.on);
      return `<label class="set-row${p.id in su.draft.profiles ? ' changed' : ''}">
        <span class="set-t"><b>${esc(p.label)}</b><span class="hint">${esc(p.help)}</span></span>
        <span class="switch"><input type="checkbox" data-profile="${p.id}"${on ? ' checked' : ''}><i></i></span></label>`;
    }).join('')}</div>`).join(''));
  // Settings: main.conf on one view, WirePlumber's on the other, by section.
  const settingRow = st => {
    const v = cur('settings', st.key, st.value);
    const changed = st.key in su.draft.settings;
    let ctl;
    if (st.type === 'bool') {
      ctl = `<select data-setting="${esc(st.key)}" data-type="bool">
        <option value=""${v == null ? ' selected' : ''}>Default</option>
        <option value="true"${v === true ? ' selected' : ''}>On</option><option value="false"${v === false ? ' selected' : ''}>Off</option></select>`;
    } else if (st.type === 'enum') {
      ctl = `<select data-setting="${esc(st.key)}" data-type="enum"><option value=""${v == null ? ' selected' : ''}>Default</option>
        ${st.options.map(o => `<option value="${esc(o.value)}"${v === o.value ? ' selected' : ''}>${esc(o.label)}</option>`).join('')}</select>`;
    } else if (st.type === 'list') {
      const sel = v || [];
      ctl = `<details class="more right"><summary>${v == null ? 'Default' : sel.length + ' chosen'}</summary><div class="pop codec-pick">
        ${st.options.map(o => `<label class="check"><input type="checkbox" data-list="${esc(st.key)}" value="${esc(o.value)}"${v == null || sel.includes(o.value) ? ' checked' : ''}> ${esc(o.label)}</label>`).join('')}
        <button class="small-btn" data-list-default="${esc(st.key)}">Back to default</button></div></details>`;
    } else {
      ctl = `<span class="num"><input type="number" data-setting="${esc(st.key)}" data-type="number" min="${st.min}" max="${st.max}" step="${st.step}"
        value="${v == null ? '' : v}" placeholder="default"><span class="muted small">${esc(st.unit)}</span></span>`;
    }
    return `<div class="set-row${changed ? ' changed' : ''}"><span class="set-t"><b>${esc(st.label)}</b>
      <span class="hint">${esc(st.help)}</span></span>${ctl}</div>`;
  };
  const section = k => k.split('.')[0];
  const SECT = {General: 'General', BR: 'Classic (BR/EDR)', LE: 'Low Energy', GATT: 'GATT', Policy: 'Policy plugin',
    Monitor: 'Bluetooth in PipeWire', Device: 'Every Bluetooth device', Settings: 'WirePlumber'};
  const settingsView = file => {
    const list = s.settings.filter(st => st.file === file);
    return [...new Set(list.map(st => section(st.key)))].map(sec => `<div class="card"><h3>${esc(SECT[sec] || sec)}</h3>
      ${list.filter(st => section(st.key) === sec).map(settingRow).join('')}</div>`).join('');
  };
  setHTML($('su-bluetooth'), settingsView('main.conf'));
  setHTML($('su-audio'), settingsView('wireplumber'));
  setHTML($('su-plugins'), `<div class="card"><h3>BlueZ plugins</h3>
    <p class="hint">bluetoothd starts without the plugins switched off here. Profiles switch the plugins they need too.</p>
    ${s.plugins.map(p => {
      const on = cur('plugins', p.name, p.on);
      return `<label class="set-row${p.name in su.draft.plugins ? ' changed' : ''}"><span class="set-t"><b>${esc(p.label)}</b> <span class="mono small muted">${esc(p.name)}</span>
        <span class="hint">${esc(p.help)}</span></span>
        <span class="switch"><input type="checkbox" data-plugin="${p.name}"${on ? ' checked' : ''}><i></i></span></label>`;
    }).join('')}</div>`);
  // The bar.
  const n = draftCount();
  $('su-bar').hidden = !n;
  const r = draftRestarts();
  setText($('su-bar-text'), `${n} change${n === 1 ? '' : 's'}` + (r.length ? ` · applying restarts ${r.join(' and ')} (connections drop)` : ''));
  setText($('su-note'), s.apply ? '' : 'On a PC: the files are written to the data directory; the desktop’s Bluetooth and PipeWire are left alone.');
}

function setDraft(kind, key, value, base) {
  if (JSON.stringify(value) === JSON.stringify(base)) delete su.draft[kind][key];
  else su.draft[kind][key] = value;
  renderSetup();
}

const refreshSetup = () => api('/setup').then(s => { su.state = s; renderSetup(); }).catch(fail);

function setupSetup() {
  const panel = $('setup');
  panel.addEventListener('change', e => {
    const t = e.target;
    const s = su.state;
    if (t.dataset.profile) setDraft('profiles', t.dataset.profile, t.checked, s.profiles.find(p => p.id === t.dataset.profile).on);
    else if (t.dataset.plugin) setDraft('plugins', t.dataset.plugin, t.checked, s.plugins.find(p => p.name === t.dataset.plugin).on);
    else if (t.dataset.setting) {
      const st = s.settings.find(x => x.key === t.dataset.setting);
      let v = t.value === '' ? null : t.dataset.type === 'bool' ? t.value === 'true' : t.dataset.type === 'number' ? parseFloat(t.value) : t.value;
      if (t.dataset.type === 'number' && v != null && (v < st.min || v > st.max)) { toast(`${st.label}: ${st.min}..${st.max} ${st.unit}`); v = st.value; }
      setDraft('settings', st.key, v, st.value);
    } else if (t.dataset.list) {
      const st = s.settings.find(x => x.key === t.dataset.list);
      const v = [...panel.querySelectorAll(`input[data-list="${CSS.escape(st.key)}"]:checked`)].map(i => i.value);
      setDraft('settings', st.key, v, st.value);
    }
  });
  panel.addEventListener('click', e => {
    const d = e.target.closest('[data-list-default]');
    if (!d) return;
    const st = su.state.settings.find(x => x.key === d.dataset.listDefault);
    setDraft('settings', st.key, null, st.value);
  });
  $('su-discard').onclick = () => { su.draft = {profiles: {}, plugins: {}, settings: {}}; renderSetup(); };
  $('su-apply').onclick = () => {
    $('su-apply').disabled = true;
    setText($('su-bar-text'), 'Applying… services are restarting');
    put('/setup', su.draft).then(s => {
      su.state = s;
      su.draft = {profiles: {}, plugins: {}, settings: {}};
      renderSetup();
      toast(s.restarted.length ? 'Applied: restarted ' + s.restarted.join(', ') : s.pending ? 'Saved (applies on the board)' : 'Applied');
    }).catch(e => { fail(e); refreshSetup(); }).finally(() => { $('su-apply').disabled = false; });
  };
}

registerTab('setup', {show: refreshSetup});
setupSetup();
