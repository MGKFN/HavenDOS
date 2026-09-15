#ifndef THEME_H
#define THEME_H
#include "vga.h"
typedef struct {
    vga_color_t bar_bg, bar_fg, desk_bg, win_bg, win_fg,
                title_bg, title_fg, accent, icon_bg, icon_fg;
    const char *name;
} theme_t;
theme_t *ui_theme(void);
void     ui_set_theme(int n);
int      ui_theme_count(void);
int      ui_current_theme(void);
const char *ui_theme_name(int n);
void ui_draw_taskbar(const char *title);
void ui_draw_desktop_bars(void);
void ui_draw_desktop(void);
void ui_draw_window(int x, int y, int w, int h, const char *title);
void ui_draw_cursor(void);
void ui_erase_cursor(void);
void ui_draw_corner_clock(void);
void ui_toggle_desk_pattern(void);
int  ui_desk_pattern(void);
void ui_load_preview(int n);
void ui_force_cursor_redraw(void);
void ui_unlock_extra_themes(void);
int  ui_extra_themes_unlocked(void);
#endif


