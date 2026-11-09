# HavenDOS v0.8.1

**This release is two dev paths merged.** They were developed separately off
0.8 Beta and touched disjoint files:

- **Desktop, start menu and help** (sections 1–5 below). Two things were
  asked for: fix the help list, and expand the start menu because there were
  too few apps in it. Both turned out to have the same cause — a list
  maintained by hand beside the thing it was supposed to describe — so both
  are now generated or checked against the real source, and the build fails
  if they drift again.
- **The UEFI blank-screen investigation** (section 7). The `fbdiag`
  diagnostic, the 64-bit framebuffer address handling, the boot-log line that
  says what the loader handed over, and two new GRUB entries.

Only `HANDOFF.md`, `isodir/boot/grub/grub.cfg` and the GRUB theme were
touched by both sides, in different places — and the version strings in the
two GRUB files are generated from `include/version.h` at build time, so they
resolved themselves. Nothing was dropped from either side.

The music player is unchanged. USB mass storage is still the next step.

---

## 1. The start menu

### There were three app tables, and they had drifted

`shell/desktop.c` held four `menu_item_t` submenu arrays, a `topmenu[]`
pointing at them, and a separate `app_t apps[]` for the desktop icon grid.
Nothing tied them together. The results:

- **Install** and **Package Manager** were on the icon grid and in no menu
  at all.
- **Notification Center** existed and was in neither, reachable only by
  pressing `N` on the desktop.
- **Starfield** was buried inside the Screensaver picker while Matrix and
  Pipes each had their own entry.
- A dozen things existed only as shell commands and so, on a machine driven
  from the desktop, did not exist: Paint, Music Player, Calendar, Quiz,
  Hacker Mode, SysInfo+, Fortune, ASCII Art, `lspci`, `usb`, `dmesg`, the
  self test, Neofetch, BOOT Chat, the user guide, the manual, the legal
  documents and the theme picker.

There is one table now:

```c
typedef struct {
    const char *id;     /* stable key for DESKTOP.CFG / FAVORITE.CFG */
    const char *name;
    const char *icon;   /* exactly two chars */
    void      (*fn)(void);
    uint8_t     cat;
} appreg_t;
static const appreg_t appreg[];   /* 45 apps */
```

Categories, All Apps, Favorites and the icon grid are all *views* over it,
computed by one function. Adding an app is one line in one place, and it
cannot appear in one view while being forgotten in another.

**23 entries became 45.** The 22 additions are everything listed above.

### The menu panel could not grow past four categories

`MENU_TOP` was `MENU_BOT-10` — a fixed eleven rows, with the category rows
at 17..20 and the Lock row at 22. A fifth category landed on the separator
at 21. It derives from `NTOP` now:

```c
#define MENU_TOP (MENU_BOT-(NTOP+6))
```

At the old `NTOP` of 4 that still evaluates to `MENU_BOT-10`, so the
familiar layout did not shift by a cell. At the new 8 it makes room.

Top level, in order: **Favorites · Apps · System · Hardware · Games ·
Media · Settings · All Apps**.

### The header rendered in the middle of the desktop at 80x50 and 90x60

The username, "Logged in", both separators and the Lock row were drawn at
the **literal rows 14, 15, 16 and 21** while the panel itself was computed
from `vga_rows()`. At 80x25 those agree, which is why nobody saw it. At
80x50 the panel sits at rows 34..48 and the header was drawn twenty rows
above it, floating over the desktop with nothing around it. Same at 90x60.

Every row is `MENU_TOP`- or `MENU_BOT`-relative now, so the panel is one
object in any mode. `tests/test_menugeom.c` checks this in all four text
modes and asserts the old literals would now be wrong, so they cannot come
back quietly.

### The flyout showed five items and mis-selected when scrolled

`vis` was `MENU_BOT - MENU_ITEM_Y - 1` — five rows at 80x25. System already
had nine and scrolled. Worse, the hover and click hit-tests read

```c
my >= MENU_ITEM_Y && my < MENU_ITEM_Y + nsub
```

and took `my - MENU_ITEM_Y` as the item index, **ignoring `sub_scroll`**.
Once a category was scrolled, hovering or clicking selected and launched the
wrong app. System was long enough to reach it.

The flyout has its own top at row 2 now — about eighteen visible rows at
80x25, so every category fits without scrolling except All Apps — and both
hit-tests go through `sub_scroll + (my - SUB_ITEM_Y)` with a bounds check.

### The right-click menu is contextual

It was a fixed Settings/Display pair wherever you clicked. It now offers the
pin actions for whatever is under the cursor:

- **on a desktop shortcut** — Open · Remove shortcut · Add to favorites /
  Unfavorite · Settings · Display
- **on a start-menu entry** — Open · Add to desktop / Remove shortcut ·
  Add to favorites / Unfavorite · Settings · Display
- **on bare desktop** — Settings · Display, as before

The labels report the current state and flip in place, and a pin action
leaves the menu open so a second pin is one keypress or click away;
anything that takes over the screen closes it.

The icon hit-test moved into `icon_hit()` so the left-click and right-click
paths cannot disagree about where an icon is — the same reason `reg_fill()`
is the only place a menu view is computed.

**`M` opens the same menu from the keyboard**, on the selected icon or the
highlighted start-menu entry. The context menu had no keyboard route at all
before, on an OS that runs perfectly well with no mouse attached and whose
PS/2 mouse is unreliable under UTM SE's interpreter. The flyout's footer
reads `D=desktop F=fav M=menu`.

### The desktop is the user's own shortlist

HavenDOS used to ship seventeen icons chosen in the source. A fresh install
now has **Shell and nothing else**, and the user decides what joins it.

- `D` on any app in the start menu adds or removes it from the desktop.
- `F` adds or removes it from Favorites.
- `M`, or a right-click, opens the context menu on it.
- `-` or Delete on a selected icon removes that shortcut (the app stays in
  the menu).
- Markers in the flyout show `D` and `F` for what is already pinned.
- An empty grid says so and says where the apps are, rather than looking
  like a failed draw.

Both lists persist in the VFS: `DESKTOP.CFG` and `FAVORITE.CFG`. An id that
is no longer in the registry is dropped on load, so removing an app cannot
leave a pin that points nowhere.

### The start button was unclickable outside 80x25

`my==24` was hardcoded in two places — the start button and the notification
area. At 80x50 the taskbar is on row 49. Both use `TASKBAR_Y` now.

---

## 2. The help list

`cmd_help()` was 150 lines of string tables inside `shell.c`. It is
`shell/help.c` now, per the house rule, and corrected against the real
dispatch table in `exec_cmd()`.

### `pong` was documented and does not exist

Help listed `pong` and described it as "also on the desktop, under Fun".
There is no `pong` command — `app_pong()` lives in `desktop.c` and has never
been reachable from the shell. The tab-completion table completed it too.
Removed from both. The desktop-only apps get an honest section at the foot
of the reference instead, which is where Pong, Tetris, File Manager, Clock,
Hex Viewer, Disk Info, Network Info, User Manager, Log Viewer and
Notifications are now named.

### Eleven working commands were undocumented

`boot`, `copy`, `host`, `hplusplus`, `lock`, `resolution`, `screensaver`,
`script`, `type`, `version`, and `.` as the pipe operator. All documented.

### `h-lang` was not a command

Help gave `h-lang` as the HavenCode command. `h-lang` is the **bpkg package
id**, a different namespace that only appears after `bpkg install`. The
commands are `h`, `hplusplus` and `boot`.

### Ten commands did not tab-complete

`?`, `copy`, `disk`, `host`, `hplusplus`, `quit`, `time`, `type`, `version`
and `.`. `disk` is the notable one — documented in help, would not complete.
`shell_cmds[]` is regenerated from `exec_cmd()` rather than maintained by
hand; `.` and `?` are deliberately left out as not worth completing.

### Names of 16 characters or more ran into their own description

The padding loop filled up to column 16 and no further, so a name at or over
that width got exactly one space: `rename / mv <o> <n> Rename or move file`
reads as if "Rename" were part of the syntax. It pads to 16 or two spaces,
whichever is greater. (0.8.1 added `h / hplusplus / boot` and
`nslookup / dns / host`, which is what surfaced it.)

### `tools/check_help.py`

The build now fails if the reference and the shell disagree, the way
`make_version.py --check` fails on a version literal and `check_boot_log.py`
fails on invented boot output. It checks that nothing documented fails to
dispatch, nothing completable fails to dispatch, and nothing that dispatches
is undocumented outside an explicit exemption list.

```
check_help: 135 commands dispatch, 134 documented, 133 complete -- consistent
```

---

## 3. dmesg has one implementation again

The desktop's new Boot Log app replays the kernel log through the shell's
own pager. That pager was inline in `exec_cmd()`, so it was lifted into
`cmd_dmesg_pager()` and both callers use it. Three installers that each had
their own disk scan is what a second copy costs — see 0.8 Beta §12.

---

## 4. A FAT16 filename bug, found on the way

`name_to_83()` converts `.desktop` to a **blank basename** with extension
`DES`: its first loop stops on the leading dot having written nothing, and
the extension loop then consumes the first three characters of the rest. The
first attempt at the pin files used dotfiles and they did not read back
reliably — the pins reverted to their default.

The pin files are `DESKTOP.CFG` and `FAVORITE.CFG` for that reason. **This
is not a fix for the underlying bug**, which affects every dotfile the
system writes to a FAT16 volume:

| written as | becomes | shown by `ls` as |
|---|---|---|
| `.accounts` | blank + `ACC` | `.acc` |
| `.admin` | blank + `ADM` | `.adm` |
| `.bpkg_db` | blank + `BPK` | `.bpk` |
| `.calc_history` | blank + `CAL` | `.cal` |
| `.havendos_config` | blank + `HAV` | `.hav` |
| `.loginlog` | blank + `LOG` | `.log` |
| `.reminders` | blank + `REM` | `.rem` |
| `.shell_history` | blank + `SHE` | `.she` |
| `.theme` | blank + `THE` | `.the` |

No two of them collide today, so this has been survivable, but a directory
entry with an empty name field is not a thing other FAT implementations will
agree about, and any two dotfiles sharing their first three characters would
become the same file. Worth its own pass; deliberately not done here.

---

## 5. Merged in: the UEFI blank-screen investigation

All of this came from the other dev path and is in `kernel/main.c` and the
GRUB files. It is unchanged by the merge beyond the generated version
strings, and the investigation is **still open** — see section 14 of the
handoff for the results so far and what to do next.

- **`fbdiag`**, a diagnostic reached from the new boot entry "Framebuffer
  test". It sits immediately after the Multiboot parse and before
  `vga_init()`, the PIT and every driver, because all of those are candidates
  for the hang being chased. With no console and no serial port on a laptop
  it answers in the two channels left — the colour of the screen, and whether
  the machine resets itself: black/no reset means the kernel never got there,
  black/reset means no usable framebuffer, magenta means a framebuffer that
  is not 32 bpp, blue→green means good and the PIT counts, blue→red means
  good but the PIT never ticks.
- **"Hardware report  --  firmware and memory"**, a pure-GRUB entry that
  echoes `$grub_platform`, `$grub_cpu` and `$root` and waits, so it works on
  a machine where the kernel never draws. It deliberately does not switch the
  terminal — `videoinfo` cannot enumerate a driver gfxterm already holds, and
  freeing it left an 8x32 screen on OVMF.
- **The 64-bit framebuffer address was being truncated.** Both `parse_mb1`
  and `parse_mb2` took only the low half. A 32-bit kernel writing pixels to a
  truncated address is a wild write that looks exactly like a kernel that
  never started; it is refused and logged now.
- **The boot log says what the loader handed over** — geometry and depth, or
  that there was none, or that the type was wrong. On a machine that comes up
  blank that is the whole diagnosis. Verified on the merged build:
  `[ 0.196] ok Framebuffer 1280x800 32 bpp at FD000000`.
- **The reset path was fixed.** The first version signalled by resetting and
  the reset did not work: `outb(0x64,0xFE)` needs an 8042 and a UEFI laptop
  may not have one. It tries `0xCF9` first, then the 8042 pulse, then a
  triple fault through a null IDT.
- **A GRUB theme error on every keypress.** `selected_item_pixmap_style =
  "none"` is parsed as a pixmap pattern and "none" is not one, so GRUB
  printed `error: missing '*' in box pixmap pattern .../none` above the menu
  on every interaction. Omitting the line is how you say "no pixmap".

### And then corrected, because the diagnostic could not answer its own question

Kyan pointed out that his HP shows black-and-reboot, and read that as the
kernel never running. He is right that it cannot be ruled out, and the
diagnostic as merged could not tell the difference.

`fbdiag` runs **before `idt_init()`** — the order in `kernel_main` is
`parse_mb*` → fbdiag → `vga_init()` → `idt_init()`. With no IDT, any
exception from kernel entry onwards has no handler, double-faults, finds none
again, and **triple-faults, which resets the CPU**. The diagnostic's own exit
was also a reset. So "black screen, then reboot" was the observation for both
"I ran and found no framebuffer" and "I died immediately", and the table
documented it as the first. The two rows were effectively inverted.

Fixed by taking the answer off the reset channel entirely:

- **Counted beeps and keyboard-LED flashes** carry the result — PIT channel 2
  and the 8042, neither of which needs video, an IDT, interrupts or
  `pit_init()`. 1 pulse = fbdiag entered, 2 = no framebuffer, 3 = wrong
  depth, 4 = good and PIT counts, 5 = good but PIT dead, 6 = above 4 GB.
  **No pulse and an instant reboot now means the kernel never really ran**,
  which was previously indistinguishable from a real result.
- **The first pulse fires before a single pixel is written**, because writing
  to a framebuffer address the firmware did not really hand over is itself a
  candidate for the hang.
- **Every 8042 wait is bounded.** A UEFI laptop may have no controller, and a
  diagnostic that hangs where it used to reset would be worse than the bug.
- **A fifteen-to-twenty second hold before the reset**, so "rebooted after a
  quarter of a minute" and "rebooted instantly" are different observations.
  The pulse lengths were sized for a modern machine too: the first draft's
  12M-iteration tone is about twelve milliseconds on an i5 — inaudible, which
  defeats the point of a channel that works without a screen.
- **A new GRUB entry, "Handoff test"**, which is the only thing that can
  establish whether the kernel gets control at all. `multiboot` loads and
  validates the image while GRUB is still able to print; `boot` transfers
  control. Splitting them across a `read` makes the printed message proof of
  how far the machine got, on a screen that still works at the time.

Re-verified under QEMU: "Framebuffer test" fills the screen **green**
(`#00C000`, matching the `0x0000C000` the code writes), holds it, and resets
— the "framebuffer good and the PIT counts" answer, correct for QEMU — with
the hold clearly separated in time from the reset.

### Then corrected again: the count itself was ambiguous

Run on the HP, "Handoff test" printed its message — so GRUB loads the kernel,
the multiboot header is valid, and **the kernel does get control**. That
closes the whole "never loaded" branch.

"Framebuffer test" then gave roughly six pulses. But the signal was one
entry pulse followed by N answer pulses, with 0.35 s between the groups and
0.22 s inside them — indistinguishable by ear. Six therefore meant 1+5 or
1+6: *PIT never ticks* or *framebuffer above 4 GB*. Two different faults,
both plausible on a 12th-gen laptop, and no way to tell which.

Counting is no longer required:

- each round is one **long** marker tone then N **short** beeps, so the group
  boundary is a duration difference rather than a gap difference;
- the round repeats twelve times instead of once;
- the answer is left in the **keyboard LEDs as a static binary pattern**
  (caps=4, num=2, scroll=1) and the machine **halts** — photograph the
  keyboard, no counting and no stopwatch;
- and because it halts, a reboot is never the diagnostic's doing. Verified
  under QEMU: green, held for over three minutes, no reset.

`tests/test_mbinfo.c` (41 checks) pins the Multiboot structures against the
specification, because the answer now depends on them. `fb_addr_hi` is read
at offset 92; misplaced onto `fb_pitch` — which is never zero on a machine
that has a framebuffer — fbdiag would report "above 4 GB" on *every* machine
and it would look exactly like a real finding. Verified by re-injecting the
original u32-VBE-field bug, which the suite catches (28/41).

**Two earlier conclusions are withdrawn.** Kyan's black-then-reboot is not
evidence about the framebuffer. And "the above-4 GB theory is dead" rested on
a diagnostic that reset only in that case, read against a machine resetting
for reasons nobody could separate — if the answer is 6, it was never ruled
out.

```
make test -> 553 checks pass across thirteen suites
```

---

## 6. Tests

`tests/test_menugeom.c` — **75 checks**, new:

- the 80x25 layout at the old `NTOP` of 4 is byte-identical to the old one
- every panel row is inside the panel in 80x25, 80x50, 90x30 and 90x60
- the old literals 14 and 21 are no longer the rows they were
- the flyout is tall enough that only All Apps scrolls
- the registry adds up, every id is unique and short enough for `PIN_IDLEN`,
  every icon is exactly two chars and distinct, the longest name fits

Its registry facts are **generated** from `shell/desktop.c` by
`tools/make_appreg_inc.py` into `tests/appreg.inc`, the arrangement
`tests/bars.inc` already uses for the desktop chrome — a test carrying its
own copy of the table stops testing it the moment the two drift, which is
the bug the registry exists to kill.

Checked by injection, per the 0.8 Beta house rule that a test which cannot
fail proves nothing. Four of the original bugs were put back one at a time:

| injected | caught |
|---|---|
| the fixed 11-row panel | 61/75 |
| the header pinned to row 14 | 71/75 |
| the flyout back at five rows | 72/75 |
| a category silently emptied | 73/75 |

```
make test -> 553 checks pass across thirteen suites  (437 across eleven in 0.8 Beta)
```

---

## 7. Verified on hardware-equivalent QEMU

Driven over QMP with the screen read back by decoding pixels against the
kernel's own font. Confirmed on a running system, not by inspection:

- the full help reference, all six pages, correct columns
- the start menu with all eight categories
- every category flyout, counts 11 / 12 / 5 / 4 / 7 / 6, All Apps at 45
- a fresh install showing Shell alone on the desktop
- `D` adding Snake and Disk Info to the grid, visible immediately
- **both surviving a reboot**
- the empty-Favorites hint
- Paint, PCI Devices, Boot Log and User Manager launching from the menu
- the right-click menu offering five items over an icon and two over bare
  desktop, confirmed by reading back where the menu actually opened
- `M` opening it from the keyboard, and a pin action flipping its own label
  with the menu still up
- after the merge: the start menu and Apps flyout unchanged, the new
  `Framebuffer 1280x800 32 bpp at FD000000` boot-log line, and the
  "Framebuffer test" entry answering green-then-reset

Driving the mouse needed the harness taught that `drivers/mouse.c` divides
raw PS/2 counts by `MOUSE_DIVISOR` (4) and doubles any delta over 24 raw, so
one cell is four units sent one at a time. Noted here because the next
person to script a mouse test will otherwise lose the same hour.

Not checked: UTM SE, and the real ThinkPad X140e.
