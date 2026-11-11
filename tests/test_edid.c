/* ════════════════════════════════════════════════════════════════════
   test_edid.c -- EDID over GMBUS (drivers/xegmbus.c) and the EDID
   parser (drivers/edid.c), against a model GMBUS controller with a
   model monitor behind it.

   ── THE OUTSIDE EVIDENCE ────────────────────────────────────────────

   The parser is judged against bytes this project did not make: the
   18-byte detailed timings of the six built-in EDIDs Linux shipped in
   drivers/gpu/drm/drm_edid_load.c (v5.15), each checked here against
   the published VESA DMT / CEA-861 timing for that mode. Where the
   kernel's bytes and VESA disagree -- 1024x768's horizontal sync (8/144
   in the kernel, 24/136 in DMT) and 1680x1050's hsync polarity (+ in
   the kernel, - in DMT) -- the check asserts what the BYTES say, and
   says so. The header fields of those six were also run through
   pyedid (PyPI) during 0.9.1 and agreed: LNX, product 0, 2012 wk 5,
   "Linux FHD" etc.

   ── THE GMBUS MODEL ─────────────────────────────────────────────────

   Strict where the hardware is strict:
     - it sees EVERY write; one outside 0xC5100-0xC5120 is a failure of
       gmbus_wr()'s guard, and is counted, not ignored
     - SW_RDY is consumed: GMBUS1 reads back without it
     - a target that does not answer raises SATOER, and stays latched
       until SW_CLR_INT
     - data arrives four bytes per GMBUS3 read; HW_RDY drops between
     - it can NAK the first N attempts, hang, keep GMBUS0 from
       holding the pin, or be busy before we start
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define GFX_TEST 1

/* ── the model GMBUS ──────────────────────────────────────────────── */
static uint32_t m_g0, m_g1;
static int      m_active, m_rdy, m_satoer, m_wait_phase;
static uint32_t m_pos, m_left;
static uint8_t  mon_edid[256];
static int      mon_pin = 2;           /* which pin the monitor is on   */
static int      mon_present = 1;
static int      mon_nak_first = 0;     /* NAK this many transfers       */
static int      mon_nak_index128 = 0;  /* NAK a read of block 1         */
static int      mon_hang = 0;          /* never raise HW_RDY            */
static int      g0_sticky = 0;         /* GMBUS0 will not hold a pin    */
static int      busy_polls = 0;        /* ACTIVE for this many G2 reads */
static int      busy_forever = 0;
/* What the model saw. */
static int      w_total, w_outside, r_total, g1_writes, g3_reads;
static uint32_t w_off[512], w_val[512];

static void model_reset(void)
{
    m_g0 = m_g1 = 0; m_active = m_rdy = m_satoer = m_wait_phase = 0;
    m_pos = m_left = 0;
    mon_pin = 2; mon_present = 1; mon_nak_first = 0; mon_nak_index128 = 0;
    mon_hang = 0; g0_sticky = 0; busy_polls = 0; busy_forever = 0;
    w_total = w_outside = r_total = g1_writes = g3_reads = 0;
}

static void model_wr(uint32_t base, uint32_t off, uint32_t v)
{
    (void)base;
    if(w_total < 512){ w_off[w_total] = off; w_val[w_total] = v; }
    w_total++;
    if(off < 0xC5100u || off > 0xC5120u){ w_outside++; return; }
    if(off == 0xC5100u){ m_g0 = g0_sticky ? (v ^ 0x1u) : v; return; }
    if(off != 0xC5104u) return;                    /* G4, G5: inert   */
    g1_writes++;
    m_g1 = v;
    if(v & (1u << 31)){ m_satoer = 0; m_rdy = 0; m_wait_phase = 0; return; }
    if(!(v & (1u << 30))) return;                  /* no SW_RDY       */
    if(((v >> 25) & 7u) == 4u){                    /* STOP            */
        m_active = 0; m_rdy = 0; m_wait_phase = 0; return;
    }
    {
        uint32_t addr = (v >> 1) & 0x7Fu, count = (v >> 16) & 0x1FFu;
        uint32_t index = ((v >> 25) & 2u) ? ((v >> 8) & 0xFFu) : 0u;
        int ok = mon_present && (int)(m_g0 & 0x1Fu) == mon_pin && addr == 0x50u;
        if(ok && mon_nak_first > 0){ mon_nak_first--; ok = 0; }
        if(ok && index == 128u && mon_nak_index128) ok = 0;
        m_active = 1;
        if(!ok){ m_satoer = 1; m_active = 0; return; }
        m_pos = index; m_left = count;
        m_rdy = !mon_hang; m_wait_phase = 0;
    }
}

static uint32_t model_rd(uint32_t base, uint32_t off)
{
    (void)base;
    r_total++;
    switch(off){
    case 0xC5100u: return m_g0;
    case 0xC5104u: return m_g1 & ~(1u << 30);      /* SW_RDY consumed */
    case 0xC5108u: {
        uint32_t s = 0;
        if(busy_forever || busy_polls > 0){
            if(busy_polls > 0) busy_polls--;
            return 1u << 9;                        /* ACTIVE          */
        }
        if(m_active)     s |= 1u << 9;
        if(m_rdy)        s |= 1u << 11;
        if(m_satoer)     s |= 1u << 10;
        if(m_wait_phase) s |= 1u << 14;
        return s;
    }
    case 0xC510Cu: {
        uint32_t v = 0, k;
        g3_reads++;
        if(!m_rdy) return 0xDEADBEEFu;             /* read too early  */
        for(k = 0; k < 4 && m_left; k++, m_left--)
            v |= (uint32_t)mon_edid[(m_pos++) & 0xFFu] << (8 * k);
        if(m_left == 0){ m_rdy = 0; m_wait_phase = 1; }
        return v;
    }
    default: return 0;
    }
}

#define GFX_RD(base, off)     model_rd((base), (off))
#define GFX_WR(base, off, v)  model_wr((base), (off), (v))
#define GFX_PHYS(addr, len)   ((const uint8_t *)0)

#include "../include/xegmbus.h"

/* PCI and the rest, for drivers/xedisp.c (it formats the screen). */
int pci_count(void){ return 0; }
const pci_dev_t *pci_get(int i){ (void)i; return 0; }
uint16_t pci_cfg_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t r)
{ (void)b; (void)d; (void)f; (void)r; return 0; }
uint32_t pci_cfg_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t r)
{ (void)b; (void)d; (void)f; (void)r; return 0; }
uint32_t pci_bar_mem64(const pci_dev_t *d, int n, uint32_t *hi)
{ (void)d; (void)n; *hi = 0; return 0; }
int highfb_map_io(uint32_t hi, uint32_t lo, uint32_t bytes, uint32_t *out)
{ (void)hi; (void)lo; (void)bytes; (void)out; return 0; }
void kernel_fb_phys(uint32_t *hi, uint32_t *lo, uint32_t *w, uint32_t *h,
                    uint32_t *pitch)
{ *hi = *lo = *w = *h = *pitch = 0; }

#include "../drivers/edid.c"
#include "../drivers/xegmbus.c"
#include "../drivers/xedisp.c"

/* ── checks ───────────────────────────────────────────────────────── */
static int total = 0, fails = 0;
static void ck(const char *what, int ok, long got)
{
    total++;
    if(!ok){ fails++; printf("  FAIL  %s   (got %ld)\n", what, got); }
}
static void head(const char *s){ printf("\n%s\n", s); }

#define MAXL 200
static char lines[MAXL][128];
static int  nl, longest;
static void cap(int tone, const char *s)
{
    int n = (int)strlen(s);
    (void)tone;
    if(n > longest) longest = n;
    if(nl < MAXL){ strncpy(lines[nl], s, 127); nl++; }
}
static int has(const char *needle)
{
    int i;
    for(i = 0; i < nl; i++) if(strstr(lines[i], needle)) return 1;
    return 0;
}

/* ── building EDIDs ───────────────────────────────────────────────── */
typedef struct {
    uint32_t pclk_khz; uint16_t ha, hb, hso, hspw, va, vb, vso, vspw;
    uint8_t hpos, vpos;
} tm_t;

static void enc_dtd(uint8_t *d, const tm_t *t)
{
    uint32_t pc = t->pclk_khz / 10u;
    memset(d, 0, 18);
    d[0] = (uint8_t)pc; d[1] = (uint8_t)(pc >> 8);
    d[2] = (uint8_t)t->ha; d[3] = (uint8_t)t->hb;
    d[4] = (uint8_t)(((t->ha >> 8) << 4) | (t->hb >> 8));
    d[5] = (uint8_t)t->va; d[6] = (uint8_t)t->vb;
    d[7] = (uint8_t)(((t->va >> 8) << 4) | (t->vb >> 8));
    d[8] = (uint8_t)t->hso; d[9] = (uint8_t)t->hspw;
    d[10] = (uint8_t)(((t->vso & 0xF) << 4) | (t->vspw & 0xF));
    d[11] = (uint8_t)(((t->hso >> 8) << 6) | ((t->hspw >> 8) << 4) |
                      ((t->vso >> 4) << 2) | (t->vspw >> 4));
    d[17] = (uint8_t)(0x18 | (t->vpos << 2) | (t->hpos << 1));
}

static void put_text(uint8_t *d, uint8_t tag, const char *s)
{
    int i;
    memset(d, 0, 18); d[3] = tag;
    for(i = 0; i < 13; i++) d[5 + i] = ' ';
    for(i = 0; i < 13 && s[i]; i++) d[5 + i] = (uint8_t)s[i];
    if(i < 13) d[5 + i] = 0x0A;
}

static void fix_sum(uint8_t *b)
{
    uint8_t s = 0; int i;
    for(i = 0; i < 127; i++) s = (uint8_t)(s + b[i]);
    b[127] = (uint8_t)(0x100 - s);
}

/* An LG-like monitor: "GSM", a 1080p60 preferred mode, a range
   descriptor, a name, and (if ext) a CTA block carrying 1080p144 and a
   repeat of the preferred mode. Not the 24GN65R's real bytes -- those
   come off the HP with `gfx edid`. */
static const tm_t T1080_60  = { 148500, 1920, 280, 88, 44, 1080, 45, 4, 5, 1, 1 };
static const tm_t T1080_144 = { 333070, 1920, 160, 48, 32, 1080, 32, 3, 5, 1, 0 };

static void build_mon(uint8_t *e, int ext, int bad_ext_sum)
{
    uint16_t id = (uint16_t)((('G' - 'A' + 1) << 10) | (('S' - 'A' + 1) << 5) |
                             ('M' - 'A' + 1));
    memset(e, 0, 256);
    memcpy(e, "\x00\xFF\xFF\xFF\xFF\xFF\xFF\x00", 8);
    e[8] = (uint8_t)(id >> 8); e[9] = (uint8_t)id;
    e[10] = 0x7F; e[11] = 0x5B;                    /* product 5B7F    */
    e[12] = 0x01; e[13] = 0x02; e[14] = 0x03; e[15] = 0x04;
    e[16] = 30; e[17] = 2024 - 1990;
    e[18] = 1; e[19] = 3;
    enc_dtd(e + 54, &T1080_60);
    {   /* range: 48-144 Hz, 30-160 kHz, 340 MHz */
        uint8_t *d = e + 72; memset(d, 0, 18);
        d[3] = 0xFD; d[5] = 48; d[6] = 144; d[7] = 30; d[8] = 160; d[9] = 34;
    }
    put_text(e + 90, 0xFC, "LG ULTRAGEAR");
    put_text(e + 108, 0xFF, "407NTAB12345");
    e[126] = (uint8_t)(ext ? 1 : 0);
    fix_sum(e);
    if(ext){
        uint8_t *x = e + 128;
        x[0] = 0x02; x[1] = 0x03; x[2] = 4;        /* DTDs start at 4 */
        enc_dtd(x + 4, &T1080_144);
        enc_dtd(x + 22, &T1080_60);                /* a repeat        */
        fix_sum(x);
        if(bad_ext_sum) x[127] ^= 0xFF;
    }
}

static gfx_dev_t     D;
static gmbus_trace_t TR;
static edid_t        ED;
static uint8_t       buf[256];

static void dev_open(void)
{
    memset(&D, 0, sizeof D);
    D.why = GFX_OK; D.mmio = 0x1000u;
}

/* The last write the model saw, and whether it was "GMBUS0 = 0". */
static int ended_released(void)
{
    return w_total > 0 && w_off[w_total - 1] == 0xC5100u && w_val[w_total - 1] == 0;
}

int main(void)
{
    uint32_t got = 0;
    int i, r;

    printf("test_edid: EDID over GMBUS, against a model controller and monitor\n");

    /* ── the parser, against bytes from outside ───────────────────── */
    head("the parser reads the six built-in Linux EDID timings as VESA defines them");
    {
        static const uint8_t k[6][18] = {
            /* 800x600 */ {0xA0,0x0F,0x20,0x00,0x31,0x58,0x1C,0x20,0x28,0x80,0x14,0x00,0x15,0xD0,0x10,0x00,0x00,0x1E},
            /* 1024x768 */ {0x64,0x19,0x00,0x40,0x41,0x00,0x26,0x30,0x08,0x90,0x36,0x00,0x63,0x0A,0x11,0x00,0x00,0x18},
            /* 1280x1024 */ {0x30,0x2A,0x00,0x98,0x51,0x00,0x2A,0x40,0x30,0x70,0x13,0x00,0xBC,0x63,0x11,0x00,0x00,0x1E},
            /* 1600x1200 */ {0x48,0x3F,0x40,0x30,0x62,0xB0,0x32,0x40,0x40,0xC0,0x13,0x00,0x2B,0xA0,0x21,0x00,0x00,0x1E},
            /* 1680x1050 */ {0x21,0x39,0x90,0x30,0x62,0x1A,0x27,0x40,0x68,0xB0,0x36,0x00,0xB5,0x11,0x11,0x00,0x00,0x1E},
            /* 1920x1080 */ {0x02,0x3A,0x80,0x18,0x71,0x38,0x2D,0x40,0x58,0x2C,0x45,0x00,0xF4,0x19,0x11,0x00,0x00,0x1E},
        };
        /* VESA DMT / CEA-861: clock, active, blank, sync offset, sync
           width (h then v), polarity h, v -- and the refresh x100.
           The two kernel-vs-VESA differences are marked K. */
        static const struct { const char *n; uint32_t pc; uint16_t ha, hb, hso, hspw,
            va, vb, vso, vspw; uint8_t hp, vp; uint32_t hz; } t[6] = {
            {"800x600",   40000, 800, 256, 40, 128, 600, 28, 1, 4, 1, 1, 6031},
            {"1024x768",  65000,1024, 320,  8, 144, 768, 38, 3, 6, 0, 0, 6000}, /* K: DMT 24/136 */
            {"1280x1024",108000,1280, 408, 48, 112,1024, 42, 1, 3, 1, 1, 6001},
            {"1600x1200",162000,1600, 560, 64, 192,1200, 50, 1, 3, 1, 1, 6000},
            {"1680x1050",146250,1680, 560,104, 176,1050, 39, 3, 6, 1, 1, 5995}, /* K: DMT -h */
            {"1920x1080",148500,1920, 280, 88,  44,1080, 45, 4, 5, 1, 1, 6000},
        };
        for(i = 0; i < 6; i++){
            edid_mode_t m;
            char what[96];
            int ok = edid_dtd(k[i], &m);
            snprintf(what, sizeof what, "%s: clock, sizes, porches, syncs, polarity",
                     t[i].n);
            ck(what, ok && m.pclk_khz == t[i].pc && m.hactive == t[i].ha &&
               m.hblank == t[i].hb && m.hso == t[i].hso && m.hspw == t[i].hspw &&
               m.vactive == t[i].va && m.vblank == t[i].vb && m.vso == t[i].vso &&
               m.vspw == t[i].vspw && m.hpos == t[i].hp && m.vpos == t[i].vp,
               (long)m.pclk_khz);
            snprintf(what, sizeof what, "  ... refresh %u.%02u Hz", t[i].hz / 100u,
                     t[i].hz % 100u);
            ck(what, m.refresh_x100 == t[i].hz, (long)m.refresh_x100);
        }
    }

    head("a sync offset past 255 (CEA-861 1080p24: front porch 638)");
    {
        /* None of the six kernel timings has an offset over 104, so
           bits 9:8 of the horizontal offset (byte 11, bits 7:6) were
           never exercised. CEA-861 VIC 32, 1920x1080p24: 74.25 MHz,
           htotal 2750, front porch 638, sync 44; vtotal 1125, 4/5. */
        static const tm_t t24 = { 74250, 1920, 830, 638, 44, 1080, 45, 4, 5, 1, 1 };
        uint8_t d[18]; edid_mode_t m;
        enc_dtd(d, &t24);
        edid_dtd(d, &m);
        ck("front porch 638 survives (bits 9:8 from byte 11)", m.hso == 638, m.hso);
        ck("  ... sync 44, blank 830 (htotal 2750)", m.hspw == 44 && m.hblank == 830,
           m.hblank);
        ck("  ... 24.00 Hz", m.refresh_x100 == 2400, (long)m.refresh_x100);
    }

    head("a whole EDID: identity, name, range, and the CTA extension");
    build_mon(buf, 1, 0);
    r = edid_parse(buf, 256, &ED);
    ck("it parses", r == EDID_OK, r);
    ck("maker GSM (LG's PNP ID)", !strcmp(ED.mfg, "GSM"), 0);
    ck("product 5B7F, little-endian", ED.product == 0x5B7Fu, ED.product);
    ck("serial 04030201, little-endian", ED.serial == 0x04030201u, (long)ED.serial);
    ck("week 30 of 2024", ED.week == 30 && ED.year == 2024, ED.year);
    ck("EDID 1.3", ED.version == 1 && ED.revision == 3, ED.revision);
    ck("name trimmed at the 0x0A", !strcmp(ED.name, "LG ULTRAGEAR"), 0);
    ck("serial string", !strcmp(ED.serial_text, "407NTAB12345"), 0);
    ck("range 48-144 Hz, 30-160 kHz, 340 MHz", ED.have_range && ED.vmin == 48 &&
       ED.vmax == 144 && ED.hmin_khz == 30 && ED.hmax_khz == 160 &&
       ED.max_pclk_mhz == 340, ED.vmax);
    ck("one CTA block read", ED.cta_blocks == 1 && ED.bad_ext == 0, ED.cta_blocks);
    ck("two modes: the preferred and the CTA 144 Hz, the repeat dropped",
       ED.nmodes == 2, ED.nmodes);
    ck("modes[0] is the preferred 1080p60 from block 0",
       ED.modes[0].block == 0 && ED.modes[0].pclk_khz == 148500 &&
       ED.modes[0].refresh_x100 == 6000, (long)ED.modes[0].refresh_x100);
    ck("modes[1] is 1080p144 from block 1", ED.modes[1].block == 1 &&
       ED.modes[1].refresh_x100 == 14400, (long)ED.modes[1].refresh_x100);
    /* +h -v: the only mode here whose polarities DIFFER, so the only
       one that can tell the hsync bit from the vsync bit. Every kernel
       EDID has them equal. */
    ck("  ... +hsync -vsync, each read from its own bit",
       ED.modes[1].hpos == 1 && ED.modes[1].vpos == 0, ED.modes[1].hpos);

    build_mon(buf, 1, 1);
    r = edid_parse(buf, 256, &ED);
    ck("a bad extension checksum keeps the base block", r == EDID_OK &&
       ED.bad_ext == 1 && ED.nmodes == 1, ED.bad_ext);
    /* A VALID CTA block in the buffer that byte 126 does not announce.
       The first version copied block 0 there instead -- which is not a
       CTA block, so it was ignored whether or not the count was obeyed,
       and check_edid_suite.py caught the suite passing over that. */
    build_mon(buf, 1, 0);
    buf[126] = 0; fix_sum(buf);
    r = edid_parse(buf, 256, &ED);
    ck("a valid CTA block present but not announced is ignored", r == EDID_OK &&
       ED.cta_blocks == 0 && ED.nmodes == 1, ED.cta_blocks);
    build_mon(buf, 1, 0);
    r = edid_parse(buf, 128, &ED);
    ck("an announced block that was not read is not parsed", r == EDID_OK &&
       ED.cta_blocks == 0 && ED.extensions == 1, ED.cta_blocks);
    build_mon(buf, 0, 0); buf[0] = 0x01;
    ck("a bad header is EHEADER", edid_parse(buf, 128, &ED) == EDID_EHEADER, ED.why);
    build_mon(buf, 0, 0); buf[60] ^= 1;
    ck("a bad checksum is ESUM", edid_parse(buf, 128, &ED) == EDID_ESUM, ED.why);
    ck("short is ESHORT", edid_parse(buf, 100, &ED) == EDID_ESHORT, ED.why);
    ck("NULL is ESHORT", edid_parse(0, 128, &ED) == EDID_ESHORT, ED.why);
    build_mon(buf, 0, 0);
    buf[72 + 4] = 0x02;                  /* EDID 1.4: vmax + 255       */
    fix_sum(buf);
    edid_parse(buf, 128, &ED);
    ck("an EDID 1.4 range offset adds 255 to vmax (399 Hz)", ED.vmax == 144 + 255,
       ED.vmax);
    {   /* The cap. Two blocks hold at most ten timings (four in the
           base, six in a CTA block), so it takes a FOUR-block buffer --
           22 distinct timings -- to reach EDID_MAX_MODES at all. */
        static uint8_t x4[512]; int k, b;
        build_mon(x4, 1, 0);
        x4[126] = 3; fix_sum(x4);
        for(b = 1; b < 4; b++){
            uint8_t *x = x4 + b * 128;
            memset(x, 0, 128); x[0] = 2; x[1] = 3; x[2] = 4;
            for(k = 0; k < 6; k++){
                tm_t t = T1080_60;
                t.pclk_khz = 200000u + (uint32_t)(b * 10 + k) * 1000u;
                enc_dtd(x + 4 + k * 18, &t);
            }
            fix_sum(x);
        }
        edid_parse(x4, 512, &ED);
        ck("22 timings into 12 slots: capped at 12, nothing written past",
           ED.nmodes == EDID_MAX_MODES && ED.cta_blocks == 3, ED.nmodes);
    }
    {   /* seven distinct modes in two blocks */
        uint8_t x[256]; int k;
        build_mon(x, 1, 0);
        memset(x + 128, 0, 128); x[128] = 2; x[129] = 3; x[130] = 4;
        for(k = 0; k < 6; k++){
            tm_t t = T1080_60; t.pclk_khz = 100000u + (uint32_t)k * 1000u;
            enc_dtd(x + 128 + 4 + k * 18, &t);
        }
        fix_sum(x + 128);
        edid_parse(x, 256, &ED);
        ck("seven distinct modes all kept (1 + 6)", ED.nmodes == 7, ED.nmodes);
    }
    {
        uint8_t d[18]; edid_mode_t m;
        enc_dtd(d, &T1080_60); d[17] |= 0x80;
        edid_dtd(d, &m);
        ck("the interlace bit is read", m.interlaced == 1, m.interlaced);
        memset(d, 0, 18); d[3] = 0xFC;
        ck("a text descriptor is not a timing", edid_dtd(d, &m) == 0, 0);
    }

    /* ── GMBUS ───────────────────────────────────────────────────── */
    head("reading the monitor over GMBUS pin 2");
    model_reset(); dev_open(); build_mon(mon_edid, 1, 0);
    r = gmbus_read_edid(&D, 2, buf, &got, &TR);
    ck("it reads", r == GMBUS_OK && TR.why == GMBUS_OK, r);
    ck("  ... both blocks, 256 bytes", got == 256, (long)got);
    ck("  ... byte for byte what the monitor holds", !memcmp(buf, mon_edid, 256), 0);
    ck("  ... in 64 GMBUS3 reads, four bytes each", g3_reads == 64, g3_reads);
    ck("NOT ONE write left the six GMBUS registers", w_outside == 0, w_outside);
    ck("  ... and the guard refused nothing (nothing was asked)",
       TR.refused == 0, (long)TR.refused);
    ck("the bus was released at the end: GMBUS0 = 0 last", ended_released(), 0);
    ck("no retry was needed", TR.retried == 0, TR.retried);
    {
        int k, g0_ok = 1, saw_idx0 = 0, saw_idx128 = 0, saw_stop = 0;
        for(k = 0; k < TR.nwlog; k++){
            const gmbus_wlog_t *w = &TR.wlog[k];
            if(w->off == 0xC5100u && w->back != w->val) g0_ok = 0;
            if(w->off == 0xC5104u && (w->val & (1u << 30)) && ((w->val >> 25) & 2u)){
                uint32_t idx = (w->val >> 8) & 0xFFu, cnt = (w->val >> 16) & 0x1FFu;
                if(idx == 0 && cnt == 128 && ((w->val >> 1) & 0x7Fu) == 0x50 &&
                   (w->val & 1u)) saw_idx0 = 1;
                if(idx == 128 && cnt == 128) saw_idx128 = 1;
            }
            if(w->off == 0xC5104u && ((w->val >> 25) & 7u) == 4u) saw_stop = 1;
        }
        ck("every GMBUS0 write read back what was written", g0_ok, 0);
        ck("block 0: an INDEX read of 128 at 0x50, index 0, READ", saw_idx0, 0);
        ck("block 1: the same at index 128", saw_idx128, 0);
        ck("each transfer ended with a STOP cycle", saw_stop, 0);
    }
    ck("GMBUS1's read-back is logged without SW_RDY (consumed), not compared",
       TR.nwlog > 1 && TR.wlog[1].off == 0xC5104u &&
       !(TR.wlog[1].back & (1u << 30)), (long)TR.wlog[1].back);

    head("one block when the monitor announces no extension");
    model_reset(); dev_open(); build_mon(mon_edid, 0, 0);
    gmbus_read_edid(&D, 2, buf, &got, &TR);
    ck("128 bytes, one transfer", got == 128 && g1_writes == 2, g1_writes);

    head("nothing on the wire: NAK, retried once, cleaned up the i915 way");
    model_reset(); dev_open(); mon_present = 0;
    r = gmbus_read_edid(&D, 2, buf, &got, &TR);
    ck("ENAK", r == GMBUS_ENAK, r);
    ck("  ... after one retry", TR.retried == 1, TR.retried);
    ck("  ... nothing came back", got == 0, (long)got);
    {
        int k, clr = 0, zero = 0;
        for(k = 0; k < TR.nwlog; k++){
            if(TR.wlog[k].off == 0xC5104u && TR.wlog[k].val == (1u << 31)) clr++;
            if(TR.wlog[k].off == 0xC5104u && TR.wlog[k].val == 0) zero++;
        }
        ck("  ... SW_CLR_INT after each NAK", clr == 2, clr);
        ck("  ... then GMBUS1 = 0 after each", zero == 2, zero);
    }
    ck("  ... and the pin released", ended_released(), 0);
    ck("  ... with nothing written outside GMBUS", w_outside == 0, w_outside);
    ck("  ... and the SATOER latch cleared for the next transfer", m_satoer == 0, m_satoer);

    model_reset(); dev_open(); build_mon(mon_edid, 1, 0); mon_nak_first = 1;
    r = gmbus_read_edid(&D, 2, buf, &got, &TR);
    ck("a monitor that misses the first address is read on the retry",
       r == GMBUS_OK && got == 256 && TR.retried == 1, r);

    model_reset(); dev_open(); build_mon(mon_edid, 1, 0); mon_pin = 9;
    r = gmbus_read_edid(&D, 2, buf, &got, &TR);
    ck("the monitor on a different pin is not reached on pin 2", r == GMBUS_ENAK, r);
    model_reset(); dev_open(); build_mon(mon_edid, 1, 0); mon_pin = 9;
    r = gmbus_read_edid(&D, 9, buf, &got, &TR);
    ck("  ... and is on pin 9", r == GMBUS_OK && got == 256, r);

    model_reset(); dev_open(); build_mon(mon_edid, 1, 0); mon_nak_index128 = 1;
    r = gmbus_read_edid(&D, 2, buf, &got, &TR);
    ck("block 1 failing keeps block 0", r == GMBUS_OK && got == 128 &&
       TR.why == GMBUS_ENAK, (long)got);

    head("a controller that hangs, lies, or is busy");
    model_reset(); dev_open(); build_mon(mon_edid, 1, 0); mon_hang = 1;
    r = gmbus_read_edid(&D, 2, buf, &got, &TR);
    ck("never ready is ETIMEOUT, bounded", r == GMBUS_ETIMEOUT, r);
    ck("  ... and the pin is released", ended_released(), 0);
    ck("  ... GMBUS2 is kept for the screen", (TR.status & (1u << 9)) != 0,
       (long)TR.status);

    model_reset(); dev_open(); build_mon(mon_edid, 1, 0); g0_sticky = 1;
    r = gmbus_read_edid(&D, 2, buf, &got, &TR);
    /* The one write that matters most: if GMBUS0 does not hold the
       pin, a transfer would run on whatever pin it does hold. */
    ck("GMBUS0 not holding the pin is ESTUCK", r == GMBUS_ESTUCK, r);
    ck("  ... and NO command was issued", g1_writes == 0, g1_writes);

    model_reset(); dev_open(); build_mon(mon_edid, 1, 0); busy_forever = 1;
    r = gmbus_read_edid(&D, 2, buf, &got, &TR);
    ck("a bus busy forever is EBUSY", r == GMBUS_EBUSY, r);
    ck("  ... and nothing at all was written", w_total == 0, w_total);

    model_reset(); dev_open(); build_mon(mon_edid, 1, 0); busy_polls = 50;
    r = gmbus_read_edid(&D, 2, buf, &got, &TR);
    ck("a bus busy for a moment is waited for", r == GMBUS_OK && got == 256, r);

    head("refusals before the bus is touched");
    model_reset(); dev_open(); build_mon(mon_edid, 1, 0);
    r = gmbus_read_edid(&D, 3, buf, &got, &TR);
    ck("pin 3 is not an ADL-P pin: EPIN", r == GMBUS_EPIN, r);
    ck("  ... with no write and no read", w_total == 0 && r_total == 0, w_total);
    for(i = 0; i <= 13; i++){
        int want = (i == 1 || i == 2 || (i >= 9 && i <= 12));
        if(gmbus_pin_valid(i) != want){ ck("the pin table", 0, i); break; }
    }
    ck("valid pins are exactly 1, 2, 9-12", 1, 0);
    model_reset(); dev_open(); D.why = GFX_EDEAD;
    r = gmbus_read_edid(&D, 2, buf, &got, &TR);
    ck("no display engine is ENODEV, untouched", r == GMBUS_ENODEV &&
       w_total == 0 && r_total == 0, r);

    head("the guard: gmbus_wr() refuses anything outside GMBUS");
    model_reset(); dev_open(); memset(&TR, 0, sizeof TR);
    gmbus_wr(&D, &TR, 0x70180u, 0);           /* PLANE_CTL_1_A          */
    gmbus_wr(&D, &TR, 0x45404u, 0);           /* the power wells        */
    gmbus_wr(&D, &TR, 0xC50FCu, 0);           /* one below GMBUS0       */
    gmbus_wr(&D, &TR, 0xC5124u, 0);           /* one past GMBUS5        */
    gmbus_wr(&D, &TR, 0xC5102u, 0);           /* unaligned              */
    ck("five refused", TR.refused == 5, (long)TR.refused);
    ck("  ... and the model saw none of them", w_total == 0, w_total);
    gmbus_wr(&D, &TR, 0xC5100u, 0);
    gmbus_wr(&D, &TR, 0xC5120u, 0);
    ck("the first and last GMBUS registers are allowed", w_total == 2 &&
       TR.refused == 5, w_total);

    /* ── the screen ──────────────────────────────────────────────── */
    head("what `gfx edid` puts on the screen");
    model_reset(); dev_open(); build_mon(mon_edid, 1, 0);
    gmbus_read_edid(&D, 2, buf, &got, &TR);
    edid_parse(buf, got, &ED);
    nl = 0; longest = 0;
    gfx_report_edid(&TR, &ED, buf, got, cap);
    ck("names the pin and port", has("GMBUS pin 2 (DDI B), I2C address 50"), nl);
    ck("says every write stayed in GMBUS", has("every one to GMBUS (C5100-C5120); 0 refused"), nl);
    ck("maker, product, week and year", has("maker GSM  product 5B7F  made week 30 of 2024"), nl);
    ck("the name", has("name  LG ULTRAGEAR"), nl);
    ck("the limits", has("limits 48-144 Hz, 30-160 kHz, pixel clock up to 340 MHz"), nl);
    ck("the preferred mode first", has("  1920x1080  60.00 Hz  148.5 MHz  total 2200x1125 +h+v"), nl);
    ck("the 144 Hz mode from block 1", has("144.00 Hz  333.07 MHz") && has("(block 1)"), nl);
    ck("sixteen raw lines for 256 bytes", has("0F0:") && has("000: 00 FF FF FF FF FF FF 00"), nl);
    ck("the write log with read-backs", has("C5100=00000002/00000002"), nl);
    ck("no line wider than the screen", longest <= GFX_LINE_MAX, longest);
    ck("none cut", gfx_test_cut() == 0, (long)gfx_test_cut());

    model_reset(); dev_open(); mon_present = 0;
    gmbus_read_edid(&D, 2, buf, &got, &TR);
    nl = 0; longest = 0; memset(&ED, 0, sizeof ED); ED.why = EDID_ESHORT;
    gfx_report_edid(&TR, &ED, buf, 0, cap);
    ck("a NAK says what to check", has("Nothing answered at 50, twice") &&
       has("HDMI port"), nl);
    ck("  ... and that it was retried", has("retried once"), nl);
    ck("  ... and still lists the writes", has("C5100=00000002/00000002"), nl);

    model_reset(); dev_open(); g0_sticky = 1;
    gmbus_read_edid(&D, 2, buf, &got, &TR);
    nl = 0; memset(&ED, 0, sizeof ED); ED.why = EDID_ESHORT;
    gfx_report_edid(&TR, &ED, buf, 0, cap);
    ck("ESTUCK says nothing was sent", has("so nothing was sent"), nl);

    model_reset(); dev_open(); build_mon(mon_edid, 0, 0); mon_edid[0] = 0x55;
    gmbus_read_edid(&D, 2, buf, &got, &TR);
    edid_parse(buf, got, &ED);
    nl = 0;
    gfx_report_edid(&TR, &ED, buf, got, cap);
    ck("bytes that are not an EDID are named and still dumped",
       has("not an EDID") && has("000: 55 FF"), nl);
    ck("no line was cut anywhere in this run", gfx_test_cut() == 0, (long)gfx_test_cut());

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
