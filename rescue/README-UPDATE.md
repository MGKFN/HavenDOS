# HavenDOS 0.8.5.6 — update payload

```
havendos.elf   2990080 bytes   crc32=1dc1850f   version 0.8.5.6
update.inf     the manifest that describes exactly those bytes
```

The two belong together. Both stagers check the manifest's `size=`
**and** `crc32=` against the kernel you hand them and refuse if either
disagrees, so do not mix this `update.inf` with a kernel from another
build.

Check it yourself without any HavenDOS source — the CRC is zlib's,
which is the same reflected-0xEDB88320 CRC-32 that `gpt_crc32()`
computes:

```sh
python3 -c "import zlib;print('%08x' % (zlib.crc32(open('havendos.elf','rb').read()) & 0xffffffff))"
# 1dc1850f
```

## Windows — one step

Put all these files in one folder and **double-click
`Update-HavenDOS.cmd`**, then approve the Administrator prompt.

It finds `havendos.elf` and `update.inf` beside itself, elevates, picks
a free drive letter, mounts the EFI System Partition, writes the two
files, unmounts, and tells you what to do next.

By hand, if you prefer:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\Stage-HavenDOS-Update.ps1 -Kernel .\havendos.elf -Manifest .\update.inf
```

## Linux — new in 0.8.5.6

`Stage-HavenDOS-Update.ps1` is PowerShell and `mountvol`, so it only
ever helped a machine dual-booting Windows. For the laptop beside Linux
Mint:

```sh
sudo ./stage-havendos-update.sh
```

It finds the files beside itself, finds the ESP by its GPT partition
type GUID (not by mountpoint or label), uses an existing mount if there
is one, and leaves it mounted if it did not mount it.

Useful flags:

```sh
./stage-havendos-update.sh -n                 # dry run: check everything, write nothing
sudo ./stage-havendos-update.sh -d /dev/nvme0n1p1   # name the ESP yourself
sudo ./stage-havendos-update.sh -k ./havendos.elf -m ./update.inf
```

It verifies the pair **before** mounting anything, and after writing it
**reads both files back and re-checks the CRC** before reporting
success.

## Either way, both write exactly this

```
/EFI/HAVENDOS/update/havendos.elf
/EFI/HAVENDOS/update/update.inf
```

Nothing else on the ESP is touched — not `/EFI/Microsoft`, not
`/EFI/BOOT`, not `/EFI/ubuntu`, not the firmware boot entries.

## Then, in HavenDOS

```
update
```

Use a boot entry **without** `live` — on the ISO that is entry 4,
"Update installed system from the ESP". `update` begins by requiring a
mounted HavenDOS partition -- FAT16, or since 0.9.3 FAT32 -- because it
writes the new kernel to the partition already on the disk, and a live
shell cannot see that partition. It refuses with "No HavenDOS partition
is mounted" if you boot a live entry.

`ver_cmp()` from `fs/update.c`, against this release:

| installed | staged | result |
|---|---|---|
| 0.8.5.5 | 0.8.5.6 | upgrade |
| 0.8.5 | 0.8.5.6 | upgrade |
| 0.8.5.6 | 0.8.5.6 | already installed, refused |
| 0.8.6 | 0.8.5.6 | older, refused |

The checksum is verified **before** anything is written, and nothing is
rolled back when nothing was written.

## What is in 0.8.5.6

`CHANGELOG-v0.8.5.6.md` has it in full. The short version: `make test`
could not fail and now can, 621 of 1995 checks were silently not
running, an ISO with no BIOS boot image passed every check, the
Settings theme picker committed your preview when you pressed ESC, the
first-run theme preview looked identical for all ten themes, the
v0.8.5.3 keyboard delay fix had reached one of three callers, and the
mouse could not reach past column 79 on a larger console.

## Two caveats, stated plainly

- `Update-HavenDOS.cmd` and the mount-and-copy half of
  `stage-havendos-update.sh` have **not been run** where they were
  written — there is no Windows in that container, and no vfat support
  either. Their structure is checked and every refusal path in the
  shell script is tested (six for six), but the PowerShell script is
  the one with real mileage on it.
- `poweroff` works on the HP, and probably by luck: `0x604` is tried
  first, so that firmware most likely set PMBASE to `0x600`, making
  QEMU's hardcoded address accidentally correct. It may well not work
  on another machine. The next release adds a read-only ACPI reporter
  to turn that guess into a number.
