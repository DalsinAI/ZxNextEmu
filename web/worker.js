// ZX Cradle runtime: one machine, running zxcore.wasm in this Worker.
//
// The page sends commands ({cmd: ...}); the worker sends back frames
// (RGBA pixels, transferred), sound (to the AudioWorklet's port when the
// page hands one over) and status. Frames are paced by a timer at the
// machine's own rate, nudged by how much sound the worklet has queued so
// sound never runs dry or piles up.

let x = null;          // wasm exports
let m = 0;             // the machine (zxm *)
let info = 0;          // its zxm_info *
let fbW = 0, fbH = 0;
let running = false;
let timer = 0;
let nextAt = 0;
let frameMs = 20;
let audioPort = null;
let audioQueued = 0;   // sound frames the worklet holds, as last reported
let scratch = 0;       // 64 bytes of wasm memory for small structs
let framesRun = 0;
let statusAt = 0, statusFrames = 0;
let tapeSlot = 0;      // "tape" as a C string, for the LED
let lut = null;

const u8 = () => new Uint8Array(x.memory.buffer);
const dv = () => new DataView(x.memory.buffer);

function cstr(s) {
  const b = new TextEncoder().encode(s + '\0');
  const p = x.malloc(b.length);
  u8().set(b, p);
  return p;
}
function readCstr(p) {
  const mem = u8();
  let e = p;
  while (mem[e]) e++;
  return new TextDecoder().decode(mem.subarray(p, e));
}
function copyIn(bytes) {
  const p = x.malloc(bytes.length || 1);
  u8().set(bytes, p);
  return p;
}

// RGB333 to RGBA (little-endian 0xAABBGGRR).
function makeLut() {
  lut = new Uint32Array(512);
  for (let c = 0; c < 512; c++) {
    const r = ((c >> 6) & 7) * 255 / 7 | 0, g = ((c >> 3) & 7) * 255 / 7 | 0, b = (c & 7) * 255 / 7 | 0;
    lut[c] = 0xff000000 | b << 16 | g << 8 | r;
  }
}

async function init(wasmUrl, romsUrl = 'roms.json', extra = {}) {
  const { instance } = await WebAssembly.instantiate(await (await fetch(wasmUrl)).arrayBuffer(), {});
  x = instance.exports;
  x._initialize?.();
  scratch = x.malloc(64);
  makeLut();
  const machines = [];
  for (let i = 0; ; i++) {
    const p = x.zxm_machine(i);
    if (!p) break;
    const roms = [];
    for (let r = dv().getUint32(p + 28, true), k = 0; ; k++) {
      const np = dv().getUint32(r + 4 * k, true);
      if (!np) break;
      roms.push(readCstr(np));
    }
    machines.push({ id: readCstr(dv().getUint32(p, true)), name: readCstr(dv().getUint32(p + 4, true)), roms,
      optional: dv().getUint32(p + 44, true) });
  }
  allMachines = machines;
  // A machine is available when every ROM it must have is in roms.json or
  // was added by the user.
  try { await loadRoms(romsUrl); } catch { romCache = {}; }
  Object.assign(romCache, extra);
  postMessage({ type: 'ready', machines: availability() });
}

let allMachines = [];
function availability() {
  for (const mc of allMachines) {
    mc.missing = mc.roms.filter((r, k) => !romCache[r] && !(mc.optional >> k & 1));
    mc.available = !mc.missing.length;
  }
  return allMachines;
}

// ROM images come in one JSON file, {"48.rom": "<base64>", ...}.
let romCache = null;
async function loadRoms(romsUrl) {
  if (romCache) return;
  const res = await fetch(romsUrl);
  if (!res.ok) throw new Error(`The ROM file ${romsUrl} is missing.`);
  romCache = await res.json();
}
async function romFile(romsUrl, name) {
  await loadRoms(romsUrl);
  const b64 = romCache[name];
  if (!b64) throw new Error(`The ROM file ${name} is missing.`);
  return Uint8Array.from(atob(b64), c => c.charCodeAt(0));
}

async function create(id, romsUrl) {
  stop();
  if (m) { x.zxm_destroy(m); x.free(m); m = 0; }
  info = x.zxm_lookup(cstr(id));
  if (!info) throw new Error(`There is no machine called ${id}.`);
  const d = dv();
  const cpuHz = d.getUint32(info + 8, true), frameT = d.getUint32(info + 12, true);
  fbW = d.getUint16(info + 16, true);
  fbH = d.getUint16(info + 18, true);
  frameMs = 1000 * frameT / cpuHz;
  // zxm_rom_set: four pointers, then four lengths.
  const set = x.malloc(32);
  u8().fill(0, set, set + 32);
  const optional = dv().getUint32(info + 44, true);   // zxm_info.rom_optional
  for (let r = dv().getUint32(info + 28, true), k = 0; ; k++) {
    const np = dv().getUint32(r + 4 * k, true);
    if (!np) break;
    const name = readCstr(np);
    let bytes;
    try {
      bytes = await romFile(romsUrl, name);
    } catch (e) {
      if (optional >> k & 1) continue;   // the Pentagon without TR-DOS
      throw e;
    }
    const ptr = copyIn(bytes);   // may grow memory: take the view after
    dv().setUint32(set + 4 * k, ptr, true);
    dv().setUint32(set + 16 + 4 * k, bytes.length, true);
  }
  m = x.malloc(x.zxm_size(info));
  if (x.zxm_create(m, info, set)) { m = 0; throw new Error(`Could not start the ${id}.`); }
  postMessage({ type: 'created', id, fbW, fbH, frameMs });
}

// The current picture as RGBA.
function pixels() {
  const fb = x.zxm_framebuffer(m, scratch);
  const pitch = dv().getInt32(scratch, true);
  const px = new Uint16Array(x.memory.buffer, fb, pitch * fbH);
  const out = new Uint32Array(fbW * fbH);
  for (let y = 0; y < fbH; y++) {
    const s = y * pitch, o = y * fbW;
    for (let i = 0; i < fbW; i++) out[o + i] = lut[px[s + i] & 511];
  }
  return out;
}

function frame() {
  x.zxm_run_frame(m);
  framesRun++;
  const out = pixels();
  postMessage({ type: 'frame', w: fbW, h: fbH, pixels: out.buffer }, [out.buffer]);
  // sound
  if (audioPort) {
    const au = x.zxm_audio(m, scratch);
    const n = dv().getInt32(scratch, true);
    const src = new Int16Array(x.memory.buffer, au, n * 2);
    const pcm = new Float32Array(n * 2);
    for (let i = 0; i < n * 2; i++) pcm[i] = src[i] / 32768;
    audioPort.postMessage(pcm, [pcm.buffer]);
    audioQueued++;
  }
}

function tick() {
  if (!running) return;
  const now = performance.now();
  // Catch up at most a few frames after a stall (a background tab).
  if (now - nextAt > frameMs * 5) nextAt = now;
  let n = 0;
  while (nextAt <= now && n < 5) {
    // Too much sound queued: skip a frame. Too little: run one more.
    if (!(audioPort && audioQueued > 6)) frame();
    if (audioPort && audioQueued < 2) frame();
    nextAt += frameMs;
    n++;
  }
  if (now - statusAt >= 500) {
    const fps = statusAt ? (framesRun - statusFrames) * 1000 / (now - statusAt) : 0;
    statusAt = now;
    statusFrames = framesRun;
    if (!tapeSlot) tapeSlot = cstr('tape');
    postMessage({ type: 'status', fps, frames: framesRun, tape: x.zxm_media_active(m, tapeSlot) === 1 });
  }
  timer = setTimeout(tick, Math.max(0, nextAt - performance.now()));
}

function start() {
  if (running || !m) return;
  running = true;
  nextAt = performance.now();
  tick();
  postMessage({ type: 'state', running: true });
}

function stop() {
  running = false;
  clearTimeout(timer);
  postMessage({ type: 'state', running: false });
}

// zxm_input_event: t u32, kind u8, a u8, b u8, down u8, dx dy dz i16.
function input(kind, a, b, down) {
  const d = dv();
  d.setUint32(scratch, 0, true);
  d.setUint8(scratch + 4, kind);
  d.setUint8(scratch + 5, a);
  d.setUint8(scratch + 6, b);
  d.setUint8(scratch + 7, down ? 1 : 0);
  d.setInt16(scratch + 8, 0, true);
  d.setInt16(scratch + 10, 0, true);
  d.setInt16(scratch + 12, 0, true);
  x.zxm_input(m, scratch);
}

// zxm_media: kind u32, name ptr, data ptr, len u32, blocks ptr, serial ptr.
function insert(slot, name, bytes) {
  const media = x.malloc(24);
  const nameP = cstr(name), data = copyIn(bytes);
  const d = dv();
  d.setUint32(media, 1, true);
  d.setUint32(media + 4, nameP, true);
  d.setUint32(media + 8, data, true);
  d.setUint32(media + 12, bytes.length, true);
  d.setUint32(media + 16, 0, true);
  d.setUint32(media + 20, 0, true);
  const r = x.zxm_media_insert(m, cstr(slot), media);
  x.free(data);
  x.free(nameP);
  x.free(media);
  return r === 0;
}

// Starting a Beta disk, per machine, checked with a scratch copy of tests/beta.c that
// types through zxm_type: | is cursor down, ~ extended mode, \x01 a pause of one key.
const BETA_BOOT = {
  scorpion: [450, '\n'],                    // menu: 128 TR-DOS
  atm2: [160, '|\n'],                       // menu: TR-DOS 48
  pentagon: [110, '|||\n' + '\x01'.repeat(10) + 't~l15616\n' + '\x01'.repeat(26) + 'r\n'],   // 48 BASIC, RANDOMIZE USR 15616, RUN
};

// Tape autostart on the clones whose start-up menu does not open on a tape loader: 128 BASIC, then
// LOAD "" (checked with each machine's ROMs). Pentagon's menu is the 128's, so Enter works there.
const TAPE_BOOT = {
  scorpion: [450, '|\n' + '\x01'.repeat(10) + 'load ""\n'],   // menu: 128 BASIC
  atm2: [160, '||\n' + '\x01'.repeat(10) + 'load ""\n'],      // menu: 128 BASIC
};

const handlers = {
  async init(msg) { await init(msg.wasm, msg.roms, msg.extra); },
  // ROMs the user added: {"name.rom": "<base64>"}.
  addRoms(msg) { Object.assign(romCache ??= {}, msg.roms); postMessage({ type: 'machines', machines: availability() }); },
  async create(msg) { await create(msg.id, msg.roms); },
  start, stop,
  step() { if (!running && m) frame(); },
  reset(msg) { x.zxm_reset(m, msg.power ? 0 : 1); },
  key(msg) { input(1, msg.row, msg.bit, msg.down); },
  kempston(msg) { input(2, msg.bits, 0, 0); },
  releaseAll() { input(4, 0, 0, 0); },
  type(msg) { const p = cstr(msg.text); x.zxm_type(m, p, msg.delay || 0); x.free(p); },
  load(msg) {
    const bytes = new Uint8Array(msg.bytes);
    const lower = msg.name.toLowerCase();
    if (lower.endsWith('.tap') || lower.endsWith('.tzx')) {
      if (!insert('tape', msg.name, bytes)) throw new Error(`${msg.name} is not a tape this machine can read.`);
      if (msg.autostart) {
        x.zxm_reset(m, 1);
        const is48 = msg.id === 'zx48', sam = msg.id === 'sam', clone = TAPE_BOOT[msg.id];
        // 48K: LOAD "" (J then quotes); SAM: past the start-up screen, then LOAD ""; 128K: the menu's Tape Loader
        const p = cstr(is48 ? 'j""\n' : sam ? '\n\n' + '\x01'.repeat(5) + 'load ""\n' : clone ? clone[1] : '\n');
        x.zxm_type(m, p, is48 ? 120 : sam ? 150 : clone ? clone[0] : 90);
        x.free(p);
      }
      postMessage({ type: 'media', slot: 'tape', name: msg.name });
    } else if (lower.endsWith('.sna') || lower.endsWith('.z80')) {
      if (!insert('snapshot', msg.name, bytes)) throw new Error(`${msg.name} could not be loaded into this machine.`);
      postMessage({ type: 'media', slot: 'snapshot', name: msg.name });
    } else {
      throw new Error(`${msg.name}: ZX Cradle loads .tap, .tzx, .sna and .z80 files.`);
    }
  },
  // Several files for one run: each into its slot, a tape or snapshot last
  // through load() so it starts as usual.
  // A disk or cartridge on its own boots the machine from it; of several
  // disks for one drive, the first goes in.
  loadSet(msg) {
    const filled = new Set();
    let boots = false, starts = false;
    for (const p of msg.parts) {
      if (p.slot === 'tape' || p.slot === 'snapshot') {
        handlers.load({ name: p.name, bytes: p.bytes, id: msg.id, autostart: msg.autostart });
        starts = true;
      } else if (p.slot === 'gfx' || !filled.has(p.slot)) {   // Spec256 takes every companion file
        if (p.slot === 'gfx' && !filled.has('gfx')) {   // drop an earlier title's companions first
          const s = cstr('gfx');
          x.zxm_media_eject(m, s);
          x.free(s);
        }
        if (!insert(p.slot, p.name, new Uint8Array(p.bytes))) throw new Error(`${p.name} could not go into the ${p.slot} slot.`);
        filled.add(p.slot);
        if (/^(disk|cart|beta)/.test(p.slot)) boots = true;
        postMessage({ type: 'media', slot: p.slot, name: p.name });
      }
    }
    if (boots && !starts && msg.autostart) {
      x.zxm_reset(m, 1);
      if (msg.id === 'plus3' || msg.id === 'plus2a') {   // the menu's Loader
        const p = cstr('\n');
        x.zxm_type(m, p, 90);
        x.free(p);
      } else if (msg.id === 'sam' && filled.has('disk.1')) {   // SAM BASIC starts a disk with BOOT
        const p = cstr('\n\n' + '\x01'.repeat(5) + 'boot\n');   // past the start-up screen, as tests/sam.c does
        x.zxm_type(m, p, 150);
        x.free(p);
      } else if (filled.has('beta.a') && BETA_BOOT[msg.id]) {   // into TR-DOS, which runs "boot"
        const [delay, keys] = BETA_BOOT[msg.id];
        const p = cstr(keys);
        x.zxm_type(m, p, delay);
        x.free(p);
      }
    }
    postMessage({ type: 'loadedSet' });
  },
  eject(msg) { x.zxm_media_eject(m, cstr(msg.slot)); postMessage({ type: 'media', slot: msg.slot, name: null }); },
  save(msg) {
    const len = x.zxm_state_size(m);
    const p = x.malloc(len);
    if (x.zxm_state_save(m, p, len)) { x.free(p); throw new Error('Could not save the machine.'); }
    const bytes = u8().slice(p, p + len);
    x.free(p);
    postMessage({ type: 'saved', name: msg.name, bytes: bytes.buffer }, [bytes.buffer]);
  },
  restore(msg) {
    const bytes = new Uint8Array(msg.bytes);
    const p = copyIn(bytes);
    const r = x.zxm_state_load(m, p, bytes.length);
    x.free(p);
    if (r) throw new Error(`${msg.name} was saved from a different machine or version, so it cannot be restored here.`);
    postMessage({ type: 'restored', name: msg.name });
  },
  // The picture as it is now, for a screenshot.
  shot(msg) {
    const out = pixels();
    postMessage({ type: 'shot', tag: msg.tag, w: fbW, h: fbH, pixels: out.buffer }, [out.buffer]);
  },
  // Run unpaced, without sound or frames, until whatever was loaded has
  // finished loading (every tape block read; a snapshot or an empty slot
  // counts as loaded once `settle` frames have run) or has got stuck part
  // way through the tape for `stall` frames, then take pictures
  // `after` frames later. The library's hidden machine uses this for
  // screenshots.
  capture(msg) {
    stop();
    const slot = cstr('tape');
    const limit = msg.limit || 30000, stall = msg.stall || 1500;
    let f = 0, last = -2, since = 0, loaded = false, typed = 0;
    for (; f < limit; f++) {
      x.zxm_run_frame(m);
      const p = x.zxm_media_progress(m, slot);
      if (p !== last) { last = p; since = f; }
      // Keys still being typed (a disk's boot command, after its delay): nothing has started
      // loading yet, so the settle time counts from when the typing ends.
      if (x.zxm_typing(m)) { typed = f; since = f; continue; }
      if (f - typed < (msg.settle || 150)) continue;
      if (p < 0 || p >= 100) { loaded = true; break; }
      if (f - since > stall) break;   // the tape has stopped part way: it did not load
    }
    x.free(slot);
    let done = 0;
    for (const n of msg.after) {
      for (; done < n; done++) x.zxm_run_frame(m);
      const out = pixels();
      postMessage({ type: 'shot', tag: msg.tag, loaded, w: fbW, h: fbH, pixels: out.buffer }, [out.buffer]);
    }
    postMessage({ type: 'captured', tag: msg.tag, loaded });
  },
  audio(msg) { audioPort = msg.port; audioQueued = 0; audioPort.onmessage = e => { audioQueued = e.data; }; },
};

onmessage = async e => {
  try {
    await handlers[e.data.cmd](e.data);
  } catch (err) {
    postMessage({ type: 'error', message: err.message || String(err) });
  }
};
