# build_symbos.py SYM_DIR FH_DL_DIR OUT_TITLES
# SymbOS apps from symbos.de (apps.json from apps.js, dl/<id>.zip, shots/<id>-<n>.gif)
# plus SymbOS itself and file-hunter's extra SymbOS files, as library titles.
# Apps are copied onto a SymbOS drive, not mounted, so they get no runs; their
# about.md says which machines they are for. The SD and IDE slots take block
# media only, which the player cannot build from bytes yet, so the SymbOS
# packages and disk images have no runs either.
import sys, os, re, json, zipfile, shutil, datetime
from PIL import Image

SYM, FHDL, OUT = sys.argv[1:4]
TODAY = datetime.date.today().isoformat()
CAT = ['all', 'utility', 'multimedia', 'game', 'network', 'desktop widget', 'system', 'programming', 'hardware', 'office']
KIND = {1: 'utility', 2: 'application', 3: 'game', 4: 'application', 5: 'utility', 6: 'system', 7: 'utility', 8: 'utility', 9: 'application'}
MSX = ['msx2', 'msx2eu']


def slug(s):
    return re.sub(r'[^a-z0-9]+', '-', s.lower()).strip('-')[:60].strip('-') or 'untitled'


def good_zip(path):
    # checked before claim() replaces an existing title
    try:
        with zipfile.ZipFile(path) as z:
            return z.testzip() is None
    except (zipfile.BadZipFile, OSError, EOFError):
        return False


def unzip(path, dest):
    with zipfile.ZipFile(path) as z:
        out = []
        for m in z.infolist():
            if m.is_dir():
                continue
            parts = [p for p in m.filename.replace('\\', '/').split('/') if p not in ('', '.', '..')]
            if not parts:
                continue
            o = os.path.join(dest, *parts)
            os.makedirs(os.path.dirname(o), exist_ok=True)
            with z.open(m) as s, open(o, 'wb') as d:
                shutil.copyfileobj(s, d)
            out.append('/'.join(parts))
        return out


INSIDE = 'Copy files/ onto the drive of a set-up that boots SymbOS, then start it from there.'


def platforms_of(platforms):
    pl = [p for p in platforms.split(',') if p]
    return [n for n, k in (('ZX Spectrum Next', '7'), ('MSX2', '2')) if platforms == '0' or k in pl]


MARK = '.build_symbos'
used = set()


def ours(tdir):
    """A folder an earlier run of this builder wrote, safe to rebuild in place."""
    if os.path.exists(os.path.join(tdir, MARK)):
        return True
    try:
        about = open(os.path.join(tdir, 'about.md')).read()
    except OSError:
        return False
    return 'From symbos.de' in about or 'OS/SymbOS' in about


def claim(base):
    """A fresh folder for a title: ours from an earlier run is rebuilt, anyone else's is left alone."""
    tid, n = base, 2
    while tid in used or (os.path.exists(os.path.join(OUT, tid)) and not ours(os.path.join(OUT, tid))):
        tid, n = '%s-%d' % (base, n), n + 1
    used.add(tid)
    tdir = os.path.join(OUT, tid)
    shutil.rmtree(tdir, ignore_errors=True)
    os.makedirs(os.path.join(tdir, 'files'))
    open(os.path.join(tdir, MARK), 'w').close()
    return tid


def write(tid, meta, about):
    tdir = os.path.join(OUT, tid)
    with open(os.path.join(tdir, 'title.json'), 'w') as f:
        json.dump(meta, f, indent=2, ensure_ascii=False)
    with open(os.path.join(tdir, 'about.md'), 'w') as f:
        f.write(about)


os.makedirs(OUT, exist_ok=True)
index, skipped, seen = [], [], set()
for a in json.load(open(os.path.join(SYM, 'apps.json'))):
    aid, name, ver, date, _, author, genre, platforms, osver, mem, add, thumb, nshots, shotname, flags, short, long_ = a[:17]
    url = a[17] if len(a) > 17 else ''
    z = os.path.join(SYM, 'dl', aid + '.zip')
    seen.add(slug(name).replace('-', ''))
    if not os.path.exists(z):
        skipped.append(name + ' (part of the SymbOS core package)')
        continue
    if not good_zip(z):
        skipped.append(name + ' (damaged download)')
        continue
    tid = claim('symbos-' + slug(name))
    tdir = os.path.join(OUT, tid)
    fdir = os.path.join(tdir, 'files')
    files = ['files/' + f for f in unzip(z, fdir)]
    on = platforms_of(platforms)
    runs = []
    if not on:
        shutil.rmtree(tdir)
        skipped.append('%s (Amstrad CPC/PCW/EP only)' % name)
        continue
    shots = []
    sd = os.path.join(tdir, 'screens')
    for i in range(1, int(nshots or 0) + 1):
        g = os.path.join(SYM, 'shots', '%s-%d.gif' % (aid, i))
        if os.path.exists(g):
            os.makedirs(sd, exist_ok=True)
            try:
                Image.open(g).convert('RGB').save(os.path.join(sd, '%d.png' % (len(shots) + 1)))
            except (OSError, ValueError) as e:   # truncated, or an error page saved as .gif
                print('  bad screenshot', g, e)
                continue
            shots.append({'file': 'screens/%d.png' % (len(shots) + 1), 'caption': '%s screenshot' % name})
    d, m, y = date.split('.')
    meta = {'format': 2, 'id': tid, 'name': name, 'kind': KIND.get(genre, 'application'), 'year': int(y),
            'version': ver, 'authors': [x.strip() for x in author.split('/')],
            'genres': [CAT[genre]] if genre else [], 'tags': ['symbos', 'symbos-app'] + ([add.lower()] if add else []),
            'summary': short, 'license': 'freeware',
            'source': 'http://www.symbos.de/appinfo.htm?' + aid,
            'sources': ['http://www.symbos.de/appinfo.htm?' + aid, 'http://www.symbos.de/download/apps/' + re.sub(r'[ \'"()/\\]', '', name) + '.zip'] + ([url] if url else []),
            'added': TODAY, 'ids': {'symbos': aid}, 'runs': runs, 'compat': {}}
    if shots:
        meta['screens'] = shots
    req = 'Needs SymbOS %s or newer, %s KB of memory%s. For %s: %s' % (osver, mem, (' and ' + add) if add else '', ' and '.join(on), INSIDE)
    about = '# %s %s\n\n%s\n\n%s\n\n%s\n\nBy %s, released %s-%s-%s. From symbos.de.\n' % (
        name, ver, short, re.sub(r'<br>', '\n', long_), req, author, y, m, d)
    write(tid, meta, about)
    index.append(tid)

# SymbOS itself, and extras from file-hunter.com OS/SymbOS not on symbos.de's app list.
osdir = os.path.join(SYM, 'os')
systems = [
    ('symbos-msx', 'SymbOS 4.0 for MSX', 'SymbOS-MSX40.zip', 'symbos-msx40.zip', MSX, 'disk0', [],
     'The SymbOS 4.0 MSX package: the MSX-DOS based setup tool and update files. Install it to a Nextor hard disk to run SymbOS.'),
    ('symbos-next', 'SymbOS 4.0 for ZX Spectrum Next', 'SymbOS-ZNX40.zip', 'symbos-znx40.zip', ['next'], 'sd0', [],
     'The SymbOS 4.0 mass storage version for the ZX Spectrum Next. Its files go onto a NextZXOS SD card (the SymbOS recipe does that).'),
    ('symbos-mass-storage-completion', 'SymbOS 4.0 Mass Storage Completion', 'SymbOS-40-MassStorageCompletion.zip',
     'SymbOS-40-MassStorageCompletion.zip', None, None, ['symbos'],
     'Add this after the platform setup for a full installation: all the standard applications.'),
    ('symbos-asm-developer-kit', 'SymbOS ASM Developer Kit 1.0', 'SymbOS-ASM-Developer-kit-1.0.zip',
     'SymbOS-ASM-Developer-kit-1.0.zip', None, None, ['symbos'], 'NYYRIKKI\'s tutorial and system library for writing SymbOS programs in Z80 assembly.'),
]
built = {}
for tid, name, remote, local, machines, slot, needs, text in systems:
    z = os.path.join(osdir, local)
    if not os.path.exists(z) or not good_zip(z):
        continue
    key, tid = tid, claim(tid)
    built[key] = tid   # by the plain id: claim() may have added -2
    fdir = os.path.join(OUT, tid, 'files')
    files = ['files/' + f for f in unzip(z, fdir)]
    runs = []
    meta = {'format': 2, 'id': tid, 'name': name, 'kind': 'system' if 'Kit' not in name else 'utility', 'year': 2025,
            'authors': ['Prodatron', 'SymbiosiS'], 'tags': ['symbos', 'operating system'], 'summary': text,
            'license': 'freeware', 'source': 'http://www.symbos.de/download/20250131-V40/' + remote,
            'added': TODAY, 'runs': runs, 'compat': {}}
    write(tid, meta, '# %s\n\n%s\n\nFrom symbos.de (download area).\n' % (name, text))
    index.append(tid)
man = os.path.join(osdir, 'SymbOS-40-Manual.pdf')
if os.path.exists(man):
    for t in (built.get('symbos-next'), built.get('symbos-msx')):
        if t:   # only titles actually built above
            shutil.copy(man, os.path.join(OUT, t, 'files', 'SymbOS-40-Manual.pdf'))

fh = os.path.join(FHDL, 'OS', 'SymbOS')
extras = [('symbos-hdd-4-0', 'SymbOS 4.0 hard disk image (File-Hunter)', 'SymbOS 4.0 Harddisk Image [File-Hunter.com].zip', MSX, 'ide0', [],
           'A ready-made MSX hard disk image with SymbOS 4.0 installed, made by File-Hunter. Boots from Nextor on the Sunrise IDE.'),
          ('symbos-hdd-3-1', 'SymbOS 3.1 hard disk image (File-Hunter)', 'SymbOS 3.1 Harddisk Image [File-Hunter.com].zip', MSX, 'ide0', [],
           'A ready-made MSX hard disk image with SymbOS 3.1 installed.'),
          ('symbos-msx-drivers', 'SymbOS MSX drivers', 'SymbOS-MSX Drivers.zip', MSX, 'ide0', ['symbos'], 'Extra SymbOS device drivers for MSX.')]
for tid, name, src, machines, slot, needs, text in extras:
    z = os.path.join(fh, src)
    if not os.path.exists(z) or not good_zip(z):
        continue
    tid = claim(tid)
    fdir = os.path.join(OUT, tid, 'files')
    files = ['files/' + f for f in unzip(z, fdir)]
    meta = {'format': 2, 'id': tid, 'name': name, 'kind': 'system', 'authors': ['Prodatron', 'SymbiosiS'],
            'tags': ['symbos', 'operating system'], 'summary': text, 'license': 'freeware',
            'source': 'https://download.file-hunter.com/OS/SymbOS/' + src, 'added': TODAY,
            'runs': [], 'compat': {}}
    write(tid, meta, '# %s\n\n%s\n\nFrom download.file-hunter.com, OS/SymbOS.\n' % (name, text))
    index.append(tid)
# file-hunter apps and development tools not on symbos.de's list
for sub in ('Apps', 'Development'):
    d = os.path.join(fh, sub)
    if not os.path.isdir(d):
        continue
    for f in sorted(os.listdir(d)):
        base = re.sub(r'\.zip$', '', f, flags=re.I)
        k = slug(base).replace('-', '')
        if k in seen or any(k.startswith(s) or s.startswith(k) for s in seen if len(s) > 3) or not f.lower().endswith('.zip'):
            continue
        seen.add(k)
        if not good_zip(os.path.join(d, f)):
            skipped.append(base + ' (damaged download)')
            continue
        tid = claim('symbos-' + slug(base))
        fdir = os.path.join(OUT, tid, 'files')
        files = ['files/' + x for x in unzip(os.path.join(d, f), fdir)]
        meta = {'format': 2, 'id': tid, 'name': base.replace('_', ' '), 'kind': 'utility' if sub == 'Development' else 'application',
                'tags': ['symbos', 'symbos-app'] + (['development'] if sub == 'Development' else []), 'license': 'freeware',
                'source': 'https://download.file-hunter.com/OS/SymbOS/%s/%s' % (sub, f), 'added': TODAY,
                'runs': [], 'compat': {}}
        write(tid, meta, '# %s\n\nSymbOS %s from download.file-hunter.com, OS/SymbOS/%s.\n' % (meta['name'], 'development tool' if sub == 'Development' else 'application', sub))
        index.append(tid)

# A titles folder this builder made that no longer comes out of the archive (a game gone, or a
# renamed slug) is stale; only folders carrying the marker are touched.
for d in os.listdir(OUT):
    if d not in used and os.path.exists(os.path.join(OUT, d, MARK)):
        shutil.rmtree(os.path.join(OUT, d))
json.dump({'titles': index, 'skipped': skipped}, open(os.path.join(OUT, '..', 'symbos-index.json'), 'w'), indent=1)
print('symbos written', len(index), 'skipped', len(skipped))
