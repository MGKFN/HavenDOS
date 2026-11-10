# HavenDOS v0.8.9.5 Beta "Helios"

*9 November 2026. 59 test suites, 5949/5949 checks.*

A fix release. One defect, found on Kyan's HP, and it is an ordering
mistake rather than a protocol one: `pair` held a lockdownd connection
open and silent for seventy-one seconds and the device, reasonably,
hung up.

---

## 1. The screen

v0.8.9.4 fixed the keygen loop, and the keys this release was tested
with are the first real ones the project has produced on hardware:

```
  generating the root key (2048-bit)...
  root key: done in 54180 ms
  generating the host key (2048-bit)...
  host key: done in 17090 ms
  building the certificates...
  root 706, host 719, device 705 bytes.
  HostID 91FE48C0-A4E5-408F-9ED5-C72A30D6E437
  the request is 3728 bytes, 2 mux packets.
  sending it...
  the request did not get a reply (-3).
```

Everything above the last line is correct. Two 2048-bit keys, three
certificates of plausible sizes, a UUID, and a pair request split
across two mux packets exactly as 0.8.8.10 built it to be.

## 2. Seventy-one seconds of silence

54180 + 17090 is 71.27 seconds, and for all of it the connection opened
four screens earlier was open and carrying nothing.

Nothing in the keygen needs the device. The only thing taken from the
phone is its public key, and that is fetched, parsed and sitting in
`g_devn` before the arithmetic starts. The connection stayed open for
no reason but the order the function happened to be written in — which
is the kind of defect that survives every unit test, because every
piece of it works.

lockdownd closes an idle connection. The FIN was on the wire before the
3728-byte request went out; the request went into a connection the
device had already finished with, and the first read afterwards picked
the FIN up.

**The fix is not a keepalive.** A keepalive means holding a connection
open for over a minute in order to send nothing down it. The right
number of packets to send during a computation is zero. So `pair` hangs
up, generates, and calls back:

```
  the device's key is 2048 bits.
   hanging up while they are generated -- lockdownd
   closes an idle connection, and this takes over a
   minute. `pair` calls back when the keys are ready.
  generating the root key (2048-bit)...
```

`lockdown_connect()` is idempotent, so the call back costs nothing when
the connection is still up — which is the case on the path where the
keys were loaded from the stored record and no time passed at all.

`tools/check_pair_idle.py` refuses a `keygen()` caller that does not
hang up first and call back after. Both halves were tested by taking
each out and watching it fail.

## 3. -3 was the wrong number, and it was nobody's fault but ours

`USB_EXFER` is the code for CRC, babble, bitstuff and buffer errors —
the bus went wrong. `usbmux_recv()` returned it for a device that had
closed a connection politely, because FIN went through the catch-all:

```c
/* usbmuxd treats anything but a bare ACK on an established
   connection as a reset, FIN included -- it has no half-close. */
if (flags != MUX_TH_ACK_F) {
    ...
    return USB_EXFER;
}
```

The comment is right about the *handling* — usbmuxd has no half-close
and the connection does end — and wrong about the *reporting*. A
close is not a fault, and a caller told it is goes looking at the USB
layer, where nothing had gone wrong at all.

FIN now has its own branch, returns `USB_ENODEV`, and says so. This is
v0.8.7.4's finding again: one error code covering four distinct
failures is four bugs you cannot tell apart.

## 4. The reason, in words

A number says which layer gave up. It does not say what the wire did.
`usbmux_why()` and `lockdown_why()` return a sentence — never null —
set at the point the decision is taken rather than reconstructed from a
code afterwards. `pair` prints it:

```
  the request failed (-4):
  the device closed the connection (FIN). lockdownd does this to a
  connection left idle -- generate the keys first, then connect
```

This is the v0.8.9.1 lesson one layer down. That release found a
diagnostic written entirely in `klog()`, on a system where
`kernel/main.c` turns the log's screen echo off before the shell
starts. The send and receive paths still had it.

## 5. The Pair request waits for a person

Every other request in this protocol is answered by software.
`LD_REPLY_TICKS` is five seconds because that is generous for a machine
— and it is the wrong kind of budget for a request that puts a dialog
on a phone somebody has to pick up.

`lockdown_reply_budget()` gives the next request, and only the next,
a longer one. `pair` asks for thirty seconds and says to watch the
phone. Making the ordinary budget that large instead would mean every
real timeout took half a minute to report.

A cheap QueryType goes down the reconnected channel before the
expensive request does. 285 bytes to learn the stream is aligned beats
spending the 3728-byte request to find out.

## 6. What the suite could already do and had never been asked

`tests/muxdev.inc` could send a FIN. `queue_tcp()` takes any flags and
always has. No check had ever asked it to.

The fixture was not short of a feature; it was short of a question.
Every established-state check in the hundred and fifty above this one
is driven by a device that answers and keeps the connection, because
that is the device a protocol test naturally models. The real one hangs
up.

This is the same shape as v0.8.9.2's gap — `muxdev.inc` had never sent
the same packet twice — and as v0.8.9.4's, where `test_rsa.c` had only
ever generated 512-bit keys. Three releases, three suites that proved
an API works and said nothing about the conditions a caller actually
meets.

`tests/test_usbmux.c` is now 334 checks (was 304), covering FIN|ACK, a
bare FIN, RST, flags that are none of the three, and that a fresh
connection does not inherit the last one's reason.

`tools/check_usbmux_suite.py` carries six new injections, all caught.
Three older ones went stale against these edits and were re-anchored —
including one that had already gone stale once, in 0.8.9.2, and whose
anchor was the comment quoted in section 3. The comment was wrong, so
splitting the FIN out took it away. An injection anchored on a wrong
comment is an injection anchored on something that was always going
to move.

**And the first version of this release's own stale-reason check did
not bite.** It read `usbmux_why()` on a connection that had never
failed, which proves only that the fixture was reset. The harness
reported the injection MISSED, which is exactly what it is for.

---

## Files

| File | What changed |
|---|---|
| `usb/usbmux.c` | FIN gets its own branch and `USB_ENODEV`; a reason string on every refusal; `usbmux_why()` |
| `include/usbmux.h` | `usbmux_why()` |
| `usb/lockdown.c` | reasons on every failure path; `lockdown_why()`; `lockdown_reply_budget()` |
| `include/lockdown.h` | both new entry points |
| `shell/paircmd.c` | hang up before the keygen, call back after, QueryType first, print the reason; `lockdown dump` → `lockdown` |
| `tests/test_usbmux.c` | 30 new checks across five sections |
| `tools/check_usbmux_suite.py` | 6 new injections, 3 re-anchored |
| `tools/check_pair_idle.py` | new |
| `Makefile` | the new checker, and test_usbmux's floor at 334 |

## Still open

- **Stage 3 is not yet confirmed on hardware.** Stages 1 and 2 are.
  The next unknown after this fix is whether iOS lockdownd will
  negotiate this stack's only cipher suite,
  `TLS_RSA_WITH_AES_128_CBC_SHA256`.
- A USB transaction error can still take the whole bus down.
- The kernel log fills and drops lines on a long session.
- The desktop is choppy; undiagnosed.
