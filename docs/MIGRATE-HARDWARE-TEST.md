# Moving the HP's install to FAT32 -- `migrate`, step by step

For the HP: HavenDOS on a FAT16 partition, beside Windows, booting under
UEFI. If something goes wrong, take a photo of the screen. Every refusal
says why, and nothing is written until you type the word it asks for.

## 0. Before you start

- **Update the FAT16 install to 0.9.6 first** (`update`, as usual), and
  reboot into it. `migrate` is a 0.9.6 command.
- **Make the space in Windows** (Disk Management):
  - either shrink a volume and leave the space **unallocated**;
  - or make a new partition in it and pick **"Do not format this
    volume"**. Don't give it a drive letter or a filesystem.
  It needs at least 512 MB. 8 GB is plenty.

## 1. Look

In HavenDOS's shell:

    migrate

**Photo this screen.** It shows:
- this system: FAT16, its partition number and size;
- **Places it can go:** your 8 GB partition, marked "empty -- never
  used" (or "old data in it", in yellow, if Windows left bytes there),
  and any free space;
- **Left alone, and why:** Windows' NTFS partition, recovery, and so on.

If your 8 GB partition is under "Left alone", the reason is printed
beside it. Send the photo; don't force it.

## 2. Move

    migrate 1

(use whichever number your partition has). It lists what it will do.
Type **ERASE** for a partition, or **MOVE** for free space, then Enter.
Five lines tick by; formatting 8 GB takes a while on the HP's disk.

**Photo the result.** It should say:
- "Done. HavenDOS is on its new FAT32 partition";
- how many files were copied;
- anything NOT copied, and why;
- the next steps.

## 3. Boot the new one

Reboot. The menu now has:
1. HavenDOS v0.9.6 Beta -- **the new FAT32 install**
2. Windows Boot Manager (still the default)
3. HavenDOS - old FAT16 install (until 'migrate finish')
4. Firmware setup

Pick the **first** one. You should get your normal login, with your own
user and password, not the setup screens. Log in and check your files.
`df` should say **FAT32** and about 8 GB.

If it doesn't boot, or asks you to set up from scratch: reboot, pick
entry 3 (the old install is untouched), and send a photo.

## 4. Remove the old one

Only when you're happy, from the **new** install:

    migrate finish

Type **REMOVE**. It takes the old install out of the menu and deletes
its partition. The ~2 GB becomes unallocated; Windows can have it back.
Reboot once more to see that it still boots.

## What to send if anything fails

- a photo of the screen with the FAILED line and the "stopped at" step;
- the output of `migrate` with no arguments, taken afterwards;
- `dmesg` if the kernel logged anything about `vol=` or `migrate`.
