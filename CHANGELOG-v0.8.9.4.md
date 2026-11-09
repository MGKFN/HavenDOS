# HavenDOS v0.8.9.4 — "done in 70 ms" was the loop, not the keygen

9 November 2026. 59 suites, **5932/5932**.

Stage 2 works. `GOT THE DEVICE KEY. Stage 2 is done. 431 bytes`, and
the log confirms `-----BEGIN RSA PUBLIC KEY-----`. Stage 3 then failed
on our own key:

```
generating the root key (2048-bit)...
root key: done in 70 ms
building the certificates...
a key too small to pair with
```

**A 2048-bit keygen is 577 steps and about half a second.** 70 ms was
not a fast keygen. It was no keygen at all.

## The cause: two conventions, one tree

`include/rsa.h`:

```c
#define RSA_KG_WORKING   1
#define RSA_KG_DONE      2
#define RSA_KG_FAILED    3
```

**All positive.** Nearly everything else in this tree uses the other
convention — zero or positive for success, negative for an error — and
`shell/paircmd.c` was written from that habit:

```c
r = rsa_keygen_step(&g_kg);
if (r < 0)  { ...the generator gave up... }
if (r == 1) break;                /* "done" */
```

`r == 1` is **RSA_KG_WORKING**. So the loop broke out after the *first*
bounded piece of work and copied `g_kg.n`, `.e`, `.d` — which were
still zero. `pair_build_certs()` then refused the key three functions
later with "a key too small to pair with": true, unhelpful, and nowhere
near the cause.

The failure path was wrong the same way and worse. `RSA_KG_FAILED` is
**3**, so `r < 0` never fired; a generator that genuinely gave up would
have been stepped for ever.

Neither line fails to compile. **`shell/rsacmd.c`, which does the same
job, had it right from the start** — `if (r != RSA_KG_WORKING) break;`.
One file right and one file wrong, with nothing able to tell them
apart.

## The fix, and two guards

- The loop names the constants.
- **`keygen()` checks the key it was handed.** `bn_bits(n) == 2048` or
  it refuses, by name, while the words "generating the root key" are
  still on the screen. One comparison, and the difference between
  failing where the cause is and failing three functions away.
- **`tools/check_rsa_kg_loop.py`** refuses a build in which the result
  of `rsa_keygen_step()` is compared against a bare integer, or tested
  for being negative. Both shapes verified to fail the check.

That checker found its own bug first: it looked at a fixed window of
characters after the call and missed the defect it was written for,
because the comment explaining the convention now sits between the call
and the comparison and `csource.blank()` preserves length. It scans to
the end of the enclosing function instead. A check that cannot see past
a comment stops working the moment somebody documents the thing being
checked.

## The test gap, which was the real one

**`tests/test_rsa.c` has only ever generated 512-bit keys.** The
reasoning was sound — 512 is quick and the state machine is the same at
any size — and it left the gap that mattered: the one caller that
matters asks for 2048, and nothing had watched a 2048-bit key come out
of this generator end to end.

It does now, in about half a second, and it asserts what the shell did
not: that the generator saying "done" and the key being the size asked
for are **two different claims**. Plus a round trip through the key
with `bn_modexp`, and that it took hundreds of steps rather than one.

Worth stating plainly, because it is the lesson rather than the bug: a
suite that drives an API correctly proves the API works and says
nothing at all about whether anybody else used it that way. Everything
in `test_rsa.c` passed for three releases while the only caller that
mattered was broken.

`rsa_why_name()` was also being reached for under a name that does not
exist, through an implicit declaration. It is declared in
`include/rsa.h` and now actually called.

## Still open, unchanged from 0.8.9.3

- **A USB transaction error takes the whole bus down.** `xHCI slot 2
  ep0: USB Transaction Error (cc 4)` → port removed → `ADDRESS_DEVICE
  refused` on the others → `usb: 0 devices, 0 disks, 0 NICs after
  hot-plug`. One device faulting costs every device on the bus. This is
  the next thing to look at.
- **The kernel log fills and drops lines.**
- **The desktop is choppy.** Undiagnosed; the hot-plug scan and the
  DHCP retry both run off the 100 Hz bottom half.
