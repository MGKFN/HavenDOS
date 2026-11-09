# HavenDOS v0.8.8.9 "Helios beta 10" — it can take itself back off, and it can make a certificate

Channel Beta, codename "Helios", **beta 10**. 7 November 2026.

52 suites, **5066/5066** (up from 51 and 5021).

---

## 1. `uninstall`

Asked for: *"give instructions to remove HavenDOS fully of PC and let
HavenDOS do everything we did manually"*.

The manual procedure is four steps in an Administrator command
prompt — `mountvol`, restore `BOOTX64.EFI` from `BOOTX64.HDB`,
`rmdir /S \EFI\HAVENDOS`, then `diskpart` with
`delete partition override`. Every one of them is a step where the
wrong keystroke takes a partition that was not ours, and
`delete partition override` does not ask twice.

Now: boot the ISO, type `uninstall`.

It shows a preview produced by a **real dry run** — the same code
path with the write functions withheld, not a second description that
could drift from it — and then asks for the word `UNINSTALL` typed in
full. It finds the HavenDOS partition **by its type GUID**, which is
the whole reason it is safer than diskpart: no size to squint at, no
partition number to mistype.

### What it refuses to do

* **It never removes `\EFI\BOOT\BOOTX64.EFI` unless that file is
  ours.** `fs/esp.c`'s test applies: our loader is byte-for-byte
  `\EFI\HAVENDOS\grubx64.efi`. Anything else there belongs to somebody
  else — a Linux shim, a firmware updater, a rescue tool — and taking
  it away would unboot a machine on the way to uninstalling something
  else. The preview says when it is leaving one alone.
* **It removes an EFI System Partition only if HavenDOS made it:**
  volume label `HAVENBOOT`, which only `fs/espmake.c` writes, **and**
  no `\EFI\Microsoft` on it. Both, not either.
* **It does not erase the partition's data.** Clearing the table entry
  is what frees the space. Writing over several gigabytes of somebody's
  files to be tidy is a different operation with a different risk.

### Order, and what a power cut leaves behind

Boot loader first, then our files, then the partition, then our ESP if
we made one. A machine that loses power half way through has its
**original loader back** and a HavenDOS partition still in the table:
it boots Windows, and the uninstall can be finished later. The reverse
order would leave a machine with no loader and no HavenDOS.

### New underneath it

* **`gpt_remove_partition()`** in `fs/gpt_write.c`. That header has
  said since v0.8.6 that there is no `gpt_write_table()` and never
  should be, because "the way to not have that bug is to not have that
  function" — and that still stands, about writing a whole table from
  memory. This is the narrow opposite of `gpt_add_partition()`: one
  entry cleared, the same four sectors rewritten, backup first and
  primary last.

  **It will not remove a partition you cannot name.** The caller
  passes the slot index **and** the type GUID **and** the first LBA,
  and all three must match. An off-by-one in a caller's loop cannot
  delete the neighbour; it would have to be wrong about the same
  partition three different ways.

* **`fat32_rmdir()`**. `fat32_delete()` refuses a directory, and
  nothing in the tree had ever needed one, because HavenDOS only ever
  ADDED to an EFI System Partition. It refuses a directory that is not
  empty, counts an orphaned long-name slot as occupied, and frees the
  cluster chain before erasing the entry.

### The test is a round trip

`test_installadd` 108 → **151 checks**. The fixture ESP now carries a
740 KB **foreign** `BOOTX64.EFI`, so the install has something to back
up and the uninstall has something to put back — that restore is the
step that decides whether the machine still boots, and without a
foreign loader in the fixture it never ran.

Install, uninstall, then ask whether the disk is where it started:

* `BOOTX64.EFI` is back and is **byte-for-byte** the loader that was
  there before HavenDOS
* `BOOTX64.HDB` is gone, `\EFI\HAVENDOS` is gone
* the partition count is back to what it was, and no partition with
  our type GUID remains
* `fsck.fat` still accepts the ESP; Windows' own `bootmgfw.efi` is
  byte-identical
* a second `uninstall` reports there is nothing left to do
* and HavenDOS **installs again** onto the cleaned disk

Plus the other direction: an ESP that HavenDOS created is removed too,
and one it did not is kept.

`test_gptwrite` 76 → **102 checks**, all of the new ones about
removal: four refusals before any success, a dry run that names the
same four sectors the real run writes, and the central claim measured
again — **not one sector outside those four changed**.

---

## 2. Pairing stage 3: certificates

`net/der.c` has been in the tree since v0.8.8.6 — written, tested, and
with **nothing calling it**. It has a caller now.

**`net/x509.c`** builds X.509 v3 certificates and signs them with
PKCS#1 v1.5 over SHA-256, which is the shape libimobiledevice's
`pair_record_generate_keys_and_certs()` produces: a self-signed root
with `basicConstraints CA:TRUE` critical, and a host certificate with
`CA:FALSE` and `keyUsage digitalSignature, keyEncipherment`, critical.

The signature and the certificate are one function on purpose. The
thing being signed is the exact bytes of the TBSCertificate, and the
only way to be sure of that is to sign the bytes the encoder just
produced, from the same buffer. An interface that hands the TBS out to
be signed elsewhere invites the caller to re-encode it — and a
certificate whose signature covers a *different encoding of the same
values* verifies nowhere while looking perfectly correct in a dump.

### Every verdict is openssl's

`test_x509`, **45 checks**. Nothing in it parses a certificate:

| judge | question |
|---|---|
| `openssl x509` | does it parse, is it v3, are the extensions there and critical |
| `openssl asn1parse` | is it DER rather than merely BER |
| `openssl verify` | **does the signature check out against the issuer** |

The keys come from `openssl genrsa`, not from `rsa_keygen_step()` —
a 2048-bit keygen is 15–25 minutes and would make the suite
unrunnable, and a key from outside means a signature failure is our
arithmetic rather than possibly our key.

And the judge is shown to have teeth: one byte is flipped inside a
signed certificate, deep in the TBS so it still *parses*, and openssl
must refuse to verify it. A good certificate against an unrelated CA
must fail too.

### Six injections, and the one that got through

Five were caught immediately: a changed byte in the SHA-256
`DigestInfo` prefix, a signature algorithm saying sha1WithRSA while
the hash is SHA-256, version encoded as v1, `basicConstraints` no
longer critical, and a padding block missing its `00 01`.

**The sixth passed 42/42**, and it is the one the code has a comment
warning about: asking `bn_to_bytes()` for the signature's *own* byte
length instead of the modulus length. RFC 8017's I2OSP says the
signature is always exactly as long as the modulus, left-padded with
zeroes — and a natural-length encoding is correct 255 times in 256.
With that key and that digest, it happened to be.

So the case is now **searched for** rather than hoped for: sign a
counter until a signature turns up whose top byte is zero, then
require the encoding to have kept it. About 256 signatures, a second
of arithmetic, and a check that cannot pass by luck. All six
injections are caught now, and the last one reports
`...and it is still exactly the modulus length  got 255`.

### What stage 3 still needs

Certificates are one of four pieces. Still missing: a **plist
writer**, **splitting a request across mux packets** (`ld_send_plist()`
refuses a multi-packet request by name today), the **pair-record
exchange** itself, and **TLS with client certificates** on top of it.
`net/tls.c` is an HTTPS client and does not do client certs.

---

## 3. `include/pit.h`

`kernel/pit.c` had no header. Seven files carried their own hand-copied
`extern void sleep_ms(uint32_t);` — `net/dhcp.c`, `net/tcp.c`,
`net/dns.c`, `net/http.c`, `net/tls.c`, `net/icmp.c`, `net/ipv4.c` —
and `kernel/main.c` forgot its one and called `sleep_ms()` through an
**implicit declaration**, which compiled and, on this ABI, worked.

A signature that lives in eight places changes in seven of them. More
immediately: v0.8.8.8 changed what these functions *mean* — `pit_us()`
was counting at twice real time, because channel 0 runs in mode 3
where the 8254 decrements by two per clock — and a reader chasing that
had nowhere to look it up. One header now, with the units and that
mode-3 warning written down beside each function.

---

## Still not done

Unchanged from beta 9, and for the same reasons: the **trackpad**
(needs PCI resource assignment *and* an ACPI interpreter), the **USB
mouse** (already written in `usb/hid.c`, never confirmed on hardware —
a test, not a task), and an **external monitor** (needs an Intel Xe
display driver).
