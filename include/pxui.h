/* include/pxui.h -- the pixel building blocks (shell/pxui.c), v0.9.10.
 *
 * Immediate mode: every frame the app draws its controls again and each
 * call says, from this frame's input, whether it was used. Drawn with the
 * px_* primitives into a canvas -- a pixel app's window (pxui_begin) or
 * any buffer -- in the theme's colours. */
#pragma once
#include "types.h"

typedef struct { int x, y, w, h; } prect_t;

typedef struct {
    uint32_t bg, panel, raised, field, text, muted, border, accent, on_accent, sel, sel_text, danger;
    int      hc;                    /* High Contrast: thicker outlines */
    int      px, rad;               /* the text size, the corner radius */
} pxui_theme_t;
const pxui_theme_t *pxui_theme(void);

/* This frame's input: a key (or -1), the pointer inside the canvas, and
   the left button's edges. Filled by pxui_poll(). */
typedef struct {
    int key;
    int mx, my, in;                 /* in: the pointer is over this window's content */
    int down, pressed, released;    /* held; went down this frame; came up */
    int dbl;                        /* the press was a double click */
    int focus_next;                 /* Tab: the app moves focus */
} pxui_in_t;

/* The current task's window as the canvas; 0 when there is none (not on
   the desktop, or no memory for its pixels). */
int  pxui_begin(void);
int  pxui_w(void);
int  pxui_h(void);
void pxui_present(void);                          /* the whole canvas to the screen */
void pxui_present_rect(prect_t r);
void pxui_end(void);
void pxui_poll(pxui_in_t *in);                    /* does not wait */
int  pxui_wait(pxui_in_t *in);                    /* waits for a key or a change of the pointer */

int  pxui_hit(prect_t r, const pxui_in_t *in);

/* drawing */
void pxui_fill(prect_t r, uint32_t c);
void pxui_panel(prect_t r, uint32_t c);           /* rounded */
void pxui_text(int x, int y, const char *s, uint32_t fg, uint32_t bg, int maxw);
void pxui_text_px(int x, int y, const char *s, int px, int bold, uint32_t fg, uint32_t bg, int maxw);
int  pxui_text_w(const char *s);
int  pxui_line_h(void);

/* controls -- each returns 1 when it was used this frame */
int  pxui_button(prect_t r, const char *label, int primary, int enabled, const pxui_in_t *in);
int  pxui_icon_button(prect_t r, const char *icon, const char *tip, const pxui_in_t *in);
int  pxui_toggle(prect_t r, const char *label, int *on, const pxui_in_t *in);
int  pxui_checkbox(prect_t r, const char *label, int *on, const pxui_in_t *in);
/* a list with a scrollbar: *sel, *top kept by the app; 1 when an item is
   opened (double click, Enter), and the selection follows clicks and the
   arrow keys when `focused` */
int  pxui_list(prect_t r, const char *const *items, int n, int *sel, int *top, int focused, const pxui_in_t *in);
void pxui_scrollbar(prect_t r, int total, int shown, int *top, const pxui_in_t *in);
/* a one-line field; keys edit it while *focus; 1 on Enter */
int  pxui_textfield(prect_t r, char *buf, int cap, int *focus, const char *hint, const pxui_in_t *in);
void pxui_progress(prect_t r, int pct);
void pxui_tooltip(int x, int y, const char *text);
/* modal: a menu at (x,y), the index chosen or -1; a dialog, the button */
int  pxui_menu(int x, int y, const char *const *items, int n);
int  pxui_dialog(const char *title, const char *msg, const char *const *buttons, int n);
