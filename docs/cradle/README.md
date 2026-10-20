# ZX Cradle

ZX Cradle is a machine manager for Z80 computers. It does for the Spectrum
family, and later for every Z80A machine worth running, what Amiga Cradle
does for the Amiga: you register machines as named instances, start, stop
and restart them, swap their tapes and disks, snapshot them, queue patches
for them and pull a field report when something goes wrong. Each instance
keeps its whole world in one folder. ZX Cradle is written in JavaScript and runs
the emulator as WebAssembly, in a browser or under Node.js, with no native
code of its own.

The emulation itself is the ZxNextEmu core in this repository: a portable C99
library, timed to the T-state, with no screen, sound or file system of its
own. ZX Cradle is everything around it.

| Document | What it covers |
| --- | --- |
| [design.md](design.md) | Architecture: core library, runtime, host, shell, MCP server, web UI |
| [machines.md](machines.md) | The machine catalogue and the order machines arrive in |
| [storage.md](storage.md) | Media: a folder per SD card, Microdrive, disk and tape; overlays; the shared library; the Next system card |
| [manifests.md](manifests.md) | On-disk formats: machine and instance manifests, state files, patches, field reports |
| [zxc-shell.md](zxc-shell.md) | The `zxc` command line and interactive shell |
| [mcp.md](mcp.md) | The MCP server: tools and resources for driving instances from an AI agent |
| [style-guide.md](style-guide.md) | Colour, type, layout and components for every ZX Cradle surface |
| [tokens/zxcradle.css](tokens/zxcradle.css) | The style guide's design tokens as CSS custom properties |

Status: design. Nothing in these documents is built yet except the core
(see the status table in the top-level README). Names, commands and formats
are proposals and can change until the first release that ships them.
