# HavenDOS v0.8.4 Beta "Hermes" — the update path

`update` can read a new kernel off the **EFI System Partition**. On a
laptop with no optical drive, no USB mass storage in this kernel and no
network adapter anything here drives, that is the difference between
upgrading and reinstalling.

This is the first release of the road to 1.0 as THS-RM-1003 writes it.

## Why this one came first

Every release from here to 1.0 has to reach the test laptop somehow.
`fs/update.c` read the new kernel from an **ATAPI CD-ROM** and from nowhere
else, and that machine has no optical drive — so the only route was a full
reinstall, which means running the additive installer beside a live Windows
partition. **Once per release. Because of where a file was.**

The EFI System Partition was already there, already FAT32, already readable
by `fs/fat32.c`, already findable by partition type GUID, and already
writable from Windows with `mountvol`. Every part of this route existed
before this release did.

## What `update` does now

```
  >> Looking for an update...
  Source  : EFI System Partition
  >> Reading update manifest...
  Running : 0.8.3.1
  Offered : 0.8.4
  Size    : 616 KB   (checksummed)
```

It reads two files, both of which a person puts there from Windows:

```
  \EFI\HAVENDOS\update\update.inf      version, size and CRC-32
  \EFI\HAVENDOS\update\havendos.elf    the kernel it describes
```

`rescue/Stage-HavenDOS-Update.ps1` does it in one command, mounting the ESP
with `mountvol`, writing both files, and reading them back to check them
before it unmounts. **Nothing else on the ESP is touched** — the volume is
mounted through `fat32_vol_mount_ro()`, which hands `fs/fat32.c` a NULL
write function, so the refusal is in the driver rather than in the
discipline of the caller.

## Two implementations from the first day, and one function that chooses

0.8.3.1's post-mortem recorded that three of its six bugs were one mistake:
*the installer has one block layer and the filesystem has another, and only
one of them got taught.* Four screens each worked a bus label out
independently and only one knew about NVMe, so the picker said `NVMe` and
the screen gated behind typing YES said `VirtIO`.

So `include/update_src.h` is an interface with **two** implementations from
the day it was written — the ESP and the CD-ROM — and exactly two functions
are allowed to have an opinion about which is in use:

- `upd_source_select()` decides. Nothing else in the tree chooses a source.
- `upd_source_label()` names it. Every screen asks this one.

An abstraction with a single implementation is a wrapper and proves
nothing. With two, the compiler keeps them honest.

## A checksum, verified before the first byte is written

The ATAPI path checked four bytes of ELF magic and trusted the rest,
because an ISO is a single object that was either read or not. **A file
copied onto an EFI System Partition by hand from Windows is not that.** It
can be truncated by a full volume, half-written by a stick pulled out
early, or simply be the wrong file. A kernel that is a valid ELF and the
wrong bytes passes every check this command had before 0.8.4, and does not
boot.

The manifest carries the length and a CRC-32, and both are checked against
the staged image **before `/boot/havendos.elf` is opened for writing**.

`gpt_crc32()` — the CRC the GPT header already uses, reflected
`0xEDB88320` — is the same CRC `python3`'s `zlib` computes, so the number
in a manifest can be checked by anybody with no HavenDOS source at all. A
checksum only our own code could compute would be our code agreeing with
itself.

**ISOs get one too.** `tools/make_iso_manifest.py` writes `/update.inf`
onto the ISO beside the kernel, so an update applied from a disc is checked
as carefully as one applied from the ESP. An ISO built before 0.8.4 has
none, and a manifest is synthesised from `/version.txt` and the directory
entry instead — with `have_crc` left at zero, and the screen saying
`(no checksum here)` rather than implying one.

## A second install no longer needs a manual `.HDB` dance

`esp_install()` stopped with `ESP_E_BACKUP` whenever
`\EFI\BOOT\BOOTX64.HDB` already existed. The rule behind it — never
overwrite a backup — is right once and wrong every time after that.

On a **first** install it is exactly right: `BOOTX64.HDB` holds whatever the
machine booted before HavenDOS existed, and on the test laptop that is
Windows' own fallback loader. On a **second** install the same rule does
the opposite of what it is for. `BOOTX64.EFI` is now *our* loader, because
we wrote it last time, and backing it up would write our loader over
`BOOTX64.HDB` and destroy the only copy of the one that matters.

So the question asked is no longer "is a backup name taken" but **"is the
file at the fallback path one we put there"**, and it is answered by
comparing bytes:

- it is byte-for-byte the loader *this* kernel carries, or
- it is byte-for-byte `\EFI\HAVENDOS\grubx64.efi`, which is where a previous
  HavenDOS put its own copy — and that covers the upgrade case, where the
  loader embedded in the new release differs from the old one's.

Anything else at that path belongs to somebody else, and if the backup name
is also taken it is **still refused**, exactly as before. `tests/test_esp.c`
proves both halves.

## Also fixed

- **A failed download no longer writes to the boot partition.** Every
  failure in `cmd_update()` ran the rollback, including the ones where
  nothing had been overwritten — so a CD read error wrote the backup back
  over a kernel nothing had touched. Rollback now runs only once something
  really has been replaced, and the screens on the other paths say *"Nothing
  has been written. The running kernel is untouched."*
- **A disc whose kernel is zero bytes is no longer offered as a source.**
  Found by the new suite: `probe()` said yes, the manifest step then
  refused it, and the selector had already committed to a source it could
  not serve.
- **The source archive could not build an ISO in an empty directory.** It
  ships no `isodir/`, so `make` died at
  `FileNotFoundError: 'isodir/version.txt'` after the kernel had compiled
  and every check had passed. `tools/make_isodir.py` creates only what is
  missing and **never touches an `isodir/` that is already there**. See the
  caution at the end of this file.

## Found while writing the release notes: `update` could not carry the kernel

`fs/update.c` stages the whole new kernel in one buffer before it writes a
byte of it, and that buffer was **2 MB**. The kernel is **2.80 MB**.

So `update` would have refused the new kernel from either source, with
*"Kernel too large for update buffer (>2MB)"* — a sentence that reads like
a corrupt ISO rather than like a kernel that quietly got bigger. **This is
not new in 0.8.4.** The limit was crossed by ordinary growth some releases
ago — AHCI, NVMe, two embedded GRUB EFI images — with no single commit to
blame, and nothing noticed because nothing had successfully run `update`
since.

Found by extracting `/boot/havendos.elf` out of the finished ISO to check
what a person would actually be staging, which is not a thing any suite
does.

- `MAX_ELF_SIZE` is **8 MB**, chosen to outlast several releases rather
  than to fit today. It costs nothing at boot: the buffer comes from the
  physical allocator only while `update` or `rollback` runs.
- The refusal now names **both** numbers — what was offered and what the
  buffer holds. The 0.8.3.1 lesson about E15 was that a refusal naming a
  limit without naming the measurement sends the reader to check the wrong
  thing.
- **`tools/check_update_size.py` is a sixth build check.** It refuses any
  build whose kernel will not fit the update buffer, and warns under 1 MB
  of headroom. A limit that only fails at the moment somebody upgrades a
  real machine is exactly the class of bug this tree makes the build
  refuse. Verified to fail: pointed at a 1 MB cap it exits 1.

Two sizes are involved and they are not the same file:

| file | size | what it is |
|---|---|---|
| `havendos_pass1.elf` | 1.32 MB | what the **installer** writes, carried inside the final kernel as `kernel_elf_data[]` |
| `havendos.elf` | 2.80 MB | what is on the ISO, and what **`update`** copies |

## Testing

**1768 checks across 28 suites**, up from 1657 across 26. Two injection
harnesses were run and both found holes, which is the only reason to
believe either suite.

`tests/test_update.c` is new: **99 checks** over the manifest parser, the
ESP source against a FAT32 volume `mkfs.vfat` formatted and `mtools`
populated, the CD-ROM source against a simulated disc that executes its
reads, and the source selection itself. It asserts that `gpt_crc32()`
agrees with `python3`'s `zlib.crc32` on the real staged image.

`tools/check_update_suite.py` injects **27 real update-path bugs** — the
magic unchecked, the format number unchecked, a size allowed to overflow, a
`kernel=` allowed to contain `..\Microsoft\Boot\bootmgfw.efi`, a synthesised
manifest claiming a checksum it does not have, the disc preferred over the
ESP, both sources named the same thing on screen. **26 were caught on the
first run and one was missed**, and the one missed is worth writing down:

> Removing the byte-by-byte comparison of the magic line was MISSED,
> because every magic test in the suite happened to fail for a *different*
> reason — the string was too short, or had no format number after it. The
> case that mattered was a first line of exactly the right **shape** and
> the wrong text, which is what a manifest for some other program looks
> like. Three checks were added and it is caught.

`tools/check_esp_suite.py` gained **5 injections** for the reinstall
decision, and **two of them were missed**: a comparison that looks at the
file's *length* only. The suite was planting a stranger's loader of 8 KB
against our 811 KB one, so a length comparison alone still told them apart.
The stranger is now exactly our loader's size and none of the same bytes,
and both are caught. **28 of 28.**

| suite | checks |
|---|---|
| test_update (new) | 99 |
| test_esp | 114 (was 102) |
| test_fat32 | 227 |
| …26 others | 1328 |
| **total** | **1768** |

## Known limitations

- **The end-to-end ESP update has not been proved on hardware, or in a
  complete QEMU run.** `harness/esp_update.py` is written and is in the
  archive, but see the caution below: it is driven blind against a boot
  menu this container had to reconstruct, and the screen timing no longer
  lines up with it. What IS proved is that the 0.8.4 ISO boots under OVMF,
  GRUB loads the kernel, and the installer reaches its preview screen with
  the four-sector guarantee rendered correctly.
- **AHCI has still never run on real silicon**, and now cannot be: there is
  no SATA machine available to this project. THS-RM-1003 states this in the
  1.0 claims rather than implying coverage that does not exist.
- **The idle panic is still unexplained** and still unreproduced.
- **No firmware boot entry.** `update` fixes getting a new version onto the
  machine; it does not fix getting the machine to offer HavenDOS in its
  boot menu. That is 0.9.2 "Prometheus".
- **An installed system still cannot install HavenDOS**, and partitions are
  still capped at 2047 MB. 0.9.2 and 0.9.1.

## CAUTION — the ISO in this release was built with a reconstructed boot menu

The v0.8.3.1 source archive contains no `isodir/`, and
`isodir/boot/grub/grub.cfg` is a **source file**, not a generated one. It
holds the ISO's boot menu, its video setup and its timeout, and three test
harnesses count on the order of its entries.

`tools/make_isodir.py` reconstructs one from the only two things that
actually define it — `kernel/main.c`'s own command-line parser
(`mode=shell`, `mode=safe`, `install=1`, `gfx=text`, `live`) and the entry
indices written down in `harness/fat32_boot.py` and
`harness/uefi_dualboot.py`. It boots, and the installer runs from it.

**It is not your file.** Two things in it were found by booting rather than
by knowing, and your real one certainly differs in more:

- with no `insmod all_video` and no `gfxmode`, GRUB stops at
  `error: no suitable video mode found.`
- with `set timeout=10`, GRUB counts down and boots the default before the
  harnesses — which sleep 12 and 26 seconds through OVMF's slow start —
  have touched the menu.

**Put your own `isodir/` back before shipping this ISO to anyone**, or send
it over and the harness run can be repeated against the real menu.
`make_isodir.py` will not overwrite it.
