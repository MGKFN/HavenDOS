# HavenDOS v0.7.12 — full-tree audit

A cold read of all 26,575 lines, with every fix traced to a root cause. No new
features. Built, booted, installed and verified in QEMU; an unmodified v0.7.11
ISO was built alongside as a control so nothing here is a pre-existing symptom
mistaken for a fix.

Thirty-two defects, one of them fatal. Grouped by how much damage they can do.

---

## FATAL — installs died on the second boot

### `fat12_init()` reformatted the boot disk on every boot — `fs/fat12.c`

**This is the "installs, boots once, then a black screen with a blinking
underscore" bug.** It is not the LBA48 truncation from v0.7.6 coming back.

`fat12_init()` runs unconditionally from `kernel_main()` on every boot, right
after `fat16_init()`. It read absolute sector 0, looked for the OEM string
`HAVENDOS` at bytes 3..10, and if that was absent declared the disk *fresh* and
formatted it.

After an install, sector 0 holds the GRUB MBR — whose OEM field is zeros. So
`fresh` was **always** true on an installed disk, and the function proceeded to
overwrite:

| sectors | contents destroyed |
|---|---|
| 0 | MBR and the entire partition table |
| 1–24 | GRUB `core.img`, including the blocklist at `1:0x1F4` |
| 25–38 | the rest of `core.img` |

Nothing in this file applies a partition offset — FAT12 here assumes it owns the
whole disk from sector 0.

The timing is what makes it look mysterious. The install itself completes
cleanly, because the damage happens on the *next* boot. That boot succeeds,
because GRUB has already loaded the kernel by the time the kernel gets around to
wrecking GRUB. Every boot after it has no MBR. Install, one good boot, then dead
— exactly as reported.

It never appeared in VirtIO testing because `ata_detected()` is false there. It
fires on real SATA hardware and on QEMU with `if=ide`.

Three guards added, and the auto-format is now effectively opt-in:

1. If FAT16 is already mounted, a HavenDOS volume exists — stand down.
2. If sector 0 carries the `0x55AA` signature or any non-zero partition entry,
   the disk is partitioned and belongs to someone else — never format it.
3. Only a sector 0 that is entirely zero may be auto-formatted.

**Reproduced and fixed under test.** Before: after an IDE install, sector 0 was
`eb 3c 90 "HAVENDOS"` with the partition table zeroed and sector 1 a blank FAT.
After: sector 0 is the GRUB MBR with an intact `0x0E` entry at LBA 2048 and the
correct 64-bit blocklist, byte-identical across an install plus three
consecutive boots.

Also fixed here: the BPB was written with unaligned 16-bit stores at offsets 11,
17 and 19 — the same undefined behaviour already removed from `fat16.c`.

### The POST screen reported four things it never checked — `shell/bootanim.c`, `kernel/main.c`

`ATA Bus 0 .... Not present` and `Boot Dev .... CD-ROM GRUB2 Multiboot` were
**hardcoded string literals**. The ATA row said "Not present" on every machine
ever booted, including the SATA laptop it describes.

The NIC row asked `rtl8139_found()` before `net_init()` had run, so the answer
was structurally guaranteed to be no. The VirtIO row read
`virtio_drive_count()`, populated by `virtio_scan_drives()`, which during boot
was never called at all.

Four rows, none of them real. That is worse than showing nothing: it points you
at the wrong layer when something genuinely is broken — and in this release the
thing genuinely broken was the disk. Now all four report actual probe results.

## Corruption and data-loss risks

### VirtIO queue-slot leak — `drivers/virtio.c`

`virtio_drive_read/write()` set `g_qslot` to the target drive's slot and never
restored it. `virtio_blk_read/write()` — the path every FAT16 access goes
through — did not set it at all.

So once anything had touched a second drive, every primary-disk access built its
descriptor chain in `qmem[1]` while the device behind `g_io` was still told, at
init time, to read `qmem[0]`. The device then sees a stale or empty descriptor
table: the request either never completes (the four-round poll in `virtio_do()`
times out) or completes against whatever descriptors were left in slot 0 — the
wrong sector, silently.

This only bites *after* the installer runs, which is exactly the path where a
wrong-sector write is unrecoverable. The primary device's slot is now recorded
at init and restored on every primary access.

### Cluster allocation past the end of the volume — `fs/fat16.c`

`fat_alloc_cluster()` was bounded against the size of the FAT cache but not
against the disk. A FAT is rounded up to whole sectors, so it always describes
more cluster slots than the data area contains; those trailing entries read as
`0x0000` (free). A genuinely full volume therefore handed back a cluster past
the last real one, and `cluster_to_lba()` pointed at sectors beyond the end of
the partition. Now bounded by the real data-cluster count as well.

### 8.3 extension silently discarded — `fs/fat16.c`

`name_to_83()` stopped the basename loop at eight characters but left the
pointer on the ninth, which is not `'.'`, so the `if(*name=='.')` never fired.
Any name longer than eight characters before the dot lost its extension:
`verylongname.txt` and `verylongname.bin` both became `VERYLONG` with a blank
extension — two files collapsing onto one directory entry, the second
overwriting the first.

### ATA read path had no LBA28 ceiling — `drivers/ata.c`

`ata_write_sector()` already refused LBAs past the 128 GiB LBA28 limit.
`ata_read_sector()` did not, so `(lba>>24)&0x0F` quietly discarded the top bits
and returned the contents of a *different* sector as a successful read. Same
silent-wrap class as the LBA48 truncation fixed in v0.7.6, in the opposite
direction. Also added the missing ERR/DF checks to both write paths, which
previously reported success on a rejected command.

### VFS never used the path-aware FAT16 entry points — `fs/vfs.c`

`fat16_write_path` / `read_path` / `exists_path` / `delete_path` have existed
since v0.5.9 and nothing called them. Every `vfs_*` call went to the flat
root-directory version, which hands the whole string to `name_to_83()` — so
`boot/grub.cfg` became the 8.3 name `BOOT/GRU` in the root directory, a file
with a slash in its name that no other FAT implementation will ever find.
Anything producing a path rather than a bare filename hit this: `cd` followed by
any write, `wget`/`curl` with a subdirectory target, bpkg installing into
`/apps`. It presented as a write that "worked" and a read that came back empty.

### PMM reserved 1 MB while claiming 4 MB — `kernel/pmm.c`

The comment said 4 MB, the loop reserved 256 blocks, and 256 × 4096 is 1 MB. The
kernel is loaded at `0x100000` and the linked binary is over 700 KB, so blocks
256 upward — the kernel's own text, rodata and bss — were marked free and
`pmm_alloc()` would hand them out. Latent only because nothing calls
`pmm_alloc()` today.

---

## Timing

### `pit_us()` destroyed time — `kernel/pit.c`

    us_accumulator += (uint32_t)elapsed * 1000000UL / PIT_BASE_HZ;

One PIT tick is 0.838 µs, so for `elapsed == 1` this evaluates to **zero**. The
tick was consumed (`last_pit_val` had already advanced) but contributed nothing.
`pit_read_counter()` costs three port accesses — on the order of one to four PIT
ticks — so the tight `sleep_ms()` poll loop threw away a large fraction of every
interval. `sleep_ms()` slept *longer* than asked, unboundedly so on a fast host,
and `get_ticks()` ran slow, dragging every network timeout, the TCP
retransmission timer and the DNS cache with it.

The same line also overflowed uint32 for `elapsed > 4294`, reachable after a
counter wrap at divisor 11932.

Replaced with a 16.16 fixed-point accumulator that carries the remainder.

### The confirm-or-revert timer was ten times too slow — `shell/shell_v076.c`

`get_ticks()` counts 10 ms units, not milliseconds. The display-mode safety
timer divided by 1000, producing ten-second units: `left` sat at 10 for a
hundred seconds and the advertised "reverts itself in 10s" was really a
hundred-second wait.

That is the one timer in the OS that has to be right. It is the safety net for a
mode that will not sync — the case where the user cannot read anything on screen
and is deciding whether to hard-reset the machine.

### DNS cache entries lived 10× their TTL — `net/dns.c`

Same unit confusion: `expires_tick = get_ticks() + ttl_s*1000`. A record with a
60 s TTL was served from cache for ten minutes after the address changed, and
`nslookup` reported the remaining lifetime 10× too small.

---

## Display — why the wide text modes never worked

v0.7.11 made the column count dynamic and left five call sites pinned to the
compile-time `VGA_WIDTH` of 80. These are independent of the Attribute
Controller bug and were still there after it was fixed.

- `vga_scroll()` used 80 as the **shadow stride**, so in any 90-column mode it
  read from ten cells before the start of each source row — scrolling smeared
  the screen diagonally.
- `vga_putchar()` wrapped the cursor at 80 instead of `g_cols`.
- `vga_putchar()`'s tab handler clamped at 80.
- `vga_puts_at()` clipped output at 80.
- `vga_getchar_at()` was the only shadow accessor with no bounds check at all.

**90x60 is now confirmed working** — 720x480 framebuffer, 8x8 cells, correct
proportions, video output enabled, full `help` output scrolling cleanly. First
time those modes have actually been exercised.

Note: the shell and desktop chrome still draw to 80 columns. That is the
existing "convert the chrome to `vga_cols()`" item, not a regression.

---

## Networking

### TLS handshake messages spanning records — `net/tls.c`

A handshake *message* and a *record* are not the same unit (RFC 5246 6.2.1). The
old parser handled coalescing but not fragmentation: when a message did not fit
a record the loop simply stopped. Two things went wrong at once — the trailing
bytes were never fed to `hs_update()`, so the transcript hash diverged and the
Finished check was guaranteed to fail; and the next record was parsed from byte
0 as a fresh message header, reading the middle of a certificate as a type and a
length.

Records cap at 16384 bytes and a chain with an intermediate routinely exceeds
that, so this hit real servers. It presents as `TLS_ERR_FINISHED` against hosts
whose chain happens to be large while smaller ones connect — which looks like an
intermittent or server-specific fault rather than a stack bug. Added a 24 KB
reassembly buffer used by both the first flight and the pre-CCS window.

### `Content-Length: 0` stalled for five seconds — `net/http.c`

`content_len>0` treated an explicit zero exactly like a missing header, so the
read loop sat there until `http_recv()` timed out. Five seconds of dead air on
every 204, every 304 and every legitimately empty 200.

### Header names are case-insensitive — `net/http.c`

The old code tested exactly two spellings per header. `Content-length:` is legal
and was missed, falling through as an unknown body length — straight into the
timeout path above. `Location:` had the same problem. Now a proper
case-insensitive compare.

### Connection refused took the full timeout — `net/tcp.c`

RST was ignored in `SYN_SENT`, so a refused connection — the most common
connection failure there is — burned the entire 5-second `poll_until()` on every
attempt. Also now verifies that a SYN-ACK actually acknowledges our SYN.

### DNS compression-pointer off-by-one — `net/dns.c`

When an inline label sequence *terminates* in a compression pointer, `pos`
already points past the name. The unconditional `if(!(buf[pos]&0xC0)) pos++`
then skipped one more byte — and since the next field is TYPE, whose high byte
is `0x00` for every type we care about, the test passed and the whole record was
read one byte out of alignment.

### One runt frame stopped the RX drain — `net/ethernet.c`

`eth_poll()` used `break` rather than `continue` on a short frame, abandoning the
rest of the receive queue for that poll. Under load that turns a single
malformed frame into dropped ACKs and stalled transfers.

---

## Boot-time reporting

### The POST screen lied about hardware on every boot — `kernel/main.c`

`boot_animation()` ran *before* `net_init()`, so the POST screen asked
`rtl8139_found()` / `e1000_found()` before either driver had been probed. The
answer was structurally guaranteed to be no.

Separately, the disk line reads `virtio_drive_count()`, which is populated by
`virtio_scan_drives()` — and during boot that was never called at all, only from
the shell and the installer.

So every single boot printed `VirtIO BLK .... Not detected` and
`NIC ..... Not detected` on machines where both were working two lines later.
Cosmetic, but it points anyone debugging a disk or network problem at the wrong
layer. Now reads `1 drive drive0=8192MB` and `RTL8139 bus-master OK`.

---

## Input

### Extended keys could tear in both directions — `drivers/keyboard.c`

`push_key()` wrote the two bytes of an extended key with a separate full/empty
test each; if the ring filled between them the `0x01` marker went in and the
keycode did not, so the next ordinary character was consumed as an arrow-key
payload and every read after it was shifted by one. Both slots are now reserved
up front.

The read side had the mirror image: `keyboard_getchar()` advanced `kbd_tail`
past the `0x01` marker and only *then* checked whether the payload had arrived,
returning -1 with the marker already consumed. The payload showed up on the next
call as a bare character, so an arrow key turned into a stray `H` or `P` in
whatever field had focus. Now peeks first and commits only when both bytes are
present.

---

## Memory safety

- **`memmove` was missing entirely** — `kernel/string.c`. `memcpy()` copies
  strictly forward, so any caller shifting a buffer down over itself had to
  hand-roll a loop or corrupt its own data. GCC can also synthesise a `memmove`
  call on its own, which would have failed to link.
- **`itoa()` indexed the digit table with a negative subscript** —
  `kernel/string.c`. `val = -val` is undefined for `INT_MIN` and leaves it
  negative; a negative value with any base other than 10 skipped the sign
  handling entirely. `vga_printf("%x", n)` on a negative int was enough to
  trigger it. Both `itoa` and `utoa` now work in unsigned and validate the base.
- **`krealloc()` dereferenced before validating** — `kernel/heap.c`. `kfree()`
  bounds-checks the pointer before treating the memory in front of it as a
  header; `krealloc()` read `p-12` first.
- **Unterminated `strncpy` then `strlen`/`strcat`** — `shell/shell_v073.c`.
  `make_path()` can fill the source to 63 characters, so the destination was
  left with no NUL, `strlen()` walked off the end of the 64-byte array, and
  `strcat()` appended past whatever length it invented. Stack smash on any long
  path.
- **Unterminated package IDs** — `shell/bpkg.c`. A 23+ character line in the
  package DB produced an entry with no terminator that `strcmp()` and
  `vga_puts()` then ran past into the next slot.
- **`FAT_BAD` accepted as a data cluster** — `fs/fat16.c`. The bound `c<FAT_EOF`
  let `0xFFF7` through, and `cluster_to_lba(0xFFF7)` resolves far outside the
  volume.
- **Signedness in the TLS Finished length check** — `net/tls.c`.
- **`find_virtio_blk()` abandoned the whole PCI scan** — `drivers/virtio.c` — on
  the first virtio-blk with a memory-mapped BAR0, so a usable legacy device
  further down the bus was never found.
- **`virtio_init()` clobbered the global capacity** — `drivers/virtio.c` — with
  whichever drive was scanned last, which is what `virtio_blk_sectors()`, the
  boot banner and the installer's size checks all read.

---

## Verified

- Clean two-pass build, no new warnings.
- ISO boot → installer → boot chain written → reboot from disk → setup wizard →
  login → shell.
- Boot chain dumped from the raw image rather than trusting the installer:
  MBR type `0x0E`, start LBA 2048, bootable flag, `55AA`; sector 0 begins
  `eb 63 90`; GRUB blocklist at sector 1 offset `0x1F4` reads
  `02 00 00 00 00 00 00 00` — 64-bit LBA 2, correct — with 300 sectors and
  segment `0x0820`; BPB `HAVEN16`, 512 b/s, 4 sec/cluster, 2 FATs, 512 root
  entries, 128 sectors/FAT.
- 90x60 text mode set, confirmed, and scrolled a full page of output.

## Not verified

- The VirtIO queue-slot fix needs two or more VirtIO drives to exercise.
- Real-hardware validation on the X140e, and UTM SE.

## Known, unchanged, needs a decision

`sects_per_fat` is 128, so the FAT describes 32768 cluster slots. At 4
sectors/cluster that is roughly 64 MB of addressable filesystem regardless of
partition size — the installer formats the whole 8 GB and everything past ~64 MB
is unreachable. Not corruption (the allocation bound above is inert on this
geometry; the arithmetic was checked against the actual dumped BPB), just
unusable space. Fixing it changes the on-disk format, so it is left as a
decision rather than a patch.
