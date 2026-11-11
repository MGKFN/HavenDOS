#pragma once
#include "types.h"
#include "xedisp.h"
#include "edid.h"

/* ════════════════════════════════════════════════════════════════════
   include/xemode.h -- lighting the external monitor.  v0.9.2.

   THE FIRST RELEASE THAT CHANGES WHAT A SCREEN SHOWS.

   0.9.0 read the engine, 0.9.1 read the monitor. This one programs a
   whole display path on the HP's SECOND output, the HDMI socket:

       DPLL0 -> DDI B (combo PHY B) <- transcoder B <- pipe B <- plane 1

   and points pipe B's plane at the framebuffer the panel already
   shows, so the monitor MIRRORS the laptop: same 1024x768 picture,
   unscaled, in the middle of a 1920x1080 frame. Nothing about the
   panel's path (pipe A, transcoder A, DDI A, DPLL1) is touched -- not
   read-modify-written, not "refreshed", not at all.

   ── THE SEQUENCE IS i915's ──────────────────────────────────────────

   hsw_crtc_enable() for an HDMI output on a display-13 combo PHY, from
   the Linux i915 driver (MIT): power, then the PLL (combo_pll_enable),
   then the port clock (icl_ddi_combo_enable_clock) and the transcoder
   clock, then the pipe and transcoder registers, watermarks and the
   buffer allocation, then TRANS_DDI_FUNC_CTL, the transcoder, the PHY's
   signal levels (icl_combo_phy_set_signal_levels with the HDMI table),
   the lanes and DDI_BUF_CTL -- then the plane. Each step in
   drivers/xemode.c names the i915 function it comes from.

   Where i915 COMPUTES something this release has no model for -- the
   watermarks, the MBUS credits, the context latency -- the value is
   COPIED from pipe A, which the firmware programmed for a plane of
   exactly the size this one mirrors. That is a template, not a guess.

   ── WHAT MAKES IT SAFE TO TRY ───────────────────────────────────────

   1. A PLAN FIRST. `gfx plan` computes every write and prints it, and
      writes nothing. `gfx on` runs the same plan, after a y/N.
   2. REFUSALS BEFORE THE FIRST WRITE. Every precondition is checked on
      a fresh read; a refused plan has written nothing.
   3. ONE WRITE SEAM, AND AN ALLOW-LIST BEHIND IT. xm_wr() is the only
      place GFX_WR appears (tools/check_gfx_readonly.py), and it asks
      xm_allowed() first. Pipe B, transcoder B, DDI B and PHY B are
      allowed whole; a SHARED register is allowed only the bits that
      belong to B -- DPCLKA's port-B bits, CHICKEN_TRANS B's frame-start
      field, a power-well REQUEST bit that is only ever ADDED. Pipe A's
      registers are not in the list, so no bug elsewhere can reach them.
   4. A READ-BACK AFTER EVERY WRITE. The bits that should hold, must;
      the first that does not stops the sequence, on screen, by name.
   5. NEVER AT BOOT. Only `gfx on`, typed, lights anything.

   0.9.2 signals DVI on the HDMI cable (TRANS_DDI_MODE_SELECT_DVI): the
   same pixels, no infoframes, no audio. HDMI mode needs an AVI
   infoframe to be well-formed and comes once the picture is proved.
   ════════════════════════════════════════════════════════════════════ */

/* ── refusals ─────────────────────────────────────────────────────── */
#define XM_OK          0
#define XM_ENODEV      1   /* no display engine was opened              */
#define XM_ENOMON      2   /* no EDID came back from the monitor        */
#define XM_ENOMODE     3   /* the EDID has no mode 0.9.2 can drive      */
#define XM_EDC         4   /* DC5/DC6 is enabled: the DMC owns power    */
#define XM_ECORE       5   /* PW_1 is off                               */
#define XM_EBUSY       6   /* pipe B, transcoder B or DDI B already on  */
#define XM_ENOPLL      7   /* no combo DPLL is free                     */
#define XM_EPHY        8   /* combo PHY B was never initialised         */
#define XM_EMBUS       9   /* MBUS is joined: pipe A owns all of DBUF   */
#define XM_EDDB        10  /* pipe A's buffer reaches into B's slices   */
#define XM_ETEMPLATE   11  /* pipe A's plane is not one to copy         */
#define XM_EFIT        12  /* the plane does not fit inside the mode    */
#define XM_ECOLOR      13  /* pipe B's gamma or CSC is switched on      */
#define XM_ECLOCK      14  /* no PLL divider reaches that pixel clock   */
#define XM_ENOWELL     15  /* PW_2 or PW_B is off (0.9.2 will not wake) */
#define XM_EREFUSED    16  /* the allow-list refused a write            */
#define XM_EREADBACK   17  /* a write did not read back                 */
#define XM_ETIMEOUT    18  /* a status bit never came                   */
#define XM_EOFF        19  /* `gfx off`: pipe B is not lit              */
#define XM_EREASONS    20
const char *xm_why_name(int why);

/* ── what is read before anything is planned ─────────────────────── */
typedef struct {
    int       read;           /* 1 once xm_read ran on a working engine */
    int       pipe_b_powered; /* PW_B on: pipe B's registers are real   */
    uint32_t  pw_drv, pw_ddi_drv, dc_state, dssm, cdclk_ctl;
    uint32_t  dpll_en[2], dpll_cfgcr0[2], dpll_cfgcr1[2], dpclka;
    uint32_t  sdeisr;
    uint32_t  phy_misc_b, comp_dw0_b, cl_dw10_b;
    uint32_t  mbus_ctl, dbuf[4];
    /* pipe A, plane 1: the template. Never written. */
    uint32_t  a_ctl, a_stride, a_size, a_surf, a_offset, a_color;
    uint32_t  a_buf, a_cur_buf, a_wm[8], a_wm_trans, a_dbox, a_scl;
    /* B, as it is now */
    uint32_t  b_conf, b_func, b_ctl, b_gamma, b_csc, b_frm, b_isr;
    uint32_t  ddi_b_buf, ddi_b_clk;
} xm_state_t;

/* ── the plan: every operation, in order ─────────────────────────── */
#define XM_OP_WR      0   /* write val                                  */
#define XM_OP_RMW     1   /* read off, clear `clr`, set `set`, write    */
#define XM_OP_RMWG    2   /* read rb (a PHY lane), clear/set, write off
                             (the PHY's group register, all four lanes) */
#define XM_OP_WSET    3   /* wait until (off & set) == set               */
#define XM_OP_WCLR    4   /* wait until (off & clr) == 0                 */

typedef struct {
    uint8_t   op;
    uint32_t  off;
    uint32_t  val;            /* XM_OP_WR                               */
    uint32_t  clr, set;       /* RMW / RMWG / waits                     */
    uint32_t  rb;             /* where the read-back is taken, 0 = off  */
    uint32_t  hold;           /* bits that must read back as written    */
    const char *what;         /* a few words, for the screen            */
} xm_op_t;

#define XM_MAX_OPS 112
typedef struct {
    int       why;            /* XM_OK, or the refusal                  */
    int       on;             /* 1 lights, 0 turns off                  */
    edid_mode_t mode;
    int       dpll;           /* 0 or 1: which combo PLL                */
    uint32_t  cfgcr0, cfgcr1, port_khz;
    int       level, reversal;          /* HDMI vswing entry, lanes     */
    uint32_t  plane_w, plane_h, pos_x, pos_y;
    uint32_t  ddb_start, ddb_end;       /* blocks, end inclusive        */
    uint32_t  linetime;
    int       wm_dropped;               /* WM levels too big, disabled  */
    int       nops;
    xm_op_t   ops[XM_MAX_OPS];
} xm_plan_t;

/* What happened when a plan ran. */
#define XM_WLOG_MAX 128
typedef struct {
    int       why;
    int       done;           /* operations completed                   */
    int       failed_op;      /* index of the one that stopped it, -1   */
    uint32_t  writes, refused;
    uint32_t  bad_off, bad_want, bad_got;   /* the failing register     */
    uint32_t  frm0, frm1;     /* pipe B frame counter, 50 ms apart      */
    uint32_t  isr;            /* GEN8_DE_PIPE_ISR(B) afterwards         */
    int       nwlog;
    struct { uint32_t off, val, back; } wlog[XM_WLOG_MAX];
} xm_result_t;

/* ── the steps ─────────────────────────────────────────────────────── */

/* Read everything the plan needs, gated on its power well. Writes
   nothing. */
void xm_read(const gfx_dev_t *dev, xm_state_t *s);

/* The WRPLL dividers for an HDMI port clock (i915 icl_calc_wrpll,
   icl_calc_dpll_state, with WA #22010492432). 0 if no divider puts the
   DCO in range. Pure. */
int  xm_wrpll(uint32_t port_khz, uint32_t dssm, uint32_t *cfgcr0, uint32_t *cfgcr1);

/* Plans. Pure: they read only `s`, never the hardware. `child` is the
   VBT's HDMI connector, or 0. */
int  xm_plan_on (const xm_state_t *s, const edid_t *e, const gfx_child_t *child,
                 xm_plan_t *p);
int  xm_plan_off(const xm_state_t *s, xm_plan_t *p);

/* Is this write allowed? `old` is the register's value now. Pure,
   given the DPCLKA value it is passed. */
int  xm_allowed(uint32_t off, uint32_t old, uint32_t val, uint32_t dpclka,
                const xm_plan_t *p);

/* Run a plan through the one write seam. Stops at the first refusal,
   failed read-back or timeout. */
int  xm_apply(const gfx_dev_t *dev, const xm_plan_t *p, xm_result_t *res);

/* The screens. */
/* full = 0 leaves out the operation list (`gfx on` shows the summary
   before it asks; `gfx plan` shows everything). */
void xm_report_plan  (const xm_state_t *s, const xm_plan_t *p, int full,
                      gfx_emit_t emit);
void xm_report_result(const xm_plan_t *p, const xm_result_t *res, gfx_emit_t emit);
