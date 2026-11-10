# HavenDOS v0.8.9 — pairing stage 3, finished

8 November 2026. **59 suites, 5883/5883** (up from 58 and 5641).

Stage 3's second half: `StartSession`, TLS with client certificates,
and a pair record that survives a reboot. The command that proves it
is `pair`, and it now ends either with `STAGE 3 IS COMPLETE.` or with
a named step and a screen that says which.

Hardware test: `docs/PAIRING-STAGE3-HARDWARE-TEST.md`. It covers
**both halves** — the first half's instructions were in the 0.8.8.11
doc and are carried forward, so if that test was never run this one
replaces it.

## Where stage 3 stands

| piece | state |
|---|---|
| 1. X.509 v3 + PKCS#1 v1.5 | 0.8.8.9, judged by openssl |
| 2. a plist writer | 0.8.8.10 |
| 3. requests bigger than one mux packet | 0.8.8.10 |
| 4a. the pair request, and the Trust dialog | 0.8.8.11 |
| **4b. StartSession, then TLS with client certs** | **done here** |
| **4c. keeping the record** | **done here** |

## The version number, and the trap in it

`ver_cmp()` compares component by component, so `0.8.9` is newer than
`0.8.8.11` and `update` offers it — checked against the parser in
`tests/test_vercmp.c` rather than assumed.

The trap bites one way only and 0.8.8.11 recorded it: `0.8.8.11` vs
`0.8.9` compares as a **downgrade**. A machine that was somehow updated
to a 0.8.9-numbered image during the 0.8.8.x series could never be
offered anything in that series again. Nothing shipped as 0.8.9 then.
This is the release that takes the number.

Codename unchanged at **Helios**, because this finishes what Helios
started. The roadmap's DAEDALUS still has its place in the sequence and
takes whatever number it reaches — that is Kyan's call to make, and
renaming a release is one line in `include/version.h`.

---

## 1. `net/tls.c` grew a transport, and a client certificate

### The transport, because a phone is not a socket

`net/tls.c` spoke to `net/tcp.c` **by name** — `tcp_send()`,
`tcp_recv()`, `tcp_eof()`, `tcp_close()` — in nine places. The session
lockdownd wants runs over a usbmux channel, which has no fd and is not
a socket.

So those four calls go through a `tls_transport_t`, and the handle is
whatever the transport wants it to be. `net/tcp.c` is one transport (a
straight forwarding, deliberately: a bug introduced by the indirection
would have to be in four one-line functions); `usb/lockdown.c` supplies
the other.

**The transport and the client certificate belong to the SLOT, not to a
global.** Two sessions can be open on two transports — an HTTPS fetch
and a lockdown session at the same time is exactly what pairing
creates — and a global would send one session's records down the
other's channel. Both ends would report success.

One thing the contract now says out loud, because an injection asked
for it and nothing could detect it: **`send` returns negative on
failure and the count is not meaningful.** `tcp_send()` returns 0 for a
complete write, so a caller comparing the result against the length
would reject every successful send. Partial writes are therefore the
transport's problem, and `usb/lockdown.c`'s says so by name and latches
it.

### CertificateRequest, Certificate, CertificateVerify

RFC 5246 §7.4.6 and §7.4.8. Three things worth stating:

- **A CertificateRequest is now answered even when there is nothing to
  send.** An empty Certificate message is what the RFC requires of a
  client with no certificate; before this release nothing was sent at
  all, and a server that asks politely and then insists would hang the
  handshake rather than refuse it. lockdownd asks every host.
- **CertificateVerify signs the transcript up to and including the
  ClientKeyExchange, and not itself.** So it is sent after the key
  exchange — §7.3's flight order — and it takes a *copy* of the running
  SHA-256 context, exactly as `compute_finished()` already did.
  `sha256_final()` consumes a context, and signing with the live one
  leaves the Finished hash computed over nothing: a defect that
  surfaces two messages later.
- **A client that sent an empty Certificate does not send a
  CertificateVerify.** There is no key to prove possession of, and a
  signature from a key the peer has no certificate for is worse than
  silence.

Signing is a **callback**. The private key is a `bn_t` pair held by
`shell/paircmd.c` and `net/tls.c` has never known what a bignum is —
but the reason that matters is testing: a suite can sign with openssl
instead, so what the suite proves is that the transcript and the
framing are right rather than that our RSA agrees with itself.

### `signature_algorithms`, which was not decoration

The ClientHello now carries it, with exactly one entry: sha256+rsa.

RFC 5246 §7.4.1.4.1: a TLS 1.2 client that omits this extension **is
taken to have offered {sha1, rsa}**, and a server is then entitled to
refuse a CertificateVerify signed with SHA-256 — which is the only kind
this stack can produce, because `pkcs1_sign_sha256()` is the only
signer in the tree. The omission cost nothing for as long as no
CertificateVerify was ever sent. The first client certificate would
have been rejected for a reason nowhere near the certificate.

Also: **no hostname means no SNI extension**, rather than an SNI
carrying an empty name. A usbmux connection has no name to send and RFC
6066 requires a non-empty HostName.

## 2. The stage names were in two places, and inserting one renamed them all

`g_tls_last_stage` exists so a failed handshake on somebody else's
machine can say how far it got. The numbers were `#define`d in
`net/tls.c`; `shell/shell_v076.c` — the `tlsinfo` screen, the only
place a human ever reads them — carried a **second copy of the names**,
as a switch on bare integers, plus `>= 9` for "established" and a
literal `"/9"`.

This release inserts three stages, two of them in the middle. Nothing
would have failed to compile. `tlsinfo` would have reported every stage
from 4 onwards under the previous stage's name, so a handshake that
died asking for a client certificate would have read
`ServerHelloDone received` — the exact sentence that sends somebody
looking in the wrong place.

The numbers are in `include/tls.h` now and `tls_stage_name()` is the
one function that turns a number into words.
**`tools/check_tls_stages.py`** refuses a build with a second copy of
either, or with `g_tls_last_stage` compared against a bare integer.
That last rule is the one that matters: the others describe how the
list is kept in one place, and it is what stops the next reader
hardcoding a 9 again.

## 3. `usb/lockdown.c`: the session

```
host -> { Request: StartSession, HostID, SystemBUID }
dev  -> { Request: StartSession, SessionID: "...",
          EnableSessionSSL: true }
```

`lockdown_start_session()`, `lockdown_enable_ssl()`,
`lockdown_stop_session()`, and the usbmux transport.

**`InvalidHostID` here is a different failure from a pairing that was
refused**, and it needs saying differently: it is what a lost or
mismatched pair record looks like from the outside. Before this release
losing the record was the only possibility.

Four things that are deliberate rather than incidental:

- **`ssl_on` is set from the handshake's success and from nothing
  else** — never from what the device asked for. Everything above that
  line travels in the clear and everything below it does not. A flag
  set on intent sends the next plaintext request into a session that
  does not exist, and lockdownd answers that with silence.
- **`ssl_wanted` is kept beside it**, so a device that asked for SSL
  and did not get it is distinguishable from one that never asked.
  `usb lockdown` prints both.
- **StopSession travels inside the session**, so it is sent first and
  the TLS is torn down after it.
- **`lockdown_close()` closes the TLS session before disconnecting the
  mux** — a close_notify has to travel down a channel that is still
  open. **`lockdown_forget()` does the opposite on purpose**: `usb
  rescan` calls it because the device may be the one that was just
  unplugged, and a farewell packet to it burns the poll budget. The
  slot is still released, because `net/tls.c` has three and one never
  given back means the next handshake reuses a session whose keys
  belong to a device that is gone.

`ld_key_bool()` is a second scanner, not a generalisation of the first.
A plist boolean is an empty element with no `<string>` anywhere near
it, so `ld_key_string()` would run past its own key into the next one's
value — and a reply whose `EnableSessionSSL` is a string and whose next
key is `<true/>` would have reported SSL requested.

This is the first time anything in `usb/` includes a `net/` header. The
dependency is one way: `net/tls.c` knows nothing about usbmux and is
handed a transport.

## 4. The pair record is kept

`.pairrec` on the system volume, and `pair show` / `pair forget`.

**It holds the private keys**, and that is the part worth being clear
about. A certificate is useless without the key it certifies: the TLS
session proves we hold the host key by signing the handshake with it.
A store that saved the record and not the keys would look complete,
reload cleanly, and fail at CertificateVerify with nothing on screen
connecting the two. So it is one struct and one file.

**The record is reused whole** — keys *and* UUIDs. A new HostID is a
new computer to the phone, so regenerating the UUIDs while keeping the
keys would ask for Trust again and look as though the keys had not been
kept either. The certificates are rebuilt each time, because the device
certificate certifies *this* device's key and a stored one may be
another phone's; that costs milliseconds, and the keygen was the
expensive part.

**Saved before the session is attempted.** A session that fails leaves
a record that is still valid — the phone has already agreed — and
throwing it away because the step after it failed would mean another
Trust tap to retry.

### A text format, and what it buys

One field per line, each certificate base64 DER on a line of its own,
so it can be pasted between PEM delimiters and handed to `openssl x509`
with no tool in between. The format is strict about everything else:
fields in order, each exactly once, no unknown fields, a version that
is refused by name if it is a later one, and a CRC-32 over everything
above it.

The serialiser is in `usb/pair.c` and contains **no filesystem call**.
The `vfs_write()` is in `shell/paircmd.c`. A serialiser that needs a
mounted volume to be exercised is a serialiser that does not get
exercised.

### It is a private key on a FAT volume, unencrypted

There is nothing to encrypt it *with*: no keyring, no user secret, no
secure element, and a key encrypted under a constant in the kernel
image is plaintext with extra steps. What it can do is bounded and
worth stating plainly — it pairs with phones that have already been
paired from this machine. Anyone who can read the HavenDOS partition
can already replace the loader and own the machine outright.
`pair forget` deletes it.

## 5. `keyUsage` on the host certificate — a header's claim the code never carried

`include/x509.h` has described the host certificate as "CA:FALSE,
keyUsage digitalSignature + keyEncipherment, critical" since 0.8.8.9,
taken from libimobiledevice's `userpref.c`. `pair_build_certs()` never
set it. Nothing noticed, because nothing used the certificate for
anything.

It stopped being harmless this release: the host certificate is now a
TLS **client** certificate and `digitalSignature` is exactly the usage
a CertificateVerify exercises. Found by the injection harness, which
could not find a reachable defect to inject there — the honest reading
of which was that the field was missing.

---

# What the tests do, and the three findings they produced

## `tests/test_tlscc.c` — 64 checks, and openssl is the judge

There is **no mock server in it.** `net/tls.c`'s `tcp_send()` and
`tcp_recv()` are implemented on a real POSIX socket and the peer is
`openssl s_server`, run as a process, doing a full TLS 1.2 handshake
and making the decision the phone makes:

```
openssl s_server -Verify 1 -CAfile <our root> -verify_return_error
```

`-www` makes it answer with a page that **names the certificate it
accepted**, so success is read off openssl's own words —
`Subject: CN=HavenDOS Host` — rather than off our believing the
handshake returned zero.

A mock would have been worthless in the specific way that matters. The
transcript a CertificateVerify signs is every handshake byte in order;
a fixture written from the same reading of the RFC as the code would
agree with the code about which bytes those are, and the phone would
not.

Five negative controls, each a defect that passes every structural
check and fails only here: the same chain against an unrelated CA; one
byte flipped in the signature; a signature over a different digest; a
signature made with the **root** key while the **host** certificate is
sent; and an empty chain where one was required.

### FINDING: `-verify_return_error` is load-bearing, and the negative control found it

`s_server -Verify 1 -CAfile <unrelated>` **accepted our chain.**
s_server installs a verify callback that prints `verify error` and then
returns 1 anyway, so the handshake completes and the page comes back
naming a certificate openssl had just failed to verify.

For one run, the suite's main case — the whole reason the file exists —
was passing against a judge that was not judging. The negative control
is what found it. That is what negative controls are for, and it is why
every crypto suite in this tree has one.

**26 injected defects into `net/tls.c`, all 26 caught**
(`tools/check_tlscc_suite.py`, under ASAN). Three needed a better
fixture first, and one of those changed the code:

- **The certificate counter was set before the send succeeded.** With
  every send succeeding there was no way to see it. The suite grew a
  transport that refuses the Certificate record specifically, and
  `net/tls.c` now sets the counter after. It is only a diagnostic —
  which is exactly the kind of thing that stays wrong for releases, and
  `tlsinfo` prints it on a machine nobody can attach a debugger to.
- **Both transports in the suite read the same socket**, so a read that
  went through `net/tcp.c` by mistake still returned the right bytes.
  Counting the writes was not enough; the reads are counted now.
- **`cert[2]` and `cert[3]` were left null**, so a chain claiming more
  entries than the array holds hit a null pointer at index 2 and was
  refused before it could read past the end. Every slot is filled now,
  leaving the bound as the only thing standing between the loop and the
  end of the array.

## `tests/test_lockdown.c` — 233 → 334 checks

The fake TLS in this suite **XORs every byte, and the device model XORs
too.** That is the whole design: a fake that merely counted calls could
not answer the question this file has to answer, because a request that
took the wrong branch would still arrive, still parse, and still be
answered. With the cipher, a request sent in the clear on an encrypted
session arrives as rubbish and lands in `bad_requests`. The branch
cannot be taken wrongly and pass.

The handshake is modelled as what it actually is: the moment both ends
start encrypting.

**65 injections, all 65 caught.** Four needed a fixture that did not
exist, and each of the four is a case the suite could not previously
express:

- **A device that declines SSL and a handshake done anyway.** Every
  other case has `ssl_on` and `ssl_wanted` agreeing, so
  `ssl_on = ssl_wanted` was indistinguishable from `ssl_on = 1`.
- **A successful StartSession followed by a refused one**, for the
  state that must not be inherited.
- **A send that fails while the session is encrypted**, for the
  difference between reporting it and going on to read a reply that
  will never come.
- **The broken-stream latch on a healthy channel.** A real partial send
  also drops the connection, so the "connected" guard refuses first and
  removing the latch changed nothing observable.

### FINDING: one injection had been stale for a release, and six were orphaned

`inject.py` reports an injection that no longer matches rather than
counting it as caught, and that is the only reason any of this was
noticed.

- **"the request is sent without its length prefix"** still named
  `usbmux_send()`, which 0.8.8.10 replaced with
  `usbmux_send_stream()`. It had silently stopped being applied.
- **Six base64 injections** aimed at the decoder that used to live in
  `usb/lockdown.c`, which 0.8.8.11 deleted when it replaced four base64
  implementations with one. They had been matching nothing since.

## FINDING: two harnesses existed only as prose

`net/b64.c`'s header records nineteen injections, all nineteen caught.
`usb/pair.c`'s trailer records twenty-seven, twenty-four on the first
run, and names the three that survived. **Neither was written down as a
harness**, so none of those forty-six could be run again.

`tools/check_harnesses.py`'s own message is the rule that was being
broken: *a harness nobody runs cannot tell you it is broken.* One run
by hand, left as a sentence in a comment, is that one step earlier.

- **`tools/check_b64_suite.py`** — 16 injections, all 16 caught,
  including the six re-homed from the lockdown harness and the two that
  file's header names as having nearly escaped. Two needed new checks:
  `b64_encoded_len()` is now compared against what `b64_encode()`
  actually writes at every wrap column including the ones that are not
  a multiple of four, and the encoder's NULL-input refusals are
  exercised.
- **`tools/check_pair_suite.py`** — 35 injections, all 35 caught, under
  ASAN. It holds the ones `usb/pair.c`'s trailer names explicitly plus
  the new store. It does not claim to be all twenty-seven of the
  originals: the rest were described in a sentence rather than in code,
  and inventing them back would be guessing at what was run.

## `tests/test_pair.c` — 78 → 149 checks

The store, judged by openssl again: the host certificate is pulled out
of a **decoded** record, wrapped in PEM, and verified against the root
from the same record. A round trip that lost or shifted one byte of DER
cannot survive that. Plus the signature `net/tls.c` will ask for,
verified with `openssl pkeyutl -verify` against the host key — and
required to fail against the root key, which is the wrong-struct-field
defect stated as a control.

Three fixture problems found and fixed, all three the same shape — a
check that passed for the wrong reason:

- **The private key PEM and the certificate PEM shared a path.**
  `make_key()` wrote the key to `/tmp/hd_pair_<tag>.pem` and the
  certificate was later written over it, so `openssl pkeyutl -verify
  -inkey` was reading a certificate as a key. It failed — and the two
  negative controls around it passed on the strength of that failure. A
  judge that always convicts is no better than one that never does.
- **The damage sweep was flaky**, about half of runs. It flipped each
  character to `'A'`, and the positions it let through were inside the
  CRC's own hex digits: the CRC is written in lower case and the reader
  accepts either, so turning an `'a'` into an `'A'` is not damage at
  all. The check was wrong, not the code. It flips to `'Z'` now, which
  is neither a hex digit nor a UUID character, and the
  case-insensitivity is asserted on purpose instead of being
  stumbled over.
- **Nothing came within a kilobyte of the buffer bound.** The store is
  now encoded into malloc'd buffers of *exactly* the record's length
  and one byte more, so ASAN guards both sides: the record needs
  `len+1` because it is NUL terminated as well as counted, and a writer
  that kept no byte back for the terminator would pass every check with
  a generous buffer.

And a fixture that is worth naming because the CRC cannot do its job:
**a record that checksums correctly and is still not a key.** The suite
forges a field and *recomputes the CRC*, which is what a file written
by a version of this code that did not check would look like. The
checksum is no defence against that; the semantic checks on the way in
are the only ones.

### Two injections recorded as unreachable rather than counted

0.8.6.3's rule — an injection that cannot express a reachable defect
must not be added, because it reports a catch the suite did not earn.

- Removing `if (nb == 0) nb = 1;` in the bignum writer. Only a zero
  bignum reaches it, and the encoder now refuses a record with a zero
  modulus, a zero private exponent **or a zero public exponent** before
  any field is written — that last refusal was added for exactly this
  reason. The guard stays: it becomes reachable the day somebody adds a
  twelfth field.
- Removing the bignum buffer bound. `tmp` is `BN_LIMBS*4 + 4` bytes and
  `bn_bits()` cannot exceed `BN_LIMBS*32`, so no value a `bn_t` can
  hold crosses it. Unreachable by construction, which is why the check
  stays.

---

## Still open

- **Tethering.** An encrypted session is what every interesting
  lockdownd request needs; the request that brings up the personal
  hotspot interface is the next one to write. Stage 3 is the
  precondition for stage 4, not stage 4.
- **The device's own certificate is not verified.** `net/tls.c` has no
  CA store and never has, for HTTPS either. Over a cable to a device we
  have just certified ourselves it is a smaller gap than on the open
  internet. It is still a gap.
- **One cipher suite**, `TLS_RSA_WITH_AES_128_CBC_SHA256`. Static RSA.
  Whether lockdownd will negotiate it is the one thing the tests cannot
  answer, because openssl is happy to and iOS may not be — it is the
  single most likely way the hardware test fails, and Part 4 of that
  doc says exactly what to photograph if it does.
- **No session resumption.** Every session is a full handshake.
- **External monitor** needs an Intel Xe display driver.
- **Multitouch gestures.** The contact count and tip switch are read;
  two-finger scrolling is possible and not done.
- **NVRAM boot entries.** Still impossible, unchanged reason:
  `SetVariable()` is a UEFI *runtime service* and a multiboot1 kernel
  never receives the EFI system table, because GRUB calls
  `ExitBootServices` before handing over.
