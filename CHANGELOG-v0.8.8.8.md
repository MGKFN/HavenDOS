# HavenDOS v0.8.8.8 "Helios beta 9" — its own boot partition, and a clock that was twice as fast as it said

Channel Beta, codename "Helios", **beta 9**. 6 November 2026.

51 suites, **4952/4952** (up from 49 and 4786).

---

## 1. HavenDOS can install on a disk with no other operating system

Until this release the install needed an EFI System Partition that
already existed. `esp_find()` returned `ESP_E_NOESP` and the install
stopped — so in practice **HavenDOS required Windows to have been
there first**, and the boot entry it created lived in Windows' ESP.

Asked for directly: *"without anything on disk detect an unused
partition and use that as free space and detect windows or linux and
then make its own grub or UEFI bios boot entry without needing other
OS like BCDEdit stuff"*.

### What is new

**`fs/fat32fmt.c`** — a FAT32 formatter. There was only a FAT16 one,
and the UEFI specification's filesystem for a partition on a fixed
disk is FAT32. The geometry is computed and iterated to a fixed
point, for the reason `include/fat16fmt.h` has carried since v0.8.6:
a FAT sized by anything other than the cluster count it has to
address is a volume that works until something allocates far enough
into it.

**`fs/espmake.c`** — surveys the disk and creates the partition.
The survey reads and nothing else: GPT or not, ESP or not, Windows
(basic data / reserved / recovery type GUIDs), Linux (filesystem /
swap / LVM), free gaps, and partitions whose type we do not
recognise.

**`fs/install.c`** — reserves 100 MB at the *front* of the gap it was
going to give HavenDOS, creates the ESP there, and puts HavenDOS
after it. Front rather than back, because the back of a gap is where
the next partition starts and a boot partition that has to move when
a gap is extended is one that will one day be moved by somebody who
does not know what it is.

### What it deliberately does NOT do

* **It never takes an existing partition.** "Detect an unused
  partition and use that as free space" is half-implemented on
  purpose: a partition with a type we do not recognise *and* no
  filesystem signature in its first sector is **reported** as a
  candidate. It is never consumed. "We did not recognise it" and "it
  is empty" are not the same fact, and the difference is somebody's
  data. Free space is taken, because free space belongs to nobody.

* **It does not create a named "HavenDOS" entry in your firmware's
  boot menu.** The entry it makes is `\EFI\BOOT\BOOTX64.EFI` — the
  removable-media fallback path every UEFI firmware boots when no
  NVRAM entry claims the machine. That needs no Windows, no bcdedit
  and no efibootmgr, which is the ask. A *named* NVRAM entry needs
  `SetVariable()`, which is a UEFI **runtime service**, and a
  multiboot 1 kernel never receives the EFI system table — GRUB has
  already called `ExitBootServices` before HavenDOS's first
  instruction. Adding one means moving to the multiboot 2 EFI handoff
  and calling `ExitBootServices` ourselves. That is a boot-path
  rewrite, not a feature, and it is written down in
  `include/espmake.h` so nobody spends a day looking for the call.

### How it is tested

Not by our own code agreeing with itself:

* **`test_fat32fmt`, 94 checks.** Twelve partition sizes formatted for
  real and handed to **`fsck.fat`**; then **mtools** mounts the
  volume, creates `/EFI/BOOT`, writes an 879 KB loader into it and
  reads it back byte for byte — which is every directory and
  allocation operation `fs/esp.c` will perform. Six write-failure
  injections, each caught.
* **`test_espmake`, 50 checks.** Every test disk is laid out by
  **`sgdisk`** and read back by `sgdisk` afterwards. Windows-shaped,
  Linux-with-no-ESP, freshly initialised, completely full, and one
  with an unknown empty partition on it.
* **`test_installadd`, 86 → 108 checks.** The wiring, end to end, on
  a disk with no ESP: `fs/fat32fmt.c` makes the volume, `fs/fat32.c`
  mounts it, `fs/esp.c` writes the loader into it, and `fsck.fat` and
  `mdir` both confirm the result. Three files that had never met.

**THE BLAST RADIUS IS MEASURED, NOT TRUSTED.** `test_espmake` snapshots
the entire 400 MB disk image before and after, compares every sector,
and requires that the only ones that changed are the four GPT sectors
and the inside of the partition just created. The protective MBR and
the neighbouring Linux partition are asserted byte-identical.

### Three defects the suites found before any disk did

1. **A FAT two entries too small at 64 MB.** The fixed-point loop
   converged, and converging is not the same as being big enough —
   the value it settled on was computed from the previous round's
   cluster count. `fsck.fat`: *"Filesystem has 129024 clusters but
   only space for 129022 FAT entries."* The same defect
   `fat16fmt.h` documents, reached by a different route.
2. **A freshly initialised disk was refused.** `gpt_scan()` returns a
   partition count, and `0` means both "not GPT" and "GPT with
   nothing on it". The survey treated zero as not-GPT — refusing
   precisely the blank disk this whole feature was asked for.
   `gpt_present()` is the question that distinguishes them.
3. **Two statics that would have become one variable.**
   `tools/check_statics.py` refused the build: `fs/espmake.c` and
   `fs/install.c` both had `static int s_disk`, and
   `tests/test_installadd.c` compiles them into one translation unit,
   where C merges tentative definitions of the same object with no
   error at any `-W` level. The installer would have been writing the
   ESP maker's private state.

Two injections were also **wrong rather than the code**, and are
recorded in the suites instead of deleted: `fsck.fat` repairs a
damaged boot sector from the backup at sector 6 rather than failing,
and it does not police the `0xAA55` trailer at all.

---

## 2. THE PIT WAS COUNTING AT TWICE REAL TIME

`kernel/pit.c` programs channel 0 with `outb(0x43, 0x36)` — **mode 3,
square wave** — where the 8254 decrements the counter by **two** on
every input clock so the output is high for half the period and low
for half. `pit_us()` converted a counter delta of N into N input
clocks' worth of microseconds.

So it has reported **twice the elapsed time** since it was written,
and everything downstream inherited it:

| call | asked for | actually got |
|---|---|---|
| `sleep_ms(100)` | 100 ms | ~52 ms |
| `get_ticks()` | 100 Hz | ~200 Hz |
| `boot_ms()` | ms since boot | roughly double, so every `dmesg` timestamp is inflated |

— and with them every network timeout, every TCP retransmission
timer, every DNS cache TTL, every animation delay and the screensaver
countdown: all half the duration their call site asks for.

### Found by measuring a screen

The early-access notice below waits fifty slices of `sleep_ms(100)`
and should hold the screen for five seconds. Screenshotting the boot
and timestamping each capture with the host's own clock showed it up
for **2.6 seconds** — and the notice's own on-screen slice counter
read `s49`, so the loop was completing all fifty. Fifty
hundred-millisecond sleeps in 2.6 seconds is the entire finding.

The fix is one constant: `54927` (one input clock in 16.16 µs) becomes
`27464` (half of one). Mode 3 is left alone, because the mode decides
the IRQ0 waveform and this is arithmetic, not hardware. Measured
again afterwards: **5.12 seconds for a requested 5.0.**

Three measurements of mine were wrong on the way, and the way they
were wrong is worth keeping: a screenshot loop whose sampling
interval I assumed was 0.18 s was really ~0.5 s, because each capture
cost a monitor round trip. It produced *the same number for four
completely different implementations*, which is what finally gave it
away — four rewrites cannot agree to three significant figures. The
later runs timestamp every capture with the host clock instead of
trusting the schedule.

No suite caught this, and none would have: every timing test in the
tree measures the PIT against itself.

---

## 3. The early-access notice

Asked for: *"the beta phases shows warning that this is early access
and beta software"*.

A full-screen notice after the boot splash and before anything that
looks like a finished operating system — because once the desktop is
up HavenDOS looks considerably more finished than it is, and the
person in front of it may not be the person who built it. Five
seconds, any key skips it, and it is on the boot splash as well.

One switch controls all of it: `HAVENDOS_PRERELEASE` in
`include/version.h`, next to the channel it follows. Set it to 0 on
the day there is a 1.0 and every warning goes quiet.

It is positioned from `vga_cols()`/`vga_rows()`, not 80x25.
v0.8.8.7 spent a release finding every literal that assumed that
grid.

It also **redraws itself every slice**. Drawn once and waited on, it
was being painted over from inside the wait — `sleep_ms()` pushes the
shadow buffer and `kernel_poll()` services every driver in the tree,
any of which may log. Four hundred cells per slice on a screen nobody
is using yet is cheaper than finding out which, and it cannot regress
when a future driver starts logging there.

---

## Also in this release

* `fs/install.c`'s preview now shows the boot partition it is about
  to create, with its size, before anything is written. A person
  approving a partition-table change should see both partitions.
* `INSTADD_E_ESPMAKE`, distinct from `INSTADD_E_ESP`: "there is no
  ESP and one could not be made" and "there is an ESP and the install
  into it was refused" are different problems with different answers.

## Still not done, and why

* **The trackpad.** Unchanged and not close. It needs two things this
  kernel does not have: PCI resource assignment, because the firmware
  left both Serial-IO controllers with no memory window at all, and
  an ACPI interpreter, because an I2C-HID device's descriptor address
  comes from an ACPI `_DSM` method and there is nowhere else to get
  it. Brute-forcing the I2C address space is possible and is not a
  release's work.
* **A USB mouse** is already implemented — `usb/hid.c`, boot protocol
  — and has never been confirmed on hardware. It is a test, not a
  task.
* **An external monitor** needs an Intel Xe display driver driving a
  second pipe and encoder. `claude/havendos-xe-graphics-and-64bit.md`
  explains why that is a different project.
* **Pairing stage 3** is where v0.8.8.7 left it: the bignum,
  Miller-Rabin and RSA keygen are done and measured, `net/der.c` is
  written and tested and still has no caller. X.509 v3 and PKCS#1
  v1.5 are next.
