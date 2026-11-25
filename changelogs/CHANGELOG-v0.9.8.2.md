# HavenDOS v0.9.8.2 Beta "Hestia" — the window keeps its frame

Kyan, 25 November, from the HP with four photos: "Why are those apps???"
(the bell and Start → power each opened an app window), "look at cursor",
"maximize should still let the buttons and taskbar and all visible", icons
"should have name under it like windows Mac and Linux", and the icons a bit
bigger, "especially the start menu button".

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU at 1024x768 (the HP's firmware mode, 128x48),
> 1280x800 and 1920x1080.

## The Shell scrolls inside its window

The worst of the photos. A maximised Shell is the 44 rows between the title
bar and the taskbar of a 48-row grid, but the console wrapped and scrolled
on the **whole grid**: it wrote four lines below its own window (those
writes went nowhere) before it scrolled once. On screen the Shell showed
every third line or so ("echo L17 … echo L20 / L22 … echo L25 / L27"),
the prompt you typed at could be invisible, and what was left near the
bottom was a mess of old text. `vga_putchar` now wraps and scrolls on the
window's own grid. Reproduced in QEMU at 1024x768 and fixed there;
`tests/test_wm.c` prints 300 lines into a maximised window and checks the
last one is where it should be, and that the panel is exactly what a
repaint from scratch draws.

The pointer "trails" in that band were most likely this: stale rows that
never repainted. If you still see copies of the arrow, a photo of where
helps.

## Maximise keeps the title bar and the taskbar

The middle title-bar button made the window **full screen**: no title bar,
no taskbar, no buttons. Now it **maximises**: the window goes edge to edge
between the title bar and the taskbar, both kept, the three buttons where
they always are. An app still laid out for 80x25 sits in the middle until
0.9.9 makes windows resizable; the button again puts it back where it was.
Full screen is still there, on **F11** or **Alt+Enter**. The Shell is
already as big as a window gets, so on it the button says so.

## Power is a flyout of the Start menu

Start → the power button opened the old "Power and Reboot" app in a window.
Now a small menu opens above the button: **Lock, Switch user, Restart, Shut
down**, by mouse or Up/Down/Enter, Esc closes it. Restart and Shut down say
so on a full screen first. If the machine does not restart or switch off,
the desktop comes back with a note instead of a hung message.

## Notifications are a flyout of the bell

The bell, and N on the desktop, opened the Notifications app, whose 80x25
drawer then stood at the right of its own window. Now a panel opens above
the bell, newest first, with **Clear all**; Esc, the bell or a click
anywhere else closes it. Opening it clears the count on the bell.

## Icons: the name under them, and bigger

- **Desktop:** each icon is a tile about 104x92 pixels, the icon on top and
  its name centred under it, on two lines when it is long, the way every
  other desktop does it. The tile is the same size whatever the cell size.
- **Taskbar:** Start, the pins, the windows and the bell are 24 px on the
  HP's 32 px bar (were 16) and 32 px on 1080p.
- **Start menu:** rows are taller; the app icons are 32 px (were 24) and
  the categories 24 (were 16).

## Also

- A floating window's edge could be painted over the taskbar when it sat
  near the bottom: the fast path checked only its title bar for anything
  above it. It checks the whole frame now.

## Not in this release

- **"It still feels sluggish."** The framebuffer is already write-combining
  (since 0.8.9.6), so that quick win is taken. RetroPC is slow because it
  emulates the CPU in software; a real speed-up there is RetroPC work.
  Using the Intel Xe's 2D engine from HavenDOS is a separate, large
  project. The next big gain is fewer pixels redrawn per change, which
  comes with the pixel building blocks in 0.9.10.
- **Modern Text Editor and File Manager, files on the desktop:** 0.9.11 and
  0.9.12 on the road to 1.0.
- **Installer and boot screens in the new look:** 0.9.13 — or sooner if
  you want them pulled forward.
