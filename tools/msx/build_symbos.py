# build_symbos.py SYM_DIR FH_DL_DIR OUT_TITLES
# SymbOS apps from symbos.de (apps.json from apps.js, dl/<id>.zip, shots/<id>-<n>.gif)
# plus SymbOS itself and file-hunter's extra SymbOS files, as library titles.
# Apps run on the machines that run SymbOS, with needs ["symbos"].
import sys, os, re, json, zipfile, shutil, datetime
from PIL import Image

SYM, FHDL, OUT = sys.argv[1:4]
TODAY = datetime.date.today().isoformat()
CAT = ['all', 'utility', 'multimedia', 'game', 'network', 'desktop widget', 'system', 'programming', 'hardware', 'office']
KIND = {1: 'utility', 2: 'application', 3: 'game', 4: 'application', 5: 'utility', 6: 'system', 7: 'utility', 8: 'utility', 9: 'application'}
MSX = ['msx2', 'msx2eu']


def slug(s):
    return re.sub(r'[^a-z0-9]+', '-', s.lower()).strip('-')[:60].strip('-') or 'untitled'


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


def runs_for(platforms, files):
    pl = [p for p in platforms.split(',') if p]
    runs = []
    if platforms == '0' or '7' in pl:
        runs.append({'platform': 'zx-spectrum', 'machines': ['next'], 'files': files, 'slot': 'sd0', 'needs': ['symbos']})
    if platforms == '0' or '2' in pl:
        runs.append({'platform': 'msx', 'machines': MSX, 'files': files, 'slot': 'ide0', 'needs': ['symbos']})
    return runs


def app_files(files):
    """What goes on the SymbOS drive: the program and its data, not sources, readmes or disk images."""
    keep = [f for f in files if not f.startswith('files/sources/') and not f.lower().endswith(('.dsk', '#readme.txt', '.asm'))]
    if any(f.lower().endswith(('.exe', '.com')) for f in keep):
        return keep
    return [f for f in files if f.lower().endswith('-fat.dsk')] or [f for f in files if f.lower().endswith('.dsk')] or files


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
    tid = 'symbos-' + slug(name)
    tdir = os.path.join(OUT, tid)
    fdir = os.path.join(tdir, 'files')
    os.makedirs(fdir, exist_ok=True)
    files = ['files/' + f for f in unzip(z, fdir)]
    runs = runs_for(platforms, app_files(files))
    if not runs:
        shutil.rmtree(tdir)
        skipped.append('%s (Amstrad CPC/PCW/EP only)' % name)
        continue
    shots = []
    sd = os.path.join(tdir, 'screens')
    for i in range(1, int(nshots or 0) + 1):
        g = os.path.join(SYM, 'shots', '%s-%d.gif' % (aid, i))
        if os.path.exists(g):
            os.makedirs(sd, exist_ok=True)
            Image.open(g).convert('RGB').save(os.path.join(sd, '%d.png' % (len(shots) + 1)))
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
    req = 'Needs SymbOS %s or newer, %s KB of memory%s.' % (osver, mem, (' and ' + add) if add else '')
    about = '# %s %s\n\n%s\n\n%s\n\n%s\n\nBy %s, released %s-%s-%s. From symbos.de.\n' % (
        name, ver, short, re.sub(r'<br>', '\n', long_), req, author, y, m, d)
    write(tid, meta, about)
    index.append(tid)

# SymbOS itself, and extras from file-hunter.com OS/SymbOS not on symbos.de's app list.
osdir = os.path.join(SYM, 'os')
systems = [
    ('symbos-msx', 'SymbOS 4.0 for MSX', 'SymbOS-MSX40.zip', 'symbos-msx40.zip', MSX, 'disk0', ['msx-disk'],
     'The SymbOS 4.0 MSX package: the MSX-DOS based setup tool and update files. Install it to a Nextor hard disk to run SymbOS.'),
    ('symbos-next', 'SymbOS 4.0 for ZX Spectrum Next', 'SymbOS-ZNX40.zip', 'symbos-znx40.zip', ['next'], 'sd0', [],
     'The SymbOS 4.0 mass storage version for the ZX Spectrum Next.'),
    ('symbos-mass-storage-completion', 'SymbOS 4.0 Mass Storage Completion', 'SymbOS-40-MassStorageCompletion.zip',
     'SymbOS-40-MassStorageCompletion.zip', None, None, ['symbos'],
     'Add this after the platform setup for a full installation: all the standard applications.'),
    ('symbos-asm-developer-kit', 'SymbOS ASM Developer Kit 1.0', 'SymbOS-ASM-Developer-kit-1.0.zip',
     'SymbOS-ASM-Developer-kit-1.0.zip', None, None, ['symbos'], 'NYYRIKKI\'s tutorial and system library for writing SymbOS programs in Z80 assembly.'),
]
for tid, name, remote, local, machines, slot, needs, text in systems:
    z = os.path.join(osdir, local)
    if not os.path.exists(z) or not zipfile.is_zipfile(z):
        continue
    fdir = os.path.join(OUT, tid, 'files')
    os.makedirs(fdir, exist_ok=True)
    files = ['files/' + f for f in unzip(z, fdir)]
    runs = [{'platform': 'msx' if m != ['next'] else 'zx-spectrum', 'machines': m, 'files': files, 'slot': s, 'needs': needs}
            for m, s in ([(machines, slot)] if machines else [(['next'], 'sd0'), (MSX, 'ide0')])]
    meta = {'format': 2, 'id': tid, 'name': name, 'kind': 'system' if 'Kit' not in name else 'utility', 'year': 2025,
            'authors': ['Prodatron', 'SymbiosiS'], 'tags': ['symbos', 'operating system'], 'summary': text,
            'license': 'freeware', 'source': 'http://www.symbos.de/download/20250131-V40/' + remote,
            'added': TODAY, 'runs': runs, 'compat': {}}
    write(tid, meta, '# %s\n\n%s\n\nFrom symbos.de (download area).\n' % (name, text))
    index.append(tid)
man = os.path.join(osdir, 'SymbOS-40-Manual.pdf')
if os.path.exists(man):
    for t in ('symbos-next', 'symbos-msx'):
        if os.path.exists(os.path.join(OUT, t, 'title.json')):   # only titles actually built above
            shutil.copy(man, os.path.join(OUT, t, 'SymbOS-40-Manual.pdf'))

fh = os.path.join(FHDL, 'OS', 'SymbOS')
extras = [('symbos-hdd-4-0', 'SymbOS 4.0 hard disk image (File-Hunter)', 'SymbOS 4.0 Harddisk Image [File-Hunter.com].zip', MSX, 'ide0', ['nextor'],
           'A ready-made MSX hard disk image with SymbOS 4.0 installed, made by File-Hunter. Boots from Nextor on the Sunrise IDE.'),
          ('symbos-hdd-3-1', 'SymbOS 3.1 hard disk image (File-Hunter)', 'SymbOS 3.1 Harddisk Image [File-Hunter.com].zip', MSX, 'ide0', ['nextor'],
           'A ready-made MSX hard disk image with SymbOS 3.1 installed.'),
          ('symbos-msx-drivers', 'SymbOS MSX drivers', 'SymbOS-MSX Drivers.zip', MSX, 'ide0', ['symbos'], 'Extra SymbOS device drivers for MSX.')]
for tid, name, src, machines, slot, needs, text in extras:
    z = os.path.join(fh, src)
    if not os.path.exists(z):
        continue
    fdir = os.path.join(OUT, tid, 'files')
    os.makedirs(fdir, exist_ok=True)
    files = ['files/' + f for f in unzip(z, fdir)]
    runf = [f for f in files if f.lower().endswith(('.dsk', '.hdd', '.img', '.vhd'))] or files
    meta = {'format': 2, 'id': tid, 'name': name, 'kind': 'system', 'authors': ['Prodatron', 'SymbiosiS'],
            'tags': ['symbos', 'operating system'], 'summary': text, 'license': 'freeware',
            'source': 'https://download.file-hunter.com/OS/SymbOS/' + src, 'added': TODAY,
            'runs': [{'platform': 'msx', 'machines': machines, 'files': runf, 'slot': slot, 'needs': needs}], 'compat': {}}
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
        tid = 'symbos-' + slug(base)
        if os.path.exists(os.path.join(OUT, tid)):
            continue
        fdir = os.path.join(OUT, tid, 'files')
        os.makedirs(fdir, exist_ok=True)
        files = ['files/' + x for x in unzip(os.path.join(d, f), fdir)]
        meta = {'format': 2, 'id': tid, 'name': base.replace('_', ' '), 'kind': 'utility' if sub == 'Development' else 'application',
                'tags': ['symbos', 'symbos-app'] + (['development'] if sub == 'Development' else []), 'license': 'freeware',
                'source': 'https://download.file-hunter.com/OS/SymbOS/%s/%s' % (sub, f), 'added': TODAY,
                'runs': runs_for('0', app_files(files)), 'compat': {}}
        write(tid, meta, '# %s\n\nSymbOS %s from download.file-hunter.com, OS/SymbOS/%s.\n' % (meta['name'], 'development tool' if sub == 'Development' else 'application', sub))
        index.append(tid)

json.dump({'titles': index, 'skipped': skipped}, open(os.path.join(OUT, '..', 'symbos-index.json'), 'w'), indent=1)
print('symbos written', len(index), 'skipped', len(skipped))
