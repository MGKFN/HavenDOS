# HavenDOS v0.9.6.3 Beta "Daedalus" — the monitor fills, and the mouse works everywhere

The third update of 0.9.6. Kyan asked for this on 16 November, after
0.9.6.2 got the HDMI monitor working. It bends the 0.9.x "fixes only"
rule once, by his decision: it adds mouse support in every app, and the
monitor picture now scales to fill the screen.

## The monitor

### The picture fills the monitor

The firmware gives HavenDOS a 1024×768 screen. The laptop panel
stretches it with a hardware scaler. In 0.9.6.2 the monitor got the
same 1024×768 unscaled, as a box in the middle of 1920×1080.

Now pipe B uses its own scaler, the same way Linux does
(`skl_pfit_enable`):
- **The mode is the monitor's own best** (1920×1080 on the LG), not
  the smallest mode that fits the box.
- **The picture is fitted to it, in one of three ways:**

  | setting | on the LG | where to set it |
  |---|---|---|
  | **fitted** (the default) | 1440×1080, the 4:3 shape kept, thin black bars left and right | Settings |
  | **stretched** | 1920×1080 edge to edge, like the laptop panel | Settings |
  | **centred** | 0.9.6.2's unscaled box | `gfx fit centre` only |

- **Settings > Display > HDMI monitor > Picture** switches between
  fitted and stretched. If the monitor is lit, the change shows straight
  away. `gfx fit keep|stretch|centre` does the same from the shell.
  The choice is saved.
- **Pipe A's scaler is never written.** Only pipe B's scaler registers
  (0x68900–0x68AFF) are on the allow-list. The fatal-error mask is set
  for the scaler's start, as Linux does on Alder Lake-P, then restored
  to exactly what it was.

### The monitor lights by itself

- **Plug in an HDMI monitor**, at boot or later, and it lights on its
  own, with no `gfx on` and no question.
- **Unplug it** and pipe B switches off.
- **How it watches:** a background job checks the HDMI hot-plug line
  once a second, and only believes a change once two reads in a row
  agree.
- **If the monitor won't light**, it tries three times, a second apart,
  then stops until the cable is unplugged and plugged back in. You get a
  notification and a `dmesg` line saying why.
- **Settings > Display > HDMI monitor > Light it by itself** turns this
  off, as does `gfx auto off`.

## The mouse in every app

Until now only the desktop, Mines, Chat, the file manager, Notes and
the login screen used the mouse. Settings and every other app were
keyboard-only.

The new mouse layer (`shell/uimouse.c`) gives every app the mouse
without rewriting it, by turning what the mouse does into the keys the
app already reads:

| you do | the app gets |
|---|---|
| **click a row** in a list | the arrow presses that select it; a click on the row that is already selected is Enter, so a double-click opens it |
| **click a key hint** on the bottom two rows ("Enter apply", "ESC close", "Q quit", "[Space] pause") | that key |
| **scroll wheel**, or **two fingers on the trackpad** | Up / Down, one per notch |
| **right-click** | ESC: back, or close |

- **Settings** is fully clickable:
  - the sections in the sidebar;
  - every row on every page;
  - the Y / N in a question;
  - the key hints at the bottom.
- **Apps built with the UI kit** get clickable rows.
- **Every other app** gets the key hints, the wheel and right-click,
  because those need nothing from the app. The hints are read off the
  screen at the moment you click.
- **The pointer** shows in every app as an inverted cell, so it is
  visible on any colour, including a selection bar.
- **Where it holds back:**
  - **games** (Snake, Tetris, Pong, 2048, Breakout) keep right-click
    and the hints but not the wheel, so a brushed trackpad doesn't
    steer a snake;
  - **the shell prompt** keeps clicks on hints, but the wheel and
    right-click do nothing there;
  - **apps that already handle the mouse themselves** (the desktop,
    Notes, Files, Mines, Chat, the login screen) are left exactly as
    they were.
- **The scroll wheel is new in the drivers:**
  - a USB mouse's fourth byte, which most mice send even in the boot
    protocol;
  - the trackpad's wheel field, if it has one;
  - otherwise a two-finger drag on the trackpad, with one notch per
    1/24 of the pad's height. While scrolling, the pointer doesn't move.

## Tests

- **New suite `test_uimouse`, 51 checks:**
  - hints, rows, buttons and prefixes;
  - the hook end to end;
  - the pointer never leaving a cell behind;
  - per-task tables.
  - `check_uimouse_suite`: 14/14 injections caught.
- **`test_xemode` 265 → 274:**
  - fit windows and phases (i915's formula);
  - the scaled plan on the model engine, with pipe A untouched;
  - `gfx off` clearing the scaler;
  - the allow-list;
  - the watcher's decisions.
  - `check_xemode_suite` 43/43, with 10 new injections.
- **Wheel input:**
  - `test_i2c_hid` 90: two-finger scroll;
  - `test_usbhid` 39: the wheel byte;
  - `check_usbhid_suite` 8/8.
- **QEMU, `harness/mouse_apps.py` 14/14,** with a USB mouse on xHCI:
  - click a section;
  - click a row twice to toggle it;
  - the wheel, checked on the screenshot's highlight;
  - right-click back;
  - click "ESC close";
  - `gfx fit`, `gfx auto`, and the watcher in `kpoll`.
- **`harness/qemu.py`'s `mouse_to` is fixed.** It still assumed 4 counts
  per row, where the driver has used 8 on QEMU's screen since 0.8.9.6.
  `desktop_layout`'s bell click passes again. Two layout checks there
  fail on 0.9.6.2 too, and are unrelated.
- **7356/7356** checks across 70 suites.

## Not yet proven on the HP

- the scaler on the LG: does the picture fill it, and is it sharp
  enough?
- the monitor lighting by itself, on plug-in and at boot;
- two-finger scrolling on the HP's trackpad (the suite uses a model
  pad);
- the wheel on your USB mouse.

QEMU has none of these.
