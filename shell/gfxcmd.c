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

   Each run reads the registers afresh; the mapping of BAR0 is made
   once and kept (drivers/xedisp.c says why). Everything printed comes
   through the pager, because the summary is longer than the HP's
   screen is tall.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/xedisp.h"
#include "../include/ui.h"

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

void cmd_gfx(const char *arg)
{
    g_stop = 0;
    ui_page_reset();

    if(arg && *arg && strcmp(arg, "regs") && strcmp(arg, "vbt")){
        vga_puts("usage: gfx         the display engine, as the firmware left it\n");
        vga_puts("       gfx regs    every register read, offset and value\n");
        vga_puts("       gfx vbt     what the Video BIOS Table says is wired\n");
        vga_puts("Read only. Nothing is written to the GPU.\n");
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

    if(arg && !strcmp(arg, "regs"))     gfx_report_regs(&g_dev, &g_regs, emit);
    else if(arg && !strcmp(arg, "vbt")) gfx_report_vbt(&g_vbt, emit);
    else                                gfx_report(&g_dev, &g_regs, &g_vbt, emit);
}
