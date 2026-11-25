# HavenDOS v0.9.10 Beta "Athena" — building blocks, screenshots and Python

Kyan, 25 November: "trackpad still no working maybe add image and
screenshot support btw also it still an bit slugish", then "pining stuff
to task bar can't be done ... click menu is old also pinnin to desktop
broken", then "Also intergrate python". This release adds HavenDOS's own
pixel building blocks and the first app drawn with them (Images), makes
the screen faster to draw, adds screenshots and picture wallpapers, fixes
pinning, draws the right-click menu in pixels, and brings in Python.

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU at 1280x800 and 1024x768 (the HP's mode).

## Screenshots

- **PrtSc** saves the whole screen as a PNG, wherever you are. A
  notification says where it went.
- **Where it goes:**
  - With a FAT32 USB stick in, to `Pictures` on the stick
    (`USB:\Pictures\Shot-20261125-213105.png`, the date and time).
  - Otherwise, to `Pictures` in your home folder on the system disk. On a
    live boot that is RAM, so it is gone after a restart.
- **`screenshot [file]`** in the Terminal does the same. Give a file name
  to choose where.
- The pointer is not in the picture.

### Sending a screenshot to Claude

1. Put a FAT32 USB stick in the HP **before** you press PrtSc.
2. Press PrtSc. The notification says `Saved to USB:\Pictures\...`.
3. Move the stick to your Windows PC. The file is in `Pictures` on the
   stick.
4. Attach the PNG in the chat, like any photo.

Without a stick, `ls Pictures` in the Terminal shows what you saved, and
the Images app opens it.

## Images

The first app drawn only in pixels, with the new building blocks.

- It lists the pictures in your `Pictures`, in `Pictures` on a USB stick,
  and in the stick's top folder.
- It opens PNG (every colour type, 1–8 bits per channel) and BMP (24 and
  32-bit). Interlaced and 16-bit PNGs are refused with a message.
- **Fit**, **1:1**, **+** and **−** zoom the picture. Drag it, or use the
  arrow keys, to move around a big one.
- **Set as wallpaper** puts the picture behind the desktop. It is scaled
  to cover the screen. **Remove wallpaper** goes back to the theme's
  colours. The choice is kept after a restart.

## Faster drawing

- Copying pixels is faster. Memory is now copied 4 bytes at a time, where
  it used to be copied 1 byte at a time.
- **The Terminal scrolls by moving pixels.** A full Terminal used to draw
  every letter again for each new line. Now it moves the picture up one
  line and draws only the new line.
- **`perf`** in the Terminal says how much was drawn and how long it took:
  - `perf 10` watches for ten seconds while you use the desktop.
  - `perf bench` times a fixed set of drawing (the whole screen, a window
    moved, 200 lines scrolled), so two machines or two versions can be
    compared.
- **Settings → Display → Best performance**, for old PCs: no shadows,
  square corners, no picture wallpaper. Everything else is the same.
- The inside of a window is one flat colour now, not a gradient. A flat
  colour is copied; a gradient had to be worked out pixel by pixel.

## Pinning and the right-click menu

- **The right-click menu is drawn in pixels**, in Plex Sans. It opens at
  the pointer and has a panel, a shadow and a highlight that follows the
  mouse. The arrow keys, Enter and Esc work too. On the desktop it opens
  with a right-click or the M key.
- **Pin to the taskbar from the Start menu.** Right-click an app in the
  Start menu for **Open**, **Pin to the desktop**, **Pin to the taskbar**
  and **Add to Favourites**. Ctrl+D, Ctrl+T and Ctrl+F do the same from
  the keyboard. Before, there was no way to pin to the taskbar at all.
- **Pinning to the desktop works again.** The icon used to appear only
  after something else made the desktop redraw. Now it appears at once,
  and unpinning removes it at once.

## Python

- **`python`** in the Terminal opens the Python prompt (`>>>`). Type
  Python and it runs line by line. **`python file.py`** runs a file.
  `py` works as a short name.
- **Python** is also an app in the Start menu, in a window of its own.
- It is **MicroPython 1.30** (MIT licence), built into HavenDOS. These
  parts of Python work:
  - the language, with whole numbers of any size, floats and exceptions
    with tracebacks;
  - `import` of your own `.py` files from the current folder;
  - `input()`;
  - the `math`, `random`, `time` and `sys` modules.
- **Ctrl+C** stops a running program. **Ctrl+D** on an empty line goes
  back to the Terminal. A Python window's **X** closes it.
- A program that recurses forever stops with `RuntimeError` and does not
  crash HavenDOS.
- **Not yet:**
  - `open()`, so Python cannot read or write files itself;
  - threads and sockets.
- Only one Python runs at a time. A second one says Python is already
  open in another window.
- It adds about 200 KB to the kernel.

## Building blocks (for app makers)

These are in `shell/pxui.c` and `include/pxui.h`.

- **Pieces:**
  - buttons and icon buttons;
  - toggles and checkboxes;
  - lists and scrollbars;
  - text fields;
  - progress bars and tooltips;
  - menus and dialogs.
- They are drawn in pixels into the app's own window, which is the
  surface from 0.9.9.
- Their colours come from the theme. With High Contrast they get 2 px
  outlines.
- An app can now draw its whole window itself: `vga_wm_canvas()` gives
  the window's pixels, and `vga_wm_present()` puts the changed part on the
  screen.
- `kernel/png.c` reads PNG and BMP files and writes PNG files.

## Also

- A window's bottom corners now look the same however they are drawn.
  Before, the content's corner pixel was cut by the rounding when
  something lay over the window, and square when nothing did. The
  content is now square to its last pixel in both cases, as designed.
- Python's MicroPython source is in `third_party/micropython`. To update
  it, run `tools/vendor_micropython.py`.

## Tests

- `make test`: 79 suites, all passing.
- `tests/test_wm.c`: 113 checks (was 100). New checks cover:
  - a zoomed window scrolling by moving its pixels, with nothing stale
    after 60 lines;
  - an app drawing its own window (canvas and present), and that picture
    moving with the window;
  - Best performance taking the shadow away and giving it back.
- `tests/test_png.c`: 20 checks. They decode ten PNG and BMP files made
  by Python's PIL and encode a PNG that PIL reads back exactly.
- New QEMU harness `harness/pixels_0910.py`, 7/7. It covers:
  - `perf bench`;
  - PrtSc saving a PNG;
  - `screenshot` writing a real PNG (its first bytes are `89 50 4E 47`);
  - Images opening and showing a picture;
  - the right-click menu in pixels.
- New QEMU harness `harness/python_0910.py`, 15/15. It covers:
  - running a file that imports another;
  - the prompt: numbers, `math`, `time.sleep`, `random` and `input()`;
  - Ctrl+C;
  - runaway recursion;
  - Ctrl+D;
  - the Python app's window, its X, and opening it again.

## Not in this release

- **The trackpad on the HP.** This needs a photo of `i2chid` on the HP
  first: it shows what the touchpad reports.
- **Apps laid out for any size:** Settings and Files in 0.9.11, then the
  rest in 0.9.12–0.9.14, built from these building blocks.
- **Python reading and writing files** (`open()`): later, with a module
  for HavenDOS's own things (the screen, notifications).
- **Room in the kernel.** The kernel is 7.67 MB and an update can carry
  8 MB. The installer keeps a second copy of the kernel inside it. That
  copy is next in line to be compressed, which frees a few MB.
