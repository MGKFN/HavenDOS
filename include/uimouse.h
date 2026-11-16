#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/uimouse.h -- the mouse in every app (v0.9.6.3)

   Until 0.9.6.3 only the desktop, Mines, Chat and the login screen
   looked at the mouse; Settings and every other app were keyboard-only.
   This layer gives all of them the mouse without rewriting any of them,
   by turning what the mouse does into the KEYS the app already reads:

     click a row          the arrow presses that select it; a click on
                          the row that is already selected is Enter
     click a key hint     "Enter apply  Left back  Q quit" on the bottom
                          two rows of the screen: that key
     scroll wheel         Up / Down, one per notch
     right-click          ESC -- back, or close

   An app that registers its rows and buttons gets exact clicks; an app
   that registers nothing still gets the hints, the wheel and
   right-click, because those need nothing from it. The hints are read
   off the screen itself at the moment of the click.

   It runs inside keyboard_getchar() (a hook drivers/keyboard.c calls
   when no key is waiting), so it only ever runs for the task in front,
   and it draws the pointer while the app waits. An app that reads the
   mouse itself -- the desktop, Mines, Chat, the login screen -- turns
   the layer off for its own task with um_set_mode(0).

   Everything is per task: rows registered by an app running behind
   (a clock, the music player redrawing) never land in the table of the
   app in front.
   ════════════════════════════════════════════════════════════════════ */

/* What the layer does for the current task. Default: all of it. */
#define UM_ZONES   0x01   /* registered rows and buttons               */
#define UM_HINTS   0x02   /* key hints on the bottom two rows          */
#define UM_WHEEL   0x04   /* wheel -> Up/Down                          */
#define UM_RIGHT   0x08   /* right-click -> ESC                        */
#define UM_POINTER 0x10   /* draw the pointer                          */
#define UM_ALL     0x1F
void um_set_mode(int mode);
int  um_mode(void);

/* Registering what is on the screen, while drawing it. um_frame()
   forgets the current task's zones -- call it where a screen is drawn
   from scratch; vga_clear() calls it too. */
void um_frame(void);
/* A row of a list. Rows of one `group` are taken in the order they are
   drawn, and Up/Down moves one row: what every list in this tree does.
   `selected` marks the row the app has highlighted. */
void um_row(int group, int x, int y, int w, int selected);
/* Keys to send before a group's arrows: e.g. Settings, when the focus
   is on its sidebar, needs Enter before a page row can be reached. */
void um_group_prefix(int group, const int *keys, int n);
/* A button: a click sends these keys (up to UM_SEQ_MAX). */
void um_keys(int x, int y, int w, int h, const int *keys, int n);
void um_key (int x, int y, int w, int h, int key);

/* The hook, installed by um_init() with keyboard_set_mouse_hook().
   Called with the key keyboard_getchar() is about to return, or -1:
   returns the key to return instead. */
int  um_hook(int key);
void um_init(void);

/* ── the pure parts, for tests/test_uimouse.c ─────────────────────── */
#define UM_SEQ_MAX 48
/* The key a hint names, for a click at column `col` of a line of text:
   the hint is the run of text between two gaps of 2+ spaces (or a '|')
   that holds the column; its first word is the key. 0 if the text
   there is not a hint. */
int  um_hint_key(const char *line, int len, int col);
/* What a left click at (x,y) sends, from the zones, rows and -- if
   `line` is non-null, the screen's text on row y -- the hints. Returns
   the number of keys written to out. */
int  um_click_keys(int x, int y, const char *line, int len, int *out, int max);
/* Test hooks: which task slot is current, and that task's id (a slot
   reused by a new task starts with an empty table). */
void um_set_task_fns(int (*slot)(void), uint32_t (*id)(int));
void um_reset_all(void);
