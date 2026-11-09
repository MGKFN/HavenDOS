# HavenDOS v0.8.7.2

**Channel:** Beta.  **Codename:** Iris.  **Date:** 26 October 2026.

**The machine was holding QEMU's address on a real network.** That, on
its own, is every `ERR 33: dns: resolution failed` on the HP — and it
would have happened with a perfectly working Ethernet driver.

---

## The address was never asked for

`net/net.c` has carried a compile-time default since v0.6.8:

```
10.0.2.15 / 255.255.255.0   gateway 10.0.2.2
```

That is QEMU's network. It is a sensible place to land in an emulator
and it is **an address nobody on a real network has heard of**.

DHCP ran in exactly one place: once, at boot, and only `if(net_ready())`
— only if a NIC was already registered when the kernel got there.

Nothing in this kernel runs on a timer, so the ordinary way a USB NIC
appears is somebody plugging it in and typing `usb rescan` — **after
boot, after that one attempt.** The adapter came up, the NIC layer
registered it, `ifconfig` printed `Status: UP`, and the machine still
held `10.0.2.15` with a gateway at `10.0.2.2`.

Every lookup then went to `8.8.8.8` through a gateway that is not on the
network, the ARP for it went unanswered, and the only thing on screen
was `dns: resolution failed`.

**`usb rescan` asks for an address now** when it brings up a NIC and the
configuration is still the fallback, and it says plainly whether it got
one.

## An address is not a configuration

Nothing recorded where the current address came from, so nothing could
tell a negotiated address from the built-in one. `net_config_source()`
does now — fallback, DHCP, or set by hand — and three places that report
the network stopped claiming more than they know:

| where | was | is |
|---|---|---|
| `ifconfig` | `Status: UP` | `UP, address from DHCP` / `UP, address set by hand` / a yellow **`LINK ONLY — this address is the built-in fallback`** with what to do about it |
| the boot log | `ok NET: eth0 10.0.2.15` | `warn NET: no DHCP answer; eth0 on the fallback 10.0.2.15` |
| `ifconfig dhcp` on failure | `DHCP failed - using static config.` | says the fallback will not route, and what to do |

A machine reporting itself configured while holding an address its own
network has never heard of is the same class of thing as a suite that
has never failed: a green light over nothing.

---

## Confirmed fixed from v0.8.7.1

`usb cfg` describes every device on the HP again — the Kingston stick,
the webcam, and the MediaTek Bluetooth with all ten of its interfaces
and alternate settings. The *"the device would not describe it (3)"*
that v0.8.7 showed for every device is gone, and the control path is
healthy.

Also confirmed on that boot: the stick enumerates and mounts
(`USB MSC Kingston DataTraveler 3.0: 29560 MB`), and no port reports
`no answer` — the v0.8.6.7 SuperSpeed fix is holding.

## Still unproved

**Whether an Ethernet frame now crosses.** v0.8.7.1 raised the xHCI
buffers past a frame's size, and no boot since has had the phone
attached, so `rx`/`tx` on `usb eth` have not been seen moving. That is
the next thing the laptop has to answer, and the one this release cannot.
