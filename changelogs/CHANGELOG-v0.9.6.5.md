# HavenDOS v0.9.6.5 Beta "Daedalus" — apps are windows, the taskbar stays

Kyan, 16 November, after 0.9.6.4 got the monitor working: *"settings no
mouse and I would like that the taskbar is visible at all times, with
the X, full screen button and minimize button at the window's top bar."*

By his decision this ships now, in 0.9.6.5, bending the 0.9.x "fixes
only" rule again. Windows proper are still 29H1's.

## New: every app is a window

Every app opened from the desktop, the Start menu or `open` now has:
- **a title bar** on top, with the app's name and three buttons:
  - **`_` minimize:** the desktop comes forward. The app stays open,
    with its chip on the taskbar.
  - **`[ ]` full screen:** the title bar and taskbar go and the app has
    the whole screen. **F11 or Alt+Enter** brings them back; Alt+Enter
    is there for laptops whose F11 is a media key. An 80x25 app keeps
    its title bar, centred on the whole screen.
  - **`X` close:** does what ESC does, so an app closes, or asks to save
    first, the way it always has. On a sub-screen, one click goes back a
    level.
- **the taskbar** on the bottom row, always:
  - **Start** opens the desktop with its start menu;
  - **a chip for every open app** brings that app forward;
  - **the clock**.

Apps that fit any size (Settings, the Shell) get the whole screen
between the title bar and the taskbar: 128×46 on the HP. 80×25 apps
keep their box, now placed between the two.

On a screen with no room for any of this it is skipped, so apps run as
before. That means 80×25, which is QEMU's default, or anything under 27
rows. It is also skipped without tasks (safe mode, `notasks`).

How it works:
- The pointer now lives on the whole screen, not just inside the app's
  box, so it can reach the title bar and the taskbar.
- An app still gets coordinates inside its own box, held at the edge,
  so no app sees a column it doesn't have.
- A click outside the box belongs to the window, not the app.

## Fixed: the mouse in Settings

Settings opened from the desktop's right-click menu, or from the
Accounts shortcut, ran inside the desktop's own task. The desktop reads
the mouse itself, so the mouse layer is switched off there. Settings now
turns the mouse on for itself however it is opened, and so does every
app the desktop runs.

## Tests

- **New `test_window`, 25 checks:** the layout on 128×48 and on grids
  with no room, and what every click hits. `check_window_suite` 9/9.
- **`test_kbdinit` 49:** the pointer outside an app's box, held at its
  edge for the app, with the click kept for the window.
- **`test_uimouse` 53.**
- **QEMU, new `harness/window_apps.py`, 11/11** three runs in a row,
  with a USB mouse at 128×50:
  - the title bar and taskbar;
  - the wheel in Settings;
  - full screen and Alt+Enter;
  - minimize, with the app still open;
  - opening it again brings back the same window;
  - X closes it.
- `mouse_apps` 14/14, `appguard_boot` 19/19, `resolution_tasks` 8/8,
  `hid_boot` 3/3, `gfx_boot` 15/15.
- `desktop_layout`: the boxed app's title bar still passes. Its two
  drawer checks fail on 0.9.6.2 too.
- **7401/7401** across 71 suites.

## On the HP

1. Open Settings from the Start menu, and also from the desktop's
   right-click menu. Click rows, scroll with two fingers, and try X.
2. Open a few apps. Try minimize, then their chips on the taskbar, then
   Start.
3. Try full screen, then **Alt+Enter** to get back. Also try F11, which
   may be a media key on the HP.
