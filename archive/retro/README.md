# The old builds

These are disk images of Tech Haven Studios operating systems from
before BOOT OS, kept exactly as they were found. Retro PC (the `retro`
command, or Apps > Retro PC) boots them inside HavenDOS on an emulated
PC, with its own pretend hard disk, so they can never touch a real one.

| Image | What it is |
|---|---|
| `kdos-beta-1.1b.img` | **K-DOS Beta 1.1b.** The oldest surviving build: a 512-byte boot sector that loads a 1.7 KB shell from the next 50 sectors. Login `admin` / `1234`. Commands: time, calc, user, taskman, sfc, fdisk, format, save, load, cls. 25,600 bytes, given to the project by Kyan on 17 November 2026. |

## What is known about this one

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
