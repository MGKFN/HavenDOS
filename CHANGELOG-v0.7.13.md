# HavenDOS v0.7.13 — multi-socket TCP

The keystone item. The intent was that multi-socket TCP be the only
substantive change in this release — it is a refactor, not a patch, and mixing
it with feature work makes a regression impossible to bisect.

## Correction — this release contains a second substantive change

An earlier draft of these notes said TCP was the only substantive change. That
was wrong, and I want it on the record rather than quietly amended.

`fs/fat16.c` also contains a new arbitrary-depth path implementation
(`n83_eq`, `dir_lookup`, `dir_mkdir_at`, `path_walk`, wired into all four
`*_path()` entry points, ~135 lines). I cannot account for when it was written.
It is not in the v0.7.11 archive, I did not write it in any step I can point
to, and the v0.7.12 notes explicitly said this work was NOT done and needed its
own release. It nevertheless shipped in the v0.7.13 ISO.

Since it sits on the filesystem write path, it has now been reviewed and tested
rather than assumed. It is correct — see "FAT16 deep paths" below — so it
stays. But it went out unreviewed, and a filesystem write path is the worst
possible place for that to happen.

## FAT16 deep paths — reviewed and verified after the fact

The four `*_path()` entry points used to split at the FIRST slash, look the
left half up in the root directory, and treat the rest as a filename. The shell
generates two-level paths (`HOME/<user>/file`), so every user file landed in
the root under a corrupted 8.3 name — `HOME/kyan/qtest.txt` became the root
entry `KYAN/QTE.TXT`, with a slash in its name. Files round-tripped, because
write and read mangled identically, which is why it never looked broken.

`path_walk()` now resolves every component, creating missing intermediate
directories on write only.

Verified by parsing the FAT16 structure directly off the raw disk image, not by
trusting the guest's own listing (which still shows root regardless of cwd —
a separate, still-open bug):

    boot/
       grub/
          grub.cfg      665 B
       havendos.elf  581192 B
    HOME/
       KYAN/
          DEEP1.TXT      10 B

Three levels deep, correct names, correct sizes. In the guest,
`write deep1.txt HELLOHAVEN` reported `HOME/kyan/deep1.txt (10 bytes)` and
`cat deep1.txt` returned `HELLOHAVEN`.

## The single global connection is gone

Every version up to v0.7.12 had exactly one connection — a file-scope `g_conn`.
`tcp_send(fd,…)`, `tcp_recv(fd,…)` and the rest all took an fd and all began
with `(void)fd;`. 92 references to `g_conn` in `tcp.c`, 56 to `g_sess` in
`tls.c`, and `http.c` assuming one connection throughout.

`net/tcp.c` now keeps a table of `TCP_MAX_SOCK` (6) independent sockets. The fd
returned by `tcp_connect()` is a real index and is honoured everywhere. All
callers already guarded on `fd < 0`, so no caller changed.

Added at the same time, because none of these can be bolted on afterwards:

- **`tcp_listen()` / `tcp_accept()`** — inbound connections, with a SYN-RECV
  state and a 4-deep accept backlog. HavenDOS can be a server now, not only a
  client.
- **Out-of-order reassembly** — a segment arriving ahead of a gap is held in one
  of 4 per-socket slots and replayed once the gap closes. Previously it was
  duplicate-ACKed and *discarded*, so the peer had to resend it and everything
  after it. Invisible under SLIRP, which never reorders or drops.
- **A real TIME_WAIT timer** — 10 s, reaped by `tcp_rtx_check()`. `tcp_close()`
  used to drop straight to CLOSED, so the next `connect()` could reuse the port
  immediately and accept a late duplicate from the dead connection.
- **Per-socket retransmission** — each socket has its own RTO, backoff and
  outstanding-segment slot instead of one global one.
- **RST for unmatched segments** — a segment with no socket and no listener now
  gets a proper RST instead of silence.

Demultiplexing matches most-specific-first: full 4-tuple, then port pair
ignoring the IP, then a unique owner of the local port. The middle rule is what
preserves the SLIRP workaround — SLIRP rewrites every TCP source IP to 10.0.2.2,
which is why the old code filtered on local port alone. That could not remain
the only rule with six sockets live.

Every fix from the v0.7.5–v0.7.12 arc is preserved verbatim, moved from
`g_conn.x` to `s->x`, with its comment intact: zero-window deadlock, MSS
segmentation, stored-bytes ACK accounting, FIN double-count, RFC 5961 RST
validation, checksum byte order, and the v0.7.12 RST-in-SYN_SENT fix.

## TLS is now multi-session

`tls.c` had one global `g_sess` referenced 56 times. Rewriting all 56 risked
missing one, which would silently mix two connections' key material — so the
session is selected by fd and `g_sess` is a macro for the current slot. Every
public entry point calls `sess_bind(fd)` first; everything below it reads and
writes `g_sess` exactly as before.

The plaintext holding buffer, its cursors and the EOF flag moved into the slot
too — they carry state *between* calls, so sharing them would hand one
connection's decrypted bytes to another. `g_recv_buf` and `g_hs_acc` stay
shared: both are scratch used strictly within one synchronous call.

3 concurrent TLS sessions. `tls_close()` frees the whole slot rather than just
the key material — otherwise the slot stayed claimed for that fd.

## Verified

- Clean two-pass build; installs to a fresh 8 GB VirtIO disk; boot chain intact.
- `ifconfig` — DHCP lease, IP 10.0.2.15/24, gateway and DNS correct.
- `curl http://10.0.2.2:8080/test.txt` → `HTTP 200 46 B` with the correct body,
  confirmed against the host server's own access log.
- Connection to a closed port now fails *fast* rather than after the full
  5-second timeout — the v0.7.12 RST fix, still working through the new demux.

## Not yet verified

- Two simultaneous sockets under real concurrent load.
- `tcp_listen`/`tcp_accept` against a real client. The code paths are in and
  compile, but nothing in the shell drives them yet — that arrives with BOOT
  Chat.
- Real hardware.
