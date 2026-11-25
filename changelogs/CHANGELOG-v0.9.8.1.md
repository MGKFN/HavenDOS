# HavenDOS v0.9.8.1 Beta "Hestia" — a pointer that glides

Kyan, 25 November, from the HP with two photos: "the cursor feels still
tied to grid instead of like Windows ... blocky movement", "cursor
multiplying", and in the photos the desktop's Shell icon painted over
Settings. Four fixes.

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU.

## The pointer moves in pixels

Since 0.8.9.6 the mouse moved a fixed 320 counts across the screen: on a
1920-pixel panel every count of the mouse was a **6-pixel jump**, however
slowly the hand moved. That was the blockiness. Now:

- A slow movement moves **one pixel a count** (two on a panel 1920 or more
  wide): precise enough to land on a 1-pixel edge.
- A fast one is **accelerated**: the bigger the mouse's report, the further
  each count goes, up to screen-width / 256 pixels (7.5 at 1920). Small
  movements are exact, big ones still cross the screen in one sweep — the
  curve every desktop has.
- The position is kept in **sixteenths of a pixel**, so slow movement adds
  up instead of being rounded away, and the arrow's tip is drawn exactly
  where the pointer is.
- "Fast" is measured two ways — the counts of the last 50 ms, and the size
  of each report — and it has to be fast by both. Reports the system gets
  to late arrive together and look fast by the first measure, and a slow
  movement then jumped; one report bigger than usual (two merged) looks
  fast by the second.

If it now feels too slow or too fast on the HP's trackpad, say which; the
curve is two numbers in `drivers/mouse.c`.

## No more copies of the pointer

Outside the desktop (the Shell, setup, the installer) the old arrow was put
back by repainting the cells under it — and three things were skipped: a
cell not painted yet (left for a flush a quiet app never does), the margin
beside a grid that does not fill the screen (no cell there at all), and a
cache dropped by a resolution change. Each left an arrow on the glass;
moved about, they multiplied. Now every pixel under the old arrow is put
back: from the screen's record, from the console, or the margin's colour.

## The Start menu keeps up with the pointer

Every pixel the pointer moved over the menu lit a row or a category, and
each time the whole menu was painted again, every pixel asking which
window owned it. In QEMU that took up to seconds a frame, so the pointer
seemed to stick and then jump. Now only what changes is painted — the
row that lights and the one that goes out — and the menu and the taskbar,
which are on top, paint without asking. Title bars of windows with
nothing above them paint the same quick way.

## An app the desktop runs itself has the screen

Settings — and any app that must run alone, or when no task is free — runs
inside the desktop's own console, not in a window of its own. With the new
window manager it was drawn as part of the desktop: the desktop's icons
were painted over it and it had no title bar (Kyan's photos). Now such an
app has the whole screen while it runs, with its title bar and taskbar
drawn as before 0.9.8.

## Tests

- `make test`: 78 suites, 7922 checks, all passing. `test_kbdinit`'s
  mouse checks are rewritten for pixels: a slow count is a pixel (two at
  1920 wide), the sub-pixel position, both axes alike, a fast movement
  going over twice as far, left and right mirror images, no debt at an
  edge.
- QEMU harnesses, one at a time: `mouse_apps` 14/14, `redraw_hunt` 12/12,
  `window_apps` 11/11, `desktop_layout` all, `hid_boot` 3/3,
  `appguard_boot` 19/19. The harnesses now move the pointer in pixels
  (four counts at a time, the exact range, 25 ms apart).
- Not done in this release: the boot screens and the installer are still
  drawn in characters; they are rebuilt in pixels with the other system
  screens (0.9.13 on the road to 1.0).
