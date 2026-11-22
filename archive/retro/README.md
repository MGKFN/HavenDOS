# The old builds

These are the old builds of Tech Haven Studios operating systems, kept
exactly as they were found: disk images from before BOOT OS, and since
0.9.7.2 the BOOT OS kernels themselves. Retro PC (the `retro`
command, or Apps > Retro PC) boots them inside HavenDOS on an emulated
PC, with its own pretend hard disk, so they can never touch a real one.

| Image | What it is |
|---|---|
| `kdos-beta-1.1b.img` | **K-DOS Beta 1.1b.** The oldest surviving build: a 512-byte boot sector that loads a 1.7 KB shell from the next 50 sectors. Login `admin` / `1234`. Commands: time, calc, user, taskman, sfc, fdisk, format, save, load, cls. 25,600 bytes, given to the project by Kyan on 17 November 2026. |

| `bootos-0.3.1.elf` + `.cfg` | **BOOT OS 0.3.1.** A 32-bit Multiboot kernel and the `grub.cfg` its ISO carried. Log in `bootos` / `1234`. Given by Kyan on 22 November 2026, from `bootos-v0.3.1-fixed`. |
| `bootos-0.5.0.elf` + `.cfg` | **BOOT OS 0.5.0.** Its banner still says v0.3.0. |
| `bootos-0.5.2.elf` + `.cfg` | **BOOT OS 0.5.2.** Hashed passwords; the last before the line was renamed HavenDOS. |

The BOOT OS kernels run on Retro PC's **386** (`emu/cpu386.c`,
`emu/pc386.c`, v0.9.7.2), which loads them the way GRUB did: the menu
from the `.cfg`, the kernel's ELF segments at their addresses, the
Multiboot information with the command line. Only the kernel and the
menu are kept here -- the rest of each 5 MB ISO was GRUB.

## What is known about K-DOS Beta 1.1b

- It was assembled for a **386**, not an 8086: its long conditional
  jumps are the 386's `0F 8x` form, and it uses PUSHA/POPA (80186).
- It uses four BIOS services: INT 10h (teletype, cursor, scroll),
  INT 16h (read a key), INT 1Ah (the clock) and INT 13h (CHS read and
  write of drive 80h).
- `save` and `format` write **sector 10 of the first hard disk**
  directly. Booted on a real PC today, it would overwrite part of that
  disk. Under Retro PC drive 80h is a file, `RETRO/KDOS11B.HDD`.
- `taskman` leaves the screen half-drawn and returns to the login
  prompt after a key. That is K-DOS itself: QEMU with a real BIOS does
  exactly the same, screen for screen.

## Adding one

Put the image here, add a line to `IMAGES.txt` with its SHA-256, and
build. tools/make_retro_images.py embeds every listed image in the
kernel and refuses a file whose hash has changed. An image does not
have to be built in to be run: `retro <file>` boots any image on the
HavenDOS disk.
