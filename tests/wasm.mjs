// Runs the zxm check of tests/zxm.c inside zxcore.wasm under Node and
// prints the same line, so the two builds can be compared byte for byte.
//
// usage: node tests/wasm.mjs zxcore.wasm romdir machine frames [text]
// ZXM_SPEC256=folder loads a Spec256 game, as tests/zxm.c does.
// ZXM_IFACE / ZXM_IFACE_ROM plug in an interface, as for tests/zxm.c.
// ZXM_SNAP=file loads a snapshot, as tests/zxm.c does.
// ZXM_DISK=file.trd|.scl puts a disk in a Beta 128's drive A; ZXM_DELAY=frames
// waits before typing.
import { readFileSync, readdirSync, existsSync } from 'node:fs';

const [wasmPath, romDir, id, framesArg, text] = process.argv.slice(2);
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {});
const x = instance.exports;
x._initialize?.();
const u8 = () => new Uint8Array(x.memory.buffer);
const dv = () => new DataView(x.memory.buffer);

function cstr(s) {
  const b = new TextEncoder().encode(s + '\0');
  const p = x.malloc(b.length);
  u8().set(b, p);
  return p;
}
function readCstr(p) {
  const m = u8();
  let e = p;
  while (m[e]) e++;
  return new TextDecoder().decode(m.subarray(p, e));
}

const info = x.zxm_lookup(cstr(id));
if (!info) { console.error(`unknown machine ${id}`); process.exit(2); }
const fbW = dv().getUint16(info + 16, true), fbH = dv().getUint16(info + 18, true);
const romNames = dv().getUint32(info + 28, true);
const romsOptional = dv().getUint32(info + 40, true);

// zxm_rom_set: four pointers, then four lengths.
const roms = x.malloc(32);
u8().fill(0, roms, roms + 32);
const romOptional = dv().getUint32(info + 44, true);
for (let i = 0; ; i++) {
  const np = dv().getUint32(romNames + 4 * i, true);
  if (!np) break;
  const path = `${romDir}/${readCstr(np)}`;
  if (!existsSync(path)) {
    if (romOptional >> i & 1) continue;
    console.log(`${id}: skipped, no ${path}`);
    process.exit(77);
  }
  const data = readFileSync(path);
  const p = x.malloc(data.length);
  u8().set(data, p);
  dv().setUint32(roms + 4 * i, p, true);
  dv().setUint32(roms + 16 + 4 * i, data.length, true);
}

const m = x.malloc(x.zxm_size(info));
if (x.zxm_create(m, info, roms)) { console.error('cannot create'); process.exit(1); }
// zxm_media: kind, name, data, len (then two device pointers).
function insert(slot, dir, name) {
  const data = readFileSync(`${dir}/${name}`);
  const p = x.malloc(data.length || 1);
  u8().set(data, p);
  const md = x.malloc(24);
  u8().fill(0, md, md + 24);
  dv().setUint32(md, 1, true);               // ZXM_MEDIA_BYTES
  const np = cstr(name);
  dv().setUint32(md + 4, np, true);
  dv().setUint32(md + 8, p, true);
  dv().setUint32(md + 12, data.length, true);
  return x.zxm_media_insert(m, cstr(slot), md);
}
const s256dir = process.env.ZXM_SPEC256;
if (s256dir) {
  const files = readdirSync(s256dir).filter(f => !f.startsWith('.')).sort();
  const sna = files.find(f => /\.sna$/i.test(f));
  for (const f of files) if (f !== sna) insert('gfx', s256dir, f);
  if (!sna || insert('snapshot', s256dir, sna)) { console.error('cannot load Spec256 game'); process.exit(1); }
}
if (process.env.ZXM_IFACE) {
  const data = readFileSync(process.env.ZXM_IFACE_ROM);
  const ir = x.malloc(32), p = x.malloc(data.length);
  u8().fill(0, ir, ir + 32);
  u8().set(data, p);
  dv().setUint32(ir, p, true);
  dv().setUint32(ir + 16, data.length, true);
  if (x.zxm_attach(m, cstr(process.env.ZXM_IFACE), ir)) { console.error('cannot plug in'); process.exit(1); }
}
if (process.env.ZXM_SNAP) {
  const path = process.env.ZXM_SNAP, i = path.lastIndexOf('/');
  if (insert('snapshot', i < 0 ? '.' : path.slice(0, i), path.slice(i + 1))) { console.error('cannot load snapshot'); process.exit(1); }
}
if (process.env.ZXM_DISK) {
  // zxm_media: kind, name, data, len, blocks, serial
  const data = readFileSync(process.env.ZXM_DISK);
  const p = x.malloc(data.length || 1);
  u8().set(data, p);
  const media = x.malloc(24);
  u8().fill(0, media, media + 24);
  dv().setUint32(media, 1, true);
  const np = cstr(process.env.ZXM_DISK);
  dv().setUint32(media + 4, np, true);
  dv().setUint32(media + 8, p, true);
  dv().setUint32(media + 12, data.length, true);
  if (x.zxm_media_insert(m, cstr('beta.a'), media)) { console.error('cannot insert disk'); process.exit(1); }
}
if (text !== undefined) x.zxm_type(m, cstr(text.replace(/\\n/g, '\n')), Number(process.env.ZXM_DELAY || 0));

const out = x.malloc(4);
function fnv(h, bytes) {
  for (let i = 0; i < bytes.length; i++) h = Math.imul(h ^ bytes[i], 16777619) >>> 0;
  return h;
}
function run(n, h) {
  for (let f = 0; f < n; f++) {
    x.zxm_run_frame(m);
    const fb = x.zxm_framebuffer(m, out);
    const pitch = dv().getInt32(out, true);
    const mem = u8();
    for (let y = 0; y < fbH; y++) {
      const a = fb + y * pitch * 2;
      h = fnv(h, mem.subarray(a, a + fbW * 2));
    }
    const au = x.zxm_audio(m, out);
    const samples = dv().getInt32(out, true);
    h = fnv(h, u8().subarray(au, au + samples * 4));
  }
  return h;
}

const frames = Number(framesArg), half = Math.floor(frames / 2);
const h0 = run(half, 2166136261);
const len = x.zxm_state_size(m);
const state = x.malloc(len);
if (x.zxm_state_save(m, state, len)) { console.error('state save failed'); process.exit(1); }
const h1 = run(frames - half, h0);
x.zxm_reset(m, 0);
run(3, 0);
if (x.zxm_state_load(m, state, len)) { console.error('state load failed'); process.exit(1); }
const h2 = run(frames - half, h0);
const hex = h1.toString(16).padStart(8, '0');
console.log(`${id} ${frames} frames: ${hex}${h1 === h2 ? '' : ' MISMATCH after state load'}`);
process.exit(h1 === h2 ? 0 : 1);
