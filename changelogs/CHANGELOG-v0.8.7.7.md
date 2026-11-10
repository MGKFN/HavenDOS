# HavenDOS v0.8.7.7 — stage 1: lockdownd

Channel Beta, codename "Iris". 30 October 2026.

**Stage 1 of the pairing road: a TCP connection over the usbmux channel,
and `QueryType` to lockdownd on port 62078.** Stage 0 was confirmed on
the HP on 26 October — the phone answered a VERSION packet with twenty
bytes, the first thing it has ever sent back over any channel. This is
the next yes-or-no the phone itself gives.

`docs/LOCKDOWN-HARDWARE-TEST.md` is the five minutes that settle it, and
it is what to read at the laptop rather than this file.

---

## The protocol is no longer a guess

0.8.7.3 shipped a sixteen-byte mux header because it **reasoned about**
the protocol. The phone has never used one, and the driver reported
"magic 2, expected FEEDFACE" — the 2 being the phone saying it speaks
usbmux 2.0.

So every field added in this release was read out of the two
implementations known to work against a real device, not derived:

| what | where it came from |
|---|---|
| the mux header and its sequence rules | usbmuxd `src/device.c`, `send_packet()` |
| the TCP header, flags, window scaling | usbmuxd `src/device.c`, `send_tcp()` |
| the handshake and what a receiver may believe | usbmuxd `src/device.c`, `device_tcp_input()` |
| the interface triple FF/FE/02 | usbmuxd `src/usb.h` — and it matches |
| the plist length framing | libimobiledevice `common/property_list_service.c` |
| `QueryType` and `com.apple.mobile.lockdown` | libimobiledevice `src/lockdown.c` |
| port 62078 | libimobiledevice `src/lockdown.c`, `.port = 0xf27e` |

Where this driver **deliberately differs**, the difference is marked and
argued in the source rather than left to look like an improvement.
There are exactly two, and both are below.

---

## THREE SEQUENCE-NUMBER RULES, ALL THREE WRONG BEFORE THIS

The mux header's `tx_seq` and `rx_seq` read like counters and are not
one. 0.8.7.6 had every rule wrong:

- **`tx_seq` advances only on a packet that CARRIES it.** The VERSION
  packet goes out on the eight-byte header and has no such field, so it
  must not move the counter. 0.8.7.6 advanced unconditionally.
- **The SETUP packet RESETS the pair** — `tx_seq` to 0 and `rx_seq` to
  0xFFFF — and writes those values before advancing. 0xFFFF means
  "nothing received from you yet", not 65535.
- **`rx_seq` is not a tally.** It is the `rx_seq` field of the last
  packet the *device* sent, mirrored straight back. usbmuxd's
  `device_data_input()` does exactly one assignment and nothing else.

0.8.7.6 therefore sent `tx 1 rx 1` where usbmuxd sends `tx 0 rx FFFF`.

**Why nothing caught it.** The only sequenced packet the kernel had ever
sent was one SETUP, and at that moment the two wrong rules happened to
produce the same number for both fields. `tests/test_usbmux.c` asserted
`tx_seq == 1` and `rx_seq == 1` and the driver obliged; the file even
recorded, in so many words, that it could not tell the two fields apart
and that the check belonged at stage 1.

It belongs here now, and not because stage 1 sends more traffic. **The
two values are no longer the same number**, so a swap is caught on the
first packet — and `tools/check_usbmux_suite.py`'s swap injection, which
0.8.7.6 recorded as unreachable and promised to bring back, is back and
caught.

`usb mux` now prints the pair, because all three rules were wrong and
nothing on screen would have shown it.

---

## The connection layer

`usb/usbmux.c` grows a single TCP connection: a standard twenty-byte
BSD `tcphdr` under protocol 6, no checksum (the mux is the reliable
layer and usbmuxd never fills the field), header offset 5, and a
**window in units of 256 bytes** — `>> 8` out, `<< 8` in. A window
field that looks 256 times too small is correct.

`usbmux_connect()` runs SYN / SYN-ACK / ACK, `usbmux_send()` and
`usbmux_recv()` carry the byte stream, every arriving payload is
acknowledged at once because usbmuxd's own comment says the device
prefers it, and `usbmux_disconnect()` closes with an RST because this
protocol has no graceful shutdown.

### The two deliberate departures from usbmuxd

**The window is what this kernel can actually take.** usbmuxd advertises
131072 bytes because it has a 262144-byte buffer behind it. `MUX_BUF` is
2048, so a 128 KB window would invite exactly the overflow the
advertisement exists to prevent. `MUX_WINDOW` is **derived** from
`MUX_BUF` in one place and re-derived independently in the suite — a
bound written as a literal twice is the shape of 0.8.7's transfer-size
bug.

**The device's SYN is acknowledged at ISN + 1.** usbmuxd does
`tx_ack++` from zero, which produces the right answer only because the
device's initial sequence number has always been 0. That is an
assumption, and as written it is unfalsifiable: a different ISN would
make usbmuxd acknowledge the wrong byte and the symptom would be a
conversation that stalls for no visible reason. Acking the SYN the
device actually sent is byte-identical for ISN 0, so nothing is risked,
and correct where usbmuxd is wrong. Because nobody has seen that case,
it is **logged** — a deviation nobody is told about is one nobody can
check.

### Named rather than discovered

- **One connection at a time.** usbmuxd keeps a collection; a table with
  one row in it is the fixture-that-cannot-distinguish-anything mistake
  moved into the driver.
- **No reassembly.** A mux packet arriving split across two USB
  transfers is **refused and named**, with both numbers, because "the
  reply was too big to read" and "the framing is wrong" have different
  fixes. usbmuxd reassembles into 64 KB; the xHCI back-end bounces every
  transfer through 2048, so a buffer above that would move the
  truncation rather than remove it.

---

## usb/lockdown.c — the plist conversation

A separate file, because stage 3 brings a plist writer, RSA key
generation and an X.509 certificate and none of that belongs in a USB
transport. usbmuxd and libimobiledevice split along exactly this line.

**The length prefix does not count itself.** The mux header four bytes
below it in the same byte stream does. Getting that wrong by four leaves
lockdownd waiting for a request that never finishes, and the symptom is
a device that does not answer — which is the one symptom this whole
release line exists to tell apart from the others.

**The reply arrives in two writes.** `property_list_service.c` sends the
four length bytes and then the XML as separate `service_send()` calls,
so they arrive as separate TCP packets. A reader written for one packet
gets four bytes and concludes the reply is empty. `usb/lockdown.c`
accumulates, and `usb lockdown` prints how many packets it took, because
that is the line which confirms the accumulation was needed at all.

**There is no XML parser and there is not going to be one for a string.**
A bounded scanner finds `<key>Type</key>` and reads the `<string>` after
it. A scanner that knows it is a scanner cannot silently misread a
structure it was never looking at.

---

## A real bug the harness found

`tools/check_usbmux_suite.py` reported "tx_seq advances on the VERSION
packet too" as MISSED. It is genuinely unreachable — the SETUP's reset
overwrites whatever the VERSION did — and establishing *why* meant
asking what happens when the SETUP does not send.

**It was never sent again.** The VERSION is answered, the version becomes
2, the header becomes sixteen bytes — and then the SETUP fails. The host
believes the version is agreed; the device never got its setup byte.
`usbmux_handshake()` returned the known version unconditionally on every
later call, so nothing retried: a channel permanently out of step,
reporting itself healthy.

The setup is **owed** until it gets out. `usb mux` run twice is the path
that reaches it.

That is the argument for running a harness even when every result in it
is going to be a shrug.

---

## Testing

**3213 checks across 40 suites**, up from 2961 across 39.

| suite | checks | |
|---|---|---|
| `test_usbmux` | 216 | was 87 — the TCP layer, and the sequence rules |
| `test_lockdown` | 123 | new — stage 1 over the real mux and its TCP |

**`tests/muxdev.inc` is now the only model of a phone in the tree.** Both
suites include it. Two copies of a device model drift apart, which is
what `tools/check_diskname_sync.py` exists to refuse and what this tree
has been bitten by twice; `tests/blkbus.inc` is the same arrangement one
layer down.

And the device in it is **not a parrot**. It validates what arrives the
way usbmuxd does — a length field that does not match the transfer, a
missing magic, a header that is not five words, ports that are not this
connection's, **and a sequence number it was not expecting** — and
records *why* it refused, so a test can assert on the reason rather than
on silence. That last check is the reason it exists: the sequence
arithmetic is the part of stage 1 with no screen of its own, and a
device that shrugs at wrong arithmetic cannot tell a correct driver from
one that merely sends plausible-looking packets.

It is deliberately **not** stricter than the real thing in one place: a
zero TCP checksum is *counted*, not refused, because usbmuxd never fills
that field and the device accepts it. A fixture stricter than the device
fails correct drivers.

### Two injection harnesses

```
python3 tools/check_usbmux_suite.py      65 of 65, harmless correctly missed
python3 tools/check_lockdown_suite.py    32 of 32, harmless correctly missed
```

**Both suites passed on their first run, and both were full of holes.**
`test_usbmux` lost 5 injections and `test_lockdown` lost 11. The twenty
that survived say more about the suites than the 339 checks did.

Three of the five, and the most useful finding in the release, shared one
shape: **a refusal that is arithmetic rather than a named error code.**
Remove the length-field guard and `mux_recv()` returns `len - hdr`, which
for a length of 4 and a header of 8 is **-4 — USB_ENODEV, "no
controller, or nothing on the port."** A length field four bytes long
would have sent the next person to check whether the phone was plugged
in. A twenty-byte header read out of an eighteen-byte packet returns -2,
USB_ESTALL, "the device returned STALL" — it did no such thing. Every
such test now asserts the **exact** code. It is `x_control()` returning
(3) for six different causes, in the one shape a return value can take
it.

The rest of the twenty were the shapes this tree already has names for:

- **a fixture value that cannot distinguish anything** — "connecting
  twice does not reconnect" compared `tcp_state` before and after, and
  it is 2 either way. The device counts SYNs now.
- **a subject masked by a guard somewhere else** — the short-TCP-header
  test left the ports zeroed, so the port check refused the packet
  before the length check was reached. Everything else about the packet
  is correct now, so only the thing under test is wrong.
- **a property that passed by luck** — the scanner's anchoring. In the
  reply used, the first "Type" sits inside "QueryType" and the next
  `<string>` after it happens to be the right one. `REPLY_TYPEISH`
  cannot be passed by luck: its first "Type" is inside a key *name*,
  `ProtocolType`, whose value is "2".
- **a subject that never came under load** — "progress resets the poll
  budget" could not fail, because the fixture delivered the length
  prefix instantly and there was effectively one wait rather than two.
  All packet delivery goes through one place now, and the slow-device
  knob applies to the first packet too.
- **an injection that was not a defect** — "the connected flag is set
  whatever the connection did" only disabled a log line. Rewritten to
  take the failure branch out.

### Recorded, not deleted

Four injections that cannot fire, each with the reason, following
v0.8.6.3's precedent — an injection deleted without a word is one
somebody writes again:

- **"tx_seq advances on the VERSION packet too"** — masked by the SETUP
  reset. Chasing it found the owed-SETUP bug above.
- **"the connection is marked up before the handshake completes"** —
  nothing can run between the SYN and the ACK while the handshake is
  synchronous. **Reachable at 0.8.8**, whose whole subject is
  `kernel_poll()` running work during exactly this kind of wait.
  Chasing it also found a comment claiming a state-machine check that
  does not exist; corrected.
- **"a packet longer than the buffer is built anyway"** — 0.8.7.6
  recorded this as unreachable because nothing came near the bound.
  Lockdownd messages exist now and it is *still* unreachable, for a new
  reason: `usbmux_send()` refuses more than `MUX_MAX_PAYLOAD` first. The
  note says so rather than looking as though nothing was checked.
- **"a reply with no Type is reported as a success"** — redundant with
  the `strcmp` below it. What the guard buys is the message, and a log
  line is not nothing — 0.8.7 lost a release to a format string in the
  one diagnostic it depended on — but it is not behaviour either.

---

## AND THE ISO BUILDS AGAIN

0.8.7.6's handoff recorded that its container had no grub, xorriso or
qemu, so `make` had never been run end to end and the QEMU harnesses had
not been run at all. This one has all three.

- **`make` completes**, all fifteen build checks included.
- **`make test` completes**: 3213 of 3213.
- **The ISO is built and `check_iso_bootable` passes** — BIOS and UEFI,
  13.5 MB.
- **`fsck.vfat`, `mtools`, `sgdisk` and `parted` are present**, so the
  filesystem suites are judged by tools outside this tree again, which
  is the discipline `test_fat32` and `test_gptwrite` exist under.

### harness/lockdown_boot.py — and it found something

**The kernel boots with `usb/lockdown.c` in it and `usb lockdown`
dispatches.** 12 of 12. QEMU has no iPhone so it cannot settle stage 1,
but it settles things no suite can: that the command dispatches rather
than falling through to the plain bus listing — 0.8.6.6's lesson, where
`usb cfg` on a kernel too old to have it printed a normal screen and
looked like success — that it is documented on `usb`'s own usage list,
and that with no device on the bus it **refuses in six seconds** rather
than spending its twenty-thousand-poll budget, which at the keyboard is
indistinguishable from a wedge.

**And the first version of the command got its own screen wrong.** With
no iPhone at all it printed the generic failure, which sends the reader
to doubt the TCP framing when the framing was never reached. Three
causes behind one screen is `x_control()`'s mistake in the shell, where
it costs a trip to the laptop. It now says which of the three it was.

Found by booting it. Which is the argument for booting it.

---

## Files

New: `usb/lockdown.c`, `include/lockdown.h`, `tests/test_lockdown.c`,
`tests/muxdev.inc`, `tools/check_lockdown_suite.py`,
`harness/lockdown_boot.py`, `docs/LOCKDOWN-HARDWARE-TEST.md`.

Changed: `usb/usbmux.c`, `include/usbmux.h`, `shell/usb.c`,
`shell/help.c`, `tests/test_usbmux.c`, `tools/check_usbmux_suite.py`,
`Makefile`, `include/version.h`.

## What this release does NOT answer

- **Whether lockdownd answers QueryType.** One boot on the HP.
  `docs/LOCKDOWN-HARDWARE-TEST.md`.
- **Whether an Ethernet frame crosses.** Still never seen. Stage 4.
- Everything else in HANDOFF.md's open list, unchanged.
