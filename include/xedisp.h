#pragma once
#include "types.h"
#include "pci.h"

/* ════════════════════════════════════════════════════════════════════
   include/xedisp.h -- the Iris Xe display engine, READ ONLY.  v0.9.0.

   WHY THIS EXISTS

   Kyan wants HavenDOS on his external monitor. GRUB and HavenDOS both
   draw into the framebuffer the UEFI firmware lit, on the screen the
   firmware chose, and that is the laptop's panel. Moving to another
   screen means programming the display engine ourselves: a power
   well, a PLL, a DDI, a transcoder, a pipe and a plane.

   That is four releases, and this is the first one. IT WRITES NOTHING.
   Before a single register is changed, the machine has to say what
   the firmware already did -- which pipe drives the panel, which
   power wells are on, which PLL clocks it, which connector the HDMI
   socket is wired to -- because every later step is a copy of what
   the firmware did, aimed at a different output.

   ── THE PLATFORM ────────────────────────────────────────────────────

   Alder Lake-P, display version 13 ("XE_LPD"), as on the HP
   (8086:46A8). Four pipes, four transcoders, two combo-PHY ports
   (DDI A and B) and four Type-C ports (TC1-TC4). Every register
   offset and bit in drivers/xedisp.c was checked against the Linux i915
   driver (drivers/gpu/drm/i915/display/, MIT licensed), which is the
   only complete public statement of this hardware; the comment beside
   each one names the i915 symbol so the next person can check it too.

   ── WHAT "READ ONLY" MEANS HERE, MECHANICALLY ───────────────────────

   drivers/xedisp.c has a read seam (GFX_RD) and NO write seam. It does
   not write PCI config space, does not call pci_enable(), does not
   change a power state. tools/check_gfx_readonly.py fails the build
   if any of that appears in the file. 0.9.2 is the release that
   writes, and it will add a write seam with a read-back beside it.

   Reading a register whose power well is off is harmless but
   meaningless -- it returns zero and sets the engine's "unclaimed
   access" flag. So every read is gated on the well that powers it,
   and the flag (FPGA_DBG bit 31) is read before and after: if it goes
   from clear to set during an inspection, a gate is wrong, and the
   screen says so.
   ════════════════════════════════════════════════════════════════════ */

/* How much of BAR0 is display registers on display 13: the first
   2 MB (i915 intel_uncore_setup_mmio: mmio_size = 2 MB for an
   integrated part before Meteor Lake). The GTT is above it and is
   never touched. */
#define GFX_MMIO_SPAN   0x00200000u

/* Refusals, named. */
#define GFX_OK          0
#define GFX_ENODEV      1   /* no Intel display controller at 00:02.0  */
#define GFX_EUNKNOWN    2   /* Intel, but not a display-13 device ID   */
#define GFX_EMEMOFF     3   /* PCI memory decoding is off              */
#define GFX_ENOBAR      4   /* BAR0 was never assigned                 */
#define GFX_EMAP        5   /* BAR0 would not map                      */
#define GFX_EDEAD       6   /* every register reads all ones           */
#define GFX_EREASONS    7

const char *gfx_why_name(int why);

/* ── the four pipes, the ports ─────────────────────────────────────── */
#define GFX_NPIPES      4
/* Port index as i915 numbers them (enum port): A=0, B=1, C=2, and on
   display 12+ the Type-C ports are TC1=3 .. TC4=6. ADL-P has no C. */
#define GFX_NPORTS      7
#define GFX_PORT_A      0
#define GFX_PORT_B      1
#define GFX_PORT_TC1    3
#define GFX_PORT_NONE   (-1)

/* ── what the PCI side says ────────────────────────────────────────── */
typedef struct {
    int       why;            /* GFX_OK or a refusal                  */
    const pci_dev_t *pci;
    uint16_t  device;
    uint16_t  pcicmd;
    uint32_t  bar0_hi, bar0_lo;   /* registers + GTT                   */
    uint32_t  bar2_hi, bar2_lo;   /* the graphics aperture             */
    uint32_t  mmio;               /* where BAR0 is mapped, 0 = not     */
    uint32_t  asls;               /* OpRegion, physical, 0 = none      */
} gfx_dev_t;

/* ── every register the inspector reads, as read ───────────────────── */
typedef struct {
    int       powered;        /* the well this needs was on            */
    uint32_t  conf;           /* TRANSCONF   bit 31 enable, 30 state   */
    uint32_t  ctl, stride, pos, size, surf, offset;    /* plane 1      */
} gfx_pipe_t;

typedef struct {
    int       powered;
    uint32_t  func;           /* TRANS_DDI_FUNC_CTL                    */
    uint32_t  htotal, hblank, hsync, vtotal, vblank, vsync, src;
    uint32_t  link_m, link_n;     /* PIPE_LINK_M1/N1 -- DP only (0.9.1) */
} gfx_trans_t;

typedef struct {
    int       exists;         /* on this platform                      */
    int       powered;        /* its registers could be read           */
    uint32_t  buf;            /* DDI_BUF_CTL                           */
    uint32_t  clk_sel;        /* DDI_CLK_SEL, Type-C ports only        */
    uint32_t  tcss;           /* TCSS_DDI_STATUS, Type-C ports only    */
} gfx_port_t;

typedef struct {
    int       core;           /* PW_1 on: the display core is awake    */
    uint32_t  fpga_before, fpga_after;   /* FPGA_DBG, around the pass  */
    uint32_t  fuse;           /* SKL_FUSE_STATUS                       */
    uint32_t  pw[4];          /* HSW_PWR_WELL_CTL1..4: BIOS, driver,
                                 KVMR, debug                           */
    uint32_t  pw_aux[2], pw_ddi[2];      /* BIOS, driver               */
    uint32_t  dc_state;       /* DC_STATE_EN                           */
    uint32_t  dmc_htp, dmc_ssp;          /* nonzero: DMC firmware ran  */
    uint32_t  cdclk_ctl, de_pll, dssm, dfsm;
    uint32_t  dpll_en[2], dpll_cfgcr0[2], dpll_cfgcr1[2];
    uint32_t  tbt_en, tcpll_en[4];
    uint32_t  dpclka;         /* ICL_DPCLKA_CFGCR0                     */
    uint32_t  sdeisr, shotplug_ddi, shotplug_tc;
    uint32_t  de_hpd_isr, tc_hpd_ctl, tbt_hpd_ctl;
    uint32_t  fia_dpsp;       /* PORT_TX_DFLEXDPSP(FIA1)               */
    gfx_pipe_t  pipe[GFX_NPIPES];
    gfx_trans_t trans[GFX_NPIPES];
    gfx_port_t  port[GFX_NPORTS];
    uint32_t  reads;          /* how many registers were read          */
    uint32_t  out_of_span;    /* reads refused for being past 2 MB     */
} gfx_regs_t;

/* ── what the Video BIOS Table says is wired where ─────────────────── */
#define GFX_VBT_MAX_CHILD 8
typedef struct {
    uint16_t  type;           /* device_type, raw                      */
    uint8_t   dvo_port, ddc_pin, aux, flags1, flags2;
    int       port;           /* GFX_PORT_*, or GFX_PORT_NONE          */
    int       is_edp, is_dp, is_hdmi, is_dsi, lspcon;
    int       gmbus_pin;      /* the GMBUS pin ddc_pin means, 0 none   */
    int       aux_port;       /* the port whose AUX channel, or NONE   */
} gfx_child_t;

typedef struct {
    int       why;            /* 0 found, or a GFX_VBT_* reason        */
    uint32_t  where;          /* physical address of the VBT           */
    uint32_t  len;
    int       from_rvda;      /* 1: outside the OpRegion, via RVDA     */
    uint16_t  opregion_ver;   /* major << 8 | minor                    */
    uint16_t  bdb_version;
    uint8_t   child_size;
    int       nchild;
    gfx_child_t child[GFX_VBT_MAX_CHILD];
} gfx_vbt_t;

#define GFX_VBT_OK          0
#define GFX_VBT_NOASLS      1   /* the firmware gave no OpRegion       */
#define GFX_VBT_NOSIG       2   /* not "IntelGraphicsMem"              */
#define GFX_VBT_NOVBT       3   /* no "$VBT" where it should be        */
#define GFX_VBT_NOBDB       4   /* no "BIOS_DATA_BLOCK"                */
#define GFX_VBT_NODEFS      5   /* no block 2, so no child devices     */
#define GFX_VBT_BADSIZE     6   /* a size field points past the end    */
const char *gfx_vbt_why_name(int why);

/* ── the steps ─────────────────────────────────────────────────────── */

/* Find the controller and map its registers. Nothing is written --
   not even PCI config space. Safe to call twice. */
int  gfx_open(gfx_dev_t *dev);

/* Read every register the report uses, gating each on its power
   well. Pure apart from the reads. */
void gfx_read(const gfx_dev_t *dev, gfx_regs_t *r);

/* Parse a VBT out of a buffer. Pure; tests/test_gfx.c feeds it built
   tables and corrupt ones. */
int  gfx_vbt_parse(const uint8_t *vbt, uint32_t len, gfx_vbt_t *out);

/* Find the OpRegion through ASLS and the VBT in it, then parse it. */
int  gfx_vbt_find(const gfx_dev_t *dev, gfx_vbt_t *out);

/* ── the report ────────────────────────────────────────────────────── */
#define GFX_T_HEAD  0       /* a section heading                       */
#define GFX_T_TEXT  1
#define GFX_T_GOOD  2
#define GFX_T_WARN  3
#define GFX_T_BAD   4
#define GFX_T_DIM   5
typedef void (*gfx_emit_t)(int tone, const char *line);

/* Every line is 78 columns or fewer -- tests/test_gfx.c checks it. */
#define GFX_LINE_MAX 78

void gfx_report (const gfx_dev_t *dev, const gfx_regs_t *r,
                 const gfx_vbt_t *vbt, gfx_emit_t emit);
void gfx_report_regs(const gfx_dev_t *dev, const gfx_regs_t *r,
                     gfx_emit_t emit);
void gfx_report_vbt (const gfx_vbt_t *vbt, gfx_emit_t emit);

/* A combo PHY PLL's port clock in kHz, from its two config registers
   and SKL_DSSM's reference field (i915 icl_ddi_combo_pll_get_freq).
   0 if the dividers are not a valid encoding. */
uint32_t gfx_combo_pll_khz(uint32_t cfgcr0, uint32_t cfgcr1, uint32_t dssm);

/* The firmware framebuffer, from kernel/main.c, so the report can say
   whether the plane it finds is the one HavenDOS draws on. */
void kernel_fb_phys(uint32_t *hi, uint32_t *lo, uint32_t *w, uint32_t *h,
                    uint32_t *pitch);
