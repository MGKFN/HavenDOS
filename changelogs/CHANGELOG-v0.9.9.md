# HavenDOS v0.9.9 Beta "Theia" — windows of any size

Kyan, 25 November: "if we transitioned to the pixels we have kinda our same
system like windows or linux?? bc i want smth like that". This is the step
that makes it so. Every window has its own pixels and can be any size: you
drag it bigger or smaller from its edges and corners, snap it to half the
screen, or maximise it. The Shell is now the **Terminal**. The boot screen
is drawn in pixels.

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU at 1024x768 (the HP's firmware mode, 128x48) and
> 1280x800.

## Resize any window

- **Edges and corners.** Put the pointer on a window's edge, or just
  outside it, and the pointer turns into a double arrow. Drag to make the
  window bigger or smaller. The corners resize both ways at once. The
  window follows the pointer while you drag.
- **Snap to half the screen.** Drag a window by its title bar to the
  left or right edge of the screen and let go: it takes that half. Drag it
  to the top edge and it is maximised.
- **Maximise works for every window**, the Terminal included. Click the
  middle button, or double-click the title bar. Do it again to put the
  window back where it was. Dragging a maximised or snapped window by its
  title bar puts it back to its own size, under the pointer.
- **Full screen** (F11 or Alt+Enter) gives a window the whole screen,
  without the title bar or the taskbar. It no longer changes the screen's
  mode, so nothing else moves while you use it.
- **New windows** open at their own size in the middle of the screen,
  each one a little further down and to the right of the last.
- Maximised and snapped windows stay inside the area the pointer can
  reach. On a screen with a margin beside the grid, the buttons of a
  maximised window used to be out of reach.

## Apps fill their window

What an app shows follows its window in one of two ways:

- **The Terminal, Settings and System lay themselves out again.** A
  bigger Terminal gets more lines and columns, like any terminal. When it
  gets shorter, the lines scroll up so the line you are typing on stays
  in view. Settings and System redraw for their new size.
- **Apps still laid out for 80x25 zoom.** They keep their 80x25 and
  their text gets bigger or smaller with the window, so they fill it at
  any size. They are rebuilt to lay themselves out in 0.9.11–0.9.14. The
  text stays smooth at any size, drawn from the same IBM Plex Mono master
  as the console. Minefield zooms too: its board is sized once, when the
  game starts.

The 80x25 box in the middle of a bigger window is gone everywhere.

## Every window has its own pixels

Each window keeps a picture of its own content, its **surface**. When a
cell of an app changes, it is drawn into that picture, and the screen is
copied from it. Moving a window, uncovering it or resizing it is now a
copy instead of drawing every letter again. This is the same model
Windows and Linux use, and the pixel building blocks of 0.9.10 will draw
into these surfaces. A machine short of memory for a surface still works:
that window is drawn the 0.9.8 way, and `tests/test_wm.c` checks both ways
give the same picture.

## The Shell is the Terminal

In a window on the desktop it is called **Terminal**: in the Start menu,
on its title bar, its icon and the taskbar. Its own "HavenDOS Shell" bar
inside the window is gone, because the title bar already says it. `open
shell` and `open terminal` both work, and pins and saved desktops keep
working. **Shell Mode**, the boot entry, keeps its name. In Settings the
section is called Terminal.

## No more second title bar

Apps that drew their own name in a bar on their first row showed it twice:
once on the window's title bar and once below it. That row is no longer
shown. The app still draws it and nothing in it moves.

## The boot screen in pixels

On every machine with a framebuffer (every one HavenDOS has run on since
0.8), the loading screen is drawn in pixels:

- the Studio graphite fading down the screen;
- **HavenDOS** in IBM Plex Sans with the oxide rule under it, and the
  version and codename below;
- a round ring of dots with a comet going round it;
- the last line of the boot log in full underneath, as before.

If something clears the screen during start-up, the splash notices and
draws itself again. A key still shows the boot log. A machine with VGA
text only keeps the character version.

## A machine with no pixel mode starts in the Terminal

Kyan: "i dont want legacy user to lack features". There is one HavenDOS
and no frozen "Classic" version. A machine whose firmware gives no
framebuffer at all, which means roughly older than 2000, cannot show the
desktop. It now starts in the Terminal, which has every command, and
says why on the first line. `exit` still opens the old character desktop,
the same way it does from Shell Mode.

## Also

- **Window shadows left a trail.** When a window moved or closed, the
  last few rows of its old shadow could stay on the screen. The shadow
  reaches its depth plus the offset it is cast at, and only the depth was
  repainted. Everything that repaints after a move, and the check for
  whether anything lies above a pixel, now use the full reach.
  `tests/test_wm.c` checks for it.
- The pointer has five shapes now: the arrow and four double arrows. Each
  shape has its own point that counts as the pointer's position.
- **The caret stays visible.** It used to disappear when a window was
  repainted, for example when the pointer passed over it, until the next
  key. It is now part of how its cell is drawn, so it stays.
- IBM Plex Sans SemiBold at 32 and 48 px, ASCII only, for the splash and,
  from 0.9.10, for headings.

## Tests

- `make test`: 78 suites, 7873 checks, all passing.
- `tests/test_wm.c`: 100 checks (was 72). New checks cover:
  - a zoomed window and its mouse mapping;
  - the resize bands;
  - surface and no surface giving the same picture;
  - the caret surviving a repaint;
  - the hidden header row;
  - full screen as a window;
  - no shadow left behind after a move and resize;
  - the Terminal's viewport resize keeping the cursor line.
- New QEMU harness `harness/windows_099.py`, 14/14 at 1280x800 and at
  1024x768 (the HP's mode). It covers the pixel splash, the floating
  Terminal, resizing it by whole cells from a corner, maximise and
  restore, snapping to the left half, the Calculator zooming, and F11 and
  back.
- `harness/px_mouse.py` places the pointer to the pixel, for those drags.
- `window_apps`, `mouse_apps`, `redraw_hunt`, `desktop_layout`,
  `resolution_tasks`, `hid_boot` and `appguard_boot` pass. `redraw_hunt`'s
  `cls` check now expects the prompt on the Terminal window's first line,
  not the screen's.
- The no-framebuffer start was checked by booting the kernel straight
  from QEMU (`-kernel`, which gives no framebuffer).
- `emu/` is still RetroPC 0.1.1 (d2a89e6).

## Not in this release

- **Apps laid out for any size** (instead of zooming): Settings and Files
  in 0.9.11, Text Editor, Notes, Calculator and the rest in 0.9.12–0.9.14,
  built from the 0.9.10 building blocks (buttons, lists, text fields,
  scrollbars). 0.9.10 also redraws only what changed, which is the next
  real speed gain.
- **The installer, setup and sign-in in the new look:** 0.9.13. The boot
  splash came first because it is the first thing anyone sees.
- **64-bit:** before 1.0 now (Kyan, 25 Nov). HavenDOS 1.0 is planned for
  **13 March 2027**. The 64-bit kernel comes after the languages (0.9.17)
  and before the release candidates. One ISO carries both kernels, and
  the boot loader picks the right one for the processor. The desktop, the
  apps and every feature are the same on both.
