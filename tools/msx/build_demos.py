# build_demos.py DL_DIR META_DIR OUT_TITLES
# File-hunter.com MSX Demos/ into library titles (format 2). Reuses the
# helpers in build_msx.py (everything above its "collect variants" section).
import sys, os, re, json, shutil
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build_msx.py')).read().split('# ---- collect variants')[0])

SLOTS = {'.rom': ('cart1', []), '.mx1': ('cart1', []), '.mx2': ('cart1', []), '.dsk': ('disk0', ['msx-disk']),
         '.di1': ('disk0', ['msx-disk']), '.cas': ('tape', ['msx-tape'])}


def hw(text):
    t = text.lower()
    needs = []
    if 'v9990' in t or 'gfx9000' in t: needs.append('v9990')
    if 'v9968' in t: needs.append('v9968')
    if 'turbo' in t and 'r' in t and re.search(r'turbo[ -]?r', t): needs.append('turbor')
    if 'msx2+' in t or 'msx2plus' in t: needs.append('msx2plus')
    machines = MSX1 if re.search(r'msx ?1\b|tms9918|tms99', t) else MSX2
    return machines, needs


root = os.path.join(DL, 'Demos')
os.makedirs(OUT, exist_ok=True)
used = set(os.listdir(OUT))
index = []
for folder in sorted(os.listdir(root)):
    fdirp = os.path.join(root, folder)
    files = sorted(f for f in os.listdir(fdirp) if not f.endswith('.part'))
    sets = {}
    split = len(files) > 4 and not any(re.search(r'disk \d', f, re.I) for f in files)
    for f in files:
        k = re.sub(r'\.\w+$', '', f) if split else folder
        sets.setdefault(k, []).append(f)
    for name0, srcs in sets.items():
        p = parse(srcs[0], 'ROM') if split else parse(folder, 'ROM')
        name = re.sub(r'ASCII(8|16)$', '', p['title']).strip() if split else folder
        name = re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', name) if split and ' ' not in name else name
        tid = 'msx-demo-' + slug(name)
        b, n = tid, 2
        while tid in used:
            tid = '%s-%d' % (b, n); n += 1
        used.add(tid)
        tdir = os.path.join(OUT, tid)
        fdir = os.path.join(tdir, 'files')
        os.makedirs(fdir, exist_ok=True)
        got = []
        for s in srcs:
            tmp = os.path.join(tdir, '.x')
            shutil.rmtree(tmp, ignore_errors=True)
            if not unpack(os.path.join(fdirp, s), tmp):
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
        if not got:
            shutil.rmtree(tdir, ignore_errors=True)
            continue
        machines, hneeds = hw(folder + ' ' + ' '.join(srcs) + ' ' + ' '.join(got))
        runs = []
        bykind = {}
        for g in got:
            e = os.path.splitext(g)[1].lower()
            if e in SLOTS:
                bykind.setdefault(e, []).append(g)
        for e in ('.rom', '.mx2', '.mx1', '.dsk', '.di1', '.cas'):
            if e not in bykind:
                continue
            slot, nd = SLOTS[e]
            if slot == 'cart1':
                for g in bykind[e]:
                    run = dict(platform='msx', machines=machines, files=[g], slot=slot, needs=nd + hneeds)
                    h = sdb.get(sha1(os.path.join(tdir, g)))
                    if h: run['mapper'] = h['mapper']
                    runs.append(run)
            else:   # a disk or tape set goes in together, disk 1 first
                runs.append(dict(platform='msx', machines=machines, files=bykind[e], slot=slot, needs=nd + hneeds))
        if not runs:   # programs (.com/.bin/.bas) and data go on the Nextor host disk
            runs.append(dict(platform='msx', machines=machines, files=got, slot='ide0', needs=['nextor'] + hneeds))
        meta = {'format': 2, 'id': tid, 'name': name, 'kind': 'demo'}
        yrs = [int(y) for y in re.findall(r'\((19[89]\d|20[0-2]\d)\)', ' '.join(srcs) + ' ' + folder)]
        if yrs: meta['year'] = min(yrs)
        if p.get('pub') and split: meta['publisher'] = p['pub']
        if split: meta['tags'] = ['msx', 'demo', folder]
        else: meta['tags'] = ['msx', 'demo']
        meta['tags'] += [n for n in hneeds]
        meta['license'] = 'unknown'
        meta['source'] = FH + 'Demos/' + folder + '/' + srcs[0]
        meta['sources'] = [FH + 'Demos/' + folder + '/' + s for s in srcs]
        meta['added'] = TODAY
        meta['runs'] = runs
        meta['compat'] = {}
        if p.get('fhid'): meta['ids'] = {'filehunter': [int(p['fhid'])]}
        imgs = [g for g in got if os.path.splitext(g)[1].lower() in ('.png', '.jpg', '.gif')]
        if imgs:
            meta['screens'] = [{'file': g, 'caption': 'From the download'} for g in imgs[:4]]
        json.dump(meta, open(os.path.join(tdir, 'title.json'), 'w'), indent=2, ensure_ascii=False)
        with open(os.path.join(tdir, 'about.md'), 'w') as f:
            f.write('# %s\n\nMSX demo from download.file-hunter.com, Demos/%s.\n' % (name, folder))
        index.append({'id': tid, 'name': name, 'runs': len(runs)})
json.dump(index, open(os.path.join(OUT, '..', 'msx-demos-index.json'), 'w'), indent=0)
print('demos written', len(index))
