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
5. **Easy to run.** Open a web page, or run one `npx`-style command. No
   installer, no native binaries to build or download.
6. **JavaScript and WebAssembly only, for now** (Dale, 2026-10-20). All of
   ZX Cradle (host, runtime, shell, MCP server, UI) is JavaScript. The only
   compiled code is the emulator core, which ZX Cradle loads as a
   WebAssembly module. There are no native Cradle builds until this is
   revisited. The core itself stays portable C99 in this repository and
   keeps its own native front ends; ZX Cradle simply does not use them.

## Concepts

| Term | Meaning |
| --- | --- |
| Machine | A model the core can emulate: `zx48`, `zx128`, `plus3`, `next`, `cpc6128`... Described by a machine manifest. |
| Instance | A configured machine with a name, a folder and a state: `jetpac-48`, `next-dev`. |
| Runtime | The worker that runs one instance: the core's WebAssembly module plus a little JavaScript around it. |
| Host | `zxcradled`, the JavaScript service (Node.js, or the page itself in a browser-only setup) that keeps the instance registry and supervises runtimes. |
| Slot | A place media goes: `tape`, `disk.a`, `disk.b`, `cart`, `sd`. Each machine declares its slots. |
| Medium | One SD card, Microdrive cartridge, disk, tape or cartridge, kept in its own folder, image-backed or folder-backed. See [storage.md](storage.md). |
| Media changer | The instance's media and which one is in each slot. |
| State | A full snapshot of an instance (`.zxcs`), exact to the T-state. |
| Patch | A `.zxpatch` archive queued against an instance and applied on its next verified restart. |
| Field report | A `.zip` of everything needed to reproduce a problem with an instance. |

## Layers

```
 ┌───────────────────────────────────────────────────────────────┐
 │  Cradle Setup (web UI)      zxc shell        MCP server       │  clients (JS)
 └──────────────┬───────────────┬──────────────┬─────────────────┘
                │   ZCP (JSON messages over WebSocket, stdio or postMessage)
 ┌──────────────┴───────────────┴──────────────┴─────────────────┐
 │  zxcradled: instance registry, supervisor, media, snapshots,  │  host (JS)
 │  patch queue, field reports, event log                        │
 └──────────────┬────────────────────────────────────────────────┘
                │   ZCP over postMessage
 ┌──────────────┴────────────────────────────────────────────────┐
 │  runtime: one Worker per instance. Pacing, video, audio,      │  runtime (JS)
 │  input, media I/O.                                            │
 └──────────────┬────────────────────────────────────────────────┘
                │   WebAssembly exports (the zxm.h API)
 ┌──────────────┴────────────────────────────────────────────────┐
 │  zxcore.wasm: Z80/Z80N CPU + machine modules, built from the  │  core (C → wasm)
 │  C sources with clang --target=wasm32. No I/O, no globals.    │
 └───────────────────────────────────────────────────────────────┘
```

Everything above the core is plain JavaScript (ES modules, no build step
needed to run it; TypeScript type declarations shipped alongside). The same
modules run in two places:

- **In the browser**, with nothing installed: the page is the host, each
  instance runs in a Web Worker, and instance folders live in the Origin
  Private File System, or in a real folder on disk the user picks through
  the File System Access API.
- **Under Node.js**, for the shell, the MCP server and folder-backed media
  on the real file system: `zxcradled` runs in Node, each instance runs in a
  `worker_threads` Worker, and Cradle Setup connects to it over a WebSocket.

### Core: `libzxcore`

The existing C code in `src/z80` and `src/zx`, compiled to `zxcore.wasm`.
Today it exposes the Spectrum family through `zx.h`. For ZX Cradle it grows
one generic interface, `zxm.h`, that every machine module implements and
that the WebAssembly module exports, so the JavaScript runtime never needs
to know a machine's own structures:

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
  file. The JavaScript runtime supplies these callbacks as WebAssembly
  imports, which is what makes folder-backed storage possible.
- **No allocation after create.** The caller allocates `zxm_size()` bytes
  in the module's linear memory. Multi-megabyte machines (the Next's 2 MB
  RAM, an SD card's cache) still come out of that one block, and the
  runtime reads the framebuffer and audio buffer straight out of linear
  memory without copying.
- **No libc beyond freestanding.** The wasm build uses no Emscripten and no
  WASI: `memcpy`, `memset` and friends come from a tiny bundled
  implementation, so the module stays small (target: under 300 KB for all
  Spectrum models) and loads the same in browsers and Node.
- **Deterministic.** Same state, same inputs at the same T-states, same
  output, on every host. Inputs are timestamped in T-states, so a recorded
  session (`.zxcr`, see [manifests.md](manifests.md)) replays exactly.

### Runtime

One Worker per running instance (a Web Worker in the browser, a
`worker_threads` Worker in Node). It loads `zxcore.wasm`, reads the instance
manifest, loads the ROM set, creates the machine, inserts the media, and
paces frames against the host clock (50.08 Hz for a 48K, 50.02 Hz for a
128K, the machine's own rate for the others), with audio as the master clock
when sound is on. It owns:

- video and audio: frames go to an `OffscreenCanvas` transferred from the
  page, audio to an `AudioWorklet` through a `SharedArrayBuffer` ring
  buffer; headless instances (under Node, or hidden in the UI) skip both,
- the input mapping from keys, pads and mice to the machine's inputs,
- media I/O: the block, sector and stream callbacks the core imports,
  backed by image files or by the folder-backed file systems in
  [storage.md](storage.md),
- the control channel: ZCP messages from the host over `postMessage`.

A Worker per instance keeps a hung machine from freezing the others or the
UI, and lets the host terminate and restart an instance cleanly.

### Host: `zxcradled`

A JavaScript module, one per user. Under Node it is a small service started
on demand by the first client; in a browser-only setup the page runs it
itself. It keeps the instance registry and is the only thing that writes to instance
folders other than the runtimes themselves.

| Job | How |
| --- | --- |
| Registry | Scans `$ZXCRADLE_HOME/instances/*/instance.toml` (default `~/.zxcradle` under Node; the chosen folder or OPFS in the browser). The folder is the source of truth; the host keeps no database. |
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
JSON messages. Between the host and its runtimes they travel over
`postMessage`; between Node and its clients, over a WebSocket on
`127.0.0.1` (Cradle Setup) or as JSON lines on stdio (the shell and the MCP
server can also run the host in-process).

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

- **Cradle Setup.** The web UI: plain JavaScript and CSS using the tokens
  and components in `tokens/`, served by the Node host on `localhost` or
  opened as a static page that runs the host itself. A grid of
  instance cards, a machine catalogue for creating instances, and per
  instance: the live screen, the media changer, snapshots, the patch queue,
  logs and the debugger. Styled by [style-guide.md](style-guide.md).
- **ZXC shell.** `zxc`, a Node.js command line tool and an interactive shell
  with a Spectrum-flavoured prompt. See [zxc-shell.md](zxc-shell.md).
- **MCP server.** `zxc mcp`, a Node.js Model Context Protocol server over stdio, so
  an AI agent can create instances, load software, type, take screenshots,
  read memory and file field reports. See [mcp.md](mcp.md).
- **Not used by ZX Cradle.** The emulator's own native front ends (`zxrun`,
  SDL2, Amiga) stay in this repository as standalone emulators for people
  who want just that. ZX Cradle neither builds nor depends on them.

## Instance folder

```
~/.zxcradle/
  token                WebSocket token (Node host)
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

- The Node host listens only on `127.0.0.1`, never on a public address, and
  the WebSocket requires the token in `$ZXCRADLE_HOME/token`, which only the
  user can read. In the browser, everything stays inside the page's origin
  and the folders the user grants.
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
| 1 | `zxm.h` interface, full state save/load, the `zxcore.wasm` build, the JavaScript runtime in a browser page running one instance. |
| 2 | Host, ZCP, instance folders (OPFS and File System Access), media changer, folder-backed SD card, snapshots, Cradle Setup. |
| 3 | The Node host, the `zxc` shell, the MCP server, field reports, patch queue, verified restart. |
| 4 | Machines beyond the Spectrum family, in the order in [machines.md](machines.md). |

## Open questions

- Does the WebAssembly core keep up? A 3.5 MHz Spectrum stepped per
  T-state is cheap, but a 28 MHz Next is not; measure early, and if a
  browser cannot hold 50 Hz, look at the core's hot loop before any native
  fallback.
- When, if ever, native builds of Cradle come back. Not for now.
- Which input recording granularity keeps files small enough to attach to
  every field report: every T-state change, or per frame with T-state
  offsets? Per frame with offsets is the current proposal.
- The Next's FPGA cores (other machines loaded as bitstreams) map naturally
  to Cradle machines, but some cores have no public timing reference yet.
