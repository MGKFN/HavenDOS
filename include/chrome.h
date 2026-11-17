#pragma once
#include "types.h"

/* include/chrome.h -- an app's window: title bar and taskbar (v0.9.6.5).
   shell/chrome.c has the design. */

typedef struct {
    int vx, vy, vw, vh;     /* the app's box on the screen              */
    int viewport;           /* 0: the app has the whole screen           */
    int title_y;            /* -1: none                                  */
    int title_x0, title_x1;
    int bar_y;              /* -1: no taskbar                            */
    int boxed, full;
} ch_layout_t;

#define CH_NONE  0
#define CH_TITLE 1
#define CH_MIN   2
#define CH_FULL  3
#define CH_CLOSE 4
#define CH_START 5
#define CH_CHIP  6
#define CH_BAR   7

/* pure, for tests/test_window.c */
void chrome_layout(int pw, int ph, int boxed, int full, ch_layout_t *L);
int  chrome_hit(const ch_layout_t *L, int pw, int x, int y, int *chip);

/* for shell/desktop.c */
int  chrome_begin(const char *name, int boxed);   /* 1: the window is up  */
void chrome_end(void);
int  chrome_on(void);
void chrome_set_surround(void (*fn)(int vx, int vy, int vw, int vh, const char *name));
int  chrome_hook(int key);
void chrome_init(void);
void chrome_leave_front(void);  /* v0.9.6.6: before another app takes the screen */
int  chrome_take_close(void);   /* v0.9.6.6: was that ESC the X? */
