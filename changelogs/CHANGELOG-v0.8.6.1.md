# HavenDOS v0.8.6.1

Channel Beta, codename "Argo". A patch release on v0.8.6, and **every
item in it came from one boot on the HP.**

0.8.6 passed 52 of 52 end-to-end checks under QEMU on both controllers,
with the stick on a root port and behind a hub. The laptop found three
things that run of QEMU could not, which is the fourth release running
that real hardware has said something the simulator would not.

---

## What the laptop confirmed first

```
hc0 port 0  PORTSC 0x00000E03
        addr 1  0951:1666  rev 0110  high-speed
        class 00/00/00  mass storage
        interface 0  class 08/06/50  2 endpoints
          ep 0x81  bulk in   max 512  interval 0
          ep 0x02  bulk out  max 512  interval 0

USB disk 0 : Kingston DataTraveler 3.0  59052 MB  [writable, removable]
FAT16 part : mounted on NVMe (persistent)
```

Bulk-Only Transport, SCSI INQUIRY and READ CAPACITY, all correct on real
silicon. The vendor and product strings came off the stick itself and
the capacity is right, so the big-endian parse is right too.

That `FAT16 part` line is 0.8.6's bus-label fix confirmed: it read
`mounted on ATA/IDE` before, on a machine whose only disk is NVMe.

And the disk ordering held — `NVMe ns0` first, `USB disk 0` after. That
is the one thing in 0.8.6 that is a safety property rather than a
feature, and it is now proved on the machine it protects.

**Port 12 reports `empty`.** It had said `attached, but it did not
answer` with `no answer (-4)` since v0.8.5.3. Recorded rather than
explained: one boot is not enough to say whether the hub walk accounts
for it.

---

## THE SLOT LEAK — `usb rescan` ran the controller out of slots

```
[185.176] ok  USB xHCI hc0 port 0: 0951:1666 ... address 4
[185.335] ok  USB xHCI hc0 port 2: 30C9:00C9 ... address 5
[185.460] ok  USB xHCI hc0 port 9: 0E8D:1EDE ... address 6
```

Boot used slots 1, 2 and 3. The first `usb rescan` took 4, 5 and 6.

`TRB_DISABLE_SLOT` was defined in `usb/xhci_hc.c` and **never sent**.
`slot_used[]` was cleared at controller init and on a *failed* Address
Device, and nowhere else. `usb_enumerate()` sets its own device count
back to zero and walks the bus again — but on xHCI the controller owns
the device, so forgetting it on this side frees nothing on that side.

The HP has 8 slots and 3 devices. So the second rescan would have taken
7 and 8, **the third would have lost a device, and the fourth would have
found an empty bus** until the machine was rebooted. The per-slot
transfer rings leaked with them: `ep_dci[slot][]` marks a ring in use
and nothing cleared it, so a reused slot would have been handed a ring
with the previous device's cursor in it — a transfer posted wherever the
last one left off.

This is in 0.8.6's own new `usb rescan` path. QEMU never showed it
because **every check in the suite enumerated once.** 150 of them passed
over it.

`usb_hc_ops_t::detach()` is the fix, and it is an op rather than
something the core does itself for the usual reason: on UHCI an address
is a number this kernel hands out and `usb_enumerate()` resets the
counter, so there is nothing held and nothing to give back. UHCI does
not supply one. That is the same asymmetry `configure()` exists for.

`usb_enumerate()` releases every device **before** clearing the table —
after that there is no record of what the controller is still holding —
and in reverse order, so a device behind a hub is released before the
hub it hangs off.

Two things were caught while writing it, both before they shipped:

- The first version of `x_detach()` cleared the DCBAA entry for the
  slot, **as two 32-bit words rather than the one 64-bit entry the array
  holds** — it would have written over the next slot's pointer. It must
  not be cleared at all: every slot's device context is allocated once
  at controller init and the array points at it for the life of the
  controller.
- `tests/test_xhci.c`'s simulated controller ignored Disable Slot
  entirely, which is why it could not see that the driver never sent
  one. It frees the slot now, and counts them.

**The suite enumerates 32 times** — four times the number of slots —
because every existing check enumerated once. With the fix reverted it
reports 25 lost devices, 25 Enable Slot failures, 8 slots held and zero
Disable Slots sent. Five injections are in `check_xhci_suite.py`, which
is at **58 of 58**.

---

## `fat32` said "none found" where the truth was "exFAT"

The second thing the laptop produced:

```
Kyan@havendos:~# fat32
FAT32 volumes
  disks scanned: 2
   #  disk  start LBA   size     source     BPB label
  0   0     2048        260 MB   GPT ESP    SYSTEM
```

Two disks scanned, and the only volume listed is the laptop's own EFI
System Partition. The 59 GB Kingston is not there — because a stick that
size ships formatted exFAT, and this kernel has no exFAT driver.

The scan read its boot sector, correctly decided it was not FAT32, and
**printed nothing at all.** Which left "this kernel cannot read that
filesystem" looking exactly like "this kernel cannot see the disk" — and
the second is the question somebody staring at that screen is actually
asking.

The installer's drive picker learned this one release earlier:
*"Refused" taught the user nothing; the reason tells them whether
anything they could do would change it.*

So `fat32` now lists every partition it examined and rejected, with what
each one turned out to be:

```
  partitions this kernel does not read:
      disk 0  2048        64    MB  MBR 0x06  FAT16
      disk 0  141312      128   MB  MBR 0x07  exFAT
  exFAT and NTFS have no driver here. A FAT16 one is read
  with 'fat16' rather than this command.
```

`fat_fs_name()` is the new half, beside `fat_probe_type()` in
`fs/fat32.c`. exFAT and NTFS both carry their name at offset 3 — which
is OEMName on a FAT volume — and both zero or omit the BPB fields
`fat_probe_type()` needs, which is exactly why they came back as "not a
FAT volume" rather than as a wrong answer. A partition whose first
sector will not read says so too, because that is a disk problem and not
a filesystem one.

---

## `fat16` is a command now

```
error: 'fat16' command not found  (try 'help' or 'which fat16')
```

Typed on the HP. The 0.8.6 handoff listed it as a known gap and called
it *"a command rather than a driver"*. It is written:

```
fat16                  every FAT16 volume the kernel can see
fat16 ls  <n>          the root directory of volume n
fat16 cat <n>:<path>   a file on volume n
```

```
FAT16 volumes
  disks scanned: 1
   #  disk  start LBA   size     source          bus
  0   0     2048        64  MB  MBR type 0x06   USB

Kyan@havendos:~# fat16 cat 0:ARGO.TXT
HavenDOS 0.8.6.1 read this off a FAT16 USB stick.
```

`fs/fat16.c` holds **one** volume — a device index, a partition LBA, a
BPB and a cached FAT — so looking at a stick means displacing the
system's own and putting it back. `fat16_borrow()` / `fat16_return()`
are that pair, the same shape as `fs/fat32.c`'s mount_ro/release and for
the same reason: the volume most likely to turn up on that screen
belongs to somebody else.

- **Read-only at the driver, not by discipline.** `fat16_borrow()` sets
  a flag that makes `fat16_raw_write()` refuse before it reaches a
  sector — the same reason `fs/fat32.c` is handed a NULL write function.
  A refusal that depends on every caller remembering is not a refusal.
- **The 128 KB FAT cache is not saved.** A kernel with a 255 KB heap has
  nowhere to put a second copy, so the return path re-mounts from the
  saved device and LBA instead. Slower, and it cannot be subtly wrong in
  the way a half-restored cache can.
- **It does not flush on return.** A borrowed volume is read-only, so
  there is nothing of the stick's to write — and flushing would put the
  stick's FAT onto the system's own volume, which is the worst thing
  that function could do.
- **If the system's volume does not come back, it says so** through
  `klog()`. An unmounted filesystem with nothing on screen to explain it
  is worse than a noisy one.

**And the partition walk is not a second copy.** `fat_scan_volumes()` is
`shell/fat32cmd.c`'s existing walk, filtered by FAT type. Writing a new
one here would be the shape of bug v0.8.6 spent its whole release
deleting.

---

## Two more things found on the way

### A header that had never matched its function

`include/fat16.h` declared

```c
int  fat16_list(const char *dir, char *out, uint32_t maxlen);
```

and the function has always been

```c
void fat16_list(void (*cb)(const char *, int, uint32_t));
```

Nothing caught it because `fs/fat16.c` **did not include its own
header**, and the only caller — `fs/vfs.c` — wrote a local `extern` with
the right signature rather than using the wrong one. That is the same
33-local-externs pattern `include/instdisk.h` was created to kill, and
it left a header that would have made any new caller pass three
arguments to a function that takes one. Writing `fat16` was that new
caller.

The header is right, `fs/fat16.c` includes it, and `fs/vfs.c`'s
workaround is gone.

### A build check that could not tell code from a comment

`check_bus_label.py` refused the build over the word "USB" in
`shell/fat16cmd.c`'s **opening paragraph** — a comment explaining why
the command exists.

It classified lines with `line.split("/*")[0]`, which handles a comment
opened and closed on one line and nothing else, so the middle of any
block comment looked like code. `check_blk_dispatch.py` had hit the same
thing on its first run, reporting a sentence in `shell/shell.c` that
mentioned `nvme_write()`, and had grown its own fix.

Two copies of that logic, in a release whose subject was three copies
that had drifted. `tools/csource.py` owns it now and both checks use it.

A false positive is not a checker being careful: it teaches whoever hits
it to add an exemption, and an exemption added to silence a bug in the
checker is one that will hide a real finding later.

---

## Testing

**2376 checks across 35 suites**, up from 2359. `test_xhci` grew from
150 to 167.

All fifteen injection harnesses pass; `check_xhci_suite.py` is at 58 of
58 with the five slot-leak injections added. One injection was
**deliberately not added** — removing `x_detach()`'s `slot > maxslots`
bound — because the `slot_used[]` check below it returns first for any
slot the controller does not hold, so it cannot express a reachable
defect. An injection that cannot is worse than none: it reports a catch
the suite did not earn, which is what that harness's first version did
eleven times over.

`tools/check_harnesses.py`, added in 0.8.6, earned itself immediately:
it refused this build the moment the `klog()` call in `fat16_return()`
left three suites unable to link.

---

## Still open, and still only the laptop can answer

- **The hub path is unproved on hardware.** The Kingston was on root
  port 0, directly. 0.8.6's hub walk works under QEMU and has not run on
  the HP with something actually behind a hub.
- **`usb rescan` four times.** That is the check for this release: with
  0.8.6 the bus empties on the third. It should now survive any number.
- **What the Kingston actually is.** `fat32` will now name it. If it
  says exFAT, that is the answer and the stick needs reformatting to
  FAT32 to be read here; if it says something else, that is new
  information.
- **Port 12**, which now reports empty, for a reason not yet established.
- **AHCI on the friend's SATA laptop** — one line of `dmesg` settles
  whether `drivers/ahci.c` has been on real silicon all along.
