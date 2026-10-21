# MSX and SymbOS library builders

Scripts that turn the File Hunter MSX archive and symbos.de into library
titles (`title.json` format 2, see docs/cradle/titles.md). Downloads go to a
local folder; nothing here is meant to be committed except the scripts.

Needs: python3 with openpyxl and Pillow, curl, lha (lhasa) and 7z.

## 1. Lists and metadata (into `meta/`)

```sh
UA='Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36'
FH=https://download.file-hunter.com
mkdir -p meta && cd meta
curl -A "$UA" -o fh.torrent "$FH/download.file-hunter.com%20-%2024-06-2026.torrent"   # name changes; see $FH/
curl -A "$UA" -o pics.xlsx "$FH/Archive/MSX%20Games%20with%20Pictures.xlsx"
curl -o softwaredb.xml https://raw.githubusercontent.com/openMSX/openMSX/master/share/softwaredb.xml
python3 ../tor.py fh.torrent sizes.json      # path -> size for every file
```

File Hunter answers 405 without a browser User-Agent.

Screenshots from the spreadsheet: run the snippet at the end of this file to
write `meta/xlimg/<n>.png` and `meta/xlimg.json`.

## 2. Game and demo files (into `msx-dl/`)

Take these folders from `sizes.json` (about 2.1 GB): Games/MSX1/{ROM, DSK,
CAS, MSX-Binaries, HDD}, Games/MSX2/{ROM, DSK, CAS, MSX-Binaries, Harddisk},
Demos/, OS/SymbOS/. WAV tapes, MSX2+ and turbo R are left out.

```sh
python3 -c "import json,urllib.parse
for k in sorted(json.load(open('meta/sizes.json'))):
  if k.startswith(('Games/MSX1/ROM/','Games/MSX1/DSK/','Games/MSX1/CAS/','Games/MSX1/MSX-Binaries/','Games/MSX1/HDD/','Games/MSX2/ROM/','Games/MSX2/DSK/','Games/MSX2/CAS/','Games/MSX2/MSX-Binaries/','Games/MSX2/Harddisk/','Demos/','OS/SymbOS/')):
    print('https://download.file-hunter.com/'+urllib.parse.quote(k)); print(k)" \
 | xargs -d '\n' -n 2 -P 12 ./get.sh
```

## 3. SymbOS (into `sym/`)

From http://www.symbos.de: `apps.js` (the `garrApps` array: id, name, version,
date, author id, author, genre, platforms, OS version, memory, needs, thumb,
screen count, screen name, flags, short text, long text, url) saved as
`sym/apps.json`; each app's zip as `sym/dl/<id>.zip` from
`download/apps/<name without space & ' " ( ) / \>.zip`; screenshots as
`sym/shots/<id>-<n>.gif` from `gfx/shots/<screen name><n>.gif`; and the OS
from `download/20250131-V40/` (SymbOS-MSX40.zip, SymbOS-ZNX40.zip,
SymbOS-40-MassStorageCompletion.zip, SymbOS-40-Manual.pdf, ASM developer kit)
into `sym/os/` (lower-case names: symbos-msx40.zip, symbos-znx40.zip). The
site answers 301 to the right capitalisation, so use `curl -L`.

## 4. Build titles

```sh
python3 build_msx.py    msx-dl meta library/titles   # one title per game
python3 build_demos.py  msx-dl meta library/titles
python3 build_symbos.py sym    msx-dl library/titles
python3 gmsx_shots.py   library/titles               # Generation MSX pictures
```

Result (2026-10-21): 6,579 games, 377 demos, 95 SymbOS titles; 7.7 GB
unpacked.

Conventions: ids `msx-*`, `msx-demo-*`, `symbos-*`. MSX1 titles run on msx1,
msx1eu, msx2, msx2eu; MSX2 titles on msx2, msx2eu. Slots are the core's:
cart1, disk0, ide0 (CAS tapes and DMK images are left out until the MSX has a tape slot and a DMK reader). Needs: v9990, turbor,
msx2plus, symbos (the disk and IDE slots need nothing extra). Loose program files (MSX-Binaries,
SymbOS apps) get runs with no files and a note to copy them onto a disk: the slots take only
images. SymbOS apps list next and msx2/msx2eu with needs ["symbos"] and tag "symbos".
ROM runs carry `mapper` from openMSX, and the ROM file is renamed with the core's marker
(`[ascii8]`, `[ascii16]`, `[konami]`, `[konamiscc]`, `[plain]`) so the core uses it.
Each variant (ROM, disk set, tape, hack, translation) is a run, best first;
bad dumps ([b]) are skipped.

## Spreadsheet screenshots

```python
import openpyxl, re, json, os
wb = openpyxl.load_workbook('meta/pics.xlsx'); out = {}; n = 0
os.makedirs('meta/xlimg', exist_ok=True)
for ws in wb.worksheets[1:]:
    labels = {c[0].row: c[0].value for c in ws.iter_rows(min_col=1, max_col=1)
              if isinstance(c[0].value, str) and c[0].value.endswith(')') and '[' in c[0].value}
    for im in ws._images:
        r = im.anchor._from.row + 1
        cand = [k for k in labels if k <= r + 1]
        m = cand and re.match(r'(.*) \[(\w+)\] \((.*)\)$', labels[max(cand)])
        if not m:
            continue
        p = 'xlimg/%d.png' % n; n += 1
        open('meta/' + p, 'wb').write(im._data())
        out.setdefault(m.group(1), []).append({'type': m.group(2), 'file': p})
json.dump(out, open('meta/xlimg.json', 'w'))
```
