# HavenDOS v0.8.9.7 Beta "Helios" — the phone was talking

*10 November 2026. 59 test suites, 6049/6049 checks.*

0.8.9.5's fix worked on the HP. `pair` hung up during the keygen and
called back afterwards, and the 3728-byte pair request went out on a
live connection for the first time. Then:

```
  the request is 3728 bytes, 2 mux packets.
  sending it...
  (watch the phone -- the Trust dialog appears there.)
  the request failed (-3):
  a packet that was not TCP on an open connection
  what arrived: 0 bytes -- NOTHING. The device never answered.
```

Two bugs. The second caused the first.

---

## 1. A CONTROL packet is the phone talking, and we hung up on it

Every usbmux protocol other than TCP on an open connection was treated
as a fault, and the connection was dropped on the spot.

Protocol 1 is **CONTROL**: the way an iPhone's usbmux sends the host a
message in words. usbmuxd (`device.c`, `device_control_input`) reads it
like this:

- the first payload byte is the type: 3 is an error, 7 is information
- the rest of the payload is text
- it **logs the text and carries on with the connection exactly as it
  was**

So the phone had explained what it objected to, in a sentence, and this
driver threw the sentence away unread and hung up.

**Now the message is kept, logged, and skipped.**
- Only printable bytes are kept, so a phone's message cannot put
  control characters on the console.
- It is kept across reconnects. The message sent as one connection died
  is the one worth reading after it.
- `pair` and `lockdown` print it on screen:

  ```
  the phone said: ERROR <its text>
  ```

The type values come from usbmuxd's source, not from the wire: no
CONTROL packet had been captured from a real phone before this. The
next photo of `pair` will be the first.

## 2. Why the phone objected: a missing zero-length packet

USB bulk transfers carry no length field. A transfer ends at the first
USB packet **shorter** than the endpoint's maximum, which is 512 bytes
on a high-speed iPhone. When a transfer's length is an exact multiple
of 512, the last packet is full, the device cannot tell the transfer
is over, and it keeps reading until the host sends a **zero-length
packet** (ZLP). usbmuxd sends one after exactly these
(`usb.c`, `usb_send`: *"Send ZLP"*). HavenDOS never did.

Nothing noticed, because nothing HavenDOS had sent before was that
size:

| Request | Size | Exact multiple of 512? |
|---|---|---|
| QueryType | 285 bytes | no |
| GetValue | 334 bytes | no |
| Pair, first mux packet | 16 + 20 + 2012 = **2048** (4 × 512) | **yes** |

So the pair request was the first full-size packet this driver ever
sent, and its length happened to be an exact multiple of 512. Without
a ZLP:
- the phone kept reading after the first packet
- it glued the second mux packet onto the end of the first, in one
  transfer
- the header described a 2048-byte packet inside a transfer of nearly
  four thousand
- the phone refused it with a CONTROL message, and section 1 dropped
  that unread

`mux_send()` now sends a ZLP after any packet whose length is a
multiple of the endpoint's packet size. A ZLP that fails counts as a
failed send. UHCI's `uhci_bulk()` could not send a zero-length OUT at
all, because its loop never ran for length 0. It builds one now: a
single TD whose length field is `0x7FF`, UHCI's spelling of "no data".

### The fixture could not have seen it

`tests/muxdev.inc` handed every bulk OUT call to the simulated device
as a complete message. A driver that never sent a ZLP was therefore
indistinguishable from one that did.

It now behaves the way USB does. It **accumulates** a transfer until a
short packet or a ZLP ends it, and only then delivers it, glued
together if two messages arrived without a terminator between them.

- **Without the ZLP fix, against the new fixture,** the multi-packet
  tests fail by the dozen.
- **With it, three tests needed re-indexing.** They stall "the Nth
  bulk OUT" by number, and a ZLP is now one of the numbered OUTs.

This is the fourth release in a row where the bug was something the
fixture had never been asked to model:

| Release | The fixture had never... |
|---|---|
| 0.8.9.2 | sent a duplicate packet |
| 0.8.9.4 | generated a 2048-bit key |
| 0.8.9.5 | sent a FIN |
| 0.8.9.7 | modelled where a USB transfer ends |

## 3. Two stale injections, two releases old

`tools/check_lockdown_suite.py` had two injections that stopped
matching in **0.8.9.5**, when `g_ld.why` lines went into the blocks
they anchored on. That harness was not run when 0.8.9.5 shipped, so
nobody saw it. Both are re-anchored and caught.

The lesson stands where it is written in the handoff: a harness nobody
runs cannot tell you it has stopped working. Every harness whose
suite's files changed in this release was run.

## 4. A flaky suite that died silently

`test_tlscc` died once in eight runs with exit 141 (SIGPIPE) and
printed nothing, so the result read *"the suite printed no result"*.
The cause was a write to a socket that `openssl s_server` had already
closed. SIGPIPE's default action kills the process. The suite now
ignores SIGPIPE, so that write fails with EPIPE and a reason instead,
and stdout is line-buffered so whatever did print is kept. Left alone,
this would have blocked a GitHub release at random.

---

## Testing

| Suite | Before | Now |
|---|---|---|
| test_usbmux | 334 | 356 — CONTROL messages (kept, sanitised, counted, surviving a reconnect, the connection staying up, data after them still delivered); the ZLP after a 2048-byte packet and after a 512-byte one, none after a 336-byte one, a failed ZLP is a failed send, no transfer glued |
| test_lockdown | 360 | 360 — re-indexed for the ZLP |

**Injection harnesses:**
- `check_usbmux_suite`: 82/82. Six new: CONTROL dropping the connection
  again, unsanitised text, a reconnect erasing the message, no ZLP, a
  ZLP after every packet, a failed ZLP ignored. That last one went
  MISSED until a test stalled the ZLP itself.
- `check_lockdown_suite`: 69/69 after the re-anchoring.

## On the HP

Run `pair`. There are three possible outcomes now:

1. **The Trust dialog on the phone.** The ZLP was the whole of it.
2. **A reply from lockdownd.** Stage 3 is past the transport and into
   the protocol.
3. **`the phone said: ERROR …`** — the phone's own sentence about what
   is still wrong. That is the first time it will ever have been
   readable.

0.8.9.6's performance and pointer fixes are in this release unchanged
and still unconfirmed.

## Files

| File | What changed |
|---|---|
| `usb/usbmux.c`, `include/usbmux.h` | CONTROL messages kept and skipped; `usbmux_device_said()`; ZLP after full-size packets; `usbmux_zlps()` |
| `drivers/uhci.c` | a zero-length bulk OUT is a real transfer |
| `shell/usb.c` | `the phone said: …` on the `pair` and `lockdown` screens |
| `tests/muxdev.inc` | `queue_control()`; transfers end on a short packet or a ZLP |
| `tests/test_usbmux.c`, `tests/test_lockdown.c` | as above |
| `tests/test_tlscc.c` | SIGPIPE ignored, stdout line-buffered |
| `tools/check_usbmux_suite.py`, `tools/check_lockdown_suite.py` | six new injections, two re-anchored |
