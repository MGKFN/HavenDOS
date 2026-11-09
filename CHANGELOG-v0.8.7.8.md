# HavenDOS v0.8.7.8 — the magic is 0xFACEFACE

Channel Beta, codename "Iris". 30 October 2026.

**One value. v0.8.7.7 refused the phone's SYN/ACK because its magic was
not the one this host writes, and usbmuxd never reads that field at
all.**

Everything else in stage 1 worked on the first boot.

---

## What the HP printed

```
usbmux: SYN -> port 62078, from 18516
warn usbmux: magic FACEFACE, expected FEEDFACE -- the framing in this driver
warn usbmux: the SYN/ACK was unreadable (-3)
warn lockdown: the connection to port 62078 failed
```

and `usb mux` afterwards, showing the packet that was thrown away:

```
usbmux  05AC:12A8  if 1  ep 85/04
        version 2  tx 3  rx 2  errs 0
        mux seq: tx 2  rx FFFF
        last packet: 36 bytes
        00 00 00 06 00 00 00 24 FA CE FA CE 00 00 00 01
```

Read it field by field:

| bytes | value | |
|---|---|---|
| `00 00 00 06` | protocol 6 | **MUX_PROTO_TCP** |
| `00 00 00 24` | length 36 | 16-byte mux header + 20-byte TCP header |
| `FA CE FA CE` | 0xFACEFACE | **the magic — and not 0xFEEDFACE** |
| `00 00 00 01` | tx 0, rx 1 | its sequence pair; rx 1 mirrors our SYN's tx_seq |

**That is the SYN/ACK.** A 36-byte TCP packet, correctly framed,
correctly sized, answering a connection request to port 62078. The
phone parsed our mux header, parsed our TCP header, accepted the port
and the sequence number, and replied.

And the driver threw it away over one field, reporting *"the framing in
this driver is wrong"* — which was true in a way it did not mean.

---

## usbmuxd never reads the magic

`src/device.c`, every occurrence in the file:

```
 80:    uint32_t magic;                 the struct field
222:    mhdr->magic = htonl(0xfeedface); send_packet(), on the way OUT
```

That is all of them. `device_data_input()` checks that the length field
equals the transfer length, takes `rx_seq`, and switches on the
protocol. **There is no magic validation anywhere in the only
implementation known to work against this device.**

So the host writes `0xFEEDFACE`, the device writes `0xFACEFACE`, and
nobody notices because nobody looks.

### The magic is evidence now, not a gate

It is still read, and still reported when it is neither known value,
because a number nobody expected is worth seeing. It is no longer
believed. The **length** is the gate, and it is the one usbmuxd
actually checks.

`MUX_MAGIC_DEVICE` is a named constant rather than a deleted check,
because "the magic we send and the magic we get back are different
values" is a fact about the protocol worth stating once where the next
person will find it.

---

## How it got in, which matters more than the fix

The check was not written for stage 1. It came from v0.8.7.3, where it
guarded the VERSION exchange — and in v0.8.7.6 it was correctly scoped
to the sixteen-byte header, because there is no magic in a VERSION
packet. All of that was right.

**It had never run against a real packet.** Stage 0 only ever received
a VERSION reply, which uses the eight-byte header and skips the check
entirely. Stage 1 is the first time a sixteen-byte-header packet ever
arrived from a device, and the check failed on the first one.

v0.8.7.7's own changelog says *"where this driver deliberately differs
from usbmuxd, the difference is marked and argued"* and lists two. This
was a third, undeclared, and arrived by **carrying a guard forward**
rather than by adding one. A departure you inherit is still a
departure.

### And the harness could not have caught it

`tools/check_usbmux_suite.py` had an injection called "a wrong magic is
parsed around". It was **caught**, every run, for three releases.

The guard it protected was wrong.

That is the lesson, and it is about the harness rather than the driver:
**an injection being caught says the suite would notice if the
behaviour changed. It says nothing about whether the behaviour is
right.** A harness can only ever ask *would we notice*, never *should
it be this way*. The fixture agreed because the fixture and the driver
had both taken the value from the same place — `include/usbmux.h` — and
the suite's own header blurb listed "the magic really is 0xFEEDFACE"
among the things it pinned down.

`tests/muxdev.inc` sends the real bytes now. *"Where a protocol comes
from outside this tree, get bytes off the wire into the fixture"* —
the rule was already in HANDOFF.md, and it was applied to the VERSION
packet in 0.8.7.6 and not to anything else.

---

## A spurious warning is a defect

The magic check now reports instead of refusing, which creates a defect
the old code could not have: a check that fires on every healthy packet
is a check that buries the one line that matters.

`dmesg | grep usbmux` is how every hardware question in this release
line has been answered, so both suites now **count warnings separately
from log lines** and assert that a complete stage-1 exchange — open,
handshake, connect, send, receive — emits **not one warning**.

That is also the only thing that can still catch the magic check being
run during the VERSION exchange. Bytes 8..11 of a VERSION packet are
the major version, so the warning would read `magic 00000002` on every
boot there has ever been. v0.8.7.3 printed exactly that, and it took
three releases to place.

---

## What was already right on that boot

Worth listing, because this release changes one constant and nothing
else, and the rest of stage 1 is confirmed on hardware:

- **`mux seq: tx 2 rx FFFF`** — the three sequence rules 0.8.7.7 fixed.
  The SETUP reset the pair and sent `tx 0`, the SYN carried `tx 1`,
  leaving 2. 0.8.7.6 would have shown `tx 1 rx 1`.
- **`SYN -> port 62078, from 18516`** — 18516 is 0x4854, big-endian,
  correct.
- **The device answered at all**, which means our twenty-byte TCP
  header, its offset nibble, its flags and its window were all
  acceptable to iOS.
- **The device's `rx_seq` is 1**, mirroring the `tx_seq` of our SYN —
  confirming the mirroring semantics 0.8.7.7 corrected.
- **`errs 0`.** The event ring, the buffer sizes, the configuration
  choice: all holding.

---

## Testing

**3228 / 3228 across 40 suites** (3213 at 0.8.7.7). `test_usbmux`
216 → 231.

| harness | |
|---|---|
| `check_usbmux_suite.py` | **66 of 66**, harmless correctly missed |
| `check_lockdown_suite.py` | **32 of 32**, harmless correctly missed |
| `harness/lockdown_boot.py` | **12 of 12** on a real ISO boot |

The magic injection is replaced by its inverse — *"an unexpected magic
is refused instead of reported"*, which is v0.8.7.7's bug — plus *"the
device's own magic is treated as unexpected"*, which is the
every-packet warning.

`harness/lockdown_boot.py` compared the screen against a **version
literal** and broke on the bump. It reads `include/version.h` now:
`make_version.py --check` refuses version literals in C sources for
exactly this reason, and a harness is not exempt from the rule it
exists to protect.

---

## Files

Changed: `usb/usbmux.c`, `include/usbmux.h`, `tests/muxdev.inc`,
`tests/test_usbmux.c`, `tests/test_lockdown.c`,
`tools/check_usbmux_suite.py`, `harness/lockdown_boot.py`, `Makefile`,
`include/version.h`.

## Next

**`usb lockdown` again.** The SYN/ACK is accepted now, so the next
thing that happens is the completing ACK, the QueryType plist, and
lockdownd's reply — or the first of the five outcomes in
`docs/LOCKDOWN-HARDWARE-TEST.md` that is not this one.
