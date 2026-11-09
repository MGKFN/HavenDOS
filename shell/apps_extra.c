/* ================================================================
   shell/apps_extra.c  --  desktop wrappers for shell-only features
   ================================================================

   Why this file exists
   --------------------
   A lot of HavenDOS was only ever reachable by typing a command: the
   bpkg package apps (paint, music, calendar, quiz, hacker, sysinfo+,
   fortune, ascii-art), the hardware probes (lspci, usb), the kernel
   boot log, the self test, neofetch, BOOT Chat, the guide, the manual
   and the legal documents.  None of them had a desktop entry, so on a
   machine driven from the desktop they did not exist.

   Each wrapper here does the one thing those commands cannot do for
   themselves: survive being called from desktop_run_app(), which
   clears the screen immediately after the app returns.  A command that
   prints a page and returns would have its output erased before it was
   read, so the wrappers that drive a one-shot printer end with
   appx_pause().  The ones that already own the screen until a key is
   pressed (paint, quiz, chat, the guide) do not pause -- a second
   "press any key" after their own exit prompt reads as a bug.

   House rule: new shell commands go in their own file.  These are not
   new commands -- they are new *entry points* to existing ones -- but
   the same reasoning applies, and keeping them out of desktop.c keeps
   that file's diff to the registry and the menu.
   ================================================================ */

#include "types.h"
#include "vga.h"
#include "theme.h"

extern int  keyboard_waitchar(void);
extern void toast_clear(void);

/* The things being wrapped, all already public except where noted. */
extern void run_package_app(const char *id, const char *args);
extern void cmd_lspci(const char *arg1);
extern void cmd_usb(const char *arg1);
extern void cmd_selftest_v6(const char *arg1);
extern void cmd_neofetch_v2(void);
extern void cmd_bootchat(const char *arg1);
extern void cmd_man(const char *cmd);
extern void cmd_guide(void);              /* un-static'd in shell.c */
extern void cmd_dmesg_pager(void);
extern int  legal_show_tos(void);
extern int  legal_show_privacy(void);
extern void screensaver_run_stars(void);
extern int  ui_theme_count(void);
extern const char *ui_theme_name(int n);
extern void ui_set_theme(int n);
extern int  ui_current_theme(void);

/* ---------------------------------------------------------------
   appx_pause -- hold a finished page on screen until a key.
   Drawn on the bottom row of the ACTIVE mode, not a hardcoded 24,
   so it does not land mid-screen at 80x50 or 90x60.
   --------------------------------------------------------------- */
static void appx_pause(void)
{
    int y = vga_rows() - 1;
    vga_puts_at("  Press any key to return to the desktop",
                0, y, VGA_DARK_GREY, VGA_BLACK);
    vga_flush();
    keyboard_waitchar();
}

/* ---------------------------------------------------------------
   appx_ask -- one-line text prompt, for the two apps that need an
   argument the command line used to supply.  Returns 0 if the user
   pressed ESC or entered nothing.
   --------------------------------------------------------------- */
static int appx_ask(const char *title, const char *hint, char *out, int outsz)
{
    theme_t *th = ui_theme();
    int w = 54, h = 7;
    int x = (vga_cols() - w) / 2;
    int y = (vga_rows() - h) / 2;

    ui_draw_window(x, y, w, h, title);
    vga_puts_at(hint, x + 2, y + 2, th->win_fg, th->win_bg);
    vga_puts_at("ESC cancels", x + 2, y + h - 2, VGA_DARK_GREY, th->win_bg);

    int n = 0;
    out[0] = 0;
    while (1) {
        /* input field */
        for (int i = 0; i < w - 4; i++)
            vga_putchar_at(' ', x + 2 + i, y + 4, th->win_fg, th->icon_bg);
        vga_puts_at(out, x + 2, y + 4, th->win_fg, th->icon_bg);
        vga_putchar_at('_', x + 2 + n, y + 4, th->accent, th->icon_bg);
        vga_flush();

        int c = keyboard_waitchar();
        if (c == 27) { out[0] = 0; return 0; }
        if (c == '\n' || c == '\r') return n > 0;
        if (c == '\b') { if (n > 0) out[--n] = 0; continue; }
        if (c >= 32 && c < 127 && n < outsz - 1) { out[n++] = (char)c; out[n] = 0; }
    }
}

/* ================================================================
   bpkg package apps
   ================================================================
   run_package_app() is how the shell reaches these, and it is the
   only entry point, so the desktop uses exactly the same one -- a
   package that gains behaviour gains it in both places at once. */

void appx_paint(void)    { vga_clear(); run_package_app("paint", ""); }
void appx_quiz(void)     { vga_clear(); run_package_app("quiz", ""); }
void appx_hacker(void)   { vga_clear(); run_package_app("hacker", ""); toast_clear(); }
void appx_music(void)    { vga_clear(); run_package_app("music", ""); }

void appx_calendar(void)
{
    vga_clear();
    run_package_app("calendar", "");
    appx_pause();
}

void appx_sysinfo_plus(void)
{
    vga_clear();
    run_package_app("sysinfo-plus", "");
    appx_pause();
}

void appx_fortune(void)
{
    vga_clear();
    ui_draw_taskbar("Fortune");
    vga_set_cursor(0, 2);
    run_package_app("fortune", "");
    appx_pause();
}

void appx_ascii_art(void)
{
    char text[40];
    vga_clear();
    ui_draw_taskbar("ASCII Art");
    if (!appx_ask("ASCII Art", "Text to render as a big banner:", text, sizeof text))
        return;
    vga_clear();
    vga_set_cursor(0, 1);
    run_package_app("ascii-art", text);
    appx_pause();
}

/* ================================================================
   Hardware and diagnostics
   ================================================================
   These print a page and return, so every one of them pauses.  The
   verbose forms are the useful ones from a desktop -- someone opening
   "PCI Devices" from a menu wants the decoded list, not the terse one
   that exists so `lspci` is short to type. */

void appx_pci(void)
{
    vga_clear();
    vga_set_cursor(0, 0);
    cmd_lspci("-v");
    appx_pause();
}

void appx_usb(void)
{
    vga_clear();
    vga_set_cursor(0, 0);
    cmd_usb("-v");
    appx_pause();
}

void appx_bootlog(void)
{
    vga_clear();
    vga_set_cursor(0, 0);
    cmd_dmesg_pager();          /* the shell's own pager, not a copy */
    appx_pause();
}

void appx_selftest(void)
{
    vga_clear();
    vga_set_cursor(0, 0);
    cmd_selftest_v6("");
    appx_pause();
}

void appx_neofetch(void)
{
    vga_clear();
    vga_set_cursor(0, 0);
    cmd_neofetch_v2();
    appx_pause();
}

/* ================================================================
   Comms, documentation and the rest
   ================================================================ */

void appx_chat(void)
{
    vga_clear();
    cmd_bootchat("");           /* owns the screen until the user quits */
}

void appx_guide(void)
{
    vga_clear();
    vga_set_cursor(0, 0);
    cmd_guide();                /* paged, waits for a key per page */
}

void appx_manual(void)
{
    char cmd[24];
    vga_clear();
    ui_draw_taskbar("Manual");
    if (!appx_ask("Manual", "Command to look up (try: ls, usb, chat):",
                  cmd, sizeof cmd))
        return;
    vga_clear();
    vga_set_cursor(0, 0);
    cmd_man(cmd);
    appx_pause();
}

void appx_legal(void)
{
    vga_clear();
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts_at("Legal", 36, 10, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_at("1. Terms of Service", 31, 12, VGA_WHITE, VGA_BLACK);
    vga_puts_at("2. Privacy Policy",   31, 13, VGA_WHITE, VGA_BLACK);
    vga_puts_at("ESC. Back",           31, 15, VGA_DARK_GREY, VGA_BLACK);
    vga_flush();
    int c = keyboard_waitchar();
    if (c == '1') legal_show_tos();
    if (c == '2') legal_show_privacy();
}

void appx_stars(void)
{
    vga_clear();
    screensaver_run_stars();
    vga_clear();
    toast_clear();
}

/* ---------------------------------------------------------------
   Theme picker.  Settings has one buried a couple of panes deep;
   this is the same list reachable in one click, and it previews as
   you move so the choice is visible before it is committed.
   --------------------------------------------------------------- */
void appx_themes(void)
{
    int n     = ui_theme_count();
    int sel   = ui_current_theme();
    if (sel < 0 || sel >= n) sel = 0;
    /* v0.8.5.6: captured AFTER the clamp. It used to be taken before,
       so an out-of-range current theme left `start` out of range too,
       and ui_set_theme() -- which range-checks and does nothing --
       silently left the previewed theme applied on cancel. */
    int start = sel;

    while (1) {
        theme_t *th = ui_theme();
        vga_clear();
        ui_draw_taskbar("Themes");
        int w = 34, h = n + 6;
        if (h > vga_rows() - 3) h = vga_rows() - 3;
        int x = (vga_cols() - w) / 2, y = 2;
        ui_draw_window(x, y, w, h, "Theme");

        for (int i = 0; i < n && i < h - 5; i++) {
            int s = (i == sel);
            vga_color_t bg = s ? th->accent : th->win_bg;
            vga_color_t fg = s ? th->win_bg : th->win_fg;
            char row[32];
            int j = 0;
            const char *nm = ui_theme_name(i);
            row[j++] = s ? '>' : ' ';
            row[j++] = ' ';
            while (*nm && j < w - 4) row[j++] = *nm++;
            while (j < w - 4) row[j++] = ' ';
            row[j] = 0;
            vga_puts_at(row, x + 2, y + 2 + i, fg, bg);
        }
        vga_puts_at("Enter applies  ESC cancels",
                    x + 2, y + h - 2, VGA_DARK_GREY, th->win_bg);
        vga_flush();

        int c = keyboard_waitchar();
        if (c == 0x148 && sel > 0)      { sel--; ui_load_preview(sel); }
        else if (c == 0x150 && sel < n - 1) { sel++; ui_load_preview(sel); }
        else if (c == '\n' || c == '\r') { ui_set_theme(sel); return; }
        /* Cancel is in-memory: ui_set_theme() would write THEME_FILE,
           so backing out of a theme picker used to cost a disk write. */
        else if (c == 27)                { ui_load_preview(start); return; }
    }
}
