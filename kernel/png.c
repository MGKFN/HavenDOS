/* kernel/png.c -- PNG and BMP, both ways enough for HavenDOS (v0.9.10).
 *
 * Kyan, 25 Nov: "maybe add image and screenshot support". This file is
 * the part with no screen in it:
 *
 *   inflate      RFC 1951 -- stored, fixed and dynamic Huffman blocks,
 *                inside RFC 1950's zlib wrapper (Adler-32 checked)
 *   deflate      fixed-Huffman blocks with LZ77 matches found through a
 *                hash of the next three bytes: no dynamic tables, so it
 *                is short, and screenshots -- long runs of one colour --
 *                still come out a fraction of their raw size
 *   png_decode   8-bit greyscale, grey+alpha, RGB, RGBA and palette, and
 *                1/2/4-bit palette and grey; all five filters; no Adam7
 *                interlacing (refused, by name)
 *   png_encode   8-bit RGB, filter "Sub" on every row (the one that suits
 *                a flat UI best)
 *   bmp_decode   uncompressed 24- and 32-bit, bottom-up or top-down
 *
 * Pixels are 0x00RRGGBB, as the framebuffer and window surfaces hold
 * them. Alpha is blended over a background colour the caller names.
 * Memory comes from kmalloc(); the caller kfree()s what it is given.
 *
 * Judged by tests/test_png.c against files PIL wrote (zlib at level 9,
 * dynamic Huffman -- the part of inflate this file's own deflate never
 * exercises) and by round trips through this encoder.
 */
#include "../include/png.h"
#include "../include/string.h"

extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);

/* ── checksums ──────────────────────────────────────────────────────── */
static uint32_t crc_tab[256];
static int crc_ready;
uint32_t png_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_tab[i] = c;
        }
        crc_ready = 1;
    }
    crc = ~crc;
    while (n--) crc = crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
uint32_t png_adler32(uint32_t a, const uint8_t *p, uint32_t n)
{
    uint32_t s1 = a & 0xFFFF, s2 = a >> 16;
    while (n) {
        uint32_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { s1 += *p++; s2 += s1; }
        s1 %= 65521u; s2 %= 65521u;
    }
    return (s2 << 16) | s1;
}

/* ── inflate ────────────────────────────────────────────────────────── */
typedef struct {
    const uint8_t *in; uint32_t inlen, pos;
    uint32_t bits; int nbits;
    uint8_t *out; uint32_t outlen, outcap;
    int err;
} inf_t;

static int getbit(inf_t *s)
{
    if (!s->nbits) {
        if (s->pos >= s->inlen) { s->err = 1; return 0; }
        s->bits = s->in[s->pos++]; s->nbits = 8;
    }
    int b = (int)(s->bits & 1); s->bits >>= 1; s->nbits--;
    return b;
}
static uint32_t getbits(inf_t *s, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)getbit(s) << i;
    return v;
}

/* A canonical Huffman code: counts per length, symbols in code order. */
typedef struct { uint16_t count[16]; uint16_t sym[288]; } huff_t;

static int huff_build(huff_t *h, const uint8_t *len, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
    return 0;
}
static int huff_decode(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= getbit(s);
        int count = h->count[l];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count;
        first <<= 1; code <<= 1;
        if (s->err) return -1;
    }
    s->err = 1;
    return -1;
}

static int out_byte(inf_t *s, uint8_t b)
{
    if (s->outlen >= s->outcap) { s->err = 2; return -1; }
    s->out[s->outlen++] = b;
    return 0;
}

static const uint16_t LBASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                                     67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEXT[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
                                     1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  DEXT[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inf_codes(inf_t *s, const huff_t *lit, const huff_t *dist)
{
    for (;;) {
        int sym = huff_decode(s, lit);
        if (s->err || sym < 0) return -1;
        if (sym < 256) { if (out_byte(s, (uint8_t)sym)) return -1; continue; }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return -1;
        uint32_t len = LBASE[sym] + getbits(s, LEXT[sym]);
        int ds = huff_decode(s, dist);
        if (ds < 0 || ds >= 30) return -1;
        uint32_t d = DBASE[ds] + getbits(s, DEXT[ds]);
        if (s->err || d > s->outlen) return -1;
        if (s->outlen + len > s->outcap) { s->err = 2; return -1; }
        for (uint32_t k = 0; k < len; k++) { s->out[s->outlen] = s->out[s->outlen - d]; s->outlen++; }
    }
}

static int inf_dynamic(inf_t *s, huff_t *lit, huff_t *dist)
{
    static const uint8_t ORD[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    uint8_t len[320];
    huff_t clen;
    int nlen = (int)getbits(s, 5) + 257, ndist = (int)getbits(s, 5) + 1, ncode = (int)getbits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) return -1;
    memset(len, 0, sizeof len);
    for (int i = 0; i < ncode; i++) len[ORD[i]] = (uint8_t)getbits(s, 3);
    if (huff_build(&clen, len, 19)) return -1;
    int i = 0;
    while (i < nlen + ndist) {
        int sym = huff_decode(s, &clen);
        if (sym < 0 || s->err) return -1;
        if (sym < 16) { len[i++] = (uint8_t)sym; continue; }
        int rep = 0; uint8_t v = 0;
        if (sym == 16) { if (!i) return -1; v = len[i - 1]; rep = 3 + (int)getbits(s, 2); }
        else if (sym == 17) rep = 3 + (int)getbits(s, 3);
        else rep = 11 + (int)getbits(s, 7);
        if (i + rep > nlen + ndist) return -1;
        while (rep--) len[i++] = v;
    }
    if (!len[256]) return -1;
    if (huff_build(lit, len, nlen)) return -1;
    if (huff_build(dist, len + nlen, ndist) && ndist > 1) return -1;
    return 0;
}

/* Raw DEFLATE: in -> out (cap bytes). Bytes written, or -1. */
int png_inflate_raw(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t cap)
{
    static huff_t flit, fdist; static int fixed_ready;
    static huff_t lit, dist;                 /* static: 1.2 KB off the stack */
    inf_t s; memset(&s, 0, sizeof s);
    s.in = in; s.inlen = inlen; s.out = out; s.outcap = cap;
    if (!fixed_ready) {
        uint8_t l[320]; int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&flit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(&fdist, l, 30);
        fixed_ready = 1;
    }
    int last;
    do {
        last = getbit(&s);
        int type = (int)getbits(&s, 2);
        if (s.err) return -1;
        if (type == 0) {
            s.nbits = 0;                                     /* to a byte boundary */
            if (s.pos + 4 > s.inlen) return -1;
            uint32_t n = (uint32_t)s.in[s.pos] | ((uint32_t)s.in[s.pos + 1] << 8);
            uint32_t nn = (uint32_t)s.in[s.pos + 2] | ((uint32_t)s.in[s.pos + 3] << 8);
            s.pos += 4;
            if ((n ^ 0xFFFFu) != nn || s.pos + n > s.inlen || s.outlen + n > s.outcap) return -1;
            memcpy(s.out + s.outlen, s.in + s.pos, n);
            s.outlen += n; s.pos += n;
        } else if (type == 1) {
            if (inf_codes(&s, &flit, &fdist)) return -1;
        } else if (type == 2) {
            if (inf_dynamic(&s, &lit, &dist) || inf_codes(&s, &lit, &dist)) return -1;
        } else return -1;
    } while (!last);
    return (int)s.outlen;
}

int png_inflate_zlib(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t cap)
{
    if (inlen < 6) return -1;
    if ((in[0] & 0x0F) != 8 || ((uint32_t)in[0] * 256u + in[1]) % 31u || (in[1] & 0x20)) return -1;
    int n = png_inflate_raw(in + 2, inlen - 6, out, cap);
    if (n < 0) return -1;
    uint32_t want = ((uint32_t)in[inlen - 4] << 24) | ((uint32_t)in[inlen - 3] << 16)
                  | ((uint32_t)in[inlen - 2] << 8) | in[inlen - 1];
    if (png_adler32(1, out, (uint32_t)n) != want) return -1;
    return n;
}

/* ── deflate (fixed Huffman, LZ77) ──────────────────────────────────── */
typedef struct { uint8_t *out; uint32_t len, cap; uint32_t bits; int nbits; int err; } bw_t;
static void put_bits(bw_t *b, uint32_t v, int n)
{
    b->bits |= v << b->nbits; b->nbits += n;
    while (b->nbits >= 8) {
        if (b->len >= b->cap) { b->err = 1; b->nbits = 0; return; }
        b->out[b->len++] = (uint8_t)b->bits; b->bits >>= 8; b->nbits -= 8;
    }
}
static uint32_t rev(uint32_t v, int n){ uint32_t r = 0; while (n--) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
static void put_lit(bw_t *b, int sym)
{
    if (sym < 144)      put_bits(b, rev(0x30u + (uint32_t)sym, 8), 8);
    else if (sym < 256) put_bits(b, rev(0x190u + (uint32_t)sym - 144u, 9), 9);
    else if (sym < 280) put_bits(b, rev((uint32_t)sym - 256u, 7), 7);
    else                put_bits(b, rev(0xC0u + (uint32_t)sym - 280u, 8), 8);
}
static void put_match(bw_t *b, uint32_t len, uint32_t d)
{
    int i = 28;
    while (LBASE[i] > len) i--;
    put_lit(b, 257 + i);
    if (LEXT[i]) put_bits(b, len - LBASE[i], LEXT[i]);
    int j = 29;
    while (DBASE[j] > d) j--;
    put_bits(b, rev((uint32_t)j, 5), 5);
    if (DEXT[j]) put_bits(b, d - DBASE[j], DEXT[j]);
}

#define HBITS 15
#define WSIZE 32768u
/* zlib stream of in[0..n). Returns bytes, or -1 (out too small, or no
   memory for the hash table). */
int png_deflate_zlib(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap)
{
    bw_t b; memset(&b, 0, sizeof b);
    b.out = out; b.cap = cap;
    int32_t *head = (int32_t *)kmalloc(sizeof(int32_t) << HBITS);
    if (!head || cap < 8) { if (head) kfree(head); return -1; }
    for (uint32_t i = 0; i < (1u << HBITS); i++) head[i] = -1;
    out[b.len++] = 0x78; out[b.len++] = 0x01;         /* deflate, 32K window, fastest */
    put_bits(&b, 1, 1);                               /* the last block */
    put_bits(&b, 1, 2);                               /* fixed Huffman */
    uint32_t i = 0;
    while (i < n && !b.err) {
        uint32_t best = 0, bd = 0;
        if (i + 3 <= n) {
            uint32_t h = (((uint32_t)in[i] << 10) ^ ((uint32_t)in[i + 1] << 5) ^ in[i + 2]) & ((1u << HBITS) - 1);
            int32_t c = head[h];
            head[h] = (int32_t)i;
            if (c >= 0 && i - (uint32_t)c <= WSIZE) {
                uint32_t m = 0, lim = n - i < 258 ? n - i : 258;
                while (m < lim && in[(uint32_t)c + m] == in[i + m]) m++;
                if (m >= 3) { best = m; bd = i - (uint32_t)c; }
            }
            /* the run of the pixel before: a flat background is a match
               at distance 3 (RGB) every time, which the hash may miss */
            if (i >= 3) {
                uint32_t m = 0, lim = n - i < 258 ? n - i : 258;
                while (m < lim && in[i - 3 + m] == in[i + m]) m++;
                if (m > best && m >= 3) { best = m; bd = 3; }
            }
        }
        if (best) {
            put_match(&b, best, bd);
            /* the hash for the bytes the match covered, every fourth */
            for (uint32_t k = 1; k < best && i + k + 3 <= n; k += 4) {
                uint32_t h = (((uint32_t)in[i + k] << 10) ^ ((uint32_t)in[i + k + 1] << 5) ^ in[i + k + 2]) & ((1u << HBITS) - 1);
                head[h] = (int32_t)(i + k);
            }
            i += best;
        } else put_lit(&b, in[i++]);
    }
    put_lit(&b, 256);
    if (b.nbits) put_bits(&b, 0, 8 - b.nbits);
    kfree(head);
    if (b.err || b.len + 4 > cap) return -1;
    uint32_t a = png_adler32(1, in, n);
    out[b.len++] = (uint8_t)(a >> 24); out[b.len++] = (uint8_t)(a >> 16);
    out[b.len++] = (uint8_t)(a >> 8);  out[b.len++] = (uint8_t)a;
    return (int)b.len;
}

/* ── PNG ─────────────────────────────────────────────────────────────── */
static uint32_t be32(const uint8_t *p){ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wbe32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static const uint8_t SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

const char *png_error;

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    return (uint8_t)(pb <= pc ? b : c);
}
static uint32_t blend(uint32_t r, uint32_t g, uint32_t b, uint32_t a, uint32_t bg)
{
    if (a == 255) return (r << 16) | (g << 8) | b;
    uint32_t br = (bg >> 16) & 0xFF, bgg = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    r = (r * a + br * (255 - a) + 127) / 255; g = (g * a + bgg * (255 - a) + 127) / 255;
    b = (b * a + bb * (255 - a) + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

int png_info(const uint8_t *f, uint32_t n, int *w, int *h)
{
    if (n < 33 || memcmp(f, SIG, 8) || memcmp(f + 12, "IHDR", 4)) return -1;
    *w = (int)be32(f + 16); *h = (int)be32(f + 20);
    return 0;
}

uint32_t *png_decode(const uint8_t *f, uint32_t n, int *pw, int *ph, uint32_t bg)
{
    png_error = 0;
    if (n < 33 || memcmp(f, SIG, 8)) { png_error = "not a PNG file"; return 0; }
    uint32_t w = 0, h = 0; int depth = 0, ctype = 0, inter = 0;
    uint8_t pal[256 * 4]; int npal = 0;
    memset(pal, 255, sizeof pal);
    uint32_t idat = 0;
    /* first pass: the header, the palette, how much compressed data */
    for (uint32_t p = 8; p + 12 <= n; ) {
        uint32_t len = be32(f + p);
        const uint8_t *t = f + p + 4, *d = f + p + 8;
        if (len > n - p - 12) { png_error = "a chunk runs past the end of the file"; return 0; }
        if (!memcmp(t, "IHDR", 4) && len >= 13) {
            w = be32(d); h = be32(d + 4); depth = d[8]; ctype = d[9]; inter = d[12];
        } else if (!memcmp(t, "PLTE", 4)) {
            npal = (int)(len / 3);
            for (int i = 0; i < npal && i < 256; i++) { pal[i*4] = d[i*3]; pal[i*4+1] = d[i*3+1]; pal[i*4+2] = d[i*3+2]; }
        } else if (!memcmp(t, "tRNS", 4) && ctype == 3) {
            for (uint32_t i = 0; i < len && i < 256; i++) pal[i*4+3] = d[i];
        } else if (!memcmp(t, "IDAT", 4)) idat += len;
        else if (!memcmp(t, "IEND", 4)) break;
        p += 12 + len;
    }
    if (!w || !h || w > 8192 || h > 8192) { png_error = "no picture size, or bigger than 8192x8192"; return 0; }
    if (inter) { png_error = "interlaced PNGs are not supported yet -- save it without interlacing"; return 0; }
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!ch) { png_error = "unknown PNG colour type"; return 0; }
    if (depth == 16) { png_error = "16-bit PNGs are not supported yet"; return 0; }
    if (depth != 8 && !(depth < 8 && (ctype == 0 || ctype == 3))) { png_error = "unsupported bit depth"; return 0; }
    uint32_t bpp = (uint32_t)ch * (uint32_t)depth;               /* bits per pixel */
    uint32_t stride = (w * bpp + 7) / 8, raw = (stride + 1) * h;
    uint8_t *z = (uint8_t *)kmalloc(idat ? idat : 1), *r = (uint8_t *)kmalloc(raw);
    uint32_t *img = (uint32_t *)kmalloc(w * h * 4u);
    if (!z || !r || !img) { if (z) kfree(z); if (r) kfree(r); if (img) kfree(img); png_error = "not enough memory for this picture"; return 0; }
    uint32_t zl = 0;
    for (uint32_t p = 8; p + 12 <= n; ) {
        uint32_t len = be32(f + p);
        if (!memcmp(f + p + 4, "IDAT", 4)) { memcpy(z + zl, f + p + 8, len); zl += len; }
        if (!memcmp(f + p + 4, "IEND", 4)) break;
        p += 12 + len;
    }
    int got = png_inflate_zlib(z, zl, r, raw);
    kfree(z);
    if (got != (int)raw) { kfree(r); kfree(img); png_error = "the compressed picture data is damaged"; return 0; }
    /* unfilter in place */
    uint32_t bpx = (bpp + 7) / 8;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = r + y * (stride + 1), ft = row[0], *c = row + 1;
        const uint8_t *up = y ? r + (y - 1) * (stride + 1) + 1 : 0;
        for (uint32_t x = 0; x < stride; x++) {
            int a = x >= bpx ? c[x - bpx] : 0, b = up ? up[x] : 0, cc = (up && x >= bpx) ? up[x - bpx] : 0;
            switch (ft) {
            case 0: break;
            case 1: c[x] = (uint8_t)(c[x] + a); break;
            case 2: c[x] = (uint8_t)(c[x] + b); break;
            case 3: c[x] = (uint8_t)(c[x] + ((a + b) >> 1)); break;
            case 4: c[x] = (uint8_t)(c[x] + paeth(a, b, cc)); break;
            default: kfree(r); kfree(img); png_error = "unknown row filter"; return 0;
            }
        }
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *c = r + y * (stride + 1) + 1;
        uint32_t *o = img + y * w;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v;
            if (depth < 8) {
                uint32_t bit = x * (uint32_t)depth, sh = 8u - (uint32_t)depth - (bit & 7);
                v = ((uint32_t)c[bit >> 3] >> sh) & ((1u << depth) - 1);
                if (ctype == 0) { v = v * 255u / ((1u << depth) - 1); o[x] = (v << 16) | (v << 8) | v; }
                else o[x] = blend(pal[v*4], pal[v*4+1], pal[v*4+2], pal[v*4+3], bg);
                continue;
            }
            switch (ctype) {
            case 0: v = c[x]; o[x] = (v << 16) | (v << 8) | v; break;
            case 2: o[x] = ((uint32_t)c[x*3] << 16) | ((uint32_t)c[x*3+1] << 8) | c[x*3+2]; break;
            case 3: v = c[x]; o[x] = blend(pal[v*4], pal[v*4+1], pal[v*4+2], pal[v*4+3], bg); break;
            case 4: v = c[x*2]; o[x] = blend(v, v, v, c[x*2+1], bg); break;
            default: o[x] = blend(c[x*4], c[x*4+1], c[x*4+2], c[x*4+3], bg); break;
            }
        }
    }
    kfree(r);
    *pw = (int)w; *ph = (int)h;
    return img;
}

/* RGB, 8 bits, filter Sub. *outlen gets the file's size. */
uint8_t *png_encode(const uint32_t *px, int w, int h, int pitch, uint32_t *outlen)
{
    uint32_t stride = (uint32_t)w * 3u + 1u, raw = stride * (uint32_t)h;
    uint8_t *r = (uint8_t *)kmalloc(raw);
    if (!r) return 0;
    for (int y = 0; y < h; y++) {
        uint8_t *o = r + (uint32_t)y * stride;
        const uint32_t *s = px + (uint32_t)y * (uint32_t)pitch;
        uint8_t pr = 0, pg = 0, pb = 0;
        *o++ = 1;                                           /* Sub */
        for (int x = 0; x < w; x++) {
            uint8_t cr = (uint8_t)(s[x] >> 16), cg = (uint8_t)(s[x] >> 8), cb = (uint8_t)s[x];
            *o++ = (uint8_t)(cr - pr); *o++ = (uint8_t)(cg - pg); *o++ = (uint8_t)(cb - pb);
            pr = cr; pg = cg; pb = cb;
        }
    }
    /* fixed Huffman never makes data much bigger than 9/8 of it */
    uint32_t zcap = raw + raw / 8u + 1024u;
    uint8_t *f = (uint8_t *)kmalloc(zcap + 64u);
    if (!f) { kfree(r); return 0; }
    uint32_t p = 0;
    memcpy(f, SIG, 8); p = 8;
    wbe32(f + p, 13); memcpy(f + p + 4, "IHDR", 4);
    wbe32(f + p + 8, (uint32_t)w); wbe32(f + p + 12, (uint32_t)h);
    f[p + 16] = 8; f[p + 17] = 2; f[p + 18] = 0; f[p + 19] = 0; f[p + 20] = 0;
    wbe32(f + p + 21, png_crc32(0, f + p + 4, 17)); p += 25;
    int zl = png_deflate_zlib(r, raw, f + p + 8, zcap);
    kfree(r);
    if (zl < 0) { kfree(f); return 0; }
    wbe32(f + p, (uint32_t)zl); memcpy(f + p + 4, "IDAT", 4);
    wbe32(f + p + 8 + (uint32_t)zl, png_crc32(0, f + p + 4, (uint32_t)zl + 4u));
    p += 12u + (uint32_t)zl;
    wbe32(f + p, 0); memcpy(f + p + 4, "IEND", 4); wbe32(f + p + 8, png_crc32(0, f + p + 4, 4)); p += 12;
    *outlen = p;
    return f;
}

/* ── BMP ─────────────────────────────────────────────────────────────── */
static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

uint32_t *bmp_decode(const uint8_t *f, uint32_t n, int *pw, int *ph)
{
    png_error = 0;
    if (n < 54 || f[0] != 'B' || f[1] != 'M') { png_error = "not a BMP file"; return 0; }
    uint32_t off = le32(f + 10);
    int32_t w = (int32_t)le32(f + 18), h = (int32_t)le32(f + 22);
    int bpp = f[28] | (f[29] << 8), comp = (int)le32(f + 30);
    int topdown = h < 0;
    if (topdown) h = -h;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { png_error = "bad BMP size"; return 0; }
    if ((bpp != 24 && bpp != 32) || (comp != 0 && !(comp == 3 && bpp == 32))) {
        png_error = "only uncompressed 24- and 32-bit BMPs are supported"; return 0;
    }
    uint32_t stride = ((uint32_t)w * (uint32_t)(bpp / 8) + 3u) & ~3u;
    if (off > n || stride * (uint32_t)h > n - off) { png_error = "the BMP is shorter than its size says"; return 0; }
    uint32_t *img = (uint32_t *)kmalloc((uint32_t)w * (uint32_t)h * 4u);
    if (!img) { png_error = "not enough memory for this picture"; return 0; }
    for (int y = 0; y < h; y++) {
        const uint8_t *s = f + off + stride * (uint32_t)(topdown ? y : h - 1 - y);
        uint32_t *o = img + (uint32_t)y * (uint32_t)w;
        for (int x = 0; x < w; x++, s += bpp / 8)
            o[x] = ((uint32_t)s[2] << 16) | ((uint32_t)s[1] << 8) | s[0];
    }
    *pw = w; *ph = h;
    return img;
}

/* Either, by its first bytes. */
uint32_t *image_decode(const uint8_t *f, uint32_t n, int *w, int *h, uint32_t bg)
{
    if (n >= 8 && !memcmp(f, SIG, 8)) return png_decode(f, n, w, h, bg);
    if (n >= 2 && f[0] == 'B' && f[1] == 'M') return bmp_decode(f, n, w, h);
    png_error = "not a PNG or BMP picture";
    return 0;
}
