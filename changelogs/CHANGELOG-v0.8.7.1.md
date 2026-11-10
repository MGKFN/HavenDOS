# HavenDOS v0.8.7.1

**Channel:** Beta.  **Codename:** Iris.  **Date:** 26 October 2026.

**Everything v0.8.7 added was refused by its own back-end.** The Apple
Ethernet driver never sent or received a byte on the HP, and the reason
was not in the driver.

---

## What the laptop said

`usb eth`, first run on real hardware:

```
eth0  %04X:%04X  if 1452 alt 4776  ep %02X/%02X
      mac %02X:%02X:%02X:%02X:%02X:%02X
      carrier 0  rx 0/0/19440  tx 0/48
      first transfer: nothing received yet
```

Two separate bugs, both this release's, both found in one screen.

## 1. The xHCI back-end cannot carry an Ethernet frame

`usb/xhci_hc.c` copies every transfer through a fixed buffer and
**refuses anything longer — before the controller is touched**:

| path | bound | was | an Ethernet frame |
|---|---|---|---|
| `x_data()` — every synchronous bulk and control | `sizeof g_bounce[0]` | **1024** | 1514 out |
| `x_intr_poll()` — every non-blocking IN | `XHCI_ABUF_SZ` | **64** | 1516 in |

Both were right when they were written. `g_bounce` was 1024 because the
largest thing this kernel moved was a 512-byte sector; `XHCI_ABUF_SZ` was
64 because the only asynchronous IN was a keyboard report. They stood up
for three releases because nothing bigger existed.

0.8.7 added a driver that moves Ethernet frames, and neither fitted. So
`un_send()` returned `USB_EPARAM` forty-eight times and `un_recv()`
returned it **nineteen thousand four hundred and forty times** — every
pass of the shell's idle loop — without one byte being requested from the
device. The counters read like a dead link. It was a local refusal.

**Both buffers are 2048 now**: 4 KB for `g_bounce`, 180 KB for `g_abuf`.

**Why the suite missed it.** `tests/test_usbnet.c`'s simulated controller
accepted a transfer of any length — **a back-end more permissive than the
real one**, which is the same mistake as a mock that cannot refuse, and
`include/usb.h` has warned about that since v0.8.5. It carries both
bounds now, and with the old values restored the suite fails 12 checks.

`tools/check_usb_buffers.py` is new and refuses three things: a driver
constant larger than the bound that will refuse it, a simulated bound
that does not **equal** the kernel's, and a missing one.

## 2. klog is not printf, and the difference is silent

`kernel/klog.c` implements `%d %u %x %s %c %%` and one pad digit 1–8, so
`%4x` prints `00c1`. Anything else reaches the default case, which prints
the specifier **literally and does not consume its argument** — so every
later specifier on the line takes the wrong one.

`%04X:%04X ... if %u alt %u` therefore printed `if 1452 alt 4776`: 1452
is `0x05AC` and 4776 is `0x12A8`, the vendor and product ids, slid two
columns to the right. The MAC line was literal end to end.

That line is the one diagnostic the whole release depends on — it is what
`usb eth` exists to print and what the hardware test plan tells the tester
to read. A formatting bug took out the instrument.

Fixed in `usb/ethernet.c`, which was the only file affected: the audit
found 14 bad specifiers there and **none anywhere else** in the kernel's
112 `klog` calls. `tools/check_klog_fmt.py` refuses them now.

---

## Still unexplained, and not claimed as fixed

`usb cfg` reported **“the device would not describe it (3)”** for every
configuration of every device on the bus — the phone, and the MediaTek
Bluetooth that has nothing to do with this driver. Error 3 is `USB_EXFER`,
which `fetch_config()` returns on a short read or a `wTotalLength` under
9. The same command printed the phone's four configurations in full on
25 October.

It is **not** explained by the two bugs above: both refuse the transfer
locally, so the Ethernet driver queued nothing to the controller and
cannot have wedged it. `fetch_config()` is byte-for-byte the logic the
0.8.6.7 survey used, and control transfers are bounded at 2048, well
above a descriptor fetch.

So this is open, and the next boot is what narrows it. See the handoff.

---

## Testing

- `test_usbnet` 110 → **118**, and the simulated controller now enforces
  the real back-end's bounds.
- **39 suites, 2,782 checks.**
- Build checks: **15**. `check_klog_fmt.py` and `check_usb_buffers.py`
  are new, and both were proved to fail on the exact defects this release
  fixes before being wired in.
