/* ════════════════════════════════════════════════════════════════════
   test_mines.c -- the minefield's rules, against the board.

   THIS SUITE HAS NO SEAMS, NO STUBS AND NO SIMULATED HARDWARE,
   because shell/mines_model.c calls nothing. It is linked directly.

   That is the point of splitting the model out, and it is worth
   saying why in a tree that has been bitten twice:

     tests/test_usbmux.c passed 79 of 79 against a sixteen-byte header
     the phone has never used.

     the first tests/test_i2c.c reached the driver through a pointer
     seam, which swallowed every write it made, and 31 of 41 checks
     failed in a way that looked like a broken driver.

   Both were suites checking a FIXTURE rather than a behaviour. There
   is no fixture here: the board is a real array, the rules really
   change it, and every check reads the array.

   ── WHAT IS ACTUALLY WORTH CHECKING ─────────────────────────────────

   Not "does a reveal reveal". The things that make a minesweeper
   right or wrong are narrower than that:

     THE FIRST CLICK. It must never be a mine, and it must be a ZERO,
     or the first move is a coin toss. That means the mines avoid nine
     cells, which means the mine count has an upper bound, which means
     mines_begin() has to refuse rather than spin.

     THE FLOOD FILL. It must open exactly the connected zero region
     AND its numbered border, stop at numbers, never cross a mine, and
     never cross a flag. Each of those is a different wrong game.

     THE COUNTS. revealed is maintained rather than scanned, so every
     path that can show a cell has to maintain it -- including the
     flood fill walking over a flagged cell, which has to take the
     flag off AND decrement the flag counter. A drifting count means
     the game is won at the wrong moment or never.

   The boards are built with FIXED SEEDS and the layout is read out of
   mine[][], so a check can name the cell it means.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include "../include/mines.h"
#include "../shell/mines_model.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %s%*sgot %ld\n", what,
                           (int)(what && strlen(what) < 54
                                 ? 56 - (int)strlen(what) : 2), "", got); }
}

static mines_t M;

/* Run on a deliberately small stack by the check near the end of
   main(). A separate function because makecontext() needs one, and
   its board is a file static because a 28 KB mines_t does not belong
   on a 128 KB stack whatever the fill does. */
static mines_t SM_board;
static void fill_whole_board(void)
{
    mines_reveal(&SM_board, MINES_MAX_W/2, MINES_MAX_H/2);
}

/* Count what is actually in the board, rather than trusting the
   counters the model keeps. The whole reason the counters are worth
   checking is that they are maintained and not computed. */
static int count_mines(const mines_t *m){
    int x, y, n = 0;
    for(y = 0; y < m->h; y++) for(x = 0; x < m->w; x++) if(m->mine[y][x]) n++;
    return n;
}
static int count_shown(const mines_t *m){
    int x, y, n = 0;
    for(y = 0; y < m->h; y++) for(x = 0; x < m->w; x++) if(m->shown[y][x]) n++;
    return n;
}
static int count_flags(const mines_t *m){
    int x, y, n = 0;
    for(y = 0; y < m->h; y++) for(x = 0; x < m->w; x++) if(m->flag[y][x]) n++;
    return n;
}

int main(void)
{
    printf("test_mines: the minefield's rules, against the board\n");

    /* ══ the board a game starts with ═══════════════════════════════ */
    head("a game begins, or is refused by name");
    ck("a 20x12 board with 30 mines is accepted",
       mines_begin(&M, 20, 12, 30, 12345) == 1, 0);
    ck("  it is 20 wide", M.w == 20, M.w);
    ck("  and 12 deep", M.h == 12, M.h);
    ck("  nothing is revealed yet", count_shown(&M) == 0, count_shown(&M));
    ck("  nothing is flagged", count_flags(&M) == 0, count_flags(&M));
    ck("  NO MINES ARE LAID YET -- that waits for the first click",
       count_mines(&M) == 0, count_mines(&M));
    ck("  the state is playing", M.state == MINES_PLAYING, M.state);
    ck("  and the counter reads all 30", mines_remaining(&M) == 30,
       mines_remaining(&M));

    /* The refusals. Each is a board that cannot be played, and the
       only alternatives to refusing are a clamp the caller cannot
       detect or a placement loop that never terminates. */
    ck("a board narrower than 4 is refused",
       mines_begin(&M, 3, 10, 5, 1) == 0, 0);
    ck("a board shallower than 4 is refused",
       mines_begin(&M, 10, 3, 5, 1) == 0, 0);
    ck("wider than MINES_MAX_W is refused",
       mines_begin(&M, MINES_MAX_W + 1, 10, 5, 1) == 0, 0);
    ck("deeper than MINES_MAX_H is refused",
       mines_begin(&M, 10, MINES_MAX_H + 1, 5, 1) == 0, 0);
    ck("zero mines is refused", mines_begin(&M, 10, 10, 0, 1) == 0, 0);
    /* 10x10 = 100 cells, minus the nine the first click needs = 91. */
    ck("91 mines on a 10x10 board is the most that fits",
       mines_begin(&M, 10, 10, 91, 1) == 1, 0);
    ck("  and 92 is refused, not clamped",
       mines_begin(&M, 10, 10, 92, 1) == 0, 0);
    ck("  a refusal leaves the previous board alone",
       M.nmines == 91, M.nmines);

    /* ══ the first click ════════════════════════════════════════════
       The property the whole game rests on, checked over many seeds
       rather than one: a single seed that happens to work says
       nothing about the rule. */
    head("the first click is never a mine, and is always a zero");
    {
        int bad_mine = 0, bad_zero = 0, s;
        for(s = 1; s <= 200; s++){
            mines_begin(&M, 20, 12, 40, (uint32_t)(s * 2654435761u));
            mines_reveal(&M, 7, 5);
            if(M.mine[5][7]) bad_mine++;
            if(mines_neighbours(&M, 7, 5) != 0) bad_zero++;
        }
        ck("over 200 seeds, the clicked cell was never a mine",
           bad_mine == 0, bad_mine);
        ck("  and was always a zero, so the first move opens a region",
           bad_zero == 0, bad_zero);
    }
    {
        /* And at the corners, which is where an off-by-one in the 3x3
           exclusion shows: at (0,0) only four of the nine cells
           exist, so a placement loop that insists on nine free cells
           inside the board would behave differently there. */
        int bad = 0, s;
        for(s = 1; s <= 100; s++){
            mines_begin(&M, 20, 12, 40, (uint32_t)(s * 40503u + 7u));
            mines_reveal(&M, 0, 0);
            if(M.mine[0][0] || mines_neighbours(&M, 0, 0) != 0) bad++;
        }
        ck("the top-left corner is safe and zero too", bad == 0, bad);
        bad = 0;
        for(s = 1; s <= 100; s++){
            mines_begin(&M, 20, 12, 40, (uint32_t)(s * 2246822519u));
            mines_reveal(&M, 19, 11);
            if(M.mine[11][19] || mines_neighbours(&M, 19, 11) != 0) bad++;
        }
        ck("the bottom-right corner is too", bad == 0, bad);
    }

    head("the mines that get laid are the mines that were asked for");
    {
        int s, wrong = 0;
        for(s = 1; s <= 100; s++){
            mines_begin(&M, 20, 12, 40, (uint32_t)(s * 97u + 13u));
            mines_reveal(&M, 10, 6);
            if(count_mines(&M) != 40) wrong++;
        }
        ck("exactly 40 mines on the board, over 100 seeds",
           wrong == 0, wrong);
    }
    {
        /* The densest board the model allows is where the rejection
           sampler is most likely to run out of tries, and the
           deterministic fallback in place() exists for exactly this.
           A counter that can never reach zero is worse than a board
           that is less random, so the count must still be right. */
        mines_begin(&M, 10, 10, 91, 555);
        mines_reveal(&M, 5, 5);
        ck("the densest legal board still gets all 91 mines",
           count_mines(&M) == 91, count_mines(&M));
        ck("  and the clicked cell is still not one of them",
           M.mine[5][5] == 0, M.mine[5][5]);
        ck("  and is still a zero",
           mines_neighbours(&M, 5, 5) == 0, mines_neighbours(&M, 5, 5));
    }

    /* ══ neighbours ═════════════════════════════════════════════════
       Checked against a board written by hand, so the expected
       numbers are not produced by the function being tested. */
    head("the adjacent count, on a board laid out by hand");
    {
        int x, y;
        mines_begin(&M, 8, 8, 1, 99);
        for(y = 0; y < 8; y++) for(x = 0; x < 8; x++) M.mine[y][x] = 0;
        /* Three mines in a row at (2,2) (3,2) (4,2). */
        M.mine[2][2] = M.mine[2][3] = M.mine[2][4] = 1;
        ck("a cell under the middle mine touches all three",
           mines_neighbours(&M, 3, 3) == 3, mines_neighbours(&M, 3, 3));
        ck("a cell under the left mine touches two",
           mines_neighbours(&M, 2, 3) == 2, mines_neighbours(&M, 2, 3));
        ck("a cell diagonally off the end touches one",
           mines_neighbours(&M, 1, 1) == 1, mines_neighbours(&M, 1, 1));
        ck("a cell two away touches none",
           mines_neighbours(&M, 0, 0) == 0, mines_neighbours(&M, 0, 0));
        ck("the middle mine itself counts its two siblings",
           mines_neighbours(&M, 3, 2) == 2, mines_neighbours(&M, 3, 2));
        ck("off the left edge reads 0, not memory before the row",
           mines_neighbours(&M, -1, 3) == 0, mines_neighbours(&M, -1, 3));
        ck("off the bottom reads 0", mines_neighbours(&M, 3, 8) == 0,
           mines_neighbours(&M, 3, 8));
    }

    /* ══ the flood fill ═════════════════════════════════════════════ */
    head("the flood fill: a hand-built board with one wall of mines");
    {
        int x, y, shown_left = 0, shown_right = 0;
        mines_begin(&M, 9, 5, 5, 4242);
        for(y = 0; y < 5; y++) for(x = 0; x < 9; x++) M.mine[y][x] = 0;
        /* A full-height wall at column 4 splits the board in two.
           Clicking left must open the whole left side and the wall's
           numbered neighbours, and NOTHING past the wall. */
        for(y = 0; y < 5; y++) M.mine[y][4] = 1;
        M.nmines = 5;
        M.placed = 1;                      /* no placement wanted */

        mines_reveal(&M, 0, 2);
        for(y = 0; y < 5; y++)
            for(x = 0; x < 9; x++){
                if(!M.shown[y][x]) continue;
                if(x < 4) shown_left++; else shown_right++;
            }
        ck("every one of the 20 cells left of the wall is open",
           shown_left == 20, shown_left);
        ck("  and NOT ONE cell right of the wall is",
           shown_right == 0, shown_right);
        ck("  the wall itself stays hidden",
           M.shown[2][4] == 0, M.shown[2][4]);
        ck("  the counter agrees with the board",
           M.revealed == count_shown(&M), M.revealed);
        ck("  still playing: 40 cells, 5 mines, 20 open",
           M.state == MINES_PLAYING, M.state);
    }

    head("a number stops the fill where it stands");
    {
        int x, y;
        mines_begin(&M, 9, 5, 1, 7);
        for(y = 0; y < 5; y++) for(x = 0; x < 9; x++) M.mine[y][x] = 0;
        M.mine[0][0] = 1;
        M.nmines = 1; M.placed = 1;
        /* (1,1) touches the mine, so it is a 1 and opens alone. */
        mines_reveal(&M, 1, 1);
        ck("clicking a numbered cell opens exactly one cell",
           count_shown(&M) == 1, count_shown(&M));
        ck("  and it is the one that was clicked",
           M.shown[1][1] == 1, M.shown[1][1]);
    }

    head("a flag is not walked through, from a click or from a fill");
    {
        int x, y;
        mines_begin(&M, 9, 5, 1, 7);
        for(y = 0; y < 5; y++) for(x = 0; x < 9; x++) M.mine[y][x] = 0;
        M.mine[4][8] = 1;                 /* far corner, out of the way */
        M.nmines = 1; M.placed = 1;

        mines_toggle_flag(&M, 3, 2);
        ck("a flag is placed", M.flag[2][3] == 1, M.flag[2][3]);
        ck("  and counted", mines_remaining(&M) == 0, mines_remaining(&M));
        ck("clicking the flagged cell does nothing",
           (mines_reveal(&M, 3, 2), M.shown[2][3] == 0), M.shown[2][3]);

        /* Now flood from the opposite corner. The fill will reach the
           flagged cell's neighbours and must stop at it -- a fill
           that opened flags would be undoing the player's work from
           several cells away, which reads as the game cheating. */
        mines_reveal(&M, 0, 0);
        ck("  and a flood fill arriving from elsewhere stops at it too",
           M.shown[2][3] == 0, M.shown[2][3]);
        ck("  the flag is still there afterwards",
           M.flag[2][3] == 1, M.flag[2][3]);
        ck("  and the flag count did not drift",
           M.flags == count_flags(&M), M.flags);
        ck("  the revealed count did not drift either",
           M.revealed == count_shown(&M), M.revealed);
    }

    head("flagging what cannot be flagged");
    {
        mines_begin(&M, 10, 10, 10, 31337);
        mines_reveal(&M, 5, 5);
        ck("a revealed cell cannot be flagged",
           mines_toggle_flag(&M, 5, 5) == 0, 0);
        ck("  and no flag appeared", M.flag[5][5] == 0, M.flag[5][5]);
        ck("  and the count is untouched", M.flags == 0, M.flags);
        ck("off the board cannot be flagged",
           mines_toggle_flag(&M, -1, 0) == 0, 0);
        ck("  nor past the right edge",
           mines_toggle_flag(&M, 10, 0) == 0, 0);
        ck("a flag toggles off again",
           (mines_toggle_flag(&M, 0, 9), mines_toggle_flag(&M, 0, 9),
            M.flag[9][0] == 0), M.flag[9][0]);
        ck("  leaving the count at zero", M.flags == 0, M.flags);
    }

    head("losing");
    {
        int x, y, found = 0, mx = 0, my = 0;
        mines_begin(&M, 12, 8, 10, 24680);
        mines_reveal(&M, 6, 4);
        for(y = 0; y < 8 && !found; y++)
            for(x = 0; x < 12 && !found; x++)
                if(M.mine[y][x]){ mx = x; my = y; found = 1; }
        ck("the board has a mine to step on", found == 1, found);
        ck("stepping on it loses",
           mines_reveal(&M, mx, my) == MINES_LOST, M.state);
        ck("  and the mine that was stepped on is recorded",
           M.boom_x == mx && M.boom_y == my, M.boom_x);
        {
            /* SEQUENCED EXPLICITLY. The first version of this check
               was
                   count_shown(&M) == (mines_reveal(...), count_shown(&M))
               and C does not specify which side of == is evaluated
               first, so it compared the board to itself and passed
               whatever the reveal did. The injection harness planted
               "a lost or won game accepts more moves" and the check
               MISSED it. */
            int before = count_shown(&M);
            int after;
            mines_reveal(&M, 0, 0);
            after = count_shown(&M);
            ck("  a reveal after losing changes nothing",
               after == before, after - before);
        }
        ck("  and a flag after losing changes nothing",
           mines_toggle_flag(&M, 0, 0) == 0, 0);
        mines_reveal_all(&M);
        ck("reveal_all shows every cell",
           count_shown(&M) == 12*8, count_shown(&M));
        ck("  and the counter still agrees",
           M.revealed == count_shown(&M), M.revealed);
    }

    head("winning: play a whole board out, the slow honest way");
    {
        int x, y, clicks = 0;
        mines_begin(&M, 12, 8, 10, 13579);
        mines_reveal(&M, 6, 4);
        /* Reveal every cell that is not a mine. A player would have to
           deduce them; the test is allowed to look, because what is
           being checked is the WIN CONDITION and not the game's
           difficulty. */
        for(y = 0; y < 8; y++)
            for(x = 0; x < 12; x++)
                if(!M.mine[y][x] && !M.shown[y][x]){
                    mines_reveal(&M, x, y);
                    clicks++;
                }
        ck("the state is won", M.state == MINES_WON, M.state);
        ck("  every non-mine is open",
           count_shown(&M) == 12*8 - 10, count_shown(&M));
        ck("  and no mine was revealed on the way",
           (long)(12*8 - 10) == (long)M.revealed, M.revealed);
        {
            int before = count_shown(&M), after;
            mines_reveal(&M, 0, 0);
            after = count_shown(&M);
            ck("  a reveal after winning changes nothing",
               after == before, after - before);
        }
        {
            /* And a flag, which goes through the same guard. */
            int before = count_flags(&M), after;
            mines_toggle_flag(&M, 0, 0);
            after = count_flags(&M);
            ck("  and a flag after winning changes nothing",
               after == before, after - before);
        }
        (void)clicks;
    }

    head("a win on the very first click, which is a real board");
    {
        /* 4x4 with 7 mines: 16 cells, 9 of them kept clear for the
           first click, so the 7 mines fill everything else exactly.
           The first click opens all nine and wins immediately. There
           is no deduction in it and it is still a legal game, and a
           win test that only fires after several clicks would miss
           it. */
        ck("a 4x4 board with 7 mines is legal",
           mines_begin(&M, 4, 4, 7, 2468) == 1, 0);
        ck("  and the first click wins it outright",
           mines_reveal(&M, 1, 1) == MINES_WON, M.state);
        ck("  with all nine safe cells open",
           count_shown(&M) == 9, count_shown(&M));
    }

    head("a seed of zero, which is xorshift's dead state");
    {
        /* xorshift32 maps 0 to 0 for ever, so a generator seeded with
           0 returns 0 every time and every mine goes to (0,0) -- a
           board with ONE mine and a counter claiming forty. The model
           replaces a zero seed; nothing checked that until the
           injection harness planted its removal and this suite missed
           it.

           A caller CAN pass 0: entropy_u32() is entitled to return
           it, and shell/mines.c passes whatever it gets. */
        mines_begin(&M, 20, 12, 40, 0);
        mines_reveal(&M, 10, 6);
        ck("seed 0 still lays all 40 mines", count_mines(&M) == 40,
           count_mines(&M));
        {
            /* ── AND THEY ARE NOT PILED IN THE TOP-LEFT ───────────────
               Counting distinct ROWS was the first version of this
               check and it did not work. A dead generator returns 0
               every time, so place() lays one mine at (0,0), spins to
               its try limit, and then the deterministic FALLBACK
               fills the remaining 39 in scan order -- which spreads
               them across several rows and gives the right total. The
               check passed and the injection harness still reported
               the defect as MISSED.

               The fallback is the right behaviour (a counter that can
               never reach zero is worse than a less random board) and
               it is also what hides a dead generator. What it cannot
               hide is WHERE the mines end up: scan order means the
               top rows, solidly.

               40 mines on 20x12 is 240 cells. Rows 0 and 1 are 40
               cells, a sixth of the board, so a live generator puts
               about 7 mines there and a dead one puts all 40. Twenty
               is the midpoint and nowhere near either. */
            int y, x, top = 0;
            for(y = 0; y < 2; y++)
                for(x = 0; x < M.w; x++)
                    if(M.mine[y][x]) top++;
            ck("  and not piled into the top two rows in scan order",
               top < 20, top);
        }
        ck("  and the first click is still safe and zero",
           M.mine[6][10] == 0 && mines_neighbours(&M, 10, 6) == 0, 0);
    }

    head("the biggest board the model will hold");
    {
        ck("160x60 is accepted",
           mines_begin(&M, MINES_MAX_W, MINES_MAX_H, 1200, 86420) == 1, 0);
        mines_reveal(&M, MINES_MAX_W/2, MINES_MAX_H/2);
        ck("  all 1200 mines are laid", count_mines(&M) == 1200,
           count_mines(&M));
        ck("  the first click is still safe",
           M.mine[MINES_MAX_H/2][MINES_MAX_W/2] == 0, 0);
        ck("  the counters agree with the board",
           M.revealed == count_shown(&M) && M.flags == count_flags(&M),
           M.revealed);
        /* A fill across 9600 cells is where a recursive version would
           have put 9600 frames on the kernel stack. Reaching this
           line at all is the check. */
        ck("  and a fill across the whole board returned",
           M.state == MINES_PLAYING || M.state == MINES_WON, M.state);
    }

    /* ══ THE FILL MUST FIT IN THE KERNEL'S STACK ════════════════════
       The only check in this file that is not about the rules, and
       the only one that cannot be made by calling the model normally.

       boot/boot.S reserves 262144 bytes of stack and apps run on it.
       A RECURSIVE fill over a 160x60 board is up to 9600 frames deep
       -- at the fifty-odd bytes a frame of that shape costs, roughly
       half a megabyte, which is twice the whole stack. Going off the
       end of it does not produce a catchable fault either:
       v0.8.8.4's `crash stack` measured what it does, which is to
       overwrite the heap and the console's shadow buffer on the way
       down and wedge the machine with nothing for the app guard to
       catch.

       A NATIVE RUN CANNOT NOTICE THIS. Linux gives this process eight
       megabytes, so a recursive fill completes comfortably and every
       check passes -- which is exactly what happened: the injection
       harness planted "the flood fill is recursive again" and all 73
       checks passed over it.

       So the fill is run on a stack of a size this project actually
       has, in a child process, through makecontext(). 131072 bytes is
       HALF the kernel's, which the iterative fill clears by three
       orders of magnitude and a recursive one cannot. The child is
       forked because the failure mode under test is a segmentation
       fault, and a suite that dies cannot report. */
    head("the fill completes in half the kernel's stack");
    {
        enum { SMALL_STACK = 131072 };     /* half of boot.S's 262144 */
        pid_t pid;

        ck("boot/boot.S reserves 262144 bytes, so this is half of it",
           SMALL_STACK * 2 == 262144, SMALL_STACK);

        fflush(stdout);
        pid = fork();
        if(pid == 0){
            static ucontext_t small, back;
            void *sp = malloc(SMALL_STACK);
            if(!sp) _exit(70);
            mines_begin(&SM_board, MINES_MAX_W, MINES_MAX_H, 50, 777);
            if(getcontext(&small) != 0) _exit(71);
            small.uc_stack.ss_sp   = sp;
            small.uc_stack.ss_size = SMALL_STACK;
            small.uc_link          = &back;
            /* 50 mines on 9600 cells is almost all zeroes, so this is
               very nearly the worst case: one click floods the lot. */
            makecontext(&small, (void(*)(void))fill_whole_board, 0);
            if(swapcontext(&back, &small) != 0) _exit(72);
            _exit(SM_board.revealed > 9000 ? 0 : 73);
        }
        if(pid < 0){
            ck("the child could be forked", 0, pid);
        } else {
            int st = 0;
            waitpid(pid, &st, 0);
            if(WIFSIGNALED(st)){
                ck("a 160x60 fill does not run off a 128 KB stack", 0,
                   WTERMSIG(st));
                printf("          (killed by signal %d -- SIGSEGV here "
                       "means the fill\n"
                       "           is recursive and would overrun the "
                       "kernel's stack)\n", WTERMSIG(st));
            } else {
                ck("a 160x60 fill does not run off a 128 KB stack",
                   WEXITSTATUS(st) == 0, WEXITSTATUS(st));
            }
        }
    }

    head("an empty-ish board: one mine, so one click opens nearly all");
    {
        mines_begin(&M, 20, 10, 1, 11111);
        mines_reveal(&M, 10, 5);
        /* With one mine, the only cells that can stay shut are the
           mine itself and -- if the click's zero region does not
           reach them -- nothing else, because every other cell is
           connected through zeros. 200 cells, 1 mine, so 199 open
           and the game is won on the first click. */
        ck("one click opens 199 of 200 cells",
           count_shown(&M) == 199, count_shown(&M));
        ck("  the mine is the one that is shut",
           M.state == MINES_WON, M.state);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
