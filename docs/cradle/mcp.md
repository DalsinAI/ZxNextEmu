# MCP server

`zxc mcp` runs a [Model Context Protocol](https://modelcontextprotocol.io)
server over stdio. It lets an AI agent use ZX Cradle the way a person uses
the shell: create a machine, load software, type into it, look at the
screen, read memory and file a field report. It connects to `zxcradled`,
starting it if needed, and every tool is one ZCP operation.

```json
{
  "mcpServers": {
    "zx-cradle": { "command": "zxc", "args": ["mcp"] }
  }
}
```

Options: `--http` serves MCP over HTTP on `127.0.0.1:7090` instead of stdio,
`--instances a,b` limits the server to those instances,
`--read-only` removes every tool that changes an instance, and
`--no-debug` removes the debugger tools.

## Tools

| Tool | Arguments | Returns |
| --- | --- | --- |
| `list_machines` | `family?` | machine ids, names, slots |
| `list_instances` | | name, machine, state, media per slot |
| `create_instance` | `name`, `machine`, `media?` (path) | the instance manifest |
| `start_instance`, `stop_instance`, `pause_instance`, `resume_instance` | `name` | new state |
| `restart_instance` | `name`, `cold?`, `verify?` | new state, boot check result |
| `reset_instance` | `name`, `kind`: `soft`, `hard` or `nmi` | |
| `list_media`, `create_medium`, `add_medium`, `fetch_system_medium`, `commit_overlay`, `reset_overlay` | see [storage.md](storage.md#commands) | |
| `insert_media` | `name`, `slot`, `medium` or `path` | slot contents |
| `eject_media` | `name`, `slot` | |
| `type_text` | `name`, `text` | frames taken |
| `press_keys` | `name`, `keys` (e.g. `["caps+5"]`), `hold_ms?` | |
| `run_frames` | `name`, `frames` | for a paused instance: advance exactly this many frames |
| `wait_for_text` | `name`, `text`, `timeout_s?` | whether it appeared, frames waited |
| `screenshot` | `name`, `scale?` | the frame as a PNG image |
| `read_screen_text` | `name` | the text on screen, recognised with the machine's character set |
| `save_snapshot`, `load_snapshot`, `list_snapshots` | `name`, `snapshot` | |
| `read_memory` | `name`, `addr`, `length` (max 4096), `bank?` | hex bytes |
| `write_memory` | `name`, `addr`, `bytes`, `bank?` | |
| `get_registers` | `name` | registers, flags, T-state |
| `disassemble` | `name`, `addr?`, `count?` | lines of Z80 / Z80N assembly |
| `set_breakpoint`, `clear_breakpoint` | `name`, `addr` or `port`, `kind` | |
| `step` | `name`, `count?` | registers after stepping |
| `queue_patch` | `name`, `path` | queue position |
| `field_report` | `name`, `summary` | path of the report zip |
| `destroy_instance` | `name`, `confirm_name` | `confirm_name` must equal `name` |

`screenshot` returns MCP image content, so the agent sees the screen
directly. `read_screen_text` matches 8x8 cells against the machine's ROM
character set (and the Next's tilemap and Layer 2 text where possible), which
is faster and cheaper than an image when the agent only needs to read a
menu or a BASIC listing.

## Resources

| URI | Contents |
| --- | --- |
| `zxcradle://instances` | the instance list, as JSON |
| `zxcradle://instances/{name}/manifest` | `instance.toml` |
| `zxcradle://instances/{name}/screen` | the current frame, PNG |
| `zxcradle://instances/{name}/log` | the last 200 events |
| `zxcradle://machines/{id}` | a machine manifest |

The server sends resource update notifications when an instance changes
state or media, so an agent subscribed to `zxcradle://instances` sees
changes made from the shell or the web UI.

## Prompts

| Prompt | Does |
| --- | --- |
| `load-and-run` | Create an instance for a file, pick the machine from the file type and contents, load it, wait for it to start |
| `diagnose` | Take a screenshot and registers of an instance that looks stuck, then file a field report |

## Limits

- The server never reads or writes files outside `~/.zxcradle` and paths the
  client passes explicitly for `create_instance`, `insert_media` and
  `queue_patch`.
- Memory reads are capped at 4 KB per call and `run_frames` at 3000 frames
  (one minute) per call, so one tool call cannot stall the agent.
- The server downloads nothing except system media (`fetch_system_medium`)
  from the official sources named in the built-in machine manifests.
  Software is passed as local paths.
