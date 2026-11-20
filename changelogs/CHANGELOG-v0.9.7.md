# HavenDOS v0.9.7 Beta "Mnemosyne" — the old builds, the new numbers, and the mouse everywhere

The 0.9.7 line, released 20 November 2026. Kyan, after testing 0.9.6.7:
*"just gimme the emulator lol, also I like it so that u release 0.9.7
with all planned stuff okay"*. So this is everything THS-RM-1005 REV C
planned for 0.9.7:

- **Retro PC** (exception E-1): the studio's old builds, running inside
  HavenDOS.
- **The new version numbers** in the updater, and update channels.
- **The editor:** select text with the mouse, and one clipboard for every
  app (exception E-2).
- **Files:** a right-click menu (exception E-3).
- **The taskbar:** pinned apps, and smaller desktop icons (exception E-4).
- **The mouse wheel on real mice**, a fix from his 0.9.6.7 test.

Mnemosyne is memory: the release that keeps the oldest builds alive, and
teaches every install the numbers it will be known by.

**This is the first GitHub *pre-release*.** From 0.9.7, every release
below Stable is marked pre-release (THS-RM-1004), so GitHub's "latest"
stays on 0.9.6.7 until HavenDOS 1 is Stable on 1 January. Get 0.9.7 from
the releases list.

## Fixed: the mouse wheel on a real mouse

Kyan, on the HP: "now the scroll wheel on mouse still no".

0.9.6.3 read the wheel from a fourth byte that QEMU's mouse sends in
boot protocol. The boot protocol does not promise it, and most real mice
send three bytes there, keeping the wheel for their own report format.

- **A USB mouse is now asked for its report descriptor.** The trackpad's
  parser (`usb/hid_rd.c`) reads it, and if X, Y and a button resolve,
  the mouse stays in report protocol, which carries the wheel.
- **Big moves are split** across packets, so none of the movement is
  clipped.
- **Anything else falls back to boot protocol, exactly as before:** no
  descriptor, a refusal, an absolute pointer (a tablet), or no X/Y.
- `dmesg` says which: "USB mouse 046d:c077 interface 0: report
  protocol, wheel".

## The new version numbers (THS-RM-1004, RM-1005 section 7)

HavenDOS 1 will call itself, the way Windows does:

```
HavenDOS 1, version 27H1 (OS Build 27100.1000)
```

0.9.7 teaches every install those numbers before they arrive.

- **`include/version.h`** gains PRODUCT, RELEASE, BUILD, REVISION,
  CHANNEL_ID and LEVEL. PRODUCT 0 is the 0.9.x line, which still shows
  itself as it always has.
- **One version string** (`kernel/verinfo.c`) for `ver`, the System app,
  Settings' About page, neofetch and the boot splash. On a channel other
  than Stable, its name goes inside the brackets: "(OS Build
  27100.1000, Release Preview)".
- **The update file (`update.inf`)** now carries `channel=` and `level=`,
  and from HavenDOS 1 also `product`, `release`, `codename`, `build` and
  `revision`. Updaters from before 0.9.7 ignore the new keys, and
  `version=` is still what gets compared. The comparison is proved, not
  assumed: 0.9.9.10 < 27100.0999 < 27100.1000 < 27100.1001 < 27101.1000.
- **Channels.** A system follows one: Beta, Public Beta, Release Preview
  or Stable.
  - `update` refuses an update from a less stable channel, by name,
    before writing anything, and says which command takes it anyway.
  - `update channel` shows which channel the system follows, and
    `update channel stable` changes it. The choice is a file beside the
    kernel (`/boot/channel.inf`), so promoting a build never changes the
    kernel's bytes.
  - With no file, a system follows its own channel: a Beta follows Beta,
    and HavenDOS 1 on Stable follows Stable. (Defaulting a 0.9.x Beta to
    Stable would refuse the next 0.9.x, since every one of them is a
    Beta.)
  - A manifest from before 0.9.7 names no channel, and is taken as it
    always was.
- **Release names** come from one script, `tools/release_names.py`, used
  by the workflow: `v0.9.7` and `HavenDOS-0.9.7.iso` now; `27100.1000`
  and `HavenDOS-1-27H1.iso` from HavenDOS 1. Everything below Stable is
  a pre-release.

## The editor: select with the mouse, one clipboard (exception E-2)

- **The editor reads the mouse on every pass.** Before, it only looked
  after a key, so on the HP the pointer never showed there; that was one
  of the "cursor doesn't appear everywhere" places.
- **Click** puts the cursor where you point, **drag** selects, and
  **Shift+click** extends the selection. **The wheel** scrolls three
  lines a notch.
- **Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+A** as before, but now through **one
  clipboard every app shares** (`shell/clipboard.c`). Copy in the editor
  and **Ctrl+V at the Shell's prompt** pastes it (its first line).
- **The right-click menu** (Copy, Cut, Paste, Select All) now works with
  the mouse.

## Files: a right-click menu (exception E-3)

**Right-click a file or folder,** or press F2, for Open, Rename, Copy,
Move, Delete and New folder. Right-click empty space for New folder.

- **Delete** asks first. A folder must be empty.
- **Move** renames into another folder where the file system can (FAT32).
  Where it can't (FAT16), it copies, checks the copy, and only then
  deletes the original.
- **Every action is judged by what happened on disk,** not by a return
  code (the FAT drivers disagree on those), and every failure is named.
- **The menu works with the mouse** (hover, click, click away) and with
  the keys.

## The taskbar: pins, and smaller icons (exception E-4)

- **Pinned apps sit beside Start,** on the desktop's bar and on the bar
  under every app. There are up to eight, and Files is pinned by
  default. Right-click an app and choose **Pin to taskbar** or **Unpin
  from taskbar**. They're kept in `TASKBAR.CFG`.
- **Small desktop icons:** one row per app, the mark and the name side by
  side, about four times as many on the screen. Settings > Appearance >
  Desktop icons, or right-click the desktop.

## New: Retro PC — the old builds, running inside HavenDOS

Kyan, 17 November, sending `kernel.img`: *"this is the first ever build
i still have i think that survived everything maybe add is as an
emulator including in HavenDOS."*

By his decision this is built and lands in 0.9.7, as exception E-1 in
THS-RM-1005.

Open **Retro PC** from the Start menu, or type `retro` in the Shell.

- **Built in: K-DOS Beta 1.1b**, the oldest build that survived. Log in
  as `admin`, password `1234`. Its command bar, `help`, `time`, `calc`,
  `save` and `load` behave as they did on a real PC, checked screen for
  screen against QEMU's own PC.
- **Your own images:** put a `.IMG` floppy image (up to 2.88 MB) in
  `RETRO` and it appears in the list. `retro FILE` boots one straight
  from the Shell.
- **Each old PC gets its own hard disk:** a 504 KB pretend disk, kept as
  a file beside it (`RETRO/KDOS11B.HDD` for K-DOS). It is saved when you
  leave. It is only ever that file: the old PC has no way to reach a
  real disk.
- **Leaving:** F4 or the window's X takes you back to the list, and Q
  closes Retro PC. ESC goes to the old PC, because old programs used it.

### What the old PC is

- An **80186 with the 386's 16-bit additions**. K-DOS was assembled for
  a 386 and uses them: the long conditional jumps, SETcc, MOVZX/MOVSX,
  the bit instructions, SHLD/SHRD, FS and GS.
- A **BIOS** that does the screen (text modes), the keyboard, the disks,
  the clock and the timer, and boots from the floppy with INT 19h.
- An **8259 interrupt controller, an 8253 timer and an 8042 keyboard
  port**, so programs that hook the timer or the keyboard interrupt work.

### When an old program asks for something it does not have

It says so on screen and in `dmesg` (lines start `retro:`), never
silently:
- **32-bit instructions** (the 66h and 67h prefixes), CPUID, and the
  protected-mode instructions stop the old PC. A box names the
  instruction, its bytes and its address. R restarts the PC.
- **A graphics mode, a call to DOS, or a BIOS service that is not
  provided** shows a notice saying which one, and the program carries on
  if it can.
- **A disk with no boot signature** is refused with that reason.

### How it was checked

- `test_emu86`, 86 checks: the 186 and 386 additions, what is refused,
  interrupt delivery while one is in service, the BIOS services, and
  K-DOS logging in and running its commands.
- The CPU was held to the **SingleStepTests 8088 suite**, real-chip
  captures, and matches all 2,522,158 cases it can be held to. The few
  where an 8088 and a 186 differ are skipped and listed.
- `check_emu86_suite` plants 28 defects in the emulator, one at a time,
  and the suite catches every one. It correctly misses the one harmless
  change.
- `harness/retro_boot.py` boots the HavenDOS ISO in QEMU, opens Retro
  PC, logs into K-DOS and runs its commands: 9/9.

### Found on the way, in K-DOS itself

These are K-DOS Beta 1.1b's own behaviour. The emulator reproduces them
on purpose, and QEMU's PC does the same:
- `save` writes to the disk head in DH, which is only right by chance.
  After `time`, DH holds the seconds, so the write goes nowhere while
  K-DOS still prints `[OK]`.
- `taskman` garbles the screen, and the next key returns to the login.

### Also in this change

- **A USB keyboard's F1–F4** now reach apps, as they already did on the
  PS/2 keyboard.
- **The window's X** can be read by an app that wants it to mean
  something other than ESC (`chrome_take_close()`). Retro PC is the
  first to use it.

## Tests

- **New suites:**
  - `test_verinfo` (43): the display string, channels however they are
    typed, and the subscription rule.
  - `test_clipboard` (11).
  - `test_emu86` (86, Retro PC).
- **Grown suites:**
  - `test_vercmp` 39: HavenDOS 1's orderings.
  - `test_update` 169: the new manifest keys and their refusals.
  - `test_usbhid` 49: a real wheel mouse in report protocol, report IDs,
    a 300-count move, and the fallbacks.
  - `test_window` 40: the pins on the bar.
- **Injection harnesses:**
  - `check_verinfo_suite`, new: 8/8. A ninth injection proved not to be
    a defect and is recorded.
  - `check_usbhid_suite` 13/13. Its first run missed the absolute
    pointer; that case is in the suite now.
  - `check_emu86_suite` 28/28.
- 7654/7654 checks across 75 suites.
- **QEMU:**
  - `features_097`, new, 10/10: the report-protocol mouse;
    `update channel`; a mouse drag selecting text in the editor, Ctrl+C,
    then Ctrl+V at the Shell's prompt; the taskbar pin opening Files;
    Files' right-click menu; Rename renaming the file on disk.
  - `retro_boot` 9/9 (twice), `fixes_0966` 9/9, `window_apps` 11/11,
    `mouse_apps` 14/14 and `hid_boot` 3/3.

## Still waiting on the HP

- **The trackpad.** A photo of `dmesg | grep I2C` says whether the
  controllers time out or the trackpad answers.
- **The monitor's unscaled fallback.** A photo of `dmesg | grep monitor`
  shows where the scaled picture stops.
- **Where else the cursor is missing.** The editor was one place, and is
  fixed.
