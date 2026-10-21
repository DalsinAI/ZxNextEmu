// ZX Cradle runtime page: one machine, its screen, sound, keyboard and media.
// The machine itself runs in worker.js.

const $ = id => document.getElementById(id);
const canvas = $('screen');
const ctx = canvas.getContext('2d');
const worker = new Worker('worker.js');

let machineId = new URLSearchParams(location.search).get('m') || 'zx48';
let running = false;
let fbW = 320, fbH = 256;
let pending = null;            // latest frame not yet drawn
let audio = null;              // AudioContext once the user has interacted

// ---- worker messages ----------------------------------------------------

function send(cmd, data = {}, transfer = []) {
  worker.postMessage({ cmd, ...data }, transfer);
}

worker.onmessage = e => {
  const msg = e.data;
  switch (msg.type) {
  case 'ready': {
    const sel = $('machine');
    for (const mc of msg.machines) {
      const o = document.createElement('option');
      o.value = mc.id;
      o.textContent = mc.name;
      if (mc.id === 'next') {
        o.disabled = true;
        o.textContent += ' (needs its SD card: coming next)';
      }
      sel.append(o);
    }
    if (!msg.machines.some(mc => mc.id === machineId && mc.id !== 'next')) machineId = 'zx48';
    sel.value = machineId;
    create(machineId);
    break;
  }
  case 'created':
    fbW = msg.fbW;
    fbH = msg.fbH;
    canvas.width = fbW;
    canvas.height = fbH;
    fit();
    resetSlots();
    $('restore').disabled = true;
    $('saved-note').textContent = 'No snapshot saved yet.';
    send('start');
    break;
  case 'frame':
    pending = msg;
    break;
  case 'state':
    running = msg.running;
    $('run').textContent = running ? 'Pause' : 'Start';
    setChip(running ? 'running' : 'paused');
    break;
  case 'media':
    setSlot(msg.slot, msg.name);
    break;
  case 'saved':
    $('restore').disabled = false;
    $('saved-note').textContent = `Snapshot saved at ${new Date().toLocaleTimeString()} (${Math.round(msg.bytes / 1024)} KB).`;
    break;
  case 'restored':
    $('saved-note').textContent = `Snapshot restored at ${new Date().toLocaleTimeString()}.`;
    break;
  case 'error':
    showError(msg.message);
    break;
  }
};

function create(id) {
  machineId = id;
  setChip('starting');
  hideError();
  history.replaceState(null, '', `?m=${id}`);
  send('create', { id, roms: 'roms.json' });
}

// ---- screen -------------------------------------------------------------

// Largest integer scale of the 320x256 picture that fits. A Next frame is
// 640 wide with half-width pixels, so it is shown at the same size.
function fit() {
  const area = $('screen-area');
  const maxW = area.clientWidth;
  const maxH = window.innerHeight - 56 - 120;
  const s = Math.max(1, Math.min(Math.floor(maxW / 320), Math.floor(maxH / 256)));
  canvas.style.width = `${320 * s}px`;
  canvas.style.height = `${256 * s}px`;
}
window.addEventListener('resize', fit);

function draw() {
  if (pending) {
    const img = new ImageData(new Uint8ClampedArray(pending.pixels), pending.w, pending.h);
    ctx.putImageData(img, 0, 0);
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

function showError(text) {
  const el = $('error');
  el.textContent = text;
  el.hidden = false;
  if (!running) setChip('failed');
}
function hideError() { $('error').hidden = true; }

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
  if ($('joystick').checked && code in joymap) {
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
  if (e.metaKey) return;
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
  if (audio || !$('sound').checked) return;
  try {
    audio = new AudioContext({ sampleRate: 44100 });
    await audio.audioWorklet.addModule('audio.js');
    const node = new AudioWorkletNode(audio, 'zx-sound', { outputChannelCount: [2] });
    node.connect(audio.destination);
    send('audio', { port: node.port }, [node.port]);
  } catch (err) {
    audio = null;
    showError(`Sound could not start: ${err.message}`);
  }
}
$('sound').addEventListener('change', () => {
  if (!audio) { startAudio(); return; }
  if ($('sound').checked) audio.resume(); else audio.suspend();
});

// ---- controls -------------------------------------------------------------

$('machine').addEventListener('change', e => create(e.target.value));
$('run').addEventListener('click', () => { startAudio(); send(running ? 'stop' : 'start'); });
$('reset').addEventListener('click', () => { send('reset', { power: false }); canvas.focus(); });
$('power').addEventListener('click', () => { send('reset', { power: true }); resetSlots(true); canvas.focus(); });
$('save').addEventListener('click', () => send('save'));
$('restore').addEventListener('click', () => { send('restore'); canvas.focus(); });
$('light').addEventListener('change', e => {
  document.documentElement.dataset.theme = e.target.checked ? 'light' : 'dark';
  try { localStorage.setItem('zxc-theme', document.documentElement.dataset.theme); } catch {}
});
try {
  const t = localStorage.getItem('zxc-theme');
  if (t) { document.documentElement.dataset.theme = t; $('light').checked = t === 'light'; }
} catch {}

// ---- media ----------------------------------------------------------------

function setSlot(slot, name) {
  const row = $(`slot-${slot}`);
  if (!row) return;
  row.classList.toggle('zxc-slot--empty', !name);
  row.querySelector('.zxc-slot__file').textContent = name || 'Empty';
  row.querySelector('.zxc-slot__file').title = name || '';
}
function resetSlots(keepTape = false) {
  if (!keepTape) setSlot('tape', null);
  setSlot('snapshot', null);
}

async function loadFile(file) {
  hideError();
  startAudio();
  const bytes = await file.arrayBuffer();
  send('load', { name: file.name, bytes, id: machineId, autostart: $('autostart').checked }, [bytes]);
  if (!running) send('start');
  canvas.focus();
}

for (const b of document.querySelectorAll('[data-insert]'))
  b.addEventListener('click', () => $('file').click());
$('file').addEventListener('change', e => {
  if (e.target.files[0]) loadFile(e.target.files[0]);
  e.target.value = '';
});

let dragDepth = 0;
window.addEventListener('dragenter', e => { e.preventDefault(); dragDepth++; $('drop').hidden = false; });
window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('drop').hidden = true; } });
window.addEventListener('dragover', e => e.preventDefault());
window.addEventListener('drop', e => {
  e.preventDefault();
  dragDepth = 0;
  $('drop').hidden = true;
  const f = e.dataTransfer.files[0];
  if (f) loadFile(f);
});

// ---- go -------------------------------------------------------------------

send('init', { wasm: 'zxcore.wasm' });
canvas.focus();
