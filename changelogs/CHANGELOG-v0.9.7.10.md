# HavenDOS v0.9.7.10 Stable "Mnemosyne" — the finished look

Kyan, 24 November: people complained that HavenDOS "is still text mode
and outdated in 2026". It has drawn its own pixels since 0.8 — what made
it look old was the font, an 8x16 bitmap blown up 2x or 3x; a pointer that
was a whole character cell; and windows with hard edges. He chose to fix
the look as 0.9.7.10, before 0.9.8, so the whole thing can be judged at
once.

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU, at 1280x800 and at the HP's 1920x1080.

The grid stays: HavenDOS is still laid out in character cells, which is
what keeps it a DOS. Everything here is drawn in pixels ON that grid.

## A smooth font, in the THS typeface

Letters are now drawn in **IBM Plex Mono** — the Tech Haven Studios
technical face, the one on the website and in the manuals — with smooth
(anti-aliased) edges, at the exact size of the screen's cells: 16x32 at
80x25 on 1280x800 or 1920x1080, 24x48 on a 1920x1200 panel, and any size
an app full screen needs.

- One 32x64 master per glyph is built into the kernel
  (`tools/make_font_aa.py` → `include/font_aa.h`); the console shrinks it
  to the cell size once, by averaging, and blends text and background per
  pixel — on Studio's fade as well.
- ASCII and the accented letters are Plex. Everything else keeps the
  8x16 shape, enlarged without smoothing, so anything that must meet its
  neighbour edge to edge still does.
- Cells smaller than 12x24 (very large grids, like `resolution max`) and
  real VGA text keep the classic font: smoothing at 8x16 reads worse, not
  better.
- **`font classic`** brings back the old font, at once and everywhere;
  **`font smooth`** returns. The choice is saved, and `migrate` carries
  it across.

## The pointer is an arrow

The mouse pointer was a character cell with its colours swapped. It is
now an **arrow**, drawn in pixels — paper-white with a graphite edge, and
oxide red while a button is held — that moves smoothly inside a cell
instead of jumping from one to the next. Its tip is always over the cell
a click goes to, so every app's mouse handling is unchanged.

As on any desktop, the arrow **gets out of the way while you type**: a key
press hides it, and it comes back the moment the mouse moves or a button
is pressed. Before the mouse has been touched at all it is not shown —
a pointer parked over the middle of the shell, covering the letters being
typed, was the first thing a QEMU run caught.

The arrow is painted over whatever cells it covers each time one of them
is drawn, so nothing can rub it out, and the cells it leaves are repainted
from what the screen shows. All three pointers — the desktop's, the
apps', and the one outside an app's window — draw it. Real VGA text keeps
the cell pointer.

## Windows have edges

An app that runs in a window (an 80x25 app on a bigger screen) now has:

- a **soft shadow** down its right side and along its bottom, half a cell
  deep;
- a **one-pixel outline** in the theme's dark grey, so a window whose
  inside is the desktop's colour still has an edge;
- **rounded corners**, the pixels outside the curve showing what is
  behind.

Like the pointer, they are drawn on top of the cells, so the app cannot
draw over them, and they go when the window closes.

## Also

- Found while testing: the hook that lets apps take mouse clicks runs on
  every keyboard poll, also on the desktop where it tracks nothing, and it
  hid the arrow as fast as the desktop drew it. Each pointer now hides
  only an arrow it put up itself.
- `resolution`'s help said glyphs were "8x8 scaled"; it describes the
  cells and the smooth font now.

## Tests

- `make test`: 77 suites, all passing. `test_console` 359 (was 337):
  - a smooth 'H' has solid ink, soft edges, and only mixes of its two
    colours;
  - `font classic` gives hard edges again at once, `font smooth` back;
  - 8x16 cells keep the bitmap;
  - the arrow's tip is at the cell plus its offset, paper-white inside,
    stays on top of a cell painted under it, and leaves the cell exactly
    as it was when hidden;
  - a window's shadow darkens right and below but not above, the outline
    runs along its edge, the corner shows what is behind, and all of it
    goes when cleared or when the whole screen is.
  - The 12x24 full-screen check now tests the classic path on purpose.
- `harness/fbtext.py`, the screen reader every QEMU harness uses, reads
  the smooth font: it estimates each pixel's ink and takes the nearest of
  the same shrunken templates the kernel makes. 0.16 s for a screen,
  faster than before. A screen in the classic font (`font classic`, or
  an older release a migrate test installs) is read the old way.
- `harness/arrowfind.py`: the harnesses that click find the arrow by its
  shape, and its tip's cell is where they click — a menu that opens as
  the pointer crosses it no longer fools them. `mouse_apps`, `window_apps`,
  `redraw_hunt` and `hid_boot` pass; text under the arrow reads as '?',
  and `mouse_apps` allows for that. `esp_update` passes.
- `migrate_fat32` could not run: its first step installs the 0.9.7.7
  release to get a FAT16 system, and that release's installer stops
  with "writing into the new partition failed" in this run, twice. This
  release changes nothing in `migrate` but one name it carries (`.font`),
  which `test_migrate`'s name check covers. Looked into for 0.9.8.
- Boots and runs the desktop in 128 MB (about 100 MB free at the shell),
  the minimum on the system requirements sheet.
