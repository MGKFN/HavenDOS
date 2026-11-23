/* ════════════════════════════════════════════════════════════════════
   retropc/font.c -- the text screen's font.  v0.9.7.5.

   include/font8x16.h is the kernel's framebuffer font: ASCII from
   koi8x16 and the CP437 accented letters at 0x80-0xA5. HavenDOS draws
   nothing else, so the rest of CP437 is blank there -- the box lines,
   the blocks, the card suits and arrows below 0x20.

   An old PC program draws exactly those. A guest that boxes its
   screen in double lines would come out as an empty frame. So the
   missing glyphs are made here, at start-up, and not copied from a
   font: the lines and blocks are geometry, worked out from which
   arms a character has, and the symbols are the 8x8 shapes
   drivers/vga.c already patches into the VGA font (v0.7.15), drawn
   twice as tall.
   ════════════════════════════════════════════════════════════════════ */
#include <string.h>
#include "font8x16.h"

/* ── box drawing, 0xB3-0xDA ───────────────────────────────────────────
   Each character is up to four arms -- up, down, left, right -- each
   none (0), single (1) or double (2). From CP437's own table. */
static const unsigned char g_arms[0xDA - 0xB3 + 1][4] = {
    /* U  D  L  R */
    {1,1,0,0}, {1,1,1,0}, {1,1,2,0}, {2,2,1,0}, {0,2,1,0}, {0,1,2,0},   /* B3 B4 B5 B6 B7 B8 */
    {2,2,2,0}, {2,2,0,0}, {0,2,2,0}, {2,0,2,0}, {2,0,1,0}, {1,0,2,0},   /* B9 BA BB BC BD BE */
    {0,1,1,0}, {1,0,0,1}, {1,0,1,1}, {0,1,1,1}, {1,1,0,1}, {0,0,1,1},   /* BF C0 C1 C2 C3 C4 */
    {1,1,1,1}, {1,1,0,2}, {2,2,0,1}, {2,0,0,2}, {0,2,0,2}, {2,0,2,2},   /* C5 C6 C7 C8 C9 CA */
    {0,2,2,2}, {2,2,0,2}, {0,0,2,2}, {2,2,2,2}, {1,0,2,2}, {2,0,1,1},   /* CB CC CD CE CF D0 */
    {0,1,2,2}, {0,2,1,1}, {2,0,0,1}, {1,0,0,2}, {0,1,0,2}, {0,2,0,1},   /* D1 D2 D3 D4 D5 D6 */
    {2,2,1,1}, {1,1,2,2}, {1,0,1,0}, {0,1,0,1},                         /* D7 D8 D9 DA       */
};

/* Where the lines run. A single vertical line is two pixels wide down
   the middle, the way the VGA font draws one; a double is two such
   lines with a two-pixel gap. A horizontal line is one row, because a
   VGA pixel is taller than it is wide; a double has three rows
   between, which looks like the same gap. */
#define HC     7                   /* single horizontal: this row     */
#define HT     5                   /* double: the top line ...        */
#define HB     9                   /* ... and the bottom one          */
/* columns as ranges: single 3-4; double 1-2 and 5-6 */

static void pset(unsigned char *g, int x, int y){ if(x >= 0 && x < 8 && y >= 0 && y < 16) g[y] |= (unsigned char)(0x80 >> x); }
static void vline(unsigned char *g, int x0, int x1, int y0, int y1){
    if(y0 > y1){ int t = y0; y0 = y1; y1 = t; }
    for(int y = y0; y <= y1; y++) for(int x = x0; x <= x1; x++) pset(g, x, y);
}
static void hline(unsigned char *g, int y, int x0, int x1){
    if(x0 > x1){ int t = x0; x0 = x1; x1 = t; }
    for(int x = x0; x <= x1; x++) pset(g, x, y);
}

/* The row (for a vertical arm) or column (for a horizontal one) one
   of its lines runs to. `from_start` is 1 for the up and left arms,
   which come from row or column 0; side_own is the perpendicular arm
   on this line's side, side_oth the one opposite. Rules:
     double meets double on its own side  -> stop at its near line
                                             (an inner corner)
     double meets single on its own side  -> run to the single line
     double, nothing on its own side      -> run to the far line of
                                             the other side (outer)
     single between two perpendicular arms -> stop at the near line
                                             of a double, else cross
     single with one perpendicular arm     -> run to its far line
     nothing perpendicular                 -> to the middle */
static int stop_at(int weight, int own, int oth, int from_start, int horizontal_target){
    /* near/far of a perpendicular arm of weight w, seen from this arm */
    int nearv[3], farv[3], mid;
    if(horizontal_target){              /* target lines are rows      */
        nearv[1] = HC; farv[1] = HC;
        nearv[2] = from_start ? HT : HB; farv[2] = from_start ? HB : HT;
        mid = HC;
    } else {                            /* target lines are columns   */
        nearv[1] = from_start ? 3 : 4;  farv[1] = from_start ? 4 : 3;
        nearv[2] = from_start ? 1 : 6;  farv[2] = from_start ? 6 : 1;
        mid = from_start ? 4 : 3;
    }
    if(weight == 2){
        if(own == 2) return nearv[2];
        if(own == 1) return farv[1];
        if(oth)      return farv[oth];
        return mid;
    }
    if(own && oth){
        if(own == 2 || oth == 2) return nearv[2];
        return farv[1];
    }
    if(own || oth) return farv[own | oth];
    return mid;
}

static void box(unsigned char *g, const unsigned char a[4]){
    int U = a[0], D = a[1], L = a[2], R = a[3];
    /* vertical arms: lines as column ranges, side 0 = left, 1 = right */
    for(int arm = 0; arm < 2; arm++){
        int w = arm == 0 ? U : D, from_start = arm == 0;
        if(!w) continue;
        if(w == 1){
            int s = stop_at(1, L, R, from_start, 1);
            vline(g, 3, 4, from_start ? 0 : 15, s);
        } else {
            vline(g, 1, 2, from_start ? 0 : 15, stop_at(2, L, R, from_start, 1));
            vline(g, 5, 6, from_start ? 0 : 15, stop_at(2, R, L, from_start, 1));
        }
    }
    for(int arm = 0; arm < 2; arm++){
        int w = arm == 0 ? L : R, from_start = arm == 0;
        if(!w) continue;
        if(w == 1){
            hline(g, HC, from_start ? 0 : 7, stop_at(1, U, D, from_start, 0));
        } else {
            hline(g, HT, from_start ? 0 : 7, stop_at(2, U, D, from_start, 0));
            hline(g, HB, from_start ? 0 : 7, stop_at(2, D, U, from_start, 0));
        }
    }
}

/* ── the 8x8 symbols drivers/vga.c patches in, drawn 8x16 ─────────── */
static const struct { unsigned char idx, rows[8]; } g_syms[] = {
    {0x01,{0x3C,0x42,0xA5,0x81,0xA5,0x99,0x42,0x3C}}, {0x02,{0x3C,0x7E,0xDB,0xFF,0xDB,0xE7,0x7E,0x3C}},
    {0x03,{0x6C,0xFE,0xFE,0xFE,0x7C,0x38,0x10,0x00}}, {0x04,{0x10,0x38,0x7C,0xFE,0x7C,0x38,0x10,0x00}},
    {0x05,{0x38,0x38,0xD6,0xFE,0xD6,0x10,0x38,0x00}}, {0x06,{0x10,0x38,0x7C,0xFE,0x7C,0x10,0x38,0x00}},
    {0x07,{0x00,0x00,0x18,0x3C,0x3C,0x18,0x00,0x00}}, {0x09,{0x00,0x3C,0x66,0x42,0x42,0x66,0x3C,0x00}},
    {0x0D,{0x0C,0x0E,0x0B,0x09,0x0B,0x3B,0x78,0x30}}, {0x0E,{0x3F,0x33,0x3F,0x33,0x33,0xF3,0xE0,0x00}},
    {0x0F,{0x18,0xDB,0x3C,0xE7,0xE7,0x3C,0xDB,0x18}}, {0x10,{0x80,0xE0,0xF8,0xFE,0xF8,0xE0,0x80,0x00}},
    {0x11,{0x02,0x0E,0x3E,0xFE,0x3E,0x0E,0x02,0x00}}, {0x16,{0x00,0x00,0x00,0x00,0x7E,0x7E,0x7E,0x00}},
    {0x18,{0x18,0x3C,0x7E,0x18,0x18,0x18,0x18,0x00}}, {0x19,{0x18,0x18,0x18,0x18,0x7E,0x3C,0x18,0x00}},
    {0x1A,{0x00,0x18,0x0C,0xFE,0x0C,0x18,0x00,0x00}}, {0x1B,{0x00,0x30,0x60,0xFE,0x60,0x30,0x00,0x00}},
    {0x1E,{0x00,0x10,0x38,0x7C,0xFE,0x00,0x00,0x00}}, {0x1F,{0x00,0x00,0xFE,0x7C,0x38,0x10,0x00,0x00}},
    {0xF8,{0x38,0x6C,0x6C,0x38,0x00,0x00,0x00,0x00}}, {0xF9,{0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00}},
    {0xFA,{0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x00}}, {0xFE,{0x00,0x00,0x3C,0x3C,0x3C,0x3C,0x00,0x00}},
};

void rp_font_build(unsigned char *f /* 256 * 16 */){
    memcpy(f, font8x16, 256 * 16);
    for(int c = 0xB3; c <= 0xDA; c++){
        unsigned char *g = f + c * 16;
        memset(g, 0, 16);
        box(g, g_arms[c - 0xB3]);
    }
    /* shades: a quarter, half and three quarters of the pixels */
    for(int y = 0; y < 16; y++){
        f[0xB0 * 16 + y] = (y & 1) ? 0x22 : 0x88;
        f[0xB1 * 16 + y] = (y & 1) ? 0x55 : 0xAA;
        f[0xB2 * 16 + y] = (y & 1) ? 0xDD : 0x77;
        f[0xDB * 16 + y] = 0xFF;
        f[0xDC * 16 + y] = y >= 8 ? 0xFF : 0x00;
        f[0xDD * 16 + y] = 0xF0;
        f[0xDE * 16 + y] = 0x0F;
        f[0xDF * 16 + y] = y < 8 ? 0xFF : 0x00;
    }
    for(unsigned i = 0; i < sizeof g_syms / sizeof g_syms[0]; i++){
        unsigned char *g = f + g_syms[i].idx * 16;
        int blank = 1;
        for(int y = 0; y < 16; y++) if(g[y]) blank = 0;
        if(!blank) continue;                     /* the font has one */
        for(int y = 0; y < 8; y++) g[2 * y] = g[2 * y + 1] = g_syms[i].rows[y];
    }
}
