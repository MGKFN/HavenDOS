# HavenDOS v0.8.9.8 Beta "Helios" — DHCP, for the first time

*11 November 2026. 60 test suites, 6072/6072 checks.*

## The phone said yes

Kyan's photos from 0.8.9.7 show two things.

**The iPhone has the Personal Hotspot indicator in its Dynamic Island.**
On 26 October the evidence that pairing was needed was exactly this
indicator *never* appearing: iOS does not turn tethering on for a host
it does not trust. It is on now, so iOS is serving tethering, and that
only happens for a trusted host. Pairing — usbmux, lockdownd, the RSA
keys, the certificates, the pair request — appears to have worked. It
took eight releases since 0.8.9.

**HavenDOS still could not reach anything:**

```
ping google.com   ->  ERR 33: dns: resolution failed
ping 8.8.8.8      ->  timeout x4
                      (tip: try 'ping 10.0.2.2' for the QEMU gateway)
```

## DHCP had never worked. Anywhere.

Booting 0.8.9.7 in QEMU — whose network has a perfectly ordinary DHCP
server — gives:

```
warn NET: no DHCP answer; eth0 on the fallback 10.0.2.15
```

A packet capture of that link shows eight ARP requests and **not one
UDP datagram**. Three separate defects, in three files, each enough on
its own to stop it:

1. **`net/ipv4.c` ARPed for the broadcast address.** A DISCOVER is sent
   to 255.255.255.255. `resolve_mac()` asked the link "who has
   255.255.255.255?" four times, a second each. Nobody holds that
   address, because it means everyone, so nobody answered, and
   `ip_send()` gave up with −2 before a byte reached the wire. The
   limited broadcast, and the subnet's own broadcast, now go straight to
   `FF:FF:FF:FF:FF:FF`.
2. **`net/udp.c` refused anything over 512 bytes.** A DHCP message is
   548. Even with (1) fixed, the DISCOVER would have been built and
   `udp_send()` would have said −1. Both UDP buffers are now 1472 bytes,
   the most one Ethernet frame carries.
3. **`net/dhcp.c` demanded a 548-byte reply.** That is the size of the
   buffer it *builds*, not of anything a server sends. BOOTP's minimum
   is 300, and a server sends what its options need. Even with (1) and
   (2) fixed, every real reply would have been refused, and udp.c's
   512-byte receive buffer would have cut it short first anyway.

**Why nobody noticed:** the fallback address is QEMU's own. When DHCP
fails, the stack uses 10.0.2.15 behind 10.0.2.2, which is exactly what
QEMU's server would have handed out. So in QEMU a machine that never got
a lease behaved exactly like one that did. Every networking feature this
tree has tested — HTTP, TLS, BOOT Chat, `bpkg` — ran on the fallback.

On the iPhone's network, which is 172.20.10.x, the fallback is nowhere
at all. That explains every timeout in the photo.

## And two more things that would have stopped it on the phone

- **One receive slot, overwritten by every datagram.** `eth_poll()`
  drains up to sixteen frames per call. An OFFER followed in the same
  poll by a broadcast was gone before `dhcp_request()` looked, and an
  iPhone link is not quiet. A caller that is waiting now says which
  port it is waiting for (`udp_want_port()`), and nothing else is stored
  meanwhile. DHCP holds port 68 and DNS its own port. The same overwrite
  could have eaten a DNS answer too.
- **A unicast OFFER was not ours.** A server may ignore the broadcast
  flag and send the OFFER to the address it is offering, which this
  machine does not have yet. With no address of its own, UDP to port 68
  is now accepted whatever its destination IP.

## The DHCP client itself

- **Option 53 is read.** An OFFER is no longer as good as an ACK, and a
  NAK starts again instead of waiting out the timeout.
- **The hardware address in a reply is checked.** A broadcast reply
  reaches every client on the link.
- **A fresh transaction ID per attempt.**
- **Two seconds per step**, where the comment said four and the code
  waited 0.8.
- **An ACK without a router takes the OFFER's router.** It used to fall
  back to the *old* configuration, which belongs to the old network:
  10.0.2.2 on an iPhone.

Checked against QEMU with a packet capture:

```
UDP 0.0.0.0:68    -> 255.255.255.255:67  DISCOVER
UDP 10.0.2.2:67   -> 255.255.255.255:68  OFFER
UDP 0.0.0.0:68    -> 255.255.255.255:67  REQUEST
UDP 10.0.2.2:67   -> 255.255.255.255:68  ACK
```

The boot log now says `ok NET: eth0 10.0.2.15`. It is the same address
as before, but this time it was handed out rather than assumed.

## `ping` says why, and stops recommending QEMU

The tip *"try 'ping 10.0.2.2' for the QEMU gateway"* was printed after
every failed ping on every machine. On the HP that advice was worse than
none. Now a failed ping or DNS lookup says which case it is.

**With no lease:**
```
  No address from DHCP: this machine is on the built-in
  10.0.2.15, which only exists inside QEMU.
  DHCP so far: 3 asked, 0 offered, 0 acknowledged.
  'ifconfig dhcp' asks again.
```

**With a lease**, it names the address and gateway, and says that
pinging the gateway tests the first hop.

---

## Testing

**`tests/test_dhcp.c` is new: 23 checks.** It compiles the real
`net/ipv4.c`, `net/udp.c` and `net/dhcp.c` and puts a simulated DHCP
server under `eth_send()`. That server sends 300-byte replies, the size
a real one sends and the size the old code could not read. It covers:

- the DISCOVER going out at all, to `FF:FF:FF:FF:FF:FF`, with no ARP
- a full lease: address, mask, gateway and DNS
- broadcast noise arriving in the same poll as each reply
- a server that sends unicast replies
- a NAK
- an OFFER sent again instead of an ACK
- a reply for another hardware address
- a reply too short to be DHCP
- an ACK without a router
- the subnet broadcast after a lease
- the receive slot being shared again once nobody is waiting

**`tools/check_dhcp_suite.py` is new: 10 injections, all caught.** The
first three put back the three defects that kept DHCP from ever
working.

One planned injection was left out deliberately, and the harness file
says why. The message type is checked twice, so removing either check
alone changes nothing a suite can see. An injection that cannot express
a reachable defect reports a catch nobody earned.

`tools/inject.py` now lets an injection name its own file. DHCP's
defects were spread over three files, and one suite covers all of them.

## On the HP

With the phone attached and the hotspot indicator showing:

1. `ifconfig dhcp`, or just wait. HavenDOS asks again by itself when the
   link comes up.
2. `ping 172.20.10.1`. That is the phone itself.
3. `ping 8.8.8.8`, then `ping google.com`.

If DHCP still gets no answer, the ping screen will show how many
requests went out and how many offers came back. That narrows it down to
the iPhone's USB Ethernet driver (frames not arriving), or something
the iPhone's DHCP server does that QEMU's does not.

0.8.9.6 and 0.8.9.7's fixes are in this release unchanged.

## Files

| File | What changed |
|---|---|
| `net/ipv4.c` | broadcasts without ARP; a unicast DHCP reply before there is an address |
| `net/udp.c` | 1472-byte buffers; `udp_want_port()` |
| `net/dhcp.c` | real reply sizes, option 53, NAK, chaddr, fresh xid, longer waits, the OFFER's router; counters |
| `net/dns.c` | holds its port while waiting |
| `shell/shell.c` | `net_explain()` instead of the QEMU tip |
| `tests/test_dhcp.c` | new |
| `tools/check_dhcp_suite.py` | new |
| `tools/inject.py` | an injection may name its own file |
| `Makefile` | test_dhcp, floor 23 |
