# HavenDOS v0.8.5.6 — "the suites can fail now"

Channel Beta, codename "Atlas" — the last planned 0.8.5. Not "final":
five patch releases in one day all existed because booting it found
something, and promising there is no seventh would be the same kind of
claim that produced them.

```
kernel   2990080 bytes   crc32=1dc1850f
ISO      HavenDOS-0.8.5.6.iso   (BIOS + UEFI)
```

0.8.5's own acceptance check passed at 0.8.5.3 — xHCI works on the HP,
HID enumerates, a whole command types at speed. Everything since has
been fallout rather than scope, so this release closes it out and the
next one is 0.8.6 "Argo", USB mass storage.

---

## `make test` could not fail

This is the headline, and it is worse than any single bug in it.

Every one of the 31 suite invocations ended in

```
/tmp/hd_tdn | tail -1
```

and a shell pipeline exits with the status of its **last** command,
which is `tail`, which always succeeds. So a suite could fail every
check it had and `make test` still exited 0.

Proved rather than argued: one character changed in
`tests/test_diskname.c` printed `19/20 passed`, and `make test` exited
**0**.

Compile failures *did* fail the build — the recipes were
`$(TESTCC) ... && /tmp/x | tail -1`, so a non-zero compiler status
short-circuits the `&&`. That asymmetry is why a missing stub in
`tests/test_accounts.c` was caught in seconds while a wrong answer
never would be. **Compile errors: fatal. Wrong answers: invisible.**

Which means every "the suites were green" claim in this project's
history rested on a person reading the scrollback. The 20 October
handoff's own line — *"the suites were green and the harness said 23 of
23"* — was a human eyeballing output, not a check.

`tools/run_suite.py` owns it now, in one place rather than 31. It exits
with the suite's status, prints one tidy line, and refuses a suite that
ran fewer checks than its declared floor. Verified against five failure
modes:

| planted | result |
|---|---|
| a wrong answer | `make test` exits 2 and stops |
| a missing fixture | caught |
| a suite that hangs | killed and caught |
| a suite that prints no result | caught |
| a suite that shrank below its floor | caught |

A suite that *grows* is fine — the floor is a floor.

## 621 of 1995 checks were silently absent

The other half of the same disease: `0/0 passed` is not a failure to
anything that only reads the text.

`tests/*_fixture.inc` ships in the source archive; `tests/*.img` — 100
MB of FAT volume — does not. The generator rules name both as targets,
which is right, but `test:` listed only the `.inc` files, so **make was
never asked about `tests/fat32.img` and never noticed it was missing.**
Five suites ran against fixtures that were not there: `test_esp`,
`test_update`, `test_fat32`, `test_fat16fmt` and `test_installadd`.

That is 621 of 1995 checks, **31%**, and they are the install, update
and filesystem suites — the ones standing between the installer and
somebody's Windows partition. `test:` depends on the six images now,
and `run_suite.py`'s floor means it cannot hide again even if a rule
breaks.

## An ISO that could not boot passed every check

`grub-mkrescue` builds whatever the installed GRUB platform packages
allow. With `grub-efi-amd64-bin` present and `grub-pc-bin` **absent**
it emits a valid ISO carrying a UEFI El Torito image and no BIOS one —
3.3 MB instead of 15 MB — and says nothing.

A BIOS machine finds no boot image, falls through to the next device,
and lands in PXE. Found by booting one: the screen readback came back
full of iPXE's banner. **All eight build checks had passed on that ISO,
`check_iso_version.py` included** — it compares the kernel's version
with `version.txt`, and both were perfectly correct inside a disc that
could not start.

`tools/check_iso_bootable.py` is in the `make` checks now, with two
judges: the raw El Torito boot record at sector 17 of the file itself,
and `xorriso`'s per-platform report. Tested against a real UEFI-only
ISO and against one with no boot catalog at all.

---

## Bugs

### Three theme pickers, three behaviours, two wrong

`shell/setup.c`, `shell/desktop.c` and `shell/apps_extra.c` each have a
theme picker.

`ui_load_preview()` overwrites `cur_theme`, so **after a preview
`ui_current_theme()` returns the preview**, not the theme that was
active when the picker opened. Two of the three cancelled with

```c
ui_set_theme(ui_current_theme());     /* "restore" */
```

which reads that clobbered value and writes it to disk. **ESC committed
the theme you were hovering over.** So did Tab. Settings also opened on
theme 0 rather than the active one, so one Down keypress followed by
ESC silently changed your theme.

`setup.c`'s preview panel — the one that shows a sample desktop during
first-time setup — was drawn entirely in literal colours: `VGA_BLUE`,
`VGA_CYAN`, `VGA_DARK_GREY`, `VGA_LIGHT_CYAN`. **It looked identical
for all ten themes.** A preview that cannot change tells you the choice
does not matter. It now draws from the theme being previewed, including
a sample window so `win_bg`, `win_fg` and the title colours are visible
and not just the taskbar. It also called `ui_set_theme()` to preview,
which writes `.theme` — one disk write per keystroke.

`apps_extra.c` was right, but captured the entry value *before*
clamping it, so an out-of-range current theme left the preview applied
on cancel; and it cancelled with `ui_set_theme()`, costing a disk write
to back out of a picker.

`tests/test_theme.c`, 41 checks, compiles `shell/ui.c` natively and
**counts writes**, which is how "a preview writes nothing" is checked
rather than asserted. One check exists purely to pin the trap, so that
a future simplification back to the old idiom fails there instead of on
somebody's laptop.

### The v0.8.5.3 keyboard fix reached one of three places

Two 8042 accesses with no I/O delay between them make a controller
emulated in SMM serve a stale status register. v0.8.5.2 and v0.8.5.3
were both that bug. v0.8.5.3 fixed `kbc_drain()`, the runtime read
path, and put `kbc_delay()` beside it.

It did not fix `keyboard_init()`, which talks to the same controller
through the same two ports and had six hand-rolled loops of the same
shape. It did not fix `drivers/mouse.c` at all.

`tests/test_kbdinit.c` compiles both drivers against a recording I/O
seam and measured **seven violations in `keyboard_init()` and none in
`kbc_drain()`** — so the rule was right and its application was
partial. Two mechanisms:

- Touch `0x60`, then read `0x64` with no delay: OBF still reads set, so
  the byte just taken is taken again. The doubling, exactly.
- Read `0x64` twice with no delay: a polling loop reading a register
  that cannot have changed. On an emulated controller it can see a
  stale "input buffer empty" and return while the buffer is still full,
  and **the command byte written next is dropped**. A dropped
  `outb(0x64,0x60)` or `outb(0x60,cfg)` means the config write never
  lands: no IRQ1, or scancode translation left off, on a machine whose
  only keyboard is that one.

For the mouse the same stale read costs differently: `mr()` returning
early makes `mouse_rd()` read `0x60` before a byte is there and return
the previous one, so a three-byte movement packet is parsed one byte
out of phase — a pointer that jumps. `mw()` returning early drops the
`0xD4` prefix or the command after it, and a mouse that misses its ACK
is one this driver gives up on.

The delay and its spin bound now live **together** in `include/io.h` as
`i8042_delay()` and `I8042_SPINS`, because they have to move together
and that is precisely why one driver had the fix and two did not.

The delay goes **before** each status read, not after. A helper that
delays after reading leaves the next helper free to read again with
nothing in between, and two status reads either side of a call boundary
are as adjacent as two inside one loop. That detail accounted for three
of the seven violations and was only visible because the suite counted
them.

**Sizing it mattered more than the fix.** An iteration used to be one
`inb`, about a microsecond, so `0x10000` was a ~65 ms budget. With the
delay in front an iteration costs ~5 µs, making the same bound a third
of a second **per wait**. The HP has no PS/2 port of either kind, so
`mouse_init()` times out on every wait it performs — measured at
**327,723 port accesses** — and keeping the old bound would have added
over a second to boot on the one machine this runs on. `I8042_SPINS` is
10000, restoring the ~50 ms figure. Caught in the suite, not on
hardware.

### The mouse could not reach most of the screen

`mouse.c` clamped the pointer to 79 and 24, written out. On 80x50,
90x60 or the framebuffer console the pointer stopped partway across and
most of the screen was unreachable. It clamps to `vga_cols()-1` and
`vga_rows()-1` now. The suite check fails against the old code — "got
79" on a 90-column grid.

---

## Updating from Linux

`rescue/stage-havendos-update.sh` is new: the Linux half of
`Stage-HavenDOS-Update.ps1`. That one is PowerShell and `mountvol`, so
it only ever helped a machine dual-booting Windows — and HavenDOS is
also installed beside Linux Mint on a second laptop, which had no way
to stage an update at all.

```sh
sudo ./stage-havendos-update.sh          # finds the files beside it
```

It finds the ESP by GPT partition type GUID rather than by mountpoint
or label, uses an existing mount if there is one, verifies the
manifest's `size=` **and** `crc32=` against the kernel before mounting
anything, writes the two files, and **reads them back and re-checks the
CRC** before reporting success. Nothing else on the ESP is touched.

Every refusal path is tested: a manifest describing a different size, a
manifest whose CRC does not match, a file that is not an ELF, a
manifest with no `size=` line, and either file missing. Six for six.

The mount-and-copy path itself is **not** tested — the container this
was built in has no vfat support, so it could not be exercised. The
checks in front of it are.

---

## Testing

**2055 checks across 32 reporting suites**, up from 1949 across 30 at
the start of 0.8.5.6. Two new suites: `test_theme` (41) and
`test_kbdinit` (19).

All five injection harnesses green: 54, 28, 27, 10 and 20 — 139
injections. `reboot`/`poweroff` under QEMU 4/4 on `pc` and `q35`, which
is the regression check that matters for the keyboard rewrite: the
harness has to **type** `reboot` at a prompt to work at all.

## Known, and deliberately not in this release

- **`poweroff` works on the HP by luck, probably.** `0x604` is tried
  first and the machine died on it, so its firmware most likely set
  PMBASE to `0x600`, making QEMU's hardcoded address accidentally
  correct. Three hardcoded emulator addresses that happen to match one
  laptop is the same bug class as nine hardcoded reset sites that
  happened to match QEMU. A read-only ACPI reporter — find the RSDP,
  walk the RSDT/XSDT, print `PM1a_CNT_BLK`, `RESET_REG` and
  `RESET_VALUE` from the FADT, write nothing — turns it into a number.
  Next release.
- **The real `isodir/` is still not in the archive.** What ships is
  `make_isodir.py`'s reconstruction.
- **AHCI's status is uncertain, in a good way.** Not-done item 7 says
  it has never run on real silicon and cannot. But HavenDOS is
  installed on a SATA laptop that somebody now uses daily. One line of
  `dmesg` from that machine settles whether `drivers/ahci.c` has been
  on real silicon all along.
