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
| Slot | A place media goes: `tape`, `snapshot`, `sd0`, `uart0`, `disk.a`, `cart`. Each machine declares its slots. |
| Medium | One SD card, Microdrive cartridge, disk, tape or cartridge, kept in its own folder, image-backed or folder-backed. See [storage.md](storage.md). |
| Media changer | The instance's media and which one is in each slot. |
| State | A full snapshot of an instance (`.zxcs`), exact to the T-state. |
| Patch | A `.zxpatch` archive queued against an instance and applied on its next verified restart. |
| Recipe | A ready-made setup (machine, software to fetch, media layout, first boot) that makes a new instance. See [recipes.md](recipes.md). |
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

The C code in `src/z80`, `src/zx` and `src/core`, compiled to one
`zxcore.wasm` (about 150 KB with every machine) that exports one generic
interface, [`src/core/zxm.h`](../../src/core/zxm.h) (PR #1). Every machine
implements it, so the JavaScript runtime never needs to know a machine's own
structures:

| Call | Does |
| --- | --- |
| `zxm_lookup(id)` | Machine description: name, clock, T-states per frame, framebuffer size and format (RGB333), audio rate, ROM file names, media slots, inputs |
| `zxm_size`, `zxm_create` | The caller allocates the instance in linear memory and creates it from ROM bytes |
| `zxm_reset` | Power on, reset button or NMI |
| `zxm_run_frame` | Exactly one video frame; the picture and stereo int16 audio are then read straight out of the instance |
| `zxm_input` | Typed input events: keyboard matrix, Kempston joystick, Kempston mouse, release all; each carries its T-state in the frame |
| `zxm_media_insert`, `zxm_media_eject` | Media as bytes (tape, snapshot), as a block device (SD card) or as a serial device (UART) |
| `zxm_state_size`, `zxm_state_save`, `zxm_state_load` | Complete machine state, for `.zxcs` |
| `zxm_debugger` | Registers, peek and poke as the CPU sees memory, frame count |

Machine-specific slots and inputs are listed in `zxm_info`, so the runtime
builds its media changer and input mapping from data. Input events are
applied at the next frame start today; applying them at their exact T-state
is a later step that the event format already allows for.

Rules the core keeps so the host can do what it needs:

- **No hidden state.** Everything that affects emulation is in the machine
  struct, so `zxm_state_save` is a complete snapshot and two instances never
  interfere. Typing text is the runtime's job, done with `zxm_input`.
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
  implementation, so the module stays small (about 150 KB for every machine today) and loads the same in browsers and Node.
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
`postMessage`; between Node and its clients, over a WebSocket at
`ws://127.0.0.1:7080/zcp` (Cradle Setup) or as JSON lines on stdio (the shell and the MCP
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
  and components in `tokens/`, served by the Node host at `http://127.0.0.1:7080/` or
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

## Ports

Every ZX Cradle port is in the 7000 range, so it never collides with Amiga
Cradle (amigachrome) running on the same computer (Dale, 2026-10-20). All of
them listen on `127.0.0.1` only.

| Port | What |
| --- | --- |
| 7080 | The Node host: Cradle Setup over HTTP, and ZCP over a WebSocket at `/zcp` |
| 7081 | Development server for the browser-only page (`zxc serve`) |
| 7090 | The MCP server over HTTP, when started with `zxc mcp --http`; the default is stdio, with no port |
| 7100 to 7199 | Per-instance ports, handed out in order and recorded in `instance.toml`: a debugger connection, a TCP bridge for an emulated serial port |

If a port is taken, the host says which program has it and stops, rather
than picking another port silently. `ZXCRADLE_PORT` moves the base port
(7080, with the others moved by the same amount), still within 7000 to 7999.

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
| 0 | The core: Z80/Z80N, classic Spectrums, Spectrum Next, tape, snapshots, `zxm.h`, full state save/load and the `zxcore.wasm` build (PR #1). |
| 1 | The JavaScript runtime in a browser page running one instance. |
| 2 | Host, ZCP, instance folders (OPFS and File System Access), media changer, folder-backed SD card, snapshots, Cradle Setup. |
| 3 | The Node host, the `zxc` shell, the MCP server, field reports, patch queue, verified restart. |
| 4 | Machines beyond the Spectrum family, in the order in [machines.md](machines.md). |

## Performance

Measured on PR #1's `zxcore.wasm`: one Spectrum Next frame takes about
3 ms, against a 20 ms budget at 50 Hz, so the WebAssembly core keeps up even
at the Next's 28 MHz with room for several instances at once. The classic
Spectrums cost much less. No native fallback is needed.

## Open questions

- When, if ever, native builds of Cradle come back. Not for now.
- Which input recording granularity keeps files small enough to attach to
  every field report: every T-state change, or per frame with T-state
  offsets? Per frame with offsets is the current proposal.
- The Next's FPGA cores (other machines loaded as bitstreams) map naturally
  to Cradle machines, but some cores have no public timing reference yet.
