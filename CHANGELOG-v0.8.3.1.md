# HavenDOS v0.8.3.1 Beta — bugfix release on 0.8.3

Three things 0.8.3 could not do: see a SATA disk, see an NVMe disk, or
install to any disk larger than 2 GB. All three are fixed here. No new
features; the version stays on 0.8.3 because nothing new was promised.

## HavenDOS has booted on real hardware, from its own partition

On **18 October 2026** HavenDOS was installed beside Windows 11 on an HP
laptop's Samsung MZVL8512HELU NVMe SSD, under UEFI firmware, and booted
from its own partition. Windows was unaffected and still boots. That is
the first time HavenDOS has been installed to, and run from, a physical
disk in a machine that was already somebody's working computer.

The install wrote exactly what the preview screen said it would: four
sectors outside the new partition — LBA 1, 3, 1000215184 and 1000215215,
the two GPT headers and one sector of each entry array — plus files added
to the EFI System Partition, with the existing `BOOTX64.EFI` copied to
`BOOTX64.HDB` before being replaced. Every Windows partition was left
where it was.

**Six bugs were found by that one afternoon of running it on metal, and
five of them had survived 1636 checks and eight injection harnesses.**
They are listed below. The lesson is recorded here rather than in a
retrospective nobody reads: a simulated controller answers the questions
you thought to ask it, and the machine on your desk asks the rest.

## The one that affected everybody

**The whole-disk installer refused every disk over 2048 MB.** Not a bus
problem and not new in 0.8.3 — any disk, any controller, any machine.

The partition size was taken as the whole disk minus the first megabyte,
with no upper bound, and computed twice — once for the MBR entry, once for
the format. FAT16 stops at 65524 clusters of 64 sectors, which is 2047 MB,
so `fat16_geom()` refused and the install died at step 4 of 9 with **E15**.

Two things made it worse than a refusal:

- By step 4 the partition table and GRUB had **already been written**, so a
  failed install left the disk unbootable.
- The error screen printed the same sentence for every failure there is:
  *"Check disk is writable and large enough (min 15MB)."* On the most
  common failure — a disk too **large** — it told the reader to go and
  check the opposite of the actual problem.

Now the size is decided once, bounded by `INSTADD_MAX_SECTORS`, and the
geometry is computed and validated **before the first sector is written**.
A 16 GB disk gets a 2047 MB HavenDOS partition and 14 GB left free, rather
than a refusal. Verified on a 16 GB NVMe: installs, `fsck.vfat` reports a
clean filesystem of 47/65495 clusters, and the MBR entry describes exactly
the partition the filesystem occupies.

The additive installer never had this bug — it has had the same bound
since 0.8.3, which is why installing beside Windows always worked and
installing to a blank disk did not.

Every error code now gets its own sentence on the failure screen.

## Disks that could not be seen

- **`drivers/ahci.c`** — SATA as every machine since about 2010 presents
  it. Per-port command list, FIS receive area, command tables and
  scatter-gather, `READ`/`WRITE DMA EXT`, polled on PxCI. Every implemented
  port is enumerated as its own disk. Optical drives are identified and
  refused as install targets.
- **`nvme_write()`** — opcode 0x01 on the existing I/O queue. A partial
  block is **read before it is changed**: on a 4096-byte-block namespace a
  512-byte write would otherwise replace the other seven sectors with
  whatever was in the bounce buffer, and report success.
- **`nvme_writable()`** — NSATTR bit 0, checked before the installer offers
  the disk rather than part-way through writing to it.
- **`include/mmio.h`** — the test seam MMIO never had, which is what makes
  an AHCI driver testable at all.

## Also fixed

- **"no free space — shrink first" on a disk that had just worked.** After
  a successful install the gap is gone, so the message was true and read as
  though the disk were damaged. The installer now finds an existing
  HavenDOS partition by its type GUID and says so.
- **The confirmation screen called an NVMe drive "(VirtIO)".** Four screens
  worked the bus label out independently and only the picker knew about
  NVMe and AHCI — so the picker said `NVMe` and the screen gated behind
  typing YES said VirtIO. All four ask one function now.
- **LBA48 was selected by capacity, not by IDENTIFY word 83** — wrong in
  both directions.
- **The 48-bit ATA paths truncated at 2 TB**: `LBA[39:32]` and `LBA[47:40]`
  were written as literal zeroes.
- **`drivers/ata.c` stopped probing at the first drive it found**, which is
  why a machine with two disks has only ever shown one.
- **`fs/fat16.c` could not read a volume on an AHCI disk** — an installed
  system could be installed to and then could not read itself back.

## Found on the laptop, after the above was already written

**`fs/fat16.c` had no NVMe case at all.** It knew VirtIO, ATA and AHCI.
An installed system on an NVMe drive therefore loaded its kernel through
GRUB and then could not read the partition that kernel had just come
from: `fat16_raw_read()` fell through to the VirtIO reader, which answers
nothing on a machine with no VirtIO. `fat16_init()` returned 0,
`kernel/main.c` read that as *this machine has no HavenDOS partition*, and
ran the installer — on a laptop that had been installed to successfully
ten minutes earlier.

The installer handles NVMe and has done since this release. It has its own
block layer. The filesystem has another one, and only the installer's was
ever taught.

**An optical drive shadowed the real disk.** `ahci_count()` counts a
DVD-ROM, and the bus selection asked only whether the count was non-zero
before addressing index 0. On a machine with both an optical drive and an
NVMe disk, the filesystem bound itself to the DVD, found no HavenDOS
partition, and never looked further. Found in QEMU while verifying the
fix above; it does not affect a laptop with no optical drive, and it would
have hit any desktop. The selection now takes the first AHCI device that
is not ATAPI, which is what the installer's picker has always done.

**Neither interrupt stub passed the frame pointer to its handler.**
`isr_handler(regs_t *r)` and `irq_handler(regs_t *r)` both take a pointer
to the saved frame, and `isr_common_stub`/`irq_common_stub` both did
`call` with nothing pushed — so `r` was whatever sat above the return
address, which is the saved DS selector, `0x10`.

For the panic screen that meant every field was read from low memory
instead of the fault. A panic on the laptop reported `Divide By Zero`,
`EIP 0x0`, and registers holding fragments of GRUB's GCC version string.
The exception *name* comes from `r->int_no`, so the screen did not even
say which exception had occurred. HavenDOS has never been able to report a
kernel fault correctly.

For the IRQ path it was worse, because that one ran about a hundred times
a second rather than only on a fault: `irq_handler` computes
`irq = r->int_no - 32` and then calls `irq_handlers[irq]()`, so the kernel
was selecting a function pointer by an index read out of low memory, on
every timer tick. It also meant IRQ0 almost never reached `pit_tick()`, so
`pit_overflows` — one of the two clocks `boot_ms()` reads — stayed near
zero and the polled clock carried everything alone.

Nothing depended on interrupts working, because HavenDOS polls its
devices, so nothing ever reported them broken.

Verified by triggering a known `int $3`: the screen now reports
`Breakpoint (#3)`, `EIP 0x1016ce`, `CS 0x8`, `EFLAGS 0x212`, and
`nm -n havendos.elf` resolves that address to `kernel_main+0x9ee`, which
is where the instruction was. `kbd_irq` and `kbd_poll` are byte-identical
and the 8042 yields each byte once, so the keyboard cannot double-read now
that IRQ1 genuinely fires.

## Testing

**1657 checks across 26 suites**, and eight injection harnesses — 185
deliberate bugs, each of which the suites must notice, all caught.

`tests/test_fat16nvme.c` is new: the other half of `test_fat16gpt.c`,
with the model disk reachable **only** through `nvme_read`/`nvme_write`
and every other bus reporting nothing — which is what a laptop made this
decade actually looks like. It covers the GPT and MBR layouts, the bus
actually chosen, and a DVD-ROM that must not shadow the disk. No test and
no harness had ever booted an installed system from NVMe; that is why both
of those bugs reached a real machine.

`tests/test_wholedisk_size.c` is also new: 76 checks over the disk sizes people
actually own, from 16 MB to 2 TB, asserting that a geometry exists, that
the partition fits, that it is megabyte-aligned, and that the MBR entry and
the filesystem describe the same extent. Nothing caught the original bug
because the whole-disk path had no test for the size it chooses.

Both new drivers are tested against simulated controllers that execute
their commands rather than mocks that return success.

## Known limitations

- **AHCI has still not run on real silicon.** The laptop this was proved
  on has no SATA controller at all, so the AHCI driver remains tested only
  against a simulated controller.
- **NVMe controller bring-up is no longer untested.** The laptop's boot log
  reads `NVMe 01:00.0 SAMSUNG MZVL8512HELU-00BH1, 488386 MB, 512 B blocks`
  — the reset, admin queue, Identify Controller, Identify Namespace and the
  namespace parse all ran correctly against real silicon, and 488386 MB is
  exactly right for that drive. The gap recorded in
  `tools/check_nvmerw_suite.py` is still a gap in the *test suite*; it is
  no longer a gap in what is known to work.
- **The idle panic is unexplained.** The laptop panicked once while sitting
  at a shell prompt with nobody touching it. The broken IRQ frame pointer
  is the best candidate and is fixed, but it has not been reproduced — 9
  minutes of idle in QEMU did not trigger it, and the value involved comes
  from whatever the firmware left in low memory, so it is machine-specific.
- **No firmware boot entry is created.** HavenDOS writes the removable-media
  fallback `\EFI\BOOT\BOOTX64.EFI`, and firmware with an existing boot
  order will not look for it. On the laptop the entry had to be added from
  Windows with `bcdedit /copy {bootmgr}` pointed at
  `\EFI\HAVENDOS\grubx64.efi`. A real NVRAM entry needs UEFI runtime
  services this kernel does not have.
- **A second install into the same gap is refused.** `esp_install()` stops
  with `ESP_E_BACKUP` when `BOOTX64.HDB` already exists, rather than
  deciding which of two backups matters — so reinstalling means restoring
  the original fallback loader by hand first. Deliberate, but a rough edge:
  the clean fix is to notice the existing fallback is already our own
  loader and skip the backup.
- **`update` cannot run on a machine like the one this was tested on.** It
  reads the new image from an **ATAPI CD-ROM**, and a modern laptop has no
  optical drive, no USB mass-storage support in HavenDOS, and no supported
  network adapter. An update route that reads from the EFI System Partition
  would fit that machine and use only parts that already exist.
- **An installed system still cannot install HavenDOS.**
- **Partitions are capped at 2047 MB** until there is a FAT32 formatter.
- USB mass storage, xHCI and a 1080p console are unchanged.
