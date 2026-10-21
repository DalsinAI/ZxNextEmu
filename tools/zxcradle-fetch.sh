#!/usr/bin/env bash
# zxcradle-fetch.sh - download the ZX Cradle software library to a local folder.
#
#   curl -fsSL https://raw.githubusercontent.com/DalsinAI/ZxNextEmu/main/tools/zxcradle-fetch.sh | bash
#   bash zxcradle-fetch.sh [TARGET] [SECTIONS]
#
# TARGET   defaults to /run/media/da1ek/WorkingSpace/zx-cradle/library
# SECTIONS comma list, default all:
#          spectrum  16K/48K/128K/+2/+3 (zxart.ee, includes ZXDB ids)
#          next      Spectrum Next (zxart.ee)
#          clones    Pentagon, Scorpion, ATM Turbo (zxart.ee)
#          sam       SAM Coupe (zxart.ee + worldofsam.org)
#          mb03      MB03+ hardware: GS, TurboSound, SAA, ULAplus, Covox, DivMMC, ZX-Uno (zxart.ee + MB03+ site)
#          vtrd      Russian TR-DOS games (vtrd.in)
#          msx       MSX1/MSX2 games and demos (file-hunter.com)
#          symbos    SymbOS and its apps for Next and MSX2 (symbos.de)
#          spec256   256-colour Spectrum games (GitHub Spec256-Games)
#
# Each title becomes TARGET/titles/<id>/ with title.json (format 2), about.md,
# files/ and screens/ (see docs/cradle/titles.md). Run it again at any time:
# finished titles are skipped, so a stopped run carries on where it left off.
# A log of failures goes to TARGET/fetch.log.
#
# Environment: LIMIT=N (titles per section, for a trial run), WORKERS=4,
# MAX_RELEASES=3 (zxart.ee releases kept per title).
#
# Needs bash, curl and python3 (standard library only). 7z or unar, if
# installed, also unpack .rar/.7z/.lzh archives; without them those stay packed.
set -euo pipefail

TARGET="${1:-/run/media/da1ek/WorkingSpace/zx-cradle/library}"
SECTIONS="${2:-all}"

for tool in curl python3; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing $tool. Install it first (Fedora: sudo dnf install $tool, Debian/Ubuntu: sudo apt install $tool)." >&2
    exit 1
  fi
done

case "$TARGET" in
  /run/media/*/*/*)
    drive="$(echo "$TARGET" | cut -d/ -f1-5)"
    if [ ! -d "$drive" ]; then
      echo "$drive is not there. Is the drive mounted? Open it in the file manager once, then run this again." >&2
      exit 1
    fi ;;
esac
mkdir -p "$TARGET/titles"
echo "Library: $TARGET"
echo "Sections: $SECTIONS"
echo "Stop with Ctrl-C at any time; run the same command again to carry on."
echo

PY="$(mktemp -t zxcradle-fetch.XXXXXX.py)"
trap 'rm -f "$PY"' EXIT
cat > "$PY" <<'PYEOF'
import sys, os, re, json, html, time, zipfile, shutil, subprocess, threading, datetime, urllib.request, urllib.parse, urllib.error, http.cookiejar
from concurrent.futures import ThreadPoolExecutor

LIB = sys.argv[1]
TITLES = os.path.join(LIB, 'titles')
SECTIONS = sys.argv[2].split(',') if sys.argv[2] != 'all' else ['spectrum', 'next', 'clones', 'sam', 'mb03', 'vtrd', 'msx', 'symbos', 'spec256']
LIMIT = int(os.environ.get('LIMIT') or 0)
WORKERS = int(os.environ.get('WORKERS') or 4)
MAX_REL = int(os.environ.get('MAX_RELEASES') or 3)
TODAY = datetime.date.today().isoformat()
UA = 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36'
LOCK = threading.Lock()
LOG = open(os.path.join(LIB, 'fetch.log'), 'a', encoding='utf-8')

FAM48 = ['zx48', 'zx128', 'plus2', 'plus2a', 'plus3', 'pentagon', 'next']
FAM128 = ['zx128', 'plus2', 'plus2a', 'plus3', 'pentagon', 'next']
BETA = ['pentagon', 'scorpion', 'atm2']
RUN_EXT = {'.nex': 'sd0', '.snx': 'sd0', '.tap': 'tape', '.tzx': 'tape', '.pzx': 'tape', '.csw': 'tape',
           '.sna': 'snapshot', '.z80': 'snapshot', '.szx': 'snapshot', '.slt': 'snapshot',
           '.trd': 'beta.a', '.scl': 'beta.a', '.fdi': 'beta.a', '.dsk': 'disk.a', '.mgt': 'disk.1', '.sad': 'disk.1',
           '.mdr': 'md.1', '.dck': 'cart', '.rom': 'cart', '.img': 'sd0'}
ARCHIVES = ('.zip', '.rar', '.7z', '.lzh', '.lha', '.arj')


def say(*a):
    with LOCK:
        print(*a, flush=True)


def log(*a):
    with LOCK:
        LOG.write(datetime.datetime.now().isoformat(timespec='seconds') + ' ' + ' '.join(map(str, a)) + '\n')
        LOG.flush()


# ---------------------------------------------------------------- network

def fixurl(u):
    return urllib.parse.quote(u, safe=":/?=&%;#,+@!$'()*~[]")


def fetch(url, data=None, headers=None, tries=5, timeout=180, opener=None):
    h = {'User-Agent': UA}
    h.update(headers or {})
    err = None
    for i in range(tries):
        try:
            req = urllib.request.Request(fixurl(url), data=data, headers=h)
            with (opener.open if opener else urllib.request.urlopen)(req, timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            err = e
            if e.code in (400, 401, 403, 404, 410):
                break
        except Exception as e:
            err = e
        time.sleep(2 * (i + 1))
    raise RuntimeError(f'{url}: {err}')


def fetch_json(url):
    for i in range(4):
        b = fetch(url)
        try:
            return json.loads(b)
        except ValueError:
            time.sleep(5 * (i + 1))
    raise RuntimeError(f'{url}: not JSON')


def save(url, path):
    """Download url to path unless it is already there. Returns True when the file exists afterwards."""
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return True
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        data = fetch(url)
    except Exception as e:
        log('download failed', e)
        return False
    tmp = path + '.part'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, path)
    return True


# ---------------------------------------------------------------- names

CYR = dict(zip('абвгдеёжзийклмнопрстуфхцчшщъыьэюяєіїґ',
               ['a', 'b', 'v', 'g', 'd', 'e', 'e', 'zh', 'z', 'i', 'y', 'k', 'l', 'm', 'n', 'o', 'p', 'r', 's', 't', 'u', 'f',
                'kh', 'ts', 'ch', 'sh', 'shch', '', 'y', '', 'e', 'yu', 'ya', 'ye', 'i', 'yi', 'g']))


def slug(s):
    s = ''.join(CYR.get(c, c) for c in html.unescape(s or '').lower())
    return re.sub(r'[^a-z0-9]+', '-', s).strip('-')[:60].strip('-') or 'untitled'


def norm(s):
    return re.sub(r'[^a-z0-9]', '', ''.join(CYR.get(c, c) for c in html.unescape(s or '').lower()))


def safe_name(n):
    n = os.path.basename(n.replace('\\', '/')) or 'file'
    return re.sub(r'[\x00-\x1f/:*?"<>|]', '_', n).strip() or 'file'


# ---------------------------------------------------------------- library

class Library:
    def __init__(self):
        self.ids, self.names, self.used = {}, {}, set()
        for t in os.listdir(TITLES):
            self.used.add(t)
            try:
                j = json.load(open(os.path.join(TITLES, t, 'title.json'), encoding='utf-8'))
            except Exception:
                continue
            for k, v in (j.get('ids') or {}).items():
                self.ids[(k, str(v))] = t
            self.names.setdefault(norm(j.get('name')), t)

    def has(self, key, val):
        return (key, str(val)) in self.ids

    def has_name(self, name):
        return norm(name) in self.names

    def claim(self, name, suffix='', key=None, val=None):
        """Pick a folder id for a new title. Unfinished folders from an earlier run with the same id are reused."""
        with LOCK:
            base = slug(name) + suffix
            mark = f'{key}:{val}' if key else None
            sid, n = base, 2
            while sid in self.used:
                d = os.path.join(TITLES, sid)
                if mark and not os.path.exists(os.path.join(d, 'title.json')):
                    try:
                        if open(os.path.join(d, '.claim')).read() == mark:
                            return sid
                    except Exception:
                        pass
                sid = f'{base}-{n}'
                n += 1
            self.used.add(sid)
            os.makedirs(os.path.join(TITLES, sid), exist_ok=True)
            if mark:
                open(os.path.join(TITLES, sid, '.claim'), 'w').write(mark)
            return sid

    def finish(self, sid, t, about=None):
        d = os.path.join(TITLES, sid)
        t['id'] = sid
        t.setdefault('format', 2)
        t.setdefault('added', TODAY)
        t.setdefault('compat', {})
        if about:
            with open(os.path.join(d, 'about.md'), 'w', encoding='utf-8') as f:
                f.write(about.strip() + '\n')
        tmp = os.path.join(d, 'title.json.part')
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(t, f, indent=2, ensure_ascii=False)
        os.replace(tmp, os.path.join(d, 'title.json'))
        try:
            os.remove(os.path.join(d, '.claim'))
        except OSError:
            pass
        with LOCK:
            for k, v in (t.get('ids') or {}).items():
                self.ids[(k, str(v))] = sid
            self.names.setdefault(norm(t.get('name')), sid)


def unpack(path, dest, charset=None):
    """Unpack an archive into dest. Returns True on success."""
    low = path.lower()
    try:
        if zipfile.is_zipfile(path) and not zip_unpack(path, dest, charset):
            shutil.rmtree(dest, ignore_errors=True)
        elif zipfile.is_zipfile(path):
            return True
        if low.endswith(ARCHIVES):
            os.makedirs(dest, exist_ok=True)
            if shutil.which('7z'):
                if subprocess.run(['7z', 'x', '-y', '-o' + dest, path], capture_output=True).returncode == 0:
                    return True
            if shutil.which('unar'):
                if subprocess.run(['unar', '-q', '-f', '-D', '-o', dest, path], capture_output=True).returncode == 0:
                    return True
            if low.endswith(('.lzh', '.lha')) and shutil.which('lha'):
                if subprocess.run(['lha', 'xqf', path], cwd=dest, capture_output=True).returncode == 0:
                    return True
            shutil.rmtree(dest, ignore_errors=True)
    except Exception as e:
        log('unpack failed', path, e)
        shutil.rmtree(dest, ignore_errors=True)
    return False


def zip_unpack(path, dest, charset):
    try:
        if True:
            with zipfile.ZipFile(path) as z:
                for m in z.infolist():
                    if m.is_dir():
                        continue
                    n = m.filename
                    if not m.flag_bits & 0x800:
                        try:
                            n = n.encode('cp437').decode('cp866' if charset == 'ru' else 'cp437')
                        except Exception:
                            pass
                    parts = [safe_name(p) for p in n.replace('\\', '/').split('/') if p not in ('', '.', '..')]
                    if not parts:
                        continue
                    out = os.path.join(dest, *parts)
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with z.open(m) as s, open(out, 'wb') as o:
                        shutil.copyfileobj(s, o)
        return True
    except Exception as e:
        log('unzip failed, trying 7z/unar', path, e)
        return False


def get_files(tdir, url, name, charset=None, sub=None):
    """Download one file into tdir/files/ and unpack it if it is an archive.
    Returns paths relative to tdir of everything it gave."""
    fdir = os.path.join(tdir, 'files')
    name = safe_name(name or url.rsplit('/', 1)[-1].split('?')[0])
    stem = sub or re.sub(r'(\.[a-z0-9]{2,4})+$', '', name, flags=re.I) or name
    dest = os.path.join(fdir, safe_name(stem))
    if os.path.isdir(dest) and os.listdir(dest):
        pass
    else:
        arc = os.path.join(fdir, name)
        if not save(url, arc):
            return []
        if name.lower().endswith(ARCHIVES) and unpack(arc, dest + '.tmp', charset):
            if os.path.isdir(dest):
                shutil.rmtree(dest)
            os.replace(dest + '.tmp', dest)
            os.remove(arc)
        else:
            os.makedirs(dest, exist_ok=True)
            os.replace(arc, os.path.join(dest, name))
    out = []
    for root, _, fs in os.walk(dest):
        for f in sorted(fs):
            out.append(os.path.relpath(os.path.join(root, f), tdir))
    return sorted(out)


def get_screens(tdir, urls, captions=None, limit=3):
    out = []
    for u in urls:
        if len(out) >= limit:
            break
        ext = os.path.splitext(u.split('?')[0].split(';')[0])[1].lower()
        if 'zximages' in u or ext not in ('.png', '.gif', '.jpg', '.jpeg'):
            ext = '.png'
        rel = f'screens/{len(out) + 1}{ext}'
        if save(u, os.path.join(tdir, rel)):
            out.append({'file': rel, 'caption': (captions or {}).get(u, 'Screenshot')})
    return out


def run_table(f, machines, platform='zx-spectrum', needs=(), likes=(), notes=None):
    ext = os.path.splitext(f)[1].lower()
    slot = RUN_EXT.get(ext)
    if not slot:
        return None
    m, nd = list(machines), list(needs)
    if ext in ('.trd', '.scl', '.fdi'):
        nd.append('beta')
        if not set(m) & set(BETA):
            m += BETA
    if ext == '.dsk' and platform == 'zx-spectrum':
        m = [x for x in m if x in ('plus3', 'plus2a')] or ['plus3']
    if ext == '.mdr':
        nd.append('interface1')
    if platform == 'sam-coupe' and ext in ('.dsk', '.mgt', '.sad'):
        slot = 'disk.1'
    r = {'platform': platform, 'machines': list(dict.fromkeys(m)), 'files': [f], 'slot': slot, 'needs': sorted(set(nd))}
    if likes:
        r['likes'] = sorted(set(likes))
    if notes:
        r['notes'] = notes
    return r


def group_runs(runs):
    """Merge runs of one release that share a slot and machines (tape sides, disk sets) into one run."""
    out = {}
    for r in runs:
        if not r:
            continue
        k = (os.path.splitext(r['files'][0])[1].lower(), r['slot'], tuple(r['machines']), tuple(r['needs']))
        if k in out:
            out[k]['files'] += r['files']
        else:
            out[k] = r
    return list(out.values())


def batch(items, fn, label, total=None):
    """Run fn over items with a few workers, printing progress."""
    total = total or len(items)
    done = [0]

    def one(it):
        try:
            msg = fn(it)
        except Exception as e:
            msg = None
            log(label, 'failed', e)
        with LOCK:
            done[0] += 1
            if msg:
                print(f'[{label} {done[0]}/{total}] {msg}', flush=True)
    with ThreadPoolExecutor(WORKERS) as ex:
        list(ex.map(one, items))


# ---------------------------------------------------------------- zxart.ee
# Classic Spectrum, Next, Russian clones, SAM Coupé and MB03+ hardware all come
# from the zxart.ee API, one title per zxart prod, its machines from the
# hardware each release lists.

ZX = 'https://zxart.ee/api/language:eng'
ZX_HW = {
    'spectrum': ['zx16', 'zx48', 'zx128'],
    'next': ['zxnext'],
    'clones': ['pentagon128', 'scorpion', 'atm', 'atm2'],
    'sam': ['samcoupe'],
    'mb03': ['gs', 'tsfm', 'saa', 'ulaplus', 'soundrive', 'specdrum', 'divmmc', 'divide', 'zxuno'],
}
HW_MACH = {'zx16': ['zx16'] + FAM48, 'zx48': FAM48, 'zx128': FAM128, 'zx+2': FAM128, 'zx+2a': ['plus2a', 'plus3'],
           'zx+3': ['plus3', 'plus2a'], 'zx+2b': ['plus2a', 'plus3'], 'pentagon128': BETA, 'pentagon512': ['pentagon'],
           'pentagon1024': ['pentagon'], 'scorpion': ['scorpion'], 'atm': ['atm2'], 'atm2': ['atm2'], 'zxnext': ['next'],
           'samcoupe': ['sam']}
HW_NEEDS = {'gs': 'gs', 'tsfm': 'turbosound', 'saa': 'mb03', 'ulaplus': 'ulaplus', 'soundrive': 'covox',
            'specdrum': 'covox', 'covox': 'covox', 'divmmc': 'divmmc', 'divide': 'divmmc', 'zxuno': 'mb03'}
HW_LIKES = {'ay': 'ay', 'kempston': 'kempston'}
LIC = {'allowed': 'freeware', 'donationware': 'freeware', 'insales': 'commercial'}
NAMES = {}


def kind_of(cat):
    top, _, rest = html.unescape(cat or '').partition('/')
    if top == 'Demoscene':
        return 'music' if 'Music' in rest else 'demo'
    if top == 'System Software':
        return 'system' if 'Operating' in rest else ('music' if rest.startswith('Sound') else 'utility')
    if top == 'Applications':
        return 'education' if 'Educational' in rest else 'application'
    return 'game'


def zx_names(prods):
    want = sorted({str(a['authorId']) for p in prods for a in p.get('authorsInfo', []) if a.get('authorId')} |
                  {str(i) for p in prods for i in p.get('publishersIds', [])})
    want = [i for i in want if i not in NAMES]
    for i in range(0, len(want), 50):
        ch = ','.join(want[i:i + 50])
        for typ, f in (('author', 'authorId'), ('group', 'groupId')):
            try:
                j = fetch_json(f'{ZX}/types:{typ}/export:{typ}/start:0/limit:50/filter:{f}={ch};')
                for a in j['responseData'].get(typ, []):
                    NAMES[str(a['id'])] = html.unescape(a['title'])
            except Exception as e:
                log('zxart names', e)


def zx_title(lib, p, rels, hw):
    rels = [r for r in rels if r.get('file') and r.get('fileName')]
    if not rels:
        return None
    rels.sort(key=lambda r: (r.get('releaseType') != 'original', -(r.get('year') or 0)))
    rels = rels[:MAX_REL]
    name = html.unescape(p['title']).strip()
    charset = 'ru' if set(p.get('language') or []) & {'ru', 'ua', 'by'} or hw in ZX_HW['clones'] else None
    sam = hw == 'samcoupe'
    sid = lib.claim(name, '-sam' if sam else '', 'zxart', p['id'])
    tdir = os.path.join(TITLES, sid)
    runs, nfiles = [], 0
    for r in rels:
        hwr = r.get('hardwareRequired') or []
        mach = [m for h in hwr for m in HW_MACH.get(h, [])] or HW_MACH.get(hw) or FAM48
        needs = [HW_NEEDS[h] for h in hwr if h in HW_NEEDS] or ([HW_NEEDS[hw]] if hw in HW_NEEDS and not hwr else [])
        likes = [HW_LIKES[h] for h in hwr if h in HW_LIKES]
        files = get_files(tdir, r['file'], r['fileName'], charset, sub=f"{r['id']}-{safe_name(r['fileName']).split('.')[0]}")
        nfiles += len(files)
        runs += group_runs([run_table(f, mach, 'sam-coupe' if sam or 'sam' in mach else 'zx-spectrum', needs, likes,
                                      ' '.join(x for x in (r.get('releaseType'), r.get('version')) if x) or None)
                            for f in files])
    if not nfiles:
        return None
    if not runs:
        runs = [{'platform': 'sam-coupe' if sam else 'zx-spectrum', 'machines': HW_MACH.get(hw) or FAM48,
                 'files': [], 'slot': 'sd0', 'needs': [], 'notes': 'No runnable file found; see files/.'}]
    url = f'https://zxart.ee/eng/software/?id={p["id"]}'
    desc = html.unescape(p.get('description') or '').strip()
    cat = html.unescape(p.get('categoriesString') or '')
    ids = {'zxart': p['id']}
    zxdb = (p.get('importIds') or {}).get('zxdb')
    if zxdb and str(zxdb).isdigit():
        ids['zxdb'] = int(zxdb)
    pubs = [NAMES[str(i)] for i in p.get('publishersIds', []) if str(i) in NAMES]
    t = {'format': 2, 'name': name, 'kind': kind_of(cat), 'year': p.get('year') or None,
         'authors': list(dict.fromkeys(NAMES[str(a['authorId'])] for a in p.get('authorsInfo', []) if str(a.get('authorId')) in NAMES)),
         'publisher': pubs[0] if pubs else None, 'genres': [g for g in cat.split('/')[1:] if g][-2:],
         'tags': [hw], 'summary': desc.split('\n')[0][:200] or None,
         'license': LIC.get(p.get('legalStatus'), 'unknown'), 'source': url,
         'sources': [url] + ([f'https://spectrumcomputing.co.uk/entry/{ids["zxdb"]}'] if 'zxdb' in ids else []),
         'ids': ids, 'runs': runs, 'compat': {},
         'screens': get_screens(tdir, p.get('imagesUrls') or [])}
    t = {k: v for k, v in t.items() if v not in (None, [], '')} | {'runs': runs}
    lib.finish(sid, t, f'# {name}\n\n{desc}\n\nFrom {url}' if desc else None)
    return f'{name} ({nfiles} files)'


def zxart(lib, section):
    for hw in ZX_HW[section]:
        start, made, total = 0, 0, None
        while True:
            try:
                j = fetch_json(f'{ZX}/types:zxProd/export:zxProd/start:{start}/limit:100/order:id,asc/filter:zxProdHardware={hw};')
            except Exception as e:
                say(f'  zxart.ee {hw}: {e}; skipping the rest of {hw}')
                log('zxart', hw, start, e)
                break
            total = j.get('totalAmount') or 0
            prods = j['responseData'].get('zxProd') or []
            if start == 0:
                say(f'zxart.ee {hw}: {total} titles')
            if not prods:
                break
            start += len(prods)
            todo = [p for p in prods if not lib.has('zxart', p['id'])
                    and not html.unescape(p.get('categoriesString') or '').startswith('Press')]
            if LIMIT:
                todo = todo[:max(0, LIMIT - made)]
            if todo:
                ids = [r for p in todo for r in p.get('releasesIds', [])]
                rels = {}
                for i in range(0, len(ids), 50):
                    try:
                        jr = fetch_json(f'{ZX}/types:zxRelease/export:zxRelease/start:0/limit:50/filter:zxReleaseId={",".join(map(str, ids[i:i + 50]))};')
                        for r in jr['responseData'].get('zxRelease') or []:
                            rels[r['id']] = r
                    except Exception as e:
                        log('zxart releases', e)
                zx_names(todo)
                batch(todo, lambda p: zx_title(lib, p, [rels[i] for i in p.get('releasesIds', []) if i in rels], hw),
                      f'{hw} {start}/{total}', len(todo))
                made += len(todo)
            if (LIMIT and made >= LIMIT) or start >= total:
                break


# ---------------------------------------------------------------- MB03+ site
# The MB03+ makers' own demos and tools, from the public Google Drive folder
# linked on https://sites.google.com/view/mb03plus/download

MB03_SITE = 'https://sites.google.com/view/mb03plus/download'
MB03_DRIVE = '1-o9CFmn3SnAc2jw3DGmseeYNntF36hNO'
MB03_RECS = [
    ('LMN Play', 'demo', 2020, 'LMN_PLAY.ZIP', 'High-quality 44.1 kHz stereo music on a 3.5 MHz Spectrum, the MB03+ demo by LMN128.'),
    ('TurboSound FM player and instrument editor', 'utility', 2020, 'TSFM.ZIP', 'TSFM player and instrument editor for the MB03+ (two YM2203).'),
    ('TurboSound FM v2', 'utility', 2020, 'TSFM_v2.tap', 'TurboSound FM software, version 2.'),
    ('GigaScreen pictures', 'demo', 2020, 'GigaScreensPictures.tap', 'GigaScreen picture show for the MB03+ flicker-free GigaScreen mode.'),
    ('HiResColour example', 'demo', 2021, 'HiResColor.tap', 'Example of the MB03+ HiResColour mode (512x192 with attributes).'),
    ('KeyLayer', 'demo', 2020, 'KeyLayer.tap', 'MB03+ KeyLayer / GigaScreen mode 3 example.'),
    ('MB03+ Wi-Fi tools', 'utility', 2019, 'Wifi.zip', 'ESP8266 Wi-Fi utilities for the MB03+.'),
    ('ePlay 1.51', 'utility', 2021, 'eplay151.zip', 'Music player for the MB03+.'),
]


def mb03_site(lib):
    try:
        page = fetch(f'https://drive.google.com/embeddedfolderview?id={MB03_DRIVE}').decode('utf-8', 'replace')
    except Exception as e:
        say(f'  MB03+ site: {e}')
        return
    ids = {html.unescape(n): i for i, n in re.findall(r'/file/d/([^/"]+).*?flip-entry-title">([^<]+)', page, re.S)}
    say(f'MB03+ site: {len(MB03_RECS)} titles')
    for name, kind, year, fn, summary in MB03_RECS:
        if lib.has('mb03site', fn) or fn not in ids:
            continue
        sid = lib.claim(name, '-mb03', 'mb03site', fn)
        tdir = os.path.join(TITLES, sid)
        files = get_files(tdir, f'https://drive.usercontent.google.com/download?id={ids[fn]}&export=download&confirm=t', fn)
        if not files:
            continue
        runs = group_runs([run_table(f, FAM128, needs=['mb03']) for f in files]) or \
            [{'platform': 'zx-spectrum', 'machines': FAM128, 'files': files, 'slot': 'sd0', 'needs': ['mb03']}]
        lib.finish(sid, {'name': name, 'kind': kind, 'year': year, 'tags': ['mb03+'], 'summary': summary,
                         'license': 'freeware', 'source': MB03_SITE, 'sources': [MB03_SITE], 'ids': {'mb03site': fn}, 'runs': runs})
        say(f'[mb03 site] {name} ({len(files)} files)')


# ---------------------------------------------------------------- World of SAM

WOS = 'https://www.worldofsam.org'


def wos_field(p, label):
    m = re.search(r'field__label">\s*' + label + r'\s*</div>(.*?)</div>\s*</div>', p, re.S)
    return [html.unescape(x).strip() for x in re.findall(r'>([^<>]+)<', m.group(1)) if x.strip()] if m else []


def worldofsam(lib):
    links = {}
    for cat, kind in ((3, 'game'), (4, 'demo')):
        page = 0
        while True:
            try:
                p = fetch(f'{WOS}/products?field_category_target_id={cat}&page={page}').decode('utf-8', 'replace')
            except Exception as e:
                say(f'  worldofsam.org: {e}')
                break
            new = [h for h in re.findall(r'<td[^>]*>\s*<a href="(/products/[^"?#]+)"', p) if h not in links]
            for h in new:
                links[h] = kind
            if not new or f'page={page + 1}"' not in p:
                break
            page += 1
    say(f'worldofsam.org: {len(links)} titles')
    todo = [x for x in links.items() if not lib.has('worldofsam', x[0])]
    if LIMIT:
        todo = todo[:LIMIT]

    def one(item):
        h, kind = item
        u = WOS + h
        p = fetch(u, timeout=60).decode('utf-8', 'replace')
        name = html.unescape(re.search(r'<h1[^>]*>(?:\s*<span[^>]*>)?\s*([^<]+)', p).group(1)).strip()
        urls = sorted(set(re.findall(r'href="(https://www\.worldofsam\.org/system/files/[^"]+)"', p)))
        if not urls or slug(name) + '-sam' in lib.used:
            return None
        sid = lib.claim(name, '-sam', 'worldofsam', h)
        tdir = os.path.join(TITLES, sid)
        runs, n = [], 0
        for f in urls:
            got = get_files(tdir, f, urllib.parse.unquote(f.rsplit('/', 1)[-1]))
            n += len(got)
            runs += group_runs([run_table(x, ['sam'], 'sam-coupe') for x in got])
        if not n:
            return None
        imgs = list(dict.fromkeys(re.findall(r'href="(https://www\.worldofsam\.org/sites/default/files/[^"]+\.(?:png|jpg|gif))"', p, re.I)))
        yr = wos_field(p, 'Release Year')
        desc = re.sub(r'\s+', ' ', ' '.join(wos_field(p, 'Description'))).strip()
        tt = {'name': name, 'kind': kind, 'year': int(yr[0]) if yr and yr[0].isdigit() else None,
              'authors': wos_field(p, 'Author'), 'publisher': (wos_field(p, 'Publisher') or [None])[0],
              'tags': ['sam coupe'], 'summary': desc[:200] or None, 'license': 'unknown', 'source': u, 'sources': [u],
              'ids': {'worldofsam': h}, 'runs': runs, 'screens': get_screens(tdir, imgs)}
        if not runs:
            tt['runs'] = [{'platform': 'sam-coupe', 'machines': ['sam'], 'files': [], 'slot': 'disk.1', 'needs': [],
                           'notes': 'No runnable file found; see files/.'}]
        lib.finish(sid, {k: v for k, v in tt.items() if v not in (None, [])} | {'runs': tt['runs']},
                   f'# {name}\n\n{desc}\n\nFrom {u}' if desc else None)
        return f'{name} ({n} files)'
    batch(todo, one, 'worldofsam')


# ---------------------------------------------------------------- vtrd.in
# Russian disk games (TR-DOS) for Pentagon, Scorpion and ATM Turbo.

def vtrd(lib):
    rows = []
    for t in ('full_ver', 'translat', 'remix'):
        try:
            page = fetch(f'https://vtrd.in/games.php?t={t}').decode('utf-8', 'replace')
        except Exception as e:
            say(f'  vtrd.in {t}: {e}')
            continue
        for m in re.finditer(r'<tr[^>]*><td nowrap><a href="(/[^"]+)">(.*?)</a></td><td>(.*?)</td><td>(.*?)</td>'
                             r'<td>[^<]*</td><td>(?:<a[^>]*href="(/release\.php\?r=[0-9a-f]+)")?', page):
            href, name, auth, grp, rel = m.groups()
            rows.append((t, href, html.unescape(re.sub('<[^>]+>', '', name)).replace('\xa0', ' ').strip(),
                         html.unescape(auth).strip(), html.unescape(grp).strip(), rel))
    say(f'vtrd.in: {len(rows)} titles')
    todo = [r for r in rows if not lib.has('vtrd', r[1])]
    if LIMIT:
        todo = todo[:LIMIT]

    def one(row):
        t, href, name, auth, grp, rel = row
        base = re.sub(r'\s*\(.*\)$', '', name) or name
        if t == 'full_ver' and lib.has_name(base):
            return None
        sid = lib.claim(name, '-pentagon', 'vtrd', href)
        tdir = os.path.join(TITLES, sid)
        files = get_files(tdir, 'https://vtrd.in' + href, href.rsplit('/', 1)[-1], 'ru')
        if not files:
            return None
        needs, shots, src = [], [], 'https://vtrd.in' + (rel or f'/games.php?t={t}')
        if rel:
            try:
                rp = fetch('https://vtrd.in' + rel).decode('utf-8', 'replace')
                if re.search(r'General sound:\s*(<[^>]+>\s*)*Present', rp):
                    needs.append('gs')
                shots = re.findall(r'<meta property="og:image" content="([^"]+)"', rp)
            except Exception as e:
                log('vtrd release', e)
        am = re.match(r"(.*?)'(\d\d)$", auth)
        year = None
        if am:
            y = int(am.group(2))
            year, auth = (1900 + y if y > 50 else 2000 + y), am.group(1)
        runs = group_runs([run_table(f, BETA, needs=needs) for f in files])
        tt = {'name': name, 'kind': 'game', 'year': year, 'authors': [auth] if auth else [],
              'publisher': grp if grp and grp != 'author' else None,
              'tags': ['tr-dos'] + (['translation'] if t == 'translat' else ['remix'] if t == 'remix' else []),
              'license': 'unknown', 'source': src, 'sources': [src], 'ids': {'vtrd': href},
              'runs': runs, 'screens': get_screens(tdir, shots, None, 1)}
        lib.finish(sid, {k: v for k, v in tt.items() if v not in (None, [])} | {'runs': runs})
        return f'{name} ({len(files)} files)'
    batch(todo, one, 'vtrd')


# ---------------------------------------------------------------- MSX (file-hunter.com)

FH = 'https://download.file-hunter.com/'
FH_DIRS = {'Games/MSX1/ROM': ('cart1', []), 'Games/MSX1/DSK': ('disk0', ['msx-disk']), 'Games/MSX1/CAS': ('tape', ['msx-tape']),
           'Games/MSX1/MSX-Binaries': ('disk0', ['msx-disk']), 'Games/MSX1/HDD': ('ide0', ['nextor']),
           'Games/MSX2/ROM': ('cart1', []), 'Games/MSX2/DSK': ('disk0', ['msx-disk']), 'Games/MSX2/CAS': ('tape', ['msx-tape']),
           'Games/MSX2/MSX-Binaries': ('disk0', ['msx-disk']), 'Games/MSX2/Harddisk': ('ide0', ['nextor']),
           'Demos': ('disk0', ['msx-disk'])}


def msx_name(fn):
    n = re.sub(r'\.[a-z0-9]{2,4}$', '', fn, flags=re.I)
    n = re.sub(r'\s*[\(\[].*$', '', n).strip()
    m = re.match(r'(.*), (The|A|An)$', n)
    return f'{m.group(2)} {m.group(1)}' if m else n


def msx(lib):
    try:
        listing = fetch(FH + 'allfiles.txt').decode('utf-8-sig', 'replace')
    except Exception as e:
        say(f'  file-hunter.com: {e}')
        return
    groups = {}
    for line in listing.splitlines():
        p = line.strip().replace('\\', '/')
        d = next((k for k in FH_DIRS if p.startswith(k + '/')), None)
        if not d or '[b]' in p or '/WAV' in p.upper() or 'MSX2+' in p or 'Turbo' in p:
            continue
        fn = p.rsplit('/', 1)[-1]
        demo = d == 'Demos'
        gen = 'msx2' if 'MSX2' in p else 'msx1'
        name = msx_name(fn)
        key = ('msx-demo-' if demo else 'msx-') + slug(name)
        g = groups.setdefault(key, {'name': name, 'demo': demo, 'gen': set(), 'files': []})
        g['gen'].add(gen)
        g['files'].append((d, p))
    say(f'file-hunter.com MSX: {len(groups)} titles')
    todo = [(k, g) for k, g in groups.items() if not lib.has('filehunter', k)]
    if LIMIT:
        todo = todo[:LIMIT]
    order = list(FH_DIRS)

    def one(kg):
        key, g = kg
        sid = lib.claim(key, '', 'filehunter', key)
        tdir = os.path.join(TITLES, sid)
        mach = ['msx1', 'msx1eu', 'msx2', 'msx2eu'] if 'msx1' in g['gen'] else ['msx2', 'msx2eu']
        runs, n = [], 0
        for d, p in sorted(g['files'], key=lambda x: order.index(x[0])):
            slot, needs = FH_DIRS[d]
            files = get_files(tdir, FH + p, p.rsplit('/', 1)[-1])
            n += len(files)
            keep = [f for f in files if not f.lower().endswith(('.txt', '.nfo', '.png', '.jpg', '.pdf', '.diz'))]
            if keep:
                if slot == 'disk0':
                    keep = [f for f in keep if f.lower().endswith(('.dsk', '.di1', '.di2'))] or keep
                runs.append({'platform': 'msx', 'machines': mach, 'files': keep, 'slot': slot, 'needs': needs,
                             'notes': d.split('/')[-1]})
        if not n:
            return None
        src = FH + g['files'][0][1]
        tt = {'name': g['name'], 'kind': 'demo' if g['demo'] else 'game', 'tags': ['msx'], 'license': 'unknown',
              'source': 'https://www.file-hunter.com/', 'sources': [src], 'ids': {'filehunter': key}, 'runs': runs}
        lib.finish(sid, tt)
        return f'{g["name"]} ({n} files)'
    batch(todo, one, 'msx')


# ---------------------------------------------------------------- SymbOS (symbos.de)

SY = 'http://www.symbos.de/'


def js_rows(src):
    tok = re.compile(r"'((?:[^'\\]|\\.)*)'|(-?\d+)")
    for line in src.splitlines():
        line = line.strip()
        if line.startswith("['"):
            yield [m.group(1).replace("\\'", "'") if m.group(1) is not None else m.group(2) for m in tok.finditer(line)]


def symbos(lib):
    try:
        src = fetch(SY + 'apps.js').decode('latin-1')
    except Exception as e:
        say(f'  symbos.de: {e}')
        return
    apps = [r for r in js_rows(src.split('garrApps', 1)[-1]) if len(r) >= 17]
    say(f'symbos.de: {len(apps)} apps + SymbOS itself')
    if not lib.has('symbos', 'os'):
        sid = lib.claim('SymbOS', '', 'symbos', 'os')
        tdir = os.path.join(TITLES, sid)
        got = {}
        for f in ('SymbOS-MSX40.zip', 'SymbOS-ZNX40.zip', 'SymbOS-40-MassStorageCompletion.zip', 'SymbOS-40-Manual.pdf'):
            got[f] = get_files(tdir, SY + 'download/20250131-V40/' + f, f)
        if any(got.values()):
            runs = [{'platform': 'zx-spectrum', 'machines': ['next'], 'files': got['SymbOS-ZNX40.zip'], 'slot': 'sd0', 'needs': []},
                    {'platform': 'msx', 'machines': ['msx2', 'msx2eu'], 'files': got['SymbOS-MSX40.zip'], 'slot': 'ide0', 'needs': []}]
            lib.finish(sid, {'name': 'SymbOS', 'kind': 'system', 'year': 2025, 'authors': ['Prodatron'], 'tags': ['symbos'],
                             'summary': 'Multitasking windowed operating system for Z80 machines.', 'license': 'freeware',
                             'source': SY, 'sources': [SY], 'ids': {'symbos': 'os'}, 'runs': runs})
            say('[symbos] SymbOS 4.0')
    todo = [a for a in apps if not lib.has('symbos', a[0]) and (a[7] == '0' or set(a[7]) & {'2', '7'})]
    if LIMIT:
        todo = todo[:LIMIT]

    def one(a):
        aid, name, ver, date, plats = a[0], a[1], a[2], a[3], a[7]
        zipn = re.sub(r'[ \'"()/\\]', '', name) + '.zip'
        sid = lib.claim(name, '-symbos', 'symbos', aid)
        tdir = os.path.join(TITLES, sid)
        files = get_files(tdir, SY + 'download/apps/' + zipn, zipn)
        if not files:
            return None
        mach = []
        if plats == '0' or '7' in plats:
            mach.append(('zx-spectrum', ['next'], 'sd0'))
        if plats == '0' or '2' in plats:
            mach.append(('msx', ['msx2', 'msx2eu'], 'ide0'))
        runs = [{'platform': pl, 'machines': m, 'files': files, 'slot': sl, 'needs': ['symbos']} for pl, m, sl in mach]
        shots = [f'{SY}gfx/shots/{a[13]}{i}.gif' for i in range(1, min(3, int(a[12] or 0)) + 1)] if a[13] else []
        ym = re.search(r'(\d{4})$', date)
        tt = {'name': name, 'kind': 'application', 'year': int(ym.group(1)) if ym else None, 'authors': [a[5]] if a[5] else [],
              'tags': ['symbos'], 'summary': a[15] or None, 'license': 'freeware', 'source': SY, 'sources': [SY + 'apps.htm'],
              'ids': {'symbos': aid}, 'runs': runs, 'screens': get_screens(tdir, shots)}
        lib.finish(sid, {k: v for k, v in tt.items() if v not in (None, [])} | {'runs': runs},
                   f'# {name} {ver}\n\n{html.unescape(re.sub("<[^>]+>", " ", a[16]))}' if a[16] else None)
        return f'{name} ({len(files)} files)'
    batch(todo, one, 'symbos')


# ---------------------------------------------------------------- Spec256

SPEC256 = os.environ.get('SPEC256_URLS', 'https://codeload.github.com/helpquick/Spec256-Games/zip/HEAD '
                                          'https://codeload.github.com/mvvproject/Spec256-Games/zip/HEAD').split()


def spec256(lib):
    work = os.path.join(LIB, '.spec256')
    for n, u in enumerate(SPEC256):
        z = os.path.join(work, f'{n}.zip')
        if not save(u, z) or not unpack(z, os.path.join(work, str(n))):
            say(f'  Spec256: could not get {u}')
            continue
        root = os.path.join(work, str(n))
        tops = os.listdir(root)
        if len(tops) == 1 and os.path.isdir(os.path.join(root, tops[0])):
            root = os.path.join(root, tops[0])
        games = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)) and not d.startswith('.'))
        say(f'Spec256 {u.split("/")[3]}: {len(games)} titles')
        if LIMIT:
            games = games[:LIMIT]
        for g in games:
            key = slug(g)
            if lib.has('spec256', key):
                continue
            sid = lib.claim(g, '-spec256', 'spec256', key)
            tdir = os.path.join(TITLES, sid)
            src = os.path.join(root, g)
            files, shots, about = [], [], None
            for f in sorted(os.listdir(src)):
                p = os.path.join(src, f)
                if not os.path.isfile(p):
                    continue
                low = f.lower()
                if low.endswith(('.png', '.jpg', '.gif')) and len(shots) < 3:
                    rel = f'screens/{len(shots) + 1}{os.path.splitext(low)[1]}'
                    os.makedirs(os.path.join(tdir, 'screens'), exist_ok=True)
                    shutil.copy(p, os.path.join(tdir, rel))
                    shots.append({'file': rel, 'caption': 'Title screen, 256 colours' if 'title' in low else '256 colours'})
                    continue
                if low == 'readme.txt':
                    about = open(p, encoding='latin-1').read()
                os.makedirs(os.path.join(tdir, 'files'), exist_ok=True)
                shutil.copy(p, os.path.join(tdir, 'files', safe_name(f)))
                files.append('files/' + safe_name(f))
            game = [f for f in files if not f.lower().endswith('.txt')]
            if not game:
                continue
            runs = [{'platform': 'zx-spectrum', 'machines': ['spec256'], 'files': game, 'slot': 'gfx', 'needs': [],
                     'notes': '256 colours (Spec256). Load every file into the gfx slot, then the .sna into snapshot.'}]
            runs += [run_table(f, ['zx48', 'zx128', 'plus2', 'plus2a', 'plus3', 'pentagon'], notes='The original game in Spectrum colours.')
                     for f in game if f.lower().endswith('.sna')]
            name = re.sub(r'\s+', ' ', g).strip()
            lib.finish(sid, {'name': name, 'kind': 'game', 'tags': ['spec256', '256 colours'], 'license': 'unknown',
                             'source': u.replace('codeload.github.com', 'github.com').rsplit('/zip/', 1)[0],
                             'ids': {'spec256': key}, 'runs': runs, 'screens': shots}, about and f'# {name}\n\n{about}')
            say(f'[spec256] {name} ({len(files)} files)')
    shutil.rmtree(work, ignore_errors=True)


# ---------------------------------------------------------------- main

def main():
    lib = Library()
    say(f'{len(lib.used)} titles already in the library')
    for s in SECTIONS:
        say(f'\n== {s} ==')
        t0 = time.time()
        try:
            if s in ZX_HW:
                zxart(lib, s)
                if s == 'sam':
                    worldofsam(lib)
                if s == 'mb03':
                    mb03_site(lib)
            elif s in ('vtrd', 'msx', 'symbos', 'spec256'):
                globals()[s](lib)
            else:
                say(f'  unknown section {s}')
        except KeyboardInterrupt:
            raise
        except Exception as e:
            say(f'  {s} stopped: {e}')
            log(s, 'stopped', e)
        say(f'== {s} done in {int(time.time() - t0)}s ==')
    n = sum(1 for t in os.listdir(TITLES) if os.path.exists(os.path.join(TITLES, t, 'title.json')))
    say(f'\nAll done. {n} titles in {TITLES}. Problems, if any, are listed in {os.path.join(LIB, "fetch.log")}.')


try:
    main()
except KeyboardInterrupt:
    print('\nStopped. Run the same command again to carry on.')
    sys.exit(130)
PYEOF

python3 -u "$PY" "$TARGET" "$SECTIONS"
