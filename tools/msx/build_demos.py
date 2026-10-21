# build_demos.py DL_DIR META_DIR OUT_TITLES
# File-hunter.com MSX Demos/ into library titles (format 2). Reuses the
# helpers in build_msx.py (everything above its "collect variants" section).
import sys, os, re, json, shutil, urllib.parse
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build_msx.py')).read().split('# ---- collect variants')[0])

# .di1/.di2 are the two halves of one disk and go in one run. No .cas: the MSX has no tape slot yet.
SLOTS = {'.rom': ('cart1', []), '.mx1': ('cart1', []), '.mx2': ('cart1', []), '.dsk': ('disk0', []),
         '.di1': ('disk0', []), '.di2': ('disk0', [])}


def hw(text):
    t = text.lower()
    needs = hw_needs(t)
    machines = MSX1 if re.search(r'msx ?1\b|tms9918|tms99', t) else MSX2
    return machines, needs


root = os.path.join(DL, 'Demos')
os.makedirs(OUT, exist_ok=True)
DMARK = '.build_demos'


def ours_demo(tdir):
    if os.path.exists(os.path.join(tdir, DMARK)):
        return True
    try:
        return 'MSX demo from download.file-hunter.com' in open(os.path.join(tdir, 'about.md')).read()
    except OSError:
        return False


PART = re.compile(r'[\s_-]*[(\[]?\b(disk|disc|side|part)\s*[0-9a-d]+(\s*of\s*\d+)?[)\]]?', re.I)
used = set()     # ids given out in this run; an earlier run's folders are rebuilt, not doubled
index = []
for folder in sorted(os.listdir(root)):
    fdirp = os.path.join(root, folder)
    have = set(os.listdir(fdirp))
    # get.sh leaves "<name>.part" when its last try fails: a failed source, unless the whole file is there
    files = sorted(f for f in have if not (f.endswith('.part') and f[:-5] in have))
    sets = {}
    # A folder holds one demo, or separate ones; the disks of one demo share a name once the part
    # marker ("(Disk 1 of 2)", "[Disk 2]", "Disk 3") is taken out.
    for f in files:
        k = PART.sub('', re.sub(r'\.\w+$', '', re.sub(r'\.part$', '', f))).strip()
        sets.setdefault(k, []).append(f)
    split = len(sets) > 1
    if not split:
        sets = {folder: files}
    for name0, srcs in sets.items():
        p = parse(re.sub(r'\.part$', '', srcs[0]), 'ROM') if split else parse(folder, 'ROM')
        name = re.sub(r'ASCII(8|16)$', '', p['title']).strip() if split else folder
        name = re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', name) if split and ' ' not in name else name
        tid = 'msx-demo-' + slug(name)
        b, n = tid, 2
        while tid in used or (os.path.exists(os.path.join(OUT, tid)) and not ours_demo(os.path.join(OUT, tid))):
            tid = '%s-%d' % (b, n); n += 1
        used.add(tid)
        tdir = os.path.join(OUT, tid)
        had = shelve(tdir)   # ours from an earlier run, kept until this build has files
        fdir = os.path.join(tdir, 'files')
        os.makedirs(fdir, exist_ok=True)
        open(os.path.join(tdir, DMARK), 'w').close()
        got, failed = [], []
        for s in srcs:
            if s.endswith('.part'):   # a download cut off: keep the earlier build
                failed.append(s)
                continue
            tmp = os.path.join(tdir, '.x')
            shutil.rmtree(tmp, ignore_errors=True)
            if not unpack(os.path.join(fdirp, s), tmp):
                failed.append(s)
                continue
            for r, _, fs in os.walk(tmp):
                for f in sorted(fs):
                    rel = os.path.relpath(os.path.join(r, f), tmp)
                    dst = safe(rel.replace(os.sep, ' - '))
                    st, ex = os.path.splitext(dst)
                    k = 2
                    while os.path.exists(os.path.join(fdir, dst)):
                        dst = '%s (%d)%s' % (st, k, ex); k += 1
                    shutil.move(os.path.join(r, f), os.path.join(fdir, dst))
                    got.append('files/' + dst)
            shutil.rmtree(tmp, ignore_errors=True)
        ok = bool(got) and not (failed and had)   # a disk missing: keep the earlier, complete build
        if not ok:
            settle(tdir, False)
            try:   # the earlier build is back: it stays in the index
                j = json.load(open(os.path.join(tdir, 'title.json')))
                index.append({'id': tid, 'name': j.get('name'), 'runs': len(j.get('runs', []))})
            except (OSError, ValueError):
                pass
            continue
        machines, hneeds = hw(folder + ' ' + ' '.join(srcs) + ' ' + ' '.join(got))
        runs = []
        bykind = {}
        for g in got:
            e = os.path.splitext(g)[1].lower()
            if e in SLOTS:
                bykind.setdefault('.di1' if e == '.di2' else e, []).append(g)
        if '.di1' in bykind:
            bykind['.di1'].sort(key=lambda g: (os.path.splitext(g)[0], g.lower().endswith('.di2')))
        for e in ('.rom', '.mx2', '.mx1', '.dsk', '.di1'):
            if e not in bykind:
                continue
            slot, nd = SLOTS[e]
            # Hardware no emulated MSX has (V9990, V9968, turbo R, MSX2+): files only, no run.
            if slot == 'cart1' and not hneeds:
                for g in bykind[e]:
                    # .mx1 is an MSX1 cartridge and .mx2 an MSX2 one, whatever the rest of the group is
                    mach = MSX1 if e == '.mx1' else MSX2 if e == '.mx2' else machines
                    mach = hz_machines(name0 + ' ' + g, mach)   # "-50Hz", "(NTSC)"
                    if not mach:
                        continue
                    run = dict(platform='msx', machines=mach, files=[g], slot=slot, needs=nd)
                    h = sdb.get(sha1(os.path.join(tdir, g)))
                    if h and ((h.get('status') or '').lower() == 'bad' or not mapper_ok(h['mapper'], os.path.join(tdir, g))):
                        continue   # a known bad dump, or a mapper the cartridge core cannot bank
                    if h:
                        run['mapper'] = h['mapper']
                        run['files'] = [mark_mapper(tdir, g, h['mapper'])]
                        got[got.index(g)] = run['files'][0]
                    runs.append(run)
            # Disks get no run: the MSX floppy slot takes block media only, which the player
            # cannot build from bytes yet. They stay in files/, as do programs and data.
        meta = {'format': 2, 'id': tid, 'name': name, 'kind': 'demo'}
        yrs = [int(y) for y in re.findall(r'\((19[89]\d|20[0-2]\d)\)', ' '.join(srcs) + ' ' + folder)]
        if yrs: meta['year'] = min(yrs)
        if p.get('pub') and split: meta['publisher'] = p['pub']
        if split: meta['tags'] = ['msx', 'demo', folder]
        else: meta['tags'] = ['msx', 'demo']
        meta['tags'] += [n for n in hneeds]
        meta['license'] = 'unknown'
        url = lambda f: FH + 'Demos/' + urllib.parse.quote(folder) + '/' + urllib.parse.quote(re.sub(r'\.part$', '', f))   # names hold # and spaces
        meta['source'] = url(srcs[0])
        meta['sources'] = [url(s) for s in srcs]
        meta['added'] = old_added(tdir) or TODAY
        meta['runs'] = runs
        meta['compat'] = {}
        if p.get('fhid'): meta['ids'] = {'filehunter': [int(p['fhid'])]}
        imgs = [g for g in got if os.path.splitext(g)[1].lower() in ('.png', '.jpg', '.gif')]
        if imgs:   # the library takes screenshots only from screens/
            os.makedirs(os.path.join(tdir, 'screens'), exist_ok=True)
            meta['screens'] = []
            for i, g in enumerate(imgs[:4], 1):
                s = 'screens/%d%s' % (i, os.path.splitext(g)[1].lower())
                shutil.copy(os.path.join(tdir, g), os.path.join(tdir, s))
                meta['screens'].append({'file': s, 'caption': 'From the download'})
        json.dump(meta, open(os.path.join(tdir, 'title.json'), 'w'), indent=2, ensure_ascii=False)
        with open(os.path.join(tdir, 'about.md'), 'w') as f:
            f.write('# %s\n\nMSX demo from download.file-hunter.com, Demos/%s.\n' % (name, folder))
        settle(tdir, True)   # the earlier build goes only once this one is complete
        index.append({'id': tid, 'name': name, 'runs': len(runs)})
# A titles folder this builder made that no longer comes out of the archive (a game gone, or a
# renamed slug) is stale; only folders carrying the marker are touched.
for d in os.listdir(OUT):
    if d not in used and os.path.exists(os.path.join(OUT, d, DMARK)):
        shutil.rmtree(os.path.join(OUT, d))
json.dump(index, open(os.path.join(OUT, '..', 'msx-demos-index.json'), 'w'), indent=0)
print('demos written', len(index))
