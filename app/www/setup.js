'use strict';
// Setup: profiles on and off, BlueZ's plugins, main.conf settings (Bluetooth) and WirePlumber's
// (Audio). Changes collect in a draft; the bar at the bottom says what applying them restarts
// (bluetoothd, WirePlumber, obexd) — a restart drops every connection, so it is never a side
// effect of flipping one switch.

const su = {state: null, draft: {profiles: {}, plugins: {}, settings: {}}, open: {}};

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
  for (const k of Object.keys(su.draft.settings)) r.add(/^(Monitor|Device|Node|Settings)\./.test(k) ? 'WirePlumber' : 'bluetooth');
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
      // available: false — the stack here lacks a part of it (a bluetoothd plugin); note: why, or
      // what no adapter reports (LE Audio's CIS and broadcast).
      const off = p.available === false && !on;
      return `<label class="set-row${p.id in su.draft.profiles ? ' changed' : ''}">
        <span class="set-t"><b>${esc(p.label)}</b><span class="hint">${esc(p.help)}</span>${p.note ? `<span class="hint warn-text">${esc(p.note)}</span>` : ''}</span>
        <span class="switch"><input type="checkbox" data-profile="${p.id}"${on ? ' checked' : ''}${off ? ' disabled' : ''}><i></i></span></label>`;
    }).join('')}</div>`).join(''));
  // Settings: main.conf on one view, WirePlumber's on the other, by section. What Default means
  // is said when the stack tells: the image's value (`image`), else BlueZ's or WirePlumber's (`default`).
  const fmt = (st, x) => {
    if (x == null) return '';
    if (st.type === 'bool') return x ? 'on' : 'off';
    if (st.type === 'enum') { const o = st.options.find(o => o.value === x); return o ? o.label : x; }
    if (st.type === 'list') { const n = x.filter(v => !(st.options.find(o => o.value === v) || {}).missing).length; return n ? n + ' chosen' : 'none'; }
    if (st.type === 'number') return `${x} ${st.unit || ''}`.trim();
    return String(x);
  };
  const dflt = st => st.image != null ? st.image : st.default;
  const dfltLabel = st => dflt(st) == null ? 'Default' : `Default (${fmt(st, dflt(st))})`;
  const settingRow = st => {
    const v = cur('settings', st.key, st.value);
    const changed = st.key in su.draft.settings;
    let ctl;
    if (st.type === 'bool') {
      ctl = `<select data-setting="${esc(st.key)}" data-type="bool">
        <option value=""${v == null ? ' selected' : ''}>${esc(dfltLabel(st))}</option>
        <option value="true"${v === true ? ' selected' : ''}>On</option><option value="false"${v === false ? ' selected' : ''}>Off</option></select>`;
    } else if (st.type === 'enum') {
      ctl = `<select data-setting="${esc(st.key)}" data-type="enum"><option value=""${v == null ? ' selected' : ''}>${esc(dfltLabel(st))}</option>
        ${st.options.map(o => `<option value="${esc(o.value)}"${v === o.value ? ' selected' : ''}>${esc(o.label)}</option>`).join('')}</select>`;
    } else if (st.type === 'list') {
      // Unset: the image's (or BlueZ's) choice, else everything there is.
      const usable = st.options.filter(o => !o.missing && !o.disabled).map(o => o.value);
      const base = dflt(st);
      const sel = v != null ? v : base != null ? base.filter(x => usable.includes(x)) : usable;
      // A codec whose plugin is not installed shows only while it is chosen.
      const opts = st.options.filter(o => !o.missing || sel.includes(o.value));
      ctl = `<details class="more right"><summary>${v == null ? esc(dfltLabel(st)) : sel.length + ' chosen'}</summary><div class="pop codec-pick${opts.some(o => o.note) ? ' wide' : ''}">
        ${opts.map(o => `<label class="check"${o.note ? ` title="${esc(o.value)}"` : ''}><input type="checkbox" data-list="${esc(st.key)}" value="${esc(o.value)}"${sel.includes(o.value) ? ' checked' : ''}${o.disabled ? ' disabled' : ''}>
          <span>${esc(o.label)}${o.missing ? ' <span class="muted small">(not installed)</span>' : ''}${o.note ? `<br><span class="muted small">${esc(o.note)}</span>` : ''}</span></label>`).join('')}
        <button class="small-btn" data-list-default="${esc(st.key)}">Back to default</button></div></details>`;
    } else if (st.type === 'text') {
      ctl = `<input type="text" class="set-text" data-setting="${esc(st.key)}" data-type="text" value="${esc(v == null ? '' : v)}" placeholder="${esc(dflt(st) == null ? 'default' : 'default: ' + dflt(st))}">`;
    } else {
      ctl = `<span class="num"><input type="number" data-setting="${esc(st.key)}" data-type="number" min="${st.min}" max="${st.max}" step="${st.step}"
        value="${v == null ? '' : v}" placeholder="${dflt(st) == null ? 'default' : 'default: ' + esc(dflt(st))}"><span class="muted small">${esc(st.unit)}</span></span>`;
    }
    return `<div class="set-row${changed ? ' changed' : ''}"><span class="set-t"><b>${esc(st.label)}</b>
      <span class="hint${st.more ? ' pre' : ''}">${esc(st.help)}</span>${st.note ? `<span class="hint warn-text">${esc(st.note)}</span>` : ''}</span>${ctl}</div>`;
  };
  const section = k => k.split('.')[0];
  const SECT = {General: 'General', BR: 'Classic (BR/EDR)', LE: 'Low Energy', GATT: 'GATT', Policy: 'Policy plugin',
    ChannelSounding: 'Channel Sounding', CSIS: 'Coordinated sets (CSIS)', AVDTP: 'AVDTP', AVRCP: 'AVRCP', AdvMon: 'Advertisement monitor',
    Monitor: 'Bluetooth in PipeWire', Device: 'Every Bluetooth device', Node: 'Every Bluetooth stream', Settings: 'WirePlumber'};
  // A setting for a part this machine lacks (a codec plugin) shows only while it is set.
  const shown = st => st.available !== false || st.value != null || st.key in su.draft.settings;
  const settingsView = file => {
    const list = s.settings.filter(st => st.file === file && shown(st));
    return [...new Set(list.map(st => section(st.key)))].map(sec => {
      const main = list.filter(st => section(st.key) === sec && !st.more);
      // The rest of main.conf's keys, as BlueZ documents them: folded away, open while one is set.
      const more = list.filter(st => section(st.key) === sec && st.more);
      const open = more.some(st => st.value != null || st.key in su.draft.settings);
      const fold = more.length ? `<details class="more-fields"${open || su.open[sec] ? ' open' : ''} data-sect="${esc(sec)}"><summary>${main.length ? 'More' : 'All'} keys in main.conf (${more.length})</summary>
        ${more.map(settingRow).join('')}</details>` : '';
      return `<div class="card"><h3>${esc(SECT[sec] || sec)}</h3>${main.map(settingRow).join('')}${fold}</div>`;
    }).join('');
  };
  // The adapters as the kernel reports them: what the LE Audio profiles and the kernel's
  // experimental features depend on.
  const NOTABLE = ['br/edr', 'le', 'secure-conn', 'privacy', 'll-privacy', 'wide-band-speech', 'cis-central', 'cis-peripheral', 'iso-broadcaster', 'sync-receiver'];
  const adapters = s.controllers && s.controllers.length ? `<div class="card"><h3>Adapters</h3>
    <p class="hint">What the kernel says each adapter can do (mgmt). LE Audio needs CIS (unicast) or ISO broadcast; the kernel's experimental features are offered per adapter.</p>
    <div class="kv">${s.controllers.map(c => `<span class="mono">hci${c.index}</span><span><span class="mono">${esc(c.address || '?')}</span>
      ${(c.supported || []).filter(x => NOTABLE.includes(x)).map(x => pill(x, (c.current || []).includes(x) ? 'good' : '')).join(' ')}</span>`).join('')}</div></div>` : '';
  setHTML($('su-bluetooth'), adapters + settingsView('main.conf'));
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
      let v = t.value.trim() === '' ? null : t.dataset.type === 'bool' ? t.value === 'true' : t.dataset.type === 'number' ? parseFloat(t.value) : t.dataset.type === 'text' ? t.value.trim() : t.value;
      if (t.dataset.type === 'number' && v != null && (v < st.min || v > st.max)) { toast(`${st.label}: ${st.min}..${st.max} ${st.unit}`); v = st.value; }
      setDraft('settings', st.key, v, st.value);
    } else if (t.dataset.list) {
      const st = s.settings.find(x => x.key === t.dataset.list);
      const v = [...panel.querySelectorAll(`input[data-list="${CSS.escape(st.key)}"]:checked`)].map(i => i.value);
      setDraft('settings', st.key, v, st.value);
    }
  });
  // A folded section stays open across renders once opened.
  panel.addEventListener('toggle', e => {
    const d = e.target;
    if (d.dataset && d.dataset.sect) su.open[d.dataset.sect] = d.open;
  }, true);
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
