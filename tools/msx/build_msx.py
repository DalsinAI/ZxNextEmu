# build_msx.py DL_DIR META_DIR OUT_TITLES
# Turns the file-hunter.com MSX mirror (Games/MSX1, Games/MSX2, Demos) into
# library titles in title.json format 2. One title per game; each variant
# (ROM, disk set, tape, binary, alternate dump, hack, translation) is a run.
import sys, os, re, json, zipfile, gzip, shutil, subprocess, hashlib, unicodedata, datetime
import xml.etree.ElementTree as ET

DL, META, OUT = sys.argv[1:4]
TODAY = datetime.date.today().isoformat()
FH = 'https://download.file-hunter.com/'
MSX1 = ['msx1', 'msx1eu', 'msx2', 'msx2eu']
MSX2 = ['msx2', 'msx2eu']
LANG_WORDS = {'english': 'en', 'en': 'en', 'jp': 'ja', 'ja': 'ja', 'japanese': 'ja', 'spanish': 'es', 'es': 'es',
              'dutch': 'nl', 'nl': 'nl', 'portuguese': 'pt', 'pt': 'pt', 'brazilian': 'pt', 'french': 'fr', 'fr': 'fr',
              'german': 'de', 'de': 'de', 'italian': 'it', 'it': 'it', 'arabic': 'ar', 'ar': 'ar', 'korean': 'ko',
              'ko': 'ko', 'russian': 'ru', 'ru': 'ru', 'finnish': 'fi', 'greek': 'el', 'swedish': 'sv', 'polish': 'pl'}
LANGS = {'ja', 'jp', 'en', 'es', 'nl', 'pt', 'fr', 'de', 'it', 'ar', 'ko', 'ru', 'fi', 'el', 'sv', 'pl', 'br', 'ca', 'en-ja', 'ja-en', 'en-jp', 'jp-en'}
# Not .dmk (a track container, the disk drive takes flat sectors) or .cas/.tsx (no MSX tape slot yet).
RUNNABLE = {'.rom', '.mx1', '.mx2', '.dsk', '.di1', '.di2', '.bin', '.com', '.bas', '.ldr', '.hdd', '.img', '.vhd'}
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

# openMSX's softwaredb.xml. The current file keeps it all in attributes:
# <software title= company= year= country= genmsxid=> with <rom sha1= type= status= remark=/> children.
# Older copies use child elements: <title>, <company>, ..., then <dump> holding a <rom> or <megarom>
# with <type> (the mapper) and <hash algo="sha1">, and maybe <original value="true"> and <remark>.
# Both are read.
def _text(e, tag):
    x = e.find(tag)
    return ' '.join(''.join(x.itertext()).split()) or None if x is not None else None


def _field(e, tag):
    return (e.get(tag) or '').strip() or _text(e, tag)


sdb = {}
for sw in ET.parse(os.path.join(META, 'softwaredb.xml')).getroot().iter('software'):
    info = dict(title=_field(sw, 'title'), company=_field(sw, 'company'), year=_field(sw, 'year'),
                country=_field(sw, 'country'), genmsx=_field(sw, 'genmsxid'))
    for rom in [e for e in sw if e.tag in ('rom', 'megarom') and e.get('sha1')]:
        sdb[rom.get('sha1').strip().lower()] = dict(info, mapper=rom.get('type') or None,
                                                    status=rom.get('status') or None, remark=rom.get('remark') or None)
    for dump in sw.iter('dump'):
        orig = dump.find('original')
        status = (orig.text or '').strip() or None if orig is not None and orig.get('value') == 'true' else None
        for rom in [e for e in dump if e.tag in ('rom', 'megarom')]:
            for h in rom.iter('hash'):
                if h.get('algo', 'sha1') == 'sha1' and (h.text or '').strip():
                    sdb[h.text.strip().lower()] = dict(info, mapper=_text(rom, 'type') or ('Normal' if rom.tag == 'rom' else None),
                                                       status=status, remark=_text(rom, 'remark') or _text(dump, 'remark'))

# openMSX mapper names the cartridge core knows by a marker in the file name (src/msx/cart.c).
MAPPER_MARK = {'ascii8': '[ascii8]', 'ascii16': '[ascii16]', 'konami': '[konami]', 'konamiscc': '[konamiscc]',
               'normal': '[plain]', 'mirrored': '[plain]', 'plain': '[plain]'}


def mapper_ok(mapper):
    """Whether src/msx/cart.c can bank a ROM openMSX lists with this mapper: the four megaROM types,
    or a plain ROM of 64K or less at any page (Mirrored, Page23, 0x4000, 16kB, ...)."""
    k = re.sub(r'[^a-z0-9]', '', (mapper or 'normal').lower())
    return k in MAPPER_MARK or bool(re.fullmatch(r'mirrored\w*|page\d+|0x[0-9a-f]+|(8|16|32)kb|rom', k))


def shelve(tdir):
    """Move an earlier build aside while a title is rebuilt; settle() then drops or restores it."""
    old = os.path.join(os.path.dirname(tdir), '.old-' + os.path.basename(tdir))
    if os.path.exists(old) and not os.path.exists(tdir):   # a build cut off last time: start from that copy
        os.rename(old, tdir)
    shutil.rmtree(old, ignore_errors=True)
    if os.path.exists(tdir):
        os.rename(tdir, old)


def settle(tdir, ok):
    """Keep the new build when it got files; otherwise put the earlier one back (a damaged download)."""
    old = os.path.join(os.path.dirname(tdir), '.old-' + os.path.basename(tdir))
    if ok:
        shutil.rmtree(old, ignore_errors=True)
    else:
        shutil.rmtree(tdir, ignore_errors=True)
        if os.path.exists(old):
            os.rename(old, tdir)


def mark_mapper(tdir, path, mapper):
    """Rename a ROM so the core picks the mapper openMSX knows it by; returns its new path."""
    m = MAPPER_MARK.get(re.sub(r'[^a-z0-9]', '', (mapper or '').lower()))
    if not m or m in path.lower():
        return path
    stem, ext = os.path.splitext(path)
    new = '%s %s%s' % (stem, m, ext)
    os.rename(os.path.join(tdir, path), os.path.join(tdir, new))
    return new

# ---- file name parsing ---------------------------------------------------------

def parse(base, typ):
    """Split a file-hunter / TOSEC name into title, year, publisher, lang, part, flags."""
    b = re.sub(r'\.(zip|lzh|lha|pma|arj|gz|7z|rar)$', '', base, flags=re.I)
    m = re.search(r'\s*[(\[]', b)    # the first ( or [, with or without a space before it
    i = m.start() if m and m.start() > 0 else len(b)
    title, rest = b[:i].strip(), b[i:]
    parens = re.findall(r'\(([^()]*)\)', rest)
    flags = re.findall(r'\[([^\]]*)\]', rest)
    year = next((p for p in parens if re.fullmatch(r'(19|20)[0-9x]{2}', p)), None)
    lang = next((p for p in parens if p.lower() in LANGS), None)
    if not lang:   # ROM names also write it in brackets: [English], [Jp]
        lang = next((LANG_WORDS[f.lower()] for f in flags if f.lower() in LANG_WORDS), None)
        flags = [f for f in flags if f.lower() not in LANG_WORDS]
    part = [p for p in parens if re.match(r'(disk|tape|side|disc|part) ', p, re.I) or re.fullmatch(r'side [ab]', p, re.I)]
    others = [p for p in parens if p not in part and p != year and p != lang and p.strip() not in ('', '-')]
    lang = lang and {'jp': 'ja', 'en-jp': 'en-ja', 'jp-en': 'ja-en'}.get(lang.lower(), lang.lower())   # the archive writes (jp)
    pub = others[0] if others else None
    # ROM style: "Title - Publisher (Year) ..." or "Title (Year) Publisher [...]"
    if typ == 'ROM':
        m = re.match(r'^\s*\((\d{4}|19xx)\)\s*([^\[(]+)', rest)
        if m and not pub:
            pub = m.group(2).strip()
        if ' - ' in title and not pub and norm(title) not in xl:
            title, pub = title.rsplit(' - ', 1)
    fid = next((f for f in flags if re.fullmatch(r'\d{1,6}', f)), None)   # File Hunter's own number, [1] up
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


ARCHIVES = ('.zip', '.lzh', '.lha', '.pma', '.arj', '.gz', '.7z', '.rar')


def unpack(path, dest):
    os.makedirs(dest, exist_ok=True)
    low = path.lower()
    try:
        if low.endswith(ARCHIVES) and os.path.getsize(path) == 0:
            raise ValueError('empty file')
        if low.endswith('.zip') and not zipfile.is_zipfile(path):
            raise ValueError('not a zip (truncated, or an error page)')
        if low.endswith('.zip'):
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
        if low.endswith(('.lzh', '.lha', '.pma')):   # lhasa reads PMarc too
            return subprocess.run(['lha', '-xqw=' + dest, path], capture_output=True).returncode == 0
        if low.endswith(('.7z', '.rar', '.arj')):
            return subprocess.run(['7z', 'x', '-y', '-o' + dest, path], capture_output=True).returncode == 0
        if low.endswith('.gz'):
            with gzip.open(path) as s, open(os.path.join(dest, os.path.basename(path)[:-3]), 'wb') as d:
                shutil.copyfileobj(s, d)
            return True
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

def hw_needs(text):
    """Hardware the emulated MSX1/MSX2 machines lack, named in a file or folder name."""
    t = text.lower()
    needs = []
    if 'v9990' in t or 'gfx9000' in t: needs.append('v9990')
    if 'v9968' in t: needs.append('v9968')
    if re.search(r'turbo[ -]?r', t): needs.append('turbor')
    if 'msx2+' in t or 'msx2plus' in t: needs.append('msx2plus')
    return needs


# ---- collect variants ----------------------------------------------------------

SRC = {  # folder -> (type, machines, slot, needs)
    'Games/MSX1/ROM': ('ROM', MSX1, 'cart1', []),
    'Games/MSX1/DSK': ('DSK', MSX1, 'disk0', []),
    'Games/MSX1/MSX-Binaries': ('BIN', MSX1, 'ide0', []),
    'Games/MSX1/HDD': ('HDD', MSX1, 'ide0', []),
    'Games/MSX2/ROM': ('ROM', MSX2, 'cart1', []),
    'Games/MSX2/DSK': ('DSK', MSX2, 'disk0', []),
    'Games/MSX2/MSX-Binaries': ('BIN', MSX2, 'ide0', []),
    'Games/MSX2/Harddisk': ('HDD', MSX2, 'ide0', []),
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
            p['title'] = re.sub(r'\.(lzh|lha|pma|arj|gz|zip)$', '', fn, flags=re.I)
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
        vkey = typ + '|' + folder + '|' + re.sub(r'[(\[](disk|tape|side|disc|part) [^)\]]*[)\]]', '', fn, flags=re.I)
        v = t['variants'].setdefault(vkey, dict(typ=typ, folder=folder, machines=machines, slot=slot, needs=needs, parse=p, srcs=[]))
        v['srcs'].append(fn)

print('titles', len(titles), 'variants', sum(len(t['variants']) for t in titles.values()))

# ---- write titles ---------------------------------------------------------------

os.makedirs(OUT, exist_ok=True)
MARK = '.build_msx'


def ours(tdir):
    """A folder an earlier run of this builder wrote, safe to rebuild in place."""
    if os.path.exists(os.path.join(tdir, MARK)):
        return True
    try:
        return 'The File Hunter MSX archive' in open(os.path.join(tdir, 'about.md')).read()
    except OSError:
        return False


used = set()     # ids given out in this run; an earlier run's folders are rebuilt, not doubled
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
    while tid in used or (os.path.exists(os.path.join(OUT, tid)) and not ours(os.path.join(OUT, tid))):
        tid = '%s-%d' % (base, n); n += 1
    used.add(tid)
    if only and key not in only:
        continue
    tdir = os.path.join(OUT, tid)
    shelve(tdir)   # ours from an earlier run, kept until this build has files
    os.makedirs(tdir)
    open(os.path.join(tdir, MARK), 'w').close()
    fdir = os.path.join(tdir, 'files')
    os.makedirs(fdir, exist_ok=True)
    runs, allfiles, sdbhit, sources, unrun = [], [], None, [], []
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
        # Only cartridges get runs. The player inserts media as plain bytes, and the MSX floppy and
        # IDE slots take block media only (src/core/zxm_msx.c), so disks, hard disks and loose
        # binaries stay in files/ without a run until it can build them.
        if v['typ'] != 'ROM':
            continue
        # A ROM that names hardware we do not emulate (V9990, MSX2+, turbo R) is tagged, not run.
        hn = hw_needs(' '.join(v['srcs']) + ' ' + ' '.join(got))
        if hn:
            t.setdefault('hneeds', set()).update(hn)
            continue
        runf = [g for g in got if os.path.splitext(g)[1].lower() in RUNNABLE] or got
        run = dict(platform='msx', machines=v['machines'], files=runf, slot=v['slot'], needs=list(v['needs']))
        p = v['parse']
        notes = []
        if p['flags']:
            notes.append(' '.join('[%s]' % f for f in p['flags']))
        if p['lang']:
            notes.append('language: ' + p['lang'])
        if v['typ'] == 'ROM':
            for i, g in enumerate(runf):
                h = sdb.get(sha1(os.path.join(tdir, g)))
                if h:
                    sdbhit = sdbhit or h
                    # A known bad dump, or a mapper the cartridge core cannot bank, stays in files/ only.
                    if (h.get('status') or '').lower() == 'bad' or not mapper_ok(h['mapper']):
                        unrun.append('%s (openMSX: %s)' % (os.path.basename(g), 'bad dump' if (h.get('status') or '').lower() == 'bad' else h['mapper'] + ' mapper'))
                        run = None
                        break
                    run['mapper'] = h['mapper']
                    new = mark_mapper(tdir, g, h['mapper'])
                    runf[i] = new
                    allfiles[allfiles.index(g)] = new
                    if h.get('status'):
                        notes.append('openMSX: ' + h['status'] + (' - ' + h['remark'] if h.get('remark') else ''))
                    break
        if run is None:
            continue
        if notes:
            run['notes'] = '; '.join(notes)
        runs.append(run)
    settle(tdir, bool(allfiles))
    if not allfiles:
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
    tags += sorted(t.get('hneeds', ()))
    meta['tags'] = tags
    meta['license'] = 'unknown'
    meta['source'] = sources[0]
    meta['sources'] = sources
    meta['added'] = TODAY
    ids = {}
    if sdbhit and (sdbhit.get('genmsx') or '').isdigit(): ids['genmsx'] = int(sdbhit['genmsx'])
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
    if unrun:
        about += ['', 'In files/ without a run: ' + ', '.join(unrun) + '.']
    about += ['', 'Files from download.file-hunter.com (The File Hunter MSX archive), %s.' % TODAY]
    with open(os.path.join(tdir, 'about.md'), 'w') as f:
        f.write('\n'.join(about) + '\n')
    index.append({'id': tid, 'name': name, 'machines': sorted(t['machines']), 'runs': len(runs), 'screens': len(shots)})

if only is None:   # a full build
    # A titles folder this builder made that no longer comes out of the archive (a game gone, or a
    # renamed slug) is stale; only folders carrying the marker are touched.
    for d in os.listdir(OUT):
        if d not in used and os.path.exists(os.path.join(OUT, d, MARK)):
            shutil.rmtree(os.path.join(OUT, d))

json.dump(index, open(os.path.join(OUT, '..', 'msx-index.json'), 'w'), indent=0)
print('written', len(index))
