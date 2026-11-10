# HavenDOS v0.7.19

The BOOT Chat client learns protocol 2.2 and 2.3, four stale version literals
are removed along with the mechanism that let them survive, three display
faults are fixed, and the build and test story is made honest: `make -B`
really does produce zero warnings, and every test suite really does run from
the source zip alone.

---

## 1. Corrections to what v0.7.18 was told about itself

Three claims in the v0.7.18 handoff do not reproduce from the zip that shipped
with it. None of them is a code fault; all three are measurement faults, and
two are the same kind the handoff was itself apologising for.

| Claim | Reality |
|---|---|
| "0 warnings, for real, on `make -B`" | **38.** (19 unique, each seen twice — the build compiles everything twice by design.) |
| `test_pmm.c` → 17/17 | **Would not compile.** `tests/shim/` was not in the zip, and line 22 included `../../tree/havendos-v0.7.17-source/kernel/pmm.c` — a path into a v0.7.17 tree that was never shipped. |
| `test_chrome.c` → 42/42 | **Would not compile.** Line 54 included `bars.inc`, which existed nowhere. |

Only `test_fixes.c` ran. The other two had been measured in a working
directory that happened to contain a shim, a `bars.inc` and a sibling v0.7.17
tree — none of which were in the zip.

The same pattern turned up in BootChat: all three assemblers and all six test
suites hardcoded the same missing `tree/havendos-v0.7.17-source` path, and one
carried an absolute `/home/claude/...` container path. Eleven files in total
pointed at a directory that does not ship. All of them now resolve relative to
themselves.

---

## 2. Zero warnings, measured properly

`make -B` reports **0 warnings, 0 errors**. The 19 unique warnings were read
individually before being silenced; none was a live bug.

* **11 misleading-indentation** — all the `while(n<W) buf[n++]=' '; buf[W]=0;`
  pad-then-terminate idiom, where the terminator genuinely belongs after the
  loop. Split onto their own lines in `net/net.c` (×3), `net/tls_crypto.c`,
  `shell/desktop.c` (×6) and `shell/boot_lang.c`.
* **5 unused functions** — `boot_screen`, `pci_read16`, `iso_upper`,
  `bn_is_zero`, `xtime`. Real helpers worth keeping, marked
  `__attribute__((unused))` rather than deleted.
* **2 unused constants** — the `kernel_elf_data`/`kernel_elf_size`
  placeholders that exist only to keep `fs/install.c` compiling in pass 1.
* **1 unused variable** — `g_strpool[4096]` in `shell/boot_lang.c` was never
  read or written; only its cursor was reset. Removed, returning 4 KB of BSS.
* **2 linker notes** — `boot/boot.S` now carries a `.note.GNU-stack` section,
  so the image is no longer marked as wanting an executable stack.

---

## 3. Four stale version literals, and the hole they came through

v0.7.18 introduced `include/version.h` as the single source of truth and said
`make_version.py` "fails the build if a version is typed anywhere it could
drift again". It did not — it only *generated* the ISO files. Four literals
survived into the shipped v0.7.18:

* **`$VER` at the shell prompt read `0.7.11`.** `shell/shell.c` set it with a
  string literal. The handoff listed this as fixed in its second pass; it was
  not.
* **`havendos_install_version_str()` returned `0.7.4`.**
* **The mode-13 info panel drew `v0.3.0`** next to live RAM, uptime and user —
  eleven releases out of date, beside data that was current.
* **`include/version.h`'s own comment contained `"0.7.18"`**, and the
  Makefile's banner scraped the version with an unanchored `sed … /p` that
  prints *every* match. At v0.7.18 both matches agreed, so it looked right;
  bumping the version made the banner print the old version beside the new one.

All four now derive from `version.h`. The banner's `sed` is anchored to
`^#define`. And `make_version.py --check` genuinely does what was claimed: it
scans the shipped C sources for OS-shaped version literals and **fails the
build**, with `version-literal-ok` as an opt-out for the handful of legitimate
ones (an H language version, an IP address, two historical comments). It runs
before the ISO is generated.

---

## 4. BOOT Chat client: protocol 2.2 and 2.3

`shell/chat.c` never sent `PROTO` at all, so it was a 2.0 client — no message
ids, no edits, no replies, no reactions, no typing, no read state, no
attachments, and no signing in with an email address. It now asks for 2.3 and
falls back to whatever the server grants, so an older server still works
unchanged.

**Messages.** The 2.1+ `MSG` and `DMMSG` shapes are parsed, including the fact
that the id comes *after* the channel in `MSG` and *first* in `DMMSG`. Edits
show `(edited)`, replies quote what they answer, and `MSGEDIT` / `MSGDEL`
rewrite the row in place, keeping the speaker prefix.

**Reactions** render as a compact summary on the message — `[:+1: x2]` — from
both the live `REACT` line and the `REACTIONS` backfill.

**Typing** is shown on the separator row and aged out locally after three
seconds, because the server sends no refresh and a dropped socket would
otherwise leave the indicator up for ever. Outgoing typing is throttled to one
notice every two seconds and is never sent while composing a slash command.

**Read state** is sent on viewing a message and honoured when mirrored from
another device, so an unread badge clears on HavenDOS when it is read on a
phone.

**Attachments** (2.3) become text, the same reasoning that spells emoji out as
`[grin]`: a cell cannot hold a picture, and a name and size is the honest
representation. `<mgkfn> have a look  [image: shot.png 2K]`. HavenDOS receives
attachments but does not send them — no file picker, no multipart client.

**Accounts.** Registration collects an email address, sign-in accepts an
address or a username, and `NEEDEMAIL` is surfaced with the command that fixes
it. The account name is taken from `OK LOGIN`, not from what was typed: the
users table is case-insensitive, and comparing the typed spelling against the
stored one is what made a person's own messages render as someone else's.

**New commands.** `/reply`, `/edit`, `/del`, `/react`, `/unreact`, `/email`,
`/verify`, `/resend`, `/me`, `/sessions`, `/ids`, `/proto`. A message is
referred to by its wire id (`c41`) or by how far back it is (`1` is the
newest); `/ids` shows the ids in the pane. Each is gated on the negotiated
level and says so plainly rather than failing obscurely.

The 2.2 administrative objects — `PROFILE`, `ROLES`, `SESSIONLIST`, `AUDIT`
and the rest — are single-line JSON meant for richer clients. They are
acknowledged rather than dumped into an 80-column pane.

---

## 5. Display faults

**Notepad rendered in the top-left corner at every mode but 80x25.**
`ED_VROWS` and `ED_VCOLS` were compile-time 21 and 74. In 80x50, 90x30 or
90x60 the editor drew a 21x74 region and left the rest of the screen holding
whatever had been there before. They are now derived from the active mode, and
reproduce 21 and 74 *exactly* at 80x25 — `tests/test_edgeom.c` checks that, so
the familiar layout has not shifted by a cell. Its title, status and hint bars
follow the real width and the real bottom rows, as do its four transient
prompts; the HavenCode window fills the screen instead of being a fixed 80x23
box at (0,1).

**The login screen wrote over long account names.** Found by walking first-run
Setup in a VM: an account created as `kyanhoekstra123` appeared on the login
screen as `kyanhoekstra1>3`. The tile drew the name at `TILE_X+5` and the
selection marker at a fixed `TILE_X+TILE_W-2`; with `TILE_W` 20 that put the
marker on column 20 while a 15-character name ran to column 21, so the marker
landed inside the name and replaced its 14th character. The `[A]`
administrator badge ran off the end of the tile for the same reason. Setup's
own prompt says "3-15 characters", so every name over 13 hit this. The tile is
now sized to the longest name on screen, clamped to the display, and the name
is cut rather than drawn over when the screen is genuinely too narrow. The
mouse hit-box uses the same width, so it tracks automatically.

**The tiles were pinned to row 23**, the bottom of an 80x25 screen; in taller
modes they were stranded mid-display. They now stack from `vga_rows()-2`, and
still land on row 23 at 80x25.

---

## 5a. `update` works — four faults, each fatal on its own

The in-system updater has never worked. It now does, both directions, verified
in a VM: install v0.7.19 to disk, insert a v0.7.20 ISO, run `update`, reboot
into v0.7.20, run `rollback`, reboot back into v0.7.19.

**The version compare never parsed the ISO's version.** `make_version.py`
writes `/version.txt` in display form, with a leading `v`, and the running
system compares against the bare `HAVENDOS_VER_NUM`. The parser started at the
first character, consumed only digits, and advanced `p` only on a `.` — so for
a v-prefixed string the inner loop matched nothing, `n` stayed 0, and `p` never
moved. The outer loop ran four times on the same character and the whole string
parsed as all zeroes. A **newer** ISO therefore compared as **older**, and the
update manager asked *"Downgrade to older version?"* for every upgrade;
answering no, the sensible answer, cancelled it. `tests/test_vercmp.c` covers
this with 30 checks against the real function.

**Everything in `/boot` was looked up with the root-only API.** `update.c`
called `fat16_exists`, `fat16_read`, `fat16_write` and `fat16_delete` with
`/boot/havendos.elf`. Those four only ever search the root directory: they hand
the whole string to `name_to_83()` and look for a single 8.3 entry in the root.
The kernel was never found and never written. The `_path` variants, which walk
the directory chain, are what this file needed.

**Two of the four 8.3-name comparisons were case-sensitive.** `dir_lookup()`,
written for the v0.7.13 path work, compares with `n83_eq()` and folds case —
its own comment says "Compare two 8.3 name fields case-insensitively".
`subdir_find()`, `root_find()` and a fourth inline scan inside
`fat16_write_path()` all still used `memcmp`. Since the installer wrote its
entries in lower case while `name_to_83()` upper-cases, `/boot` was found and
`havendos.elf` inside it was not.

That produced something worse than a clean failure. The write did not find the
existing entry, so it allocated a fresh slot: a disk that had been through one
update run held **two** kernels in `/boot` —

    /boot   havendos.elf    616100   the installed one
    /boot   HAVENDOS.ELF   1230708   written by the update

with GRUB loading whichever it reached first. All four comparisons now use
`n83_eq()`, and the installer writes spec-correct upper-case names. FAT16 short
names are upper case by specification; the lookups still fold case, so **disks
installed before this keep working**.

**Every write was misread as a failure.** `fat16_write()` and
`fat16_write_path()` both return the NUMBER OF BYTES WRITTEN on success and a
negative value on failure. All three call sites in `update.c` tested `!= 0`, so
a successful 1.2 MB write returned 1230708 and was read as a failure: the
update manager printed *"FAT16 write failed"* and rolled back a write that had
in fact just succeeded. Failure is `< 0`.

`tests/test_fat16path.c` compiles the real `fs/fat16.c` natively and runs it
against an actual installed disk image — 12 checks covering subdirectory
lookup at one and two levels, the two APIs agreeing, replace-not-duplicate on
write, and the return convention. It skips cleanly when `$BOOTDISK` is unset,
so `make test` still runs anywhere.

## 5b. Text that only ever rendered as garbage

23 string literals across the tree contained a UTF-8 em dash. VGA text mode is
one byte per cell over code page 437, so each one was drawn as three wrong
glyphs — the update manager's own output read

    WARN  No existing kernel found ГÇö first install path.

`make_version.py --check` now refuses non-ASCII inside any string literal, with
comments excluded (a comment is never drawn, and one legitimately spells
"cafe" properly while explaining the font work).


## 6. Tests

`make test` runs nine suites from the source tree alone — **291 checks**:

| suite | checks | |
|---|---|---|
| `test_fixes` | 10 | the v0.7.5–v0.7.12 regressions |
| `test_pmm` | 17 | the real allocator, via a named test seam |
| `test_chrome` | 42 | the desktop chrome, against the real function |
| `test_chat22` | 60 | the chat client's 2.2/2.3 parse, against real wire lines |
| `test_edgeom` | 29 | the editor viewport in every mode |
| `test_accounts` | 27 | Setup-created accounts, end to end |
| `test_tiles` | 64 | the login tile geometry |
| `test_vercmp` | 30 | the update manager's version compare |
| `test_fat16path` | 12 | real fat16.c against a real installed disk |

Two of these needed the suites repaired first. `test_pmm.c` now includes this
tree's own `kernel/pmm.c` through a named `PMM_KERNEL_END_ADDR` seam, rather
than macro-mangling an `extern` declaration and reaching into a v0.7.17 tree.
`bars.inc` is **generated** from `shell/ui.c` by `tools/make_bars_inc.py`
rather than committed, so the chrome test cannot certify a stale copy of the
function it is testing — a hand-copied duplicate drifting from its original is
exactly how this project has been bitten before.

---

## 7. The question v0.7.18 left open

**"Can you log into a HavenDOS account that Setup just created?"** — yes.

The handoff called this blocking and could not settle it because scripted
keystrokes never got in. It is a pure-logic question, so `tests/test_accounts.c`
answers it against the real `shell/login.c`: `add_user()` is exactly what Setup
calls and `try_login()` exactly what the login screen calls, both running
unmodified. 27 checks cover the ordinary case, the 15-character boundary that
`strncpy(name, 15)` into a `char[16]` sits on, a blank password, passwords with
punctuation, case sensitivity, a slot reused after a deletion, and survival
across `accounts_save()`/`accounts_load()`.

It was then confirmed in a VM: first-run Setup was walked through, an account
named `kyanhoekstra123` was created, and it appears on the login screen. The
v0.7.18 failure was the keystroke harness, not the account logic — and the one
real fault in that path was the display bug in §5, which is why the account
worked while the screen was wrong.

---

## 8. Verified in a VM, on this release

Booted from the ISO, installed to a VirtIO disk, and booted again from the
disk with no ISO attached:

* POST, boot log, installer and login screen all read **v0.7.19**.
* Install-to-disk writes a real MBR, GRUB core image and FAT16 partition
  (type 0x0E at LBA 2048), and the disk boots on its own afterwards.
* First-run Setup completes: terms, privacy, account, theme, add-ons, summary.
* `update` and `rollback` both run end to end (see section 5a).
* The desktop has been looked at: icons, taskbar, clock and the `user.admin`
  badge all render correctly at 80x25.
* POST reports `ATA Bus 0 - Not present` and the boot log reports
  `ATA: 0x1F0 not present`. The v0.7.18 handoff recorded these two disagreeing;
  on VirtIO they agree, so whatever was seen was specific to another
  configuration and is not reproduced here.

### Still not verified

* **No hardware testing since v0.7.11.** Everything since, this release
  included, is QEMU-only. The ThinkPad X140e remains the real test.
* **Clicking a channel in the HavenDOS chat sidebar** is still unverified.

### Two more cosmetic faults, seen but not fixed

* The Setup summary screen is headed **"Step 7 of 6"**.
* Its last line is clipped at the right edge — `…(enable in Setting` rather
  than `Settings)`.
* On the account screen, the password input field is drawn at column 43 and
  overwrites the tail of its own label, so it reads
  `Password  (leave blank = no p********` instead of `…no password):`.

---

## 9. Releasing

One line in `include/version.h`. `make` regenerates `isodir/version.txt` and
the GRUB menu titles from it, and `tools/make_version.py --check` now fails
the build if a version is typed anywhere it could drift again — this time it
actually does.

```sh
make -B          # 0 warnings, 0 errors, produces havendos.iso
make test        # nine suites, 291 checks

# The filesystem suite needs a disk the installer produced:
BOOTDISK=/path/to/disk.img ./tests/... # or just: make test (it skips)
```
