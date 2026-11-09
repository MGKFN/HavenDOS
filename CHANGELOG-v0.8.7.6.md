# HavenDOS v0.8.7.6

**Channel:** Beta.  **Codename:** Iris.  **Date:** 26 October 2026.

0.8.7.5 fixed the event ring, and on the HP every consequence of that
landed at once: `usb cfg` described every configuration of every device,
and `usb mux` found the Apple Mobile Device interface it had never once
been able to see.

**And the phone answered.** Twenty bytes. The first thing it has sent
back over any channel, in any release.

This release is about what those twenty bytes said.

---

## The framing was wrong, and the driver said so

```
usbmux: 05AC:12A8 cfg 2 if 1 is Apple Mobile Device
ok usbmux: open, ep 85 in / 04 out, 512/512 bytes
Sending VERSION...
warn usbmux: magic 2, expected FEEDFACE -- the framing in this driver is wrong
warn usbmux: VERSION reply unreadable (-3)
     last packet: 20 bytes
     00 00 00 00 00 00 00 14 00 00 00 02 00 00 00 00
```

The warning was correct. Here is what arrived:

| bytes | value | meaning |
|---|---|---|
| 0–3 | `00000000` | protocol 0 — VERSION |
| 4–7 | `00000014` | length **20** |
| 8–11 | `00000002` | **major 2** |
| 12–15 | `00000000` | minor 0 |
| 16–19 | `00000000` | padding |

**Eight bytes of header, twelve of payload.** 0.8.7.3 used sixteen
everywhere, read byte 8 as the magic, found a 2 there, and said so.
The `2` it complained about was the phone saying it speaks usbmux 2.0.

The magic, `tx_seq` and `rx_seq` are a version-2 addition. They cannot
be on the packet that *establishes* the version — neither end knows it
yet. So the header length is a function of the connection's state:
**eight until both ends have agreed on version 2, sixteen after.**

A second bug was hiding behind the first. Even with the magic check out
of the way, the version was read at offset 16 — the padding — so a phone
answering "2" would have been recorded as version 0. Both die with
`mux_hdr_len()`.

`usbmux_handshake()` is now idempotent: versioning happens once, and a
repeat would go out in the framing the first exchange agreed, which no
device expects.

## The two interfaces were never in competition

`usb cfg` printed the phone's configuration 3:

```
interface 1  class FF/FE/02   Apple Mobile Device
interface 2  class FF/FD/01   APPLE USB ETHERNET
```

**They are in the same configuration.** `usbmux_open()` took the first
configuration carrying a mux interface — index 2, which has no Ethernet
in it — so claiming the channel switched away the configuration
tethering needs, every time `usb mux` ran. It now prefers a
configuration that carries both, and says so:

```
usbmux: 05AC:12A8 cfg 3 if 1 is Apple Mobile Device, sharing it with Ethernet
```

The handoff recorded this as *"the USB core claims one interface per
device, so usbmux and Ethernet cannot be held at once."* The core's
limit is real. The configurations were never the thing forcing the
choice, and nobody had looked.

## A receive path, because stage 1 needs one

`usbmux_recv_packet()` and `usbmux_payload()` read one packet in
whatever framing the connection settled on. lockdownd is a conversation
rather than a handshake, so stage 1 is built on this — and it is also
the only way to exercise the magic check, which by definition cannot
fire during the version exchange.

---

## Tests

`tests/test_usbmux.c` — **87**, from 79.

**The suite passed 79 of 79 against framing the phone has never used.**
The device model and the driver shared one author's misunderstanding, so
every check agreed with every other check and none of them agreed with
the phone. That is the fourth time a green suite has been measuring a
wrong model, and the first time the model was wrong about something
outside this tree entirely.

The fixture now answers the way an iPhone does, and one test is built
from **the literal twenty bytes off the HP's screen** — not a fixture
anybody reasoned their way to. Those bytes cannot be wrong.

New: the configuration-preference test, which needed a device shaped
like the real one (PTP, then mux-only, then mux **and** Ethernet) —
the old fixture had no mux-only configuration, so the wrong choice was
not expressible.

`tools/check_usbmux_suite.py` — **24 injections, all caught**, from 22.

It earned its keep three times in one run:

- **Seven injections had gone stale** against the rewritten source. The
  harness reports that rather than counting them as passes, which is the
  only reason they were not silently lost.
- **One "catch" was only a compile failure**, not a test noticing. Fixed
  so the injected driver builds and a test refuses it.
- **A reordering in `survey_cb()` masked an existing injection.** Putting
  the Ethernet test first meant a class-only match on the mux interface
  could no longer reach the wrong interface. The guard still read as
  present and could no longer fire. Reordered back: *an ordering that
  makes a bug untestable is not a fix for it.*

One injection was **removed and the reasoning written down**: tx_seq and
rx_seq cannot be told apart today, because the only packet carrying them
is the SETUP that follows a handshake, where both are 1. It returns at
stage 1, when the counts diverge.

---

## What this does not fix

**The phone still will not tether.** It needs a pairing record and this
release does not create one. What it does is get the transport right, so
stage 1 — lockdownd `QueryType`, then `GetValue` for the device public
key — can start. Neither needs any crypto.

Also worth recording against future confusion: **this phone reports
carrier ON while untrusted.** 0.8.7.5 added a carrier check before DHCP
precisely to avoid a pointless wait, and on the HP it answered "up" and
DHCP ran anyway. The check is honest; it is simply not a tethering
signal on this device.
