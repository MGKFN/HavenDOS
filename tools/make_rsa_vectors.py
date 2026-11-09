#!/usr/bin/env python3
"""make_rsa_vectors.py -- primes, composites, and the numbers chosen to
defeat a primality test that is not quite Miller-Rabin.

WHY THE HARD CASES ARE NAMED INDIVIDUALLY

A primality test is unusually easy to get almost right, and almost
right is indistinguishable from right on random input. Three ways:

  A FERMAT TEST. Compute a^(n-1) mod n, compare with 1, done. It
  accepts every CARMICHAEL NUMBER -- 561, 1105, 1729, 2465, 2821,
  6601, 8911 and infinitely many more -- for every base coprime to
  it. A random 1024-bit candidate search will produce one eventually,
  and the key it produces factors instantly.

  MILLER-RABIN WITH THE SQUARING LOOP MISSING. Compute a^q mod n for
  n-1 = q*2^s and compare with 1 and n-1, but never square. This is a
  Fermat test with extra steps and fails on exactly the same numbers.

  MILLER-RABIN WITH ONE FIXED BASE. Base 2 alone accepts every STRONG
  PSEUDOPRIME TO BASE 2 -- 2047, 3277, 4033, 4681, 8321, 15841. Base 3
  alone has its own set. The numbers below include both lists, so a
  test that happens to draw 2 every time fails here.

Every prime and composite below is classified by SymPy if it is
installed and by `openssl prime` otherwise -- not by this script's
own arithmetic, and not by net/rsa.c's.

THE CARMICHAEL AND PSEUDOPRIME LISTS ARE NOT GENERATED. They are the
published sequences (OEIS A002997 and A001262), typed in, because a
script that generated them would be using a primality test to produce
the test data for a primality test.

    python3 tools/make_rsa_vectors.py tests
"""
import os
import subprocess
import sys


def sh(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("make_rsa_vectors: %s failed:\n%s"
                 % (" ".join(cmd), r.stderr))
    return r.stdout


# ── the independent classifier ──────────────────────────────────────
_HAVE_SYMPY = False
try:
    from sympy import isprime as _isprime          # noqa: F401
    _HAVE_SYMPY = True
except Exception:
    pass


def classify(n):
    """Prime or not, decided by something that is not this script."""
    if _HAVE_SYMPY:
        from sympy import isprime
        return bool(isprime(n))
    out = sh(["openssl", "prime", "-hex", "%x" % n])
    low = out.strip().lower()
    if "is not prime" in low:
        return False
    if "is prime" in low:
        return True
    sys.exit("make_rsa_vectors: could not read `openssl prime` output: "
             + out)


def hx(v):
    if v == 0:
        return "{ 0 }"
    b = v.to_bytes((v.bit_length() + 7) // 8, "big")
    return "{ " + ",".join("0x%02x" % x for x in b) + " }"


# ── the published hard cases, typed in on purpose ───────────────────
#
# OEIS A002997, Carmichael numbers: composite n with a^(n-1) = 1 mod n
# for every a coprime to n. A Fermat test calls all of these prime.
CARMICHAEL = [561, 1105, 1729, 2465, 2821, 6601, 8911, 10585, 15841,
              29341, 41041, 46657, 52633, 62745, 63973, 75361]

# OEIS A001262, strong pseudoprimes to base 2: they survive a full
# Miller-Rabin round with the witness 2 and no other.
SPSP2 = [2047, 3277, 4033, 4681, 8321, 15841, 29341, 42799, 49141,
         52633, 65281, 74665, 80581, 85489, 88357, 90751]

# Strong pseudoprimes to base 3 -- a different list, so a test that
# swapped its one fixed base for another still fails.
SPSP3 = [121, 703, 1891, 3281, 8401, 8911, 10585, 12403, 16531,
         18721, 19345, 23521, 31621, 44287, 47197, 55969]

# The first primes, and primes just past the trial-division table, so
# the hand-off between "proved by trial division" and "tested by
# Miller-Rabin" is covered from both sides.
# 993997 and 994013 straddle 997*997 = 994009, which is where
# "surviving trial division proves it" stops and Miller-Rabin starts.
# The first version of this list had 994007 twice -- as itself and as
# `994009 - 2` -- and 994007 is composite (7 * 142001). The classifier
# refused to emit the vectors, which is the check working: a wrong
# "prime" in this list would have made a correct net/rsa.c look broken.
SMALL_PRIMES = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 97, 101,
                991, 997, 1009, 1013, 7919, 65521, 65537, 104729,
                993997, 994013]
SMALL_COMPOSITES = [0, 1, 4, 6, 8, 9, 15, 21, 25, 27, 49, 51, 91,
                    121, 169, 341, 1001, 9409, 994007, 994009]



# ════════════════════════════════════════════════════════════════════
# THE HARD GROUPS -- the ones that actually reach Miller-Rabin
# ════════════════════════════════════════════════════════════════════
#
# EVERY PSEUDOPRIME ABOVE IS SMALLER THAN 997 * 997, AND NONE OF THEM
# EXECUTES MILLER-RABIN AT ALL.
#
# net/rsa.c proves primality below 994009 by trial division alone --
# a composite under 997^2 must have a factor at or below 997 and the
# table holds every one of them, so rsa_is_probable_prime() returns
# before a single witness is chosen. 561, 2047, 3277 and the rest are
# all caught by a division, which is correct and is not the thing
# those vectors were put there to test.
#
# tools/check_rsa_suite.py found it: TEN of eighteen injections were
# MISSED, including "the witness is always 2", "an even modulus is
# accepted" and "a non-trivial square root of 1 is not treated as
# proof". All ten live in code the suite never ran. The suite passed
# 295 of 295 against a Miller-Rabin that was never called.
#
# That is the fixture being looser than the device, and it is the
# same defect shape as tools/make_exfat_fixture.py's first run --
# three contiguous files and no fragmented one, so half the driver
# never executed and the check that should have noticed passed.
#
# So these are CONSTRUCTED, not listed, and every one is asserted to
# have NO prime factor at or below 997. Trial division must hand them
# straight to Miller-Rabin.

def _is_prime(n):
    if n < 2:
        return False
    for q in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % q == 0:
            return n == q
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def _is_spsp(n, a):
    """n passes a Miller-Rabin round to base a. Composite-ness is the
    caller's to check -- a prime passes every base."""
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    x = pow(a, d, n)
    if x == 1 or x == n - 1:
        return True
    for _ in range(r - 1):
        x = x * x % n
        if x == n - 1:
            return True
    return False


def _survives_table(n):
    """No prime factor at or below 997 -- so net/rsa.c's trial
    division cannot refuse it and Miller-Rabin has to."""
    for q in range(2, 998):
        if n % q == 0:
            return False
    return True


def hard_spsp(base, want):
    """Strong pseudoprimes to `base` with both factors above the table.

    n = p(2p-1) with p and 2p-1 both prime. Checked rather than
    assumed: the construction is a good source of candidates and not
    a guarantee, so each one is tested and only the ones that pass
    are kept."""
    out = []
    p = 1009
    while len(out) < want and p < 1000000:
        if _is_prime(p) and _is_prime(2 * p - 1):
            n = p * (2 * p - 1)
            if _is_spsp(n, base) and not _is_prime(n) and _survives_table(n):
                out.append(n)
        p += 2
    if len(out) < want:
        sys.exit("make_rsa_vectors: only found %d hard base-%d "
                 "pseudoprimes" % (len(out), base))
    return out


def hard_carmichael(want):
    """Carmichael numbers with every factor above the table.

    Chernick: (6k+1)(12k+1)(18k+1) when all three are prime. Korselt's
    criterion is then CHECKED rather than taken on trust."""
    out = []
    k = 167                         # 6k+1 > 997
    while len(out) < want and k < 500000:
        a, b, c = 6 * k + 1, 12 * k + 1, 18 * k + 1
        if _is_prime(a) and _is_prime(b) and _is_prime(c):
            n = a * b * c
            if all((n - 1) % (q - 1) == 0 for q in (a, b, c)) \
               and _survives_table(n):
                out.append(n)
        k += 1
    if len(out) < want:
        sys.exit("make_rsa_vectors: only found %d hard Carmichael "
                 "numbers" % len(out))
    return out


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "tests"
    a = []
    w = a.append

    w("/* GENERATED by tools/make_rsa_vectors.py -- do not edit.")
    w("")
    w("   Primality of every value below was decided by SymPy or by")
    w("   `openssl prime`, never by net/rsa.c and never by the")
    w("   generator's own arithmetic.")
    w("")
    w("   The Carmichael numbers and the strong-pseudoprime lists are")
    w("   the published sequences (OEIS A002997, A001262), typed in --")
    w("   generating them would mean using a primality test to make")
    w("   the test data for a primality test. */")
    w("")
    w("typedef struct {")
    w("    const char          *what;")
    w("    const unsigned char *v; unsigned v_len;")
    w("    int                  prime;")
    w("} rsavec_t;")
    w("")

    groups = []

    def group(name, note, values, expect=None):
        rows = []
        for v in values:
            p = classify(v) if v > 1 else False
            if expect is not None and p != expect:
                sys.exit("make_rsa_vectors: %d was expected to be %s and "
                         "the classifier disagrees -- the list is wrong"
                         % (v, "prime" if expect else "composite"))
            rows.append((v, p))
        w("/* %s */" % note)
        for i, (v, p) in enumerate(rows):
            w("static const unsigned char %s_%d[] = %s;" % (name, i, hx(v)))
        w("static const rsavec_t %s[] = {" % name)
        for i, (v, p) in enumerate(rows):
            label = ("%d" % v) if v < 10**7 else ("a %d-bit value"
                                                  % v.bit_length())
            w('    { "%s", %s_%d, sizeof %s_%d, %d },'
              % (label, name, i, name, i, 1 if p else 0))
        w("};")
        w("#define %s_N ((int)(sizeof(%s)/sizeof(%s[0])))"
          % (name.upper(), name, name))
        w("")
        groups.append((name, len(rows)))

    group("RSA_SMALL_P",
          "the first primes, and primes either side of 997*997 -- the "
          "bound below which surviving trial division is a PROOF",
          SMALL_PRIMES, expect=True)

    group("RSA_SMALL_C", "composites small enough to check by hand",
          SMALL_COMPOSITES)

    group("RSA_CARMICHAEL",
          "CARMICHAEL NUMBERS (OEIS A002997). A Fermat test calls every "
          "one of these prime, for every base coprime to it. If these "
          "pass, net/rsa.c is a Fermat test wearing another name",
          CARMICHAEL, expect=False)

    group("RSA_SPSP2",
          "STRONG PSEUDOPRIMES TO BASE 2 (OEIS A001262). These survive "
          "a full Miller-Rabin round with the witness 2, so a test "
          "whose entropy source always hands it 2 accepts them",
          SPSP2, expect=False)

    group("RSA_SPSP3",
          "strong pseudoprimes to base 3 -- a DIFFERENT list, so "
          "swapping one fixed base for another does not help",
          SPSP3, expect=False)

    # ── and the same hazards ABOVE the trial-division table ─────────
    hc  = hard_carmichael(8)
    h2  = hard_spsp(2, 10)
    h3  = hard_spsp(3, 10)
    for nm, lst in (("hard carmichael", hc), ("hard spsp2", h2),
                    ("hard spsp3", h3)):
        for n in lst:
            if n <= 994009 or _is_prime(n) or not _survives_table(n):
                sys.exit("make_rsa_vectors: %s contains %d, which does "
                         "not reach Miller-Rabin" % (nm, n))

    group("RSA_HARD_CARMICHAEL",
          "CARMICHAEL NUMBERS WITH EVERY FACTOR ABOVE 997. The list "
          "above is all below 997*997, where net/rsa.c proves "
          "primality by trial division and never calls Miller-Rabin "
          "-- so those vectors test a division table, not a primality "
          "test. These reach it",
          hc, expect=False)

    group("RSA_HARD_SPSP2",
          "strong pseudoprimes to base 2 that trial division CANNOT "
          "refuse. These are what 'the witness is always 2' fails, "
          "and the small list does not",
          h2, expect=False)

    group("RSA_HARD_SPSP3",
          "the same, to base 3",
          h3, expect=False)

    # ── real RSA primes, from openssl ───────────────────────────────
    pem = os.path.join(out_dir, "rsakey.pem")
    sh(["openssl", "genrsa", "-out", pem, "2048"])
    txt = sh(["openssl", "rsa", "-in", pem, "-text", "-noout"])
    os.unlink(pem)

    fields, key, buf = {}, None, []
    labels = {"modulus": "n", "prime1": "p", "prime2": "q"}
    for line in txt.splitlines():
        s = line.strip()
        head = s.split(":")[0]
        if head in labels:
            if key:
                fields[key] = buf
            key, buf = labels[head], []
            continue
        if key is not None:
            if s and all(c in "0123456789abcdef:" for c in s):
                buf.append(s)
            else:
                fields[key] = buf
                key, buf = None, []
    if key:
        fields[key] = buf

    def val(nm):
        return int("".join(fields[nm]).replace(":", ""), 16)

    n, p, q = val("n"), val("p"), val("q")
    if p * q != n:
        sys.exit("make_rsa_vectors: openssl's p*q is not n -- parse is wrong")
    for v, want in ((p, True), (q, True), (n, False)):
        if classify(v) != want:
            sys.exit("make_rsa_vectors: the classifier disagrees with "
                     "openssl about a key component")

    w("/* A REAL 2048-BIT KEY's p, q and n, from `openssl genrsa`.")
    w("   p and q are 1024-bit primes the library chose; n is their")
    w("   product and must come back COMPOSITE. These are the only")
    w("   values here at the size the kernel will actually generate. */")
    for tag, v in (("p", p), ("q", q), ("n", n)):
        w("static const unsigned char RSA_KEY_%s[] = %s;" % (tag, hx(v)))
    w("")
    w("#define RSA_KEY_BITS 2048")
    w("")

    path = os.path.join(out_dir, "rsa_vectors.inc")
    open(path, "w", encoding="utf-8").write("\n".join(a) + "\n")
    print("make_rsa_vectors: %s" % path)
    print("  classifier: %s"
          % ("sympy" if _HAVE_SYMPY else "openssl prime"))
    for name, n_ in groups:
        print("  %-16s %d" % (name, n_))
    print("  plus a real 2048-bit key's p, q and n from openssl")
    return 0


if __name__ == "__main__":
    sys.exit(main())
