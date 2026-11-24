# HavenDOS v0.9.7.9 Stable "Mnemosyne" — the installer asks which free space

Kyan, 24 November, after installing 0.9.7.8 on his laptop: the disk had
two free spaces — 34 GB he was keeping for macOS, and the 2 GB he had made
for HavenDOS. The installer took the 34 GB one and made 32 GB of it
HavenDOS. It never asked.

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU.

## What went wrong

The install beside another system took the **first free space on the
disk** that was big enough, and used up to 32 GB of it. On a disk with
one free space that is the right answer. On a disk with two, it is a
guess, and here it guessed the space that belonged to something else.
The only sign was the size in the disk list's Notes column.

Nothing outside the new partition was written (the install's four-sector
rule held), so the space is not lost: `uninstall` in HavenDOS removes the
partition and its boot files, and the 34 GB is free again, in one piece.

## Fixed

- **When a disk has more than one free space, the installer asks which.**
  A new screen, *Choose the Free Space*, lists each one: its size, where it
  is on the disk, and the partitions either side of it. It says how much of
  the highlighted one HavenDOS would use. The disk list says *keeps disk -
  space chosen next* for such a disk.
- **The bar starts on the smallest free space that fits**, and an install
  with no choice made (one free space, or an older caller) takes the
  smallest that fits rather than the first. A small space is the one most
  likely to have been made for this; a big one is the one most likely to be
  meant for something else.
- The plan, the preview and the install all use the chosen space. If the
  partition table changes so that it no longer exists, the installer falls
  back to the smallest that fits rather than to the first.

## Install guide

THS-IG-1001 is now **REV B**, for 0.9.7.9: Step 8 shows the new screen,
and every screenshot is from this release.

## Tests

- `make test`: 77 suites, 7836 checks. `test_installadd` 220 (was 208):
  a disk with 34 GB free, a partition, then 2 GB free —
  - both spaces are listed, in disk order;
  - with no choice it plans in the 2 GB one (0.9.7.8: the 34 GB one), FAT32;
  - told to use the 34 GB one, it does, at the 32 GB cap;
  - a choice that no longer exists falls back to the smallest;
  - the real install leaves the 34 GB space free and whole.
- New QEMU harness `harness/two_gaps_0979.py`, **14/14**: builds that disk
  (6 GB standing in for 34), boots the ISO and drives the real installer —
  the new screen appears, lists both, starts on the 2 GB one; the install
  goes there; sgdisk afterwards finds the 6 GB space free and whole and
  every partition where it was.
- Rerun against this build: uefi_dualboot (all checks), selfhost 14/14,
  fat16_regress.
