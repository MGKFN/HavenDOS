> **ANSWERED ON THE HP, 11 November 2026, on 0.8.9.9.** iPhone tethering
> works end to end: `ifconfig dhcp` gave 172.20.10.8 with gateway
> 172.20.10.1, the gateway, 8.8.8.8 and google.com all answered `ping`,
> and `wget google.com` saved 7.2 KB. The first time HavenDOS has reached
> the internet from real hardware. Kept for the record; nothing here
> needs running again.

# v0.8.7 "Iris" on the HP — the five minutes that settle it

**Nothing in this release has been on hardware.** 37 suites and 2774
checks pass, eighteen injection harnesses catch every defect they
plant, and every one of the 0.8.6 bugs those suites did not catch was
found by this laptop rather than by QEMU. A green suite is not a
hardware test and this tree has five releases of evidence for that.

What is new, in the order it gets used:

| what | where | proved by |
|---|---|---|
| one NIC table and dispatch | `net/netdev.c` | 121 checks, 30/30 injected |
| alternate settings | `usb/core.c` | 294 checks (was 218), 76/76 |
| Configure Endpoint DROP flags | `usb/xhci_hc.c` | same |
| Apple USB Ethernet | `usb/ethernet.c` | 110 checks, 28/28 |
| **any of it on real silicon** | — | **nothing** |

---

## Before you boot

The phone must be **unlocked and trusted**, with **Personal Hotspot
ON**. On 25 October an unlocked and trusted phone enumerated during the
boot scan at 1.18 s; an untrusted one needed a rescan 331 seconds in.

Plug it in **before** booting. Nothing in this kernel runs on a timer,
so a device plugged in afterwards is noticed when it is asked for —
`usb rescan`.

Boot **installed, not live**, on a boot entry without `live`.

---

## The five minutes, in order

```
ver                  0.8.7 actually took
dmesg | grep NIC     the one line that says whether any of this worked
usb cfg              the phone, and which configuration it is in now
usb eth              THE TEST -- see below
net                  an address, if DHCP got one
ping 8.8.8.8
```

### `dmesg` — what to expect

```
usbnet: 05AC:12A8 cfg 3 if 2 is Apple Ethernet
usbnet: ep 83 in / 04 out, 512/512 bytes
NIC: Apple USB Ethernet, 1 adapter(s)
```

The endpoint addresses are read off the descriptor, so `83` and `04` are
what the simulated phone used and may well differ on yours. The
**classes and the configuration index are the thing to check**: `cfg 3`,
`if 2`.

If instead you get one of these, the release tells you *which* step
refused — that is deliberate, and it is v0.8.6.5's lesson about
`update` naming which of four reasons a stick was not used:

| line | what it means |
|---|---|
| `NIC: no adapter, networking off` | no `FF/FD/01` interface in **any** configuration. Hotspot off, or a phone that never offers it |
| `SET_CONFIGURATION 3 refused` | the phone would not leave PTP. A locked phone does this |
| `SET_INTERFACE 2 alt 1 refused` | it offers the interface and will not switch it. **This is the pairing question** — see below |
| `alt 1 has no bulk pair` | the endpoints are not where the descriptors said. Send me `usb cfg` |
| `GET_MACADDR refused` | the two vendor control requests are wrong, or need pairing first |

Each has a different fix, and "tethering did not work" is all five
wearing one face.

---

## `usb eth` — the one screen that matters

This is why the command exists. The probe path is built on **your**
descriptors. The **framing is not**: the two bytes this driver strips
from a received frame, the fixed 1514-byte zero-padded transmit, and
the two vendor requests are what the published Linux `ipheth` driver
does. No Apple document says so and this kernel has never exchanged a
byte with the interface.

So the driver keeps the **first transfer exactly as it arrived, before
stripping anything**, and this prints it raw:

```
eth0  05AC:12A8  if 2 alt 1  ep 83/04
      mac 02:1C:B3:44:55:66
      carrier 1  rx 3/0/0  tx 1/0
      first transfer: 66 bytes
      01 02 FF FF FF FF FF FF AA BB CC DD EE FF 08 06
```

**Read the last line.** An Ethernet frame starts with a six-byte
destination, and a freshly tethered link is full of broadcasts, so
`FF FF FF FF FF FF` is hard to mistake:

- **two bytes, then the FFs** → `IPHETH_RX_SKIP` is 2 and the framing in
  this driver is right.
- **the FFs start immediately** → there is no prefix. Set
  `IPHETH_RX_SKIP` to 0 in `usb/ethernet.c` and the suite's
  "RX drops exactly two bytes" checks need the same number.
- **some other number of bytes before the FFs** → that is the prefix
  length, and it is a thing nobody has written down. Send me the line.
- **`first transfer: nothing received yet`** → the link is up and the
  phone has sent nothing. Try `ping`, or `net dhcp`, to make it talk.

`rx a/b/c` is frames / runts / errors; `tx a/b` is frames / errors. A
rising **runt** count with no frames is the prefix length being wrong in
the other direction.

---

## What this release cannot tell you, and the HP can

**Whether the Ethernet interface is usable without pairing.**

Interface 1 of the same configuration is `FF/FE/02` — the Apple Mobile
Device interface, usbmux, the lockdown channel. Whether the Ethernet
interface is merely *offered* without a trust handshake or also
*works* is not written down anywhere, and no amount of simulation
answers it.

The shape of the answer:

- `SET_INTERFACE` **accepted** and frames flow → no pairing needed, and
  0.8.7 is done.
- `SET_INTERFACE` accepted, `GET_MACADDR` refused → the interface is
  there and the device wants something first. That is a usbmux
  implementation, and it is not a 0.8.7 item.
- `SET_INTERFACE` **refused** → same conclusion, reached one step
  earlier.

---

## The parts that are not the phone

Worth checking in the same boot, because they are new and cheap to
look at:

```
usb -vv              port 12, and the whole bus
disk                 the Kingston, as a sanity check on 0.8.6.7
usb rescan           then `usb eth` again
```

`usb rescan` now **releases the NIC before re-enumerating** and
re-probes afterwards. Each NIC holds an index into the USB device
table, and `usb_enumerate()` rebuilds that table — so a NIC left
registered across a rescan would address whatever moved into its old
slot. That is the v0.8.6.1 slot leak's shape one layer up. The rescan
line reports the count:

```
3 devices.  1 USB disk.  3 disks in all.  1 USB NIC.
```

Rescanning two or three times in a row is worth doing: it is the
pattern that found the slot leak, and the NIC layer has the same shape
of bug available to it.

---

## If the machine locks up

The most likely new cause is the receive path blocking. `net_poll()`
runs from the shell's idle loop, nothing here runs on a timer, and a
bulk IN with no packet waiting NAKs — which an xHCI retries for ever
rather than completing. `usb_bulk_poll()` exists to stop that and the
suite proves the driver uses it, but the suite's controller is not this
laptop's.

Symptom: the shell stops responding **only once a NIC is registered**,
and recovers if you boot with the phone unplugged. If that happens, say
so and do not try to debug it from the prompt.

---

## What a pass looks like

```
ver                  v0.8.7
dmesg | grep NIC     NIC: Apple USB Ethernet, 1 adapter(s)
usb eth              two prefix bytes, then FF FF FF FF FF FF
net                  an address from the phone's DHCP server
ping 8.8.8.8         a reply
```

The last line is the first packet this operating system has ever sent
over something other than an emulated NIC.
