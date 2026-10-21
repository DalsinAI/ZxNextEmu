// ZX Cradle's Node host, first part: serves the Cradle page (build/web) on
// http://127.0.0.1:7080/ and the software library from a folder of titles
// (docs/cradle/titles.md), so a library of any size is read in place instead
// of being copied into the browser.
//
//   node host/host.mjs [--web build/web] [--library ~/.zxcradle/library]
//
// GET /library/titles.json   every title: {id, json, about, reviews, files, screens}
//                            with file sizes and paths, no file contents
// GET /library/titles/<id>/<path>   one file of a title
//
// GET /roms.json             the page's ROMs plus the ROM files in ~/.zxcradle/roms
//                            (or ZXCRADLE_ROMS): the ones that may not be shipped,
//                            such as TR-DOS, the Scorpion and ATM sets and esxDOS
//
// ZXCRADLE_PORT moves the port (docs/cradle/design.md, "Ports");
// ZXCRADLE_LIBRARY names the library folder. Standard library only.

import http from 'node:http';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const arg = (name, def) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : def; };
const WEB = path.resolve(arg('--web', path.join(here, '..', 'build', 'web')));
const LIBRARY = path.resolve(arg('--library', process.env.ZXCRADLE_LIBRARY || path.join(os.homedir(), '.zxcradle', 'library')));
const TITLES = path.join(LIBRARY, 'titles');
const ROMS = path.resolve(process.env.ZXCRADLE_ROMS || path.join(os.homedir(), '.zxcradle', 'roms'));
const PORT = Number(process.env.ZXCRADLE_PORT || 7080);
const RESCAN_MS = 10 * 60 * 1000;

const TYPES = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.json': 'application/json', '.wasm': 'application/wasm', '.md': 'text/markdown; charset=utf-8',
  '.png': 'image/png', '.gif': 'image/gif', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.webp': 'image/webp', '.svg': 'image/svg+xml',
};

// ---- the title index ------------------------------------------------------------

// id -> {mtime, entry}. A title folder is read again only when its mtime
// changes, so a rescan of a library that a fetch is still filling is quick.
const cache = new Map();
let index = null;           // {json: Buffer, gz: Buffer, count}
let scanning = null;

async function readJson(p) {
  try { return JSON.parse(await fsp.readFile(p, 'utf8')); } catch { return undefined; }
}

async function listFiles(dir, base) {
  const out = [];
  let names;
  try { names = await fsp.readdir(dir, { withFileTypes: true }); } catch { return out; }
  for (const d of names.sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0)) {
    const p = path.join(dir, d.name);
    const rel = `${base}/${d.name}`;
    if (d.isDirectory()) out.push(...await listFiles(p, rel));
    else if (d.isFile()) {
      // A fetch still filling the library can move a file away between the listing and here.
      try { out.push({ path: rel, size: (await fsp.stat(p)).size }); } catch { /* gone */ }
    }
  }
  return out;
}

async function readTitle(id) {
  const d = path.join(TITLES, id);
  let json = await readJson(path.join(d, 'title.json')), toml;
  if (json === undefined) {
    try { toml = await fsp.readFile(path.join(d, 'title.toml'), 'utf8'); } catch { return null; }
  }
  let about;
  try { about = await fsp.readFile(path.join(d, 'about.md'), 'utf8'); } catch { /* optional */ }
  const reviews = await readJson(path.join(d, 'reviews.json'));
  return { id, ...(json !== undefined ? { json } : { toml }), about, reviews,
    files: await listFiles(path.join(d, 'files'), 'files'), screens: await listFiles(path.join(d, 'screens'), 'screens') };
}

async function scan() {
  const t0 = Date.now();
  let ids;
  try { ids = (await fsp.readdir(TITLES, { withFileTypes: true })).filter(d => d.isDirectory() && !d.name.startsWith('.')).map(d => d.name).sort(); }
  catch (err) { console.warn(`ZX Cradle: no library at ${TITLES} yet (${err.code}); trying again later.`); return; }
  const seen = new Set(ids);
  for (const id of cache.keys()) if (!seen.has(id)) cache.delete(id);
  let read = 0;
  // A few folders at a time: the library is often on a spinning disk.
  const queue = [...ids];
  await Promise.all(Array.from({ length: 16 }, async () => {
    for (let id; (id = queue.shift()) !== undefined;) {
      let mtime;
      try { mtime = (await fsp.stat(path.join(TITLES, id))).mtimeMs; } catch { continue; }
      const c = cache.get(id);
      if (c && c.mtime === mtime) continue;
      let entry = null;
      try { entry = await readTitle(id); } catch (err) { console.warn(`ZX Cradle: ${id}: ${err.message}`); }
      if (entry) { cache.set(id, { mtime, entry }); read++; } else cache.delete(id);
    }
  }));
  const list = ids.filter(id => cache.has(id)).map(id => cache.get(id).entry);
  const json = Buffer.from(JSON.stringify({ format: 2, host: true, titles: list }));
  index = { json, gz: zlib.gzipSync(json, { level: 6 }), count: list.length };
  console.log(`ZX Cradle: ${list.length} titles in the library (${read} read in ${((Date.now() - t0) / 1000).toFixed(1)} s).`);
}

function rescan() {
  scanning ??= scan().catch(err => console.warn(`ZX Cradle: library scan failed: ${err.message}`)).finally(() => { scanning = null; });
  return scanning;
}

// ---- HTTP ----------------------------------------------------------------------------

// A path inside root, or null if the request tries to leave it.
function inside(root, rel) {
  const p = path.resolve(root, '.' + path.posix.normalize('/' + rel));
  return p === root || p.startsWith(root + path.sep) ? p : null;
}

async function sendFile(req, res, p) {
  let st;
  try { st = await fsp.stat(p); } catch { st = null; }
  if (st?.isDirectory()) return sendFile(req, res, path.join(p, 'index.html'));
  if (!st?.isFile()) { res.writeHead(404, { 'content-type': 'text/plain' }); res.end('Not found\n'); return; }
  res.writeHead(200, { 'content-type': TYPES[path.extname(p).toLowerCase()] || 'application/octet-stream', 'content-length': st.size, 'cache-control': 'no-cache' });
  if (req.method === 'HEAD') { res.end(); return; }
  fs.createReadStream(p).pipe(res);
}

async function handle(req, res) {
  if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405); res.end(); return; }
  const url = new URL(req.url, 'http://localhost');
  const rel = decodeURIComponent(url.pathname);
  if (rel === '/library/titles.json') {
    if (!index || url.searchParams.has('rescan')) await rescan();
    if (!index) { res.writeHead(503, { 'content-type': 'text/plain' }); res.end('The library is not there yet.\n'); return; }
    const gz = /\bgzip\b/.test(req.headers['accept-encoding'] || '');
    const body = gz ? index.gz : index.json;
    res.writeHead(200, { 'content-type': 'application/json', 'content-length': body.length, 'cache-control': 'no-cache', ...(gz ? { 'content-encoding': 'gzip' } : {}) });
    res.end(req.method === 'HEAD' ? undefined : body);
    return;
  }
  if (rel === '/roms.json') {
    let roms = {};
    try { roms = JSON.parse(await fsp.readFile(path.join(WEB, 'roms.json'), 'utf8')); } catch { /* none shipped */ }
    let names = [];
    try { names = (await fsp.readdir(ROMS, { withFileTypes: true })).filter(d => d.isFile() && /\.(rom|bin)$/i.test(d.name)).map(d => d.name); } catch { /* no ROM folder */ }
    for (const n of names) roms[n] = (await fsp.readFile(path.join(ROMS, n))).toString('base64');
    const body = Buffer.from(JSON.stringify(roms));
    res.writeHead(200, { 'content-type': 'application/json', 'content-length': body.length, 'cache-control': 'no-cache' });
    res.end(req.method === 'HEAD' ? undefined : body);
    return;
  }
  if (rel.startsWith('/library/titles/')) {
    const p = inside(TITLES, rel.slice('/library/titles/'.length));
    return p ? sendFile(req, res, p) : (res.writeHead(403), res.end());
  }
  const p = inside(WEB, rel);
  return p ? sendFile(req, res, p) : (res.writeHead(403), res.end());
}

const server = http.createServer((req, res) => handle(req, res).catch(err => {
  console.warn(`ZX Cradle: ${req.url}: ${err.message}`);
  if (!res.headersSent) res.writeHead(500);
  res.end();
}));
server.on('error', err => {
  if (err.code === 'EADDRINUSE') console.error(`ZX Cradle: port ${PORT} is taken by another program (see: ss -ltnp 'sport = :${PORT}'). Stopping.`);
  else console.error(`ZX Cradle: ${err.message}`);
  process.exit(1);
});
if (!fs.existsSync(path.join(WEB, 'index.html'))) console.warn(`ZX Cradle: no page in ${WEB}; run make web first.`);
server.listen(PORT, '127.0.0.1', () => {
  console.log(`ZX Cradle: http://127.0.0.1:${PORT}/ (library ${LIBRARY})`);
  rescan();
  setInterval(rescan, RESCAN_MS).unref();
});
