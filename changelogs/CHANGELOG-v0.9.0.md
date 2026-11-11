# HavenDOS v0.9.0 Beta "Janus": the display engine, read only

*11 November 2026. 61 test suites, 6304/6304 checks.*

## First: what the HP showed on 0.8.9.9

Kyan ran 0.8.9.9 on the HP with the iPhone attached and Personal Hotspot
on, and sent two photos:

```
pair            ->  reusing the stored record (no keygen)
                    paired, saved to .pairrec, 4453 bytes
                    StartSession -> session FB244821-...
                    TLS handshake ... the handshake failed (-1)
                    reached: ClientHello sent
                    mux: connection gone; the device reset the connection
ifconfig dhcp   ->  DHCP OK. IP: 172.20.10.8  GW: 172.20.10.1
ping 172.20.10.1 -> 4 replies, 0 ms
ping 8.8.8.8    ->  4 replies, 285 / 25 / 20 / 30 ms
ping google.com ->  resolved to 172.217.17.206, 4 replies, 15-20 ms
wget google.com ->  Saved 7.2 KB
```

**Tethering works. HavenDOS has reached the internet from real hardware
for the first time:** DHCP, ARP, ICMP, DNS and a TCP download, all
through the iPhone. All three 0.8.9.9 fixes held on the wire:

- **The interface claim.** DHCP asked and was answered, after `pair` had
  opened the mux on the same device. In 0.8.9.8 this read "0 asked".
- **The split mux read.** TLS got past its first read and sent a
  ClientHello. In 0.8.9.8 it stopped before that.
- **The rotating source port.** Nothing in either photo reads REFUSED.

The 2-byte receive skip in the USB Ethernet driver, which the handoff
called "never seen on a real wire", has now been seen on one. A wrong
skip would have broken every received frame, and DHCP, ping and DNS
replies all arrived intact.

### The one thing that did not work: TLS to lockdownd

`pair` failed at "ClientHello sent". The phone **reset the TCP
connection** rather than sending an alert. This is the handoff's case of
a failure with no alert and "connection gone". The mux line names the
reason ("the device reset the connection"), and the transport underneath
is evidently fine, because tethering ran over the same USB device a
minute later.

So lockdownd read our ClientHello and hung up. We offer exactly one
cipher suite, 0x003C (TLS_RSA_WITH_AES_128_CBC_SHA256), and that is the
first suspect. It is not proved. Some TLS stacks have dropped the
CBC-SHA256 suites entirely, and a server with nothing in common with the
client may close the connection instead of sending an alert. The next
step, when pairing matters again, is the one the handoff already names:
also offer 0x002F, which needs an HMAC-SHA1. **Tethering does not need
any of this,** so it waits until after the display work.

## The release: `gfx`, a read-only inspector for the Iris Xe

Kyan wants GRUB and HavenDOS on his external monitor. Both draw into the
framebuffer the UEFI firmware lit, on the screen the firmware picked.
Moving HavenDOS means programming the display engine ourselves: a power
well, a PLL, a DDI, a transcoder, a pipe and a plane. That is the 0.9
series, and this is its first step.

**It writes nothing.** Every later step copies what the firmware already
did for the panel, aimed at a different output. So before any register
changes, the machine has to show what the firmware did.

```
gfx         power wells, CDCLK, pipes, transcoders, ports, hot-plug and
            PLLs -- then what it means: which pipe drives the panel,
            which is free, and which connector HDMI is wired to
gfx regs    every register that was read, offset and value, in order
gfx vbt     the Video BIOS Table: which socket is on which port, with
            its DDC pin and AUX channel
```

### Where the numbers come from

QEMU has no Intel display engine, so there was nothing to probe. Every
register offset and bit was checked against the Linux i915 driver's
display-13 (Alder Lake-P) definitions, which are MIT-licensed. Each
define in `drivers/xedisp.c` names the i915 symbol it mirrors, so the
next person can check them as well. This covers:

- **Power wells** (`xelpd_power_wells`): PW_1 holds transcoder A and
  DDI A/B, PW_A holds pipe A, PW_2 holds Type-C, and PW_B/C/D sit
  under PW_2.
- **The ports.** ADL-P has DDI A and B plus TC1-TC4, and no port C.
- **The VBT renumbering.** On display 13 the VBT calls TC1-TC4 "F" to
  "I". DDC pins and AUX channels map through the ADL-P tables.
- **Where the VBT lives.** It can be in mailbox 4 of the OpRegion, or
  outside it through RVDA. From OpRegion 2.1, RVDA is relative to the
  OpRegion.

### What makes "read only" mechanical

- `drivers/xedisp.c` has a read seam, `GFX_RD`, and no write seam.
- **`tools/check_gfx_readonly.py`** (new, in `make`) refuses the build
  if the driver or the command contains any of: a write seam, a store
  through a volatile pointer, a PCI config write, `pci_enable`,
  `pci_set_d0`, a port write, an MSR write, or a framebuffer
  memory-type change. Six planted writes were all refused, and a
  `==` comparison was not.
- `tests/test_gfx.c` links against a PCI layer that has no write
  functions. A write added to the driver stops the suite building.

### Every read is gated on its power well

A register in a powered-down well reads zero, and zero looks plausible
for almost anything here: "plane off", "PLL off", "nothing plugged in".
A wrong gate would not look wrong; it would look like a quieter machine.
So every read is gated on the well that powers it. The engine's own
"unclaimed access" flag (FPGA_DBG bit 31) is read first and last, and if
it goes from clear to set during the pass, the screen says
`UNCLAIMED READ` in red. If the firmware had already set it, the screen
says that instead and does not blame this pass.

### The window is mapped once

BAR0 is above 4 GB on the HP. It is mapped through `highfb_map_io()`
(uncached), **2 MB and no more**. The GTT sits above the registers in
BAR0 and is never touched. The mapping is kept: `kernel/highfb.c` has
eight windows for the life of the machine, and a command that mapped
every time it ran would use one up on every run.

### The framebuffer cross-check

`kernel_fb_phys()` (new, in `kernel/main.c`) hands back the physical
address GRUB gave us. Pipe A's plane scans out a GTT address, and the
aperture (BAR2) is a window onto the GTT. If BAR2 plus that address
equals the framebuffer, the report says so. That is the photo's built-in
check that the inspector is reading the right registers.

## Tests

| what | result |
|---|---|
| `tests/test_gfx.c` (new) | **174/174**, under ASAN and UBSAN as well |
| `tools/check_gfx_suite.py` (new) | **31/31** injections caught, and the harmless one correctly missed |
| `harness/gfx_boot.py` (new) | **9/9** in QEMU: the kernel boots, `gfx` dispatches, refuses by name with no Iris Xe present, and the shell answers afterwards |
| `make test` | **6304/6304** across 61 suites (6130 across 60 before) |
| `make` | every build check passes, including the new `check_gfx_readonly` |

The model engine is strict on purpose. Its registers belong to power
wells, written from i915's power map and not copied from the driver's
gates. A read in a dark well returns zero, sets FPGA_DBG, and is
counted. Reads are counted per register.

**The harness found four misses on its first run, after the suite
had passed 172 of 172.** These are the same shapes the handoff lists:

- **"detect off" was checked with `has()`.** Any line satisfied it, and
  every Type-C row on the HP says "detect off", so the DDI B check
  could never fail. Per-port checks now read the port's own line.
- **The MIPI block in the test VBT was zero-filled.** A walk that used
  the wrong size crept through zero-length "blocks" three bytes at a
  time and landed on block 2 regardless. The payload is junk bytes now.
- **No test child was DVI-only,** so "DVI is called HDMI" had nothing
  to break.
- **One injection was too short.** The "line too long" injection made
  a 77-character line, which fits. That was the harness's mistake, not
  the suite's.

Before any of that, the suite's own first run failed two checks. The
fixture counted reads per 4 KB page, and DDI A/B share a page with the
Type-C buffers. A fixture that cannot tell two registers apart cannot
test a gate between them. It counts per register now.

## Also fixed

- **`tools/check_machine_reset.py` matched port numbers as substrings.**
  The display driver's `TRANS_DDI_FUNC_CTL` offset, `0x60400`, contains
  `0x604`, QEMU's power-off port, so a register offset failed the build.
  The check now compares whole hex literals by value. That also catches
  `0x0604`, which the substring test missed. It was proved against
  `0x604`, `0x0604`, `0x604u`, `0xCF9` and `0xb004` (all refused), and
  `0x60400` (accepted). The checker was fixed rather than given an
  exemption.

## Things worth knowing

- **`include/gfx.h` already exists.** It is the old display-mode enum
  that `drivers/vesa.c` and `drivers/mode13.c` use. The display engine
  header is `include/xedisp.h`, and the driver is `drivers/xedisp.c`.
  The command is still `gfx`.
- **The build needs `grub-efi-amd64-bin`.** The handoff's `apt-get`
  line lacked it, and without it `check_iso_bootable` refuses the ISO
  because it has no 64-bit UEFI loader. The handoff now has it.
- **The 0.9.x numbers are the display series now.** 0.9.0 to 0.9.3 are
  the four display steps. RM-1003's labels for those numbers (0.9.1
  Gaia, 0.9.2 Prometheus, 0.9.3 Orpheus) no longer apply. The handoff
  already said those "float".

## What has not been tested

**All of `gfx` on real hardware.** `docs/GFX-INSPECTOR-HARDWARE-TEST.md`
is the ten-minute test: `gfx`, `gfx vbt` and `gfx regs` with the
monitor plugged in, then `gfx` again with the cable out. Nothing in it
can change the laptop's state.

## Next: 0.9.1

0.9.1 reads the monitor's EDID: over GMBUS if the VBT says the HDMI
socket is driven directly, or over DP AUX if it sits behind an LSPCON.
Which one, and which pin, is what `gfx` will say on the HP.
