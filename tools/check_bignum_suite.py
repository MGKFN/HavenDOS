#!/usr/bin/env python3
"""check_bignum_suite.py -- prove tests/test_bignum.c can fail.

It did not pass first time, which for once is the reassuring version
of this file's opening line. 273 of 277, and the four failures were
two real defects:

  bn_modexp REFUSED EVERY 2048-BIT MODULUS. It built R^2 = 2^(32*2n)
  as a literal, which for n = 64 limbs is 2^4096 and needs limb index
  128 -- one past the end of a bn_t, because BN_LIMBS is 128. A bound
  check turned the overflow into an honest refusal rather than a write
  into the next static, so nothing was corrupted; what it refused was
  the only modulus size RSA uses here. 1024-bit modexp passed, which
  is why a suite that stopped at 1024 would have gone out green.

  bn_modinv COULD NOT COMPUTE THE ONE INVERSE RSA NEEDS. It was the
  binary extended Euclid, which requires an odd modulus -- and
  d = e^-1 mod lcm(p-1, q-1), where the lcm of two even numbers is
  always even. Every modulus in the generated vectors had `| 1` on it,
  so the vectors agreed with the restriction and only the real OpenSSL
  key disagreed.

Both are the same shape of mistake: arithmetic that is correct on
every size and parity the fixture happened to contain. That is what
this harness is for.

── WHY A BIGNUM NEEDS THIS MORE THAN ANYTHING ELSE IN THE TREE ───────

Wrong arithmetic here produces numbers that look exactly as random as
right arithmetic. There is no screen to look at, no log line that
reads wrong, and no shape a person would recognise -- a dropped carry
in limb 40 of a modular exponentiation gives a 2048-bit number, and
every 2048-bit number looks like every other one.

Worse, RSA is self-consistent under a broken implementation. If
modexp is wrong in the same way in both directions, m -> m^e -> ^d
still returns m, the suite passes, and the ciphertext is rejected by
every server on the internet. The round-trip check in the suite is
therefore NOT what protects this: the known-answer vectors are,
because they come from CPython and from `openssl genrsa`.

── WHAT A BUILD CHECK HAS TO CATCH INSTEAD OF A SUITE ────────────────

64-bit DIVISION. It is free in a native suite and a LINK ERROR in the
kernel, because __udivmoddi4 lives in libgcc and libgcc is not linked
into a freestanding 32-bit kernel. fs/exfat.c hit exactly that in this
same release and `make` caught it, not a suite -- so an injection that
plants a uint64_t divide is passed to `checks` below, where `make` is
what refuses it.

Run:  python3 tools/check_bignum_suite.py    (foreground, never &)

NEVER BACKGROUND IT. It restores net/bignum.c between injections; a
killed harness leaves a planted defect in the tree, and a planted
defect in THIS file is an RSA that produces ciphertext nobody can
decrypt.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "net/bignum.c"
TEST = "tests/test_bignum.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function",
      "net/bignum.c"]

BUGS = [
    # ── carries, which is where every bignum goes wrong ─────────────
    ("the final carry out of an addition is dropped",
     "a + b is right whenever the sum fits in the operands' limbs and "
     "silently loses 2^32 at every power-of-two boundary above them. "
     "0xFFFFFFFF + 1 becomes 0, which is a number",
     """    if(c){
        if(n >= BN_LIMBS){ r->n = n; norm(r); return 0; }
        r->v[n++] = (uint32_t)c;
    }""",
     """    if(0){
        if(n >= BN_LIMBS){ r->n = n; norm(r); return 0; }
        r->v[n++] = (uint32_t)c;
    }"""),

    ("the borrow in a subtraction is never propagated",
     "every limb is computed against the raw operand instead of the "
     "one the previous borrow reduced, so a - b is right only when no "
     "limb needs to borrow -- which is most random pairs",
     """        else      { r->v[i] = (uint32_t)(x + 0x100000000ull - y);
                    borrow = 1; }""",
     """        else      { r->v[i] = (uint32_t)(x + 0x100000000ull - y);
                    borrow = 0; }"""),

    ("the multiply drops the high half of each limb product",
     "32x32 -> 64 is the whole of a bignum multiply; keeping 32 bits "
     "of it gives a plausible number for any single-limb input and "
     "garbage for everything else",
     """            t.v[i + j] = (uint32_t)p;
            carry = p >> 32;""",
     """            t.v[i + j] = (uint32_t)p;
            carry = 0;"""),

    # ── the one that gives ALMOST the right answer ───────────────────
    ("the multiply accumulates into the destination while reading it",
     "the aliasing case. bn_mul(&x, &x, &x) is how squaring is "
     "written and the header promises it works; an implementation "
     "that writes r as it reads a is wrong in a way that is close "
     "enough to the right answer to look like a number",
     """    static bn_t t;
    uint32_t i, j;

    if(a->n + b->n > BN_LIMBS) return 0;
    bn_zero(&t);""",
     """    bn_t *tp = r;
#define t (*tp)
    uint32_t i, j;

    if(a->n + b->n > BN_LIMBS) return 0;
    bn_zero(&t);"""),

    # ── the normalised form ─────────────────────────────────────────
    ("the limb count keeps its leading zero limbs",
     "two bn_t holding the same value compare unequal when one was "
     "built from a padded encoding and the other from a minimal one. "
     "A certificate then verifies on one path and not on another, "
     "which reads as a network fault",
     """static void norm(bn_t *a)""",
     """static void norm_unused(bn_t *a)"""),

    # ── division ────────────────────────────────────────────────────
    ("the remainder is not reduced below the divisor",
     "binary long division's one invariant. The quotient comes out "
     "right and the remainder comes out one multiple of m too large, "
     "so q*m + rem still equals a and only a comparison notices",
     """        if(bn_cmp(&rr, m) >= 0){""",
     """        if(0){"""),

    ("division by zero returns a quotient instead of refusing",
     "a refusal has to be a refusal. Arithmetic on a zero divisor "
     "here loops to the top of the number and returns something",
     """    static bn_t qq, rr;
    uint32_t i, bits;

    if(bn_is_zero(m)) return 0;""",
     """    static bn_t qq, rr;
    uint32_t i, bits;

    if(0) return 0;"""),

    # ── Montgomery ──────────────────────────────────────────────────
    ("the Montgomery constant is off by one Newton doubling",
     "n0inv needs 32 correct bits and four doublings give 16. The "
     "reduction then leaves a value that is not congruent, and modexp "
     "returns a wrong 2048-bit number -- which is indistinguishable "
     "from a right one by eye",
     """    for(i = 0; i < 5; i++) x *= 2u - m0 * x;""",
     """    for(i = 0; i < 4; i++) x *= 2u - m0 * x;"""),

    ("the conditional subtraction after a Montgomery multiply is skipped",
     "CIOS leaves a result in [0, 2m) and the caller is promised "
     "[0, m). Dropping it gives answers that are correct modulo m but "
     "not reduced, so they compare unequal to the expected value and "
     "feed an out-of-range operand into the next multiply",
     """    if(bn_cmp(r, m) >= 0) bn_sub(r, r, m);
    return 1;
}

int bn_modexp""",
     """    return 1;
}

int bn_modexp"""),

    ("modexp accepts an even modulus",
     "Montgomery has no constant for one -- mont_n0inv is the inverse "
     "of -m mod 2^32 and does not exist when m is even. The refusal "
     "is the only correct answer; without it modexp returns a number",
     """    if(bn_is_zero(m) || !bn_is_odd(m)) return 0;""",
     """    if(bn_is_zero(m)) return 0;"""),

    ("the exponent is scanned least-significant bit first",
     "square-and-multiply only works from the top down. Bottom-up "
     "gives a^(reversed bits), which for a palindromic exponent is "
     "RIGHT -- and 65537 is 0b10000000000000001, which is a "
     "palindrome, so the public operation still passes and only the "
     "private exponent fails",
     """    bits = bn_bits(e);
    for(i = bits; i > 0; i--){""",
     """    bits = bn_bits(e);
    for(i = 1; i <= bits; i++){"""),

    # ── the inverse ─────────────────────────────────────────────────
    ("the modular inverse comes back negated",
     "m - x instead of x. This is the defect the first division-form "
     "bn_modinv actually had: modinv(3, 11) returned 7. The "
     "known-answer vectors catch it, and so does the a * r mod m == 1 "
     "check next to them, which is what an inverse IS",
     """    if(parity == 0u || bn_is_zero(&xp)){""",
     """    if(parity == 1u || bn_is_zero(&xp)){"""),

    ("bn_modinv refuses an even modulus",
     "the binary extended Euclid's restriction, which this release "
     "replaced the algorithm to escape. d = e^-1 mod lcm(p-1, q-1) "
     "and that lcm is always even, so this refuses the only inverse "
     "RSA key generation needs -- while passing every vector whose "
     "modulus was generated with `| 1` on it",
     """    if(bn_is_zero(m)) return 0;

    /* m == 1: everything is 0 mod 1, and 0 is its inverse there.""",
     """    if(bn_is_zero(m) || !bn_is_odd(m)) return 0;

    /* m == 1: everything is 0 mod 1, and 0 is its inverse there."""),

    ("no inverse is reported as an error instead of as the answer",
     "gcd(a, m) != 1 means there is no inverse, which a keygen loop "
     "uses to reject a candidate. Returning success with a wrong "
     "value makes the loop accept it",
     """    if(bn_cmp(&u, &t) != 0) return 0;""",
     """    if(0) return 0;"""),

    ("gcd loses the common powers of two",
     "Stein's algorithm factors them out at the start and has to put "
     "them back. Without the shift, gcd(2^20, 2^14) is 1 -- and "
     "gcd(e, lambda) is 1 for a valid key, so this looks correct on "
     "exactly the input a keygen cares about",
     """    bn_shl(r, &x, shift);""",
     """    bn_copy(r, &x);"""),

    # ── bytes in and out ────────────────────────────────────────────
    ("bn_to_bytes truncates instead of refusing a short buffer",
     "a silently truncated modulus is the worst thing this file can "
     "produce. It is still a number and it still encrypts, and the "
     "result is a key nobody can decrypt with",
     """    if(bn_bits(a) > len * 8u) return 0;""",
     """    if(0) return 0;"""),

    # ── NOT A DEFECT, and recorded ──────────────────────────────────
    #
    # "a whole-limb left shift shifts by 32 within a limb" was written
    # as an injection against the FIRST half of the shift:
    #
    #     t.v[i + words] |= (sb ? (a->v[i] << sb) : a->v[i]);
    #                  ->  t.v[i + words] |= (uint32_t)(a->v[i] << sb);
    #
    # and reported MISSED. It is missed because it is not a defect:
    # x << 0 is x, and that ternary is redundant. The branch that
    # matters is the one on the line BELOW it -- `>> (32u - sb)` is a
    # shift by 32 when sb is 0, which is undefined in C and on x86
    # returns the operand unchanged -- and that one is already behind
    # `if(sb)`. The injection below removes THAT guard, which is the
    # real hazard, and the suite catches it.
    #
    # Left as a note because v0.8.6.3's harness proved two of its ten
    # injections not to be defects, and an injection deleted without a
    # word is one somebody writes again.

    ("a whole-limb left shift shifts a limb right by 32",
     "the second half of the shift reaches for the bits carried into "
     "the next limb with `>> (32 - sb)`, which is a shift by 32 when "
     "sb is 0 -- undefined in C, and on x86 it returns the operand "
     "unchanged. Every whole-limb shift then duplicates each limb "
     "into the one above it, so 1 << 32 is 2^64 + 2^32",
     """        if(sb) t.v[i + words + 1] |= (uint32_t)(a->v[i] >> (32u - sb));""",
     """        t.v[i + words + 1] |= (uint32_t)(a->v[i] >> (32u - sb));"""),

    # ── and the one a suite cannot see at all ───────────────────────
    #
    # Caught by `make`, not by the suite: __udivmoddi4 is not linked
    # into a freestanding 32-bit kernel, so a 64-bit divide is a LINK
    # error. It is free in a native build, which is why this is in
    # `checks` rather than left to tests/test_bignum.c.
    ("a 64-bit division is used to reduce a limb",
     "free in a native suite and a LINK ERROR in the kernel, because "
     "__udivmoddi4 lives in libgcc. fs/exfat.c hit this in the same "
     "release and `make` is what caught it; no native suite can",
     """    n0inv = mont_n0inv(m->v[0]);""",
     """    { uint64_t d_ = ((uint64_t)m->v[0] << 32) | 3u;
      n0inv = (uint32_t)(d_ / (uint64_t)(m->v[0] | 1u)); }"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the arithmetic changed",
    """int bn_is_odd(const bn_t *a)""",
    """/* The low bit, which is all parity is. */
int bn_is_odd(const bn_t *a)""")

# `make` is part of this harness's coverage, because the 64-bit
# division defect is invisible to any native build. The kernel link
# is the only place it shows up.
CHECKS = (
    ["make"],
)

if __name__ == "__main__":
    sys.exit(inject.run(
        "bignum", SRC, TEST, CC, BUGS, HARMLESS, timeout=180,
        checks=CHECKS,
        preamble="Proving tests/test_bignum.c can fail.\n"
                 "It did not pass first time -- 273 of 277, and both\n"
                 "failures were real: modexp refused every 2048-bit\n"
                 "modulus, and modinv could not do the one inverse RSA\n"
                 "needs. Wrong arithmetic here produces numbers that\n"
                 "look exactly as random as right arithmetic, so the\n"
                 "vectors come from CPython and openssl, not from us."))
