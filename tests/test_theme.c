/* ════════════════════════════════════════════════════════════════════
   test_theme.c -- previewing a theme is not choosing one.

   The Settings Themes tab committed the theme you were hovering over
   when you pressed ESC. Not a cosmetic bug: ESC is the cancel key, and
   it wrote .theme to disk.

   The mechanism is a trap in the API rather than a typo in the caller,
   which is why this suite is over the API. ui_load_preview() overwrites
   cur_theme, so AFTER a preview ui_current_theme() returns the PREVIEWED
   theme -- and the obvious-looking restore,

       ui_set_theme(ui_current_theme());

   reads that clobbered value and commits it. Two of the three theme
   pickers in this tree wrote exactly that. shell/apps_extra.c saved the
   entry value first and got it right; shell/desktop.c and shell/setup.c
   did not.

   So the checks below pin three things:

     - what preview does and does not do (change the theme; touch no file)
     - that the trap is real, so nobody "simplifies" a caller back into it
     - that the correct cancel restores without writing

   shell/ui.c is COMPILED here, not copied. The stubs are only the VGA,
   keyboard, clock and vfs edges; every line of theme logic under test is
   the shipping line. vfs_write is an in-memory file that counts writes,
   which is how "no disk write" is checked rather than asserted.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-56s got %ld\n", what, got); }
}

#include "../include/vga.h"

/* ── the in-memory .theme file ────────────────────────────────────── */
#define FAKEFS 64
static char g_file[FAKEFS];
static int  g_filelen = -1;        /* -1 = the file does not exist */
static int  g_writes;              /* how many times it was written */
static int  g_extra_installed;     /* what bpkg_is_installed reports */

int vfs_write(const char *p, const char *d, uint32_t n)
{
    (void)p;
    g_writes++;
    if(n > FAKEFS - 1) n = FAKEFS - 1;
    memcpy(g_file, d, (size_t)n);
    g_file[n] = 0;
    g_filelen = (int)n;
    return (int)n;
}
int vfs_read(const char *p, char *out, uint32_t max)
{
    (void)p;
    if(g_filelen < 0) return -1;
    uint32_t n = (uint32_t)g_filelen < max - 1 ? (uint32_t)g_filelen : max - 1;
    memcpy(out, g_file, (size_t)n);
    out[n] = 0;
    return (int)n;
}

/* ── the edges ui.c draws and reads through ───────────────────────── */
void vga_putchar_at(char c,int x,int y,vga_color_t f,vga_color_t b)
{ (void)c;(void)x;(void)y;(void)f;(void)b; }
void vga_puts_at(const char *s,int x,int y,vga_color_t f,vga_color_t b)
{ (void)s;(void)x;(void)y;(void)f;(void)b; }
void vga_puts(const char *s){ (void)s; }
void vga_set_color(vga_color_t f,vga_color_t b){ (void)f;(void)b; }
int  vga_cols(void){ return 80; }
int  vga_rows(void){ return 25; }
uint16_t vga_shadow_read(int x,int y){ (void)x;(void)y; return 0; }
void vga_shadow_write(int x,int y,uint16_t v){ (void)x;(void)y;(void)v; }
int  keyboard_waitchar(void){ return 27; }
void mouse_get(int *x,int *y,int *b){ if(x)*x=0; if(y)*y=0; if(b)*b=0; }
int  mouse_is_ready(void){ return 0; }
/* v0.8.9.6: shell/ui.c's ui_idle() waits and listens for keys. */
void sleep_ms(uint32_t ms){ (void)ms; }
int  keyboard_haschar(void){ return 0; }
void rtc_get_time(uint8_t *h,uint8_t *m,uint8_t *s){ *h=12;*m=0;*s=0; }
void rtc_get_date(uint8_t *d,uint8_t *mo,uint16_t *y){ *d=1;*mo=1;*y=2026; }
void vga_fill_row(int y,char c,vga_color_t f,vga_color_t b){ for(int x=0;x<80;x++) vga_putchar_at(c,x,y,f,b); }
const char *current_username(void){ return "admin"; }
int  current_user_admin(void){ return 1; }
int  notif_get_unread(void){ return 0; }
int         desktop_tbpin_count(void){ return 0; }      /* v0.9.7: the taskbar's pins */
const char *desktop_tbpin_icon(int i){ (void)i; return "??"; }
int  bpkg_is_installed(const char *n){ (void)n; return g_extra_installed; }
/* v0.9.5: the wallpaper switch lives in kernel/config.c. */
static int g_cfg_pattern = 1, g_cfg_saves;
int  cfg_desk_pattern(void){ return g_cfg_pattern; }
void cfg_set_desk_pattern(int v){ g_cfg_pattern = v; }
void cfg_save(void){ g_cfg_saves++; }

/* ── the code under test, as it ships ─────────────────────────────── */
#include "taskstub.inc"     /* v0.9.6: tasks off */
#include "../shell/ui.c"
/* v0.9.6.5: not linked here */
int mouse_outside(void){ return 0; }
/* v0.9.6.3: shell/uimouse.c is not linked here */
void um_frame(void){}
void um_row(int g,int x,int y,int w,int s){ (void)g;(void)x;(void)y;(void)w;(void)s; }

static void fs_reset(void){ g_filelen = -1; g_writes = 0; g_file[0] = 0; }

int main(void)
{
    int n = ui_theme_count();

    head("the theme table is sane");
    ck("there is more than one theme",      n > 1, n);
    ck("the current theme is in range",
       ui_current_theme() >= 0 && ui_current_theme() < n, ui_current_theme());
    ck("every theme has a name",
       ui_theme_name(0)[0] && ui_theme_name(n-1)[0] &&
       strcmp(ui_theme_name(0), "?") && strcmp(ui_theme_name(n-1), "?"), 0);
    ck("an out-of-range name is '?'", !strcmp(ui_theme_name(n), "?"), 0);
    ck("a negative name is '?'",      !strcmp(ui_theme_name(-1), "?"), 0);

    head("preview changes the theme and writes nothing");
    fs_reset();
    ui_set_theme(0);                       /* a known starting point */
    {
        int writes_after_set = g_writes;
        ck("ui_set_theme wrote the file", writes_after_set == 1, writes_after_set);

        g_writes = 0;
        ui_load_preview(n - 1);
        ck("preview changed the active theme",
           ui_current_theme() == n - 1, ui_current_theme());
        ck("preview wrote nothing",       g_writes == 0, g_writes);

        ui_load_preview(1);
        ck("previewing again still writes nothing", g_writes == 0, g_writes);
        ck("and the active theme followed", ui_current_theme() == 1,
           ui_current_theme());
    }

    head("an out-of-range preview is ignored, not clamped");
    {
        ui_load_preview(2);
        int before = ui_current_theme();
        g_writes = 0;
        ui_load_preview(n);                /* one past the end */
        ck("past the end leaves the theme alone",
           ui_current_theme() == before, ui_current_theme());
        ui_load_preview(-1);
        ck("negative leaves the theme alone",
           ui_current_theme() == before, ui_current_theme());
        ck("and neither wrote anything",   g_writes == 0, g_writes);

        g_writes = 0;
        ui_set_theme(n);
        ui_set_theme(-1);
        ck("an out-of-range ui_set_theme writes nothing either",
           g_writes == 0, g_writes);
        ck("and does not move the theme",
           ui_current_theme() == before, ui_current_theme());
    }

    head("THE TRAP: after a preview, ui_current_theme() is the preview");
    {
        /* This is the bug, written down. shell/desktop.c cancelled with
           ui_set_theme(ui_current_theme()) and that is not a restore --
           it is a commit of whatever the cursor was resting on. The
           check exists so that a future "simplification" back to that
           idiom fails here instead of on somebody's laptop. */
        fs_reset();
        ui_set_theme(0);
        int entry = ui_current_theme();
        ck("entry theme is 0", entry == 0, entry);

        ui_load_preview(3 < n ? 3 : n - 1);
        int previewed = ui_current_theme();
        ck("the preview moved the active theme", previewed != entry, previewed);
        ck("ui_current_theme() now reports the PREVIEW, not the entry value",
           previewed == (3 < n ? 3 : n - 1), previewed);

        /* The wrong restore. */
        g_writes = 0;
        ui_set_theme(ui_current_theme());
        ck("the old 'restore' idiom writes to disk", g_writes == 1, g_writes);
        ck("and commits the PREVIEW rather than the entry theme",
           ui_current_theme() == previewed, ui_current_theme());
    }

    head("the correct cancel: restore the saved entry value, in memory");
    {
        fs_reset();
        ui_set_theme(2 < n ? 2 : 0);
        int entry = ui_current_theme();    /* saved BEFORE any preview */
        ui_load_preview(n - 1);
        g_writes = 0;
        ui_load_preview(entry);            /* what a cancel must do */
        ck("the entry theme is restored",
           ui_current_theme() == entry, ui_current_theme());
        ck("and the cancel wrote nothing", g_writes == 0, g_writes);
    }

    head("applying a theme round-trips through the file");
    {
        for(int i = 0; i < n; i++){
            fs_reset();
            ui_set_theme(i);
            /* Forget it in memory, then reload the way boot does. */
            ui_load_preview(0);
            ui_load_theme();
            if(ui_current_theme() != i){
                ck("every theme index survives a save/load", 0, i);
                break;
            }
            if(i == n - 1)
                ck("every theme index survives a save/load", 1, 0);
        }
        /* v0.9.5: saved by name, so reordering the table can never swap
           someone's theme again. */
        fs_reset();
        ui_set_theme(4);
        ck("the file holds the theme's name",
           strncmp(g_file, ui_theme_name(4), strlen(ui_theme_name(4))) == 0, g_filelen);
        ck("and not a number", !(g_file[0] >= '0' && g_file[0] <= '9'), g_file[0]);
        ck("the base table is twelve themes", n == 12, n);
    }

    head("a number from 0.9.4 or older is migrated once, by meaning");
    {
        static const struct { const char *old; const char *want; } mig[] = {
            {"0\n", "Midnight"},   /* Midnight Cyan  */
            {"1\n", "Haven"},      /* Classic Blue   */
            {"2\n", "Terminal"},
            {"3\n", "Amber CRT"},
            {"4\n", "Slate"},      /* Dark           */
            {"7\n", "Forest"},     /* Mint           */
            {"9\n", "Retro"},
            {"5\n", "Haven"},      /* Arctic: an extra now, not unlocked */
            {"6\n", "Midnight"},   /* Neon: likewise                     */
        };
        int ok = 1, bad = -1;
        for(int i = 0; i < (int)(sizeof mig / sizeof mig[0]); i++){
            fs_reset();
            memcpy(g_file, mig[i].old, strlen(mig[i].old));
            g_filelen = (int)strlen(mig[i].old);
            ui_load_preview(0);
            ui_load_theme();
            if(strcmp(ui_theme_name(ui_current_theme()), mig[i].want)){ ok = 0; bad = i; break; }
            if(g_file[0] >= '0' && g_file[0] <= '9'){ ok = 0; bad = 100 + i; break; }
        }
        ck("each old number lands on the theme most like it, and the file "
           "is rewritten by name", ok, bad);
        fs_reset();
        memcpy(g_file, "Nonsense\n", 9); g_filelen = 9;
        ui_load_preview(2);
        ui_load_theme();
        ck("an unknown name leaves the theme alone", ui_current_theme() == 2,
           ui_current_theme());
    }

    head("every theme is a whole look, and a readable one");
    {
        int all = NTHEMES_ALL, bg_ok = 1, fg_ok = 1, pat_ok = 1, names = 1;
        for(int i = 0; i < all; i++){
            const theme_t *t = ui_theme_get(i);
            /* 8-15 as a background blinks in VGA text mode. */
            if(t->desk_bg > 7 || t->bar_bg > 7 || t->win_bg > 7) bg_ok = 0;
            if(t->desk_fg == t->desk_bg || t->bar_fg == t->bar_bg) fg_ok = 0;
            /* Window titles are drawn title_fg on win_bg (ui_draw_window),
               the clock accent on bar_bg, icon dots icon_bg on desk_bg.
               Haven shipped a black title on a black window in the first
               0.9.5 build; the screenshot caught it, this keeps it caught. */
            if(t->title_fg == t->win_bg || t->win_fg == t->win_bg ||
               t->accent == t->bar_bg || t->icon_bg == t->desk_bg) fg_ok = 0;
            if(t->pat_kind != UI_PAT_NONE){
                int cells = 0;
                for(int y = 1; y < 24; y++) for(int x = 0; x < 80; x++){
                    char c; vga_color_t fg;
                    if(ui_pattern_at_theme(i, x, y, &c, &fg)){
                        cells++;
                        if(fg == t->desk_bg) pat_ok = 0;
                    }
                }
                if(cells < 8 || cells > 80*23/3) pat_ok = 0;
            }
            for(int j = 0; j < i; j++) if(!strcmp(ui_theme_get(j)->name, t->name)) names = 0;
        }
        ck("no background is a blinking colour (8-15)", bg_ok, 0);
        ck("every text colour differs from what it is drawn on", fg_ok, 0);
        ck("every wallpaper draws something, visibly, and leaves room", pat_ok, 0);
        ck("every name is different", names, 0);
        int desks = 0;
        for(int i = 0; i < NTHEMES_BASE; i++)
            for(int j = 0; j <= i; j++)
                if(j == i){ desks++; }
                else if(ui_theme_get(j)->desk_bg == ui_theme_get(i)->desk_bg) break;
        ck("the base themes use at least five desktop colours", desks >= 5, desks);

        int was = ui_desk_pattern(), saves = g_cfg_saves;
        ui_set_desk_pattern(0);
        int any = 0;
        for(int y = 1; y < 24; y++) for(int x = 0; x < 80; x++){
            char c; vga_color_t fg; if(ui_pattern_at(x, y, &c, &fg)) any = 1;
        }
        ck("wallpaper off draws nothing", !any, any);
        ck("and the switch is saved", g_cfg_saves == saves + 1 && g_cfg_pattern == 0, g_cfg_saves);
        ui_set_desk_pattern(was);
    }

    head("extra themes are gated until the package is installed");
    {
        /* g_extra_themes is sticky by design (a package does not
           uninstall), so this is checked last: NTHEMES_BASE is only
           observable before anything unlocks. */
        ck("the base count is less than the full table",
           NTHEMES_BASE < NTHEMES_ALL, NTHEMES_BASE);
        int base = ui_theme_count();
        ui_unlock_extra_themes();
        ck("unlocking widens the list", ui_theme_count() > base,
           ui_theme_count());
        ck("and reports itself unlocked", ui_extra_themes_unlocked() == 1, 0);

        int all = ui_theme_count();
        fs_reset();
        ui_set_theme(all - 1);
        ck("the last extra theme can now be applied",
           ui_current_theme() == all - 1, ui_current_theme());
        ui_load_preview(0);
        ui_load_theme();
        ck("and it survives a save/load", ui_current_theme() == all - 1,
           ui_current_theme());

        ck("there are more than twelve themes once unlocked", all > 12, all);
        {
            int ok = 1;
            for(int i = 12; i < all; i++){
                fs_reset();
                ui_set_theme(i);
                ui_load_preview(0);
                ui_load_theme();
                if(ui_current_theme() != i){ ok = 0; break; }
            }
            ck("every extra survives a save/load by name", ok, 0);
        }
        fs_reset();
        memcpy(g_file, "12\n", 3); g_filelen = 3;
        ui_load_preview(0);
        ui_load_theme();
        ck("an old two-digit number migrates too (12, Ocean)",
           !strcmp(ui_theme_name(ui_current_theme()), "Ocean"), ui_current_theme());
        fs_reset();
        memcpy(g_file, "5\n", 2); g_filelen = 2;
        ui_load_preview(0);
        ui_load_theme();
        ck("and old Arctic is Arctic again once the extras exist",
           !strcmp(ui_theme_name(ui_current_theme()), "Arctic"), ui_current_theme());
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
