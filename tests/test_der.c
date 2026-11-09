/* ════════════════════════════════════════════════════════════════════
   test_der.c -- writing DER, checked against the bytes.

   net/der.c calls nothing but memcpy, memmove and the bignum's byte
   export, so it is linked directly here -- no seam, no stub, no
   simulated hardware. Same arrangement as shell/mines_model.c and for
   the same reason.

   ── EVERY EXPECTED BYTE STRING IS WRITTEN OUT BY HAND ───────────────

   Not computed. The whole hazard in an encoder suite is writing the
   expectation with the same arithmetic as the encoder, which is what
   tests/test_usbmux.c did at 0.8.7.6 when it "passed 79 of 79 against
   a sixteen-byte header the phone has never used".

   So the vectors below are literal hex, taken from the rules in
   X.690 rather than from this encoder: `02 01 00` is INTEGER zero
   because a DER INTEGER has at least one content byte and leading
   zeroes are stripped, not because der_integer() happens to emit it.

   ── THE TWO THAT ACTUALLY BREAK THINGS ──────────────────────────────

   THE SIGN BYTE. A DER INTEGER is signed, so a 2048-bit RSA modulus
   -- top bit set, by definition of being 2048 bits -- must carry a
   leading 0x00 and encode as 257 content bytes. Without it the
   certificate is rejected by everything, and the hex dump looks
   perfectly reasonable.

   THE MINIMAL LENGTH. 200 bytes of content is `81 C8`. `82 00 C8`
   parses as BER, is not DER, and a strict parser refuses it -- while
   a lenient one accepts it and nothing ever says which kind the phone
   has.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdint.h>

/* ── THE KERNEL'S OWN memcpy AND memmove, NOT THE HOST'S ──────────────
   This matters and the injection harness is what showed it.

   net/der.c's der_seq_end() moves a sequence's contents up to make
   room for the header, and the regions overlap whenever the header is
   shorter than the contents -- which is almost always. Writing that
   as memcpy instead of memmove is a real defect, and the harness
   planted it and the suite DID NOT NOTICE, because glibc's memcpy
   happens to survive this overlap.

   The kernel does not use glibc. kernel/string.c's memcpy is

       while(n--) *d++ = *s++;

   -- strictly forward, byte by byte, as its own v0.7.12 note says --
   so with the destination ahead of the source it clobbers what it has
   not read yet, deterministically. Linking it here tests the memcpy
   that actually ships instead of one that does not.

   <string.h> is deliberately not included: the declarations would
   conflict, and strlen/memcmp come from the same file. */
#include "../kernel/string.c"

#include "../include/der.h"
#include "../net/der.c"
#include "../net/bignum.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %s%*sgot %ld\n", what,
                           (int)(what && strlen(what) < 54
                                 ? 56 - (int)strlen(what) : 2), "", got); }
}

static uint8_t  B[8192];
static der_t    D;

static void reset(void){ memset(B, 0, sizeof B); der_init(&D, B, sizeof B); }

/* Compare what was written against a literal byte string. */
static int is(const uint8_t *want, uint32_t n)
{
    if(!der_ok(&D)) return 0;
    if(der_len(&D) != n) return 0;
    return memcmp(B, want, n) == 0;
}
static void show(uint32_t n)
{
    uint32_t i;
    printf("          got:");
    for(i = 0; i < der_len(&D) && i < n + 8; i++) printf(" %02X", B[i]);
    printf("  (%u bytes)\n", der_len(&D));
}
/* Variadic because the expected bytes are a brace initialiser and its
   commas would otherwise be read as macro arguments. */
#define CKB(what, ...) do {                                             \
        static const uint8_t w_[] = __VA_ARGS__;                        \
        int ok_ = is(w_, (uint32_t)sizeof w_);                          \
        ck(what, ok_, (long)der_len(&D));                               \
        if(!ok_) show((uint32_t)sizeof w_);                             \
    } while(0)

int main(void)
{
    printf("test_der: writing DER, checked against the bytes\n");

    /* ══ INTEGER: the sign byte and the stripped zeroes ═════════════ */
    head("INTEGER, which is signed and minimal");

    reset(); der_integer_u32(&D, 0);
    CKB("zero is 02 01 00, not 02 00", { 0x02, 0x01, 0x00 });

    reset(); der_integer_u32(&D, 1);
    CKB("one is 02 01 01", { 0x02, 0x01, 0x01 });

    reset(); der_integer_u32(&D, 127);
    CKB("127 needs no sign byte", { 0x02, 0x01, 0x7F });

    reset(); der_integer_u32(&D, 128);
    CKB("128 DOES -- its top bit is set", { 0x02, 0x02, 0x00, 0x80 });

    reset(); der_integer_u32(&D, 255);
    CKB("255 is 02 02 00 FF", { 0x02, 0x02, 0x00, 0xFF });

    reset(); der_integer_u32(&D, 256);
    CKB("256 is 02 02 01 00 -- no sign byte needed",
        { 0x02, 0x02, 0x01, 0x00 });

    reset(); der_integer_u32(&D, 65537);
    CKB("65537, the RSA public exponent, is 02 03 01 00 01",
        { 0x02, 0x03, 0x01, 0x00, 0x01 });

    {
        /* Leading zeroes in the INPUT are stripped, which is how a
           fixed-width bignum export becomes a minimal INTEGER. */
        static const uint8_t padded[] = { 0, 0, 0, 0x2A };
        reset(); der_integer(&D, padded, sizeof padded);
        CKB("leading zeroes in the input are stripped",
            { 0x02, 0x01, 0x2A });
    }
    {
        static const uint8_t allzero[] = { 0, 0, 0, 0, 0, 0 };
        reset(); der_integer(&D, allzero, sizeof allzero);
        CKB("an all-zero input is still 02 01 00",
            { 0x02, 0x01, 0x00 });
    }

    /* ══ THE 2048-BIT MODULUS ═══════════════════════════════════════ */
    head("a 2048-bit modulus: 257 content bytes, not 256");
    {
        /* Every byte 0xFF, so the top bit is certainly set and the
           length is certainly 256 before the sign byte. The content
           must be 0x00 followed by 256 bytes, and the length must be
           the long form `82 01 01` -- 257. */
        static uint8_t m[256];
        uint32_t i;
        for(i = 0; i < sizeof m; i++) m[i] = 0xFF;
        reset(); der_integer(&D, m, sizeof m);
        ck("the whole thing is 4 header + 257 content bytes",
           der_len(&D) == 4 + 257, (long)der_len(&D));
        ck("  tag 02", B[0] == 0x02, B[0]);
        ck("  length is the long form, two bytes: 82",
           B[1] == 0x82, B[1]);
        ck("  and those two bytes are 01 01, which is 257",
           B[2] == 0x01 && B[3] == 0x01, (B[2] << 8) | B[3]);
        ck("  the FIRST content byte is the sign byte 00",
           B[4] == 0x00, B[4]);
        ck("  and the modulus follows it intact",
           memcmp(B + 5, m, sizeof m) == 0, 0);
    }
    {
        /* And the same length with the top bit CLEAR needs no sign
           byte: 256 content bytes, `82 01 00`. The pair is what
           separates "always pads" from "pads when signed". */
        static uint8_t m[256];
        uint32_t i;
        m[0] = 0x7F;
        for(i = 1; i < sizeof m; i++) m[i] = 0xFF;
        reset(); der_integer(&D, m, sizeof m);
        ck("a 256-byte value with the top bit CLEAR gets no sign byte",
           der_len(&D) == 4 + 256, (long)der_len(&D));
        ck("  and its length bytes are 01 00, which is 256",
           B[2] == 0x01 && B[3] == 0x00, (B[2] << 8) | B[3]);
        ck("  with the value starting immediately",
           B[4] == 0x7F, B[4]);
    }

    /* ══ LENGTHS ════════════════════════════════════════════════════ */
    head("the length must be minimal, or it is BER and not DER");
    {
        static uint8_t big[300];
        memset(big, 0x41, sizeof big);

        reset(); der_tlv(&D, DER_OCTET_STRING, big, 127);
        ck("127 bytes: one length byte, 7F",
           der_len(&D) == 2 + 127 && B[1] == 0x7F, B[1]);

        reset(); der_tlv(&D, DER_OCTET_STRING, big, 128);
        ck("128 bytes: 81 80 -- the short form stops at 127",
           der_len(&D) == 3 + 128 && B[1] == 0x81 && B[2] == 0x80,
           (B[1] << 8) | B[2]);

        reset(); der_tlv(&D, DER_OCTET_STRING, big, 200);
        ck("200 bytes: 81 C8, and NOT 82 00 C8",
           der_len(&D) == 3 + 200 && B[1] == 0x81 && B[2] == 0xC8,
           (B[1] << 8) | B[2]);

        reset(); der_tlv(&D, DER_OCTET_STRING, big, 255);
        ck("255 bytes: 81 FF",
           der_len(&D) == 3 + 255 && B[1] == 0x81 && B[2] == 0xFF,
           (B[1] << 8) | B[2]);

        reset(); der_tlv(&D, DER_OCTET_STRING, big, 256);
        ck("256 bytes: 82 01 00 -- two length bytes, no leading zero",
           der_len(&D) == 4 + 256 && B[1] == 0x82 &&
           B[2] == 0x01 && B[3] == 0x00, B[1]);
    }

    /* ══ OID ════════════════════════════════════════════════════════ */
    head("OBJECT IDENTIFIER, with the first two arcs folded together");
    {
        /* 1.2.840.113549.1.1.11 -- sha256WithRSAEncryption, which is
           the signature algorithm stage 3 will name. The bytes are
           from X.690's rules: 1*40+2 = 42 = 0x2A, then 840 as
           86 48, 113549 as 86 F7 0D, then 01 01 0B. */
        static const uint32_t sha256rsa[] = {1,2,840,113549,1,1,11};
        reset(); der_oid(&D, sha256rsa, 7);
        CKB("1.2.840.113549.1.1.11 is 2A 86 48 86 F7 0D 01 01 0B",
            { 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D,
              0x01, 0x01, 0x0B });
    }
    {
        /* 1.2.840.113549.1.1.1 -- rsaEncryption, the key algorithm. */
        static const uint32_t rsa[] = {1,2,840,113549,1,1,1};
        reset(); der_oid(&D, rsa, 7);
        CKB("1.2.840.113549.1.1.1 is the same but ending 01",
            { 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D,
              0x01, 0x01, 0x01 });
    }
    {
        /* 2.5.4.3 -- commonName. 2*40+5 = 85 = 0x55. */
        static const uint32_t cn[] = {2,5,4,3};
        reset(); der_oid(&D, cn, 4);
        CKB("2.5.4.3 (commonName) is 55 04 03",
            { 0x06, 0x03, 0x55, 0x04, 0x03 });
    }
    {
        /* An arc of exactly 128 is the base-128 boundary: 81 00. */
        static const uint32_t edge[] = {1,2,128};
        reset(); der_oid(&D, edge, 3);
        CKB("an arc of 128 is 81 00, the base-128 boundary",
            { 0x06, 0x03, 0x2A, 0x81, 0x00 });
    }
    {
        static const uint32_t edge[] = {1,2,127};
        reset(); der_oid(&D, edge, 3);
        CKB("and 127 is a single byte 7F",
            { 0x06, 0x02, 0x2A, 0x7F });
    }
    {
        static const uint32_t bad[] = {3,1};
        reset(); der_oid(&D, bad, 2);
        ck("a first arc above 2 is refused", der_ok(&D) == 0, 0);
        static const uint32_t bad2[] = {1,40};
        reset(); der_oid(&D, bad2, 2);
        ck("a second arc of 40 under a first of 1 is refused",
           der_ok(&D) == 0, 0);
        static const uint32_t bad3[] = {1};
        reset(); der_oid(&D, bad3, 1);
        ck("fewer than two arcs is refused", der_ok(&D) == 0, 0);
    }

    /* ══ the simple ones ════════════════════════════════════════════ */
    head("the simple types");
    reset(); der_null(&D);
    CKB("NULL is 05 00", { 0x05, 0x00 });
    reset(); der_bool(&D, 1);
    CKB("TRUE is FF, not 01 -- BER allows any non-zero, DER does not",
        { 0x01, 0x01, 0xFF });
    reset(); der_bool(&D, 0);
    CKB("FALSE is 00", { 0x01, 0x01, 0x00 });
    reset(); der_string(&D, DER_PRINTABLE, "Haven");
    CKB("a PrintableString carries its bytes",
        { 0x13, 0x05, 'H', 'a', 'v', 'e', 'n' });
    reset(); der_string(&D, DER_UTF8STRING, "");
    CKB("an empty string is a zero-length TLV", { 0x0C, 0x00 });
    {
        static const uint8_t key[] = { 0xDE, 0xAD, 0xBE, 0xEF };
        reset(); der_bitstring(&D, key, sizeof key);
        CKB("a BIT STRING gains a leading unused-bits octet",
            { 0x03, 0x05, 0x00, 0xDE, 0xAD, 0xBE, 0xEF });
    }

    /* ══ SEQUENCE ═══════════════════════════════════════════════════ */
    head("SEQUENCE: the length is known only after the contents");
    {
        uint32_t m;
        reset();
        m = der_seq_begin(&D, DER_SEQUENCE);
        der_integer_u32(&D, 1);
        der_integer_u32(&D, 2);
        der_seq_end(&D, m);
        CKB("two small INTEGERs wrap as 30 06",
            { 0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x02 });
    }
    {
        uint32_t m;
        reset();
        m = der_seq_begin(&D, DER_SEQUENCE);
        der_seq_end(&D, m);
        CKB("an empty SEQUENCE is 30 00", { 0x30, 0x00 });
    }
    {
        /* Nesting, which is what a certificate is: the inner length
           has to be right before the outer one can be computed. */
        uint32_t o, i;
        reset();
        o = der_seq_begin(&D, DER_SEQUENCE);
        i = der_seq_begin(&D, DER_SEQUENCE);
        der_integer_u32(&D, 0x2A);
        der_seq_end(&D, i);
        der_null(&D);
        der_seq_end(&D, o);
        CKB("a nested SEQUENCE gets both lengths right",
            { 0x30, 0x07,
                0x30, 0x03, 0x02, 0x01, 0x2A,
                0x05, 0x00 });
    }
    {
        /* A SET and a context-specific wrapper carry their own tags.
           The marker is what remembers which, so a SEQUENCE opened
           and a SET closed cannot happen. */
        uint32_t m;
        reset();
        m = der_seq_begin(&D, DER_SET);
        der_integer_u32(&D, 7);
        der_seq_end(&D, m);
        CKB("a SET closes as 31, not 30",
            { 0x31, 0x03, 0x02, 0x01, 0x07 });

        reset();
        m = der_seq_begin(&D, DER_CTX(0));
        der_integer_u32(&D, 2);
        der_seq_end(&D, m);
        CKB("X.509's [0] version wrapper closes as A0",
            { 0xA0, 0x03, 0x02, 0x01, 0x02 });
    }
    {
        /* A sequence whose contents cross the 127-byte boundary: the
           header grows from two bytes to three and the contents have
           to move by three, not two. An off-by-one here shifts every
           byte of a certificate. */
        static uint8_t filler[130];
        uint32_t m;
        memset(filler, 0x5A, sizeof filler);
        reset();
        m = der_seq_begin(&D, DER_SEQUENCE);
        der_tlv(&D, DER_OCTET_STRING, filler, 130);
        der_seq_end(&D, m);
        ck("contents over 127 bytes get a 3-byte header",
           der_len(&D) == 3 + 3 + 130, (long)der_len(&D));
        ck("  tag 30", B[0] == 0x30, B[0]);
        ck("  length 81 85 -- 133 content bytes",
           B[1] == 0x81 && B[2] == 0x85, (B[1] << 8) | B[2]);
        ck("  and the inner TLV survived the move intact",
           B[3] == 0x04 && B[4] == 0x81 && B[5] == 0x82 &&
           B[6] == 0x5A && B[der_len(&D)-1] == 0x5A, B[3]);
    }

    /* ══ OVERFLOW ═══════════════════════════════════════════════════ */
    head("a buffer that will not hold it fails, and stays failed");
    {
        uint8_t small[8];
        der_t   s;
        static const uint8_t big[64] = {0};
        der_init(&s, small, sizeof small);
        ck("a fresh small writer is ok", der_ok(&s) == 1, 0);
        der_tlv(&s, DER_OCTET_STRING, big, 64);
        ck("  writing 64 bytes into 8 fails", der_ok(&s) == 0, 0);
        /* THE POINT: it stays failed. A caller writes a whole
           certificate and checks once, and must not be handed a short
           document that looks complete. */
        der_null(&s);
        der_integer_u32(&s, 1);
        ck("  and every later write is a no-op", der_ok(&s) == 0, 0);
        ck("  so the length is not trusted either",
           der_len(&s) <= sizeof small, (long)der_len(&s));
    }
    {
        der_t z;
        der_init(&z, 0, 0);
        ck("a writer with no buffer is not ok", der_ok(&z) == 0, 0);
        der_null(&z);
        ck("  and writing to it does not crash", der_ok(&z) == 0, 0);
    }

    /* ══ FROM A BIGNUM ══════════════════════════════════════════════ */
    head("INTEGER from a bignum, which is how a key gets in");
    {
        bn_t a;
        bn_set_u32(&a, 65537);
        reset(); der_integer_bn(&D, &a);
        CKB("65537 from a bignum is the same 02 03 01 00 01",
            { 0x02, 0x03, 0x01, 0x00, 0x01 });

        bn_set_u32(&a, 0);
        reset(); der_integer_bn(&D, &a);
        CKB("zero from a bignum is 02 01 00", { 0x02, 0x01, 0x00 });

        bn_set_u32(&a, 0x80);
        reset(); der_integer_bn(&D, &a);
        CKB("0x80 from a bignum gets its sign byte",
            { 0x02, 0x02, 0x00, 0x80 });
    }
    {
        /* A value with the top bit of its top byte set, built from
           bytes, so the bignum's own left-padding is exercised: a
           fixed-width export is all leading zeroes until the value
           starts, and der_integer() has to strip exactly those. */
        static const uint8_t be[] = { 0xC0, 0xDE, 0xBA, 0x5E };
        bn_t a;
        ck("a 4-byte value loads into a bignum",
           bn_from_bytes(&a, be, sizeof be) == 1, 0);
        reset(); der_integer_bn(&D, &a);
        CKB("  and encodes with one sign byte and no padding",
            { 0x02, 0x05, 0x00, 0xC0, 0xDE, 0xBA, 0x5E });
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
