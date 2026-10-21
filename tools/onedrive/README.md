# OneDrive collection import

`onedrive_local.py` downloads a shared OneDrive folder of Spectrum tapes and
snapshots and files each game into a ZX Cradle library
([docs/cradle/titles.md](../../docs/cradle/titles.md)). It needs only Python 3.

```
python3 tools/onedrive/onedrive_local.py LIBRARY SHARE_LINK
```

- `LIBRARY` is the library folder (the one holding `titles/`).
- `SHARE_LINK` is the folder's "anyone with the link" share URL. It is never
  stored in this repository.

The raw files land in `LIBRARY/_onedrive/` and each title is copied into
`LIBRARY/titles/<id>/` with a `title.json` (machines, year, authors, publisher,
genre, ZXDB id) and up to three ZXDB screenshots. Raw `.hdf` hard-disk images
are skipped; the same images are in the folder as `.rar`. Each tape side or
part becomes a run of its own, a `.zip` is unpacked to the tapes and snapshots
inside it, and files the player cannot load (`.sp`, `.slt`, `.mgt`, `.xml`) are
left out. The script exits with status 3 if any download failed.

`onedrive-records.json.gz` lists which files make up each title and the ZXDB
entry each was matched to (4,035 titles from 4,492 files). A title whose ZXDB
id is already in the library gets the OneDrive files added to it under
`files/onedrive/` instead of a second folder. Re-running skips finished
downloads and titles. `ZXC_LIMIT=25` limits a run to the first 25 titles.
