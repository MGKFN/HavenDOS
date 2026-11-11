# HavenDOS v0.9.2.1 Beta "Janus": two things the HP found

*11 November 2026. 64 test suites, 6668/6668 checks.*

## What the HP said on 0.9.2

Kyan ran `gfx`, `gfx plan`, `gfx on` and `gfx off`. **The panel never
changed.** The monitor showed nothing, which is correct for where the
sequence stopped.

**The good news, from real silicon:**
- The 0.9.1 PLL fix holds. `gfx` reads DPLL1 at **270 MHz** and the panel
  at **60.00 Hz**.
- **25 writes landed and read back exactly** before the stop:
  - the DDI B IO well came on;
  - DBUF slices 3 and 4 came on;
  - **DPLL0 powered up, took 001001D0 / 00000E84, and LOCKED at
    148.5 MHz** (`8C000000` read back, and the lock wait passed);
  - DDI B's clock was selected and ungated;
  - the transcoder clock, PIPESRC, PIPE_MISC, every timing register,
    TRANSCONF, the line time and PIPE_CHICKEN all took their values.
- Afterwards, `gfx` showed DPLL0 running at 148.5 MHz from those
  dividers. The writer's PLL arithmetic and the reader's agree on the
  silicon.
- `gfx off` undid it cleanly, with 4 writes, all read back.

**The stop:** step 28 of 73, `MBUS credits (as A)`:

```
register 07103C: wanted A9038C04, read B1038C04
```

## Fix 1: copy the fields, not the word

PIPE_MBUS_DBOX_CTL's credit fields occupy bits 24:0. Bits 31:25 are not
in i915's register header. On the HP they are **read-only and differ
per pipe**: pipe A reads `A9…`, pipe B reads `B1…`, and bits 28:27 are
`01` on A and `10` on B, which looks like the pipe's own number. 0.9.2
copied pipe A's whole word and held all 32 bits, so the read-back
mismatch stopped the sequence. That was correct behaviour from the
safety net, but on a register that was fine.

`gfx on` now copies only the **fields** of A's value (`01038C04`: B2B
16/1, BW credit 2, B credit 12, A credit 4) and checks only those. The
same treatment applies to the watermarks: their fields are copied and
held, and anything else in the word is left as pipe B has it.

The test model now has pipe B's read-only bits, from the HP's own
read-back, along with pipe A's real DBOX and watermark values from the
0.9.2 photos. It ends with DBOX B = `B1038C04`, exactly what the HP
read. Putting the whole-word copy back makes the harness catch it as
"0.9.2's stop".

**A stopped `gfx on` can leave DPLL0 running.** The next `gfx on` then
finds no free PLL. That refusal now says that `gfx off` releases it.

## Fix 2: USB keyboards and mice on every interface

The external keyboard and the mouse's wireless receiver did nothing.

`usb/hid.c` had only ever looked at the interface the USB core chose,
which is a device's **first**. A wired mouse has one interface and never
noticed. A wireless receiver has several: usually a boot keyboard on
interface 0, a boot mouse on interface 1 and a vendor interface after
them. So the mouse half of a mouse's own receiver was never looked at.
A gaming keyboard often puts a second report on interface 1, too.

`usb_hid_init()` now:
- surveys each HID device's configuration, which selects nothing;
- drives **every** boot keyboard and boot mouse interface it finds;
- claims the ones the core didn't select **beside** the others, with the
  same `usb_claim_interface()` that lets usbmux and Apple Ethernet share
  an iPhone;
- sends SET_PROTOCOL and SET_IDLE to that interface's own number.

A rescan leaves an interface that is already held alone, so a keyboard
someone is typing on is never reconfigured. The log now names the
interface: `USB mouse 046d:c52b interface 1 on endpoint 82`. Non-boot
interfaces are named as such and left alone; that needs the report
parser 0.8.8.11 wrote for I2C-HID, and comes later.

**This fixes a receiver whose mouse isn't on its first interface. It
does not explain a plain wired keyboard that does nothing.** If the
keyboard still does nothing on 0.9.2.1, the `usb -vv` and `dmesg`
photos in the test doc will say why.

## Tests

- **`test_usbhid` (30, new).** It links the real `usb/core.c` and
  `usb/hid.c` against a simulated xHCI with:
  - a three-interface receiver;
  - a vendor-first receiver;
  - a wired keyboard;
  - an NKRO keyboard;
  - a device refusing boot protocol on one interface;
  - a refused claim;
  - nine boot interfaces in one device.

  **Run against 0.9.2's driver, it fails 14 of its checks.**
  `tools/check_usbhid_suite.py` catches 7/7. Its first run missed one:
  requests sent to the core's `ifnum`, which only a rescan exposes, and
  the suite now checks the rescan's requests.
- **`test_xemode`** is 195 → 199. `check_xemode_suite.py` catches 28/28
  (new: the whole-word DBOX copy).
- **New QEMU harness `harness/hid_boot.py`:** a usb-kbd and a usb-mouse
  on qemu-xhci, both bound on interface 0, shell answering. 3/3.
- `harness/gfx_boot.py`: 13/13.
- `check_pointer_suite.py`: 11/11.

## What it needs from the HP

`docs/GFX-ON-HARDWARE-TEST.md`, updated. Run `gfx on` again, and add
`usb -vv` and `dmesg` photos with the keyboard and receiver plugged in.
