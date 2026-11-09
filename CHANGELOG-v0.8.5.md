# HavenDOS v0.8.5 Beta "Atlas" — xHCI, the USB core, and HID

The laptop can be typed at.

`lspci -vv` on that machine reports `00:14.0 8086:51ED 0C/03/30` and
nothing else carrying USB. No UHCI, no PS/2 port of either kind. Until
this release, HavenDOS could see that controller and do nothing with it:
`usb -vv` said *"There IS a USB controller, programming interface 0x30
(xHCI), which this kernel cannot drive yet."*

It drives it now.

## The core came first, and it was built with two back-ends

0.8.3.1's post-mortem recorded that three of its six bugs were one
mistake: *the installer has one block layer and the filesystem has
another, and only one of them got taught.* Four screens each worked a bus
label out independently and only one knew about NVMe, so the disk picker
said `NVMe` and the screen gated behind typing YES said `VirtIO`.

`drivers/uhci.c` was shaped the same way — the device struct is
`uhci_dev_t`, the transfers are `uhci_control()` and `uhci_bulk()`, and
the enumeration walk lives inside the driver. Writing `xhci.c` beside it
would have produced two device models, two enumeration walks and two
places that decide which controller a device is on, in the layer that
input, removable storage and the only available network adapter all sit
on.

So `usb/core.c` exists, and `usb_hc_ops_t` has **two implementations from
its first commit**. An abstraction with one implementation is a wrapper
and proves nothing.

**Where the line falls was not where it first looked.** The obvious split
— the core enumerates, the driver moves bytes — does not survive contact
with xHCI, because the two controllers disagree about who owns a device's
address:

- **UHCI** is a dumb pipe. The driver sends `SET_ADDRESS` as an ordinary
  control transfer and puts the new address in every transfer descriptor
  itself.
- **xHCI** owns the device. The driver asks for a slot, hands over an
  input context, and the *controller* issues `SET_ADDRESS` and remembers
  the result. Endpoints beyond the default pipe do not exist until a
  Configure Endpoint command has been accepted.

So the back-end owns everything up to *"there is an addressed device on
this port whose default pipe works"*, plus the endpoint configuration
step, and the core owns everything after. `usb_hc_ops_t::configure()` is
a no-op on UHCI and real work on xHCI, and that asymmetry is the reason
`include/usb.h` exists.

## UHCI was moved without being edited

`drivers/uhci.c` is byte-for-byte what it was. The move is an adapter,
`usb/uhci_hc.c`, so `tests/test_uhci.c` still compiles the same file and
still tests the same thing. **110/110, unchanged** — which is the proof
the move was faithful, and it cost nothing to get.

One thing is written down rather than discovered later: **UHCI has no
interrupt transfers.** `include/uhci.h` says so in its own header; its
frame list points all 1024 entries at the control queue. A UHCI transfer
descriptor does not distinguish interrupt from bulk, so the adapter runs
interrupt transfers through the bulk queue and it works — what is lost is
the *scheduling*. It does not matter on the machine this release is for:
that one has no UHCI at all.

## xHCI

Capability and operational registers, reset waiting on **CNR**,
scratchpad buffers, the device context base array, the command ring, the
event ring and its segment table, port power and reset, Enable Slot,
Address Device, Evaluate Context, Configure Endpoint, Reset Endpoint,
control transfers as Setup/Data/Status TRBs, bulk and interrupt as Normal
TRBs, and a cycle bit on every one of the three rings.

Five traps are named in the file rather than learned twice: the 32-vs-64
byte **context size** from `HCCPARAMS1.CSZ`; the **cycle bit** and the
Link TRB's Toggle Cycle; the **scratchpad buffers** a controller can
demand through DCBAA entry 0; **CNR**, which makes every register write
during it a dropped write; and **DCI** — endpoint 1 IN is doorbell target
3, not 1.

Polled, like every other driver here.

## HID

Boot-protocol keyboard and mouse. `SET_PROTOCOL(0)`, then the interrupt
IN endpoint is polled from the two places that already read the keyboard,
so USB input is drained whenever anything asks for input.

**The input goes into the same two queues PS/2 feeds.** A key from a USB
keyboard and a key from a PS/2 keyboard are indistinguishable by the time
anything reads them, including the extended codes for the arrows. The
mouse report is turned back into a PS/2 packet and pushed through
`mouse_feed()` — so the acceleration, the clamping and the button-edge
detection in `drivers/mouse.c` are not written a second time. A second
copy of that arithmetic is a second place for the v0.7.17 precedence bug
to live, the one where every leftward movement got a 2x boost and the
pointer drifted.

## The two things the HP turned up

- **`usb -vv` told a physical laptop to change a UTM setting.** It now
  reports what it found; and since xHCI *is* driven, reaching the
  "cannot drive" branch with an xHCI present means that controller was
  found and refused to start — a different thing, said differently.
- **`lspci -vv` scrolled off the top with no way to read it.** There is
  now a shared pager in `shell/ui.c`, and it takes its page height from
  `vga_rows()` rather than a constant. The pager in `shell/help.c` is
  hardcoded to 20 lines, which is already wrong in the 90x60 modes and
  will be wrong on every grid 0.8.8 adds.

Also fixed: **`shell.c` called `nvme_writable()` without including
`nvme.h`** — an implicit declaration that happened to work and would not
have survived a change to that function's return type.

## Testing — the harness found ten holes

**1835 checks across 29 suites**, up from 1768 across 28.

`tests/test_xhci.c` is new: **67 checks** against a simulated controller
that *executes its commands*. It reads the command ring, follows Link
TRBs, matches cycle bits, copies input contexts into device contexts at
the size it advertised, runs control transfers against a device with real
descriptors, and posts events. A driver that gets the cycle bit wrong
stops dead here exactly as it would on metal.

**And it was not good enough.** `tools/check_xhci_suite.py` injected 24
real xHCI bugs and the suite **missed ten of them on the first run**:

> the dequeue pointer written before the segment table; the command ring
> never toggling its cycle; the event ring never toggling its cycle; ERDP
> written without the handler-busy bit; Address Device with no slot
> context; the endpoint-0 packet size never corrected; Configure Endpoint
> not raising Context Entries; a failed command reported as success; and
> PORTSC written back with every bit it was read with — which disables
> the port, because PED is write-1-to-clear.

Almost every miss was the **simulator being kinder than hardware**, not a
missing assertion. So the simulator was made to refuse the things a real
controller refuses — Address Device without A0, Configure Endpoint with
too few Context Entries, a port write that turns PED off, a host talking
to an 8-byte endpoint in 64-byte packets — and the ring-wrap loops were
lengthened past the event ring's 64 entries as well as the transfer
ring's 15. **23 of 23 caught.** The 24th is recorded in the harness as
genuinely redundant rather than faked as a miss: the Toggle Cycle bit
`ring_init()` lays down is rewritten by the first push, so it has never
been load-bearing.

| harness | result |
|---|---|
| `check_xhci_suite.py` | 23 of 23 |
| `check_update_suite.py` | 27 of 27 |
| `check_esp_suite.py` | 28 of 28 |

## Known limitations

- **None of this has run on hardware.** It is proved against a simulated
  controller and the suites. The `isodir/` caution from 0.8.4 still
  stands and this ISO was built with the same reconstructed boot menu.
- **No hub support.** Devices on a root port are found; devices behind a
  hub are not. A laptop's internal keyboard and trackpad are usually on
  root ports, so this is expected to be enough for the HP — but it is an
  assumption until somebody boots it.
- **Boot protocol only.** A HID device with `bInterfaceSubClass` 0 — legal,
  and some keyboards are — is not driven. The report-descriptor parser
  that would fix it is real work and is not here.
- **The trackpad is still almost certainly I2C-HID**, not USB. `lspci` on
  the HP shows two Intel Serial-IO I2C controllers (`8086:51E8`,
  `8086:51E9`). If so, 0.9.4 "Midas" is needed and this release gives you
  an external keyboard and mouse, not the built-in pointer.
- **UHCI interrupt endpoints run through the bulk queue**, as above.
- **`usb -vv` is not paged**, only `lspci -vv` is.
- Everything carried from 0.8.4: no firmware boot entry, an installed
  system cannot install, partitions capped at 2047 MB, AHCI never on
  silicon, the idle panic unexplained.

## What to do with it

Boot it on the HP with a **USB keyboard plugged in** and run `usb -vv`.
That is the acceptance check: a device listed, its interface class, its
endpoints, and a key reaching the prompt from it. `dmesg` carries what
the xHCI reported about itself — ports, slots and context size.

If the internal keyboard appears in that listing, the trackpad question
is answered too and 0.9.4 is cancelled.
