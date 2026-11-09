# HavenDOS v0.8.8.7 "Helios beta 8" — the ISO did not boot on your laptop

Channel Beta, codename "Helios", **beta 8**. 6 November 2026.

Three things, and the first one is the serious one.

---

## 1. The ISO could not UEFI-boot at all, and a check said it could

Booted under OVMF — real UEFI firmware, not an argument about file
formats — the v0.8.8.6 ISO produced:

```
BdsDxe: failed to load Boot0001 "UEFI QEMU DVD-ROM QM00005 "
        from PciRoot(0x0)/Pci(0x1F,0x2)/Sata(0x2,0xFFFF,0x0) : Not Found
>>Start PXE over IPv4.
```

No loader found; fall through to network boot. That is what Kyan's HP
does with it, and his HP has **no CSM** — there is no BIOS path to
fall back on.

The disc carried only `BOOTIA32.EFI`. Every x86_64 machine looks for
`\EFI\BOOT\BOOTX64.EFI` and nothing else. `grub-mkrescue` emits
whichever it can from the GRUB platform packages present, so the
ISO's UEFI-bootability silently depended on whether
`grub-efi-amd64-bin` happened to be installed in the build container.

### The check that blessed it

`check_iso_bootable.py` reported:

```
check_iso_bootable: ok, havendos.iso boots BIOS and UEFI, 8 menu entries
```

on that disc. El Torito platform id `0xEF` means "UEFI" and says
**nothing** about the architecture of the loader inside. The platform
list was necessary and not sufficient, and that gap is the actual
defect — the ISO was one missing package away from unbootable, every
time, with no signal.

So the check now opens the EFI image, reads
`/EFI/BOOT/BOOTX64.EFI` out of its FAT filesystem with mtools, and
checks the PE machine field is `0x8664`. The build fails if it is not.

Four injections, because a check that has only ever passed is not a
check:

| injected into a real ISO | reported |
|---|---|
| `BOOTIA32.EFI` renamed to `BOOTX64.EFI` | `is i386 (PE machine 0x014C), not x86_64` |
| random bytes under that name | `is not a PE executable` |
| no `BOOTX64.EFI` at all | `the EFI image has no /EFI/BOOT/BOOTX64.EFI` |
| mtools removed from `PATH` | falls back to a raw scan, **and says it is weaker** |

A fifth injection — a truncated PE — turned out not to be a defect:
the PE header sits in the first 2 KB, so the truncated file is still a
valid i386 PE and was correctly reported as i386. **Recorded rather
than deleted**: the injection was wrong, not the check.

The rebuilt ISO boots the full eight-entry menu under OVMF and comes
up in the setup wizard.

---

## 2. The gfxmode list in grub.cfg never reached the kernel

v0.8.8.6 added

```
set gfxmode=1920x1080,1600x900,...,1024x768,800x600,auto
```

and a comment saying this is what decides the console's resolution.
**It is not.** GRUB honours the mode requested in the kernel's own
multiboot header above `gfxmode` and `gfxpayload`, and
`boot/boot.S` has asked for a fixed `1280x800` since the field was
written.

Measured, not reasoned: with the list forced to a single
`1920x1080`, the kernel still came up on a 1280x800 framebuffer.

**This is the real cause of the 64x24 console.** 1280/16 × 800/32 is
exactly 80×25, so on any machine that can do 1280x800 — QEMU included
— the grid came out right and nothing looked wrong for three
releases. The HP cannot, GRUB fell back to its own "auto", that
landed on 1024x768, and 1024/16 × 768/32 is 64×24. The console was
never choosing badly. It was being handed the wrong framebuffer.

Setting the header to `0` ("no preference", which both Multiboot 1 and
the Multiboot 2 framebuffer tag define) was tried and is **worse**:
GRUB then skips the list entirely and goes to "auto", which under
QEMU gave 800x600. So the request stays as it is, the console keeps
coping with whatever arrives, and the comment that said otherwise has
been corrected in both files. No behaviour changed here — only a
belief the tree was carrying.

---

## 3. The desktop chrome, on a console that is not 80x25

Kyan, on beta 7: *"the apps now go top left or center notification
center goes in middle"*.

Same family as the 64x24 bug: a number that was once a measurement of
the screen and became a constant.

* **`NC_X` was a literal 55 and `NC_BOT` a literal 23.** On a 160×50
  console the notification drawer opened at column 57 and stopped at
  row 23 — floating mid-desktop, **78 columns away from the bell that
  opens it**, and 26 rows short of the taskbar. Both are derived now,
  and at 80×25 the expressions evaluate to exactly 55 and 23, so the
  console every app was designed against is unchanged.

* **The bell's click target was a literal `mx>=55 && mx<=63`.** That
  was the drawn extent at W=80 *minus the unread count*, so the two
  copies had already drifted on the console they were both written
  for. There is one function now, `ui_taskbar_notif_hit()`, beside the
  code that paints it.

* **A boxed app got a ring of dots.** On a big panel that reads as an
  80×25 screen stranded in the middle of a larger one, which is
  exactly the report. It gets a window now: a title bar carrying the
  app's name and `ESC closes`, a frame, and a shadow.

* `ui_draw_corner_clock()` wrote the clock at a literal column 71.
  Nothing calls it — the desktop repaints in full every second — but
  it is derived now so a future caller does not reintroduce this.

### Proved against a control, not against itself

`harness/desktop_layout.py` boots the real ISO, widens the console
with the shipping `resolution max` command, and reads the cells back.
Six checks. Against this release all six pass. Against a control
built from this same tree with **only** these three fixes reverted:

```
  FAIL  the drawer is anchored to the right edge, not floating mid-desktop
        'Notifications' starts at column 57 of 160. The bell that opens it
        is painted at column 135, so the drawer opened 78 columns away
        from its own control.
  FAIL  the drawer reaches the taskbar instead of stopping at row 23
        its last row is 23 of 50; the taskbar is row 49
  FAIL  the boxed app is given a window title bar saying how to leave
  FAIL  clicking the bell opens the drawer
```

Four failures, each naming the literal it came from.

The harness earned its own entry in this list on the way: its first
version booted at 1280x800, which at the readable cell is **exactly
80×25**, so every coordinate landed where its literal said it would
and all three bugs came back clean. A layout harness that runs at the
one resolution the layout was written for tests nothing at all.

`harness/qemu.py` had the same disease: `mouse_to()` homed the pointer
with a flat `-100`/`-40`, written when the clamp really was 0..79 and
0..24. On a 160×50 console that does not reach the corner, so every
click landed at an invisible offset and a correct hit box looked dead.
Derived from the grid now, with the old values as a floor.

---

## Also found, not fixed

`login.c`'s first-run **Create Account** form is a `while(1)` with no
way out. The footer advertises `ESC=Back`, but that belongs to the
outer chooser: once Create Account is entered, the only exit is an
account that validates, and a blank password is refused there even
though the setup wizard allows one. Someone who picks it by mistake
is stuck. Found by a harness getting trapped in it. Not touched in
this release because it is a login-path change and this one is about
display, but it is real and it is next.

---

## Suites

49 suites, **4786/4786**, unchanged. Nothing native was added, on
purpose: every fix in section 3 is a coordinate, and a native test
that computes a coordinate from the same macro the kernel uses agrees
with itself by construction. The screen is the only judge that is not
ours, so the new evidence is a harness and a control build.
