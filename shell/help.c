/* ================================================================
   shell/help.c  --  `help`.  Rewritten for v0.9.5.

   The old screen was one long list in six categories, written by
   hand, paged every 20 lines whatever the screen height, and wrong
   by 0.9.4: it still sent people to the desktop for apps that had
   become commands, called the editor "notepad", missed `uninstall`,
   and had no way to ask about one command.

   Now it is an index. `help` lists the topics; `help <topic>` the
   commands in one; `help <command>` one command's usage in a few
   lines; `help all` everything. The words come from shell/cmddb.c --
   the same table `man` renders and tools/check_help.py holds to the
   shell's dispatcher -- so help cannot describe a command that does
   not exist or miss one that does.

   It pages by the console's real height and wraps to its real width,
   from 64x24 to 240x135.
   ================================================================ */
#include "types.h"
#include "vga.h"
#include "version.h"
#include "string.h"
#include "cmddb.h"

extern int keyboard_waitchar(void);

/* ── Paged output ────────────────────────────────────────────────── */

static int hp_lines, hp_stop;

static void hp_reset(void){ hp_lines = 0; hp_stop = 0; }

static void hp_nl(void){
    if (hp_stop) return;
    vga_putchar('\n');
    if (++hp_lines >= vga_rows() - 2) {
        vga_set_color(VGA_BLACK, VGA_LIGHT_GREY);
        vga_puts(" -- more -- any key, Q stops ");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        int k = keyboard_waitchar();
        vga_puts("\r                              \r");
        if (k == 'q' || k == 'Q' || k == 27) hp_stop = 1;
        hp_lines = 0;
    }
}

static void hp_put(vga_color_t fg, const char *s){
    if (hp_stop) return;
    vga_set_color(fg, VGA_BLACK);
    vga_puts(s);
}

static void hp_putn(vga_color_t fg, const char *s, int n){
    if (hp_stop) return;
    vga_set_color(fg, VGA_BLACK);
    for (int i = 0; i < n && s[i]; i++) vga_putchar(s[i]);
}

static void hp_pad(int n){ if (!hp_stop) while (n-- > 0) vga_putchar(' '); }

/* Wrap `s` at the console width, every line starting at `indent`.
   Stops at the first blank line ("\n\n") when `first_para` is set. */
static void hp_wrap(vga_color_t fg, int indent, const char *s, int first_para){
    int width = vga_cols() - indent - 1;
    if (width < 20) width = 20;
    while (*s && !hp_stop) {
        if (*s == '\n') {
            if (s[1] == '\n') { if (first_para) return; hp_nl(); s += 2; }
            else s++;
            continue;
        }
        int n = 0, sp = -1;
        while (s[n] && s[n] != '\n' && n < width) { if (s[n] == ' ') sp = n; n++; }
        int take = (s[n] && s[n] != '\n' && s[n] != ' ' && sp > 0) ? sp : n;
        hp_pad(indent);
        hp_putn(fg, s, take);
        hp_nl();
        s += take;
        while (*s == ' ') s++;
    }
}

static void hp_title(const char *left){
    int cols = vga_cols();
    vga_set_color(VGA_BLACK, VGA_CYAN);
    int n = 0;
    vga_putchar(' '); n++;
    for (const char *s = left; *s && n < cols - 1; s++, n++) vga_putchar(*s);
    const char *right = "HavenDOS " HAVENDOS_VER " ";
    int rl = (int)strlen(right);
    if (n + rl + 2 < cols) { while (n < cols - rl) { vga_putchar(' '); n++; } vga_puts(right); n += rl; }
    while (n < cols) { vga_putchar(' '); n++; }
    vga_set_color(VGA_WHITE, VGA_BLACK);
    /* A full-width line already wrapped the cursor on most consoles;
       count it as a printed line either way. */
    hp_lines++;
}

/* ── One topic: name + summary per command ───────────────────────── */

static void help_topic(int cat){
    const cmdcat_t *c = &cmddb_cats[cat];
    hp_put(VGA_LIGHT_CYAN, "  ");
    hp_put(VGA_LIGHT_CYAN, c->title);
    hp_put(VGA_DARK_GREY, "  -- help ");
    hp_put(VGA_DARK_GREY, c->key);
    hp_nl();
    int cols = vga_cols();
    for (int i = 0; cmddb[i].name && !hp_stop; i++) {
        if (cmddb[i].cat != cat) continue;
        int nl = (int)strlen(cmddb[i].name);
        hp_pad(4);
        hp_put(VGA_LIGHT_GREEN, cmddb[i].name);
        int col = 4 + nl;
        int at = (col + 2 > 18) ? col + 2 : 18;
        int sl = (int)strlen(cmddb[i].summary);
        if (at + sl >= cols) { hp_nl(); hp_pad(8); col = 8; at = 8; }
        hp_pad(at - col);
        hp_putn(VGA_LIGHT_GREY, cmddb[i].summary, cols - at - 1);
        hp_nl();
    }
}

/* ── The index ───────────────────────────────────────────────────── */

static void help_index(void){
    int cols = vga_cols();
    hp_title("Command reference");
    hp_nl();
    hp_put(VGA_WHITE, "  Topics");
    hp_put(VGA_DARK_GREY, "  -- type help <topic>");
    hp_nl();
    for (int c = 0; c < CDB_NCAT && !hp_stop; c++) {
        hp_pad(4);
        hp_put(VGA_LIGHT_GREEN, cmddb_cats[c].key);
        hp_pad(10 - (int)strlen(cmddb_cats[c].key));
        /* the topic's commands, as many as fit */
        int x = 14;
        int first = 1;
        for (int i = 0; cmddb[i].name; i++) {
            if (cmddb[i].cat != c) continue;
            int nl = (int)strlen(cmddb[i].name);
            if (x + nl + (first ? 0 : 1) + 4 >= cols) { hp_put(VGA_DARK_GREY, " ..."); break; }
            if (!first) { hp_put(VGA_LIGHT_GREY, " "); x++; }
            hp_put(VGA_LIGHT_GREY, cmddb[i].name);
            x += nl; first = 0;
        }
        hp_nl();
    }
    hp_nl();
    hp_put(VGA_WHITE, "  More");
    hp_nl();
    static const char *more[][2] = {
        {"help <command>", "how to use one command"},
        {"man <command>",  "its full page"},
        {"help all",       "every command by topic"},
        {"man",            "browse them all"},
        {"guide",          "a tour of HavenDOS"},
    };
    /* Two columns where they fit (80 and up), one below that. */
    int two = cols >= 80;
    for (int i = 0; i < 5 && !hp_stop; i++) {
        hp_pad((two && (i % 2)) ? 2 : 4);
        hp_put(VGA_LIGHT_GREEN, more[i][0]);
        hp_pad(16 - (int)strlen(more[i][0]));
        hp_put(VGA_LIGHT_GREY, more[i][1]);
        if (two && (i % 2) == 0 && i + 1 < 5) {      /* 4+16+24 = 44 */
            hp_pad(24 - (int)strlen(more[i][1]));
            continue;
        }
        hp_nl();
    }
    hp_nl();
    hp_wrap(VGA_DARK_GREY, 2,
        "Tab completes names, Up/Down recall commands. cmd . cmd pipes, "
        "> file saves output, >> file appends, $NAME is a variable.", 0);
}

/* ── One command, briefly ────────────────────────────────────────── */

static void help_command(const cmddoc_t *d){
    hp_pad(2);
    hp_put(VGA_LIGHT_GREEN, d->name);
    hp_put(VGA_DARK_GREY, "  -  ");
    hp_put(VGA_WHITE, d->summary);
    hp_nl();
    if (d->aliases) {
        hp_pad(2);
        hp_put(VGA_DARK_GREY, "also: ");
        hp_put(VGA_LIGHT_GREY, d->aliases);
        hp_nl();
    }
    hp_nl();
    hp_put(VGA_LIGHT_CYAN, "  Usage");
    hp_nl();
    for (const char *s = d->synopsis; *s && !hp_stop; ) {
        int n = 0; while (s[n] && s[n] != '\n') n++;
        hp_pad(4); hp_putn(VGA_YELLOW, s, n); hp_nl();
        s += n; if (*s) s++;
    }
    hp_nl();
    hp_wrap(VGA_LIGHT_GREY, 2, d->desc, 1);
    if (d->examples) {
        /* The first example only; man has them all. */
        const char *e = d->examples;
        int cn = 0; while (e[cn] && e[cn] != '\t' && e[cn] != '\n') cn++;
        hp_nl();
        hp_put(VGA_LIGHT_CYAN, "  Example  ");
        hp_putn(VGA_LIGHT_GREEN, e, cn);
        if (e[cn] == '\t') {
            const char *m = e + cn + 1; int mn = 0;
            while (m[mn] && m[mn] != '\n') mn++;
            if (11 + cn + 3 + mn < vga_cols()) { hp_put(VGA_DARK_GREY, "   "); hp_putn(VGA_DARK_GREY, m, mn); }
        }
        hp_nl();
    }
    hp_nl();
    hp_put(VGA_DARK_GREY, "  The whole page: man ");
    hp_put(VGA_DARK_GREY, d->name);
    if (d->see) { hp_put(VGA_DARK_GREY, "     Related: "); hp_put(VGA_DARK_GREY, d->see); }
    hp_nl();
}

/* ── Entry ───────────────────────────────────────────────────────── */

void cmd_help_arg(const char *word){
    hp_reset();
    if (!word || !word[0]) { help_index(); return; }

    if (!strcmp(word, "all")) {
        hp_title("Every command");
        for (int c = 0; c < CDB_NCAT && !hp_stop; c++) { hp_nl(); help_topic(c); }
        hp_nl();
        hp_put(VGA_DARK_GREY, "  help <command> or man <command> for one of them.");
        hp_nl();
        return;
    }
    int cat = cmddb_cat_find(word);
    if (cat >= 0) {
        help_topic(cat);
        hp_nl();
        hp_put(VGA_DARK_GREY, "  help <command> for its usage, man <command> for the full page.");
        hp_nl();
        return;
    }
    const cmddoc_t *d = cmddb_find(word);
    if (d) { help_command(d); return; }

    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    vga_printf("help: no topic or command called '%s'\n", word);
    int shown = 0, n = (int)strlen(word);
    if (n > 2) n = 2;
    for (int i = 0; cmddb[i].name && shown < 8; i++) {
        if (!strncmp(cmddb[i].name, word, (size_t)n)) {
            if (!shown) { vga_set_color(VGA_LIGHT_GREY, VGA_BLACK); vga_puts("  Did you mean:"); }
            vga_putchar(' '); vga_puts(cmddb[i].name); shown++;
        }
    }
    if (shown) vga_putchar('\n');
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts("  'help' on its own lists the topics.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

void cmd_help(void){ cmd_help_arg(""); }
