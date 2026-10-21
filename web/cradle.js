// ZX Cradle's own screens around the machine: instances, recipes and ROMs.
// An instance is a named machine that keeps its state between visits, the
// way Amiga Cradle keeps one folder per instance. In the browser it lives in
// IndexedDB: {id, name, machine, recipe, notes, created, lastRun, state,
// stateSize, thumb, title}. See docs/cradle/design.md.

const $ = id => document.getElementById(id);
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);

// ---- storage ----------------------------------------------------------------

let dbp = null;
function db() {
  dbp ??= new Promise((resolve, reject) => {
    const r = indexedDB.open('zx-cradle-host', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('instances', { keyPath: 'id' });
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  return dbp;
}
function tx(mode, fn) {
  return db().then(d => new Promise((resolve, reject) => {
    const t = d.transaction('instances', mode);
    const req = fn(t.objectStore('instances'));
    t.oncomplete = () => resolve(req?.result);
    t.onerror = () => reject(t.error);
  }));
}
export const instanceAll = () => tx('readonly', s => s.getAll());
export const instanceGet = id => tx('readonly', s => s.get(id));
export const instancePut = i => tx('readwrite', s => s.put(i));
export const instanceDelete = id => tx('readwrite', s => s.delete(id));

// Merge fields into a stored instance.
export async function instanceUpdate(id, fields) {
  const i = await instanceGet(id);
  if (i) await instancePut({ ...i, ...fields });
  return i && { ...i, ...fields };
}

async function freeId(base) {
  const have = new Set((await instanceAll()).map(i => i.id));
  let id = base, n = 2;
  while (have.has(id)) id = `${base}-${n++}`;
  return id;
}
const slug = s => s.toLowerCase().normalize('NFKD').replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'instance';

export async function instanceMake({ name, machine, recipe = null, notes = '' }) {
  const id = await freeId(slug(name));
  const inst = { id, name, machine, recipe, notes, created: Date.now(), lastRun: null, state: null, stateSize: 0, thumb: null, title: null };
  await instancePut(inst);
  return inst;
}

// ---- recipes ----------------------------------------------------------------

// Built-in recipes: a machine and what the recipe adds to it, in the
// library's needs vocabulary (ram:128, ay, beta, divmmc, mouse...); the
// machine's own offers come from library.js offersOf(). An instance made
// from one shows the titles that suit it. See docs/cradle/recipes.md.
export const RECIPES = [
  { id: 'spectrum-48', title: 'Spectrum 48K', machine: 'zx48', offers: [],
    summary: 'The rubber-key Spectrum: most of the 1980s catalogue runs here.' },
  { id: 'spectrum-128', title: 'Spectrum 128K', machine: 'zx128', offers: ['ram:128', 'ay'],
    summary: '128K with AY music. Plays 48K software too, from the 48 BASIC menu.' },
  { id: 'spectrum-plus2', title: 'Spectrum +2', machine: 'plus2', offers: ['ram:128', 'ay'],
    summary: 'The grey +2 with its built-in tape deck.' },
  { id: 'spectrum-plus2a', title: 'Spectrum +2A', machine: 'plus2a', offers: ['ram:128', 'ay'],
    summary: 'The black +2A, a +3 without the disk drive.' },
  { id: 'spectrum-plus3', title: 'Spectrum +3', machine: 'plus3', offers: [],
    summary: '+3DOS and the 3-inch disk drive.' },
  { id: 'pentagon-trdos', title: 'Pentagon 128 with TR-DOS', machine: 'pentagon', offers: ['ram:128', 'ay', 'beta'],
    summary: 'The Russian clone most demos were written for, with the Beta Disk interface.' },
  { id: 'scorpion-256', title: 'Scorpion ZS-256', machine: 'scorpion', offers: ['ram:128', 'ram:256', 'ay', 'beta'],
    summary: '256K Russian clone with TR-DOS and its own service ROM.' },
  { id: 'atm-turbo', title: 'ATM Turbo 2+', machine: 'atm2', offers: ['ram:128', 'ram:1024', 'ay', 'beta'],
    summary: 'Turbo clone with extra video modes and up to 1 MB.' },
  { id: 'spec256', title: 'Spec256', machine: 'spec256', offers: ['spec256'],
    summary: 'Spectrum games redrawn in 256 colours.' },
  { id: 'sam-coupe', title: 'SAM Coupé', machine: 'sam', offers: ['ram:256'],
    summary: 'MGT\'s 1989 machine: four screen modes, SAA sound and a disk drive.' },
  { id: 'msx1', title: 'MSX', machine: 'msx1', offers: [], summary: 'A standard MSX1 for cartridges and tapes.' },
  { id: 'msx2', title: 'MSX2', machine: 'msx2', offers: ['ram:128'], summary: 'MSX2 with the V9938 video chip.' },
  { id: 'symbos-msx2', title: 'SymbOS desktop on MSX2', machine: 'msx2', offers: ['ram:128', 'mouse'], later: 'Fetching SymbOS is the next step; see recipes/symbos.md.',
    summary: 'Prodatron\'s multitasking desktop OS.' },
  { id: 'symbos-next', title: 'SymbOS desktop on the Next', machine: 'next', offers: ['ram:1024', 'divmmc', 'mouse'], later: 'Needs the Next\'s SD card in the browser first.',
    summary: 'SymbOS on a 2 MB Spectrum Next.' },
  { id: 'nextzxos', title: 'NextZXOS', machine: 'next', offers: ['ram:1024', 'divmmc', 'ay', 'ulaplus'], later: 'Needs the Next\'s SD card in the browser first.',
    summary: 'The Spectrum Next and its operating system.' },
];
export const recipeFor = id => RECIPES.find(r => r.id === id);

// ---- the screens --------------------------------------------------------------

let o = null;          // hooks from app.js
let selected = null;   // the instance shown in the inspector
let query = '';
let sort = 'recent';
const thumbs = new Map();   // instance id -> object URL of its thumbnail

// o: {machines(), current() -> {id, state}, start(inst), romDialog(machineId),
//     show(view), library(inst), onCount(n)}
export function initCradle(hooks) {
  o = hooks;
  for (const b of document.querySelectorAll('.cradle-nav [data-view]'))
    b.addEventListener('click', () => o.show(b.dataset.view));
  $('inst-new').addEventListener('click', () => newDialog());
  $('inst-q').addEventListener('input', e => { query = e.target.value.trim().toLowerCase(); renderInstances(); });
  $('inst-sort').addEventListener('change', e => { sort = e.target.value; renderInstances(); });
  $('recipe-q').addEventListener('input', () => renderRecipes());
  return refresh();
}

// Draw whichever screen is showing again (instances changed, ROMs added).
export async function refresh() {
  const all = await instanceAll().catch(() => []);
  o.onCount(all.length);
  await renderInstances(all);
  renderRecipes();
  renderRoms();
}

const machineOf = id => o.machines().find(m => m.id === id);
const machineName = id => machineOf(id)?.name || id;
const usable = id => id !== 'next' && !!machineOf(id)?.available;

function when(t) {
  if (!t) return 'Never';
  const d = (Date.now() - t) / 1000;
  if (d < 60) return 'Just now';
  if (d < 3600) return `${Math.round(d / 60)} min ago`;
  if (d < 86400) return `${Math.round(d / 3600)} h ago`;
  return new Date(t).toLocaleDateString();
}
const kb = n => n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1024))} KB`;

function chipFor(inst) {
  const cur = o.current();
  if (cur.id === inst.id) return cur.running ? ['running', 'R', 'Running'] : ['paused', 'P', 'Paused'];
  if (!usable(inst.machine)) return ['failed', 'X', 'ROM needed'];
  return inst.state ? ['stopped', 'S', 'Saved'] : ['stopped', 'S', 'Stopped'];
}
const chipHtml = inst => {
  const [k, l, w] = chipFor(inst);
  return `<span class="zxc-chip zxc-chip--${k}"><b>${l}</b>${w}</span>`;
};

function thumbUrl(inst) {
  if (!inst.thumb) return '';
  const old = thumbs.get(inst.id);
  if (old?.blob === inst.thumb) return old.url;
  if (old) URL.revokeObjectURL(old.url);
  const url = URL.createObjectURL(inst.thumb);
  thumbs.set(inst.id, { blob: inst.thumb, url });
  return url;
}

async function renderInstances(all) {
  all ??= await instanceAll().catch(() => []);
  const list = all.filter(i => !query || `${i.name} ${machineName(i.machine)} ${i.notes} ${i.title?.name || ''}`.toLowerCase().includes(query));
  list.sort(sort === 'name' ? (a, b) => a.name.localeCompare(b.name)
    : sort === 'machine' ? (a, b) => machineName(a.machine).localeCompare(machineName(b.machine)) || a.name.localeCompare(b.name)
    : (a, b) => (b.lastRun || b.created) - (a.lastRun || a.created));
  const grid = $('inst-grid');
  grid.textContent = '';
  $('inst-empty').hidden = all.length > 0;
  for (const inst of list) {
    const li = document.createElement('li');
    const url = thumbUrl(inst);
    li.innerHTML = `<button class="zxc-card inst-card${selected === inst.id ? ' on' : ''}" data-id="${esc(inst.id)}">
      ${url ? `<img class="zxc-card__screen" src="${url}" alt="">` : '<span class="zxc-card__screen inst-blank"><i class="zxc-stripe"></i></span>'}
      <span class="zxc-card__row"><span class="zxc-card__name">${esc(inst.name)}</span>${chipHtml(inst)}</span>
      <span class="zxc-card__row"><span class="zxc-card__machine">${esc(machineName(inst.machine))}</span><small>${esc(when(inst.lastRun))}</small></span>
      ${inst.title ? `<span class="zxc-card__row inst-title"><small>${esc(inst.title.name)}</small></span>` : ''}
    </button>`;
    li.firstElementChild.addEventListener('click', () => { selected = inst.id; renderInstances(); });
    li.firstElementChild.addEventListener('dblclick', () => go(inst));
    grid.append(li);
  }
  inspect(all.find(i => i.id === selected) || null);
}

async function go(inst) {
  if (!usable(inst.machine)) return o.romDialog(inst.machine);
  o.start(inst);
}

function inspect(inst) {
  const box = $('inst-inspector');
  if (!inst) {
    box.innerHTML = '<p class="inst-hint">Select an instance to see its machine, what it has loaded and what you can do with it. Double-click one to start it.</p>';
    return;
  }
  const r = recipeFor(inst.recipe);
  const url = thumbUrl(inst);
  const ok = usable(inst.machine);
  box.innerHTML = `<div class="insp">
    ${url ? `<img class="insp-screen" src="${url}" alt="">` : ''}
    <h2>${esc(inst.name)}</h2>
    ${chipHtml(inst)}
    <dl class="insp-list">
      <dt>Machine</dt><dd>${esc(machineName(inst.machine))} <code>${esc(inst.machine)}</code></dd>
      <dt>Recipe</dt><dd>${r ? esc(r.title) : 'None'}</dd>
      <dt>Last loaded</dt><dd>${inst.title ? esc(inst.title.name) : 'Nothing yet'}</dd>
      <dt>State</dt><dd>${inst.state ? `Saved, ${kb(inst.stateSize || 0)}` : 'Starts cold'}</dd>
      <dt>Last run</dt><dd>${esc(when(inst.lastRun))}</dd>
      <dt>Made</dt><dd>${esc(new Date(inst.created).toLocaleString())}</dd>
    </dl>
    ${inst.notes ? `<p class="insp-notes">${esc(inst.notes)}</p>` : ''}
    ${ok ? '' : `<p class="insp-warn">The ${esc(machineName(inst.machine))} needs ROM files before it can start.</p>`}
    <div class="insp-actions">
      <button class="zxc-btn zxc-btn--primary" data-act="start">${ok ? (o.current().id === inst.id ? 'Show it' : 'Start') : 'Add ROMs'}</button>
      <button class="zxc-btn" data-act="library">Software for it</button>
      <button class="zxc-btn" data-act="rename">Rename</button>
      <button class="zxc-btn" data-act="copy">Duplicate</button>
      <button class="zxc-btn" data-act="cold"${inst.state ? '' : ' disabled'}>Forget state</button>
      <button class="zxc-btn zxc-btn--danger" data-act="delete">Delete</button>
    </div>
  </div>`;
  const act = (name, fn) => box.querySelector(`[data-act="${name}"]`).addEventListener('click', fn);
  act('start', () => go(inst));
  act('library', () => o.library(inst));
  act('rename', () => nameDialog(inst));
  act('copy', async () => {
    const copy = await instanceMake({ name: `${inst.name} copy`, machine: inst.machine, recipe: inst.recipe, notes: inst.notes });
    await instanceUpdate(copy.id, { state: inst.state, stateSize: inst.stateSize, thumb: inst.thumb, title: inst.title });
    selected = copy.id;
    refresh();
  });
  act('cold', async () => {
    await instanceUpdate(inst.id, { state: null, stateSize: 0 });
    refresh();
  });
  const del = box.querySelector('[data-act="delete"]');
  del.addEventListener('click', async () => {
    if (o.current().id === inst.id) return o.error('Stop using this instance first: start another one, then delete this.');
    if (!del.dataset.sure) { del.dataset.sure = '1'; del.textContent = 'Delete for good?'; return; }
    await instanceDelete(inst.id);
    selected = null;
    refresh();
  });
}

function nameDialog(inst) {
  const dlg = $('inst-dlg');
  dlg.innerHTML = `<form method="dialog" class="edit">
    <header class="det-head"><h2>Rename ${esc(inst.name)}</h2></header>
    <div class="edit-grid">
      <label class="wide">Name<input name="name" required maxlength="60" value="${esc(inst.name)}"></label>
      <label class="wide">Notes<input name="notes" value="${esc(inst.notes || '')}"></label>
    </div>
    <footer class="det-foot"><span class="spacer"></span><button class="zxc-btn" value="cancel" formnovalidate>Cancel</button><button class="zxc-btn zxc-btn--primary" value="save">Save</button></footer>
  </form>`;
  const form = dlg.querySelector('form');
  form.addEventListener('submit', async e => {
    if (e.submitter?.value !== 'save') return;
    const f = new FormData(form);
    await instanceUpdate(inst.id, { name: f.get('name').trim() || inst.name, notes: f.get('notes').trim() });
    o.renamed?.(inst.id);
    refresh();
  });
  dlg.showModal();
}

// Make an instance, from a recipe or from a bare machine.
export function newDialog(recipeId = null) {
  const dlg = $('inst-dlg');
  const ready = RECIPES.filter(r => !r.later);
  const pick = recipeFor(recipeId) || ready.find(r => usable(r.machine)) || ready[0];
  dlg.innerHTML = `<form method="dialog" class="edit">
    <header class="det-head"><div><h2>New instance</h2>
      <p class="det-line">A machine of its own, with a name. It keeps its state when you leave it, so you come back to where you were.</p></div></header>
    <div class="edit-grid">
      <label class="wide">Recipe<select name="recipe">${ready.map(r => `<option value="${r.id}"${r === pick ? ' selected' : ''}>${esc(r.title)}${usable(r.machine) ? '' : ' (ROM needed)'}</option>`).join('')}</select></label>
      <p class="wide new-sum" id="new-sum"></p>
      <label class="wide">Name<input name="name" required maxlength="60" spellcheck="false"></label>
      <label class="wide">Notes <small>optional</small><input name="notes" placeholder="What it's for"></label>
    </div>
    <footer class="det-foot"><span class="new-own">Each instance keeps its own state and screenshot in this browser.</span><span class="spacer"></span>
      <button class="zxc-btn" value="cancel" formnovalidate>Cancel</button><button class="zxc-btn zxc-btn--primary" value="make">Make it</button></footer>
  </form>`;
  const form = dlg.querySelector('form');
  const sel = form.elements.recipe, nameIn = form.elements.name;
  let auto = true;
  nameIn.addEventListener('input', () => { auto = false; });
  const update = async () => {
    const r = recipeFor(sel.value);
    $('new-sum').innerHTML = `${esc(r.summary)} <span class="new-machine">Machine: ${esc(machineName(r.machine))}</span>${usable(r.machine) ? '' : ' <span class="insp-warn">Needs ROM files: you can add them after making it.</span>'}`;
    if (auto) {
      const names = new Set((await instanceAll()).map(i => i.name));
      let n = r.title, k = 2;
      while (names.has(n)) n = `${r.title} ${k++}`;
      nameIn.value = n;
    }
  };
  sel.addEventListener('change', update);
  update();
  form.addEventListener('submit', async e => {
    if (e.submitter?.value !== 'make') return;
    const r = recipeFor(sel.value);
    const inst = await instanceMake({ name: nameIn.value.trim(), machine: r.machine, recipe: r.id, notes: form.elements.notes.value.trim() });
    selected = inst.id;
    o.show('instances');
    await refresh();
  });
  dlg.showModal();
}

function renderRecipes() {
  const q = $('recipe-q').value.trim().toLowerCase();
  const list = $('recipe-list');
  list.textContent = '';
  for (const r of RECIPES) {
    if (q && !`${r.title} ${r.summary} ${machineName(r.machine)} ${r.offers.join(' ')}`.toLowerCase().includes(q)) continue;
    const ok = usable(r.machine);
    const li = document.createElement('li');
    li.className = 'recipe' + (r.later ? ' later' : '');
    li.innerHTML = `<div class="recipe-text">
        <h3>${esc(r.title)}</h3>
        <p>${esc(r.summary)}</p>
        <p class="recipe-meta"><span>${esc(machineName(r.machine))}</span>${r.offers.map(x => `<code>${esc(x)}</code>`).join('')}</p>
        ${r.later ? `<p class="recipe-why">${esc(r.later)}</p>` : ok ? '' : '<p class="recipe-why">Needs ROM files.</p>'}
      </div>
      <div class="recipe-go">${r.later ? '<button class="zxc-btn" disabled>Coming</button>'
        : `${ok ? '' : '<button class="zxc-btn" data-rom>Add ROMs</button>'}<button class="zxc-btn zxc-btn--primary" data-make>Make an instance</button>`}</div>`;
    li.querySelector('[data-make]')?.addEventListener('click', () => newDialog(r.id));
    li.querySelector('[data-rom]')?.addEventListener('click', () => o.romDialog(r.machine));
    list.append(li);
  }
}

function renderRoms() {
  const body = $('rom-rows');
  body.textContent = '';
  for (const mc of o.machines()) {
    const tr = document.createElement('tr');
    const state = mc.id === 'next' ? 'Waits for the SD card' : mc.available ? 'Ready' : `Missing ${mc.missing.length} of ${mc.roms.length}`;
    tr.innerHTML = `<td>${esc(mc.name)}<br><code>${esc(mc.id)}</code></td>
      <td>${mc.roms.map(r => `<code${mc.missing?.includes(r) ? ' class="missing"' : ''}>${esc(r)}</code>`).join(' ')}</td>
      <td class="rom-st ${mc.id === 'next' ? 'wait' : mc.available ? 'ok' : ''}">${state}</td>
      <td>${mc.id === 'next' ? '' : `<button class="zxc-btn">${mc.available ? 'Replace' : 'Add ROMs'}</button>`}</td>`;
    tr.querySelector('button')?.addEventListener('click', () => o.romDialog(mc.id));
    body.append(tr);
  }
}
