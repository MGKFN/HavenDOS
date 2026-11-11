/* ════════════════════════════════════════════════════════════════════
   test_xemode.c -- lighting pipe B (drivers/xemode.c), against a model
   display engine that behaves like one.

   THIS IS THE FIRST HAVENDOS CODE THAT CHANGES WHAT A SCREEN SHOWS,
   and the only engine it will ever really run on is the HP's. QEMU has
   no Iris Xe. So this model is built to be hard to please:

     - It starts from the HP's OWN registers: the 96 that 0.9.0's
       `gfx regs` photographed on 11 November, the same table
       tests/test_gfx.c uses. Registers 0.9.0 never read (DBUF, MBUS,
       the PHY lanes, pipe A's watermarks) are filled with values in
       the shape i915 programs, and are labelled SYNTHETIC where set.
     - STATUS BITS MOVE. A PLL locks only after it is powered and
       enabled; a power well's state follows its request; DDI_BUF_CTL's
       idle bit clears only when the buffer is enabled; the transcoder
       reports running only once enabled; PIPE_FRMCOUNT advances only
       while it runs. A driver that waits on the wrong bit HANGS here
       (bounded by the poll count) rather than passing.
     - The PHY's GROUP registers fan a write out to all four lanes, as
       the hardware does, and READ BACK GARBAGE: i915 never reads a
       group register, and a driver that did would act on noise.
     - EVERY WRITE IS SEEN. One that lands in pipe A's half of the
       machine -- transcoder A, pipe A, DDI A, PHY A, the PLL port A is
       on -- is counted as a failure even if the driver "meant" it, and
       pipe A's registers are compared word for word before and after.

   ── WHAT THIS CANNOT PROVE ──────────────────────────────────────────

   That the monitor lights. The sequence and every value are i915's,
   and this suite proves the driver does what it says, in order, with
   nothing outside B touched. Whether that is what the silicon needs is
   the HP's answer, and why `gfx on` prints every write it made.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ── the model engine ─────────────────────────────────────────────── */
#define SPAN 0x200000u
static uint32_t *regs;
static uint32_t *before;                  /* snapshot for pipe A checks */
static int  w_total, w_forbidden, grp_reads, unclaimed;
static uint32_t w_off[512], w_val[512];
static int  knob_nolock, knob_nopower, knob_stuck_off, knob_buf_stays_idle;
static int  knob_frm_stuck, knob_ddi_io_stuck;
static uint32_t knob_stuck_reg;           /* ignores writes             */
static uint32_t frames;

#define W_PW1 0
#define W_PW2 1
#define W_PWA 5
#define W_PWB 6

static int pw_on(int idx){ return (regs[0x45404u / 4] >> (idx * 2)) & 1u; }

/* Which well a register lives in -- the model's statement, from i915's
   xelpd power map, written independently of the driver's gates. */
static int owner(uint32_t off)
{
    if(off >= 0x45000u && off < 0x45600u) return -1;          /* PW0  */
    if(off >= 0x44000u && off < 0x44500u) return -1;          /* MBUS, DBUF, ISR */
    if(off >= 0xC4000u && off < 0xC6000u) return -1;          /* south */
    if(off >= 0x61000u && off < 0x62000u) return W_PWB;
    if(off >= 0x71000u && off < 0x72000u) return W_PWB;
    if(off >= 0x4AC00u && off < 0x4B000u) return W_PWB;       /* gamma B */
    if(off >= 0x49100u && off < 0x49200u) return W_PWB;       /* CSC B   */
    if(off >= 0x70000u && off < 0x71000u)
        return (off & 0xFFFu) == 0x008u ? W_PW1 : W_PWA;
    return W_PW1;
}

static int pipe_a_side(uint32_t off)
{
    return (off >= 0x60000u && off < 0x61000u) ||   /* transcoder A */
           (off >= 0x70000u && off < 0x71000u) ||   /* pipe A       */
           off == 0x64000u ||                       /* DDI A buffer */
           (off >= 0x162000u && off < 0x163000u) || /* combo PHY A  */
           off == 0x46014u || off == 0x16428Cu || off == 0x164290u || /* DPLL1 */
           off == 0x45404u || off == 0x45400u ||    /* the main wells */
           off == 0x45008u || off == 0x44FE8u ||    /* DBUF S1, S2  */
           off == 0x420C0u || off == 0x46140u ||    /* A's chicken, clock */
           off == 0x45270u;                         /* A's line time */
}

static int is_grp(uint32_t off)
{
    return off >= 0x6C600u && off < 0x6C700u;
}

static uint32_t model_rd(uint32_t base, uint32_t off)
{
    int w;
    (void)base;
    if(off >= SPAN || (off & 3u)){ fprintf(stderr, "bad read %x\n", off); abort(); }
    w = owner(off);
    if(w >= 0 && !pw_on(w)){ unclaimed++; return 0; }
    if(is_grp(off)){ grp_reads++; return 0xDEADBEEFu; }
    if(off == 0x71040u) return frames;                        /* PIPE_FRMCOUNT B */
    return regs[off / 4u];
}

/* Status bits, after a write, the way the hardware moves them. */
static void settle(void)
{
    uint32_t *r = regs, n;
    /* DDI IO B: state follows request */
    if(!knob_ddi_io_stuck){
        if(r[0x45454u / 4] & (1u << 3)) r[0x45454u / 4] |= 1u << 2;
        else r[0x45454u / 4] &= ~(1u << 2);
    }
    for(n = 2; n < 4; n++){
        uint32_t o = n == 2 ? 0x44300u : 0x44304u;
        if(r[o / 4] & (1u << 31)) r[o / 4] |= 1u << 30; else r[o / 4] &= ~(1u << 30);
    }
    for(n = 0; n < 2; n++){
        uint32_t *e = &r[(0x46010u + 4u * n) / 4];
        if((*e & (1u << 27)) && !knob_nopower) *e |= 1u << 26;
        if(!(*e & (1u << 27))) *e &= ~(1u << 26);
        if((*e & (1u << 31)) && (*e & (1u << 26)) && !knob_nolock) *e |= 1u << 30;
        if(!(*e & (1u << 31))) *e &= ~(1u << 30);
    }
    /* TRANSCONF B: running once enabled -- if its clock is there */
    if((r[0x71008u / 4] & (1u << 31)) && !(r[0x164280u / 4] & (1u << 11)) &&
       (r[0x46144u / 4] >> 28) == 2u && (r[0x46010u / 4] & (1u << 30)))
        r[0x71008u / 4] |= 1u << 30;
    if(!(r[0x71008u / 4] & (1u << 31))) r[0x71008u / 4] &= ~(1u << 30);
    /* DDI_BUF_CTL B: idle unless enabled */
    if((r[0x64100u / 4] & (1u << 31)) && !knob_buf_stays_idle)
        r[0x64100u / 4] &= ~(1u << 7);
    else r[0x64100u / 4] |= 1u << 7;
}

static void model_wr(uint32_t base, uint32_t off, uint32_t v)
{
    int w;
    (void)base;
    if(off >= SPAN || (off & 3u)){ fprintf(stderr, "bad write %x\n", off); abort(); }
    if(w_total < 512){ w_off[w_total] = off; w_val[w_total] = v; }
    w_total++;
    if(pipe_a_side(off)) w_forbidden++;
    w = owner(off);
    if(w >= 0 && !pw_on(w)){ unclaimed++; return; }
    if(knob_stuck_reg && off == knob_stuck_reg) return;
    /* PIPE_MBUS_DBOX_CTL B: bits 31:25 are read-only and pipe B's own.
       The HP, 0.9.2: written A9038C04, read back B1038C04. */
    if(off == 0x7103Cu) v = (v & 0x01FFFFFFu) | 0xB0000000u;
    if(is_grp(off)){
        uint32_t ln, d = off - 0x6C600u;
        for(ln = 0; ln < 4; ln++) regs[(0x6C800u + 0x100u * ln + d) / 4] = v;
    } else {
        regs[off / 4u] = v;
    }
    settle();
}

static void sleep_model(uint32_t ms)
{
    if((regs[0x71008u / 4] & (1u << 30)) && !knob_frm_stuck) frames += ms * 60u / 1000u;
}

#define GFX_RD(base, off)    model_rd((base), (off))
#define GFX_WR(base, off, v) model_wr((base), (off), (v))
#define XM_SLEEP_MS(ms)      sleep_model(ms)

#include "../include/xedisp.h"
#include "../include/xemode.h"
#include "../include/edid.h"
#include "../drivers/edid.c"
#include "../drivers/xemode.c"

/* ── the suite ────────────────────────────────────────────────────── */
static int total = 0, fails = 0;
static void ck(const char *what, int ok, long got)
{
    total++;
    if(!ok){ fails++; printf("  FAIL  %s   (got %ld)\n", what, got); }
}
static void head(const char *s){ printf("\n%s\n", s); }

#define MAXL 400
static char lines[MAXL][GFX_LINE_MAX + 8];
static int  nl, longest;
static void cap(int tone, const char *s)
{
    int n = (int)strlen(s);
    (void)tone;
    if(n > longest) longest = n;
    if(nl < MAXL){ strncpy(lines[nl], s, GFX_LINE_MAX + 7); nl++; }
}
static int has(const char *needle)
{
    int i;
    for(i = 0; i < nl; i++) if(strstr(lines[i], needle)) return 1;
    return 0;
}
static void dump(void){ int i; for(i = 0; i < nl; i++) printf("    | %s\n", lines[i]); }

static void R(uint32_t off, uint32_t v){ regs[off / 4u] = v; }

/* The HP as 0.9.0 read it (tests/test_gfx.c has the same table and the
   provenance). */
static const uint32_t HP[][2] = {
    {0x042300,0x80000001},{0x042000,0x8E380000},{0x045400,0x00005405},
    {0x045404,0x0000FC0F},{0x045408,0x40000000},{0x04540C,0x00005405},
    {0x045440,0x00000001},{0x045444,0x00000003},{0x045450,0x00000001},
    {0x045454,0x00000003},{0x045504,0x00000000},{0x08F004,0x00000000},
    {0x08F074,0x00000000},{0x0C4000,0x00030000},{0x0C4030,0x000000A8},
    {0x0C4034,0x00000000},{0x046000,0x00780164},{0x046070,0xC000000E},
    {0x051004,0x40000000},{0x051000,0x800C1C00},{0x046010,0x00000000},
    {0x164284,0x018001D4},{0x164288,0x00002644},{0x046014,0xCC000000},
    {0x16428C,0x00E001A5},{0x164290,0x00000088},{0x046020,0x00000000},
    {0x046038,0x00000000},{0x046040,0x00000000},{0x046048,0x00000000},
    {0x046050,0x00000000},{0x164280,0x01E07801},{0x044470,0x00000000},
    {0x044030,0x00000000},{0x044038,0x00000000},{0x070008,0xC0000000},
    {0x070180,0x94000008},{0x070188,0x00000040},{0x07018C,0x00000000},
    {0x070190,0x02FF03FF},{0x07019C,0x00000000},{0x0701A4,0x00000000},
    {0x071008,0x00000000},{0x071180,0x00000008},{0x071188,0x00000000},
    {0x07118C,0x00000000},{0x071190,0x00000000},{0x07119C,0x00000000},
    {0x0711A4,0x00000000},{0x072008,0x00000000},{0x072180,0x00000008},
    {0x072188,0x00000000},{0x07218C,0x00000000},{0x072190,0x00000000},
    {0x07219C,0x00000000},{0x0721A4,0x00000000},{0x060400,0x8A010102},
    {0x060000,0x081F077F},{0x060004,0x081F077F},{0x060008,0x07CF07AF},
    {0x06000C,0x04570437},{0x060010,0x04570437},{0x060014,0x04470441},
    {0x06001C,0x03FF02FF},{0x061400,0x00030000},{0x061000,0x00000000},
    {0x061004,0x00000000},{0x061008,0x00000000},{0x06100C,0x00000000},
    {0x061010,0x00000000},{0x061014,0x00000000},{0x06101C,0x00000000},
    {0x062400,0x00030000},{0x062000,0x00000000},{0x062004,0x00000000},
    {0x062008,0x00000000},{0x06200C,0x00000000},{0x062010,0x00000000},
    {0x062014,0x00000000},{0x06201C,0x00000000},{0x064000,0x80000002},
    {0x064100,0x00000080},{0x04610C,0x00000000},{0x064300,0x00000080},
    {0x161500,0x00000000},{0x046110,0x00000000},{0x064400,0x00000080},
    {0x161504,0x00000000},{0x046114,0x00000000},{0x064500,0x00000080},
    {0x161508,0x00000000},{0x046118,0x00000000},{0x064600,0x00000080},
    {0x16150C,0x00000000},{0x1638A0,0xFFFFFFFF},
};

#define WM(lines_, blocks_) ((1u << 31) | ((uint32_t)(lines_) << 14) | (uint32_t)(blocks_))

static void hp(void)
{
    unsigned k, ln;
    memset(regs, 0, SPAN);
    for(k = 0; k < sizeof HP / sizeof HP[0]; k++) R(HP[k][0], HP[k][1]);
    /* ── SYNTHETIC: registers 0.9.0 never read, in i915's shapes ── */
    R(0x45008, 0xC0000000u | (8u << 19)); R(0x44FE8, 0xC0000000u | (8u << 19)); /* S1,S2 on */
    R(0x44300, 8u << 19); R(0x44304, 8u << 19);       /* S3, S4 off: the plan wakes them */
    R(0x4438C, 7u << 26);                             /* MBUS not joined, no pipe */
    R(0x7027C, 0x07BF0000u);                          /* A plane 1: blocks 0-1983 */
    R(0x7017C, 0x07FF07C0u);                          /* A cursor: 1984-2047     */
    /* REAL (0.9.2's `gfx plan` on the HP, which printed them as copied
       from pipe A): level 0 and the SAGV level enabled, the rest not. */
    R(0x70240, 0x80004011u); R(0x70244, 0x00004011u); R(0x70248, 0x00004011u);
    R(0x7024C, 0x00004011u); R(0x70250, 0x00004011u); R(0x70254, 0x00004011u);
    R(0x70258, 0x8000C031u); R(0x7025C, 0x00004007u); R(0x70268, 0x00004007u);
    R(0x7003C, 0xA9038C04u);                          /* REAL: pipe A's DBOX     */
    R(0x7103C, 0xB0000000u);                          /* REAL: B's read-only top */
    R(0x701CC, 0x00002000u);                          /* PLANE_COLOR_CTL A       */
    R(0x6007C, 0);                                    /* context latency A       */
    R(0x64C04, 0);                                    /* PHY B powered           */
    R(0x6C100, 1u << 31);                             /* COMP_INIT               */
    R(0x6C028, 0x000000F0u);                          /* lanes powered down      */
    R(0x6C014, 0);
    for(ln = 0; ln < 4; ln++){
        R(0x6C804 + 0x100 * ln, 1u << 26);            /* keeper on (DP default)  */
        R(0x6C890 + 0x100 * ln, 0x0000A035u);
        R(0x6C894 + 0x100 * ln, 0x80000000u | (5u << 18) | (2u << 3) | (1u << 26));
        R(0x6C888 + 0x100 * ln, 0x00000000u);
        R(0x6C89C + 0x100 * ln, 0x60000000u);
    }
    frames = 1000;
    knob_nolock = knob_nopower = knob_stuck_off = knob_buf_stays_idle = 0;
    knob_frm_stuck = knob_ddi_io_stuck = 0; knob_stuck_reg = 0;
    w_total = w_forbidden = grp_reads = unclaimed = 0;
}

static gfx_dev_t D;
static void dev_ok(void){ memset(&D, 0, sizeof D); D.why = GFX_OK; D.mmio = 0x1000u; }

/* ── a monitor: the HP's LG, as `gfx edid` decoded it on 11 Nov ───── */
typedef struct { uint32_t pclk; uint16_t ha, hb, hso, hsw, va, vb, vso, vsw; int hp, vp, il; } tm_t;
static const tm_t LG144 = { 346500, 1920, 160, 48, 32, 1080, 77, 3, 5, 1, 0, 0 };
static const tm_t CEA60 = { 148500, 1920, 280, 88, 44, 1080, 45, 4, 5, 1, 1, 0 };
static const tm_t LG120 = { 285500, 1920, 160, 48, 32, 1080, 64, 3, 5, 1, 0, 0 };
static const tm_t QHD75 = { 304250, 2560, 160, 48, 32, 1440, 62, 3, 5, 1, 0, 0 };
static const tm_t SVGA  = {  40000,  800, 256, 40,128,  600, 28, 1, 4, 1, 1, 0 };
static const tm_t RB60  = { 138500, 1920, 160, 48, 32, 1080, 31, 3, 5, 1, 0, 0 }; /* CVT-RB, +h -v */
static const tm_t I1080 = {  74250, 1920, 280, 88, 44,  540, 22, 2, 5, 1, 1, 1 };

static void enc(uint8_t *d, const tm_t *t)
{
    uint32_t p = t->pclk / 10u;
    memset(d, 0, 18);
    d[0] = (uint8_t)p; d[1] = (uint8_t)(p >> 8);
    d[2] = (uint8_t)t->ha; d[3] = (uint8_t)t->hb;
    d[4] = (uint8_t)(((t->ha >> 8) << 4) | (t->hb >> 8));
    d[5] = (uint8_t)t->va; d[6] = (uint8_t)t->vb;
    d[7] = (uint8_t)(((t->va >> 8) << 4) | (t->vb >> 8));
    d[8] = (uint8_t)t->hso; d[9] = (uint8_t)t->hsw;
    d[10] = (uint8_t)(((t->vso & 15) << 4) | (t->vsw & 15));
    d[11] = (uint8_t)(((t->hso >> 8) << 6) | ((t->hsw >> 8) << 4) |
                      ((t->vso >> 4) << 2) | (t->vsw >> 4));
    d[17] = (uint8_t)(0x18 | (t->vp ? 4 : 0) | (t->hp ? 2 : 0) | (t->il ? 0x80 : 0));
}
static void fix(uint8_t *b){ uint8_t s = 0; int i; for(i = 0; i < 127; i++) s = (uint8_t)(s + b[i]); b[127] = (uint8_t)(0u - s); }

static uint8_t eb[256];
static edid_t  E;
/* modes: up to 4 in the CTA block after the preferred one in block 0 */
static void monitor(const tm_t *pref, const tm_t *const *more, int nmore)
{
    int i;
    memset(eb, 0, sizeof eb);
    memcpy(eb, "\x00\xFF\xFF\xFF\xFF\xFF\xFF\x00", 8);
    eb[8] = 0x1E; eb[9] = 0x6D; eb[10] = 0x19; eb[11] = 0x5C;   /* GSM 5C19 */
    eb[16] = 7; eb[17] = 2024 - 1990; eb[18] = 1; eb[19] = 3;
    enc(eb + 54, pref);
    eb[126] = 1; fix(eb);
    eb[128] = 0x02; eb[129] = 0x03; eb[130] = 4;
    for(i = 0; i < nmore && i < 6; i++) enc(eb + 128 + 4 + 18 * i, more[i]);
    fix(eb + 128);
    memset(&E, 0, sizeof E);
    edid_parse(eb, 256, &E);
}
static void lg(void)
{
    const tm_t *m[3] = { &CEA60, &LG120, &QHD75 };
    monitor(&LG144, m, 3);
}

static gfx_child_t KID;   /* the HP's HDMI child: level 0 = "none" below */

/* An independent decoder (i915 icl_ddi_combo_pll_get_freq), written
   here rather than borrowed, so the PLL arithmetic is checked against
   a second statement of it. */
static uint32_t decode_ref(uint32_t c0, uint32_t c1, int x2, uint32_t ref)
{
    static const uint32_t P[16] = { 0, 2, 3, 0, 5, 0, 0, 0, 7 };
    static const uint32_t K[8]  = { 0, 1, 2, 0, 3 };
    uint64_t f = (uint64_t)((c0 >> 10) & 0x7FFFu) * (x2 ? 2u : 1u);
    uint64_t dco = (uint64_t)(c0 & 0x3FFu) * ref + f * ref / 32768u;
    uint32_t p0 = P[(c1 >> 2) & 0xFu], p2 = K[(c1 >> 6) & 7u];
    uint32_t p1 = (c1 & (1u << 9)) ? ((c1 >> 10) & 0xFFu) : 1u;
    if(!p0 || !p1 || !p2) return 0;
    return (uint32_t)(dco / (p0 * p1 * p2 * 5u));
}
static uint32_t decode(uint32_t c0, uint32_t c1, int x2){ return decode_ref(c0, c1, x2, 19200u); }

static int find_write(uint32_t off, int from)
{
    int i;
    for(i = from; i < w_total && i < 512; i++) if(w_off[i] == off) return i;
    return -1;
}
static int last_write(uint32_t off)
{
    int i, k = -1;
    for(i = 0; i < w_total && i < 512; i++) if(w_off[i] == off) k = i;
    return k;
}
static int op_index(const xm_plan_t *p, uint32_t off, uint32_t val)
{
    int i;
    for(i = 0; i < p->nops; i++)
        if(p->ops[i].off == off && p->ops[i].op == XM_OP_WR && p->ops[i].val == val) return i;
    return -1;
}
static int op_rmw(const xm_plan_t *p, uint32_t off, uint32_t set)
{
    int i;
    for(i = 0; i < p->nops; i++)
        if(p->ops[i].off == off && p->ops[i].op == XM_OP_RMW && p->ops[i].set == set) return i;
    return -1;
}
static int pipe_a_unchanged(void)
{
    uint32_t o;
    for(o = 0; o < SPAN; o += 4)
        if(pipe_a_side(o) && regs[o / 4] != before[o / 4]){
            if(o == 0x45404u) continue;   /* status bits are not ours to move */
            printf("    pipe A side changed: %06X %08X -> %08X\n", o, before[o / 4], regs[o / 4]);
            return 0;
        }
    return 1;
}

static xm_state_t  S;
static xm_plan_t   P;
static xm_result_t RES;

static int plan_hp(void)
{
    dev_ok(); xm_read(&D, &S);
    return xm_plan_on(&S, &E, &KID, &P);
}

int main(void)
{
    uint32_t c0, c1, k;
    int i, r;

    regs   = calloc(SPAN / 4, sizeof *regs);
    before = calloc(SPAN / 4, sizeof *before);
    memset(&KID, 0, sizeof KID);
    KID.is_hdmi = 1; KID.port = GFX_PORT_B; KID.gmbus_pin = 2; KID.hdmi_level = -1;

    /* ── the PLL ─────────────────────────────────────────────────── */
    head("the WRPLL: i915's search, and its 38.4 MHz workaround");
    ck("148.5 MHz: dividers found", xm_wrpll(148500, 0x40000000u, &c0, &c1) == 1, 0);
    ck("  ... DCO 8910 MHz = 464 + 2048/32768 of 19.2, fraction HALVED: 001001D0",
       c0 == 0x001001D0u, (long)c0);
    ck("  ... /60 as p2 q3 k2: 00000E84", c1 == 0x00000E84u, (long)c1);
    ck("  ... and decodes back to 148500 kHz exactly", decode(c0, c1, 1) == 148500u,
       (long)decode(c0, c1, 1));
    ck("285.5 MHz (the LG's 120 Hz): 001801BE / 00000488",
       xm_wrpll(285500, 0x40000000u, &c0, &c1) && c0 == 0x001801BEu && c1 == 0x488u,
       (long)c0);
    ck("a 24 MHz reference is not halved",
       xm_wrpll(148500, 0, &c0, &c1) && decode_ref(c0, c1, 0, 24000u) == 148500u &&
       ((c0 >> 10) & 0x7FFFu) == 8192u, (long)c0);
    {
        uint32_t f, worst = 0, bad = 0;
        for(f = 25000; f <= 340000; f += 250){
            uint32_t d;
            if(!xm_wrpll(f, 0x40000000u, &c0, &c1)){ bad++; continue; }
            d = decode(c0, c1, 1);
            d = d > f ? d - f : f - d;
            if(d > worst) worst = d;
        }
        ck("every clock 25-340 MHz in 250 kHz steps has dividers", bad == 0, (long)bad);
        ck("  ... and decodes back within 2 kHz", worst <= 2, (long)worst);
    }
    /* 25.175 MHz fits five dividers (64 .. 78); the FIRST in i915's
       list is 64, at the bottom edge of the DCO range, and the most
       central is 72. The harness found every other clock here had a
       single candidate, or a first one that was also the best. */
    ck("25.175 MHz: the most central divider (72, DCO 9063 MHz), not the first",
       xm_wrpll(25175, 0x40000000u, &c0, &c1) && c1 == 0x00004A84u &&
       decode(c0, c1, 1) == 25175u, (long)c1);
    ck("an undefined reference field is refused", xm_wrpll(148500, 7u << 29, &c0, &c1) == 0, 0);
    ck("no clock is refused", xm_wrpll(0, 0x40000000u, &c0, &c1) == 0, 0);
    ck("a clock no divider reaches is refused", xm_wrpll(1900000, 0x40000000u, &c0, &c1) == 0, 0);

    /* ── which mode ──────────────────────────────────────────────── */
    head("which mode `gfx on` picks");
    lg();
    ck("the LG parses: 4 modes, 144 Hz first", E.why == EDID_OK && E.nmodes == 4 &&
       E.modes[0].pclk_khz == 346500u, E.nmodes);
    ck("1080p60 is picked, not the preferred 346.5 MHz one",
       edid_pick(&E, EDID_TMDS_NOSCRAMBLE_KHZ, 1024, 768) == 1,
       edid_pick(&E, EDID_TMDS_NOSCRAMBLE_KHZ, 1024, 768));
    ck("  ... and not the bigger 2560x1440, which is 75 Hz",
       E.modes[3].hactive == 2560, E.modes[3].hactive);
    {
        const tm_t *m[1] = { &LG120 };
        monitor(&LG144, m, 1);
        ck("no 60 Hz mode: the largest under 340 MHz (1080p120)",
           edid_pick(&E, EDID_TMDS_NOSCRAMBLE_KHZ, 1024, 768) == 1, 0);
        monitor(&LG144, 0, 0);
        ck("only a 346.5 MHz mode: none", edid_pick(&E, EDID_TMDS_NOSCRAMBLE_KHZ, 1024, 768) == -1, 0);
        m[0] = &I1080;
        monitor(&I1080, 0, 0);
        ck("interlaced only: none", edid_pick(&E, EDID_TMDS_NOSCRAMBLE_KHZ, 0, 0) == -1, 0);
        monitor(&SVGA, 0, 0);
        ck("800x600 only: none that fits 1024x768",
           edid_pick(&E, EDID_TMDS_NOSCRAMBLE_KHZ, 1024, 768) == -1 &&
           edid_pick(&E, EDID_TMDS_NOSCRAMBLE_KHZ, 0, 0) == 0, 0);
        ck("a refused EDID: none", edid_pick(0, 1, 0, 0) == -1, 0);
    }

    /* ── the plan, on the HP ─────────────────────────────────────── */
    head("the plan, on the HP's registers and the LG");
    hp(); lg();
    r = plan_hp();
    ck("planned", r == XM_OK, r);
    ck("nothing was written by reading and planning", w_total == 0, w_total);
    ck("no read hit a dark well", unclaimed == 0, unclaimed);
    ck("1920x1080 at 60.00 Hz, 148.5 MHz", P.mode.hactive == 1920 &&
       P.mode.refresh_x100 == 6000u && P.mode.pclk_khz == 148500u, (long)P.mode.refresh_x100);
    ck("DPLL0 -- port A is on DPLL1 (DPCLKA 01E07801)", P.dpll == 0, P.dpll);
    ck("cfg 001001D0 / 00000E84", P.cfgcr0 == 0x001001D0u && P.cfgcr1 == 0xE84u, (long)P.cfgcr0);
    ck("HTOTAL 0897077F (1920 / 2200)", op_index(&P, 0x61000, 0x0897077Fu) >= 0, 0);
    ck("HBLANK 0897077F", op_index(&P, 0x61004, 0x0897077Fu) >= 0, 0);
    ck("HSYNC 080307D7 (2008-2052)", op_index(&P, 0x61008, 0x080307D7u) >= 0, 0);
    ck("VTOTAL 04640437 (1080 / 1125)", op_index(&P, 0x6100C, 0x04640437u) >= 0, 0);
    ck("VBLANK 04640000 (start unused on display 13)", op_index(&P, 0x61010, 0x04640000u) >= 0, 0);
    ck("VSYNC 0440043B (1084-1089)", op_index(&P, 0x61014, 0x0440043Bu) >= 0, 0);
    ck("PIPESRC 077F0437", op_index(&P, 0x6101C, 0x077F0437u) >= 0, 0);
    ck("DDI_FUNC_CTL B 91030000: on, DDI B, DVI, 8 bpc, +h +v",
       op_index(&P, 0x61400, 0x91030000u) >= 0, 0);
    ck("TRANS_CLK_SEL B 20000000 (PHY B)", op_index(&P, 0x46144, 0x20000000u) >= 0, 0);
    ck("line time 119 eighths of a microsecond (2200 / 148.5 MHz)",
       P.linetime == 119u && op_index(&P, 0x45274, 119u) >= 0, (long)P.linetime);
    ck("plane centred: 448,156", P.pos_x == 448 && P.pos_y == 156 &&
       op_index(&P, 0x7118C, (156u << 16) | 448u) >= 0, (long)P.pos_x);
    ck("buffer blocks 2048-4031 (slices 3 and 4)", op_index(&P, 0x7127C, 0x0FBF0800u) >= 0, 0);
    ck("the plane is pipe A's: CTL 94000008, stride 40, size 02FF03FF, surf 0",
       op_index(&P, 0x71180, 0x94000008u) >= 0 && op_index(&P, 0x71188, 0x40u) >= 0 &&
       op_index(&P, 0x71190, 0x02FF03FFu) >= 0 && op_index(&P, 0x7119C, 0) >= 0, 0);
    ck("PLANE_SURF is the LAST operation (it arms the plane)",
       P.ops[P.nops - 1].off == 0x7119Cu, (long)P.ops[P.nops - 1].off);
    ck("watermarks are pipe A's fields, none dropped",
       op_rmw(&P, 0x71240, 0x80004011u) >= 0 && op_rmw(&P, 0x71258, 0x8000C031u) >= 0 &&
       op_rmw(&P, 0x71268, 0x00004007u) >= 0 && P.wm_dropped == 0, P.wm_dropped);
    {
        int k = op_rmw(&P, 0x7103C, 0x01038C04u);
        ck("FIXED: MBUS credits copy only the FIELDS of A's A9038C04, and hold only them",
           k >= 0 && P.ops[k].clr == 0x01FFDFEFu && P.ops[k].hold == 0x01FFDFEFu, k);
    }
    ck("vswing entry 6 (the default) and no lane reversal", P.level == 6 && !P.reversal, P.level);
    ck("the plan fits its array", P.nops > 60 && P.nops < XM_MAX_OPS, P.nops);

    /* ── the screen of the plan ─────────────────────────────────── */
    nl = 0; longest = 0;
    xm_report_plan(&S, &P, 1, cap);
    ck("plan screen: the mode", has("1920x1080 at 60.00 Hz, 148.5 MHz, from the EDID"), nl);
    ck("  ... the PLL and that port A stays put",
       has("DPLL0  148.5 MHz  cfg 001001D0 00000E84  (port A stays on DPLL1)"), nl);
    ck("  ... DVI on DDI B", has("DDI B: DVI signalling, 4 lanes, vswing entry 6"), nl);
    ck("  ... the plane", has("plane  1024x768 at 448,156, unscaled"), nl);
    ck("  ... every operation listed", nl > P.nops, nl);
    ck("  ... the DDI_FUNC_CTL line", has("wr   061400 = 91030000") , nl);
    ck("  ... no line wider than the screen", longest <= GFX_LINE_MAX, longest);
    if(fails || getenv("XM_DUMP")) dump();
    nl = 0; longest = 0;
    xm_report_plan(&S, &P, 0, cap);
    ck("the `gfx on` summary points at `gfx plan` for the list",
       has("operations; `gfx plan` lists every one.") && nl < 15, nl);

    /* ── running it ─────────────────────────────────────────────── */
    head("`gfx on`, against the model");
    memcpy(before, regs, SPAN);
    r = xm_apply(&D, &P, &RES);
    ck("lit", r == XM_OK, r);
    ck("every operation done", RES.done == P.nops, RES.done);
    ck("NOTHING written on pipe A's side", w_forbidden == 0, w_forbidden);
    ck("  ... and pipe A's registers are word for word as they were", pipe_a_unchanged(), 0);
    ck("no write refused", RES.refused == 0, (long)RES.refused);
    ck("no group register was ever READ", grp_reads == 0, grp_reads);
    ck("no access to a dark well", unclaimed == 0, unclaimed);
    ck("DDI B IO power requested and on (45454 = F)", regs[0x45454 / 4] == 0xFu,
       (long)regs[0x45454 / 4]);
    ck("DBUF slices 3 and 4 on", (regs[0x44300 / 4] & 0xC0000000u) == 0xC0000000u &&
       (regs[0x44304 / 4] & 0xC0000000u) == 0xC0000000u, 0);
    ck("DPLL0 enabled, locked, powered (CC000000)", regs[0x46010 / 4] == 0xCC000000u,
       (long)regs[0x46010 / 4]);
    ck("DPCLKA 01E07001: B on DPLL0, ungated; A's bits as they were",
       regs[0x164280 / 4] == 0x01E07001u, (long)regs[0x164280 / 4]);
    ck("transcoder B running", (regs[0x71008 / 4] & 0xC0000000u) == 0xC0000000u, 0);
    ck("DDI B enabled and out of idle", (regs[0x64100 / 4] & 0x80000080u) == 0x80000000u,
       (long)regs[0x64100 / 4]);
    ck("plane B on", regs[0x71180 / 4] == 0x94000008u, (long)regs[0x71180 / 4]);
    ck("DBOX B is B1038C04: A's credits under B's own read-only bits -- what the HP read",
       regs[0x7103C / 4] == 0xB1038C04u, (long)regs[0x7103C / 4]);
    {
        int ok = 1;
        for(k = 0; k < 4; k++){
            uint32_t b = 0x6C800 + 0x100 * k;
            if(regs[(b + 0x04) / 4] & (1u << 26)) ok = 0;                    /* keeper   */
            if(((regs[(b + 0x90) / 4] >> 31) & 1u) != (k >= 1)) ok = 0;      /* loadgen  */
            if((regs[(b + 0x90) / 4] & 0x3FFFFu) != 0xA035u) ok = 0;         /* coeffs   */
            if(regs[(b + 0x88) / 4] != ((6u << 11) | 0x98u)) ok = 0;         /* swing    */
            if(regs[(b + 0x9C) / 4] != 0x7F000000u) ok = 0;                  /* n scalar */
            if(regs[(b + 0x94) / 4] != (0x80000000u | (2u << 18) | (6u << 3) | (1u << 29)))
                ok = 0;                                                       /* DW5      */
        }
        ck("all four PHY lanes: keeper off, loadgen 0/1/1/1, entry 6, training on", ok, 0);
    }
    ck("PHY lanes powered up (CL_DW10 bits 7:4 clear)", !(regs[0x6C028 / 4] & 0xF0u), 0);
    ck("frame counter moved: 1000 -> 1003 in 50 ms", RES.frm0 == 1000u && RES.frm1 == 1003u,
       (long)RES.frm1);
    /* order, from the write log */
    ck("the PLL is powered before its dividers, enabled after them",
       find_write(0x46010, 0) < find_write(0x164284, 0) &&
       find_write(0x164288, 0) < last_write(0x46010), 0);
    {
        int sel = find_write(0x164280, 0), ungate = find_write(0x164280, sel + 1);
        ck("DPCLKA: select, then ungate, in SEPARATE writes",
           sel >= 0 && ungate > sel && (w_val[sel] & (1u << 11)) && !(w_val[ungate] & (1u << 11)),
           ungate);
        ck("  ... after the PLL has locked (last DPLL write precedes them)",
           last_write(0x46010) < sel, sel);
    }
    ck("DDI_FUNC_CTL before TRANSCONF is enabled",
       find_write(0x61400, 0) < last_write(0x71008), 0);
    ck("the arbiter workaround before TRANSCONF is enabled",
       find_write(0x71028, 0) >= 0 && find_write(0x71028, 0) < last_write(0x71008), 0);
    ck("TRANSCONF enabled before DDI_BUF_CTL", last_write(0x71008) < find_write(0x64100, 0), 0);
    ck("the PHY's swing is set before the buffer is enabled",
       last_write(0x6C688) < find_write(0x64100, 0) || last_write(0x6C888) < find_write(0x64100, 0), 0);
    ck("PLANE_SURF is the last write", w_off[w_total - 1] == 0x7119Cu, (long)w_off[w_total - 1]);
    nl = 0; longest = 0;
    xm_report_result(&P, &RES, cap);
    ck("result: 'Lit.' and every write read back", has("Lit. ") && has("read back as it should"), nl);
    ck("  ... scanning out", has("1000 -> 1003 in 50 ms: scanning out."), nl);
    ck("  ... the write log", has("061400=91030000/91030000"), nl);
    ck("  ... no line wider than the screen", longest <= GFX_LINE_MAX, longest);

    /* ── a second `gfx on` refuses ───────────────────────────────── */
    xm_read(&D, &S);
    r = xm_plan_on(&S, &E, &KID, &P);
    ck("a second `gfx on` is refused: already lit", r == XM_EBUSY, r);

    /* ── `gfx off` ──────────────────────────────────────────────── */
    head("`gfx off`");
    memcpy(before, regs, SPAN);
    w_total = 0; w_forbidden = 0;
    r = xm_plan_off(&S, &P);
    ck("planned", r == XM_OK && P.dpll == 0, r);
    r = xm_apply(&D, &P, &RES);
    ck("off", r == XM_OK, r);
    ck("NOTHING written on pipe A's side", w_forbidden == 0 && pipe_a_unchanged(), w_forbidden);
    ck("plane B off", !(regs[0x71180 / 4] & (1u << 31)), 0);
    ck("transcoder B stopped", !(regs[0x71008 / 4] & 0xC0000000u), (long)regs[0x71008 / 4]);
    ck("DDI_FUNC_CTL B cleared, clock deselected", regs[0x61400 / 4] == 0 &&
       regs[0x46144 / 4] == 0, 0);
    ck("DDI B disabled and idle", (regs[0x64100 / 4] & 0x80000080u) == 0x80u, 0);
    ck("DDI B's clock gated again; A's bits untouched (01E07801)",
       regs[0x164280 / 4] == 0x01E07801u, (long)regs[0x164280 / 4]);
    ck("DPLL0 off and unpowered", regs[0x46010 / 4] == 0, (long)regs[0x46010 / 4]);
    ck("DPLL1 -- the panel's -- still CC000000", regs[0x46014 / 4] == 0xCC000000u, 0);
    ck("power requests are left as they were (added, never cleared)",
       regs[0x45454 / 4] == 0xFu, (long)regs[0x45454 / 4]);
    xm_read(&D, &S);
    ck("a second `gfx off` has nothing to do", xm_plan_off(&S, &P) == XM_EOFF, P.why);
    nl = 0; xm_report_plan(&S, &P, 1, cap);
    ck("  ... and says so", has("Refused: pipe B is not lit") && has("Nothing was written."), nl);
    r = plan_hp();
    ck("and `gfx on` works again after it", r == XM_OK, r);

    /* ── refusals: each one, and nothing written ───────────────── */
    head("refusals, before the first write");
    {
        struct { const char *what; uint32_t off, val; int why; } c[] = {
            { "DC6 enabled",                  0x45504, 2,           XM_EDC },
            { "pipe B's well off",            0x45404, 0x00000FCF,  XM_ENOWELL },
            { "PW_2 off",                     0x45404, 0x0000FC03,  XM_ENOWELL },
            { "transcoder B already enabled", 0x71008, 0x80000000,  XM_EBUSY },
            { "DDI_FUNC_CTL B enabled",       0x61400, 0x91030000,  XM_EBUSY },
            { "DDI B's buffer enabled",       0x64100, 0x80000000,  XM_EBUSY },
            { "plane B enabled",              0x71180, 0x80000008,  XM_EBUSY },
            { "pipe A's plane disabled",      0x70180, 0x14000008,  XM_ETEMPLATE },
            { "pipe A's plane X-tiled",       0x70180, 0x94000408,  XM_ETEMPLATE },
            { "pipe A's plane 10-bit",        0x70180, 0x92000008,  XM_ETEMPLATE },
            { "gamma on pipe B",              0x4AC80, 0x40000000,  XM_ECOLOR },
            { "CSC on pipe B",                0x49128, 0x80000000,  XM_ECOLOR },
            { "PHY B's comp powered down",    0x64C04, 1u << 23,    XM_EPHY },
            { "PHY B never calibrated",       0x6C100, 0,           XM_EPHY },
            { "MBUS joined",                  0x4438C, 0x80000000,  XM_EMBUS },
            { "pipe A's buffer reaches 2100", 0x7027C, 0x08340000,  XM_EDDB },
            { "pipe A's cursor in slice 3",   0x7017C, 0x08400800,  XM_EDDB },
            { "DPLL0 busy (port A on DPLL1)", 0x46010, 0xCC000000,  XM_ENOPLL },
            { "DPLL0 merely powered",         0x46010, 0x0C000000,  XM_ENOPLL },
            { "CDCLK 65 MHz",                 0x46000, 0x00780080,  XM_ECLOCK },
            { "an undefined DSSM reference",  0x51004, 0xE0000000,  XM_ECLOCK },
        };
        unsigned n;
        for(n = 0; n < sizeof c / sizeof c[0]; n++){
            char t[96];
            hp(); lg(); R(c[n].off, c[n].val);
            r = plan_hp();
            snprintf(t, sizeof t, "%s: %s", c[n].what, xm_why_name(c[n].why));
            ck(t, r == c[n].why && P.nops == 0, r);
            r = xm_apply(&D, &P, &RES);
            ck("  ... and applying the refused plan writes nothing",
               r == c[n].why && w_total == 0, w_total);
            nl = 0; longest = 0;
            xm_report_plan(&S, &P, 1, cap);
            ck("  ... the screen names the refusal and fits",
               has("Refused: ") && has("Nothing was written.") && longest <= GFX_LINE_MAX, longest);
        }
        hp(); lg(); dev_ok(); memset(&S, 0, sizeof S);
        ck("no engine read: ENODEV", xm_plan_on(&S, &E, &KID, &P) == XM_ENODEV, P.why);
        hp(); dev_ok(); xm_read(&D, &S);
        ck("no EDID: ENOMON", xm_plan_on(&S, 0, &KID, &P) == XM_ENOMON, P.why);
        monitor(&LG144, 0, 0); r = plan_hp();
        ck("only a 346.5 MHz mode: ENOMODE", r == XM_ENOMODE, r);
        monitor(&SVGA, 0, 0); r = plan_hp();
        ck("only 800x600: EFIT -- the 1024x768 plane does not fit", r == XM_EFIT, r);
        hp(); lg(); R(0x46010, 0xCC000000u); R(0x164280, 0x01E07001u);
        r = plan_hp(); nl = 0; longest = 0; xm_report_plan(&S, &P, 1, cap);
        ck("a PLL left running by a stopped `gfx on`: the screen says `gfx off` frees it",
           r == XM_ENOPLL && has("`gfx off` releases") && longest <= GFX_LINE_MAX, r);
        dev_ok(); xm_read(&D, &S);
        ck("  ... and `gfx off` does: it plans DPLL0's release", xm_plan_off(&S, &P) == XM_OK &&
           P.dpll == 0, P.dpll);
        hp(); lg(); R(0x45404, 0x0000FC0C);
        ck("the core off: ECORE", plan_hp() == XM_ECORE, P.why);
    }

    /* ── choices the HP does not exercise ───────────────────────── */
    head("choices the HP's state does not exercise");
    hp(); lg(); R(0x164280, 0x01E07800u);        /* port A on DPLL0 */
    R(0x46010, 0xCC000000u); R(0x46014, 0);
    ck("port A on DPLL0: DPLL1 is used", plan_hp() == XM_OK && P.dpll == 1, P.dpll);
    hp(); lg(); KID.hdmi_level = 3; KID.lane_reversal = 1;
    r = plan_hp();
    ck("the VBT's vswing entry 3 and lane reversal are honoured",
       r == XM_OK && P.level == 3 && P.reversal &&
       op_index(&P, 0x64100, 0x80010000u) >= 0, P.level);
    KID.hdmi_level = 9; r = plan_hp();
    ck("an entry past the table falls back to the default (6)", P.level == 6, P.level);
    KID.hdmi_level = -1; KID.lane_reversal = 0;
    /* Every mode the HP's plan used so far is +h +v, so swapped sync
       bits were invisible -- the harness said so. CVT reduced blanking
       is +h -v. */
    hp(); monitor(&RB60, 0, 0);
    r = plan_hp();
    ck("a +h -v mode: DDI_FUNC_CTL carries PHSYNC (bit 16) only -- 91010000",
       r == XM_OK && op_index(&P, 0x61400, 0x91010000u) >= 0, r);
    hp(); lg(); R(0x70250, WM(40, 3000));
    r = plan_hp();
    ck("a pipe A watermark bigger than B's buffer is written DISABLED",
       r == XM_OK && P.wm_dropped == 1 && op_rmw(&P, 0x71250, WM(40, 3000) & ~(1u << 31)) >= 0,
       P.wm_dropped);
    hp(); lg(); R(0x44300, 0xC0000000u); R(0x44304, 0xC0000000u); R(0x45454, 0xF);
    r = plan_hp();
    ck("slices and DDI IO already on: not requested again",
       r == XM_OK && op_index(&P, 0x44300, 0) < 0 && P.ops[0].off == 0x46010u,
       (long)P.ops[0].off);

    /* ── the allow-list ─────────────────────────────────────────── */
    head("the allow-list, register by register");
    hp(); lg(); plan_hp();
    {
        static const uint32_t A_SIDE[] = {
            0x60000, 0x60008, 0x6001C, 0x60400, 0x6007C, 0x70008, 0x70180,
            0x7019C, 0x7027C, 0x70240, 0x64000, 0x162000, 0x162880, 0x46014,
            0x16428C, 0x164290, 0x45404, 0x45400, 0x45450, 0x45008, 0x44FE8,
            0x420C0, 0x46140, 0x45270, 0x4438C, 0xC5100, 0x72008, 0x62400,
        };
        unsigned n, bad = 0;
        for(n = 0; n < sizeof A_SIDE / sizeof A_SIDE[0]; n++)
            if(xm_allowed(A_SIDE[n], 0, 0xFFFFFFFFu, 0x01E07801u, &P)) bad++;
        ck("28 registers of pipe A, port A, DPLL1, the main wells, MBUS, GMBUS: refused",
           bad == 0, (long)bad);
    }
    ck("transcoder B, pipe B, PHY B, DDI B: allowed",
       xm_allowed(0x61000, 0, 1, 0x01E07801u, &P) && xm_allowed(0x71180, 0, 1, 0x01E07801u, &P) &&
       xm_allowed(0x6C894, 0, 1, 0x01E07801u, &P) && xm_allowed(0x64100, 0, 1, 0x01E07801u, &P), 0);
    ck("DPCLKA: B's select and gate may change",
       xm_allowed(0x164280, 0x01E07801u, 0x01E07001u, 0x01E07801u, &P) &&
       xm_allowed(0x164280, 0x01E07801u, 0x01E07805u, 0x01E07801u, &P), 0);
    ck("  ... A's select and gate may NOT",
       !xm_allowed(0x164280, 0x01E07801u, 0x01E07800u, 0x01E07801u, &P) &&
       !xm_allowed(0x164280, 0x01E07801u, 0x01E07C01u, 0x01E07801u, &P), 0);
    ck("DDI IO wells: adding B's request only",
       xm_allowed(0x45454, 3, 0xB, 0, &P) && !xm_allowed(0x45454, 0xB, 3, 0, &P) &&
       !xm_allowed(0x45454, 3, 0x9, 0, &P) && !xm_allowed(0x45454, 3, 0x2B, 0, &P), 0);
    ck("DBUF slices 3/4: adding the request only; slices 1/2 never",
       xm_allowed(0x44300, 0, 0x80000000u, 0, &P) && !xm_allowed(0x44300, 0x80000000u, 0, 0, &P) &&
       !xm_allowed(0x45008, 0, 0x80000000u, 0, &P), 0);
    ck("CHICKEN_TRANS B: the frame-start field only",
       xm_allowed(0x420C4, 0x18000001u, 1u, 0, &P) && !xm_allowed(0x420C4, 0, 1u, 0, &P), 0);
    ck("DPLL0 allowed when the plan owns it and port A is elsewhere",
       xm_allowed(0x46010, 0, 1, 0x01E07801u, &P) && xm_allowed(0x164284, 0, 1, 0x01E07801u, &P), 0);
    ck("  ... and REFUSED if port A has moved onto it since",
       !xm_allowed(0x46010, 0, 1, 0x01E07800u, &P), 0);
    ck("an unaligned offset is refused", !xm_allowed(0x71182, 0, 1, 0, &P), 0);

    /* ── when it goes wrong ─────────────────────────────────────── */
    head("when it goes wrong, it stops, and says where");
    hp(); lg(); plan_hp(); memcpy(before, regs, SPAN); knob_nolock = 1;
    r = xm_apply(&D, &P, &RES);
    ck("a PLL that never locks: ETIMEOUT", r == XM_ETIMEOUT, r);
    ck("  ... at 'DPLL: locked'", RES.failed_op >= 0 &&
       !strcmp(P.ops[RES.failed_op].what, "DPLL: locked"), RES.failed_op);
    ck("  ... and nothing after it was written (no DPCLKA write)",
       find_write(0x164280, 0) < 0 && find_write(0x71180, 0) < 0, 0);
    ck("  ... pipe A untouched", w_forbidden == 0 && pipe_a_unchanged(), 0);
    nl = 0; longest = 0; xm_report_result(&P, &RES, cap);
    ck("  ... the screen: step, register, wanted and read",
       has("Stopped: a status bit never came.") && has("DPLL: locked") &&
       has("register 046010: waited for 40000000") && has("`gfx off` undoes"), nl);
    ck("  ... and fits", longest <= GFX_LINE_MAX, longest);
    if(fails) dump();

    hp(); lg(); plan_hp(); knob_stuck_reg = 0x164284u;
    r = xm_apply(&D, &P, &RES);
    ck("a register that ignores the write: EREADBACK", r == XM_EREADBACK, r);
    ck("  ... naming it, with what was wanted and what came back",
       RES.bad_off == 0x164284u && RES.bad_want == 0x001001D0u && RES.bad_got == 0x018001D4u,
       (long)RES.bad_got);
    ck("  ... and the very next operation never ran", find_write(0x164288, 0) < 0, 0);

    hp(); lg(); plan_hp(); knob_buf_stays_idle = 1;
    r = xm_apply(&D, &P, &RES);
    ck("a DDI buffer that stays idle: ETIMEOUT at 'DDI B active', plane never armed",
       r == XM_ETIMEOUT && !strcmp(P.ops[RES.failed_op].what, "DDI B active") &&
       find_write(0x7119C, 0) < 0, r);

    hp(); lg(); plan_hp(); knob_ddi_io_stuck = 1;
    r = xm_apply(&D, &P, &RES);
    ck("a DDI IO well that never comes on: stops at the first wait",
       r == XM_ETIMEOUT && RES.failed_op == 1 && w_total == 1, RES.failed_op);

    hp(); lg(); plan_hp();
    P.ops[P.nops - 2].off = 0x70180u;            /* a corrupted plan aims at pipe A */
    memcpy(before, regs, SPAN);
    r = xm_apply(&D, &P, &RES);
    ck("a plan aimed at pipe A's PLANE_CTL: EREFUSED, not written",
       r == XM_EREFUSED && RES.refused == 1 && w_forbidden == 0 && pipe_a_unchanged(), r);
    nl = 0; xm_report_result(&P, &RES, cap);
    ck("  ... the screen says the allow-list refused it",
       has("Stopped: the allow-list refused a write.") && has("register 070180"), nl);

    hp(); lg(); plan_hp(); knob_frm_stuck = 1;
    r = xm_apply(&D, &P, &RES);
    nl = 0; xm_report_result(&P, &RES, cap);
    ck("every write landed but the frame counter is still: said in red",
       r == XM_OK && has("NOT MOVING."), nl);

    /* ── v0.9.2.2: the background colour, and pipe B live ─────── */
    head("`gfx color` and `gfx b`: the diagnostics for a black monitor");
    hp(); lg(); plan_hp(); xm_apply(&D, &P, &RES);
    memcpy(before, regs, SPAN); w_total = 0; w_forbidden = 0;
    dev_ok(); xm_read(&D, &S);
    r = xm_plan_color(&S, XM_RGB_BLUE, &P);
    ck("lit: the colour is one write, to PIPE_BOTTOM_COLOR B",
       r == XM_OK && P.nops == 1 && P.ops[0].off == 0x71034u && P.ops[0].val == 0x3FFu, r);
    r = xm_apply(&D, &P, &RES);
    ck("  ... applied, read back, nothing on pipe A's side",
       r == XM_OK && regs[0x71034 / 4] == 0x3FFu && w_total == 1 && w_forbidden == 0 &&
       pipe_a_unchanged(), r);
    nl = 0; longest = 0;
    xm_report_result(&P, &RES, cap);
    ck("  ... and the screen says how to read the monitor",
       has("colour, black box inside") && has("still black") && longest <= GFX_LINE_MAX, nl);
    ck("white is all thirty bits, and no gamma/CSC bit is ever set",
       xm_plan_color(&S, 0xFFFFFFFFu, &P) == XM_OK && P.ops[0].val == 0x3FFFFFFFu, 0);
    nl = 0; longest = 0;
    xm_report_live(&D, cap);
    ck("`gfx b`: the frame counter moves while it runs", has(": running"), nl);
    ck("  ... SURFLIVE and BOTTOM are shown", has("SURFLIVE") && has("BOTTOM     000003FF"), nl);
    ck("  ... in lines that fit", longest <= GFX_LINE_MAX, longest);
    ck("  ... and reading wrote nothing", w_total == 1, w_total);
    if(fails) dump();
    hp(); dev_ok(); xm_read(&D, &S);
    ck("not lit: `gfx color` refuses, naming `gfx on`",
       xm_plan_color(&S, XM_RGB_BLUE, &P) == XM_ENOTLIT && P.nops == 0, P.why);
    nl = 0; xm_report_live(&D, cap);
    ck("  ... and `gfx b` says the pipe is stopped", has(": STOPPED"), nl);

    /* ── off, when B's PLL is the panel's ──────────────────────── */
    head("`gfx off` never stops the panel's PLL");
    hp(); R(0x164280, 0x01E07005u);              /* B selects DPLL1 (A's), ungated */
    R(0x64100, 0x80000000u);
    dev_ok(); xm_read(&D, &S);
    r = xm_plan_off(&S, &P);
    {
        int touches = 0;
        for(i = 0; i < P.nops; i++) if(P.ops[i].off == 0x46014u) touches++;
        ck("B on DPLL1 with port A: DDI B goes off, DPLL1 does not",
           r == XM_OK && P.dpll == -1 && touches == 0, touches);
    }
    memcpy(before, regs, SPAN); w_total = 0; w_forbidden = 0;
    r = xm_apply(&D, &P, &RES);
    ck("  ... applied, the panel's path is word for word unchanged",
       r == XM_OK && w_forbidden == 0 && pipe_a_unchanged(), r);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
