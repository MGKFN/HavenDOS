# HavenDOS v0.9.7.5 Beta "Mnemosyne" — Retro PC on Windows and Linux

Kyan, 23 November, choosing what comes after 0.9.7.4: "1 and 2". The
first is Retro PC as a program of its own, for Windows and Linux. The
second, HavenDOS inside HavenDOS, comes next.

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU. RetroPC.exe was built and its imports checked,
> but it has not been run on Windows: Wine crashes on start-up on the
> build machine, before it loads any program. The Linux window was run
> on a virtual X display.

## New: Retro PC as a program

Two new downloads beside the ISO:

| File | What it is |
|---|---|
| `RetroPC-0.9.7.5-windows.zip` | `RetroPC.exe`, 640 KB. Nothing to install: it uses only DLLs that come with Windows. |
| `RetroPC-0.9.7.5-linux-x86_64.tar.gz` | `retropc`, an X11 window, and `retropc-headless`, the same program with no window |

It is the Retro PC from inside HavenDOS:

- the same 8086 and 386 emulators, compiled unchanged;
- the same four built-in images: K-DOS Beta 1.1b and BOOT OS 0.3.1, 0.5.0
  and 0.5.2;
- the same list, the same GRUB menu for the 386 builds, the same F4 to
  leave.

**Your own images:**

- put `.IMG` floppies and `.ISO` CDs in the `RETRO` folder beside the
  program;
- or press **O** in the list for the Open dialog;
- or, on Windows, drop a file on the window or on `RetroPC.exe`;
- or give it on the command line: `RetroPC game.img`.

**Hard disks are the same files as on HavenDOS.** A built-in image's
disk is `RETRO\KDOS11B.HDD`, 504 KB, and a file's disk is its name with
`.HDD`. Copy one from a HavenDOS disk to Windows, or back, and the old
PC finds it.

**On Windows:**

- **Alt+Enter** switches full screen.
- The window scales the picture to fit, in whole steps where it can, so
  the pixels stay sharp.
- Started from a Command Prompt, `RetroPC --help` prints there.

Windows will warn the first time ("Windows protected your PC"), because
the program is not signed: More info, then Run anyway.

**What it shows that HavenDOS's own Retro PC does not:**

- **The box lines and blocks.** HavenDOS's 8x16 font leaves the CP437
  line-drawing characters blank, because nothing in HavenDOS draws them.
  BOOT OS's desktop uses them for its panels. Retro PC draws each one
  from which arms it has (single or double, up, down, left, right), so
  every one from B0 to DF has a shape.
- **Blinking**, as on a VGA: attribute bit 7 blinks, unless the old
  system turns blinking off. BOOT OS never does, so its selected icon
  blinks, as it would have on a real PC.

## Fixed: F1 to F12 in Retro PC's 386

- The 386 PC sent every key above the letters with the E0 prefix that
  marks an extended key. F1 became E0 3B, which a keyboard never sends.
- A kernel that reads E0 as "a cursor key follows" saw nothing at all.
- Now F1-F10, F11 and F12 arrive without it, as on a real keyboard. The
  cursor keys keep it.
- This is in HavenDOS's own Retro PC too.

## Tests

- `make test`: 78 suites, 7788 checks.
  - **test_retropc** (new), 46 checks. It runs the program through
    `retropc-headless`, typing scripts on a virtual clock:
    - K-DOS boots and logs in;
    - `save` writes the word "save" at LBA 72 of `RETRO/KDOS11B.HDD`
      and nothing else, which is what QEMU's disk showed;
    - a boot that writes nothing leaves the disk alone;
    - a file's disk lands beside it, and the image itself is never
      written;
    - files that will not boot say why, in words;
    - the list finds the `RETRO` folder;
    - BOOT OS 0.3.1 boots, logs in, opens its shell and answers `help`;
    - an ISO in `RETRO`, read through ISO 9660, boots to its login;
    - the glyphs, by shape.

    The K-DOS cases run twice, once under AddressSanitizer. Two planted
    defects were both caught: the hard disk never saved, and the box
    glyphs not drawn.
  - **test_emu386** 36 (+2): F1 reaches the keyboard controller as
    3B BB, and Up as E0 48 E0 C8.
- QEMU: `harness/retro386.py` 7/7. Inside HavenDOS, BOOT OS 0.5.2 boots
  from a CD image on a stick, reaches its desktop, and F4 comes back.
- The Linux window was run on a virtual X display: the list, K-DOS's
  shell, BOOT OS's GRUB menu, and BOOT OS's desktop with its box lines.
- `make retropc` refuses an `.exe` that imports a DLL Windows does not
  ship. `tools/make_retropc_dist.py` refuses a program built from an
  older `version.h`.

## Building it

```
make retropc        # needs gcc-mingw-w64-x86-64 and libx11-dev
```

The release workflow installs both and publishes the two downloads. The
sources are in `retropc/`:

- `retropc.c`: the program;
- `font.c`: the glyphs;
- `host_win32.c`, `host_x11.c`, `host_null.c`: the three hosts;
- `plat_posix.c`: Linux files and folders.

## Not in this release

- Graphics modes (mode 13h, VESA). An entry that uses one boots, and a
  box says the screen cannot be shown yet.
- 64-bit kernels.
- A mouse, sound or a disk for the 386.

The graphics modes and a disk for the 386 are what HavenDOS inside
HavenDOS needs, so they come next.
