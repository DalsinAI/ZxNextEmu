// ZX Cradle software library: titles (games, demos, utilities...) with their
// files, screenshots and a title.toml saying what each one is and what it
// runs on. See docs/cradle/titles.md for the format.
//
// In the browser, titles live in IndexedDB. Export and import use a .zip of
// the title's folder, so a title moves between Cradles unchanged.

// ---- storage ----------------------------------------------------------------

let dbp = null;
export function db() {
  dbp ??= new Promise((resolve, reject) => {
    const r = indexedDB.open('zx-cradle', 3);
    r.onupgradeneeded = () => {
      const d = r.result;
      if (!d.objectStoreNames.contains('media')) d.createObjectStore('media', { keyPath: 'id', autoIncrement: true });
      if (!d.objectStoreNames.contains('titles')) d.createObjectStore('titles', { keyPath: 'id' });
      if (!d.objectStoreNames.contains('roms')) d.createObjectStore('roms', { keyPath: 'name' });
    };
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  return dbp;
}

export function tx(store, mode, fn) {
  return db().then(d => new Promise((resolve, reject) => {
    const t = d.transaction(store, mode);
    const req = fn(t.objectStore(store));
    t.oncomplete = () => resolve(req?.result);
    t.onerror = () => reject(t.error);
  }));
}

export const titleAll = () => tx('titles', 'readonly', s => s.getAll());
export const titleGet = id => tx('titles', 'readonly', s => s.get(id));
export const titlePut = t => tx('titles', 'readwrite', s => s.put({ ...t, updated: Date.now() }));
export const titleDelete = id => tx('titles', 'readwrite', s => s.delete(id));

// ROMs the user added for machines whose ROMs are not shipped, as
// {"name.rom": "<base64>"}, the shape of roms.json.
export async function romsAdded() {
  const out = {};
  for (const r of await tx('roms', 'readonly', s => s.getAll())) out[r.name] = r.b64;
  return out;
}
export const romAdd = (name, b64) => tx('roms', 'readwrite', s => s.put({ name, b64, added: Date.now() }));

// ---- machines -------------------------------------------------------------

// Short labels for badges, and the family a plain tape suits.
export const machineLabels = {
  zx48: '48K', zx128: '128K', plus2: '+2', plus2a: '+2A', plus3: '+3', pentagon: 'Pentagon',
  next: 'Next', spec256: 'Spec256', scorpion: 'Scorpion', atm2: 'ATM Turbo', sam: 'SAM Coupé',
  msx1: 'MSX1', msx1eu: 'MSX1 50 Hz', msx2: 'MSX2', msx2eu: 'MSX2 50 Hz', oric1: 'Oric-1', atmos: 'Atmos',
};
export const label = id => machineLabels[id] || id;

const TAPE_48 = ['zx48', 'zx128', 'plus2', 'plus2a', 'plus3', 'pentagon'];
const TAPE_128 = ['zx128', 'plus2', 'plus2a', 'plus3', 'pentagon'];

const ext = name => (name.match(/\.([^./]+)$/)?.[1] || '').toLowerCase();

// What a file runs on, from its name and contents. null if it is not software.
export function detect(name, bytes) {
  const e = ext(name);
  const lower = name.toLowerCase();
  const wants128 = /128|\+2|\+3|ay\b/.test(lower);
  switch (e) {
  case 'tap': case 'tzx': case 'pzx':
    return { platform: 'zx-spectrum', slot: 'tape', machines: wants128 ? TAPE_128 : TAPE_48 };
  case 'sna':
    return { platform: 'zx-spectrum', slot: 'snapshot', machines: bytes.length === 49179 ? TAPE_48 : TAPE_128 };
  case 'z80': {
    // Version 1 headers have PC set; version 2 and 3 name the hardware.
    const pc = bytes[6] | bytes[7] << 8;
    if (pc) return { platform: 'zx-spectrum', slot: 'snapshot', machines: TAPE_48 };
    const extLen = bytes[30] | bytes[31] << 8, hw = bytes[34];
    const v3 = extLen >= 54;
    const is48 = v3 ? hw <= 3 : hw <= 2;
    if (is48) return { platform: 'zx-spectrum', slot: 'snapshot', machines: TAPE_48 };
    if (hw === 9) return { platform: 'zx-spectrum', slot: 'snapshot', machines: ['pentagon'] };
    if (hw === 7 || hw === 8) return { platform: 'zx-spectrum', slot: 'snapshot', machines: ['plus3', 'plus2a'] };
    return { platform: 'zx-spectrum', slot: 'snapshot', machines: TAPE_128 };
  }
  case 'nex': return { platform: 'zx-spectrum', slot: 'sd0', machines: ['next'] };
  case 'dsk': return { platform: 'zx-spectrum', slot: 'disk.a', machines: ['plus3'] };
  case 'trd': case 'scl': return { platform: 'zx-spectrum', slot: 'beta.a', machines: ['pentagon', 'scorpion', 'atm2'] };
  case 'mgt': case 'sad': return { platform: 'sam-coupe', slot: 'disk.1', machines: ['sam'] };
  case 'mx1': case 'mx2': return { platform: 'msx', slot: 'cart1', machines: e === 'mx1' ? ['msx1', 'msx2'] : ['msx2'] };
  case 'mdr': return { platform: 'zx-spectrum', slot: 'md.1', machines: ['zx48'] };
  default: return null;
  }
}

export const isImage = name => ['png', 'jpg', 'jpeg', 'gif', 'webp', 'scr'].includes(ext(name));
const isNote = name => ['txt', 'md', 'nfo', 'doc'].includes(ext(name)) && !/licen[cs]e|copying/i.test(name);

// "sanxion48v3.tap" -> "Sanxion48v3"; "Dizzy_VII_-_Crystal_Kindom" -> "Dizzy VII - Crystal Kindom".
export function tidyName(name) {
  const base = name.replace(/^.*\//, '').replace(/\.[^.]+$/, '').replace(/[_]+/g, ' ').replace(/\s+/g, ' ').trim();
  return base.charAt(0).toUpperCase() + base.slice(1);
}

export function slug(text) {
  return text.toLowerCase().normalize('NFKD').replace(/[^\w\s-]/g, '').replace(/[\s_]+/g, '-')
    .replace(/-+/g, '-').replace(/^-|-$/g, '').slice(0, 48) || 'title';
}

async function freeId(base) {
  const all = new Set((await titleAll()).map(t => t.id));
  if (!all.has(base)) return base;
  for (let i = 2; ; i++) if (!all.has(`${base}-${i}`)) return `${base}-${i}`;
}

// ---- title.toml -------------------------------------------------------------

// The subset of TOML that title.toml uses: strings, numbers, booleans, bare
// dates, arrays of those, [tables] and [[arrays of tables]].
export function parseToml(text) {
  const root = {};
  let cur = root;
  const lines = text.replace(/\r/g, '').split('\n');
  for (let i = 0; i < lines.length; i++) {
    let line = stripComment(lines[i]).trim();
    if (!line) continue;
    let m;
    if ((m = line.match(/^\[\[\s*([\w.-]+)\s*\]\]$/))) {
      (root[m[1]] ??= []).push(cur = {});
      continue;
    }
    if ((m = line.match(/^\[\s*([\w.-]+)\s*\]$/))) {
      cur = root[m[1]] ??= {};
      continue;
    }
    m = line.match(/^("?)([\w.-]+)\1\s*=\s*(.*)$/);
    if (!m) throw new Error(`title.toml line ${i + 1} is not "key = value".`);
    let value = m[3];
    // An array may run over several lines.
    while (value.startsWith('[') && !balanced(value) && i + 1 < lines.length) value += ' ' + stripComment(lines[++i]).trim();
    cur[m[2]] = parseValue(value.trim(), i + 1);
  }
  return root;
}

function stripComment(line) {
  let inStr = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (c === '\\' && inStr) i++;
    else if (c === '"') inStr = !inStr;
    else if (c === '#' && !inStr) return line.slice(0, i);
  }
  return line;
}

function balanced(s) {
  let depth = 0, inStr = false;
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (c === '\\' && inStr) i++;
    else if (c === '"') inStr = !inStr;
    else if (!inStr && c === '[') depth++;
    else if (!inStr && c === ']') depth--;
  }
  return depth === 0;
}

function parseValue(v, line) {
  if (v.startsWith('"')) return JSON.parse(v.replace(/\\'/g, "'"));
  if (v.startsWith('[')) {
    const items = [];
    let i = 1, start = 1, inStr = false, depth = 0;
    for (; i < v.length - 1; i++) {
      const c = v[i];
      if (c === '\\' && inStr) { i++; continue; }
      if (c === '"') inStr = !inStr;
      else if (!inStr && c === '[') depth++;
      else if (!inStr && c === ']') depth--;
      else if (!inStr && !depth && c === ',') { push(v.slice(start, i)); start = i + 1; }
    }
    push(v.slice(start, v.length - 1));
    return items;
    function push(s) { s = s.trim(); if (s) items.push(parseValue(s, line)); }
  }
  if (v === 'true' || v === 'false') return v === 'true';
  if (/^[+-]?\d[\d_]*(\.\d+)?$/.test(v)) return Number(v.replace(/_/g, ''));
  if (/^\d{4}-\d{2}-\d{2}/.test(v)) return v;
  throw new Error(`title.toml line ${line}: cannot read the value ${v}.`);
}

const tomlValue = v => Array.isArray(v) ? `[${v.map(tomlValue).join(', ')}]`
  : typeof v === 'string' ? (/^\d{4}-\d{2}-\d{2}$/.test(v) ? v : JSON.stringify(v)) : String(v);
const isTable = v => v && typeof v === 'object' && !Array.isArray(v);
const isTableArray = v => Array.isArray(v) && v.length && v.every(isTable);

// Key order for the fields we know; anything else follows, so it survives.
const ORDER = ['format', 'name', 'kind', 'year', 'authors', 'publisher', 'genres', 'tags', 'summary', 'license', 'source', 'added'];
const SUB_ORDER = ['platform', 'machines', 'file', 'files', 'slot', 'notes', 'caption'];

export function toToml(meta) {
  const keys = Object.keys(meta);
  const sortKeys = (ks, order) => ks.sort((a, b) => ((order.indexOf(a) + 1) || 99) - ((order.indexOf(b) + 1) || 99));
  const scalars = sortKeys(keys.filter(k => meta[k] != null && meta[k] !== '' && !isTable(meta[k]) && !isTableArray(meta[k])), ORDER);
  const width = Math.max(...scalars.map(k => k.length));
  let out = scalars.map(k => `${k.padEnd(width)} = ${tomlValue(meta[k])}`).join('\n') + '\n';
  const body = obj => {
    const ks = sortKeys(Object.keys(obj).filter(k => obj[k] != null && obj[k] !== ''), SUB_ORDER);
    const w = Math.max(0, ...ks.map(k => k.length));
    return ks.map(k => `${k.padEnd(w)} = ${tomlValue(obj[k])}`).join('\n') + '\n';
  };
  for (const k of keys.filter(k => isTable(meta[k]))) if (Object.keys(meta[k]).length) out += `\n[${k}]\n` + body(meta[k]);
  for (const k of ['runs', ...keys.filter(k => k !== 'runs' && k !== 'screens'), 'screens'].filter(k => isTableArray(meta[k])))
    for (const t of meta[k]) out += `\n[[${k}]]\n` + body(t);
  return out;
}

// ---- zip (store to write; store and deflate to read) --------------------------

const CRC = new Uint32Array(256).map((_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC[(c ^ bytes[i]) & 255] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

export function makeZip(entries) {
  const enc = new TextEncoder();
  const parts = [], central = [];
  let offset = 0;
  const now = new Date();
  const time = now.getHours() << 11 | now.getMinutes() << 5 | now.getSeconds() >> 1;
  const date = (now.getFullYear() - 1980) << 9 | (now.getMonth() + 1) << 5 | now.getDate();
  for (const { path, bytes } of entries) {
    const name = enc.encode(path), crc = crc32(bytes);
    const head = new DataView(new ArrayBuffer(30));
    head.setUint32(0, 0x04034b50, true); head.setUint16(4, 20, true); head.setUint16(6, 0x0800, true);
    head.setUint16(8, 0, true); head.setUint16(10, time, true); head.setUint16(12, date, true);
    head.setUint32(14, crc, true); head.setUint32(18, bytes.length, true); head.setUint32(22, bytes.length, true);
    head.setUint16(26, name.length, true); head.setUint16(28, 0, true);
    const cd = new DataView(new ArrayBuffer(46));
    cd.setUint32(0, 0x02014b50, true); cd.setUint16(4, 20, true); cd.setUint16(6, 20, true); cd.setUint16(8, 0x0800, true);
    cd.setUint16(10, 0, true); cd.setUint16(12, time, true); cd.setUint16(14, date, true);
    cd.setUint32(16, crc, true); cd.setUint32(20, bytes.length, true); cd.setUint32(24, bytes.length, true);
    cd.setUint16(28, name.length, true); cd.setUint32(42, offset, true);
    parts.push(head, name, bytes);
    central.push(cd, name);
    offset += 30 + name.length + bytes.length;
  }
  const cdSize = central.reduce((n, p) => n + p.byteLength, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true); end.setUint16(8, entries.length, true); end.setUint16(10, entries.length, true);
  end.setUint32(12, cdSize, true); end.setUint32(16, offset, true);
  return new Blob([...parts, ...central, end], { type: 'application/zip' });
}

export async function readZip(buf) {
  const bytes = new Uint8Array(buf), d = new DataView(buf);
  let e = bytes.length - 22;
  while (e >= 0 && d.getUint32(e, true) !== 0x06054b50) e--;
  if (e < 0) throw new Error('This is not a zip file.');
  const count = d.getUint16(e + 10, true);
  let p = d.getUint32(e + 16, true);
  const dec = new TextDecoder();
  const out = [];
  for (let i = 0; i < count; i++) {
    const method = d.getUint16(p + 10, true), size = d.getUint32(p + 20, true);
    const nameLen = d.getUint16(p + 28, true), extraLen = d.getUint16(p + 30, true), commentLen = d.getUint16(p + 32, true);
    const local = d.getUint32(p + 42, true);
    const path = dec.decode(bytes.subarray(p + 46, p + 46 + nameLen));
    p += 46 + nameLen + extraLen + commentLen;
    if (path.endsWith('/')) continue;
    const start = local + 30 + d.getUint16(local + 26, true) + d.getUint16(local + 28, true);
    const raw = bytes.subarray(start, start + size);
    let data;
    if (method === 0) data = raw.slice();
    else if (method === 8) data = new Uint8Array(await new Response(new Blob([raw]).stream().pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer());
    else throw new Error(`${path} in the zip uses a compression ZX Cradle cannot read.`);
    out.push({ path, bytes: data });
  }
  return out;
}

// ---- pictures ---------------------------------------------------------------

export async function rgbaToPng(pixels, w, h) {
  const c = new OffscreenCanvas(w, h);
  c.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(pixels), w, h), 0, 0);
  return c.convertToBlob({ type: 'image/png' });
}

// A .scr file is the Spectrum's screen memory: 6144 bytes of bitmap, then 768 attributes.
const SPECCY = [0x000000, 0x0000d7, 0xd70000, 0xd700d7, 0x00d700, 0x00d7d7, 0xd7d700, 0xd7d7d7,
  0x000000, 0x0000ff, 0xff0000, 0xff00ff, 0x00ff00, 0x00ffff, 0xffff00, 0xffffff];
export function scrToRgba(bytes) {
  const out = new Uint32Array(256 * 192);
  for (let y = 0; y < 192; y++) {
    const row = ((y & 0xc0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2);
    for (let cx = 0; cx < 32; cx++) {
      const bits = bytes[row + cx], attr = bytes[6144 + (y >> 3) * 32 + cx];
      const bright = attr & 0x40 ? 8 : 0;
      const ink = SPECCY[(attr & 7) + bright], paper = SPECCY[((attr >> 3) & 7) + bright];
      for (let b = 0; b < 8; b++) {
        const rgb = bits & (0x80 >> b) ? ink : paper;
        out[y * 256 + cx * 8 + b] = 0xff000000 | (rgb & 0xff) << 16 | (rgb & 0xff00) | (rgb >> 16);
      }
    }
  }
  return out.buffer;
}

async function imageToPng(name, bytes) {
  if (ext(name) === 'scr') return bytes.length >= 6912 ? rgbaToPng(scrToRgba(bytes), 256, 192) : null;
  if (ext(name) === 'png') return new Blob([bytes], { type: 'image/png' });
  try {
    const bmp = await createImageBitmap(new Blob([bytes]));
    const c = new OffscreenCanvas(bmp.width, bmp.height);
    c.getContext('2d').drawImage(bmp, 0, 0);
    return c.convertToBlob({ type: 'image/png' });
  } catch { return null; }
}

// Is a picture worth keeping? Not if it is nearly one colour (a blank or
// loading screen) or the same as one we already have.
export function pictureScore(pixels) {
  const px = new Uint32Array(pixels);
  const counts = new Map();
  for (let i = 0; i < px.length; i += 7) counts.set(px[i], (counts.get(px[i]) || 0) + 1);
  const top = Math.max(...counts.values());
  return { colours: counts.size, plain: top / Math.ceil(px.length / 7), hash: hashPixels(px) };
}
function hashPixels(px) {
  let h = 2166136261;
  for (let i = 0; i < px.length; i += 3) h = Math.imul(h ^ px[i], 16777619);
  return h >>> 0;
}

// ---- import -------------------------------------------------------------------

// Turn dropped files (with folder paths when a folder was dropped) and zip
// files into titles:
// - a folder or zip with a title.toml is one title, as described;
// - other folders become one title each, named after the folder;
// - loose software files become one title each.
// Returns the ids added.
export async function importFiles(files) {
  const items = [];
  for (const f of files) {
    const path = f.webkitRelativePath || f.relPath || f.name;
    const bytes = new Uint8Array(await f.arrayBuffer());
    if (ext(f.name) === 'zip') {
      const base = f.name.replace(/\.zip$/i, '');
      for (const e of await readZip(bytes.buffer)) items.push({ path: `${base}/${e.path}`, bytes: e.bytes, fromZip: true });
    } else items.push({ path, bytes });
  }
  // Group by the folder that holds a title.toml, else by top-level folder.
  const groups = new Map();
  const tomlDirs = items.filter(i => /(^|\/)title\.toml$/i.test(i.path)).map(i => i.path.replace(/title\.toml$/i, ''));
  for (const it of items) {
    let key = tomlDirs.find(d => it.path.startsWith(d));
    if (key == null) {
      const segs = it.path.split('/');
      // A dropped folder of folders: each subfolder is a title. Loose files are their own.
      key = segs.length > 2 ? segs.slice(0, 2).join('/') + '/' : segs.length === 2 && !tomlDirs.length && items.some(o => o.path.split('/').length > 2 && o.path.startsWith(segs[0] + '/')) ? `${it.path}#` : segs.length > 1 ? segs[0] + '/' : `${it.path}#`;
    }
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(it);
  }
  const added = [];
  for (const [key, group] of groups) {
    const t = await titleFromGroup(key, group);
    if (t) { await titlePut(t); added.push(t.id); }
  }
  return added;
}

async function titleFromGroup(key, group) {
  const prefix = key.endsWith('#') ? key.slice(0, -1).replace(/[^/]*$/, '') : key;
  const rel = it => it.path.slice(prefix.length);
  const toml = group.find(i => /(^|\/)title\.toml$/i.test(i.path) && rel(i).toLowerCase() === 'title.toml');
  const files = [], screens = [];
  let about = '';
  if (toml) {
    const meta = parseToml(new TextDecoder().decode(toml.bytes));
    for (const it of group) {
      const r = rel(it);
      if (r.startsWith('files/')) files.push({ path: r, bytes: it.bytes });
      else if (r.startsWith('screens/') && isImage(r)) {
        const png = await imageToPng(r, it.bytes);
        const cap = (meta.screens || []).find(s => s.file === r)?.caption || '';
        if (png) screens.push({ path: r.replace(/\.[^.]+$/, '.png'), blob: png, caption: cap });
      } else if (r.toLowerCase() === 'about.md') about = new TextDecoder().decode(it.bytes);
    }
    const id = await freeId(slug(prefix.replace(/\/$/, '').replace(/^.*\//, '') || meta.name || 'title'));
    delete meta.screens;
    return { id, meta, about, files, screens, added: Date.now(), needsShots: !screens.length };
  }
  // No title.toml: work out what we can.
  const runs = [];
  let name = key.endsWith('#') ? tidyName(key.slice(0, -1)) : tidyName(prefix.replace(/\/$/, ''));
  for (const it of group) {
    const r = rel(it).replace(/^.*\//, '');
    const what = detect(r, it.bytes);
    if (what) {
      const path = `files/${r}`;
      files.push({ path, bytes: it.bytes });
      runs.push({ platform: what.platform, machines: what.machines, file: path, slot: what.slot });
    } else if (isImage(r)) {
      const png = await imageToPng(r, it.bytes);
      if (png) screens.push({ path: `screens/${screens.length + 1}.png`, blob: png, caption: '' });
    } else if (isNote(r) && it.bytes.length < 65536) {
      about += (about ? '\n\n' : '') + new TextDecoder().decode(it.bytes).replace(/\r/g, '').trim();
    }
  }
  if (!files.length) return null;
  // Best run first: the one that suits the most machines' newest member last.
  runs.sort((a, b) => (b.machines.includes('zx128') ? 1 : 0) - (a.machines.includes('zx128') ? 1 : 0));
  const meta = { format: 1, name, kind: 'game', runs, added: new Date().toISOString().slice(0, 10) };
  const id = await freeId(slug(name));
  return { id, meta, about, files, screens, added: Date.now(), needsShots: !screens.length };
}

// ---- the shared collection --------------------------------------------------

// collection.json, packed from a folder of titles: {titles: [{id, toml, about,
// files: [{path, b64}], screens: [{path, b64}]}]}. Titles already in the
// library are left alone. Returns the ids added.
export async function importCollection(data) {
  const have = new Set((await titleAll()).map(t => t.id));
  const bytes = b64 => Uint8Array.from(atob(b64), c => c.charCodeAt(0));
  const added = [];
  for (const c of data.titles) {
    if (have.has(c.id)) continue;
    const meta = parseToml(c.toml);
    const screens = c.screens.filter(s => isImage(s.path)).map(s => ({
      path: s.path, blob: new Blob([bytes(s.b64)], { type: 'image/png' }),
      caption: (meta.screens || []).find(x => x.file === s.path)?.caption || '',
    }));
    delete meta.screens;
    await titlePut({ id: c.id, meta, about: c.about || '', files: c.files.map(f => ({ path: f.path, bytes: bytes(f.b64) })), screens, added: Date.now(), needsShots: !screens.length });
    added.push(c.id);
  }
  return added;
}

// ---- export -------------------------------------------------------------------

export async function exportTitle(t) {
  const enc = new TextEncoder();
  const meta = { ...t.meta, screens: t.screens.map(s => ({ file: s.path, ...(s.caption ? { caption: s.caption } : {}) })) };
  if (!meta.screens.length) delete meta.screens;
  const entries = [{ path: `${t.id}/title.toml`, bytes: enc.encode(toToml(meta)) }];
  if (t.about) entries.push({ path: `${t.id}/about.md`, bytes: enc.encode(t.about) });
  for (const f of t.files) entries.push({ path: `${t.id}/${f.path}`, bytes: f.bytes });
  for (const s of t.screens) entries.push({ path: `${t.id}/${s.path}`, bytes: new Uint8Array(await s.blob.arrayBuffer()) });
  return makeZip(entries);
}

// ---- search -------------------------------------------------------------------

export function haystack(t) {
  const m = t.meta;
  return [m.name, m.publisher, m.summary, m.kind, m.license, ...(m.authors || []), ...(m.genres || []), ...(m.tags || []),
    ...t.files.map(f => f.path.replace(/^files\//, '')), ...(m.runs || []).flatMap(r => (r.machines || []).map(label))]
    .filter(Boolean).join(' ').toLowerCase();
}

export function matches(t, query) {
  if (!query) return true;
  const h = t._hay ??= haystack(t);
  return query.toLowerCase().split(/\s+/).filter(Boolean).every(w => h.includes(w));
}

// The files a run loads, in order, and where each goes. A multi-file run
// (Spec256: graphics files, then the snapshot) sends its snapshot last.
export function runParts(t, run) {
  const paths = run.files || (run.file ? [run.file] : []);
  const missing = paths.filter(p => !t.files.some(f => f.path === p));
  const parts = paths.filter(p => !missing.includes(p)).map(p => {
    const name = p.replace(/^.*\//, '');
    const e = ext(name);
    const slot = paths.length > 1 && ['sna', 'z80'].includes(e) ? 'snapshot' : run.slot || detect(name, new Uint8Array(64))?.slot;
    return { name, slot, bytes: t.files.find(f => f.path === p).bytes };
  });
  parts.sort((a, b) => (a.slot === 'snapshot') - (b.slot === 'snapshot'));
  return { parts, missing };
}

// Put the user's own copies of missing files into a title, matched by name.
export function attachFiles(t, picked) {
  const wanted = (t.meta.runs || []).flatMap(r => r.files || (r.file ? [r.file] : []))
    .filter(p => !t.files.some(f => f.path === p));
  const added = [];
  for (const { name, bytes } of picked) {
    const p = wanted.find(w => w.replace(/^.*\//, '').toLowerCase() === name.toLowerCase());
    if (p && !added.includes(p)) { t.files.push({ path: p, bytes }); added.push(p); }
  }
  return added;
}

export const titleMachines = t => [...new Set((t.meta.runs || []).flatMap(r => r.machines || []))];
