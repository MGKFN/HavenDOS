# HavenDOS v0.8.8.10 "Helios beta 11" — a message can now be bigger than a packet

Channel Beta, codename "Helios", **beta 11**. 8 November 2026.

53 suites, **5271/5271** (up from 52 and 5066).

This is stage 3 of iPhone pairing, pieces two and three of four. No
new shell command, no new screen: everything here is the plumbing the
pair request has to travel down, and all of it is tested.

---

## 1. `net/plist.c` — writing Apple property lists

Every message lockdownd reads or sends is an XML property list. Until
now the tree wrote them as string constants in C source:

```c
/* usb/lockdown.c, the QueryType request */
"<?xml version=\"1.0\" ...><dict><key>Request</key>..."
```

That is fine for a message whose contents never change. The pair
request does not qualify: it carries a HostID, a SystemBUID, a device
public key and **three certificates**, each several hundred DER bytes
expanded by base64 into hundreds of characters that have to be wrapped
and escaped. A format string cannot do that.

So there is now a streaming writer, shaped like `net/der.c` on purpose:
a buffer, a length, and an `ok` flag that never recovers, so the caller
checks once at the end instead of writing twenty `if`s and getting
nineteen of them right.

### What judges it

**Python's `plistlib`**, in `tests/plist_judge.py`. There is no plist
*reader* in HavenDOS, so this writer cannot be checked against one of
ours even in principle — and if there were one, a writer and a reader
built from the same misunderstanding would agree with each other
perfectly and be rejected by the phone. The judge answers two
questions with two different exit codes, because "lockdownd would
refuse this" and "the value came back wrong" are different findings.

93 checks. Among them: a string carrying `&` and `<` (every
certificate subject with a comma in it), base64 of **all 256 byte
values** (a table with one character out of place is a public key
wrong by one bit), each of the three padding cases separately, a
2048-bit modulus whose top and bottom bytes both survive, and a
buffer one byte too small — checked with a `0xAA` canary across the
rest of the array rather than by trusting the length field.

### Proved against 23 injected defects

Nineteen caught. The four that were not are written down at the end of
`net/plist.c` rather than deleted, because an injection that turns out
not to be a defect is a finding about the format:

1. **Dropping the `>` escape survives, and should.** A bare `>` is
   legal in XML character data; it only has to be escaped inside
   `]]>`. The escape stays to match Apple's own writer, which makes a
   diff against a known-good message readable. It is not correctness
   and no test pretends it is.
2. **Wrapping base64 mid-quad survives.** Decoders skip whitespace
   wherever it falls. 68 characters a line matches Apple; nothing more.
3. **Negating `INT32_MIN` in 32 bits survives** — it prints the same
   digits. The `int64_t` cast is there because the 32-bit negation is
   *undefined behaviour that currently works*. This is the one
   survivor that is a limit of testing rather than a property of the
   format, and the note exists so the cast is not "simplified" away.
4. **Indenting with spaces survives.** Whitespace is not semantic.

---

## 2. A request longer than one mux packet

`usbmux_send()` carries at most **2012 bytes**. `ld_send_plist()` used
to refuse anything bigger **by name**, and the refusal said so:

> `lockdown: a %d-byte request needs more than one packet (%d) -- not written yet`

It is written now. `usbmux_send_stream()` splits the request inside
the transport, where the sequence numbers and the device's window
live. TCP is a byte stream and lockdownd reads four bytes of length
and then that many, so where the packet boundaries fall is not its
concern.

`tests/test_plist.c` builds the real pair request at full size: it
comes to a little over five kilobytes and goes out in three packets.
`LD_REQ` is 12288, which is seven.

### Data that arrives while we are sending

The split sender reads between segments, to collect the device's ACKs
and the window they carry. A read can return **data** instead — and
those bytes have been acknowledged by the time they are in our hands,
so the device will never send them again. Dropping them loses the
first packet of a reply and the symptom is a plist that parses as
truncated.

So they are stashed, and the next `usbmux_recv()` returns them
**before anything newer**, because a byte stream that delivers them
second has reordered itself. One packet of stash is enough because the
harvest does not run while it is occupied: un-read bytes stay on the
device's side of the wire, where the protocol already knows how to
keep them.

### The window: this warns, it does not block

usbmuxd enforces the device's advertised window. This does not, and
the reason is specific rather than lazy: the window arrives in units
of 256 bytes and the device advertises **131072** of them, so a
ten-kilobyte request is nowhere near it and an enforcement path would
never run — on hardware or in a suite. An unexercised blocking path in
a transport is how a working transfer becomes a hang.

Instead the unacknowledged total is compared against the window before
every segment, and exceeding it is **counted and logged by name**,
then sent anyway. If a device ever does advertise a window this fills,
the next `usbmux` dump says so in one line, and *that* is the
measurement that would justify writing the blocking path.

### A partial send tears the connection down

The failure worth designing for. Once some segments are out, lockdownd
is part way through a length prefix: every later request on that
connection is read at the wrong offset and **nothing anywhere reports
an error**. There is no recovery short of dropping it, so that is what
happens, and the byte count says where it stopped.

A failure with **nothing** on the wire is the opposite case and is
handled as the opposite case: the stream is still aligned, the channel
is fine, and throwing it away would cost a working connection for no
reason.

---

## 3. What the tests needed before they had teeth

The split sender passed 57 new checks on its first run, which on its
own is the least trustworthy result a suite can produce. 20 defects
were injected into `usbmux_send_stream()`, `mux_harvest()` and the
stash branch of `usbmux_recv()`. **Three of them survived**, and
closing two of those required changing the fixture:

* **`tests/muxdev.inc` can now hold TWO packets in flight.** The guard
  that stops the harvest taking a second packet while the first is
  still in the stash cannot be exercised by a device that can only
  have one packet outstanding. With two slots, removing the guard
  loses five bytes and three checks fail.
* **The ACK *value* is now asserted, not its presence.** The device
  model rejects an acknowledgement that is too high and accepts one
  that is too low, so a harvest that stashed without acking was
  invisible to every other check in the suite — and on hardware the
  symptom would be a reply that arrives once and then silence.
* **The fixture accumulates the host's whole byte stream.** A segment
  sent twice with another dropped keeps the byte count right and the
  XML well-formed. It shows up in a byte-for-byte comparison and
  nowhere else. That check is the one the whole block exists for, and
  it is what catches `usbmux_send(p, chunk)` instead of
  `usbmux_send(p + off, chunk)`.

The three survivors are all **redundancy rather than gaps** and are
named in `usb/usbmux.c`: a `conn != 2` test that `usbmux_send()`
repeats one layer down, an RST branch that the flags check below it
also catches, and a `return` after a failed harvest that the next
segment's failure reaches anyway. Each stays, each for a stated
reason, none of them load-bearing.

### Two gaps in the lockdown layer, both the same shape

Eight defects injected into `ld_send_plist()`. Two survived until the
suite was given checks it did not have, and both were a condition
whose halves no input could tell apart:

* **`len > sizeof out` instead of `len + 4 > sizeof out`** accepts
  exactly four body lengths and copies up to four bytes past the end
  of a static array. Every oversize document the suite had was
  oversize by *more* than four, so both bounds refused it. Now checked
  at the byte: `LD_REQ-4` accepted, `LD_REQ-3` refused.
* **Tearing the connection down on `sent >= 0`** rather than
  `sent > 0` survived because every failure the suite could produce
  had already put bytes on the wire. The suite now stalls the first
  segment and the second separately and requires the opposite outcome
  from each.

---

## 4. Also in this release

* **`include/plist.h`** and **`net/plist.c`** are new; `net/plist.c`
  carries the tree's second base64 **encoder**, and says so. The first
  is `b64_encode_buf()`, `static` inside `shell/shell_v073.c` — three
  thousand lines of VGA drawing, so nothing can reach it, and it does
  not wrap. Merging them means moving code out of that file, which is
  a refactor and not this release. `usb/lockdown.c` already records
  the same situation for the two decoders.
* **`LD_REQ` (12288)** is a new bound, separate from `LD_BUF` (4096),
  because one is about a request and the other about a reply. The one
  time those were the same number it was by accident.

---

## Still not done, and why

* **Stage 3 piece four: the pair exchange itself.** The device public
  key is read (stage 2), certificates can be built and signed
  (v0.8.8.9, judged by openssl), the request can be composed
  (`net/plist.c`) and it can be sent (`usbmux_send_stream`). What is
  missing is the orchestration and then **TLS with client
  certificates** — `net/tls.c` is an HTTPS client with no client-cert
  path.

  **A correction, made the morning after this was first written:** an
  earlier draft of this file said a 2048-bit keygen costs 15–25
  minutes. That was the *superseded* estimate from v0.8.7.9, for the
  byte-wise modexp that existed then. `include/rsa.h` records the
  measurement that replaced it: Montgomery reduction took one
  1024-bit modexp from 2.3 s to **5.2 ms**, so a keygen is seconds in
  this container and a minute or two on the HP — and that last figure
  is an estimate, not a measurement, because it has never been timed
  on the HP. Quoting the old number was my error, not the tree's.
* **Trackpad.** Needs PCI resource assignment *and* an ACPI
  interpreter to find the I2C-HID descriptor address. Unchanged.
* **External monitor.** Needs an Intel Xe display driver. Unchanged.
* **USB mouse.** `usb/hid.c` exists and has never been confirmed on
  hardware. That is a test, not a coding task.
* **NVRAM boot entries.** HavenDOS still cannot create or remove a
  named UEFI boot entry, and the reason has not changed: `SetVariable()`
  is a UEFI *runtime service*, and a multiboot1 kernel never receives
  the EFI system table because GRUB calls `ExitBootServices` before
  handing over. `uninstall` removes our files and our partition; the
  firmware entry has to go by `bcdedit` or by the firmware's own menu.
