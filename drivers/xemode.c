/* ════════════════════════════════════════════════════════════════════
   drivers/xemode.c -- lighting pipe B on the HDMI socket.  v0.9.2.

   include/xemode.h carries the reasoning: what is lit, the order, and
   the five things that make it safe to try on the only laptop there
   is. This file is in four parts:

     1. xm_read()      reads, gated on power wells -- writes nothing
     2. xm_plan_on()   PURE: state + EDID + VBT in, a list of operations
        xm_plan_off()  out, or a refusal before the first one
     3. xm_allowed()   PURE: may this register take this value?
        xm_apply()     runs a plan through xm_wr(), the ONE write
     4. the screens

   ── THE SEAMS ───────────────────────────────────────────────────────

   GFX_RD(base, off)     a register read
   GFX_WR(base, off, v)  a register write -- used in xm_wr() and
                         NOWHERE ELSE; tools/check_gfx_readonly.py
                         fails the build otherwise, and also if xm_wr()
                         stops asking xm_allowed() first
   XM_SLEEP_MS(ms)       the one timed wait (the frame-counter check)

   tests/test_xemode.c replaces all three with a model engine that
   flips status bits the way the hardware does, and fails any write
   to pipe A's half of the machine.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/xedisp.h"
#include "../include/xemode.h"
#include "../include/edid.h"

#ifndef GFX_RD
#define GFX_RD(base, off) \
    (*(volatile uint32_t *)(uintptr_t)((base) + (off)))
#endif
#ifndef GFX_WR
#define GFX_WR(base, off, v) \
    (*(volatile uint32_t *)(uintptr_t)((base) + (off)) = (uint32_t)(v))
#endif
#ifndef XM_SLEEP_MS
#include "../include/pit.h"
#define XM_SLEEP_MS(ms) sleep_ms(ms)
#endif

/* ── registers ─────────────────────────────────────────────────────
   Offsets into BAR0, display 13, each named for its i915 symbol. */
#define BIT(n)              (1u << (n))

/* power: PW0 / PW1, always readable while the engine is */
#define R_PWR_DRV           0x45404u   /* HSW_PWR_WELL_CTL2 (driver)   */
#define R_PWR_DDI_DRV       0x45454u   /* ICL_PWR_WELL_CTL_DDI2        */
#define R_DC_STATE          0x45504u   /* DC_STATE_EN                  */
#define   DC_ANY            0x4000000Bu/* DC5 | DC6 | DC9 | DC3CO      */
#define PW_1    0
#define PW_2    1
#define PW_A    5
#define PW_B    6
#define PW_STATE(i)         (1u << ((i) * 2))
#define PW_REQ(i)           (2u << ((i) * 2))
#define DDI_IO_B            1          /* ICL_PW_CTL_IDX_DDI_B         */

#define R_DSSM              0x51004u   /* SKL_DSSM                     */
#define R_CDCLK_CTL         0x46000u   /* CDCLK_CTL                    */
#define R_DPLL_EN(n)        (0x46010u + 4u * (n))  /* ICL_DPLL_ENABLE  */
#define   PLL_ENABLE        BIT(31)
#define   PLL_LOCK          BIT(30)
#define   PLL_POWER_ENABLE  BIT(27)
#define   PLL_POWER_STATE   BIT(26)
#define R_CFGCR0(n)         (0x164284u + 8u * (n)) /* TGL_DPLL_CFGCR0  */
#define R_CFGCR1(n)         (0x164288u + 8u * (n)) /* TGL_DPLL_CFGCR1  */
#define R_DPCLKA            0x164280u  /* ICL_DPCLKA_CFGCR0            */
#define   DPCLKA_A_OFF      BIT(10)    /* ..._DDI_CLK_OFF(PHY_A)       */
#define   DPCLKA_B_OFF      BIT(11)    /* ..._DDI_CLK_OFF(PHY_B)       */
#define   DPCLKA_B_SEL      (3u << 2)  /* ..._DDI_CLK_SEL_MASK(PHY_B)  */
#define R_SDEISR            0xC4000u
#define   SDE_HPD_B         BIT(17)    /* SDE_DDI_HOTPLUG_ICP(HPD_PORT_B) */

/* combo PHY B (intel_combo_phy_regs.h, _ICL_COMBOPHY_B) */
#define PHYB                0x6C000u
#define R_PHY_MISC_B        0x64C04u   /* ICL_PHY_MISC(PORT_B)         */
#define   PHY_COMP_PWR_DOWN BIT(23)    /* ICL_PHY_MISC_DE_IO_COMP_PWR_DOWN */
#define R_COMP_DW0_B        (PHYB + 0x100u)        /* ICL_PORT_COMP_DW0 */
#define   COMP_INIT         BIT(31)
#define R_CL_DW5_B          (PHYB + 0x014u)        /* ICL_PORT_CL_DW5  */
#define   SUS_CLOCK_CONFIG  3u
#define R_CL_DW10_B         (PHYB + 0x028u)        /* ICL_PORT_CL_DW10 */
#define   PWR_DOWN_LN_MASK  (0xFu << 4)
#define R_PCS_DW1_GRP       (PHYB + 0x600u + 4u)  /* _ICL_PORT_PCS_GRP + 4*dw */
#define R_PCS_DW1_LN0       (PHYB + 0x800u + 4u)  /* _ICL_PORT_PCS_LN(0) + 4*dw */
#define   COMMON_KEEPER_EN  BIT(26)
#define R_TX_GRP(dw)        (PHYB + 0x680u + 4u * (dw))
#define R_TX_LN(dw, ln)     (PHYB + 0x880u + 0x100u * (ln) + 4u * (dw))
#define   LOADGEN_SELECT    BIT(31)                /* TX_DW4           */
#define   DW4_COEFF_MASK    0x3FFFFu               /* post1|post2|cursor */
#define   TX_TRAINING_EN    BIT(31)                /* TX_DW5           */
#define   TAP2_DISABLE      BIT(30)
#define   TAP3_DISABLE      BIT(29)
#define   CURSOR_PROGRAM    BIT(26)
#define   COEFF_POLARITY    BIT(25)
#define   SCALING_MODE_MASK (7u << 18)
#define   RTERM_MASK        (7u << 3)
#define   DW2_MASK          (BIT(15) | (7u << 11) | 0xFFu)
#define   N_SCALAR_MASK     (0x7Fu << 24)          /* TX_DW7           */

/* DBUF / MBUS (skl_watermark_regs.h) */
#define R_MBUS_CTL          0x4438Cu
#define   MBUS_JOIN         BIT(31)
static const uint32_t k_dbuf[4] = { 0x45008u, 0x44FE8u, 0x44300u, 0x44304u };
#define   DBUF_POWER_REQ    BIT(31)
#define   DBUF_POWER_STATE  BIT(30)
#define DDB_SLICE_BLOCKS    1024u      /* 4096 blocks, four slices     */
#define DDB_B_START         2048u      /* adlp_allowed_dbufs: B on S3+S4 */
#define DDB_B_END           4031u      /* leaves 64 for a cursor later */

/* pipe A plane 1 -- the template, read and NEVER written */
#define R_A_CTL     0x70180u
#define R_A_STRIDE  0x70188u
#define R_A_SIZE    0x70190u
#define R_A_SURF    0x7019Cu
#define R_A_OFFSET  0x701A4u
#define R_A_COLOR   0x701CCu   /* PLANE_COLOR_CTL                      */
#define R_A_BUF     0x7027Cu   /* PLANE_BUF_CFG                        */
#define R_A_CURBUF  0x7017Cu   /* CUR_BUF_CFG                          */
#define R_A_WM(l)   (0x70240u + 4u * (l))  /* PLANE_WM 0-5, SAGV, SAGV_TRANS */
#define R_A_WMTRANS 0x70268u
#define R_A_DBOX    0x7003Cu   /* PIPE_MBUS_DBOX_CTL                   */
#define R_A_SCL     0x6007Cu   /* TRANS_SET_CONTEXT_LATENCY            */

/* pipe B / transcoder B -- 0.9.2's own */
#define R_B_CONF    0x71008u   /* TRANSCONF                            */
#define   TRANSCONF_EN      BIT(31)
#define   TRANSCONF_STATE   BIT(30)
#define R_B_ARB     0x71028u   /* PIPE_ARB_CTL                         */
#define   PIPE_ARB_PROG_SLOTS BIT(13)
#define R_B_BOTTOM  0x71034u   /* SKL_BOTTOM_COLOR (PIPE_BOTTOM_COLOR)  */
#define R_B_DSL     0x71000u   /* PIPEDSL: the scanline being sent      */
#define R_B_SURFLIVE 0x711ACu  /* PLANE_SURFLIVE: what is scanned NOW   */
#define R_B_IIR     0x44418u   /* GEN8_DE_PIPE_IIR(PIPE_B)              */
#define R_B_MISC    0x71030u   /* PIPE_MISC                            */
#define R_B_CHICKEN 0x71038u   /* PIPE_CHICKEN                         */
#define R_B_DBOX    0x7103Cu
/* v0.9.2.1: the FIELDS of PIPE_MBUS_DBOX_CTL (skl_watermark_regs.h:
   A credit 3:0, I credit 7:5, B credit 12:8, BW credit 15:14, regulate
   16, B2B delay 19:17, B2B max 24:20). Bits 31:25 are not in i915's
   header, and on the HP they are READ-ONLY AND DIFFERENT PER PIPE:
   pipe A reads A9038C04, and pipe B, written A9038C04, read back
   B1038C04 -- bits 28:27 are 01 on A and 10 on B, which looks like
   the pipe's own index. 0.9.2 copied A's whole word, held all 32 bits
   and stopped at step 28. Now only the fields are copied and held. */
#define   DBOX_FIELDS       0x01FFDFEFu
/* and of PLANE_WM: enable 31, ignore-lines 30, auto-min-alloc 29,
   lines 26:14, blocks 12:0 -- the same caution, for the same reason */
#define   WM_FIELDS         0xE7FFDFFFu
#define R_B_FRM     0x71040u   /* PIPE_FRMCOUNT_G4X                    */
#define R_B_CTL     0x71180u
#define   PLANE_EN          BIT(31)
#define R_B_STRIDE  0x71188u
#define R_B_POS     0x7118Cu
#define R_B_SIZE    0x71190u
#define R_B_SURF    0x7119Cu
#define R_B_OFFSET  0x711A4u
#define R_B_COLOR   0x711CCu
#define R_B_WM(l)   (0x71240u + 4u * (l))
#define R_B_WMTRANS 0x71268u
#define R_B_BUF     0x7127Cu
#define R_B_HTOTAL  0x61000u
#define R_B_HBLANK  0x61004u
#define R_B_HSYNC   0x61008u
#define R_B_VTOTAL  0x6100Cu
#define R_B_VBLANK  0x61010u
#define R_B_VSYNC   0x61014u
#define R_B_SRC     0x6101Cu   /* PIPESRC                              */
#define R_B_VSHIFT  0x61028u   /* TRANS_VSYNCSHIFT                     */
#define R_B_MULT    0x6102Cu   /* TRANS_MULT                           */
#define R_B_SCL     0x6107Cu   /* TRANS_SET_CONTEXT_LATENCY            */
#define R_B_DIP     0x61200u   /* HSW_TVIDEO_DIP_CTL                   */
#define R_B_FUNC    0x61400u   /* TRANS_DDI_FUNC_CTL                   */
#define   FUNC_EN           BIT(31)
#define R_B_FUNC2   0x61404u
#define R_B_GAMMA   0x4AC80u   /* GAMMA_MODE                           */
#define R_B_CSC     0x49128u   /* PIPE_CSC_MODE                        */
#define R_B_ISR     0x44410u   /* GEN8_DE_PIPE_ISR(PIPE_B)             */
#define   PIPE_UNDERRUN     BIT(31)
/* shared registers with a B part */
#define R_B_CLKSEL  0x46144u   /* TRANS_CLK_SEL(TRANSCODER_B)          */
#define R_B_LINETIME 0x45274u  /* WM_LINETIME(PIPE_B)                  */
#define R_B_CHICKTR 0x420C4u   /* CHICKEN_TRANS(TRANSCODER_B)          */
#define   FRAME_START_DELAY (3u << 27)
#define R_DDI_BUF_B 0x64100u   /* DDI_BUF_CTL(PORT_B)                  */
#define   DDI_BUF_EN        BIT(31)
#define   DDI_BUF_REVERSAL  BIT(16)
#define   DDI_BUF_IDLE      BIT(7)

/* Polls, not time: an uncached register read on the HP is about a
   microsecond, so this is roughly a fifth of a second -- ten frames,
   and far beyond every timeout i915 uses for these bits. */
#define XM_POLLS 200000u

const char *xm_why_name(int why)
{
    switch(why){
    case XM_OK:        return "ok";
    case XM_ENODEV:    return "no display engine was opened";
    case XM_ENOMON:    return "no monitor answered: no EDID came back";
    case XM_ENOMODE:   return "the monitor offers no mode 0.9.2 can drive";
    case XM_EDC:       return "DC5/DC6 is on: the DMC firmware owns power";
    case XM_ECORE:     return "the display core (PW_1) is off";
    case XM_EBUSY:     return "pipe B is already lit -- `gfx off` first";
    case XM_ENOPLL:    return "both combo PLLs are in use";
    case XM_EPHY:      return "combo PHY B was never initialised";
    case XM_EMBUS:     return "MBUS is joined: pipe A owns every buffer slice";
    case XM_EDDB:      return "pipe A's buffer reaches into pipe B's slices";
    case XM_ETEMPLATE: return "pipe A's plane is not one 0.9.2 can copy";
    case XM_EFIT:      return "the mirrored plane is larger than every mode";
    case XM_ECOLOR:    return "pipe B's gamma or CSC is switched on";
    case XM_ECLOCK:    return "no PLL setting reaches that pixel clock";
    case XM_ENOWELL:   return "PW_2 or PW_B is off, and 0.9.2 does not wake them";
    case XM_EREFUSED:  return "the allow-list refused a write";
    case XM_EREADBACK: return "a register did not keep what was written";
    case XM_ETIMEOUT:  return "a status bit never came";
    case XM_EOFF:      return "pipe B is not lit; there is nothing to turn off";
    case XM_ENOTLIT:   return "pipe B is not running -- `gfx on` first";
    default:           return "unknown";
    }
}

/* ════ 1. READING ══════════════════════════════════════════════════ */

void xm_read(const gfx_dev_t *dev, xm_state_t *s)
{
    int i;
    uint32_t m = dev ? dev->mmio : 0;
    memset(s, 0, sizeof *s);
    if(!dev || dev->why != GFX_OK || !m) return;
    s->read = 1;

    /* PW0: power, DC, south display, DBUF, MBUS */
    s->pw_drv     = GFX_RD(m, R_PWR_DRV);
    s->pw_ddi_drv = GFX_RD(m, R_PWR_DDI_DRV);
    s->dc_state   = GFX_RD(m, R_DC_STATE);
    s->sdeisr     = GFX_RD(m, R_SDEISR);
    s->mbus_ctl   = GFX_RD(m, R_MBUS_CTL);
    for(i = 0; i < 4; i++) s->dbuf[i] = GFX_RD(m, k_dbuf[i]);
    if(!(s->pw_drv & PW_STATE(PW_1))) return;

    /* PW1: clocks, PLLs, transcoder A, DDI A/B, the combo PHYs */
    s->dssm      = GFX_RD(m, R_DSSM);
    s->cdclk_ctl = GFX_RD(m, R_CDCLK_CTL);
    for(i = 0; i < 2; i++){
        s->dpll_en[i]     = GFX_RD(m, R_DPLL_EN(i));
        s->dpll_cfgcr0[i] = GFX_RD(m, R_CFGCR0(i));
        s->dpll_cfgcr1[i] = GFX_RD(m, R_CFGCR1(i));
    }
    s->dpclka     = GFX_RD(m, R_DPCLKA);
    s->phy_misc_b = GFX_RD(m, R_PHY_MISC_B);
    s->comp_dw0_b = GFX_RD(m, R_COMP_DW0_B);
    s->cl_dw10_b  = GFX_RD(m, R_CL_DW10_B);
    s->ddi_b_buf  = GFX_RD(m, R_DDI_BUF_B);
    s->ddi_b_clk  = GFX_RD(m, R_B_CLKSEL);
    s->a_scl      = GFX_RD(m, R_A_SCL);

    /* PW_A: pipe A's planes -- the template */
    if(s->pw_drv & PW_STATE(PW_A)){
        s->a_ctl    = GFX_RD(m, R_A_CTL);
        s->a_stride = GFX_RD(m, R_A_STRIDE);
        s->a_size   = GFX_RD(m, R_A_SIZE);
        s->a_surf   = GFX_RD(m, R_A_SURF);
        s->a_offset = GFX_RD(m, R_A_OFFSET);
        s->a_color  = GFX_RD(m, R_A_COLOR);
        s->a_buf    = GFX_RD(m, R_A_BUF);
        s->a_cur_buf= GFX_RD(m, R_A_CURBUF);
        for(i = 0; i < 8; i++) s->a_wm[i] = GFX_RD(m, R_A_WM(i));
        s->a_wm_trans = GFX_RD(m, R_A_WMTRANS);
        s->a_dbox   = GFX_RD(m, R_A_DBOX);
    }

    /* PW_B: pipe B and transcoder B, as they are now */
    if(s->pw_drv & PW_STATE(PW_B)){
        s->pipe_b_powered = 1;
        s->b_conf  = GFX_RD(m, R_B_CONF);
        s->b_func  = GFX_RD(m, R_B_FUNC);
        s->b_ctl   = GFX_RD(m, R_B_CTL);
        s->b_gamma = GFX_RD(m, R_B_GAMMA);
        s->b_csc   = GFX_RD(m, R_B_CSC);
        s->b_frm   = GFX_RD(m, R_B_FRM);
        s->b_isr   = GFX_RD(m, R_B_ISR);
    }
}

/* ════ 2. PLANNING -- pure ══════════════════════════════════════════ */

/* i915 icl_calc_wrpll: the DCO must land in 7998-10000 MHz; of the
   dividers that put it there, the one nearest the middle wins. */
static const uint8_t k_div[] = {  2,  4,  6,  8, 10, 12,  14,  16,
                                 18, 20, 24, 28, 30, 32,  36,  40,
                                 42, 44, 48, 50, 52, 54,  56,  60,
                                 64, 66, 68, 70, 72, 76,  78,  80,
                                 84, 88, 90, 92, 96, 98, 100, 102,
                                  3,  5,  7,  9, 15, 21 };

int xm_wrpll(uint32_t port_khz, uint32_t dssm, uint32_t *cfgcr0, uint32_t *cfgcr1)
{
    uint32_t ref, afe, dco, best_dco = 0, best_c = 0xFFFFFFFFu;
    uint32_t dco_min = 7998000u, dco_max = 10000000u, mid, integer, rem, frac;
    uint32_t i, best = 0, pdiv = 0, qdiv = 0, kdiv = 0, penc, kenc;
    int halve = 0;

    switch((dssm >> 29) & 7u){                 /* icl_wrpll_ref_clock  */
    case 0: ref = 24000; break;
    case 1: ref = 19200; break;
    case 2: ref = 19200; halve = 1; break;     /* 38.4: the PLL halves it,
                                                  and WA #22010492432   */
    default: return 0;
    }
    if(!port_khz || port_khz > 2000000u) return 0;
    afe = port_khz * 5u;
    mid = (dco_min + dco_max) / 2u;
    for(i = 0; i < sizeof k_div; i++){
        uint32_t c;
        if(afe > dco_max / k_div[i]) continue;   /* no overflow, and too high */
        dco = afe * k_div[i];
        if(dco < dco_min || dco > dco_max) continue;
        c = dco > mid ? dco - mid : mid - dco;
        if(c < best_c){ best_c = c; best = k_div[i]; best_dco = dco; }
    }
    if(!best) return 0;

    /* icl_wrpll_get_multipliers */
    if(best % 2u == 0){
        if(best == 2)            { pdiv = 2; qdiv = 1;           kdiv = 1; }
        else if(best % 4u == 0)  { pdiv = 2; qdiv = best / 4u;   kdiv = 2; }
        else if(best % 6u == 0)  { pdiv = 3; qdiv = best / 6u;   kdiv = 2; }
        else if(best % 5u == 0)  { pdiv = 5; qdiv = best / 10u;  kdiv = 2; }
        else if(best % 14u == 0) { pdiv = 7; qdiv = best / 14u;  kdiv = 2; }
    } else if(best == 3 || best == 5 || best == 7){
        pdiv = best; qdiv = 1; kdiv = 1;
    } else {
        pdiv = best / 3u; qdiv = 1; kdiv = 3;
    }
    switch(pdiv){ case 2: penc = 1; break; case 3: penc = 2; break;
                  case 5: penc = 4; break; case 7: penc = 8; break;
                  default: return 0; }
    switch(kdiv){ case 1: kenc = 1; break; case 2: kenc = 2; break;
                  case 3: kenc = 4; break; default: return 0; }

    /* icl_wrpll_params_populate: dco = (dco_freq << 15) / ref, split
       so it never needs 64 bits: the remainder is under ref (24000),
       and 24000 << 15 fits. */
    integer = best_dco / ref;
    rem     = best_dco % ref;
    frac    = (rem << 15) / ref;
    if(halve) frac = (frac + 1u) / 2u;          /* DIV_ROUND_CLOSEST(f, 2) */
    if(integer > 0x3FFu) return 0;
    *cfgcr0 = (frac << 10) | integer;
    *cfgcr1 = (qdiv << 10) | ((qdiv == 1 ? 0u : 1u) << 9) |
              (kenc << 6) | (penc << 2);        /* CFSELOVRD_NORMAL_XTAL = 0 */
    return 1;
}

static int refuse(xm_plan_t *p, int why){ p->why = why; p->nops = 0; return why; }

static xm_op_t *op(xm_plan_t *p, int kind, uint32_t off, const char *what)
{
    xm_op_t *o;
    if(p->nops >= XM_MAX_OPS){ p->why = XM_EREFUSED; return 0; }
    o = &p->ops[p->nops++];
    memset(o, 0, sizeof *o);
    o->op = (uint8_t)kind; o->off = off; o->what = what; o->hold = 0xFFFFFFFFu;
    return o;
}
static void wr(xm_plan_t *p, uint32_t off, uint32_t v, uint32_t hold, const char *what)
{
    xm_op_t *o = op(p, XM_OP_WR, off, what);
    if(o){ o->val = v; o->hold = hold; }
}
static void rmw(xm_plan_t *p, uint32_t off, uint32_t clr, uint32_t set,
                uint32_t hold, const char *what)
{
    xm_op_t *o = op(p, XM_OP_RMW, off, what);
    if(o){ o->clr = clr; o->set = set; o->hold = hold; }
}
static void rmwg(xm_plan_t *p, uint32_t grp, uint32_t ln0, uint32_t clr,
                 uint32_t set, const char *what)
{
    xm_op_t *o = op(p, XM_OP_RMWG, grp, what);
    if(o){ o->rb = ln0; o->clr = clr; o->set = set; o->hold = clr | set; }
}
static void wset(xm_plan_t *p, uint32_t off, uint32_t bits, const char *what)
{
    xm_op_t *o = op(p, XM_OP_WSET, off, what);
    if(o) o->set = bits;
}
static void wclr(xm_plan_t *p, uint32_t off, uint32_t bits, const char *what)
{
    xm_op_t *o = op(p, XM_OP_WCLR, off, what);
    if(o) o->clr = bits;
}

/* The PLL port A runs on, or -1 if its clock is gated. */
static int port_a_pll(uint32_t dpclka)
{
    if(dpclka & DPCLKA_A_OFF) return -1;
    return (int)(dpclka & 3u);
}

/* i915 _icl_combo_phy_trans_hdmi: { dw2_swing_sel, dw7_n_scalar,
   dw4_cursor_coeff, dw4_post_cursor_2, dw4_post_cursor_1 }, used for
   HDMI on ADL-P (adlp_get_combo_buf_trans). Entry 6 is the default
   (hdmi_default_entry = last). */
static const uint8_t k_hdmi_trans[7][5] = {
    { 0xA, 0x60, 0x3F, 0x00, 0x00 },     /* 450 / 450 mV, 0.0 dB  */
    { 0xB, 0x73, 0x36, 0x00, 0x09 },     /* 450 / 650,    3.2     */
    { 0x6, 0x7F, 0x31, 0x00, 0x0E },     /* 450 / 850,    5.5     */
    { 0xB, 0x73, 0x3F, 0x00, 0x00 },     /* 650 / 650,    0.0     */
    { 0x6, 0x7F, 0x37, 0x00, 0x08 },     /* 650 / 850,    2.3     */
    { 0x6, 0x7F, 0x3F, 0x00, 0x00 },     /* 850 / 850,    0.0     */
    { 0x6, 0x7F, 0x35, 0x00, 0x0A },     /* 600 / 850,    3.0     */
};

int xm_plan_on(const xm_state_t *s, const edid_t *e, const gfx_child_t *child,
               xm_plan_t *p)
{
    uint32_t w, h, ht, hss, hse, vt, vss, vse, cdclk, alloc, func, ln;
    int k, n, apll, i;
    const uint8_t *t;

    memset(p, 0, sizeof *p);
    p->on = 1; p->dpll = -1; p->level = -1;
    if(!s || !s->read) return refuse(p, XM_ENODEV);
    if(!(s->pw_drv & PW_STATE(PW_1))) return refuse(p, XM_ECORE);
    if(s->dc_state & DC_ANY) return refuse(p, XM_EDC);
    if(!(s->pw_drv & PW_STATE(PW_2)) || !(s->pw_drv & PW_STATE(PW_B)) ||
       !(s->pw_drv & PW_STATE(PW_A)))
        return refuse(p, XM_ENOWELL);
    if((s->b_conf & (TRANSCONF_EN | TRANSCONF_STATE)) || (s->b_func & FUNC_EN) ||
       (s->b_ctl & PLANE_EN) || (s->ddi_b_buf & DDI_BUF_EN))
        return refuse(p, XM_EBUSY);

    /* The template: pipe A plane 1, as the firmware set it. Enabled,
       linear, XRGB8888 (ICL format field 8), a stride. */
    if(!(s->a_ctl & PLANE_EN) || ((s->a_ctl >> 10) & 7u) != 0 ||
       ((s->a_ctl >> 23) & 0x1Fu) != 8u || !(s->a_stride & 0x7FFu))
        return refuse(p, XM_ETEMPLATE);
    w = (s->a_size & 0x1FFFu) + 1u;
    h = ((s->a_size >> 16) & 0x1FFFu) + 1u;
    p->plane_w = w; p->plane_h = h;

    if(!e || e->why != EDID_OK) return refuse(p, XM_ENOMON);
    k = edid_pick(e, EDID_TMDS_NOSCRAMBLE_KHZ, w, h);
    if(k < 0)
        return refuse(p, edid_pick(e, EDID_TMDS_NOSCRAMBLE_KHZ, 0, 0) >= 0
                         ? XM_EFIT : XM_ENOMODE);
    p->mode = e->modes[k];

    if((s->b_gamma & (BIT(31) | BIT(30))) || (s->b_csc & (BIT(31) | BIT(30))))
        return refuse(p, XM_ECOLOR);
    if((s->phy_misc_b & PHY_COMP_PWR_DOWN) || !(s->comp_dw0_b & COMP_INIT))
        return refuse(p, XM_EPHY);
    if(s->mbus_ctl & MBUS_JOIN) return refuse(p, XM_EMBUS);
    if(((s->a_buf >> 16) & 0x1FFFu) >= DDB_B_START ||
       ((s->a_cur_buf >> 16) & 0x1FFFu) >= DDB_B_START)
        return refuse(p, XM_EDDB);

    /* A PLL: free, and not the one port A runs on. DPLL0 first. */
    apll = port_a_pll(s->dpclka);
    for(n = 0; n < 2; n++)
        if(n != apll && !(s->dpll_en[n] & (PLL_ENABLE | PLL_POWER_ENABLE))) break;
    if(n == 2) return refuse(p, XM_ENOPLL);
    p->dpll = n;

    /* HDMI at 8 bpc: the port clock is the pixel clock. */
    p->port_khz = p->mode.pclk_khz;
    if(!xm_wrpll(p->port_khz, s->dssm, &p->cfgcr0, &p->cfgcr1))
        return refuse(p, XM_ECLOCK);
    /* Two pixels a CDCLK on display 13: CDCLK_CTL's decimal field is
       (kHz - 1000) / 500. */
    cdclk = ((s->cdclk_ctl & 0x7FFu) + 2u) * 500u;
    if(p->mode.pclk_khz > 2u * cdclk) return refuse(p, XM_ECLOCK);

    p->level = (child && child->hdmi_level >= 0 && child->hdmi_level < 7)
               ? child->hdmi_level : 6;
    p->reversal = (child && child->lane_reversal) || (s->ddi_b_buf & DDI_BUF_REVERSAL);
    p->pos_x = (p->mode.hactive - w) / 2u;
    p->pos_y = (p->mode.vactive - h) / 2u;
    p->ddb_start = DDB_B_START; p->ddb_end = DDB_B_END;
    alloc = DDB_B_END - DDB_B_START + 1u;

    ht  = (uint32_t)p->mode.hactive + p->mode.hblank;
    hss = (uint32_t)p->mode.hactive + p->mode.hso;
    hse = hss + p->mode.hspw;
    vt  = (uint32_t)p->mode.vactive + p->mode.vblank;
    vss = (uint32_t)p->mode.vactive + p->mode.vso;
    vse = vss + p->mode.vspw;
    /* skl_linetime_wm: line time in 1/8 us, rounded up */
    p->linetime = (ht * 8000u + p->mode.pclk_khz - 1u) / p->mode.pclk_khz;
    if(p->linetime > 0x1FFu) p->linetime = 0x1FFu;

    /* ── power (hsw_power_well_enable, gen9_dbuf_slice_set) ─────────
       Requests are only ever ADDED. */
    if(!(s->pw_ddi_drv & PW_STATE(DDI_IO_B))){
        rmw(p, R_PWR_DDI_DRV, 0, PW_REQ(DDI_IO_B), PW_REQ(DDI_IO_B), "DDI B IO power: request");
        wset(p, R_PWR_DDI_DRV, PW_STATE(DDI_IO_B), "DDI B IO power: on");
    }
    for(i = 2; i < 4; i++){
        if(s->dbuf[i] & DBUF_POWER_STATE) continue;
        rmw(p, k_dbuf[i], 0, DBUF_POWER_REQ, DBUF_POWER_REQ,
            i == 2 ? "DBUF slice 3: request" : "DBUF slice 4: request");
        wset(p, k_dbuf[i], DBUF_POWER_STATE, i == 2 ? "DBUF slice 3: on" : "DBUF slice 4: on");
    }

    /* ── the PLL (combo_pll_enable) ─────────────────────────────── */
    rmw(p, R_DPLL_EN(n), 0, PLL_POWER_ENABLE, PLL_POWER_ENABLE, "DPLL: power");
    wset(p, R_DPLL_EN(n), PLL_POWER_STATE, "DPLL: powered");
    wr(p, R_CFGCR0(n), p->cfgcr0, 0xFFFFFFFFu, "DPLL: DCO");
    wr(p, R_CFGCR1(n), p->cfgcr1, 0xFFFFFFFFu, "DPLL: dividers");
    rmw(p, R_DPLL_EN(n), 0, PLL_ENABLE, PLL_ENABLE | PLL_POWER_ENABLE, "DPLL: enable");
    wset(p, R_DPLL_EN(n), PLL_LOCK, "DPLL: locked");

    /* ── clocks (icl_ddi_combo_enable_clock: two separate writes) ── */
    rmw(p, R_DPCLKA, DPCLKA_B_SEL, (uint32_t)n << 2, DPCLKA_B_SEL, "DDI B clock: select");
    rmw(p, R_DPCLKA, DPCLKA_B_OFF, 0, DPCLKA_B_OFF, "DDI B clock: ungate");
    /* intel_ddi_enable_transcoder_clock: TGL_TRANS_CLK_SEL_PORT(PHY_B) */
    wr(p, R_B_CLKSEL, (1u + 1u) << 28, 0xF0000000u, "transcoder B clock");
    /* set_infoframes(enable = false): DVI carries none */
    wr(p, R_B_DIP, 0, 0xFFFFFFFFu, "infoframes off");

    /* ── pipe and transcoder (hsw_crtc_enable) ─────────────────── */
    wr(p, R_B_SRC, ((p->mode.hactive - 1u) << 16) | (p->mode.vactive - 1u),
       0xFFFFFFFFu, "PIPESRC");
    /* bdw_set_pipe_misc: 8 bpc, PIXEL_ROUNDING_TRUNC */
    wr(p, R_B_MISC, BIT(8), BIT(8) | (7u << 5), "PIPE_MISC");
    /* intel_set_transcoder_timings, display 13: VBLANK_START unused (1) */
    wr(p, R_B_SCL, s->a_scl & 0xFFFFu, 0xFFFFu, "context latency (as A)");
    wr(p, R_B_VSHIFT, 0, 0xFFFFFFFFu, "VSYNCSHIFT");
    wr(p, R_B_HTOTAL, (p->mode.hactive - 1u) | ((ht - 1u) << 16), 0xFFFFFFFFu, "HTOTAL");
    wr(p, R_B_HBLANK, (p->mode.hactive - 1u) | ((ht - 1u) << 16), 0xFFFFFFFFu, "HBLANK");
    wr(p, R_B_HSYNC,  (hss - 1u) | ((hse - 1u) << 16), 0xFFFFFFFFu, "HSYNC");
    wr(p, R_B_VTOTAL, (p->mode.vactive - 1u) | ((vt - 1u) << 16), 0xFFFFFFFFu, "VTOTAL");
    wr(p, R_B_VBLANK, 0u | ((vt - 1u) << 16), 0xFFFFFFFFu, "VBLANK");
    wr(p, R_B_VSYNC,  (vss - 1u) | ((vse - 1u) << 16), 0xFFFFFFFFu, "VSYNC");
    wr(p, R_B_MULT, 0, 0xFFFFFFFFu, "TRANS_MULT");
    rmw(p, R_B_CHICKTR, FRAME_START_DELAY, 0, FRAME_START_DELAY, "frame start delay 1");
    wr(p, R_B_CONF, 0, TRANSCONF_EN, "TRANSCONF (still off)");
    wr(p, R_B_LINETIME, p->linetime, 0x1FFu, "line time");
    /* icl_set_pipe_chicken */
    rmw(p, R_B_CHICKEN, 0, BIT(30) | BIT(15) | BIT(7), BIT(30) | BIT(15) | BIT(7),
        "PIPE_CHICKEN");
    /* MBUS credits and watermarks: pipe A's, for a plane this size */
    rmw(p, R_B_DBOX, DBOX_FIELDS, s->a_dbox & DBOX_FIELDS, DBOX_FIELDS,
        "MBUS credits (as A)");
    wr(p, R_B_BUF, p->ddb_start | (p->ddb_end << 16), 0x1FFF1FFFu, "buffer blocks");
    for(i = 0; i < 8; i++){
        uint32_t v = s->a_wm[i];
        if((v & BIT(31)) && (v & 0x1FFFu) > alloc){ v &= ~BIT(31); p->wm_dropped++; }
        rmw(p, R_B_WM(i), WM_FIELDS, v & WM_FIELDS, WM_FIELDS,
            i < 6 ? "watermark (as A)" : i == 6 ? "SAGV watermark (as A)"
                                                : "SAGV trans wm (as A)");
    }
    {
        uint32_t v = s->a_wm_trans;
        if((v & BIT(31)) && (v & 0x1FFFu) > alloc){ v &= ~BIT(31); p->wm_dropped++; }
        rmw(p, R_B_WMTRANS, WM_FIELDS, v & WM_FIELDS, WM_FIELDS, "transition wm (as A)");
    }

    /* ── the transcoder (intel_ddi_enable) ──────────────────────── */
    func = FUNC_EN | ((1u + 1u) << 27) /* TGL_TRANS_DDI_SELECT_PORT(B) */
         | (1u << 24)                  /* TRANS_DDI_MODE_SELECT_DVI    */
         | (0u << 20)                  /* TRANS_DDI_BPC_8              */
         | (p->mode.vpos ? BIT(17) : 0u) | (p->mode.hpos ? BIT(16) : 0u);
    wr(p, R_B_FUNC2, 0, 0xFFFFFFFFu, "DDI_FUNC_CTL2");
    wr(p, R_B_FUNC, func, 0xFFFFFFFFu, "DDI_FUNC_CTL: DDI B, DVI");
    /* intel_enable_transcoder: Wa_22012358565 (display 13) first. No
       wait after: i915 waits for nothing here either, and the frame
       counter at the end is the proof the pipe runs. */
    rmw(p, R_B_ARB, 0, PIPE_ARB_PROG_SLOTS, PIPE_ARB_PROG_SLOTS, "PIPE_ARB_CTL (WA)");
    rmw(p, R_B_CONF, 0, TRANSCONF_EN, TRANSCONF_EN, "TRANSCONF: enable");

    /* ── the PHY (icl_combo_phy_set_signal_levels, HDMI) ────────── */
    t = k_hdmi_trans[p->level];
    rmwg(p, R_PCS_DW1_GRP, R_PCS_DW1_LN0, COMMON_KEEPER_EN, 0, "PHY: keeper off");
    for(ln = 0; ln < 4; ln++)          /* icl_combo_phy_loadgen_select, 4 lanes */
        rmw(p, R_TX_LN(4, ln), LOADGEN_SELECT, ln >= 1 ? LOADGEN_SELECT : 0,
            LOADGEN_SELECT, "PHY: loadgen");
    rmw(p, R_CL_DW5_B, 0, SUS_CLOCK_CONFIG, SUS_CLOCK_CONFIG, "PHY: SUS clock");
    rmwg(p, R_TX_GRP(5), R_TX_LN(5, 0), TX_TRAINING_EN, 0, "PHY: training off");
    /* icl_ddi_combo_vswing_program */
    rmwg(p, R_TX_GRP(5), R_TX_LN(5, 0),
         SCALING_MODE_MASK | RTERM_MASK | COEFF_POLARITY | CURSOR_PROGRAM |
         TAP2_DISABLE | TAP3_DISABLE,
         (2u << 18) | (6u << 3) | TAP3_DISABLE, "PHY: scaling, rterm");
    for(ln = 0; ln < 4; ln++)
        rmw(p, R_TX_LN(2, ln), DW2_MASK,
            ((uint32_t)(t[0] >> 3) << 15) | ((uint32_t)(t[0] & 7u) << 11) | 0x98u,
            DW2_MASK, "PHY: swing");
    for(ln = 0; ln < 4; ln++)
        rmw(p, R_TX_LN(4, ln), DW4_COEFF_MASK,
            ((uint32_t)t[4] << 12) | ((uint32_t)t[3] << 6) | t[2],
            DW4_COEFF_MASK, "PHY: de-emphasis");
    for(ln = 0; ln < 4; ln++)
        rmw(p, R_TX_LN(7, ln), N_SCALAR_MASK, (uint32_t)t[1] << 24,
            N_SCALAR_MASK, "PHY: n scalar");
    rmwg(p, R_TX_GRP(5), R_TX_LN(5, 0), 0, TX_TRAINING_EN, "PHY: training on");
    /* intel_combo_phy_power_up_lanes: HDMI is four lanes */
    rmw(p, R_CL_DW10_B, PWR_DOWN_LN_MASK, 0, PWR_DOWN_LN_MASK, "PHY: lanes up");
    /* intel_ddi_buf_enable, then wait for it to leave idle */
    wr(p, R_DDI_BUF_B, DDI_BUF_EN | (p->reversal ? DDI_BUF_REVERSAL : 0u),
       DDI_BUF_EN | DDI_BUF_REVERSAL, "DDI_BUF_CTL: enable");
    wclr(p, R_DDI_BUF_B, DDI_BUF_IDLE, "DDI B active");

    /* ── the plane: pipe A's, at the centre. SURF last -- it arms. ── */
    wr(p, R_B_COLOR,  s->a_color,  0xFFFFFFFFu, "PLANE_COLOR_CTL (as A)");
    wr(p, R_B_STRIDE, s->a_stride, 0xFFFFFFFFu, "PLANE_STRIDE (as A)");
    wr(p, R_B_POS,    (p->pos_y << 16) | p->pos_x, 0xFFFFFFFFu, "PLANE_POS: centred");
    wr(p, R_B_SIZE,   s->a_size,   0xFFFFFFFFu, "PLANE_SIZE (as A)");
    wr(p, R_B_OFFSET, s->a_offset, 0xFFFFFFFFu, "PLANE_OFFSET (as A)");
    wr(p, R_B_CTL,    s->a_ctl,    0xFFFFFFFFu, "PLANE_CTL (as A)");
    wr(p, R_B_SURF,   s->a_surf,   0xFFFFF000u, "PLANE_SURF: the framebuffer");

    if(p->why) return refuse(p, p->why);       /* ran out of room */
    return XM_OK;
}

int xm_plan_off(const xm_state_t *s, xm_plan_t *p)
{
    int n, apll;
    memset(p, 0, sizeof *p);
    p->on = 0; p->dpll = -1; p->level = -1;
    if(!s || !s->read) return refuse(p, XM_ENODEV);
    if(!(s->pw_drv & PW_STATE(PW_1))) return refuse(p, XM_ECORE);

    if(s->pipe_b_powered && (s->b_ctl & PLANE_EN)){
        rmw(p, R_B_CTL, PLANE_EN, 0, PLANE_EN, "plane off");
        wr(p, R_B_SURF, 0, 0xFFFFF000u, "plane: arm");
    }
    if(s->pipe_b_powered && (s->b_conf & TRANSCONF_EN)){
        rmw(p, R_B_CONF, TRANSCONF_EN, 0, TRANSCONF_EN, "TRANSCONF: disable");
        wclr(p, R_B_CONF, TRANSCONF_STATE, "transcoder B stopped");
    }
    if(s->pipe_b_powered && (s->b_func & FUNC_EN))
        wr(p, R_B_FUNC, 0, 0xFFFFFFFFu, "DDI_FUNC_CTL: off");
    if(s->ddi_b_clk)
        wr(p, R_B_CLKSEL, 0, 0xF0000000u, "transcoder B clock off");
    if(s->ddi_b_buf & DDI_BUF_EN){
        rmw(p, R_DDI_BUF_B, DDI_BUF_EN, 0, DDI_BUF_EN, "DDI_BUF_CTL: disable");
        wset(p, R_DDI_BUF_B, DDI_BUF_IDLE, "DDI B idle");
    }
    if(!(s->dpclka & DPCLKA_B_OFF))
        rmw(p, R_DPCLKA, 0, DPCLKA_B_OFF, DPCLKA_B_OFF, "DDI B clock: gate");

    /* The PLL port B selects, if it is on and port A is not on it. */
    n = (int)((s->dpclka & DPCLKA_B_SEL) >> 2);
    apll = port_a_pll(s->dpclka);
    if(n < 2 && n != apll && (s->dpll_en[n] & (PLL_ENABLE | PLL_POWER_ENABLE))){
        p->dpll = n;
        if(s->dpll_en[n] & PLL_ENABLE){
            rmw(p, R_DPLL_EN(n), PLL_ENABLE, 0, PLL_ENABLE, "DPLL: disable");
            wclr(p, R_DPLL_EN(n), PLL_LOCK, "DPLL: unlocked");
        }
        rmw(p, R_DPLL_EN(n), PLL_POWER_ENABLE, 0, PLL_POWER_ENABLE, "DPLL: power off");
        wclr(p, R_DPLL_EN(n), PLL_POWER_STATE, "DPLL: unpowered");
    }
    if(!p->nops) return refuse(p, XM_EOFF);
    return XM_OK;
}

int xm_plan_color(const xm_state_t *s, uint32_t rgb, xm_plan_t *p)
{
    memset(p, 0, sizeof *p);
    p->on = 2; p->dpll = -1; p->level = -1;
    if(!s || !s->read) return refuse(p, XM_ENODEV);
    if(!(s->pw_drv & PW_STATE(PW_1))) return refuse(p, XM_ECORE);
    if(!s->pipe_b_powered || !(s->b_conf & TRANSCONF_EN)) return refuse(p, XM_ENOTLIT);
    /* bits 31:30 are gamma/CSC for the colour itself: left clear, so it
       reaches the port exactly as written */
    wr(p, R_B_BOTTOM, rgb & 0x3FFFFFFFu, 0x3FFFFFFFu, "background colour");
    return XM_OK;
}

/* ════ 3. WRITING ══════════════════════════════════════════════════ */

/* May `off` go from `old` to `val`? The whole of the answer is here.

   Pipe B, transcoder B, combo PHY B and DDI B's buffer are B's alone.
   Everything else is SHARED with the panel's path, and only the bits
   that belong to B may change -- the rest must be written back exactly
   as they were read. Anything not named is refused, which is what
   keeps pipe A, transcoder A, DDI A, PHY A and port A's PLL out of
   reach whatever a plan says. */
int xm_allowed(uint32_t off, uint32_t old, uint32_t val, uint32_t dpclka,
               const xm_plan_t *p)
{
    uint32_t ch = old ^ val;
    if(off & 3u) return 0;
    if(off >= 0x61000u && off < 0x62000u) return 1;      /* transcoder B */
    if(off >= 0x71000u && off < 0x72000u) return 1;      /* pipe B       */
    if(off >= PHYB && off < PHYB + 0x1000u) return 1;    /* combo PHY B  */
    if(off == R_DDI_BUF_B || off == R_B_CLKSEL || off == R_B_LINETIME) return 1;

    if(off == R_PWR_DDI_DRV)            /* add DDI B's request, only that */
        return !(ch & ~PW_REQ(DDI_IO_B)) && (val & PW_REQ(DDI_IO_B));
    if(off == k_dbuf[2] || off == k_dbuf[3])  /* B's slices: add, only */
        return !(ch & ~DBUF_POWER_REQ) && (val & DBUF_POWER_REQ);
    if(off == R_B_CHICKTR) return !(ch & ~FRAME_START_DELAY);
    if(off == R_DPCLKA)    return !(ch & ~(DPCLKA_B_SEL | DPCLKA_B_OFF));

    if(p && p->dpll >= 0 && p->dpll < 2 && p->dpll != port_a_pll(dpclka)){
        uint32_t n = (uint32_t)p->dpll;
        if(off == R_DPLL_EN(n) || off == R_CFGCR0(n) || off == R_CFGCR1(n))
            return 1;
    }
    return 0;
}

/* THE write. Asks first, writes once, reads back, compares what must
   hold. Nothing else in this file writes a register. */
static int xm_wr(const gfx_dev_t *dev, const xm_plan_t *p, xm_result_t *res,
                 uint32_t off, uint32_t old, uint32_t val, uint32_t rb, uint32_t hold)
{
    uint32_t back, dpclka = GFX_RD(dev->mmio, R_DPCLKA);
    if(off >= GFX_MMIO_SPAN || !xm_allowed(off, old, val, dpclka, p)){
        res->refused++;
        res->bad_off = off; res->bad_want = val; res->bad_got = old;
        return XM_EREFUSED;
    }
    GFX_WR(dev->mmio, off, val);
    res->writes++;
    back = GFX_RD(dev->mmio, rb ? rb : off);
    if(res->nwlog < XM_WLOG_MAX){
        res->wlog[res->nwlog].off = off; res->wlog[res->nwlog].val = val;
        res->wlog[res->nwlog].back = back; res->nwlog++;
    }
    if((back ^ val) & hold){
        res->bad_off = rb ? rb : off; res->bad_want = val; res->bad_got = back;
        return XM_EREADBACK;
    }
    return XM_OK;
}

int xm_apply(const gfx_dev_t *dev, const xm_plan_t *p, xm_result_t *res)
{
    int i, why = XM_OK;
    memset(res, 0, sizeof *res);
    res->failed_op = -1;
    if(!dev || dev->why != GFX_OK || !dev->mmio) return res->why = XM_ENODEV;
    if(!p || p->why) return res->why = (p ? p->why : XM_ENODEV);

    for(i = 0; i < p->nops; i++){
        const xm_op_t *o = &p->ops[i];
        uint32_t v, k;
        switch(o->op){
        case XM_OP_WR:
            v = GFX_RD(dev->mmio, o->off);
            why = xm_wr(dev, p, res, o->off, v, o->val, 0, o->hold);
            break;
        case XM_OP_RMW:
            v = GFX_RD(dev->mmio, o->off);
            why = xm_wr(dev, p, res, o->off, v, (v & ~o->clr) | o->set, 0, o->hold);
            break;
        case XM_OP_RMWG:
            /* read lane 0, write the group: i915 never reads a group
               register, so neither does this */
            v = GFX_RD(dev->mmio, o->rb);
            why = xm_wr(dev, p, res, o->off, v, (v & ~o->clr) | o->set, o->rb, o->hold);
            break;
        case XM_OP_WSET:
        case XM_OP_WCLR:
            for(k = 0; k < XM_POLLS; k++){
                v = GFX_RD(dev->mmio, o->off);
                if(o->op == XM_OP_WSET ? (v & o->set) == o->set : !(v & o->clr)) break;
            }
            if(k == XM_POLLS){
                res->bad_off = o->off;
                res->bad_want = o->op == XM_OP_WSET ? o->set : 0;
                res->bad_got = v;
                why = XM_ETIMEOUT;
            }
            break;
        default:
            why = XM_EREFUSED;
        }
        if(why){ res->failed_op = i; return res->why = why; }
        res->done++;
    }

    /* Is pipe B actually scanning? Its frame counter, 50 ms apart --
       three frames at 60 Hz -- and the underrun flag. */
    if(p->on == 1){
        res->frm0 = GFX_RD(dev->mmio, R_B_FRM);
        XM_SLEEP_MS(50);
        res->frm1 = GFX_RD(dev->mmio, R_B_FRM);
        res->isr  = GFX_RD(dev->mmio, R_B_ISR);
    }
    return res->why = XM_OK;
}

/* ════ 4. THE SCREENS ══════════════════════════════════════════════ */

typedef struct { char b[GFX_LINE_MAX + 1]; int n; } xl_t;
static gfx_emit_t x_emit;
static void X0(xl_t *l){ l->n = 0; l->b[0] = 0; }
static void Xc(xl_t *l, char c){ if(l->n < GFX_LINE_MAX){ l->b[l->n++] = c; l->b[l->n] = 0; } }
static void Xs(xl_t *l, const char *s){ while(s && *s) Xc(l, *s++); }
static void Xh(xl_t *l, uint32_t v, int d)
{
    const char *h = "0123456789ABCDEF";
    while(d--) Xc(l, h[(v >> (d * 4)) & 0xFu]);
}
static void Xu(xl_t *l, uint32_t v)
{
    char t[11]; int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while(v);
    while(n) Xc(l, t[--n]);
}
static void Xcol(xl_t *l, int c){ while(l->n < c && l->n < GFX_LINE_MAX) Xc(l, ' '); }
static void XE(xl_t *l, int tone){ x_emit(tone, l->b); X0(l); }
static void xsay(int tone, const char *s){ xl_t l; X0(&l); Xs(&l, s); XE(&l, tone); }
static void Xmhz(xl_t *l, uint32_t khz)
{
    Xu(l, khz / 1000u);
    if(khz % 1000u){ Xc(l, '.'); Xu(l, (khz % 1000u) / 100u); }
    Xs(l, " MHz");
}
static void Xhz(xl_t *l, uint32_t x100)
{
    Xu(l, x100 / 100u); Xc(l, '.');
    Xc(l, (char)('0' + (x100 % 100u) / 10u)); Xc(l, (char)('0' + x100 % 10u));
    Xs(l, " Hz");
}

/* What the person can do about each refusal, in a line or two. */
static void refusal_help(const xm_state_t *s, const xm_plan_t *p)
{
    xl_t l; X0(&l);
    switch(p->why){
    case XM_ENOMON:
        xsay(GFX_T_TEXT, "  Is the HDMI cable in, and the monitor on? `gfx edid` shows");
        xsay(GFX_T_TEXT, "  what the DDC wires answered.");
        break;
    case XM_ENOMODE: case XM_EFIT:
        xsay(GFX_T_TEXT, "  `gfx edid` lists the monitor's modes. 0.9.2 needs one that is");
        xsay(GFX_T_TEXT, "  progressive, 340 MHz or under, and at least as big as the plane.");
        break;
    case XM_EDC:
        Xs(&l, "  DC_STATE_EN = "); Xh(&l, s->dc_state, 8); XE(&l, GFX_T_TEXT);
        break;
    case XM_EBUSY:
        Xs(&l, "  TRANSCONF B "); Xh(&l, s->b_conf, 8); Xs(&l, "  DDI_FUNC B ");
        Xh(&l, s->b_func, 8); Xs(&l, "  DDI_BUF B "); Xh(&l, s->ddi_b_buf, 8);
        XE(&l, GFX_T_TEXT);
        break;
    case XM_ENOPLL:
        Xs(&l, "  DPLL0 "); Xh(&l, s->dpll_en[0], 8); Xs(&l, "  DPLL1 ");
        Xh(&l, s->dpll_en[1], 8); Xs(&l, "  DPCLKA "); Xh(&l, s->dpclka, 8);
        XE(&l, GFX_T_TEXT);
        /* v0.9.2.1: the HP, 11 Nov -- a `gfx on` that stopped part-way
           leaves DPLL0 running, and the next one finds no free PLL. */
        xsay(GFX_T_TEXT, "  If an earlier `gfx on` stopped part-way, `gfx off` releases");
        xsay(GFX_T_TEXT, "  the PLL it left running; then `gfx on` again.");
        break;
    case XM_EPHY:
        Xs(&l, "  PHY_MISC B "); Xh(&l, s->phy_misc_b, 8); Xs(&l, "  PORT_COMP_DW0 B ");
        Xh(&l, s->comp_dw0_b, 8); XE(&l, GFX_T_TEXT);
        break;
    case XM_EMBUS:
        Xs(&l, "  MBUS_CTL "); Xh(&l, s->mbus_ctl, 8); XE(&l, GFX_T_TEXT);
        break;
    case XM_EDDB:
        Xs(&l, "  pipe A PLANE_BUF_CFG "); Xh(&l, s->a_buf, 8); Xs(&l, "  CUR_BUF_CFG ");
        Xh(&l, s->a_cur_buf, 8); XE(&l, GFX_T_TEXT);
        break;
    case XM_ETEMPLATE:
        Xs(&l, "  pipe A PLANE_CTL "); Xh(&l, s->a_ctl, 8); Xs(&l, "  STRIDE ");
        Xh(&l, s->a_stride, 8); XE(&l, GFX_T_TEXT);
        xsay(GFX_T_TEXT, "  0.9.2 mirrors a linear XRGB8888 plane, as the firmware leaves it.");
        break;
    case XM_ECOLOR:
        Xs(&l, "  GAMMA_MODE B "); Xh(&l, s->b_gamma, 8); Xs(&l, "  CSC_MODE B ");
        Xh(&l, s->b_csc, 8); XE(&l, GFX_T_TEXT);
        break;
    case XM_ENOWELL: case XM_ECORE:
        Xs(&l, "  HSW_PWR_WELL_CTL2 "); Xh(&l, s->pw_drv, 8); XE(&l, GFX_T_TEXT);
        break;
    case XM_EOFF:
        xsay(GFX_T_TEXT, "  Transcoder B, DDI B and the PLL behind it are already off.");
        break;
    default: break;
    }
}

static void op_line(int i, const xm_op_t *o)
{
    xl_t l; X0(&l);
    Xs(&l, "  "); if(i + 1 < 10) Xc(&l, ' '); if(i + 1 < 100) Xc(&l, ' ');
    Xu(&l, (uint32_t)i + 1u); Xc(&l, ' ');
    switch(o->op){
    case XM_OP_WR:   Xs(&l, "wr   "); Xh(&l, o->off, 6); Xs(&l, " = "); Xh(&l, o->val, 8); break;
    case XM_OP_RMW:
    case XM_OP_RMWG:
        Xs(&l, o->op == XM_OP_RMW ? "rmw  " : "grp  "); Xh(&l, o->off, 6);
        Xs(&l, " -"); Xh(&l, o->clr, 8); Xs(&l, " +"); Xh(&l, o->set, 8); break;
    case XM_OP_WSET: Xs(&l, "wait "); Xh(&l, o->off, 6); Xs(&l, " has "); Xh(&l, o->set, 8); break;
    default:         Xs(&l, "wait "); Xh(&l, o->off, 6); Xs(&l, " not "); Xh(&l, o->clr, 8); break;
    }
    Xcol(&l, 42); Xs(&l, o->what);
    XE(&l, GFX_T_DIM);
}

void xm_report_plan(const xm_state_t *s, const xm_plan_t *p, int full,
                    gfx_emit_t emit)
{
    xl_t l; int i;
    x_emit = emit; X0(&l);
    xsay(GFX_T_HEAD, p->on == 2 ? "Pipe B's background colour: the plan"
                   : p->on ? "The external monitor: the plan -- nothing is written yet"
                           : "Turning the external monitor off: the plan");
    if(p->why){
        Xs(&l, "  Refused: "); Xs(&l, xm_why_name(p->why)); Xc(&l, '.');
        XE(&l, GFX_T_BAD);
        refusal_help(s, p);
        xsay(GFX_T_TEXT, "  Nothing was written.");
        return;
    }
    if(p->on == 1){
        const edid_mode_t *m = &p->mode;
        uint32_t ht = (uint32_t)m->hactive + m->hblank, vt = (uint32_t)m->vactive + m->vblank;
        Xs(&l, "  mode   "); Xu(&l, m->hactive); Xc(&l, 'x'); Xu(&l, m->vactive);
        Xs(&l, " at "); Xhz(&l, m->refresh_x100); Xs(&l, ", "); Xmhz(&l, m->pclk_khz);
        Xs(&l, ", from the EDID"); XE(&l, GFX_T_GOOD);
        Xs(&l, "  total  "); Xu(&l, ht); Xc(&l, 'x'); Xu(&l, vt);
        Xs(&l, ", hsync "); Xu(&l, (uint32_t)m->hactive + m->hso); Xc(&l, '-');
        Xu(&l, (uint32_t)m->hactive + m->hso + m->hspw); Xc(&l, m->hpos ? '+' : '-');
        Xs(&l, ", vsync "); Xu(&l, (uint32_t)m->vactive + m->vso); Xc(&l, '-');
        Xu(&l, (uint32_t)m->vactive + m->vso + m->vspw); Xc(&l, m->vpos ? '+' : '-');
        XE(&l, GFX_T_TEXT);
        Xs(&l, "  DPLL"); Xu(&l, (uint32_t)p->dpll); Xs(&l, "  "); Xmhz(&l, p->port_khz);
        Xs(&l, "  cfg "); Xh(&l, p->cfgcr0, 8); Xc(&l, ' '); Xh(&l, p->cfgcr1, 8);
        Xs(&l, "  (port A stays on DPLL"); Xu(&l, s->dpclka & 3u); Xc(&l, ')');
        XE(&l, GFX_T_TEXT);
        Xs(&l, "  DDI B: DVI signalling, 4 lanes, vswing entry "); Xu(&l, (uint32_t)p->level);
        if(p->reversal) Xs(&l, ", lanes reversed");
        XE(&l, GFX_T_TEXT);
        Xs(&l, "  plane  "); Xu(&l, p->plane_w); Xc(&l, 'x'); Xu(&l, p->plane_h);
        Xs(&l, " at "); Xu(&l, p->pos_x); Xc(&l, ','); Xu(&l, p->pos_y);
        Xs(&l, ", unscaled: the panel's picture, GTT "); Xh(&l, s->a_surf, 8);
        XE(&l, GFX_T_TEXT);
        Xs(&l, "  buffer blocks "); Xu(&l, p->ddb_start); Xc(&l, '-'); Xu(&l, p->ddb_end);
        Xs(&l, " (slices 3-4), line time "); Xu(&l, p->linetime); Xs(&l, "/8 us");
        XE(&l, GFX_T_TEXT);
        Xs(&l, "  watermarks copied from pipe A");
        if(p->wm_dropped){ Xs(&l, ", "); Xu(&l, (uint32_t)p->wm_dropped); Xs(&l, " too big: off"); }
        XE(&l, GFX_T_TEXT);
        if(!(s->sdeisr & SDE_HPD_B))
            xsay(GFX_T_WARN, "  DDI B's hot-plug line is low, though the EDID answered.");
    }
    if(!full){
        Xs(&l, "  "); Xu(&l, (uint32_t)p->nops);
        Xs(&l, " operations; `gfx plan` lists every one.");
        XE(&l, GFX_T_TEXT);
        return;
    }
    Xu(&l, (uint32_t)p->nops); Xs(&l, " operations, in order (wr = write, rmw = read-modify-write,");
    XE(&l, GFX_T_HEAD);
    xsay(GFX_T_TEXT, "  grp = all four PHY lanes; -clear +set):");
    for(i = 0; i < p->nops; i++) op_line(i, &p->ops[i]);
    if(p->on == 2) return;
    xsay(GFX_T_TEXT, p->on ? "  Nothing outside pipe B, transcoder B, DDI B and PHY B changes,"
                           : "  Power wells and buffer slices are left on: harmless, and");
    xsay(GFX_T_TEXT, p->on ? "  except B's own bits of shared registers. Pipe A is not touched."
                           : "  turning them off is not B's decision alone.");
}

void xm_report_result(const xm_plan_t *p, const xm_result_t *res, gfx_emit_t emit)
{
    xl_t l; int i;
    x_emit = emit; X0(&l);
    if(res->why == XM_OK && p->on == 2){
        xsay(GFX_T_GOOD, "Pipe B's background colour is set. On the monitor:");
        xsay(GFX_T_TEXT, "  all that colour          -> the plane is not being shown");
        xsay(GFX_T_TEXT, "  colour, black box inside -> the plane is there, fetching black");
        xsay(GFX_T_TEXT, "  still black              -> pixels are not reaching the monitor");
        xsay(GFX_T_TEXT, "  the picture, colour round it -> it works; `gfx color off`");
        return;
    }
    if(res->why == XM_OK){
        Xs(&l, p->on ? "Lit. " : "Off. "); Xu(&l, res->writes);
        Xs(&l, " writes, every one read back as it should.");
        XE(&l, GFX_T_GOOD);
        if(p->on){
            uint32_t d = res->frm1 - res->frm0;
            Xs(&l, "  pipe B frame counter "); Xu(&l, res->frm0); Xs(&l, " -> ");
            Xu(&l, res->frm1); Xs(&l, " in 50 ms");
            Xs(&l, d >= 2u ? ": scanning out." : ": NOT MOVING.");
            XE(&l, d >= 2u ? GFX_T_GOOD : GFX_T_BAD);
            xsay(res->isr & PIPE_UNDERRUN ? GFX_T_WARN : GFX_T_TEXT,
                 res->isr & PIPE_UNDERRUN ? "  pipe B has underrun (its FIFO ran dry at least once)."
                                          : "  no underrun on pipe B.");
            xsay(GFX_T_TEXT, "  `gfx off` turns it off again. Nothing here survives a reboot.");
        }
    } else {
        Xs(&l, "Stopped: "); Xs(&l, xm_why_name(res->why)); Xc(&l, '.');
        XE(&l, GFX_T_BAD);
        if(res->failed_op >= 0 && res->failed_op < p->nops){
            Xs(&l, "  at step "); Xu(&l, (uint32_t)res->failed_op + 1u); Xs(&l, " of ");
            Xu(&l, (uint32_t)p->nops); Xs(&l, ", "); Xs(&l, p->ops[res->failed_op].what);
            XE(&l, GFX_T_BAD);
            Xs(&l, "  register "); Xh(&l, res->bad_off, 6);
            Xs(&l, res->why == XM_ETIMEOUT ? ": waited for " : ": wanted ");
            Xh(&l, res->bad_want, 8); Xs(&l, ", read "); Xh(&l, res->bad_got, 8);
            XE(&l, GFX_T_BAD);
        }
        Xs(&l, "  "); Xu(&l, res->writes); Xs(&l, " writes were made before it stopped.");
        XE(&l, GFX_T_TEXT);
        if(p->on == 1) xsay(GFX_T_TEXT, "  `gfx off` undoes what was done; then photograph this.");
    }
    if(res->nwlog){
        xsay(GFX_T_HEAD, "Every write (register=written/read back)");
        for(i = 0; i < res->nwlog; i++){
            if(i % 3 == 0) Xc(&l, ' ');
            Xc(&l, ' '); Xh(&l, res->wlog[i].off, 6); Xc(&l, '=');
            Xh(&l, res->wlog[i].val, 8); Xc(&l, '/'); Xh(&l, res->wlog[i].back, 8);
            if(i % 3 == 2 || i == res->nwlog - 1) XE(&l, GFX_T_DIM);
        }
    }
}

/* ── pipe B, live (v0.9.2.2) ─────────────────────────────────────────
   On the HP on 11 November `gfx on` reported every write read back, the
   frame counter moving and no underrun -- and the monitor, locked on
   to the signal, showed black. These are the registers that tell the
   cases apart. PLANE_SURFLIVE is the one that matters most: PLANE_SURF
   is what was ARMED, SURFLIVE is what the plane is actually scanning. */
static void Xreg(xl_t *l, const char *name, uint32_t v)
{
    int start = l->n;
    Xs(l, "  "); Xs(l, name); Xcol(l, start + 13);
    Xh(l, v, 8);
}

void xm_report_live(const gfx_dev_t *dev, gfx_emit_t emit)
{
    static const struct { const char *name; uint32_t off; } R[] = {
        { "TRANSCONF",  R_B_CONF },    { "DDI_FUNC",   R_B_FUNC },
        { "DDI_BUF",    R_DDI_BUF_B }, { "PIPESRC",    R_B_SRC },
        { "PLANE_CTL",  R_B_CTL },     { "PLANE_SURF", R_B_SURF },
        { "SURFLIVE",   R_B_SURFLIVE },{ "PLANE_POS",  R_B_POS },
        { "PLANE_SIZE", R_B_SIZE },    { "STRIDE",     R_B_STRIDE },
        { "COLOR_CTL",  R_B_COLOR },   { "BUF_CFG",    R_B_BUF },
        { "WM0",        R_B_WM(0) },   { "MBUS_DBOX",  R_B_DBOX },
        { "ARB_CTL",    R_B_ARB },     { "BOTTOM",     R_B_BOTTOM },
        { "GAMMA_MODE", R_B_GAMMA },   { "CSC_MODE",   R_B_CSC },
        { "DBUF_S3",    0x44300u },    { "DBUF_S4",    0x44304u },
        { "MBUS_CTL",   R_MBUS_CTL },  { "PIPE_ISR",   R_B_ISR },
        { "PIPE_IIR",   R_B_IIR },     { "DPCLKA",     R_DPCLKA },
    };
    xl_t l;
    uint32_t pw, dsl0, dsl1, f0, f1;
    unsigned i;
    x_emit = emit; X0(&l);
    xsay(GFX_T_HEAD, "Pipe B, live -- read only");
    if(!dev || dev->why != GFX_OK || !dev->mmio){
        xsay(GFX_T_BAD, "  No display engine was opened."); return;
    }
    pw = GFX_RD(dev->mmio, R_PWR_DRV);
    if(!(pw & PW_STATE(PW_1)) || !(pw & PW_STATE(PW_B))){
        xsay(GFX_T_WARN, "  PW_1 or PW_B is off: pipe B's registers cannot be read.");
        return;
    }
    dsl0 = GFX_RD(dev->mmio, R_B_DSL) & 0x1FFFu;
    f0 = GFX_RD(dev->mmio, R_B_FRM);
    XM_SLEEP_MS(20);
    dsl1 = GFX_RD(dev->mmio, R_B_DSL) & 0x1FFFu;
    f1 = GFX_RD(dev->mmio, R_B_FRM);
    Xs(&l, "  scanline "); Xu(&l, dsl0); Xs(&l, " -> "); Xu(&l, dsl1);
    Xs(&l, ", frame "); Xu(&l, f0); Xs(&l, " -> "); Xu(&l, f1); Xs(&l, " in 20 ms");
    Xs(&l, f1 != f0 ? ": running" : ": STOPPED");
    XE(&l, f1 != f0 ? GFX_T_GOOD : GFX_T_BAD);
    for(i = 0; i < sizeof R / sizeof R[0]; i++){
        Xreg(&l, R[i].name, GFX_RD(dev->mmio, R[i].off));
        if(i % 3 == 2 || i == sizeof R / sizeof R[0] - 1) XE(&l, GFX_T_TEXT);
        else Xs(&l, " ");
    }
    xsay(GFX_T_TEXT, "  SURFLIVE is what the plane scans now; PLANE_SURF only what was armed.");
}
