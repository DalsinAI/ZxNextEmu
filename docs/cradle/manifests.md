# Formats

ZX Cradle keeps everything as plain files. Manifests are written by people as
often as by programs, so they use TOML (a strict subset: tables, arrays of
tables, strings, integers, booleans and arrays; no dates, no inline tables
spanning lines). The control protocol and logs use JSON. Binary formats
start with a four-byte tag and a version so they can be recognised and
upgraded.

## Machine manifest

Built-in machines have their manifest compiled in; `zxc machines show zx128`
prints it. A user can add a variant (a different ROM, more RAM, another
keyboard issue) by putting a manifest in `~/.zxcradle/machines/`.

```toml
id      = "zx128"
name    = "ZX Spectrum 128K"
family  = "spectrum"
module  = "zx"               # the core module that emulates it
variant = "128k"             # passed to the module
year    = 1986
maker   = "Sinclair Research"

[clock]
cpu_hz   = 3546900
frame_t  = 70908

[[rom]]
file   = "128-0.rom"
size   = 16384
crc32  = "<crc32 of the ROM>" # the host refuses a ROM that does not match
[[rom]]
file   = "128-1.rom"
size   = 16384
crc32  = "<crc32 of the ROM>"

[[slot]]
id     = "tape"
types  = ["tap", "tzx", "pzx", "wav"]
[[slot]]
id     = "joystick"
types  = ["kempston", "sinclair1", "sinclair2", "cursor"]

[boot]
check   = "screen-text"
text    = "128 BASIC"
timeout = 5                 # seconds of emulated time

[sources]
timing = ["https://github.com/... (schematic, FPGA core or measurement)"]
```

## Instance manifest: `instance.toml`

```toml
name    = "jetpac-48"
machine = "zx48"
created = "2026-10-20T21:30:00Z"
notes   = "Ultimate, 1983. Kempston on the left pad."

[run]
speed     = 100             # percent; 0 = as fast as the host can go
sound     = true
autostart = false           # start when the host starts
restore   = "last"          # "last" snapshot, a named snapshot, or "cold"

[display]
scale  = 3
border = "full"             # "full", "small", "none"
filter = "none"             # "none", "scanlines", "crt"

[media]                     # slot = medium folder under media/ (see storage.md)
tape      = "tape/jetpac"
tape_mode = "instant"       # "instant" through the ROM, or "real" pulses

[input]
joystick = "kempston"
pad1     = "joystick"     # first host game pad drives the joystick
keys     = "default"        # or a key map file in the instance folder

[boot]                      # optional: overrides the machine's boot check
check = "pc"
pc    = 0x6000
```

Unknown keys are kept when the host rewrites the file, so newer manifests
survive older tools.

## State file: `.zxcs`

A complete, exact snapshot of an instance. Chunked so that a reader can skip
what it does not know:

```
"ZXCS"  u32 version  u32 chunk_count
chunk:  char tag[4]  u32 length  u8 data[length]
```

| Chunk | Contents |
| --- | --- |
| `INFO` | machine id, core version, instance name, timestamp (JSON) |
| `CPU ` | registers, WZ, Q, IFF1/2, IM, halt, pending interrupt |
| `MACH` | the machine module's own state, versioned by the module |
| `RAMP` | RAM pages, compressed with a simple run-length scheme |
| `MEDI` | slot contents: file name, checksum, position (tape block and pulse, disk head) |
| `SCRN` | the last frame as a PNG, for previews |

Loading a state into a different core version is allowed when the module
says the `MACH` chunk is compatible; otherwise the host offers to export the
nearest classic format (`.szx`, `.z80`, `.sna`) instead.

## Input recording: `.zxcr`

Inputs per frame, each with its T-state offset in the frame, starting from a
`.zxcs` state. Replaying it reproduces a session exactly, which makes it the
best attachment for a bug report.

## Patch: `.zxpatch`

A zip with a `patch.toml` and the files it needs:

```toml
id          = "jetpac-infinite-lives"
title       = "Infinite lives"
machine     = ["zx48", "zx128"]
apply       = "after-boot"   # "rom", "before-boot", "after-boot"

[[poke]]
addr  = 0x5DE0
value = 0x00

[[file]]
src   = "jetpac-fixed.tzx"
dest  = "media/tape/jetpac/image.tzx"
```

`rom` patches change the instance's copy of the ROM. Pokes are applied once
the boot check passes. A patch that would change a file outside the instance
folder is refused.

## Field report

`reports/<instance>-<timestamp>.zip`:

| File | Contents |
| --- | --- |
| `report.json` | what the user typed, host OS and CPU, core and Cradle versions, instance state |
| `instance.toml` | the manifest |
| `state.zxcs` | a snapshot taken when the report was made |
| `before.zxcs` | the rolling snapshot the runtime keeps from about ten seconds earlier |
| `last.zxcr` | the input from `before.zxcs` up to `state.zxcs`, so the problem can be replayed |
| `screen.png` | the current frame |
| `log/` | `runtime.log` and the last 500 lines of `events.jsonl` |

ROM files and media are listed by name and checksum, not included, so a
report can be shared without passing on copyrighted software.
