# HavenDOS v0.8.5.4 Beta "Atlas" — a disk is named for the bus it is on

Four releases have now shipped a disk announced as being on a bus it is
not on, on screens that ask a person to type YES before something
irreversible happens. This release is the one that makes it mechanical
rather than careful.

## What was wrong, on two real laptops

```
WARNING: No disks found. Attach a disk and reboot.
1 VirtIO disk found - will be auto-selected.
```

Both lines, together, on a machine whose only disk is a 488 GB Samsung
NVMe. Both generated from the same scan, four lines apart.

And on an older HP with one SATA disk, on the confirmation screen:

```
This will be written to disk 0 (VirtIO)
```

while the boot log two screens earlier read `AHCI port 0: ZEB-SD13,
122104 MB`.

## Why it kept coming back

Naming a bus is four lines of `switch`. So every screen that needed it
wrote those four lines, and they disagreed. The v0.8.3.1 post-mortem
found three such copies and fixed it by writing `bus_name()` **in one
place** — a `static` function inside `shell/iso_installer.c`.

Static, inside a three-thousand-line file. So `shell/desktop.c` could not
call it and had its own:

```c
const char *bus = inst_disk_is_ata(i) ? "ATA/SATA" : "VirtIO";
```

That knows about legacy ATA and calls everything else VirtIO. And the
welcome screen in `iso_installer.c` itself never used `bus_name()`
either — it tested two booleans, `has_virtio` and `has_ata`, and fell
through to the red warning for anything that was neither. On a laptop,
that is every machine.

**The root cause was that there was no header.** `iso_installer.c` owns
the disk table, and thirty-three `extern int inst_disk_count(void);`
declarations scattered through other files is how everyone reached it.
With nowhere to declare a shared function, there was nowhere to put one.

## What changed

**`include/instdisk.h` exists.** The whole disk interface is declared
once, the thirty-three externs are gone from `desktop.c`, and
`inst_disk_bus_name(i)` is reachable from every screen. Out of range it
returns `"none"` rather than falling through a `default:` arm to
`"VirtIO"` — which is how a welcome screen named a disk that was not
there.

**`inst_disk_bus_counts()`** replaces the two booleans. A summary line
can now describe a machine with only NVMe, because it is counting what
was found rather than testing for two particular buses.

**Four label sites are now one.** The welcome screen says `Found: 1 NVMe
disk.`; the auto-select hint no longer names a bus at all, because the
line above it already did; `desktop.c`'s picker and confirmation ask the
same function the ISO installer does.

**One more was found by the new check** and is a real one:
`notif_push("VirtIO disk mounted")` fired for whatever disk had been
mounted. It says `Disk mounted` now — there is no disk index to hand
there, so it says the true thing it knows rather than guessing.

### The check, because a comment did not hold

`tools/check_bus_label.py` fails the build if the words `VirtIO`, `NVMe`,
`SATA`, `AHCI`, `IDE` or `ATA/SATA` appear as a literal in any `.c` file
outside the one function that owns them. Driver banners and boot-log
lines are exempt by name, with the reason written down; a single line can
opt out with `bus-label-ok: <reason>` in a comment, the same convention
`make_version.py` uses for version literals, and **the reason is
required** — a bare marker is how an exemption becomes a habit.

It found three sites on its first run. One was the notification above.
Two genuinely describe the VirtIO block driver rather than labelling a
disk, and carry the marker.

## The other thing that stopped an install

**Every install menu entry passed `live`.** `live` means what
`kernel/main.c` says it means — no disk probed, nothing mounted, RAM only
— and the installer refuses to write in a live session:

```
Installation Failed
Nothing further was written.
a live session can preview but not write -- boot the "Install to Disk" entry
```

Which was the entry that had just been booted. The ISO could not install,
from any entry, and the one screen that could have explained it was
pointing at itself. Found on a real laptop after typing YES on the
confirmation.

`live` is right for the entries that only look around — desktop, shell,
safe mode — and wrong for anything that writes. It is off the install
entries, in the shipped menu and in `tools/make_isodir.py`.

## And the error messages that said nothing

`shell/desktop.c`'s installer reported `unknown error / code -14`.

−14 is not a failure. In the whole-disk installer it means **"this disk
carries a GUID Partition Table"**, and step 1 would have zeroed LBA 0..63
and the last 33 sectors — the GPT header, the entry array and the backup
header. On the machine it fired on, that was a working Linux Mint
install, both copies of its partition table. The guard did exactly its
job and the UI threw the reason away.

That table stopped at −11. The three codes past it are the ones
`fs/install.c` deliberately puts "below every UI so a future caller
cannot reach step 1 by forgetting to check": a live session, an NVMe
namespace with no write path, and a GPT. All three printed `unknown
error`, which is the single least useful thing to tell someone watching a
refusal they could act on.

Worse: the two installers have **two different numberings that overlap**.
`iso_installer.c` says so in as many words and keeps them apart. This
screen did not, and `-9` means "a file write failed" in one numbering and
"the disk did not read back correctly" in the other — so an in-range code
was reported with the wrong meaning. It speaks one numbering now, and
covers every code in it.

## Testing

**1949 checks across 30 suites**, up from 1929.

`tests/test_diskname.c` is new: 20 checks over the label and the counts,
including both real machines' configurations by name.

It carries a **copy** of the two functions, because they sit behind three
thousand lines of VGA and keyboard code that cannot be compiled into a
unit test. A test holding its own copy of the thing it tests proves
nothing — it tests the copy, passes for ever, and goes green while the
real one rots. So `tools/check_diskname_sync.py` compares the two,
normalised, and fails the build if they drift.

| harness | result |
|---|---|
| `check_xhci_suite.py` | 54 of 54 |
| `check_esp_suite.py` | 28 of 28 |
| `check_update_suite.py` | 27 of 27 |
| `check_diskname_suite.py` | **10 of 10** (new) |

The new harness injects into **both copies at once**, because
`check_diskname_sync.py` would otherwise catch a one-sided injection and
nothing would be learned about the suite. The ten are: AHCI named VirtIO
(the v0.8.5.3 bug); NVMe named VirtIO (the v0.8.3.1 bug); ATA named SATA;
AHCI and NVMe swapped, so every individual name still appears somewhere;
an index past the end falling through to VirtIO; a negative index
accepted; the AHCI count added to the VirtIO total; the NVMe count never
incremented; the count loop stopping one short; and a null out-pointer
written to anyway.

Build checks are now **seven**, with `check_bus_label.py` and
`check_diskname_sync.py` joining the five from before.

## Known limitations

- **The desktop's storage panel only describes VirtIO.** On an NVMe
  machine it reports `VirtIO Block: Not detected` and then `Disk Size:
  ---`, beside a mounted 488 GB disk. Those rows are honest about the
  driver they report on — they are not mislabelling a bus, which is why
  they carry the opt-out marker rather than a fix — but the panel is
  incomplete. It needs to describe whichever bus is present, and that is
  a design change rather than a name fix.
- Everything carried from v0.8.5.3.

## What to do with it

The install path works now. On a machine with an existing OS, take the
**additive** install: it adds a partition in free space, writes four
sectors of the partition table, and adds files to the EFI System
Partition without overwriting anything that is not copied and verified
first. The whole-disk install replaces the partition table and will
refuse outright on a GPT disk — and now says so.

A firmware boot entry is still not created for you. That is 0.9.2
"Prometheus". Until then, `bcdedit` on Windows or `efibootmgr` on Linux.
