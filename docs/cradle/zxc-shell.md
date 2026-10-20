# ZXC shell

`zxc` is the command line for ZX Cradle. Run with a command, it does that one
thing and exits, so it works in scripts. Run with no command, it opens the
interactive shell. Every command maps to one ZCP operation (see
[design.md](design.md#control-protocol-zcp)), and the MCP server offers the
same set.

```
$ zxc
ZX Cradle 0.1  ·  3 instances, 1 running
K> ls
  NAME          MACHINE   STATE     SLOTS
  jetpac-48     zx48      running   tape: jetpac.tzx
  next-dev      next      stopped   sd: media/sd/
  plus3-work    plus3     paused    disk.a: work.dsk
K> use jetpac-48
jetpac-48 K> snap save before-level-3
saved snapshots/before-level-3.zxcs (49 KB)
jetpac-48 K> _
```

The prompt ends in `K>` in homage to the Spectrum's K (keyword) cursor. It
shows the selected instance first, if there is one. Errors print on the
error stream in the Spectrum report style, with a letter code and the line
that caused them, so scripts can match on the code:

```
jetpac-48 K> insert disk.a game.dsk
B Slot not found, 0:1   (zx48 has: tape, joystick)
```

| Code | Meaning |
| --- | --- |
| `0` | OK |
| `2` | Instance not found |
| `B` | Slot or option not found |
| `D` | Interrupted (Ctrl-C) |
| `E` | Instance is in the wrong state for this |
| `F` | File not found or unreadable |
| `H` | Boot check failed |
| `R` | ROM missing or checksum mismatch |

The format copies the Spectrum's reports (`0 OK, 0:1`, `D BREAK - CONT
repeats`), and `0` and `D` keep their Spectrum meaning. The other letters
are ZX Cradle's own and do not match the ROM's messages.

## Global options

| Option | Meaning |
| --- | --- |
| `-i NAME` | Act on this instance (instead of the shell's `use`) |
| `--json` | Print the raw ZCP reply, for scripts |
| `--no-host` | Run without `zxcradled`: `start` runs the instance in the foreground |
| `--home DIR` | Use this Cradle folder instead of `~/.zxcradle` |

## Commands

### Instances

| Command | Does |
| --- | --- |
| `ls` | List instances with machine, state and media |
| `machines [ID]` | List the machine catalogue, or show one machine's manifest |
| `create NAME MACHINE [--from FILE]` | Create an instance; `--from` copies in a tape, disk or snapshot and inserts it |
| `start [NAME]` | Start, restoring per the manifest's `restore` setting |
| `stop [NAME]` | Stop, saving a `last` snapshot first |
| `restart [NAME] [--cold] [--verify]` | Restart; `--verify` waits for the boot check and applies queued patches |
| `pause`, `resume` | Pause or resume emulation |
| `reset [NAME] [--hard\|--nmi]` | Soft reset, power cycle, or NMI |
| `show [NAME]` | Show the instance manifest and runtime status |
| `set KEY VALUE` | Change a manifest setting (`set run.speed 200`) |
| `rename OLD NEW`, `clone NAME NEW` | |
| `destroy NAME NAME` | Delete the instance folder; the name is typed twice |
| `export NAME FILE.zip`, `import FILE.zip` | Move an instance between computers, ROMs included |

### Media

| Command | Does |
| --- | --- |
| `media` | List files in the media folder and what is in each slot |
| `insert SLOT FILE` | Insert media; the file is copied into `media/` unless already there |
| `eject SLOT` | |
| `tape play\|stop\|rewind\|block N` | Tape deck controls |
| `tape mode instant\|real` | |

### Snapshots and recordings

| Command | Does |
| --- | --- |
| `snap save NAME`, `snap load NAME`, `snap ls`, `snap rm NAME NAME` | |
| `snap export NAME FILE.szx` | Export to `.szx`, `.z80` or `.sna` |
| `rec start`, `rec stop NAME`, `rec play NAME` | Input recordings |

### Input and output

| Command | Does |
| --- | --- |
| `type TEXT` | Type text through the keyboard matrix, with shift keys as needed; `\n` is ENTER |
| `key KEY [--hold MS]` | Press one key: `key enter`, `key caps+5`, `key sym+p` |
| `joy DIR [--fire]` | Joystick input |
| `screenshot [FILE.png]` | Save the current frame |
| `wait FRAMES` or `wait screen-text TEXT` | Wait for frames to pass or for text to appear |

### Patches and reports

| Command | Does |
| --- | --- |
| `patch queue FILE.zxpatch`, `patch ls`, `patch drop ID` | The patch queue |
| `report ["what happened"]` | Build a field report zip and print its path |
| `log [-f]` | Show the event log; `-f` follows it |

### Debugger

| Command | Does |
| --- | --- |
| `regs` | CPU registers, flags, T-state in frame |
| `peek ADDR [LEN]`, `poke ADDR VALUE...` | Memory, in the CPU's current paging; `bank:addr` for a specific page |
| `dis [ADDR] [N]` | Disassemble, Z80N opcodes included on the Next |
| `break ADDR`, `watch ADDR`, `break io PORT` | Breakpoints and watchpoints |
| `step [N]`, `over`, `cont` | |
| `trace on\|off FILE` | Per-instruction trace with T-states, for comparing against real hardware logs |

## Scripts

A file of shell commands runs with `zxc run-script FILE`, or with
`#!/usr/bin/env zxc` on the first line. Lines starting with `#` are
comments. A script stops at the first command that does not report `0`
unless the line starts with `-`.

```sh
#!/usr/bin/env zxc
# Boot a fresh 48K, load Jetpac, check it reached the title screen.
create ci-jetpac zx48 --from jetpac.tzx
start ci-jetpac
wait screen-text "JETPAC"
screenshot title.png
-destroy ci-jetpac ci-jetpac
```
