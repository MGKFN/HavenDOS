# HavenDOS v0.9.7.2 Beta "Mnemosyne" — BOOT OS runs inside HavenDOS

Kyan, 22 November, after 0.9.7.1:

> my imgs are 1.5 mb but older isos not. I wish to use those to within
> HavenDOS idk if technically possible

It was possible, and this is it. He sent five old builds. Three are BOOT
OS, the 32-bit line that became HavenDOS: 0.3.1, 0.5.0 and 0.5.2. They
are ISOs: GRUB plus a Multiboot kernel. Retro PC's CPU was an 8086, which
cannot run a single instruction of them. Asked when, he chose "Now, as
0.9.7.2". This is exception E-6 in THS-RM-1005 REV E.

> **Not tested on a real machine.** Tested in QEMU and on the build
> machine. Nothing in it has run on the HP yet.

## New: a 386 in Retro PC

- **A 32-bit CPU** (`emu/cpu386.c`). It covers the 386's protected mode
  as a flat kernel uses it:
  - a GDT whose segments are honoured;
  - an IDT through which exceptions and hardware interrupts arrive;
  - ring 0 and no paging;
  - the integer instructions gcc emits for 32-bit code, plus the 486
    and 686 additions it reaches for (CMOVcc, BSWAP, XADD, CMPXCHG,
    CPUID, RDTSC).

  BOOT OS has no floating-point code, so there is no FPU.

  It is checked against the build machine's own processor. That means
  all 8 ALU operations and all 7 shifts and rotates, at 8, 16 and 32
  bits, on 120,000 random and edge-case operands, comparing the result
  and every defined flag.
- **The PC around it** (`emu/pc386.c`):
  - two 8259 interrupt controllers;
  - the 8254 timer, ticking with real time;
  - the 8042 with a keyboard and a PS/2 mouse;
  - the CMOS clock;
  - VGA text mode with its cursor;
  - no PCI devices and no disks. Every BOOT OS driver already handles
    that, because QEMU without the device looks the same.
- **GRUB's job, done by Retro PC.** GRUB needs a BIOS, a CD drive and
  real mode, so it is not emulated. Instead Retro PC reads the ISO's own
  `/boot/grub/grub.cfg` and shows its menu entries as GRUB did. Then it
  loads the chosen kernel the way GRUB loads it:
  - the ELF segments at their addresses;
  - a Multiboot information block with memory, a memory map and the
    command line (`mode=desktop gfx=text` and so on);
  - the CPU in flat protected mode at the entry point, with `EAX` set
    to `2BADB002`.

  The kernel cannot tell the difference.
- **Built in:** BOOT OS 0.3.1, 0.5.0 and 0.5.2, kept in `archive/retro`
  with their SHA-256 like K-DOS. Open Retro PC, choose one and pick a
  text entry. Log in as `bootos`, password `1234`.
- **Your own ISOs:** put them in `RETRO` on a USB stick (FAT32 or exFAT)
  or on the HavenDOS disk. Retro PC lists `.ISO` files beside `.IMG`
  floppies. A CD image is read into memory, up to 48 MB, and never
  written.

## What it is like

- **The boot animations run slowly.** BOOT OS times its delays by
  counting loop passes, not with the clock. On the build machine Retro
  PC runs about 70 million instructions a second, so BOOT OS 0.5.2 takes
  about 15 seconds to reach its login screen. The HP should be similar.
  Once it is up, the desktop and the shell respond normally.
- **Uptime stays at 0 seconds,** and the desktop clock shows 12:00:00.
  That is BOOT OS's own bug, faithfully reproduced: its timer interrupt
  handler takes the saved DS register as a pointer and reads the
  interrupt number from address 34h. So it never recognises the timer,
  under QEMU or on real hardware either.
- **The banners say v0.3.0** on 0.3.1 and 0.5.0. They did then too.

## Not covered (yet)

- **Graphics entries** (mode 13h, VESA). They boot, but Retro PC shows a
  note instead of the picture. Choose a "Text Desktop" or "Shell Mode"
  entry.
- **The two K-DOS ISOs.**
  - `kdos_2.iso` (K-DOS 1.0 Beta) is a 64-bit kernel. It switches to
    long mode, which needs a 64-bit CPU emulated on top of the 386. Its
    menu entry is listed, marked 64-bit, and refused with that reason.
    Kyan chose to do this later, after BOOT OS works.
  - `KDOS.iso` is not a bootable CD. It holds the source, a
    `kernel.bin` and another data ISO, with no boot menu on it. Retro PC
    says exactly that.

## Tests

- **`test_emu386` 34, new.** It covers:
  - the ALU and the shifts against this CPU;
  - small programs: loops, string instructions, LGDT with a segment
    base, INT through the IDT and IRET, HLT, #UD and #DE, MOVZX, SETcc,
    CMOVcc, BSF, BSWAP and IMUL;
  - each of the three built-in BOOT OS kernels loaded and booted to its
    login screen, with the command line checked;
  - 0.5.2 logged into, its shell opened, and `sysinfo` reporting the
    386's 16 MB and "x86 32-bit Protected Mode".
- **`harness/retro386.py`, new.** In QEMU, a USB stick holds
  `RETRO/BOOT52.ISO`, made with xorriso. Retro PC lists it, shows its
  boot menu, and boots BOOT OS 0.5.2 to its login and desktop.
- **`check_machine_reset`** now exempts the emulated PC's own files.
  They name the reset and power-off ports because they *are* the
  hardware behind them, and they never write a real port.
