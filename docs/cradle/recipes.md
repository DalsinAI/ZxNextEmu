# Recipes

A **recipe** is a ready-made machine setup: "SymbOS on a Spectrum Next",
"NextZXOS with wifi", "MSX2 with Nextor". Picking a recipe in Cradle Setup
(or `zxc recipe make symbos-next`) creates an ordinary instance, fetches the
software it needs, lays out its media and boots it to a known point. After
that the instance is like any other: the recipe is how it was made, not
something it keeps depending on.

Recipes are data, never code. A recipe cannot run host code, cannot reach a
URL it does not name with a checksum, and cannot write outside the new
instance's folder and the library.

## What a recipe holds

A recipe is a folder with a `recipe.toml` and any small files it adds
(an `autoexec`, a key map, an icon):

```
recipes/symbos-next/
  recipe.toml
  icon.png
  files/            copied into the instance's media, see [[media]] below
```

Built-in recipes are compiled into Cradle like built-in machines. Users add
their own in `~/.zxcradle/recipes/`, and `zxc recipe export NAME` turns an
existing instance back into a recipe (manifest, media layout and checksums,
without the downloaded software).

```toml
id       = "symbos-next"
title    = "SymbOS desktop"
summary  = "Prodatron's multitasking desktop OS, on a 2 MB Spectrum Next"
machine  = "next"
version  = 1                    # of the recipe, not of the software

[requires]                      # checked against zxm_info before anything is fetched
ram_kb   = 1024
inputs   = ["keyboard", "mouse"]
slots    = ["sd0"]

[instance]                      # merged into the new instance.toml
run.speed     = 100
display.scale = 2
input.mouse   = "pointer-lock"
machine.ram   = "2M"

# Software the recipe needs. Each item is fetched once into the library,
# checked, and shared by every instance made from any recipe.
[[fetch]]
id       = "nextzxos"           # a system medium already known to `media fetch`
[[fetch]]
id       = "symbos-4.0-next"
title    = "SymbOS 4.0 for the ZX Spectrum Next (mass storage package)"
url      = "http://www.symbos.de/files/<package>.zip"
sha256   = "<pinned when the recipe is written>"
size     = 0                    # bytes, to show before downloading
licence  = "freeware (symbos.de); not redistributed by ZX Cradle"
homepage = "http://www.symbos.de/download.htm"
unpack   = "zip"

[[media]]                       # how the instance's media are built
slot   = "sd0"
name   = "sd/main"
kind   = "sd"
format = "fat32"
write  = "overlay"
layers = ["library:sd/nextzxos-latest", "fetch:symbos-4.0-next", "files/sd"]

[first_boot]                    # once, right after the instance is made
check   = "screen-text"
text    = "SymbOS"
timeout = 120                   # seconds of emulated time
snapshot = "desktop"            # saved when the check passes

[[notes]]
text = "Right Alt releases the mouse."
```

## What making an instance from a recipe does

1. **Check.** The host compares `[requires]` with the machine's `zxm_info`
   and the instance options. A recipe that the build cannot run (a missing
   machine, too little RAM) is shown greyed out with the reason.
2. **Show what will be downloaded.** Each `[[fetch]]` item that is not in
   the library yet is listed with its title, size, source and licence line.
   Nothing is fetched until the user accepts. This is the user getting the
   software from its author, the same as clicking the download link.
3. **Fetch** into `library/fetch/<id>/` (the original archive plus
   `fetch.toml` with URL, sha256 and date). A checksum mismatch stops here:
   a new upstream release means a new recipe version, not a silent change.
4. **Build the media.** Each `[[media]]` entry becomes a medium folder in the
   instance. `layers` is a stack, bottom first, of read-only sources: a
   library medium, an unpacked fetch, the recipe's own `files/`. The machine
   sees them merged into one volume, upper layers winning on a name clash,
   with the instance's overlay on top for its writes. An entry can instead
   name `image = "fetch:<id>/<file>"` to use a downloaded disk image as an
   image-backed medium.
5. **Write `instance.toml`** from the machine defaults plus `[instance]`.
6. **First boot.** Start the instance, wait for `[first_boot].check`, save
   the named snapshot and set `run.restore` to it, so later starts open
   straight on the desktop. If the check times out, the instance stays,
   marked `failed`, with a field report attached.

## Fetching in the browser

A page can only download from servers that send CORS headers, and many
retro software sites (symbos.de included) do not, and some are plain HTTP.
So:

- **Under the Node host**, `zxcradled` fetches the URL itself.
- **Browser only**, Cradle Setup opens the recipe's `homepage` in a new tab
  and asks the user to drop the downloaded file onto the page. The file is
  accepted only if its sha256 matches, so the result is the same as a fetch.

Either way the software comes from its author's site, and Cradle never
mirrors it unless the author has said that is fine (recorded in the recipe
as `mirror = "<url>"` with the permission noted in `licence`).

## Commands and protocol

| Command | ZCP op | Does |
| --- | --- | --- |
| `recipes` | `recipe.list` | List recipes, with why any cannot run here |
| `recipe show ID` | `recipe.get` | Print a recipe, its downloads and their licences |
| `recipe make ID [NAME]` | `recipe.make` | Steps 1 to 6 above; events report each step |
| `recipe export NAME [ID]` | `recipe.export` | Turn an instance into a recipe folder |

The MCP server gets `list_recipes` and `make_from_recipe`. An agent can make
an instance from a recipe, but accepting a download stays a user action:
the tool returns the download list and needs `accept_downloads` repeated
back with the recipe id, like the destructive tools in [mcp.md](mcp.md).

## Recipes so far

| Recipe | Machine | Document |
| --- | --- | --- |
| `symbos-next` | Spectrum Next | [recipes/symbos.md](recipes/symbos.md) |
| `symbos-msx2` | MSX2 | [recipes/symbos.md](recipes/symbos.md) |
