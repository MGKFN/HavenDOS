# HavenDOS 0.8 Beta "Beacon"

This replaces the three releases that went out in quick succession —
v0.7.21, v0.7.22 and v0.7.23 — with a single version. They were one body of
work and three numbers was confusing, so here is what each of them was, in
order, and what it all adds up to.

```
  v0.7.20   the PCI foundation        one bus walk, all functions, lspci
  v0.7.21   USB                       the UHCI host controller
  v0.7.22   the boot log              one real kernel log, three screens gone
  v0.7.23   the display               1280x800, and UEFI boot with it
  --------------------------------------------------------------------
  0.8 Beta  all of the above
```

437 checks pass, up from 303 at v0.7.20. The ISO is hybrid: it boots on BIOS
machines and on UEFI machines.

---

## 1 — USB (was v0.7.21)

**The UHCI host controller.** A USB stick is detected, its port reset, an
address assigned and its device descriptor read. Control and bulk transfer
primitives, all polled, no interrupts.

v0.7.20 had made the controller *findable* — every PCI scan before it looked
at function 0 only, and on a PIIX3 the USB controller is function 2 of device
1, so it was not undetected but **undetectable**. v0.7.21 made it work.

Two things that only show up on real machines, found by asking how it ran on
UTM SE:

- **The EHCI holds the sockets.** A USB 2.0 controller is normally an EHCI
  with UHCI companions sharing the same physical ports, and which one gets a
  device is decided by speed — a USB 2.0 stick attaches to the EHCI at 480
  Mbit and the companion sees an empty port. The driver asks for the ports
  back, which the EHCI specification provides for (CONFIGFLAG = 0 routes
  every port to the companions; it is the state the hardware resets into).
  The device re-attaches at full speed, 12 Mbit. Without this, USB does not
  work on UTM SE or on any ICH chipset at all.
- **Every controller is driven, not just the first.** A PIIX3 has one UHCI;
  an ICH has three to five. A device in any other socket was invisible with
  no error anywhere.

**What it does not do yet:** read files off the stick. That needs the
configuration-descriptor walk, Bulk-Only Transport and a small SCSI layer,
all of which sit on the transfer primitives that now exist.

New: `usb`, `usb -v`, `usb -vv`.

## 2 — The boot log (was v0.7.22)

HavenDOS had three boot screens and two were fiction.

The POST screen announced "TechHaven BIOS v2.1" and performed a power-on self
test — but HavenDOS is not firmware, and the real BIOS had already POSTed the
machine before GRUB loaded the kernel. The "verbose boot log" was 24 string
literals of which four were real; it told every machine that ATA was absent,
that FAT16 was on a VirtIO partition, that the heap was 4 MB (it is 256 KB)
and that the GDT had 256 descriptors (it has 3). `dmesg` printed its own copy
with timestamps typed one millisecond apart that nothing had measured.

Now there is one kernel log, written by each subsystem as it initialises,
from inside the code that knows the numbers:

```
[   0.700]  ok  GDT: 3 descriptors at 0x229000, flat 32-bit segments
[   0.704]  ok  Heap: 255 KB static pool
[   0.711]      PCI 00:1F.2  8086:2922  class 01/06/01
[   0.884] warn ATA: nothing on 0x1F0/0x170 (no legacy IDE here)
[   3.258]  ok  NET: eth0 10.0.2.15
```

Echo is on from the first line, so a kernel that dies during init has already
said how far it got. `quiet` on the kernel command line silences it; `dmesg`
replays it paged. The POST screen became a splash with no hardware claims and
no keypress — the old pair cost two mandatory keypresses and half a second of
padding.

Four bugs it found on its first run: the heap was off by sixteen, the GDT by
a factor of eighty-five, the first `NET:` line printed the IP backwards
(`net.c` keeps the first octet in the low byte), and on a Q35 machine it
immediately said `ATA: nothing on 0x1F0/0x170` — which is the whole reason
`update` fails there, stated at boot.

`tools/check_boot_log.py` now runs with every build and refuses a typed
timestamp or a table of log lines, so the fiction cannot come back.

## 3 — The display, and UEFI (was v0.7.23)

Every character HavenDOS drew went into VGA text memory at `0xB8000` — 720x400
on a BIOS machine, and **nonexistent under UEFI**. That is why it looked like
1985 and why it showed nothing at all on modern firmware.

`drivers/vga.c` already had a shadow buffer and exactly one place where a
cell became pixels. Routing that through a glyph blitter means the desktop,
every app, the shell, the setup wizard and the boot log all render at
**1280x800** with nothing changing above the driver. The grid stays a
character grid, which keeps the retro look and keeps every existing layout
pixel-correct.

The 8x8 font is scaled 2x across and 4x down to give exactly 80x25, which is
how an 8x8 CGA font looked on a 400-line display. `res 80 50` gives uniform
2x with square pixels.

**It boots on UEFI.** The ISO is hybrid — a BIOS boot image and a UEFI one —
and on a machine with no BIOS, no VGA text memory and no INT 10h anywhere, it
comes up to a usable shell. That only works because the console exists: a
UEFI boot without it is a black screen.

Underneath this was a real bug: `mb1_info_t` was **wrong by eight bytes**.
The four VBE fields at offsets 80-87 are `u16` in the Multiboot specification
and were declared `u32`, so every framebuffer field after them was read from
the wrong place. Nothing had noticed because the header never asked for a
framebuffer — which is why `drivers/vesa.c` had been dead code, not broken
code, for its whole life.

### The installer, rebuilt

There were three installers and only one of them worked. `shell/iso_installer.c`
had a unified disk layer that probes VirtIO and then ATA/SATA; the desktop
Install app and the shell's `install` command each carried their own older,
VirtIO-only copy and passed the installer a VirtIO index, which only ever
matched a unified one while VirtIO was the only bus. On an IDE machine both
reported **"No VirtIO drives found"** next to a disk the ISO installer had no
trouble with.

There is one installer now, reached from the desktop icon and from `install`,
with a redesigned flow: pick a disk from a list that names its bus and size,
see exactly what will be written, type YES, watch a progress bar that follows
the window rather than fixed coordinates.

Worse than the wrong scan: on an installed system `install` **wiped the disk
and then failed**. The build is two-pass — pass 1 has no embedded image, pass 2
embeds it and ships on the ISO — so the kernel an installed system runs carries
nothing to install, and `havendos_install_to_drive()` discovered that at step 7
of 9, after repartitioning and formatting. It is a precondition now; the
installer refuses before touching anything and says why.

And the ISO's **"Shell Mode"** menu entry now does what it says: install media
used to force the installer regardless of boot mode. A live shell on the ISO
gives `lspci`, `usb -vv`, `dmesg` and `ifconfig` on a machine with nothing
installed, and is where the installer can be run by hand.

### Flicker and bleed on the framebuffer

Two symptoms, one root cause each, both only visible once the console drew
pixels instead of writing VGA text memory.

**Unchanged cells were repainted.** `put_cell()` skipped a cell the screen
already showed in text mode and did not in framebuffer mode. A repaint is one
store in text mode and 512 in a framebuffer, with the screen live — so every
draw-poll-sleep loop (the clock, the dashboard, the monitor, the games, the
screensavers) re-rendered its static labels several times a second in front of
the user. The framebuffer branch carries the same dirty check now: a running
clock touches a 2-4 cell box around the digits that change, and nothing else.

**Apps showed the desktop through themselves.** `vga_clear()` is shadow-only by
design, so a frame reaches the screen when something flushes.
`keyboard_waitchar()` flushes before it blocks, which covers anything waiting
for a key; a program that polls reaches only `sleep_ms()`, which did not. Those
apps ran with the desktop still underneath them wherever they did not draw.
`sleep_ms()` presents the frame now, and those two call sites are the whole
commit contract.

### The help screen was missing a third of the commands

`help` listed 89 of the 126 commands the shell knows. Missing: `usb`,
`lspci`, `neofetch`, `wget`, `curl`, `selftest`, `tlsinfo`, `calendar`,
`todo`, `paint`, `bpkg`, `df`, `pwd`, `cd`, `zip`, `unzip` and twenty more —
several of them the headline feature of the release that added them. All of
them are listed now.

The renderer was cutting the name column at exactly 16 characters whatever was
there, so `nslookup / dns / host` came out as `nslookup / dns /host` with the
rest read as its description, and `dashboard / htop`, being exactly 16, ran
into its own description with no gap. It splits on the separator now, and the
few names that cannot fit the column carry their syntax in the description
instead.

### Found while photographing it

Driving a real install through every screen for the manual turned up five more
things that only show on a running system:

- **GRUB could not find the installed volume by label.** The installer wrote
  `HAVENDOS` into the BPB, which is the convenience copy that tools print, but
  never wrote the root-directory entry with attribute `0x08` that actually
  names a FAT volume. `search --label HAVENDOS` therefore failed on every
  installed disk and the menu's first entry fell through to the explicit
  `(hd0,msdos1)` one behind an error message.
- **`disk` reported VirtIO on machines that have none.** It was written when
  FAT16 only ever lived on a VirtIO disk and still said so after the installer
  learned to write to IDE. It now names the bus the volume is actually on.
- **`neofetch` drew its colour swatch at a fixed row 23**, which landed under
  the next prompt once the output had scrolled. It follows the word that
  labels it now.
- **The setup wizard counted "Step 7 of 6".** The terms and privacy screens
  are steps 1 and 2 and are drawn elsewhere, so the wizard's own screens start
  at 3 and the run is seven screens long.
- **The quiz header said twelve questions and ended after eight**, and drew
  the score on top of its own title.

The document pane in the terms screens is painted before the text goes into
it, the boot menu is drawn at 1280x800 to match what it boots into, and its
background is generated at that size rather than scaled up from 800x600.

Also fixed: apps no longer leave the desktop showing around them, and the
version in the GRUB menu footer — which read **v0.5.9.9**, thirteen releases
stale — is a generated theme label instead of text painted into a PNG where
no literal check could ever find it.

---

## Known limits in this beta

- **A USB stick is seen, not read.** Mass storage is the next piece of work.
- **Bootable is not usable on modern hardware.** On a real UEFI laptop the
  keyboard is USB behind xHCI, which this kernel does not drive, and storage
  is AHCI or NVMe rather than legacy IDE.
- **An installed system cannot install HavenDOS.** The kernel it runs carries
  no image of itself, for the two-pass reason above. The installer says so and
  refuses rather than wiping anything. Installing from the ISO — or from the
  ISO's new live shell — works normally. A self-hosting install would have to
  stream `/boot/havendos.elf` from the running filesystem, which needs a
  read-at-offset path in `fat16.c`: the kernel is 631 KB and the heap is 255 KB.
- **Only one ATA disk is seen.** `ata_detected()` probes the primary master, so
  a second IDE disk does not appear in the installer's list.
- **`update` needs legacy IDE.** On a Q35/AHCI machine the CD is not on the
  ports `atapi_init()` probes, so the updater cannot read the ISO. Use an
  i440FX machine, or reinstall from the ISO.
- **Apps still lay out for 80x25.** They are cleared and redrawn so nothing
  bleeds through, but they do not yet fill a larger grid.
- **The ISO is about 16 MB**, not the 5-6 MB of earlier releases, because it
  carries a UEFI boot image as well as a BIOS one.
