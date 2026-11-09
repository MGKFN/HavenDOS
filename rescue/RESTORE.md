# Putting it back

This is the other half of `Backup-HavenDOS-Rescue.ps1`. Read it once
*before* you need it, so that if you ever do, you are not reading it for
the first time with a laptop that will not boot.

## What this restores

Everything HavenDOS's installer is capable of changing:

| file | what it is | where it goes |
|---|---|---|
| `gpt-primary.bin` | protective MBR, GPT header, entry array | sector 0, 34 sectors |
| `gpt-backup.bin` | backup GPT header and entry array | the last 33 sectors |
| `esp.img` | the whole EFI System Partition | at its own offset |

`manifest.json` holds the numbers: total sectors, the ESP's start LBA, the
disk's serial number, and a sha256 of each file.

**This was tested, not assumed.** Both GPT headers were zeroed and 100 KB
of random bytes written over an ESP; after the commands below the entire
4 GB disk was byte-identical to before, `sgdisk -v` was clean, `fsck.vfat`
was clean, and `bootmgfw.efi` matched byte for byte.

## What it does not restore

The Windows partition itself. Nothing in the installer writes there — that
is checked byte for byte by its test suite and by an end-to-end run under
UEFI firmware — but "nothing writes there" is a claim about code, and a
full disk image is a fact about your disk. If you have an external drive
big enough, a full image is better than this. This exists because most
people do not.

## Before you start

You need a Linux USB. Ubuntu, Mint, anything live. Boot it, choose "Try"
rather than "Install", and open a terminal.

Have the `HavenDOS-Rescue` folder on a second USB stick.

## 1. Find the disk

```sh
lsblk -o NAME,SIZE,MODEL,SERIAL
```

Match the size and serial against `manifest.json`. On a laptop with one
NVMe it is almost always `/dev/nvme0n1`. **Check the serial.** Writing
these files to the wrong disk is the one way this procedure can do harm.

```sh
DISK=/dev/nvme0n1          # <-- set this, carefully
RESCUE=/media/$USER/<your-usb>/HavenDOS-Rescue
```

## 2. Check you have the right disk and the right files

```sh
sudo blockdev --getsz $DISK                 # must equal totalSectors in manifest.json
sha256sum $RESCUE/gpt-primary.bin $RESCUE/gpt-backup.bin $RESCUE/esp.img
```

Compare those against `manifest.json`. If the sector count does not match,
**stop** — it is not the disk the backup came from.

## 3. Restore

Three commands. `conv=notrunc` matters: without it `dd` truncates the
device node's view and you lose everything after what you wrote.

```sh
SEC=$(sudo blockdev --getsz $DISK)
ESP=<espStartLba from manifest.json>

sudo dd if=$RESCUE/gpt-primary.bin of=$DISK bs=512 seek=0            conv=notrunc
sudo dd if=$RESCUE/gpt-backup.bin  of=$DISK bs=512 seek=$((SEC-33))  conv=notrunc
sudo dd if=$RESCUE/esp.img         of=$DISK bs=512 seek=$ESP         conv=notrunc
sync
```

## 4. Check it

```sh
sudo sgdisk -v $DISK          # "No problems found"
sudo sgdisk -p $DISK          # your partitions, as they were
sudo partprobe $DISK
sudo fsck.vfat -n ${DISK}p1   # the ESP, clean
```

Reboot without the USB. Windows Boot Manager should be back.

## If only the partition table is damaged

GPT keeps two copies, and if one survives you may not need the backup at
all:

```sh
sudo sgdisk --verify $DISK
sudo gdisk $DISK
# then:  r  (recovery menu)
#        b  (rebuild the primary from the backup)
#   or   d  (rebuild the backup from the primary)
#        w  (write)
```

Try that first. It uses the disk's own second copy, which is newer than
anything you backed up.

## If Windows boots but its boot entry is gone

The firmware forgot where `bootmgfw.efi` is; the file is still there. From
a Windows recovery USB:

```
bcdboot C:\Windows /s S: /f UEFI
```

where `S:` is the ESP, mounted with `mountvol S: /S`.
