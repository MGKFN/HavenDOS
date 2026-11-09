# HavenDOS v0.8.2 Beta "Bedrock"

**Phase A of the 1.0 build plan: FAT32.**

An EFI System Partition is FAT32 and HavenDOS had FAT12 and FAT16 only.
Everything in THS-RM-1001 that installs beside another operating system goes
through an ESP, so this is the floor the rest of the plan stands on. USB
sticks and essentially everything else formatted this decade are the same
filesystem, which is the second reason.

Nothing else in the plan has been started. The phase list is where 0.8.1
left it, minus Phase A.

---

## What shipped

| | |
|---|---|
| `fs/fat32.c` | the driver: read, write, long names, FSInfo, subdirectories |
| `include/fat32.h` | the interface, and why it is not a generalisation of `fat16.c` |
| `include/fat32vol.h` | finding FAT32 volumes on real disks — what Phase B's B2 needs |
| `shell/fat32cmd.c` | the `fat32` command, read-only, and the volume scan behind it |
| `tools/make_fat32_fixture.py` | builds the test images and emits the manifest |
| `tools/check_fat32_suite.py` | injects 26 real FAT32 bugs and checks the suite notices |
| `tests/test_fat32.c` | 227 checks against an image this project did not format |
| `harness/fat32_boot.py` | the same driver, through a real boot |
| `harness/fat16_regress.py` | proof that installed FAT16 systems are unaffected |

`make test` → **1014 checks across seventeen suites** (787 at 0.8.1).

## It is a separate driver, deliberately

`fs/fat32.c` sits beside `fs/fat16.c` rather than generalising it. FAT16
works today and is what every installed HavenDOS boots from; putting that
code on the critical path of a new filesystem buys nothing a user can see.
The plan says the same thing in its Phase A risk note. Factor them later, if
ever.

The differences that actually bite, all handled:

- **`BPB_FATSz16` is zero and the real size is in `BPB_FATSz32`** — but
  offset 36 is only that field on FAT32. On FAT12/16 it is the start of the
  extended boot record, so the 16-bit field has to be read FIRST and the
  32-bit one consulted only when it is zero. Reading them the other way
  round reports a 16 MB FAT16 volume as having a FAT of 1,529,413,760
  sectors. Found by the fixture generator making exactly that mistake on its
  first run; the same rule is now in the driver, the generator, and the
  injection harness.
- **`BPB_RootEntCnt` is zero and the root is an ordinary cluster chain** at
  `BPB_RootClus`. A FAT32 root can grow. Code that assumes a fixed root
  region finds whatever fits in the first cluster and stops.
- **FAT entries are 32 bits of which only the low 28 are the cluster.** The
  top four are reserved: masked off on read, preserved on write.
- **The FAT is too big to cache whole.** `fat16.c` holds all of it in a
  128 KB array, which is viable at 65,524 clusters and is not viable here —
  the FAT on the 64 MB test volume alone is 497 KB. This driver keeps a
  one-sector window with write-back, mirrored to every FAT on flush.
- **FSInfo** carries a free-cluster count and a next-free hint. They are
  maintained, and written back as `0xFFFFFFFF` ("unknown", which is legal and
  which every implementation recomputes from) the moment the count cannot be
  accounted for. A stale free count is how a volume comes back wrong under
  Windows.
- **Long file names** are read properly rather than merely skipped:
  sequence numbers, the checksum that ties a run to the 8.3 entry after it,
  and UCS-2 folded to ASCII. They are also *written*, which the plan says
  could wait — see below for why it did not.
- **The NTRes case flags.** `mkfs`/`mcopy` store `readme.md` as an
  all-upper-case 8.3 name with two case bits set and no long name at all. A
  driver that ignores them reports `README.MD` and never matches what the
  user typed.

## The dotfile bug is not inherited

`name_to_83()` in `fs/fat16.c` turns `.desktop` into a **blank basename**
with extension `DES`. Every dotfile HavenDOS writes to a FAT16 volume today
is a directory entry with an empty name field — `.accounts`, `.theme`,
`.shell_history` and six more. No two collide yet, which is the only reason
it has been survivable.

**That bug is still there. It is Phase G and this release does not touch
it**, because destabilising a working FAT16 to tidy a new FAT32 is the trade
the plan warns against.

What this release does is not repeat it. A name that is not 8.3-legal gets a
generated short name with a `~N` tail *and a long name beside it*, so
`.config` is `.config`. That is why LFN **writing** landed in this phase
rather than waiting: the alternative is silently storing a file under a name
the user did not ask for and cannot look it up by, which is the dotfile bug
again in a new filesystem. The suite asserts, reading the raw directory off
the disk rather than through our own renderer, that no live entry this
driver writes has a blank name field.

## Mounting and routing

- `vfs_init()` tries FAT16 first, **then** FAT32, then FAT12. That order
  means this release cannot change the behaviour of any disk that works
  today: on every installed HavenDOS `fat16_detected()` is true and the new
  code is never reached. It also means a machine with both a HavenDOS
  partition and an ESP mounts the HavenDOS one, rather than losing every
  saved setting to a partition that belongs to somebody else.
- **The live-media guarantee is unchanged and matters more now.** Handoff
  section 13b exists because an ESP is FAT, sits first on the disk, and is
  what a careless probe lands on. Until this release HavenDOS could not read
  one even if it tried. `fat32_init()` is called below `vfs_init()`'s live
  check and from nowhere else in the tree.
- The partition type byte is a hint, never an answer. `0x0B` and `0x0C` are
  what the scan looks for first, and every candidate's boot sector is read
  and its clusters counted before it is called FAT32 — a partition labelled
  something else that really is FAT32 still shows up.
- `disk` reports the FAT32 volume with the cluster count and size it read
  off the volume, and says "unknown" rather than a number when FSInfo
  carries no free count.
- The boot log reports a FAT32 mount in its own line. Before this, a system
  running on FAT32 would have been told "no HavenDOS partition mounted".

## The `fat32` command

```
fat32                  every FAT32 volume the kernel can see, on every disk
fat32 ls  <n>[:dir]    list a directory
fat32 cat <n>:<path>   read a file
```

**Read-only at the driver**, not by discipline: `fat32_mount_dev()` is handed
a NULL write function, so every write entry point refuses before it reaches
a sector. The partition most likely to turn up here belongs to whatever else
is installed on the machine.

This is the thing to run first on the HP. It exercises the whole stack —
PCI, the unified disk scan, the partition walk, type detection, the mount,
the directory iterator and LFN reassembly — in one line, on live media,
without writing anything.

## How it was tested

**Against somebody else's FAT32.** The fixture is made by `mkfs.vfat` and
populated by `mcopy`. A FAT32 driver checked only against volumes it created
itself proves it is self-consistent, which is not the claim anybody needs.
The geometry the suite asserts is **read back off the boot sector**, not
taken from the mkfs command line, so what is proved is agreement with
mkfs.vfat rather than agreement with our own fixture script.

**The fixture is generated by the build.** `tests/test_fat16path.c` has
never run: it skips unless `BOOTDISK` names an installer-produced image, no
such image was ever in the tree, and the skip notice was being swallowed by a
`tail -1` in the Makefile — so it sat in `make test` output with no result
for three releases. The rule that produces is that a fixture a suite needs
is generated by the build, or the suite is decoration. The FAT32 image is a
Makefile prerequisite and its absence is a build failure with the package
name in the message, not a skip.

**Writes go to a scratch copy and `fsck.vfat` is the arbiter.** After
writing files, directories, long names and dotfiles, the partition is
extracted and `fsck.vfat -n` must report it clean.

**26 real bugs were injected into the driver to check the suite notices.**
`tests/test_uhci.c` was validated this way with 21 injections and three of
its checks exist because that exercise found gaps; this is the same exercise.
It was worth doing: the suite reported **168/168 on its first run and missed
10 of the 26**. The misses were real holes —

- the fixture's cluster numbers all fit in sixteen bits, so dropping the
  high half of the directory entry's cluster field read the whole volume
  correctly. The suite now pushes an allocation past cluster 65535.
- every FAT entry `mkfs` writes has a zero top nibble, so a driver that
  never masks and never preserves looks right. The suite now sets those bits
  by hand and checks both halves.
- nothing in the fixture had an orphaned or out-of-order LFN run, so the two
  checks that reject one were untested. The suite now builds both cases on a
  copy it then throws away.
- nothing counted a directory listing after a delete, so a scan that failed
  to skip `0xE5` entries went unnoticed.

Two of the injections turned out not to be bugs at all, and that is recorded
rather than papered over: the field-order trap cannot reach the mount path
because `fat_probe_type()` gates it, and a redundant leading-dot guard in
`fits_83()` hid the real guard from the harness. The redundant line was
removed so the guard that does the work is the one under test, and a check
that a FAT16 volume is refused a FAT32 mount was added, because that gate is
now load-bearing.

All 26 are caught.

**Through a real boot, twice.** `harness/fat32_boot.py` boots the ISO in
Shell Mode with a FAT32 disk attached and drives the `fat32` command: the
volume is found, a long name is reassembled, a dotfile keeps its leading
dot, a nested directory walks, a file reads back — and the attached disk is
byte-for-byte identical afterwards, checked by MD5. The same passes under
OVMF, which is the path that matters for the HP.

**And the thing most likely to break was checked rather than reasoned
about.** `vfs_init()` is the single most load-bearing function this phase
touched. `harness/fat16_regress.py` installs the ISO onto a blank disk,
boots the installed system, and reads the boot log: FAT16 is still mounted,
the VFS has not fallen through to RAM, and the new FAT32 branch does not fire
on a disk with no FAT32 on it.

## neofetch stopped lying

Not part of Phase A, found while making a release screenshot.

`cmd_neofetch_v2()` described the machine in string literals:

```c
{ "Net:     ", "RTL8139 / e1000 (QEMU SLIRP)" },
{ "Disk:    ", "VirtIO Block (UTM SE)"        },
```

It printed those on every machine. On a real PC with an IDE disk and no
network card it claimed a VirtIO disk and named two NICs that were not
there. That is the same fiction 0.8 Beta removed from the boot screens, and
`tools/check_boot_log.py` only scans the boot path, so this screen kept doing
it for three more releases.

It is `shell/neofetch.c` now, and every line is a value something was just
asked for: uptime from `boot_ms()`, memory from the physical allocator, the
CPU from CPUID's brand string (falling back to the vendor id, not to a guess),
the framebuffer and text grid from the console, the disk from whichever bus
actually answered and its real size, the filesystem from what the VFS
mounted — which since this release has three possible answers — the NIC from
`e1000_found()`/`rtl8139_found()` with the address it holds, and the package
count from the bpkg database. Where the answer is nothing, the line says
"none found" or "no NIC" instead of naming absent hardware.

The layout is the usual one, logo left and keys right, and the version line
carries the channel and codename: `HavenDOS v0.8.2 Beta "Bedrock"`.

## Not in this release

- **Writing to a real disk's FAT32 partition from the installer.** The
  driver can write and is proved to; nothing wires it to an install. That is
  Phase B, and the plan's sequencing rule is explicit that the refusals stay
  as they are until the additive installer is proved.
- **`rmdir`, and rename on FAT32.** `vfs_rename()` returns 0 on a FAT32
  volume rather than quietly calling the FAT16 one, which is not mounted.
- **exFAT, NTFS, and writing long names on FAT16.** All still out of 1.0.
- **Timestamps.** Directory entries are written with zero dates rather than
  invented ones. There is an RTC driver in the tree and wiring it in is an
  honest separate change.
