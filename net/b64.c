/* net/b64.c -- see include/b64.h. */
#include "b64.h"

static const char ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* The inverse table. 128 entries because anything with the top bit
   set is refused before it is indexed -- a signed char index into a
   table is how a decoder reads memory in front of itself. */
static const signed char DEC[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
};

static int is_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int b64_decode(const uint8_t *in, int inlen, uint8_t *out, int outcap)
{
    uint32_t acc = 0;
    int bits = 0, n = 0, i, pad = 0, quad = 0;
    int measure = (out == 0 && outcap == 0);

    if (!in || inlen < 0)          return -1;
    if (!out && !measure)          return -1;
    if (outcap < 0)                return -1;

    for (i = 0; i < inlen; i++) {
        uint8_t c = in[i];
        int v;
        if (is_ws(c)) continue;
        if (c == '=') {
            pad++;
            quad++;
            if (quad == 4) quad = 0;
            /* At most two padding characters. "A===" is not base64. */
            if (pad > 2) return -1;
            continue;
        }
        /* Padding ends the value. Anything after it belongs to
           something else, and guessing which is how a decoder
           concatenates two keys into one. */
        if (pad) return -1;
        if (c >= 128) return -1;
        v = DEC[c];
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        quad++;
        if (quad == 4) quad = 0;
        if (bits >= 8) {
            bits -= 8;
            if (!measure) {
                if (n >= outcap) return -1;
                out[n] = (uint8_t)((acc >> bits) & 0xFF);
            }
            n++;
        }
    }
    /* ── WHAT A SHORT TRAILING GROUP MEANS ──────────────────────────
       TWO characters is not truncation: twelve bits carry one byte
       with four left over, which is exactly what an unpadded single
       byte looks like. THREE carry two bytes. ONE is the impossible
       case -- six bits and nothing to put them in.

       This is usb/lockdown.c's rule, not a new one, and it is kept
       deliberately. The first version of this consolidation was
       stricter -- it required a whole number of quads and required
       the leftover bits to be zero -- and it broke four checks in
       tests/test_lockdown.c that exist because an EARLIER version of
       that suite made the same mistake in the same direction and the
       comment there says so. Unpadded base64 is common in the wild
       and a two- or three-character tail is unambiguous. Refusing it
       would have been strictness about the wrong thing. */
    if (quad == 1) return -1;
    return n;
}

int b64_encoded_len(int inlen, int wrap)
{
    int quads, chars, lines;
    if (inlen < 0) return -1;
    quads = (inlen + 2) / 3;
    chars = quads * 4;
    if (wrap <= 0) return chars;
    wrap -= wrap % 4;
    if (wrap < 4) return chars;
    if (chars == 0) return 0;
    /* One newline after every full line except a last line that ends
       exactly on the wrap column -- that one gets no trailing break,
       because b64_encode() does not add one. */
    lines = (chars - 1) / wrap;
    return chars + lines;
}

int b64_encode(const uint8_t *in, int inlen, char *out, int outcap,
               int wrap)
{
    int i, n = 0, col = 0, need;

    if (!in && inlen) return -1;
    if (!out || inlen < 0 || outcap < 0) return -1;
    if (wrap > 0) { wrap -= wrap % 4; if (wrap < 4) wrap = 0; }

    need = b64_encoded_len(inlen, wrap);
    if (need < 0 || need > outcap) return -1;

    for (i = 0; i < inlen; i += 3) {
        int rem = inlen - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i+1] << 8;
        if (rem > 2) v |= (uint32_t)in[i+2];
        if (wrap && col == wrap) { out[n++] = '\n'; col = 0; }
        out[n++] = ENC[(v >> 18) & 63];
        out[n++] = ENC[(v >> 12) & 63];
        out[n++] = (rem > 1) ? ENC[(v >> 6) & 63] : '=';
        out[n++] = (rem > 2) ? ENC[v & 63]        : '=';
        col += 4;
    }
    return n;
}

/* ════════════════════════════════════════════════════════════════════
   WHAT THIS WAS PROVED AGAINST

   19 defects injected one at a time, each with a prediction written
   first. All 19 are caught -- but two of them only after the suite
   was given checks it did not have, and both were the same kind of
   blind spot: an input the tests had never produced.

     - Removing `if (pad > 2) return -1;` survived, because every
       over-padded string the suite had was ALSO refused by the
       trailing-group rule. "aGVs====" lands on a quad boundary with
       four padding characters and nothing else catches it.

     - Removing `if (c >= 128) return -1;` survived, because no test
       had ever fed the decoder a byte with the top bit set. DEC has
       128 entries; a byte of 0xFF indexes twelve past the end of it
       and trusts whatever the linker put there. The suite now tries
       all 128 high values, in all four positions of a quad, and in
       measure mode -- so the refusal cannot be an accident of what
       currently follows the table in .rodata.

   The other seventeen are the ones a codec has: a swapped alphabet
   character, the urlsafe alphabet, padding written as 'A', a dropped
   pad, a length function that forgets the newlines or is off by a
   quad, a wrap column that splits a quad, a newline on the wrong side
   of it, a missing capacity check in either direction, whitespace
   treated as data, the accumulator read before it is shifted, and a
   measure mode that counts nothing or validates nothing.
   ════════════════════════════════════════════════════════════════════ */
