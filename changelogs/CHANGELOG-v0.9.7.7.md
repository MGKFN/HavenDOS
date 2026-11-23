# HavenDOS v0.9.7.7 Stable "Mnemosyne" — a bug sweep

Kyan, 23 November: "we need to fix unknown bugs ... just go on fixing,
we can't start 0.9.8 soon enough". There was no bug list, so this
release went looking for bugs itself, in three ways:

1. **Static analysis** of all 152 kernel source files, with GCC's
   `-fanalyzer` plus extra warnings, and with cppcheck.
2. **Fuzzing** every reader of input HavenDOS does not control: USB
   sticks, monitors, the network, USB devices, and Retro PC's CDs and
   kernels. Each reader was given millions of damaged inputs under
   AddressSanitizer and UBSan.
3. **Every QEMU harness**, all of them, against this build.

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU.

## Fixed: a key press could go missing after a stray mouse byte

The main fix, and one you could hit on a real machine. The keyboard
and the PS/2 mouse share one controller. To keep a mouse packet in one
piece, HavenDOS sends the second and third bytes of a packet to the
mouse even when the controller does not mark them as mouse bytes
(some controllers, and UTM, leave that mark off).

A single stray byte from the mouse side (for example its "acknowledged"
reply, 0xFA) looked like the first byte of a packet. HavenDOS then
waited for the rest of that packet forever, and the next key press and
release were given to the mouse instead. The key was lost, and a
program waiting for Enter could seem frozen until another key was
pressed.

A half-finished mouse packet now expires after 50 ms. A real packet
arrives in about 1 ms, so nothing that should go to the mouse goes to
the keyboard. This also explains the Retro PC 386 harness's occasional
freeze, which went back to at least 0.9.7.5 (1 run in 3): it stopped
in the keyboard wait, holding a lost key. It now passes 7 runs out
of 7.

## Fixed: a damaged CD or kernel could crash Retro PC

`emu/` is now Retro PC 0.1.1, from
[MGKFN/RetroPC](https://github.com/MGKFN/RetroPC). These are the only
crashes the sweep found, and both are in the code that reads files a
user brings to Retro PC:

- **The CD reader** trusted each directory entry's name length. A name
  longer than its entry read past the end of the image. A directory far
  enough into the disc also wrapped its position around 32 bits.
- **The kernel loader** trusted the kernel's own sizes:
  - a part whose file size was bigger than its memory size made it clear
    almost 4 GB of memory;
  - offsets near 4 GB wrapped past the checks;
  - a Multiboot header at the very end of the file was read past the end.

Inside HavenDOS, either one could have brought down the whole system
from an `.ISO` in a `RETRO` folder. Both are now checked in 64 bits
before anything is copied, and a damaged kernel says "the kernel's
program headers are damaged".

## What was looked at and found sound

These readers survived the fuzzing without a single crash or
out-of-bounds access:

| Reader | Input | Runs |
|---|---|---|
| FAT32 driver: mount, walk, read, write, rename, delete | a USB stick image with damaged boot sector, FSInfo, FATs and directories | 60,000 |
| exFAT driver: mount, walk, read | a factory-format stick image, damaged the same way | 320,000 |
| GPT reader | a partition table, its CRCs repaired after each change so the entries are reached | 3,000,000 |
| EDID parser | what a monitor sends over HDMI, checksums repaired | 3,000,000 |
| DHCP, DNS and IPv4 | replies and packets off the network | 3,000,000 |
| HID report descriptors | what a mouse, keyboard or trackpad says it sends | 20,000,000 |
| DER and PEM keys and certificates | what a TLS server or an iPhone sends | 2,000,000 |
| Retro PC's 386 and 8086 | random machine code | 3,000 and 20,000 runs |

Static analysis found nothing that was a real bug. Every finding was
checked by reading the code:

- copies that stop at the terminating zero before their bound;
- a test command that crashes on purpose;
- a Stopwatch line that assigns a variable to itself, harmlessly,
  because the countdown is recomputed every frame.

## The QEMU harnesses

- `desktop_layout` reported two failures. The notification drawer on a
  160×50 screen was right: anchored to the right edge and reaching the
  taskbar. The harness was out of date: it still looked for the footer
  the drawer had before 0.9.5 ("Any key=Close", now "any key closes"),
  and it assumed the drawer was under 30 columns wide. The harness is
  fixed.
- `wc_boot` looked for a version line beginning "0.8.9". It now
  accepts any version.
- `gptwrite_real` no longer compiled: its stand-in `memset`, `memcpy`
  and `memcmp` took a 32-bit length while the kernel's `string.h` now
  says `size_t`. Fixed; it passes, and sgdisk and parted still find a
  512 GB disk intact after every write and every refusal.
- `capture` and `freshdisk` are old helpers tied to the 0.8.0 ISO,
  not tests. They were left alone.
- Everything else passed, run one at a time: appguard 19/19,
  esp_update, fat16_regress, fat32_boot 7/7, gfx_boot 15/15,
  helios 32/32, hid_boot 3/3, layout_audit, lockdown 12/12,
  machine_reset 8/8, migrate_fat32, mouse_apps 14/14,
  redraw_hunt 12/12, replug 11/11, resolution_tasks 8/8,
  retro_boot 9/9, retro386 7/7, selfhost, uefi_dualboot,
  usb_files 17/17, wholedisk, window_apps 11/11.

## Tests

- `make test`: 77 suites, 7758 checks. `test_kbdinit` gains three
  (52): Enter after a stray mouse byte arrives as Enter (0.9.7.6 lost
  it), the half packet expires, and a real packet whose second and
  third bytes lack the mouse mark still reaches the mouse.
- RetroPC's own `make test` gains **test_fuzz** (12 checks, under
  AddressSanitizer). It builds each of the two bugs exactly, and runs
  bounded fuzzing on every build, so they cannot come back unnoticed.
