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
}

void cmd_gfx(const char *arg, const char *arg2)
{
    g_stop = 0;
    ui_page_reset();

    if(arg && *arg && strcmp(arg, "regs") && strcmp(arg, "vbt") &&
       strcmp(arg, "edid") && strcmp(arg, "plan") && strcmp(arg, "on") &&
       strcmp(arg, "off")){
        vga_puts("usage: gfx         the display engine, as the firmware left it\n");
        vga_puts("       gfx regs    every register read, offset and value\n");
        vga_puts("       gfx vbt     what the Video BIOS Table says is wired\n");
        vga_puts("       gfx edid [pin]  the monitor's EDID over GMBUS\n");
        vga_puts("       gfx plan    what `gfx on` would write, or why it refuses\n");
        vga_puts("       gfx on      light the HDMI monitor (asks first)\n");
        vga_puts("       gfx off     turn it off again\n");
        vga_puts("Only `gfx on` and `gfx off` change what a screen shows.\n");
        return;
    }

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
    else if(arg && !strcmp(arg, "edid")) cmd_edid(arg2);
    else if(arg && !strcmp(arg, "regs")) gfx_report_regs(&g_dev, &g_regs, emit);
    else if(arg && !strcmp(arg, "vbt")) gfx_report_vbt(&g_vbt, emit);
    else                                gfx_report(&g_dev, &g_regs, &g_vbt, emit);
}
