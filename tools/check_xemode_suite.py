#!/usr/bin/env python3
"""check_xemode_suite.py -- prove tests/test_xemode.c can fail.

0.9.2 lights pipe B on the HP's HDMI socket: a PLL, a port clock, a
transcoder, a pipe, a combo PHY's signal levels, a buffer allocation and
a plane. drivers/xemode.c is the first HavenDOS code that changes what a
screen shows, and QEMU has no Iris Xe to show it on. Between the code
and the HP there is this suite, and this harness is how the suite is
trusted.

Three families of mistake matter:

  the panel     a write that reaches pipe A's path -- its transcoder,
                pipe, port, PHY or PLL -- or a shared register written
                back with A's bits changed. That is the laptop's only
                screen going dark, on a machine with no serial port.
  the sequence  i915's order is not decoration: a port clock ungated
                before its PLL locks, a transcoder enabled before its
                DDI function, a plane armed before its buffer. Each is a
                monitor that shows nothing, or a pipe that underruns.
  the values    a PLL fraction not halved, a sync pulse a pixel long, a
                lane's swing from the wrong table entry. Each is a
                plausible-looking register dump and a black screen.

Defects are planted in drivers/xemode.c and drivers/edid.c (the fifth
element names the file when it is not the first). tools/check_gfx_
readonly.py runs too: some defects are a write the BUILD refuses.

── WHAT IT FOUND ON ITS FIRST RUN ─────────────────────────────────────

193 of 193 passed, and this reported TWO misses out of 27 -- both in
the suite, neither in the driver:

  every clock the suite asked the PLL for had either one divider that
      fit or a first one that was also the most central, so "take the
      first" passed. 25.175 MHz fits five (64-78) and wants 72.
  every mode the HP's plan lit was +h +v, so swapping the two sync
      bits in DDI_FUNC_CTL changed nothing. A CVT reduced-blanking
      mode (+h -v) is planned now.

The same two shapes as every release before it: a fixture that cannot
produce the case.

Run:  python3 tools/check_xemode_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "drivers/xemode.c"
PARSER = "drivers/edid.c"
TEST = "tests/test_xemode.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function",
      "-fsanitize=address,undefined"]

BUGS = [
    # ── the panel ─────────────────────────────────────────────────
    ("the allow-list waves everything through",
     "a corrupted plan, or a bug elsewhere, could then write pipe A's "
     "PLANE_CTL and blank the panel",
     """    if(off & 3u) return 0;""",
     """    if(off & 3u) return 0;
    return 1;"""),

    ("DPCLKA may change A's bits too",
     "port A's clock select or gate rewritten: the panel loses its clock",
     """    if(off == R_DPCLKA)    return !(ch & ~(DPCLKA_B_SEL | DPCLKA_B_OFF));""",
     """    if(off == R_DPCLKA)    return 1;"""),

    ("the DDI IO request may be cleared, not only added",
     "a request removed is a well the firmware -- or DDI A -- relied on",
     """        return !(ch & ~PW_REQ(DDI_IO_B)) && (val & PW_REQ(DDI_IO_B));""",
     """        return !(ch & ~PW_REQ(DDI_IO_B));"""),

    ("the PLL port A runs on can be chosen",
     "the search skips port A's PLL; without that it reprograms the "
     "panel's link clock under it",
     """        if(n != apll && !(s->dpll_en[n] & (PLL_ENABLE | PLL_POWER_ENABLE))) break;""",
     """        if(!(s->dpll_en[n] & PLL_ENABLE)) break;"""),

    ("`gfx off` stops the PLL even when port A is on it",
     "turning the monitor off would turn the panel off",
     """    if(n < 2 && n != apll && (s->dpll_en[n] & (PLL_ENABLE | PLL_POWER_ENABLE))){""",
     """    if(n < 2 && (s->dpll_en[n] & (PLL_ENABLE | PLL_POWER_ENABLE))){"""),

    ("the write happens before the allow-list is asked",
     "the guard becomes a report of what already went wrong",
     """    if(off >= GFX_MMIO_SPAN || !xm_allowed(off, old, val, dpclka, p)){""",
     """    GFX_WR(dev->mmio, off, val);
    if(off >= GFX_MMIO_SPAN || !xm_allowed(off, old, val, dpclka, p)){"""),

    # ── refusals ──────────────────────────────────────────────────
    ("an already-lit pipe B is planned over",
     "a second `gfx on` would reprogram a running PLL and transcoder",
     """       (s->b_ctl & PLANE_EN) || (s->ddi_b_buf & DDI_BUF_EN))
        return refuse(p, XM_EBUSY);""",
     """       (s->b_ctl & PLANE_EN))
        return refuse(p, XM_EBUSY);"""),

    ("a joined MBUS is not refused",
     "with MBUS joined pipe A owns all four DBUF slices; B's allocation "
     "would overlap it and both pipes underrun",
     """    if(s->mbus_ctl & MBUS_JOIN) return refuse(p, XM_EMBUS);""",
     """    (void)s->mbus_ctl;"""),

    ("a DC state is not refused",
     "with DC5/DC6 armed the DMC can power wells down under the sequence",
     """    if(s->dc_state & DC_ANY) return refuse(p, XM_EDC);""",
     """    (void)s->dc_state;"""),

    # ── the sequence ──────────────────────────────────────────────
    ("the port clock is selected and ungated in ONE write",
     "i915: 'This step and the step before must be done with separate "
     "register writes'",
     """    rmw(p, R_DPCLKA, DPCLKA_B_SEL, (uint32_t)n << 2, DPCLKA_B_SEL, "DDI B clock: select");
    rmw(p, R_DPCLKA, DPCLKA_B_OFF, 0, DPCLKA_B_OFF, "DDI B clock: ungate");""",
     """    rmw(p, R_DPCLKA, DPCLKA_B_SEL | DPCLKA_B_OFF, (uint32_t)n << 2,
        DPCLKA_B_SEL | DPCLKA_B_OFF, "DDI B clock");"""),

    ("there is no wait for the PLL to lock",
     "the port clock is ungated onto a PLL that is still hunting",
     """    wset(p, R_DPLL_EN(n), PLL_LOCK, "DPLL: locked");""",
     """    /* no lock wait */"""),

    ("the arbiter workaround is dropped",
     "Wa_22012358565, every display-13 pipe, before TRANSCONF",
     """    rmw(p, R_B_ARB, 0, PIPE_ARB_PROG_SLOTS, PIPE_ARB_PROG_SLOTS, "PIPE_ARB_CTL (WA)");""",
     """    /* WA dropped */"""),

    ("the plane is armed before the DDI buffer is up",
     "PLANE_SURF arms the plane; it is the LAST write",
     """    wr(p, R_B_SURF,   s->a_surf,   0xFFFFF000u, "PLANE_SURF: the framebuffer");""",
     """    p->ops[p->nops] = p->ops[p->nops - 1];
    p->ops[p->nops - 1].op = XM_OP_WR; p->ops[p->nops - 1].off = R_B_SURF;
    p->ops[p->nops - 1].val = s->a_surf; p->ops[p->nops - 1].hold = 0xFFFFF000u;
    p->ops[p->nops - 1].what = "PLANE_SURF: the framebuffer"; p->nops++;"""),

    ("a group register is used for the read-back",
     "i915 never reads a PHY group register; what comes back is not a "
     "lane's value",
     """            why = xm_wr(dev, p, res, o->off, v, (v & ~o->clr) | o->set, o->rb, o->hold);""",
     """            why = xm_wr(dev, p, res, o->off, v, (v & ~o->clr) | o->set, 0, o->hold);"""),

    ("a failed step does not stop the sequence",
     "the next forty writes land on a half-configured pipe",
     """        if(why){ res->failed_op = i; return res->why = why; }""",
     """        if(why && res->failed_op < 0) res->failed_op = i;"""),

    # ── the values ────────────────────────────────────────────────
    ("the PLL fraction is not halved on a 38.4 MHz reference",
     "WA #22010492432: the DCO lands at twice its fraction and the port "
     "clock is off by a fraction of a percent -- outside HDMI's tolerance",
     """    if(halve) frac = (frac + 1u) / 2u;          /* DIV_ROUND_CLOSEST(f, 2) */""",
     """    (void)halve;"""),

    ("the most-central divider is not preferred",
     "the first divider that fits is taken, at the edge of the DCO range",
     """        if(c < best_c){ best_c = c; best = k_div[i]; best_dco = dco; }""",
     """        if(!best){ best_c = c; best = k_div[i]; best_dco = dco; }"""),

    ("sync end is the pulse width, not start + width",
     "a 44-pixel hsync programmed to end at pixel 44",
     """    hse = hss + p->mode.hspw;""",
     """    hse = p->mode.hspw;"""),

    ("the polarities are swapped",
     "+h-v monitors (the LG's 144 Hz mode) get -h+v",
     """         | (p->mode.vpos ? BIT(17) : 0u) | (p->mode.hpos ? BIT(16) : 0u);""",
     """         | (p->mode.hpos ? BIT(17) : 0u) | (p->mode.vpos ? BIT(16) : 0u);"""),

    ("HDMI mode is signalled with no infoframe",
     "0.9.2 signals DVI; HDMI mode needs an AVI infoframe it does not send",
     """         | (1u << 24)                  /* TRANS_DDI_MODE_SELECT_DVI    */""",
     """         | (0u << 24)                  /* TRANS_DDI_MODE_SELECT_DVI    */"""),

    ("lane 0 gets a loadgen too",
     "icl_combo_phy_loadgen_select: four lanes under 6 GHz is 0,1,1,1",
     """        rmw(p, R_TX_LN(4, ln), LOADGEN_SELECT, ln >= 1 ? LOADGEN_SELECT : 0,""",
     """        rmw(p, R_TX_LN(4, ln), LOADGEN_SELECT, LOADGEN_SELECT,"""),

    ("the VBT's vswing entry is ignored",
     "a board that needs entry 3 gets 6 and the signal overshoots",
     """    p->level = (child && child->hdmi_level >= 0 && child->hdmi_level < 7)
               ? child->hdmi_level : 6;""",
     """    p->level = 6;"""),

    ("an oversized watermark is copied enabled",
     "a level asking for more blocks than B's buffer has stalls the plane",
     """        if((v & BIT(31)) && (v & 0x1FFFu) > alloc){ v &= ~BIT(31); p->wm_dropped++; }
        wr(p, R_B_WM(i), v,""",
     """        wr(p, R_B_WM(i), v,"""),

    ("the plane is not centred",
     "1024x768 in the top-left corner of a 1920x1080 frame",
     """    p->pos_x = (p->mode.hactive - w) / 2u;""",
     """    p->pos_x = 0;"""),

    ("the line time is truncated, not rounded up",
     "skl_linetime_wm is DIV_ROUND_UP: 118 instead of 119",
     """    p->linetime = (ht * 8000u + p->mode.pclk_khz - 1u) / p->mode.pclk_khz;""",
     """    p->linetime = (ht * 8000u) / p->mode.pclk_khz;"""),

    # ── the mode ──────────────────────────────────────────────────
    ("the preferred mode is lit, scrambling or not",
     "the LG's 1080p144 is 346.5 MHz: over 340 without SCDC is no picture",
     """        if(m->interlaced || !m->pclk_khz || m->pclk_khz > max_khz) continue;""",
     """        if(m->interlaced || !m->pclk_khz) continue;""", PARSER),

    ("60 Hz is not preferred over bigger modes",
     "the LG's 2560x1440 at 75 Hz is a downscaled mode; the panel's "
     "picture belongs at 60",
     """        if(best < 0 || (at60 && !best60) ||
           (at60 == best60 && area > best_area)){""",
     """        if(best < 0 || area > best_area){""", PARSER),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the driver changed",
    """    case XM_OK:        return "ok";""",
    """    /* all is well */
    case XM_OK:        return "ok";""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "xemode", SRC, TEST, CC, BUGS, HARMLESS, timeout=240,
        files=(SRC, PARSER, TEST),
        checks=(["python3", "tools/check_gfx_readonly.py"],),
        preamble="Proving tests/test_xemode.c can fail.\n"
                 "drivers/xemode.c lights a screen, and nothing but the HP\n"
                 "can run it for real."))
