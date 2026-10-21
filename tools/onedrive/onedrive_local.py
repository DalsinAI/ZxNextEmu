#!/usr/bin/env python3
# onedrive_local.py LIBRARY SHARE_LINK [RECORDS.json.gz]
# Downloads a shared OneDrive folder into LIBRARY/_onedrive/ and files every title listed in the
# records file into LIBRARY/titles/<id>/ (title.json format 2, files/, screens/).
# A title whose ZXDB id is already in the library gets the OneDrive files added to it, never a second folder.
# Python 3 standard library only. Safe to re-run: finished files and titles are skipped.
import base64, gzip, json, os, re, shutil, struct, sys, time, urllib.request, zipfile, zlib, datetime

API = 'https://my.microsoftpersonalcontent.com/_api/v2.0/'
SKIP_EXT = ('.hdf',)  # raw hard-disk images; the same images are in the folder as .rar
TODAY = datetime.date.today().isoformat()
# What the player can load, by slot. Anything else in the records (.sp, .slt, .xml, .mgt, and +3 .dsk,
# which no machine has a drive for yet) is left out.
LOADS = {'tape': ('.tap', '.tzx'), 'snapshot': ('.sna', '.z80')}
NO_SLOT = {'snapshot': {'next'}}  # machines without that slot (src/core/zxm_zx.c)

def say(*a):
    print(*a, flush=True)

def http(url, headers=None, data=None, tries=6):
    for k in range(tries):
        try:
            req = urllib.request.Request(url, data=data, headers={'User-Agent': 'zxcradle', **(headers or {})})
            return urllib.request.urlopen(req, timeout=300)
        except urllib.error.HTTPError as e:
            if 400 <= e.code < 500 and e.code != 429:
                raise
            err = e
        except Exception as e:
            err = e
        time.sleep(2 ** k)
    raise err

class OneDrive:
    def __init__(self, link):
        # Try the link as pasted, then without its ?query part.
        tries = [link] + ([link.split('?')[0]] if '?' in link else [])
        for n, self.link in enumerate(tries):
            self.renew()
            try:
                root = self.get(f'shares/{self.sid}/driveitem')
                break
            except urllib.error.HTTPError as e:
                if n == len(tries) - 1:
                    raise
        self.drive = root['parentReference']['driveId']
        self.root = root

    def renew(self):
        r = http('https://api-badgerp.svc.ms/v1.0/token', {'Content-Type': 'application/json'},
                 json.dumps({'appId': '5cbed6ac-a083-4e14-b191-b4ba07653de2'}).encode())
        self.h = {'Authorization': 'Badger ' + json.load(r)['token'], 'Prefer': 'autoredeem'}
        self.sid = 'u!' + base64.urlsafe_b64encode(self.link.encode()).decode().rstrip('=')
        self.born = time.time()

    def headers(self):
        if time.time() - self.born > 1800:
            self.renew()
        return self.h

    def get(self, path):
        return json.load(http(path if path.startswith('http') else API + path, self.headers()))

    def walk(self, item=None, path=None):
        item = item or self.root['id']
        path = path or self.root['name']
        url = f'drives/{self.drive}/items/{item}/children?$top=1000&select=id,name,size,folder,file'
        while url:
            j = self.get(url)
            for c in j['value']:
                p = path + '/' + c['name']
                if 'folder' in c:
                    yield from self.walk(c['id'], p)
                else:
                    yield {'id': c['id'], 'path': p, 'size': c.get('size') or 0}
            url = j.get('@odata.nextLink')

    def download(self, it, out):
        if os.path.exists(out) and os.path.getsize(out) == it['size']:
            return False
        os.makedirs(os.path.dirname(out), exist_ok=True)
        for k in range(6):
            try:
                r = http(API + f'drives/{self.drive}/items/{it["id"]}/content', self.headers())
                with open(out + '.part', 'wb') as f:
                    shutil.copyfileobj(r, f, 1 << 20)
                if os.path.getsize(out + '.part') == it['size']:
                    os.replace(out + '.part', out)
                    return True
            except Exception as e:
                say('  retry', it['path'], e)
            time.sleep(2 ** k)
        say('  FAILED', it['path'])
        return False

# --- screenshots: .scr -> PNG without Pillow ---
PAL = [(0, 0, 0), (0, 0, 0xd7), (0xd7, 0, 0), (0xd7, 0, 0xd7), (0, 0xd7, 0), (0, 0xd7, 0xd7), (0xd7, 0xd7, 0), (0xd7, 0xd7, 0xd7),
       (0, 0, 0), (0, 0, 0xff), (0xff, 0, 0), (0xff, 0, 0xff), (0, 0xff, 0), (0, 0xff, 0xff), (0xff, 0xff, 0), (0xff, 0xff, 0xff)]

def scr_png(d):
    rows = []
    for y in range(192):
        row = bytearray([0])
        base = ((y & 0xc0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2)
        for xb in range(32):
            b, a = d[base + xb], d[6144 + (y >> 3) * 32 + xb]
            br = 8 if a & 0x40 else 0
            ink, paper = PAL[(a & 7) + br], PAL[((a >> 3) & 7) + br]
            for bit in range(8):
                row += bytes(ink if b & (0x80 >> bit) else paper)
        rows.append(bytes(row))
    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 256, 192, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))

def save_screen(url, sdir, n):
    data = http(url, tries=3).read()
    if url.lower().endswith('.scr') or len(data) == 6912:
        name, data = f'{n}.png', scr_png(data)
    else:
        ext = os.path.splitext(url.split('?')[0])[1].lower() or '.png'
        name = f'{n}{ext}'
    os.makedirs(sdir, exist_ok=True)
    open(os.path.join(sdir, name), 'wb').write(data)
    return 'screens/' + name

def slug(s):
    return re.sub(r'[^a-z0-9]+', '-', s.lower()).strip('-')[:60].strip('-') or 'untitled'

def write_json(p, j):
    open(p + '.tmp', 'w', encoding='utf-8').write(json.dumps(j, indent=2, ensure_ascii=False) + '\n')
    os.replace(p + '.tmp', p)

ZXDB_INDEX = {}

def index_library(lib):
    for tid in os.listdir(lib):
        try:
            z = (json.load(open(os.path.join(lib, tid, 'title.json'), encoding='utf-8')).get('ids') or {}).get('zxdb')
        except Exception:
            continue
        if z:
            ZXDB_INDEX.setdefault(str(z), tid)

def folder_for(lib, r):
    if r['zxdb'] and str(r['zxdb']) in ZXDB_INDEX:
        return ZXDB_INDEX[str(r['zxdb'])], True
    base, n = slug(r['name']), 1
    while True:
        tid = base if n == 1 else f'{base}-{n}'
        p = os.path.join(lib, tid, 'title.json')
        if not os.path.exists(os.path.join(lib, tid)):
            return tid, False
        try:
            j = json.load(open(p, encoding='utf-8'))
        except Exception:
            j = {}
        if r['zxdb'] and str((j.get('ids') or {}).get('zxdb')) == str(r['zxdb']):
            return tid, True
        if not r['zxdb'] and j.get('name') == r['name'] and 'onedrive' in (j.get('tags') or []):
            return tid, True
        n += 1

def slot_of(name):
    ext = os.path.splitext(name)[1].lower()
    return next((k for k, v in LOADS.items() if ext in v), None)

def copy_file(src, out):
    # Copy through a .part file so an interrupted copy is never mistaken for a finished one.
    if os.path.exists(out) and os.path.getsize(out) == os.path.getsize(src):
        return
    os.makedirs(os.path.dirname(out), exist_ok=True)
    shutil.copy2(src, out + '.part')
    os.replace(out + '.part', out)

def gather(r, dest, fdir):
    # Copies each loadable file into fdir and returns [(record run, file name, slot)].
    # A .zip is unpacked: each tape or snapshot inside becomes a file of its own.
    got = []
    for x in r['runs']:
        src = os.path.join(dest, '_onedrive', x['src'])
        if src.lower().endswith('.zip'):
            try:
                z = zipfile.ZipFile(src)
            except zipfile.BadZipFile:
                continue
            for zi in z.infolist():
                name = os.path.basename(zi.filename)
                if zi.is_dir() or not slot_of(name):
                    continue
                out = os.path.join(fdir, name)
                if not (os.path.exists(out) and os.path.getsize(out) == zi.file_size):
                    os.makedirs(fdir, exist_ok=True)
                    with z.open(zi) as f, open(out + '.part', 'wb') as o:
                        shutil.copyfileobj(f, o)
                    os.replace(out + '.part', out)
                got.append((x, name, slot_of(name)))
        elif slot_of(src) == x['slot']:
            copy_file(src, os.path.join(fdir, os.path.basename(src)))
            got.append((x, os.path.basename(src), x['slot']))
    return got

def runs_for(got, sub):
    # One run per file: the player loads one tape at a time, so each side or part is its own run.
    many = len(got) > 1
    rs = []
    for x, name, slot in got:
        machines = [m for m in x['machines'] if m not in NO_SLOT.get(slot, ())]
        if not machines:
            continue
        v = {'platform': 'zx-spectrum', 'machines': machines, 'files': ['files/' + sub + name], 'slot': slot, 'needs': []}
        if many:
            v['notes'] = f"{x['variant']}: {name}"
        rs.append(v)
    rs.sort(key=lambda v: (v['slot'] != 'tape', '(Europe)' not in v.get('notes', '(Europe)')))
    return rs

def file_title(r, dest, lib, link):
    tid, merge = folder_for(lib, r)
    tdir = os.path.join(lib, tid)
    if merge:
        try:
            old = json.load(open(os.path.join(tdir, 'title.json'), encoding='utf-8'))
        except Exception:
            old = {}
        if 'onedrive' in (old.get('tags') or []):
            return tid, 'merged'
    sub = 'onedrive/' if merge else ''
    srcs = [os.path.join(dest, '_onedrive', x['src']) for x in r['runs']]
    if not all(os.path.exists(s) for s in srcs):
        return tid, 'missing'
    # A new title is built in a staging folder and renamed into place once title.json is written,
    # so an interrupted run leaves nothing that looks like a title. A stale staging folder is redone.
    final = tdir
    if not merge:
        tdir = os.path.join(dest, '_onedrive', '.staging', tid)
        shutil.rmtree(tdir, ignore_errors=True)
    got = gather(r, dest, os.path.join(tdir, 'files', sub))
    runs = runs_for(got, sub)
    if not runs:
        if not merge:  # only ever delete our own staging folder, never an existing title
            shutil.rmtree(tdir, ignore_errors=True)
        return tid, 'unsupported'
    source = link
    p = os.path.join(tdir, 'title.json')
    if merge and os.path.exists(p):
        j = json.load(open(p, encoding='utf-8'))
        have = {f for ru in j.get('runs', []) for f in ru.get('files', [])}
        j.setdefault('runs', []).extend(ru for ru in runs if not set(ru['files']) & have)
        if r['zxdb']:
            j.setdefault('ids', {}).setdefault('zxdb', r['zxdb'])
        j['tags'] = (j.get('tags') or []) + ['onedrive']
        s = j.get('sources') or [j.get('source')]
        if source not in s:
            j['sources'] = [x for x in s if x] + [source]
        write_json(p, j)
        return tid, 'merged'
    j = {'format': 2, 'id': tid, 'name': r['name'], 'kind': r['kind']}
    for k in ('year', 'authors', 'publisher', 'genres'):
        if r.get(k):
            j[k] = r[k]
    j['tags'] = ['onedrive']
    j['license'] = r['license']
    j['source'] = source
    if r['zxdb']:
        j['sources'] = [source, f'https://spectrumcomputing.co.uk/entry/{r["zxdb"]}']
        j['ids'] = {'zxdb': r['zxdb']}
    j['added'] = TODAY
    j['runs'] = runs
    j['compat'] = {}
    screens = []
    for url, cap in r['screens'][:3]:
        try:
            screens.append({'file': save_screen(url, os.path.join(tdir, 'screens'), len(screens) + 1), 'caption': cap})
        except Exception as e:
            say('  no screenshot', url, e)
    if screens:
        j['screens'] = screens
    write_json(p, j)
    about = [f'# {r["name"]}', '', '## Where it came from', '',
             '- OneDrive Spectrum collection: ' + ', '.join(f'`{os.path.basename(x["src"])}`' for x in r['runs'])]
    if r['zxdb']:
        about.append(f'- Details and screenshots: ZXDB entry {r["zxdb"]}, https://spectrumcomputing.co.uk/entry/{r["zxdb"]}')
    if any(c not in ('exact', 'manual') for c in r['conf']):
        about += ['', f'Matched to ZXDB by name ({", ".join(r["conf"])}) from "{r["groups"][0]}"; check it if the details look wrong.']
    open(os.path.join(tdir, 'about.md'), 'w', encoding='utf-8').write('\n'.join(about) + '\n')
    os.replace(tdir, final)
    if r['zxdb']:
        ZXDB_INDEX[str(r['zxdb'])] = tid
    return tid, 'new'

def clean_link(link):
    link = link.strip().strip('\'"<>').strip()
    if not re.match(r'^https://(1drv\.ms|onedrive\.live\.com)/', link):
        sys.exit('That does not look like a OneDrive share link. It should start with https://1drv.ms/ '
                 'or https://onedrive.live.com/. In OneDrive, right-click the folder, choose Share, then Copy link.')
    return link

def main():
    if len(sys.argv) < 3:
        sys.exit('usage: onedrive_local.py LIBRARY SHARE_LINK [RECORDS.json.gz]')
    dest, link = os.path.abspath(sys.argv[1]), clean_link(sys.argv[2])
    recp = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'onedrive-records.json.gz')
    limit = int(os.environ.get('ZXC_LIMIT', '0')) or None
    lib = os.path.join(dest, 'titles')
    os.makedirs(lib, exist_ok=True)
    try:
        od = OneDrive(link)
    except urllib.error.HTTPError as e:
        if e.code >= 500 or e.code == 429:   # OneDrive itself is having trouble: the link may be fine
            print(f'OneDrive is not answering properly (HTTP {e.code}). Run this again later.', file=sys.stderr)
            sys.exit(4)
        sys.exit(f'OneDrive refused the link (HTTP {e.code}). Check it is the whole link and that the folder is '
                 'shared as "Anyone with the link can view", then run this again.')
    except urllib.error.URLError as e:
        # 4, not 1: a network failure says nothing about the link, so a caller keeps it.
        print(f'Could not reach OneDrive ({e.reason}). Check the internet connection and run this again.', file=sys.stderr)
        sys.exit(4)
    say('Listing the OneDrive folder...')
    try:
        items = [i for i in od.walk() if not i['path'].lower().endswith(SKIP_EXT)]
    except (urllib.error.URLError, OSError) as e:
        print(f'Listing the OneDrive folder failed ({e}). Run this again to carry on.', file=sys.stderr)
        sys.exit(4)
    want = None
    recs = json.load(gzip.open(recp, 'rt', encoding='utf-8')) if os.path.exists(recp) else []
    if limit:
        recs = recs[:limit]
        want = {x['src'] for r in recs for x in r['runs']}
        items = [i for i in items if i['path'] in want]
    total = sum(i['size'] for i in items)
    say(f'{len(items)} files, {total / 1e6:.0f} MB to fetch into {dest}/_onedrive')
    got, failed = 0, []
    for n, it in enumerate(items, 1):
        out = os.path.join(dest, '_onedrive', it['path'])
        if not od.download(it, out) and not (os.path.exists(out) and os.path.getsize(out) == it['size']):
            failed.append(it['path'])
        got += it['size']
        if n % 100 == 0 or n == len(items):
            say(f'  {n}/{len(items)} files, {got / 1e6:.0f} of {total / 1e6:.0f} MB')
    if failed:
        say(f'{len(failed)} files could not be downloaded; run this again to retry them.')
    if not recs:
        say('No records file found, so the files are downloaded but not filed into the library.')
        sys.exit(3 if failed else 0)
    logp = os.path.join(dest, '_onedrive', 'filed.json')
    done = json.load(open(logp)) if os.path.exists(logp) else {}
    index_library(lib)
    say(f'Filing {len(recs)} titles into {lib}...')
    for n, r in enumerate(recs, 1):
        key = str(r['zxdb'] or r['groups'][0])
        if done.get(key) in ('new', 'merged'):
            continue
        tid, st = file_title(r, dest, lib, link)
        done[key] = st
        if n % 100 == 0 or n == len(recs):
            write_json(logp, done)
            say(f'  {n}/{len(recs)} titles')
    write_json(logp, done)
    c = {s: sum(1 for v in done.values() if v == s) for s in ('new', 'merged', 'missing', 'unsupported')}
    say(f'Done: {c["new"]} new titles, {c["merged"]} merged into existing ones, {c["missing"]} with files missing, '
        f'{c["unsupported"]} with no file the player can load.')
    if failed:
        sys.exit(3)  # 3, not 1, so a caller can tell failed downloads from a bad link

if __name__ == '__main__':
    main()
