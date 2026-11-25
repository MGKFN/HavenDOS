/* include/wm.h -- the window manager (v0.9.8).
 * drivers/wm_core.inc draws: layers, windows, the pixel primitives.
 * shell/wm.c decides: dragging, focus, the title-bar buttons.
 * shell/taskbar.c and shell/startmenu.c paint through the primitives. */
#ifndef WM_H
#define WM_H
#include "types.h"

#define WM_FLOAT   0        /* a window that can be dragged            */
#define WM_MAXED   1        /* fills the space between title and bar   */
#define WM_TILED   2        /* v0.9.9: snapped to half the screen      */
#define WM_FULL    3        /* v0.9.9: the whole screen, no title bar  */

/* parts of the screen, vga_wm_hit() */
#define WMP_NONE    0
#define WMP_CONTENT 1
#define WMP_TITLE   2
#define WMP_EDGE    3
#define WMP_BAR     4
#define WMP_POPUP   5
#define WMP_MIN     6
#define WMP_MAX     7
#define WMP_CLOSE   8
/* v0.9.9: the edges a floating window is resized by -- a few pixels
   either side of its outline, and the corners where two meet */
#define WMP_RS_N    9
#define WMP_RS_S    10
#define WMP_RS_W    11
#define WMP_RS_E    12
#define WMP_RS_NW   13
#define WMP_RS_NE   14
#define WMP_RS_SW   15
#define WMP_RS_SE   16
#define WMP_IS_RESIZE(p) ((p) >= WMP_RS_N && (p) <= WMP_RS_SE)

/* v0.9.9: the pointer's shapes */
#define PTR_ARROW   0
#define PTR_EW      1       /* <-> */
#define PTR_NS      2
#define PTR_NWSE    3
#define PTR_NESW    4
#define PTR_MOVE    5

/* colour roles, from the theme's palette */
#define WMC_TITLE     0
#define WMC_TITLE_OFF 1
#define WMC_TEXT      2
#define WMC_TEXT_OFF  3
#define WMC_EDGE      4
#define WMC_ACCENT    5
#define WMC_CLOSE     6
#define WMC_PANEL     7

#define WM_BG_GRAD 0xFF000000u     /* "the wallpaper's gradient" as a bg */

typedef void (*wm_paint_fn)(int x0, int y0, int x1, int y1);

void vga_wm_enable(int on);
int  vga_wm_on(void);
void vga_wm_open(int slot, const char *title, const char *icon);
void vga_wm_close(int slot);
void vga_wm_geometry(int slot, int vx, int vy, int vw, int vh, int x, int y, int mode);
/* v0.9.9: a window's content is any size in pixels (pw x ph at x,y), and
   its cells are drawn at a size of the window's own (cw x ch) -- the
   global cell for an app that reflows, bigger or smaller for one that
   zooms. `skip` rows at the top of the viewport (the app's own header,
   which the title bar already says) are not shown. */
void vga_wm_place(int slot, int vx, int vy, int vw, int vh, int skip,
                  int x, int y, int pw, int ph, int cw, int ch, int mode);
void vga_wm_rect(int slot, int *x, int *y, int *pw, int *ph);
int  vga_wm_mode_of(int slot);
int  vga_wm_has_surface(int slot);
void vga_pointer_kind(int kind);
void vga_wm_get(int slot, int *x, int *y, int *w, int *h);
int  vga_wm_is_open(int slot);
int  vga_wm_is_min(int slot);
void vga_wm_move(int slot, int x, int y);
void vga_wm_minimize(int slot, int on);
void vga_wm_raise(int slot);
void vga_wm_front(int slot);
int  vga_wm_front_slot(void);
void vga_wm_title(int slot, const char *title);
const char *vga_wm_title_of(int slot);
int  vga_wm_icon_of(int slot);
void vga_wm_buttons(int slot, int hover, int down);
void vga_wm_solo(int on);
int  vga_wm_solo_on(void);
void vga_wm_full(int slot, int on);
void vga_wm_desk_icons_begin(void);
void vga_wm_desk_icon(int x0, int y0, int x1, int y1, const char *icon, const char *name, int sel);
void vga_wm_desk_icons_end(void);
void vga_wm_set_bar(wm_paint_fn fn);
void vga_wm_repaint_bar(void);
void vga_wm_set_popup(int x, int y, int w, int h, wm_paint_fn fn);
void vga_wm_repaint_popup(int x0, int y0, int x1, int y1);
void vga_wm_repaint_rect(int x0, int y0, int x1, int y1);
void vga_wm_repaint_all(void);
void vga_wm_repaint_title(int slot);
int  vga_wm_hit(int x, int y, int *slot, int *part);
int  vga_wm_cur_map(int x, int y, int *cx, int *cy);
int  vga_wm_click_is_app(int x, int y);

int  vga_wm_title_h(void);
int  vga_wm_title_rows(void);
int  vga_wm_bar_h(void);
int  vga_wm_font_px(void);
int  vga_screen_w(void);
void vga_wm_reach(int *x0, int *x1);
int  vga_screen_h(void);
int  vga_cell_w(void);
int  vga_cell_h(void);
void vga_mouse_px(int col, int row, int fx, int fy, int *x, int *y);

/* drawing, inside a paint callback (clipped to its layer) */
uint32_t wm_rgb(int role);
int  wm_icon_find(const char *name);
int  px_icon_size(int want);
void px_icon(int x, int y, int icon, int size, uint32_t bg);
int  px_text(int x, int y, const char *s, int px, int bold, uint32_t fg, uint32_t bg, int maxw);
int  px_text_w(const char *s, int px, int bold);
int  px_text_h(int px, int bold);
void px_fill(int x, int y, int w, int h, uint32_t c);
void px_round(int x, int y, int w, int h, int r, uint32_t c, uint32_t bg);
void px_line(int x0, int y0, int x1, int y1, int t, uint32_t c, uint32_t bg);
/* v0.9.9: paint straight onto the screen with the primitives above, with
   no window manager -- the boot splash. 1 begins, 0 ends, 2 ends and
   hands the whole screen back to the cells. */
void vga_px_direct(int on);
uint32_t vga_px_read(int x, int y);
#endif
