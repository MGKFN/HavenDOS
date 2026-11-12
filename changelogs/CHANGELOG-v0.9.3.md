# HavenDOS v0.9.3 Beta "Gaia": the whole disk, not the first 2047 MB

*12 November 2026. 65 test suites, 6896/6896 checks.*

RM-1003's FAT32 formatter release. It was planned as 0.9.1; the display
series took those numbers, so it is 0.9.3, and the road-to-1.0 releases
after it move up by two.

## Installs over 2047 MB are FAT32

Every install before this one made a FAT16 volume. FAT16 stops at 65524
clusters of 32 KB, which is 2047 MB, so a 512 GB laptop got a 2 GB
HavenDOS.

| partition | filesystem | MBR type (whole disk) |
|---|---|---|
| up to 2047 MB | FAT16, exactly as before | 0x0E |
| over 2047 MB | **FAT32** | 0x0C |
| largest | **32 GB** | |

The rest of a larger gap or disk is left free. The 32 GB cap has three
reasons:
- It's the largest FAT32 volume Windows' own format makes.
- The formatter writes both FATs a sector at a time. At 32 GB that is
  32 thousand writes, which is fast. At 512 GB it would be half a million.
- The free space is yours.

How it works:
- **The size rule is in one place.** `fs/instsize.c`'s `instadd_fs_for()`
  and `havendos_wholedisk_sectors()` are what both installers, both
  installer screens and the tests ask.
- **The FAT32 volume is made and filled by the same code that reads it.**
  `fs/fat32fmt.c`, which has made ESPs since 0.8.8.8, formats it.
  `fs/fat32.c`, the driver the installed system mounts it with, writes
  `/boot/grub/grub.cfg`, `/boot/havendos.elf` and the marker. The
  installer doesn't get a third FAT writer.
- **Writes can't leave the volume.** Under the driver, `v32_wr()` refuses
  any sector outside the volume being installed.
- **The preview says which filesystem you'll get:** "formatted FAT32,
  labelled HAVENDOS", and the whole-disk layout screen gives the type and
  size.

## The installed system finds and keeps its FAT32 volume

**Finding it.** `fat32_init()` read the MBR and nothing else, so a FAT32
volume on a GPT disk could never be found. It now looks for:
- on GPT, the HavenDOS type GUID, with the ESP skipped by name;
- on MBR, a volume labelled HAVENDOS first, then any FAT32 volume as
  before, so no disk that mounted yesterday stops mounting.

**Keeping it.** `fs/fat32.c` holds one volume at a time. The ESP update
source, the `fat32` command, the installer and the uninstaller all borrow
that slot, and until now that didn't matter because the system volume was
never FAT32. Now it can be, so the system volume is **parked, not
dropped**:
- `fat32_mount_dev()` puts it aside whole.
- `fat32_unmount()` brings it back, and never unmounts it.
- A failed borrow leaves it mounted.
- `fat32_sys_begin()` and `fat32_sys_end()` give it back for one
  operation while a borrow is still open.

Every VFS route to FAT32 takes that turn. So a settings file read during
an update comes from the system volume, not the stick.

**Things that assumed FAT16, fixed:**
- **`kernel/main.c` sent the machine to the installer whenever
  `!vfs_using_fat16()`.** Every FAT32 install would have booted, mounted
  its own volume, and then come up as the installer, with "reinstall" one
  key away.
- **`update` and `rollback`** go through a new `fs/sysvol.c`. It keeps
  fs/fat16.c's return conventions and takes the system volume's turn
  around each call. Before, `update` told a FAT32 install that it wasn't a
  disk install.
- **`vfs_rename()` returned 0 for FAT32.** `fat32_rename()` now moves the
  directory entry. It refuses an existing target, the root, `.` and `..`,
  and a directory moved to another parent.
- **`disk` printed "ATA FAT12 : present" on a FAT32 system.** It now names
  the bus, and reads the system volume even while `fat32` holds a stick.
- The notification centre, Notepad's RAMFS warning, `sysinfo` and the
  installers' layout screens no longer say FAT16 when it isn't.

## Bugs found on the way

- **FAT16 dotfiles had illegal names.** `.shell_history` was written as a
  blank name with extension SHE, `.accounts` as blank-dot-ACC.
  `fsck.fat`, and Windows' `chkdsk`, rename those entries to
  FSCK0000.000. `.bpkg_db` and `.bpkg` were the same entry. A leading dot
  is now an underscore (`_SHELL_H`, `_ACCOUNT`). A file saved the old way
  is still found and renamed in place the first time it is used, so
  nothing saved before 0.9.3 is lost. Found by
  `harness/esp_update.py` running `fsck.fat` on an installed volume.
- **`fat32_format()` read past the end of its label.** It took 11 bytes
  whatever the string's length, so "HAVENBOOT" got two bytes of whatever
  came next in .rodata. They happened to be zero.
- **The first `fs/sysvol.c` refused a read shorter than the file.**
  `update`'s verify reads four bytes (the ELF magic) of the kernel it has
  just written, so every update on FAT32 failed its own verify and rolled
  back. Caught by the new QEMU run below before it shipped.
  `fat16_read_path()` truncates, so `sysvol_read()` does too.
- **`harness/esp_update.py` could never have passed.** It never drove the
  installer: it printed that "uefi_dualboot.py does that" and booted an
  empty disk. It now installs, stages a kernel on the ESP, runs `update`
  from the ISO's update entry, and checks the result with mtools and
  `fsck.fat`.
- **Five of `check_installadd_suite.py`'s injections had stopped matching
  the source**, one of them since 0.8.8.8. All rewritten. One of the
  rewrites showed the end-of-install read-back was untested, so the suite
  now has a disk that drops the boot sector and says it didn't.

## Tests

| suite | before | now |
|---|---|---|
| `test_installadd` | 151 | **180**: a real 3 GB FAT32 install, fsck.fat and mtools on the volume, every sector outside it compared, read back through fs/fat32.c, and the write bound asked at both edges |
| `test_wholedisk_size` | 76 | **124**: tests the installer's own functions, not a copy, each size against the formatter of its type |
| `test_fat32sys` | new | **112**: two real mkfs.fat images as system and stick, parked and borrowed. `fs/sysvol.c` writes a kernel to the system while the stick is mounted. `fat32_rename`. `fat32_init` on sgdisk-made GPT disks and hand-made MBRs, never the ESP |
| `test_fat16gpt` | 44 | **56**: dotfile names, and the in-place rename of an old one |

**Injection harnesses:**
- `check_fat32sys_suite.py` is new and catches 18 of 18.
- `check_installadd_suite.py` catches 30 of 30, including 6 new FAT32
  defects.

**QEMU:**
- `uefi_dualboot.py --gb 12`, `--gb 8` (FAT32) and `--gb 7` (FAT16):
  install beside Windows under OVMF, at most four sectors changed outside
  the partition, then boot the disk with no ISO. It now waits for the
  first-boot wizard, which only an installed system runs.
- `wholedisk_fat32.py` (new): a blank 6 GB disk under SeaBIOS gets a
  6143 MB FAT32 partition, type 0x0C, which GRUB boots to the wizard.
  `--gb 1` does the same for FAT16.
- `esp_update.py --gb 12` (FAT32) and the default (FAT16): `update`
  writes the staged kernel byte-for-byte, keeps `.bak`, and both volumes
  pass `fsck.fat`.

## Not in this release

- **No firmware boot entry.** Kyan's HP already adds one for HavenDOS by
  itself, so that part of RM-1003's Prometheus is dropped.
- **A system larger than 32 GB is not made**, by design.
- **FAT16 subdirectories** still write a dotfile the old way, if one is
  ever put there. Every dotfile the system keeps is in the root.

## What it needs from the HP

Nothing new to photograph. The next install onto the HP's free space
will be FAT32; `disk` will say so.
