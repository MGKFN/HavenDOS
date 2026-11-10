# HavenDOS v0.8.8 "Helios" — any grid, and the tick

Channel Beta, codename "Helios". 31 October 2026.

Two things, and a correction that turned out to be a correction to the
correction.

**The console stopped being four register tables.** A framebuffer
console can now have any grid up to 32400 cells — 240x135 is 1920x1080
at one pixel per pixel — where before it was clamped to 90x60, which is
the largest grid a *VGA CRTC* can be programmed to.

**And something in this kernel runs on a timer.** IRQ0 sets due flags;
one `kernel_poll()` runs the work, from the keyboard's idle loop. DHCP
retries with backoff, carrier transitions are reported, and a device
plugged in after boot is noticed without `usb rescan`.

The phrase *"nothing in this kernel runs on a timer"* came out of six
places in `HANDOFF.md` and three in the source.

---

## 1. The cap came from the wrong hardware

`VGA_COLS_MAX` was 90 and `VGA_ROWS_MAX` was 60 because 720 pixels over
an 8-dot character clock is 90 columns and 480 scanlines over an 8-line
cell is 60 rows. Those are facts about a CRTC.

`fbc_fit()` clamped the **framebuffer** grid to them, and
`vga_set_mode()` refused anything that was not one of the four text
modes **before it looked at which console was active**. So on a machine
with no VGA text memory at all — which is every UEFI laptop made this
decade — the thing deciding how much of the screen HavenDOS could use
was a register table that machine cannot execute.

That is the whole of "the console cannot express a 1080p grid": not a
missing feature, a cap imported from hardware that is not there.

### What changed

- **Two maxima, because they are two different things.**
  `VGA_TEXT_COLS_MAX`/`VGA_TEXT_ROWS_MAX` are what a CRTC can be told;
  `VGA_COLS_MAX`/`VGA_ROWS_MAX` are how far an index may reach.
- **`VGA_CELLS_MAX` is the real bound** — 32400 cells, 127 KB of BSS
  for `shadow[]` and `fbc_painted[]` together — and it is deliberately
  **smaller** than the product of the axes. See below.
- **`vga_set_mode()` validates against the right thing.** On a
  framebuffer: the axis limits, the cell budget, and the panel. On VGA
  text memory: the four register sets, untouched.
- **`vga_fb_best_grid()`** gives the largest grid that *fills* the
  panel without exceeding the budget — raising the glyph scale rather
  than letterboxing. 1920x1080 is 240x135 at 1x; 2560x1440 is 160x90 at
  2x; 3840x2160 is 240x135 at 2x. All three divide exactly.
- **`resolution` takes any grid, plus `max`.** It prints the panel in
  pixels and the largest grid that fits, and on a text console it still
  prints the four modes — rather than, as before, printing advice about
  720x400 timings to a machine that has no text memory.

### And the refusal has a name

`vga_set_mode()` returned 0 for four unrelated causes and the shell
printed one sentence naming the four text modes for all of them. On a
1080p panel `resolution 300x40` and `resolution 200x200` are refused
for completely different reasons, and both were answered with advice
about 90x60 — a mode that machine cannot run.

`vga_mode_why()` now returns `VGA_MODE_ETEXT`, `ERANGE`, `EBUDGET` or
`EPANEL`, and `resolution` prints the remedy for the one it got. It is
read *after* a refusal rather than returned, so the three existing
callers are unchanged.

This is `x_control()` returning `(3)` for six different causes, in a
screen instead of a driver.

### Two moved register writes

The sequencer reset and the `MISC_W` write used to happen **before**
the `fbc_active` branch, i.e. they were issued to a card GRUB had
already put in a graphics mode — on the way into a branch whose own
comment explains that doing exactly that "would tear it down and leave
a blank screen". Nothing ever saw it, because those two are harmless
alone and the real teardown is further down in code the branch did
skip. A guard that stops three of five writes is not a guard.

### The panic screen painted 80x25

`isr_handler()` cleared a fixed 80x25 rectangle red. Correct while that
was the largest grid there was; on a 240x135 framebuffer console it is
a small red box in the top-left sixth of a screen still showing the
desktop. Found while writing `tools/check_console_grid.py`, and it is
the one screen in the system that must not be half-drawn.

### A seam, and the console it finally put under test

The framebuffer console has been testable since it was written — it
takes the address it draws into as an argument. The **text** console
never has, because `0xB8000` was written into `drivers/vga.c`, and a
native suite that let one successful 80x25 mode change through died on
the first store.

So the console with sixteen years of VGA register programming behind it
was the one no suite could compile, and v0.7.12's five `VGA_WIDTH`
stride bugs were all found by eye. `VGA_MEM_PTR` is one override, the
same idea as `io.h`'s `IO_H` and `machine_asm.h`, and both consoles are
now compiled by `tests/test_console.c` — including the thing worth
testing most, that the two of them agree.

### A recorded difference between the two consoles

CP437 `0xDB` is a solid block. The BIOS 8x16 font has it, so 80x25
draws it; `font8x8[]` is all zeroes from 0x80 up except the accented
letters `tools/make_font_ext.py` generates, so on the framebuffer
console it is a blank cell.

**Nothing is broken by it today**: `shell/ui.c` draws every bar, window
and highlight as a space in a background colour. It is recorded because
the next person to reach for a shade character for a progress bar will
get one on BIOS and nothing under UEFI — and because a difference
between two consoles that both "work" is what the new seam exists to be
able to see.

---

## 2. The tick, as a bottom half

### The timer was already running

IRQ0 has been unmasked since there was an IDT (`idt.c` writes `0xFC` to
port `0x21`), `irq_handler()` has fired at 100 Hz, and `pit_tick()` has
incremented a counter and done nothing else. Nothing was hung off it.

`kernel/idt.c`'s own header records that `irq_common_stub` was missing
its `push %esp` for an unknown number of releases, so `irq_handler()`
read `r->int_no` out of the saved DS selector and *"the kernel was
picking a function pointer out of an array by lottery, on every timer
tick"*. **Nothing noticed, because nothing depended on it.** Something
depends on it now, and `kpoll` prints the tick count — which is the
first evidence this project has had that IRQ0 reaches a handler.

### The tick does no work, and that is the whole design

Every layer in this tree shares module-level buffers and not one is
reentrant: `g_bounce` and `g_inctx` in `usb/xhci_hc.c`, `g_cfgbuf` in
`usb/core.c` (whose own comment calls it borrowed), `g_rx`/`g_tx`/
`g_ctl` in `usb/ethernet.c`, `eth_rx_buf` and a static frame inside
`eth_send()`, and the sector buffers under `fs/`.

An IRQ0 that called `net_poll()` while the shell was part-way through
`usb_control()` would corrupt that transfer. The symptom is a
once-an-hour impossibility, and v0.8.7.5's shared event ring is this
project's record of what one costs: an unanswered DHCP write broke
descriptor reads on a webcam, and it took a release to find.

So `kpoll_tick()` counts and sets flags. `kernel_poll()` runs the work.
The read-and-clear between them is the one critical section, and it
uses `pushf/cli/popf` rather than a bare `cli/sti` pair — because
`kernel_poll()` is reached from paths that may already have interrupts
off, and a bare `sti` would turn them on behind the caller's back.

### And the rules that keep it there are mechanical

A design that depends on nobody calling the wrong function is a
comment. Three new build checks:

| check | what it refuses |
|---|---|
| `check_kpoll_sites.py` | `kernel_poll()` outside two named files; `kpoll_sleep_ms()` anywhere in `drivers/`, `usb/`, `net/` or `fs/`; `kpoll_register()` outside four |
| `check_kpoll_guard.py` | an entry point that loses its `kpoll_enter()`/`kpoll_leave()` bracket, or returns before the leave; a second copy of the rescan sequence |
| `check_console_grid.py` | a compile-time constant used as a stride in `drivers/vga.c`; a cell buffer not sized by the budget; the two maxima collapsing into one |

`sleep_ms()` is unchanged and is the one a driver calls — it polls the
PIT and nothing else. `kpoll_sleep_ms()` is the one that polls, for
idle paths only. They are two names rather than one function with a
flag, because a flag is something a caller gets wrong and a name is
something a grep refuses.

### The guard, as a wrapper

Six entry points bracket themselves: `usb_control()`, `usb_enumerate()`,
`blk_read()`, `blk_write()`, `eth_send()` and `eth_poll()`. Each is a
four-line wrapper round an **unchanged** inner function rather than an
enter/leave pair added to a body — so there is no return path that can
be missed, and the pairing is one line a grep can read.

`kernel_poll()` refuses while the depth is non-zero, and that is **not**
the same check as the reentrancy guard: `fs/fat16.c`'s writable borrow
can be open across a keyboard wait, so "the shell is idle" and "no
subsystem is mid-operation" are different statements.

### Every refusal has a name

`kernel_poll()` returns `KPOLL_RAN`, `ENOINIT`, `EREENTER`, `EBUSY` or
`EOFF`, counted separately, and `kpoll` prints them broken out. "The
poll did not run" has four causes and they are four different bugs: not
initialised is a boot-order mistake, already-polling is a client that
called a polling sleep, a-driver-holds-a-buffer is normal and expected,
and switched-off is somebody having typed `kpoll off`.

### `next_due` is owned by the tick

Not by the poll, and the difference shows up on an unattended machine.
If the main path owned it, a poll that was never reached — a long
install, a large download — would leave `next_due` in the past and the
client would fire on every subsequent poll until it caught up: a
hundred seconds unattended becoming a hundred DHCP attempts in a row.
Owned by the tick, a client that is already due simply stays due, and
`late` counts how often that happened — which is the only number that
can tell "the work is slow" from "the poll is rare".

### Three clients

**`net dhcp` — retry, with backoff.** v0.8.7.2 added a DHCP request at
boot and v0.8.7 added one to `usb rescan`; both are patches around not
having a timer, and until one fired the machine held `10.0.2.15` behind
a gateway at `10.0.2.2` — QEMU's network and nobody else's — with
`ifconfig` reporting "Status: UP" over the top of it. That was the
whole of it on the HP on 26 October.

Five attempts at 10 s, 30 s, 60 s, 2 min, 5 min, then silence until
something changes. The backoff is not fastidiousness:
`dhcp_request()` blocks for about four seconds and this runs from the
keyboard's idle loop, so a fixed ten-second retry on a machine with no
DHCP server would leave the shell unresponsive about 40% of the time,
for ever, and the user would read that as the OS being broken. It also
defers an attempt while a keystroke is already waiting — which does not
make the freeze shorter, it makes it land when nobody is typing.

**`net carrier` — transitions, and it is real for exactly one NIC.**
`usb/ethernet.c`'s `un_link()` issues `IPHETH_REQ_CARRIER` and reports
the device's answer. `drivers/e1000.c` and `drivers/rtl8139.c` have no
`link_up` op at all, and `netdev_link_up()` returns 1 for a NIC with
none — documented in `include/netdev.h` as "this NIC cannot say". So a
carrier poll of a PCI NIC reads a constant, and a client watching that
would be a fixture that cannot distinguish anything, in the kernel
rather than in a test. Said here rather than left for somebody to infer
from an event that never arrives. Reading link state out of the e1000
and RTL8139 status registers is a **driver** change, not a timer
change, and it is recorded as open.

A link that comes back restarts the DHCP sequence: an iPhone hotspot
switched off and on hands out a different subnet, and the address held
came from the old one.

**`usb ports` — hot-plug, off the root ports.** Through the
`port_connected()` op both back-ends already supply: one PORTSC read
and a test of one bit, per port, per second. Pure reads.

That was chosen **over** xHCI's CSC change bit deliberately. CSC would
catch an unplug-and-replug inside one interval, which a bitmap cannot —
but it is write-1-to-clear, so using it means writing to a live
controller register on a timer, on hardware nobody here has read the
specification of, with the existing enumeration path also acking CSC
during a reset. Two writers of one latch. v0.8.7.8 cost a release to
the principle that a guard which has never run against real data is not
a guard; when somebody has a port-change trace off real silicon the
bits are the better answer, and until then they are a guess that
writes.

**Not detected, and recorded rather than claimed:** a device behind a
**hub** (a control transfer per hub per interval, and the HP cannot
test any of it — two boots on 25 October had the stick on root port 0
directly and `usb -vv` showed no hub anywhere), and an unplug-replug
inside one interval. `usb rescan` is still there.

### One copy of the rescan sequence

`usb_rescan_all()` in the new `usb/hotplug.c` is the body that was
inside `usb rescan`. The **order** is its entire content and every step
is there because of a specific bug — release the NIC before the walk
(v0.8.7: a NIC holds an index into a table `usb_enumerate()` rebuilds),
forget lockdownd and close the mux (v0.8.7.7), then enumerate, re-probe
storage (v0.8.6: each device holds a copy of its `usb_dev_t`), rebuild
the block layer, re-probe the NIC.

Two copies of that would drift — which is what
`tools/check_diskname_sync.py` refuses one layer down, and why v0.8.6
moved the disk table out of `shell/iso_installer.c` so a suite could
*link* it rather than copy it. `check_kpoll_guard.py` refuses a second
copy.

---

## 3. The boot log claimed an IRQ — and the note about it was wrong too

```
ok  PS/2: keyboard on IRQ1, mouse on IRQ12
```

The note that found this said the slave PIC stays masked, so IRQ12
cannot fire. **That is false.** `irq_enable_mouse()` unmasks IRQ12 *and*
the cascade on IRQ2, and `kernel/main.c` calls it on the line
immediately before printing that entry.

Three layers have to agree before an IRQ reaches a handler, and the PIC
is only the middle one:

1. **the 8042 must generate it.** `drivers/mouse.c`'s `mouse_init()`
   clears bit 1 of the config byte — *"IRQ12 stays off, we poll
   instead"*. So it never does.
2. **the PIC must not mask it.** This layer was fine.
3. **something must be registered for it.** Nothing is:
   `drivers/keyboard.c` is the only caller of `irq_register()` in the
   tree and it registers IRQ1.

So the mouse is polled, the log line was fiction, and a note blaming
layer 2 would have sent somebody to edit the one layer that was
innocent. **A recorded gap is a hypothesis, not a finding** — v0.8.7.6
wrote down that it could not tell `tx_seq` from `rx_seq` and was right
that the check was impossible and wrong about why.

The line now reads `PS/2: keyboard on IRQ1, mouse polled (8042 IRQ12
off)`, and `check_boot_log.py` refuses a boot log line claiming work on
an IRQ nothing registers a handler for. The unmask stays: it costs
nothing, a spurious IRQ12 is already handled correctly, and a third
edit in this area is not what a release about one claim too many should
add.

### And that check had been reading raw lines for three releases

The new IRQ rule found it on its first run: the comment explaining the
corrected line **quotes the old one**, so the check refused the fix for
the bug it was written to catch. `check_boot_log.py` goes through
`tools/csource.py` now, like the other grep-shaped checks. The other
two rules had the same exposure — a paragraph describing the removed log
table, or quoting a timestamp, would have been refused too.

`csource.py`'s own header says it: a false positive is not a harmless
checker being careful. It teaches whoever hits it to add an exemption,
and an exemption added to silence a checker bug is one that will hide a
real finding later.

---

## Testing

**3601 of 3601 checks**, across the 43 suites `make test` invokes — 42
report and `test_fat16path` skips without a `BOOTDISK=` image, as it
always has. Up from 3299 at v0.8.7.9. **`make test` was RUN, not
reasoned about.**

| suite | checks | what it is for |
|---|---|---|
| `test_console` | 154 | the console grid, on BOTH consoles, at sizes no VGA card has |
| `test_kpoll` | 146 | the tick, the bottom half, the four refusals, and the three clients |

`tests/kpollguard.inc` gives the fifteen existing suites that compile a
guarded module their `kpoll_enter()`/`kpoll_leave()`. It **counts**
rather than doing nothing — the mock-that-cannot-refuse mistake is what
`include/usb.h` has warned about since v0.8.5 — so a suite can assert
`kpollguard_depth() == 0` after the work it drives. `tests/test_kbdinit.c`
uses the same file's counting `kernel_poll()` to assert that the
keyboard's idle loop polls at all: that call has no return value anybody
reads and no screen, so without it nothing in the tree could notice it
being deleted.

### Both new suites passed first run and the harnesses found thirteen holes

| harness | result |
|---|---|
| `check_console_suite.py` | **14 of 14**, harmless correctly missed |
| `check_kpoll_suite.py` | **16 of 16**, harmless correctly missed |
| `check_hotplug_suite.py` | **10 of 10**, harmless correctly missed |
| `check_nettick_suite.py` | **13 of 13**, harmless correctly missed |
| `check_xhci_suite.py` | **82 of 82**, re-run — `usb/core.c` changed |
| `check_usbmux_suite.py` | **66 of 66**, re-run |
| `check_lockdown_suite.py` | **50 of 50**, re-run |
| `check_netdev_suite.py` | **30 of 30**, re-run |
| `check_usbnet_suite.py` | **28 of 28**, re-run |

The thirteen, in the shapes this tree already has names for:

- **TWO REFUSAL CODES WERE UNREACHABLE.** `VGA_CELLS_MAX` was exactly
  `VGA_COLS_MAX * VGA_ROWS_MAX`, so no grid could be inside both axis
  limits and over the budget: `VGA_MODE_EBUDGET` was dead and so was
  `fbc_fit()`'s area clamp, and 124 checks "covered" both without being
  able to reach either. The budget is smaller than the product now —
  which also buys a wide short grid like 320x100 on an ultrawide — and
  `check_console_grid.py` refuses them collapsing again.
- **`vga_grid_capped()` COULD NEVER BE 1.** It asked "was the last fit
  clamped", and `vga_set_mode()` validates everything *before* calling
  `fbc_fit()`. A flag that is never set cannot fail to be cleared, so
  the injection into it was missed. It asks about the **panel** now,
  which is the question the screen needs answered.
- **TWO REDUNDANT WRITES, EACH MASKING THE OTHER.** `usb/hotplug.c`
  recorded the port bitmap inside the change loop *and* re-read it
  after the rescan. Delete either alone and behaviour is identical, so
  an injection into either was missed — v0.8.6.7's failing-only-in-pairs
  shape, and four of `test_netdev`'s seven holes. The redundant one is
  gone; putting it back is recorded as **not a defect**, because the
  post-rescan reading overwrites it either way.
- **TWO CHECKS WERE VACUOUS.** "a NIC that arrives resets the DHCP
  sequence" and "a link coming back resets it" both called
  `net_dhcp_retry_reset()` first and then asserted the counter was
  zero, which it had never left. A fixture that cannot produce the
  before-state cannot test the transition.
- **A GUARD MASKED BY ANOTHER GUARD.** "it asks when there is no NIC at
  all" passed with the no-NIC check deleted, because
  `netdev_link_up(-1)` returns 0 and the carrier check stops it too. The
  test counts the *wire* now: `netdev_link_up` must never be asked
  about index -1.
- **THE WRITE-THROUGH PATH WAS NEVER EXERCISED.** Every pixel check
  called `vga_flush()` first, and `vga_flush()` paints directly — so
  all 134 of them passed with write-through completely disabled. There
  is a check now that does not flush.
- **A FIXTURE'S OWN REPORTING POLLED A SECOND TIME.**
  `ck(..., kernel_poll() == KPOLL_EOFF, kernel_poll())` evaluates the
  `got` argument too, and then asserted a refusal count of 2. It
  passed.
- **A WRAP TEST THAT COULD NOT WRAP.** `0xFFFFFF00 + 100` does not
  cross zero. The check "next_due wrapped" was asserting something that
  had not happened.
- **THE MARGIN CHECK RAN AFTER A MODE SET**, which paints the whole
  framebuffer itself, so deleting `fbc_clear_margin()` from
  `vga_fb_init()` changed nothing it could see.
- **A SUITE COULD NOT REACH `fbc_fit()`'s CLAMPS** through the public
  entry point, because `vga_set_mode()` validates first. It calls the
  static function directly now — it compiles the file, so it can — and
  `drivers/vga.c` records that the clamps are defence in depth.
- **A BOUNDS CHECK READ THROUGH THE ACCESSOR IT WAS TESTING.**
  `vga_shadow_read(239,134)` bounds against the grid itself, so it
  returns the blank whatever is in the buffer. The check reads
  `shadow[]` directly now.
- **A FIXTURE THAT COULD NOT PRODUCE A NULL OP.** "a back-end with no
  `port_connected`" used a mock with neither `port_count` nor
  `port_connected`, so the loop body never ran and the null call never
  happened. There is a third ops table now with the asymmetry
  `include/usb.h` permits: counts its ports, cannot read them.
- **A CHECK THAT COUNTED THE WRONG CONTROLLER'S READS**, and read 4
  because the *other* controller had four ports.

### What booting it found

Four things, none of which any suite could have.

**1. A PERIODIC CLIENT'S LOG LINE LANDS ON TOP OF THE SCREEN.** The
first boot after wiring the clients up printed

```
-- more -- (any key, Q to stop)[  28.874]      dhcp: no addre
ss yet, asking again (try 1 of 5)
```

— a DHCP retry written over `dmesg`'s own pager prompt. `klog`'s live
echo was safe for as long as every call came from a driver bringing
itself up, in a fixed order, before anything was on the screen. The
clients made that false: they log from `kernel_poll()`, which runs from
the keyboard's idle loop, which is to say at exactly the moments the
screen belongs to somebody else. In a text editor it would be worse
than confusing.

`include/klog.h` says the echo exists so that *"a kernel that dies
halfway through init has already said how far it got"*. That argument
is about **init**, and `kernel/main.c` now calls `klog_set_echo(0)`
after "System ready". The ring keeps every line and `dmesg` is the view
of it, which is what a printk ring is for.

**2. THE FRAMEBUFFER TEXT DECODER ASSUMED THE GRID FILLS THE SCREEN.**
`harness/fbtext.py` computed a cell as `W//cols` by `H//rows`.
`drivers/vga.c` has never done that: it picks an integer glyph scale in
each axis and **centres** the result. On 1280x800 at 80x25 the two
agree — 1280/80 is 16 and 16/8 is 2 — and that is the only panel QEMU
has ever been run at here, so the error never showed. On 1366x768 it
gives a 17-pixel cell against the driver's 16 and no 43-pixel left
margin, so the sampling point drifts a pixel per column and the decode
turns to noise a third of the way across.

v0.8.8 is what exposed it: `resolution 100x50` on 1280x800 is a scale
of 1x2 with a 240-pixel margin down each side, and the harness read the
resulting screen as ASCII art and reported a working feature as a
broken one.

**3. TWO COMMANDS PAGE, WITH TWO DIFFERENT PROMPTS.** `dmesg` stops
with `-- more -- (any key, Q to stop)` and `help` with `-- more --
press any key --`. The harness read the first page of each, then typed
the next command into the open pager: `kpoll` was swallowed whole, and
`resolution reset` arrived as `ution reset` with its first three
characters consumed as the keys that dismissed three pages of help.
**The shell then reported a command that does not exist**, which reads
nothing like an unread prompt — four failures and then two more, from
one unhandled prompt each time.

And the obvious fix was wrong: testing whether `-- more --` appears
*anywhere* on screen reports a pager that closed pages ago, because the
prompt is printed in place and stays visible. It has to be the **last**
thing on screen.

**4. THE CONSOLE IS SLOWER AT A BIG GRID.** At 100x50 on 1280x800 a
repaint is 5000 cells of 8x16 physical pixels. A person types at about
five characters a second and will not notice; a harness typing at
twenty can. Recorded rather than papered over.

### The boot harness itself, because a screen is a deliverable

`harness/helios_boot.py` boots the ISO and reads the two new screens.
It is not redundant with 300 suite checks: `tests/test_kpoll.c` drives
`kpoll_tick()` by hand, which is what lets it produce the
interleavings, and therefore proves nothing whatever about IRQ0
arriving. The harness asks `kpoll` twice, four seconds apart, and
requires the tick count to have **moved** — a static non-zero number
would be a counter incremented once during init.

It also checks that the boot log says "mouse polled" and not "mouse on
IRQ12", that `resolution` names the panel, that a grid no CRTC can
express is taken (or refused by name on a text console), and that the
shell still answers at it.

**32 of 32, on the ISO this release built.** The numbers that matter:

```
ok   ...which is not zero -- IRQ0 reaches a handler
ok   ...by roughly four seconds' worth at 100 Hz (825 ticks)
ok   at least one task has a non-zero run count
ok   and 'already polling' stayed at zero
ok   100x50 was accepted and the confirm prompt is up
ok   ...and the grid is really 50 rows deep
```

and, in the boot log of every run:

```
[   3.141]  ok  kpoll: 3 periodic task(s) at 100 Hz
[  28.874]      dhcp: no address yet, asking again (try 1 of 5)
```

That second line is the thing this release exists for: a retry nobody
typed, on a timer, twenty-five seconds after the one-shot at boot gave
up.

---

## What RM-1003 asked for, and what this is

THS-RM-1003 lists **five** work items under 0.8.8 and calls the release
*"any grid, chosen at boot"*. This is not that, and the difference
matters enough to say plainly.

| RM-1003 | here |
|---|---|
| 1. Mode discovery — ask the firmware, enumerate VBE, choose by panel size | **not done** |
| 2. A console sized at runtime | **done**, and it is most of this release |
| 3. The viewport layer | **not done** |
| 4. A confirm-or-revert timer | already existed (v0.7.11); unchanged, and confirmed working on the boot |
| 5. An 8x16 font option | **not done** |

**The auto-detect is deliberately absent, and RM-1003 is the reason.**
Its own box says it: *"Without it this release is a regression. A
240x67 console with 45 apps still drawing at 80x25 puts every one of
them in the top-left corner of a very large screen, which is worse than
the centred box they sit in today."* That is exactly right, and it
applies to picking a big grid at boot whether or not anything else
changed. So the boot grid is still 80x25, the desktop still pins 25
rows, and **nothing regresses**.

What landed is the half that has to come first: the console can now
*express* any grid, there is a build check that keeps the stride
honest, and `tests/test_console.c` compiles both consoles for the first
time. What it is **not** is a console that chooses for itself.

So the honest name for this half is **any grid, chosen by the user**.
The other half — mode discovery, the viewport, and the 8x16 font so
that 240x135 on a 1080p panel is readable rather than merely possible —
is a console release of its own, and the three belong together because
each one alone is a regression.

RM-1003 is otherwise unchanged by this release and still supersedes
RM-1002 in full. It should be regenerated at the next plan revision,
when 0.8.8 can be written up as done and the remaining three items
given a release of their own.

---

## Not done, and named

- **Carrier on the PCI NICs.** No `link_up` op in `drivers/e1000.c` or
  `drivers/rtl8139.c`. A driver change.
- **Hot-plug behind a hub.** The change endpoint is a control transfer
  per hub per interval and the HP appears to have no hub to test it on.
- **Mode discovery, the viewport layer and an 8x16 font** — RM-1003's
  other three items for this release. They belong together: each alone
  is a regression. See "What RM-1003 asked for" above.
- **The desktop still pins 80x25.** Row 22 and row 24 are hardcoded in
  roughly 145 places across `desktop.c`, `ui.c` and `shell.c`;
  `desktop_run()` forces 25 rows on entry and restores the shell's
  preference on exit. Shipping a half-converted desktop would be a new
  class of misplaced-element bug.
- **The eight-line text modes are out of `test_console.c`'s reach.** A
  successful 80x50 or 90x60 calls `vga_font_load8x8()`, which writes
  the font plane through a raw pointer at `0xA0000`, and the `io.h` seam
  does not cover a memory window. What would close it is a second seam
  for the plane — not a stub for the font functions, which would remove
  the thing worth testing (that the BIOS 8x16 plane is saved before the
  first overwrite and restored for a 16-pixel cell, which is the v0.7.11
  bug). Recorded with the reason it actually has.
- **Pairing stage 3** is still the next large piece, and it is next
  after this: a limb-based bignum, Miller-Rabin, RSA keygen, DER, X.509
  v3 and PKCS#1 v1.5 signing. The measurements are in `HANDOFF.md`
  under "WHY HELIOS BEFORE STAGE 3", and the reason this release came
  first is that a 15-25 minute keygen on a kernel where nothing runs on
  a timer is a frozen machine with no way to tell a slow prime search
  from a hang. `kpoll_sleep_ms()` is what that keygen will yield
  through.
- **Eleven injection harnesses still carry their own loop.** Unchanged
  at eleven: `ahci`, `esp`, `fat16fmt`, `fat16gpt`, `fat32`,
  `gptwrite`, `installadd`, `machine`, `nvmerw`, `update`, `xhci`. The
  four added this release all use `tools/inject.py`.

---

## Credentials, unchanged

`admin`=`Admin@123`, `bootos`=`BootOS2026`, `guest`=(empty).
