# HavenDOS v0.8.8.11 "Helios beta 12" — the trackpad, and the message that asks for trust

Channel Beta, codename "Helios", **beta 12**. 8 November 2026.

58 suites, **5641/5641** (up from 53 and 5271).

Two things you can see, and one correction to something I told you
wrongly.

---

## 1. The trackpad — and the ACPI blocker turned out to be dodgeable

`include/i2c.h` has said since 0.8.8.3 why the HP's trackpad was out
of reach:

> A device on an I2C bus has no enumeration. There is no descriptor to
> ask for and no vendor ID to read: you must already know the
> seven-bit address. On a PC that address lives in ACPI, in the
> device's `_CRS`, and reaching it means an AML interpreter.

Half of that was already solved there: `i2c_scan()` asks the bus which
addresses answer, which is a question a bus can be asked directly.

I2C-HID needs one more number ACPI normally supplies — the register
where the HID descriptor lives. In practice there are about four
values in use, 0x01 on nearly everything and 0x20 on some Elan parts.

**Guessing would be unacceptable. This is not a guess, because the
descriptor validates itself.** An HID descriptor's first four bytes
are `wHIDDescLength` and `bcdVersion`, and for every conforming device
they are `1E 00` and `00 01` — thirty bytes long, version 1.00. Then
twelve further fields have to be internally consistent and four
reserved bytes have to be zero.

So: for every address that answers, try the handful of known
registers, and accept the one where what comes back **is** a
descriptor. That is a probe with a checkable signature. No AML
interpreter, no DSDT walk, no RSDP.

### `usb/hid_rd.c` — the report descriptor parser

I2C-HID has **no boot protocol**. A trackpad describes itself and only
itself, so without a descriptor parser there is no way to know which
bits of a report are X — however well `drivers/i2c.c` talks to it.

Judged against **`hid_parser`**, an independent Python implementation
of the same grammar. 28 injected defects, all 28 caught — but the
first run caught only 21, with the suite passing 70 of 70 checks, and
all seven survivors survived because no descriptor in the suite could
tell the right behaviour from the wrong one. A four-byte Usage was
only ever tested where the page in force was already the usage's own
page; every descriptor declared as many usages as its report count;
every descriptor was balanced. Seven fixtures later, all 28.

This also unblocks the subclass-0 USB keyboards `usb/hid.c` has never
been able to drive, which its own header flagged as the cost of boot
protocol.

**Three places the external judge and this parser disagree**, all
resolved by reading the specification rather than preferring our own
answer, and all three written down in `tests/test_hid_rd.c` so the
claim can be checked instead of taken:

1. A **four-byte Usage** carries its page in the high sixteen bits
   (HID 1.11 §6.2.2.8). `hid_parser` keeps the page bits in the usage
   and reports 0x10030, so it would never match usage 0x30 — no X on
   any device using the extended form.
2. **The last declared usage repeats** for remaining controls, which
   is what Linux's `hid-core` does (`usage[min(j, n-1)]`).
   `hid_parser` emits one field and pads the rest, which would lose
   the second axis of any mouse declaring X and Y with a count of 2.
3. `hid_parser` raises `NotImplementedError` on **Push and Pop**.

### `drivers/i2c_hid.c` — the driver

29 injected defects, 26 caught; the four that mattered all survived
because of blind fixtures (the decoy report carried the same
coordinates as the real one; every lift cleared the contact count as
well as the tip switch, so the tip switch was never tested alone).

**One injection changed the code rather than the suite.** Removing the
`break` after a register validates survived, and made visible that the
inner loop's bound depended on that break three lines below it.
`found < max` is now on both loops: a bound that relies on a break
somewhere else is not a bound.

### Why absolute coordinates become deltas

A touchpad reports where the finger **is**. A pointer moves by how far
the finger **moved**. Mapping absolute position onto the screen is
what a drawing tablet does, and on a laptop it means lifting and
replacing a finger teleports the cursor.

The check this is all for, and the one you would notice within a
second of using it:

> **lift the finger, put it down 2900 units away, and the pointer does
> not move.**

### `i2chid` — one screen, photographable

If it found your trackpad: absolute or relative, how many buttons,
multitouch or not, which report id. If it found nothing it **rescans
and lists every address that did answer**, because "the bus works and
we guessed the register wrong" and "the bus is dead" are completely
different problems with completely different next steps.

---

## 2. The USB mouse: a working driver and a consumer that hung on it

You asked to finish the USB mouse. `usb/hid.c` was already there and
already right. What was wrong was downstream.

**Right-clicking in Notepad froze the machine.**

```c
if(mb&2){                                          /* right button down */
    do { mouse_get(&mx,&my,&mb); } while(mb&2);    /* wait for release  */
```

PS/2 *pushes*: a packet lands at port 0x60 and the cached position
updates on its own. USB has to be *asked*, and `usb_hid_poll()` was
called from exactly two places — `drivers/keyboard.c`'s two read
paths. That loop reads no keyboard, so nothing polled USB, so the
button never came up, so the loop never ended. It reads no keys
either, so there was no way out.

It was reachable precisely because `mouse_is_ready()` correctly
returns true for a USB-only pointer.

Two fixes, because fixing the one loop would have left the next one to
be written:

* **`mouse_get()` and `mouse_get_clicks()` now drain USB** (and I2C).
  "Where is the pointer" now means the same thing on both buses, which
  is what every caller already assumed. Reentrancy guarded: a
  `mouse_get()` reached from inside a USB transfer would recurse
  through the host controller, and nothing in a polled design survives
  that.
* **The loop is bounded anyway** — two seconds, and ESC gets out. A
  mouse unplugged with its button held leaves a button stuck down.

A whole class of consumer — anything that reads clicks but not keys,
like `shell/chat.c` — never saw USB input at all.

`drivers/mouse.c`'s accumulator, acceleration, clamp and button edge
are now **one copy** in `mouse_apply()`, reached by all three sources.
That file's own header says why: *"a second copy of that arithmetic is
a second place for the v0.7.17 acceleration-precedence bug to live."*
A third source would have been a third place.

---

## 3. Pairing stage 3, piece four: the request

`pair` now builds and sends the message that makes an iPhone put
**"Trust this computer?"** on screen.

Pairing is not a password exchange. The host invents a small
certificate authority of its own and asks the device to remember it:

| | |
|---|---|
| `RootCertificate` | self-signed, CA:TRUE. The authority. |
| `HostCertificate` | our key, signed by the root. |
| `DeviceCertificate` | **the device's own public key**, signed by our root. |

The third one surprises people. The host certifies the device to
itself, so that in the TLS session afterwards both ends are vouched
for by the same authority. We do not have the device's private key and
do not need it: a certificate is a signature over a *public* key.

### The judge is `openssl verify`

```
openssl verify -no_check_time -CAfile root.pem host.pem
```

Real signature verification by an independent implementation — the
same check the device performs on the record it is being asked to
remember. Plus a **negative control**: a separate CA built from its own
key must *not* verify our certificates, because without it "verify
succeeded" could mean openssl was not checking signatures at all.

And `openssl x509 -modulus` on the device certificate must print the
**device's** modulus. Certifying our own key three times would pass
every structural check and fail on the phone.

27 injected defects, all 27 caught. The three that needed better
fixtures are named at the end of `usb/pair.c` — one of them was the
negative control itself, whose first version overwrote the host key
and quietly broke the round-trip checks downstream.

### What is still missing

* **The pair record is not saved.** A reboot loses it and the phone
  asks again. Where to put it is a filesystem question for the next
  release, and it is noted rather than half-done.
* **`StartSession` and TLS with client certificates** — the second
  half of piece four. `net/tls.c` is an HTTPS client with no
  client-certificate path.

So what this delivers is the request and the reply: the phone shows
the dialog, you tap Trust, and the reply says in words what happened —
`PairingDialogResponsePending`, `UserDeniedPairing`,
`PasswordProtected`, `InvalidHostID`. `ExtendedPairingErrors` is asked
for, which is the difference between those names and a bare
"PairingFailed".

---

## 4. Four base64 implementations became one, and two of them could not refuse

Stage 3 needs the base64 **decoder** from `net/` — the device's public
key is base64 inside a plist value and PEM inside that. A third copy
was the alternative to consolidating.

There were four. Two of them were in `shell/shell_v073.c` and
**neither took the size of its output buffer**:

```c
static void b64_encode_buf(const uint8_t *in, int inlen, char *out);
static int  b64_decode_buf(const char *in, uint8_t *out);
```

`cmd_base64_v3()` called them with `static char out[512]`, and an
encode writes four characters per three input bytes plus a terminator
— so 384 characters of argument writes 513 bytes. Whether a shell
argument could ever reach 384 was never the point: the functions could
not refuse, so the bound was a property of the caller rather than of
the code, and the next caller would not have known.

`net/b64.c` takes a capacity and returns -1. 19 injected defects, all
19 caught — the last two only after the suite learned to feed it a
byte with the top bit set (the decode table has 128 entries; 0xFF
indexes twelve past the end of it) and four padding characters on a
quad boundary.

**One correction I had to make to my own work.** The first draft was
stricter than `usb/lockdown.c`'s decoder — it required a whole number
of quads and zero leftover bits — and it broke four checks in
`tests/test_lockdown.c`. Those checks exist because an *earlier*
version of that suite made the same mistake in the same direction, and
the comment there records it. Unpadded base64 is common and a two- or
three-character tail is unambiguous. Being strict about something
base64 actually permits is not rigour.

---

## 5. `net/der_read.c` — reading DER

`net/der.c` writes DER. This reads enough of it to get an RSA public
key out of `DevicePublicKey`, in either encoding, **deciding which by
the structure rather than by the PEM label** — lockdownd has used both
"BEGIN RSA PUBLIC KEY" and "BEGIN PUBLIC KEY" depending on the iOS
version, and the structure never lies.

Judged by openssl, under ASAN. Two sweeps carry most of the weight:
every one of the 294 prefixes of a real key must be refused, and every
byte corrupted in turn must yield either a refusal or a **different
key**.

**The bit-flip sweep found a real bug in one line.** Corrupting byte 6
of a real key — the algorithm OID's own tag — produced the *same
modulus*, because this file used to skip the AlgorithmIdentifier with
a comment claiming that checking it would need an OID table. It needs
one `memcmp` against nine bytes.

22 injections, 19 caught. The **first run caught ten**, with the suite
passing 64 of 64 checks and looking thorough. Three rounds of fixing
the *suite*:

1. Every refusal fixture was a toy — `SEQUENCE { INTEGER 1, INTEGER 1 }`
   with one rule broken, refused by the minimum-key-size check no
   matter what the length rules did. `tests/der_mutants.py` now takes a
   real openssl key apart and puts it back with exactly one thing
   wrong.
2. Every fixture had 4096 bytes of legal memory behind it, so a read
   two bytes past a 294-byte key was inside a static array and neither
   the suite nor ASAN saw it. Every fixture now goes through a
   `malloc` of exactly its own length.
3. Two guards were unreachable through the public API. An unreachable
   guard is either tested or deleted.

---

## 6. A correction to something I told you last night

I said a 2048-bit keygen runs **15–25 minutes** and that pairing would
need a cached key or take a coffee break.

That was the **superseded** estimate from 0.8.7.9, for the byte-wise
modexp that existed then. `include/rsa.h` records the measurement that
replaced it:

```
one 1024-bit modexp (= one Miller-Rabin round)   5.2 ms   (was 2.3 s)
one 2048-bit private-key operation              28.3 ms
one 2048-bit public operation, e = 65537         0.81 ms
```

Montgomery reduction bought three orders of magnitude. A keygen is
**seconds** in a virtual machine and a minute or two on the HP — and
that last part is an estimate, not a measurement, because nobody has
timed it there. `pair` prints the elapsed time for each key, so that
is where to find out.

---

## 7. The version number, and a trap worth knowing

This was built as **0.8.9** and renumbered to **0.8.8.10**, then 11,
at your request, to keep Helios inside one version. `ver_cmp()`
compares component by component:

```
0.8.8.11 vs 0.8.8.10  ->  1    the update IS offered
0.8.8.11 vs 0.8.9     -> -1    a DOWNGRADE
```

**A machine that ran `update` to the one 0.8.9 bundle will not be
offered anything in the 0.8.8.x series again** and has to be
reinstalled from an ISO. Nothing else shipped as 0.8.9.

---

## Still not done, and why

* **Stage 3's second half.** `StartSession` then TLS with client
  certificates. `net/tls.c` has no client-certificate path: it needs
  the Certificate message, ClientKeyExchange and CertificateVerify,
  and CertificateVerify means signing the handshake transcript with
  our own key — which `pkcs1_sign_sha256()` can already do.
* **Saving the pair record.** Without it every boot re-pairs.
* **External monitor.** Needs an Intel Xe display driver. Unchanged.
* **NVRAM boot entries.** Still impossible from HavenDOS, unchanged
  reason: `SetVariable()` is a UEFI *runtime service* and a multiboot1
  kernel never receives the EFI system table, because GRUB calls
  `ExitBootServices` before handing over. `uninstall` removes our
  files and our partition; a firmware entry goes by `bcdedit` or the
  firmware's own menu.
* **Multitouch gestures.** The contact count and tip switch are read
  and one finger moves the pointer. Two-finger scrolling is a thing
  this could now do and does not.
* **No GPIO, so no interrupt for the trackpad.** It polls. The device
  holds its most recent report, so what a missed poll loses is
  intermediate motion, not the finger's position.
