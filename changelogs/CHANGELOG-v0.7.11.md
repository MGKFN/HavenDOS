# HavenDOS v0.7.11

**TechHaven Studios — September 2026**

Clearing roadmap backlog. **v0.7.8, v0.7.9 and v0.7.10 shipped display work and
emergency fixes under those numbers, so the planned content of all three was
skipped.** This release starts paying that back.

| Roadmap item | Status |
|---|---|
| 0.7.7 robust certificate parsing | **done here** |
| 0.7.8 DNS caching / CNAME / multi-A | **done here** |
| 0.7.8 `nslookup` | **done here** |
| 0.7.8 HTTP POST, keep-alive, DHCP renewal, traceroute, wget resume | still open |
| 0.7.9 multi-socket TCP | still open — see below |
| 0.7.10 BOOT Chat | blocked on 0.7.9 |

---

## 1. Certificate parsing is now bounds-checked (roadmap 0.7.7)

The old parser scanned the whole DER blob for the RSA OID byte pattern and
walked forward from wherever it matched, with almost no bounds checks:

```c
if(list_len+3>blen) return -1;    /* uint32 + 3 compared against uint16 */
if(cert_len+6>blen) return -1;
```

Both add a 24-bit length read **off the wire** to a small constant. A crafted
value near 2³² wraps and the check passes. After that, `p[0]`, `p += 1+lc`,
`p += mlen` and the rest walked forward guided only by the *declared* lengths,
never re-checking against the end of the certificate. And because the OID
pattern can legitimately appear inside a subject name or an extension, the walk
could start from the middle of unrelated data.

A hostile — or merely unusual — certificate could read past the end of the
receive buffer. Every step is now bounded against the real end of the
certificate via an `AVAIL()` guard, declared lengths are checked against actual
remaining bytes before use, and the key is only accepted where the OID is
followed by a well-formed AlgorithmIdentifier and SubjectPublicKeyInfo.
Modulus sizes outside 512–4096 bits are rejected rather than truncated.

`selftest` gains a **DER length bounds** group that pins the arithmetic,
including the two wrap-around values that used to pass. Verified on the host:
6/6, and the old expressions fail them.

## 2. DNS: caching, TTLs, CNAME, multiple A records (roadmap 0.7.8)

The resolver returned the **first A record and nothing else**. No cache, so
every `wget`, `curl` and `bpkg` call paid a full round trip. No CNAME handling,
so any host behind a CNAME — which is most CDN-hosted hosts — failed outright
*even though the answer section contained the address*, because the scan
stopped at the first non-A record.

Now:

- **8-entry cache** keyed by hostname, honouring the record TTL, clamped to
  30s–1h so it neither thrashes nor goes stale
- **All A records collected** (up to 4) instead of just the first
- **CNAME chains resolve**, because the scan continues past non-A records to
  find the canonical name's address in the same answer section
- `dns_resolve_all()` for callers that want the full set

## 3. `nslookup` (roadmap 0.7.8)

```
nslookup example.com     resolve, showing every A record
nslookup -cache          dump the resolver cache with remaining TTLs
nslookup -flush          clear it
```

Aliased as `host`.

---

## What is still open, and why I am not shipping it blind

**Multi-socket TCP (roadmap 0.7.9)** is the keystone — BOOT Chat is blocked on
it — and it is not a patch. `tcp.c` has **92 references to the single global
`g_conn`**, and `tls.c` has **56 to its single global `g_sess`**; `http.c`
assumes one connection throughout. Replacing that with a socket table means
touching the retransmission slot, the receive ring, the state machine and the
TLS session all at once.

The last three releases are a good argument for not attaching a change of that
size to a release that also does other things. It gets its own session, where
it is the only variable and `selftest` can grow real socket-table coverage
before anything ships.

The remaining 0.7.8 items — HTTP POST and keep-alive, DHCP lease renewal,
`traceroute`, `wget` resume — are each self-contained and can come next
release; none of them blocks anything.

---

## Build

Clean build, warnings hold at **40**. `selftest` is now **51 checks** across
eight groups. Source zip rebuilds a working ISO, verified.

---

## Carried forward

- **LBA48 8 GiB wrap** (v0.7.6) — reinstall if you have not since.
- **Display modes untested**: 80×50, 90×30 and 90×60 were never actually
  exercised, because the v0.7.9 Attribute Controller bug blanked the screen
  before anything could be judged. The v0.7.10 confirm-or-revert timer makes
  trying them safe — worst case you wait ten seconds.
- Desktop pinned to 80×25; coordinate conversion is its own pass.
- TLS: still no CA store, so certificates are parsed safely but **not
  verified against a trust anchor**.
