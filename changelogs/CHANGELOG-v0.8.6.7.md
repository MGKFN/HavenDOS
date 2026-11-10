# HavenDOS v0.8.6.7 "Argo" (Beta)

**A SuperSpeed device could never enumerate. Four releases, one byte.**

## The symptom

`hc0 port 12` on the HP has reported

```
attached, but it did not answer.
```

on **every boot since v0.8.5.3**, with nothing plugged into it that
anyone could identify. And a USB 3 stick that worked perfectly on
25 October stopped being found at all after being moved to a different
socket — `usb rescan` showing `3 devices, 0 USB disks`, no port changing
state, and the stick physically in the machine.

Those are the same bug.

## Finding it: the port status, in words

`usb -vv` printed `PORTSC 0x00201203` and, for a port that did not come
up, *"attached, but it did not answer."* An unreadable number and a
shrug — and they were the entire evidence available.

The bits are not secret. PORTSC is now decoded on screen:

```
hc0 port 12  PORTSC 0x00201203
        connected, enabled, powered, link U0 active, SuperSpeed
        attached, but it did not answer.
        the link is up and the device did not reply to enumeration.
```

That is a diagnosis rather than a shrug. A port that is connected but
**NOT ENABLED** never finished training. One that is **NOT POWERED**
cannot answer at all. One that is enabled, powered and in U0 — a link
fully up — and still silent is a **protocol fault**, and that is a
different search entirely.

The line under it says which of the three it is.

## The bug: bMaxPacketSize0 is not always a byte count

At low, full and high speed, byte 7 of the device descriptor is endpoint
0's packet size in bytes: 8, 16, 32 or 64.

**At SuperSpeed it is an EXPONENT.** The USB 3 specification fixes it at
9, meaning 2⁹ = **512 bytes**, and a SuperSpeed device may not say
anything else.

`usb/xhci_hc.c` copied the byte straight through:

```c
if (desc[7] && desc[7] != out->maxpacket0) {
    out->maxpacket0 = desc[7];
    ...
    ctx_set32(iec, 1, ... | ((uint32_t)desc[7] << 16));
```

So the control endpoint, which the controller had correctly been told
was 512 bytes, was **reprogrammed to nine bytes**. The next descriptor
request is 18 bytes. Eighteen bytes do not fit through a nine-byte
control endpoint, the transfer never completes, enumeration ends in
`USB_EXFER` — and this kernel prints that as *"attached, but it did not
answer."*

### And a second half that caused the first

```c
out->maxpacket0 = (uint8_t)default_mps(speed);
```

`maxpacket0` was a `uint8_t`. `default_mps(USB_SPEED_SUPER)` returns
**512**, which truncates to **zero**. So every SuperSpeed device started
with a recorded endpoint-0 size of 0 — and that is what made the
correction path fire in the first place: it compared the descriptor's 9
against 0, found a difference, and "corrected" a pipe that was already
right.

Two bugs that only produce a failure together, which is why high speed
never showed it.

## The fix

* `maxpacket0` is a **`uint16_t`**, because 512 does not fit in eight
  bits.
* `mps0_from_desc(speed, b)` reads the value as an **exponent** at
  SuperSpeed and a byte count below it, and returns 0 for a value this
  layer cannot make sense of — the descriptor is somebody else's
  firmware, and the caller keeps what it had rather than programming
  nonsense.

## Why the stick worked before and not now

At high speed, endpoint 0 is 64 bytes and the descriptor says 64 — the
byte count and the exponent reading never diverge, because the exponent
reading is not applied. The same stick in a socket that trains at
SuperSpeed hits the path above every time.

So it was never the stick, never the FAT32 partition, and never the
`\EFI\HAVENDOS\update\` layout. It was the socket.

## Testing

`tests/test_xhci.c`: **202 → 212 checks.** The simulator has a
SuperSpeed device now — port speed id 4, a descriptor whose
`bMaxPacketSize0` is **9**, and an endpoint-0 guard that expects the
host to have programmed **512**. A host that copies the byte through is
stalled exactly as a real controller stalls it.

With the fix removed, three checks fail:

```
FAIL ...and its descriptor was read in full
FAIL ...so the 18-byte request completed, which a nine-byte endpoint cannot carry
FAIL the controller never saw a wrong ep0 packet size
```

`tools/check_xhci_suite.py` gains three permanent injections — the
exponent, the truncation, and an exponent of nonsense being programmed
anyway — so this cannot come back quietly.

**36 suites, 2537 checks.**

## What to do with it

Put the stick back in the socket that did not work and run `usb rescan`.
If the socket trains at SuperSpeed, `usb -vv` will now say so **and the
device will enumerate**. And port 12 should finally say what it is.
