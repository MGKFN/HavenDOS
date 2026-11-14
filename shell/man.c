/* ================================================================
   shell/man.c  --  manual pages.  Rewritten for v0.9.5.

   What was wrong with the old one, from Kyan's photos and a read of
   the code:

     * 46 hand-written pages for ~150 commands, kept apart from
       help.c, and wrong in places that mattered: `ls` "lists all
       files in the current filesystem" from "RAMFS", `help 2` showed
       a second page that did not exist, `cd` "RAMFS supports basic
       directory structure".
     * Drawn on a fixed 80x25 canvas: every coordinate a literal, the
       text wrapped at column 74 whatever the screen, and anything
       below row 22 silently dropped. On a 160x50 console the page sat
       in the top-left corner and still lost its notes.
     * No scrolling: one screen, any key closes.

   Now: the text comes from shell/cmddb.c, the one table that `help`
   and the build check read too. The page is laid out for the
   console it is shown on and scrolls -- Up/Down, PgUp/PgDn,
   Home/End, Space -- so nothing is ever cut off. `man` with no
   argument is a browser: every command by topic, arrow keys and
   Enter. The desktop's Manual app is that browser.
   ================================================================ */
#include "vga.h"
#include "version.h"
#include "string.h"
#include "types.h"
#include "keys.h"
#include "cmddb.h"

extern int  keyboard_waitchar(void);

/* ── The page, as laid-out lines ─────────────────────────────────── */

#define ML_MAX 200
#define ML_W   241                  /* 240 columns + NUL */

static char    ml_txt[ML_MAX][ML_W];
static uint8_t ml_fg[ML_MAX];       /* colour up to ml_split            */
static uint8_t ml_fg2[ML_MAX];      /* colour from ml_split on          */
static uint8_t ml_split[ML_MAX];
static int     ml_n;
static int     ml_width;            /* text columns available           */

static void ml_reset(void){
    int cols = vga_cols();
    if (cols > ML_W - 1) cols = ML_W - 1;
    ml_width = cols - 3;            /* a column of margin each side     */
    ml_n = 0;
}

/* One physical line: `indent` spaces, then `len` chars of `s`. */
static int ml_line(uint8_t fg, int indent, const char *s, int len){
    if (ml_n >= ML_MAX) return -1;
    int i = ml_n++, p = 0;
    while (p < indent && p < ML_W - 1) ml_txt[i][p++] = ' ';
    for (int k = 0; k < len && p < ML_W - 1; k++) ml_txt[i][p++] = s[k];
    ml_txt[i][p] = 0;
    ml_fg[i] = ml_fg2[i] = fg;
    ml_split[i] = 255;
    return i;
}

static void ml_blank(void){ ml_line(VGA_LIGHT_GREY, 0, "", 0); }

/* Word-wrap `s` into lines of at most `width` columns after `indent`.
   "\n" is a hard break, "\n\n" a blank line between paragraphs. */
static void ml_wrap(uint8_t fg, int indent, const char *s){
    int width = ml_width - indent;
    if (width < 16) width = 16;
    while (*s) {
        if (*s == '\n') {
            if (s[1] == '\n') { ml_blank(); s += 2; }
            else s++;
            continue;
        }
        /* Take as many whole words as fit. */
        int n = 0, last_space = -1;
        while (s[n] && s[n] != '\n' && n < width) {
            if (s[n] == ' ') last_space = n;
            n++;
        }
        int take = n;
        if (s[n] && s[n] != '\n' && s[n] != ' ' && last_space > 0)
            take = last_space;              /* do not split a word      */
        ml_line(fg, indent, s, take);
        s += take;
        while (*s == ' ') s++;
    }
}

static void ml_heading(const char *h){
    ml_line(VGA_LIGHT_CYAN, 1, h, (int)strlen(h));
}

/* "command\tcomment" lines: the command green, the comment grey and
   lined up in a column when the screen is wide enough for that. */
static void ml_examples(const char *ex){
    int col = 0;
    for (const char *p = ex; *p; ) {        /* widest command + 3       */
        int n = 0;
        while (p[n] && p[n] != '\t' && p[n] != '\n') n++;
        if (n + 3 > col) col = n + 3;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    while (*ex) {
        const char *c = ex; int cn = 0;
        while (c[cn] && c[cn] != '\t' && c[cn] != '\n') cn++;
        const char *m = 0; int mn = 0;
        if (c[cn] == '\t') {
            m = c + cn + 1;
            while (m[mn] && m[mn] != '\n') mn++;
        }
        ex = (m ? m + mn : c + cn);
        if (*ex == '\n') ex++;

        if (m && 5 + col + mn <= ml_width) {
            char buf[ML_W]; int p = 0;
            for (int k = 0; k < cn && p < ML_W - 1; k++) buf[p++] = c[k];
            while (p < col && p < ML_W - 1) buf[p++] = ' ';
            for (int k = 0; k < mn && p < ML_W - 1; k++) buf[p++] = m[k];
            int i = ml_line(VGA_LIGHT_GREEN, 5, buf, p);
            if (i >= 0) { ml_split[i] = (uint8_t)(5 + col); ml_fg2[i] = VGA_DARK_GREY; }
        } else {
            ml_line(VGA_LIGHT_GREEN, 5, c, cn);
            if (m) ml_line(VGA_DARK_GREY, 9, m, mn);
        }
    }
}

/* A space-separated word list, written as "a, b, c" and wrapped. */
static void ml_list(uint8_t fg, int indent, const char *words){
    char buf[512]; int p = 0;
    for (const char *w = words; *w && p < (int)sizeof buf - 3; w++) {
        if (*w == ' ') { if (p && buf[p-1] != ' ') { buf[p++] = ','; buf[p++] = ' '; } }
        else buf[p++] = *w;
    }
    buf[p] = 0;
    ml_wrap(fg, indent, buf);
}

static void ml_build_page(const cmddoc_t *d){
    ml_reset();
    ml_heading("NAME");
    {
        char buf[ML_W]; int p = 0;
        for (const char *s = d->name; *s && p < 60; s++) buf[p++] = *s;
        const char *dash = " - ";
        for (const char *s = dash; *s; s++) buf[p++] = *s;
        for (const char *s = d->summary; *s && p < ML_W - 1; s++) buf[p++] = *s;
        buf[p] = 0;
        ml_wrap(VGA_WHITE, 5, buf);
    }
    if (d->aliases) {
        ml_blank();
        ml_heading("ALSO TYPED AS");
        ml_list(VGA_WHITE, 5, d->aliases);
    }
    ml_blank();
    ml_heading("SYNOPSIS");
    for (const char *s = d->synopsis; *s; ) {
        int n = 0; while (s[n] && s[n] != '\n') n++;
        ml_line(VGA_YELLOW, 5, s, n);
        s += n; if (*s) s++;
    }
    ml_blank();
    ml_heading("DESCRIPTION");
    ml_wrap(VGA_LIGHT_GREY, 5, d->desc);
    if (d->examples) {
        ml_blank();
        ml_heading("EXAMPLES");
        ml_examples(d->examples);
    }
    if (d->see) {
        ml_blank();
        ml_heading("SEE ALSO");
        ml_list(VGA_LIGHT_GREEN, 5, d->see);
    }
    ml_blank();
    ml_heading("TOPIC");
    {
        char buf[96]; int p = 0;
        const cmdcat_t *c = &cmddb_cats[d->cat];
        for (const char *s = c->title; *s; s++) buf[p++] = *s;
        const char *mid = "   (help ";
        for (const char *s = mid; *s; s++) buf[p++] = *s;
        for (const char *s = c->key; *s; s++) buf[p++] = *s;
        buf[p++] = ')'; buf[p] = 0;
        ml_line(VGA_DARK_GREY, 5, buf, p);
    }
}

/* Every command in one topic, as a page of its own (`man network`). */
static void ml_build_topic(int cat){
    ml_reset();
    ml_heading(cmddb_cats[cat].title);
    ml_wrap(VGA_DARK_GREY, 5, cmddb_cats[cat].blurb);
    ml_blank();
    for (int i = 0; cmddb[i].name; i++) {
        if (cmddb[i].cat != cat) continue;
        char buf[ML_W]; int p = 0;
        for (const char *s = cmddb[i].name; *s; s++) buf[p++] = *s;
        while (p < 14) buf[p++] = ' ';
        int i2 = -1;
        int sp = p;
        for (const char *s = cmddb[i].summary; *s && p < ML_W - 1; s++) buf[p++] = *s;
        if (5 + p <= ml_width) {
            i2 = ml_line(VGA_LIGHT_GREEN, 5, buf, p);
            if (i2 >= 0) { ml_split[i2] = (uint8_t)(5 + sp); ml_fg2[i2] = VGA_LIGHT_GREY; }
        } else {
            ml_line(VGA_LIGHT_GREEN, 5, cmddb[i].name, (int)strlen(cmddb[i].name));
            ml_wrap(VGA_LIGHT_GREY, 9, cmddb[i].summary);
        }
    }
    ml_blank();
    ml_wrap(VGA_DARK_GREY, 5, "man <command> opens a command's page.");
}

/* ── The viewer ──────────────────────────────────────────────────── */

static void bar(int y, vga_color_t fg, vga_color_t bg){
    vga_fill_row(y, ' ', fg, bg);
}

static void put_right(const char *s, int y, vga_color_t fg, vga_color_t bg){
    int x = vga_cols() - 1 - (int)strlen(s);
    if (x < 0) x = 0;
    vga_puts_at(s, x, y, fg, bg);
}

static void utoa10(uint32_t v, char *out){
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}

/* npg > 1: a multi-page document (the guide). Left and Right then turn
   pages and the return value says which: -1 back, +1 on, 0 closed. */
static int ml_view_pg(const char *title, int pg, int npg){
    int top = 0;
    for (;;) {
        int rows = vga_rows(), cols = vga_cols();
        int body = rows - 2;
        int maxtop = ml_n - body; if (maxtop < 0) maxtop = 0;
        if (top > maxtop) top = maxtop;
        if (top < 0) top = 0;

        vga_begin_frame();
        bar(0, VGA_BLACK, VGA_CYAN);
        vga_puts_at(" ", 0, 0, VGA_BLACK, VGA_CYAN);
        vga_puts_at(title, 1, 0, VGA_BLACK, VGA_CYAN);
        if (cols >= 48) put_right("HavenDOS " HAVENDOS_VER " manual ", 0, VGA_BLACK, VGA_CYAN);

        for (int r = 0; r < body; r++) {
            int y = r + 1, li = top + r;
            bar(y, VGA_LIGHT_GREY, VGA_BLACK);
            if (li >= ml_n) continue;
            const char *t = ml_txt[li];
            for (int x = 0; t[x] && x + 1 < cols; x++) {
                vga_color_t fg = (x >= ml_split[li]) ? ml_fg2[li] : ml_fg[li];
                if (t[x] != ' ') vga_putchar_at(t[x], x + 1, y, fg, VGA_BLACK);
            }
        }

        bar(rows - 1, VGA_LIGHT_GREY, VGA_DARK_GREY);
        if (npg > 1) {
            char pgs[48]; int p = 0; char a[8], n[8];
            utoa10((uint32_t)(pg + 1), a); utoa10((uint32_t)npg, n);
            for (const char *s2 = " page "; *s2; s2++) pgs[p++] = *s2;
            for (const char *s2 = a; *s2; s2++) pgs[p++] = *s2;
            pgs[p++] = '/';
            for (const char *s2 = n; *s2; s2++) pgs[p++] = *s2;
            for (const char *s2 = cols >= 64 ? "   Left/Right page  Up/Down scroll  ESC close"
                                             : "  </> page  ESC"; *s2; s2++) pgs[p++] = *s2;
            pgs[p] = 0;
            vga_puts_at(pgs, 0, rows - 1, VGA_WHITE, VGA_DARK_GREY);
        } else if (ml_n > body)
            vga_puts_at(cols >= 60 ? " Up/Down PgUp/PgDn Home/End scroll   Q close"
                                   : " arrows scroll  Q close",
                        0, rows - 1, VGA_WHITE, VGA_DARK_GREY);
        else
            vga_puts_at(" Q or ESC closes", 0, rows - 1, VGA_WHITE, VGA_DARK_GREY);
        if (ml_n > body && (npg <= 1 || cols >= 80)) {
            char pos[40], a[12], b[12], n[12];
            utoa10((uint32_t)(top + 1), a);
            utoa10((uint32_t)((top + body < ml_n) ? top + body : ml_n), b);
            utoa10((uint32_t)ml_n, n);
            int p = 0;
            for (const char *s = "lines "; *s; s++) pos[p++] = *s;
            for (const char *s = a; *s; s++) pos[p++] = *s;
            pos[p++] = '-';
            for (const char *s = b; *s; s++) pos[p++] = *s;
            for (const char *s = " of "; *s; s++) pos[p++] = *s;
            for (const char *s = n; *s; s++) pos[p++] = *s;
            pos[p++] = ' '; pos[p] = 0;
            put_right(pos, rows - 1, VGA_LIGHT_GREY, VGA_DARK_GREY);
        }
        vga_end_frame();

        int k = keyboard_waitchar();
        if (k == 27 || k == 'q' || k == 'Q') break;
        else if (npg > 1 && k == KEY_RIGHT) { if (pg + 1 < npg) { vga_clear(); return 1; } }
        else if (npg > 1 && k == KEY_LEFT)  { if (pg > 0)       { vga_clear(); return -1; } }
        else if (npg > 1 && (k == ' ' || k == '\n' || k == '\r') && top >= maxtop) {
            if (pg + 1 < npg) { vga_clear(); return 1; }
            break;
        }
        else if (k == KEY_DOWN || k == 'j' || k == '\n' || k == '\r') top++;
        else if (k == KEY_UP || k == 'k') top--;
        else if (k == KEY_PGDN || k == ' ') top += body - 1;
        else if (k == KEY_PGUP || k == 'b') top -= body - 1;
        else if (k == KEY_HOME || k == 'g') top = 0;
        else if (k == KEY_END  || k == 'G') top = maxtop;
        else if (ml_n <= body && npg <= 1) break;   /* nothing to scroll: any key */
    }
    vga_clear();
    return 0;
}
static void ml_view(const char *title){ (void)ml_view_pg(title, 0, 0); }

/* ── Light markup, for the guide ──────────────────────────────────
     # Heading
     - a bullet, wrapped with a hanging indent
     > command<TAB>what it does      (a run of these lines up together)
     plain lines                      joined into one wrapped paragraph
     an empty line                    a gap                              */
static void ml_build_markup(const char *t){
    ml_reset();
    static char para[1024];
    static char ex[1024];
    int pn = 0, en = 0;
    while (*t) {
        const char *e = t; while (*e && *e != '\n') e++;
        int len = (int)(e - t);
        int is_ex = (len >= 2 && t[0] == '>' && t[1] == ' ');
        if (!is_ex && en) { ex[en] = 0; ml_examples(ex); en = 0; }
        if (len == 0 || t[0] == '#' || t[0] == '-' || is_ex) {
            if (pn) { para[pn] = 0; ml_wrap(VGA_LIGHT_GREY, 3, para); pn = 0; }
        }
        if (len == 0) {
            ml_blank();
        } else if (t[0] == '#') {
            const char *h = t + 1; while (*h == ' ') h++;
            ml_line(VGA_LIGHT_CYAN, 1, h, (int)(e - h));
        } else if (t[0] == '-' && len > 1 && t[1] == ' ') {
            char b[ML_W]; int k = 0;
            for (const char *c = t + 2; c < e && k < ML_W - 1; c++) b[k++] = *c;
            b[k] = 0;
            int first = ml_n;
            ml_wrap(VGA_LIGHT_GREY, 7, b);
            if (first < ml_n) { ml_txt[first][5] = '-'; }
        } else if (is_ex) {
            for (const char *c = t + 2; c < e && en < (int)sizeof ex - 2; c++) ex[en++] = *c;
            ex[en++] = '\n';
        } else {
            if (pn && pn < (int)sizeof para - 1) para[pn++] = ' ';
            for (const char *c = t; c < e && pn < (int)sizeof para - 1; c++) para[pn++] = *c;
        }
        t = *e ? e + 1 : e;
    }
    if (en) { ex[en] = 0; ml_examples(ex); }
    if (pn) { para[pn] = 0; ml_wrap(VGA_LIGHT_GREY, 3, para); }
}

/* A document of pages: the guide. */
void man_pages_view(const char *const *titles, const char *const *texts, int n, int start){
    int pg = (start >= 0 && start < n) ? start : 0;
    for (;;) {
        ml_build_markup(texts[pg]);
        int r = ml_view_pg(titles[pg], pg, n);
        if (r == 0) break;
        pg += r;
        if (pg < 0) pg = 0;
        if (pg >= n) break;
    }
    vga_clear();
}

/* ── The browser: `man` with no argument, and the Manual app ─────── */

#define BR_MAX 200
static int16_t br_item[BR_MAX];     /* >= 0 command index, < 0 -(cat+1) */
static int     br_n;

static void br_build(void){
    br_n = 0;
    for (int c = 0; c < CDB_NCAT && br_n < BR_MAX; c++) {
        br_item[br_n++] = (int16_t)(-(c + 1));
        for (int i = 0; cmddb[i].name && br_n < BR_MAX; i++)
            if (cmddb[i].cat == c) br_item[br_n++] = (int16_t)i;
    }
}

static int br_next(int from, int dir){
    for (int i = from + dir; i >= 0 && i < br_n; i += dir)
        if (br_item[i] >= 0) return i;
    return from;
}

void man_browse(void){
    br_build();
    int sel = br_next(-1, 1), top = 0;
    for (;;) {
        int rows = vga_rows(), cols = vga_cols();
        int body = rows - 3;
        if (sel < top + 1) top = sel - 1;
        if (sel >= top + body) top = sel - body + 1;
        if (top < 0) top = 0;

        vga_begin_frame();
        bar(0, VGA_BLACK, VGA_CYAN);
        vga_puts_at(" Manual -- every HavenDOS command", 0, 0, VGA_BLACK, VGA_CYAN);
        if (cols >= 64) put_right("HavenDOS " HAVENDOS_VER " ", 0, VGA_BLACK, VGA_CYAN);
        bar(1, VGA_LIGHT_GREY, VGA_BLACK);
        for (int r = 0; r < body; r++) {
            int y = r + 2, li = top + r;
            bar(y, VGA_LIGHT_GREY, VGA_BLACK);
            if (li >= br_n) continue;
            int it = br_item[li];
            if (it < 0) {
                const cmdcat_t *c = &cmddb_cats[-it - 1];
                vga_puts_at(c->title, 2, y, VGA_LIGHT_CYAN, VGA_BLACK);
                continue;
            }
            const cmddoc_t *d = &cmddb[it];
            vga_color_t bg = (li == sel) ? VGA_CYAN : VGA_BLACK;
            if (li == sel) for (int x = 3; x < cols - 1; x++) vga_putchar_at(' ', x, y, VGA_BLACK, bg);
            vga_puts_at(d->name, 4, y, li == sel ? VGA_BLACK : VGA_LIGHT_GREEN, bg);
            /* summary, cut to the screen */
            int x = 19;
            for (const char *s = d->summary; *s && x < cols - 2; s++, x++)
                vga_putchar_at(*s, x, y, li == sel ? VGA_BLACK : VGA_LIGHT_GREY, bg);
        }
        bar(rows - 1, VGA_LIGHT_GREY, VGA_DARK_GREY);
        vga_puts_at(cols >= 64 ? " Up/Down choose  Enter open  a-z jump  ESC close"
                               : " arrows  Enter  ESC",
                    0, rows - 1, VGA_WHITE, VGA_DARK_GREY);
        vga_end_frame();

        int k = keyboard_waitchar();
        if (k == 27 || k == 'Q') break;
        else if (k == KEY_DOWN) sel = br_next(sel, 1);
        else if (k == KEY_UP)   sel = br_next(sel, -1);
        else if (k == KEY_PGDN) { for (int i = 0; i < body - 1; i++) sel = br_next(sel, 1); }
        else if (k == KEY_PGUP) { for (int i = 0; i < body - 1; i++) sel = br_next(sel, -1); }
        else if (k == KEY_HOME) { sel = br_next(-1, 1); top = 0; }
        else if (k == KEY_END)  { sel = br_next(br_n, -1); }
        else if (k == '\n' || k == '\r') {
            const cmddoc_t *d = &cmddb[br_item[sel]];
            ml_build_page(d);
            ml_view(d->name);
        } else if (k >= 'a' && k <= 'z') {
            /* next command starting with that letter, wrapping round */
            for (int step = 1; step <= br_n; step++) {
                int i = (sel + step) % br_n;
                if (br_item[i] >= 0 && cmddb[br_item[i]].name[0] == (char)k) { sel = i; break; }
            }
        }
    }
    vga_clear();
}

/* ── man <command> ───────────────────────────────────────────────── */

/* Names that start like `w`, for "did you mean". */
static void suggest(const char *w){
    int shown = 0, n = (int)strlen(w);
    if (n > 2) n = 2;
    for (int i = 0; cmddb[i].name && shown < 8; i++) {
        if (!strncmp(cmddb[i].name, w, (size_t)n)) {
            if (!shown) vga_puts("  Did you mean:");
            vga_putchar(' ');
            vga_puts(cmddb[i].name);
            shown++;
        }
    }
    if (shown) vga_putchar('\n');
}

void cmd_man(const char *word){
    if (!word || !word[0]) { man_browse(); return; }
    const cmddoc_t *d = cmddb_find(word);
    if (d) { ml_build_page(d); ml_view(d->name); return; }
    int cat = cmddb_cat_find(word);
    if (cat >= 0) { ml_build_topic(cat); ml_view(cmddb_cats[cat].title); return; }
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    vga_printf("man: no page for '%s'\n", word);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    suggest(word);
    vga_puts("  'man' on its own lists every command; 'help' lists the topics.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}
