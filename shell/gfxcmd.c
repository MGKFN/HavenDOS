/* ════════════════════════════════════════════════════════════════════
   shell/gfxcmd.c -- the `gfx` command.  v0.9.0.

   THE FIRST STEP TOWARDS THE EXTERNAL MONITOR, AND IT WRITES NOTHING.

     gfx         what the firmware set up: power, pipes, transcoders,
                 ports and hot-plug, PLLs -- and what it means
     gfx regs    every register that was read, offset and value, in
                 the order it was read. This is the one to photograph
                 when the summary looks wrong.
     gfx vbt     the Video BIOS Table: which connector the firmware
                 says is wired to which port, with its DDC pin and AUX
     gfx edid [pin]   (0.9.1) the monitor's EDID over GMBUS: maker,
                 name, the modes it offers, the raw bytes, and every
                 GMBUS register write with its read-back. The pin
                 defaults to the one the VBT gives the HDMI port.
                 THE FIRST COMMAND IN THIS SERIES THAT WRITES -- to
                 the six GMBUS registers only; include/xegmbus.h.
     gfx plan    (0.9.2) what `gfx on` would write, every operation,
                 or why it refuses. Writes nothing but the EDID read.
     gfx on      (0.9.2) light the HDMI monitor: pipe B mirrors the
                 panel. Shows the plan's summary and asks y/N first.
     gfx off     (0.9.2) turn pipe B, DDI B and its PLL off again.
                 include/xemode.h has the whole story.

   Each run reads the registers afresh; the mapping of BAR0 is made
   once and kept (drivers/xedisp.c says why). Everything printed comes
   through the pager, because the summary is longer than the HP's
   screen is tall.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/xedisp.h"
#include "../include/xegmbus.h"
#include "../include/edid.h"
#include "../include/xemode.h"
#include "../include/ui.h"

extern int keyboard_waitchar(void);
extern void sleep_ms(uint32_t ms);
extern int  cfg_monitor_auto(void);  extern void cfg_set_monitor_auto(int);
extern int  cfg_monitor_fit(void);   extern void cfg_set_monitor_fit(int);
extern void cfg_save(void);
#include "../include/klog.h"
extern void toast_info(const char *m);
extern void toast_warning(const char *m);
static int  g_gfx_busy;              /* a `gfx` command is running     */
static void put_u(uint32_t v);
static void cmd_fit_auto(const char *arg, const char *v);

static int g_stop;

static void emit(int tone, const char *line)
{
    if(g_stop) return;
    switch(tone){
    case GFX_T_HEAD: vga_set_color(VGA_LIGHT_CYAN,  VGA_BLACK); break;
    case GFX_T_GOOD: vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK); break;
    case GFX_T_WARN: vga_set_color(VGA_YELLOW,      VGA_BLACK); break;
    case GFX_T_BAD:  vga_set_color(VGA_LIGHT_RED,   VGA_BLACK); break;
    case GFX_T_DIM:  vga_set_color(VGA_DARK_GREY,   VGA_BLACK); break;
    default:         vga_set_color(VGA_WHITE,       VGA_BLACK); break;
    }
    vga_puts(line);
    vga_puts("\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    if(!ui_page_line()) g_stop = 1;
}

/* Big structures, so not on the stack: the kernel stack is small and
   a register snapshot plus a VBT is a few kilobytes. */
static gfx_dev_t  g_dev;
static gfx_regs_t g_regs;
static gfx_vbt_t  g_vbt;
static gmbus_trace_t g_tr;
static edid_t        g_edid;
static uint8_t       g_raw[256];

/* The pin the VBT gives the HDMI connector; 2 (DDI B, the HP's) if the
   VBT names none or the connector is behind an LSPCON (EDID would come
   over DP AUX there, which 0.9.1 does not do). */
static int hdmi_pin(void)
{
    int i;
    if(g_vbt.why == GFX_VBT_OK)
        for(i = 0; i < g_vbt.nchild; i++){
            const gfx_child_t *c = &g_vbt.child[i];
            if(c->is_hdmi && !c->is_edp && !c->lspcon && c->gmbus_pin)
                return c->gmbus_pin;
        }
    return 2;
}

static void cmd_edid(const char *pinarg)
{
    uint32_t got = 0;
    int pin = (pinarg && *pinarg) ? atoi(pinarg) : hdmi_pin();
    memset(g_raw, 0, sizeof g_raw);
    memset(&g_edid, 0, sizeof g_edid);
    gmbus_read_edid(&g_dev, pin, g_raw, &got, &g_tr);
    if(got) edid_parse(g_raw, got, &g_edid);
    else g_edid.why = EDID_ESHORT;
    if(g_dev.why != GFX_OK){
        vga_puts("No display engine to read through: ");
        vga_puts(gfx_why_name(g_dev.why));
        vga_puts(".\n");
        return;
    }
    gfx_report_edid(&g_tr, &g_edid, g_raw, got, emit);
}

static xm_state_t  g_xs;
static xm_plan_t   g_plan;
static xm_result_t g_res;

/* The VBT's HDMI connector on DDI B, for its vswing entry and lane
   reversal; 0 if the VBT names none (the defaults are i915's). */
static const gfx_child_t *hdmi_child(void)
{
    int i;
    if(g_vbt.why != GFX_VBT_OK) return 0;
    for(i = 0; i < g_vbt.nchild; i++){
        const gfx_child_t *c = &g_vbt.child[i];
        if(c->is_hdmi && !c->is_edp && c->port == GFX_PORT_B) return c;
    }
    return 0;
}

/* gfx plan / gfx on / gfx off. The EDID is read fresh each time: a
   monitor plugged in since the last command is the monitor that
   counts. */
/* ── v0.9.6.4: lighting, robustly ─────────────────────────────────────
   The HP, 16 Nov: the watcher's first try at a SCALED light stopped
   part-way, which left pipe B half set up -- the scaler on, a PLL, a
   clock -- and every `gfx on` after it was refused as busy, `gfx off`
   included, because the refusal looked at the scaler and `gfx off`
   did not clear what was not enabled. Now, here, for `gfx on`, the
   watcher and Settings alike:

     1. pipe B half set up and not running: cleared first (`gfx off`)
     2. the plan, and -- for `gfx on` -- the question
     3. a scaled light that stops: what stopped it is kept, B is
        cleared, and it is lit unscaled, the way 0.9.6.2 lit it on the
        HP. The monitor shows something either way.

   L_ASK asks before writing; the report goes to `out` (null: silent). */
#define L_ASK 1
static xm_state_t  *g_ls;  static xm_plan_t *g_lp;  static xm_result_t *g_lr;
static int          g_fell_back;            /* scaled failed, centred lit */
static xm_plan_t    g_first;                /* the scaled plan that failed */
static xm_result_t  g_first_res;

static void quiet_emit(int tone, const char *line){ (void)tone; (void)line; }

static int clear_b(gfx_emit_t out, const char *why)
{
    xm_read(&g_dev, g_ls);
    xm_plan_off(g_ls, g_lp);
    if(g_lp->why) return g_lp->why == XM_EOFF ? XM_OK : g_lp->why;
    if(out != quiet_emit){ vga_set_color(VGA_YELLOW, VGA_BLACK); vga_puts(why); vga_set_color(VGA_WHITE, VGA_BLACK); }
    xm_apply(&g_dev, g_lp, g_lr);
    if(g_lr->why) xm_report_result(g_lp, g_lr, out);
    return g_lr->why;
}

static int plan_light(gfx_emit_t out, int report)
{
    uint32_t got = 0;
    xm_read(&g_dev, g_ls);
    memset(g_raw, 0, sizeof g_raw);
    memset(&g_edid, 0, sizeof g_edid);
    gmbus_read_edid(&g_dev, hdmi_pin(), g_raw, &got, &g_tr);
    if(got) edid_parse(g_raw, got, &g_edid);
    else g_edid.why = EDID_ESHORT;
    xm_plan_on(g_ls, got ? &g_edid : 0, hdmi_child(), g_lp);
    if(report) xm_report_plan(g_ls, g_lp, 0, out);
    return g_lp->why;
}

static int light(int flags, gfx_emit_t out, xm_state_t *xs, xm_plan_t *pl, xm_result_t *rs)
{
    int why, keep = xm_get_fit();
    g_ls = xs; g_lp = pl; g_lr = rs; g_fell_back = 0;
    memset(rs, 0, sizeof *rs); rs->failed_op = -1;

    xm_read(&g_dev, xs);
    if(xm_b_residue(xs)){
        why = clear_b(out, "Pipe B was left half set up by an attempt that stopped: clearing it first.\n");
        if(why) return why;
    }
    why = plan_light(out, 1);
    if(why) return why;
    if(flags & L_ASK){
        int c;
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("Light the HDMI monitor now? The panel is not touched. y/N ");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        c = keyboard_waitchar();
        vga_puts("\n");
        if(c != 'y' && c != 'Y'){ vga_puts("Nothing was written.\n"); return XM_EREFUSED; }
    }
    ui_page_reset(); g_stop = 0;
    xm_apply(&g_dev, pl, rs);
    xm_report_result(pl, rs, out);
    if(rs->why == XM_OK || pl->fit == XM_FIT_CENTRE){
        if(rs->why) clear_b(out, "Clearing what was done, so the next try starts clean.\n");
        return rs->why;
    }

    /* the scaled light stopped: keep why, clean up, light it unscaled */
    g_first = *pl; g_first_res = *rs;
    if(out != quiet_emit){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("Scaling stopped it. Clearing pipe B, then lighting it unscaled (as 0.9.6.2 did).\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
    }
    why = clear_b(out, "");
    if(why) return why;
    xm_set_fit(XM_FIT_CENTRE);
    why = plan_light(out, 0);
    if(!why){ xm_apply(&g_dev, pl, rs); xm_report_result(pl, rs, out); why = rs->why; }
    xm_set_fit(keep);
    if(why) clear_b(out, "Clearing what was done, so the next try starts clean.\n");
    else g_fell_back = 1;
    return why;
}

/* "<what> (register XXXXXX: wanted XXXXXXXX, read XXXXXXXX)" for dmesg */
static void say_failure(char *m, int max, const xm_plan_t *p, const xm_result_t *r)
{
    static const char hx[] = "0123456789ABCDEF";
    int n = 0, i; const char *w;
    w = (r->failed_op >= 0 && r->failed_op < p->nops) ? p->ops[r->failed_op].what : xm_why_name(r->why);
    while(*w && n < max - 48) m[n++] = *w++;
    if(r->failed_op >= 0){
        const char *a = ": register ";
        while(*a) m[n++] = *a++;
        for(i = 20; i >= 0; i -= 4) m[n++] = hx[(r->bad_off >> i) & 15];
        a = " wanted "; while(*a) m[n++] = *a++;
        for(i = 28; i >= 0; i -= 4) m[n++] = hx[(r->bad_want >> i) & 15];
        a = " read "; while(*a) m[n++] = *a++;
        for(i = 28; i >= 0; i -= 4) m[n++] = hx[(r->bad_got >> i) & 15];
    }
    m[n] = 0;
}

static void cmd_mode(const char *what)
{
    uint32_t got = 0;
    int on = strcmp(what, "off") != 0;
    if(g_dev.why != GFX_OK){
        vga_puts("No display engine: ");
        vga_puts(gfx_why_name(g_dev.why));
        vga_puts(".\n");
        return;
    }
    xm_read(&g_dev, &g_xs);
    if(on && strcmp(what, "plan")){
        int why = light(L_ASK, emit, &g_xs, &g_plan, &g_res);
        if(why == XM_OK){
            uint32_t h;
            if(g_fell_back){
                char m[160];
                say_failure(m, sizeof m, &g_first, &g_first_res);
                vga_set_color(VGA_YELLOW, VGA_BLACK);
                vga_puts("Lit UNSCALED. Scaling stopped at: "); vga_puts(m);
                vga_puts("\nPhotograph this line.\n");
                vga_set_color(VGA_WHITE, VGA_BLACK);
            }
            sleep_ms(100);
            h = xm_underrun_hits(&g_dev, 20000u);
            vga_set_color(h ? VGA_LIGHT_RED : VGA_LIGHT_GREEN, VGA_BLACK);
            vga_puts("  underrun in "); put_u(h); vga_puts(" of 20000 reads");
            vga_puts(h ? " -- the plane is starving; photograph `gfx b`.\n"
                       : ": the plane keeps up. Look at the monitor.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
        }
        return;
    }
    if(on){
        memset(g_raw, 0, sizeof g_raw);
        memset(&g_edid, 0, sizeof g_edid);
        gmbus_read_edid(&g_dev, hdmi_pin(), g_raw, &got, &g_tr);
        if(got) edid_parse(g_raw, got, &g_edid);
        else g_edid.why = EDID_ESHORT;
        xm_plan_on(&g_xs, got ? &g_edid : 0, hdmi_child(), &g_plan);
    } else {
        xm_plan_off(&g_xs, &g_plan);
    }

    if(!strcmp(what, "plan")){
        xm_report_plan(&g_xs, &g_plan, 1, emit);
        return;
    }
    xm_report_plan(&g_xs, &g_plan, 0, emit);
    if(g_plan.why) return;
    if(on){
        int c;
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("Light the HDMI monitor now? The panel is not touched. y/N ");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        c = keyboard_waitchar();
        vga_puts("\n");
        if(c != 'y' && c != 'Y'){ vga_puts("Nothing was written.\n"); return; }
    }
    ui_page_reset(); g_stop = 0;
    xm_apply(&g_dev, &g_plan, &g_res);
    xm_report_result(&g_plan, &g_res, emit);
    if(g_plan.on && g_res.why == XM_OK){
        /* v0.9.6.2: a rate, not one read -- after the vblank that arms it */
        uint32_t h;
        sleep_ms(100);
        h = xm_underrun_hits(&g_dev, 20000u);
        vga_set_color(h ? VGA_LIGHT_RED : VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("  underrun in "); put_u(h); vga_puts(" of 20000 reads");
        vga_puts(h ? " -- the plane is starving; photograph `gfx b`.\n"
                   : ": the plane keeps up. Look at the monitor.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
    }
}

/* gfx color <red|green|blue|white|off>, v0.9.2.2: pipe B's background,
   the diagnostic for a monitor that locks on and shows black. Then the
   live readout, so one photograph has both. */
static void cmd_color(const char *name)
{
    uint32_t rgb;
    if(!name || !*name) name = "blue";
    if(!strcmp(name, "blue"))       rgb = XM_RGB_BLUE;
    else if(!strcmp(name, "red"))   rgb = XM_RGB_RED;
    else if(!strcmp(name, "green")) rgb = XM_RGB_GREEN;
    else if(!strcmp(name, "white")) rgb = XM_RGB_WHITE;
    else if(!strcmp(name, "off") || !strcmp(name, "black")) rgb = 0;
    else { vga_puts("Usage: gfx color [blue|red|green|white|off]\n"); return; }
    if(g_dev.why != GFX_OK){
        vga_puts("No display engine: ");
        vga_puts(gfx_why_name(g_dev.why));
        vga_puts(".\n");
        return;
    }
    xm_read(&g_dev, &g_xs);
    xm_plan_color(&g_xs, rgb, &g_plan);
    if(g_plan.why){ xm_report_plan(&g_xs, &g_plan, 1, emit); return; }
    xm_apply(&g_dev, &g_plan, &g_res);
    xm_report_result(&g_plan, &g_res, emit);
    xm_report_live(&g_dev, emit);
}

/* gfx try, v0.9.6.1: the HP's monitor shows pipe B's background edge to
   edge, and pipe B underruns. Four trials, each changing one thing on the
   plane and painting the background its own colour, so the photograph
   says which trial it was. The underrun rate is measured for each, which
   needs nobody to look. The snapshot goes back at the end, or at Q. */
static void put_u(uint32_t v)
{
    char b[12]; int n = 0;
    if(!v){ vga_puts("0"); return; }
    while(v){ b[n++] = (char)('0' + v % 10u); v /= 10u; }
    while(n) { char c[2]; c[0] = b[--n]; c[1] = 0; vga_puts(c); }
}
static void cmd_try(void)
{
    static xm_snap_t sn;
    uint32_t hits[XM_TRIALS + 1];
    int n, stop = 0, done = 0;
    const uint32_t READS = 20000u;
    if(g_dev.why != GFX_OK){
        vga_puts("No display engine: "); vga_puts(gfx_why_name(g_dev.why)); vga_puts(".\n");
        return;
    }
    xm_snapshot(&g_dev, &sn);
    if(!sn.lit){ vga_puts("Pipe B's plane is not on -- run `gfx on` first.\n"); return; }

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("gfx try: five changes to pipe B's plane, one at a time\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("Each one paints the monitor's background its own colour. For\n"
             "each: LOOK AT THE MONITOR. Is there a picture (the HavenDOS\n"
             "screen, or a small square), or only the colour? Photograph it.\n"
             "Any key goes on, Q stops; what was there is put back at the end.\n\n");
    hits[0] = xm_underrun_hits(&g_dev, READS);
    vga_puts("  before: underrun in "); put_u(hits[0]); vga_puts(" of 20000 reads\n\n");

    for(n = 1; n <= XM_TRIALS && !stop; n++){
        xm_plan_trial(&sn, n, &g_plan);
        if(g_plan.why){ vga_puts("  refused: "); vga_puts(xm_why_name(g_plan.why)); vga_puts("\n"); break; }
        xm_apply(&g_dev, &g_plan, &g_res);
        if(g_res.why){ xm_report_result(&g_plan, &g_res, emit); break; }
        sleep_ms(100);                          /* past the vblank that arms it */
        hits[n] = xm_underrun_hits(&g_dev, READS);
        done = n;
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  trial "); put_u((uint32_t)n); vga_puts(" -- background ");
        vga_puts(xm_trial_colour(n)); vga_puts("\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        vga_puts("    "); vga_puts(xm_trial_name(n)); vga_puts("\n");
        vga_puts("    underrun in "); put_u(hits[n]); vga_puts(" of 20000 reads\n");
        vga_puts("    Look at the monitor, photograph it, then a key (Q stops). ");
        {
            int c = keyboard_waitchar();
            vga_puts("\n");
            if(c == 'q' || c == 'Q') stop = 1;
        }
    }

    xm_plan_trial(&sn, 0, &g_plan);
    xm_apply(&g_dev, &g_plan, &g_res);
    vga_puts("\n");
    vga_puts(g_res.why ? "  Putting it back FAILED -- `gfx off` then `gfx on`.\n"
                       : "  Put back as it was.\n");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  trial  colour       underrun / 20000\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    for(n = 0; n <= done; n++){
        vga_puts("  "); put_u((uint32_t)n); vga_puts("      ");
        vga_puts(n ? xm_trial_colour(n) : "before");
        { int pad = 13 - (int)strlen(n ? xm_trial_colour(n) : "before"); while(pad-- > 0) vga_puts(" "); }
        put_u(hits[n]); vga_puts("\n");
    }
    vga_puts("  Photograph this table too, with which colours had a picture.\n");
}

void cmd_gfx(const char *arg, const char *arg2)
{
    g_stop = 0;
    ui_page_reset();

    if(arg && *arg && strcmp(arg, "regs") && strcmp(arg, "vbt") &&
       strcmp(arg, "edid") && strcmp(arg, "plan") && strcmp(arg, "on") &&
       strcmp(arg, "off") && strcmp(arg, "color") && strcmp(arg, "b") &&
       strcmp(arg, "try") && strcmp(arg, "fit") && strcmp(arg, "auto")){
        vga_puts("usage: gfx         the display engine, as the firmware left it\n");
        vga_puts("       gfx regs    every register read, offset and value\n");
        vga_puts("       gfx vbt     what the Video BIOS Table says is wired\n");
        vga_puts("       gfx edid [pin]  the monitor's EDID over GMBUS\n");
        vga_puts("       gfx plan    what `gfx on` would write, or why it refuses\n");
        vga_puts("       gfx on      light the HDMI monitor (asks first)\n");
        vga_puts("       gfx off     turn it off again\n");
        vga_puts("       gfx b       pipe B as it runs, read only\n");
        vga_puts("       gfx color [blue|red|green|white|off]  pipe B's background\n");
        vga_puts("       gfx try     five changes to the plane, one at a time (asks)\n");
        vga_puts("       gfx fit [keep|stretch|centre]  how the picture fills the monitor\n");
        vga_puts("       gfx auto [on|off]  light the monitor by itself when plugged in\n");
        vga_puts("Only `gfx on`, `off`, `color` and `try` change what a screen shows.\n");
        return;
    }

    if(arg && (!strcmp(arg, "fit") || !strcmp(arg, "auto"))){
        cmd_fit_auto(arg, arg2);
        return;
    }
    g_gfx_busy = 1;
    gfx_open(&g_dev);
    gfx_read(&g_dev, &g_regs);
    if(g_dev.why == GFX_OK){
        gfx_vbt_find(&g_dev, &g_vbt);
    } else {
        memset(&g_vbt, 0, sizeof g_vbt);
        g_vbt.why = GFX_VBT_NOASLS;
    }

    if(arg && (!strcmp(arg, "plan") || !strcmp(arg, "on") || !strcmp(arg, "off")))
        cmd_mode(arg);
    else if(arg && !strcmp(arg, "color")) cmd_color(arg2);
    else if(arg && !strcmp(arg, "try"))   cmd_try();
    else if(arg && !strcmp(arg, "b")){
        if(g_dev.why != GFX_OK){
            vga_puts("No display engine: ");
            vga_puts(gfx_why_name(g_dev.why));
            vga_puts(".\n");
        } else xm_report_live(&g_dev, emit);
    }
    else if(arg && !strcmp(arg, "edid")) cmd_edid(arg2);
    else if(arg && !strcmp(arg, "regs")) gfx_report_regs(&g_dev, &g_regs, emit);
    else if(arg && !strcmp(arg, "vbt")) gfx_report_vbt(&g_vbt, emit);
    else                                gfx_report(&g_dev, &g_regs, &g_vbt, emit);
    g_gfx_busy = 0;
}

/* ── v0.9.6.3: gfx fit, gfx auto ─────────────────────────────────── */
static void cmd_fit_auto(const char *arg, const char *v)
{
    if(!strcmp(arg, "fit")){
        int f = -1;
        if(v && (!strcmp(v, "keep") || !strcmp(v, "fit")))      f = XM_FIT_KEEP;
        else if(v && (!strcmp(v, "stretch") || !strcmp(v, "fill"))) f = XM_FIT_STRETCH;
        else if(v && (!strcmp(v, "centre") || !strcmp(v, "center") || !strcmp(v, "none")))
            f = XM_FIT_CENTRE;
        else if(v && *v){ vga_puts("gfx fit keep | stretch | centre\n"); return; }
        if(f >= 0){ cfg_set_monitor_fit(f); cfg_save(); xm_set_fit(f); }
        vga_puts("Monitor picture: "); vga_puts(xm_fit_name(xm_get_fit()));
        vga_puts(f >= 0 ? ". Saved; `gfx off` then `gfx on` shows it now.\n" : ".\n");
        return;
    }
    if(v && !strcmp(v, "on"))  { cfg_set_monitor_auto(1); cfg_save(); }
    else if(v && !strcmp(v, "off")){ cfg_set_monitor_auto(0); cfg_save(); }
    else if(v && *v){ vga_puts("gfx auto on | off\n"); return; }
    vga_puts(cfg_monitor_auto() ? "The monitor lights by itself when it is plugged in.\n"
                                : "The monitor is lit only by `gfx on`.\n");
}

/* ── v0.9.6.3: the monitor, by itself ─────────────────────────────────
   A kpoll client, once a second, from the shell's idle points (never
   from an interrupt). It reads two registers: DDI B's hot-plug line
   and whether transcoder B is on. Two reads in a row decide, so a
   connector being pushed in, or a monitor waking, does not flap.

     plugged, not lit, auto on  -> `gfx on`, without the question
     unplugged, lit             -> `gfx off`

   A failed attempt is not repeated until the cable comes out: the same
   EDID or the same refusal would only fail again, every second. What
   happened is in `dmesg` and, on the desktop, a toast. */
static int g_mon_open;
static xm_state_t  g_mxs;
static xm_plan_t   g_mplan;
static xm_result_t g_mres;

int gfx_monitor_light(void)
{
    return light(0, quiet_emit, &g_mxs, &g_mplan, &g_mres);
}

static xm_hp_t g_hp = { -1, 0, 0 };

static void mon_work(void)
{
    int in, act, why;
    if(g_gfx_busy) return;
    if(!g_mon_open){
        g_mon_open = 1;
        gfx_open(&g_dev);
        if(g_dev.why == GFX_OK) gfx_vbt_find(&g_dev, &g_vbt);
    }
    if(g_dev.why != GFX_OK) return;          /* not an Xe: nothing to watch */

    in  = xm_hpd_b(&g_dev);
    act = xm_hotplug_step(&g_hp, in, xm_b_lit(&g_dev), cfg_monitor_auto());
    if(act == XM_HP_LIGHT){
        g_gfx_busy = 1;
        why = gfx_monitor_light();
        g_gfx_busy = 0;
        if(why == XM_OK){
            char m[64]; int n = 0;
            const char *pre = "Monitor on: ", *f = xm_fit_name(g_mplan.fit);
            while(*pre) m[n++] = *pre++;
            while(*f && n < 62) m[n++] = *f++;
            m[n] = 0;
            xm_hotplug_lit(&g_hp, 1);
            klog(KLOG_OK, "monitor: lit by itself, %ux%u, %s",
                 (unsigned)g_mplan.mode.hactive, (unsigned)g_mplan.mode.vactive,
                 xm_fit_name(g_mplan.fit));
            if(g_fell_back){
                char f2[160];
                say_failure(f2, sizeof f2, &g_first, &g_first_res);
                klog(KLOG_WARN, "monitor: scaling stopped at %s; lit unscaled instead", f2);
                toast_warning("Monitor on, unscaled -- `dmesg` says why");
            } else toast_info(m);
        } else {
            /* A monitor still waking can miss its first EDID read */
            char f2[160];
            say_failure(f2, sizeof f2, g_lp, g_lr);
            klog(KLOG_WARN, "monitor: plugged in, not lit: %s: %s (try %d of 3)",
                 xm_why_name(why), f2, g_hp.tries + 1);
            if(xm_hotplug_lit(&g_hp, 0)) toast_warning("Monitor not lit -- `dmesg` says why");
        }
    } else if(act == XM_HP_OFF){
        g_gfx_busy = 1;
        xm_read(&g_dev, &g_mxs);
        xm_plan_off(&g_mxs, &g_mplan);
        if(!g_mplan.why) xm_apply(&g_dev, &g_mplan, &g_mres);
        g_gfx_busy = 0;
        klog(KLOG_INFO, "monitor: unplugged, pipe B off");
    }
}

/* Settings > Display: a new fit, shown now. A monitor that is lit is
   switched off and lit again with it; one that is not just keeps the
   setting for next time. Returns an XM_* code. */
int gfx_monitor_refit(int fit)
{
    int why = XM_OK;
    xm_set_fit(fit);
    cfg_set_monitor_fit(fit); cfg_save();
    if(g_gfx_busy) return XM_OK;
    if(!g_mon_open){ g_mon_open = 1; gfx_open(&g_dev); if(g_dev.why == GFX_OK) gfx_vbt_find(&g_dev, &g_vbt); }
    if(g_dev.why != GFX_OK || !xm_b_lit(&g_dev)) return XM_OK;
    g_gfx_busy = 1;
    xm_read(&g_dev, &g_mxs);
    xm_plan_off(&g_mxs, &g_mplan);
    if(!g_mplan.why) xm_apply(&g_dev, &g_mplan, &g_mres);
    why = g_mplan.why ? g_mplan.why : g_mres.why;
    if(!why) why = gfx_monitor_light();
    g_gfx_busy = 0;
    if(!why && g_fell_back){                      /* lit, but not as asked */
        char f2[160];
        say_failure(f2, sizeof f2, &g_first, &g_first_res);
        klog(KLOG_WARN, "monitor: scaling stopped at %s; lit unscaled instead", f2);
        return XM_EREFUSED;
    }
    return why;
}
int gfx_monitor_lit(void){ return g_dev.why == GFX_OK && xm_b_lit(&g_dev); }

/* From kernel/main.c, once the config is loaded. */
void gfx_monitor_register(void)
{
    extern int kpoll_register(const char *name, uint32_t interval_ms, void (*fn)(void));
    xm_set_fit(cfg_monitor_fit());
    kpoll_register("monitor", 1000, mon_work);  /* kpoll-site-ok: the HDMI watcher, once at boot; g_gfx_busy keeps it off a running `gfx` */
}
