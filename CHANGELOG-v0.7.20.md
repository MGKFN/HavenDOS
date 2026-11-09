# HavenDOS v0.7.20 — the PCI foundation for USB and audio

This supersedes the placeholder v0.7.20 (the version-bump-only image built to
test the v0.7.19 updater). It is the first instalment of the music player:
the groundwork both USB and the sound card need before either can exist.

Everything here is tested and the tree builds clean. 303 checks pass.

---

## One PCI enumeration, replacing three

`drivers/pci.c` carried three separate walks of the bus — one for IDE, one for
the RTL8139, one for both NICs — each with its own copy of the config-space
arithmetic and its own hardcoded device list. USB and audio would have made
five.

The bus is now walked **once**, into a table, following PCI-to-PCI bridges
rather than assuming everything sits on bus 0. Every driver asks the table.
The old accessors (`pci_ide_found()`, `pci_scan_net()`, `pci_scan_net2()`)
still work and still return the same answers; they read from the table now.

### The bug all three shared

They enumerated **function 0 only**.

A PCI device may present up to eight functions. On a PIIX3 — QEMU's
southbridge, and the real chipset in a great many machines of that era — they
sit like this:

```
    00:01.0   ISA bridge       header type 0x80: "I have more functions"
    00:01.1   IDE controller   function 1
    00:01.2   USB (UHCI)       function 2
```

So the USB controller was not merely undetected, it was **undetectable**. No
amount of driver work would have found hardware the enumerator never looked
at, and the failure would have looked like a broken USB driver rather than a
blind scan. Confirmed against QEMU, which reports exactly that layout:

```
    Bus 0, device 1, function 2:   IRQ 11, pin D
        BAR4: I/O at 0xc540 [0xc55f]
```

### A second bug, found by the test

`pci_init()` cleared `ide_found` but left the IDE port variables holding the
previous scan's values. "No controller found" therefore no longer implied
"using the legacy ports", and a second call would have handed ATA the
addresses of hardware that was not there.

### New interface

```c
int              pci_count(void);
const pci_dev_t *pci_get(int i);
const pci_dev_t *pci_find_class(cls, sub, progif, nth);
const pci_dev_t *pci_find_id(vendor, device);
uint16_t         pci_bar_io(d, n);
uint32_t         pci_bar_mem(d, n);
void             pci_enable(d, bus_master);
uint32_t         pci_cfg_read32(...);   /* and write32, read16, write16 */
```

`pci_enable()` exists because a device that is not a bus master cannot do DMA,
and DMA is how both USB and audio move their data. `pci_cfg_write16()` reads,
splices and writes back, because config space is written 32 bits at a time and
a naive 16-bit write to the command register clobbers the status bits beside
it — there is a test for exactly that.

---

## Drivers can now be tested without booting

`include/io.h` gained a real include guard (`IO_H`) alongside its `#pragma
once`. Every driver in this tree reaches hardware through those six functions
and nothing else, so with them replaceable a driver compiles natively against
a **simulated device** and can actually be tested.

`tests/test_pci.c` is the first: it builds a config space matching a real
PIIX3, multifunction bit and all, and runs the shipped `pci.c` against it. 24
checks, in `make test`. The same technique is what will test the USB
controller, which is otherwise only observable by booting a VM and squinting
at a screen.

---

## New command: `lspci`

`pci.c` had kept a log of its scan since the beginning and **nothing ever
printed it** — `pci_get_log()` had no callers at all. So the first question
worth asking when a driver finds no hardware, "is the device even there?",
could only be answered by adding print statements and rebuilding.

```
    lspci        one line per device, with what this kernel can drive
    lspci -v     BARs and interrupt line as well
```

In its own file, `shell/lspci.c`.

---

## The build could ship an ISO that lied about its own version

Found by testing the update end to end in a VM, and it had nothing to do with
the updater.

**An ISO carries its version twice.** `/version.txt` is **generated** from
`include/version.h` on every build; the version the kernel reports is
**compiled** into it. Nothing in the Makefile made a version change recompile
anything:

```make
%.pass1.o: %.c include/grub_embed.h include/grub_disk_embed.h
%.o:       %.c include/grub_embed.h include/grub_disk_embed.h include/kernel_elf.h
```

`include/version.h` is not there. Neither is any other header — editing
`vga.h`, `pci.h` or `string.h` rebuilt nothing that included them either.

So bumping the version produced an ISO that **announced v0.7.20 and contained
v0.7.19**. The update manager then behaved perfectly: read the marker, offered
the upgrade, copied the kernel, verified the write, reported success. Reboot,
and the machine is still on the old version.

Nothing in that chain could notice, because every part of it did its own job
correctly. The v0.7.18 note said version.txt and the GRUB titles are generated
"so they cannot drift from the running kernel again" — and that fixed the
generated files while leaving the compiled ones free to drift.

Three changes:

- **`-MMD -MP`**, so gcc records every header each object really included and
  the Makefile reads those back. All headers tracked, not three named by hand.
- **`include/version.h` named explicitly** on both compile rules as well, so
  the intent is visible in the rule rather than only in a generated `.d`.
- **`tools/check_iso_version.py`**, which unpacks the finished ISO and refuses
  the build if `version.txt` and the kernel disagree. It runs at the end of
  every ISO build, and it catches the broken ISO this release was first built
  as.

Verified afterwards: touching `version.h` recompiles 124 files; touching
`pci.h` recompiles everything that includes it.

---

## Tested end to end in a VM

Not just built — installed and driven:

1. Genuine v0.7.19 installed to a blank disk from its own ISO
2. v0.7.20 ISO inserted, `update` run

```
    Running : 0.7.19
    On ISO  : v0.7.20
    Install this update [Y/N]:
```

   "Install this update", not "Downgrade to older version?" — the v0.7.19
   version-comparison fix holding.

3. Kernel written, backup kept at `/boot/havendos.bak`, reboot
4. Title bar reads **HavenDOS v0.7.20**
5. `lspci` on the upgraded machine:

```
    00:00.0  8086:1237  06/00/00  host bridge
    00:01.0  8086:7000  06/01/00  ISA bridge
    00:01.1  8086:7010  01/01/80  IDE controller
    00:01.2  8086:7020  0C/03/00  USB controller (UHCI)
    00:01.3  8086:7113  06/80/00  bridge
    00:02.0  1234:1111  03/00/00  display controller
    00:03.0  8086:100E  02/00/00  e1000 network
    00:04.0  8086:2415  04/01/00  AC'97 audio
```

   and with `-v`, the USB controller's `BAR4 io 0xC540, IRQ 11` and the
   AC'97's `BAR0 io 0xC000, BAR1 io 0xC400` — matching QEMU's own `info pci`
   exactly.

That is the function-2 device the old scan could not see, now listed on a
running machine, with the addresses the driver will need.

---

## Where the music player stands

| | |
|---|---|
| PCI enumeration | **done**, tested |
| `lspci` | **done** |
| UHCI host controller | next |
| USB enumeration and mass storage | after that |
| AC'97 driver | after that |
| WAV playback | then |
| MP3 via minimp3 | last, and needs the x87 FPU enabling |

The order is USB first by request. Worth repeating that the audio half does
not depend on it: a CD or a second hard disk already works with the drivers
in this release, so sound could be brought up first if hearing something
sooner matters more.

### Not yet verified in a VM

`lspci` has not been run inside a booted HavenDOS. The enumeration itself is
covered by the native tests and its expectations were confirmed against
QEMU's own `info pci`, but the command's output has not been seen on screen.
The test accounts on the disk image here have unknown passwords, so reaching
a shell needs a fresh install first.
