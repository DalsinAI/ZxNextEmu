# Pack the shared title folders into collection.json for the published page.
# Files are included only for titles whose licence allows redistribution;
# every title keeps its title.json (or older title.toml), about.md, reviews.json
# and screenshots.
import base64, json, os, re, sys
src, out = sys.argv[1], sys.argv[2]
FREE = {'freeware', 'public-domain'}
titles = []
for tid in sorted(os.listdir(src)):
    d = os.path.join(src, tid)
    json_path, toml_path = os.path.join(d, 'title.json'), os.path.join(d, 'title.toml')
    if os.path.isfile(json_path):
        meta = json.load(open(json_path, encoding='utf-8'))
        lic = meta.get('license', 'unknown')
        t = {'id': tid, 'json': meta, 'files': [], 'screens': []}
    elif os.path.isfile(toml_path):
        toml = open(toml_path, encoding='utf-8').read()
        lic = (re.search(r'^license\s*=\s*"([^"]*)"', toml, re.M) or [None, 'unknown'])[1]
        t = {'id': tid, 'toml': toml, 'files': [], 'screens': []}
    else:
        continue
    about = os.path.join(d, 'about.md')
    if os.path.isfile(about):
        t['about'] = open(about, encoding='utf-8').read()
    reviews = os.path.join(d, 'reviews.json')
    if os.path.isfile(reviews):
        t['reviews'] = json.load(open(reviews, encoding='utf-8'))
    for kind in ('files', 'screens'):
        if kind == 'files' and lic not in FREE:
            continue
        base = os.path.join(d, kind)
        for root, _, names in os.walk(base):
            for n in sorted(names):
                p = os.path.join(root, n)
                rel = os.path.relpath(p, d).replace(os.sep, '/')
                t[kind].append({'path': rel, 'b64': base64.b64encode(open(p, 'rb').read()).decode()})
    titles.append(t)
json.dump({'format': 2, 'titles': titles}, open(out, 'w'))
print(len(titles), 'titles,', sum(len(t['files']) for t in titles), 'files,', os.path.getsize(out) // 1024, 'KB')
