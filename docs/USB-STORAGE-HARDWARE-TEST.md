# USB mass storage on the HP — the hardware test

v0.8.6 "Argo" adds USB mass storage, hub support and a block layer under
all of it. The whole path works under QEMU: `harness/usb_storage.py`
passes 26 checks per topology, 52 of 52 across xHCI and UHCI, with the
stick on a root port and behind a hub.

**None of that is this machine.**

0.8.5 shipped three releases of fixes QEMU could not have found:

- a 64-bit BAR truncated, so the xHCI never started — QEMU will not place
  a 64 KB BAR above 4 GB however crowded the low hole is
- a HID poll that blocked for a second per keystroke, because the
  simulated device always had a report ready and so could never NAK
- an 8042 read loop with no I/O delay, invisible under QEMU because QEMU
  has no SMM

So this is the part only the laptop can answer, and it is about five
minutes.

---

## 1. What you need

- the HP, with HavenDOS installed, or the 0.8.6 ISO on a stick to boot
- **a second USB stick**, formatted **FAT32**, with a file on it
- ideally, a USB hub, so both topologies get tested

A FAT32 stick with a partition table is what every stick sold in the last
twenty years is. A stick you formatted yourself with a tool that writes a
bare filesystem and no partition table will report no volumes, and that is
correct behaviour rather than a fault — see §6.

## 2. Boot, and plug the stick into the side

Boot HavenDOS. **Entry 1, "Shell Mode"**, is enough: USB storage is
probed on live media in 0.8.6, so the ISO works for this and nothing of
yours is touched.

Plug the stick in **before** booting if you can. Nothing in this kernel
runs on a timer, so a stick inserted afterwards needs `usb rescan` — see
§5.

## 3. `usb -vv` — is it on the bus at all?

```
usb -vv
```

Page with the **space bar only**. Every other key is a command name at a
prompt, and `q` quits the shell to the desktop.

**What you are looking for:**

```
hc0 port 5  PORTSC 0x00000603
        addr 2  0781:5567  rev 0100  high-speed
        class 00/00/00  mass storage
        interface 0  class 08/06/50  2 endpoints
          ep 0x81  bulk in   max 512  interval 0
          ep 0x02  bulk out  max 512  interval 0
```

`08/06/50` is the thing: class 8 (mass storage), subclass 6 (SCSI
transparent), protocol 0x50 (Bulk-Only Transport). Two bulk endpoints,
one in and one out.

**If the stick is behind a hub** you should see the hub first, and the
stick indented under it:

```
hc0 port 5  PORTSC 0x00000603
        addr 1  2109:0812  rev 0100  high-speed
        class 09/00/00  hub
            addr 2  0781:5567  rev 0100  high-speed
            behind the hub at addr 1 port 3  tier 1  route 00003
            class 00/00/00  mass storage
```

**`route 00003` is the number that matters.** It is the hub's port, counted
from 1, in the bottom four bits. A wrong route string is a perfectly valid
address to a different socket, so the symptom of getting it wrong is a
device that does not answer — indistinguishable from an empty port, with
no error anywhere. If the stick appears at all, the route was right.

### If nothing appears

`dmesg` is where the walk says what it saw. Three things to look for:

```
USB: <n> hub(s), <m> device(s) behind them
```

If there are hubs and no devices behind them, the hub walk ran and found
nothing — check the physical port.

```
USB hub addr 1 port 4: over-current, powered off by the hub
```

The hub cut the power to that socket because something on it drew too
much. Try another port.

```
USB hub addr 1 port 4: reset failed (-1)
```

The port never came out of reset in half a second. That is a bounded
wait, so it reports rather than hanging.

```
USB: <n> device(s) behind a hub on a controller this kernel cannot
address through one (UHCI)
```

Should not happen on the HP, which has no UHCI. If it does, something is
very wrong with the controller detection.

**Also look at port 12.** Since v0.8.5.3 that port has reported
`attached, but it did not answer` with `no answer (-4)`, unexplained.
0.8.6 walks hubs, so if what is on port 12 is a hub, it should now be
listed. That would close an item that has been open for three releases.

## 4. `disk` — is it a disk?

```
disk
```

```
  USB disk 0 : SanDisk Cruzer Blade  15280 MB  [writable, removable]
  A stick plugged in after boot needs 'usb rescan'.
```

The vendor and product strings come off the stick's own INQUIRY response,
so if they match what is printed on the casing, SCSI is working.

The size comes from READ CAPACITY, in the device's own blocks, converted.
If it is roughly right, the big-endian parse is right too — SCSI is
big-endian on the wire and everything else in the driver is not, so a
little-endian parse here would report something absurd.

**If it says `[read-only: 4096-byte blocks]`**, that is deliberate and
correct. Reads work; writes need read-modify-write, and a torn 4096-byte
block on media that can be pulled out of the machine loses seven sectors
nobody asked to write.

**If it says `[write protected]`**, the stick's MODE SENSE reported the
tab across, or it genuinely is.

**Check the disk numbers.** The USB disk must come *after* the NVMe one.
`fs/blkdev.c` registers removable disks last on purpose: the installer's
confirmation screen names a disk by index, so a stick that renumbered the
fixed disks could shift "disk 1" from a spare drive onto your Windows
partition. If a stick ever appears as disk 0 on a machine that has an
NVMe drive, **stop and report it** — that is the one thing in this
release that is a safety property rather than a feature.

## 5. `usb rescan` — FOUR TIMES, and this is v0.8.6.1's own check

Run it four times in a row and watch the device count:

```
usb rescan
usb rescan
usb rescan
usb rescan
```

Each one must report the same number of devices.

**On v0.8.6 the bus empties on the third.** `TRB_DISABLE_SLOT` was
defined and never sent, so every rescan took fresh slots from the
controller and freed none. This machine has 8 slots and 3 devices: boot
took 1, 2 and 3, the first rescan took 4, 5 and 6 — the addresses are in
the photographs — the second took 7 and 8, and after that Enable Slot
had nothing left to give.

If the count ever drops, or `usb -vv` comes back empty, the leak is
back. `dmesg` will show the enumeration stopping part way through.

## 5b. `usb rescan` — plug it in now

Unplug the stick, then:

```
usb rescan
```

```
Walking the bus again...
1 device.  0 USB disks.  1 disk in all.
```

Plug it back in and run it again:

```
usb rescan
```

```
Walking the bus again...
2 devices, 1 of them behind 1 hub.  1 USB disk.  2 disks in all.
```

That is the whole hot-plug story: there is no interrupt-driven one,
because nothing in this kernel runs periodically. A hub reports port
changes on an interrupt endpoint and nobody is listening.

## 6. `fat32` — read a file off it

```
fat32
```

```
FAT32 volumes
  disks scanned: 2
   #  disk  start LBA   size     source          BPB label
  0   1     2048        15270 MB  MBR type 0x0c   MYSTICK
```

Then:

```
fat32 ls 0
fat32 cat 0:SOMEFILE.TXT
```

**That last line is the acceptance check for the whole release.** Everything
before it could pass on a stick whose descriptors were read and whose data
never was.

### If `fat32` says "none found"

**v0.8.6.1 now tells you why.** It lists every partition it examined and
what each one turned out to be:

```
  partitions this kernel does not read:
      disk 1  2048        59048 MB  MBR 0x07  exFAT
  exFAT and NTFS have no driver here. A FAT16 one is read
  with 'fat16' rather than this command.
```

That line is the answer to "then what IS on my stick". On v0.8.6 this
screen printed nothing at all, which left "this kernel cannot read that
filesystem" looking exactly like "this kernel cannot see the disk".

What it can say:

- **exFAT** or **NTFS** — no driver here. Reformat the stick as FAT32 if
  you want to read it, or use a smaller one; anything over 32 GB ships
  exFAT.
- **FAT16** — use `fat16` instead. New in v0.8.6.1.
- **not formatted** — the partition was allocated and never formatted.
- **could not be read** — a disk problem, not a filesystem one. Check
  `dmesg` for MSC errors.
- nothing listed at all — the stick has **no partition table**.
  `fat32_scan_volumes()` looks for partition entries, which is right: a
  volume found without one would be a volume found by guessing.

### Reading a FAT16 stick

```
fat16
fat16 ls 0
fat16 cat 0:SOMEFILE.TXT
```

New in v0.8.6.1, because somebody typed `fat16` on this laptop and got
"command not found". Read-only at the driver: the volume is borrowed and
the system's own is re-mounted afterwards. `fat16 ls` walks the root
directory only and says so; `fat16 cat` takes a path.

## 7. What to report back

Five lines settle it:

0. whether four `usb rescan`s in a row all report the same device count


1. the `08/06/50` line from `usb -vv`, and the `behind the hub` /
   `route` line if there was a hub
2. the `USB MSC` line from `dmesg`
3. the `USB disk 0` line from `disk`, and the disk numbers of everything
   else
4. whether `fat32 cat` printed the file

If all four are right, 0.8.6 is proved on real silicon and 0.8.7 can
start on the USB Ethernet adapter — which will be on the same bus, very
probably behind the same hub.

And one more, for an item that has been open since 20 October: **does
port 12 say anything different now?**
