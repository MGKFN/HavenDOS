#!/usr/bin/env python3
"""check_rsa_suite.py -- prove tests/test_rsa.c can fail.

It passed 295 of 295 on its first run, which is the least trustworthy
result this project produces. net/bignum.c's suite failed on its first
run and both failures were real defects; this one did not, and the
only way to tell a good suite from a lucky one is to break the code
underneath it on purpose.

── WHAT MAKES PRIMALITY DIFFERENT FROM EVERY OTHER FILE HERE ─────────

A wrong primality test is CORRECT ON ALMOST ALL INPUT. A Fermat test
agrees with Miller-Rabin on every number except the Carmichael
numbers; the first is 561, and the density of them among 1024-bit
candidates is small enough that a keygen could run for a long time
before producing one, and large enough that it eventually will. The
key it produces then factors instantly.

So the injections below are not "does it still compute". They are:
does the suite notice when the test degrades into a WEAKER but still
usually-correct test. The three degradations that matter each have a
published list of numbers that defeat them, and
tools/make_rsa_vectors.py carries all three:

  Fermat                  -> the Carmichael numbers (OEIS A002997)
  one fixed base          -> the strong pseudoprimes to it (A001262)
  no squaring loop        -> the Carmichael numbers again

An injection that the suite misses here is not a cosmetic gap. It is
a key that looks fine.

── AND TWO THAT ARE NOT ABOUT PRIMALITY AT ALL ───────────────────────

A 2047-bit modulus from a "2048-bit" keygen is accepted by some peers
and rejected by others without explanation, and the cause is two
candidate primes whose top bits were not forced. And a 64-bit
division anywhere in this file is a LINK error in the kernel and free
in a native build -- `make` is passed as a build check for that one,
because no native suite can see it.

Run:  python3 tools/check_rsa_suite.py    (foreground, never &)

NEVER BACKGROUND IT. It restores net/rsa.c between injections, and a
killed harness leaves a planted defect in the tree -- here, a planted
defect is a weakened primality test, which is the one kind of bug
that produces no symptom at all until somebody factors the key.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "net/rsa.c"
TEST = "tests/test_rsa.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function",
      "net/bignum.c", "net/rsa.c"]

BUGS = [
    # ── the three degradations ──────────────────────────────────────
    ("Miller-Rabin becomes a Fermat test",
     "the squaring loop is the whole test. Without it this computes "
     "a^q mod n, compares with 1 and n-1, and accepts EVERY Carmichael "
     "number for every base coprime to it -- 561, 1105, 1729 and "
     "infinitely many more. It agrees with the real test on all other "
     "input, which is what makes it dangerous rather than broken",
     """    for(i = 1; i < s; i++){
        if(!bn_mulmod(&x, &x, &x, n)) return 1;""",
     """    for(i = 1; i < 1; i++){
        if(!bn_mulmod(&x, &x, &x, n)) return 1;"""),

    ("the witness is always 2",
     "one fixed base accepts every strong pseudoprime to that base -- "
     "2047, 3277, 4033, 4681. A suite whose entropy source happens to "
     "produce varied witnesses would never notice, which is why "
     "tests/test_rsa.c also runs its sets with the source pinned",
     """static void mr_random_witness(bn_t *a, const bn_t *n)
{""",
     """static void mr_random_witness(bn_t *a, const bn_t *n)
{
    (void)n; bn_set_u32(a, 2); return;"""),

    ("a non-trivial square root of 1 is not treated as proof",
     "reaching x == 1 before n-1 means a square root of 1 that is "
     "neither 1 nor -1, which only a composite modulus has. Treating "
     "it as 'witness satisfied' turns a proof of compositeness into a "
     "pass",
     """        if(x.n == 1 && x.v[0] == 1) return 0;""",
     """        if(x.n == 1 && x.v[0] == 1) return 1;"""),

    ("the witness loop runs once whatever was asked",
     "rounds is the caller's confidence. Honouring only the first "
     "witness makes a 64-round test a 1-round test, and the keygen "
     "keeps primes on the strength of 64",
     """    for(r = 0; r < rounds; r++){""",
     """    for(r = 0; r < 1; r++){"""),

    # ── trial division ──────────────────────────────────────────────
    ("trial division accepts everything",
     "not a correctness defect on its own -- Miller-Rabin catches what "
     "it misses -- but it is the pre-filter that makes a keygen "
     "seconds instead of minutes, and a suite that cannot tell it "
     "stopped working cannot tell when it is reintroduced wrongly",
     """        if(mod_u32(n, p) == 0) return 0;""",
     """        if(0) return 0;"""),

    ("a value equal to a small prime is called composite",
     "the table is divisors, and a candidate that IS 7 is divisible "
     "by 7. Getting the order wrong makes every small prime composite",
     """        if(n->n == 1 && n->v[0] == p) return 1;""",
     """        if(0) return 1;"""),

    ("mod_u32 carries the remainder only within a limb",
     "the remainder has to cross from one limb to the next. Reset it "
     "per limb and the answer is right for any single-limb value and "
     "wrong for every real candidate -- which is the direction that "
     "LOSES composites rather than primes",
     """    for(i = n->n; i > 0; i--){
        uint32_t w = n->v[i - 1];""",
     """    for(i = n->n; i > 0; i--){
        uint32_t w = n->v[i - 1];
        rem = 0;"""),

    ("the trial-division proof bound is wrong by a square",
     "surviving trial division proves primality only below 997*997. "
     "Raising the bound returns 'prime' for composites whose smallest "
     "factor is above the table -- 1009*1009 among them",
     """    if(n->n == 1 && n->v[0] < 994009u) return 1;    /* 997 * 997 */""",
     """    if(n->n == 1 && n->v[0] < 99400900u) return 1;"""),

    # ── even and degenerate input ───────────────────────────────────
    # ── NOT A DEFECT, and recorded ──────────────────────────────────
    #
    # "an even modulus is accepted into Miller-Rabin" was written as
    # an injection -- remove `if(!bn_is_odd(n)) return 0;` from
    # rsa_is_probable_prime() -- and reported MISSED.
    #
    # It is missed because rsa_trial_divide(), on the next line but
    # one, opens with the SAME test and returns 0 for any even value.
    # There is no even n that passes the second check and would have
    # failed the first, so removing one of them changes no answer.
    # n == 2 is handled earlier still, by its own line.
    #
    # The check STAYS. It is the one that documents why an even
    # modulus must never reach bn_modexp -- Montgomery has no
    # constant for one, every witness would come back "satisfied",
    # and 4 would be probably prime -- and it is what still holds if
    # somebody later makes trial division cheaper by dropping its
    # parity test. But it is redundant defence today, not a tested
    # behaviour, and tests/test_rsa.c's even-number checks pass
    # through trial division rather than through this line.
    #
    # Left as a note because v0.8.6.3's harness proved two of its ten
    # injections not to be defects, and an injection deleted without
    # a word is one somebody writes again.

    ("the whole-limb branch forces only the top bit",
     "512, 1024 and 2048 all give a prime width that is a whole "
     "number of limbs, so the `else` arm is the one every supported "
     "key size takes -- and it had no check of its own. With one bit "
     "forced instead of two, p*q is 2b-1 bits about half the time: a "
     "2047-bit 'RSA-2048' key, which some peers accept and others "
     "reject without saying why",
     """        k->cand.v[limbs - 1] |= 0xC0000000u;""",
     """        k->cand.v[limbs - 1] |= 0x80000000u;"""),

    # ── keygen: the key's own properties ────────────────────────────
    ("the candidate's top two bits are not forced",
     "p and q each just above 2^(b-1) give a modulus of 2b-1 bits -- a "
     "2047-bit 'RSA-2048' key. Some peers accept it, others reject it "
     "without saying why, and it is intermittent because it depends on "
     "where in the range the two primes landed",
     """        k->cand.v[limbs - 1] |= (uint32_t)(1u << (top - 1u));
        if(top >= 2u) k->cand.v[limbs - 1] |= (uint32_t)(1u << (top - 2u));""",
     """        k->cand.v[limbs - 1] |= (uint32_t)(1u << (top - 1u));"""),

    ("the candidate is not forced odd",
     "an even candidate is refused by rsa_trial_divide, so the search "
     "discards half its candidates for nothing -- and if the even "
     "refusal above were also wrong, it would accept 'primes' "
     "divisible by two",
     """    k->cand.v[0] |= 1u;                  /* odd */""",
     """    /* no parity forced */"""),

    ("p and q are allowed to be equal",
     "n = p^2 is factored by a square root. Vanishingly unlikely from "
     "a working entropy source, which is exactly why the check is "
     "there -- a stuck source makes it certain",
     """        if(bn_cmp(&k->cand, &k->p) == 0){""",
     """        if(0){"""),

    ("d is computed against phi instead of lambda",
     "both are working private exponents and both decrypt, so a round "
     "trip passes either way. They are DIFFERENT NUMBERS, and the one "
     "a peer reconstructs from the key file is the lambda one -- "
     "net/bignum.c's header records the afternoon this cost",
     """        if(!bn_divmod(&k->lam, &g, &k->lam, 0)) goto arith;""",
     """        /* lambda = phi */"""),

    ("the primes are kept after one witness",
     "rounds_needed is 64 because the key goes into a certificate a "
     "phone will trust for as long as the pair record lasts. One "
     "witness is the right number for rejecting candidates and the "
     "wrong number for accepting them",
     """    k->rounds_needed = 64;""",
     """    k->rounds_needed = 1;"""),

    ("the candidate budget is never reached",
     "a stuck entropy source then spins for ever instead of refusing "
     "by name, and a keygen that never returns is indistinguishable "
     "from a hung machine. A budget that cannot be reached is not a "
     "budget",
     """        if(k->tried >= KG_BUDGET){""",
     """        if(0){"""),

    ("a finished generator restarts when stepped again",
     "the caller's loop ends on RSA_KG_DONE, and anything that polls "
     "the state afterwards would silently begin a second keygen",
     """    case KG_DONE:
        return RSA_KG_DONE;""",
     """    case KG_DONE:
        k->stage = KG_NEW_CANDIDATE;
        return RSA_KG_WORKING;"""),

    ("an unsupported modulus size is accepted",
     "rsa_keygen_begin's bounds are what keeps prime_bits inside a "
     "bn_t. Accepting BN_BITS means candidates that cannot be "
     "multiplied together",
     """    if(modulus_bits < 512u || modulus_bits > (uint32_t)(BN_BITS / 2))
        return RSA_EBITS;""",
     """    if(0) return RSA_EBITS;"""),

    # ── the one no native suite can see ─────────────────────────────
    #
    # mod_u32's first version did exactly this and the comment above it
    # claimed gcc would emit a single `divl`. It does not: both
    # operands are 64-bit by the time the % happens, so it emits
    # __udivmoddi4, which is in libgcc and is not linked into a
    # freestanding 32-bit kernel. `make` is passed as a build check
    # below because that is the only place the defect exists.
    ("mod_u32 takes 32 bits at a time with a 64-bit remainder",
     "free in a native build and a LINK ERROR in the kernel: "
     "__udivmoddi4 lives in libgcc. This is the exact code the first "
     "version of mod_u32 had, under a comment warning against it",
     """        uint32_t w = n->v[i - 1];
        rem = ((rem << 16) | (w >> 16))    % d;
        rem = ((rem << 16) | (w & 0xFFFFu)) % d;""",
     """        uint64_t acc = ((uint64_t)rem << 32) | (uint64_t)n->v[i - 1];
        rem = (uint32_t)(acc % (uint64_t)d);"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the arithmetic or the search changed",
    """const char *rsa_why_name(int why)""",
    """/* Every refusal has a name. */
const char *rsa_why_name(int why)""")

# `make` is part of this harness's coverage: the 64-bit division
# defect is invisible to every native build, and the kernel link is
# the only place it appears.
CHECKS = (
    ["make"],
)

if __name__ == "__main__":
    sys.exit(inject.run(
        "rsa", SRC, TEST, CC, BUGS, HARMLESS, timeout=600,
        checks=CHECKS,
        preamble="Proving tests/test_rsa.c can fail.\n"
                 "It passed 295 of 295 first time, which is the least\n"
                 "trustworthy result this project produces. A wrong\n"
                 "primality test is CORRECT ON ALMOST ALL INPUT -- a\n"
                 "Fermat test differs from Miller-Rabin only on the\n"
                 "Carmichael numbers -- so these injections ask whether\n"
                 "the suite notices the test DEGRADING, not whether it\n"
                 "notices it breaking."))
