# build_msx.py DL_DIR META_DIR OUT_TITLES
# Turns the file-hunter.com MSX mirror (Games/MSX1, Games/MSX2, Demos) into
# library titles in title.json format 2. One title per game; each variant
# (ROM, disk set, tape, binary, alternate dump, hack, translation) is a run.
import sys, os, re, json, zipfile, shutil, subprocess, hashlib, unicodedata, datetime
import xml.etree.ElementTree as ET

DL, META, OUT = sys.argv[1:4]
TODAY = datetime.date.today().isoformat()
FH = 'https://download.file-hunter.com/'
MSX1 = ['msx1', 'msx1eu', 'msx2', 'msx2eu']
MSX2 = ['msx2', 'msx2eu']
LANGS = {'ja', 'en', 'es', 'nl', 'pt', 'fr', 'de', 'it', 'ar', 'ko', 'ru', 'fi', 'el', 'sv', 'pl', 'br', 'ca', 'en-ja', 'ja-en'}
RUNNABLE = {'.rom', '.mx1', '.mx2', '.dsk', '.cas', '.di1', '.di2', '.bin', '.com', '.bas', '.ldr', '.hdd', '.img', '.vhd', '.dmk', '.tsx'}
TYPE_RANK = {'ROM': 0, 'DSK': 1, 'HDD': 2, 'BIN': 3, 'CAS': 4}

# ---- metadata sources --------------------------------------------------------

def norm(s):
    s = unicodedata.normalize('NFKD', s).encode('ascii', 'ignore').decode().lower()
    m = re.match(r'^(.*), (the|a|an|el|la|le|les|los|las|de|het|die|der|das|o|il)$', s.strip())
    if m:
        s = m.group(2) + ' ' + m.group(1)
    s = re.sub(r'^(the|a|an) ', '', s)
    return re.sub(r'[^a-z0-9]+', '', s)


def slug(s):
    s = unicodedata.normalize('NFKD', s).encode('ascii', 'ignore').decode().lower()
    return re.sub(r'[^a-z0-9]+', '-', s).strip('-')[:60].strip('-') or 'untitled'


xl = {}
try:
    import openpyxl
    wb = openpyxl.load_workbook(os.path.join(META, 'pics.xlsx'), read_only=True)
    for i, r in enumerate(wb['Games'].iter_rows(values_only=True)):
        if i == 0 or not r[0]:
            continue
        xl.setdefault(norm(str(r[0])), dict(zip(['name', 'genre', 'year', 'developer', 'publisher', 'company', 'type', 'model', 'score'], r)))
except Exception as e:
    print('xlsx', e)
xlimg = {norm(k): v for k, v in json.load(open(os.path.join(META, 'xlimg.json'))).items()}

sdb = {}
for sw in ET.parse(os.path.join(META, 'softwaredb.xml')).getroot().iter('software'):
    for rom in sw.iter('rom'):
        sdb[rom.get('sha1')] = dict(title=sw.get('title'), company=sw.get('company'), year=sw.get('year'),
                                    country=sw.get('country'), genmsx=sw.get('genmsxid'), mapper=rom.get('type'),
                                    status=rom.get('status'), remark=rom.get('remark'))

# ---- file name parsing ---------------------------------------------------------

def parse(base, typ):
    """Split a file-hunter / TOSEC name into title, year, publisher, lang, part, flags."""
    b = re.sub(r'\.(zip|lzh|7z|rar)$', '', base, flags=re.I)
    i = min([x for x in (b.find(' ('), b.find(' [')) if x >= 0] or [len(b)])
    title, rest = b[:i].strip(), b[i:]
    parens = re.findall(r'\(([^()]*)\)', rest)
    flags = re.findall(r'\[([^\]]*)\]', rest)
    year = next((p for p in parens if re.fullmatch(r'(19|20)[0-9x]{2}', p)), None)
    lang = next((p for p in parens if p.lower() in LANGS), None)
    part = [p for p in parens if re.match(r'(disk|tape|side|disc|part) ', p, re.I) or re.fullmatch(r'side [ab]', p, re.I)]
    others = [p for p in parens if p not in part and p != year and p != lang and p.strip() not in ('', '-')]
    pub = others[0] if others else None
    # ROM style: "Title - Publisher (Year) ..." or "Title (Year) Publisher [...]"
    if typ == 'ROM':
        m = re.match(r'^\s*\((\d{4}|19xx)\)\s*([^\[(]+)', rest)
        if m and not pub:
            pub = m.group(2).strip()
        if ' - ' in title and not pub and norm(title) not in xl:
            title, pub = title.rsplit(' - ', 1)
    fid = next((f for f in flags if re.fullmatch(r'\d{3,6}', f)), None)
    flags = [f for f in flags if f != fid]
    return dict(title=title, year=year, pub=pub, lang=lang, part=part, flags=flags, fhid=fid)


def is_bad(flags):
    return any(re.fullmatch(r'b\d*( .*)?', f) for f in flags)


def variant_rank(p):
    f = [x.lower() for x in p['flags']]
    r = 0
    if any(x in ('goodmsx', 'original') for x in f): r -= 1
    if any(re.match(r'a\d*$', x) for x in f): r += 1
    if any(x.startswith(('h ', 'hack', 't ', 'cr', 'p ', 'f ')) or x in ('h', 't', 'p', 'f', 'cr') or 'hack' in x for x in f): r += 2
    if any(x.startswith('tr ') or 'transl' in x for x in f): r += 1
    if p['lang'] and p['lang'] not in ('en',): r += 1
    return r


def unpack(path, dest):
    os.makedirs(dest, exist_ok=True)
    low = path.lower()
    try:
        if low.endswith('.zip') and zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as z:
                for m in z.infolist():
                    if m.is_dir():
                        continue
                    n = m.filename
                    if m.flag_bits & 0x800 == 0:
                        try:
                            n = n.encode('cp437').decode('cp437')
                        except Exception:
                            pass
                    parts = [re.sub(r'[\x00-\x1f]', '_', x) for x in n.replace('\\', '/').split('/') if x not in ('', '.', '..')]
                    if not parts:
                        continue
                    out = os.path.join(dest, *parts)
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with z.open(m) as s, open(out, 'wb') as d:
                        shutil.copyfileobj(s, d)
            return True
        if low.endswith(('.lzh', '.lha')):
            return subprocess.run(['lha', '-xqw=' + dest, path], capture_output=True).returncode == 0
        if low.endswith(('.7z', '.rar')):
            return subprocess.run(['7z', 'x', '-y', '-o' + dest, path], capture_output=True).returncode == 0
    except Exception as e:
        print('  unpack failed', path, e)
        return False
    shutil.copy(path, os.path.join(dest, os.path.basename(path)))
    return True


def sha1(p):
    h = hashlib.sha1()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''):
            h.update(c)
    return h.hexdigest()


def safe(n):
    b = n.encode('utf-8', 'surrogateescape')
    try:
        n = b.decode('utf-8')
    except UnicodeDecodeError:
        try:
            n = b.decode('cp932')
        except UnicodeDecodeError:
            n = b.decode('latin-1')
    return re.sub(r'[\x00-\x1f/\\:*?"<>|]', '_', n)

# ---- collect variants ----------------------------------------------------------

SRC = {  # folder -> (type, machines, slot, needs)
    'Games/MSX1/ROM': ('ROM', MSX1, 'cart1', []),
    'Games/MSX1/DSK': ('DSK', MSX1, 'disk0', ['msx-disk']),
    'Games/MSX1/CAS': ('CAS', MSX1, 'tape', ['msx-tape']),
    'Games/MSX1/MSX-Binaries': ('BIN', MSX1, 'ide0', ['nextor']),
    'Games/MSX1/HDD': ('HDD', MSX1, 'ide0', ['nextor']),
    'Games/MSX2/ROM': ('ROM', MSX2, 'cart1', []),
    'Games/MSX2/DSK': ('DSK', MSX2, 'disk0', ['msx-disk']),
    'Games/MSX2/CAS': ('CAS', MSX2, 'tape', ['msx-tape']),
    'Games/MSX2/MSX-Binaries': ('BIN', MSX2, 'ide0', ['nextor']),
    'Games/MSX2/Harddisk': ('HDD', MSX2, 'ide0', ['nextor']),
}

titles = {}      # key -> {'variants': {vkey: {...}}, ...}
for folder, (typ, machines, slot, needs) in SRC.items():
    d = os.path.join(DL, folder)
    if not os.path.isdir(d):
        continue
    for fn in sorted(os.listdir(d)):
        if fn.endswith('.part'):
            continue
        p = parse(fn, typ)
        if is_bad(p['flags']):
            continue
        if typ == 'BIN':   # binaries are named by 8.3 file names; use them as-is
            p['title'] = re.sub(r'\.(lzh|zip)$', '', fn, flags=re.I)
        key = norm(p['title']) or norm(fn)
        x = xl.get(key)
        t = titles.setdefault(key, {'names': [], 'variants': {}, 'machines': set(), 'years': [], 'pubs': [], 'langs': set()})
        t['names'].append(p['title'])
        t['machines'].update(machines)
        if p['year'] and 'x' not in p['year']:
            t['years'].append(int(p['year']))
        if p['pub']:
            t['pubs'].append(p['pub'])
        if p['lang']:
            t['langs'].add(p['lang'])
        vkey = typ + '|' + folder + '|' + re.sub(r'\((disk|tape|side|disc|part) [^)]*\)', '', fn, flags=re.I)
        v = t['variants'].setdefault(vkey, dict(typ=typ, folder=folder, machines=machines, slot=slot, needs=needs, parse=p, srcs=[]))
        v['srcs'].append(fn)

print('titles', len(titles), 'variants', sum(len(t['variants']) for t in titles.values()))

# ---- write titles ---------------------------------------------------------------

os.makedirs(OUT, exist_ok=True)
used = set(os.listdir(OUT))
index = []
only = set(sys.argv[4].split(',')) if len(sys.argv) > 4 else None
for key, t in sorted(titles.items()):
    x = xl.get(key)
    name = (x and x['name']) or t['names'][0]
    m = re.match(r'^(.*), (The|A|An|El|La|Le|Les|Los|De|Het|Die|Der|O|Il)$', name)
    if m:
        name = m.group(2) + ' ' + m.group(1)
    tid = 'msx-' + slug(name)
    base = tid
    n = 2
    while tid in used:
        tid = '%s-%d' % (base, n); n += 1
    used.add(tid)
    if only and key not in only:
        continue
    tdir = os.path.join(OUT, tid)
    fdir = os.path.join(tdir, 'files')
    os.makedirs(fdir, exist_ok=True)
    runs, allfiles, sdbhit, sources = [], [], None, []
    vs = sorted(t['variants'].values(), key=lambda v: (TYPE_RANK[v['typ']], variant_rank(v['parse']), v['srcs'][0]))
    for v in vs:
        got = []
        for src in sorted(v['srcs'], key=lambda s: [int(c) if c.isdigit() else c for c in re.split(r'(\d+)', s)]):
            sp = os.path.join(DL, v['folder'], src)
            sources.append(FH + v['folder'] + '/' + src)
            tmp = os.path.join(tdir, '.x')
            shutil.rmtree(tmp, ignore_errors=True)
            if not unpack(sp, tmp):
                continue
            for root, _, fs in os.walk(tmp):
                for f in sorted(fs):
                    full = os.path.join(root, f)
                    rel = os.path.relpath(full, tmp)
                    dst = safe(rel.replace(os.sep, ' - '))
                    if len(vs) > 1 and v['typ'] == 'BIN':
                        dst = os.path.join(safe(re.sub(r'\.\w+$', '', src)), safe(rel.replace(os.sep, '/')))
                    k = 2
                    stem, ext = os.path.splitext(dst)
                    while os.path.exists(os.path.join(fdir, dst)):
                        dst = '%s (%d)%s' % (stem, k, ext); k += 1
                    os.makedirs(os.path.dirname(os.path.join(fdir, dst)), exist_ok=True)
                    shutil.move(full, os.path.join(fdir, dst))
                    got.append('files/' + dst)
            shutil.rmtree(tmp, ignore_errors=True)
        if not got:
            continue
        allfiles += got
        runf = [g for g in got if os.path.splitext(g)[1].lower() in RUNNABLE] or got
        if v['typ'] == 'BIN':
            runf = got     # the whole program folder goes on the host disk
        run = dict(platform='msx', machines=v['machines'], files=runf, slot=v['slot'], needs=list(v['needs']))
        p = v['parse']
        notes = []
        if p['flags']:
            notes.append(' '.join('[%s]' % f for f in p['flags']))
        if p['lang']:
            notes.append('language: ' + p['lang'])
        if v['typ'] == 'ROM':
            for g in runf:
                h = sdb.get(sha1(os.path.join(tdir, g)))
                if h:
                    sdbhit = sdbhit or h
                    run['mapper'] = h['mapper']
                    if h.get('status'):
                        notes.append('openMSX: ' + h['status'] + (' - ' + h['remark'] if h.get('remark') else ''))
                    break
        if notes:
            run['notes'] = '; '.join(notes)
        runs.append(run)
    if not runs:
        shutil.rmtree(tdir, ignore_errors=True)
        continue
    year = (x and x.get('year') and str(x['year']).isdigit() and int(x['year'])) or (sdbhit and sdbhit['year'] and sdbhit['year'].isdigit() and int(sdbhit['year'])) or (t['years'] and min(t['years'])) or None
    pub = (x and x.get('publisher') not in (None, 'unknown') and x['publisher']) or (sdbhit and sdbhit['company']) or (t['pubs'] and t['pubs'][0]) or None
    meta = {'format': 2, 'id': tid, 'name': name, 'kind': 'game'}
    if year: meta['year'] = year
    if x and x.get('developer') not in (None, 'unknown'): meta['authors'] = [x['developer']]
    if pub: meta['publisher'] = pub
    if x and x.get('genre'): meta['genres'] = [x['genre'].lower()]
    tags = ['msx'] + (['msx2'] if 'msx1' not in t['machines'] else ['msx1'])
    tags += sorted('lang:' + l for l in t['langs'])
    meta['tags'] = tags
    meta['license'] = 'unknown'
    meta['source'] = sources[0]
    meta['sources'] = sources
    meta['added'] = TODAY
    ids = {}
    if sdbhit and sdbhit.get('genmsx'): ids['genmsx'] = int(sdbhit['genmsx'])
    fhids = sorted({v['parse']['fhid'] for v in vs if v['parse']['fhid']})
    if fhids: ids['filehunter'] = [int(i) for i in fhids]
    if ids: meta['ids'] = ids
    meta['runs'] = runs
    meta['compat'] = {}
    shots = []
    for i, im in enumerate(xlimg.get(key, [])[:4]):
        sd = os.path.join(tdir, 'screens')
        os.makedirs(sd, exist_ok=True)
        shutil.copy(os.path.join(META, im['file']), os.path.join(sd, '%d.png' % (i + 1)))
        shots.append({'file': 'screens/%d.png' % (i + 1), 'caption': 'File-Hunter games list picture'})
    if shots: meta['screens'] = shots
    with open(os.path.join(tdir, 'title.json'), 'w') as f:
        json.dump(meta, f, indent=2, ensure_ascii=False)
    about = ['# ' + name, '']
    if x:
        about.append('%s, %s. Developer %s, publisher %s.' % (x.get('genre') or 'Game', x.get('year') or 'year unknown', x.get('developer') or '?', x.get('publisher') or '?'))
        about.append('')
    about.append('Runs, best first:')
    for r in runs:
        about.append('- %s on %s (%s)%s' % (', '.join(os.path.basename(f) for f in r['files'][:4]), ', '.join(r['machines']), r['slot'], (': ' + r['notes']) if r.get('notes') else ''))
    about += ['', 'Files from download.file-hunter.com (The File Hunter MSX archive), %s.' % TODAY]
    with open(os.path.join(tdir, 'about.md'), 'w') as f:
        f.write('\n'.join(about) + '\n')
    index.append({'id': tid, 'name': name, 'machines': sorted(t['machines']), 'runs': len(runs), 'screens': len(shots)})

json.dump(index, open(os.path.join(OUT, '..', 'msx-index.json'), 'w'), indent=0)
print('written', len(index))
