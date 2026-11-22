# HavenDOS v0.9.7.1 Beta "Mnemosyne" — the sign-in screen, and your own images in Retro PC

The first maintenance update of 0.9.7, from Kyan's photo of 0.9.7 on the
HP and his message of 22 November:

> those kind of bugs are evrywhere this is 0.9.7 beta stuff i tested the
> reotro PC its cool, how do i add my own imgs???

> **Not tested on a real machine.** Everything below was tested in QEMU,
> on the HP's size of screen (128x50). Nothing has been run on the HP yet.
> From now on a release says this until it has been.

## What the photo shows

The sign-in screen, drawn over the Start menu. The account box and the
clock are at the top. The bottom half of the screen still shows the
Start menu, the taskbar and a Retro PC button, with the user tile
"[K] Kyan" drawn on top of the menu.

The sign-in screen was laid out for 80x25 and painted only 80 columns of
25 rows. The HP runs 128x48, so everything outside that box stayed on
the screen. Apps don't have this problem: since 0.9.6.5 each app runs in
its own box and cannot draw outside it. The screens that run with no box
did, and they are the ones fixed here.

## Fixed

- **The sign-in screen covers the whole screen**, whatever its size:
  background, top bar, bottom strip and clock. The account dialogs
  (Welcome, Create Account) and the password box are centred.
- **The first letter of a password is no longer lost.** After choosing a
  user, the first key typed was read and thrown away. A harness typing
  `Admin@123` got eight stars and "Wrong password". It is now the first
  character of the password.
- **Lock from inside an app no longer has a taskbar over it.** The Shell's
  `lock` and the Power app's Switch user run the sign-in screen from
  inside a window. That window kept repainting its taskbar clock over
  the sign-in screen's bottom strip, and a click on the taskbar could
  still bring another app forward past the lock. The window is now
  paused while the sign-in screen is up, and put back afterwards.
- **The Shell's `lock` is the real sign-in screen.** It had its own
  80x25 black screen and its own password check. Now it is the same
  screen as Start → Lock.
- **`cls` clears all of the Shell**, and the next prompt is at the top.
  It cleared 80 columns of 25 rows and left the cursor where it was.
- **First-boot setup is centred** on a cleared screen instead of drawn
  in the top-left corner over the boot log.
- **Files bigger than 4 KB:**
  - `cp` copied the first 4 KB of any file and said "Copied".
  - `>>` past 4 KB overwrote the whole file with just the new text.
  - `tee -a` past 4 KB did nothing.
  - `cat`, `head`, `tail`, `wc` and `grep` only read the first 4 KB.
  - All of these now read the whole file, up to 16 MB. A `cp` whose
    write comes up short says so instead of "Copied".
- **In a live session (the ISO), files can be bigger than 2 KB.** Anything
  longer was cut at 2 KB without a word. Up to 2 KB still lives where it
  did; anything bigger uses the heap, up to 8 MB.

## New: your own images in Retro PC

1. On Windows, make a folder called **`RETRO`** on a USB stick formatted
   FAT32 or exFAT.
2. Put your `.img` files in it. Names must be 8.3, like `MYDOS.IMG`.
3. Plug the stick into the HP and open Retro PC. If it was already open,
   press **R** to look again.

The list now holds images from all three places, built in, the HavenDOS
disk and a USB stick, with where each one comes from beside it. An image
on a stick is read into memory to boot. Nothing is written to the stick:
the image's hard disk is saved as a `.HDD` file in `RETRO` on the
HavenDOS disk.

What boots: floppy images of up to 2.88 MB that boot in 16-bit real
mode and use text mode. Your old K-DOS and HavenDOS builds are exactly
that. Graphics modes, sound and DOS itself are not emulated.

You can also copy an image into `RETRO` on the HavenDOS disk with `cp`,
which now copies the whole file.

## Not fixed yet

- The trackpad still needs the `dmesg | grep I2C` photo.
- The external monitor fallback still needs `dmesg | grep monitor`.
- A line typed at the Shell prompt stops at the edge of the screen.
- `sort` and `uniq` still read the first 4 KB of a file.

## Tests

- **`harness/redraw_hunt.py` 12/12, new.** At 128x50 it checks that:
  - `exit` and Start → Lock show a sign-in screen covering the whole
    screen, with nothing of the Shell or the Start menu left on it;
  - signing in again brings back the desktop as it was;
  - `cls` clears everything and puts the prompt at the top;
  - `lock` shows the whole-screen sign-in screen and returns to the
    Shell.
- **`harness/fixes_0971.py` 7/7, new.** It checks that:
  - `>>` 110 times keeps all 110 lines (4.8 KB);
  - `cp` and a copy of the copy are both whole;
  - a FAT32 stick with `RETRO/MYDOS.IMG` shows in Retro PC as on a USB
    stick, boots K-DOS from the stick, and the log names it.
- **`test_ramfs` 18, new.** Small and big files, writing over a big file
  with a smaller one, deleting, and running out of memory.
- 7672/7672 checks across 76 suites.
- QEMU: `features_097` 10/10 and `retro_boot` 9/9 were run again on this
  build.
