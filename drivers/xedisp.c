/* ════════════════════════════════════════════════════════════════════
   drivers/xedisp.c -- the Iris Xe display engine, READ ONLY.  v0.9.0.

   include/xedisp.h carries the reasoning: why HavenDOS needs its own
   display driver to reach an external monitor, why this first step
   writes nothing, and what "read only" means mechanically.

   ── WHERE EVERY NUMBER IN THIS FILE COMES FROM ─────────────────────

   The Linux i915 driver, drivers/gpu/drm/i915/display/ (MIT licence),
   read for display version 13 / Alder Lake-P. Each define below names
   the i915 symbol it mirrors. Intel's own PRMs say the same things at
   greater length; i915 is the version of them that has demonstrably
   run on this exact chip. NONE of this has run on real silicon from
   HavenDOS yet -- the HP's first `gfx` is the first time, and that is
   why the command prints what it read rather than only what it
   concluded.

   ── THE SEAMS ───────────────────────────────────────────────────────

   GFX_RD(base, off)   one 32-bit register read. There is no GFX_WR.
   GFX_PHYS(addr, len) a pointer to physical memory (the OpRegion and
                       the VBT), or 0 if that range cannot be reached.

   tests/test_gfx.c replaces both with a model of the engine whose
   power wells gate its registers the way the hardware's do.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/xedisp.h"
#include "../include/pci.h"
#include "../include/highfb.h"
#include "../include/edid.h"
#include "../include/xegmbus.h"

#ifndef GFX_RD
#define GFX_RD(base, off) \
    (*(volatile uint32_t *)(uintptr_t)((base) + (off)))
#endif
#ifndef GFX_PHYS
/* The low 4 GB is identity-mapped whether paging is on or off (see
   kernel/highfb.c), so a physical address below 4 GB is a pointer. */
#define GFX_PHYS(addr, len) gfx_phys_identity((addr), (len))
static const uint8_t *gfx_phys_identity(uint32_t addr, uint32_t len)
{
    if(!addr || !len) return 0;
    if(addr + len < addr) return 0;            /* wraps past 4 GB     */
    return (const uint8_t *)(uintptr_t)addr;
}
#endif

/* ── registers (offsets into BAR0) ───────────────────────────────── */
#define R_FPGA_DBG          0x42300u   /* FPGA_DBG                     */
#define   FPGA_NOCLAIM      (1u << 31) /* FPGA_DBG_RM_NOCLAIM          */
#define R_FUSE_STATUS       0x42000u   /* SKL_FUSE_STATUS              */
#define   FUSE_DOWNLOADED   (1u << 31) /* SKL_FUSE_DOWNLOAD_STATUS     */
#define R_PWR_WELL_CTL1     0x45400u   /* HSW_PWR_WELL_CTL1  (BIOS)    */
#define R_PWR_WELL_CTL2     0x45404u   /* HSW_PWR_WELL_CTL2  (driver)  */
#define R_PWR_WELL_CTL3     0x45408u   /* HSW_PWR_WELL_CTL3  (KVMR)    */
#define R_PWR_WELL_CTL4     0x4540Cu   /* HSW_PWR_WELL_CTL4  (debug)   */
#define R_PWR_AUX1          0x45440u   /* ICL_PWR_WELL_CTL_AUX1        */
#define R_PWR_AUX2          0x45444u   /* ICL_PWR_WELL_CTL_AUX2        */
#define R_PWR_DDI1          0x45450u   /* ICL_PWR_WELL_CTL_DDI1        */
#define R_PWR_DDI2          0x45454u   /* ICL_PWR_WELL_CTL_DDI2        */
#define R_DC_STATE_EN       0x45504u   /* DC_STATE_EN                  */
#define R_DMC_HTP           0x8F004u   /* DMC_HTP_SKL                  */
#define R_DMC_SSP           0x8F074u   /* DMC_SSP_BASE                 */
#define R_CDCLK_CTL         0x46000u   /* CDCLK_CTL                    */
#define R_DE_PLL_ENABLE     0x46070u   /* BXT_DE_PLL_ENABLE            */
#define R_DSSM              0x51004u   /* SKL_DSSM                     */
#define R_DFSM              0x51000u   /* SKL_DFSM                     */
#define R_DPLL_ENABLE(n)    (0x46010u + 4u * (n))   /* ICL_DPLL_ENABLE */
#define R_DPLL_CFGCR0(n)    (0x164284u + 8u * (n))  /* TGL_DPLL_CFGCR0 */
#define R_DPLL_CFGCR1(n)    (0x164288u + 8u * (n))  /* TGL_DPLL_CFGCR1 */
#define R_TBT_PLL_ENABLE    0x46020u   /* TBT_PLL_ENABLE               */
#define R_TCPLL_ENABLE(tc)  (0x46038u + 8u * (tc))  /* ADLP_PORTTC_PLL_ENABLE */
#define R_DPCLKA_CFGCR0     0x164280u  /* ICL_DPCLKA_CFGCR0            */
#define R_DDI_CLK_SEL(p)    (0x46100u + 4u * (p))   /* DDI_CLK_SEL     */
#define R_SDEISR            0xC4000u   /* SDEISR                       */
#define R_SHOTPLUG_DDI      0xC4030u   /* SHOTPLUG_CTL_DDI             */
#define R_SHOTPLUG_TC       0xC4034u   /* SHOTPLUG_CTL_TC              */
#define R_DE_HPD_ISR        0x44470u   /* GEN11_DE_HPD_ISR             */
#define R_TBT_HPD_CTL       0x44030u   /* GEN11_TBT_HOTPLUG_CTL        */
#define R_TC_HPD_CTL        0x44038u   /* GEN11_TC_HOTPLUG_CTL         */
#define R_FIA1_DPSP         0x1638A0u  /* PORT_TX_DFLEXDPSP(FIA1)      */
#define R_TCSS_STATUS(tc)   (0x161500u + 4u * (tc)) /* TCSS_DDI_STATUS */
#define R_DDI_BUF_CTL(p)    (0x64000u + 0x100u * (p)) /* DDI_BUF_CTL   */

/* Per pipe (PIPE_A_OFFSET 0x70000, 0x1000 apart). */
#define R_TRANSCONF(p)      (0x70008u + 0x1000u * (p)) /* TRANSCONF    */
#define R_PLANE_CTL(p)      (0x70180u + 0x1000u * (p)) /* _PLANE_CTL_1_A */
#define R_PLANE_STRIDE(p)   (0x70188u + 0x1000u * (p))
#define R_PLANE_POS(p)      (0x7018Cu + 0x1000u * (p))
#define R_PLANE_SIZE(p)     (0x70190u + 0x1000u * (p))
#define R_PLANE_SURF(p)     (0x7019Cu + 0x1000u * (p))
#define R_PLANE_OFFSET(p)   (0x701A4u + 0x1000u * (p))
/* Per transcoder (TRANSCODER_A_OFFSET 0x60000, 0x1000 apart). */
#define R_HTOTAL(t)         (0x60000u + 0x1000u * (t))
#define R_HBLANK(t)         (0x60004u + 0x1000u * (t))
#define R_HSYNC(t)          (0x60008u + 0x1000u * (t))
#define R_VTOTAL(t)         (0x6000Cu + 0x1000u * (t))
#define R_VBLANK(t)         (0x60010u + 0x1000u * (t))
#define R_VSYNC(t)          (0x60014u + 0x1000u * (t))
#define R_PIPESRC(t)        (0x6001Cu + 0x1000u * (t))
#define R_DDI_FUNC(t)       (0x60400u + 0x1000u * (t)) /* TRANS_DDI_FUNC_CTL */
#define R_LINK_M1(t)        (0x60040u + 0x1000u * (t)) /* _PIPEA_LINK_M1 */
#define R_LINK_N1(t)        (0x60044u + 0x1000u * (t)) /* _PIPEA_LINK_N1 */

/* Power well indices in HSW_PWR_WELL_CTL (two bits each: state at
   2*idx, request at 2*idx+1). i915 intel_display_power_map.c,
   xelpd_power_wells: */
#define PW_1    0       /* ICL_PW_CTL_IDX_PW_1 -- transcoder A, DDI A/B */
#define PW_2    1       /* ICL_PW_CTL_IDX_PW_2 -- Type-C, under B/C/D  */
#define PW_A    5       /* XELPD_PW_CTL_IDX_PW_A -- pipe A             */
#define PW_B    6       /* XELPD_PW_CTL_IDX_PW_B -- pipe/trans B       */
#define PW_C    7
#define PW_D    8
#define PW_STATE(idx)   (1u << ((idx) * 2))
#define PW_REQ(idx)     (2u << ((idx) * 2))

/* ── device IDs that are display version 13 ──────────────────────────
   i915 include/drm/intel/pciids.h: INTEL_ADLP_IDS, INTEL_ADLN_IDS,
   INTEL_RPLU_IDS, INTEL_RPLP_IDS -- all xe_lpd_display. Anything else
   is refused: these offsets are display 13's and on another generation
   a register here can mean something else entirely. */
static const uint16_t k_ids[] = {
    0x46A0, 0x46A1, 0x46A2, 0x46A3, 0x46A6, 0x46A8, 0x46AA, 0x462A,
    0x4626, 0x4628, 0x46B0, 0x46B1, 0x46B2, 0x46B3, 0x46C0, 0x46C1,
    0x46C2, 0x46C3,                                         /* ADL-P */
    0x46D0, 0x46D1, 0x46D2, 0x46D3, 0x46D4,                 /* ADL-N */
    0xA721, 0xA7A1, 0xA7A9, 0xA7AC, 0xA7AD,                 /* RPL-U */
    0xA720, 0xA7A0, 0xA7A8, 0xA7AA, 0xA7AB,                 /* RPL-P */
};

static int known_id(uint16_t id)
{
    uint32_t i;
    for(i = 0; i < sizeof k_ids / sizeof k_ids[0]; i++)
        if(k_ids[i] == id) return 1;
    return 0;
}

const char *gfx_why_name(int why)
{
    switch(why){
    case GFX_OK:       return "ok";
    case GFX_ENODEV:   return "no Intel display controller at 00:02.0";
    case GFX_EUNKNOWN: return "an Intel display, but not display version 13";
    case GFX_EMEMOFF:  return "PCI memory decoding is off for it";
    case GFX_ENOBAR:   return "its register BAR was never assigned";
    case GFX_EMAP:     return "its register BAR would not map";
    case GFX_EDEAD:    return "every register reads FFFFFFFF";
    default:           return "unknown";
    }
}

/* ── opening ───────────────────────────────────────────────────────── */

/* The mapping is made ONCE. kernel/highfb.c has eight windows for the
   life of the machine and the framebuffer, the xHCI and the I2C
   controllers already hold some; a command that mapped afresh every
   time it ran would run the machine out of them on the fifth `gfx`. */
static uint32_t g_map_hi = 0, g_map_lo = 0, g_map_virt = 0;

static const pci_dev_t *find_igd(void)
{
    int i, n = pci_count();
    for(i = 0; i < n; i++){
        const pci_dev_t *d = pci_get(i);
        if(!d) continue;
        if(d->vendor == 0x8086 && d->class == 0x03 &&
           d->bus == 0 && d->dev == 2 && d->fn == 0)
            return d;
    }
    return 0;
}

int gfx_open(gfx_dev_t *dev)
{
    const pci_dev_t *d;
    uint32_t hi = 0, fuse, pw;

    memset(dev, 0, sizeof *dev);
    d = find_igd();
    if(!d){ dev->why = GFX_ENODEV; return dev->why; }
    dev->pci    = d;
    dev->device = d->device;
    /* READ config space. Nothing in this file writes it. */
    dev->pcicmd = pci_cfg_read16(d->bus, d->dev, d->fn, 0x04);
    dev->asls   = pci_cfg_read32(d->bus, d->dev, d->fn, 0xFC);
    dev->bar0_lo = pci_bar_mem64(d, 0, &hi); dev->bar0_hi = hi;
    hi = 0;
    dev->bar2_lo = pci_bar_mem64(d, 2, &hi); dev->bar2_hi = hi;

    if(!known_id(d->device)){ dev->why = GFX_EUNKNOWN; return dev->why; }
    /* Bit 1 is memory space. The firmware is scanning the panel out of
       this device right now, so it is on; if it is not, turning it on
       is a write and this release does not make one. */
    if(!(dev->pcicmd & 0x0002u)){ dev->why = GFX_EMEMOFF; return dev->why; }
    if(!dev->bar0_lo && !dev->bar0_hi){ dev->why = GFX_ENOBAR; return dev->why; }

    if(g_map_virt && g_map_hi == dev->bar0_hi && g_map_lo == dev->bar0_lo){
        dev->mmio = g_map_virt;
    } else if(dev->bar0_hi){
        uint32_t virt = 0;
        /* Cache-DISABLED: a register read served from a cache is a
           register that never changes, which is the one lie an
           inspector cannot afford. highfb_map_io() returns success
           and writes the address through its last argument. */
        if(!highfb_map_io(dev->bar0_hi, dev->bar0_lo, GFX_MMIO_SPAN, &virt)
           || !virt){
            dev->why = GFX_EMAP; return dev->why;
        }
        g_map_hi = dev->bar0_hi; g_map_lo = dev->bar0_lo; g_map_virt = virt;
        dev->mmio = virt;
    } else {
        dev->mmio = dev->bar0_lo;
    }

    /* A function in D3, or a BAR pointing at nothing, reads all ones.
       Two registers, so one that legitimately reads FFFFFFFF cannot
       make the whole engine look dead. */
    fuse = GFX_RD(dev->mmio, R_FUSE_STATUS);
    pw   = GFX_RD(dev->mmio, R_PWR_WELL_CTL2);
    if(fuse == 0xFFFFFFFFu && pw == 0xFFFFFFFFu){
        dev->why = GFX_EDEAD; return dev->why;
    }
    dev->why = GFX_OK;
    return GFX_OK;
}

/* ── reading ───────────────────────────────────────────────────────── */

/* Every read goes through here: it refuses an offset outside the
   register span (the GTT is above it, and so is somebody else's
   memory if the span is wrong) and it keeps the log `gfx regs`
   prints. */
#define GFX_LOG_MAX 192
static struct { uint32_t off, val; } g_log[GFX_LOG_MAX];
static int g_nlog = 0;

static uint32_t rd(const gfx_dev_t *dev, gfx_regs_t *r, uint32_t off)
{
    uint32_t v;
    if(off >= GFX_MMIO_SPAN || (off & 3u)){ r->out_of_span++; return 0; }
    v = GFX_RD(dev->mmio, off);
    r->reads++;
    if(g_nlog < GFX_LOG_MAX){ g_log[g_nlog].off = off; g_log[g_nlog].val = v; g_nlog++; }
    return v;
}

static int well_on(const gfx_regs_t *r, int idx)
{
    /* The state bits are the hardware's, mirrored in every requester
       register; i915 hsw_power_well_enabled() reads the driver one. */
    return (r->pw[1] & PW_STATE(idx)) != 0;
}

/* Which well each pipe's planes and each transcoder live in, on
   display 13 (xelpd_power_wells): transcoder A is in PW_1 with the
   core; pipe A's planes in PW_A; B, C, D each in their own well,
   which itself sits under PW_2. */
static int pipe_well(int p)  { return PW_A + p; }
static int trans_on(const gfx_regs_t *r, int t)
{
    return t == 0 ? r->core : well_on(r, PW_A + t);
}

void gfx_read(const gfx_dev_t *dev, gfx_regs_t *r)
{
    int p, t, i;

    memset(r, 0, sizeof *r);
    g_nlog = 0;
    if(dev->why != GFX_OK || !dev->mmio) return;

    /* The unclaimed-access flag FIRST, so a set bit afterwards can be
       told apart from one the firmware left. */
    r->fpga_before = rd(dev, r, R_FPGA_DBG);

    /* Power-well 0 registers: readable whatever else is asleep. */
    r->fuse     = rd(dev, r, R_FUSE_STATUS);
    r->pw[0]    = rd(dev, r, R_PWR_WELL_CTL1);
    r->pw[1]    = rd(dev, r, R_PWR_WELL_CTL2);
    r->pw[2]    = rd(dev, r, R_PWR_WELL_CTL3);
    r->pw[3]    = rd(dev, r, R_PWR_WELL_CTL4);
    r->pw_aux[0]= rd(dev, r, R_PWR_AUX1);
    r->pw_aux[1]= rd(dev, r, R_PWR_AUX2);
    r->pw_ddi[0]= rd(dev, r, R_PWR_DDI1);
    r->pw_ddi[1]= rd(dev, r, R_PWR_DDI2);
    r->dc_state = rd(dev, r, R_DC_STATE_EN);
    r->dmc_htp  = rd(dev, r, R_DMC_HTP);
    r->dmc_ssp  = rd(dev, r, R_DMC_SSP);
    r->sdeisr       = rd(dev, r, R_SDEISR);       /* south display: PCH */
    r->shotplug_ddi = rd(dev, r, R_SHOTPLUG_DDI);
    r->shotplug_tc  = rd(dev, r, R_SHOTPLUG_TC);

    r->core = well_on(r, PW_1);
    if(r->core){
        /* The display core: clocks, PLLs, transcoder A, DDI A and B. */
        r->cdclk_ctl = rd(dev, r, R_CDCLK_CTL);
        r->de_pll    = rd(dev, r, R_DE_PLL_ENABLE);
        r->dssm      = rd(dev, r, R_DSSM);
        r->dfsm      = rd(dev, r, R_DFSM);
        for(i = 0; i < 2; i++){
            r->dpll_en[i]    = rd(dev, r, R_DPLL_ENABLE(i));
            r->dpll_cfgcr0[i]= rd(dev, r, R_DPLL_CFGCR0(i));
            r->dpll_cfgcr1[i]= rd(dev, r, R_DPLL_CFGCR1(i));
        }
        r->tbt_en = rd(dev, r, R_TBT_PLL_ENABLE);
        for(i = 0; i < 4; i++) r->tcpll_en[i] = rd(dev, r, R_TCPLL_ENABLE(i));
        r->dpclka     = rd(dev, r, R_DPCLKA_CFGCR0);
        r->de_hpd_isr = rd(dev, r, R_DE_HPD_ISR);
        r->tbt_hpd_ctl= rd(dev, r, R_TBT_HPD_CTL);
        r->tc_hpd_ctl = rd(dev, r, R_TC_HPD_CTL);
    }

    /* Pipes: TRANSCONF follows the transcoder's well, the plane
       follows the pipe's. */
    for(p = 0; p < GFX_NPIPES; p++){
        gfx_pipe_t *pp = &r->pipe[p];
        if(r->core && trans_on(r, p))
            pp->conf = rd(dev, r, R_TRANSCONF(p));
        if(!r->core || !well_on(r, pipe_well(p))) continue;
        pp->powered = 1;
        pp->ctl    = rd(dev, r, R_PLANE_CTL(p));
        pp->stride = rd(dev, r, R_PLANE_STRIDE(p));
        pp->pos    = rd(dev, r, R_PLANE_POS(p));
        pp->size   = rd(dev, r, R_PLANE_SIZE(p));
        pp->surf   = rd(dev, r, R_PLANE_SURF(p));
        pp->offset = rd(dev, r, R_PLANE_OFFSET(p));
    }

    for(t = 0; t < GFX_NPIPES; t++){
        gfx_trans_t *tt = &r->trans[t];
        if(!r->core || !trans_on(r, t)) continue;
        tt->powered = 1;
        tt->func   = rd(dev, r, R_DDI_FUNC(t));
        tt->htotal = rd(dev, r, R_HTOTAL(t));
        tt->hblank = rd(dev, r, R_HBLANK(t));
        tt->hsync  = rd(dev, r, R_HSYNC(t));
        tt->vtotal = rd(dev, r, R_VTOTAL(t));
        tt->vblank = rd(dev, r, R_VBLANK(t));
        tt->vsync  = rd(dev, r, R_VSYNC(t));
        /* 0.9.1: the DP link M/N. With the port clock they give the
           pixel clock, and so the refresh rate the panel runs at. */
        tt->link_m = rd(dev, r, R_LINK_M1(t));
        tt->link_n = rd(dev, r, R_LINK_N1(t));
        /* PIPESRC is a PIPE register at a transcoder address. */
        if(well_on(r, pipe_well(t)))
            tt->src = rd(dev, r, R_PIPESRC(t));
    }

    /* Ports. ADL-P has A, B and TC1-TC4; C does not exist. */
    for(i = 0; i < GFX_NPORTS; i++){
        gfx_port_t *pt = &r->port[i];
        pt->exists = (i == GFX_PORT_A || i == GFX_PORT_B || i >= GFX_PORT_TC1);
        if(!pt->exists || !r->core) continue;
        if(i < GFX_PORT_TC1){
            pt->powered = 1;             /* DDI A and B live in PW_1 */
            pt->buf = rd(dev, r, R_DDI_BUF_CTL(i));
        } else {
            pt->clk_sel = rd(dev, r, R_DDI_CLK_SEL(i));
            if(!well_on(r, PW_2)) continue;   /* Type-C lanes: PW_2 */
            pt->powered = 1;
            pt->buf  = rd(dev, r, R_DDI_BUF_CTL(i));
            pt->tcss = rd(dev, r, R_TCSS_STATUS(i - GFX_PORT_TC1));
        }
    }
    if(r->core && well_on(r, PW_2))
        r->fia_dpsp = rd(dev, r, R_FIA1_DPSP);

    /* And the flag again, LAST. */
    r->fpga_after = rd(dev, r, R_FPGA_DBG);
}

/* ── the VBT ──────────────────────────────────────────────────────── */

const char *gfx_vbt_why_name(int why)
{
    switch(why){
    case GFX_VBT_OK:      return "ok";
    case GFX_VBT_NOASLS:  return "the firmware gave no OpRegion (ASLS is 0)";
    case GFX_VBT_NOSIG:   return "ASLS does not point at an OpRegion";
    case GFX_VBT_NOVBT:   return "no $VBT where the OpRegion says";
    case GFX_VBT_NOBDB:   return "the VBT has no BIOS_DATA_BLOCK";
    case GFX_VBT_NODEFS:  return "the VBT lists no child devices";
    case GFX_VBT_BADSIZE: return "a size in the VBT points past its end";
    default:              return "unknown";
    }
}

static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* intel_vbt_defs.h DEVICE_TYPE_* bits. */
#define DT_INTERNAL     (1u << 12)
#define DT_NOT_HDMI     (1u << 11)
#define DT_MIPI         (1u << 10)
#define DT_TMDS         (1u << 4)
#define DT_DP           (1u << 2)

/* xelpd_port_mapping in intel_bios.c: display 13 renumbers the Type-C
   ports in the VBT as F..I. */
static int dvo_to_port(uint8_t dvo)
{
    switch(dvo){
    case 0:  case 10: return GFX_PORT_A;         /* HDMIA, DPA        */
    case 1:  case 7:  return GFX_PORT_B;         /* HDMIB, DPB        */
    case 2:  case 8:  return 2;                  /* HDMIC, DPC (none) */
    case 14: case 13: return GFX_PORT_TC1;       /* HDMIF, DPF        */
    case 16: case 15: return GFX_PORT_TC1 + 1;   /* HDMIG, DPG        */
    case 18: case 17: return GFX_PORT_TC1 + 2;   /* HDMIH, DPH        */
    case 20: case 19: return GFX_PORT_TC1 + 3;   /* HDMII, DPI        */
    case 21:          return GFX_PORT_A;         /* MIPIA: DSI on A   */
    default:          return GFX_PORT_NONE;
    }
}

/* adlp_ddc_pin_map: VBT ddc_pin -> GMBUS pin. */
static int ddc_to_gmbus(uint8_t ddc)
{
    switch(ddc){
    case 1: return 1;  case 2: return 2;           /* DDI A, DDI B   */
    case 3: return 9;  case 4: return 10;          /* TC1, TC2       */
    case 5: return 11; case 6: return 12;          /* TC3, TC4       */
    default: return 0;
    }
}

/* adlp_aux_ch_map: VBT aux_channel -> which port's AUX. */
static int aux_to_port(uint8_t aux)
{
    switch(aux){
    case 0x40: return GFX_PORT_A;
    case 0x10: return GFX_PORT_B;
    case 0x60: return GFX_PORT_TC1;
    case 0x70: return GFX_PORT_TC1 + 1;
    case 0x80: return GFX_PORT_TC1 + 2;
    case 0x90: return GFX_PORT_TC1 + 3;
    default:   return GFX_PORT_NONE;
    }
}

/* The child fields this reads, and their offsets in
   struct child_device_config (intel_vbt_defs.h). Everything past
   CHILD_MIN is ignored, so a newer, longer child still parses. */
#define CH_TYPE      2
#define CH_DVO_PORT  16
#define CH_DDC_PIN   19
#define CH_LEVEL     7      /* hdmi_level_shifter_value (bits 4:0), 158+ */
#define CH_FLAGS1    23     /* efp_routed, lane_reversal, lspcon(2) ... */
#define CH_FLAGS2    24     /* hdmi_support(0), dp_support(1) ...       */
#define CH_AUX       25
#define CHILD_MIN    27

int gfx_vbt_parse(const uint8_t *vbt, uint32_t len, gfx_vbt_t *out)
{
    uint32_t vbt_size, bdb_off, bdb_size, hdr, idx, defs = 0, defs_len = 0;
    const uint8_t *bdb;
    int i, n;

    out->nchild = 0; out->bdb_version = 0; out->child_size = 0;
    if(!vbt || len < 0x30u || memcmp(vbt, "$VBT", 4) != 0)
        return out->why = GFX_VBT_NOVBT;

    vbt_size = le16(vbt + 24);                  /* struct vbt_header */
    bdb_off  = le32(vbt + 28);
    if(vbt_size > len || bdb_off >= vbt_size || bdb_off + 22u > vbt_size)
        return out->why = GFX_VBT_BADSIZE;

    bdb = vbt + bdb_off;
    if(memcmp(bdb, "BIOS_DATA_BLOCK", 15) != 0)
        return out->why = GFX_VBT_NOBDB;
    out->bdb_version = le16(bdb + 16);
    hdr      = le16(bdb + 18);
    bdb_size = le16(bdb + 20);
    if(bdb_size > vbt_size - bdb_off || hdr < 22u || hdr > bdb_size)
        return out->why = GFX_VBT_BADSIZE;

    /* Walk the blocks: id (1 byte), size (2), data. intel_bios.c
       find_raw_section(), including its one exception -- the MIPI
       sequence block (53) from version 3 on carries a 32-bit size
       after its header, and reading the 16-bit one walks into its
       middle. */
    idx = hdr;
    while(idx + 3u < bdb_size){
        uint8_t  id = bdb[idx];
        uint32_t sz = le16(bdb + idx + 1);
        if(id == 53 && idx + 8u <= bdb_size && bdb[idx + 3] >= 3)
            sz = le32(bdb + idx + 4);
        idx += 3u;
        if(sz > bdb_size - idx) return out->why = GFX_VBT_BADSIZE;
        if(id == 2){ defs = idx; defs_len = sz; break; }
        idx += sz;
    }
    if(!defs) return out->why = GFX_VBT_NODEFS;

    /* struct bdb_general_definitions: five bytes, then the children. */
    if(defs_len < 5u) return out->why = GFX_VBT_NODEFS;
    out->child_size = bdb[defs + 4];
    if(out->child_size < CHILD_MIN) return out->why = GFX_VBT_BADSIZE;

    n = (int)((defs_len - 5u) / out->child_size);
    for(i = 0; i < n && out->nchild < GFX_VBT_MAX_CHILD; i++){
        const uint8_t *c = bdb + defs + 5u + (uint32_t)i * out->child_size;
        gfx_child_t *k = &out->child[out->nchild];
        uint16_t type = le16(c + CH_TYPE);
        if(!type) continue;                     /* an empty slot   */
        memset(k, 0, sizeof *k);
        k->type     = type;
        k->dvo_port = c[CH_DVO_PORT];
        k->ddc_pin  = c[CH_DDC_PIN];
        k->flags1   = c[CH_FLAGS1];
        k->flags2   = c[CH_FLAGS2];
        k->aux      = c[CH_AUX];
        k->port     = dvo_to_port(k->dvo_port);
        /* intel_bios_encoder_supports_*(): */
        k->is_dsi  = (type & DT_MIPI) != 0;
        k->is_dp   = (type & DT_DP) != 0;
        k->is_hdmi = (type & DT_TMDS) && !(type & DT_NOT_HDMI);
        k->is_edp  = k->is_dp && (type & DT_INTERNAL);
        k->lspcon  = (out->bdb_version >= 192) && (k->flags1 & 0x04u);
        /* 0.9.2: the two the HDMI output needs (intel_bios.c
           intel_bios_hdmi_level_shift, intel_bios_encoder_lane_reversal). */
        k->hdmi_level = (out->bdb_version >= 158) ? (int)(c[CH_LEVEL] & 0x1Fu) : -1;
        k->lane_reversal = (out->bdb_version >= 184) && (k->flags1 & 0x02u);
        k->gmbus_pin = ddc_to_gmbus(k->ddc_pin);
        k->aux_port  = aux_to_port(k->aux);
        out->nchild++;
    }
    return out->why = GFX_VBT_OK;
}

/* The OpRegion (intel_opregion.c): 8 KB at ASLS; signature at 0,
   version at 0x14..0x17, mailboxes at 0x58. Mailbox 3 (ASLE) is at
   0x300 and from version 2.0 holds RVDA/RVDS -- where a VBT too big
   for mailbox 4 has been put instead. From 2.1 RVDA is relative to
   the OpRegion. Otherwise the VBT is mailbox 4, at 0x400. */
#define OPR_SIZE      0x2000u
#define OPR_VBT_OFF   0x400u
#define OPR_ASLE_EXT  0x1C00u     /* OPREGION_ASLE_EXT_OFFSET          */
#define OPR_RVDA      0x3BAu      /* 0x300 + offsetof(asle, rvda)      */
#define OPR_RVDS      0x3C2u
#define MBOX_ASLE     (1u << 2)
#define MBOX_ASLE_EXT (1u << 4)
#define VBT_MAX       0x10000u    /* a sanity bound, not a spec value  */

int gfx_vbt_find(const gfx_dev_t *dev, gfx_vbt_t *out)
{
    const uint8_t *opr, *v;
    uint32_t mboxes, len;
    uint8_t  major, minor;

    memset(out, 0, sizeof *out);
    if(!dev->asls) return out->why = GFX_VBT_NOASLS;
    opr = GFX_PHYS(dev->asls, OPR_SIZE);
    if(!opr || memcmp(opr, "IntelGraphicsMem", 16) != 0)
        return out->why = GFX_VBT_NOSIG;
    minor  = opr[0x16];
    major  = opr[0x17];
    out->opregion_ver = (uint16_t)((major << 8) | minor);
    mboxes = le32(opr + 0x58);

    if(major >= 2 && (mboxes & MBOX_ASLE)){
        uint32_t rv_lo = le32(opr + OPR_RVDA), rv_hi = le32(opr + OPR_RVDA + 4);
        uint32_t rvds  = le32(opr + OPR_RVDS);
        /* !rv_hi: a VBT above 4 GB is not reachable from here, and
           taking the low half would parse whatever lives there. */
        if(rv_lo && !rv_hi && rvds && rvds <= VBT_MAX){
            int      rel = (major > 2 || minor >= 1);
            uint32_t at  = rel ? dev->asls + rv_lo : rv_lo;
            /* Relative and wrapping past 4 GB, or relative and
               pointing back inside the OpRegion: i915 warns on the
               second; both are a table that is lying. */
            if(!rel || (at > rv_lo && rv_lo >= OPR_SIZE)){
                v = GFX_PHYS(at, rvds);
                if(v && gfx_vbt_parse(v, rvds, out) == GFX_VBT_OK){
                    out->where = at; out->len = rvds; out->from_rvda = 1;
                    return GFX_VBT_OK;
                }
            }
        }
    }
    len = ((mboxes & MBOX_ASLE_EXT) ? OPR_ASLE_EXT : OPR_SIZE) - OPR_VBT_OFF;
    v = opr + OPR_VBT_OFF;
    out->where = dev->asls + OPR_VBT_OFF; out->len = len; out->from_rvda = 0;
    return gfx_vbt_parse(v, len, out);
}

/* ── the report ───────────────────────────────────────────────────── */

/* A line, built in a fixed buffer. A line that would run past
   GFX_LINE_MAX is cut there and counted: tests/test_gfx.c requires the
   count to stay zero, because a cut line on this screen is a register
   value with its last digits missing. */
typedef struct { char b[GFX_LINE_MAX + 1]; int n; } ln_t;
static uint32_t g_cut = 0;
static gfx_emit_t g_emit;

static void L0(ln_t *l){ l->n = 0; l->b[0] = 0; }
static void Lc(ln_t *l, char c)
{
    if(l->n >= GFX_LINE_MAX){ g_cut++; return; }
    l->b[l->n++] = c; l->b[l->n] = 0;
}
static void Ls(ln_t *l, const char *s){ while(*s) Lc(l, *s++); }
static void Lhex(ln_t *l, uint32_t v, int digits)
{
    const char *h = "0123456789ABCDEF";
    while(digits--) Lc(l, h[(v >> (digits * 4)) & 0xFu]);
}
static void Lu(ln_t *l, uint32_t v)
{
    char t[11]; int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while(v);
    while(n) Lc(l, t[--n]);
}
static void Lcol(ln_t *l, int col){ while(l->n < col && l->n < GFX_LINE_MAX) Lc(l, ' '); }
/* A 64-bit physical address as HI_LLLLLLLL, or just LLLLLLLL below
   4 GB -- never the two halves run together with no separator. */
static void Laddr(ln_t *l, uint32_t hi, uint32_t lo)
{
    if(hi){ Lhex(l, hi, hi > 0xFFu ? 8 : 2); Lc(l, '_'); }
    Lhex(l, lo, 8);
}
static void E(ln_t *l, int tone){ g_emit(tone, l->b); L0(l); }
static void say(int tone, const char *s){ ln_t l; L0(&l); Ls(&l, s); E(&l, tone); }

static const char *port_name(int p)
{
    switch(p){
    case 0: return "DDI A"; case 1: return "DDI B"; case 2: return "DDI C";
    case 3: return "TC1";   case 4: return "TC2";   case 5: return "TC3";
    case 6: return "TC4";   default: return "none";
    }
}
static char pipe_letter(int p){ return (char)('A' + p); }

static const char *ddi_mode(uint32_t func)
{
    switch((func >> 24) & 7u){
    case 0: return "HDMI";   case 1: return "DVI";
    case 2: return "DP SST"; case 3: return "DP MST";
    case 4: return "128b/132b";
    default: return "mode ?";
    }
}
static uint32_t ddi_bpc(uint32_t func)
{
    switch((func >> 20) & 7u){
    case 0: return 8; case 1: return 10; case 2: return 6; case 3: return 12;
    default: return 0;
    }
}
/* TGL_TRANS_DDI_PORT: bits 30:27 hold port + 1, 0 is none. */
static int func_port(uint32_t func)
{
    uint32_t v = (func >> 27) & 0xFu;
    return v ? (int)v - 1 : GFX_PORT_NONE;
}

static const char *plane_fmt(uint32_t ctl)
{
    /* PLANE_CTL_FORMAT_MASK_ICL, bits 27:23 -- the SKL values doubled. */
    switch((ctl >> 23) & 0x1Fu){
    case 8:  return "XRGB8888";
    case 4:  return "XRGB2101010";
    case 12: return "XRGB16161616F";
    case 28: return "RGB565";
    default: return "format ?";
    }
}
static const char *plane_tiling(uint32_t ctl)
{
    switch((ctl >> 10) & 7u){
    case 0: return "linear"; case 1: return "X-tiled";
    case 4: return "Y-tiled"; case 5: return "4-tiled";
    default: return "tiled ?";
    }
}

/* Hot-plug for one port, as a short phrase. For a combo port the
   south display (PCH) watches the pin; for Type-C the north display
   does for DP-alt and Thunderbolt, and the PCH again for a legacy
   connector. A detector the firmware did not enable reports nothing,
   and enabling it is a write -- so say that instead of "nothing". */
static void hpd_phrase(ln_t *l, const gfx_regs_t *r, int port)
{
    if(port < GFX_PORT_TC1){
        int en   = (r->shotplug_ddi >> (port * 4 + 3)) & 1;
        int live = (r->sdeisr >> (16 + port)) & 1;
        if(!en)       Ls(l, "detect off");
        else if(live) Ls(l, "CONNECTED");
        else          Ls(l, "nothing");
        return;
    }
    {
        int tc = port - GFX_PORT_TC1, any = 0;
        int leg_en = (r->shotplug_tc >> (tc * 4 + 3)) & 1;
        int alt_en = (r->tc_hpd_ctl  >> (tc * 4 + 3)) & 1;
        int tbt_en = (r->tbt_hpd_ctl >> (tc * 4 + 3)) & 1;
        if(leg_en && ((r->sdeisr >> (24 + tc)) & 1)){ Ls(l, "legacy "); any = 1; }
        if(alt_en && ((r->de_hpd_isr >> (16 + tc)) & 1)){ Ls(l, "DP-alt "); any = 1; }
        if(tbt_en && ((r->de_hpd_isr >> tc) & 1)){ Ls(l, "TBT "); any = 1; }
        if(any) Ls(l, "CONNECTED");
        else if(!leg_en && !alt_en && !tbt_en) Ls(l, "detect off");
        else Ls(l, "nothing");
    }
}

static const gfx_child_t *child_on(const gfx_vbt_t *vbt, int port)
{
    int i;
    if(!vbt || vbt->why != GFX_VBT_OK) return 0;
    for(i = 0; i < vbt->nchild; i++)
        if(vbt->child[i].port == port && !vbt->child[i].is_dsi)
            return &vbt->child[i];
    return 0;
}

static void child_kind(ln_t *l, const gfx_child_t *c)
{
    if(c->is_edp)                    Ls(l, "eDP panel");
    else if(c->is_hdmi && c->is_dp)  Ls(l, "DP + HDMI");
    else if(c->is_hdmi)              Ls(l, "HDMI");
    else if(c->is_dp)                Ls(l, "DP");
    else if(c->is_dsi)               Ls(l, "DSI panel");
    else { Ls(l, "type "); Lhex(l, c->type, 4); }
    if(c->lspcon) Ls(l, " (LSPCON)");
}

/* CDCLK from its PLL: reference x ratio / 2 / CD2X divider, kHz. */
static uint32_t cdclk_khz(const gfx_regs_t *r)
{
    uint32_t ref, ratio, vco;
    switch((r->dssm >> 29) & 7u){
    case 0: ref = 24000; break;
    case 1: ref = 19200; break;
    case 2: ref = 38400; break;
    default: return 0;
    }
    ratio = r->de_pll & 0xFFu;                  /* ICL_CDCLK_PLL_RATIO */
    if(!(r->de_pll & (1u << 31)) || !ratio) return 0;
    vco = ref * ratio;
    switch((r->cdclk_ctl >> 22) & 3u){         /* BXT_CDCLK_CD2X_DIV  */
    case 0: return vco / 2u;
    case 1: return vco / 3u;                   /* / 2 / 1.5           */
    case 2: return vco / 4u;
    default: return vco / 8u;
    }
}

/* a * b / c in 32 bits of result, through a 64-bit product but WITHOUT
   64-bit division: __udivdi3 is not linked into this kernel. Shift and
   subtract, 64 rounds. 0 if c is 0 or the result does not fit. */
static uint32_t muldiv(uint32_t a, uint32_t b, uint32_t c)
{
    uint64_t n = (uint64_t)a * b, q = 0, rem = 0;
    int i;
    if(!c) return 0;
    for(i = 63; i >= 0; i--){
        rem = (rem << 1) | ((n >> i) & 1u);
        if(rem >= c){ rem -= c; q |= (uint64_t)1 << i; }
    }
    return (q >> 32) ? 0 : (uint32_t)q;
}

/* i915 icl_ddi_combo_pll_get_freq(): DCO from the integer and 15-bit
   fraction, against a reference the PLL halves when it is 38.4 MHz;
   then divided by P (pdiv), Q (qdiv, when its mode bit is set), K
   (kdiv) and 5. */
uint32_t gfx_combo_pll_khz(uint32_t cfgcr0, uint32_t cfgcr1, uint32_t dssm)
{
    uint32_t ref, dco, p0, p1, p2, frac, frac_x2 = 0;
    switch((dssm >> 29) & 7u){
    case 0: ref = 24000; break;
    case 1: ref = 19200; break;
    case 2: ref = 19200; frac_x2 = 1; break;  /* 38.4, halved by the PLL */
    default: return 0;
    }
    switch((cfgcr1 >> 2) & 0xFu){          /* DPLL_CFGCR1_PDIV        */
    case 1: p0 = 2; break; case 2: p0 = 3; break;
    case 4: p0 = 5; break; case 8: p0 = 7; break;
    default: return 0;
    }
    switch((cfgcr1 >> 6) & 7u){            /* DPLL_CFGCR1_KDIV        */
    case 1: p2 = 1; break; case 2: p2 = 2; break; case 4: p2 = 3; break;
    default: return 0;
    }
    p1 = (cfgcr1 & (1u << 9)) ? ((cfgcr1 >> 10) & 0xFFu) : 1u;
    if(!p1) return 0;
    /* v0.9.2: Display WA #22010492432 (ehl, tgl, adl-s, adl-p): with a
       38.4 MHz reference the fraction is programmed at HALF its value
       and must be doubled to read it (i915 icl_ddi_combo_pll_get_freq,
       ehl_combo_pll_div_frac_wa_needed). 0.9.1 missed this and showed
       the HP's HBR link as 269.7 MHz; it is 270.0 exactly, which is
       what HBR is. The HP's own DPLL1 proves it: fraction 0x3800 is
       half of the 0x7000 that 8.1 GHz / 19.2 MHz needs. */
    frac = (cfgcr0 >> 10) & 0x7FFFu;
    if(frac_x2) frac *= 2u;
    dco = (cfgcr0 & 0x3FFu) * ref + (frac * ref) / 0x8000u;
    return dco / (p0 * p1 * p2 * 5u);
}

/* The port clock behind a combo port: which DPLL DPCLKA selects, and
   that DPLL's frequency. 0 if the clock is gated or the PLL is off. */
static uint32_t port_clock_khz(const gfx_regs_t *r, int port)
{
    uint32_t sel;
    if(port != GFX_PORT_A && port != GFX_PORT_B) return 0;
    if(r->dpclka & (1u << (10 + port))) return 0;
    sel = (r->dpclka >> (port * 2)) & 3u;
    if(sel > 1 || !(r->dpll_en[sel] & (1u << 31))) return 0;
    return gfx_combo_pll_khz(r->dpll_cfgcr0[sel], r->dpll_cfgcr1[sel], r->dssm);
}

static void put_hz100(ln_t *l, uint32_t x100)
{
    uint32_t f = x100 % 100u;
    Lu(l, x100 / 100u); Lc(l, '.');
    Lc(l, (char)('0' + f / 10u)); Lc(l, (char)('0' + f % 10u));
    Ls(l, " Hz");
}

static void put_mhz(ln_t *l, uint32_t khz)
{
    Lu(l, khz / 1000u);
    if(khz % 1000u){ Lc(l, '.'); Lu(l, (khz % 1000u) / 100u); }
    Ls(l, " MHz");
}

void gfx_report(const gfx_dev_t *dev, const gfx_regs_t *r,
                const gfx_vbt_t *vbt, gfx_emit_t emit)
{
    ln_t l;
    int p, t, i, panel_trans = -1, panel_port = GFX_PORT_NONE;
    int edp_port = GFX_PORT_NONE;
    uint32_t fb_hi = 0, fb_lo = 0, fb_w = 0, fb_h = 0, fb_pitch = 0;

    g_emit = emit; L0(&l);
    if(vbt && vbt->why == GFX_VBT_OK)
        for(i = 0; i < vbt->nchild; i++)
            if(vbt->child[i].is_edp){ edp_port = vbt->child[i].port; break; }
    say(GFX_T_HEAD, "Intel display engine -- read only, nothing is written");

    if(!dev->pci){
        say(GFX_T_BAD, "  No Intel display controller at 00:02.0.");
        say(GFX_T_TEXT, "  That is expected in QEMU and UTM: they have no");
        say(GFX_T_TEXT, "  Iris Xe. This command is for the HP (8086:46A8).");
        return;
    }
    Ls(&l, "  00:02.0  8086:"); Lhex(&l, dev->device, 4);
    Ls(&l, dev->why == GFX_EUNKNOWN ? "  not display 13"
                                    : "  Alder Lake-P, display version 13");
    E(&l, GFX_T_TEXT);
    Ls(&l, "  registers  BAR0 "); Laddr(&l, dev->bar0_hi, dev->bar0_lo);
    if(dev->mmio){ Ls(&l, ", 2 MB mapped at "); Lhex(&l, dev->mmio, 8);
                   Ls(&l, " uncached"); }
    E(&l, GFX_T_TEXT);
    Ls(&l, "  aperture   BAR2 "); Laddr(&l, dev->bar2_hi, dev->bar2_lo);
    E(&l, GFX_T_TEXT);
    kernel_fb_phys(&fb_hi, &fb_lo, &fb_w, &fb_h, &fb_pitch);
    if(fb_w){
        Ls(&l, "  firmware framebuffer "); Laddr(&l, fb_hi, fb_lo);
        Ls(&l, "  "); Lu(&l, fb_w); Lc(&l, 'x'); Lu(&l, fb_h);
        Ls(&l, ", pitch "); Lu(&l, fb_pitch);
        E(&l, GFX_T_TEXT);
    }

    if(dev->why != GFX_OK){
        Ls(&l, "  Stopped: "); Ls(&l, gfx_why_name(dev->why)); Lc(&l, '.');
        E(&l, GFX_T_BAD);
        switch(dev->why){
        case GFX_EUNKNOWN:
            say(GFX_T_TEXT, "  The offsets in this release are display 13's.");
            say(GFX_T_TEXT, "  On another generation they can mean other things,");
            say(GFX_T_TEXT, "  so nothing was read.");
            break;
        case GFX_EMEMOFF:
            say(GFX_T_TEXT, "  Turning it on is a write; 0.9.0 makes none.");
            break;
        case GFX_EMAP:
            say(GFX_T_TEXT, "  The kernel has eight windows above 4 GB and they");
            say(GFX_T_TEXT, "  may all be taken. `dmesg` lists who holds them.");
            break;
        case GFX_EDEAD:
            say(GFX_T_TEXT, "  The function may be powered down (D3), or BAR0");
            say(GFX_T_TEXT, "  points at nothing. Photograph `lspci -v`.");
            break;
        default: break;
        }
        return;
    }

    /* ── power ─────────────────────────────────────────────────── */
    say(GFX_T_HEAD, "Power");
    {
        static const struct { const char *n; int idx; } w[] = {
            {"PW_1", PW_1}, {"PW_A", PW_A}, {"PW_2", PW_2},
            {"PW_B", PW_B}, {"PW_C", PW_C}, {"PW_D", PW_D},
        };
        Ls(&l, "  ");
        for(i = 0; i < 6; i++){
            Ls(&l, w[i].n); Lc(&l, ' ');
            Ls(&l, well_on(r, w[i].idx) ? "on " : "off");
            Ls(&l, "  ");
        }
        E(&l, GFX_T_TEXT);
        /* Who asked, per requester register. 0.9.0 looked only at the
           BIOS one and printed "none" on the HP -- where every request
           is in the DRIVER register (45404 = 0000FC0F), set by the
           firmware's own GOP driver. The BIOS register there holds
           state bits only. Each register's odd bits are its requests. */
        {
            static const char *who[4] = { "BIOS", "driver", "KVMR", "debug" };
            int q, any = 0;
            for(q = 0; q < 4; q++){
                uint32_t req = r->pw[q] & 0xAAAAAAAAu, named = 0;
                if(!req) continue;
                any = 1;
                Ls(&l, "  requested in the "); Ls(&l, who[q]); Ls(&l, " register:");
                for(i = 0; i < 6; i++)
                    if(req & PW_REQ(w[i].idx)){
                        Lc(&l, ' '); Ls(&l, w[i].n); named |= PW_REQ(w[i].idx);
                    }
                if(req & ~named){ Ls(&l, " +bits "); Lhex(&l, req & ~named, 8); }
                E(&l, GFX_T_DIM);
            }
            if(!any) say(GFX_T_DIM, "  no requester register asks for any well");
        }
    }
    Ls(&l, "  DC states ");
    switch(r->dc_state & 3u){
    case 0: Ls(&l, "off"); break;
    case 1: Ls(&l, "up to DC5"); break;
    case 2: Ls(&l, "up to DC6"); break;
    default: Ls(&l, "DC3CO"); break;
    }
    if(r->dc_state & (1u << 3)) Ls(&l, " +DC9");
    Ls(&l, ".  DMC firmware ");
    Ls(&l, (r->dmc_htp || r->dmc_ssp) ? "has run." : "not loaded.");
    E(&l, (r->dc_state & 3u) ? GFX_T_WARN : GFX_T_TEXT);
    if(!(r->fuse & FUSE_DOWNLOADED))
        say(GFX_T_WARN, "  Fuse download is NOT complete -- the engine is not "
                        "initialised.");

    if(!r->core){
        say(GFX_T_BAD, "  PW_1 is off: the display core is asleep and every");
        say(GFX_T_BAD, "  register past this point would read zero. Not read.");
        goto selfcheck;
    }
    {
        uint32_t khz = cdclk_khz(r);
        Ls(&l, "  CDCLK ");
        if(khz) put_mhz(&l, khz);
        else    Ls(&l, "PLL off");
        Ls(&l, "  (ref ");
        switch((r->dssm >> 29) & 7u){
        case 0: Ls(&l, "24"); break; case 1: Ls(&l, "19.2"); break;
        case 2: Ls(&l, "38.4"); break; default: Ls(&l, "?"); break;
        }
        Ls(&l, " MHz, ratio "); Lu(&l, r->de_pll & 0xFFu);
        Ls(&l, (r->de_pll & (1u << 30)) ? ", locked)" : ", NOT locked)");
        E(&l, GFX_T_TEXT);
    }

    /* ── pipes ─────────────────────────────────────────────────── */
    say(GFX_T_HEAD, "Pipes (plane 1 of each)");
    for(p = 0; p < GFX_NPIPES; p++){
        const gfx_pipe_t *pp = &r->pipe[p];
        static const uint32_t dfsm_off[4] = { 1u<<30, 1u<<21, 1u<<28, 1u<<22 };
        Ls(&l, "  "); Lc(&l, pipe_letter(p)); Ls(&l, "  ");
        if(r->dfsm & dfsm_off[p]){ Ls(&l, "fused off"); E(&l, GFX_T_DIM); continue; }
        if(!pp->powered){
            Ls(&l, "off  (PW_"); Lc(&l, pipe_letter(p));
            Ls(&l, " is off -- not read)");
            E(&l, GFX_T_DIM); continue;
        }
        Ls(&l, (pp->conf & (1u << 30)) ? "ON " : "off");
        if(!(pp->ctl & (1u << 31))){ Ls(&l, "  plane off"); E(&l, GFX_T_TEXT); continue; }
        Ls(&l, "  "); Lu(&l, (pp->size & 0xFFFFu) + 1u); Lc(&l, 'x');
        Lu(&l, (pp->size >> 16) + 1u);
        Lc(&l, ' '); Ls(&l, plane_fmt(pp->ctl));
        Lc(&l, ' '); Ls(&l, plane_tiling(pp->ctl));
        /* PLANE_STRIDE__MASK 11:0, in 64-byte units for linear. */
        Ls(&l, ", stride "); Lu(&l, (pp->stride & 0xFFFu) * 64u);
        E(&l, GFX_T_GOOD);
        Ls(&l, "        scans out GTT "); Lhex(&l, pp->surf & 0xFFFFF000u, 8);
        /* Is this the framebuffer HavenDOS draws on? The aperture
           (BAR2) is a window onto the GTT, so the GOP's framebuffer
           sits at BAR2 + the plane's surface address. */
        if(fb_w && dev->bar2_hi == fb_hi &&
           dev->bar2_lo + (pp->surf & 0xFFFFF000u) == fb_lo){
            Ls(&l, " = the framebuffer HavenDOS draws on");
            E(&l, GFX_T_GOOD);
        } else {
            Ls(&l, " (aperture "); Laddr(&l, dev->bar2_hi,
                                      dev->bar2_lo + (pp->surf & 0xFFFFF000u));
            Lc(&l, ')'); E(&l, GFX_T_TEXT);
        }
        /* PIPESRC against the transcoder's active size: when they
           differ the pipe scaler is stretching the image. On the HP
           the GOP runs 1024x768 and the panel is 1920x1080. */
        {
            const gfx_trans_t *tt = &r->trans[p];
            uint32_t sw = (tt->src >> 16) + 1u, sh = (tt->src & 0xFFFFu) + 1u;
            uint32_t aw = (tt->htotal & 0xFFFFu) + 1u, ah = (tt->vtotal & 0xFFFFu) + 1u;
            if(tt->powered && tt->src && (tt->func & (1u << 31)) &&
               (sw != aw || sh != ah)){
                Ls(&l, "        pipe source "); Lu(&l, sw); Lc(&l, 'x'); Lu(&l, sh);
                Ls(&l, ", scaled to "); Lu(&l, aw); Lc(&l, 'x'); Lu(&l, ah);
                Ls(&l, " by the pipe scaler");
                E(&l, GFX_T_TEXT);
            }
        }
    }

    /* ── transcoders ───────────────────────────────────────────── */
    say(GFX_T_HEAD, "Transcoders");
    for(t = 0; t < GFX_NPIPES; t++){
        const gfx_trans_t *tt = &r->trans[t];
        Ls(&l, "  "); Lc(&l, pipe_letter(t)); Ls(&l, "  ");
        if(!tt->powered){ Ls(&l, "off  (well off -- not read)"); E(&l, GFX_T_DIM); continue; }
        if(!(tt->func & (1u << 31))){ Ls(&l, "off"); E(&l, GFX_T_TEXT); continue; }
        /* The panel's transcoder is the one feeding the port the VBT
           calls eDP; without a VBT, the first one that is on. */
        if(func_port(tt->func) == edp_port ||
           (panel_trans < 0 && edp_port == GFX_PORT_NONE)){
            panel_trans = t; panel_port = func_port(tt->func);
        }
        Ls(&l, "ON   -> "); Ls(&l, port_name(func_port(tt->func)));
        Lc(&l, ' '); Ls(&l, ddi_mode(tt->func));
        Ls(&l, ", "); Lu(&l, ddi_bpc(tt->func)); Ls(&l, " bpc, x");
        Lu(&l, ((tt->func >> 1) & 7u) + 1u);
        E(&l, GFX_T_GOOD);
        Ls(&l, "        "); Lu(&l, (tt->htotal & 0xFFFFu) + 1u); Lc(&l, 'x');
        Lu(&l, (tt->vtotal & 0xFFFFu) + 1u);
        Ls(&l, " active, "); Lu(&l, (tt->htotal >> 16) + 1u); Lc(&l, 'x');
        Lu(&l, (tt->vtotal >> 16) + 1u); Ls(&l, " total");
        Ls(&l, ", hsync "); Lu(&l, (tt->hsync & 0xFFFFu) + 1u); Lc(&l, '-');
        Lu(&l, (tt->hsync >> 16) + 1u);
        E(&l, GFX_T_TEXT);
        /* The pixel clock: for DP it is link M/N times the port clock
           (i915 intel_dotclock_calculate), for HDMI at 8 bpc the port
           clock itself. Then the refresh rate from the totals. */
        {
            int      port = func_port(tt->func);
            uint32_t mode = (tt->func >> 24) & 7u;
            uint32_t pk   = port_clock_khz(r, port), pclk = 0;
            uint32_t ht   = (tt->htotal >> 16) + 1u, vt = (tt->vtotal >> 16) + 1u;
            if(pk && (mode == 2 || mode == 3))
                pclk = muldiv(tt->link_m & 0xFFFFFFu, pk, tt->link_n & 0xFFFFFFu);
            else if(pk && mode == 0 && ddi_bpc(tt->func) == 8)
                pclk = pk;
            if(pclk){
                uint32_t lines = pclk * 1000u / ht;
                Ls(&l, "        pixel clock "); put_mhz(&l, pclk);
                Ls(&l, ", "); put_hz100(&l, lines * 100u / vt);
                if(mode == 2 || mode == 3){
                    Ls(&l, "  (link M/N "); Lu(&l, tt->link_m & 0xFFFFFFu);
                    Lc(&l, '/'); Lu(&l, tt->link_n & 0xFFFFFFu); Lc(&l, ')');
                }
                E(&l, GFX_T_TEXT);
            }
        }
    }

    /* ── ports ─────────────────────────────────────────────────── */
    Ls(&l, "Ports"); Lcol(&l, 11); Ls(&l, "buffer"); Lcol(&l, 19);
    Ls(&l, "hot-plug"); Lcol(&l, 37); Ls(&l, "the VBT says");
    E(&l, GFX_T_HEAD);
    for(i = 0; i < GFX_NPORTS; i++){
        const gfx_port_t *pt = &r->port[i];
        const gfx_child_t *c;
        if(!pt->exists) continue;
        Ls(&l, "  "); Ls(&l, port_name(i)); Lcol(&l, 11);
        if(!pt->powered) Ls(&l, "--");
        else if(pt->buf & (1u << 31)) Ls(&l, "ON");
        else Ls(&l, "off");
        Lcol(&l, 19);
        hpd_phrase(&l, r, i);
        Lcol(&l, 37);
        c = child_on(vbt, i);
        if(c){
            child_kind(&l, c);
            /* DDC is HDMI/DVI's wire. An eDP or DP child's ddc_pin is
               meaningless and printing it (0.9.0: "eDP panel, DDC 9")
               invites someone to read EDID down the wrong wires. */
            if(c->gmbus_pin && (c->type & DT_TMDS)){
                Ls(&l, ", DDC "); Lu(&l, (uint32_t)c->gmbus_pin);
            }
            if(c->aux_port != GFX_PORT_NONE && c->is_dp){
                Ls(&l, ", AUX "); Ls(&l, port_name(c->aux_port));
            }
        } else Ls(&l, (vbt && vbt->why == GFX_VBT_OK) ? "not wired" : "?");
        E(&l, (pt->buf & (1u << 31)) ? GFX_T_GOOD : GFX_T_TEXT);
        if(i >= GFX_PORT_TC1 && pt->powered && pt->tcss == 0xFFFFFFFFu){
            say(GFX_T_DIM, "           (Type-C PHY asleep: TC-cold)");
        }
    }

    /* ── PLLs ──────────────────────────────────────────────────── */
    say(GFX_T_HEAD, "PLLs");
    for(i = 0; i < 2; i++){
        Ls(&l, "  DPLL"); Lu(&l, (uint32_t)i); Lc(&l, ' ');
        if(r->dpll_en[i] & (1u << 31))
            Ls(&l, (r->dpll_en[i] & (1u << 30)) ? "ON, locked" : "ON, NOT LOCKED");
        else Ls(&l, "off, free");
        if(r->dpll_en[i] & (1u << 31)){
            uint32_t k = gfx_combo_pll_khz(r->dpll_cfgcr0[i], r->dpll_cfgcr1[i],
                                           r->dssm);
            Ls(&l, ", ");
            if(k) put_mhz(&l, k); else Ls(&l, "dividers ?");
            Ls(&l, "  cfg "); Lhex(&l, r->dpll_cfgcr0[i], 8);
            Lc(&l, ' '); Lhex(&l, r->dpll_cfgcr1[i], 8);
        }
        E(&l, GFX_T_TEXT);
    }
    Ls(&l, "  TBT PLL "); Ls(&l, (r->tbt_en & (1u << 31)) ? "on" : "off");
    Ls(&l, "   TC PLLs");
    for(i = 0; i < 4; i++){
        Ls(&l, (r->tcpll_en[i] & (1u << 31)) ? " on" : " off");
    }
    E(&l, GFX_T_TEXT);
    /* Which PLL clocks each combo port: ICL_DPCLKA_CFGCR0, two select
       bits per PHY and a clock-off bit at 10 + PHY. */
    for(i = 0; i < 2; i++){
        Ls(&l, "  "); Ls(&l, port_name(i)); Ls(&l, " clock: ");
        if(r->dpclka & (1u << (10 + i))) Ls(&l, "gated off");
        else { Ls(&l, "DPLL"); Lu(&l, (r->dpclka >> (i * 2)) & 3u); }
        E(&l, GFX_T_TEXT);
    }

selfcheck:
    say(GFX_T_HEAD, "Self-check");
    Ls(&l, "  "); Lu(&l, r->reads); Ls(&l, " registers read, 0 written");
    if(r->out_of_span){ Ls(&l, ", "); Lu(&l, r->out_of_span); Ls(&l, " REFUSED past 2 MB"); }
    E(&l, r->out_of_span ? GFX_T_BAD : GFX_T_TEXT);
    if(!(r->fpga_before & FPGA_NOCLAIM) && (r->fpga_after & FPGA_NOCLAIM)){
        say(GFX_T_BAD, "  UNCLAIMED READ: a register was read in a well that");
        say(GFX_T_BAD, "  was off. A gate in drivers/xedisp.c is wrong -- photograph");
        say(GFX_T_BAD, "  `gfx regs` so the read can be found.");
    } else if(r->fpga_before & FPGA_NOCLAIM){
        say(GFX_T_DIM, "  unclaimed-access flag was already set (by the firmware);");
        say(GFX_T_DIM, "  clearing it is a write, so this pass cannot use it.");
    } else {
        say(GFX_T_GOOD, "  unclaimed-access flag clear before and after: every");
        say(GFX_T_GOOD, "  register read was in a well that was on.");
    }
    if(!r->core) return;

    /* ── what it means ─────────────────────────────────────────── */
    say(GFX_T_HEAD, "So");
    if(panel_trans >= 0){
        Ls(&l, "  The panel runs on pipe "); Lc(&l, pipe_letter(panel_trans));
        Ls(&l, ", transcoder "); Lc(&l, pipe_letter(panel_trans));
        Ls(&l, ", "); Ls(&l, port_name(panel_port)); Lc(&l, '.');
        E(&l, GFX_T_TEXT);
    } else say(GFX_T_WARN, "  No transcoder is on. Something else drives the panel.");
    if(!(r->trans[1].func & (1u << 31)) && !(r->pipe[1].conf & (1u << 30)))
        say(GFX_T_TEXT, "  Pipe B and transcoder B are free for the monitor.");
    if(vbt && vbt->why == GFX_VBT_OK){
        int found = 0;
        for(i = 0; i < vbt->nchild; i++){
            const gfx_child_t *c = &vbt->child[i];
            if(c->is_edp || c->is_dsi || c->port == GFX_PORT_NONE) continue;
            if(!c->is_hdmi) continue;
            found = 1;
            Ls(&l, "  HDMI is wired to "); Ls(&l, port_name(c->port));
            if(c->lspcon){
                Ls(&l, " through an LSPCON (DP inside).");
                E(&l, GFX_T_GOOD);
                Ls(&l, "  Its EDID comes over DP AUX ");
                Ls(&l, port_name(c->aux_port)); Ls(&l, ", not GMBUS (not in 0.9.1).");
            } else {
                Lc(&l, '.'); E(&l, GFX_T_GOOD);
                Ls(&l, "  `gfx edid` reads the monitor over GMBUS pin ");
                Lu(&l, (uint32_t)c->gmbus_pin); Lc(&l, '.');
            }
            E(&l, GFX_T_TEXT);
        }
        if(!found) say(GFX_T_WARN, "  The VBT names no HDMI connector.");
    } else {
        Ls(&l, "  No VBT: "); Ls(&l, gfx_vbt_why_name(vbt ? vbt->why : 0));
        E(&l, GFX_T_WARN);
    }
}

void gfx_report_regs(const gfx_dev_t *dev, const gfx_regs_t *r,
                     gfx_emit_t emit)
{
    ln_t l;
    int i;
    (void)r;
    g_emit = emit; L0(&l);
    say(GFX_T_HEAD, "Every register read, in order (offset  value)");
    if(dev->why != GFX_OK){
        Ls(&l, "  none: "); Ls(&l, gfx_why_name(dev->why)); E(&l, GFX_T_BAD);
        return;
    }
    /* Three to a line, so a screen holds a photograph's worth. */
    for(i = 0; i < g_nlog; i++){
        if(i % 3 == 0) Ls(&l, " ");
        Ls(&l, " "); Lhex(&l, g_log[i].off, 6); Ls(&l, " ");
        Lhex(&l, g_log[i].val, 8); Ls(&l, "  ");
        if(i % 3 == 2 || i == g_nlog - 1) E(&l, GFX_T_TEXT);
    }
    if(g_nlog >= GFX_LOG_MAX) say(GFX_T_WARN, "  (log full -- later reads not shown)");
}

void gfx_report_vbt(const gfx_vbt_t *vbt, gfx_emit_t emit)
{
    ln_t l;
    int i;
    g_emit = emit; L0(&l);
    say(GFX_T_HEAD, "Video BIOS Table -- what the firmware says is wired");
    if(vbt->why != GFX_VBT_OK){
        Ls(&l, "  "); Ls(&l, gfx_vbt_why_name(vbt->why)); Lc(&l, '.');
        E(&l, GFX_T_BAD);
        return;
    }
    Ls(&l, "  OpRegion "); Lu(&l, vbt->opregion_ver >> 8u); Lc(&l, '.');
    Lu(&l, vbt->opregion_ver & 0xFFu);
    Ls(&l, ", VBT at "); Lhex(&l, vbt->where, 8);
    Ls(&l, vbt->from_rvda ? " (RVDA)" : " (mailbox 4)");
    Ls(&l, ", BDB version "); Lu(&l, vbt->bdb_version);
    E(&l, GFX_T_TEXT);
    Ls(&l, "  "); Lu(&l, (uint32_t)vbt->nchild);
    Ls(&l, " child devices, "); Lu(&l, vbt->child_size); Ls(&l, " bytes each");
    E(&l, GFX_T_TEXT);
    say(GFX_T_DIM, "  type  dvo  port   kind              ddc gmbus  aux");
    for(i = 0; i < vbt->nchild; i++){
        const gfx_child_t *c = &vbt->child[i];
        Ls(&l, "  "); Lhex(&l, c->type, 4); Ls(&l, "  ");
        Lu(&l, c->dvo_port); Lcol(&l, 13);
        Ls(&l, port_name(c->port)); Lcol(&l, 20);
        child_kind(&l, c); Lcol(&l, 38);
        Lu(&l, c->ddc_pin); Lcol(&l, 42);
        if(c->gmbus_pin) Lu(&l, (uint32_t)c->gmbus_pin); else Lc(&l, '-');
        Lcol(&l, 49);
        Lhex(&l, c->aux, 2); Ls(&l, " "); Ls(&l, port_name(c->aux_port));
        E(&l, GFX_T_TEXT);
    }
}


/* ── `gfx edid` ───────────────────────────────────────────────────── */
static void put_pclk(ln_t *l, uint32_t khz)
{
    /* EDID clocks are 10 kHz steps: 148500 -> "148.5 MHz",
       325080 -> "325.08 MHz". */
    uint32_t f = (khz % 1000u) / 10u;
    Lu(l, khz / 1000u);
    if(f){ Lc(l, '.'); Lc(l, (char)('0' + f / 10u)); if(f % 10u) Lc(l, (char)('0' + f % 10u)); }
    Ls(l, " MHz");
}

void gfx_report_edid(const gmbus_trace_t *tr, const edid_t *e,
                     const uint8_t *raw, uint32_t rawlen, gfx_emit_t emit)
{
    ln_t l;
    int i;
    uint32_t b;
    g_emit = emit; L0(&l);

    Ls(&l, "Monitor EDID -- GMBUS pin "); Lu(&l, (uint32_t)tr->pin);
    switch(tr->pin){
    case 1: Ls(&l, " (DDI A)"); break;  case 2: Ls(&l, " (DDI B)"); break;
    case 9: Ls(&l, " (TC1)"); break;    case 10: Ls(&l, " (TC2)"); break;
    case 11: Ls(&l, " (TC3)"); break;   case 12: Ls(&l, " (TC4)"); break;
    default: break;
    }
    Ls(&l, ", I2C address "); Lhex(&l, tr->addr, 2);
    E(&l, GFX_T_HEAD);

    Ls(&l, "  "); Lu(&l, tr->writes);
    Ls(&l, " writes, every one to GMBUS (C5100-C5120); ");
    Lu(&l, tr->refused); Ls(&l, " refused");
    E(&l, tr->refused ? GFX_T_BAD : GFX_T_TEXT);
    if(tr->retried) say(GFX_T_DIM, "  the first attempt was NAKed and retried once, as i915 does");

    if(tr->why != GMBUS_OK && rawlen < EDID_BLOCK){
        Ls(&l, "  Stopped: "); Ls(&l, gmbus_why_name(tr->why)); Lc(&l, '.');
        E(&l, GFX_T_BAD);
        switch(tr->why){
        case GMBUS_ENAK:
            say(GFX_T_TEXT, "  Nothing answered at 50, twice. Is the monitor switched on,");
            say(GFX_T_TEXT, "  on the right input, and its cable in the laptop's HDMI port?");
            break;
        case GMBUS_ETIMEOUT:
            Ls(&l, "  GMBUS2 was "); Lhex(&l, tr->status, 8);
            Ls(&l, " when it gave up. Photograph this screen.");
            E(&l, GFX_T_TEXT);
            break;
        case GMBUS_ESTUCK:
            say(GFX_T_TEXT, "  GMBUS0 read back something other than the pin written,");
            say(GFX_T_TEXT, "  so nothing was sent: the transfer would have used");
            say(GFX_T_TEXT, "  whatever pin was there. The write log below has both.");
            break;
        case GMBUS_EBUSY:
            Ls(&l, "  Another transfer was still running (GMBUS2 ");
            Lhex(&l, tr->status, 8); Ls(&l, "). Nothing was started.");
            E(&l, GFX_T_TEXT);
            break;
        case GMBUS_EPIN:
            say(GFX_T_TEXT, "  Pins on Alder Lake-P: 1 (DDI A), 2 (DDI B), 9-12 (TC1-TC4).");
            break;
        default: break;
        }
    } else if(e->why != EDID_OK){
        Ls(&l, "  "); Lu(&l, rawlen); Ls(&l, " bytes came back, but they are not an EDID: ");
        E(&l, GFX_T_BAD);
        Ls(&l, "  "); Ls(&l, edid_why_name(e->why)); Lc(&l, '.');
        E(&l, GFX_T_BAD);
    } else {
        Ls(&l, "  maker "); Ls(&l, e->mfg);
        Ls(&l, "  product "); Lhex(&l, e->product, 4);
        Ls(&l, "  made week "); Lu(&l, e->week); Ls(&l, " of "); Lu(&l, e->year);
        Ls(&l, "  EDID "); Lu(&l, e->version); Lc(&l, '.'); Lu(&l, e->revision);
        E(&l, GFX_T_GOOD);
        if(e->name[0]){ Ls(&l, "  name  "); Ls(&l, e->name); E(&l, GFX_T_GOOD); }
        if(e->serial_text[0]){ Ls(&l, "  serial "); Ls(&l, e->serial_text); E(&l, GFX_T_TEXT); }
        if(e->have_range){
            Ls(&l, "  limits "); Lu(&l, e->vmin); Lc(&l, '-'); Lu(&l, e->vmax);
            Ls(&l, " Hz, "); Lu(&l, e->hmin_khz); Lc(&l, '-'); Lu(&l, e->hmax_khz);
            Ls(&l, " kHz");
            if(e->max_pclk_mhz){ Ls(&l, ", pixel clock up to "); Lu(&l, e->max_pclk_mhz); Ls(&l, " MHz"); }
            E(&l, GFX_T_TEXT);
        }
        Ls(&l, "  "); Lu(&l, e->extensions); Ls(&l, " extension block(s) announced, ");
        Lu(&l, e->cta_blocks); Ls(&l, " CTA-861 read");
        if(e->bad_ext){ Ls(&l, ", "); Lu(&l, e->bad_ext); Ls(&l, " with a bad checksum"); }
        if(tr->why != GMBUS_OK){ Ls(&l, "; block 1 failed: "); Ls(&l, gmbus_why_name(tr->why)); }
        E(&l, (e->bad_ext || tr->why != GMBUS_OK) ? GFX_T_WARN : GFX_T_TEXT);

        say(GFX_T_HEAD, "Modes (the first is the one the monitor prefers)");
        if(!e->nmodes) say(GFX_T_WARN, "  none -- no detailed timing in the blocks read");
        for(i = 0; i < e->nmodes; i++){
            const edid_mode_t *m = &e->modes[i];
            Ls(&l, "  "); Lu(&l, m->hactive); Lc(&l, 'x'); Lu(&l, m->vactive);
            if(m->interlaced) Lc(&l, 'i');
            Lcol(&l, 13); put_hz100(&l, m->refresh_x100);
            Ls(&l, "  "); put_pclk(&l, m->pclk_khz);
            Ls(&l, "  total "); Lu(&l, (uint32_t)m->hactive + m->hblank); Lc(&l, 'x');
            Lu(&l, (uint32_t)m->vactive + m->vblank);
            Ls(&l, m->hpos ? " +h" : " -h"); Ls(&l, m->vpos ? "+v" : "-v");
            if(m->block){ Ls(&l, " (block "); Lu(&l, m->block); Lc(&l, ')'); }
            E(&l, i == 0 ? GFX_T_GOOD : GFX_T_TEXT);
        }
        if(e->nmodes){
            /* v0.9.2: NOT the first one. On the HP's LG the first is
               1080p144 at 346.5 MHz, over HDMI 1.4's 340 MHz. */
            int k = edid_pick(e, EDID_TMDS_NOSCRAMBLE_KHZ, 1024, 768);
            if(k < 0){
                say(GFX_T_WARN, "  `gfx on` has no mode it can light here: none is");
                say(GFX_T_WARN, "  progressive, 1024x768 or larger, and 340 MHz or under.");
            } else {
                const edid_mode_t *m = &e->modes[k];
                Ls(&l, "  `gfx on` lights "); Lu(&l, m->hactive); Lc(&l, 'x');
                Lu(&l, m->vactive); Ls(&l, " at "); put_hz100(&l, m->refresh_x100);
                Ls(&l, ", "); put_pclk(&l, m->pclk_khz); Lc(&l, '.');
                E(&l, GFX_T_GOOD);
                if(e->modes[0].pclk_khz > EDID_TMDS_NOSCRAMBLE_KHZ)
                    say(GFX_T_TEXT, "  The preferred mode is over 340 MHz: it needs HDMI 2.0"
                                    " scrambling.");
            }
        }
    }

    if(rawlen){
        Ls(&l, "Raw ("); Lu(&l, rawlen); Ls(&l, " bytes)"); E(&l, GFX_T_HEAD);
        for(b = 0; b < rawlen; b += 16){
            uint32_t k;
            Ls(&l, "  "); Lhex(&l, b, 3); Lc(&l, ':');
            for(k = 0; k < 16 && b + k < rawlen; k++){ Lc(&l, ' '); Lhex(&l, raw[b + k], 2); }
            E(&l, GFX_T_DIM);
        }
    }

    say(GFX_T_HEAD, "Every GMBUS write, in order (register=written/read back)");
    for(i = 0; i < tr->nwlog; i++){
        if(i % 3 == 0) Ls(&l, " ");
        Lc(&l, ' '); Lhex(&l, tr->wlog[i].off, 5); Lc(&l, '=');
        Lhex(&l, tr->wlog[i].val, 8); Lc(&l, '/'); Lhex(&l, tr->wlog[i].back, 8);
        if(i % 3 == 2 || i == tr->nwlog - 1) E(&l, GFX_T_TEXT);
    }
    if(!tr->nwlog) say(GFX_T_DIM, "  none");
}

#ifdef GFX_TEST
uint32_t gfx_test_cut(void){ return g_cut; }
int      gfx_test_nlog(void){ return g_nlog; }
void     gfx_test_unmap(void){ g_map_hi = g_map_lo = g_map_virt = 0; }
#endif
