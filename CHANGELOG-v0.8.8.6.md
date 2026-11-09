# HavenDOS v0.8.8.6 "Helios beta 7" — 64x24 was the whole story

Channel Beta, codename "Helios", **beta 7**. 3 November 2026.

A small release with one finding in it, and the finding closes a
report that has been open across three betas.

---

## `layout` worked, and it named the problem in one line

Kyan ran beta 6 on the HP and sent back `dmesg`:

```
warn layout: System Monitor drew 1466 cell(s) outside its 64x24 console
warn layout: Shell drew 17 cell(s) outside its 64x24 console
warn layout: Text Editor drew 50 cell(s) outside its 64x24 console
warn layout: Dashboard drew 3855 cell(s) outside its 64x24 console
```

**Sixty-four by twenty-four.**

His panel is 1024x768. `vga_fb_readable_grid()` at the readable
16-pixel cell gives 1024/16 = 64 columns and 768/32 = 24 rows — and
**beta 6's `resolution fit` offered exactly that, he took it, and it
was saved.**

Every app in the registry lays out for 80x25. A console *smaller*
than that cannot be rescued by the viewport: there is nowhere to
centre an 80x25 box inside a 64x24 screen. So the apps drew off the
edge with only the grid to clip them.

**It was the console again, one size further down than last time.**
Beta 6 fixed a console too small for the panel; this fixes a console
too small for the apps.

### Reproduced exactly, then fixed

| console | apps drawing outside |
|---|---|
| 64x24 | **21**, including Text Editor at **the same 50 cells** he saw |
| 80x25 | **none** |

Both measured by `layout` on a booted ISO.

---

## The fix: a grid the apps can actually use

`vga_fb_app_grid()` is the policy, in one place:

- the **readable** 16-pixel cell if the grid is still at least 80x25,
- a **smaller cell** if that is what it takes to get there,
- and a **refusal** if the panel cannot show 80x25 at any cell size.

| panel | readable cell | what it gets now |
|---|---|---|
| **1024x768** | 64x24 | **128x48** at an 8-pixel cell |
| 1280x800 | 80x25 | 80x25, unchanged |
| 1920x1080 | 120x33 | 120x33, unchanged |
| 800x600 | 50x18 | **100x37** |
| 640x384 | 40x12 | **refused** — 80 columns but 24 rows, one short, at every cell size |

`resolution fit` uses it, so it can no longer offer a console no app
fits in. The `resolution` report uses it too: it was printing
`Readable: 64x24` beside a command that would then set it, and **a
report that names a different grid from the command beside it is
worse than no report.**

### And a saved 64x24 heals itself

A saved grid is normally left alone, and still is — except when it is
under 80x25. That one is replaced at boot, once and loudly:

```
warn Console: the saved 64x24 grid is smaller than the 80x25 every app lays out for
ok   Console: 128x48 instead, and saved ('resolution' changes it)
```

The reasoning: such a grid could only have been chosen because **this
OS offered it**, and a console that cannot display a single app is a
broken state rather than a preference anybody expressed. `resolution`
sets it back at any time.

---

## GRUB now offers the panel's native mode first

His HP landed on 1024x768 because the mode list started at 1280x800,
which that GPU does not offer. The list now leads with 1920x1080 and
the common laptop modes, and still ends with 640x480 — GRUB takes the
first the hardware offers, so adding modes to the front costs nothing
on a machine that cannot do them.

The console refuses to go below 80x25 whatever mode it is given now,
but a bigger framebuffer is a better starting point than a rescued
one.

---

## An ISO with an empty menu used to pass every check

While widening the mode list I deleted `isodir/boot/grub/grub.cfg`,
expecting the build to regenerate it. **It did not.** The file came
back zero bytes, the ISO was built from it — and the build said:

```
check_iso_bootable: ok, havendos.iso boots BIOS and UEFI (13.7 MB)
```

The El Torito catalog was intact and both platform images were there,
so the disc "booted" in the only sense anything was measuring. What
it booted to was a GRUB rescue prompt.

`check_iso_bootable.py` extracts `grub.cfg` **out of the ISO** now —
not off disk, because the file on disk is not necessarily the file
that got packed — and fails a disc with no menu entries.

> And the first version of that check **failed the real ISO**:
> `xorriso -extract ... -` returns 0 and writes nothing, so a
> perfectly good disc read as having an empty menu. A checker that
> fails a correct build is worse than the gap it was closing, and it
> was caught only by extracting the file by hand to see. It extracts
> to a real file now, and is tested both ways: it passes the shipped
> ISO and fails one built from an emptied `grub.cfg`.

---

## Memory, from the HP

```
1810716KB free / 1883136KB total
```

Confirmed working — beta 6's heap and PMM changes are doing what they
should on real hardware, and the figure is the machine's usable RAM
below 4 GB rather than anything this kernel is clamping.

---

## The numbers

| | v0.8.8.5 | v0.8.8.6 |
|---|---|---|
| suites | 50 | 50 |
| checks | 4773 | **4786** |
| injection harnesses | 32 | 32 |
| `test_console` | 278 | **291** |
| console injections | 28 of 28 | **31 of 31** |
| apps clipped at 64x24 | 21 | 21 — now unreachable |
| apps clipped at 80x25 | 0 | 0 |

## What is NOT verified

- **The 1024x768 → 128x48 path on real hardware.** Proven in the
  suite against a simulated panel; QEMU hands this project 1280x800,
  which is the one size where every console change is a no-op.
- **The self-heal of a saved 64x24 grid.** The decision is tested, but
  a live ISO keeps no config across boots, so the boot path itself has
  only run on his machine's shape in theory.
- **Whether GRUB picks 1920x1080 on the HP.** It depends on what that
  GPU reports; the list only says what to prefer.
- **Reading the 32 GB stick** — still the half that is unproved.

## Still open, and one new one

- **The stick did not come back on hot-plug.** His `dmesg` shows
  `usb: 3 devices, 0 disks, 0 NICs after hot-plug` with the Kingston
  plugged in, while beta 5 enumerated it fine **at boot** on port 12.
  So boot enumeration works and the hot-plug rescan does not find it
  — a new, narrow question with a known-good comparison case.
- **I2C-HID**, waiting on PCI BAR assignment for `00:15.0`/`00:15.1`.
- **Stage 3 piece two**: X.509 v3 and PKCS#1 signing on top of the
  DER writer.
- **A real allocator**, **64-bit**, **NTFS**, **ext4** — unchanged.
