# HavenDOS v0.8.7.9 — stage 2: GetValue DevicePublicKey

Channel Beta, codename "Iris". 31 October 2026.

**Stage 1 closed on the HP.** lockdownd answered `QueryType` with
`com.apple.mobile.lockdown` — the first time this kernel has talked to
a *service* on the phone rather than to its mux.

This release adds **stage 2**: `GetValue` for `DevicePublicKey`, on the
same connection, still with no cryptography. `usb lockdown` runs both.

---

## What the HP settled

```
ok  usbmux: connected to port 62078, seq 1 ack 1
ok  lockdown: connected to port 62078
    lockdown: QueryType sent, 285 bytes
ok  lockdown: LOCKDOWND ANSWERED -- com.apple.mobile.lockdown
    lockdown  18516->62078  seq 286 ack 302  their window 131072
            requests 1  replies 1
            reply: 301 bytes in 1 packets
            length prefix: 00 00 01 29 = 297
ok          Type:  com.apple.mobile.lockdown
```

Four decisions confirmed that no suite could settle:

- **The length prefix does not count itself.** `297` declared against a
  `301`-byte reply. The mux header four bytes below it in the same byte
  stream does count itself; these two being opposite is the detail the
  whole plist layer was built around.
- **`seq 286 ack 302`** — 1+285 and 1+301. The ack arithmetic is right,
  and so is acking the device's SYN at ISN+1 rather than usbmuxd's
  `tx_ack++`.
- **`their window 131072`** — the `<<8` on the way in is right.
- **`errs 0`** throughout.

### And one correction to v0.8.7.7

**`reply: 301 bytes in 1 packets`.**

`usb/lockdown.c` accumulates across packets because
libimobiledevice's `internal_plist_send()` writes the length and the
body as two separate `service_send()` calls. **The device does not.**
It sends them together, and a reader written for one packet would have
worked here.

The accumulator stays — it handles both, the one-packet case was
already tested as the deliberate contrast, and a reply larger than the
window will arrive in pieces regardless. But the stated *reason* for it
was about upstream's **host** side and did not transfer to the device,
which is why the hardware test plan asked for this number.

---

## Stage 2

`GetValue` differs from `QueryType` in the one way that matters: the
value comes back as **`<data>`, not `<string>`** — base64, wrapped
across lines with a tab on each. Stage 1's scanner reads `<string>` and
would find nothing at all.

Decoded, it is a PEM RSA public key, so there are **two layers of
base64** between the wire and the modulus. Stage 2 unwraps the first
and stops. Stage 3 unwraps the second, parses the DER, and is where the
cryptography starts.

`usb lockdown` now runs stage 1 and then stage 2 on the **same
connection** — lockdownd answers several requests on one, and a second
connection is a second chance for the thing under test to be the
transport rather than the request.

### The base64 decoder, and why it is a second one

There is already a base64 decoder in this tree, in
`shell/shell_v073.c`, and it could not be used:

- it is **`static` inside a three-thousand-line shell file**, so
  nothing outside that file can reach it — the same shape as the disk
  table v0.8.6 had to move out of `shell/iso_installer.c` before
  `tests/test_diskname.c` could link it;
- it walks a **NUL-terminated string and stops at the first character
  outside the alphabet.** A `<data>` element has newlines and tabs
  *inside* it, so it would return the first line — about a sixth of the
  key, still beginning `-----BEGIN RSA PUBLIC KEY-----`, and therefore
  still looking like a key.

The new one is length-bounded and skips whitespace. **Two base64
decoders in one tree is a smell**; it is recorded in HANDOFF.md rather
than left to be discovered, and merging them means moving the shell's
out of a file full of VGA drawing, which is a refactor and not this
release.

It **refuses rather than guessing**: a character that is neither
whitespace nor base64, anything after the padding, or a group cut
short. Skipping what it did not recognise would turn a wrong guess
about the framing into a **shorter key that still looks like a key**,
and stage 3 would sign something with it.

---

## Testing

**3299 / 3299 across 40 suites** (3228 at 0.8.7.8). `test_lockdown`
123 → 194.

| harness | |
|---|---|
| `check_lockdown_suite.py` | **50 of 50** (was 32), harmless correctly missed |
| `check_usbmux_suite.py` | 66 of 66, unchanged |
| `harness/lockdown_boot.py` | 12 of 12 |

The suite passed on its first run with 176 checks and the harness then
**lost three injections and found five stale ones**. Both halves are
worth recording.

### The five stale ones were caused by the new code

`lockdown_get_value()` builds its request rather than using a fixed
string, so `<key>Label</key>`, `<string>HavenDOS</string>`,
`<key>Request</key>` and the not-connected guard each now appear
**twice** in the file. Four injections that had been unique started
matching two places, and `tools/inject.py` reported them as stale
rather than counting them as passes — which is the only reason they
were not silently lost. Each is anchored to its own request now.

**This is the third release in which that mechanism has earned its
place.** An injection is a pointer into source that moves.

### The three misses were the shapes this tree has names for

- **A property that passed by luck.** `REPLY_GETVALUE` has exactly one
  `<data>` in it, so an unanchored search finds the right one anyway.
  `REPLY_GETVALUE_TWODATA` puts an `EscrowBag` `<data>` ahead of the
  Value — a real lockdownd key, and binary, so the shape is plausible
  as well as useful.
- **A guard whose effect was already there.** The terminator after the
  decoded bytes is invisible until a **shorter** value follows a longer
  one, because the buffer is otherwise still zeroed. Without it,
  `lockdown_value()` runs into the tail of the previous key while the
  length beside it says 3.
- **A precedence rule nothing exercised.** An `Error` reply that also
  carries a `Value`. lockdownd does not send one, and the rule is worth
  pinning anyway: if the device said no, no key is reported whatever
  else is in the document — otherwise stage 3 signs a pair record with
  something out of a refused reply.

The two constructed replies are labelled in the fixture as shapes
chosen to make a property testable, not as transcripts of something a
phone sent.

---

## Files

Changed: `usb/lockdown.c`, `include/lockdown.h`, `shell/usb.c`,
`tests/test_lockdown.c`, `tools/check_lockdown_suite.py`, `Makefile`,
`include/version.h`, `docs/LOCKDOWN-HARDWARE-TEST.md`.

## Next

**Stage 3 is not one more request.** It needs a plist writer (the pair
request carries certificates and is several kilobytes — more than one
mux packet and more than `LD_BUF`, and `ld_send_plist()` refuses it by
name today), RSA key generation, a root certificate, a host
certificate, a device certificate signed by the root, and a second
base64 decode to get the DER out of the PEM stage 2 returns.

Say so before starting it.
