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
#          demos     demoscene productions from pouet.net (ZX Spectrum, ZX Enhanced,
#                    SAM Coupe, MSX, MSX2) that zxart.ee doesn't already have
#          reviews   ratings and reviews for every title in the library, translated
#                    to English, into reviews.json next to each title.json
#          onedrive  your own OneDrive Spectrum folder (asks for the share link once,
#                    keeps it in TARGET/.onedrive-link, never in the repo)
#
# Each title becomes TARGET/titles/<id>/ with title.json (format 2), about.md,
# files/ and screens/ (see docs/cradle/titles.md). Run it again at any time:
# finished titles are skipped, so a stopped run carries on where it left off.
# A log of failures goes to TARGET/fetch.log.
#
# Environment: LIMIT=N (titles per section, for a trial run), WORKERS=4,
# MAX_RELEASES=0 (zxart.ee versions kept per title; 0 keeps them all).
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
  /run/media/*/*)   # a removable drive (its mount point itself, or a folder on it)
    drive="$(echo "$TARGET" | cut -d/ -f1-5)"
    mounted=yes
    if command -v mountpoint >/dev/null 2>&1; then
      mountpoint -q "$drive" || mounted=no
    else
      [ -d "$drive" ] || mounted=no
    fi
    if [ "$mounted" = no ]; then
      echo "$drive is not mounted. Open the drive in the file manager once, then run this again." >&2
      exit 1
    fi ;;
esac
mkdir -p "$TARGET/titles"
case ",$SECTIONS," in
  *,all,*|*,onedrive,*)
    if [ ! -s "$TARGET/.onedrive-link" ] && [ -r /dev/tty ]; then
      printf 'Paste your OneDrive share link for the Spectrum folder (or just press Enter to skip it): '
      read -r link < /dev/tty || link=""
      if [ -n "$link" ]; then printf '%s\n' "$link" > "$TARGET/.onedrive-link"; chmod 600 "$TARGET/.onedrive-link"; fi
    fi ;;
esac
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
# zxart.ee first, so the OneDrive importer merges into its titles (by ZXDB id) instead of the
# zxart pass doubling OneDrive's.
SECTIONS = sys.argv[2].split(',') if sys.argv[2] != 'all' else ['next', 'spectrum', 'clones', 'sam', 'mb03', 'onedrive', 'vtrd', 'msx', 'symbos', 'spec256', 'demos', 'reviews']
LIMIT = int(os.environ.get('LIMIT') or 0)
WORKERS = int(os.environ.get('WORKERS') or 4)
MAX_REL = int(os.environ.get('MAX_RELEASES') or 0)
TODAY = datetime.date.today().isoformat()
UA = 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36'
LOCK = threading.Lock()
LOG = open(os.path.join(LIB, 'fetch.log'), 'a', encoding='utf-8')

FAM48 = ['zx48', 'zx128', 'plus2', 'plus2a', 'plus3', 'pentagon', 'next']
FAM128 = ['zx128', 'plus2', 'plus2a', 'plus3', 'pentagon', 'next']
BETA = ['pentagon', 'scorpion', 'atm2']
# Only media the browser player can mount today (src/core/zxm_zx.c, zxm_sam.c, web/worker.js): tape,
# snapshot, Beta disks and SAM disks, all inserted as plain bytes. SD cards, IDE disks and MSX floppies
# need block media the player has no path for yet, and the classic Spectrums have no +3 disk or
# Microdrive slot, so .nex, .snx, .img, Spectrum .dsk, .mdr, .szx, .pzx and the like stay in files/
# without a run.
RUN_EXT = {'.tap': 'tape', '.tzx': 'tape', '.sna': 'snapshot', '.z80': 'snapshot',
           '.trd': 'beta.a', '.scl': 'beta.a', '.dsk': 'disk.1', '.mgt': 'disk.1', '.sad': 'disk.1'}
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
        self.ids, self.names, self.used, self.zxart_names = {}, {}, set(), set()
        for t in os.listdir(TITLES):
            self.used.add(t)
            try:
                j = json.load(open(os.path.join(TITLES, t, 'title.json'), encoding='utf-8'))
            except Exception:
                continue
            for k, v in (j.get('ids') or {}).items():
                self.ids[(k, str(v))] = t
            self.names.setdefault(norm(j.get('name')), t)
            self.note_zxart(j)

    def note_zxart(self, j):
        # Spectrum titles filed from zxart.ee, by name: vtrd.in skips a full version zxart already has.
        runs = j.get('runs') or []
        if (j.get('ids') or {}).get('zxart') and runs and all(r.get('platform', 'zx-spectrum') == 'zx-spectrum' for r in runs):
            self.zxart_names.add(norm(j.get('name')))

    def zxart_sam(self, name):
        """The SAM title zxart.ee filed under exactly this name, if there is one."""
        for sid in [t for t in self.used if t == slug(name) + '-sam' or t.startswith(slug(name) + '-sam-')]:
            try:
                j = json.load(open(os.path.join(TITLES, sid, 'title.json'), encoding='utf-8'))
            except Exception:
                continue
            if (j.get('ids') or {}).get('zxart') and 'samcoupe' in (j.get('tags') or []) and norm(j.get('name')) == norm(name):
                return sid
        return None

    def has_zxart_name(self, name):
        return norm(name) in self.zxart_names

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
            self.note_zxart(t)


    def link(self, sid, key, val):
        """Note another source's id on a title already filed, so later runs know it is done."""
        path = os.path.join(TITLES, sid, 'title.json')
        with LOCK:
            j = json.load(open(path, encoding='utf-8'))
            j.setdefault('ids', {})[key] = val
            tmp = path + '.part'
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(j, f, indent=2, ensure_ascii=False)
            os.replace(tmp, path)
            self.ids[(key, str(val))] = sid
            self.note_zxart(j)


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
                if subprocess.run(['lha', 'xqf', os.path.abspath(path)], cwd=dest, capture_output=True).returncode == 0:
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
                    # Clean each part first: cleaning can turn ".. " into "..".
                    parts = [x for x in (safe_name(p) for p in n.replace('\\', '/').split('/') if p) if x not in ('.', '..')]
                    if not parts:
                        continue
                    out = os.path.join(dest, *parts)
                    if os.path.commonpath([os.path.realpath(dest), os.path.realpath(out)]) != os.path.realpath(dest):
                        continue
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with z.open(m) as s, open(out, 'wb') as o:
                        shutil.copyfileobj(s, o)
        return True
    except Exception as e:
        log('unzip failed, trying 7z/unar', path, e)
        return False


def can_unpack(name):
    """Whether this machine has a tool for the archive's format (zip is built in)."""
    low = name.lower()
    if low.endswith('.zip') or shutil.which('7z') or shutil.which('unar'):
        return True
    return low.endswith(('.lzh', '.lha')) and bool(shutil.which('lha'))


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
        elif name.lower().endswith(ARCHIVES) and can_unpack(name):
            # A damaged download (or an error page saved as .zip): nothing is filed, so the next run retries.
            log('unpack failed', url)
            os.remove(arc)
            return []
        else:   # not an archive, or no extractor here for its format: kept as it is
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
    if ext in ('.trd', '.scl'):
        nd.append('beta')
        # Only the clones have Beta drives; a plain Spectrum family list means any of them.
        m = [x for x in m if x in BETA]
        if m in ([], ['pentagon']):
            m = list(BETA)
    if ext in ('.dsk', '.mgt', '.sad') and platform != 'sam-coupe':
        return None
    if slot == 'snapshot':   # the SAM has no snapshot slot (src/core/zxm_sam.c: disks and tape only)
        m = [x for x in m if x != 'sam']
    # The Next has no snapshot slot, and a tape needs NextZXOS from an SD card the player cannot mount
    # from bytes yet; zxart's 16K Spectrum is no machine here (a 48K runs those).
    m = [x for x in m if x not in ('next', 'zx16')]
    if not m:
        return None
    r = {'platform': platform, 'machines': list(dict.fromkeys(m)), 'files': [f], 'slot': slot, 'needs': sorted(set(nd))}
    if likes:
        r['likes'] = sorted(set(likes))
    if notes:
        r['notes'] = notes
    return r


def group_runs(runs):
    """Merge disk sets of one release that share a slot and machines into one run. Tapes and snapshots stay
    separate runs, because the player loads every tape or snapshot part in turn and the last one would win."""
    out = {}
    for r in runs:
        if not r:
            continue
        if r['slot'] in ('tape', 'snapshot'):
            out[id(r)] = r
            continue
        k = (os.path.splitext(r['files'][0])[1].lower(), r['slot'], tuple(r['machines']), tuple(r['needs']))
        if k in out:
            out[k]['files'] += r['files']
        else:
            out[k] = r
    return list(out.values())


MISSED = [0]   # titles left unfinished by a failed download or page; main() fails the section on any


def attempt(fn, it, label):
    """fn(it) for one title: a message when filed, False when there is nothing to file (no download, a
    duplicate), None or an exception when it failed and stays unfinished for the next run."""
    try:
        msg = fn(it)
    except Exception as e:
        msg = None
        log(label, 'failed', e)
    if msg is None:
        with LOCK:
            MISSED[0] += 1
    return msg


def batch(items, fn, label, total=None):
    """Run fn over items with a few workers, printing progress."""
    total = total or len(items)
    done = [0]

    def one(it):
        msg = attempt(fn, it, label)
        with LOCK:
            done[0] += 1
            if msg:
                print(f'[{label} {done[0]}/{total}] {msg}', flush=True)
    with ThreadPoolExecutor(WORKERS) as ex:
        list(ex.map(one, items))


def batch_or_trial(items, fn, label, left=None):
    """batch() for a full run. With LIMIT, one title at a time until `left` (default LIMIT) are filed:
    a title that fails stays unfinished and does not use up the trial, so later runs move past it."""
    if not LIMIT:
        return batch(items, fn, label)
    left = LIMIT if left is None else left
    for it in items:
        if left <= 0:
            break
        msg = attempt(fn, it, label)
        if msg:
            left -= 1
            say(f'[{label}] {msg}')


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
HW_MACH = {'zx16': FAM48, 'zx48': FAM48, 'zx128': FAM128, 'zx+2': FAM128, 'zx+2a': ['plus2a', 'plus3'],
           'zx+3': ['plus3', 'plus2a'], 'zx+2b': ['plus2a', 'plus3'], 'pentagon128': BETA,
           'scorpion': ['scorpion'], 'atm': ['atm2'], 'atm2': ['atm2'], 'zxnext': ['next'],
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


def zx_people(p):
    # Authors and groups are separate id spaces on zxart, so every key carries its type.
    return ({('author', str(a['authorId'])) for a in p.get('authorsInfo', []) if a.get('authorId')} |
            {('group', str(i)) for i in p.get('publishersIds', [])})


def zx_names(prods):
    """Fill NAMES; returns the (type, id) keys whose lookup failed, so their titles can wait for the next run."""
    want = sorted({k for p in prods for k in zx_people(p) if k not in NAMES})
    lost = set()
    for typ, f in (('author', 'authorId'), ('group', 'groupId')):
        ids = [i for t, i in want if t == typ]
        for i in range(0, len(ids), 50):
            ch = ','.join(ids[i:i + 50])
            try:
                j = fetch_json(f'{ZX}/types:{typ}/export:{typ}/start:0/limit:50/filter:{f}={ch};')
                for a in j['responseData'].get(typ, []):
                    NAMES[(typ, str(a['id']))] = html.unescape(a['title'])
            except Exception as e:
                log('zxart names', e)
                lost.update((typ, x) for x in ids[i:i + 50])
    return lost


def zx_pubs(r):
    return [NAMES[('group', str(i))] for i in r.get('publishersIds', []) if ('group', str(i)) in NAMES]


def zx_authors(r):
    return list(dict.fromkeys(NAMES[('author', str(a['authorId']))] for a in r.get('authorsInfo', [])
                              if ('author', str(a.get('authorId'))) in NAMES))


def zx_title(lib, p, rels, hw):
    rels = [r for r in rels if r.get('file') and r.get('fileName')]
    if not rels:
        return False   # nothing to download
    rels.sort(key=lambda r: (r.get('releaseType') != 'original', -(r.get('year') or 0)))
    rels = rels[:MAX_REL or None]
    name = html.unescape(p['title']).strip()
    charset = 'ru' if set(p.get('language') or []) & {'ru', 'ua', 'by'} or hw in ZX_HW['clones'] else None
    sam = hw == 'samcoupe'
    sid = lib.claim(name, '-sam' if sam else '', 'zxart', p['id'])
    tdir = os.path.join(TITLES, sid)
    runs, nfiles, versions, vids = [], 0, [], set()
    plang = (p.get('language') or ['en'])[0]
    for r in rels:
        lang = (r.get('language') or [plang])[0] if isinstance(r.get('language'), list) else (r.get('language') or plang)
        vid = {'original': 'orig'}.get(r.get('releaseType'), slug(r.get('releaseType') or 'release'))
        if lang and lang != 'en':
            vid += '-' + lang
        base, n = vid, 2
        while vid in vids:
            vid, n = f'{base}-{n}', n + 1
        vids.add(vid)
        rpubs = zx_pubs(r)
        versions.append({'id': vid, 'label': ', '.join(x for x in (r.get('releaseType') or 'release', r.get('version'),
                                                                  '/'.join(r.get('releaseFormat') or []), str(r.get('year') or '')) if x),
                         'year': r.get('year') or None, 'language': lang or None,
                         'authors': zx_authors(r),
                         'publisher': rpubs[0] if rpubs else None,
                         'source': f'https://zxart.ee/eng/software/?id={p["id"]}', 'ids': {'zxart_release': r['id']},
                         'notes': safe_name(r['fileName'])})
        hwr = r.get('hardwareRequired') or []
        # No emulated machine has a Pentagon's 512K or 1 MB, so those releases are kept in files/ without a run.
        big = bool(set(hwr) & {'pentagon512', 'pentagon1024'})
        mach = [] if big else [m for h in hwr for m in HW_MACH.get(h, [])] or HW_MACH.get(hw) or FAM48
        needs = [HW_NEEDS[h] for h in hwr if h in HW_NEEDS] or ([HW_NEEDS[hw]] if hw in HW_NEEDS and not hwr else [])
        likes = [HW_LIKES[h] for h in hwr if h in HW_LIKES]
        files = get_files(tdir, r['file'], r['fileName'], charset, sub=vid)
        if not files:
            # Leave the title unfinished (its .claim stays) so the next run tries every release again.
            log('zxart', p['id'], 'release', r['id'], 'gave no files')
            return None
        nfiles += len(files)
        vruns = group_runs([run_table(f, mach, 'sam-coupe' if sam or 'sam' in mach else 'zx-spectrum', needs, likes,
                                      versions[-1]['label']) for f in files]) if mach else []
        for x in vruns:
            x['version'] = vid
        runs += vruns
    if not nfiles:
        return None
    url = f'https://zxart.ee/eng/software/?id={p["id"]}'
    desc = html.unescape(p.get('description') or '').strip()
    cat = html.unescape(p.get('categoriesString') or '')
    ids = {'zxart': p['id']}
    imp = p.get('importIds') or {}
    for k, key in (('zxdb', 'zxdb'), ('pouet', 'pouet'), ('dzoo', 'demozoo'), ('3a', 'zxaaa')):
        if str(imp.get(k) or '').isdigit() and int(imp[k]):
            ids[key] = int(imp[k])
    pubs = zx_pubs(p)
    t = {'format': 2, 'name': name, 'kind': kind_of(cat), 'year': p.get('year') or None,
         'authors': zx_authors(p),
         'publisher': pubs[0] if pubs else None, 'genres': [g for g in cat.split('/')[1:] if g][-2:],
         'tags': [hw], 'summary': desc.split('\n')[0][:200] or None,
         'license': LIC.get(p.get('legalStatus'), 'unknown'), 'source': url,
         'sources': [url] + ([f'https://spectrumcomputing.co.uk/entry/{ids["zxdb"]}'] if 'zxdb' in ids else []),
         'ids': ids, 'runs': runs, 'compat': {}, 'versions': versions,
         'party': {'zxart_party': int(p['partyId']), 'name': None, 'year': p.get('year') or None, 'place': int(p['partyPlace']) if str(p.get('partyPlace') or '').isdigit() else None,
                   'compo': p.get('compo')} if p.get('partyId') else None,
         'screens': get_screens(tdir, p.get('imagesUrls') or [])}
    t = {k: v for k, v in t.items() if v not in (None, [], '')} | {'runs': runs}
    lib.finish(sid, t, f'# {name}\n\n{desc}\n\nFrom {url}' if desc else None)
    return f'{name} ({nfiles} files)'


def zxart(lib, section):
    """Returns how many titles it filed, so LIMIT covers the whole section, and the catalog parts that failed."""
    made, broken = 0, []
    for hw in ZX_HW[section]:
        if LIMIT and made >= LIMIT:
            break
        start, total = 0, None
        while True:
            j = None
            for lim in (100, 20, 5, 1):
                try:
                    j = fetch_json(f'{ZX}/types:zxProd/export:zxProd/start:{start}/limit:{lim}/order:id,asc/filter:zxProdHardware={hw};')
                    break
                except Exception as e:
                    log('zxart', hw, start, lim, e)
            if j is None:
                if total is None or start + 1 >= total:
                    say(f'  zxart.ee {hw}: page {start} keeps failing; skipping the rest of {hw}')
                    broken.append(f'{hw} from title {start}')
                    break
                say(f'  zxart.ee {hw}: title {start} keeps failing on their server; skipping just that one')
                broken.append(f'{hw} title {start}')
                start += 1
                continue
            total = j.get('totalAmount') or 0
            prods = j['responseData'].get('zxProd') or []
            if start == 0:
                say(f'zxart.ee {hw}: {total} titles')
            if not prods:
                break
            start += len(prods)
            todo = [p for p in prods if not lib.has('zxart', p['id'])
                    and not html.unescape(p.get('categoriesString') or '').startswith('Press')]
            # A game already filed from ZXDB (the OneDrive importer) gets the zxart id, not a second folder.
            for p in list(todo):
                z = str((p.get('importIds') or {}).get('zxdb') or '')
                if z.isdigit() and int(z) and lib.has('zxdb', int(z)):
                    lib.link(lib.ids[('zxdb', str(int(z)))], 'zxart', p['id'])
                    todo.remove(p)
            if todo:
                ids = [r for p in todo for r in p.get('releasesIds', [])]
                rels, lost = {}, set()
                for i in range(0, len(ids), 50):
                    try:
                        jr = fetch_json(f'{ZX}/types:zxRelease/export:zxRelease/start:0/limit:50/filter:zxReleaseId={",".join(map(str, ids[i:i + 50]))};')
                        for r in jr['responseData'].get('zxRelease') or []:
                            rels[r['id']] = r
                        miss = set(ids[i:i + 50]) - set(rels)   # asked for but not sent: as good as lost
                        if miss:
                            lost.update(miss)
                            broken.append(f'{hw} release details near title {start}')
                    except Exception as e:
                        log('zxart releases', e)
                        lost.update(ids[i:i + 50])
                        broken.append(f'{hw} release details near title {start}')
                # A title whose release details did not arrive waits for the next run instead of being filed short.
                todo = [p for p in todo if not lost & set(p.get('releasesIds', []))]
                gone = zx_names(todo + list(rels.values()))
                if gone:
                    broken.append(f'{hw} author and publisher names near title {start}')
                # Likewise a title whose author or publisher names did not arrive.
                todo = [p for p in todo if not gone & (zx_people(p) | {x for i in p.get('releasesIds', []) if i in rels
                                                                         for x in zx_people(rels[i])})]
                title = lambda p: zx_title(lib, p, [rels[i] for i in p.get('releasesIds', []) if i in rels], hw)
                if not LIMIT:
                    batch(todo, title, f'{hw} {start}/{total}', len(todo))
                    made += len(todo)
                else:
                    # A trial run goes one at a time and counts only titles filed, so a prod with
                    # nothing to download does not use up the limit on every run.
                    for p in todo:
                        if made >= LIMIT:
                            break
                        msg = attempt(title, p, hw)
                        if msg:
                            made += 1
                            say(f'[{hw}] {msg}')
            if (LIMIT and made >= LIMIT) or start >= total:
                break
    return made, broken


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


def mb03_site(lib, made=0):
    try:
        page = fetch(f'https://drive.google.com/embeddedfolderview?id={MB03_DRIVE}').decode('utf-8', 'replace')
    except Exception as e:
        raise RuntimeError(f'MB03+ site: {e}')
    ids = {html.unescape(n): i for i, n in re.findall(r'/file/d/([^/"]+).*?flip-entry-title">([^<]+)', page, re.S)}
    say(f'MB03+ site: {len(MB03_RECS)} titles')
    absent = [fn for _, _, _, fn, _ in MB03_RECS if fn not in ids and not lib.has('mb03site', fn)]
    for name, kind, year, fn, summary in MB03_RECS:
        if lib.has('mb03site', fn) or fn not in ids:
            continue
        if LIMIT and made >= LIMIT:
            break
        sid = lib.claim(name, '-mb03', 'mb03site', fn)
        tdir = os.path.join(TITLES, sid)
        files = get_files(tdir, f'https://drive.usercontent.google.com/download?id={ids[fn]}&export=download&confirm=t', fn)
        if not files:   # left unfinished; the section is marked failed so a rerun fetches it
            with LOCK:
                MISSED[0] += 1
            continue
        runs = group_runs([run_table(f, FAM128, needs=['mb03']) for f in files])
        lib.finish(sid, {'name': name, 'kind': kind, 'year': year, 'tags': ['mb03+'], 'summary': summary,
                         'license': 'freeware', 'source': MB03_SITE, 'sources': [MB03_SITE], 'ids': {'mb03site': fn}, 'runs': runs})
        made += 1   # counted once filed, so a download that fails does not use up a trial run
        say(f'[mb03 site] {name} ({len(files)} files)')
    if absent:  # the files listed were done; the page has changed or lost some, so mark the section stopped
        raise RuntimeError(f'MB03+ site: {", ".join(absent)} not found in the Drive folder')


# ---------------------------------------------------------------- World of SAM

WOS = 'https://www.worldofsam.org'


def wos_field(p, label):
    m = re.search(r'field__label">\s*' + label + r'\s*</div>(.*?)</div>\s*</div>', p, re.S)
    return [html.unescape(x).strip() for x in re.findall(r'>([^<>]+)<', m.group(1)) if x.strip()] if m else []


def worldofsam(lib, made=0):
    links, broken = {}, []
    for cat, kind in ((3, 'game'), (4, 'demo')):
        page = 0
        while True:
            try:
                p = fetch(f'{WOS}/products?field_category_target_id={cat}&page={page}').decode('utf-8', 'replace')
            except Exception as e:
                say(f'  worldofsam.org: {e}')
                broken.append(f'{kind}s page {page + 1}')
                break
            new = [h for h in re.findall(r'<td[^>]*>\s*<a href="(/products/[^"?#]+)"', p) if h not in links]
            for h in new:
                links[h] = kind
            if not new or f'page={page + 1}"' not in p:
                break
            page += 1
    say(f'worldofsam.org: {len(links)} titles')
    todo = [x for x in links.items() if not lib.has('worldofsam', x[0])]

    def one(item):
        h, kind = item
        u = WOS + h
        p = fetch(u, timeout=60).decode('utf-8', 'replace')
        name = html.unescape(re.search(r'<h1[^>]*>(?:\s*<span[^>]*>)?\s*([^<]+)', p).group(1)).strip()
        urls = sorted(set(re.findall(r'href="(https://www\.worldofsam\.org/system/files/[^"]+)"', p)))
        if not urls:
            return False
        same = lib.zxart_sam(name)
        if same:
            lib.link(same, 'worldofsam', h)   # already filed from zxart.ee; noted so it is not fetched again
            return False
        sid = lib.claim(name, '-sam', 'worldofsam', h)
        tdir = os.path.join(TITLES, sid)
        runs, n = [], 0
        for f in urls:
            got = get_files(tdir, f, urllib.parse.unquote(f.rsplit('/', 1)[-1]))
            if not got:
                # Leave the title unfinished so the next run tries every download again.
                log('worldofsam', h, f, 'gave no files')
                return None
            n += len(got)
            runs += group_runs([run_table(x, ['sam'], 'sam-coupe') for x in got])
        imgs = list(dict.fromkeys(re.findall(r'href="(https://www\.worldofsam\.org/sites/default/files/[^"]+\.(?:png|jpg|gif))"', p, re.I)))
        yr = wos_field(p, 'Release Year')
        desc = re.sub(r'\s+', ' ', ' '.join(wos_field(p, 'Description'))).strip()
        tt = {'name': name, 'kind': kind, 'year': int(yr[0]) if yr and yr[0].isdigit() else None,
              'authors': wos_field(p, 'Author'), 'publisher': (wos_field(p, 'Publisher') or [None])[0],
              'tags': ['sam coupe'], 'summary': desc[:200] or None, 'license': 'unknown', 'source': u, 'sources': [u],
              'ids': {'worldofsam': h}, 'runs': runs, 'screens': get_screens(tdir, imgs)}
        lib.finish(sid, {k: v for k, v in tt.items() if v not in (None, [])} | {'runs': tt['runs']},
                   f'# {name}\n\n{desc}\n\nFrom {u}' if desc else None)
        return f'{name} ({n} files)'
    if not LIMIT:
        batch(todo, one, 'worldofsam')
    else:
        # A trial run goes one title at a time, so titles already filed from zxart.ee do not use up the limit.
        left = max(0, LIMIT - made)
        for item in todo:
            if left <= 0:
                break
            msg = attempt(one, item, 'worldofsam')
            if msg:   # only a title filed counts; a failed download is tried again next run
                left -= 1
                say(f'[worldofsam] {msg}')
    if broken:  # the pages that came were done; this marks the section as stopped so a rerun fills the gap
        raise RuntimeError(f'worldofsam.org {", ".join(broken)} could not be fetched')


# ---------------------------------------------------------------- vtrd.in
# Russian disk games (TR-DOS) for Pentagon, Scorpion and ATM Turbo.

def vtrd(lib):
    rows, lost = [], []
    for t in ('full_ver', 'translat', 'remix'):
        try:
            page = fetch(f'https://vtrd.in/games.php?t={t}').decode('utf-8', 'replace')
        except Exception as e:
            say(f'  vtrd.in {t}: {e}')
            lost.append(t)
            continue
        n0 = len(rows)
        for m in re.finditer(r'<tr[^>]*><td nowrap><a href="(/[^"]+)">(.*?)</a></td><td>(.*?)</td><td>(.*?)</td>'
                             r'<td>[^<]*</td><td>(?:<a[^>]*href="(/release\.php\?r=[0-9a-f]+)")?', page):
            href, name, auth, grp, rel = m.groups()
            rows.append((t, href, html.unescape(re.sub('<[^>]+>', '', name)).replace('\xa0', ' ').strip(),
                         html.unescape(auth).strip(), html.unescape(grp).strip(), rel))
        if len(rows) == n0:   # a page with no games: changed markup or an error page, not an empty list
            say(f'  vtrd.in {t}: no games found on the page')
            lost.append(t)
    say(f'vtrd.in: {len(rows)} titles')
    # A full version whose name zxart.ee already filed is skipped before LIMIT, so trial runs keep moving on.
    todo = [r for r in rows if not lib.has('vtrd', r[1])
            and not (r[0] == 'full_ver' and lib.has_zxart_name(re.sub(r'\s*\(.*\)$', '', r[2]) or r[2]))]

    def one(row):
        t, href, name, auth, grp, rel = row
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
                # Leave the title unfinished: the release page is where General Sound is noted.
                log('vtrd release', href, e)
                return None
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
    batch_or_trial(todo, one, 'vtrd')
    if lost:  # the other lists were done; this marks the section as stopped so a rerun fills the gap
        raise RuntimeError(f'vtrd.in list {", ".join(lost)} could not be fetched')


# ---------------------------------------------------------------- MSX (file-hunter.com)

FH = 'https://download.file-hunter.com/'
# No CAS folders: the MSX machines have no tape slot yet.
FH_DIRS = {'Games/MSX1/ROM': 'cart1', 'Games/MSX1/DSK': 'disk0', 'Games/MSX1/MSX-Binaries': 'disk0',
           'Games/MSX1/HDD': 'ide0', 'Games/MSX2/ROM': 'cart1', 'Games/MSX2/DSK': 'disk0',
           'Games/MSX2/MSX-Binaries': 'disk0', 'Games/MSX2/Harddisk': 'ide0', 'Demos': 'disk0'}
FH_MACH = {'msx1': ['msx1', 'msx1eu', 'msx2', 'msx2eu'], 'msx2': ['msx2', 'msx2eu']}


def msx_name(fn):
    n = re.sub(r'\.[a-z0-9]{2,4}$', '', fn, flags=re.I)
    n = re.sub(r'\s*[\(\[].*$', '', n).strip()
    m = re.match(r'(.*), (The|A|An)$', n)
    return f'{m.group(2)} {m.group(1)}' if m else n


def msx(lib):
    try:
        listing = fetch(FH + 'allfiles.txt').decode('utf-8-sig', 'replace')
    except Exception as e:
        raise RuntimeError(f'file-hunter.com list: {e}')
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
        g = groups.setdefault(key, {'name': name, 'demo': demo, 'files': []})
        g['files'].append((d, p, gen))
    say(f'file-hunter.com MSX: {len(groups)} titles')
    todo = [(k, g) for k, g in groups.items() if not lib.has('filehunter', k)]
    order = list(FH_DIRS)

    def one(kg):
        key, g = kg
        sid = lib.claim(key, '', 'filehunter', key)
        tdir = os.path.join(TITLES, sid)
        runs, n = [], 0
        for d, p, gen in sorted(g['files'], key=lambda x: order.index(x[0])):
            slot = FH_DIRS[d]
            # Each folder's file in its own directory: Games/MSX1/ROM/Foo.zip and Games/MSX2/ROM/Foo.zip both come.
            fn = p.rsplit('/', 1)[-1]
            files = get_files(tdir, FH + p, fn, sub=slug(d) + '-' + re.sub(r'(\.[a-z0-9]{2,4})+$', '', fn, flags=re.I))
            if not files:
                # Leave the title unfinished so the next run tries every file of the group again.
                log('filehunter', p, 'gave no files')
                return None
            n += len(files)
            keep = [f for f in files if not f.lower().endswith(('.txt', '.nfo', '.png', '.jpg', '.pdf', '.diz'))]
            # Only cartridges get runs, one per ROM (a slot takes one file): floppy and hard disk slots
            # take block media the player cannot make yet.
            if slot == 'cart1':
                runs += [{'platform': 'msx', 'machines': FH_MACH[gen], 'files': [f], 'slot': slot, 'needs': [],
                          'notes': d.split('/')[-1]} for f in keep if f.lower().endswith(('.rom', '.mx1', '.mx2'))]
        if not n:
            return None
        src = FH + g['files'][0][1]
        tt = {'name': g['name'], 'kind': 'demo' if g['demo'] else 'game', 'tags': ['msx'], 'license': 'unknown',
              'source': 'https://www.file-hunter.com/', 'sources': [src], 'ids': {'filehunter': key}, 'runs': runs}
        lib.finish(sid, tt)
        return f'{g["name"]} ({n} files)'
    batch_or_trial(todo, one, 'msx')


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
        raise RuntimeError(f'symbos.de app list: {e}')
    apps = [r for r in js_rows(src.split('garrApps', 1)[-1]) if len(r) >= 17]
    say(f'symbos.de: {len(apps)} apps + SymbOS itself')
    made = 0
    if not lib.has('symbos', 'os'):
        sid = lib.claim('SymbOS', '', 'symbos', 'os')
        tdir = os.path.join(TITLES, sid)
        got = {}
        for f in ('SymbOS-MSX40.zip', 'SymbOS-ZNX40.zip', 'SymbOS-40-MassStorageCompletion.zip', 'SymbOS-40-Manual.pdf'):
            got[f] = get_files(tdir, SY + 'download/20250131-V40/' + f, f)
        if all(got.values()):  # otherwise the next run tries all four again
            made += 1
            # Both packages are files to install onto a NextZXOS SD card or an MSX hard disk (the SymbOS
            # recipe does that), and the player cannot mount either medium from bytes yet, so no runs.
            runs = []
            lib.finish(sid, {'name': 'SymbOS', 'kind': 'system', 'year': 2025, 'authors': ['Prodatron'], 'tags': ['symbos'],
                             'summary': 'Multitasking windowed operating system for Z80 machines.', 'license': 'freeware',
                             'source': SY, 'sources': [SY], 'ids': {'symbos': 'os'}, 'runs': runs})
            say('[symbos] SymbOS 4.0')
        else:   # left unfinished; the section is marked failed so a rerun fetches it
            with LOCK:
                MISSED[0] += 1
    todo = [a for a in apps if not lib.has('symbos', a[0]) and (a[7] == '0' or set(a[7]) & {'2', '7'})]

    def one(a):
        aid, name, ver, date, plats = a[0], a[1], a[2], a[3], a[7]
        zipn = re.sub(r'[ \'"()/\\]', '', name) + '.zip'
        sid = lib.claim(name, '-symbos', 'symbos', aid)
        tdir = os.path.join(TITLES, sid)
        files = get_files(tdir, SY + 'download/apps/' + zipn, zipn)
        if not files:
            return None
        # An app is copied onto a running SymbOS's drive, not mounted, so it has no run of its own.
        on = [n for n, k in (('ZX Spectrum Next', '7'), ('MSX2', '2')) if plats == '0' or k in plats]
        runs = []
        shots = [f'{SY}gfx/shots/{a[13]}{i}.gif' for i in range(1, min(3, int(a[12] or 0)) + 1)] if a[13] else []
        ym = re.search(r'(\d{4})$', date)
        tt = {'name': name, 'kind': 'application', 'year': int(ym.group(1)) if ym else None, 'authors': [a[5]] if a[5] else [],
              'tags': ['symbos'], 'summary': a[15] or None, 'license': 'freeware', 'source': SY, 'sources': [SY + 'apps.htm'],
              'ids': {'symbos': aid}, 'runs': runs, 'screens': get_screens(tdir, shots)}
        lib.finish(sid, {k: v for k, v in tt.items() if v not in (None, [])} | {'runs': runs},
                   f'# {name} {ver}\n\n{html.unescape(re.sub("<[^>]+>", " ", a[16] or ""))}\n\n'
                   f'A SymbOS app for {" and ".join(on)}: copy files/ onto the drive of a set-up that boots SymbOS.')
        return f'{name} ({len(files)} files)'
    batch_or_trial(todo, one, 'symbos', max(0, LIMIT - made))


# ---------------------------------------------------------------- Spec256

SPEC256 = os.environ.get('SPEC256_URLS', 'https://codeload.github.com/helpquick/Spec256-Games/zip/HEAD '
                                          'https://codeload.github.com/mvvproject/Spec256-Games/zip/HEAD').split()


def z80_is_48k(path):
    with open(path, 'rb') as f:
        h = f.read(35)
    if len(h) < 35 or h[6] or h[7]:
        return len(h) >= 30   # version 1 files are all 48K
    ext = h[30] | h[31] << 8
    return h[34] in ((0, 1) if ext == 23 else (0, 1, 3))   # 48K, 48K + IF1, and in v3 48K + MGT


def spec256(lib):
    work = os.path.join(LIB, '.spec256')
    made, broken = 0, []
    for n, u in enumerate(SPEC256):
        if LIMIT and made >= LIMIT:
            break
        z = os.path.join(work, f'{n}.zip')
        if not save(u, z) or not unpack(z, os.path.join(work, str(n))):
            say(f'  Spec256: could not get {u}')
            broken.append(u)
            continue
        root = os.path.join(work, str(n))
        tops = os.listdir(root)
        if len(tops) == 1 and os.path.isdir(os.path.join(root, tops[0])):
            root = os.path.join(root, tops[0])
        games = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)) and not d.startswith('.'))
        say(f'Spec256 {u.split("/")[3]}: {len(games)} titles')
        games = [g for g in games if not lib.has('spec256', slug(g))]
        if LIMIT:
            games = games[:LIMIT - made]
        made += len(games)
        for g in games:
            key = slug(g)
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
            # Just what the Spec256 machine loads: its companion files and the snapshot.
            game = [f for f in files if re.search(r'\.(gfx|gf[0-7ab]|b\d\d|cfg|pal|sna|z80)$', f, re.I)]
            snaps = [f for f in game if re.search(r'\.(sna|z80)$', f, re.I)]
            # A run needs a snapshot to start and a companion to colour it. The Spec256 machine takes a
            # 128K .sna but only a 48K .z80, so a 128K .z80 package is kept without a run too.
            if not snaps or len(snaps) == len(game) or not all(f.lower().endswith('.sna') or z80_is_48k(os.path.join(tdir, f)) for f in snaps):
                game = []
            runs = [{'platform': 'zx-spectrum', 'machines': ['spec256'], 'files': game, 'slot': 'gfx', 'needs': [],
                     'notes': '256 colours (Spec256). Load every file into the gfx slot, then the .sna into snapshot.'}] if game else []
            # A 128K snapshot (longer than the 49179-byte 48K one) will not load on a 48K machine.
            small = lambda f: (os.path.getsize(os.path.join(tdir, f)) <= 49179 if f.lower().endswith('.sna')
                               else z80_is_48k(os.path.join(tdir, f)))
            runs += [run_table(f, (['zx48'] if small(f) else []) + ['zx128', 'plus2', 'plus2a', 'plus3', 'pentagon'],
                               notes='The original game in Spectrum colours.') for f in snaps]
            name = re.sub(r'\s+', ' ', g).strip()
            lib.finish(sid, {'name': name, 'kind': 'game', 'tags': ['spec256', '256 colours'], 'license': 'unknown',
                             'source': u.replace('codeload.github.com', 'github.com').rsplit('/zip/', 1)[0],
                             'ids': {'spec256': key}, 'runs': runs, 'screens': shots}, about and f'# {name}\n\n{about}')
            say(f'[spec256] {name} ({len(files)} files)')
    shutil.rmtree(work, ignore_errors=True)
    if broken:  # the other repository was done; this marks the section as stopped so a rerun fills the gap
        raise RuntimeError(f'Spec256 {", ".join(broken)} could not be fetched')


# ---------------------------------------------------------------- OneDrive
# Dale's own collection. The importer lives in tools/onedrive/ in the repo; the
# share link stays on this machine in .onedrive-link.

RAW = os.environ.get('ZXC_RAW', 'https://raw.githubusercontent.com/DalsinAI/ZxNextEmu/{ref}/tools/onedrive/')
REFS = ['main']


def onedrive(lib):
    lf = os.path.join(LIB, '.onedrive-link')
    link = open(lf).read().strip() if os.path.exists(lf) else ''
    if not link:
        say('  No OneDrive link saved, so skipping it. Run with the section "onedrive" to be asked for it.')
        return
    tool = os.path.join(LIB, '.onedrive-tool')
    os.makedirs(tool, exist_ok=True)
    for ref in REFS:
        try:
            for f in ('onedrive_local.py', 'onedrive-records.json.gz'):
                data = fetch(RAW.format(ref=ref) + f, tries=2)
                open(os.path.join(tool, f), 'wb').write(data)
            break
        except Exception as e:
            log('onedrive tool', ref, e)
    else:
        raise RuntimeError('could not get the OneDrive importer from GitHub; run again to retry')
    env = dict(os.environ)
    if LIMIT:
        env['ZXC_LIMIT'] = str(LIMIT)
    rc = subprocess.run([sys.executable, '-u', '-I', os.path.join(tool, 'onedrive_local.py'), LIB, link], env=env).returncode
    if rc in (3, 4):  # 3: some downloads failed, 4: OneDrive could not be reached; the link is fine either way
        lib.__init__()
        raise RuntimeError('OneDrive could not be reached or some files did not download; run again to retry them')
    if rc:
        os.replace(lf, lf + '.failed')
        say(f'  OneDrive importer stopped (code {rc}). The saved link may be wrong, so it has been set aside;\n'
            f'  run the "onedrive" section again and paste the share link (it starts https://1drv.ms/) when asked.')
        lib.__init__()
        raise RuntimeError(f'the OneDrive importer stopped with code {rc}')
    lib.__init__()


# ---------------------------------------------------------------- demos (pouet.net)
# Demoscene productions with their party placings. zxart.ee already carries most
# ZX demos (with pouet ids), so this adds what it lacks plus SAM and MSX demos.

POUET_PLATFORMS = {
    'ZX Spectrum': ('zx-spectrum', FAM128), 'ZX Enhanced': ('zx-spectrum', BETA + ['next']),
    'SAM Coupé': ('sam-coupe', ['sam']), 'MSX': ('msx', ['msx1', 'msx1eu', 'msx2', 'msx2eu']), 'MSX 2': ('msx', ['msx2', 'msx2eu']),
}
POUET_KIND = {'demo': 'demo', 'intro': 'demo', '4k': 'demo', '1k': 'demo', '256b': 'demo', '128b': 'demo', '64b': 'demo',
              '32b': 'demo', 'dentro': 'demo', 'demotool': 'utility', 'musicdisk': 'music', 'procedural graphics': 'demo',
              'slideshow': 'demo', 'artpack': 'demo', 'diskmag': 'application', 'game': 'game', 'cracktro': 'demo',
              'invitation': 'demo', 'report': 'demo', 'wild': 'demo', 'votedisk': 'demo', 'fastdemo': 'demo', 'bbstro': 'demo'}
POUET_CACHE = {}


def pouet_prod(pid):
    """The pouet prod with its comments (one API call), cached for the reviews pass."""
    if pid not in POUET_CACHE:
        POUET_CACHE[pid] = json.loads(fetch(f'https://api.pouet.net/v1/prod/comments/?id={pid}'))['prod']
    return POUET_CACHE[pid]


def pouet_reviews(pid):
    p = pouet_prod(pid)
    url = f'https://www.pouet.net/prod.php?which={pid}'
    up, pig, down = (int(p.get(k) or 0) for k in ('voteup', 'votepig', 'votedown'))
    ratings = [{'site': 'pouet', 'url': url, 'score': round((float(p.get('voteavg') or 0) + 1) * 5, 2), 'scale': 10,
                'votes': up + pig + down, 'version': None}] if up + pig + down else []
    reviews = []
    for c in p.get('comments') or []:
        text = html.unescape(c.get('comment') or '').strip()
        if not text:
            continue
        rt = str(c.get('rating'))
        reviews.append({'site': 'pouet', 'url': url, 'author': (c.get('user') or {}).get('nickname'),
                        'date': (c.get('addedDate') or '')[:10] or None,
                        'score': {'1': 10.0, '0': 5.0, '-1': 0.0}.get(rt), 'scale': 10 if rt in ('1', '0', '-1') else None,
                        'version': None, 'title': None} | english(text))
    return ratings, reviews


def pouet_runs(files, plat):
    platform, mach = POUET_PLATFORMS[plat]
    if platform == 'msx':  # cartridges only: MSX floppies need block media the player cannot make yet
        # .mx2 is an MSX2 cartridge, whichever MSX list the prod is under
        return [{'platform': 'msx', 'machines': [m for m in mach if not m.startswith('msx1')] if f.lower().endswith('.mx2') else mach,
                 'files': [f], 'slot': 'cart1', 'needs': []}
                for f in files if os.path.splitext(f)[1].lower() in ('.rom', '.mx1', '.mx2')]
    return group_runs([run_table(f, mach, platform) for f in files])


def demos(lib):
    """One title per pouet prod, with runs for every platform of ours it is listed under."""
    seen, broken = {}, []   # prod id -> the platform list it was found in; list pages that failed

    def listed():
        """Productions not in the library yet, page by page, so a trial run reads only as far as it needs."""
        for plat in POUET_PLATFORMS:
            ids, page = set(), 1
            while True:
                try:
                    pg = fetch('https://www.pouet.net/prodlist.php?' + urllib.parse.urlencode({'platform[]': plat, 'page': page})).decode('utf-8', 'replace')
                except Exception as e:
                    say(f'  pouet.net {plat} page {page}: {e}')
                    broken.append(f'{plat} page {page}')
                    break
                new = [i for i in dict.fromkeys(re.findall(r'prod\.php\?which=(\d+)', pg)) if i not in ids]
                if not new:
                    break
                ids.update(new)
                page += 1
                for i in new:
                    if not lib.has('pouet', i) and i not in seen:
                        seen[i] = plat
                        yield i

    def one(pid):
        p = pouet_prod(pid)
        name = html.unescape(p.get('name') or '').strip() or pid
        dl = (p.get('download') or '').strip()
        if not dl:
            return False
        if 'files.scene.org/view/' in dl:
            dl = dl.replace('files.scene.org/view/', 'files.scene.org/get/')
        elif not re.search(r'\.(zip|rar|7z|lzh|lha|tap|tzx|trd|scl|sna|z80|dsk|mgt|sad|rom|mx1|mx2|nex)(\?|$)', dl, re.I):
            return False
        # A prod listed under several of our platforms gets runs for each, so none is lost.
        listed = [(x or {}).get('name') for x in (p.get('platforms') or {}).values()] if isinstance(p.get('platforms'), dict) else []
        plats = [x for x in POUET_PLATFORMS if x in listed] or [seen[pid]]
        plat = plats[0]
        groups = [html.unescape(g.get('name') or '') for g in p.get('groups') or [] if g.get('name')]
        sid = lib.claim(name, '-' + slug(plat), 'pouet', pid)
        tdir = os.path.join(TITLES, sid)
        files = get_files(tdir, dl, urllib.parse.unquote(dl.rsplit('/', 1)[-1].split('?')[0]),
                          'ru' if POUET_PLATFORMS[plat][0] == 'zx-spectrum' else None)
        if not files:
            return None
        runs = [r for x in plats for r in pouet_runs(files, x)]
        # Nothing the player can mount: the title is still filed, with its files and no run.
        pl = (p.get('placings') or [{}])[0] if p.get('placings') else {}
        party = {'name': html.unescape(((pl.get('party') or {}).get('name')) or ''), 'year': int(pl['year']) if str(pl.get('year') or '').isdigit() else None,
                 'compo': pl.get('compo_name'), 'place': int(pl['ranking']) if str(pl.get('ranking') or '').isdigit() and int(pl['ranking']) < 97 else None} if pl else None
        yr = (p.get('releaseDate') or '')[:4]
        url = f'https://www.pouet.net/prod.php?which={pid}'
        types = p.get('types') or [p.get('type')]
        ids = {'pouet': int(pid)}
        if str(p.get('demozoo') or '0') not in ('0', ''):
            ids['demozoo'] = int(p['demozoo'])
        tt = {'name': name, 'kind': POUET_KIND.get((types[0] or '').lower(), 'demo'), 'year': int(yr) if yr.isdigit() else None,
              'authors': groups, 'publisher': groups[0] if groups else None, 'genres': [t for t in types if t],
              'tags': ['demoscene'] + [x.lower() for x in plats], 'license': 'freeware', 'source': url, 'sources': [url, dl],
              'ids': ids, 'party': party, 'runs': runs,
              'screens': get_screens(tdir, [p['screenshot']] if p.get('screenshot') else [], None, 1)}
        lib.finish(sid, {k: v for k, v in tt.items() if v not in (None, [], '')} | {'runs': runs})
        return f'{name} ({len(files)} files)'
    # A trial run counts only titles actually filed, and keeps reading the lists until it has them.
    batch_or_trial(listed() if LIMIT else list(listed()), one, 'pouet')
    if broken:  # the prods listed were done; this marks the section as stopped so a rerun fills the gap
        raise RuntimeError(f'pouet.net {", ".join(broken)} could not be fetched')


# ---------------------------------------------------------------- ratings and reviews
# reviews.json next to each title.json: user ratings and comments from zxart.ee,
# user score and full magazine review transcripts from Spectrum Computing
# (ZXDB/ZXSR). Anything not in English is translated, keeping the original.
# Refreshed when older than REVIEWS_DAYS (default 30).

SC = 'https://spectrumcomputing.co.uk'
REVIEWS_DAYS = int(os.environ.get('REVIEWS_DAYS') or 30)


def translate(text):
    """Returns (english, source language). Uses Google's free translate endpoint; on failure keeps the text."""
    if not text or not text.strip():
        return text, None
    out, lang = [], None
    chunks, cur = [], ''
    for para in text.split('\n'):
        if len(cur) + len(para) > 4000 and cur:
            chunks.append(cur)
            cur = ''
        cur += para + '\n'
    chunks.append(cur)
    try:
        for c in chunks:
            q = urllib.parse.urlencode({'client': 'gtx', 'sl': 'auto', 'tl': 'en', 'dt': 't', 'q': c})
            j = json.loads(fetch('https://translate.googleapis.com/translate_a/single', data=q.encode(),
                                 headers={'Content-Type': 'application/x-www-form-urlencoded'}, tries=3))
            out.append(''.join(seg[0] for seg in j[0] if seg and seg[0]))
            lang = lang or j[2]
        return ''.join(out).strip(), lang
    except Exception as e:
        log('translate', e)
        return text, None


def english(text, lang=None):
    """Fields for a review text: text in English, original and translated flag."""
    if not text:
        return {'language': lang, 'text': None, 'original': None, 'translated': False}
    if lang == 'en':
        return {'language': 'en', 'text': text, 'original': None, 'translated': False}
    tr, src = translate(text)
    if not src or src == 'en' or tr == text:
        return {'language': src or lang, 'text': text, 'original': None, 'translated': False}
    return {'language': src, 'text': tr, 'original': text, 'translated': True}


def plain(h):
    h = re.sub(r'<br\s*/?>', '\n', h or '')
    return html.unescape(re.sub(r'<[^>]+>', '', h)).strip()


def score_of(s):
    m = re.search(r'(\d+(?:\.\d+)?)\s*(?:%|/\s*(\d+))', s or '')
    if not m:
        return None, None
    return float(m.group(1)), float(m.group(2) or 100)


def sc_reviews(zxdb):
    """User score and magazine reviews from Spectrum Computing for one ZXDB entry."""
    ratings, reviews = [], []
    url = f'{SC}/entry/{zxdb}'
    page = fetch(url).decode('utf-8', 'replace')
    w = re.search(r'Score \(weighted\): </b></div><div[^>]*>([\d.]+)', page)
    v = re.search(r'Total votes: </b></div><div[^>]*>(\d+)', page)
    if w:
        ratings.append({'site': 'spectrumcomputing', 'url': url, 'score': float(w.group(1)), 'scale': 10,
                        'votes': int(v.group(1)) if v else None, 'version': None})
    i = page.find('>Magazine Reviews<')
    refs = []
    if i > 0:
        blk = page[i:page.find('panel panel-primary', i)]
        for row in blk.split('<div class="row zxdb_stripes">')[1:]:
            mag = plain(re.search(r'<b><a[^>]*>(.*?)</a></b>', row, re.S).group(1)) if '<b><a' in row else None
            iss = re.search(r'Issue: </b>\s*<a[^>]*>(.*?)<span', row, re.S)
            pdf = re.search(r'href="([^"]+)"[^>]*>\(VIEW\)', row) or re.search(r'href="([^"]+\.pdf)"', row)
            typ = re.search(r'Article Type: </b>(.*?)</div>', row, re.S)
            sc = re.search(r'Score: </b>(.*?)</div>', row, re.S)
            refs.append({'magazine': mag, 'issue': plain(iss.group(1)) if iss else None, 'url': pdf.group(1) if pdf else url,
                         'type': plain(typ.group(1)) if typ else None, 'score': plain(sc.group(1)) if sc else None})
    texts = {}
    if 'zxsr.php?id=' in page:
        zp = fetch(f'{SC}/zxsr.php?id={zxdb}').decode('utf-8', 'replace')
        for m in re.finditer(r'<div class="panel-heading p_white">(.*?)</div><div class="panel-body"><div class="panel-default">'
                             r'(.*?)<hr>(.*?)</div></div></div>', zp, re.S):
            head, body, foot = plain(m.group(1)).replace('\xa0', ' '), m.group(2), m.group(3)
            sc = re.search(r'Overall</th><td>(.*?)</td>', foot, re.S)
            texts[head] = (plain(body), plain(sc.group(1)) if sc else None)
    used = set()
    for r in refs:
        if r['type'] and not r['type'].startswith('Review'):
            continue
        key = next((h for h in texts if h not in used and r['magazine'] and r['magazine'].split(' (')[0].lower() in h.lower()
                    and (r['issue'] or '').split(' ')[0].lstrip('#') in h), None)
        text, tscore = texts.get(key, (None, None))
        if key:
            used.add(key)
        score, scale = score_of(r['score'] or tscore)
        date = re.search(r'\((\d{4}-\d{2}-\d{2})\)', key or '')
        who = re.search(r'Reviewer:\s*([^\n]+)', text or '')
        reviews.append({'site': 'spectrumcomputing', 'url': r['url'], 'author': who.group(1).strip() if who else None,
                        'date': date.group(1) if date else None, 'score': score, 'scale': scale, 'version': None,
                        'title': ' '.join(x for x in (r['magazine'], r['issue']) if x) or None} | english(text))
    for h, (text, tscore) in texts.items():
        if h not in used:
            score, scale = score_of(tscore)
            date = re.search(r'\((\d{4}-\d{2}-\d{2})\)', h)
            reviews.append({'site': 'spectrumcomputing', 'url': f'{SC}/zxsr.php?id={zxdb}', 'author': None,
                            'date': date.group(1) if date else None, 'score': score, 'scale': scale, 'version': None,
                            'title': h} | english(text))
    return ratings, reviews


def zxart_reviews(zid, prod):
    ratings, reviews = [], []
    url = f'https://zxart.ee/eng/software/?id={zid}'
    if prod and prod.get('votesAmount'):
        ratings.append({'site': 'zxart', 'url': url, 'score': float(prod.get('votes') or 0), 'scale': 5,
                        'votes': int(prod['votesAmount']), 'version': None})
    # A failed fetch raises, so this title keeps its old reviews.json and is tried again next run.
    try:
        cs = json.loads(fetch(f'https://zxart.ee/comments-data/id:{zid}/?lang=eng'))
    except RuntimeError as e:
        if 'HTTP Error 404' not in str(e):
            raise
        cs = []  # no comments page: nothing to keep
    stack = list(cs or [])
    while stack:
        c = stack.pop(0)
        stack += c.get('children') or []
        orig = html.unescape(c.get('originalContent') or plain(c.get('content'))).strip()
        if not orig:
            continue
        d = re.match(r'(\d\d)\.(\d\d)\.(\d{4})', c.get('date') or '')
        base = {'site': 'zxart', 'url': url, 'author': (c.get('author') or {}).get('name'),
                'date': f'{d.group(3)}-{d.group(2)}-{d.group(1)}' if d else None, 'score': None, 'scale': None,
                'version': None, 'title': None}
        if c.get('translated'):
            reviews.append(base | {'language': None, 'text': plain(c['translated']), 'original': orig, 'translated': True})
        else:
            reviews.append(base | english(orig))
    return ratings, reviews


def fill_party(t, p):
    """Give a zxart party placing its party name and year from pouet."""
    tf = os.path.join(TITLES, t, 'title.json')
    j = json.load(open(tf, encoding='utf-8'))
    party = j.get('party')
    pl = (p.get('placings') or [None])[0]
    if not party or party.get('name') or not pl:
        return
    party['name'] = html.unescape((pl.get('party') or {}).get('name') or '') or None
    party['year'] = int(pl['year']) if str(pl.get('year') or '').isdigit() else j.get('year')
    party['compo'] = party.get('compo') or pl.get('compo_name')
    with open(tf + '.part', 'w', encoding='utf-8') as f:
        json.dump(j, f, indent=2, ensure_ascii=False)
    os.replace(tf + '.part', tf)


def reviews(lib):
    todo = []
    for t in sorted(os.listdir(TITLES)):
        d = os.path.join(TITLES, t)
        try:
            j = json.load(open(os.path.join(d, 'title.json'), encoding='utf-8'))
        except Exception:
            continue
        ids = j.get('ids') or {}
        if not (ids.get('zxart') or ids.get('zxdb') or ids.get('pouet')):
            continue
        rf = os.path.join(d, 'reviews.json')
        if os.path.exists(rf) and time.time() - os.path.getmtime(rf) < REVIEWS_DAYS * 86400:
            continue
        todo.append((t, ids))
    say(f'Ratings and reviews: {len(todo)} titles to check')
    prods, lost = {}, set()
    # A trial run fetches each title's zxart rating as it goes (below) rather than all of them up front.
    zids = [] if LIMIT else [str(i['zxart']) for _, i in todo if i.get('zxart')]
    for k in range(0, len(zids), 50):
        try:
            j = fetch_json(f'{ZX}/types:zxProd/export:zxProd/start:0/limit:50/filter:zxProdId={",".join(zids[k:k + 50])};')
            for p in j['responseData'].get('zxProd') or []:
                prods[str(p['id'])] = p
        except Exception as e:
            log('zxart votes', e)
            lost.update(zids[k:k + 50])
    # Titles whose zxart rating did not arrive keep their old reviews.json until the next run.
    todo = [x for x in todo if str(x[1].get('zxart')) not in lost]

    def one(item):
        t, ids = item
        ratings, revs = [], []
        if ids.get('zxart'):
            if LIMIT and str(ids['zxart']) not in prods:   # an error here leaves the title for the next run
                j = fetch_json(f'{ZX}/types:zxProd/export:zxProd/start:0/limit:1/filter:zxProdId={ids["zxart"]};')
                for p in j['responseData'].get('zxProd') or []:
                    prods[str(p['id'])] = p
            a, b = zxart_reviews(ids['zxart'], prods.get(str(ids['zxart'])))
            ratings += a
            revs += b
        if ids.get('zxdb'):
            a, b = sc_reviews(ids['zxdb'])
            ratings += a
            revs += b
        if ids.get('pouet'):
            a, b = pouet_reviews(ids['pouet'])
            ratings += a
            revs += b
            fill_party(t, pouet_prod(ids['pouet']))
        out = {'format': 1, 'title': t, 'updated': TODAY, 'ratings': ratings, 'reviews': revs}
        rf = os.path.join(TITLES, t, 'reviews.json')
        with open(rf + '.part', 'w', encoding='utf-8') as f:
            json.dump(out, f, indent=2, ensure_ascii=False)
        os.replace(rf + '.part', rf)
        n = sum(1 for r in revs if r.get('translated'))
        return f'{t}: {len(ratings)} ratings, {len(revs)} reviews' + (f' ({n} translated)' if n else '')
    batch_or_trial(todo, one, 'reviews')
    if lost:  # the others were refreshed; this marks the section as stopped so a rerun fills the gap
        raise RuntimeError(f'zxart.ee ratings for {len(lost)} titles could not be fetched')


# ---------------------------------------------------------------- main

def main():
    failed = []
    lib = Library()
    say(f'{len(lib.used)} titles already in the library')
    for s in SECTIONS:
        say(f'\n== {s} ==')
        t0, missed = time.time(), MISSED[0]
        try:
            if s in ZX_HW:
                made, broken = zxart(lib, s)
                if s == 'sam':
                    worldofsam(lib, made)
                if s == 'mb03':
                    mb03_site(lib, made)
                if broken:  # the rest was done; this marks the section as stopped so a rerun fills the gap
                    raise RuntimeError(f'zxart.ee {", ".join(broken)} could not be fetched')
            elif s in ('vtrd', 'msx', 'symbos', 'spec256', 'onedrive', 'demos', 'reviews'):
                globals()[s](lib)
            else:
                say(f'  unknown section {s}')
                failed.append(s)
        except KeyboardInterrupt:
            raise
        except Exception as e:
            say(f'  {s} stopped: {e}')
            log(s, 'stopped', e)
            failed.append(s)
        if MISSED[0] > missed and s not in failed:   # the others were filed; these wait for a rerun
            say(f'  {s}: {MISSED[0] - missed} titles could not be fetched')
            failed.append(s)
        say(f'== {s} done in {int(time.time() - t0)}s ==')
    n = sum(1 for t in os.listdir(TITLES) if os.path.exists(os.path.join(TITLES, t, 'title.json')))
    say(f'\nAll done. {n} titles in {TITLES}. Problems, if any, are listed in {os.path.join(LIB, "fetch.log")}.')
    if failed:
        say(f'These sections stopped early: {", ".join(failed)}. Run the same command again to carry on.')
        sys.exit(1)


try:
    main()
except KeyboardInterrupt:
    print('\nStopped. Run the same command again to carry on.')
    sys.exit(130)
PYEOF

python3 -u "$PY" "$TARGET" "$SECTIONS"
