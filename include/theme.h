#ifndef THEME_H
#define THEME_H
#include "vga.h"
typedef struct {
    vga_color_t bar_bg, bar_fg, desk_bg, win_bg, win_fg,
                title_bg, title_fg, accent, icon_bg, icon_fg;
    const char *name;
    /* v0.9.5. Appended, so code that only knows the fields above (toast.c,
       the host tests) still reads them at the right offsets. */
    vga_color_t desk_fg;    /* text drawn straight on the desktop        */
    uint8_t     pat_kind;   /* UI_PAT_*                                  */
    char        pat_ch;     /* the wallpaper's character                 */
    vga_color_t pat_fg;     /* and its colour, on desk_bg                */
} theme_t;

/* Wallpaper shapes. One function draws them (ui_pattern_at), so the
   login screen and the desktop cannot disagree about a theme's look --
   before 0.9.5 they had separate switch statements that did. */
enum { UI_PAT_NONE = 0, UI_PAT_GRID, UI_PAT_RAIN, UI_PAT_HATCH, UI_PAT_WAVE,
       UI_PAT_TREES, UI_PAT_BANDS, UI_PAT_SCATTER, UI_PAT_COLUMNS, UI_PAT_CHECK };

/* The wallpaper cell at (x, y): 1 and *c / *fg set if something is drawn
   there, 0 for plain background. Honours the pattern on/off setting. */
int  ui_pattern_at(int x, int y, char *c, vga_color_t *fg);
/* Same, for theme n regardless of the setting (Settings' preview). */
int  ui_pattern_at_theme(int n, int x, int y, char *c, vga_color_t *fg);
const theme_t *ui_theme_get(int n);
int  ui_theme_base_count(void);     /* the themes every install has      */
int  ui_theme_total(void);          /* including themes-extra's         */
void ui_set_desk_pattern(int on);   /* saved; 1 by default               */
theme_t *ui_theme(void);
void     ui_set_theme(int n);
int      ui_theme_count(void);
int      ui_current_theme(void);
const char *ui_theme_name(int n);
void ui_draw_taskbar(const char *title);
void ui_draw_desktop_bars(void);
void ui_draw_desktop(void);
void ui_draw_window(int x, int y, int w, int h, const char *title);
/* v0.9.5.1: the shared look (shell/ui.c, "THE 0.9.5.1 LOOK"). */
void ui_sel_colors(vga_color_t *fg, vga_color_t *bg);
void ui_kit_begin(const char *title, const char *keys);
void ui_kit_footer(const char *keys);
void ui_kit_footer_at(int y, const char *keys);
void ui_kit_panel(int x,int y,int w,int h,const char *title);
void ui_kit_heading(int x,int y,const char *s);
void ui_kit_kv(int x,int y,int vx,const char *label,const char *value);
void ui_kit_row(int x,int y,int w,int sel,const char *text);
void ui_kit_cell(int x,int y,vga_color_t c);
void ui_kit_bar(int x,int y,int w,int filled,vga_color_t c);
void ui_draw_cursor(void);
void ui_erase_cursor(void);
void ui_draw_corner_clock(void);
int  ui_taskbar_notif_hit(int x, int y);
void ui_toggle_desk_pattern(void);
int  ui_desk_pattern(void);
void ui_load_preview(int n);
void ui_force_cursor_redraw(void);
void ui_idle(uint32_t ms);   /* v0.8.9.6: a wait that keeps reading the pointer */
void ui_unlock_extra_themes(void);
int  ui_extra_themes_unlocked(void);
#endif


