# HavenDOS v0.9.4 Beta "Prometheus": an installed system installs itself

*12 November 2026. 65 test suites, 6916/6916 checks.*

RM-1003 gave Prometheus two jobs: a UEFI boot entry, and "a system that
can install itself". **Kyan's HP already registers HavenDOS with its
firmware by itself**, so the first job is dropped. This release is the
second.

## `install` works on an installed system

The kernel an installed system runs is the pass-1 build, which carries
no image of itself to install. Until now, `install` on an installed
system said "This copy cannot install HavenDOS".

**An installed system now installs the kernel it booted from:
`/boot/havendos.elf` on its own volume.** That file is exactly what the
ISO's installer wrote there, or what `update` put there since.
- **Two sources, checked in order:** the image this kernel carries (the
  ISO's kernel), then the system volume's `/boot/havendos.elf`.
- **It reads the system volume through `fs/sysvol.c`**, so it works on
  FAT16 and FAT32. It also reaches the system volume while the installer
  has the ESP mounted.
- **The file is read once**, into memory taken from the physical
  allocator, and checked for the ELF magic. It is kept until the installer
  closes, because the drive picker plans every disk with a dry run.
- `havendos_install_image_source()` says which source an install uses.
  The additive report has `image_from_system`.

**It won't install over itself.** The disk the system volume was mounted
from shows **"running from this disk"** in the drive picker, and picking
it is refused:
- additive installs return `INSTADD_E_SELF`, even to preview;
- whole-disk installs return `-16`, before step 1.

The check asks the same question `fat16_init()` and `fat32_init()` ask
(`blk_boot_dev()`), through the installer's disk table
(`inst_disk_is_boot()`).

**The installer, run from an installed system:**
- The welcome screen says it copies this system's own kernel and leaves
  the running disk alone.
- ESC goes back instead of rebooting.
- At the end it says the new disk boots on its own, and returns instead
  of rebooting the running system.

**The desktop's Install app now opens the ISO's installer.** It used to
carry its own whole-disk installer. That one:
- couldn't install beside Windows;
- labelled every non-ATA disk "(VirtIO)";
- could only ever run from the ISO.

## Tests

- **`test_installadd` 180 → 200:**
  - With the embedded image off and a different kernel on the simulated
    system volume, a 3 GB FAT32 install holds that kernel, byte for byte,
    per mtools. `fsck.fat` accepts the volume.
  - The kernel is read once across a preview and an install.
  - The running disk is refused, for preview, install and whole-disk,
    with nothing written.
  - A `/boot/havendos.elf` without ELF magic counts as no image.
  - From live media with no image, `INSTADD_E_NOIMAGE` and nothing
    written.
- **`check_installadd_suite.py` catches 34 of 34,** including four new
  Prometheus defects:
  - the self check removed;
  - a re-read on every call;
  - no ELF check;
  - the embedded image winning when there is none.
- **QEMU, `harness/selfhost.py` (new), 14 of 14:**
  1. A FAT32 system made from the ISO boots its own shell entry, with a
     blank USB stick beside it.
  2. `install` opens the installer, marks the system disk and refuses
     it, and installs FAT32 onto the stick.
  3. The stick's kernel matches the system's byte for byte, per mtools,
     and `fsck.fat` accepts the stick.
  4. The stick, booted alone, reaches the first-boot wizard.

## Also fixed

- **`harness/wholedisk_fat32.py`** sometimes lost the ENTER on the
  installer's welcome screen, which draws before its keyboard loop
  starts. It now repeats ENTER until the drive picker is up, as
  `esp_update.py` does.

## Known limits

- **A second IDE disk is invisible.** `drivers/ata.c` operates only the
  first IDE drive it finds; it logs the others. In practice the copy goes
  to a USB stick, or to an NVMe or AHCI disk, which are enumerated
  properly.
- **Which disk is "the system's" is decided by bus, not by content:**
  VirtIO, then ATA, then AHCI, then NVMe, then USB. On the HP that is the
  NVMe. So a HavenDOS copied onto a stick and booted on the HP will mount
  the NVMe's HavenDOS volume, if there is one, and run the installer if
  there isn't. The stick boots properly as a machine's only disk. Fixing
  this means the kernel learning which volume GRUB booted from, for
  example through a volume serial on the kernel command line. That's a
  later release.
