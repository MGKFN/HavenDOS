# HavenDOS v0.8.3 Beta "Lodger"

Installs beside an operating system that is already there, instead of
replacing it. The name is the point: this release moves in as a tenant.

Verified end to end under real UEFI firmware (OVMF): a disk shaped like a
Windows laptop goes in, HavenDOS is installed into its free space, and the
machine boots a menu offering both. Every sector outside the new partition
is byte-identical afterwards, proved by checksum and confirmed by sgdisk,
parted, fsck.vfat and mtools.

## The guarantee, in the terms it is made

Outside the new partition, **four sectors** are written: the two GPT
headers and one sector of each entry array. Their absolute LBAs are shown
on screen before anything is written. Inside the new partition, everything
is written — it is free space. On the EFI System Partition, files are
added; nothing is deleted, and nothing is overwritten without a verified
copy being taken first.

`fs/gpt_write.c` has no function that rewrites a whole table. 31 of the 32
entry sectors are never written by any code path, because none exists.

## New

- **`fs/gpt_write.c`** — additive GPT writer. Claims a free entry slot,
  leaves every other entry untouched, writes the backup copy before the
  primary so a power cut leaves the firmware-read copy wholly old or
  wholly new.
- **`fs/esp.c`** — writes HavenDOS' loader and menu onto the existing EFI
  System Partition beside Windows'. Free space counted before the first
  byte; every file read back and compared; an existing `BOOTX64.EFI`
  copied aside and the copy verified before anything replaces it.
- **`fs/fat16fmt.c`** — FAT16 geometry computed from the partition size.
- **`fs/install.c: havendos_install_additive()`** — the install, with a
  dry run that shares its code path exactly: the same function, with the
  write function withheld.
- **A UEFI loader to install.** `include/grub_efi_embed.h`, built
  `-O x86_64-efi`. The two existing embeds are both `i386-pc` — BIOS only.
- **The preview screen**, which prints the four sector numbers, the
  partition's bounds, and what will happen to the EFI partition, before
  the confirmation.
- `tools/make_release.sh` — builds the archive, then builds *from* the
  archive and runs the suite there.

## Fixed

- **The installer produced a malformed filesystem on any partition over
  about 64 MB.** `BPB_SECTS_FAT` was fixed at 128 while the BPB carried the
  partition's real size, so a 1 GB partition claimed 524,215 clusters in a
  FAT that holds 32,766. `fsck.vfat`: *"Too many clusters (524215) for
  FAT16 filesystem."* Never noticed because a fresh install touches a few
  hundred clusters at the very start. Shipping since before 0.8.
- **The build produced a kernel with no UEFI loader in it.** `HAVE_GRUB_EFI_EMBED`
  was never defined, so `fs/esp.c` compiled against its zero-byte stub and
  the installer would have written an empty `BOOTX64.EFI` over the real
  one, verified that nothing read back as nothing, and reported success.
  `tools/check_efi_embed.py` now fails the build on it, and `fs/esp.c`
  refuses at runtime anything that is not a PE32+ x86_64 image.
- **The installed system could not find its own partition.** `fs/fat16.c`
  mounted at sector 2048, always. With the partition elsewhere on a GPT
  disk, `fat16_init()` returned 0, and `kernel/main.c` read that as "this
  machine needs installing" — so the installed system booted the
  installer. Found by booting it.
- **The ESP's menu had no kernel command line**, which did the same thing
  for a different reason.
- `esp_install()` took its target disk out of the report struct, so a
  caller that zeroed the struct wrote to disk 0.
- The GPT read callback returns 0 for success and `inst_disk_read` returns
  1; the additive installer passed one through the other.
- `shell/setup.c` wrote 27 characters starting at column 55 of an
  80-column screen. `tools/check_screen_width.py` now fails the build on
  any string literal placed past the edge.

## Testing

**1409 checks across 22 suites**, and six injection harnesses that put
real bugs into the code and require the suites to notice each one:

| harness | injections | caught |
|---|---|---|
| `check_fat32_suite.py` | 26 | 26 |
| `check_gptwrite_suite.py` | 26 | 26 |
| `check_esp_suite.py` | 23 | 23 |
| `check_fat16fmt_suite.py` | 21 | 21 |
| `check_installadd_suite.py` | 21 | 21 |
| `check_fat16gpt_suite.py` | 8 | 8 |

Every one of those suites passed on its first run and every one had holes:
the ESP suite missed 8 of 16, the formatter 6 of 21, the installer 9 of
21. A suite that cannot fail proves nothing.

`harness/uefi_dualboot.py` is the end-to-end test: build a Windows-shaped
GPT disk, boot the ISO under OVMF, drive the installer, then boot the disk
with no removable media attached.

## Known limitations

- **NVMe is still read-only.** A machine whose disk is NVMe can run the
  preview and see exactly what would happen; it cannot install. That is
  `nvme_write()`, next release.
- **AHCI is not supported.** On a machine whose SATA controller is in AHCI
  mode — QEMU's q35, and most firmware's default — no disk is found. Also
  next, after NVMe.
- **No firmware boot entry is created.** HavenDOS writes
  `\EFI\BOOT\BOOTX64.EFI`, the removable-media fallback path. Firmware
  meeting the disk for the first time finds it; firmware with an existing
  boot order may not look. Use the one-time boot menu and pick the generic
  UEFI entry. Writing an NVRAM entry needs UEFI runtime services.
- **An installed system still cannot install HavenDOS** — the two-pass
  build means its kernel carries no installable image.
- Partitions over about 2 GB are refused: FAT16 with 32 KB clusters stops
  there, and there is no FAT32 formatter yet.
