/* The editor viewport, in every text mode.
 *
 * ED_VROWS and ED_VCOLS were compile-time 21 and 74 — correct for 80x25 and
 * wrong everywhere else, so in 80x50 / 90x30 / 90x60 Notepad drew a 21x74
 * region in the top-left corner and left the rest of the screen holding
 * whatever had been there before. That is the "desktop apps render top-left
 * instead of centred" report.
 *
 * Two things are checked, and the first matters as much as the second:
 *
 *   1. At 80x25 the computed values are EXACTLY 21 and 74, so the layout
 *      everyone is used to has not shifted by a single cell.
 *   2. In every other mode the viewport actually grows to fill the screen,
 *      and never past the buffer it is reading from.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int g_cols = 80, g_rows = 25;
int vga_cols(void){ return g_cols; }
int vga_rows(void){ return g_rows; }

/* The constants and the two functions, lifted from shell/desktop.c. */
#define ED_ROWS        200
#define ED_COLS        79
#define ED_LNPAD       5

int ed_vrows(void)
{
    int r = vga_rows() - 4;
    if(r < 4)       r = 4;
    if(r > ED_ROWS) r = ED_ROWS;
    return r;
}

int ed_vcols(void)
{
    int c = vga_cols() - ED_LNPAD - 1;
    if(c < 16)      c = 16;
    if(c > ED_COLS) c = ED_COLS;
    return c;
}

static int fails = 0, total = 0;
static void ck(const char *what, int cond, long got)
{
    total++;
    if(cond) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s  (got %ld)\n", what, got); }
}
static void head(const char *t){ printf("\n%s\n", t); }

/* The modes HavenDOS actually offers. */
struct mode { int c, r; const char *name; };
static const struct mode MODES[] = {
    {80, 25, "80x25"}, {80, 50, "80x50"},
    {90, 30, "90x30"}, {90, 60, "90x60"},
};
#define NMODES ((int)(sizeof(MODES)/sizeof(MODES[0])))

int main(void)
{
    printf("Notepad viewport geometry\n=========================\n");

    head("80x25 is unchanged, to the cell");
    g_cols = 80; g_rows = 25;
    ck("ED_VROWS is still exactly 21", ed_vrows() == 21, ed_vrows());
    ck("ED_VCOLS is still exactly 74", ed_vcols() == 74, ed_vcols());

    head("the viewport follows the mode");
    for(int i = 0; i < NMODES; i++){
        g_cols = MODES[i].c; g_rows = MODES[i].r;
        int vr = ed_vrows(), vc = ed_vcols();
        char w[96];

        /* Rows 0 and 1 are the desktop bar and the title; the last two are the
           status and hint bars. Everything else must be text. */
        sprintf(w, "%s: text rows + 4 chrome rows == screen rows", MODES[i].name);
        ck(w, vr + 4 == MODES[i].r, vr);

        sprintf(w, "%s: the last text row is above the status bar", MODES[i].name);
        ck(w, 2 + vr - 1 < MODES[i].r - 2, 2 + vr - 1);

        sprintf(w, "%s: the gutter plus the text fits the width", MODES[i].name);
        ck(w, ED_LNPAD + vc <= MODES[i].c, ED_LNPAD + vc);

        sprintf(w, "%s: nothing is drawn in the final column", MODES[i].name);
        ck(w, ED_LNPAD + vc < MODES[i].c, ED_LNPAD + vc);

        sprintf(w, "%s: the viewport never exceeds the buffer", MODES[i].name);
        ck(w, vc <= ED_COLS && vr <= ED_ROWS, vc);
    }

    head("a taller mode really does show more text");
    g_cols = 80; g_rows = 25; int base = ed_vrows();
    g_cols = 80; g_rows = 50;
    ck("80x50 shows more rows than 80x25", ed_vrows() > base, ed_vrows());
    g_cols = 90; g_rows = 60;
    ck("90x60 shows more again", ed_vrows() > 21 + 20, ed_vrows());

    head("a wider mode really does show more text");
    g_cols = 80; g_rows = 25; int basec = ed_vcols();
    g_cols = 90; g_rows = 30;
    ck("90 columns shows more than 80", ed_vcols() > basec, ed_vcols());
    ck("...but clamps at the buffer width", ed_vcols() <= ED_COLS, ed_vcols());

    head("absurd modes degrade instead of going negative");
    g_cols = 8; g_rows = 4;
    ck("a tiny mode still reports a usable row count",
       ed_vrows() >= 4, ed_vrows());
    ck("a tiny mode still reports a usable column count",
       ed_vcols() >= 16, ed_vcols());
    g_cols = 400; g_rows = 400;
    ck("an enormous mode is bounded by the buffer, not the screen",
       ed_vcols() == ED_COLS && ed_vrows() == ED_ROWS, ed_vcols());

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
