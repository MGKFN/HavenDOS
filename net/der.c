/* ════════════════════════════════════════════════════════════════════
   net/der.c -- writing DER.  v0.8.8.5.

   include/der.h says what DER is and why the two rules that matter --
   minimal lengths and signed INTEGERs -- are the ones this file can
   be plausibly wrong about while still looking right in a hex dump.

   NOTHING IN HERE CALLS ANYTHING but memcpy, memmove and the bignum's
   byte export. So tests/test_der.c links it directly, with no seam
   and no simulated hardware, and every check is against the bytes.
   That is the same arrangement as shell/mines_model.c and for the
   same reason: a module with no outside world in it cannot be tested
   against a fixture written from the same guess as the code.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/der.h"
#include "../include/string.h"

void der_init(der_t *d, uint8_t *buf, uint32_t cap)
{
    d->buf = buf;
    d->cap = cap;
    d->len = 0;
    d->ok  = (buf && cap) ? 1 : 0;
}

int      der_ok(const der_t *d)  { return d && d->ok; }
uint32_t der_len(const der_t *d) { return d ? d->len : 0; }

/* ── the one place bytes are added ───────────────────────────────────
   Everything goes through here, so there is exactly one overflow
   check and exactly one place that clears ok. A second copy of that
   check is a second place it can be forgotten. */
static void put(der_t *d, const uint8_t *p, uint32_t n)
{
    if(!d->ok) return;
    if(n > d->cap - d->len){ d->ok = 0; return; }
    if(p) memcpy(d->buf + d->len, p, n);
    else  memset(d->buf + d->len, 0, n);
    d->len += n;
}

static void put1(der_t *d, uint8_t b){ put(d, &b, 1); }

/* ── the length, minimally ───────────────────────────────────────────
   Under 128: one byte. From 128: 0x80|n then n big-endian bytes with
   NO LEADING ZERO. Encoding 200 as `82 00 C8` parses as BER and is
   refused as DER, and nothing about the dump looks wrong. */
static uint32_t len_bytes(uint32_t n)
{
    if(n < 0x80u) return 1;
    if(n <= 0xFFu) return 2;
    if(n <= 0xFFFFu) return 3;
    if(n <= 0xFFFFFFu) return 4;
    return 5;
}

static void put_len(der_t *d, uint32_t n)
{
    if(n < 0x80u){ put1(d, (uint8_t)n); return; }
    if(n <= 0xFFu){        put1(d, 0x81); put1(d, (uint8_t)n); return; }
    if(n <= 0xFFFFu){      put1(d, 0x82);
                           put1(d, (uint8_t)(n >> 8));
                           put1(d, (uint8_t)n); return; }
    if(n <= 0xFFFFFFu){    put1(d, 0x83);
                           put1(d, (uint8_t)(n >> 16));
                           put1(d, (uint8_t)(n >> 8));
                           put1(d, (uint8_t)n); return; }
    put1(d, 0x84);
    put1(d, (uint8_t)(n >> 24));
    put1(d, (uint8_t)(n >> 16));
    put1(d, (uint8_t)(n >> 8));
    put1(d, (uint8_t)n);
}

void der_tlv(der_t *d, uint8_t tag, const uint8_t *val, uint32_t len)
{
    if(!d->ok) return;
    put1(d, tag);
    put_len(d, len);
    put(d, val, len);
}

void der_raw(der_t *d, const uint8_t *p, uint32_t len){ put(d, p, len); }

/* ── INTEGER ─────────────────────────────────────────────────────────
   THE ONE THAT BREAKS RSA. A DER INTEGER is SIGNED, so a value whose
   top bit is set needs a leading 0x00 or it reads as negative -- a
   2048-bit modulus is 257 content bytes, not 256. And leading zeroes
   are stripped, so zero is `02 01 00`: one content byte, not none.

   Both halves are here rather than at the call sites, because every
   call site would otherwise have to remember them and one of them
   would not. */
void der_integer(der_t *d, const uint8_t *be, uint32_t len)
{
    uint32_t i = 0;
    int pad;

    if(!d->ok) return;

    while(i < len && be[i] == 0) i++;      /* strip leading zeroes */

    if(i == len){                          /* the value is zero */
        put1(d, DER_INTEGER);
        put1(d, 1);
        put1(d, 0);
        return;
    }

    pad = (be[i] & 0x80u) ? 1 : 0;         /* keep it positive */
    put1(d, DER_INTEGER);
    put_len(d, (len - i) + (uint32_t)pad);
    if(pad) put1(d, 0x00);
    put(d, be + i, len - i);
}

void der_integer_bn(der_t *d, const bn_t *a)
{
    /* BN_BYTES is the widest a bignum can be; bn_to_bytes() writes
       exactly that many, left-padded, and der_integer() strips the
       padding back off. Going through the full width rather than
       bn_bits()/8 keeps the two functions from having to agree about
       where a number starts. */
    uint8_t tmp[BN_LIMBS * 4];
    if(!d->ok) return;
    if(!bn_to_bytes(a, tmp, sizeof tmp)){ d->ok = 0; return; }
    der_integer(d, tmp, sizeof tmp);
}

void der_integer_u32(der_t *d, uint32_t v)
{
    uint8_t be[4];
    be[0] = (uint8_t)(v >> 24);
    be[1] = (uint8_t)(v >> 16);
    be[2] = (uint8_t)(v >> 8);
    be[3] = (uint8_t)v;
    der_integer(d, be, 4);
}

/* ── BIT STRING ──────────────────────────────────────────────────────
   The content begins with a count of unused bits in the final octet.
   Zero here: every use in X.509 except KeyUsage is a whole number of
   bytes, and a missing leading octet shifts the entire key by one
   byte with no other sign of trouble. */
void der_bitstring(der_t *d, const uint8_t *val, uint32_t len)
{
    if(!d->ok) return;
    put1(d, DER_BIT_STRING);
    put_len(d, len + 1);
    put1(d, 0x00);                         /* unused bits */
    put(d, val, len);
}

/* ── OBJECT IDENTIFIER ───────────────────────────────────────────────
   The first two arcs are one byte, 40*a+b, because the first is only
   ever 0, 1 or 2 and the second is under 40 when the first is. The
   rest are base-128, most significant group first, with bit 7 set on
   every byte but the last. */
void der_oid(der_t *d, const uint32_t *arcs, uint32_t n)
{
    uint8_t  body[64];
    uint32_t bl = 0, i;

    if(!d->ok) return;
    if(n < 2){ d->ok = 0; return; }        /* an OID has at least two */
    if(arcs[0] > 2){ d->ok = 0; return; }
    if(arcs[0] < 2 && arcs[1] >= 40){ d->ok = 0; return; }

    body[bl++] = (uint8_t)(arcs[0] * 40u + arcs[1]);

    for(i = 2; i < n; i++){
        uint32_t v = arcs[i];
        uint8_t  g[5];
        int      gn = 0;
        /* Emit groups least-significant first, then reverse: the
           count is not known until the value is shifted down. */
        do { g[gn++] = (uint8_t)(v & 0x7Fu); v >>= 7; } while(v);
        while(gn){
            gn--;
            if(bl >= sizeof body){ d->ok = 0; return; }
            body[bl++] = (uint8_t)(g[gn] | (gn ? 0x80u : 0u));
        }
    }

    der_tlv(d, DER_OID, body, bl);
}

void der_null(der_t *d){ der_tlv(d, DER_NULL, 0, 0); }

void der_bool(der_t *d, int v)
{
    /* DER spells true as all-ones, not as 1. BER allows any non-zero
       and DER does not, which is a one-byte difference a parser that
       is strict about one thing and not another will disagree on. */
    uint8_t b = v ? 0xFF : 0x00;
    der_tlv(d, DER_BOOLEAN, &b, 1);
}

void der_string(der_t *d, uint8_t tag, const char *s)
{
    uint32_t n = 0;
    if(!d->ok) return;
    if(!s){ d->ok = 0; return; }
    while(s[n]) n++;
    der_tlv(d, tag, (const uint8_t *)s, n);
}

/* ── constructed types ───────────────────────────────────────────────
   begin() writes nothing and returns where the contents will start.
   end() knows the length, encodes the header, and moves the contents
   up to make room for it. include/der.h argues why this rather than a
   reserved fixed-size length (not DER) or building backwards
   (correct, and unreadable). */
/* ── THE MARKER CARRIES THE TAG ──────────────────────────────────────
   der_seq_end() needs the tag and der_seq_begin() is where it is
   given, so the marker packs both: the tag in the top byte and the
   offset in the low 24 bits.

   The alternative was to make the caller pass the tag to end() as
   well, which means every nested structure names its tag twice and
   the two can disagree -- a SEQUENCE opened and a SET closed, with
   the length still correct, is a document that parses as something
   nobody wrote.

   24 bits is 16 MB of offset against buffers measured in kilobytes,
   and the packing is refused rather than silently wrapped if that is
   ever not true. */
#define DER_MARK(tag, off)  (((uint32_t)(tag) << 24) | (off))
#define DER_MARK_TAG(m)     ((uint8_t)((m) >> 24))
#define DER_MARK_OFF(m)     ((m) & 0x00FFFFFFu)

uint32_t der_seq_begin(der_t *d, uint8_t tag)
{
    if(!d->ok) return DER_MARK(tag, 0);
    if(d->len > 0x00FFFFFFu){ d->ok = 0; return DER_MARK(tag, 0); }
    return DER_MARK(tag, d->len);
}

void der_seq_end(der_t *d, uint32_t marker)
{
    uint8_t  tag     = DER_MARK_TAG(marker);
    uint32_t off     = DER_MARK_OFF(marker);
    uint32_t content, hdr;

    if(!d->ok) return;
    if(off > d->len){ d->ok = 0; return; }

    content = d->len - off;
    hdr     = 1 + len_bytes(content);

    if(hdr > d->cap - d->len){ d->ok = 0; return; }

    /* Move the contents up by the header size. memmove and not
       memcpy: the regions overlap whenever the header is shorter than
       the contents, which is almost always. */
    memmove(d->buf + off + hdr, d->buf + off, content);

    /* Write the header into the gap, through a second writer pointed
       at it -- so the length goes through put_len() and cannot
       acquire a different, non-minimal spelling here. */
    {
        der_t h;
        der_init(&h, d->buf + off, hdr);
        put1(&h, tag);
        put_len(&h, content);
        if(!h.ok || h.len != hdr){ d->ok = 0; return; }
    }

    d->len += hdr;
}
