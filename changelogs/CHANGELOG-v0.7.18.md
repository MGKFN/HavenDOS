# HavenDOS v0.7.18

A defect-and-performance release. No new user-facing features in the OS; the
new work is in BootChat, which gets a rebuilt web client and a 2.1 protocol.

Everything below was verified before it was written down. Where a fix claims
the old code was broken, there is a test that fails against the old code and
passes against the new one. What was *not* verified is listed at the bottom,
separately, and not dressed up.

---

## Defects fixed

### 1. BootChat silently truncated the wire protocol  `shell/chat.c`

`CHAT_LINE` (96) is the width of one scrollback row on screen. It was also
being used to size the **receive assembler**:

```c
char rxline[CHAT_LINE];
...
} else if(c != '\r' && rlen < CHAT_LINE-1){ rxline[rlen++] = c; }
```

Every byte past 95 in any server line was thrown away. The server's limit is
2048 and the web client allows 900, so this was losing real data on the
HavenDOS side only:

* A `MSG <server> <channel> <user> <ts> ` prefix costs roughly 40 bytes, so a
  300-character message arrived as **57 characters**. Measured, not estimated.
* It applied to `CHANLIST`, `DMLIST` and `ONLINE` too. With enough channels
  the tail was cut and **channels silently vanished from the sidebar** — an
  8-channel list lost one in testing.

The wire now has its own budget (`CHAT_WIRE`, 1024) separate from the display
width. The display buffers behind it were re-truncating the text a second
time before `sb_add_wrapped()` ever saw it; those were widened too. The
wrapper itself was always correct — the callers were the defect.

### 2. The updater compared against a frozen version  `fs/update.c`

```c
#define HAVENDOS_VERSION "0.5.9.14"
```

That string feeds `ver_cmp()` against the version read off an update ISO.
Consequences, on a system that had shipped 0.7.17:

* "Same version already installed" could never fire.
* Any ISO from 0.5.9.15 onward compared as **newer** — including a
  **downgrade**, which would have been applied as an upgrade with no warning.

### 3. The version number was typed out in ~50 places and they disagreed

A v0.7.17 build shipped screens reading v0.7.0, v0.7.4 and v0.7.11; the ISO's
GRUB menu said v0.7.11; `isodir/version.txt` said v0.7.11; and the installer
wrote a `grub.cfg` whose menu entries said v0.7.4.

There is now one `include/version.h`. 84 string literals were rewritten to
derive from it, and `tools/make_version.py` regenerates `version.txt` and the
ISO's GRUB menu titles at build time, so a release is a one-line change and
the three can no longer drift apart. Historical `v0.7.x` references in
comments were deliberately left alone — they record which release fixed what.

### 4. The Multiboot1 memory map was dead code  `kernel/main.c`

A Multiboot1 mmap entry is `size@0, base_addr@4 (u64), length@12 (u64),
type@20 (u32)`. The code read `type` from `p+16` — the **upper dword of
`length`**, which is zero for every region below 4 GB. So `type==1` never
matched, the loop never updated `highest`, and the whole "more accurate"
memory-map path did nothing. RAM size always fell back to `mem_upper`, which
some BIOSes cap well below the truth.

Test: against a synthetic map describing 2 GB of usable RAM, the old offset
finds **0 bytes**; the fixed offset finds **2048 MB**.

### 5. Multiboot2 ignored the memory map entirely  `kernel/main.c`

```c
if(tag->type==6) ram_bytes=64u*1024u*1024u;
```

Tag 6 *is* the memory map. It was never parsed — RAM was pinned to 64 MB on
any Multiboot2 boot regardless of the machine. Now parsed properly. Latent
for HavenDOS (which boots Multiboot1) but live for HavenOS bare-metal.

### 6. The protocol parser desynced on an over-long field  `shell/chat.c`

`field()` stopped copying at the output buffer's capacity but left `p` sitting
**inside** the token, so the remainder was parsed as the next field and every
field after it shifted by one. With an 8-byte buffer, `verylongserver
general` yielded a second field of `gserver` instead of `general`. Not
reachable through the server's own `valid_name()` limits, but it is a parser
that mis-parses, which is worth closing.

### 7. `pmm_alloc()` would have handed out the kernel's own memory  `kernel/pmm.c`

`PMM_RESERVED_BLOCKS` was a hard-coded 1024 blocks = 4 MB, described as
covering "low memory + kernel image". The v0.7.17 image actually ends at
**6.31 MB**, so 2.31 MB of live BSS sat above the reserved mark and was
allocatable. The v0.7.12 comment in that file calls it "a loaded gun pointed
at the first caller" — and item 8 below makes this release the first caller.

The boundary now comes from the linker's own `_kernel_end` symbol, so it
tracks the image instead of a constant someone has to remember to raise.

---

## Performance

### 8. 2.50 MB of BSS removed (4.20 MB → 1.70 MB, 60%)

Two objects dominated the kernel's static footprint, both reserved at every
boot for features used occasionally:

| object | size | where | now |
|---|---|---|---|
| `g_elf_buf` | 2 MB | `fs/update.c` | taken from pmm while `update`/`rollback` runs |
| `g_filebuf` | 512 KB | `net/http.c` | taken from pmm for the duration of a request |

For scale: the kernel heap is 256 KB, so the OS was reserving ten times its
own heap for two scratch buffers it usually never touched.

`pmm_alloc_contig()` / `pmm_free_contig()` were added for this, since both
callers index their buffer as a flat array. The allocator was tested
natively — 17 checks covering alignment, non-overlap, free-and-reuse,
fragmentation (a run larger than a freed hole skips past it; one that fits
reuses it), and refusal (an impossible run returns NULL rather than wrapping).

Both call sites degrade honestly if memory is short: `update` says so and
stops, and `http_get` returns the new `HTTP_ERR_NOMEM` rather than writing a
truncated file.

### 9. The kernel image is smaller

Ends at **3.81 MB**, down from 6.31 MB.

---

## Desktop

### 10. The desktop no longer force-switches the display to 80x25

`desktop_run()` used to call `vga_set_mode(80,25)` on entry because the
chrome was nailed to rows 21-24 and 80 columns; the shell then put the user's
mode back at its next prompt.

The chrome — top status bar, taskbar, info panel, toast row, start menu,
context-menu clamping, icon grid — now derives its geometry from
`vga_rows()` / `vga_cols()`. At 80x25 every expression evaluates to the
number it replaced, so the familiar layout is unchanged to the column.

Verified by compiling the real `ui_draw_desktop_bars()` natively against a
fake VGA and asserting placement in 80x25, 80x50, 90x30 and 90x60: 42 checks,
including that the bell, NOTIF and clock still land on columns 55, 59 and 71
at 80x25 exactly as before.

**Not claimed:** apps launched *from* the desktop (calculator, notepad, file
manager, settings, and the rest) still lay themselves out for 80x25 and draw
in the top-left of a larger screen. Converting those is a separate pass.

---

## BootChat

### 11. Protocol 2.1, opt-in, with no break for existing clients

Editing and deleting need a message id on the wire, and the 2.0 `MSG` line
has nowhere to put one — its trailing field runs to end of line. Changing
`MSG` outright would have broken every HavenDOS client already installed.

So a client **asks**: send `PROTO 2.1` and the server switches that
connection's format. Say nothing and you get 2.0, byte for byte. The server
tracks this per connection, so a 2.0 HavenDOS and a 2.1 browser can sit in
the same channel and each receives a line it can parse.

**Your installed v0.7.17 HavenDOS needs no change and keeps working.**

New verbs: `PROTO`, `USERS`, `EDIT`, `DEL`, `REPLY`.
New server lines: `USERLIST`, `MSGEDIT`, `MSGDEL`, and the 2.1 forms of
`MSG` / `DMMSG`. Message ids are `c<rowid>` for channel messages and
`d<rowid>` for DMs — one field, self-describing, still a single `strchr()`
in C.

### 12. Server hardening

* Ownership enforced on edit and delete; a 15-minute edit window.
* Input validated server-side: length capped at 1500, control characters
  stripped (HavenDOS draws straight into VGA cells, where a stray escape
  corrupts the display).
* Per-connection token-bucket rate limit, 4/s sustained after a burst of 10.
* SQLite migration adds `edited` and `reply_to` to an existing `bootchat.db`
  — `CREATE TABLE IF NOT EXISTS` does not add columns to a table that already
  exists, so without this every insert against your current database would
  have failed.

### 13. Web client rebuilt

Message grouping, date separators (Today / Yesterday / weekday / date,
localised), a new-message indicator that does **not** force-scroll the
reader, `@` mention autocomplete drawn from the real accounts table with
keyboard navigation, an auto-expanding composer with Enter to send and
Shift+Enter for a newline, reply with a clickable quote, edit and delete on
your own messages, a mobile drawer, and visible connection state with
exponential-backoff reconnect.

Every control maps onto a verb the server implements. Nothing is mocked.

Security and robustness:

* All message content is rendered with `textContent` and real DOM nodes,
  never `innerHTML`. Backed by a strict Content-Security-Policy.
* The chat server's address is never sent to the browser. The sign-in screen
  shows a configured public name; the bridge holds the address. There is no
  IP:port anywhere in the delivered page.
* Configuration via `BOOTCHAT_PUBLIC_URL`, `BOOTCHAT_CHAT_HOST`, and others —
  development and production differ by environment, not by edited source.
* A message the server refuses is put **back in the composer** instead of
  being silently eaten.

### 14. Long polling

The old client polled every second regardless of activity: one request per
second per open tab, and up to a second of latency on every message. The
bridge now holds the request open until a line arrives or 25 seconds pass.

One practical consequence worth knowing: "wait for network idle" never fires
against BootChat, because there is always a request in flight.

---

## Testing

| suite | what it covers | result |
|---|---|---|
| `test_fixes.c` | mmap offset, `field()` desync, wire truncation | 10/10 |
| `test_pmm.c` | the real `pmm.c`, compiled natively | 17/17 |
| `test_chrome.c` | the real `ui_draw_desktop_bars()` in 4 modes | 42/42 |
| `test_server.py` | v2.0/v2.1 coexistence, ownership, limits, migration | 15/15 |
| `test_web.py` | the real page in Chromium against a real server | 34/34 |
| `test_delivery.py` | message delivery under and over the rate limit | 5/5 |

Each defect test is written to **fail against the old code**, so it
demonstrates the bug rather than just asserting the fix.

Build: clean, and **zero new compiler warnings** against the v0.7.17
baseline (38 before, 38 after). Boots in QEMU on VirtIO to `System ready`.

## Not verified

Stated plainly rather than left for you to discover:

* **No hardware testing.** v0.7.11 remains the last version validated on the
  ThinkPad X140e. Everything since, this release included, is QEMU-only.
* **UTM SE untested** for this release.
* **Install-to-disk was not exercised.** The ISO boot path was tested; the
  installer writing to a real partition was not.
* **`update` / `rollback` were not run end to end.** Their buffer now comes
  from pmm, and the allocator is tested, but the full update flow needs an
  update ISO and was not performed.
* **Desktop apps at non-80x25 modes** render top-left, as described in 10.
* **Clicking a channel in the HavenDOS chat sidebar remains unverified** —
  unchanged from v0.7.17, and still awkward to drive from the QEMU monitor
  because `mouse_move` is relative for a PS/2 mouse.
