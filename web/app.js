// ZX Cradle page: the software library, and one machine with its screen,
// sound, keyboard and media. The machine runs in worker.js; the library and
// saved states live in IndexedDB.

import { tx, runParts } from './library.js';
import { initLibraryView, addFiles as libraryAdd, addShot, reload as libraryReload } from './libview.js';

const $ = id => document.getElementById(id);
const canvas = $('screen');
const ctx = canvas.getContext('2d');
const frameCanvas = new OffscreenCanvas(320, 256);
const frameCtx = frameCanvas.getContext('2d');
const worker = new Worker('worker.js');

const prefs = loadPrefs();
let machines = [];
let machineId = new URLSearchParams(location.search).get('m') || prefs.machine || 'zx48';
let running = false;
let fbW = 320, fbH = 256;
let pending = null;            // latest frame not yet drawn
let audio = null, gain = null; // sound, once the user has interacted
let lastSnapshot = null;       // library id of the last saved snapshot
let soundState = 'Starts when you click the screen';
let shown = 'machine';      // which view is on screen: 'library' or 'machine'
let resumeOnReturn = false;
let userPaused = false;        // paused with the pause button, so stay paused
let pendingLoad = null;        // a library title to load once the machine exists
let currentTitle = null;       // the library title running now

function loadPrefs() {
  try { return JSON.parse(localStorage.getItem('zxc-prefs')) || {}; } catch { return {}; }
}
function savePrefs() {
  try { localStorage.setItem('zxc-prefs', JSON.stringify(prefs)); } catch {}
}

// ---- worker messages ----------------------------------------------------

function send(cmd, data = {}, transfer = []) {
  worker.postMessage({ cmd, ...data }, transfer);
}

worker.onmessage = async e => {
  const msg = e.data;
  switch (msg.type) {
  case 'ready':
    machines = msg.machines;
    buildMachineMenu();
    if (!machines.some(mc => mc.id === machineId && mc.id !== 'next' && mc.available)) machineId = 'zx48';
    create(machineId);
    initLibraryView({
      machines: () => machines,
      runnable: id => id !== 'next' && machines.some(mc => mc.id === id && mc.available),
      machineName: id => machines.find(mc => mc.id === id)?.name || id,
      run: runTitle,
      onCount: n => { $('lib-readout').textContent = n; },
    }).then(() => showView(prefs.view || 'library'));
    break;
  case 'created':
    fbW = msg.fbW;
    fbH = msg.fbH;
    frameCanvas.width = fbW;
    frameCanvas.height = fbH;
    fit();
    setSlot(null);
    lastSnapshot = null;
    updateSnapshotItem();
    showStatus();
    if (pendingLoad) {
      send('loadSet', pendingLoad, pendingLoad.parts.map(p => p.bytes));
      pendingLoad = null;
    } else {
      currentTitle = null;
      updateShotItem();
    }
    if (shown === 'machine') send('start');
    else resumeOnReturn = true;
    break;
  case 'shot':
    if (currentTitle) {
      await addShot(currentTitle.id, msg.pixels, msg.w, msg.h);
      $('take-shot-note').textContent = `Added to ${currentTitle.meta.name}`;
    }
    break;
  case 'frame':
    pending = msg;
    break;
  case 'state':
    running = msg.running;
    $('run').setAttribute('aria-label', running ? 'Pause' : 'Start');
    $('run').title = running ? 'Pause' : 'Start';
    $('run-icon').innerHTML = running ? '<path d="M5.5 3.5v9M10.5 3.5v9"/>' : '<path d="M4.5 3l8 5-8 5z"/>';
    $('step').disabled = running;
    $('led-run').classList.toggle('on', running);
    setChip(running ? 'running' : 'paused');
    break;
  case 'status':
    $('led-tape').classList.toggle('on', msg.tape);
    $('st-fps').textContent = `${msg.fps.toFixed(1)} frames/s`;
    $('st-frames').textContent = msg.frames.toLocaleString();
    break;
  case 'media':
    setSlot(msg.name);
    break;
  case 'saved': {
    let id = null;
    try { id = await libAdd({ name: msg.name, kind: 'state', machine: machineId, bytes: msg.bytes }); } catch {
      showError('This browser is not keeping files for this page, so the snapshot could not be kept.');
    }
    lastSnapshot = id;
    updateSnapshotItem(id != null ? msg.name : null);
    renderLibrary();
    break;
  }
  case 'error':
    showError(msg.message);
    break;
  }
};

function create(id) {
  machineId = id;
  prefs.machine = id;
  savePrefs();
  setChip('starting');
  hideError();
  const mc = machines.find(m => m.id === id);
  $('machine-name').textContent = mc ? mc.name : id;
  $('media-machine').textContent = mc ? mc.name : id;
  for (const b of document.querySelectorAll('#machine-list .pop-item'))
    b.classList.toggle('current', b.dataset.id === id);
  history.replaceState(null, '', `?m=${id}`);
  send('create', { id, roms: 'roms.json' });
  renderLibrary();
}

function buildMachineMenu() {
  const list = $('machine-list');
  list.textContent = '';
  for (const mc of machines) {
    const b = document.createElement('button');
    b.className = 'pop-item';
    b.dataset.id = mc.id;
    b.innerHTML = '<svg viewBox="0 0 16 16"><rect x="1.5" y="4.5" width="13" height="7"/></svg><span></span><small></small>';
    b.querySelector('span').textContent = mc.name;
    if (mc.id === 'next') {
      b.disabled = true;
      b.querySelector('small').textContent = 'SD card next';
    } else if (!mc.available) {
      b.disabled = true;
      b.querySelector('small').textContent = 'ROM needed';
    } else {
      b.querySelector('small').textContent = mc.id;
    }
    b.addEventListener('click', () => { closeMenus(); if (mc.id !== machineId) create(mc.id); canvas.focus(); });
    list.append(b);
  }
}

// ---- screen -------------------------------------------------------------

// Frames 512 or more wide have half-width pixels (the Next, SAM, MSX, ATM),
// so they show at half their width in "base" pixels. Only a Spectrum-shaped
// 320x256 frame has a border that can be hidden.
const baseW = () => fbW >= 512 ? fbW / 2 : fbW;
function view() {
  const spectrum = baseW() === 320 && fbH === 256;
  if (spectrum && prefs.border === false) return { x: 32, y: 32, w: 256, h: 192 };
  return { x: 0, y: 0, w: baseW(), h: fbH };
}

function fit() {
  const v = view();
  const stage = $('stage');
  const maxW = stage.clientWidth - 32, maxH = stage.clientHeight - 56;
  const best = Math.max(1, Math.min(Math.floor(maxW / v.w), Math.floor(maxH / v.h)));
  const s = prefs.scale ? Math.min(prefs.scale, best) : best;
  canvas.width = v.w * (fbW / baseW());
  canvas.height = v.h;
  canvas.style.width = `${v.w * s}px`;
  canvas.style.height = `${v.h * s}px`;
  $('scale-readout').textContent = prefs.scale ? `${s}×` : `Fit ${s}×`;
  for (const b of document.querySelectorAll('#scale-seg button'))
    b.setAttribute('aria-pressed', String(Number(b.dataset.scale) === (prefs.scale || 0)));
  $('border').setAttribute('aria-pressed', String(prefs.border !== false));
  $('border-state').textContent = prefs.border !== false ? 'Shown' : 'Hidden';
}
new ResizeObserver(fit).observe($('stage'));

function draw() {
  if (pending) {
    frameCtx.putImageData(new ImageData(new Uint8ClampedArray(pending.pixels), pending.w, pending.h), 0, 0);
    const v = view(), k = fbW / baseW();
    ctx.drawImage(frameCanvas, v.x * k, v.y, v.w * k, v.h, 0, 0, canvas.width, canvas.height);
    pending = null;
  }
  requestAnimationFrame(draw);
}
requestAnimationFrame(draw);

// ---- state chip -----------------------------------------------------------

const states = {
  running: ['R', 'Running'],
  paused: ['P', 'Paused'],
  starting: ['S', 'Starting'],
  failed: ['F', 'Failed'],
};
function setChip(state) {
  const [letter, word] = states[state];
  const chip = $('chip');
  chip.className = `zxc-chip zxc-chip--${state}`;
  chip.innerHTML = `<b>${letter}</b>${word}`;
}

let errorTimer = 0;
function showError(text) {
  const el = $('error');
  el.textContent = text;
  el.hidden = false;
  clearTimeout(errorTimer);
  errorTimer = setTimeout(hideError, 8000);
  if (!running) setChip('failed');
}
function hideError() { $('error').hidden = true; }

function showStatus() {
  const mc = machines.find(m => m.id === machineId);
  $('st-machine').textContent = mc ? `${mc.name} (${mc.id})` : machineId;
  $('st-cpu').textContent = ['zx48', 'pentagon', 'next'].includes(machineId) ? 'Z80 at 3.5 MHz' : 'Z80 at 3.5469 MHz';
  $('st-roms').textContent = mc && mc.roms.length ? mc.roms.join(', ') : 'none';
  $('st-sound').textContent = soundState;
}

// ---- menus and drawers ----------------------------------------------------

const menus = [['volume-btn', 'volume-pop'], ['machine-btn', 'machine-pop'], ['power-btn', 'power-pop'],
  ['screen-btn', 'screen-pop'], ['input-btn', 'input-pop'], ['debug-btn', 'debug-pop']];

function closeMenus(except) {
  for (const [b, p] of menus) {
    if (p === except) continue;
    $(p).hidden = true;
    $(b).setAttribute('aria-expanded', 'false');
  }
}
for (const [b, p] of menus) {
  $(b).addEventListener('click', e => {
    e.stopPropagation();
    const open = $(p).hidden;
    closeMenus(p);
    $(p).hidden = !open;
    $(b).setAttribute('aria-expanded', String(open));
  });
  $(p).addEventListener('click', e => e.stopPropagation());
}
document.addEventListener('click', () => closeMenus());
document.addEventListener('keydown', e => {
  if (e.key === 'Escape' && document.activeElement !== canvas) {
    closeMenus();
    $('media-drawer').hidden = $('status-drawer').hidden = true;
    $('media-btn').setAttribute('aria-pressed', 'false');
  }
});

function toggleDrawer(id, open) {
  const d = $(id);
  const show = open ?? d.hidden;
  $('media-drawer').hidden = $('status-drawer').hidden = true;
  d.hidden = !show;
  $('media-btn').setAttribute('aria-pressed', String(id === 'media-drawer' && show));
  if (show && id === 'status-drawer') showStatus();
}
$('media-btn').addEventListener('click', e => { e.stopPropagation(); closeMenus(); toggleDrawer('media-drawer'); });
$('media-close').addEventListener('click', () => toggleDrawer('media-drawer', false));
$('status-btn').addEventListener('click', () => { closeMenus(); toggleDrawer('status-drawer', true); });
$('status-close').addEventListener('click', () => toggleDrawer('status-drawer', false));
for (const d of ['media-drawer', 'status-drawer']) $(d).addEventListener('click', e => e.stopPropagation());

// ---- controls -------------------------------------------------------------

$('run').addEventListener('click', e => { e.stopPropagation(); startAudio(); userPaused = running; send(running ? 'stop' : 'start'); });
$('reset').addEventListener('click', () => { closeMenus(); send('reset', { power: false }); canvas.focus(); });
$('power').addEventListener('click', () => { closeMenus(); send('reset', { power: true }); canvas.focus(); });
$('save').addEventListener('click', () => {
  closeMenus();
  const mc = machines.find(m => m.id === machineId);
  const when = new Date().toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' });
  send('save', { name: `${mc ? mc.name : machineId}, ${when}` });
});
$('quick-restore').addEventListener('click', async () => {
  closeMenus();
  if (lastSnapshot != null) await useItem(await libGet(lastSnapshot));
  canvas.focus();
});
function updateSnapshotItem(name) {
  $('quick-restore').disabled = lastSnapshot == null;
  $('last-snap').textContent = name ? name.replace(/^.*, /, '') : 'None yet';
}
$('step').addEventListener('click', () => send('step'));

for (const b of document.querySelectorAll('#scale-seg button'))
  b.addEventListener('click', () => { prefs.scale = Number(b.dataset.scale); savePrefs(); fit(); });
$('border').addEventListener('click', () => { prefs.border = prefs.border === false; savePrefs(); fit(); });
$('fullscreen').addEventListener('click', async () => {
  closeMenus();
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await document.documentElement.requestFullscreen();
  } catch { showError('This window does not allow full screen.'); }
  canvas.focus();
});
function applyTheme() {
  const theme = prefs.theme || 'dark';
  document.documentElement.dataset.theme = theme;
  $('theme-state').textContent = theme === 'light' ? 'Paper' : 'Case';
}
$('theme').addEventListener('click', () => { prefs.theme = prefs.theme === 'light' ? 'dark' : 'light'; savePrefs(); applyTheme(); });
applyTheme();

function applyJoystick() {
  $('joystick').setAttribute('aria-pressed', String(!!prefs.joystick));
  $('joystick-state').textContent = prefs.joystick ? 'Kempston' : 'Cursor keys';
}
$('joystick').addEventListener('click', () => { prefs.joystick = !prefs.joystick; savePrefs(); applyJoystick(); releaseAll(); });
applyJoystick();
$('type-btn').addEventListener('click', () => {
  closeMenus();
  send('type', { text: machineId === 'zx48' ? 'j""\n' : '\n' });
  canvas.focus();
});

// ---- keyboard -------------------------------------------------------------

// Spectrum matrix: [row, bit]. Row 0 is CAPS SHIFT Z X C V ... row 7 is
// SPACE SYMBOL-SHIFT M N B.
const CAPS = [0, 0], SYM = [7, 1];
const keymap = {
  ShiftLeft: [CAPS], ShiftRight: [CAPS],
  ControlLeft: [SYM], ControlRight: [SYM], AltLeft: [SYM], AltRight: [SYM],
  KeyZ: [[0, 1]], KeyX: [[0, 2]], KeyC: [[0, 3]], KeyV: [[0, 4]],
  KeyA: [[1, 0]], KeyS: [[1, 1]], KeyD: [[1, 2]], KeyF: [[1, 3]], KeyG: [[1, 4]],
  KeyQ: [[2, 0]], KeyW: [[2, 1]], KeyE: [[2, 2]], KeyR: [[2, 3]], KeyT: [[2, 4]],
  Digit1: [[3, 0]], Digit2: [[3, 1]], Digit3: [[3, 2]], Digit4: [[3, 3]], Digit5: [[3, 4]],
  Digit0: [[4, 0]], Digit9: [[4, 1]], Digit8: [[4, 2]], Digit7: [[4, 3]], Digit6: [[4, 4]],
  KeyP: [[5, 0]], KeyO: [[5, 1]], KeyI: [[5, 2]], KeyU: [[5, 3]], KeyY: [[5, 4]],
  Enter: [[6, 0]], NumpadEnter: [[6, 0]], KeyL: [[6, 1]], KeyK: [[6, 2]], KeyJ: [[6, 3]], KeyH: [[6, 4]],
  Space: [[7, 0]], KeyM: [[7, 2]], KeyN: [[7, 3]], KeyB: [[7, 4]],
  Backspace: [CAPS, [4, 0]],
  Escape: [CAPS, [7, 0]],
  ArrowLeft: [CAPS, [3, 4]], ArrowDown: [CAPS, [4, 4]], ArrowUp: [CAPS, [4, 3]], ArrowRight: [CAPS, [4, 2]],
  Comma: [SYM, [7, 3]], Period: [SYM, [7, 2]], Slash: [SYM, [0, 4]], Semicolon: [SYM, [5, 1]],
  Quote: [SYM, [5, 0]], Minus: [SYM, [6, 3]], Equal: [SYM, [6, 1]],
};
// Kempston bits: 0 right, 1 left, 2 down, 3 up, 4 fire.
const joymap = { ArrowRight: 1, ArrowLeft: 2, ArrowDown: 4, ArrowUp: 8, Tab: 16 };

const held = new Map();        // "row,bit" -> how many PC keys hold it
const down = new Map();        // PC key code -> matrix keys it pressed
const downAt = new Map();      // PC key code -> when it went down
// The ROM reads the keyboard once a frame, so a key must stay down for at
// least two frames or a quick tap can fall between two reads.
const MIN_HOLD_MS = 45;
let joy = 0;

function press(code) {
  if (prefs.joystick && code in joymap) {
    joy |= joymap[code];
    send('kempston', { bits: joy });
    return true;
  }
  const keys = keymap[code];
  if (!keys || down.has(code)) return !!keys;
  down.set(code, keys);
  downAt.set(code, performance.now());
  for (const [row, bit] of keys) {
    const k = `${row},${bit}`;
    const n = held.get(k) || 0;
    held.set(k, n + 1);
    if (!n) send('key', { row, bit, down: true });
  }
  return true;
}

function release(code) {
  if (code in joymap && joy & joymap[code]) {
    joy &= ~joymap[code];
    send('kempston', { bits: joy });
  }
  const keys = down.get(code);
  if (!keys) return;
  const wait = MIN_HOLD_MS - (performance.now() - downAt.get(code));
  if (wait > 0) {
    setTimeout(() => release(code), wait);
    return;
  }
  down.delete(code);
  for (const [row, bit] of keys) {
    const k = `${row},${bit}`;
    const n = (held.get(k) || 1) - 1;
    held.set(k, n);
    if (!n) send('key', { row, bit, down: false });
  }
}

function releaseAll() {
  down.clear();
  downAt.clear();
  held.clear();
  joy = 0;
  send('releaseAll');
  send('kempston', { bits: 0 });
}

canvas.addEventListener('keydown', e => {
  if (e.metaKey || e.key === 'F11') return;
  startAudio();
  if (press(e.code)) e.preventDefault();
});
canvas.addEventListener('keyup', e => {
  release(e.code);
  if (keymap[e.code] || joymap[e.code]) e.preventDefault();
});
canvas.addEventListener('blur', releaseAll);
canvas.addEventListener('pointerdown', () => { canvas.focus(); startAudio(); });

// ---- sound ----------------------------------------------------------------

async function startAudio() {
  if (audio) { if (audio.state === 'suspended') audio.resume(); return; }
  try {
    audio = new AudioContext({ sampleRate: 44100 });
    await audio.audioWorklet.addModule('audio.js');
    const node = new AudioWorkletNode(audio, 'zx-sound', { outputChannelCount: [2] });
    gain = audio.createGain();
    node.connect(gain).connect(audio.destination);
    applyVolume();
    send('audio', { port: node.port }, [node.port]);
    soundState = 'On, 44.1 kHz stereo';
  } catch (err) {
    audio = null;
    soundState = 'Off: this browser would not start it';
    showError(`Sound could not start: ${err.message}`);
  }
  showStatus();
}
function applyVolume() {
  const v = prefs.volume ?? 80;
  $('volume').value = v;
  $('volume-value').textContent = `${v}%`;
  $('vol-wave').style.display = v ? '' : 'none';
  if (gain) gain.gain.value = (v / 100) ** 2;
}
$('volume').addEventListener('input', e => { prefs.volume = Number(e.target.value); savePrefs(); startAudio(); applyVolume(); });
applyVolume();

// ---- kept media (IndexedDB) -----------------------------------------------

const libAdd = item => tx('media', 'readwrite', st => st.add({ ...item, size: item.bytes.byteLength, added: Date.now() }));
const libGet = id => tx('media', 'readonly', st => st.get(id));
const libDelete = id => tx('media', 'readwrite', st => st.delete(id));
const libAll = () => tx('media', 'readonly', st => st.getAll());

const icons = {
  tape: '<rect x="1.5" y="3.5" width="13" height="9"/><circle cx="5" cy="8" r="1.5"/><circle cx="11" cy="8" r="1.5"/>',
  snapshot: '<rect x="2.5" y="1.5" width="11" height="13"/><path d="M5 5.5h6M5 8h6"/>',
  state: '<rect x="1.5" y="4.5" width="13" height="9"/><path d="M5.5 4.5l1-2h3l1 2"/><circle cx="8" cy="9" r="2.5"/>',
};
const kindWords = { tape: 'Tape', snapshot: 'Snapshot file', state: 'Saved state' };

function sizeText(n) {
  return n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1024))} KB`;
}

async function renderLibrary() {
  let items = [];
  try { items = await libAll(); } catch {
    $('library-note').textContent = 'This browser is not keeping files for this page, so media lasts until you close it.';
  }
  const list = $('library');
  list.textContent = '';
  items.sort((a, b) => b.added - a.added);
  const mc = machines.find(m => m.id === machineId);
  for (const it of items) {
    if (it.kind !== 'state' || it.machine !== machineId) continue;
    const li = document.createElement('li');
    li.innerHTML = `<svg viewBox="0 0 16 16">${icons[it.kind]}</svg><div class="lib-name"><b></b><small></small></div><div class="lib-actions"><button class="zxc-btn small use"></button><button class="zxc-btn small remove">Remove</button></div>`;
    li.querySelector('b').textContent = it.name;
    li.querySelector('b').title = it.name;
    li.querySelector('small').textContent = `${kindWords[it.kind]} · ${sizeText(it.size)}`;
    const use = li.querySelector('.use');
    use.textContent = it.kind === 'tape' ? 'Insert' : it.kind === 'state' ? 'Restore' : 'Load';
    use.addEventListener('click', async () => { await useItem(await libGet(it.id)); canvas.focus(); });
    li.querySelector('.remove').addEventListener('click', async () => {
      await libDelete(it.id);
      if (lastSnapshot === it.id) { lastSnapshot = null; updateSnapshotItem(); }
      renderLibrary();
    });
    list.append(li);
  }
  if (!list.children.length) {
    const li = document.createElement('li');
    li.className = 'empty';
    li.textContent = `No saved states yet. Save the ${mc ? mc.name : 'machine'} from the Power menu.`;
    list.append(li);
  }
}

async function useItem(it) {
  if (!it) return;
  hideError();
  startAudio();
  const bytes = it.bytes.slice(0);
  if (it.kind === 'state') {
    send('restore', { name: it.name, bytes }, [bytes]);
  } else {
    send('load', { name: it.name, bytes, id: machineId, autostart: $('autostart').checked }, [bytes]);
  }
  if (!running) send('start');
}

// Dropped files go into the library.
async function addFiles(files) {
  hideError();
  showView('library');
  await libraryAdd(files);
}

function setSlot(name) {
  const row = $('slot-tape');
  row.classList.toggle('zxc-slot--empty', !name);
  row.querySelector('.zxc-slot__file').textContent = name || 'Empty';
  row.querySelector('.zxc-slot__file').title = name || '';
  $('eject-tape').disabled = !name;
}
$('eject-tape').addEventListener('click', () => send('eject', { slot: 'tape' }));
$('add-media').addEventListener('click', () => { toggleDrawer('media-drawer', false); showView('library'); });

// ---- library and machine views ----------------------------------------------

function showView(v) {
  if (v === shown) return;
  shown = v;
  prefs.view = v;
  savePrefs();
  closeMenus();
  const lib = v === 'library';
  $('libview').hidden = !lib;
  $('stage').hidden = lib;
  $('lib-btn').setAttribute('aria-pressed', String(lib));
  if (lib) {
    toggleDrawer('media-drawer', false);
    // The machine may still be starting, so stop it whatever it says now.
    resumeOnReturn = !userPaused;
    send('stop');
    $('lib-q').focus();
  } else {
    if (resumeOnReturn) send('start');
    resumeOnReturn = false;
    fit();
    canvas.focus();
  }
}
$('lib-btn').addEventListener('click', e => { e.stopPropagation(); showView(shown === 'library' ? 'machine' : 'library'); });

// Run a library title: switch machine if it needs another, then load it.
function runTitle(t, run, id) {
  const { parts, missing } = runParts(t, run);
  if (missing.length || !parts.length) return;
  currentTitle = t;
  updateShotItem();
  const load = { parts: parts.map(p => ({ ...p, bytes: p.bytes.slice().buffer })), id, autostart: true };
  resumeOnReturn = true;
  userPaused = false;
  showView('machine');
  if (id !== machineId) {
    pendingLoad = load;
    create(id);
  } else {
    send('loadSet', load, load.parts.map(p => p.bytes));
    send('start');
  }
  $('machine-name').textContent = `${t.meta.name} · ${machines.find(mc => mc.id === id)?.name || id}`;
}

function updateShotItem() {
  $('take-shot').disabled = !currentTitle;
  $('take-shot-note').textContent = currentTitle ? `Adds to ${currentTitle.meta.name}` : 'Run a library title first';
}
$('take-shot').addEventListener('click', () => send('shot', { tag: 'user' }));

let dragDepth = 0;
window.addEventListener('dragenter', e => { e.preventDefault(); dragDepth++; $('drop').hidden = false; });
window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('drop').hidden = true; } });
window.addEventListener('dragover', e => e.preventDefault());
window.addEventListener('drop', e => {
  e.preventDefault();
  dragDepth = 0;
  $('drop').hidden = true;
  if (e.dataTransfer.files.length) addFiles([...e.dataTransfer.files]);
});

// ---- go -------------------------------------------------------------------

send('init', { wasm: 'zxcore.wasm' });
canvas.focus();
