# HavenDOS v0.8.8.4 "Helios beta 5" — an app can crash, the screen is yours, and there is a minefield

Channel Beta, codename "Helios", **beta 5**. 2 November 2026.

| release | called | what it was |
|---|---|---|
| v0.8.8 | **Helios beta 1** | the console grid, and the timer tick |
| v0.8.8.1 | **Helios beta 2** | the mouse, exFAT, the bignum |
| v0.8.8.2 | **Helios beta 3** | the stretch, the stick, the primality bug |
| v0.8.8.3 | **Helios beta 4** | the keygen in seconds, and an I2C bus |
| v0.8.8.4 | **Helios beta 5** | this one |

The first four betas were all under the hood. This one is the one you
can see.

---

## 1. An app can crash without taking the machine with it

Before this, every CPU exception went to `isr_handler()`, which painted
KERNEL PANIC and halted. A divide by zero in the calculator, a null
dereference in the editor, a bad opcode anywhere: the machine stopped
and the only way out was the power button.

Now the desktop arms a guard before it calls an app and disarms it
after. A fault while the guard is armed restores the desktop's own
stack, returns there, and the desktop says which program stopped.

```
crash where     is a guard armed right now?  (faults nothing)
crash div       divide by zero          (#DE)
crash null      write through a null    (#PF)
crash opcode    an invalid instruction  (#UD)
crash wild      call a wild pointer     (#PF)
crash stack     run the stack off its end -- NEEDS A REBOOT
```

### `crash` is not a toy, it is the only way to test this

A recovery path is the one kind of code that cannot be proved by a
suite: it is reached only from a real CPU exception, and a native
suite has none to offer. A unit test could call `appguard_enter()` and
`appguard_fault()` in sequence and watch a flag change, and that would
say nothing whatever about whether `iret` lands where the header says
it lands.

That is the exact shape of the bug beta 3 found in `net/rsa.c` --
`for(r = 0; r < 1; r++)`, Miller-Rabin running one round for ever,
with 361 checks passing over it because none of them ever asked for a
second round.

So `harness/appguard_boot.py` boots the ISO and crashes apps on
purpose, four ways, five times in a row, and reads the screen. **19
checks. It found three bugs, two of which would have shipped.**

### Bug one: the recovery returned to garbage

`EIP 0x7`. `appguard_enter()` was an ordinary function, and it called
setjmp **in its own frame and then returned**. The app's stack then
grew over that dead frame, so restoring `esp` and returning there
landed in the tail of a function whose saved registers had been
written all over -- and `ret` jumped to whatever the first four of
those bytes happened to be.

Restoring the callee-saved registers from the jump buffer does not
help: the epilogue pops over them again straight afterwards.

It is a macro now, expanded into `desktop_run_app()`, whose frame
stays on the stack for exactly as long as the app does. This is the
same reason the C standard requires `setjmp` itself to be a macro and
makes calling it through a function pointer undefined.

### Bug two: the recovery window was on screen for less than a frame

`desktop_app_crashed()` ended in `keyboard_getchar()`, which returned
**immediately, every time** -- the app that had just faulted still had
unread keystrokes in the buffer, at minimum the ENTER that ran the
command that crashed it.

So the recovery worked perfectly and the notice flashed and vanished.
From the outside, an app crash looked like the app quitting with no
explanation at all. The harness reported "no recovery window" four
times while the machine was working correctly, and that is the only
reason this was found: nothing about it is visible from the code, and
a person would have reported it as *"it just closes"*.

It drains the dead app's keys now, holds the notice for ~600 ms in
case a key is already on its way down, and then waits for one the
person meant.

### What is caught, and what is not

```
caught       divide by zero, a jump through a null or wild pointer,
             an invalid opcode, a read or write to an unmapped page
NOT caught   a write to kernel memory that SUCCEEDS
```

There is no per-task address space here -- one page directory, one flat
4 GB mapping, everything in ring 0 -- so an app can write anywhere it
likes and nothing stops it. This catches the fault *after* the damage.
It turns most crashes from "reboot the machine" into "that app
stopped", and it is **not a sandbox**.

`crash stack` is the demonstration of that, and it is kept rather than
removed for exactly that reason. Measured: the stack walks down through
mapped memory for a long way first, zeroing the heap and the console's
shadow buffer, and the result is a corrupted screen and a machine that
stops answering -- no panic, no recovery, nothing to read. It is
labelled in the command as the one that needs a reboot.

> `crash wild` first called `0xF0000000` on the reasoning that it is
> far above installed RAM. **It did not fault.** This kernel maps 4 GB
> flat, so that is a perfectly valid linear address; the call went
> there, read whatever the chipset returns for unbacked memory, and
> executed it. An address being outside RAM does not make it unmapped.
> It targets the unmapped first page now.

> A double fault is still a panic, by design: #DF means the CPU could
> not deliver an earlier exception, usually because the stack has gone,
> and the handler that would recover has no stack to run on either.
> The harness **measures** which of the two a stack overflow produces
> rather than asserting it, because that depends on the memory map and
> not on the guard.

## 2. Your screen is yours: apps are not nailed to 80x25 any more

v0.7.18 made the desktop's own chrome derive from the real grid, and
its header recorded what it could not reach:

> *"Apps launched FROM the desktop still lay themselves out for 80x25
> and render in the top-left of a larger screen -- converting those is
> a separate pass and is not claimed here."*

This is that pass. **No app was modified.**

There are forty-three of them and they place text at absolute columns
and rows: `vga_puts_at("CPU Activity",8,4,...)`, a 60-cell bar drawn
from column 8, `ui_draw_window(4,2,72,20,...)`. Rewriting all of that
by hand is several thousand lines of blind diff over layouts that can
only be checked by opening each app on each grid, and the likely result
is forty-three apps slightly wrong in new ways instead of consistently
wrong in one old way.

**So the grid lies to them instead.** An app that lays out for 80x25
is given an 80x25 console: `vga_cols()` returns 80, `vga_rows()`
returns 25, and its coordinates are translated into a centred box with
a clip. One transform, in the one function every cell this driver ever
writes goes through.

```
         ..........................................
         . PAINT  Arrows=Move  Space=Draw  C=Clear .
         .                                         .
         .|_                                      |.
            ...  (Paint, centred on a 100x50 grid)
```

A one-cell rule is painted round the box so it reads as a window on a
desk rather than as a screen that failed to finish drawing.

It is **not a window system**: one viewport, no nesting, no stacking,
because one app runs at a time. `vga_clear()` clears the viewport and
not the screen. The mouse already clamped to the logical grid, so it
lands in the app's own coordinates for free. The caret is translated
too.

An app that *does* use the real grid says so in the registry
(`fits=1`) and is handed the whole screen. Two do: the shell, and the
new one below.

> A viewport bigger than the panel is **refused, not clamped**. An app
> told it has 80 columns inside a 70-column box would draw its
> right-hand edge into the clip and never find out. A refusal hands
> back 0 and the app runs full-screen, which is what happened before
> this existed.

### The suite found nothing. The harness found something.

`tests/test_console.c` gained 42 checks, every one reading the
**simulated VGA memory** rather than asking the driver where it put
things -- asking the translation to confirm itself is the mistake
`tests/test_usbmux.c` made at 0.8.7.6, where "a fixture written from
the same guess as the code agrees with it perfectly."

259 checks, all passing. Then
`tools/check_console_suite.py` planted eight new defects and one of
them survived:

> **"the hardware cursor is placed in app coordinates" — MISSED.**
> The caret of an app in a centred viewport would blink in the
> top-left of the panel while its text appeared in the middle. The
> text being right is what makes that read as a cursor bug rather
> than a missing offset, so it is exactly the kind of thing that
> ships. **Nothing in 252 checks had ever looked at where the caret
> went.**

The suite captures the position off the simulated CRTC now. All 25
injections caught, including two whose anchors this release's rewrite
had broken -- repaired against the real source, not deleted.

## 3. The Windows key opens the start menu

It was being **discarded**. PS/2 sends `E0 5B` for the left one and
`E0 5C` for the right, and both fell past the switch in
`drivers/keyboard.c`'s extended block and were dropped, so the key did
nothing anywhere in the system.

On USB it is not a key at all: it is bit 3 (left) or bit 7 (right) of
the modifier byte, so `usage_to_key()` could never see it and
`usb/hid.c` needed its own edge detection. A keyboard where the
Windows key worked on PS/2 and not on USB is exactly the sort of thing
that goes unnoticed until it is on a laptop.

Both sides report one `KEY_SUPER`, and it toggles the start menu from
any state -- handled above the per-state branches, so it can close the
menu it opened. ESC still works.

## 4. A loading screen, with a loading circle

A logo, a turning ring, and the name of the thing the kernel is doing
right now, instead of the blank screen `quiet` used to give.

```
                      HavenDOS
                   v0.8.8.4 Beta
                      "Helios"
                       .  .  o
                     .         o
                    .           O
                     .         .
                       .  .  .
              VirtIO blk: not present
           press any key for the boot log
```

**It is driven by `klog()`, not by a timer.** Every line the kernel
records advances the ring one position and replaces the caption, so it
turns exactly as fast as the machine is getting things done -- and
when it stops turning, something has stopped.

There is no percentage, because the only honest source for "how many
steps is a boot" is a previous boot of the same machine, which the
first boot does not have and a changed configuration invalidates.

`shell/bootanim.c`'s header is the warning this was written against:
that file used to be "260 lines of theatre" -- a POST screen for a
test SeaBIOS had already done, a boot log of twenty string literals
"four of them untrue on any given machine", and two mandatory
keypresses.

**The caption keeps `include/klog.h`'s promise.** It is the last
recorded line in full, so a kernel that wedges during init leaves the
name of the step it wedged in on the screen -- the same information
the scrolling log would have left, in one line instead of
twenty-four. The ring buffer still holds every line for `dmesg`, and
any keypress drops the screen and replays the whole log.

The ring is twelve positions on an **ellipse**, as a table of integer
offsets -- there is no FPU set up here. The x offsets are twice the y
offsets because a text cell is twice as tall as it is wide, which is
the same correction `fbc_fit()` got wrong in v0.8.8.

Shell and Safe mode keep their scrolling log: those are the diagnostic
entries, and a shell that opened on a cleared screen after a picture
would have thrown away the one thing the person came for.

> The first version drew the wordmark **inside** the ring. On a real
> boot the head landed one cell left of the version string and the
> screen read `Ov0.8.8.4 Beta` -- the spinner looking like a typo in
> the version number -- while the wordmark, drawn after the ring,
> painted over three of the five tail positions.

> And a leftover GRUB keystroke dismissed the whole thing before
> anyone saw it. The ENTER that chose the boot entry was still in the
> 8042's buffer, so the first log line concluded the person had asked
> for the boot log. Same shape as the crash notice being closed by the
> dead app's unread ENTER, found in the same release. Both are
> drained now.

## 5. Minefield

A new app, and the first one written for the real grid.

```
 # # # # # # # # # # # # # # # # # # # # # # # # # #
 # # # # # # # # # # # 3 1 3 # # # # # # # # # # # #
 # # # # # # # 2 1 1 . 1 1 3 # # # # # # # # # # # #
 # # # # # # # 1 . . . . . 2 # # # # # # # # # # # #
 # # # # # # # 1 1 1 1 . . 3 # # # # # # # # # # # #
 Mines 124     Time 8        Normal
```

It uses three things this release added and nothing else does:

- **the real grid** — the board is sized from `vga_cols()`/`vga_rows()`,
  so a bigger console is a bigger minefield. A bigger screen is a
  reason to have a bigger screen.
- **the mouse** — left reveals, right flags. v0.8.8.1's pointer work
  has had no app that depended on it; now one does.
- **the tick** — the main loop runs the periodic work, so the clock
  keeps time and DHCP keeps retrying through a long game.

Cells are two columns wide, because a text cell is twice as tall as it
is wide and a board drawn one column per cell is a board of tall thin
rectangles. Classic number colours, because the colour is how a player
reads a neighbourhood at a glance instead of one cell at a time.

### The rules are in a file with no screen in it

`shell/mines_model.c` calls **nothing** -- no vga, no keyboard, no
heap, no clock. So `tests/test_mines.c` links it directly, with no
seam, no simulated hardware and no stub, and every one of its 79
checks reads the board.

That is deliberate. Both of this project's fixture failures were
suites checking a fixture instead of a behaviour, and neither could
have happened to a model with no outside world in it.

The first click is never a mine **and is always a zero**, so the
opening move opens a region rather than a single number. That means
nine cells must stay clear, which means the mine count has an upper
bound, which `mines_begin()` refuses rather than clamping -- a clamp
would leave the placement loop looking for somewhere to put a mine
that does not exist.

The flood fill is **iterative**, with its frontier in bss. A recursive
fill over 9600 cells is up to 9600 frames, `boot/boot.S` reserves
262144 bytes of stack, and this release's own `crash stack` measured
what going off the end does.

### 79 of 79 first run, which is the least trustworthy result there is

`tools/check_mines_suite.py` plants nineteen defects. **Four of them
survived the first run**, and all four were real gaps:

| gap | what was wrong |
|---|---|
| a lost game accepting moves | the check read `count_shown() == (reveal(), count_shown())` and **C does not specify which side of `==` is evaluated first** -- it compared the board to itself |
| xorshift's dead state | nothing passed seed 0, the one seed that makes the generator return 0 for ever. And the first fix did not work either: the deterministic fallback in `place()` spreads the mines across rows and gives the right total, so the check had to look at *where* they land |
| a recursive flood fill | **a native run cannot see it.** Linux gives the process eight megabytes and a recursive fill completes comfortably. The suite runs the fill on 131072 bytes -- half the kernel's stack -- in a forked child through `makecontext()` |
| a flag-counter anchor | two identical lines in two functions; the injection matched both and was reported stale rather than silently damaging one |

**And two of the planted defects turned out not to be defects at all.**
Both are kept in the model with a note, and recorded in the harness
rather than deleted:

- Removing the mine check from `flood()`'s inner loop. A cell enters
  the queue only when it has no adjacent mines, so no neighbour of a
  queued cell can be a mine, by the definition of zero. Measured over
  3000 boards: **1,499,471 neighbour examinations of queued cells, of
  which 0 were mines.**
- Removing the flag decrement from `show_one()`. Nothing can hand it a
  flagged cell: `mines_reveal()` refuses a flagged click and `flood()`
  skips flagged neighbours. Measured: 2000 boards, 120 random flags
  each, every cell clicked -- **7700 cells revealed, of which 0 were
  flagged.**

Both are dead *because of a decision made elsewhere*, and the first
feature that queues a non-zero cell or opens a flagged one makes them
live again. One comparison each against a silently drifting counter.

All 17 of the remaining injections caught.

## 6. The screensaver is off by default — and `screensaver off` now works at all

`screensaver off` set the timeout to 0 and printed "Screensaver
disabled." Settings showed the number and let it be changed. **Neither
of them did anything**, because the desktop's trigger was a literal
`3000` and never read the config -- so it came on after thirty seconds
no matter what the machine had been told, and the one place a person
would go to turn it off reported success without effect.

Found while looking for something else: `harness/appguard_boot.py`
waits for a crash-recovery window, and three of its four checks failed
reading a screen full of falling characters. The recovery had worked;
the screensaver had painted over the evidence.

It reads the setting now, and the default is **0 — off**.

## 7. Nothing long-running stops the clock any more

`sleep_ms()` polls the PIT and nothing else, deliberately. So any app
with its own idle loop stopped the periodic work for as long as it
ran, and before v0.8.8 wired the clients up nothing noticed, because
nothing depended on the tick.

Eight loops now run the periodic work:

| where | what it was |
|---|---|
| `shell/tetris.c` | the game tick -- a long game stopped the clock, the DHCP retries and hotplug for its whole length |
| `shell/chat.c` x2 | a login prompt waiting for a person to type. One called `net_poll()` -- the receive path -- and not `kernel_poll()`, which is the periodic work and a different thing |
| `shell/screensaver.c` x3 | the longest-running screen there is. A machine left overnight came back with no address and no notice of anything plugged in |
| `shell/shell_v076.c` | the ten-second confirm-or-revert countdown, on the one screen where the machine may be about to be told its display is unreadable |
| `shell/mines.c` | the new app's event loop |

Each is one marked line, and `tools/check_kpoll_sites.py` still reports
periodic work running from only the two files allowed to start it.

> One of these was nearly a lie. The minefield's loop first had no poll
> of its own and a comment claiming the work ran from
> `keyboard_waitchar()`. It did not -- that loop never calls it,
> because it cannot block. The comment described the behaviour the code
> was *supposed* to have, which is the one direction a comment must
> never be wrong in.

---

## The numbers

| | v0.8.8.3 | v0.8.8.4 |
|---|---|---|
| suites | 46 | **48** |
| checks | 4514 | **4635** |
| injection harnesses | 29 | **30** |
| boot harnesses | 12 | **13** |
| `test_mines` | — | **79** |
| `test_console` | 217 | **259** |
| mines injections | — | **17 of 17**, plus 2 recorded non-defects |
| console injections | 17 of 17 | **25 of 25** |
| `appguard_boot` | — | **19 of 19** |

## What is NOT verified

- **The I2C driver has still never seen an I2C controller**, and
  `i2c scan` has still never scanned anything. QEMU has none. The
  first real run is the HP, and that has not happened yet.
- **The viewport has only been seen on a 100x50 text grid.** The
  framebuffer console takes the same path -- the transform is in
  `vga_write()`, which both consoles go through -- but a 1920x1080
  panel with a centred app has not been looked at.
- **The Windows key has only been tested on PS/2**, in QEMU. The USB
  half is a modifier-byte edge and is checked by reading, not by a
  keyboard.
- **Minefield's mouse has only been tested in QEMU's PS/2 pointer.**
  The HP's trackpad still does not answer the PS/2 aux port at all --
  that is what the I2C work is for.
- **Still unconfirmed from earlier betas:** the USB mouse, exFAT on a
  real stick, the 8x16 font on a 1920x1080 panel, and any TLS
  handshake since `rsa_public_encrypt()` moved onto the bignum.

## Still open

- **The FAT32 stick.** Reported as *"it says the port didn't answer"*,
  which is **USB enumeration failing**, not a filesystem problem.
  `usb -vv` prints which of three causes it is and
  `xhci_attach_why()` names the step that refused. That one line is
  still the blocker and has been for six releases.
- **I2C-HID.** Beta 4 is the bus; reading the HID descriptor, the
  report descriptor, and polling input reports is the next one, and it
  needs the scan result from the HP first.
- **Real app isolation.** A page directory per task and user-mode
  segments, which is a release of its own and a large one. Until then
  the guard catches the fault, not the damage.
- **NTFS read-only**, **ext4**. Requested, not started.
- **Stage 3 from the certificates on**: DER, X.509 v3, PKCS#1 signing,
  a plist writer, PEM→DER base64.
