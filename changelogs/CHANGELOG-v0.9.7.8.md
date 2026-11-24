# HavenDOS v0.9.7.8 Stable "Mnemosyne" — the THS look, FAT32, Notepad

Kyan, 24 November: a fresh install on a 2000 MiB partition should be
FAT32; the boot menu, installed and on the ISO, was plain; HavenDOS
should follow the gradient and colours of the THS documents and website,
by default, with a little back story; and Notepad should wrap at the edge
of the screen, hold far more than 200 lines, and make Ctrl+A and Ctrl+X
work. Plus an install guide that someone who has never done this can
follow.

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU.

## Every new install is FAT32 from 64 MB

The installer made FAT16 up to 2047 MB and FAT32 above it. It now makes
**FAT32 for any partition of 64 MB or more**; FAT16 is only for the
16–63 MB a tiny disk leaves. 64 MB rather than FAT32's bare minimum of
about 33 MB, so a volume is never a few clusters from the 65,525 FAT32
needs: at 64 MB it has about 130,000.

Nothing decides the filesystem from a partition's size after the
install — every part of the system mounts it and asks — so a small
FAT32 volume boots, updates and uninstalls like a big one. Installs that
are already FAT16 keep working; `migrate` moves one to FAT32, or a fresh
install does.

## The boot menu has the THS look

The GRUB menu was white text on black. It now wears the studio's
"Reference Manual" console style, the one the website and the printed
manuals use: a graphite background fading down the screen, the
**HavenDOS** masthead in paper-white under an oxide-red **TECH HAVEN
STUDIOS** label and spot rule, the entries in DejaVu Sans Mono, the
chosen one on an oxide-red bar, and a hairline foot with the countdown
and the keys.

- **On the ISO**, from `isodir/boot/grub/themes/havendos/`.
- **On an installed system**, from the EFI partition: the installer writes
  the theme beside `grub.cfg` in `/EFI/HAVENDOS/`, and the UEFI loader it
  installs now carries GRUB's `gfxmenu` and `png` modules. `uninstall`
  removes the theme files with the rest.
- If a font will not load, the menu falls back to the plain one, still
  in graphite and red, and still works.

`tools/make_grub_theme.py` makes both copies from one place (it replaces
`tools/make_grub_bg.py`, which drew an older background nothing used).
The picture holds no version or date, so it cannot go stale.

An install made before 0.9.7.8 keeps its plain menu until HavenDOS is
installed again; `update` replaces the kernel, not the boot files.

## Studio: the THS look as the default theme

A new theme, **Studio**, first in the list and the default for a new
install or anyone who has never chosen one. It maps HavenDOS's 16
colours onto the studio's palette — graphite, paper-white, oxide red,
the website's status colours — so the whole system, the shell included,
takes the look without a single app being changed. And black is not
flat: it fades from the top of the screen to the foot, like the glass of
an old monitor.

- **Its story:** `theme story` tells it; the theme list marks which themes
  have one.
- **Everyone else keeps their theme.** Themes are saved by name; a
  0.9.4-era number file still lands on the right one.
- **Retro PC** switches to the standard VGA colours while it is open, so
  K-DOS and BOOT OS look as they did, and gives the theme back after.
- Other themes use the standard colours, exactly as before.

How: `drivers/vga.c`'s colour table is now one a theme can replace
(`vga_set_palette`), and background 0 can be drawn one pixel row at a
time from a gradient. A cell's pixels depend only on the cell and its
row, so the painted-cell cache that stops flicker stays correct. On real
VGA text memory the 16 colours go to the DAC instead, without the fade.

## Notepad

- **Ctrl+A works.** Ctrl+A is the byte 0x01, which the keyboard queue
  also uses to mark arrow keys and the other extended keys. A bare 0x01
  was read as half of an extended key and lost, along with the key after
  it. It is now stored escaped. This was in the keyboard driver, so it
  was true in every app, not only Notepad.
- **Ctrl+X works.** Cutting part of one line ran a broken copy loop
  first — `<` binds tighter than `?:` — and scrambled the line. Every
  such cut, and the menu's Cut, did this since cutting was added.
- **Wraps at the edge of the screen.** When a word crosses the edge, it
  moves down to a new line whole; one longer than the screen breaks at
  the edge. The break is a real line break.
- **4000 lines** (was 200), of up to 126 characters (was 78).
- **Long lines in a file are wrapped when it opens**, at their last space,
  instead of being cut at 78 characters — and then saved back cut.
- **Text left on screen after a cut is gone.** Deleting lines left the old
  bytes after each line's end, and the editor drew them: a cut of several
  lines left their text on screen over an empty buffer.
- **Save as** on a screen taller than 25 rows typed the name into row 23.
- After a wrap, the view no longer stays scrolled one column sideways.

## The install guide

`docs/HavenDOS-Install-Guide.pdf`, **THS-IG-1001** — "Installing HavenDOS
beside Windows", in the house manual style, for someone who has never
installed an operating system: backing up and the BitLocker key, Rufus,
Disk Management, Secure Boot, the boot-menu keys by make, every installer
screen as it reads, the first start and setup, switching between Windows
and HavenDOS, what to do when something goes wrong, and how to remove it.
Built by `docs/install-guide/build.py`; it is in the release bundle too.

## Tests

- `make test`: 77 suites, 7824 checks.
  - `test_wholedisk_size` 161 (was 124): the 64 MB boundary, 2000 MiB is
    FAT32, 2047 MB is FAT32.
  - `test_installadd` 208 (was 200): the plan is FAT32 for a 1 GB gap;
    the fault injections now measure where the kernel copy starts instead
    of assuming a FAT16 format's write count; a 48 MB gap still proves
    the read-back catches a dropped boot sector on FAT16.
  - `test_kbdinit` 57 (was 52): Ctrl+A alone, Ctrl+A then Up, Ctrl+X, an
    injected 0x01. The 0.9.7.7 driver fails all three.
  - `test_esp` 120 (was 114): every theme file on the EFI partition at its
    size, counted in the space estimate, and a menu that loads it only if
    its fonts do.
  - `test_theme` 53 (was 44): Studio is the default with no saved theme,
    brings its palette and fade; Haven keeps standard colours.
- New QEMU harness `harness/notepad_0978.py`, 9/9: wrapping between words
  at 73 characters, Ctrl+A/X/V on wrapped text, a cut inside one line,
  the cursor past line 200 — typed through QEMU's keyboard.
- **Every QEMU harness, one at a time**, against this build:
  - passed: notepad_0978 9/9, uefi_dualboot (installed beside Windows,
    themed EFI menu, booted), wholedisk_fat32 13/13, fat32_boot, retro_boot
    9/9, retro386 7/7, gfx_boot 15/15, mouse_apps 14/14, window_apps 11/11,
    selfhost 14/14, migrate_fat32 77/77, esp_update, usb_files 17/17,
    replug 11/11, redraw_hunt, resolution_tasks 8/8, appguard 19/19,
    hid_boot 3/3, machine_reset 8/8, lockdown 12/12, helios 32/32,
    fat16_regress, layout_audit, wc_boot 8/8, desktop_layout.
- Harnesses brought up to date with this release, none of it a fault in
  HavenDOS:
  - `fbtext.py`, the screen reader, reads text on Studio's fade: a cell
    whose every pixel row is one colour is blank, and each colour is also
    tried as the ink. Flat-background screens read as fast as before.
  - `mouse_apps` decided a row was highlighted when a pixel was "not
    black"; on the fade nothing is black. It now looks for a saturated
    colour.
  - `migrate_fat32` made its FAT16 system with this ISO, which now makes
    FAT32 at that size. It makes it with the 0.9.7.7 release instead
    (`MIGRATE_OLD_ISO`), then puts this release's kernel on it as `update`
    does -- which is how real FAT16 installs will meet `migrate` now.
  - `fat16_regress` watched the boot log scroll by for the VFS line,
    which the boot splash keeps off the screen; it failed against 0.9.7.7
    as well. It now boots the installed Shell Mode entry and asks `disk`.
  - `uefi_dualboot` expects FAT32 from 64 MB.
- New `harness/guide_shots.py` takes the install guide's screenshots.
