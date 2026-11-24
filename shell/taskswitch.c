/* ════════════════════════════════════════════════════════════════════
   shell/taskswitch.c -- Alt+Tab, and the shell's task commands.  v0.9.6.

   include/task.h says what a task is. This file is the person's side of
   it: the switcher that Alt+Tab opens, and `ps`, `fg`, `kill` and
   background jobs (`<command> &`).

   ── HOW THE SWITCHER DRAWS WITHOUT HARMING THE APP UNDER IT ─────────

   It runs inside keyboard_getchar() of whatever task is in front, so
   anything it drew would land in that app's console and stay there.
   Instead it copies the console into a scratch buffer, makes the copy
   the current console (with no viewport, so the panel is centred on
   the whole screen), draws on the copy, and loads the real console back
   when it is done. vga_ctx_load() repaints from the real buffer, so the
   app reappears exactly as it was -- it never knew.

   ── ALT+TAB, THE WAY PEOPLE EXPECT IT ───────────────────────────────

   Tab with Alt held opens the list on the app used before this one.
   More Tabs move down, Shift+Tab moves up, and letting go of Alt
   switches. A quick Alt+Tab therefore flips between the two most
   recent apps, as it does everywhere else. Enter also switches and Esc
   cancels, for a keyboard whose Alt release is not seen.

   The list is in most-recently-used order: task_front_seq() is stamped
   every time a task comes to the front.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/task.h"
#include "../include/vga.h"
#include "../include/keys.h"
#include "../include/theme.h"
#include "../include/string.h"
#include "../include/appguard.h"
#include "../include/klog.h"

extern int   keyboard_getchar_raw(void);
extern int   keyboard_getchar(void);
extern int   keyboard_waitchar(void);
extern int   kbd_alt(void);
extern void  sleep_ms(uint32_t);
extern void *kmalloc(size_t);
extern void  kfree(void *);
extern void  notif_push(const char *);
extern void  ui_sel_colors(vga_color_t *fg, vga_color_t *bg);
extern void  ui_kit_panel(int x, int y, int w, int h, const char *title);
extern void  speaker_off(void);
extern int   desktop_switch_to(int slot);
extern void  shell_exec_line(const char *line);

static uint16_t *g_ov;          /* the switcher's scratch console    */

/* Live tasks, most recently in front first. */
static int mru_list(int *out)
{
    int n = 0;
    for(int s = 0; s < TASK_MAX; s++){
        int st = task_state(s);
        if(st == TS_READY || st == TS_WAIT) out[n++] = s;
    }
    for(int i = 1; i < n; i++){               /* insertion sort, n <= 8 */
        int v = out[i], j = i - 1;
        while(j >= 0 && task_front_seq(out[j]) < task_front_seq(v)){
            out[j + 1] = out[j]; j--;
        }
        out[j + 1] = v;
    }
    return n;
}

static void put_u(char *b, int *n, uint32_t v)
{
    char d[12]; int k = 0;
    if(v == 0) d[k++] = '0';
    while(v){ d[k++] = (char)('0' + v % 10); v /= 10; }
    while(k) b[(*n)++] = d[--k];
}

static void draw_list(const int *list, int n, int sel, const char *note)
{
    theme_t *th = ui_theme();
    vga_color_t sf, sb; ui_sel_colors(&sf, &sb);
    int W = vga_cols(), H = vga_rows();
    int w = 52, h = n + 6;
    int x = (W - w) / 2, y = (H - h) / 2;
    if(x < 0) x = 0;
    if(y < 0) y = 0;

    ui_kit_panel(x, y, w, h, " Open apps");
    for(int i = 0; i < n; i++){
        int s = list[i];
        char row[64]; int k = 0;
        row[k++] = ' '; row[k++] = (i == sel) ? '>' : ' '; row[k++] = ' ';
        const char *nm = task_name(s);
        int m = 0;
        while(nm[m] && m < 22) row[k++] = nm[m++];
        while(m++ < 24) row[k++] = ' ';
        const char *st = task_state_name(s);
        m = 0;
        while(st[m] && m < 10) row[k++] = st[m++];
        while(m++ < 11) row[k++] = ' ';
        if(s != 0){ row[k++] = '#'; put_u(row, &k, task_id(s)); }
        while(k < w - 2) row[k++] = ' ';
        row[k] = 0;
        vga_color_t fg = (i == sel) ? sf : th->win_fg;
        vga_color_t bg = (i == sel) ? sb : th->win_bg;
        vga_puts_at(row, x + 1, y + 2 + i, fg, bg);
    }
    for(int i = 1; i < w - 1; i++)
        vga_putchar_at(' ', x + i, y + h - 2, th->win_fg, th->win_bg);
    if(note && note[0])
        vga_puts_at(note, x + 2, y + h - 2, VGA_YELLOW, th->win_bg);
    vga_fill_row(y + h - 1, ' ', th->win_fg, th->win_bg);
    for(int i = 0; i < w; i++)
        vga_putchar_at(' ', x + i, y + h - 1, VGA_WHITE, VGA_DARK_GREY);
    vga_puts_at(" Tab next  Enter switch  X close  Esc cancel",
                x, y + h - 1, VGA_WHITE, VGA_DARK_GREY);
    vga_flush();
}

/* The overlay: a copy of the current console with no viewport. */
static int overlay_begin(vga_ctx_t *saved)
{
    vga_ctx_save(saved);
    if(!g_ov) g_ov = (uint16_t *)kmalloc(VGA_CELLS_MAX * sizeof(uint16_t));
    if(!g_ov) return 0;
    for(int i = 0; i < VGA_CELLS_MAX; i++) g_ov[i] = saved->cells[i];
    vga_ctx_t o = *saved;
    o.cells = g_ov;
    o.vp_x = o.vp_y = o.vp_w = o.vp_h = 0;
    o.defer = 0;
    vga_ctx_load(&o);
    { extern void vga_wm_solo(int on); vga_wm_solo(1); }   /* v0.9.8: over every window */
    return 1;
}

static void overlay_end(vga_ctx_t *saved)
{
    { extern void vga_wm_solo(int on); vga_wm_solo(0); }   /* v0.9.8 */
    vga_forget_cells(g_ov);
    vga_ctx_load(saved);
    /* The panel must come off the screen NOW, even if the app under it
       was mid-frame when it read the key -- vga_ctx_load() leaves a
       mid-frame console for the frame's end to paint, and that would
       leave the panel up until then. The app is stopped at a keyboard
       read, so what its buffer holds is what it meant to show. */
    vga_flush();
}

static void switcher(int back)
{
    /* v0.9.6.6: a full-screen 80x25 app has the console at its own grid;
       it goes back to the person's before the panel is drawn */
    { extern void chrome_leave_front(void); chrome_leave_front(); }
    int list[TASK_MAX];
    int n = mru_list(list);
    vga_ctx_t saved;

    /* A solo app (the installer) holds the lock. Say so briefly rather
       than offering a list that cannot be acted on. */
    if(task_locked() || task_front_pinned()){
        if(!overlay_begin(&saved)) return;
        int one[1] = { task_current() };
        draw_list(one, 1, 0, task_front_pinned()
                  ? "The screen is locked: sign in first."
                  : "Switching is off while this is open.");
        sleep_ms(1200);
        overlay_end(&saved);
        return;
    }

    if(!overlay_begin(&saved)){
        /* No memory for the panel: still do the quick switch. */
        if(n > 1) task_set_front(list[back ? n - 1 : 1]);
        return;
    }

    int sel = (n > 1) ? (back ? n - 1 : 1) : 0;
    int commit = 0;
    const char *note = (n > 1) ? "" : "Nothing else is open.";
    task_lock();                    /* nothing switches under the panel */
    for(;;){
        draw_list(list, n, sel, note);
        int c = keyboard_getchar_raw();
        if(c == -1){
            /* Letting go of Alt is the commit. */
            if(!kbd_alt()){ commit = 1; break; }
            sleep_ms(5);
            continue;
        }
        if(c == KEY_TASKSW || c == '\t' || c == KEY_DOWN) sel = (sel + 1) % n;
        else if(c == KEY_TASKSW_BACK || c == KEY_UP)      sel = (sel + n - 1) % n;
        else if(c == '\n' || c == '\r'){ commit = 1; break; }
        else if(c == 27){ commit = 0; break; }
        else if(c == 'x' || c == 'X' || c == KEY_DEL){
            int s = list[sel];
            if(s == 0)                 note = "The desktop cannot be closed.";
            else if(s == task_current()) note = "Close this one from inside it.";
            else {
                task_unlock();
                task_kill(s);
                task_lock();
                n = mru_list(list);
                if(sel >= n) sel = n - 1;
                note = "Closed.";
            }
        }
    }
    task_unlock();
    overlay_end(&saved);
    if(commit && n > 0 && list[sel] != task_front()) task_set_front(list[sel]);
}

void taskswitch_init(void){ task_set_switcher(switcher); }

/* ════════════════════════════════════════════════════════════════════
   THE SHELL'S SIDE: ps, fg, kill, and `<command> &`
   ════════════════════════════════════════════════════════════════════ */
static void out_u(uint32_t v){ char b[12]; int n = 0; put_u(b, &n, v); b[n] = 0; vga_puts(b); }
static void out_pad(const char *s, int w){
    int n = 0;
    while(s[n] && n < w){ vga_putchar(s[n]); n++; }
    while(n++ < w) vga_putchar(' ');
}

void cmd_ps(void)
{
    if(!task_ready()){
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_puts("  Tasks are off (safe mode, or `notasks` on the boot line):\n"
                 "  one app at a time, as before 0.9.6.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    int list[TASK_MAX];
    int n = mru_list(list);
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  ID  NAME                    STATE      UP      STACK     RUNS\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    for(int i = 0; i < n; i++){
        int s = list[i];
        vga_puts("  ");
        char id[8]; int k = 0;
        if(s == 0) id[k++] = '-'; else put_u(id, &k, task_id(s));
        id[k] = 0;
        out_pad(id, 4);
        out_pad(task_name(s), 24);
        if(s == task_front()) vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        out_pad(task_state_name(s), 11);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        {
            uint32_t up = task_age_ticks(s) / 100u;
            char t[12]; int m = 0;
            put_u(t, &m, up / 60u); t[m++] = ':';
            t[m++] = (char)('0' + (up % 60u) / 10u); t[m++] = (char)('0' + up % 10u);
            t[m] = 0;
            out_pad(t, 8);
        }
        if(task_stack_size(s)){
            char t[16]; int m = 0;
            put_u(t, &m, (task_stack_used(s) + 1023u) / 1024u); t[m++] = '/';
            put_u(t, &m, task_stack_size(s) / 1024u); t[m++] = 'K'; t[m] = 0;
            out_pad(t, 10);
        } else out_pad("boot", 10);
        out_u(task_switches(s));
        vga_putchar('\n');
    }
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("\n  Alt+Tab switches. `fg <id>` brings one forward, `kill <id>`\n"
             "  closes it, and `<command> &` runs a command as a job behind.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* An id from `ps`, or the start of a name. */
static int find_task(const char *w)
{
    if(!w || !w[0]) return -1;
    if(w[0] >= '0' && w[0] <= '9'){
        uint32_t v = 0;
        for(const char *p = w; *p >= '0' && *p <= '9'; p++) v = v * 10u + (uint32_t)(*p - '0');
        return v ? task_find_id(v) : -1;
    }
    for(int s = 0; s < TASK_MAX; s++){
        int st = task_state(s);
        if(st != TS_READY && st != TS_WAIT) continue;
        const char *nm = task_name(s);
        int k = 0;
        while(w[k] && nm[k]){
            char a = w[k], b = nm[k];
            if(a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if(b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if(a != b) break;
            k++;
        }
        if(!w[k]) return s;
    }
    return -1;
}

void cmd_fg(const char *arg)
{
    if(!task_ready()){ vga_puts("Tasks are off -- see `ps`.\n"); return; }
    int s = find_task(arg);
    if(!arg || !arg[0]){ vga_puts("Usage: fg <id or name>   (ids are in `ps`)\n"); return; }
    if(s < 0){ vga_puts("No open task by that id or name. `ps` lists them.\n"); return; }
    if(s == task_current()){ vga_puts("That is this one.\n"); return; }
    desktop_switch_to(s);
}

void cmd_kill(const char *arg)
{
    if(!task_ready()){ vga_puts("Tasks are off -- see `ps`.\n"); return; }
    if(!arg || !arg[0]){ vga_puts("Usage: kill <id or name>   (ids are in `ps`)\n"); return; }
    int s = find_task(arg);
    if(s < 0){ vga_puts("No open task by that id or name. `ps` lists them.\n"); return; }
    if(s == 0){ vga_puts("The desktop cannot be closed.\n"); return; }
    if(s == task_current()){ vga_puts("That is this one -- use `exit`.\n"); return; }
    char nm[TASK_NAME];
    strncpy(nm, task_name(s), TASK_NAME - 1); nm[TASK_NAME - 1] = 0;
    if(task_kill(s) == 0){
        vga_puts("Closed "); vga_puts(nm); vga_puts(".\n");
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("Anything it had allocated stays allocated until reboot.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
    } else vga_puts("It could not be closed.\n");
}

/* ── JOBS ────────────────────────────────────────────────────────────
   `<command> &` runs the command in a task of its own that keeps
   running behind the front. It has a console like any app: Alt+Tab or
   `fg <id>` shows what it printed. When it finishes it says so in the
   alerts, stops running, and waits for someone to look -- a key then
   closes it.

   Cooperative means a job runs between the points where it waits: a
   keyboard read or kpoll_sleep_ms(). A command that computes without
   waiting holds the machine until it is done, job or not. */
typedef struct { uint32_t id; char line[120]; } job_t;

static void job_entry(void *arg)
{
    job_t *j = (job_t *)arg;
    vga_clear();
    vga_set_color(VGA_BLACK, VGA_LIGHT_GREY);
    vga_fill_row(0, ' ', VGA_BLACK, VGA_LIGHT_GREY);
    vga_set_cursor(1, 0);
    vga_puts("Job "); out_u(j->id); vga_puts(": "); vga_puts(j->line);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_set_cursor(0, 2);

    if(appguard_enter("a job")){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("\nThe job stopped: ");
        vga_puts(appguard_exception_name());
        vga_puts(".\n");
    } else {
        shell_exec_line(j->line);
        appguard_leave();
    }

    {
        char m[48]; int n = 0;
        const char *a = "Job ";
        while(*a) m[n++] = *a++;
        put_u(m, &n, j->id);
        a = " finished: ";
        while(*a) m[n++] = *a++;
        for(int i = 0; j->line[i] && n < 47; i++) m[n++] = j->line[i];
        m[n] = 0;
        notif_push(m);
    }
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("\n-- finished. Any key closes this job. --");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    task_set_policy(TP_PAUSE);
    keyboard_waitchar();
    kfree(j);
}

int shell_start_job(const char *line)
{
    if(!task_ready()){ vga_puts("Tasks are off -- a job cannot run behind.\n"); return -1; }
    job_t *j = (job_t *)kmalloc(sizeof *j);
    if(!j){ vga_puts("No memory for a job.\n"); return -1; }
    strncpy(j->line, line, sizeof j->line - 1); j->line[sizeof j->line - 1] = 0;
    char nm[TASK_NAME]; int n = 0;
    const char *a = "job: ";
    while(*a) nm[n++] = *a++;
    for(int i = 0; j->line[i] && n < TASK_NAME - 1; i++) nm[n++] = j->line[i];
    nm[n] = 0;
    int s = task_spawn(nm, job_entry, j, TP_RUN);
    if(s < 0){
        kfree(j);
        vga_puts("No room for another task -- `ps` shows what is open.\n");
        return -1;
    }
    j->id = task_id(s);
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts("[job "); out_u(j->id); vga_puts("] started: "); vga_puts(j->line);
    vga_putchar('\n');
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("It runs behind. Alt+Tab or `fg "); out_u(j->id);
    vga_puts("` shows it. The desktop's alerts say when it ends.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    return s;
}
