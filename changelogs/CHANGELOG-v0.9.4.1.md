# HavenDOS v0.9.4.1 Beta "Prometheus": what the 1.0 screenshots found

*13 November 2026. 65 test suites, 6916/6916 checks.*

A patch to 0.9.4. Kyan asked for screenshots of the working features for
the 1.0 trailer. Taking them meant actually using an installed 8 GB
FAT32 system, and that found eight bugs. A screenshot with a failure on it
is no use in a trailer, so they're fixed here and the screenshots were
taken again on this build.

## Fixed

- **On a FAT32 install, nothing could be saved in a home directory.**
  `fat16_write_path()` creates missing parent directories, because "the
  shell hands us HOME/<user>/<file> for a home directory that may not
  exist yet". `fat32_write()` doesn't, and nothing created `HOME/kyan` on
  the disk. `kernel/main.c` seeds it on the RAM disk only.
  - So `write`, `mkdir` and the editor's save all failed in `~`, on every
    install over 2047 MB since 0.9.3.
  - The VFS's FAT32 write and mkdir now create the parents.
  - Found when `ls` showed nothing in a home where three files had just
    been written.
- **`vfs_write()` on a FAT32 system returned 1 or 0, not the byte
  count.** That is the convention every caller is written against, and
  `fs/fat16.c` has always followed it. On a FAT32 install:
  - A FAILED write returned 0. The text editor and `bpkg` test for
    `>= 0`, so they reported a lost save as saved.
  - `selftest` failed "VFS round trip: write returns byte count".

  The route now returns the size on success and -1 on failure. 0.9.3
  made FAT32 the system filesystem above 2047 MB, so every large install
  had this.
- **The last setup screen said "Step 8 of 7".** `hdr("Ready", 5)` should
  have been 4. This was on every first boot.
- **`calc` gave wrong answers without saying so.** `^` was an unknown
  character, and an unknown character silently ended the expression:
  `calc (2 + 3) * 7 ^ 2` printed 35.
  - `^` is now a power, right-associative and tighter than `*`, so the
    same line prints 245.
  - Anything else it can't read is now reported, instead of answered.
- **`edit notes.txt` opened the wrong file.** It ignored the working
  directory, so in `HOME/kyan` it opened an empty buffer for `/notes.txt`
  (and saved it there), while `cat notes.txt` showed the real file. It
  now resolves paths like every other file command.
- **`vga_printf` understood one character after `%`.** So `%02u` printed
  the literal text "2u" and never consumed its argument, which shifted
  every value after it. `sysinfo` showed "Uptime 0:2u:2u". Eighteen
  format strings in the tree use a width or a flag. Now supported: `-`
  and `0` flags, a width, `l`, and `%X`.
- **Snake warned "Timer ticks not advancing" on a working timer.** It
  waited a fixed 500,000 `pause`s, which can finish inside one 10 ms tick
  on a fast CPU. It happened under QEMU every time, and could on the HP.
  It now waits for a tick, with a bound of seconds.
- **The shell's banner still advertised what was "new in 0.7.6".** It
  now suggests `neofetch`, `disk`, `usb`, `selftest` and `help`.

## Not fixed, noted

- Snake at its start speed reaches the wall in about a second, so it
  isn't in the screenshots. The game logic itself is unchanged.
- The QEMU harness's first keystroke into a password field is lost.
  That's a harness problem: QEMU's sendkey arrives before the field has
  drawn. The screenshot run used an account with no password.
- The desktop shows only the icons you pin. A new install pins the Shell
  and nothing else, so the desktop looks empty until you press D on apps
  in the start menu. Whether 1.0 should pin a sensible default set is
  Kyan's call.
