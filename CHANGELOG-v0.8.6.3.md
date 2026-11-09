# HavenDOS v0.8.6.3 "Argo" (Beta)

**Update from a USB stick, and copy a file onto one.**

v0.8.6 taught this kernel to read a USB stick. This release is the two
things that makes worth having: staging an update on a stick instead of
booting Windows to mount the EFI System Partition, and putting a file on
a stick from inside HavenDOS.

## `update` has a third source

`fs/update_src.c` had two: the EFI System Partition and the ATAPI
CD-ROM. It now has `UPD_SRC_USB` — **the first FAT32 volume on a
removable disk carrying a manifest.**

Everything needed for it arrived in v0.8.6 and went unused: `fs/blkdev.c`
registers USB disks, so `fat32_scan_volumes()` already walked them, and
`blk_removable()` already knew which disks were sticks. The USB source
differs from the ESP source by **one line** — `blk_removable(v[i].disk)`
where the other asks `v[i].is_esp` — and shares the manifest reader and
the kernel reader with it rather than carrying a second copy of either.

### The layout on the stick is the ESP's layout

    \EFI\HAVENDOS\update\update.inf     the manifest
    \EFI\HAVENDOS\update\havendos.elf   the kernel beside it

An `\EFI` folder is an odd thing to find on a plain stick. It is used
anyway because `stage-havendos-update.sh` and
`Stage-HavenDOS-Update.ps1` — shipped in every bundle since 0.8.4 —
already build exactly that layout. Point either one at a mounted stick
and it produces a working update stick with no new code. One layout that
looks strange beats two that can disagree.

### A stick is chosen BEFORE the EFI System Partition

This changes what an existing machine does, so here is the reason.

`update` does not delete a staged update after applying it. So an ESP
that has been updated from once keeps that manifest for ever — and with
the ESP probed first it would be found every time. `update` would answer
*"that version is already installed"* and a stick plugged in two minutes
ago would never be looked at. The leftover would beat the thing somebody
just did.

A removable disk is present because a person put it there. That is the
more recent intent, so it is asked first. It is not silent either way:
`update` prints which source it chose before it touches anything.

### It is FAT32 on the stick, not FAT16, and that is not an oversight

A source stays open until `cmd_update()` reaches `done:`, which is
*after* the new kernel has been written. The ESP source holds
`fs/fat32.c`'s mount slot for that whole time, which is harmless — the
kernel is written through `fs/fat16.c`, a different driver.

A FAT16 source would have to hold `fat16_borrow()`, pointing **that**
driver at the stick — and `cmd_update()` reads and writes the system
volume through it in between, to take the backup at step 3. The backup
would be looked for on the stick and the write would be refused by the
borrow.

Reordering those steps is not the fix: step 3's own comment says the
backup is taken before the image is read *"because both use the same
staging buffer"*, and there is one 8 MB buffer. So a FAT16 stick means
changing how `update` stages bytes — the most dangerous command in this
system — which does not belong in a patch release. It gets its own.

Until then, an update stick needs a FAT32 partition. **Note for the HP:**
the Kingston's 59 GB partition is exFAT and its 32 MB one is FAT16, so
neither will serve as an update source until one is reformatted FAT32.

## `fat16 cp <src> <n>:<dst>`

Copies a file from this system's own volume onto a volume on a stick.
The first thing in this tree that writes to a partition it does not own.

    fat16 cp /boot/havendos.elf 1:/HAVENDOS.ELF

### A second door, not a flag

`fs/fat16.c` has a new entry point, `fat16_borrow_rw()`. The read-only
`fat16_borrow()` was **not** given a parameter that makes it writable —
a caller has to ask for the writable one by name, because the difference
between them is whether this kernel may write to somebody else's
partition.

Everything that makes it safe is a refusal in the driver, where no
caller can forget it:

| Refused | Why |
|---|---|
| a disk that is not removable | on this laptop, disk 0 holds Windows |
| the boot disk | the system's own volume goes through the VFS, which knows about the mounted filesystem; a borrow does not |
| a read-only disk or volume | a write-protect switch is an answer |
| a FAT larger than the 128 KB cache | a cluster lookup past the cached FAT would allocate a cluster that is already in use |
| a second borrow while one is held | `g_saved_dev` would be overwritten and the system volume would never come back |

`fat16 cp` also reads its source file **before** borrowing, because
borrowing displaces the system volume. A `cp` that borrowed first would
look for its source on the *stick*.

### The one line that could destroy an installed system

While a borrow is held, the driver's device index names the stick. After
`fat16_return()` switches it back, it names the machine's own disk. The
FAT cache flushed on the wrong side of that switch writes **the stick's
allocation table over the system volume's** — which does not fail, does
not warn, and shows up at the next boot that walks that FAT.

So the flush happens first, while the device still names the stick, and
`fat_dirty` is cleared before the switch whatever the flush did. A
failed flush has already lost the data; the only remaining question is
whose disk it lands on, and the answer has to be nobody's.

## Testing

`tests/test_fat16borrow.c` — **82 checks**, two real FAT16 volumes made
by this tree's own formatter on three RAM disks, and **write counters per
disk**. The assertion that matters is not "the file arrived", it is "the
system disk was not written to", which is a fact about sectors.

It passed 82/82 the first time, which by this tree's rule is the least
trustworthy result available — and `tools/check_fat16borrow_suite.py`
found **six holes in it**, including the flush-ordering defect above:

* The flush order was invisible because `fat16_write()` flushes as it
  goes, so the cache was always clean by the time `fat16_return()` ran.
  The suite now **fails the stick's writes part way through**, which is
  what a yanked stick does and the only state in which the ordering can
  be seen at all.
* The `blk_removable()` and `blk_boot_dev()` refusals both guarded the
  same disk, so removing either changed nothing. The suite now has a
  third disk — fixed, not the boot disk — and a case where the stick
  *is* the boot disk, so each refusal can be told from the other.
* Block indices were written as literals. `blk_scan()` registers
  removable disks last, so the stick is device 1 with two disks and
  device 2 with three: the suite was testing the wrong disk the moment a
  third appeared. Nothing is a literal now; `blk_find()` is asked.

**And two of the ten injections turned out not to be defects.**
`fat_loaded = 0` in `borrow_core()` and `g_borrow_rw = 0` in
`fat16_return()` are both unreachable-by-construction — `mount_at_part_lba()`
clears the first itself, and every reader of `g_borrow_rw` is gated on
`g_borrowed`, which is only ever set alongside it. Both lines stay in the
source as hygiene and are **not** injected, with the reasoning recorded
in the harness. Deleting working safety code so a harness reports a clean
run is the harness grading the code instead of the other way round.

`tests/test_update.c` grew from 99 checks to 119 for the USB source, and
`tools/check_update_suite.py` from 27 injections to 31 — the four new
ones are the USB probe's own, since the two sources share a reader but
not a probe.

**36 suites, 2478 checks.**

## Also fixed

**`include/fat16.h` declared `fat16_file_size()` and nothing defined
it.** A prototype with no function behind it, in a public header; the
first caller would have failed at link time. It is the `fat16_list()`
mistake from v0.8.6.1 with the sign flipped, and it sat three lines below
that bug's own explanatory note. Removed rather than invented.

**The four `_path` functions are in the header now.** `fat16_read_path()`
and its three relatives were reached through local `extern` lines at the
top of `fs/update.c` — the pattern that same note says `include/instdisk.h`
was created to kill. A second caller arrived and that was one copy of
each signature too many.
