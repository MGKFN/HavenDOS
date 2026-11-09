# HavenDOS v0.8.5.5 — "one place that resets the machine"

Channel Beta, codename "Atlas" — unchanged: this is a patch
release over 0.8.5, not a new one.

```
kernel   2985984 bytes   crc32=6ce770c0
ISO      HavenDOS-0.8.5.5.iso
```

`reboot` and `poweroff` hung the HP. Reported 20 October. Neither was a
crash: both were a single write to a port that exists only in an
emulator, followed by `for(;;) cli; hlt`, so on a machine where the
write does nothing the halt is reached and the screen keeps saying
"Rebooting HavenDOS... Please wait." for ever.

## It was nine places, not five

The handoff said five. It was nine, across five files:

| file | resets | power-offs |
|---|---|---|
| `kernel/main.c` | 1 | |
| `shell/login.c` | 1 | |
| `shell/shell.c` | 1 | 1 |
| `shell/desktop.c` | 1 | 2 |
| `shell/iso_installer.c` | 2 | |
| **total** | **6** | **3** |

**Not one of the nine had heard of port `0xCF9`.** Two of the six were
written in inline assembly rather than through `outb`, which is why a
grep for `outb(0x64` found four.

## The fix

`machine_reset()` and `machine_poweroff()` in `kernel/machine.c`,
declared in `include/machine.h`. All nine sites call them. The ladder:

```
0xCF9 <- 0x02, 0x06     platform reset, armed then triggered
0xCF9 <- 0x02, 0x0E     + FULL_RST, a power cycle, same edge
0x64  <- 0xFE           the 8042 pulse, bounded wait
cli; lidt null; int3    triple fault
message + park          "Reset failed", then say why
```

`RST_CPU` triggers on a 0→1 edge, so each trigger has an arming write
before it: writing `0x06` into a register already holding `0x06` does
nothing at all. That is why stage 1 and stage 2 are two writes each.

The `cli` before the triple fault is for the case where it does not
work. A hypervisor that catches it leaves the machine running with a
destroyed IDT, and the next timer tick would be a fault with nowhere to
go — part-way through drawing the message that explains what happened.
Exceptions are not maskable, so the deliberate `int3` still faults.

## Two bugs in the code being replaced, neither of them the reported one

1. **The 8042 wait could hang before the reset write.** It was
   `1: inb $0x64; testb $2, %al; jnz 1b` with no exit. On a controller
   whose status register never clears — an SMM emulation under load,
   which is what that laptop has — it spun there for ever, and *never
   reached the `outb`*. From the outside that is indistinguishable from
   a reset that did not work. Bounded at `KBC_SPINS` now, and the
   reset command is sent even when the wait gives up: a controller that
   will not report itself ready may still have a reset line.
2. **The status reads had no I/O delay between them.** That is the
   v0.8.5.3 keystroke-doubling bug in a second place — an emulated
   controller hands back a stale register when the read follows too
   closely. `io_wait()` between them now, the same fix.

## `poweroff` is honest, not fixed

Real power-off means ACPI: find the RSDP, parse the FADT for
`PM1a_CNT_BLK`, parse the DSDT for the `\_S5` sleep type, then write
`SLP_TYP|SLP_EN`. There is no shortcut and it is a release of its own.
The three emulator ports are still tried; what changed is what happens
when they do nothing:

```
Power off is not supported on this machine yet.
Hold the power button to switch off.
```

**The lie it replaces was worse than the hang.** Both the shell and the
desktop printed "It is now safe to power off." *before* trying, so a
machine with its fans still running told the person it was off. That
line is gone from both.

## What stops a tenth copy

`tools/check_machine_reset.py`, in the `make` checks. It refuses
`0xCF9`, `0x604`, `0xB004`, `0x4004` and the 8042 reset pulse anywhere
outside `kernel/machine.c` — the pulse matched as a *write* (`outb(0x64,
0xFE)` or `$0xFE, %al`) rather than as the port, so `drivers/keyboard.c`,
`drivers/mouse.c` and `drivers/vga.c`'s font table do not trip it.
Comments are blanked before scanning, so the prose in `include/machine.h`
describing the bug does not fail the build that documents it.

Checked against a reintroduced copy in all five forms, including the
inline-asm one: refused, then clean again.

## Testing

`tests/test_machine.c`, 46 checks. `include/machine_asm.h` gives the
triple fault and the park the same `MACHINE_ASM_H` seam that `io.h` and
`mmio.h` use, so the suite runs the real `machine_reset()` against
recording accessors rather than a copy of the sequence — and because
nothing actually resets, every fallback stage is reached in one run.
The simulated 8042 can hold its input buffer full for ever, which is
the case that used to hang.

`tools/check_machine_suite.py`, 20 injections, all 20 caught. Four of
them have shipped.

**The harness was itself checked**, because all 20 passing on the first
run is the least trustworthy result this tree recognises: a harmless
injection was appended last and correctly reported MISSED. That proves
MISSED is reachable *and* that the 20 damaging injections before it were
fully restored — the exact bug `check_xhci_suite.py` had.

`harness/machine_reset.py` drives `reboot` and `poweroff` under QEMU on
`pc` and `q35`. The signal is the QEMU process, not the screen:
`-no-reboot` makes QEMU exit on a guest reset, so an exit proves the
command was typed and reached the shell; without it QEMU restarts the
guest, so staying alive proves it was a reset and not a shutdown. Both
machine types implement `0xCF9` (`piix-reset-control` and
`lpc-reset-control`), and it is tried first, so the reset is
attributable to stage 1.

**1995 checks across 31 suites**, up from 1949 across 30.

## Updating to this release

The payload is a pair, and they must match: `Stage-HavenDOS-Update.ps1`
checks the manifest's `size=` **and** `crc32=` against the kernel you
hand it and refuses on either mismatch.

```
havendos.elf   2985984 bytes   crc32=6ce770c0
update.inf     version=0.8.5.5, describing exactly those bytes
```

Verified three ways: `tools/make_iso_manifest.py` computed the CRC with
zlib, `python3 -c "import zlib; ..."` recomputed it independently, and
`upd_manifest_parse()` from `fs/update_src.c` — the shipping parser, not
a copy — was run against both the ISO's `/update.inf` and the staging
pair and accepted them with all four fields present.

**From Windows, onto the EFI System Partition** (the 0.8.4 route, no ISO
needed):

```powershell
.\Stage-HavenDOS-Update.ps1 -Kernel .\havendos.elf -Manifest .\update.inf
```

It writes only `\EFI\HAVENDOS\update\havendos.elf` and
`\EFI\HAVENDOS\update\update.inf`, and goes nowhere near
`\EFI\Microsoft` or `\EFI\BOOT`.

**From the ISO**: the same manifest is at `/update.inf` on the disc.

Either way, boot an entry **without** `live` — on the ISO that is entry
4, "Update installed system from the ESP" — and type `update`. A live
shell cannot see the HavenDOS partition and `update` refuses with "No
FAT16 partition found".

`ver_cmp()` from `fs/update.c`, run against this release:

| installed | staged | result |
|---|---|---|
| 0.8.5.4 | 0.8.5.5 | upgrade |
| 0.8.5 | 0.8.5.5 | upgrade |
| 0.8.5.5 | 0.8.5.5 | already installed, refused |
| 0.8.6 | 0.8.5.5 | older, refused |
| 0.8.5.10 | 0.8.5.5 | older — compared as numbers, not strings |

## Still unproved, and only the HP can answer it

**That `reboot` works on a machine with no 8042.** QEMU has a working
one, so under QEMU the old code rebooted too — which is exactly why this
shipped nine times. Steps and expected screens:
`docs/MACHINE-RESET-HARDWARE-TEST.md`.
