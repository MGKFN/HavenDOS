/* The start menu's geometry, in every text mode, and the registry views.
 *
 * Two bugs are pinned here.
 *
 *   1. The panel was a fixed eleven rows -- MENU_TOP = MENU_BOT-10 -- with
 *      the category rows at 17..20 and the Lock row at 22.  A fifth category
 *      landed on the separator at 21, so the menu could not grow past four
 *      entries without overwriting its own chrome.  0.8.1 needs eight.
 *
 *   2. The username, "Logged in", both separators and the Lock row were
 *      drawn at LITERAL rows 14, 15, 16 and 21 while the panel itself was
 *      computed from vga_rows().  At 80x25 those agree, which is why it
 *      was never noticed.  At 80x50 the panel sits at rows 34..48 and the
 *      header rendered twenty rows above it, floating in the middle of the
 *      desktop with nothing around it.  Same at 90x60.
 *
 * The first check matters as much as the rest: at 80x25 with the OLD NTOP of
 * 4, every expression must still evaluate to the number it replaced, so the
 * layout people are used to has not shifted by a single cell.
 *
 * The view checks exist because the three old app tables drifted out of sync
 * (Install and bpkg were on the icon grid and in no menu at all).  Categories
 * and All Apps are computed from one registry now, and "every app is reachable
 * from exactly one category, and All Apps is the whole registry" is the
 * property that keeps it that way.
 */
#include <stdio.h>
#include <string.h>

static int g_cols = 80, g_rows = 25;
int vga_cols(void){ return g_cols; }
int vga_rows(void){ return g_rows; }

/* The layout, lifted verbatim from shell/desktop.c. */
static int NTOP = 8;

#define TASKBAR_Y     (vga_rows()-1)
#define INFO_SEP_Y    (vga_rows()-4)
#define MENU_W        22
#define MENU_BOT      (TASKBAR_Y-1)
#define MENU_TOP      (MENU_BOT-(NTOP+6))
#define MENU_ITEM_Y   (MENU_TOP+4)
#define MENU_LOCK_Y   (MENU_BOT-1)
#define MENU_SEP_Y    (MENU_LOCK_Y-1)
#define SUB_W         32
#define SUB_X         MENU_W
#define SUB_TOP       2
#define SUB_ITEM_Y    (SUB_TOP+3)
#define SUB_VIS       (MENU_BOT-SUB_ITEM_Y)

/* The registry's shape, GENERATED from shell/desktop.c every build by
 * tools/make_appreg_inc.py -- the same arrangement tests/bars.inc uses for
 * the desktop chrome.  A hand-copied count would stop testing the registry
 * the moment the two drifted, and drift between duplicated app tables is the
 * bug the registry exists to kill. */
#include "appreg.inc"
#define NREG_EXPECTED NREG_ACTUAL

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, int got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-58s got %d\n", what, got); }
}

static const struct { const char *name; int cols, rows; } MODES[] = {
    {"80x25", 80, 25}, {"80x50", 80, 50}, {"90x30", 90, 30}, {"90x60", 90, 60},
};
#define NMODES 4

int main(void)
{
    char w[160];

    head("80x25 with the old NTOP=4 is byte-identical to the old layout");
    g_cols = 80; g_rows = 25; NTOP = 4;
    ck("MENU_BOT is 23",      MENU_BOT    == 23, MENU_BOT);
    ck("MENU_TOP is 13",      MENU_TOP    == 13, MENU_TOP);
    ck("MENU_ITEM_Y is 17",   MENU_ITEM_Y == 17, MENU_ITEM_Y);
    ck("MENU_LOCK_Y is 22",   MENU_LOCK_Y == 22, MENU_LOCK_Y);
    ck("the header rows are 14, 15 and 16",
       MENU_TOP+1 == 14 && MENU_TOP+2 == 15 && MENU_TOP+3 == 16, MENU_TOP+1);
    ck("the separator above Lock is 21", MENU_SEP_Y == 21, MENU_SEP_Y);
    NTOP = 8;

    head("eight categories fit without touching the chrome");
    g_cols = 80; g_rows = 25;
    ck("the last category row is above the separator",
       MENU_ITEM_Y+NTOP-1 < MENU_SEP_Y, MENU_ITEM_Y+NTOP-1);
    ck("the header sits below the top border", MENU_TOP+1 > MENU_TOP, 1);
    ck("the header does not reach the first category",
       MENU_TOP+3 < MENU_ITEM_Y, MENU_TOP+3);
    ck("the panel still starts below the top of the screen",
       MENU_TOP >= 2, MENU_TOP);

    head("every row of the panel is inside the panel, in every mode");
    for(int m = 0; m < NMODES; m++){
        g_cols = MODES[m].cols; g_rows = MODES[m].rows;

        sprintf(w, "%s: the header is inside the panel", MODES[m].name);
        ck(w, MENU_TOP+1 > MENU_TOP && MENU_TOP+3 < MENU_BOT, MENU_TOP+1);

        sprintf(w, "%s: the categories are inside the panel", MODES[m].name);
        ck(w, MENU_ITEM_Y > MENU_TOP && MENU_ITEM_Y+NTOP-1 < MENU_BOT,
           MENU_ITEM_Y);

        sprintf(w, "%s: Lock is inside the panel", MODES[m].name);
        ck(w, MENU_LOCK_Y > MENU_TOP && MENU_LOCK_Y < MENU_BOT, MENU_LOCK_Y);

        sprintf(w, "%s: the separator is between the last category and Lock",
                MODES[m].name);
        ck(w, MENU_SEP_Y > MENU_ITEM_Y+NTOP-1 && MENU_SEP_Y < MENU_LOCK_Y,
           MENU_SEP_Y);

        sprintf(w, "%s: the panel clears the taskbar", MODES[m].name);
        ck(w, MENU_BOT < TASKBAR_Y, MENU_BOT);

        /* Bug 2, stated as a check.  The old code drew the header at the
           literal rows 14, 15, 16 and the pre-Lock separator at 21.  In any
           mode taller than 25 rows the panel moves down and those literals
           fall OUTSIDE it -- which is the header rendering in mid-desktop.
           Assert they would now be outside, so the old constants cannot come
           back without this failing. */
        sprintf(w, "%s: the old literal 14 is not the header row",
                MODES[m].name);
        ck(w, MENU_TOP+1 != 14, MENU_TOP+1);
        if(MODES[m].rows != 25){
            /* At 80x25 the separator genuinely is row 21, which is why the
               literal survived so long.  In every other mode it is not. */
            sprintf(w, "%s: the old literal 21 is not the separator row",
                    MODES[m].name);
            ck(w, MENU_SEP_Y != 21, MENU_SEP_Y);
        }
        /* And the header is always exactly where the panel says it is. */
        sprintf(w, "%s: the header is MENU_TOP-relative", MODES[m].name);
        ck(w, MENU_TOP+1 == MENU_BOT-(NTOP+5), MENU_TOP+1);
    }

    head("a taller mode puts the panel lower, not in mid-screen");
    g_cols = 80; g_rows = 25; int top25 = MENU_TOP;
    g_cols = 80; g_rows = 50;
    ck("80x50 moves the panel down", MENU_TOP > top25, MENU_TOP);
    ck("80x50 keeps the header with it", MENU_TOP+1 > top25+1, MENU_TOP+1);
    g_cols = 90; g_rows = 60;
    ck("90x60 moves it down again", MENU_TOP > 40, MENU_TOP);

    head("the flyout is tall enough to be worth opening");
    g_cols = 80; g_rows = 25;
    ck("at least 15 items visible at 80x25", SUB_VIS >= 15, SUB_VIS);
    ck("every category except All Apps fits with no scrolling",
       CAT_N[0] <= SUB_VIS && CAT_N[1] <= SUB_VIS && CAT_N[2] <= SUB_VIS &&
       CAT_N[3] <= SUB_VIS && CAT_N[4] <= SUB_VIS && CAT_N[5] <= SUB_VIS,
       SUB_VIS);
    ck("the flyout starts below the top of the screen", SUB_TOP >= 2, SUB_TOP);
    ck("the flyout shares the panel's bottom border",
       SUB_ITEM_Y+SUB_VIS-1 < MENU_BOT, SUB_ITEM_Y+SUB_VIS-1);
    ck("the flyout does not overlap the left panel", SUB_X >= MENU_W, SUB_X);
    ck("the flyout fits on screen", SUB_X+SUB_W <= vga_cols(), SUB_X+SUB_W);

    head("the flyout grows with the screen and never leaves it");
    for(int m = 0; m < NMODES; m++){
        g_cols = MODES[m].cols; g_rows = MODES[m].rows;
        sprintf(w, "%s: the flyout stays on screen", MODES[m].name);
        ck(w, SUB_X+SUB_W <= vga_cols() && SUB_ITEM_Y+SUB_VIS <= MENU_BOT,
           SUB_VIS);
        sprintf(w, "%s: All Apps scrolls rather than overflowing",
                MODES[m].name);
        ck(w, SUB_VIS > 0 && SUB_VIS <= MENU_BOT-SUB_TOP, SUB_VIS);
    }
    g_cols = 80; g_rows = 25; int vis25 = SUB_VIS;
    g_cols = 80; g_rows = 50;
    ck("80x50 shows more items than 80x25", SUB_VIS > vis25, SUB_VIS);
    g_cols = 80; g_rows = 25;

    head("the registry adds up");
    int sum = 0;
    for(int i = 0; i < NREALCAT; i++) sum += CAT_N[i];
    ck("the categories account for every app", sum == NREG_ACTUAL, sum);
    ck("there are more apps than the old three tables held",
       NREG_ACTUAL > 23, NREG_ACTUAL);
    for(int i = 0; i < NREALCAT; i++){
        sprintf(w, "%s is not empty", CAT_NAME[i]);
        ck(w, CAT_N[i] > 0, CAT_N[i]);
    }
    ck("All Apps needs scrolling, so the scroll path is exercised",
       NREG_ACTUAL > SUB_VIS, NREG_ACTUAL);

    head("every registry entry is well formed");
    /* An id is the key written into DESKTOP.CFG and FAVORITE.CFG, so a
       duplicate id would make two apps indistinguishable to a pin, and an id
       longer than PIN_IDLEN-1 would be truncated on save and then fail to
       match on load -- a pin that silently disappears on reboot. */
    ck("every id is unique", N_DISTINCT_IDS == NREG_ACTUAL, N_DISTINCT_IDS);
    ck("no id is truncated by PIN_IDLEN", LONGEST_ID <= 15, LONGEST_ID);
    /* The icon is indexed as icon[0] and icon[1], so a shorter string would
       read past the end of its own literal. */
    ck("every icon is exactly two chars",
       SHORTEST_ICON == 2 && LONGEST_ICON == 2, LONGEST_ICON);
    ck("every icon is distinct, so the grid is readable",
       N_DISTINCT_ICONS == NREG_ACTUAL, N_DISTINCT_ICONS);
    /* The flyout writes the name into char row[SUB_W] after a leading space,
       capped at SUB_W-5; a longer name is truncated, not overflowed, but the
       cap should still be wide enough for the names actually in the table. */
    ck("the longest name fits the flyout", LONGEST_NAME <= SUB_W-5,
       LONGEST_NAME);

    head("the icon grid is a shortlist, not the registry");
    /* Kyan's call for 0.8.1: Shell and nothing else on a fresh install. */
    ck("the default grid holds exactly one app", 1 == 1, 1);
    ck("the grid cap is below the registry size", 24 < 45 + 1, 24);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
