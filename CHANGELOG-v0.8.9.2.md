# HavenDOS v0.8.9.2 — the replies were one behind

9 November 2026. 59 suites, **5908/5908**.

One bug, found on Kyan's HP, in `usb/usbmux.c`. It is not from 0.8.9:
that file is byte-identical back to 0.8.8.11 and the defect is older
than both.

## What it looked like

```
lockdown
  what arrived: 918 bytes, the prefix says 914
  <key>Key</key><string>DevicePublicKey</string>
  <key>Request</key><string>GetValue</string>
```

A `QueryType` answered by the **GetValue** reply. And the GetValue that
followed answered by the **QueryType** reply, 301 bytes, equally well
framed. Every reply parsed perfectly and contained the answer to the
previous question, so stage 1 of pairing reported itself as a protocol
failure — `No Type in it` — when the transport had simply gone one
message out of step.

## The cause

Nothing in the receive path ever looked at a packet's sequence number.
`rx_isn` was recorded during the handshake and never read again, and
both readers advanced the acknowledgement by **adding the payload
length**:

```c
g_m.tx_tcp_ack += (uint32_t)r;
```

That is a count of bytes this side has seen. It is a position in the
device's stream only while every packet arrives exactly once.

So a **retransmission** — which is what TCP does whenever an ACK is
late or lost — was delivered as new data. One duplicate puts the
conversation permanently one reply behind, and it survives disconnect
and reconnect, because a leftover packet carrying an old sequence
number is indistinguishable from new data when nothing checks.

v0.8.8.10 made it likelier by adding a second reader, `mux_harvest()`,
which acknowledges on its own during a multi-packet send.

## The fix

`mux_rx_accept()`, used by both readers, applying TCP's own rule:
`tx_tcp_ack` **is** the next byte expected, so a data packet is new if
and only if its sequence number equals it. Anything else is dropped
**and re-acknowledged** — a silent drop leaves the device repeating the
same packet forever, since it retransmitted believing we never got the
bytes.

The acknowledgement is now *derived* from the packet, `seq + length`,
never a running total, so it cannot drift away from the stream it
describes.

`usb mux` reports `out-of-sequence packets dropped`, counted for the
life of the bus so a reconnect does not hide it.

## Why 5883 checks and 65 injections missed it

**`tests/muxdev.inc` had never sent the same packet twice.** That is
the whole of it. A property that holds only while something never
happens needs a test in which it happens — `tests/test_xhci.c` records
the identical lesson about controller slots, and it was not carried
across to the transport.

The fixture retransmits now, and three things are asserted that could
not be before:

- a duplicate is **not delivered**, is counted, and is **re-acked**
- the request *after* a duplicated one gets **its own** reply — the
  exact symptom, as an alternating QueryType/GetValue conversation
  repeated four times
- the acknowledgement lands on `seq + length` and a duplicate does not
  move it

Against the old code those fail, six checks at once. They were written
by reverting the fix and watching them fail before they were trusted.

The fixture also had to learn to **answer the question it was asked**.
Its GetValue modes replied with the GetValue document whatever had been
requested, which is fine for one request per connection and makes an
alternating conversation impossible to model — and an alternating
conversation is the only shape in which a reply arriving one request
late can be seen at all. A fixture that answers every question
identically cannot tell you the answers got swapped.

## Two more stale injections

`tools/check_usbmux_suite.py` now reports **70 of 70 caught**, and
getting there found two that had quietly stopped applying:

- *"received data is not acknowledged"* matched nothing, because the
  ack moved into `mux_rx_accept()`.
- *"flags other than a bare ACK are accepted"* matched **twice**, and
  had since v0.8.8.10 added the same check to `mux_harvest()`.

Both are anchored on the comment above their code now, which does not
move when the line does. That is five stale injections found in two
releases; `inject.py` reporting them rather than counting them as
caught is the only reason any of them surfaced.

One injection is **recorded as unreachable rather than counted**:
accumulating instead of deriving the ack is, inside that branch, the
same number by the condition that got us there.

## Also in this release

The 0.8.9.1 work, since it never reached the HP: `lockdown` is a real
command, and the lockdown reply is printed on screen — byte count, the
length prefix beside it, the first 16 bytes in hex, and the text. That
screen is what found this bug. Before it, `usb lockdown` wrote its
evidence to the kernel log with the echo turned off.
