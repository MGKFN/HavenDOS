# HavenDOS v0.8.9.3 — the reply budget was counted in the wrong unit

9 November 2026. 59 suites, **5922/5922**.

**The device public key was arriving all along.** The HP's kernel log
is the whole diagnosis:

```
[20.090] lockdown: GetValue DevicePublicKey sent, 334 bytes
[20.090] warn lockdown: no reply at all          <- the SAME 10 ms tick
[26.990] lockdown: QueryType sent, 285 bytes
[27.000] warn lockdown: a 914-byte reply with no Type in it
```

Those 914 bytes were the GetValue answer — `<key>Value</key><data>` and
a PEM RSA key — turning up a moment late and being read as the reply to
the QueryType that followed. From then on every reply was one request
behind, each one perfectly framed and containing the answer to the
previous question.

## The cause

`LD_POLL_TRIES 20000`. A count of **polls**, not a duration — and
described elsewhere in the file as "a ten-second budget", which it was
not. A `usbmux_recv()` with nothing waiting is one bulk-IN that NAKs
and returns at once, so on that machine twenty thousand of them
finished in under ten milliseconds.

An iPhone answers QueryType immediately and takes appreciably longer
over `DevicePublicKey`, because it has to go and fetch the key. So
stage 1 always worked and stage 2 never did, which is exactly what it
looked like.

## The fix, in two halves

**Wait on time.** `LD_REPLY_TICKS` is 500 and `LD_FIRST_TICKS` 1000 —
five and ten seconds, in `get_ticks()`'s 10 ms units, which this
project has got wrong twice and so is spelled out at every use. The
first reply of a connection gets the longer budget; the device may
still be waking the service up. Progress still resets it, so a reply
in several packets is not abandoned between them.

**And a read that gives up must drop the channel.** This is the half
that matters more. Lengthening a budget makes a late reply rare; it
cannot make it impossible, because a device busy enough beats any
budget. The answer is still coming, and the next reader of that stream
is the next request. So `ld_desync()` tears the connection down — on a
timeout, on a reply shorter than its own length prefix, and on one that
stopped part way. The next request reconnects, costs one SYN, and
starts from a position both ends agree on.

`ld_send_plist()` has had this rule for a partial send since 0.8.8.10,
for the identical reason stated the identical way. The read side simply
never got it.

**`lockdown_dump()` outlives the connection.** It used to return
"not connected" and stop, which was fine while the only way to be
disconnected was never to have connected. Now that a timeout drops the
channel on purpose, the reply fragment it was holding is the most
useful thing on the screen at that moment — returning early would throw
away exactly what the teardown exists to let somebody look at.

## Why no suite could have found it

**In the fixture, one poll costs one tick.** A poll count and a tick
count are the same number there, and no suite can reproduce a machine
where a poll costs half a microsecond. That is the finding, and it is
recorded in `tools/check_lockdown_suite.py` rather than papered over:
the shipped defect is not injectable.

What the suite can check is the behaviour the fix specifies, and it
does now — the fixture has a clock, advanced one tick per poll, because
asking the device whether it has anything yet is what takes the time:

- a device slower than the budget times out, **and the channel is
  dropped**, and the next request reconnects and gets **its own** reply
- a device just inside the budget is waited for rather than abandoned
- a reply that stops after its length prefix with no RST — the device
  is still working on the body — drops the channel too, and keeps the
  four bytes as evidence
- the two-packet slow-reply case, re-expressed in ticks, still proves
  the budget is reset by progress

All of these were written by reverting the fix and watching them fail
first. Four injections, all caught; `tools/check_lockdown_suite.py`
reports **69 of 69**. One more stale injection was found and removed on
the way — it still named the `tries = 0` reset.

## What 0.8.9.2's sequence check turned out to be

Not this bug. The ack arithmetic in the HP's log is clean throughout —
302, then 1220, then 1521, each exactly one reply apart — so nothing
was ever duplicated. The sequence check stands as correct hardening
that a transport should have had from the start, and the duplicate
tests stay. It was not the cause, and this changelog says so because
the previous one implied otherwise.

## Still open, and seen in the same log

Neither is touched here.

- **A USB transaction error took the whole bus down.** At 69.29 s:
  `xHCI slot 2 ep0: USB Transaction Error (cc 4)`, then port 3 removed,
  then `ADDRESS_DEVICE was refused` on ports 2, 9 and 12, then
  `usb: 0 devices, 0 disks, 0 NICs after hot-plug`. One device faulting
  should not cost the Kingston and the webcam. That is a real defect
  and it is the next thing to look at.
- **The kernel log fills and drops lines.** `(log full: later lines
  were dropped)` arrived before the interesting part of a session.
- **The desktop is choppy.** Not diagnosed. The hot-plug scan and the
  DHCP retry both run off the 100 Hz bottom half and both appear
  repeatedly in that log; that is where to look first.
