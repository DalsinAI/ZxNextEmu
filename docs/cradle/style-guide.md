# ZX Cradle style guide

ZX Cradle should look like it was made in the same factory as the machines it
runs: a black case, grey rubber keys with white legends, and the four-colour
stripe across one corner. Everything else stays quiet so the emulated screen,
with its own loud colours, is the brightest thing on the page.

The tokens below are in [tokens/zxcradle.css](tokens/zxcradle.css). Every
ZX Cradle surface (Cradle Setup, the web runtime, the `zxc` shell,
documentation) uses them.

## Principles

1. **The machine's screen is the hero.** UI chrome is black, grey and white.
   Colour in the chrome means something: a state, an action, an error.
2. **The stripe is a signature, not a pattern.** It appears once per view, in
   the same place, at the same angle.
3. **Square and solid.** Small radii, hairline borders, no gradients, no
   glass, no drop shadows except on the floating debugger.
4. **The 8-pixel cell.** Spacing, icon grids and the shell's layout follow
   the Spectrum's 8x8 character cell.
5. **Works in the dark.** Dark ("case") is the default theme. Light
   ("paper") must work just as well and is chosen by the system setting or
   a toggle.

## Colour

### The stripe

| Token | Hex | Use |
| --- | --- | --- |
| `--zxc-stripe-red` | `#E3262A` | stripe, graphics |
| `--zxc-stripe-yellow` | `#F7C51E` | stripe, primary buttons, focus ring on the case theme |
| `--zxc-stripe-green` | `#36A852` | stripe, graphics |
| `--zxc-stripe-cyan` | `#1C9AD6` | stripe, graphics |

The four stripe colours always appear together and always in this order,
red, yellow, green, cyan, reading left to right. They are slightly warmer and
deeper than the Spectrum's own palette so they read as print rather than as
a screen.

The stripe colours are for graphics. Red, green and cyan stripe colours fall
below 4.5:1 for text on one theme or the other, so text uses the
`--zxc-text-*` tokens instead.

### Neutrals

| Token | Dark (case) | Light (paper) | Use |
| --- | --- | --- | --- |
| `--zxc-bg` | `#0E0F11` | `#EDEFF2` | page background |
| `--zxc-surface` | `#181A1E` | `#FFFFFF` | panels, cards |
| `--zxc-surface-2` | `#22252B` | `#F6F7F9` | inputs, raised panels |
| `--zxc-line` | `#2E323A` | `#D3D7DD` | borders, dividers |
| `--zxc-key` | `#5C6670` | `#5C6670` | keycaps, secondary buttons |
| `--zxc-fg` | `#E9EBEE` | `#141519` | text |
| `--zxc-muted` | `#9AA3AD` | `#5A626B` | secondary text |

The neutrals lean slightly blue, after the 48K's grey-blue rubber keys.

### Signal colours for text

| Token | Dark | Light | Contrast (dark / light background) |
| --- | --- | --- | --- |
| `--zxc-text-red` | `#FF5A52` | `#C0181C` | 6.2 / 5.4 |
| `--zxc-text-yellow` | `#F7C51E` | `#7A5D00` | 11.8 / 5.4 |
| `--zxc-text-green` | `#4CC266` | `#1E7A36` | 8.4 / 4.7 |
| `--zxc-text-cyan` | `#3FB2E8` | `#0B6FA8` | 8.0 / 4.7 |

### Roles

| Role | Token | Notes |
| --- | --- | --- |
| Primary action | `--zxc-accent` (stripe yellow) with `--zxc-on-accent` text | One primary button per view |
| Focus ring | `--zxc-focus`, 2px outline, 2px offset | Stripe yellow on the case theme, ink black on paper, where yellow would fall below 3:1 |
| Link | `--zxc-link` (text cyan) | Underlined in running text |
| Danger | `--zxc-danger` (text red) | Destroy, delete snapshot, drop patch |

### Instance states

| State | Letter | Token |
| --- | --- | --- |
| Running | `R` | `--zxc-state-running` (green) |
| Paused | `P` | `--zxc-state-paused` (yellow) |
| Starting, stopping, restarting | `S` | `--zxc-state-starting` (cyan) |
| Failed | `F` | `--zxc-state-failed` (red) |
| Stopped | `-` | `--zxc-state-stopped` (muted) |

A state is always shown with its letter and its word, never by colour alone.

### The Spectrum palette

The `--zx-*` tokens hold the machine's 15 colours: normal at `0xD7`, bright at
`0xFF`. Use them only for things that show what the machine shows: the
border swatch on an instance card, an attribute editor, a palette picker in
the debugger. Never use them for UI chrome. (The core renders in RGB333 and
the runtime converts with the same values, so the swatches match the
screen.)

## Type

| Role | Face | Token | Notes |
| --- | --- | --- | --- |
| Display | Chakra Petch, 600 | `--zxc-font-display` | The wordmark, page titles, panel titles. Squared corners echo the Sinclair-era lettering without copying it. |
| Body | IBM Plex Sans, 400 / 600 | `--zxc-font-body` | All UI and documentation text |
| Mono | IBM Plex Mono, 400 / 500 | `--zxc-font-mono` | The shell, addresses, registers, file names, tables of numbers |

All three are open fonts (SIL OFL) on Google Fonts and can be bundled with
Cradle Setup for offline use.

Scale: 12, 14, 16, 20, 28, 40 px (`--zxc-size-xs` to `--zxc-size-2xl`). Body
text is 16 px with a line height of 1.5; UI text is 14 px. Uppercase labels
use 12 px with 0.08em letter spacing. Hex values are always upper case with a
`0x` prefix in the UI (`0x5C00`) and `$` is never used, so the UI and the
shell read the same.

**The machine's own font.** Where ZX Cradle shows text that comes from the
machine (screen text recognised by `read_screen_text`, the debugger's memory
view as characters), it draws it with the instance's ROM character set, read
from the ROM at run time. ZX Cradle never ships a copy of a ROM font.

## Layout and space

- Spacing steps: 4, 8, 16, 24, 32, 48 px. Gaps between related controls are
  8; between groups 16 or 24; between sections 32 or 48.
- Radius: 2 px on panels, buttons, chips and inputs. Keycaps use 4 px.
  Nothing is round except the record indicator.
- Borders are 1 px `--zxc-line`. A panel is a surface and a border, with no
  shadow.
- The emulated screen is always shown at an integer scale with nearest-
  neighbour scaling, and with its border unless the user turns it off. It
  sits on `--zxc-bg` with no frame of its own: the emulated border is the
  frame.

## Components

### The stripe

Four parallel bands at 60 degrees, each as wide as the others, with no gap,
cut off by the edge of the element they sit on. It goes in the top-right
corner of the app bar on every Cradle Setup view, and in the bottom-right
corner of a splash or about screen. It is never animated, recoloured,
reordered or used as a divider.

### Wordmark

"ZX Cradle" in Chakra Petch 600, with "ZX" in `--zxc-fg` and "Cradle" in
`--zxc-muted`, followed by the stripe. The shell's banner uses the same words
in plain text.

### Instance card

A card shows, from top to bottom: the live screen thumbnail at 1x (320 by 256
for the Spectrums) with the emulated border included, so a loading tape
stripes the card exactly as it stripes the TV; then the instance name in
mono, the machine name, the state chip, and the media in each slot. The card
itself is a plain surface with a 1 px line and no coloured edge: the
emulated border is the only colour it carries.

### State chip

A 2 px radius chip: the state letter in mono, bold, in the state colour,
inside a 16 px square outlined in the same colour, then the state word in
`--zxc-fg`. A failed instance's chip uses FLASH (below), which fills the
square with the state colour and turns the letter `--zxc-bg`.

### Buttons

| Kind | Look | Use |
| --- | --- | --- |
| Primary | stripe yellow fill, case black text | Start, Create, Restore |
| Secondary | `--zxc-surface-2` fill, `--zxc-line` border | most actions |
| Key | `--zxc-key` fill, white text, 4 px radius | keyboard shortcuts, on-screen keyboard |
| Danger | transparent, `--zxc-danger` border and text | destructive actions; asks for the instance name |

Buttons are 32 px tall (four cells), 14 px text, 16 px side padding.

### Keycap

For shortcuts and the on-screen keyboard, keycaps follow the 48K's legends:
the key's letter large in white with its keyword under it, the extended-mode
word above the key in `--zxc-text-green`, and the symbol-shift word below
it in `--zxc-text-red` (on the real key that word is printed on the key; it
moves below so it keeps its contrast). Shortcuts in menus show only the
letter.

### Media slot

A row with a slot icon (tape, disk, cartridge, SD card), the slot id in
mono, the file name, and Eject. Dropping a file on the row inserts it. A tape
slot adds the deck controls and a block counter.

### Shell

The `zxc` shell maps ANSI colours onto the stripe and signal tokens: the
prompt `K>` in yellow, instance names in cyan, errors in red with the report
code in bold, success in green. With `NO_COLOR` set it prints plain text.

## FLASH

The Spectrum's FLASH attribute swaps ink and paper every 16 frames. ZX Cradle
uses it for exactly one thing: a state that needs the user's attention
(a failed instance, a boot check that timed out). It swaps every 32 frames
(0.64 s), which keeps it under three changes a second, and it stops when the
user has seen it or when `prefers-reduced-motion` is set.

## Motion

Transitions are 120 ms, ease-out, on opacity and position only. The live
screen is never animated by the UI.

## Icons

Drawn on an 8 by 8 or 16 by 16 pixel grid, 1 px (at 1x) strokes, square
ends. Slots: cassette, 3-inch disk, cartridge, SD card. Actions: play
(triangle), pause, stop (square), eject, snapshot (camera), report (folded
page).

## Writing

- Name things the way the user does: "tape", "disk", "snapshot", not "media
  object" or "state blob".
- Buttons say what they do: "Start", "Insert tape", "Save snapshot".
- Errors say what went wrong and what to do next: "The 128K needs the ROM
  file 128-0.rom. Put it in ~/.zxcradle/roms/zx128/ and press Start again."
- Machine names are written as their makers wrote them: "ZX Spectrum +3",
  "Amstrad CPC 6128", "MSX2".

## Names and trademarks

"Sinclair", "ZX Spectrum" and the Sinclair logo are trademarks of their
owners. ZX Cradle uses machine names to say what it emulates, and evokes the
look of the era with its own stripe, colours and type. It does not use the
Sinclair logo, the Spectrum's rainbow badge artwork, or the ROM fonts in its
own branding.
