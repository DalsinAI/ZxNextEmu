# gmsx_shots.py TITLES_DIR
# For MSX titles with a Generation MSX id and no screenshots, fetch the
# Generation MSX page and save its title and in-game pictures as screens.
import sys, os, re, json, subprocess, concurrent.futures as cf

T = sys.argv[1]
UA = 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36'


def get(url, out=None):
    cmd = ['curl', '-sS', '-f', '-L', '-m', '60', '-A', UA, url]
    if out:
        cmd[1:1] = ['-o', out]
    r = subprocess.run(cmd, capture_output=True)
    return r.returncode == 0 and (out or r.stdout.decode('utf-8', 'replace'))


def one(tid):
    p = os.path.join(T, tid, 'title.json')
    m = json.load(open(p))
    gid = (m.get('ids') or {}).get('genmsx')
    if not gid or m.get('screens'):
        return 0
    html = get('https://www.generation-msx.nl/software/-/-/%d/' % gid)
    if not html:
        return 0
    imgs = []
    for kind in ('game', 'title', 'loading'):
        for u in re.findall(r'https://images\.generation-msx\.nl/software_%s/[0-9a-f]+\.png' % kind, html):
            if u not in imgs:
                imgs.append(u)
    shots = []
    sd = os.path.join(T, tid, 'screens')
    for u in imgs[:4]:
        os.makedirs(sd, exist_ok=True)
        f = os.path.join(sd, '%d.png' % (len(shots) + 1))
        if get(u, f):
            shots.append({'file': 'screens/%d.png' % (len(shots) + 1), 'caption': 'Generation MSX ' + ('title screen' if 'software_title' in u else 'loading screen' if 'software_loading' in u else 'in-game picture')})
    if shots:
        m['screens'] = shots
        m.setdefault('sources', [m.get('source')]).append('https://www.generation-msx.nl/software/-/-/%d/' % gid)
        json.dump(m, open(p, 'w'), indent=2, ensure_ascii=False)
    return len(shots)


ids = sorted(d for d in os.listdir(T) if d.startswith('msx-') and not d.startswith('msx-demo-'))
with cf.ThreadPoolExecutor(8) as ex:
    got = list(ex.map(one, ids))
print('titles with new screens', sum(1 for g in got if g), 'pictures', sum(got))
