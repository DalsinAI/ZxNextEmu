// The library screen: search, filters, title cards, a title's page, editing,
// import and export, and screenshots taken on a hidden machine.

import {
  titleAll, titleGet, titlePut, titleDelete, importFiles, exportTitle, matches, titleMachines,
  importCollection, label, machineLabels, rgbaToPng, pictureScore, toToml, runParts, attachFiles, romsAdded,
} from './library.js';

const $ = id => document.getElementById(id);
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const KINDS = ['game', 'demo', 'utility', 'application', 'music', 'system', 'education'];
const LICENSES = ['unknown', 'freeware', 'public-domain', 'shareware', 'commercial'];
const JOYSTICKS = ['', 'kempston', 'sinclair', 'cursor', 'none'];

let titles = [];
let opts = null;          // { machines(): [{id, name}], runnable(id), run(title, run, machine) }
const facets = { machine: new Set(), kind: new Set(), genre: new Set(), decade: new Set() };
const urls = new Map();   // blob -> object URL
let openId = null;

const url = blob => { if (!urls.has(blob)) urls.set(blob, URL.createObjectURL(blob)); return urls.get(blob); };
const decade = y => y ? `${Math.floor(y / 10) * 10}s` : null;

export async function initLibraryView(o) {
  opts = o;
  $('lib-q').addEventListener('input', render);
  $('lib-sort').addEventListener('change', render);
  $('lib-add-files').addEventListener('click', () => $('lib-files').click());
  $('lib-add-folder').addEventListener('click', () => $('lib-folder').click());
  for (const b of document.querySelectorAll('[data-collection]')) b.addEventListener('click', addCollection);
  fetch('collection.json', { method: 'HEAD' }).then(r => {
    for (const b of document.querySelectorAll('[data-collection]')) b.hidden = !r.ok;
  }).catch(() => {});
  for (const id of ['lib-files', 'lib-folder'])
    $(id).addEventListener('change', e => { addFiles([...e.target.files]); e.target.value = ''; });
  $('lib-detail').addEventListener('click', e => { if (e.target === $('lib-detail')) closeDetail(); });
  $('lib-edit').addEventListener('click', e => { if (e.target === $('lib-edit')) $('lib-edit').close(); });
  await reload();
}

export async function reload() {
  try { titles = await titleAll(); } catch { titles = []; status('This browser is not keeping files for this page, so the library cannot be stored.'); }
  for (const t of titles) delete t._hay;
  render();
  if (openId) showDetail(openId);
  queueShots();
}

export const count = () => titles.length;

function status(text) {
  const el = $('lib-status');
  el.textContent = text || '';
  el.hidden = !text;
}

// ---- adding -----------------------------------------------------------------

export async function addFiles(files) {
  if (!files.length) return [];
  status(`Adding ${files.length} file${files.length === 1 ? '' : 's'}…`);
  try {
    const ids = await importFiles(files);
    status(ids.length ? `Added ${ids.length} title${ids.length === 1 ? '' : 's'}.` : 'Nothing in those files is software ZX Cradle knows.');
    await reload();
    if (ids.length === 1) showDetail(ids[0]);
    return ids;
  } catch (err) {
    status(`Could not add those files: ${err.message}`);
    return [];
  }
}

async function addCollection() {
  status('Adding the Cradle collection…');
  try {
    const res = await fetch('collection.json');
    if (!res.ok) throw new Error('it is not on this page');
    const ids = await importCollection(await res.json());
    status(ids.length ? `Added ${ids.length} titles from the Cradle collection.` : 'You already have every title in the Cradle collection.');
    await reload();
  } catch (err) {
    status(`Could not add the collection: ${err.message}`);
  }
}

// ---- the grid -----------------------------------------------------------------

function facetValues(t) {
  return {
    machine: titleMachines(t),
    kind: [t.meta.kind || 'game'],
    genre: t.meta.genres || [],
    decade: [decade(t.meta.year)].filter(Boolean),
  };
}

function visible() {
  const q = $('lib-q').value.trim();
  return titles.filter(t => {
    if (!matches(t, q)) return false;
    const v = facetValues(t);
    return Object.entries(facets).every(([k, set]) => !set.size || v[k].some(x => set.has(x)));
  });
}

function render() {
  const sort = $('lib-sort').value;
  const list = visible().sort((a, b) =>
    sort === 'year' ? (a.meta.year || 9999) - (b.meta.year || 9999) || a.meta.name.localeCompare(b.meta.name)
      : sort === 'added' ? b.added - a.added
        : a.meta.name.localeCompare(b.meta.name));
  opts.onCount?.(titles.length);
  $('lib-count').textContent = titles.length === list.length
    ? `${titles.length} title${titles.length === 1 ? '' : 's'}`
    : `${list.length} of ${titles.length} titles`;
  renderFilters();
  const grid = $('lib-grid');
  grid.textContent = '';
  for (const t of list) {
    const li = document.createElement('li');
    const cover = t.screens[0];
    const machines = titleMachines(t);
    const runs = (t.meta.runs || []).some(r => (r.machines || []).some(m => opts.runnable(m)) && !runParts(t, r).missing.length);
    li.innerHTML = `<button class="lib-card" data-id="${esc(t.id)}">
      <span class="lib-cover">${cover ? `<img alt="" src="${url(cover.blob)}">` : `<span class="lib-nocover"><i class="zxc-stripe"></i><span>${t.needsShots && runs ? 'Taking screenshots' : t.loadFailed ? 'Did not load in the browser' : 'No screenshot'}</span></span>`}</span>
      <span class="lib-card-text">
        <b>${esc(t.meta.name)}</b>
        <small>${esc([t.meta.year, t.meta.publisher || (t.meta.authors || [])[0]].filter(Boolean).join(' · ') || kindWord(t.meta.kind))}</small>
        <span class="badges">${machines.slice(0, 4).map(m => `<span class="badge${opts.runnable(m) ? '' : ' off'}">${esc(label(m))}</span>`).join('')}${machines.length > 4 ? `<span class="badge more">${machines.length - 4} more</span>` : ''}</span>
      </span></button>`;
    li.firstElementChild.addEventListener('click', () => showDetail(t.id));
    grid.append(li);
  }
  $('lib-empty').hidden = titles.length > 0;
  $('lib-none').hidden = !titles.length || list.length > 0;
}

const kindWord = k => (k || 'game').replace(/^./, c => c.toUpperCase()).replace('-', ' ');

function renderFilters() {
  const box = $('lib-filters');
  const counts = { machine: new Map(), kind: new Map(), genre: new Map(), decade: new Map() };
  for (const t of titles) for (const [k, vs] of Object.entries(facetValues(t))) for (const v of vs) counts[k].set(v, (counts[k].get(v) || 0) + 1);
  const heads = { machine: 'Runs on', kind: 'Kind', genre: 'Genre', decade: 'Decade' };
  const names = { machine: label, kind: kindWord, genre: g => g, decade: d => d };
  box.textContent = '';
  for (const k of Object.keys(heads)) {
    if (!counts[k].size) continue;
    const sec = document.createElement('section');
    sec.innerHTML = `<h3>${heads[k]}</h3>`;
    const order = Object.keys(machineLabels);
    const entries = [...counts[k]].sort((a, b) => k === 'decade' ? a[0].localeCompare(b[0])
      : k === 'machine' ? ((order.indexOf(a[0]) + 1) || 99) - ((order.indexOf(b[0]) + 1) || 99)
        : b[1] - a[1] || String(a[0]).localeCompare(b[0]));
    for (const [v, n] of entries) {
      const l = document.createElement('label');
      l.className = 'check';
      l.innerHTML = `<input type="checkbox"${facets[k].has(v) ? ' checked' : ''}><span>${esc(names[k](v))}</span><small>${n}</small>`;
      l.querySelector('input').addEventListener('change', e => { e.target.checked ? facets[k].add(v) : facets[k].delete(v); render(); });
      sec.append(l);
    }
    box.append(sec);
  }
  if (Object.values(facets).some(s => s.size)) {
    const b = document.createElement('button');
    b.className = 'zxc-btn small';
    b.textContent = 'Clear filters';
    b.addEventListener('click', () => { for (const s of Object.values(facets)) s.clear(); render(); });
    box.append(b);
  }
}

// ---- a title's page -------------------------------------------------------------

function closeDetail() { openId = null; $('lib-detail').close(); }

export async function showDetail(id) {
  const t = titles.find(x => x.id === id) || await titleGet(id);
  if (!t) return;
  openId = id;
  const m = t.meta;
  const d = $('lib-detail');
  const line = [m.year, m.publisher, (m.authors || []).join(', ')].filter(Boolean).join(' · ');
  const chips = [kindWord(m.kind), ...(m.genres || []), ...(m.tags || []), m.license && m.license !== 'unknown' ? m.license.replace('-', ' ') : null].filter(Boolean);
  const runs = (m.runs || []).map((r, i) => {
    const machines = r.machines || [];
    const ok = machines.filter(x => opts.runnable(x));
    const paths = r.files || (r.file ? [r.file] : []);
    const file = paths.length > 1 ? `${paths.length} files` : (paths[0] || '').replace(/^files\//, '');
    const { missing } = runParts(t, r);
    const need = missing.map(p => p.replace(/^.*\//, ''));
    return `<li class="det-run">
      <div><span class="badges">${machines.map(x => `<span class="badge${opts.runnable(x) ? '' : ' off'}">${esc(label(x))}</span>`).join('')}</span>
      <small>${esc(file)}${r.notes ? ` · ${esc(r.notes)}` : ''}</small></div>
      ${missing.length ? `<div class="det-need"><small>Needs your copy of ${esc(need.join(', '))}</small><button class="zxc-btn small" data-need="${i}">Add the file${need.length > 1 ? 's' : ''}</button></div>`
        : ok.length ? `<div class="det-go"><select data-run="${i}" aria-label="Machine">${ok.map(x => `<option value="${esc(x)}">${esc(opts.machineName(x))}</option>`).join('')}</select>
        <button class="zxc-btn zxc-btn--primary small" data-play="${i}">Run</button></div>`
        : `<small class="det-later">${machines.includes('next') ? 'The Next is not in the browser yet' : 'Not in the browser yet'}</small>`}
    </li>`;
  }).join('');
  const controls = m.controls ? [m.controls.keyboard, m.controls.joystick && `Joystick: ${m.controls.joystick}`].filter(Boolean) : [];
  d.innerHTML = `<article class="det">
    <header class="det-head">
      <div><h2>${esc(m.name)}</h2>${line ? `<p class="det-line">${esc(line)}</p>` : ''}</div>
      <button class="icon-btn" data-close aria-label="Close"><svg viewBox="0 0 16 16"><path d="M3.5 3.5l9 9M12.5 3.5l-9 9"/></svg></button>
    </header>
    <div class="det-body">
      <div class="det-shots">
        ${t.screens.length ? `<figure><img class="det-cover" alt="${esc(t.screens[0].caption || m.name)}" src="${url(t.screens[0].blob)}"><figcaption>${esc(t.screens[0].caption)}</figcaption></figure>`
          : `<div class="det-cover lib-nocover"><i class="zxc-stripe"></i><span>${t.loadFailed ? `It did not load on the ${t.loadFailed.map(opts.machineName).join(' or the ')}, so there are no screenshots` : 'No screenshot yet'}</span></div>`}
        <div class="det-thumbs">${t.screens.map((s, i) => `<button class="det-thumb${i ? '' : ' on'}" data-shot="${i}" aria-label="Screenshot ${i + 1}"><img alt="" src="${url(s.blob)}"></button>`).join('')}</div>
        <div class="det-shot-actions">
          ${t.screens.length > 1 ? '<button class="zxc-btn small" data-cover>Make this the cover</button>' : ''}
          ${t.screens.length ? '<button class="zxc-btn small" data-dropshot>Remove this one</button>' : ''}
          ${titleMachines(t).some(x => opts.runnable(x)) ? '<button class="zxc-btn small" data-reshoot>Take new screenshots</button>' : ''}
        </div>
      </div>
      <div class="det-info">
        ${chips.length ? `<p class="chips">${chips.map(c => `<span class="badge">${esc(c)}</span>`).join('')}</p>` : ''}
        ${m.summary ? `<p class="det-summary">${esc(m.summary)}</p>` : ''}
        <h3>Runs on</h3><ul class="det-runs">${runs || '<li class="det-later">No files to run.</li>'}</ul>
        ${controls.length ? `<h3>Controls</h3><p>${controls.map(esc).join('<br>')}</p>` : ''}
        ${t.about ? `<h3>About</h3><div class="det-about">${esc(t.about)}</div>` : ''}
        <h3>Files</h3><ul class="det-files">${t.files.map(f => `<li><span>${esc(f.path.replace(/^files\//, ''))}</span><small>${size(f.bytes.length)}</small></li>`).join('')}</ul>
        ${m.source ? `<p class="det-source">From ${/^https?:/.test(m.source) ? `<a href="${esc(m.source)}" target="_blank" rel="noopener">${esc(m.source)}</a>` : esc(m.source)}</p>` : ''}
      </div>
    </div>
    <footer class="det-foot">
      <button class="zxc-btn" data-edit>Edit info</button>
      <button class="zxc-btn" data-export>Export as .zip</button>
      <button class="zxc-btn" data-toml>Show title.toml</button>
      <span class="spacer"></span>
      <button class="zxc-btn zxc-btn--danger" data-remove>Remove from library</button>
    </footer>
  </article>`;
  let shot = 0;
  const pick = i => {
    shot = i;
    const s = t.screens[i];
    d.querySelector('.det-cover').src = url(s.blob);
    d.querySelector('figcaption').textContent = s.caption || '';
    d.querySelectorAll('.det-thumb').forEach((b, j) => b.classList.toggle('on', j === i));
  };
  d.querySelector('[data-close]').addEventListener('click', closeDetail);
  d.querySelectorAll('[data-shot]').forEach(b => b.addEventListener('click', () => pick(Number(b.dataset.shot))));
  d.querySelectorAll('[data-need]').forEach(b => b.addEventListener('click', () => {
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.addEventListener('change', async () => {
      const picked = await Promise.all([...input.files].map(async f => ({ name: f.name, bytes: new Uint8Array(await f.arrayBuffer()) })));
      const added = attachFiles(t, picked);
      if (!added.length) { status(`Those are not the files ${t.meta.name} needs.`); return; }
      status(`Added ${added.length} file${added.length === 1 ? '' : 's'} to ${t.meta.name}.`);
      if (!t.screens.length) t.needsShots = true;
      await save(t);
    });
    input.click();
  }));
  d.querySelectorAll('[data-play]').forEach(b => b.addEventListener('click', () => {
    const i = Number(b.dataset.play);
    closeDetail();
    opts.run(t, m.runs[i], d.querySelector(`select[data-run="${i}"]`)?.value || m.runs[i].machines[0]);
  }));
  d.querySelector('[data-cover]')?.addEventListener('click', async () => {
    t.screens.unshift(...t.screens.splice(shot, 1));
    await save(t);
  });
  d.querySelector('[data-dropshot]')?.addEventListener('click', async () => {
    t.screens.splice(shot, 1);
    await save(t);
  });
  d.querySelector('[data-reshoot]')?.addEventListener('click', async () => {
    t.needsShots = true;
    t.screens = t.screens.filter(s => !s.auto);
    await save(t);
  });
  d.querySelector('[data-edit]').addEventListener('click', () => edit(t));
  d.querySelector('[data-export]').addEventListener('click', async () => download(await exportTitle(t), `${t.id}.zip`));
  d.querySelector('[data-toml]').addEventListener('click', () => {
    const meta = { ...m, screens: t.screens.map(s => ({ file: s.path, ...(s.caption ? { caption: s.caption } : {}) })) };
    if (!meta.screens.length) delete meta.screens;
    const pre = document.createElement('pre');
    pre.className = 'zxc-shell det-toml';
    pre.textContent = toToml(meta);
    d.querySelector('.det-info').prepend(pre);
    d.querySelector('[data-toml]').remove();
  });
  d.querySelector('[data-remove]').addEventListener('click', async e => {
    const b = e.currentTarget;
    if (!b.dataset.sure) { b.dataset.sure = '1'; b.textContent = `Remove ${m.name}? Click again`; return; }
    await titleDelete(t.id);
    closeDetail();
    await reload();
  });
  if (!d.open) d.showModal();
}

async function save(t) {
  await titlePut(t);
  await reload();
}

function size(n) { return n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1024))} KB`; }

function download(blob, name) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
}

// ---- editing --------------------------------------------------------------------

function edit(t) {
  const m = t.meta;
  const dlg = $('lib-edit');
  const list = v => (v || []).join(', ');
  const all = opts.machines();
  dlg.innerHTML = `<form method="dialog" class="edit">
    <header class="det-head"><h2>Edit ${esc(m.name)}</h2></header>
    <div class="edit-grid">
      <label>Name<input name="name" required value="${esc(m.name)}"></label>
      <label>Kind<select name="kind">${KINDS.map(k => `<option${k === (m.kind || 'game') ? ' selected' : ''}>${k}</option>`).join('')}</select></label>
      <label>Year<input name="year" inputmode="numeric" pattern="\\d{4}" value="${esc(m.year || '')}"></label>
      <label>Publisher<input name="publisher" value="${esc(m.publisher || '')}"></label>
      <label class="wide">Authors <small>separated by commas</small><input name="authors" value="${esc(list(m.authors))}"></label>
      <label>Genres <small>commas</small><input name="genres" value="${esc(list(m.genres))}"></label>
      <label>Tags <small>commas</small><input name="tags" value="${esc(list(m.tags))}"></label>
      <label class="wide">Summary <small>one line</small><input name="summary" value="${esc(m.summary || '')}"></label>
      <label>Licence<select name="license">${LICENSES.map(k => `<option${k === (m.license || 'unknown') ? ' selected' : ''}>${k}</option>`).join('')}</select></label>
      <label>Source<input name="source" type="url" value="${esc(m.source || '')}" placeholder="https://"></label>
      <label class="wide">Keys<input name="keyboard" value="${esc(m.controls?.keyboard || '')}" placeholder="Q up, A down, O left, P right, Space fire"></label>
      <label>Joystick<select name="joystick">${JOYSTICKS.map(k => `<option value="${k}"${k === (m.controls?.joystick || '') ? ' selected' : ''}>${k || '—'}</option>`).join('')}</select></label>
      ${(m.runs || []).map((r, i) => `<fieldset class="wide"><legend>Runs on: ${esc((r.file || (r.files || [])[0] || '').replace(/^files\//, ''))}</legend>
        <div class="edit-machines">${all.map(x => `<label class="check"><input type="checkbox" name="run${i}" value="${esc(x.id)}"${(r.machines || []).includes(x.id) ? ' checked' : ''}>${esc(label(x.id))}</label>`).join('')}</div>
        <label>Notes<input name="notes${i}" value="${esc(r.notes || '')}"></label></fieldset>`).join('')}
      <label class="wide">About <small>what it is, how to play, credits</small><textarea name="about" rows="6">${esc(t.about || '')}</textarea></label>
    </div>
    <footer class="det-foot"><span class="spacer"></span><button class="zxc-btn" value="cancel" formnovalidate>Cancel</button><button class="zxc-btn zxc-btn--primary" value="save">Save</button></footer>
  </form>`;
  const form = dlg.querySelector('form');
  form.addEventListener('submit', async e => {
    if (e.submitter?.value !== 'save') return;
    const f = new FormData(form);
    const words = k => String(f.get(k) || '').split(',').map(s => s.trim()).filter(Boolean);
    const set = (k, v) => { if (v === '' || v == null || (Array.isArray(v) && !v.length)) delete m[k]; else m[k] = v; };
    m.name = String(f.get('name')).trim() || m.name;
    set('kind', f.get('kind'));
    set('year', f.get('year') ? Number(f.get('year')) : null);
    set('publisher', String(f.get('publisher')).trim());
    set('authors', words('authors'));
    set('genres', words('genres').map(g => g.toLowerCase()));
    set('tags', words('tags'));
    set('summary', String(f.get('summary')).trim());
    set('license', f.get('license') === 'unknown' ? null : f.get('license'));
    set('source', String(f.get('source')).trim());
    const controls = { keyboard: String(f.get('keyboard')).trim(), joystick: f.get('joystick') };
    set('controls', controls.keyboard || controls.joystick ? Object.fromEntries(Object.entries(controls).filter(([, v]) => v)) : null);
    (m.runs || []).forEach((r, i) => {
      const picked = f.getAll(`run${i}`).map(String);
      // Keep the order (the first is the one Run uses); add new ones at the end.
      r.machines = [...(r.machines || []).filter(x => picked.includes(x)), ...picked.filter(x => !(r.machines || []).includes(x))];
      const n = String(f.get(`notes${i}`)).trim();
      if (n) r.notes = n; else delete r.notes;
    });
    t.about = String(f.get('about') || '').trim();
    await save(t);
  });
  dlg.showModal();
}

// ---- screenshots on a hidden machine ------------------------------------------------

let shooter = null, shooting = false;

function hidden() {
  if (shooter) return shooter;
  const w = new Worker('worker.js');
  const waiters = [];
  w.onmessage = e => {
    const i = waiters.findIndex(x => x.types.includes(e.data.type));
    if (i >= 0) waiters.splice(i, 1)[0].resolve(e.data);
    for (const x of waiters.filter(x => x.collect)) x.collect(e.data);
  };
  const call = (msg, types, collect) => new Promise((resolve, reject) => {
    waiters.push({ types: [...types, 'error'], resolve: d => d.type === 'error' ? reject(new Error(d.message)) : resolve(d), collect });
    w.postMessage(msg);
  });
  shooter = { w, call, ready: romsAdded().catch(() => ({})).then(extra => call({ cmd: 'init', wasm: 'zxcore.wasm', extra }, ['ready'])) };
  return shooter;
}

// The user added ROMs: the hidden machine gets them too.
export function romsChanged(roms) {
  shooter?.ready.then(() => shooter.w.postMessage({ cmd: 'addRoms', roms }));
}

export async function queueShots() {
  if (shooting) return;
  shooting = true;
  try {
    for (;;) {
      const t = titles.find(x => x.needsShots && titleMachines(x).some(m => opts.runnable(m)));
      if (!t) break;
      await takeShots(t);
    }
  } finally {
    shooting = false;
  }
}

// Run the title on a hidden machine and keep the pictures worth keeping. If
// it does not load on the first machine it lists (a 128K game offered to the
// 48K), try the next kind of machine, and list the one that worked first.
async function takeShots(t) {
  const run = t.meta.runs.find(r => (r.machines || []).some(m => opts.runnable(m)) && !runParts(t, r).missing.length);
  const fresh0 = () => titleGet(t.id);
  if (!run) {
    const f = await fresh0();
    if (f) { f.needsShots = false; await titlePut(f); }
    return;
  }
  const { parts } = runParts(t, run);
  const tried = [];
  let keep = [], worked = null;
  for (const machine of run.machines.filter(m => opts.runnable(m))) {
    if (tried.length >= 3 || tried.some(m => family(m) === family(machine))) continue;
    tried.push(machine);
    const r = await shootOn(machine, parts, t.id);
    if (r.loaded && r.keep.length) { keep = r.keep; worked = machine; break; }
  }
  const fresh = await titleGet(t.id);
  if (!fresh) return;
  if (worked && worked !== run.machines[0]) {
    const fr = fresh.meta.runs.find(r => r.file === run.file && String(r.files) === String(run.files));
    if (fr) fr.machines = [worked, ...fr.machines.filter(m => m !== worked)];
  }
  for (const [i, sh] of keep.slice(0, 3).entries())
    fresh.screens.push({ path: `screens/auto-${Date.now().toString(36)}-${i + 1}.png`, blob: await rgbaToPng(sh.pixels, sh.w, sh.h), caption: '', auto: true });
  fresh.needsShots = false;
  // Pictures of a tape that never finished loading are no use; say so instead.
  fresh.loadFailed = !worked ? tried : undefined;
  await titlePut(fresh);
  const i = titles.findIndex(x => x.id === t.id);
  if (i >= 0) titles[i] = fresh;
  render();
  if (openId === t.id) showDetail(t.id);
}

// 48K-only machines and 128K machines behave differently enough to retry.
const family = m => m === 'zx48' ? '48' : ['zx128', 'plus2', 'plus2a', 'plus3'].includes(m) ? '128' : m;

async function shootOn(machine, parts, tag) {
  const s = hidden();
  const shots = [];
  let loaded = false;
  try {
    await s.ready;
    await s.call({ cmd: 'create', id: machine, roms: 'roms.json' }, ['created']);
    s.call({ cmd: 'loadSet', parts: parts.map(p => ({ ...p, bytes: p.bytes.slice().buffer })), id: machine, autostart: true }, ['media']).catch(() => {});
    loaded = (await s.call({ cmd: 'capture', tag, after: [100, 500, 1500, 3000] }, ['captured'], d => { if (d.type === 'shot') shots.push(d); })).loaded;
  } catch (err) {
    console.warn(`Screenshots of ${tag} on ${machine}: ${err.message}`);
  }
  const seen = new Set(), keep = [];
  for (const sh of shots) {
    const sc = pictureScore(sh.pixels);
    if (sc.plain > 0.92 || sc.colours < 2 || seen.has(sc.hash)) continue;
    seen.add(sc.hash);
    keep.push(sh);
  }
  // Later pictures are more often the game itself than its loading screen.
  return { loaded, keep: keep.reverse() };
}

// A picture from the running machine, added to a title.
export async function addShot(id, pixels, w, h) {
  const t = await titleGet(id);
  if (!t) return;
  t.screens.push({ path: `screens/${Date.now().toString(36)}.png`, blob: await rgbaToPng(pixels, w, h), caption: '' });
  await save(t);
}
