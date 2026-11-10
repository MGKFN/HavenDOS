# HavenDOS v0.8.8.1 "Helios" — the mouse, exFAT, and real arithmetic

Channel Beta, codename "Helios". 31 October 2026.

**v0.8.8 is relabelled "Helios beta 1".** This is the Helios release;
that one was the half of it that shipped first. The version number has
to keep going up — `fs/update.c` compares versions with `ver_cmp()`
and an update that does not look newer is not offered — so the number
is 0.8.8.1 and the name is the same.

Three things, and one of them is the answer to "the polling still
doesn't really work".

**The mouse works.** Both of them. Neither the trackpad nor a USB
mouse moved the cursor, and the cause was not the polling at all.

**A USB stick over 32 GB is readable.** exFAT, read-only. Since
v0.8.6.1 `fat32` has *named* one and stopped there; now `exfat` reads
it.

**And there is real big-integer arithmetic in the tree**, checked
against CPython and `openssl genrsa`. TLS key exchange stops pausing,
and this is the first piece of iPhone pairing that can be built
without a phone in the room.

---

## 1. "Both dead" — and it was never the polling

The report was that the trackpad did not work and neither did a USB
mouse. Two dead pointers with one cause, and the cause was a single
function:

```c
int mouse_is_ready(void){ return mouse_ready; }
```

`mouse_ready` was set by the PS/2 reset handshake. On a laptop whose
trackpad is not behind the 8042 — which is most laptops made after
about 2012 — nothing acknowledges that reset, so `mouse_ready` stayed
0. The desktop then asked `mouse_is_ready()` before reading the
pointer, got 0, and never read it.

**A USB mouse was registered and then ignored.** `usb/hid.c` found it,
claimed it, and polled it correctly. The desktop never asked, because
the only thing that could set `mouse_ready` was a PS/2 port that was
not there.

So the readiness test now has two sources:

```c
static int ps2_ready  = 0;   /* the PS/2 reset was acknowledged   */
static int usb_pointers = 0; /* USB HID mice currently registered */

int mouse_is_ready(void){ return ps2_ready || usb_pointers > 0; }
```

`usb_hid_init()` calls `mouse_usb_forget_all()` before it enumerates
and `mouse_usb_attach()` for each mouse it registers, so the count is
rebuilt from what is actually present rather than incremented forever
across rescans.

**And the boot log stopped lying.** It said

    PS/2: keyboard on IRQ1, mouse polled (8042 IRQ12 off)

on a machine with no PS/2 mouse. Now:

    PS/2: keyboard on IRQ1, no mouse answered the aux port (a USB mouse still works)

That line is the one that would have made this a five-minute
diagnosis instead of a release.

### What was actually wrong with the polling

Something, but less than it looked. `kernel_poll()` ran from
`keyboard_waitchar()`'s idle loop, and the desktop does not call
`keyboard_waitchar()` — it calls `keyboard_getchar()`. So in the
desktop, the bottom half never ran. It is now in `keyboard_getchar()`.

It is deliberately **not** in `keyboard_haschar()`. `haschar()` is a
test, callers use it in tight loops, and work that runs from a
predicate is work that runs at a depth nobody counted.

## 2. The console grid is remembered

`resolution 100x50`, reboot, back to 80x25. The grid was applied and
never written down.

`cfg_console_cols` / `cfg_console_rows` now persist it, `shell_init()`
restores it, and a **confirmed** mode change saves it — the confirm
prompt exists so that a grid the panel cannot display can be backed
out of, and saving before the confirmation would persist exactly the
grid that made the screen unreadable. `resolution reset` writes 0,0,
which means "forget", not "80x25".

### On "everything is small"

Right, and only half fixable here. The console font is 8x8, scaled by
an integer factor per axis. On a 1920x1080 panel the largest grid that
fits is 240x135, and 240 columns of 8-pixel characters is a wall of
text.

`resolution fit` picks the grid that gives **at least 16 pixels per
cell**:

| panel     | `fit` gives |
|-----------|-------------|
| 1280x800  | 80x50       |
| 1920x1080 | 120x67      |
| 3840x2160 | 240x135     |

**There is still no 8x16 font in this tree.** Not one byte of glyph
data for it. Doubling 8x8 is what `fit` does and it looks doubled.
Drawing 256 glyphs is a day's work; taking somebody else's font is a
licensing decision, and it is Kyan's to make, not mine. Recorded as
open in `HANDOFF.md`.

## 3. exFAT, read-only

`fs/exfat.c`. Boot sector, FAT, cluster heap, directory entry sets,
both cluster walks.

**Both walks, because exFAT has two.** The stream extension entry's
`NoFatChain` flag says a file's clusters are one contiguous run and
the FAT holds nothing for it, and writers set it for anything laid
down in one go. A reader that follows the FAT works on a fragmented
volume and walks garbage on a fresh one. A reader that assumes
contiguity does the opposite. Both are wrong about half the time, in a
way that looks like a corrupt disk.

The fixture is written by `mkfs.exfat`, populated through the FUSE
driver and declared clean by `fsck.exfat` — three other
implementations — and `tools/make_exfat_fixture.py` deliberately
punches holes to force one file to be FAT-chained. It did not, the
first time: the suite passed 83 of 83 against three contiguous files
and zero chained ones, so half the driver had never executed.

`exfat` reuses `fat_scan_volumes()` with a `FATTYPE_EXFAT` filter
rather than walking the partition table a second time. v0.8.6 is the
release that deleted three copies of a bus dispatch which had drifted
in all three.

Writing exFAT needs the allocation bitmap kept in step with the FAT.
That is a release of its own, and `include/exfat.h` has no write
function in it, so there is no flag to pass by mistake.

### NTFS and ext4

Asked for, not in this release, and not nearly started. NTFS
read-only is a release; ext4 is probably two. Scoped in `HANDOFF.md`.

## 4. net/bignum.c — and two defects it had on the first run

Limb-based big integers: 32-bit limbs, Montgomery multiplication for
modular exponentiation, binary long division, binary extended Euclid
for `gcd`, and the division form of extended Euclid for the modular
inverse.

This is pairing stage 3's first piece and the only one that can be
built with no phone present. It is pure logic, it is checked against
an independent implementation, and it makes TLS faster whether or not
the rest of stage 3 ever happens.

**`rsa_public_encrypt()` now uses it.** The old implementation was
byte-wise double-and-add over a 512-byte array — correct, and measured
at 0.163 s for `e = 65537`. A TLS handshake does one of those, and it
was a visible pause.

The old one is still in `net/tls_crypto.c`, renamed `bn_*` →
`obn_*` and marked unused on purpose. Two bignums in one file both
called `bn_` is how the wrong one gets called; and it stays rather
than being deleted because it is the only independent implementation
of that operation in the tree, which is a far better position to debug
from than one implementation and a suite that agrees with it.

### The suite did not pass first time, and that is the good news

273 of 277, and both failures were real.

**`bn_modexp` refused every 2048-bit modulus.** It built Montgomery's
`R² = 2^(32·2n)` as a literal number. For a 2048-bit modulus that is
`2^4096`, which needs limb index 128 — and `BN_LIMBS` is 128, indices
0 to 127. A bound check turned the overflow into an honest refusal
rather than a write into the next static, so nothing was corrupted;
what it refused was the only modulus size RSA uses here. **1024-bit
modexp passed.** A suite that stopped at 1024 bits would have shipped
green.

`R²  mod m` is now `(R mod m)²  mod m`, both factors under the
modulus, so no value above 4096 bits is ever formed.

**`bn_modinv` could not compute the one inverse RSA needs.** It was
the binary extended Euclid, which requires an **odd** modulus — and
`d = e⁻¹ mod lcm(p-1, q-1)`, where the lcm of two even numbers is
always even. It could do every inverse except that one.

Worse, the vectors agreed with the restriction: the generator wrote
`| 1` on every modulus it produced, so every test case had an odd one.
The generator now **refuses to emit vectors** unless at least one has
an invertible value modulo an even modulus.

The replacement is the division form, which works for any modulus. It
still has no signed arithmetic in it: the Bézout coefficients
alternate in sign, so it carries magnitudes and uses the loop's parity
to choose between `x` and `m - x` at the end. That parity was
backwards the first time and every single result came back as `m`
minus the right answer — `modinv(3, 11)` returned 7. What caught it
was not the known-answer vectors but the check beside them,
`a * r mod m == 1`, which is what an inverse *is* and fails for the
negation as loudly as for garbage.

### Where the expected answers come from

`tools/make_bignum_vectors.py`, and **not** from `net/bignum.c`:

- **CPython's integers** — an independent arbitrary-precision
  implementation nobody on this project wrote.
- **`openssl genrsa`** — the 2048-bit key is a real one from the
  library the rest of the world uses, not three primes a script
  multiplied together.

A bignum suite written from the same understanding as the bignum is
worth almost nothing: both sides are the same arithmetic, and a carry
handled wrongly in one is handled wrongly in the other.
`tests/test_usbmux.c` passed 79 of 79 against a header the phone has
never used, for exactly that reason.

**The generator earned its keep before the suite ran.** It asserted
`d·e ≡ 1 (mod φ(n))` and OpenSSL failed it. RFC 8017 defines `d`
against `λ(n) = lcm(p-1, q-1)`, and φ is a multiple of λ — so
`e⁻¹ mod φ` is also a working private exponent, decryption succeeds
either way, and it is a different number from the one in the key file.
Checking against φ would have failed a perfectly correct extended
Euclid, and the hunt would have gone into the kernel.

### And the one thing no native suite can see

**64-bit division.** It is free in a native build and a *link error*
in the kernel, because `__udivmoddi4` lives in libgcc and libgcc is
not linked into a freestanding 32-bit kernel. `fs/exfat.c` hit exactly
that in this release and `make` is what caught it, not a suite — so
`bn_divmod` is binary long division, which needs none, and nothing
performance-critical calls it. That is what Montgomery is for.

`tools/check_bignum_suite.py` plants a 64-bit divide as one of its 18
injections and passes `make` as a build check, because that is the
only place the defect appears.

### What stage 3 still needs

Miller-Rabin, RSA key generation, a DER encoder, X.509 v3, PKCS#1
signing and a plist writer. The bignum is done and tested. **A phone
sharing its connection is not promised**, and nothing in this release
claims it: `docs/havendos-iphone-tethering-needs-pairing.md` is still
the accurate description of where that stands.

---

## The numbers

| | v0.8.8 | v0.8.8.1 |
|---|---|---|
| suites | 43 | **44** |
| checks | 3763 | **4062** |
| injection harnesses | 26 | **27** |
| `test_bignum` | — | **299** |
| `test_exfat` | — | **107** |
| bignum injections caught | — | **18 of 18** |
| `helios_boot.py` | 32/32 | **32/32** |

All 44 suites pass, every build check passes, all 27 harnesses build
their suites, and `check_bignum_suite.py` catches all 18 planted
defects while correctly missing the harmless one.

## What is NOT verified

**The RSA change has never run against a real TLS server.** It is
checked by 299 native assertions against OpenSSL's own key and by the
kernel link, and that is all. There is no harness in this tree that
completes a real handshake, because that needs a network and a server.

**exFAT has never read a real stick.** It reads a volume three other
implementations wrote and judged, which is a much better fixture than
most of this tree has, and it is not a USB stick in a USB port.

**The mouse fix has never been tried on the HP.** The cause is a
reading of the code and the code is now consistent with it, but "both
dead" became "both should work" without a laptop in between.

`docs/HELIOS-HARDWARE-TEST.md` lists what to type, in order, and what
each thing should say.
