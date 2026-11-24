/* shell/uimouse.c -- the mouse in every app (v0.9.6.3)
 *
 * include/uimouse.h has the whole design. In short: clicks, the wheel
 * and the right button become the keys an app already reads, from
 * inside keyboard_getchar(), for the task in front only.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/keys.h"
#include "../include/uimouse.h"

extern void mouse_get(int *x, int *y, int *btn);
extern int  mouse_is_ready(void);
extern int  mouse_get_clicks(void);
extern int  mouse_get_wheel(void);
extern int  mouse_outside(void);
extern void keyboard_set_mouse_hook(int (*fn)(int));

/* ── per task ──────────────────────────────────────────────────────── */
#define UM_SLOTS   9
#define UM_MAXZ    32
#define UM_ZKEYS   16
#define UM_MAXR    64
#define UM_GROUPS  4
#define UM_PKEYS   4

typedef struct { int16_t x, y, w, h; uint8_t n; int keys[UM_ZKEYS]; } um_zone_t;
typedef struct { int16_t x, y, w; uint8_t group, sel; } um_row_t;

typedef struct {
    uint32_t  id;                     /* the task this table belongs to */
    int       mode;
    int       nz, nr;
    um_zone_t z[UM_MAXZ];
    um_row_t  r[UM_MAXR];
    int       np[UM_GROUPS];
    int       pre[UM_GROUPS][UM_PKEYS];
} um_slot_t;

static um_slot_t g_s[UM_SLOTS];
static int      (*g_slot_fn)(void);
static uint32_t (*g_id_fn)(int);

void um_set_task_fns(int (*slot)(void), uint32_t (*id)(int))
{
    g_slot_fn = slot; g_id_fn = id;
}

void um_reset_all(void)
{
    int i;
    memset(g_s, 0, sizeof g_s);
    for(i = 0; i < UM_SLOTS; i++) g_s[i].mode = UM_ALL;
}

/* The current task's table. A slot whose task id changed belongs to a
   new task: it starts empty, with every behaviour on. */
static um_slot_t *cur(void)
{
    int s = g_slot_fn ? g_slot_fn() : 0;
    uint32_t id;
    if(s < 0 || s >= UM_SLOTS) s = 0;
    id = g_id_fn ? g_id_fn(s) : 0;
    if(g_s[s].id != id){
        memset(&g_s[s], 0, sizeof g_s[s]);
        g_s[s].id = id;
        g_s[s].mode = UM_ALL;
    }
    return &g_s[s];
}

void um_set_mode(int mode){ cur()->mode = mode & UM_ALL; }
int  um_mode(void){ return cur()->mode; }

void um_frame(void)
{
    um_slot_t *t = cur();
    t->nz = 0; t->nr = 0;
    memset(t->np, 0, sizeof t->np);
}

void um_row(int group, int x, int y, int w, int selected)
{
    um_slot_t *t = cur();
    int i;
    if(group < 0 || group >= UM_GROUPS || w <= 0) return;
    /* drawn again at the same place: the same row, updated */
    for(i = 0; i < t->nr; i++)
        if(t->r[i].y == y && t->r[i].x == x && t->r[i].group == group){
            t->r[i].w = (int16_t)w; t->r[i].sel = (uint8_t)(selected != 0);
            return;
        }
    if(t->nr >= UM_MAXR) return;
    t->r[t->nr].x = (int16_t)x; t->r[t->nr].y = (int16_t)y; t->r[t->nr].w = (int16_t)w;
    t->r[t->nr].group = (uint8_t)group; t->r[t->nr].sel = (uint8_t)(selected != 0);
    t->nr++;
}

void um_group_prefix(int group, const int *keys, int n)
{
    um_slot_t *t = cur();
    int i;
    if(group < 0 || group >= UM_GROUPS) return;
    if(n > UM_PKEYS) n = UM_PKEYS;
    if(n < 0) n = 0;
    for(i = 0; i < n; i++) t->pre[group][i] = keys[i];
    t->np[group] = n;
}

void um_keys(int x, int y, int w, int h, const int *keys, int n)
{
    um_slot_t *t = cur();
    um_zone_t *z;
    int i;
    if(w <= 0 || h <= 0 || n <= 0) return;
    for(i = 0; i < t->nz; i++)        /* the same button, drawn again */
        if(t->z[i].x == x && t->z[i].y == y && t->z[i].w == w && t->z[i].h == h) break;
    if(i == t->nz){ if(t->nz >= UM_MAXZ) return; t->nz++; }
    z = &t->z[i];
    if(n > UM_ZKEYS) n = UM_ZKEYS;
    z->x = (int16_t)x; z->y = (int16_t)y; z->w = (int16_t)w; z->h = (int16_t)h;
    z->n = (uint8_t)n;
    for(i = 0; i < n; i++) z->keys[i] = keys[i];
}

void um_key(int x, int y, int w, int h, int key){ um_keys(x, y, w, h, &key, 1); }

/* ── hints ─────────────────────────────────────────────────────────── */
static int lower(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int tok_is(const char *t, int n, const char *name)
{
    int i;
    for(i = 0; i < n; i++) if(!name[i] || lower(t[i]) != name[i]) return 0;
    return name[n] == 0;
}

/* One alternative of a hint's key, e.g. "Enter", "ESC", "Q", "^X". */
static int tok_key(const char *t, int n)
{
    if(n <= 0) return 0;
    if(n == 2 && t[0] == '^') return t[1] & 0x1F;
    if(n == 6 && (tok_is(t, 5, "ctrl+") || tok_is(t, 5, "ctrl-"))) return t[5] & 0x1F;
    if(n == 1){
        int c = t[0];
        if(c >= 'A' && c <= 'Z') return c + 32;
        if((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return c;
        if(c == '?' || c == '+' || c == '-' || c == '/' || c == '*') return c;
        return 0;
    }
    if(tok_is(t, n, "enter") || tok_is(t, n, "return")) return '\n';
    if(tok_is(t, n, "esc") || tok_is(t, n, "escape")) return 27;
    if(tok_is(t, n, "tab"))   return '\t';
    if(tok_is(t, n, "space") || tok_is(t, n, "spc")) return ' ';
    if(tok_is(t, n, "up"))    return KEY_UP;
    if(tok_is(t, n, "down") || tok_is(t, n, "dn")) return KEY_DOWN;
    if(tok_is(t, n, "left"))  return KEY_LEFT;
    if(tok_is(t, n, "right")) return KEY_RIGHT;
    if(tok_is(t, n, "pgup"))  return KEY_PGUP;
    if(tok_is(t, n, "pgdn"))  return KEY_PGDN;
    if(tok_is(t, n, "home"))  return KEY_HOME;
    if(tok_is(t, n, "end"))   return KEY_END;
    if(tok_is(t, n, "del") || tok_is(t, n, "delete")) return KEY_DEL;
    if(tok_is(t, n, "backspace") || tok_is(t, n, "bksp")) return '\b';
    if(tok_is(t, n, "f1")) return KEY_F1;
    if(tok_is(t, n, "f2")) return KEY_F2;
    if(tok_is(t, n, "f3")) return KEY_F3;
    if(tok_is(t, n, "f4")) return KEY_F4;
    return 0;
}

static int opposite(int a, int b)
{
    return (a == KEY_UP && b == KEY_DOWN) || (a == KEY_DOWN && b == KEY_UP) ||
           (a == KEY_LEFT && b == KEY_RIGHT) || (a == KEY_RIGHT && b == KEY_LEFT) ||
           (a == KEY_PGUP && b == KEY_PGDN) || (a == KEY_PGDN && b == KEY_PGUP) ||
           (a == '+' && b == '-') || (a == '-' && b == '+');
}

/* What separates hints: two or more spaces, a '|', a ", " (the pager's
   "SPACE continues, Q stops") and a " -- " dash. */
static int is_gap(const char *l, int len, int i)
{
    if(i < 0 || i >= len) return 1;
    if(l[i] == '|' || l[i] == ',') return 1;
    if(l[i] == '-' && ((i + 1 < len && l[i + 1] == '-') || (i > 0 && l[i - 1] == '-'))) return 1;
    if(l[i] != ' ') return 0;
    if((i + 1 < len && (l[i + 1] == ' ' || (l[i + 1] == '-' && i + 2 < len && l[i + 2] == '-'))) ||
       (i > 0 && (l[i - 1] == ' ' || l[i - 1] == ',' || (l[i - 1] == '-' && i > 1 && l[i - 2] == '-'))))
        return 1;
    return 0;
}

int um_hint_key(const char *line, int len, int col)
{
    int a, b, i, k, first = 0, n;
    const char *t;
    if(!line || col < 0 || col >= len || is_gap(line, len, col)) return 0;
    a = col; while(a > 0 && !is_gap(line, len, a - 1)) a--;
    b = col; while(b + 1 < len && !is_gap(line, len, b + 1)) b++;
    /* "[Q] quit" and "<Enter> ok": the brackets are decoration */
    if(line[a] == '[' || line[a] == '<'){
        for(i = a + 1; i <= b && line[i] != ']' && line[i] != '>'; i++) ;
        if(i > b) return 0;
        t = line + a + 1; n = i - a - 1;
        i++;                                   /* past the bracket   */
    } else {
        t = line + a;
        for(i = a; i <= b && line[i] != ' ' && line[i] != '=' && line[i] != ':'; i++) ;
        n = i - a;
    }
    /* a key needs words after it: "Enter apply", "Q=quit" */
    while(i <= b && (line[i] == ' ' || line[i] == '=' || line[i] == ':')) i++;
    if(i > b) return 0;
    if(!((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z'))) return 0;
    /* "Enter/D", "Up/Down", "Left/Right": the first alternative -- unless
       the alternatives are opposites, which no single click can mean */
    for(i = 0, k = 0; i <= n; i++){
        if(i == n || t[i] == '/'){
            int key = tok_key(t + k, i - k);
            if(!key) return 0;
            if(!first) first = key;
            else if(opposite(first, key)) return 0;
            k = i + 1;
        }
    }
    return first;
}

/* ── clicks ────────────────────────────────────────────────────────── */
int um_click_keys(int x, int y, const char *line, int len, int *out, int max)
{
    um_slot_t *t = cur();
    int i, n = 0;
    if(max <= 0) return 0;

    if(t->mode & UM_ZONES){
        for(i = t->nz - 1; i >= 0; i--){       /* the last drawn is on top */
            um_zone_t *z = &t->z[i];
            if(x >= z->x && x < z->x + z->w && y >= z->y && y < z->y + z->h){
                for(n = 0; n < z->n && n < max; n++) out[n] = z->keys[n];
                return n;
            }
        }
        for(i = 0; i < t->nr; i++){
            um_row_t *r = &t->r[i];
            if(y == r->y && x >= r->x && x < r->x + r->w){
                /* d: the clicked row's place in its group; s: the
                   selected row's. Both in drawing order. */
                int g = r->group, j = 0, s = -1, k, d = 0, np = t->np[g];
                for(k = 0; k < t->nr; k++){
                    if(t->r[k].group != g) continue;
                    if(k == i) d = j;
                    if(t->r[k].sel) s = j;
                    j++;
                }
                for(k = 0; k < np && n < max; k++) out[n++] = t->pre[g][k];
                if(s < 0) return n;             /* nothing selected: can't count */
                if(d == s && !np){ out[n++] = '\n'; return n; }
                for(k = s; k < d && n < max; k++) out[n++] = KEY_DOWN;
                for(k = s; k > d && n < max; k--) out[n++] = KEY_UP;
                return n;
            }
        }
    }
    if((t->mode & UM_HINTS) && line && y >= vga_rows() - 2){
        int key = um_hint_key(line, len, x);
        if(key){ out[0] = key; return 1; }
    }
    return 0;
}

/* ── the pointer ───────────────────────────────────────────────────── */
static int      p_x = -1, p_y = -1;
static uint16_t p_saved, p_drawn;

static int      p_arrow;              /* v0.9.7.10: this code put the arrow up */
static void pointer_hide(void)
{
    /* v0.9.7.10: only an arrow THIS code showed. The hook runs for
       every keyboard poll, also where nothing here is tracking the
       mouse (the desktop), and hiding unconditionally took the
       desktop's own arrow away as fast as it was drawn. */
    if(p_arrow){ p_arrow = 0; vga_pointer_hide(); }
    if(p_x < 0) return;
    /* Only if the cell still holds the pointer: an app that drew over it
       has the right cell there already, and putting the old one back
       would undo its drawing. */
    if(vga_shadow_read(p_x, p_y) == p_drawn) vga_shadow_write(p_x, p_y, p_saved);
    p_x = p_y = -1;
}

static void pointer_show(int x, int y)
{
    uint16_t c;
    /* v0.9.7.10: on a framebuffer, the arrow (drivers/vga.c), drawn in
       pixels over the screen; no cell is touched. */
    if(vga_pointer_pixels()){
        extern void mouse_get_phys(int*,int*);
        extern void mouse_get_frac(int*,int*);
        extern int  mouse_pointer_wanted(void);
        int px, py, fx, fy;
        if(p_x >= 0){ if(vga_shadow_read(p_x, p_y) == p_drawn) vga_shadow_write(p_x, p_y, p_saved); p_x = p_y = -1; }
        if(!mouse_pointer_wanted()){ pointer_hide(); return; }
        mouse_get_phys(&px, &py); mouse_get_frac(&fx, &fy);
        if(vga_pointer_show(px, py, fx, fy, 0)){ p_arrow = 1; return; }
    }
    if(x == p_x && y == p_y && vga_shadow_read(x, y) == p_drawn) return;
    pointer_hide();
    if(x < 0 || y < 0 || x >= vga_cols() || y >= vga_rows()) return;
    c = vga_shadow_read(x, y);
    p_saved = c;
    /* the classic text-mode pointer: the cell's colours swapped, so it
       shows on any background -- a cyan pointer vanished on Settings'
       cyan selection bar. A cell whose colours are the same both ways
       gets white on grey instead. */
    {
        uint8_t a = (uint8_t)(c >> 8), inv = (uint8_t)(((a & 0x0Fu) << 4) | ((a >> 4) & 0x07u));
        if(inv == a || (a & 0x0Fu) == ((a >> 4) & 0x07u)) inv = 0x7F;
        p_drawn = (uint16_t)((c & 0x00FFu) | ((uint16_t)inv << 8));
    }
    vga_shadow_write(x, y, p_drawn);
    p_x = x; p_y = y;
}

/* ── the hook ──────────────────────────────────────────────────────── */
static int g_q[UM_SEQ_MAX], g_qh, g_qt;
static int q_push(int k){ int n = (g_qt + 1) % UM_SEQ_MAX; if(n == g_qh) return 0; g_q[g_qt] = k; g_qt = n; return 1; }
static int q_pop(void){ int k; if(g_qh == g_qt) return -1; k = g_q[g_qh]; g_qh = (g_qh + 1) % UM_SEQ_MAX; return k; }

int um_hook(int key)
{
    um_slot_t *t;
    int mx, my, mb, clicks, w, i;
    if(key != -1){ pointer_hide(); return key; }
    if((key = q_pop()) != -1){ pointer_hide(); return key; }

    t = cur();
    if(!t->mode || !mouse_is_ready()){ pointer_hide(); return -1; }
    /* v0.9.6.5: outside the app's box the pointer and its clicks are the
       window's (shell/chrome.c draws it there); here, the box's edge
       would only show a second, wrong pointer */
    if(mouse_outside()){ pointer_hide(); return -1; }
    mouse_get(&mx, &my, &mb);
    if(t->mode & UM_POINTER) pointer_show(mx, my);

    clicks = (t->mode & (UM_ZONES | UM_HINTS | UM_RIGHT)) ? mouse_get_clicks() : 0;
    if(clicks & 1){
        char line[256]; int len = vga_cols(), out[UM_SEQ_MAX], n;
        if(len > 255) len = 255;
        for(i = 0; i < len; i++){
            int c = vga_shadow_read(i, my) & 0xFF;
            /* the pointer's own cell reads as what it covers */
            if(i == p_x && my == p_y) c = p_saved & 0xFF;
            line[i] = (char)(c ? c : ' ');
        }
        line[len] = 0;
        n = um_click_keys(mx, my, line, len, out, UM_SEQ_MAX);
        for(i = 0; i < n; i++) q_push(out[i]);
    }
    if((clicks & 2) && (t->mode & UM_RIGHT)) q_push(27);
    if(t->mode & UM_WHEEL){
        w = mouse_get_wheel();
        if(w > 5) w = 5;
        if(w < -5) w = -5;
        for(; w > 0; w--) q_push(KEY_UP);
        for(; w < 0; w++) q_push(KEY_DOWN);
    }
    if((key = q_pop()) != -1){ pointer_hide(); return key; }
    return -1;
}

/* The clear hook: a screen wiped is a screen whose zones are gone. */
static void on_clear(void){ um_frame(); p_x = p_y = -1; }

void um_init(void)
{
    extern int      task_current(void);
    extern uint32_t task_id(int slot);
    extern void     vga_set_clear_hook(void (*fn)(void));
    um_reset_all();
    um_set_task_fns(task_current, task_id);
    vga_set_clear_hook(on_clear);
    keyboard_set_mouse_hook(um_hook);
}
