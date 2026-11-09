#!/usr/bin/env python3
"""check_mines_suite.py -- prove tests/test_mines.c can fail.

IT PASSED 73 OF 73 ON THE FIRST RUN, which in this tree is the least
trustworthy result there is. The whole point of a suite written at the
same time as the code is that it agrees with it, and three releases
have been caught doing exactly that:

  tests/test_usbmux.c passed 79 of 79 against a sixteen-byte header
  the phone has never used.

  the first tests/test_i2c.c reached the driver through a pointer
  seam that swallowed every write, and failed 31 of 41 in a way that
  looked like a broken driver.

  net/rsa.c shipped with `for(r = 0; r < 1; r++)` -- Miller-Rabin
  running one round for ever -- and 361 checks passed over it,
  because not one of them ever asked for a second round.

── WHAT MAKES A MINESWEEPER WRONG ────────────────────────────────────

Not crashing. The defects below are all games that RUN, and that a
player would experience as the game being unfair or broken rather
than as software failing:

  the first click      a first click that can be a mine turns the
                       opening move into a coin toss, and one that is
                       merely not-a-mine leaves you staring at a
                       single number with nothing to deduce from.
  the flood fill       one that crosses mines reveals the board; one
                       that crosses flags undoes the player's own
                       marks from several cells away; one that does
                       not stop at numbers opens everything.
  the counters         revealed is MAINTAINED, not scanned, so every
                       path that can show a cell has to maintain it.
                       A drift of one means the game is won a click
                       early, or never.
  the refusals         mines_begin() refuses a board where the nine
                       cells the first click needs cannot be kept
                       free. Clamp it instead and the placement loop
                       looks for somewhere to put a mine that does
                       not exist.

── AND THE ONE THAT IS NOT A LOGIC BUG AT ALL ────────────────────────

The flood fill is iterative because a recursive one is up to 9600
frames deep on the kernel stack. The injection that makes it recursive
is in the list, and the suite catches it by filling a 160x60 board --
which is the only check in the file whose value is that it RETURNS.

── ONE INJECTION THAT PROVED NOT TO BE A DEFECT ──────────────────────

"the flood fill crosses mines" -- removing

    if(m->mine[ny][nx]) continue;

from flood()'s inner loop. The suite did not catch it, and the suite
was right: THE LINE IS UNREACHABLE. A cell enters the queue only when
it has no adjacent mines, so no neighbour of a queued cell can be a
mine, by the definition of zero.

Measured rather than argued: over 3000 random 30x16 boards with 60
mines, 1,499,471 neighbour examinations of queued cells, of which 0
were mines.

So it is recorded here and NOT in the list below, and the line stays
in the model with a note saying why -- anything that ever queues a
non-zero cell (a chord click, an auto-open, a solver) makes it live
again, and a fill that reveals mines ends the game without saying
why. One comparison against that.

── AND A SECOND ONE, FOUND THE SAME WAY ──────────────────────────────

"revealing over a flag leaves the flag counter wrong" -- removing the
`m->flags--` from show_one(). Also not caught, and also correctly:
nothing can hand show_one() a flagged cell. mines_reveal() refuses a
flagged click and flood() skips flagged neighbours.

Measured: 2000 boards, 120 random flags each, every cell clicked --
7700 cells became revealed, of which 0 were flagged.

Kept for the same reason. It is dead because of a decision made
elsewhere, and the first feature that opens a flagged cell makes it
live; a silently drifting counter is the hardest kind of bug to
attribute afterwards.

TWO dead branches out of nineteen planted defects is worth noticing
on its own. Both are defensive checks against a state the current
rules make impossible, and in both cases the harness -- not a person
-- is what established that, by planting the removal and watching a
correct suite shrug.

── AND FOUR GAPS IT FOUND IN A SUITE THAT PASSED 73 OF 73 ────────────

  a lost game accepting moves     the check was written
                                  `count_shown() == (reveal(), count_shown())`
                                  and C does not specify which side of
                                  == is evaluated first, so it compared
                                  the board to itself.
  xorshift's dead state           nothing passed seed 0, which is the
                                  one seed that makes the generator
                                  return 0 for ever.
  a recursive flood fill          a NATIVE run cannot notice: Linux
                                  gives the process eight megabytes.
                                  The suite now runs the fill on
                                  131072 bytes -- half of boot.S's
                                  stack -- in a forked child.
  a flag counter anchor           two identical lines in two
                                  functions; the injection matched
                                  both and was reported stale rather
                                  than silently damaging one.

Run:  python3 tools/check_mines_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "shell/mines_model.c"
TEST = "tests/test_mines.c"
CC = ["gcc", "-std=gnu99", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    ("the first click can be a mine",
     "the opening move becomes a coin toss. No minesweeper has worked "
     "this way in thirty years, and a player losing on click one has "
     "no way to tell a bug from bad luck -- which is what makes it "
     "ship",
     """        if(x >= safe_x-1 && x <= safe_x+1 &&
           y >= safe_y-1 && y <= safe_y+1) continue;
        m->mine[y][x] = 1;""",
     """        if(x == safe_x && y == safe_y) continue;
        m->mine[y][x] = 1;"""),

    ("the first click is safe but not a zero",
     "subtler and still a broken game: the click lands on a number, "
     "nothing floods, and the opening position has one revealed cell "
     "and nothing to deduce from. The rule is not 'not a mine', it is "
     "'no adjacent mines'",
     """    if(nmines > w*h - 9) return 0;""",
     """    if(nmines > w*h - 1) return 0;"""),

    ("the flood fill walks through flags",
     "a fill arriving from several cells away silently un-marks "
     "squares the player deliberately marked. It reads as the game "
     "cheating, and it is the one defect here that a player would "
     "describe as 'it keeps forgetting my flags'",
     """                if(m->flag[ny][nx]) continue;
                show_one(m, nx, ny);""",
     """                show_one(m, nx, ny);"""),

    ("the fill does not stop at a number",
     "CON_RESTART of minesweeper: every cell is queued, not just the "
     "zeroes, so the first click opens every cell that is not a mine "
     "and wins the game",
     """        if(mines_neighbours(m, nx, ny) == 0){
                    qx[tail] = (uint16_t)nx;
                    qy[tail] = (uint16_t)ny;
                    tail++;
                }""",
     """        {
                    qx[tail] = (uint16_t)nx;
                    qy[tail] = (uint16_t)ny;
                    tail++;
                }"""),

    ("clicking a numbered cell still floods from it",
     "the early return is what makes a number a wall. Without it the "
     "first numbered click opens its whole neighbourhood and keeps "
     "going",
     """    if(mines_neighbours(m, sx, sy) != 0) return;   /* a number stops here */""",
     """    /* a number no longer stops here */"""),

    ("a flagged cell can be clicked anyway",
     "the entire function of a flag is that it stops you. A player "
     "who has marked a mine and then clicks it loses to their own "
     "correct deduction",
     """    if(m->flag[y][x])  return m->state;   /* the point of a flag */""",
     """    /* a flag no longer stops a click */"""),

    ("the revealed count is not kept",
     "the win test is `revealed >= w*h - nmines`. A count that does "
     "not rise means the game can never be won, however completely "
     "it is played",
     """    if(m->shown[y][x]) return;
    m->shown[y][x] = 1;
    m->revealed++;""",
     """    if(m->shown[y][x]) return;
    m->shown[y][x] = 1;"""),

    ("the win is declared one cell early",
     "an off-by-one in the one comparison that ends the game. The "
     "board is announced as won with a cell still shut, which on a "
     "dense board is most of the time",
     """    if(m->revealed >= m->w * m->h - m->nmines)""",
     """    if(m->revealed >= m->w * m->h - m->nmines - 1)"""),

    ("a mine that is stepped on does not lose",
     "the game continues with a mine revealed, which looks like the "
     "mine was a normal cell. Every subsequent deduction is made from "
     "a board that is lying",
     """        m->boom_x = x; m->boom_y = y;
        m->state = MINES_LOST;""",
     """        m->boom_x = x; m->boom_y = y;"""),

    ("a lost or won game accepts more moves",
     "the final board changes under the player after the result is "
     "shown, so the screen explaining what happened stops matching "
     "what happened",
     """    if(m->state != MINES_PLAYING) return m->state;
    if(!in_range(m, x, y)) return m->state;""",
     """    if(!in_range(m, x, y)) return m->state;"""),

    ("a revealed cell can be flagged",
     "a flag on an open cell is drawn over a number the player needs, "
     "and it counts against the mine total, so the counter stops "
     "meaning anything",
     """    if(m->shown[y][x]) return 0;          /* nothing left to mark */""",
     """    /* a revealed cell can now be flagged */"""),

    ("too many mines is clamped instead of refused",
     "there is nowhere to put them. place() asks the generator for a "
     "free cell outside the first click's 3x3 and there is none, so "
     "it spins to its try limit and then fills deterministically -- a "
     "board with the wrong number of mines and a counter that cannot "
     "reach zero",
     """    if(nmines > w*h - 9) return 0;""",
     """    if(nmines > w*h - 9) nmines = w*h - 9;"""),

    ("a board smaller than 4x4 is accepted",
     "4x4 is the smallest board with room for the nine cells a first "
     "click needs plus a single mine. Anything smaller cannot be "
     "played and the refusal is what keeps place() from looking for "
     "space that does not exist",
     """    if(w < 4 || h < 4) return 0;""",
     """    if(w < 1 || h < 1) return 0;"""),

    ("a board bigger than the arrays is accepted",
     "mine[][] is MINES_MAX_H by MINES_MAX_W. A board past that "
     "writes into whatever follows it in bss -- which here is the "
     "flood fill's own queue, so the corruption arrives as a fill "
     "that walks off the board",
     """    if(w > MINES_MAX_W || h > MINES_MAX_H) return 0;""",
     """    /* the arrays are no longer a limit */"""),

    ("neighbours counts off the edge of the board",
     "the count at a corner reads cells from the end of the row above "
     "or from before the array. Corners get numbers that do not match "
     "what is around them, and a player deducing from them is being "
     "lied to",
     """            if(in_range(m, x+dx, y+dy) && m->mine[y+dy][x+dx]) n++;""",
     """            if(m->mine[y+dy][x+dx]) n++;"""),

    ("the generator's dead state is not replaced",
     "xorshift32 maps 0 to 0, for ever. A seed of 0 gives a generator "
     "that returns 0 every time, so every mine is placed at (0,0) and "
     "the board ends up with one mine and a counter claiming forty",
     """    m->rng = seed ? seed : 0x9E3779B9u;   /* 0 is xorshift's dead state */""",
     """    m->rng = seed;"""),

    ("the flood fill is recursive again",
     "NOT A LOGIC BUG. The game plays identically and the stack does "
     "not: 9600 cells is up to 9600 frames, boot/boot.S reserves "
     "262144 bytes and apps run on it, and v0.8.8.4's `crash stack` "
     "measured what going off the end does -- it overwrites the heap "
     "and the console's shadow buffer on the way down and wedges the "
     "machine with no fault for the app guard to catch. A NATIVE RUN "
     "CANNOT SEE IT: Linux gives this process eight megabytes and a "
     "recursive fill completes comfortably, which is what happened -- "
     "all 73 checks passed over it. The suite runs the fill on half "
     "the kernel's stack in a forked child now",
     """static void flood(mines_t *m, int sx, int sy)
{
    int head = 0, tail = 0;

    show_one(m, sx, sy);
    if(mines_neighbours(m, sx, sy) != 0) return;   /* a number stops here */

    qx[tail] = (uint16_t)sx; qy[tail] = (uint16_t)sy; tail++;

    while(head < tail){
        int x = qx[head], y = qy[head], dx, dy;
        head++;
        for(dy = -1; dy <= 1; dy++)
            for(dx = -1; dx <= 1; dx++){
                int nx = x+dx, ny = y+dy;
                if(!dx && !dy) continue;
                if(!in_range(m, nx, ny)) continue;
                if(m->shown[ny][nx]) continue;
                /* ── UNREACHABLE, AND KEPT ─────────────────────────────
                   A cell enters the queue only when it has NO adjacent
                   mines, so no neighbour of a queued cell can be a
                   mine -- by the definition of zero.

                   MEASURED, after the injection harness planted the
                   removal of this line and the suite correctly did not
                   notice: over 3000 random boards, 1,499,471 neighbour
                   examinations of queued cells, of which 0 were mines.
                   So this is a dead branch and its removal is NOT a
                   defect. The harness records that rather than
                   pretending otherwise.

                   It stays because the thing making it dead is the
                   `mines_neighbours(...) == 0` test at the bottom of
                   this loop. Anything that ever queues a non-zero cell
                   -- a chord click, an auto-open, a solver -- makes it
                   live again instantly, and a fill that reveals mines
                   ends the game without saying why. One comparison. */
                if(m->mine[ny][nx]) continue;
                /* A FLAGGED CELL IS NOT OPENED BY A FLOOD EITHER.
                   A player who has marked a square is telling the game
                   not to touch it, and a fill that walked through
                   flags would undo that work from several cells away
                   -- which reads as the game cheating. */
                if(m->flag[ny][nx]) continue;
                show_one(m, nx, ny);
                if(mines_neighbours(m, nx, ny) == 0){
                    qx[tail] = (uint16_t)nx;
                    qy[tail] = (uint16_t)ny;
                    tail++;
                }
            }
    }
}""",
     """static void flood(mines_t *m, int sx, int sy)
{
    int dx, dy;
    (void)qx; (void)qy;
    show_one(m, sx, sy);
    if(mines_neighbours(m, sx, sy) != 0) return;
    for(dy = -1; dy <= 1; dy++)
        for(dx = -1; dx <= 1; dx++){
            int nx = sx+dx, ny = sy+dy;
            if(!dx && !dy) continue;
            if(!in_range(m, nx, ny)) continue;
            if(m->shown[ny][nx]) continue;
            if(m->mine[ny][nx]) continue;
            if(m->flag[ny][nx]) continue;
            flood(m, nx, ny);
        }
}"""),
]

# Must NOT be caught. A comment reworded: nothing about the rules
# changes, so a catch here means the loop is restoring badly or the
# suite is asserting something that is not behaviour.
HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the rules changed",
    """int mines_remaining(const mines_t *m)""",
    """/* mines minus flags */
int mines_remaining(const mines_t *m)""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "mines", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        preamble="Proving tests/test_mines.c can fail.\n"
                 "It passed 73 of 73 first time, which is the least\n"
                 "trustworthy result in this tree: a suite written\n"
                 "beside the code agrees with the code."))
