# HavenDOS v0.8.7.3

**Channel:** Beta.  **Codename:** Iris.  **Date:** 26 October 2026.

`usb mux` — the first packet this kernel has ever sent to the half of an
iPhone that decides whether tethering is allowed at all.

---

## Why this exists

26 October settled what three releases had been circling. With the phone
attached, **every `usb rescan` re-prompted for trust** and **the Personal
Hotspot indicator never appeared in the Dynamic Island**.

Both say the same thing: the pairing is never completed and never
persisted, so iOS never activates the tethering function. It is not
sending frames. There was never anything wrong with the receive path —
there was nothing on the link.

That answers the handoff's standing question. The Ethernet interface is
**offered** without a trust handshake. It is **not usable** without one.
Descriptor-level cooperation is not consent: the phone accepted
`SET_CONFIGURATION`, accepted `SET_INTERFACE` to the alternate setting
with the bulk endpoints, and returned a real MAC address, and none of
those turn the interface on.

## What this release is

The transport for the channel pairing runs over — interface 1, class
`FF/FE/02`, which `usb cfg` found on 25 October. **Stage 0 only**, and
deliberately so.

The road to a paired phone, staged so every step is a yes or no the
device itself gives:

| stage | proves | crypto |
|---|---|---|
| **0** | **the channel carries packets at all** | **none** — this release |
| 1 | lockdownd answers `QueryType` | none |
| 2 | `GetValue` returns the device's public key | none |
| 3 | `Pair` is accepted | RSA, X.509 |
| 4 | the hotspot indicator appears | — |

Stages 0 to 2 need no cryptography whatsoever. That matters, because the
objection to attempting this at all was that nothing could be tested
until nearly all of it existed. Built bottom-up it can be, and each
stage is one boot.

Two things also shrink it from the original estimate: **lockdownd
accepts XML plists**, so no binary plist writer is needed, and **the
`Pair` request goes unencrypted** — the client-certificate TLS session
is for other lockdown services, not for pairing itself.

## `usb mux`

Claims the interface and sends a VERSION packet:

```
usbmux  05ac:12a8  if 2  ep 85/06
        version 2  tx 2  rx 1  errs 0
        last packet: 28 bytes
        00 00 00 00 00 00 00 1C FE ED FA CE 00 00 00 00
```

The last line is the packet **as it arrived, before anything in this
driver decided what it meant** — the same discipline `usb eth` has, for
the same reason. The framing here is read off the published behaviour of
usbmuxd and nobody in this tree has seen a byte of it on a wire.

If the device answers, the channel works and the road is long but known.
If it does not, that line is what says so, and the money goes on an
adapter instead.

## The protocol, as implemented

Sixteen big-endian header bytes and a payload:

```
uint32 protocol    0 version, 1 control, 2 setup, 6 TCP
uint32 length      the whole packet, HEADER INCLUDED
uint32 magic       0xFEEDFACE
uint16 tx_seq
uint16 rx_seq
```

`length` counting the header is the field most easily got wrong and the
one a device refuses in silence, so the suite checks it byte for byte.

A wrong magic is **reported, not parsed around**: a driver that shrugs at
one carries on into a protocol it has already been told it misunderstands.

---

## Testing

`tests/test_usbmux.c` — **79 checks**, against a device that speaks the
protocol and can refuse: silent, wrong magic, wrong protocol, short
reply, a length under the header, a stalled endpoint, one bulk endpoint,
and no such interface at all.

`tools/check_usbmux_suite.py` — **21 of 21 caught**, harmless correctly
missed. The first run found **ten holes**, in the shapes this tree keeps
meeting:

- **The fixture could not distinguish anything.** `tx_seq` and `rx_seq`
  were both 1 where they were compared, so swapping them was invisible.
  The configuration had one `0xFF` interface, so "matches the class" and
  "matches the whole triple" were the same test — **the real phone has
  two**, and a class-only match claims the Ethernet interface instead.
  Opening twice compared a configuration *value* that does not move.
- **The packet that fails to parse is the one worth keeping.** The
  "raw bytes recorded first" check only looked at a *successful*
  handshake, where both orderings record it.

Two injections are recorded as not defects rather than deleted: a buffer
bound nothing can yet reach (no payload here exceeds 13 bytes — it
becomes reachable with lockdownd), and a short-reply guard masked by the
magic and length checks downstream.

**40 suites, 2,861 checks.** 16 build checks — `check_usb_buffers.py`
covers this driver's buffer too.

## Not done

- **Everything above, on hardware.** One boot answers it.
- Stages 1 to 4. `usb mux` sends VERSION and stops there on purpose.
- The core still claims **one interface per device**, so usbmux and the
  Ethernet interface cannot be held at the same time. Pairing does not
  need both at once; tethering eventually will.
