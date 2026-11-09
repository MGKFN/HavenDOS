/* The login screen's account tiles, at every name length and every text mode.
 *
 * Found by walking first-run Setup in a VM on v0.7.19: an account created as
 * "kyanhoekstra123" appeared on the login screen as "kyanhoekstra1>3".
 *
 * The tile drew the name at TILE_X+5 and the selection marker at a FIXED
 * TILE_X+TILE_W-2. With TILE_X=2 and TILE_W=20 that put the marker on column
 * 20, while a 15-character name occupied columns 7..21 — so the marker was
 * written into the middle of the name, replacing its 14th character. The [A]
 * administrator badge, positioned after the name, ran past the end of the
 * tile for the same reason.
 *
 * Setup's own prompt reads "Username (3-15 characters)", so every account
 * created with more than 13 characters hit this. It is only a display fault —
 * try_login() matched fine, which is why tests/test_accounts.c passes while
 * the screen was still wrong — but the name a person types to identify
 * themselves was shown back to them corrupted.
 *
 * The geometry below is the real one, lifted from shell/login.c.
 */
#include <stdio.h>
#include <string.h>

#define TILE_X 2

static int g_cols = 80, g_rows = 25;
int vga_cols(void){ return g_cols; }
int vga_rows(void){ return g_rows; }

/* the names currently on screen */
#define MAXV 6
static const char *g_names[MAXV];
static int g_nv;

static int tile_w(void)
{
    int longest = 0;
    for(int i=0;i<g_nv && i<MAXV;i++){
        int n=(int)strlen(g_names[i]);
        if(n>longest) longest=n;
    }
    int w = longest + 11;
    if(w < 20) w = 20;
    int max = vga_cols() - TILE_X - 1;
    if(w > max) w = max;
    return w;
}

static int tile_base(void)
{
    int b = vga_rows() - 2;
    if(b < 10) b = 10;
    return b;
}

/* Where each piece lands, exactly as draw_tiles() places it. */
typedef struct { int name_x, name_len, badge_x, badge_end, marker_x, tile_end; } lay_t;

static lay_t layout(const char *name)
{
    int TW = tile_w();
    int nlen = (int)strlen(name);
    int room = TW - 10;
    if(room < 1) room = 1;
    if(nlen > room) nlen = room;
    lay_t l;
    l.name_x   = TILE_X + 5;
    l.name_len = nlen;
    l.badge_x  = TILE_X + 5 + nlen + 1;
    l.badge_end= l.badge_x + 2;           /* "[A]" is 3 wide */
    l.marker_x = TILE_X + TW - 2;
    l.tile_end = TILE_X + TW - 1;
    return l;
}

static int fails = 0, total = 0;
static void ck(const char *what, int cond, long got)
{
    total++;
    if(cond) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s  (got %ld)\n", what, got); }
}
static void head(const char *t){ printf("\n%s\n", t); }

int main(void)
{
    printf("Login account tiles\n===================\n");

    head("the regression: a 15-character name is not written over");
    g_names[0] = "kyanhoekstra123"; g_nv = 1;      /* exactly 15 */
    {
        lay_t l = layout(g_names[0]);
        ck("the whole name is drawn, not 13 of it", l.name_len == 15, l.name_len);
        ck("the marker sits past the end of the name",
           l.marker_x >= l.name_x + l.name_len, l.marker_x);
        ck("the badge sits past the end of the name",
           l.badge_x >= l.name_x + l.name_len, l.badge_x);
        ck("the marker does not land on the badge",
           l.marker_x > l.badge_end, l.marker_x);
        ck("the badge stays inside the tile",
           l.badge_end <= l.tile_end, l.badge_end);
        ck("the tile stays on screen", l.tile_end < g_cols, l.tile_end);
    }

    head("every length Setup will accept, 3 through 15");
    {
        char buf[20];
        for(int n = 3; n <= 15; n++){
            for(int i=0;i<n;i++) buf[i] = 'a' + (char)(i % 26);
            buf[n] = 0;
            g_names[0] = buf; g_nv = 1;
            lay_t l = layout(buf);
            char w[96];
            sprintf(w, "%2d characters: the name is complete", n);
            ck(w, l.name_len == n, l.name_len);
            sprintf(w, "%2d characters: nothing overlaps the name", n);
            ck(w, l.badge_x >= l.name_x + l.name_len
                  && l.marker_x >= l.name_x + l.name_len, l.marker_x);
            sprintf(w, "%2d characters: everything fits the tile", n);
            ck(w, l.badge_end <= l.tile_end && l.marker_x <= l.tile_end,
               l.badge_end);
        }
    }

    head("the tile grows for the longest name on screen, not the first");
    g_names[0] = "guest"; g_names[1] = "kyanhoekstra123"; g_nv = 2;
    {
        lay_t sh = layout("guest");
        lay_t lo = layout("kyanhoekstra123");
        ck("a short name in the same list still gets a wide tile",
           sh.tile_end == lo.tile_end, sh.tile_end);
        ck("and the long name is still complete", lo.name_len == 15,
           lo.name_len);
        ck("the short name's badge does not reach the marker",
           sh.badge_end < sh.marker_x, sh.badge_end);
    }

    head("the default look is unchanged for ordinary names");
    g_names[0] = "admin"; g_nv = 1;
    ck("a 5-character name still gets the original 20-wide tile",
       tile_w() == 20, tile_w());
    ck("...with the marker on the original column",
       layout("admin").marker_x == TILE_X + 20 - 2, layout("admin").marker_x);

    head("the tiles sit at the bottom of whatever mode is active");
    struct { int c, r; } modes[] = {{80,25},{80,50},{90,30},{90,60}};
    for(int i=0;i<4;i++){
        g_cols = modes[i].c; g_rows = modes[i].r;
        char w[96];
        sprintf(w, "%dx%d: the first tile is two rows off the bottom",
                modes[i].c, modes[i].r);
        ck(w, tile_base() == modes[i].r - 2, tile_base());
        sprintf(w, "%dx%d: it is on screen", modes[i].c, modes[i].r);
        ck(w, tile_base() < modes[i].r, tile_base());
    }
    /* At 80x25 this must still be the row it always was. */
    g_cols = 80; g_rows = 25;
    ck("80x25 still puts the first tile on row 23", tile_base() == 23,
       tile_base());

    head("a narrow display cuts the name instead of the furniture");
    g_cols = 24; g_rows = 25;
    g_names[0] = "kyanhoekstra123"; g_nv = 1;
    {
        lay_t l = layout(g_names[0]);
        ck("the tile is clamped to the screen", l.tile_end < g_cols,
           l.tile_end);
        ck("the name is shortened", l.name_len < 15, l.name_len);
        ck("the badge still fits", l.badge_end <= l.tile_end, l.badge_end);
        ck("the marker still fits", l.marker_x <= l.tile_end, l.marker_x);
        ck("the name never runs under the badge",
           l.badge_x >= l.name_x + l.name_len, l.badge_x);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
