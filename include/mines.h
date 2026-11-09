#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/mines.h -- the minefield, as a model with no screen.
   v0.8.8.4.

   WHY THIS IS A SEPARATE FILE FROM THE APP.

   shell/mines_model.c calls nothing. No vga, no keyboard, no mouse,
   no heap, no clock. So tests/test_mines.c links it DIRECTLY, with no
   seam, no simulated hardware and no stub -- and every check is
   against the board, which is the thing that can actually be wrong.

   That matters because this project's suites have twice been caught
   agreeing with the code instead of checking it: tests/test_usbmux.c
   passed 79 of 79 against a header the phone has never used, and the
   first tests/test_i2c.c swallowed every write the driver made
   because its seam was a pointer. Neither could have happened to a
   model with no outside world in it.

   The app -- shell/mines.c -- draws this and reads the keyboard and
   the mouse, and has no rules in it.

   ── WHAT THE MODEL GUARANTEES ───────────────────────────────────────

   THE FIRST CLICK IS ALWAYS SAFE, and more than that: it is always a
   zero, so the first move opens a region rather than a single number.
   Mines are not laid until the first reveal, and then they are laid
   avoiding that cell AND its eight neighbours. A minesweeper whose
   first click can be a mine is a coin toss, which is why no
   implementation has worked that way in thirty years.

   THE FLOOD FILL IS ITERATIVE. A recursive flood fill over a 160x60
   board is up to 9600 frames deep, and this kernel's apps run on the
   kernel stack -- v0.8.8.4's own `crash stack` demonstrates what
   running off the end of it does, which is to overwrite the heap and
   the console's shadow buffer on the way down and wedge the machine
   with no fault to catch. So the frontier is an explicit queue.
   ════════════════════════════════════════════════════════════════════ */

/* The biggest board the model will hold. Sized to the console's own
   maxima rather than to a guess: VGA_COLS_MAX is 320 and
   VGA_ROWS_MAX is 101 in the framebuffer console, and a board that
   big is 32320 cells -- three bytes each is 97 KB of bss, which is
   more than the heap. 160x60 is 9600 cells, 28 KB, and is larger than
   any grid a person will actually set. A bigger console simply gets a
   board that does not fill it, which the app says. */
#define MINES_MAX_W 160
#define MINES_MAX_H 60

#define MINES_PLAYING 0
#define MINES_WON     1
#define MINES_LOST    2

typedef struct {
    int      w, h;
    int      nmines;
    int      placed;          /* have the mines been laid yet?      */
    int      revealed;        /* cells shown, for the win test      */
    int      flags;           /* flags placed, for the counter      */
    int      state;           /* MINES_PLAYING / WON / LOST         */
    int      boom_x, boom_y;  /* the mine that was stepped on, or -1 */
    uint32_t rng;             /* the model's own generator          */
    uint8_t  mine [MINES_MAX_H][MINES_MAX_W];
    uint8_t  shown[MINES_MAX_H][MINES_MAX_W];
    uint8_t  flag [MINES_MAX_H][MINES_MAX_W];
} mines_t;

/* Start a game. Returns 0 and changes nothing if the board will not
   work: outside the maxima, or so many mines that the nine cells the
   first click needs cannot be kept clear. Refused rather than
   clamped, for the same reason vga_viewport_centre() refuses -- a
   caller handed a board it did not ask for has no way to find out. */
int  mines_begin(mines_t *m, int w, int h, int nmines, uint32_t seed);

/* How many mines touch (x,y). 0 for a mine itself is not special:
   ask mine[][] for that. Out of range reads 0. */
int  mines_neighbours(const mines_t *m, int x, int y);

/* Reveal (x,y). Lays the mines on the first call, avoiding (x,y) and
   its neighbours. Flood-fills through cells with no adjacent mines.
   A flagged cell is NOT revealed -- that is the whole point of a flag.
   Returns the state afterwards. */
int  mines_reveal(mines_t *m, int x, int y);

/* Flag or unflag (x,y). A revealed cell cannot be flagged. Returns 1
   if anything changed. */
int  mines_toggle_flag(mines_t *m, int x, int y);

/* Mines minus flags, which is what the counter shows. May go
   negative, and should: a player who has placed more flags than there
   are mines wants to know. */
int  mines_remaining(const mines_t *m);

/* Show everything. For the end of a lost game. */
void mines_reveal_all(mines_t *m);
