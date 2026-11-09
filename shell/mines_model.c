/* ════════════════════════════════════════════════════════════════════
   shell/mines_model.c -- the minefield's rules.  v0.8.8.4.

   NOTHING IN THIS FILE CALLS ANYTHING. No vga, no keyboard, no mouse,
   no heap, no clock, no string.h. include/mines.h says why: a model
   with no outside world can be linked straight into a native suite,
   so every check is against the board rather than against a simulated
   device that was written from the same guess as the code.

   shell/mines.c draws this and has no rules in it.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/mines.h"

/* ── the generator ───────────────────────────────────────────────────
   xorshift32, which is four shifts and gives a full 2^32-1 period.
   The model carries its OWN state rather than calling entropy_u32(),
   so a given seed always produces the same board -- which is what
   lets tests/test_mines.c check a specific layout rather than only
   statistical properties. A seed of 0 is a dead state for xorshift
   (it maps to itself), so it is replaced. */
static uint32_t nextr(mines_t *m)
{
    uint32_t x = m->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rng = x;
    return x;
}

static int in_range(const mines_t *m, int x, int y)
{
    return x >= 0 && x < m->w && y >= 0 && y < m->h;
}

int mines_begin(mines_t *m, int w, int h, int nmines, uint32_t seed)
{
    int x, y;

    if(!m) return 0;
    if(w < 4 || h < 4) return 0;
    if(w > MINES_MAX_W || h > MINES_MAX_H) return 0;
    if(nmines < 1) return 0;

    /* ── THE NINE CELLS THE FIRST CLICK NEEDS ─────────────────────────
       The first reveal must land on a zero, which means the cell and
       its eight neighbours have to be mine-free. So a board can hold
       at most w*h - 9 mines, and asking for more is refused rather
       than clamped: a caller handed a different game from the one it
       asked for cannot tell, and the placement loop below would spin
       for ever looking for somewhere to put the last mine. */
    if(nmines > w*h - 9) return 0;

    m->w = w; m->h = h; m->nmines = nmines;
    m->placed = 0; m->revealed = 0; m->flags = 0;
    m->state = MINES_PLAYING;
    m->boom_x = -1; m->boom_y = -1;
    m->rng = seed ? seed : 0x9E3779B9u;   /* 0 is xorshift's dead state */

    for(y = 0; y < h; y++)
        for(x = 0; x < w; x++){
            m->mine[y][x] = 0;
            m->shown[y][x] = 0;
            m->flag[y][x] = 0;
        }
    return 1;
}

int mines_neighbours(const mines_t *m, int x, int y)
{
    int dx, dy, n = 0;
    if(!m || !in_range(m, x, y)) return 0;
    for(dy = -1; dy <= 1; dy++)
        for(dx = -1; dx <= 1; dx++){
            if(!dx && !dy) continue;
            if(in_range(m, x+dx, y+dy) && m->mine[y+dy][x+dx]) n++;
        }
    return n;
}

/* ── laying the mines, after the first click ──────────────────────────
   Rejection sampling: pick a cell, skip it if it already has a mine
   or is in the 3x3 the first click needs. The refusal in
   mines_begin() guarantees at least nine free cells exist, so this
   terminates -- and the bound below is belt and braces rather than
   the reason it terminates, because a loop whose termination depends
   on a check in another function is one edit away from hanging the
   machine. */
static void place(mines_t *m, int safe_x, int safe_y)
{
    int left = m->nmines;
    uint32_t tries = 0;
    uint32_t limit = (uint32_t)(m->w * m->h) * 64u + 4096u;

    while(left > 0 && tries++ < limit){
        int x = (int)(nextr(m) % (uint32_t)m->w);
        int y = (int)(nextr(m) % (uint32_t)m->h);
        if(m->mine[y][x]) continue;
        if(x >= safe_x-1 && x <= safe_x+1 &&
           y >= safe_y-1 && y <= safe_y+1) continue;
        m->mine[y][x] = 1;
        left--;
    }

    /* If the sampler somehow ran out of tries, fill deterministically
       rather than starting a game with fewer mines than the counter
       claims -- a counter that can never reach zero is worse than a
       board that is slightly less random. */
    if(left > 0){
        int x, y;
        for(y = 0; y < m->h && left > 0; y++)
            for(x = 0; x < m->w && left > 0; x++){
                if(m->mine[y][x]) continue;
                if(x >= safe_x-1 && x <= safe_x+1 &&
                   y >= safe_y-1 && y <= safe_y+1) continue;
                m->mine[y][x] = 1;
                left--;
            }
    }

    m->placed = 1;
}

/* ── the flood fill ──────────────────────────────────────────────────
   ITERATIVE, with the frontier in bss. include/mines.h says why: a
   recursive fill over 9600 cells is 9600 stack frames, apps here run
   on the kernel stack, and v0.8.8.4's own `crash stack` shows what
   running off the end of it does -- it overwrites the heap and the
   console's shadow buffer on the way down and wedges the machine with
   no fault for the app guard to catch.

   A cell is pushed only when it is first revealed, so it enters the
   queue at most once and the queue can never need more entries than
   there are cells. */
static uint16_t qx[MINES_MAX_W * MINES_MAX_H];
static uint16_t qy[MINES_MAX_W * MINES_MAX_H];

static void show_one(mines_t *m, int x, int y)
{
    if(m->shown[y][x]) return;
    m->shown[y][x] = 1;
    m->revealed++;
    /* ── ALSO UNREACHABLE, AND ALSO KEPT ──────────────────────────────
       A revealed cell must not stay flagged, and the flag counter has
       to follow -- otherwise the mine counter drifts every time a
       flagged square is opened.

       As it stands nothing can open one: mines_reveal() refuses a
       flagged click and flood() skips flagged neighbours, so this
       function never meets a flag.

       MEASURED, after the injection harness planted the removal of
       the decrement and the suite correctly did not notice: 2000
       boards, 120 random flags each, every cell clicked -- 7700 cells
       became revealed, of which 0 were flagged. Dead branch, and its
       removal is NOT a defect.

       Kept for the same reason as the mine check in flood(): it is
       dead because of a decision made elsewhere, and the first
       feature that opens a flagged cell -- a chord click, an
       auto-open, "reveal everything" at the end of a game -- makes it
       live. A silently drifting counter is the hardest kind of bug to
       attribute afterwards. */
    if(m->flag[y][x]){ m->flag[y][x] = 0; m->flags--; }
}

static void flood(mines_t *m, int sx, int sy)
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
}

int mines_reveal(mines_t *m, int x, int y)
{
    if(!m) return MINES_LOST;
    if(m->state != MINES_PLAYING) return m->state;
    if(!in_range(m, x, y)) return m->state;
    if(m->shown[y][x]) return m->state;
    if(m->flag[y][x])  return m->state;   /* the point of a flag */

    if(!m->placed) place(m, x, y);

    if(m->mine[y][x]){
        m->shown[y][x] = 1;
        m->revealed++;
        m->boom_x = x; m->boom_y = y;
        m->state = MINES_LOST;
        return m->state;
    }

    flood(m, x, y);

    /* Won when every cell that is not a mine is showing. Counted, not
       scanned: a scan of the board on every reveal is 9600 cells per
       click, and the count is maintained in the one place a cell can
       become revealed. */
    if(m->revealed >= m->w * m->h - m->nmines)
        m->state = MINES_WON;

    return m->state;
}

int mines_toggle_flag(mines_t *m, int x, int y)
{
    if(!m) return 0;
    if(m->state != MINES_PLAYING) return 0;
    if(!in_range(m, x, y)) return 0;
    if(m->shown[y][x]) return 0;          /* nothing left to mark */

    if(m->flag[y][x]){ m->flag[y][x] = 0; m->flags--; }
    else             { m->flag[y][x] = 1; m->flags++; }
    return 1;
}

int mines_remaining(const mines_t *m)
{
    if(!m) return 0;
    return m->nmines - m->flags;          /* may be negative, and should */
}

void mines_reveal_all(mines_t *m)
{
    int x, y;
    if(!m) return;
    for(y = 0; y < m->h; y++)
        for(x = 0; x < m->w; x++)
            if(!m->shown[y][x]){ m->shown[y][x] = 1; m->revealed++; }
}
