# HavenDOS v0.9.8 Beta "Hestia" — the desktop

Kyan, 24 November: friends and users say HavenDOS "feels old". The name
stays — it is known, and it is what search finds — and the look changes.
He chose to do it step by step instead of in one rewrite (the road to 1.0
is in `claude/havendos-road-to-1.0-pixel-desktop.md`). This is the first
step: **the desktop is drawn in pixels**. Windows can be dragged and
stacked, the taskbar and the Start menu are new, and every app has an
icon. The apps inside the windows are unchanged; that comes from 0.9.9 on.

HavenDOS is **Beta** again from this release (channel 20) until
HavenDOS 1.0 Stable, which moves from January to spring.

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU, at 1280x800 and 1920x1080.

> **Updating from 0.9.7.x:** those systems follow the Stable channel and
> will not take a Beta update on their own. Type `update channel beta`
> once, then `update`. (A fresh install from the ISO needs nothing.)

## Windows you can move

- An app opened from the desktop is a **window** with a title bar in
  pixels: its icon, its name in **IBM Plex Sans**, and three drawn
  buttons — minimise, full screen and close — that light up under the
  pointer (close turns red).
- **Drag a window by its title bar** to move it, pixel by pixel. Its title
  bar always stays on the screen and above the taskbar.
- **Windows stack.** Several can be on the screen at once; click one to
  bring it to the front. The one in front has a coloured title bar, the
  others grey. A window behind another keeps drawing where it shows.
- Each window has a 1-pixel edge, **rounded corners** and a **soft
  shadow** below and to the right.
- An app that uses the whole screen opens **edge to edge**, between the
  title bar and the taskbar; an 80x25 app opens as a window of its own
  size in the middle.
- **Full screen** (the middle button, F11 or Alt+Enter) gives a window the
  whole screen; again to come back.
- On a grid with no room for a title bar and a taskbar — 80x25, the
  default in a virtual machine — an app has the whole screen, as before.

## The taskbar

One taskbar, in pixels, on top of every window:

- **Start** (the HavenDOS grid), then your **pinned apps** as icons, then
  a button for **every open window** with its icon and name. The window
  in front is lighter with an oxide line under it; a minimised one is
  dimmed. Click a window's button to bring it up, again to minimise it.
- On the right: the **bell** with the number of unread notifications
  (click it for the Notifications window) and the **clock**.
- What the pointer is over lights up.
- Long names are cut with "..." instead of running into the next button.

## The Start menu

A panel above the Start button, instead of the old cascade that grew
taller than the screen with fifty apps in it:

- **Type to search.** Any letter goes into the search field and the list
  shows every app whose name contains it, upper or lower case, A to Z.
  Esc clears the search; Esc again closes the menu.
- Categories on the left: **All apps** (A to Z), **Favourites**,
  **Recent** (the apps you opened, newest first), and the six real ones —
  Apps, System, Hardware, Games, Media, Settings.
- The list **scrolls** — wheel, keys or its scrollbar — and shows how many
  apps it holds; an empty category or search says so.
- Every app has its **icon**; a favourite has a star.
- At the bottom: who is signed in, **Lock** and **Power**.
- Keys: Up/Down choose, Left/Right between categories and the list,
  Enter opens, PgUp/PgDn/Home/End. **Ctrl+D** puts the chosen app on the
  desktop and **Ctrl+F** in Favourites — D and F alone did that in the old
  menu, but a letter now searches.
- The mouse: hovering chooses, a click opens, a click outside closes.
- The Windows key opens and closes it from anywhere, also from inside an
  app; ESC on the desktop opens it too.

## Icons

A family of **65 icons**, one for every app and for the system's own
functions: a rounded tile in the app's category colour (Apps blue, System
teal, Hardware green, Games purple, Media amber, Settings slate, and
oxide for the few that matter most — Paint, Install, Power) with a white
symbol. The symbols are **Material Symbols Rounded** (Google, Apache
License 2.0). Each icon is stored at 16, 24, 32 and 48 pixels as two
coverages a pixel — the tile and the symbol — so it is mixed over
whatever is behind it, on any theme, without reading the screen back
(`tools/make_icons.py` → `include/icons.h`). They are on the desktop, in
the Start menu, on the taskbar and in every title bar.

The **desktop's icons** are drawn in pixels now: the icon and the name,
and a rounded highlight on the chosen one. Where a click lands is the
same as before.

## The desktop's own text

**IBM Plex Sans**, the THS sans-serif, Regular and SemiBold, is the
desktop's typeface: title bars, the taskbar, the Start menu, icon names.
FreeType renders each size ahead of time (`tools/make_font_ui.py` →
`include/font_ui.h`), so the kernel only copies coverage. Apps and the
Shell keep IBM Plex Mono.

## How it works

`drivers/wm_core.inc`, a window manager in the console driver. The screen
is a stack of layers — the desktop, the windows bottom to top, the
taskbar, the Start menu, the pointer — and **every pixel is painted once,
by the layer that owns it**, darkened by the shadows of windows above it.
So moving a window, a window behind changing, a menu opening never paint
something that is then painted over: nothing flickers, and no copy of the
whole screen is kept (one would be 8 MB at 1080p). Each console keeps its
own record of what is on the screen, so only cells that changed are drawn,
wherever their window is.

The window manager is on only while the desktop runs. The boot console,
Shell Mode, safe mode and real VGA text work exactly as in 0.9.7.10.

The sign-in screen (Lock), the screensaver and Alt+Tab take the whole
screen over every window while they are up.

## Also

- The Shell's `exit` and the window's close button behave as before; the
  title bar no longer says "ESC closes" — the close button is there.
- The desktop's rows end above the taskbar, so the notification drawer's
  last line is no longer hidden under it on an 8x16 grid.

## Known limits (for 0.9.9 and later)

- Windows cannot be resized yet; an app has the size it opens with (0.9.9).
- The apps inside the windows still draw their own header, so a window
  may show its name twice (0.9.11 onwards rebuilds them).
- The pointer reaches the grid, not the narrow margins beside it on a
  screen the grid does not fill; the taskbar and the Start menu are laid
  out inside the grid's width for that reason.
- Alt+Tab's panel is drawn over the app's own console, not over the
  windows as they are on the screen.
- The cursor is the arrow everywhere; text, resize and busy shapes come
  with the building blocks (0.9.10).
- `migrate_fat32` still cannot make its FAT16 system with the 0.9.7.7
  release (see 0.9.7.10).

## Tests

- `make test`: **78 suites, 7920 checks, all passing**. New:
  - **`test_wm`** (62): drivers/wm_core.inc drawing into a simulated
    1920x1080 panel, read back by pixel — a window covers the desktop and
    the top window the one below; the corner pixel is the desktop (round);
    a shadow below, none above; dragging leaves nothing behind; a window
    behind draws only where it shows; minimise, restore, close, full
    screen and solo show exactly what was under; hit-testing and click
    routing agree with what is drawn; the taskbar and the popup are on top;
    nothing is written outside the panel.
  - `tests/wm_stubs.inc`: the suites that compile the mouse, task, chrome
    and ui code without the console get the window manager switched off,
    so they test what they always tested.
- QEMU harnesses, one at a time: `mouse_apps` 14/14, `hid_boot` 3/3,
  `window_apps` 11/11, `redraw_hunt` 12/12, `desktop_layout` all,
  `appguard_boot` 19/19, `resolution_tasks` 8/8, `layout_audit` (all 45
  audited apps opened from the new Start menu; none drew outside its
  console), `usb_storage` 52/52, `lockdown_boot` 12/12, `retro386` 7/7,
  `esp_update` all.
  - `redraw_hunt` opens the Start menu and clicks its Lock button by its
    pixels, and opens the Shell from the desktop's first tile;
    `desktop_layout` opens an app by typing its name into the Start menu
    and clicks the pixel taskbar's bell; `layout_audit` opens every app by
    search. `window_apps.click_px()` clicks a screen pixel.
- Booted at **128 MB**: the desktop and apps run.
