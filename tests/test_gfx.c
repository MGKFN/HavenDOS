/* ════════════════════════════════════════════════════════════════════
   test_gfx.c -- the Iris Xe inspector (drivers/xedisp.c), against a
   model of the display engine.

   THIS CODE HAS NEVER SEEN AN IRIS XE. It was written from the Linux
   i915 driver's register definitions for display version 13, and the
   first engine it reads will be the HP's. QEMU has no Intel display
   engine at all, so there is no emulator rung on this ladder: this
   suite and the photographs are the only two.

   So the model is written to be STRICT rather than agreeable:

     - Its registers belong to POWER WELLS, the way display 13's do
       (i915 xelpd_power_wells). A read of a register whose well is
       off returns zero AND sets FPGA_DBG bit 31, the engine's own
       "unclaimed access" flag. The suite counts those reads directly:
       a gate in the driver that is wrong shows up as a number, not
       as a plausible-looking zero in a report.
     - It has NO write path. The driver's seam is GFX_RD only; there
       is no GFX_WR to define and no pci_cfg_write to link against.
       A write added to drivers/xedisp.c would fail to build here, and
       tools/check_gfx_readonly.py refuses it in the kernel build too.
     - Every line the report emits is captured, so the checks assert
       the TEXT a person will photograph -- including that no line was
       cut at the screen edge, which on this screen would be a
       register value with its last digits missing.

   ── WHAT THIS CANNOT PROVE ──────────────────────────────────────────

   That the offsets are right. The model and the driver both come from
   i915's headers, and a driver that agrees with its own reading of a
   header proves nothing about the silicon. The HP's first `gfx` is the
   test of that, and it is why `gfx regs` prints every raw value.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define GFX_TEST 1

/* ── the model engine ─────────────────────────────────────────────── */
#define SPAN      0x200000u
static uint32_t  *regs;                   /* SPAN/4 words, calloc'd   */
static uint32_t   model_wells;            /* bit idx = well idx on    */
static uint32_t   model_lie_wells;        /* wells the STATE bits claim
                                             on that are really off   */
static int        unclaimed;              /* reads of a dark register */
/* Reads counted PER REGISTER. The first version counted per 4 KB
   page, and DDI A/B (0x64000, 0x64100) share a page with the Type-C
   buffers (0x64300+): "no TC read" failed on reads of DDI A and B.
   A fixture that cannot tell two registers apart cannot test a gate
   between them. */
static int        reads_in[0x200000 / 4];
static int        dead;                   /* everything reads FFFFFFFF */
static int        tc_cold;                /* TCSS reads FFFFFFFF      */

#define W_PW1  0
#define W_PW2  1
#define W_PWA  5
#define W_PWB  6
#define W_PWC  7
#define W_PWD  8
#define W_PW0  (-1)                       /* always on                */

/* Which well each register lives in, on display 13. This is the
   model's statement, written from i915's power map -- NOT copied from
   the driver's gates, which is the point. */
static int owner(uint32_t off)
{
    if(off == 0x42300u || off == 0x42000u) return W_PW0;   /* FPGA, fuse */
    if(off >= 0x45400u && off < 0x45600u)  return W_PW0;   /* PWR_WELL, DC */
    if(off >= 0x8F000u && off < 0x90000u)  return W_PW0;   /* DMC        */
    if(off >= 0xC4000u && off < 0xC5000u)  return W_PW0;   /* south (PCH) */
    /* transcoders: A in PW_1, B..D in their own wells; PIPESRC is a
       pipe register at a transcoder address */
    if(off >= 0x60000u && off < 0x64000u){
        int t = (int)((off - 0x60000u) >> 12);
        if((off & 0xFFFu) == 0x01Cu) return W_PWA + t;
        return t == 0 ? W_PW1 : W_PWA + t;
    }
    if(off >= 0x70000u && off < 0x74000u){
        int p = (int)((off - 0x70000u) >> 12);
        if((off & 0xFFFu) == 0x008u) return p == 0 ? W_PW1 : W_PWA + p;
        return W_PWA + p;                                  /* planes     */
    }
    if(off >= 0x64000u && off < 0x64200u)  return W_PW1;   /* DDI A, B   */
    if(off >= 0x64300u && off < 0x64700u)  return W_PW2;   /* DDI TC1-4  */
    if(off >= 0x161500u && off < 0x161510u) return W_PW2;  /* TCSS       */
    if(off >= 0x163000u && off < 0x164000u) return W_PW2;  /* FIA1       */
    return W_PW1;                       /* clocks, PLLs, north HPD ... */
}

static int well_really_on(int w){ return w == W_PW0 || ((model_wells >> w) & 1u); }

static uint32_t model_rd(uint32_t base, uint32_t off)
{
    int w;
    (void)base;
    if(off >= SPAN){ fprintf(stderr, "read past span %x\n", off); abort(); }
    reads_in[off >> 2]++;
    if(dead) return 0xFFFFFFFFu;
    /* The power-well registers report the state the test chose --
       and, for a "lying" well, claim it on when it is not. */
    if(off >= 0x45400u && off <= 0x4540Cu){
        uint32_t v = regs[off / 4u], i;
        for(i = 0; i < 16; i++)
            if(((model_wells | model_lie_wells) >> i) & 1u) v |= 1u << (i * 2);
        return v;
    }
    w = owner(off);
    if(!well_really_on(w)){
        unclaimed++;
        regs[0x42300u / 4u] |= 1u << 31;          /* FPGA_DBG_RM_NOCLAIM */
        return 0;
    }
    if(tc_cold && off >= 0x161500u && off < 0x161510u) return 0xFFFFFFFFu;
    return regs[off / 4u];
}
static void R(uint32_t off, uint32_t v){ regs[off / 4u] = v; }

#define GFX_RD(base, off)   model_rd((base), (off))

/* ── physical memory, for the OpRegion and the VBT ────────────────── */
#define PHYS_BASE  0x7A000000u
#define PHYS_LEN   0x20000u
static uint8_t phys[PHYS_LEN];
static int     phys_calls;
static const uint8_t *fake_phys(uint32_t addr, uint32_t len)
{
    phys_calls++;
    if(addr < PHYS_BASE || len > PHYS_LEN) return 0;
    if(addr - PHYS_BASE > PHYS_LEN - len) return 0;
    return phys + (addr - PHYS_BASE);
}
#define GFX_PHYS(addr, len) fake_phys((addr), (len))

#include "../include/xedisp.h"

/* ── PCI, faked: reads only. There is deliberately no
   pci_cfg_write*, pci_enable or pci_set_d0 here: if drivers/xedisp.c
   ever calls one, this suite stops LINKING. ──────────────────────── */
static pci_dev_t fake_igd;
static int       have_igd = 1;
static uint16_t  cfg_cmd = 0x0007;
static uint32_t  cfg_asls = PHYS_BASE;
static uint32_t  bar0_hi = 0x60u, bar0_lo = 0x01000000u;
static uint32_t  bar2_hi = 0x40u, bar2_lo = 0x00000000u;
int pci_count(void){ return have_igd ? 1 : 0; }
const pci_dev_t *pci_get(int i){ return (i == 0 && have_igd) ? &fake_igd : 0; }
uint16_t pci_cfg_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t reg)
{ (void)b; (void)d; (void)f; return reg == 0x04 ? cfg_cmd : 0; }
uint32_t pci_cfg_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t reg)
{ (void)b; (void)d; (void)f; return reg == 0xFC ? cfg_asls : 0; }
uint32_t pci_bar_mem64(const pci_dev_t *d, int n, uint32_t *hi)
{
    (void)d;
    if(n == 0){ *hi = bar0_hi; return bar0_lo; }
    if(n == 2){ *hi = bar2_hi; return bar2_lo; }
    *hi = 0; return 0;
}
static int      map_calls, map_ok = 1;
static uint32_t map_hi, map_lo, map_bytes;
int highfb_map_io(uint32_t hi, uint32_t lo, uint32_t bytes, uint32_t *out)
{
    map_calls++; map_hi = hi; map_lo = lo; map_bytes = bytes;
    if(!map_ok) return 0;
    *out = 0xE3000000u;
    return 1;
}
static uint32_t fb_hi = 0x40u, fb_lo = 0x00400000u, fb_w = 1920, fb_h = 1080;
void kernel_fb_phys(uint32_t *hi, uint32_t *lo, uint32_t *w, uint32_t *h,
                    uint32_t *pitch)
{ *hi = fb_hi; *lo = fb_lo; *w = fb_w; *h = fb_h; *pitch = fb_w * 4u; }

#include "../drivers/xedisp.c"

/* ── checks ───────────────────────────────────────────────────────── */
static int total = 0, fails = 0;
static void ck(const char *what, int ok, long got)
{
    total++;
    if(!ok){ fails++; printf("  FAIL  %s   (got %ld)\n", what, got); }
}
static void head(const char *s){ printf("\n%s\n", s); }

/* The report, captured. */
#define MAXL 400
static char lines[MAXL][128];
static int  tones[MAXL];
static int  nl, longest;
static void cap(int tone, const char *s)
{
    int n = (int)strlen(s);
    if(n > longest) longest = n;
    if(nl < MAXL){ strncpy(lines[nl], s, 127); tones[nl] = tone; nl++; }
}
static void cap_reset(void){ nl = 0; longest = 0; }
static int has(const char *needle)
{
    int i;
    for(i = 0; i < nl; i++) if(strstr(lines[i], needle)) return 1;
    return 0;
}
static int has_tone(const char *needle, int tone)
{
    int i;
    for(i = 0; i < nl; i++)
        if(strstr(lines[i], needle) && tones[i] == tone) return 1;
    return 0;
}
/* The ONE line that begins with `prefix` -- for per-port rows, where
   has() would be satisfied by any other port saying the same thing.
   (check_gfx_suite.py found that: "detect off" was asserted for DDI B
   and passed on the Type-C rows.) */
static const char *line_of(const char *prefix)
{
    int i; size_t n = strlen(prefix);
    for(i = 0; i < nl; i++) if(!strncmp(lines[i], prefix, n)) return lines[i];
    return "";
}
static void dump(void)
{
    int i;
    for(i = 0; i < nl; i++) printf("      | %s\n", lines[i]);
}

/* ── a VBT, built ─────────────────────────────────────────────────── */
static uint8_t vbtbuf[0x2000];
static uint32_t vbt_len;

typedef struct { uint16_t type; uint8_t dvo, ddc, aux, f1, f2; } kid_t;

static void p16(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v){ p16(p, v); p16(p + 2, v >> 16); }

/* opts: bit0 = put a MIPI sequence block (v3, 32-bit size, with a
   16-bit size field that LIES) before block 2. */
static uint32_t build_vbt(uint8_t *out, uint16_t bdbver, int child_size,
                          const kid_t *k, int nk, int slots, int opts)
{
    uint32_t i = 0, bdb, sz;
    int c;
    memset(out, 0, 0x2000);
    memcpy(out, "$VBT HAVENDOS TEST  ", 20);
    p16(out + 20, 100); p16(out + 22, 0x30);
    bdb = 0x30;
    p32(out + 28, bdb);
    i = bdb;
    memcpy(out + i, "BIOS_DATA_BLOCK ", 16);
    p16(out + i + 16, bdbver); p16(out + i + 18, 22);
    i += 22;
    /* block 1, general features: 8 bytes of nothing */
    out[i] = 1; p16(out + i + 1, 8); i += 3 + 8;
    if(opts & 1){
        /* block 53, version 3: header byte 3 = version, then a u32
           size. Its 16-bit size field says 4, which is a lie that
           would land the walk in the middle of the block. */
        /* The payload is JUNK, not zeros: with zeros a walk that
           took the 16-bit size crept through 0-sized "blocks" three
           bytes at a time and landed on block 2 anyway. The harness
           caught the suite passing over that. */
        out[i] = 53; p16(out + i + 1, 4); out[i + 3] = 3;
        p32(out + i + 4, 40);
        memset(out + i + 8, 0xA5, 35);
        i += 3 + 40;
    }
    out[i] = 2;
    sz = 5u + (uint32_t)slots * (uint32_t)child_size;
    p16(out + i + 1, sz);
    i += 3;
    out[i + 4] = (uint8_t)child_size;
    for(c = 0; c < nk && c < slots; c++){
        uint8_t *ch = out + i + 5 + c * child_size;
        p16(ch + 0, 0x100 + (uint32_t)c);
        p16(ch + 2, k[c].type);
        ch[16] = k[c].dvo; ch[19] = k[c].ddc;
        ch[23] = k[c].f1;  ch[24] = k[c].f2; ch[25] = k[c].aux;
    }
    i += sz;
    p16(out + bdb + 20, i - bdb);           /* bdb_size */
    p16(out + 24, i);                       /* vbt_size */
    return i;
}

/* The HP as it is believed to be: eDP on DDI A, HDMI on DDI B, two
   Type-C ports that do DP. */
static const kid_t HP_KIDS[] = {
    { 0x78C6, 10, 0, 0x40, 0x00, 0x02 },   /* eDP, DPA, AUX A        */
    { 0x60D2,  1, 2, 0x00, 0x00, 0x01 },   /* HDMI, HDMIB, DDC 2     */
    { 0x68C6, 13, 0, 0x60, 0x00, 0x02 },   /* DP, DPF -> TC1, AUX F  */
    { 0x68C6, 15, 0, 0x70, 0x00, 0x02 },   /* DP, DPG -> TC2, AUX G  */
};

/* An OpRegion at PHYS_BASE with the VBT in mailbox 4 (or via RVDA). */
static void put_opregion(int major, int minor, uint32_t mboxes,
                         const uint8_t *vbt, uint32_t len,
                         uint32_t rvda, uint32_t rvda_hi, uint32_t rvds)
{
    memset(phys, 0, sizeof phys);
    memcpy(phys, "IntelGraphicsMem", 16);
    phys[0x16] = (uint8_t)minor; phys[0x17] = (uint8_t)major;
    p32(phys + 0x58, mboxes);
    p32(phys + 0x3BA, rvda); p32(phys + 0x3BE, rvda_hi); p32(phys + 0x3C2, rvds);
    if(vbt && len <= 0x1800u) memcpy(phys + 0x400, vbt, len);
}

/* ── the HP, as the firmware is believed to leave it ──────────────── */
static void hp_state(void)
{
    memset(regs, 0, SPAN);
    memset(reads_in, 0, sizeof reads_in);
    unclaimed = 0; dead = 0; tc_cold = 0; model_lie_wells = 0;
    /* PW_1 and PW_A on, the firmware asked for them; nothing else. */
    model_wells = (1u << W_PW1) | (1u << W_PWA);
    R(0x45400, (2u << (W_PW1 * 2)) | (2u << (W_PWA * 2)));
    R(0x42000, 1u << 31);                              /* fuses in    */
    /* CDCLK: 38.4 MHz reference, ratio 16, CD2X /1 -> 307.2 MHz */
    R(0x51004, 2u << 29);
    R(0x46070, (1u << 31) | (1u << 30) | 16u);
    R(0x46000, 0);
    /* DPLL0 on and locked, clocking DDI A; DDI B clock gated */
    R(0x46010, (1u << 31) | (1u << 30) | (1u << 27) | (1u << 26));
    R(0x164284, 0x00E001A5u); R(0x164288, 0x00000088u);
    R(0x164280, (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 21));
    /* transcoder A -> DDI A, DP SST, 8 bpc, 4 lanes, on */
    R(0x60400, (1u << 31) | ((0u + 1u) << 27) | (2u << 24) | (0u << 20) | (3u << 1));
    R(0x60000, ((2080u - 1u) << 16) | (1920u - 1u));
    R(0x6000C, ((1111u - 1u) << 16) | (1080u - 1u));
    R(0x60008, ((1968u - 1u) << 16) | (1936u - 1u));
    R(0x6001C, ((1920u - 1u) << 16) | (1080u - 1u));
    R(0x70008, (1u << 31) | (1u << 30));
    /* plane 1 A: on, XRGB8888 (ICL field 8), linear, stride 7680 */
    R(0x70180, (1u << 31) | (8u << 23));
    R(0x70188, 7680u / 64u);
    R(0x70190, ((1080u - 1u) << 16) | (1920u - 1u));
    R(0x7019C, 0x00400000u);
    /* DDI A buffer on, DDI B off. HPD: detect enabled on B, nothing in. */
    R(0x64000, 1u << 31);
    R(0xC4030, 8u << 4);                                /* B enabled   */
    /* the rest of the PCI side */
    have_igd = 1; cfg_cmd = 0x0007; cfg_asls = PHYS_BASE;
    bar0_hi = 0x60u; bar0_lo = 0x01000000u;
    bar2_hi = 0x40u; bar2_lo = 0x00000000u;
    fb_hi = 0x40u; fb_lo = 0x00400000u; fb_w = 1920; fb_h = 1080;
    memset(&fake_igd, 0, sizeof fake_igd);
    fake_igd.bus = 0; fake_igd.dev = 2; fake_igd.fn = 0;
    fake_igd.vendor = 0x8086; fake_igd.device = 0x46A8; fake_igd.class = 0x03;
    map_ok = 1;
    vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 8, 0);
    put_opregion(2, 1, 0x7u, vbtbuf, vbt_len, 0, 0, 0);
}

static gfx_dev_t  D;
static gfx_regs_t Rg;
static gfx_vbt_t  V;

static void run_all(void)
{
    gfx_test_unmap();
    gfx_open(&D);
    gfx_read(&D, &Rg);
    gfx_vbt_find(&D, &V);
    cap_reset();
    gfx_report(&D, &Rg, &V, cap);
}

static int reads_between(uint32_t lo, uint32_t hi)
{
    int n = 0; uint32_t p;
    for(p = lo >> 2; p <= (hi - 1u) >> 2; p++) n += reads_in[p];
    return n;
}

int main(void)
{
    uint32_t cut0;
    int i;

    regs = calloc(SPAN / 4u, 4u);
    if(!regs) return 2;
    printf("test_gfx: the Iris Xe inspector, against a model engine\n");

    /* ── opening ─────────────────────────────────────────────────── */
    head("opening: refusals come before any register is touched");
    hp_state(); have_igd = 0; map_calls = 0;
    gfx_test_unmap(); gfx_open(&D);
    ck("no Intel display at 00:02.0 is ENODEV", D.why == GFX_ENODEV, D.why);
    ck("  ... and nothing was mapped", map_calls == 0, map_calls);

    hp_state(); fake_igd.device = 0x9A49; map_calls = 0;      /* Tiger Lake */
    memset(reads_in, 0, sizeof reads_in);
    gfx_test_unmap(); gfx_open(&D); gfx_read(&D, &Rg);
    ck("a Tiger Lake ID is refused: not display 13", D.why == GFX_EUNKNOWN, D.why);
    ck("  ... nothing was mapped", map_calls == 0, map_calls);
    ck("  ... and NO register was read", reads_between(0, SPAN) == 0,
       reads_between(0, SPAN));
    cap_reset(); gfx_report(&D, &Rg, &V, cap);
    ck("  ... and the screen says why", has("not display version 13"), nl);

    hp_state(); fake_igd.device = 0xA7A0; gfx_test_unmap(); gfx_open(&D);
    ck("a Raptor Lake-P ID is display 13 and is accepted", D.why == GFX_OK, D.why);
    hp_state(); fake_igd.device = 0x46D1; gfx_test_unmap(); gfx_open(&D);
    ck("an Alder Lake-N ID is accepted", D.why == GFX_OK, D.why);

    hp_state(); cfg_cmd = 0x0004; map_calls = 0; gfx_test_unmap(); gfx_open(&D);
    ck("memory decoding off is EMEMOFF, not turned on", D.why == GFX_EMEMOFF, D.why);
    ck("  ... and nothing was mapped", map_calls == 0, map_calls);

    hp_state(); bar0_hi = 0; bar0_lo = 0; gfx_test_unmap(); gfx_open(&D);
    ck("an unassigned BAR0 is ENOBAR", D.why == GFX_ENOBAR, D.why);

    hp_state(); map_calls = 0; gfx_test_unmap(); gfx_open(&D);
    ck("a BAR0 above 4 GB opens", D.why == GFX_OK, D.why);
    ck("  ... through highfb_map_io, once", map_calls == 1, map_calls);
    ck("  ... with the BAR's high half", map_hi == 0x60u, (long)map_hi);
    ck("  ... and its low half", map_lo == 0x01000000u, (long)map_lo);
    /* 2 MB and no more: the GTT sits above the registers in BAR0. */
    ck("  ... for exactly the 2 MB register span", map_bytes == 0x200000u,
       (long)map_bytes);
    ck("  ... and the registers are read at the mapped address",
       D.mmio == 0xE3000000u, (long)D.mmio);
    gfx_open(&D); gfx_open(&D); gfx_open(&D);
    /* highfb has eight windows for the life of the machine. A command
       that mapped every time would exhaust them on the fifth run. */
    ck("running it four more times maps nothing more", map_calls == 1, map_calls);

    hp_state(); bar0_hi = 0; bar0_lo = 0xA0000000u; map_calls = 0;
    gfx_test_unmap(); gfx_open(&D);
    ck("a BAR0 below 4 GB is used directly", D.why == GFX_OK && D.mmio == 0xA0000000u,
       (long)D.mmio);
    ck("  ... with no window taken", map_calls == 0, map_calls);

    hp_state(); map_ok = 0; gfx_test_unmap(); gfx_open(&D);
    ck("a map that fails is EMAP", D.why == GFX_EMAP, D.why);
    hp_state(); dead = 1; gfx_test_unmap(); gfx_open(&D);
    ck("a function reading all ones is EDEAD", D.why == GFX_EDEAD, D.why);
    cap_reset(); gfx_report(&D, &Rg, &V, cap);
    ck("  ... and the screen says what to photograph", has("lspci -v"), nl);

    /* ── reading: every read inside a well that is on ─────────────── */
    head("reading the HP as the firmware leaves it (PW_1 + PW_A only)");
    hp_state(); run_all();
    ck("it opens", D.why == GFX_OK, D.why);
    ck("NO read reached a register in a powered-down well",
       unclaimed == 0, unclaimed);
    ck("  ... so the engine's own unclaimed flag is still clear",
       !(Rg.fpga_after & (1u << 31)), (long)Rg.fpga_after);
    ck("the display core is reported awake", Rg.core == 1, Rg.core);
    ck("pipe A's plane was read", Rg.pipe[0].powered == 1, Rg.pipe[0].powered);
    ck("pipe B's plane was NOT (PW_B is off)", Rg.pipe[1].powered == 0,
       Rg.pipe[1].powered);
    ck("  ... no read at all in pipe B's register page",
       reads_between(0x71000u, 0x72000u) == 0, reads_between(0x71000u, 0x72000u));
    ck("  ... nor in transcoder B's", reads_between(0x61000u, 0x62000u) == 0,
       reads_between(0x61000u, 0x62000u));
    ck("transcoder A was read (it is in PW_1, not PW_A)",
       Rg.trans[0].powered == 1, Rg.trans[0].powered);
    ck("the Type-C buffers were not read (PW_2 is off)",
       reads_between(0x64300u, 0x64700u) == 0, reads_between(0x64300u, 0x64700u));
    ck("  ... nor TCSS, nor the FIA",
       reads_between(0x161000u, 0x162000u) + reads_between(0x163000u, 0x164000u) == 0,
       reads_between(0x161000u, 0x162000u));
    ck("nothing was refused for being past 2 MB", Rg.out_of_span == 0,
       (long)Rg.out_of_span);
    ck("every read is in the log `gfx regs` prints",
       gfx_test_nlog() == (int)Rg.reads, gfx_test_nlog());
    ck("the flag was read FIRST", g_log[0].off == 0x42300u, (long)g_log[0].off);
    ck("  ... and LAST", g_log[gfx_test_nlog() - 1].off == 0x42300u,
       (long)g_log[gfx_test_nlog() - 1].off);

    head("every well on");
    hp_state();
    model_wells = 0x1E3u;              /* PW_1, PW_2, PW_A..PW_D      */
    run_all();
    ck("still no read in a dark well", unclaimed == 0, unclaimed);
    ck("pipes B, C, D were read", Rg.pipe[1].powered && Rg.pipe[2].powered &&
       Rg.pipe[3].powered, 0);
    ck("the Type-C buffers were read", Rg.port[GFX_PORT_TC1].powered == 1, 0);
    ck("  ... and TCSS", reads_between(0x161000u, 0x162000u) == 4,
       reads_between(0x161000u, 0x162000u));
    ck("a pipe that is off says so", has("  B  off  plane off") || has("  B  off"), nl);

    head("PW_2 on but PW_B off: TC registers yes, pipe B no");
    hp_state(); model_wells |= 1u << W_PW2; run_all();
    ck("no dark read", unclaimed == 0, unclaimed);
    ck("TC buffers read", reads_between(0x64300u, 0x64700u) == 4,
       reads_between(0x64300u, 0x64700u));
    ck("pipe B untouched", reads_between(0x71000u, 0x72000u) == 0,
       reads_between(0x71000u, 0x72000u));

    head("the display core asleep (PW_1 off, a DC state)");
    hp_state(); model_wells = 0; R(0x45504, 2); run_all();
    ck("no dark read even so", unclaimed == 0, unclaimed);
    ck("the core is reported asleep", Rg.core == 0, Rg.core);
    ck("only power-well-0 registers were touched",
       reads_between(0x46000u, 0x47000u) + reads_between(0x60000u, 0x74000u) == 0,
       reads_between(0x60000u, 0x74000u));
    ck("the screen says it is asleep and was not read",
       has("display core is asleep") && has("Not read."), nl);
    ck("  ... and names the DC state", has("up to DC6"), nl);

    head("PW_1 on, PW_A off: transcoder A yes, pipe A's registers no");
    /* Transcoder A lives in PW_1 but PIPESRC, at a transcoder
       address, is a PIPE register in PW_A. The one case that tells a
       transcoder gate from a pipe gate. */
    hp_state(); model_wells = 1u << W_PW1; run_all();
    ck("no dark read", unclaimed == 0, unclaimed);
    ck("transcoder A was read", Rg.trans[0].powered == 1, Rg.trans[0].powered);
    ck("  ... but not PIPESRC A", reads_between(0x6001Cu, 0x60020u) == 0,
       reads_between(0x6001Cu, 0x60020u));
    ck("  ... nor pipe A's plane", reads_between(0x70180u, 0x70200u) == 0,
       reads_between(0x70180u, 0x70200u));

    head("a read past the 2 MB register span is refused, not made");
    hp_state(); run_all();
    {
        uint32_t before = Rg.out_of_span, v;
        int n0 = gfx_test_nlog();
        v = rd(&D, &Rg, 0x200000u);
        ck("the read returns 0", v == 0, (long)v);
        ck("  ... and is counted as refused", Rg.out_of_span == before + 1u,
           (long)Rg.out_of_span);
        ck("  ... and never reached the engine (nothing logged)",
           gfx_test_nlog() == n0, gfx_test_nlog());
        v = rd(&D, &Rg, 0x45402u);
        ck("an unaligned offset is refused too", Rg.out_of_span == before + 2u,
           (long)Rg.out_of_span);
        cap_reset(); gfx_report(&D, &Rg, &V, cap);
        ck("  ... and the self-check says REFUSED, in red",
           has_tone("REFUSED past 2 MB", GFX_T_BAD), nl);
    }

    head("a well whose state bit lies is CAUGHT by the engine's own flag");
    /* The PW_2 state bit says on; the model treats it as off. A
       driver trusting the bit reads the TC registers in the dark,
       FPGA_DBG goes from clear to set, and the self-check must say so
       on screen -- this is the line that would tell us on the HP that
       a gate is wrong. */
    hp_state(); model_lie_wells = 1u << W_PW2; run_all();
    ck("the dark reads happened (the model saw them)", unclaimed > 0, unclaimed);
    ck("the flag was clear before", !(Rg.fpga_before & (1u << 31)), 0);
    ck("  ... and set after", (Rg.fpga_after & (1u << 31)) != 0, 0);
    ck("the report says UNCLAIMED READ, in red", has_tone("UNCLAIMED READ", GFX_T_BAD), nl);
    ck("  ... and says what to photograph", has("gfx regs"), nl);

    head("a flag the firmware already set is not blamed on us");
    hp_state(); R(0x42300, 1u << 31); run_all();
    ck("no dark read", unclaimed == 0, unclaimed);
    ck("the report says it was already set", has("already set"), nl);
    ck("  ... and does NOT claim an unclaimed read", !has("UNCLAIMED READ"), nl);

    /* ── the report, line by line ────────────────────────────────── */
    head("what the HP's photograph should say");
    hp_state(); run_all();
    ck("it says nothing is written", has("nothing is written"), nl);
    ck("it names the device", has("8086:46A8") && has("display version 13"), nl);
    ck("BAR0 printed with its halves apart", has("60_01000000"), nl);
    ck("  ... and where it is mapped", has("mapped at E3000000"), nl);
    ck("PW_1 and PW_A on, PW_B off",
       has("PW_1 on ") && has("PW_A on ") && has("PW_B off"), nl);
    ck("who asked for them: the firmware", has("firmware: PW_1 PW_A"), nl);
    ck("DC states off, DMC not loaded", has("DC states off") && has("not loaded"), nl);
    ck("CDCLK is 307.2 MHz from 38.4 x 16", has("CDCLK 307.2 MHz") &&
       has("ref 38.4 MHz, ratio 16, locked"), nl);
    ck("pipe A: 1920x1080 XRGB8888 linear, stride 7680",
       has("1920x1080 XRGB8888 linear, stride 7680"), nl);
    ck("  ... scanning out GTT 00400000", has("scans out GTT 00400000"), nl);
    ck("  ... which IS the framebuffer (BAR2 + surface == fb)",
       has_tone("= the framebuffer HavenDOS draws on", GFX_T_GOOD), nl);
    ck("pipe B is reported as not read, not as off-and-read",
       has("PW_B is off -- not read"), nl);
    ck("transcoder A -> DDI A DP SST, 8 bpc, x4",
       has("-> DDI A DP SST, 8 bpc, x4"), nl);
    ck("  ... 1920x1080 active, 2080x1111 total",
       has("1920x1080 active, 2080x1111 total"), nl);
    ck("DDI A buffer on, eDP panel per the VBT", has("eDP panel"), nl);
    ck("DDI B: HDMI, DDC 2, and nothing plugged in", has("HDMI, DDC 2") &&
       has("nothing"), nl);
    ck("TC1: DP with AUX TC1", has("DP, AUX TC1"), nl);
    ck("TC3 is not wired", has("not wired"), nl);
    ck("DPLL0 on and locked", has("DPLL0 ON, locked"), nl);
    ck("DPLL1 free", has("DPLL1 off, free"), nl);
    ck("DDI A clocked by DPLL0", has("DDI A clock: DPLL0"), nl);
    ck("DDI B clock gated", has("DDI B clock: gated off"), nl);
    ck("the self-check counts the reads and 0 written",
       has(" registers read, 0 written"), nl);
    ck("  ... and the flag was clear both times", has("clear before and after"), nl);
    ck("the verdict names the panel's path",
       has("The panel runs on pipe A, transcoder A, DDI A."), nl);
    ck("pipe B is free for the monitor", has("Pipe B and transcoder B are free"), nl);
    ck("HDMI is wired to DDI B", has_tone("HDMI is wired to DDI B.", GFX_T_GOOD), nl);
    ck("  ... so 0.9.1 reads EDID over GMBUS pin 2", has("GMBUS pin 2."), nl);
    ck("no line is wider than the screen", longest <= GFX_LINE_MAX, longest);
    ck("  ... and none was cut to fit", gfx_test_cut() == 0, (long)gfx_test_cut());
    if(fails) dump();

    head("the framebuffer cross-check is a real comparison");
    hp_state(); fb_lo = 0x00800000u; run_all();
    ck("a different surface is NOT called the framebuffer",
       !has("= the framebuffer HavenDOS draws on"), nl);
    ck("  ... the aperture address is printed instead", has("(aperture 40_00400000)"), nl);
    hp_state(); fb_hi = 0x41u; run_all();
    ck("a different high half is not the framebuffer either",
       !has("= the framebuffer HavenDOS draws on"), nl);

    head("hot-plug: three answers, kept apart");
    hp_state(); R(0xC4030, 0); run_all();
    ck("detector off on DDI B says 'detect off', not 'nothing'",
       strstr(line_of("  DDI B"), "detect off") != 0, nl);
    hp_state(); run_all();
    ck("detector on and nothing live on DDI B says 'nothing'",
       strstr(line_of("  DDI B"), "nothing") != 0, nl);
    hp_state(); R(0xC4000, 1u << 17); run_all();
    ck("enabled and live on B says CONNECTED",
       strstr(line_of("  DDI B"), "CONNECTED") != 0, nl);
    hp_state(); R(0xC4000, 1u << 16); run_all();
    ck("live on A's bit does not make B connected",
       strstr(line_of("  DDI B"), "CONNECTED") == 0, nl);
    hp_state(); model_wells |= 1u << W_PW2;
    R(0x44038, 8u << 0); R(0x44470, 1u << 16); run_all();
    ck("TC1 DP-alt live with detect on says DP-alt CONNECTED",
       has("DP-alt CONNECTED"), nl);
    hp_state(); model_wells |= 1u << W_PW2;
    R(0xC4034, 8u << 4); R(0xC4000, 1u << 25); run_all();
    ck("TC2 legacy live says legacy CONNECTED", has("legacy CONNECTED"), nl);
    hp_state(); model_wells |= 1u << W_PW2; tc_cold = 1; run_all();
    ck("a Type-C PHY in TC-cold is named", has("TC-cold"), nl);

    head("decoding variants");
    hp_state(); R(0x46000, 2u << 22); run_all();
    ck("CD2X /2 halves CDCLK: 153.6 MHz", has("CDCLK 153.6 MHz"), nl);
    hp_state(); R(0x46000, 1u << 22); run_all();
    ck("CD2X /1.5: 204.8 MHz", has("CDCLK 204.8 MHz"), nl);
    hp_state(); R(0x46000, 3u << 22); run_all();
    ck("CD2X /4: 76.8 MHz", has("CDCLK 76.8 MHz"), nl);
    hp_state(); R(0x51004, 1u << 29); run_all();
    ck("a 19.2 MHz reference x 16 is 153.6 MHz", has("CDCLK 153.6 MHz") &&
       has("ref 19.2 MHz"), nl);
    hp_state(); R(0x51004, 0); R(0x46070, (1u << 31) | (1u << 30) | 25u); run_all();
    ck("a 24 MHz reference x 25 is 300 MHz", has("CDCLK 300 MHz") &&
       has("ref 24 MHz"), nl);
    hp_state(); R(0x46070, 0); run_all();
    ck("a CDCLK PLL that is off says so", has("CDCLK PLL off"), nl);
    hp_state(); R(0x51000, 1u << 21); run_all();
    ck("a pipe fused off in DFSM says fused off", has("  B  fused off"), nl);
    hp_state(); R(0x60400, (1u << 31) | ((1u + 1u) << 27) | (0u << 24) | (1u << 20));
    run_all();
    ck("an HDMI transcoder on DDI B, 10 bpc", has("-> DDI B HDMI, 10 bpc"), nl);
    hp_state(); R(0x42000, 0); run_all();
    ck("fuse download incomplete is warned", has("Fuse download is NOT complete"), nl);
    hp_state(); R(0x46010, 1u << 31); run_all();
    ck("a PLL on but unlocked is NOT LOCKED", has("DPLL0 ON, NOT LOCKED"), nl);

    head("verdict variants");
    {
        kid_t k[2] = { HP_KIDS[0], { 0x60D2, 1, 2, 0x10, 0x04, 0x01 } };
        hp_state();
        vbt_len = build_vbt(vbtbuf, 250, 40, k, 2, 4, 0);
        put_opregion(2, 1, 0x7u, vbtbuf, vbt_len, 0, 0, 0);
        run_all();
        ck("an LSPCON HDMI port says 'DP inside'", has("through an LSPCON (DP inside)"), nl);
        ck("  ... and points 0.9.1 at AUX, not GMBUS", has("over AUX DDI B") &&
           !has("GMBUS pin"), nl);
        /* the same flag on an old BDB is a different field */
        hp_state();
        vbt_len = build_vbt(vbtbuf, 180, 40, k, 2, 4, 0);
        put_opregion(2, 1, 0x7u, vbtbuf, vbt_len, 0, 0, 0);
        run_all();
        ck("  ... but not on a BDB older than 192", !has("LSPCON"), nl);
    }
    hp_state(); memset(phys, 0, sizeof phys); run_all();
    ck("no VBT is said plainly", has("No VBT:"), nl);
    ck("  ... and the port column shows ?, not 'not wired'", !has("not wired"), nl);
    {
        kid_t k[1] = { HP_KIDS[0] };
        hp_state();
        vbt_len = build_vbt(vbtbuf, 250, 40, k, 1, 4, 0);
        put_opregion(2, 1, 0x7u, vbtbuf, vbt_len, 0, 0, 0);
        run_all();
        ck("a VBT with no HDMI says so", has("names no HDMI connector"), nl);
    }
    hp_state(); R(0x60400, 0); R(0x70008, 0); run_all();
    ck("no transcoder on is a warning, not a guess",
       has_tone("No transcoder is on", GFX_T_WARN), nl);

    /* ── the VBT parser ──────────────────────────────────────────── */
    head("the VBT parser");
    vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 8, 0);
    ck("the HP's table parses", gfx_vbt_parse(vbtbuf, vbt_len, &V) == GFX_VBT_OK, V.why);
    ck("  ... four children; the four empty slots skipped", V.nchild == 4, V.nchild);
    ck("  ... BDB version read", V.bdb_version == 250, V.bdb_version);
    ck("  ... child size read", V.child_size == 40, V.child_size);
    ck("eDP is eDP, not HDMI", V.child[0].is_edp && !V.child[0].is_hdmi, 0);
    ck("  ... on DDI A", V.child[0].port == GFX_PORT_A, V.child[0].port);
    ck("HDMI is HDMI, not DP", V.child[1].is_hdmi && !V.child[1].is_dp, 0);
    ck("  ... HDMIB is DDI B", V.child[1].port == GFX_PORT_B, V.child[1].port);
    ck("  ... DDC pin 2 is GMBUS pin 2", V.child[1].gmbus_pin == 2, V.child[1].gmbus_pin);
    /* display 13 renumbers the Type-C ports F..I in the VBT */
    ck("DPF is TC1 on display 13", V.child[2].port == GFX_PORT_TC1, V.child[2].port);
    ck("DPG is TC2", V.child[3].port == GFX_PORT_TC1 + 1, V.child[3].port);
    ck("AUX 0x60 is TC1's", V.child[2].aux_port == GFX_PORT_TC1, V.child[2].aux_port);
    ck("AUX 0x40 is DDI A's", V.child[0].aux_port == GFX_PORT_A, V.child[0].aux_port);
    {
        kid_t k[3] = { { 0x60D6, 14, 3, 0x60, 0, 3 },     /* dual mode, HDMIF */
                       { 0x60D2, 20, 6, 0, 0, 1 },        /* HDMII, ddc 6     */
                       { 0x1400, 21, 0, 0, 0, 0 } };      /* DSI on MIPIA     */
        vbt_len = build_vbt(vbtbuf, 250, 40, k, 3, 3, 0);
        gfx_vbt_parse(vbtbuf, vbt_len, &V);
        ck("DP dual mode is both DP and HDMI", V.child[0].is_dp && V.child[0].is_hdmi, 0);
        ck("HDMIF is TC1", V.child[0].port == GFX_PORT_TC1, V.child[0].port);
        ck("DDC 3 is GMBUS pin 9 (TC1)", V.child[0].gmbus_pin == 9, V.child[0].gmbus_pin);
        ck("HDMII is TC4, DDC 6 is GMBUS 12", V.child[1].port == GFX_PORT_TC1 + 3 &&
           V.child[1].gmbus_pin == 12, V.child[1].port);
        ck("a MIPI child is DSI, not counted as a port's connector",
           V.child[2].is_dsi && child_on(&V, GFX_PORT_A) == 0, V.child[2].is_dsi);
    }
    {
        /* DEVICE_TYPE_DVI, 0x68D2: TMDS signalling with NOT_HDMI set.
           A DVI socket cannot carry HDMI infoframes or audio. */
        kid_t k[1] = { { 0x68D2, 1, 2, 0, 0, 0 } };
        vbt_len = build_vbt(vbtbuf, 250, 40, k, 1, 1, 0);
        gfx_vbt_parse(vbtbuf, vbt_len, &V);
        ck("a DVI-only connector is not called HDMI", V.nchild == 1 &&
           !V.child[0].is_hdmi, V.child[0].is_hdmi);
    }
    vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 4, 1);
    ck("a MIPI sequence v3 block is walked by its 32-bit size",
       gfx_vbt_parse(vbtbuf, vbt_len, &V) == GFX_VBT_OK && V.nchild == 4, V.why);
    {
        kid_t many[12];
        for(i = 0; i < 12; i++) many[i] = HP_KIDS[i % 4];
        vbt_len = build_vbt(vbtbuf, 250, 40, many, 12, 12, 0);
        ck("twelve children are capped at eight, not overrun",
           gfx_vbt_parse(vbtbuf, vbt_len, &V) == GFX_VBT_OK &&
           V.nchild == GFX_VBT_MAX_CHILD, V.nchild);
    }
    vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 4, 0);
    vbtbuf[0] = 'X';
    ck("no $VBT is NOVBT", gfx_vbt_parse(vbtbuf, vbt_len, &V) == GFX_VBT_NOVBT, V.why);
    vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 4, 0);
    vbtbuf[0x30] = 'X';
    ck("no BIOS_DATA_BLOCK is NOBDB", gfx_vbt_parse(vbtbuf, vbt_len, &V) == GFX_VBT_NOBDB,
       V.why);
    vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 4, 0);
    ck("a vbt_size bigger than the buffer is BADSIZE",
       gfx_vbt_parse(vbtbuf, vbt_len - 1u, &V) == GFX_VBT_BADSIZE, V.why);
    vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 4, 0);
    p32(vbtbuf + 28, 0x1FFF);
    ck("a bdb_offset past the end is BADSIZE",
       gfx_vbt_parse(vbtbuf, vbt_len, &V) == GFX_VBT_BADSIZE, V.why);
    vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 4, 0);
    p16(vbtbuf + 0x30 + 22 + 1, 0xF000);           /* block 1 too long */
    ck("a block running past the BDB is BADSIZE",
       gfx_vbt_parse(vbtbuf, vbt_len, &V) == GFX_VBT_BADSIZE, V.why);
    vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 4, 0);
    vbtbuf[0x30 + 22 + 11] = 9;                     /* block 2 renamed */
    ck("no block 2 is NODEFS", gfx_vbt_parse(vbtbuf, vbt_len, &V) == GFX_VBT_NODEFS, V.why);
    vbt_len = build_vbt(vbtbuf, 250, 20, HP_KIDS, 4, 4, 0);
    ck("a child size too small for the fields is BADSIZE",
       gfx_vbt_parse(vbtbuf, vbt_len, &V) == GFX_VBT_BADSIZE, V.why);
    ck("a NULL table is NOVBT", gfx_vbt_parse(0, 100, &V) == GFX_VBT_NOVBT, V.why);

    head("finding the VBT through the OpRegion");
    hp_state(); gfx_test_unmap(); gfx_open(&D);
    ck("mailbox 4 is found", gfx_vbt_find(&D, &V) == GFX_VBT_OK && !V.from_rvda, V.why);
    ck("  ... at ASLS + 0x400", V.where == PHYS_BASE + 0x400u, (long)V.where);
    ck("  ... with 0x1C00 bytes when there is no ASLE-ext mailbox",
       V.len == 0x1C00u, (long)V.len);
    put_opregion(2, 1, 0x7u | 0x10u, vbtbuf, vbt_len, 0, 0, 0);
    gfx_vbt_find(&D, &V);
    ck("  ... and 0x1800 when there is", V.len == 0x1800u, (long)V.len);

    /* RVDA, relative (2.1+): the VBT outside the OpRegion. */
    vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 4, 0);
    put_opregion(2, 1, 0x7u, 0, 0, 0x4000u, 0, vbt_len);
    memcpy(phys + 0x4000, vbtbuf, vbt_len);
    ck("a 2.1 RVDA is relative to the OpRegion",
       gfx_vbt_find(&D, &V) == GFX_VBT_OK && V.from_rvda &&
       V.where == PHYS_BASE + 0x4000u, (long)V.where);
    put_opregion(2, 0, 0x7u, 0, 0, PHYS_BASE + 0x6000u, 0, vbt_len);
    memcpy(phys + 0x6000, vbtbuf, vbt_len);
    ck("a 2.0 RVDA is a physical address",
       gfx_vbt_find(&D, &V) == GFX_VBT_OK && V.from_rvda &&
       V.where == PHYS_BASE + 0x6000u, (long)V.where);
    {
        /* A small but VALID table at ASLS + 0x100, so a driver that
           wrongly accepted the pointer would parse it and say RVDA --
           with nothing there it would fall back and look right. */
        kid_t k[1] = { HP_KIDS[1] };
        uint8_t small[0x200];
        uint32_t sl = build_vbt(vbtbuf, 250, 40, k, 1, 1, 0);
        memcpy(small, vbtbuf, sl);
        vbt_len = build_vbt(vbtbuf, 250, 40, HP_KIDS, 4, 4, 0);
        put_opregion(3, 0, 0x7u, vbtbuf, vbt_len, 0x100u, 0, sl);
        memcpy(phys + 0x100, small, sl);
        ck("a relative RVDA pointing INSIDE the OpRegion is ignored",
           gfx_vbt_find(&D, &V) == GFX_VBT_OK && !V.from_rvda, V.from_rvda);
    }
    put_opregion(2, 1, 0x7u, vbtbuf, vbt_len, 0x4000u, 1, vbt_len);
    memcpy(phys + 0x4000, vbtbuf, vbt_len);       /* valid if truncated */
    ck("an RVDA above 4 GB is ignored, not truncated",
       gfx_vbt_find(&D, &V) == GFX_VBT_OK && !V.from_rvda, V.from_rvda);
    put_opregion(2, 1, 0x3u, vbtbuf, vbt_len, 0x4000u, 0, vbt_len);
    memcpy(phys + 0x4000, vbtbuf, vbt_len);
    ck("RVDA is ignored when there is no ASLE mailbox",
       gfx_vbt_find(&D, &V) == GFX_VBT_OK && !V.from_rvda, V.from_rvda);
    put_opregion(2, 1, 0x7u, vbtbuf, vbt_len, 0x4000u, 0, vbt_len);
    memset(phys + 0x4000, 0, vbt_len);
    ck("a broken RVDA table falls back to mailbox 4",
       gfx_vbt_find(&D, &V) == GFX_VBT_OK && !V.from_rvda, V.from_rvda);
    memset(phys, 0, 16);
    ck("no IntelGraphicsMem is NOSIG", gfx_vbt_find(&D, &V) == GFX_VBT_NOSIG, V.why);
    D.asls = 0; phys_calls = 0;
    i = gfx_vbt_find(&D, &V);
    ck("ASLS 0 is NOASLS", i == GFX_VBT_NOASLS, i);
    ck("  ... and no physical memory was touched", phys_calls == 0, phys_calls);

    /* ── the other two screens ───────────────────────────────────── */
    head("`gfx regs` and `gfx vbt`");
    hp_state(); run_all();
    cap_reset(); gfx_report_regs(&D, &Rg, cap);
    {
        int entries = 0, j;
        for(j = 1; j < nl; j++){
            const char *s = lines[j];
            while((s = strchr(s, ' '))){
                while(*s == ' ') s++;
                if(strlen(s) >= 15 && s[6] == ' ' && s[15] != 0){ entries++; s += 15; }
                else if(*s) s++;
            }
        }
        ck("`gfx regs` prints one entry per read", entries == (int)Rg.reads, entries);
    }
    ck("  ... starting with the flag", strstr(lines[1], "042300") != 0, 0);
    ck("  ... in lines that fit", longest <= GFX_LINE_MAX, longest);
    cap_reset(); gfx_report_vbt(&V, cap);
    ck("`gfx vbt` lists the four children", has("HDMI") && has("eDP panel") &&
       has("RVDA") == 0, nl);
    ck("  ... with the OpRegion version", has("OpRegion 2.1"), nl);
    ck("  ... in lines that fit", longest <= GFX_LINE_MAX, longest);

    /* The cut counter has been watching every report above. */
    cut0 = gfx_test_cut();
    ck("not one line in this whole run was cut at the screen edge", cut0 == 0,
       (long)cut0);

    printf("\n%d/%d passed\n", total - fails, total);
    free(regs);
    return fails ? 1 : 0;
}
