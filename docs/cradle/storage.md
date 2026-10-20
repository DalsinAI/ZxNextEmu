# Media and storage

ZX Cradle manages storage the way Amiga Cradle manages drives: **every
medium is a folder**. An SD card, a Microdrive cartridge, a +3 disk, a Beta
Disk, a tape or a ROM cartridge each gets its own folder with a small
manifest, and the contents are either an image file or plain host files.
You can open the folder on the host, copy files in, back it up, or move it
to another instance.

## Kinds of media

| Kind | Machines | Slot ids | Image formats | Folder-backed as |
| --- | --- | --- | --- | --- |
| `sd` | Next; divMMC on the 48K/128K/+3 | `sd`, `sd.1` | `.img` (FAT16/FAT32), `.mmc` | FAT32, built on the fly |
| `ide` | divIDE, +3e, ZXATASP | `ide.0`, `ide.1` | `.hdf`, `.img` | FAT32 or +3DOS partition |
| `microdrive` | Interface 1 (up to 8 drives) | `md.1` to `md.8` | `.mdr` | Microdrive cartridge, built on the fly |
| `disk` | +3, +2A with drive, CPC | `disk.a`, `disk.b` | `.dsk` (standard and extended) | +3DOS / AMSDOS (CP/M 2.2 layout) |
| `beta` | Pentagon, Scorpion, Beta 128 | `beta.a` to `beta.d` | `.trd`, `.scl` | TR-DOS |
| `tape` | every Spectrum, ZX81, CPC, MSX | `tape` | `.tap`, `.tzx`, `.pzx`, `.wav`, `.p`, `.cdt`, `.cas` | a playlist of files, in name order |
| `cart` | Interface 2, Timex, CPC Plus, MSX, consoles | `cart` | `.rom`, `.dck`, `.cpr` | not folder-backed: a cartridge is one image |

The machine manifest lists which kinds each slot accepts; adding an
interface (Interface 1, divMMC, Beta 128) to an instance adds its slots.

## A medium's folder

```
media/sd/main/
  medium.toml
  files/            folder-backed: the card's contents as host files
media/microdrive/work/
  medium.toml
  image.mdr         image-backed: one image file
media/disk/game-disk-1/
  medium.toml
  image.dsk
```

```toml
kind    = "sd"
label   = "NextZXOS"
backing = "folder"        # "folder" (files/) or "image" (image.<ext>)
format  = "fat32"         # what the machine sees
size    = "4G"            # the size the machine sees; folder-backed media grow up to it
write   = "overlay"       # "rw", "ro" or "overlay" (see below)
base    = "library:sd/nextzxos-<version>"  # for an overlay: the read-only medium underneath
source  = "https://... (where it came from)"
sha256  = "..."           # of the original download or image, for field reports
```

Folders live in the instance's `media/` folder, one level per kind, so
`media/sd/`, `media/microdrive/`, `media/disk/` and `media/tape/` hold that
instance's cards, cartridges, disks and tapes. The instance manifest
assigns them to slots:

```toml
[media]
sd     = "sd/main"
"md.1" = "microdrive/work"
"md.2" = "microdrive/games"
tape   = "tape/jetpac"
```

## Image-backed and folder-backed

**Folder-backed is the default for SD cards** (Dale, 2026-10-20): each
instance has one folder per SD card, which the guest sees as a live FAT32
volume synthesised on the fly, with the guest's writes mapped back to host
files. Microdrives and disks follow the same model as they arrive. The core
work for this is being done in the emulator itself.

**Image-backed** media are bit-exact: the runtime reads and writes the image
file and nothing else. Use them for copy-protected disks, for testing, and
for anything that must match a real medium byte for byte.

**Folder-backed** media let you work with plain files. The runtime builds
the medium's file system in memory from `files/` when the medium is
inserted (FAT32 for SD cards, sectors and a catalogue for Microdrives, a
CP/M directory for +3 disks, a TR-DOS catalogue for Beta disks) and writes
changes back as files: a file the machine saves appears in `files/`, a file
it erases disappears. Spectrum file headers (type, start address, BASIC
autostart line) are kept in a `+3DOS` header for disks and in a sidecar
`.zxhdr` file for Microdrives and TR-DOS, so a round trip loses nothing.

Rules that keep folder-backed media safe:

- **One writer at a time.** While an instance has a medium inserted, the
  runtime holds `medium.lock` in its folder. Another instance can insert it
  read-only, never read-write.
- **The host waits its turn.** Files edited on the host while the medium is
  inserted are picked up at the next insert or `zxc media sync`, not mid-run,
  because the machine may have the old directory cached.
- **Capacity is real.** A folder-backed medium refuses writes beyond the
  size the machine sees (about 90 KB for a Microdrive cartridge, 173 KB for
  a +3 disk side, 640 KB for a TR-DOS disk), the same way the real medium
  fills up. Files on the host that do not fit are left out of the
  catalogue and listed in the log.
- **Names are translated, not lost.** Host names that the machine's file
  system cannot hold (long names on TR-DOS, characters CP/M refuses) are
  shown under a short name; the mapping is kept in `names.toml` so a save
  goes back to the same host file.

`zxc media convert` turns one backing into the other: a `.dsk` into a folder
to edit its files, or a folder into an `.mdr` to share it.

## Write modes and overlays

| `write` | What happens |
| --- | --- |
| `rw` | Changes go straight to the medium. |
| `ro` | The machine sees a write-protected medium (the Microdrive's tab removed, the disk's write-protect hole open). |
| `overlay` | The machine can write, but changes go to an overlay in the instance's medium folder; the `base` underneath never changes. `zxc media commit` writes the overlay into a new medium, `zxc media reset` throws it away. |

Overlays make shared masters safe: ten instances can boot from one
NextZXOS card, each keeping its own saved files.

## The library

`~/.zxcradle/library/` holds media shared between instances, in the same
folder-per-medium layout (`library/sd/nextzxos-<version>/`,
`library/tape/jetpac/`). Library media are always read-only to instances;
an instance uses one through an overlay, or copies it in with
`zxc media copy`.

**The Next system card.** `zxc media fetch nextzxos` downloads the official
Spectrum Next distribution, checks its checksum, unpacks it into
`library/sd/nextzxos-<version>/` as a folder-backed FAT32 card, and records
the download URL and checksum in `medium.toml`. A new Next instance gets
`media/sd/main/` as an overlay on the newest card in the library. When a
newer distribution arrives, `zxc media rebase sd/main nextzxos-<new>` moves
the overlay onto it, keeping the user's own files; files the user changed
that the new distribution also changed are listed, not overwritten.

The same mechanism serves other system media: the +3's CP/M disk, esxDOS for
divMMC instances, TR-DOS utility disks.

## Commands

| Command | Does |
| --- | --- |
| `media` | List the instance's media folders and what is in each slot |
| `media new KIND NAME [--image\|--folder] [--size S]` | Create a blank, formatted medium |
| `media add FILE [NAME]` | Make a medium from an image file (copied into its own folder) |
| `media fetch nextzxos\|esxdos\|...` | Download a system medium into the library |
| `media copy library:KIND/NAME [NAME]` | Copy a library medium into the instance |
| `media overlay library:KIND/NAME NAME` | Use a library medium through an overlay |
| `media commit NAME [NEW]`, `media reset NAME` | Keep or discard an overlay's changes |
| `media rebase NAME BASE` | Move an overlay onto a newer base |
| `media convert NAME --image\|--folder` | Switch backing |
| `media sync NAME` | Flush the runtime's writes and re-read host changes |
| `media protect NAME on\|off` | Write protect |
| `insert SLOT NAME`, `eject SLOT` | Put a medium in a slot, take it out |

The MCP server has the matching tools (`list_media`, `create_medium`,
`add_medium`, `fetch_system_medium`, `insert_media`, `eject_media`,
`commit_overlay`, `reset_overlay`), with the same rules: `fetch` only
downloads from the official sources named in the built-in machine
manifests.

## In the core

The core never sees a folder. Every medium reaches the WebAssembly module
through `zxm_media` imports as a block device (SD, IDE), a sector device
(disk, Beta, Microdrive) or a byte stream (tape, cartridge). The JavaScript
runtime implements those over an image file or over a folder, with the
folder's file system built in memory:

- **Under Node.js**, folders are real folders under `~/.zxcradle/`, read and
  written with `node:fs`; `medium.lock` is a lock file.
- **In the browser**, folders are in the Origin Private File System, or in a
  real folder on disk the user grants through the File System Access API
  (Chromium browsers). The lock is a Web Lock named after the medium.
  Writes from the Worker use synchronous OPFS access handles, so a guest
  write is on disk before the next frame.
