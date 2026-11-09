# HavenDOS v0.8.5.1 Beta "Atlas" — two reasons v0.8.5 did not work

v0.8.5's headline was *the laptop can be typed at*. It was not true, for
two separate reasons, and only one of them was visible from the screen.

1. **The controller never started**, because its BAR was read 384 GiB
   wrong. That is the one `usb -vv` reported.
2. **Even once it starts, typing does not work**, because the HID poll
   waited a full second on every poll with no key pressed. That one took
   booting it under QEMU with a keyboard attached to see: at 2.5 seconds
   between keystrokes every character arrives, and at human speed three
   arrive and the rest vanish.

The second was found while verifying the fix for the first, which is the
argument for booting a thing rather than trusting a green suite.

---

## Part one: the BAR

On the laptop the controller did not start, and `usb -vv` said so:

> No USB controller this kernel could start. There IS a USB controller,
> programming interface 0x30 (xHCI). xHCI is driven from v0.8.5, so this
> one was found and did not come up. `dmesg` has the reason it gave.

That message was right, and it was the most useful thing on the screen,
and it still was not enough: it did not say what address the driver had
used. This release fixes the defect and makes the log say that.

## What was wrong

From the photograph of `lspci -vv` on the HP:

```
00:14.0  8086:51ED  0C/03/30  USB controller
         BAR0 mem 0x02120000
         BAR1 mem 0x00000060
```

**`BAR1` there is not a BAR.** A PCI memory BAR whose type field (bits
2:1) reads 2 is sixty-four bits wide, and the next BAR register holds its
high dword. So that device has ONE BAR, and the controller is at
**0x60_02120000** — about 384 GiB up, where a UEFI firmware with Above-4G
decoding enabled puts MMIO on a machine with enough RAM to need it.

`pci_bar_mem()` knew nothing about this and returned the low half. The
driver took `0x02120000`, read a capability register out of whatever
happens to sit at 33 MB, got a CAPLENGTH outside the legal `0x10..0x100`,
and refused to start — correctly. A wrong address and a dead controller
look identical from inside `xhci_start()`.

The same repeated `0x00000060` appears under seven devices in that
`lspci` listing. It was never seven BARs. It was one high dword printed
seven times as though it were an address.

## The correct code was already in the tree, in one driver

`drivers/nvme.c` decodes its own 64-bit BAR properly and has since the
framebuffer work:

```c
uint32_t lo = p->bar[0] & 0xFFFFFFF0u;
uint32_t hi = 0;
if((p->bar[0] & 0x06u) == 0x04u) hi = p->bar[1];   /* type 10b = 64-bit */
```

So the knowledge existed. It had been written as a local fix inside the
one driver that needed it, the shared accessor was left naive, and the
next driver trusted the shared accessor.

**That is the 0.8.3.1 post-mortem happening again in a different layer.**
That release found that three of its six bugs were one mistake — *the
installer has one block layer and the filesystem has another, and only
one of them got taught*. This is the same sentence with the words
changed: the NVMe driver had one BAR decoder and the PCI layer had
another, and only one of them was right.

So the decode now lives in the PCI layer, `drivers/nvme.c` is a caller of
it rather than a second copy, and `pci_bar_mem()` no longer truncates:

- `pci_bar_mem64(d, n, &hi)` — both halves.
- `pci_bar_is_64(d, n)` — whether `bar[n+1]` is this BAR's high dword
  rather than a BAR.
- `pci_bar_mem(d, n)` — unchanged for a BAR that fits in 32 bits, and now
  returns **0** for one that does not.

That last one is the important half. Truncating is worse than failing: a
caller handed 0 can say so, and a caller handed the low 32 bits of a
40-bit address reads someone else's memory and calls it a controller.

## Reaching 384 GiB from a 32-bit kernel

`kernel/highfb.c` already does this. It was written for a framebuffer the
firmware put above 4 GB, it maps through PSE-36 or PAE, and it has
**eight windows** rather than one — because, as its own comment says, the
framebuffer is not the only thing up there. The xHCI is the second
tenant; nothing new was needed.

### Except that a device window is not a framebuffer window

Those windows were mapped `PDE_PWT` — write-through. Writes reach memory;
**reads may still be served from cache.** For a framebuffer that is
merely slow. For a controller register it is wrong in a way that is very
hard to find: a driver polling `USBSTS` for the reset bit to clear can
read the same cached dword forever, and the symptom is a controller that
hangs during reset with no bad write anywhere in the trace.

`highfb_map_io()` is new and maps cache-**disabled**. The xHCI uses it and
so does NVMe, which had the same latent problem on any machine that put
its BAR up there.

Being straight about this one: on most real machines the firmware's MTRRs
already mark the above-4G PCI range uncacheable, and the effective memory
type is the stricter of the two — so `PDE_PWT` would probably have
worked. *Probably, because the firmware happened to* is not a guarantee
worth resting a driver on, and the fix is one bit.

## The failure log now says which address it used

`xhci_start()` sets a reason string on every failure path, and the caller
prints it with the address and the CAPLENGTH it rejected. The five
reasons are: CAPLENGTH out of range (wrong address or not an xHCI), no
ports or slots in HCSPARAMS1, CNR never cleared, still halted after
Run/Stop, and — before any of those — a BAR above 4 GB that could not be
mapped.

---

## Part two: a keyboard cannot be polled with a blocking transfer

With the BAR fixed, QEMU boots to a shell with a USB keyboard and mouse
enumerated:

```
[ 0.678] ok  xHCI 00:03.0 at FEBD4000, 8 ports, 8 slots, 32-byte contexts
[ 0.684] ok  USB xHCI hc0 port 4: 0627:0001 class 03, address 1, 1 endpoint(s)
[ 0.687] ok  USB keyboard 0627:0001 on endpoint 81, 8-byte reports
```

And then typing `usb -vv` produced `usb`.

`usb/hid.c` polled with `usb_interrupt()`, which on xHCI is this:

```c
ring_push(...);                                    /* queue the transfer */
ring_doorbell(...);
cc = evt_wait(x, hcno, TRB_XFER_EVENT, 0, &st, 1000);   /* wait up to 1s */
if (cc < 0) return cc;      /* timed out -- and the TRB is STILL QUEUED */
```

**An interrupt IN endpoint with nothing to report NAKs**, and an xHCI
retries a NAK for as long as the transfer is queued. It posts no event: a
NAK is not a completion and not an error. So every poll where no key had
been pressed — which is almost all of them — waited out the entire
timeout, and then returned leaving a live transfer descriptor in a ring
of sixteen.

Two consequences, and the first is the one you feel:

- **The kernel accepts about one keystroke per second.** `usb_hid_poll()`
  is called from the two places that read the keyboard, so every wait for
  input parks for a second per device.
- **The ring fills with transfers nobody is waiting for.** Sixteen polls
  later the enqueue cursor is walking over descriptors the controller
  still intends to complete, into a bounce buffer shared with every other
  transfer.

### What replaces it

`usb_hc_ops_t::intr_poll()` — the shape every real driver uses. Keep
**exactly one** transfer outstanding per endpoint; each poll asks whether
it has completed and, if it has, takes the data and immediately queues
the next. Nothing waits. A key pressed between two polls is sitting in a
completed transfer when the next one looks.

It is optional: a back-end without one falls back to the blocking path,
which is what UHCI does, and correctly — a UHCI transfer descriptor
records a NAK as a status bit the driver reads rather than retrying
silently, so a poll there returns promptly on its own.

Two things the asynchronous path needs that the synchronous one does not:

- **Its own buffer per endpoint.** A transfer the controller has not
  finished with still owns its buffer, so `g_bounce` cannot be used.
- **Transfer events filed BY ENDPOINT.** The event ring is one queue for
  the whole controller, so a poll of the keyboard will meet the mouse's
  completion. `evt_wait()` discards events of the wrong type; doing that
  here would hand the mouse's report to the keyboard. `evt_drain()` files
  each event under the endpoint it belongs to and **stops** at the first
  one it cannot attribute — a command completion, or a synchronous
  caller's transfer — rather than discarding it.

### And the ops tables are bound by name now

`intr_poll` went into the middle of `usb_hc_ops_t`, and both ops tables
were positional initialisers. That silently rebinds every member after
the insertion point: here it would have made `x_configure` the interrupt
poll and left `configure` null, which compiles without a word and breaks
every endpoint beyond the default pipe. Both tables use designated
initialisers now.

---

## The harness was lying, and that is the worst finding here

`tools/check_xhci_suite.py` injects bugs into the driver and demands the
suite notice every one. It now damages **two** files, because the defect
in this release was not in the driver at all.

The first version of that loop restored only the file it was about to
damage. So the moment a BAR bug had been injected, `drivers/pci_bar.c`
stayed broken for all eleven injections after it — and every one of them
reported **caught**, because the suite was failing on the leftover bug
rather than the planted one.

One of those eleven was a real miss, hidden by the false ones.

The loop now restores every file before every injection. With that fixed,
three injections were genuinely missed:

> **only one page of the BAR is mapped** — the capability and operational
> registers are reachable, the doorbell array is not, so the controller
> comes up and never notices a transfer. The suite had reported this
> caught. It was not: the `highfb_map()` stub handed back a working base
> address no matter how little was asked for, so a 4 KB window reached the
> doorbells at 0x1000 perfectly happily. **The simulator was kinder than
> hardware**, which is the lesson v0.8.5 learned ten times over and then
> walked into once more.
>
> **a BAR whose low half is exactly zero is skipped** — 0x60_00000000 is a
> legal base and its `bar[0]` is 0 once masked. Deciding "is there a BAR
> here" from the low half alone throws such a controller away silently.
>
> **the high half is tested for truth instead of the 64-bit type** — a
> 32-bit BAR0 beside an unrelated BAR1 is then mapped into a window built
> from someone else's number.

Fixes: the simulator now models the window's **extent** and counts every
access past it; and the two BAR shapes above have tests of their own.

**A harness that reports a catch it did not earn is worse than no
harness**, because it is the thing you check instead of thinking.

## Also in this release

- **`usb -vv` is paged**, the way `lspci -vv` was in v0.8.5 — he had to
  photograph the screen to read it. Every print in `shell/usb.c` goes
  through one wrapper that counts lines, rather than a page check after
  each of forty prints: that version needs an early return out of three
  nested loops, and the one written in a hurry returns from the inner
  loop only and pages forever. A stop flag instead, so the command
  finishes normally and simply prints nothing more.
- **The pager has a header.** `ui_page_reset()` and `ui_page_line()` were
  declared with a bare `extern` inside `shell/lspci.c` when it had one
  caller. `include/ui.h` now holds them. (`shell/help.c` still has its own
  pager hardcoded to 20 lines, which is already wrong in the 90x60 modes.)
- **`test_pci` had no banner in `make test`**, so its 26 checks were
  printed under the next suite's heading and the suite itself looked
  absent. Cosmetic, and exactly the kind of cosmetic that makes you count
  28 suites when you have 29.
- **Four suites now LINK the real BAR decoder** (`drivers/pci_bar.c`)
  instead of stubbing it. `tests/test_ahci.c` still stubs it, and now says
  why in the file: AHCI's ABAR is BAR5 and the specification makes it 32
  bits wide, so the decode cannot arise there.

## Testing

**1909 checks across 29 suites**, up from 1835. `tests/test_xhci.c` went
from 67 to **130**.

The simulated controller learned two things it did not know, and both
were kindnesses it had been extending:

- **A device may have nothing to say.** It used to complete every Normal
  TRB the instant the doorbell rang, because it always had a report
  ready. A real interrupt endpoint NAKs, the controller puts the TRB back
  and retries, and no event is posted. Without that, the bug above cannot
  be reproduced at all.
- **A device accepts `CLEAR_FEATURE(ENDPOINT_HALT)`.** It used to stall
  it, which made `usb_clear_halt()` return before resetting anything —
  and hid a missing piece of the reset path behind a test that passed.

| harness | result |
|---|---|
| `check_xhci_suite.py` | 35 of 35 (was 23 of 23, over one file) |
| `check_update_suite.py` | 27 of 27 |
| `check_esp_suite.py` | 28 of 28 |

`tools/check_xhci_suite.py` carries **22 new injections** — twelve for the
BAR and ten for the polling path. The polling ten are: the back-end
offering no non-blocking poll at all (**the v0.8.5 behaviour**); a
transfer queued on every poll rather than one at a time; the
outstanding-transfer flag never set; the next transfer not queued after
one completes; the drain consuming events belonging to other endpoints;
the drain deciding ownership by "does this endpoint have a ring" instead
of "is a transfer of mine outstanding"; the completion flag not cleared,
so one keypress repeats forever; a stall re-arming the halted endpoint
instead of reporting it; a reset leaving the endpoint marked busy, so the
device goes quiet after its first stall; and the asynchronous transfer
sharing the synchronous bounce buffer.

Two of those were genuinely missed on the first run and now have tests:
the ownership rule, and the reset-leaves-it-busy case — which the stall
test only *looked* like it covered, because the simulated device stalled
`CLEAR_FEATURE(ENDPOINT_HALT)` and so `usb_clear_halt()` was returning
early without ever resetting the endpoint. The device accepts it now.

The twelve BAR injections are: the high half ignored and the low half
returned (**the defect this release fixes**); `pci_bar_mem()` truncating
instead of refusing; the high dword read from `bar[n]` rather than
`bar[n+1]`; the low four flag bits left in the address; the 64-bit type
tested as 01b; a 64-bit BAR accepted in the last slot, where there is no
register left to hold its high half; a cacheable device window; a failed
mapping ignored and the truncated address used anyway; the window mapped
and then the physical address used; one page mapped instead of the
register file; a zero low half discarded; and the high half tested for
truth rather than the type bits.

## Known limitations

- **The above-4G mapping has still never run.** QEMU will not place a
  64 KB BAR above 4 GB however much the low hole is crowded, so that
  path is proved by the suites only: the BAR decode, the window's cache
  attributes and the in-page offset all have checks, and 45 injected bugs
  are caught. **The HP will be the first machine to execute it.** If it
  goes wrong, `dmesg` now names the address and what was read there.
- **What QEMU did prove** is everything downstream of it: the xHCI comes
  up, both HID devices enumerate on their own ports, the keyboard types
  at full speed, and `usb -vv` pages. That is the first time any of the
  USB work has run outside a unit test.
- **Still not on the real laptop.** The acceptance check is unchanged:
  boot it on the HP with a USB keyboard in and run `usb -vv`.
- **No hub support.** Devices on a root port are found; devices behind a
  hub are not.
- **Boot protocol only.** A HID device with `bInterfaceSubClass` 0 is not
  driven.
- **The trackpad is still almost certainly I2C-HID**, not USB.
- **`tests/test_fat16path.c` still has no fixture** and still skips. The
  rule it violates is written in the Makefile beside the fixtures that do
  get generated: *a fixture a suite needs is generated by the build, or
  the suite is decoration.*
- **This ISO still carries the reconstructed boot menu**, not the real
  `isodir/`. That reconstruction has been wrong twice, each time found
  only by booting.
- Everything carried from 0.8.5: no firmware boot entry, an installed
  system cannot install, partitions capped at 2047 MB, AHCI never on
  silicon, the idle panic unexplained.

## What to do with it

Apply it with `update` from the EFI System Partition, the same way 0.8.5
went on. Then:

```
usb -vv
dmesg
```

`usb -vv` pages now, so it can be read rather than photographed. If the
xHCI still does not come up, `dmesg` names the address it used and what it
read there — which is the one thing the last release could not tell you.

What to look for in `dmesg`, in order:

```
xHCI 00:14.0 BAR 0000006002120000 mapped at E0120000
xHCI 00:14.0 at E0120000, N ports, N slots, 32-byte contexts
USB xHCI hc0 port N: VVVV:PPPP class 03, address 1, 1 endpoint(s)
USB keyboard VVVV:PPPP on endpoint 81, 8-byte reports
```

The first line is the whole of part one. If it is absent, the BAR was not
above 4 GB after all and something else is going on; if it is present and
the second is not, the window was mapped and the controller still would
not start, and the line after it says what CAPLENGTH read as.

Then type at it. Not one character at a time — type a whole command
quickly. That is part two.
