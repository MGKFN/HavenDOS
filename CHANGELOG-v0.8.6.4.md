# HavenDOS v0.8.6.4 "Argo" (Beta)

**An iPhone enumerated on the HP, and this release is the instrument
that asks it the one question that matters.**

## What the laptop reported, 25 October

Kyan plugged an iPhone into the HP and ran `usb rescan`:

```
[371.006] ok   USB xHCI hc0 port 2: 30C9:00C9 class 0E, address 6
[371.134] ok   USB xHCI hc0 port 3: 05AC:12A8 class 06, address 4, 3 endpoint(s)
[371.254] ok   USB xHCI hc0 port 9: 0E8D:1EDE class E0, address 2
[371.255] warn USB xHCI hc0 port 12: no answer (-4)
```

`05AC:12A8` is Apple. **HavenDOS's xHCI driver, USB core and enumeration
talk to an iPhone on real hardware** — address assigned, descriptors
read, three endpoints found.

Two things about that line are worth more than the fact itself.

**It did not answer at boot.** The boot scan reported port 3 as
*"attached, but it did not answer"*. The phone answered on the rescan,
331 seconds later. A phone takes longer to come up than a boot scan
waits — or it was waiting to be unlocked and trusted. Either way it is
the second explained case of that message, and port 12 remains the
first and still unexplained.

**Class 06 is Still Image — PTP, and it is the phone's DEFAULT
configuration.** USB Ethernet tethering is not in it. It lives in a
different *configuration*, and this kernel had no way to look at one:

```c
r = usb_control(d, ..., USB_REQ_GET_DESCRIPTOR,
                (uint16_t)(USB_DESC_CONFIG << 8), 0, g_cfgbuf, 9);
```

wValue's low byte is the descriptor **index**, and it has been a
hardcoded zero since the USB core was written — correctly, because every
device this kernel had met offered exactly one configuration. `usb -vv`
can therefore only ever show the one in use, which for a phone is the
one it boots into.

## `usb cfg [addr]`

Every configuration a device offers, and every interface in each.

```
addr 4  05AC:12A8  4 configurations, in use 1
  configuration index 0:
      interface 0  class 06/01/01  3 endpoints  still image
  configuration index 1:
      ...
      interface 1  class FF/FD/01  2 endpoints  vendor   <-- APPLE USB ETHERNET
```

Apple's tethering interface is vendor-specific `255/253/1`. There is no
class code for it, so nothing else on screen would mark it out, and the
command calls it out by name when it sees it.

**It selects nothing.** No `SET_CONFIGURATION`, and the device table is
not modified — surveying a phone that is working must not stop it
working. `usb_walk_config()` takes `USB_IF_NONE`, which means *walk,
report, change nothing*, and the suite asserts the live device's
`config`, `ifnum`, `if_class`, endpoint count and first endpoint are all
untouched across a survey.

### One walker, not two

`usb_parse_config()` is now a one-line call into `usb_walk_config()`,
which is the same function it always was plus an optional observer. A
second descriptor parser would have been the obvious way to do this and
the wrong one: that function is pure precisely because it is the part
most likely to be handed something malformed by somebody else's
firmware, and two of them would be two things to get right.

## A real parser bound, found by the test

`wTotalLength` was read for a sanity check and then **ignored** — the
walk ran to the end of whatever buffer it was handed instead.

Those are not the same number. A controller that reports no residual
hands back the length that was *asked for*, not the length the device
sent, so the tail of the buffer is whatever was there before. The walk
then read a zero `bLength` past the last real descriptor and refused the
**entire configuration** as malformed.

`wTotalLength` bounds the walk now. A total larger than what was read is
an ordinary short read and stays bounded by the buffer, which the
existing `i + dlen > len` break has always handled.

This surfaced from a deliberately inconsistent descriptor blob while
writing the survey test. A device that overstates `wTotalLength` is a
device contradicting itself, which is exactly what this parser exists to
survive, so the suite keeps one — along with the short-read case, in
both directions.

## Testing

`tests/test_xhci.c`: **167 → 202 checks.** The simulated device honours
the descriptor index now and carries a second configuration with three
interfaces, one of them an alternate setting and one vendor-specific
`255/253/1`. Before this it ignored the index entirely, so no test could
have told a working index from a hardcoded one.

The checks that matter:

* the device is asked for **index 1** when index 1 is surveyed — on the
  wire, through the controller, not from a blob in hand
* the live device is **unchanged** across a survey, field by field
* **no `SET_CONFIGURATION`** is sent by a survey
* an index the device does not have is refused, not faked
* a configuration with no interfaces is *empty*, not broken — a survey
  accepts it where `usb_parse_config()` refuses it, because "it offers a
  configuration with nothing in it" is an answer

`tools/check_xhci_suite.py`: **all 58 injections caught.**

**36 suites, 2513 checks.**

## What to run on the HP

With the phone plugged in and Personal Hotspot **on**:

```
usb rescan
usb cfg
```

The number of configurations and whether `FF/FD/01` appears in any of
them decides whether 0.8.7 "Iris" is a tethering release or not. If it
is there, the path is `SET_CONFIGURATION` to that index and a driver for
two bulk endpoints. If the phone offers one configuration and it is PTP,
that is the answer and the adapter is the way.
