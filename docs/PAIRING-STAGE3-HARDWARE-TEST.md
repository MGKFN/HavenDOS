# Pairing, stage 3, end to end — the hardware test for v0.8.9

Fifteen minutes on the HP, an iPhone, and a camera.

**This doc covers BOTH HALVES of stage 3.** The first half shipped in
0.8.8.11 and its instructions were in
`docs/TRACKPAD-AND-PAIR-HARDWARE-TEST.md`; if you never ran that test,
start here instead — Part 2 below is the same thing, and Parts 3 to 5
are the half that is new. If you did run it, you can skim Part 2 and
spend the time on Part 4, which is the one nobody has ever seen work.

Nothing in Part 4 has run on hardware. Every byte of the TLS
client-certificate path is tested against `openssl s_server -Verify`,
which makes the same decision the phone makes — and that proves the
code does what the specification says and proves nothing at all about
what lockdownd on your particular iOS build actually accepts.

---

## What is being tested, in one paragraph

Pairing makes the phone *remember* this computer. A **session** is what
the host does with that afterwards, and lockdownd refuses nearly every
interesting request — including the one that turns tethering on —
outside one. It will not open a session for a host it does not already
trust, and it will not run the session in the clear: it answers
`StartSession` with `EnableSessionSSL`, and then expects a TLS
handshake in which **the phone is the server and we are a client
presenting a client certificate**. That certificate is the host
certificate out of the pair record. So the chain is: pair → save the
record → StartSession → TLS with our certificate → a request that
works. Part 4 is the first time the last three of those have existed.

---

## Before you start

- Boot the **0.8.9 ISO**, or `update` an installed 0.8.8.11 to it.
- **Install it, if you want Part 5 to mean anything.** The pair record
  is written to the system volume; a live ISO session has nowhere to
  put it and `pair` says so rather than pretending.
- The phone must be **unlocked** — awake, passcode entered, on the
  home screen. A locked phone answers `PasswordProtected`, which is a
  correct refusal.
- Have a camera. Every part below says what to photograph.

---

## Part 1 — is anything there at all, one minute

```
usb
```

The iPhone should be listed. Then:

```
usb lockdown
dmesg
```

**`dmesg` is not optional here.** `usb lockdown` prints its verdict on
screen and writes its EVIDENCE -- byte counts, the step each request
reached, the raw reply -- to the kernel log, and `kernel/main.c` turns
the log's screen echo off before the shell starts. So the gap between
"Sending QueryType..." and the verdict is blank, and the half worth
photographing is in `dmesg`.

This is stages 1 and 2 and has worked since 0.8.7.9. **If it fails
here, stop.** Pairing cannot work and the problem is not pairing —
`usb mux` and `usb -vv` are the next screens, not anything below.

---

## Part 2 — the pair request (the FIRST half, from 0.8.8.11)

```
pair
```

It narrates every step, and the useful information is which step it
reached. **Photograph the whole screen.**

```
  connecting to lockdownd...
  asking for DevicePublicKey...
  the device's key is 2048 bits.
  generating the root key (2048-bit)...
  root key: done in 4120 ms
  generating the host key (2048-bit)...
  host key: done in 3980 ms
  building the certificates...
  root 712, host 712, device 701 bytes.
  HostID C1A2B3C4-D5E6-47F8-9A0B-1C2D3E4F5A6B
  the request is 3705 bytes, 2 mux packets.
  sending it...

  the device is asking the user -- tap Trust
```

**The two "done in" numbers are a measurement nobody has taken.** The
keygen has only ever been timed in a container. A correction worth
knowing, because the wrong figure is in older notes: the 15–25 minute
estimate was for the *superseded* byte-wise modexp. With Montgomery
reduction a 1024-bit modexp went from 2.3 s to 5.2 ms, so a keypair
should be seconds — estimated, never measured on this machine. Any key
aborts the generation, so you are never stuck waiting.

The host certificate is one byte or so larger than in 0.8.8.11. That is
the `keyUsage` extension, which is new in 0.8.9 and is explained in the
changelog.

### Look at the phone

> **Trust This Computer?**

**Photograph the phone too.** Tap **Trust**.

---

## Part 3 — the record is kept, and the session opens

Back on the HP, run it again:

```
pair
```

This time the whole of it should happen in one go, and three things
should be different from 0.8.8.11:

```
  connecting to lockdownd...
  asking for DevicePublicKey...
  the device's key is 2048 bits.
  reusing the stored record -- HostID C1A2B3C4-...
  (no keygen: that is what saving it buys.)
  building the certificates...
  ...
  paired
  saved to .pairrec, 3912 bytes.
  It holds a PRIVATE KEY, unencrypted -- this kernel has
  nothing to encrypt it with. See include/pair.h.

  StartSession...
  session 0BD1C4E7-6A9F-4A1D-9C3E-5A1B2C3D4E5F
  TLS handshake, with our host certificate...
  the session is ENCRYPTED.
  QueryType inside the session...
  the device answered "com.apple.mobile.lockdown" over TLS.

  STAGE 3 IS COMPLETE.
```

**Photograph that.** The three new things:

1. **No keygen on the second run.** `reusing the stored record` means
   the file was written, read back, and its CRC and its keys checked.
2. **The same HostID.** A new HostID is a new computer to the phone and
   would ask for Trust again. If the HostID differs between runs, the
   record is not being reused and everything below is a fresh pairing.
3. **`STAGE 3 IS COMPLETE.`** That line is printed only after a request
   has gone down the encrypted channel and come back understood.

### If it stops before that line

Each step fails differently on purpose. Find the last line it printed:

| last line | what it means | next screen |
|---|---|---|
| `the device does not know this HostID` | the record is for another phone, or you unpaired it on the phone. `pair forget` then `pair` | — |
| `refused: <something else>` | lockdownd declined the session and named a reason | `usb lockdown` then `dmesg` |
| `the device did NOT ask for SSL` | unusual, and not a failure — the session continues in the clear. Worth a photograph | `usb lockdown` |
| `the handshake failed (…)` | **the interesting one.** See Part 4 | `tlsinfo` |
| `nothing usable came back` | the handshake succeeded and the session does not work. Worth a photograph on its own | `usb lockdown` |

---

## Part 4 — THE NEW HALF: what to do if the TLS handshake fails

This is the part with no hardware history. If it worked first time,
skip to Part 5 — but still run `tlsinfo` and photograph it, because a
*successful* handshake's diagnostics are a baseline nobody has.

```
tlsinfo
```

Three lines decide where to look next.

### "Server chose" — the cipher suite

HavenDOS offers exactly one: `0x003C TLS_RSA_WITH_AES_128_CBC_SHA256`,
static RSA key exchange.

If this says **NOT supported**, or the handshake died with
`the device sent alert 40`, then lockdownd on your iOS build will not
do static RSA, and that is a capability limit rather than a bug in the
pairing. It is also the single most likely way Part 3 fails, because it
is the one thing the openssl-based tests cannot answer: openssl is
happy to negotiate static RSA when asked; iOS may not be. **Photograph
this screen** — the alert number and the chosen suite together are
exactly what the next release needs to decide whether to implement an
ECDHE suite.

### "Client cert" — three states, three different problems

| it says | meaning | where to look |
|---|---|---|
| `the peer did not ask for one` | lockdownd never requested a certificate, so the failure is somewhere else entirely | the cipher line above |
| `asked for, NONE sent (empty Certificate)` | the pair record is missing or unusable — the handshake got as far as being asked and had nothing | `pair show` |
| `2 sent, CertificateVerify signed` | we sent the chain and signed the transcript. If the handshake still failed, the phone rejected what we sent | the alert number |
| `2 sent, NO CertificateVerify` | the signature step failed — a key problem, not a certificate problem | `pair show` |

### "Reached stage" — how far it got

The stage names are the handshake's own messages. The ones that are new
in 0.8.9 and therefore the ones worth reporting:

- `CertificateRequest received` — the phone asked. Good: this is the
  normal path.
- `client Certificate sent` — our chain went out and the phone did not
  like it.
- `CertificateVerify sent` — everything went out and the phone refused
  the signature or the Finished.

Also useful, and a different screen:

```
usb lockdown
```

It now prints the session and whether it is encrypted, and it keeps
`asked` separate from `handshake` — a device that asked for SSL and did
not get it is a different fact from one that never asked. It also
prints a record count and a `[a record went out half way]` marker,
which would explain a session that worked once and then went silent.

### What to send me

A photograph of `tlsinfo` and one of `usb lockdown`, plus the last
screen `pair` printed. Those three together name the failure
unambiguously; without them the handshake has about six distinct ways
of looking identical.

---

## Part 5 — it survives a reboot

This is the point of saving the record, and it needs an **installed**
system — the pair record lives on the system volume.

```
pair show
```

Expected:

```
  HostID      C1A2B3C4-D5E6-47F8-9A0B-1C2D3E4F5A6B
  SystemBUID  A1B2C3D4-0000-4000-8000-0123456789AB
  root 712, host 712, device 701 bytes of certificate
  root key 2048 bits, host key 2048 bits
```

Then **reboot**, log in, plug the phone in, and:

```
pair session
```

That skips the Pair request entirely and uses the stored record. The
phone must **not** ask for Trust again. Expected, in about a second:

```
  using the stored HostID C1A2B3C4-...
  StartSession...
  session ...
  TLS handshake, with our host certificate...
  the session is ENCRYPTED.
  QueryType inside the session...
  the device answered "com.apple.mobile.lockdown" over TLS.

  STAGE 3 IS COMPLETE.
```

**Photograph it.** If the phone asks for Trust again, the record was
not reused and `pair show` before and after the reboot will say which
part went missing.

### And the file itself

`.pairrec` is deliberately a text file, one field per line, so it can
be read without a tool:

```
cat .pairrec
```

Each certificate is base64 DER on a line of its own, which means it can
be pasted between `-----BEGIN CERTIFICATE-----` delimiters and handed
to `openssl x509` on another machine if anything about the chain is
ever in question.

**It holds two private keys, unencrypted.** There is nothing in this
kernel to encrypt them with — no keyring, no user secret, no secure
element, and a key encrypted under a constant in the kernel image is
plaintext with extra steps. What those keys can do is bounded: they
pair with phones that have already been paired from this machine.
Anyone who can read the HavenDOS partition can already replace the
loader and own the machine outright.

```
pair forget
```

deletes it. The phone still remembers the HostID until it is unpaired
there too (Settings → General → Transfer or Reset iPhone), and the next
`pair` invents a new one.

---

## What is still NOT done, so you do not look for it

- **Tethering does not turn on yet.** An encrypted session is what every
  interesting lockdownd request needs, and the request that starts the
  personal hotspot interface is the next one to write. Stage 3 being
  complete is the precondition for stage 4, not stage 4.
- **The device's own certificate is not verified.** `net/tls.c` has no
  CA store and does not check the server's chain — it never has, for
  HTTPS either. Over a USB cable to a device we have just certified
  ourselves, that is a smaller gap than it is on the open internet, but
  it is a gap and it is not closed.
- **No session resumption.** Every `pair session` is a full handshake.
- **One cipher suite.** See Part 4.

---

## The quick version

```
usb              phone listed?
usb lockdown     stage 1 and 2 work?
pair             Trust dialog on the phone -> tap Trust
pair             reused record, session, ENCRYPTED, STAGE 3 COMPLETE
pair show        what was stored
<reboot>
pair session     same again, no Trust dialog, in about a second
tlsinfo          photograph this whether it worked or not
usb lockdown     and this
```
