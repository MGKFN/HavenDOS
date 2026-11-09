# HavenDOS v0.8.6 — "Argo"

Channel Beta. RM-1003's 0.8.6: **USB mass storage.**

```
kernel   havendos.elf
ISO      HavenDOS-0.8.6.iso   (BIOS + UEFI)
```

The acceptance check passed under QEMU, on both controllers and both
topologies:

```
admin@havendos:/# fat32 cat 0:ARGO.TXT
HavenDOS reached this file over Bulk-Only Transport.
```

That file came off a FAT32 USB stick, **behind a hub**, through SCSI over
Bulk-Only Transport, through a route string in an xHCI slot context, out
of a block layer that did not exist last release, and through a partition
table read off the stick itself. `harness/usb_storage.py` runs it: 26
checks per topology, 52 of 52 on xHCI and UHCI.

**Unproved on the HP.** QEMU's xHCI is not the HP's, and 0.8.5 shipped
three releases of fixes QEMU could not have found — a 64-bit BAR
truncated, a blocking interrupt poll, an 8042 read loop with no I/O
delay. Booting it on the laptop with a stick in is what closes 0.8.6.

---

## The block layer, because the label was never the whole bug

v0.8.3.1's lesson was written down as *"the installer had one block layer
and the filesystem had another, and only one of them got taught about
NVMe."* The fix that release made was to write the bus **label** in one
place, and `check_bus_label.py` has guarded the words ever since.

Adding a fifth bus meant following this tree's own rule — *when you add a
bus, grep for every place that chooses one.* There were **three**:

| site | knew about |
|---|---|
| `shell/iso_installer.c` `disk_read/disk_write` | VirtIO, ATA, AHCI, NVMe |
| `fs/fat16.c` `fat16_raw_read/write` | VirtIO, ATA, AHCI, NVMe |
| `fs/fat32.c` `local_read/local_write` | **VirtIO and ATA** |

So `fat32_init()` — the entry point for "a FAT32 partition on the disk
the machine already knows how to read" — **could not see an NVMe or an
AHCI disk at all**, which is to say it returned "no disk" on every laptop
made this decade. Two releases after the lesson was recorded. The lesson
was in a comment and the third copy was never looked at.

Their scan **orders** disagreed too:

```
the installer   VirtIO, NVMe, AHCI, ATA
fat16_init()    VirtIO, ATA, AHCI, NVMe
```

On a machine where both the NVMe and the legacy ATA ports answer, the
installer called the NVMe disk 0 and the filesystem bound itself to the
ATA one. Nothing on any screen could tell you.

`fs/blkdev.c` owns all of it now: the table, the dispatch, the per-disk
refusal, the bus word, and **both** orders — because they are two
questions and both answers are needed.

- **Registration order is identity.** It is what the installer's picker
  shows, what `fat32_vol_t::disk` stores, and what a person reads off the
  screen that asks them to type YES.
- **Boot preference is "which of these disks is this machine's own"**,
  which `fat16_init()` and `fat32_init()` ask and the installer never
  does.

Each is one function, with the argument for its order written beside it.

`shell/iso_installer.c`, `fs/fat16.c` and `fs/fat32.c` are callers.
`tools/check_blk_dispatch.py` refuses a raw sector call outside
`fs/blkdev.c` — it is the half `check_bus_label.py` could never see, and
on its first run it found six dead `extern` declarations in the installer
that nothing called any more.

### A stick must not renumber a fixed disk

This is the one property the layer exists to hold, and it is a safety
property rather than a tidiness one.

The installer's confirmation screen names a disk by **index**, and so
does the dry-run plan `plan_disks()` caches. If plugging a stick in
between the picker and the confirmation could shift "disk 1" from a spare
drive onto somebody's Windows NVMe, the four-sectors guarantee this
release ships on would be worth nothing.

So `blk_scan()` registers every fixed bus before any removable one, and
`tests/test_blkdev.c` holds it as a check: two sticks arrive, and every
fixed disk keeps its index, its bus and its size.

### Raising the bound broke a screen in another file

The old disk table held 8. `fs/blkdev.c` holds 16, because a hub of
sticks fits now.

The drive picker drew `i < ndisks && i < 10` and navigated
`sel < ndisks - 1`. With 8 that was slack. With 16 it is not: on a
machine with eleven disks **the selection could move onto a row that was
never painted** — an installer whose cursor is somewhere the user cannot
see, on the screen before the one that asks them to type YES. Three
`MAX_DISKS`-sized plan caches would also have been written past.

One constant, `PICK_MAX_ROWS`, used by the draw loop *and* the key
handler, and a line that says how many disks are not shown. A list that
is quietly truncated is how somebody installs to the wrong disk.

---

## `usb/msc.c` — and why the recovery paths are the substance

A stick has two bulk endpoints and no state machine worth the name: a
31-byte Command Block Wrapper out, the data, a 13-byte Command Status
Wrapper in. That is the whole protocol.

What makes it 600 lines is phase 3. A SCSI command a device cannot do
**stalls the data endpoint and then still has a CSW for you.** A driver
that treats a stall as a transfer error loses that wrapper, the next
command reads the *previous* command's status, and from there every
answer is one command out of step — which on a write is the difference
between "it worked" and data somewhere else.

That is not hypothetical. It is the shape of what 0.8.5.1 shipped (a poll
that left a live TRB behind) and of v0.8.5.3's (a status register read
before it had caught up). So the tag is treated as load-bearing:

- every CBW carries an incrementing tag, and a CSW whose tag does not
  match is **not believed**
- a stalled data phase is recovered and the CSW still collected
- a stalled status phase is cleared and asked once more — exactly once
- a phase error, a bad signature or a mismatched tag is a Mass Storage
  Reset and a failed command, not a guess
- the reset clears the halts on **both** endpoints: the class reset is
  the device's half and the halts are the host's, and one without the
  other leaves half the pipe thinking the command is still live

Plus the things real sticks do: almost every one refuses its first
command with UNIT ATTENTION (believing that reports a working stick as
absent), plenty do not implement MODE SENSE at all (treating a missing
optional command as a fault rejects working hardware), and a card reader
with an empty slot says NOT READY / medium not present, which is a slot
and not an error.

### Deliberately not done, and said here rather than found later

- **Writes to a device whose block size is not 512 are refused.** Reads
  work at any size up to 4096, by offsetting into the device block.
  Writing would need read-modify-write, which `drivers/nvme.c` does —
  but an NVMe drive cannot be pulled out of the machine half way
  through and a stick can. A torn 4096-byte block loses seven sectors
  nobody asked to write. The disk reports read-only and the installer
  already has a row for it: *"preview only - no write path"*.
- **One LUN.** `GET_MAX_LUN` is asked and the answer logged; a card
  reader with four slots shows the first.
- **No hot-plug.** Nothing in this kernel runs periodically, so a hub's
  change endpoint is never polled. `usb rescan` re-walks the bus,
  re-probes the devices and rebuilds the block layer's table, in that
  order.

---

## `usb/hub.c` — a stick is not an internal device

0.8.5 walked root ports only and said why: *"a laptop's internal keyboard
and trackpad are usually on root ports, which is why this was judged
enough for now -- but it is an assumption until somebody boots it."* The
HP bore it out — 16 root ports, devices on 2 and 9, both internal.

A USB stick goes in the socket on the side, and on most laptops that
socket is behind an internal hub. The 20 October handoff named a hub as
the first suspect if a stick did not appear, and a release whose whole
subject is removable storage cannot ship with that as its acceptance
risk.

**The class is cheap. The controller is not, and the two disagree
completely.**

- **UHCI** — nothing. A hub is transparent; a device behind one is
  addressed by its address like anything else.
- **xHCI** — the controller owns the device, so it has to be *told* the
  path. The slot context carries a 20-bit **route string**: four bits per
  tier, port numbers **one-based**, tier one in bits 0..3.

Getting the route wrong does not look like a bug. An off-by-one in the
shift, or zero-based ports, produces a **perfectly valid route string
pointing down a different socket** — so the device does not answer, which
is indistinguishable from an empty port. There is no error to read, and
QEMU is as happy with a wrong route as a right one.

The same is true of the transaction translator: a low or full speed
device behind a high speed hub needs the hub's slot and port in the slot
context or the controller issues full-speed traffic at high-speed timing.
Silence again.

So the arithmetic is in `usb/core.c`, beside the device table, because
the core owns how a device is addressed — a hub driver doing it itself
would be the second place that knows, which is the mistake
`include/usb.h` was written after. `usb_hc_ops_t::attach_path()` is the
asymmetry, and **both** back-ends implement it: `usb/uhci_hc.c` does its
own SET_ADDRESS through `drivers/uhci.c`'s unchanged public transfer
calls, because an interface with a single implementation is a wrapper
that proves nothing.

`tests/test_hub.c` watches what the back-end is *handed* — every
`attach_path()` call, with its route, root port and translator — rather
than whether it succeeded.

---

## Testing

**2359 checks across 35 suites**, up from 2055 across 32. Three new:
`test_blkdev` (81), `test_msc` (114), `test_hub` (92).

Three new injection harnesses, and all 15 pass: 20, 40, 32 injections for
the new ones. Running all fifteen in a row **for the first time** found
two that had stopped working.

### `check_nvmerw_suite.py` had not built since v0.8.5.1

v0.8.5.1 moved the 64-bit BAR decoder into `drivers/pci_bar.c` — *"a stub
is not the function"* — and the Makefile learned to link it into the four
suites that need it. That harness's own compiler line did not, so every
run since ended in

```
undefined reference to `pci_bar_mem64'
```

and reported **nothing at all**. Five releases. The same disease v0.8.5.6
found in `make test`: a check nobody reads the output of. The 20 October
handoff's table of harness results does not list it, which is how it went
unnoticed.

`tools/check_harnesses.py` is in the `make` checks now and refuses a
harness whose baseline does not build. Building all fifteen is seconds;
running them is minutes, so this is the cheap half — and it is the
failure that actually happened.

### The harness loop lives in one place now

`tools/check_xhci_suite.py`'s first version restored only the file it was
about to damage, so once a bug was injected that file stayed broken for
every injection after it: **eleven injections "caught" the leftover and
one of them was a real miss hiding in the pile.** That bug is in the
loop, not in the list of defects, and five harnesses with five copies is
five chances to write it again — in the release whose whole subject is a
thing copied three times that had drifted in all three.

`tools/inject.py` owns the loop. Each harness is a list of defects.

And the **self-check is part of it**. `check_machine_suite.py` reported
all 20 caught on its first run and was checked by hand: a harmless
injection appended last, correctly reported MISSED, proving both that
MISSED is reachable and that the damaging injections had been fully
restored. Done by hand that is something somebody remembered once. Every
harness now declares a harmless change, it runs last, and the harness
**fails if it is reported as caught**.

### The new harnesses found ten holes in the new suites

All three suites passed on their first run, which by this tree's rule is
the least trustworthy result available. The pattern was consistent: **the
suite was checking the outcome where it needed to check the wire.**

- *"a write to a read-only device is attempted"* — MISSED, because the
  simulated stick refuses the write itself, so counting completed writes
  cannot tell the driver's refusal from the device's. It counts commands
  sent now.
- *"a null buffer reaches the transfer"* — MISSED for the same reason:
  without the guard the command goes out, the data phase is skipped
  because the pointer is null, the status read collects sector data as a
  wrapper and the signature check fails. Result 0 either way.
- *"the halt is not cleared after a stalled data phase"* — MISSED,
  because the driver's stalled-status path clears it on the way past, so
  the command's result is identical. The simulated endpoint now refuses
  **everything** while halted, as a real one does, and counts the
  refusals — so the check is that the halt was cleared *before* the next
  transfer, not eventually.
- *"the tag is not incremented"* — MISSED, because a device that echoes
  what it is given cannot tell a fresh tag from a reused one. The device
  records every tag it was sent and the suite asserts no two commands
  shared one.
- *"a CD-ROM is offered as writable"* — MISSED, because the test used
  2048-byte blocks, which are read-only for a **different** reason. One
  reason per check.
- *"low and full speed are swapped"* — MISSED, because both get a
  translator, so checking for one could not see it. The suite asserts
  the speed number handed over.
- Three hub injections were toothless and were replaced; one was dropped,
  because the copy-versus-pointer issue it modelled cannot be expressed
  as a one-line change and an injection that cannot express a real defect
  is worse than none.

### And the hardened simulator found a bug in the code

Making `tests/test_hub.c`'s back-end scribble its own path numbers — as a
real one could — showed that `usb_attach_behind()` **restored five of the
six path fields and not the route string it had just built.** A back-end
could have clobbered it and the core would not have noticed.

`tests/test_diskname.c` links `fs/blkdev.c` now instead of carrying a
hand-kept copy of the function it tests, so `check_diskname_sync.py`
changes job: it used to compare the two copies, and now it makes sure the
copy cannot come back, and that both files' bus words agree with the
table *it* holds. The suite covers the logic; the check covers the text.

---

## Bugs found elsewhere while doing this

### `disk` reported an NVMe volume as ATA/IDE

```
  FAT16 part : mounted on ATA/IDE (persistent)
```

came from `fat16_on_ata()`, which only ever meant *"not VirtIO"*. So an
installed system on an NVMe laptop — the machine this is developed on —
reported its own volume as mounted on ATA/IDE, on the screen somebody
checks to find out where their disk is. The fourth time this bug has been
found, and the first time the fix is to **ask**: `blk_bus_name(fat16_dev())`.

### A live session now probes USB storage, and says so precisely

The first version of this release put `msc_probe()` behind the `g_live`
guard with every other disk probe. The result was a live shell in which
`usb -vv` listed a mass-storage device and `disk` said there were none —
the same contradiction v0.8.5.3's welcome screen shipped.

Live mode exists to leave **the machine's** disks exactly as they were:
somebody's Windows install, somebody's Linux Mint. A stick the person has
just plugged into the side is not the machine, and it is the reason they
booted the ISO. So it is probed, and both the boot log and the `disk`
screen now state the guarantee they actually make instead of "no disk is
probed, mounted or written", which had stopped being true:

```
  LIVE SESSION - this machine's own disks are not probed, mounted or
                 written. A USB stick you plug in is yours and is listed.
```

`fat16_init()` and `fat12_init()` still do not run, the VFS still routes
every write to RAM, and nothing mounts a partition that was on the
machine.

### The harness was paging with a key that does not exist

`q.key("space")` — QEMU's QKeyCode is `spc`. `send-key` with "space" does
nothing at all, so the first version of `harness/usb_storage.py` never
paged: every capture was the same first screen, forty times over, and two
checks failed on a run where the driver had worked perfectly. A key name
that is silently ignored is worse than one that raises.

---

## Carried forward, not done

- **Everything above, on the HP.** Boot it with a stick in. `usb -vv`
  should show the stick, with `behind the hub` and a route string if it
  is behind one; `disk` should list it; `fat32 ls 0` should read it.
  Port 12's `no answer (-4)` is still unexplained and a hub walk may now
  say more about it.
- **A FAT16 volume on a stick is not reachable from the shell.** The
  disk is registered and the installer can see it, and `fat32` reads a
  FAT32 volume on it — but there is no `mount` for a FAT16 partition on
  a disk that is not the boot disk. Most sticks over 32 MB are FAT32, so
  this is narrower than it sounds, and it is a command rather than a
  driver.
- **exFAT and NTFS are not read at all**, which is what a stick over
  32 GB usually is.
- **A read-only ACPI reporter** — find the RSDP, walk the RSDT/XSDT,
  print `PM1a_CNT_BLK`, `RESET_REG` and `RESET_VALUE` from the FADT.
  `poweroff` works on the HP, which means one of three hardcoded
  emulator addresses happens to be live on that chipset. Three addresses
  that happen to match one laptop is the same bug class as nine reset
  sites that happened to match QEMU.
- **The real `isodir/` is still not in the archive.** What ships is
  `make_isodir.py`'s reconstruction.
- **AHCI's status is still uncertain in a good way.** HavenDOS is
  installed on a SATA laptop somebody uses daily; one line of `dmesg`
  from that machine settles whether `drivers/ahci.c` has been on real
  silicon all along.
