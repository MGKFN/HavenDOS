# HavenDOS v0.8.9.9 Beta "Helios": the phone's two halves

*11 November 2026. 60 test suites, 6130/6130 checks.*

## What the HP showed

Kyan ran 0.8.9.8 on the HP with the iPhone attached and took five photos:

```
pair            ->  paired, saved to .pairrec
                    StartSession -> session 1879F0F2-...
                    TLS handshake ... the handshake failed (-1)
                    reached: ClientHello sent
ifconfig dhcp   ->  DHCP failed
lockdown        ->  The device REFUSED the port.
ping 192.168.2.20 -> timeout x4
                    DHCP so far: 0 asked, 0 offered, 0 acknowledged.
tlsinfo         ->  Reached stage   ClientHello sent (1/12)
                    Server chose    (no ServerHello seen)
                    Last alert      none
```

The phone still showed the Personal Hotspot indicator, so the pairing
itself held. The photos show three separate faults, all on our side.

## 1. "0 asked": pairing took the Ethernet interface away

The iPhone is one USB device with two drivers on it. In configuration
index 3, interface 1 is usbmux (pairing) and interface 2 is Apple
Ethernet (tethering). The USB core could hold only **one interface per
device**:

- at boot, `usb/ethernet.c` selected configuration 3 and put interface 2
  on alt 1;
- `pair` then opened usbmux, which selected configuration 3 **again**,
  for interface 1.

`SET_CONFIGURATION` resets every interface on the device. Interface 2
went back to alt 0, which has no endpoints at all. The core's table was
rebuilt with the mux's two endpoints only, and `x_configure()` dropped
the Ethernet pair at the controller as well. The NIC stayed registered,
pointing at endpoints that no longer existed, so every frame failed in
`un_send()`. DHCP's DISCOVER never left the laptop. That is "0 asked",
not "0 answered".

**Fix:** `usb_claim_interface(dev, cfgidx, ifnum, alt)` in `usb/core.c`.
If the device is not yet in that configuration it is selected as
before. If it already is, the configuration is left alone:

- the new interface's endpoints are added beside the ones already held;
- the held ones are marked `keep`;
- `SET_INTERFACE` is sent only for a non-zero alternate;
- `x_configure()` adds the new endpoints and leaves the running ones
  alone. Re-adding them would re-lay their rings under an outstanding
  receive and reset a sequence the device did not reset.

Every endpoint now records its interface (`usb_ep_t.iface`), and
`usb_find_ep_if()` finds a driver's own bulk pair rather than the first
one on the phone. Both `usb/ethernet.c` and `usb/usbmux.c` claim; neither
selects.

## 2. TLS died at "ClientHello sent": the mux refused a five-byte read

`net/tls.c` reads a record the way every TLS stack does: five bytes of
header, then the body. The phone answers a ClientHello with its whole
flight (ServerHello, Certificate, CertificateRequest, ServerHelloDone) in
**one** mux packet of about a kilobyte.

`usbmux_recv()` refused any packet larger than the caller's buffer, with
`USB_EPARAM`. It did this on purpose, to avoid a short read that leaves
the rest unacknowledged. So the first read of the handshake, five bytes,
got a refusal. The transport called the connection dead, and the
handshake failed with −1 at "ClientHello sent" with no alert. The
ClientHello was fine: the answer arrived, and this function would not
hand it over.

**Fix:** the caller gets what fits. The rest goes into the stash, in
order, and the whole packet is accepted and **acked** at once. The stash
path serves partial reads the same way.

**Why no test saw it:** every TLS check in `test_lockdown` runs against
a fake TLS layer that reads whole packets. `test_lockdown` now also
drives the real transport the way `net/tls.c` does: 5 bytes, then the
rest. `test_usbmux` reads a 1200-byte packet five bytes at a time and
checks that every byte arrives in order and that the whole packet was
acked at once.

## 3. "REFUSED the port": one source port for ever

Every connection came from source port 0x4854. Because of (2), the
broken handshake left the phone holding that connection with unacked
data. The next SYN from 0x4854 → 62078 looked to the phone like a SYN
for a connection it already had, so it answered RST. That gave "The
device REFUSED the port", on the one port an iPhone always listens on.

**Fix:** a fresh source port per connection, as usbmuxd does
(0x4854, 0x4855, … wrapping inside 256). The counter lives outside the
state that `usb rescan` clears.

## The screen says more now

- `ifconfig dhcp` always prints asked/offered/acknowledged. When a
  request never left the laptop, it also says so, names the network
  card, gives its link state and the error the stack got, and points at
  `usb eth`.
- A failed `pair` handshake also prints what the mux layer knows:
  whether the connection is still up, and why it went if it did.

## Tests

| Suite | Checks | Change |
|---|---|---|
| test_xhci | 334 | +24, both claim orders, through a controller that executes the commands |
| test_usbmux | 378 | +24: split reads, the 5-byte TLS read, fresh ports |
| test_usbnet | 125 | +7: the mux holds interface 1 first, and Ethernet joins it |
| test_lockdown | 365 | +5: the real transport read the way TLS reads |

New injections:

| Harness | New | Result |
|---|---|---|
| `check_xhci_suite.py` | 7 | 89/89 caught |
| `check_usbmux_suite.py` | 5, plus 1 re-anchored | all caught |
| `check_lockdown_suite.py` | 1, in `usb/usbmux.c`: the 0.8.9.8 bug itself | 70/70 caught |
| `check_usbnet_suite.py` | 1, plus 1 re-anchored | 29/29 caught |

The hub, msc, hotplug and dhcp harnesses were also re-run, all caught.

## Still not proved on the HP

- **The TLS session to lockdownd.** The transport fix removes the
  failure in the photos, but this stack offers exactly one cipher suite
  (TLS_RSA_WITH_AES_128_CBC_SHA256). If lockdownd does not take it, the
  phone will now answer with an alert, and `pair` will print its number.
- **DHCP over the phone.** The DISCOVER can leave now. Whether the
  phone's hotspot answers it is the next photo.
- The 0.8.9.6 write-combining, pointer and trackpad work.
