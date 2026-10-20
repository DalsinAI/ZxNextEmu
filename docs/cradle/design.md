# ZX Cradle design

## Goals

1. **Every Z80 machine, one manager.** Spectrum Next first, the classic
   Spectrums with it, then the rest of the Z80 world (see
   [machines.md](machines.md)). Adding a machine means adding a machine
   module and a manifest, never changing the host.
2. **Instances, not emulator windows.** A machine you have set up is a named
   instance with its own folder: configuration, ROMs, media, snapshots,
   logs. You can copy the folder to another computer and it runs the same.
3. **Timing accuracy is not negotiable.** The core stays T-state exact. The
   host never takes shortcuts that change what the emulated machine sees
   (no frame skipping inside the core, no "fast" paths that alter contention).
4. **Scriptable from the first day.** Everything the UI can do, the `zxc`
   shell and the MCP server can do, through the same control protocol.
5. **Easy to run.** One binary on Linux, Windows, macOS and AmigaOS, or one
   web page. No installer, no runtime to download first.
6. **C, built with GCC.** C99 for the core and runtime, built with GCC
   16.2.0-open3 for AmigaOS and any GCC elsewhere. The WebAssembly build is
   the one exception: it needs clang's wasm32 target, as in Amiga Cradle.

## Concepts

| Term | Meaning |
| --- | --- |
| Machine | A model the core can emulate: `zx48`, `zx128`, `plus3`, `next`, `cpc6128`... Described by a machine manifest. |
| Instance | A configured machine with a name, a folder and a state: `jetpac-48`, `next-dev`. |
| Runtime | The process (or WebAssembly module) that runs one instance. |
| Host | `zxcradled`, the background service that keeps the instance registry and supervises runtimes. |
| Slot | A place media goes: `tape`, `disk.a`, `disk.b`, `cart`, `sd`. Each machine declares its slots. |
| Medium | One SD card, Microdrive cartridge, disk, tape or cartridge, kept in its own folder, image-backed or folder-backed. See [storage.md](storage.md). |
| Media changer | The instance's media and which one is in each slot. |
| State | A full snapshot of an instance (`.zxcs`), exact to the T-state. |
| Patch | A `.zxpatch` archive queued against an instance and applied on its next verified restart. |
| Field report | A `.zip` of everything needed to reproduce a problem with an instance. |

## Layers

```
 ┌───────────────────────────────────────────────────────────────┐
 │  Cradle Setup (web UI)   zxc shell   MCP server   front ends  │  clients
 └──────────────┬───────────────┬──────────────┬─────────────────┘
                │   ZCP (JSON lines over a local socket / WebSocket)
 ┌──────────────┴───────────────┴──────────────┴─────────────────┐
 │  zxcradled: instance registry, supervisor, media, snapshots,  │  host
 │  patch queue, field reports, event log                        │
 └──────────────┬────────────────────────────────────────────────┘
                │   ZCP over the runtime's stdin/stdout (or postMessage)
 ┌──────────────┴────────────────────────────────────────────────┐
 │  zxc-run: one instance. Owns timing, video, audio, input,     │  runtime
 │  media I/O. Native (GCC) or WebAssembly (clang).              │
 └──────────────┬────────────────────────────────────────────────┘
                │   C API (zxm.h)
 ┌──────────────┴────────────────────────────────────────────────┐
 │  libzxcore: Z80/Z80N CPU + machine modules. No I/O, no        │  core
 │  globals, no allocation after create.                         │
 └───────────────────────────────────────────────────────────────┘
```

### Core: `libzxcore`

The existing code in `src/z80` and `src/zx`. Today it exposes the Spectrum
family through `zx.h`. For ZX Cradle it grows one generic interface, `zxm.h`,
that every machine module implements, so the runtime never includes a
machine's own header:

```c
typedef struct zxm zxm;                   /* opaque machine instance */

typedef struct zxm_info {
    const char *id;                       /* "zx128", "next", "cpc6128" */
    const char *name;                     /* "ZX Spectrum 128K" */
    uint32_t cpu_hz, frame_t;             /* nominal clock, T-states per frame */
    uint16_t fb_w, fb_h;                  /* framebuffer size in pixels */
    zxm_pixfmt fb_format;                 /* RGB333, RGB565, palette index... */
    uint32_t audio_rate;
    const zxm_slot *slots;                /* media slots, NULL-terminated */
    const zxm_input *inputs;              /* keyboard matrix, joysticks, mouse */
} zxm_info;

const zxm_info *zxm_lookup(const char *id);
size_t zxm_size(const zxm_info *info);           /* caller allocates */
int    zxm_create(zxm *m, const zxm_info *info, const zxm_rom_set *roms);
void   zxm_reset(zxm *m, zxm_reset_kind kind);   /* power-on, soft, NMI */
void   zxm_run_frame(zxm *m);                    /* exactly one video frame */
const void *zxm_framebuffer(const zxm *m);
const int16_t *zxm_audio(const zxm *m, int *samples);
void   zxm_input(zxm *m, const zxm_input_event *ev);
int    zxm_media_insert(zxm *m, const char *slot, const zxm_media *media);
int    zxm_media_eject(zxm *m, const char *slot);
size_t zxm_state_size(const zxm *m);
int    zxm_state_save(const zxm *m, void *buf, size_t len);
int    zxm_state_load(zxm *m, const void *buf, size_t len);
/* Debug: registers, memory, breakpoints, a per-T-state trace hook. */
const zxm_debug *zxm_debugger(zxm *m);
```

Rules the core keeps so the host can do what it needs:

- **No hidden state.** Everything that affects emulation is in the machine
  struct, so `zxm_state_save` is a complete snapshot and two instances never
  interfere. (The typing queue in `src/frontend/media.c` is a global today;
  it moves into the runtime.)
- **No I/O.** Media arrives as byte buffers or as callbacks (`zxm_media`
  carries read/write/seek for disks and SD cards); the core never opens a
  file. This is what makes the WebAssembly build and folder-backed storage
  possible.
- **No allocation after create.** The caller allocates `zxm_size()` bytes.
  Multi-megabyte machines (the Next's 2 MB RAM, an SD card's cache) still
  come out of that one block.
- **Deterministic.** Same state, same inputs at the same T-states, same
  output, on every host. Inputs are timestamped in T-states, so a recorded
  session (`.zxcr`, see [manifests.md](manifests.md)) replays exactly.

### Runtime: `zxc-run`

One process per running instance. It reads the instance manifest, loads the
ROM set, creates the machine, inserts the media, and then paces frames
against the host clock (50.08 Hz for a 48K, 50.02 Hz for a 128K, the
machine's own rate for the others), with audio as the master clock when
sound is on. It owns:

- the video and audio output (SDL2 window, Amiga screen, a canvas in the
  browser, or nothing when headless),
- the input mapping from host keys, pads and mice to the machine's inputs,
- media I/O: reading tape and disk files, writing back disk changes, and the
  folder-backed SD card,
- the control channel: ZCP commands from the host on stdin, events and
  replies on stdout.

Running each instance in its own process keeps a crash in one machine from
taking down the others and lets the host kill and restart a runtime cleanly.
On AmigaOS, where processes are cheap and share memory, the runtime is a
task started by the host, with the same message protocol over a message port.

**WebAssembly.** The core and runtime compile with clang to `wasm32` with no
Emscripten runtime dependency beyond what the web front end needs. The
module exports the ZCP entry points; the page feeds it frames through
`requestAnimationFrame` and an `AudioWorklet`, and stores instance folders in
the browser's Origin Private File System, with import and export as a zip.

### Host: `zxcradled`

A small C service, one per user, started on demand by the first client. It
keeps the instance registry and is the only thing that writes to instance
folders other than the runtimes themselves.

| Job | How |
| --- | --- |
| Registry | Scans `$ZXCRADLE_HOME/instances/*/instance.toml` (default `~/.zxcradle`). The folder is the source of truth; the host keeps no database. |
| Lifecycle | `create`, `start`, `pause`, `resume`, `stop`, `restart`, `destroy`. States: `stopped`, `starting`, `running`, `paused`, `stopping`, `failed`. |
| Verified restart | Restart, then watch for the machine's boot check (see below) within a time limit. If the check fails, the instance goes to `failed` and the previous state is kept for a field report. |
| Media changer | Keeps the instance's `media/` folder and slot assignments; inserting media while running sends it to the runtime. |
| Snapshots | Asks the runtime for a `.zxcs`, stores it in `snapshots/` with a name, a timestamp and a screenshot. Restores by starting the runtime from the file. Exports to `.sna`, `.z80` and `.szx` when the machine supports them. |
| Patch queue | Holds `.zxpatch` files in `patches/queue/`, applies them on the next verified restart, moves them to `patches/applied/` or `patches/failed/`. |
| Field reports | Builds a zip of the manifest, the last log, the last screenshot, a state file, the last ten seconds of input as a recording, and host details. |
| Events | Every lifecycle change, media change and error goes to `log/events.jsonl` and out to subscribed clients. |

**Boot checks.** Each machine manifest names how to tell that the machine
came up: a PC address the ROM reaches only once it is idle and waiting for
a key, a checksum of the screen once the copyright message is drawn, or text
recognised from the screen with the machine's own character set. An
instance can override it, for a machine that autoboots straight into a game.

### Control protocol: ZCP

Clients talk to the host, and the host talks to runtimes, in one protocol:
JSON objects, one per line, over a Unix domain socket
(`$ZXCRADLE_HOME/zxcradled.sock`), a named pipe on Windows, an Exec message
port on AmigaOS, or a WebSocket for the browser.

```json
{"id":7,"op":"media.insert","instance":"jetpac-48","slot":"tape","file":"jetpac.tzx"}
{"id":7,"ok":true}
{"event":"media.changed","instance":"jetpac-48","slot":"tape","file":"jetpac.tzx"}
```

Requests carry an `id`; replies echo it. Events have no `id`. The operations
are the ones listed in [zxc-shell.md](zxc-shell.md); each shell command is a
thin wrapper around one operation, and each MCP tool is too. Large payloads
(screenshots, state files) are written to a file in the instance folder and
the reply carries the path, so the protocol stays line based.

### Clients

- **Cradle Setup.** The web UI, served by the host on `localhost` and also
  shipped as a static page with the WebAssembly runtime built in. A grid of
  instance cards, a machine catalogue for creating instances, and per
  instance: the live screen, the media changer, snapshots, the patch queue,
  logs and the debugger. Styled by [style-guide.md](style-guide.md).
- **ZXC shell.** `zxc`, a command line tool and an interactive shell with a
  Spectrum-flavoured prompt. See [zxc-shell.md](zxc-shell.md).
- **MCP server.** `zxc mcp`, a Model Context Protocol server over stdio, so
  an AI agent can create instances, load software, type, take screenshots,
  read memory and file field reports. See [mcp.md](mcp.md).
- **Front ends.** The existing headless `zxrun` and the SDL2 and Amiga front
  ends stay as single-machine programs for people who want just an emulator.
  They become thin wrappers around the runtime.

## Instance folder

```
~/.zxcradle/
  zxcradled.sock
  machines/            user-added machine manifests (built-in ones are compiled in)
  library/             media shared between instances: the NextZXOS card, esxDOS, system disks
  roms/                shared ROM sets, by machine id
  instances/
    jetpac-48/
      instance.toml    the instance manifest
      media/           one folder per medium: sd/main/, microdrive/work/, disk/side-a/, tape/jetpac/
      snapshots/       name.zxcs + name.png
      patches/queue/   .zxpatch files waiting for the next restart
      patches/applied/
      patches/failed/
      recordings/      .zxcr input recordings
      log/             runtime.log, events.jsonl
      reports/         field report zips
```

An instance folder is self-contained except for shared ROMs, which are found
by checksum; `zxc export` copies the ROMs in so the folder runs anywhere.

**Media.** Every SD card, Microdrive cartridge, disk and tape is a folder
of its own with a `medium.toml`, backed either by an image file or by plain
host files that the runtime presents as the machine's file system. Shared
masters such as the NextZXOS card live in the library and are used through
per-instance overlays. All of this is in [storage.md](storage.md).

## Security

- The host listens only on a local socket or `127.0.0.1`, never on a public
  address. The WebSocket requires a token from `$ZXCRADLE_HOME/token`, which
  only the user can read.
- Instance names are restricted to `[a-z0-9-]{1,40}` and every file path in
  a request is resolved inside the instance folder; `..` and absolute paths
  are refused.
- Patches can only change ROM images, memory at boot and files in the
  instance folder. They cannot run host code.
- The MCP server exposes the same operations as the shell and nothing more.
  Destructive operations (`destroy`, `snapshot.delete`) need the instance
  name repeated as a confirmation argument.

## Phases

| Phase | Delivers |
| --- | --- |
| 0 | The core: Z80/Z80N, classic Spectrums, tape, snapshots (in progress, PR #1). Spectrum Next machine. |
| 1 | `zxm.h` interface, full state save/load, `zxc-run` with SDL2, the instance folder, `zxc` without a host (runs one instance in the foreground). |
| 2 | `zxcradled`, ZCP, lifecycle, media changer, snapshots, field reports, the MCP server. |
| 3 | Cradle Setup web UI, WebAssembly runtime, patch queue, verified restart. |
| 4 | Machines beyond the Spectrum family, in the order in [machines.md](machines.md). |

## Open questions

- Should the host be optional for good? Running `zxc run jetpac-48` without a
  service is simpler for one machine; a service is needed only for several
  machines and for the MCP server's event stream. The design above allows
  both.
- Which input recording granularity keeps files small enough to attach to
  every field report: every T-state change, or per frame with T-state
  offsets? Per frame with offsets is the current proposal.
- The Next's FPGA cores (other machines loaded as bitstreams) map naturally
  to Cradle machines, but some cores have no public timing reference yet.
