/* ════════════════════════════════════════════════════════════════════
   test_task.c -- cooperative tasks, switched for real.  v0.9.6.

   ── WHAT MAKES THIS SUITE ABLE TO SAY ANYTHING ──────────────────────

   kernel/task.c's switch is eight instructions of 32-bit assembly with
   nothing privileged in them: push four registers and EFLAGS, swap
   esp, pop them back. So this suite is built -m32 and runs the REAL
   switch, on REAL stacks allocated by the real task_spawn(), in a user
   process. Every task below is a C function that genuinely runs on its
   own stack -- the suite checks the address of a local against the
   stack the task was given -- and genuinely comes back to main()
   through task_switch_stack(). (pushfl/popfl from ring 3 leave the
   interrupt flag alone rather than faulting.)

   The alternative was a fake switch that records "would have switched
   to slot 2" and returns. That tests the choosing and nothing about
   the switch, and the switch is where a cooperative scheduler is
   wrong: a register not saved, a stack laid out one word off, a task
   resumed on the wrong console. Those are what the checks below can
   see.

   ── WHAT IS STUBBED, AND HOW ────────────────────────────────────────

   The console and the crash guard are FAKES THAT REMEMBER: vga_ctx_*
   keeps a "live console" -- which buffer, visible or not -- and the
   suite asserts that when a task runs, the live console is THAT
   task's. The same for the guard's name. kpoll_depth() and
   kpoll_in_poll() are plain variables the suite sets, so the refusals
   are reached by the condition they name. kmalloc() can be told to
   fail. The keyboard and mouse hooks are captured, so the gate a real
   getchar() calls is called here.

   ── AND THE REFUSALS ARE ASSERTED BY CODE ───────────────────────────

   This tree's rule since 0.8.7.7: a refusal is a named code, never
   "something non-zero". "The switch did not happen" has four causes
   and four remedies here.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-56s got %ld\n", what, got); }
}

/* ── the kernel's include path, and its types ───────────────────────── */
#include "../include/types.h"
#include "../include/vga.h"
#include "../include/appguard.h"
#include "../include/klog.h"
#include "../include/task.h"

/* ── the heap ───────────────────────────────────────────────────────── */
static int g_alloc_fail_after = -1;      /* -1: never fail             */
static int g_allocs, g_frees, g_live_allocs;
void *kmalloc(size_t n){
    if(g_alloc_fail_after == 0) return 0;
    if(g_alloc_fail_after > 0) g_alloc_fail_after--;
    g_allocs++; g_live_allocs++;
    return malloc(n);
}
void kfree(void *p){ if(p){ g_frees++; g_live_allocs--; free(p); } }

/* ── the clock ──────────────────────────────────────────────────────── */
static uint32_t g_ticks = 1000;
uint32_t get_ticks(void){ return g_ticks; }

/* ── klog, counted by level ─────────────────────────────────────────── */
static int g_log_err, g_log_warn;
static char g_last_log[160];
void klog(int level, const char *fmt, ...){
    if(level == KLOG_ERR) g_log_err++;
    if(level == KLOG_WARN) g_log_warn++;
    strncpy(g_last_log, fmt, sizeof g_last_log - 1);
}

/* ── kpoll: the two conditions a switch is refused for ──────────────── */
static int g_depth, g_in_poll;
static int (*g_kp_yield)(void);
int  kpoll_depth(void){ return g_depth; }
int  kpoll_in_poll(void){ return g_in_poll; }
void kpoll_set_yield(int (*fn)(void)){ g_kp_yield = fn; }

/* ── the input hooks, captured ──────────────────────────────────────── */
static int  (*g_gate)(void);
static void (*g_sw)(int);
static int  (*g_mouse_front)(void);
static int  (*g_ui_yield)(void);
static int  g_clamps;
void keyboard_set_task_hooks(int (*gate)(void), void (*sw)(int)){ g_gate = gate; g_sw = sw; }
void mouse_set_front_check(int (*fn)(void)){ g_mouse_front = fn; }
void ui_set_yield(int (*fn)(void)){ g_ui_yield = fn; }
void mouse_clamp_to_grid(void){ g_clamps++; }

/* ── the console: a fake that remembers which one is live ───────────── */
static vga_ctx_t g_live;                 /* what vga.c's globals hold  */
static int       g_repaints;             /* a visible console loaded   */
static const uint16_t *g_forgot;
static uint16_t  g_boot_cells[4];
void vga_ctx_init(vga_ctx_t *c, uint16_t *cells){
    memset(c, 0, sizeof *c);
    c->cells = cells; c->fg = VGA_LIGHT_GREY; c->bg = VGA_BLACK;
    c->clip_maxx = c->clip_maxy = -1;
    cells[0] = 0x0720;
}
void vga_ctx_save(vga_ctx_t *c){ *c = g_live; }
void vga_ctx_load(vga_ctx_t *c){ g_live = *c; if(c->visible) g_repaints++; }
void vga_set_visible(int on){ g_live.visible = on; if(on) g_repaints++; }
void vga_forget_cells(const uint16_t *cells){ g_forgot = cells; }

/* ── the guard: the same, by name ───────────────────────────────────── */
static appguard_ctx_t g_guard_live;
void appguard_ctx_save(appguard_ctx_t *c){ *c = g_guard_live; }
void appguard_ctx_load(const appguard_ctx_t *c){ g_guard_live = *c; }
void appguard_ctx_clear(appguard_ctx_t *c){ memset(c, 0, sizeof *c); c->name = "(none)"; }

/* THE REAL THING. Included rather than linked so the suite can read
   the table -- which stack a task was given, what state it is in --
   the way tests/test_kpoll.c reads kpoll's. */
#include "wm_stubs.inc"          /* v0.9.8: the window manager, off */
#include "../kernel/task.c"

/* ════════════════════════════════════════════════════════════════════
   the tasks the suite runs
   ════════════════════════════════════════════════════════════════════ */
static int  g_trace[64], g_ntrace;
static void trace(int v){ if(g_ntrace < 64) g_trace[g_ntrace++] = v; }

/* Records, on its first run, where its own local variable lives -- the
   proof that it is on the stack it was given. */
static uintptr_t g_local_at[TASK_MAX];
static uint16_t *g_seen_cells[TASK_MAX];
static int g_seen_visible[TASK_MAX];

static void t_once(void *arg){
    int me = (int)(intptr_t)arg;
    volatile int here = me;
    g_local_at[task_current()] = (uintptr_t)&here;
    g_seen_cells[task_current()] = g_live.cells;
    g_seen_visible[task_current()] = g_live.visible;
    trace(100 + me);
}

/* Yields `n` times, tracing each turn. Holds values in locals across
   the yields: if a callee-saved register were not restored by the
   switch, these would come back wrong. */
static void t_loop(void *arg){
    int me = (int)(intptr_t)arg;
    volatile int a = me * 7, b = me * 11, c = me * 13;
    int sum = 0;
    for(int i = 0; i < 3; i++){
        trace(me * 10 + i);
        sum += a + b + c;
        task_yield();
    }
    if(sum != 3 * (me * 31)) trace(-1);       /* a register was lost */
}

/* Names its guard, then yields, then checks the live guard is still its. */
static void t_guard(void *arg){
    const char *nm = (const char *)arg;
    g_guard_live.name = nm;
    task_yield();
    if(g_guard_live.name != nm) trace(-2);
    trace(nm[0]);
}

/* A task that waits behind the front, as getchar's gate does. */
static int g_gate_got[TASK_MAX];
static void t_gate(void *arg){
    (void)arg;
    g_gate_got[task_current()] = g_gate();
    trace(200 + task_current());
}

/* Uses roughly `n` KB of its stack. */
static int g_deep_sink;
static void t_deep(void *arg){
    int kb = (int)(intptr_t)arg;
    volatile char buf[20 * 1024];
    for(int i = 0; i < kb * 1024 && i < (int)sizeof buf; i += 64) buf[i] = (char)i;
    g_deep_sink = buf[0];
    task_yield();
}

/* Overwrites its own canary -- what a stack overflow does -- and yields. */
static int g_smashed_resumed;
static void t_smash(void *arg){
    (void)arg;
    g_t[task_current()].stack[0] = 0;
    task_yield();
    g_smashed_resumed = 1;                /* must never run */
}

static int g_cleanups;
static void cleanup_hook(void){ g_cleanups++; }
static void t_park(void *arg){
    (void)arg;
    task_set_cleanup(cleanup_hook);
    for(;;) task_wait_front(), task_yield();
}

/* Opens a child and waits for the front, as the desktop does. */
static int g_child_slot, g_wait_ret;
static void t_opener(void *arg){
    (void)arg;
    g_child_slot = task_spawn("child", t_once, (void *)(intptr_t)9, TP_PAUSE);
    task_set_front(g_child_slot);
    g_wait_ret = task_wait_front();
    trace(300);
}

static void reset_world(void){
    for(int s = 0; s < TASK_MAX; s++){
        if(g_t[s].stack) free(g_t[s].stack);
        if(g_t[s].cells) free(g_t[s].cells);
        g_t[s].stack = 0; g_t[s].cells = 0;
    }
    memset(&g_live, 0, sizeof g_live);
    g_live.cells = g_boot_cells; g_live.visible = 1;
    memset(&g_guard_live, 0, sizeof g_guard_live);
    g_guard_live.name = "desktop";
    g_depth = g_in_poll = 0;
    g_alloc_fail_after = -1;
    g_allocs = g_frees = g_live_allocs = 0;
    g_log_err = g_log_warn = 0;
    g_ntrace = 0;
    g_repaints = 0; g_clamps = 0; g_cleanups = 0;
    memset(g_local_at, 0, sizeof g_local_at);
    memset(g_seen_cells, 0, sizeof g_seen_cells);
    memset(g_gate_got, 0, sizeof g_gate_got);
    task_init();
}

static int in_stack(int slot, uintptr_t p){
    uintptr_t b = (uintptr_t)g_t[slot].stack;
    return g_t[slot].stack && p >= b && p < b + TASK_STACK;
}

int main(void)
{
    int s, s1, s2, r;
    printf("task: cooperative tasks, switched for real on real stacks\n");

    head("before task_init(), nothing switches");
    ck("task_yield() refuses with TY_ENOINIT", task_yield() == TY_ENOINIT, task_yield());
    ck("  ... and counts it", task_refusals(TY_ENOINIT) == 2, task_refusals(TY_ENOINIT));
    ck("task_is_front() is 1 -- a machine without tasks is always in front",
       task_is_front() == 1, task_is_front());
    ck("task_spawn() refuses", task_spawn("x", t_once, 0, TP_PAUSE) == -1, 0);

    /* ── init ─────────────────────────────────────────────────────── */
    reset_world();
    head("task_init(): slot 0 is the boot context");
    ck("one task, and it is slot 0", task_count() == 1 && task_current() == 0, task_count());
    ck("  ... in front", task_front() == 0 && task_is_front(), task_front());
    ck("  ... called Desktop", !strcmp(task_name(0), "Desktop"), 0);
    ck("  ... with no stack of its own (the boot stack)", task_stack_size(0) == 0,
       (long)task_stack_size(0));
    ck("the keyboard gate and the switcher are registered", g_gate && g_sw, 0);
    ck("the mouse asks the task layer whose clicks they are", g_mouse_front != 0, 0);
    ck("kpoll_sleep_ms() and ui_idle() yield through task_yield()",
       g_kp_yield == task_yield && g_ui_yield == task_yield, 0);
    ck("alone, task_yield() switches nothing (TY_ALONE)", task_yield() == TY_ALONE, 0);

    /* ── spawn ────────────────────────────────────────────────────── */
    head("task_spawn() makes a task and does not run it");
    s = task_spawn("Calculator", t_once, (void *)(intptr_t)1, TP_PAUSE);
    ck("slot 1", s == 1, s);
    ck("  ... not run yet", g_ntrace == 0, g_ntrace);
    ck("  ... READY, opened by slot 0, id 1",
       task_state(s) == TS_READY && task_opener(s) == 0 && task_id(s) == 1,
       task_id(s));
    ck("  ... a stack and a console were allocated", g_allocs == 2, g_allocs);
    ck("  ... the stack's canary is in place", canary_ok(s), 0);
    ck("  ... NOT in front: spawning does not steal the screen",
       task_front() == 0, task_front());
    ck("  ... its console starts hidden", g_t[s].vga.visible == 0, g_t[s].vga.visible);
    ck("  ... and its guard starts disarmed", g_t[s].guard.armed == 0, 0);

    head("the first yield runs it, on its own stack");
    r = task_yield();
    ck("task_yield() reports TY_RAN", r == TY_RAN, r);
    ck("the task ran", g_ntrace == 1 && g_trace[0] == 101, g_ntrace);
    ck("ITS LOCAL VARIABLE LIVED ON THE STACK IT WAS GIVEN",
       in_stack(s, g_local_at[s]), (long)g_local_at[s]);
    ck("the console live while it ran was its own, not the desktop's",
       g_seen_cells[s] == g_t[s].cells, 0);
    ck("  ... and hidden, because it is not in front", g_seen_visible[s] == 0, 0);
    ck("and main() is back on the boot console", g_live.cells == g_boot_cells, 0);
    ck("its entry returned, so it ended (DEAD until reaped)",
       g_t[s].state == TS_DEAD, g_t[s].state);
    task_yield();
    ck("the next yield reaped it: slot free, stack and console freed",
       g_t[s].state == TS_FREE && g_frees == 2 && g_live_allocs == 0, g_frees);
    ck("  ... and the console was forgotten by the screen (no ABA repaint)",
       g_forgot != 0, 0);

    /* ── interleaving ─────────────────────────────────────────────── */
    reset_world();
    head("two tasks and the desktop take turns, round robin");
    s1 = task_spawn("A", t_loop, (void *)(intptr_t)1, TP_RUN);
    s2 = task_spawn("B", t_loop, (void *)(intptr_t)2, TP_RUN);
    for(int i = 0; i < 6; i++) task_yield();
    {
        static const int want[] = { 10, 20, 11, 21, 12, 22 };
        int ok = g_ntrace == 6;
        for(int i = 0; ok && i < 6; i++) ok = g_trace[i] == want[i];
        ck("A0 B0 A1 B1 A2 B2, one turn each per round", ok, g_ntrace);
    }
    ck("NO REGISTER WAS LOST across a switch (locals survived three yields)",
       !(g_ntrace > 0 && g_trace[g_ntrace - 1] == -1), 0);
    for(int i = 0; i < 4; i++) task_yield();
    ck("both finished and were reaped", task_count() == 1, task_count());
    ck("  ... every allocation was given back", g_live_allocs == 0, g_live_allocs);

    head("each task runs under its own crash guard");
    reset_world();
    task_spawn("Alpha", t_guard, (void *)"Alpha", TP_RUN);
    task_spawn("Bravo", t_guard, (void *)"Bravo", TP_RUN);
    task_yield(); task_yield(); task_yield(); task_yield();
    ck("the guard live in each task was its own after the other ran",
       g_ntrace == 2 && g_trace[0] == 'A' && g_trace[1] == 'B', g_ntrace);
    ck("  ... and the desktop got its own guard back", !strcmp(g_guard_live.name, "desktop"), 0);

    /* ── the front ────────────────────────────────────────────────── */
    reset_world();
    head("bringing a task to the front");
    s = task_spawn("Notes", t_once, (void *)(intptr_t)5, TP_PAUSE);
    g_repaints = 0;
    ck("task_set_front() takes it", task_set_front(s) == 0, 0);
    ck("  ... the desktop's console is hidden at once", g_live.visible == 0, g_live.visible);
    ck("  ... the task's is marked to show", g_t[s].vga.visible == 1, 0);
    ck("  ... its MRU stamp is the newest", task_front_seq(s) > task_front_seq(0), 0);
    task_yield();
    ck("switched in, the console live is its own AND visible",
       g_seen_cells[s] == g_t[s].cells && g_seen_visible[s] == 1, g_seen_visible[s]);
    ck("  ... which repainted the screen from it", g_repaints >= 1, g_repaints);
    ck("  ... and the pointer was clamped to its grid", g_clamps >= 1, g_clamps);
    ck("it ended in front, so the front went back to its opener",
       task_front() == 0, task_front());
    ck("  ... and the desktop's console is visible again", g_live.visible == 1, 0);
    ck("task_set_front() refuses a dead or empty slot",
       task_set_front(s) == -1 && task_set_front(7) == -1 && task_set_front(-1) == -1, 0);

    head("an opener waits for the front, as the desktop does");
    reset_world();
    s = task_spawn("Shell", t_opener, 0, TP_PAUSE);
    task_set_front(s);
    task_wait_front();
    ck("the desktop got the front back", task_front() == 0 && task_is_front(), task_front());
    ck("the child ran, then the opener resumed from its wait",
       g_ntrace >= 2 && g_trace[0] == 109 && g_trace[1] == 300, g_ntrace);
    ck("  ... task_wait_front() returned 1 there", g_wait_ret == 1, g_wait_ret);
    ck("  ... because the child's front went to ITS opener, the Shell",
       g_child_slot == 2, g_child_slot);

    head("a front whose opener is gone goes to slot 0");
    reset_world();
    s1 = task_spawn("A", t_park, 0, TP_PAUSE);
    task_yield();                                   /* park A          */
    {
        /* B is opened BY A: spawn it as if A had. */
        int b;
        g_cur = s1; b = task_spawn("B", t_park, 0, TP_PAUSE); g_cur = 0;
        task_yield();
        ck("B's opener is A", task_opener(b) == s1, task_opener(b));
        ck("killing A reparents B to A's opener (the desktop)",
           task_kill(s1) == 0 && task_opener(b) == 0, task_opener(b));
        task_set_front(b);
        task_kill(b);
        ck("killing the front sends the front to its opener",
           task_front() == 0, task_front());
    }

    /* ── the gate getchar calls ───────────────────────────────────── */
    reset_world();
    head("the keyboard gate: who may have a key");
    ck("in front, the gate says yes", g_gate() == 1, 0);
    s1 = task_spawn("Paused", t_gate, 0, TP_PAUSE);
    s2 = task_spawn("Running", t_gate, 0, TP_RUN);
    task_yield();                         /* both reach the gate        */
    task_yield();                         /* the running one comes back */
    ck("a RUNNING task behind the front is told no, and carries on",
       g_ntrace == 1 && g_trace[0] == 200 + s2 && g_gate_got[s2] == 0, g_ntrace);
    ck("a PAUSED task behind the front STOPS in the gate",
       task_state(s1) == TS_WAIT, task_state(s1));
    {
        /* Count the WIRE: a parked task that is woken only to find it
           is still behind, and yields straight back, changes nothing a
           trace can see -- it costs a switch and two console copies per
           parked app on every keyboard read. task_switches() sees it. */
        uint32_t before = task_switches(s1);
        int r1 = task_yield(), r2 = task_yield();
        ck("  ... and stays stopped however often the others yield",
           task_state(s1) == TS_WAIT && g_ntrace == 1, g_ntrace);
        ck("  ... it is NOT EVEN SWITCHED IN while it waits",
           task_switches(s1) == before, (long)(task_switches(s1) - before));
        ck("  ... so with only parked tasks left, a yield is TY_ALONE",
           r1 == TY_ALONE && r2 == TY_ALONE, r1);
    }
    task_set_front(s1);
    task_yield();
    ck("brought to the front, it leaves the gate with a yes",
       g_gate_got[s1] == 1 && g_ntrace == 2 && g_trace[1] == 200 + s1, g_gate_got[s1]);
    ck("the mouse's front check follows the front", g_mouse_front() == 1, 0);

    /* ── refusals ─────────────────────────────────────────────────── */
    reset_world();
    head("a switch is refused for exactly the reasons a poll is");
    s = task_spawn("X", t_once, 0, TP_RUN);
    g_depth = 1;
    ck("a driver holds a shared buffer: TY_EBUSY", task_yield() == TY_EBUSY, 0);
    ck("  ... and nothing ran", g_ntrace == 0, g_ntrace);
    g_depth = 0; g_in_poll = 1;
    ck("inside kernel_poll(): TY_EPOLL", task_yield() == TY_EPOLL, 0);
    ck("  ... and nothing ran", g_ntrace == 0, g_ntrace);
    g_in_poll = 0;
    task_lock();
    ck("inside task_lock(): TY_ELOCKED", task_yield() == TY_ELOCKED, 0);
    task_lock();
    task_unlock();
    ck("  ... it nests: one unlock of two is still locked",
       task_yield() == TY_ELOCKED && task_locked() == 1, task_locked());
    task_unlock();
    ck("  ... each refusal has its own counter",
       task_refusals(TY_EBUSY) == 1 && task_refusals(TY_EPOLL) == 1 &&
       task_refusals(TY_ELOCKED) == 2, task_refusals(TY_ELOCKED));
    ck("unlocked, it runs", task_yield() == TY_RAN && g_ntrace == 1, g_ntrace);
    head("a paused task behind the front that cannot switch gives up waiting");
    {
        int p = task_spawn("P", t_park, 0, TP_PAUSE);
        task_yield();                        /* P parks in wait_front  */
        g_cur = p;                           /* stand in P's shoes ... */
        g_depth = 1;
        r = task_wait_front();
        g_depth = 0; g_cur = 0;
        ck("  ... task_wait_front() returns 0 rather than spinning", r == 0, r);
    }

    /* ── kill ─────────────────────────────────────────────────────── */
    reset_world();
    head("task_kill()");
    s = task_spawn("Music", t_park, 0, TP_RUN);
    task_yield();
    ck("refuses slot 0", task_kill(0) == -1, 0);
    g_cur = s; r = task_kill(s); g_cur = 0;
    ck("refuses the caller", r == -1, r);
    ck("refuses an empty slot", task_kill(6) == -1, 0);
    ck("kills a parked task", task_kill(s) == 0, 0);
    ck("  ... its cleanup hook ran (the speaker is off)", g_cleanups == 1, g_cleanups);
    ck("  ... it is gone and its memory returned",
       task_state(s) == TS_FREE && g_live_allocs == 0, g_live_allocs);
    ck("  ... and a second kill refuses", task_kill(s) == -1, 0);

    /* ── capacity ─────────────────────────────────────────────────── */
    reset_world();
    head("slots and memory run out honestly");
    for(int i = 1; i < TASK_MAX; i++) task_spawn("p", t_park, 0, TP_PAUSE);
    ck("seven tasks beside the desktop", task_count() == TASK_MAX, task_count());
    g_log_warn = 0;
    ck("an eighth is refused with -1", task_spawn("one too many", t_once, 0, TP_PAUSE) == -1, 0);
    ck("  ... and the log says why", g_log_warn == 1, g_log_warn);
    task_kill(3);
    ck("one closed, the next spawn takes its slot",
       task_spawn("again", t_once, 0, TP_PAUSE) == 3, 0);
    reset_world();
    g_alloc_fail_after = 1;               /* the stack, then nothing   */
    ck("no memory for the console: refused", task_spawn("m", t_once, 0, TP_PAUSE) == -1, 0);
    ck("  ... and the stack it did get was given back", g_live_allocs == 0, g_live_allocs);

    /* ── the stack ────────────────────────────────────────────────── */
    reset_world();
    head("the stack: how much is used, and an overrun");
    s = task_spawn("Deep", t_deep, (void *)(intptr_t)12, TP_RUN);
    task_yield();
    ck("12 KB touched shows as at least 12 KB used",
       task_stack_used(s) >= 12u * 1024u, (long)task_stack_used(s));
    ck("  ... and well under the stack's size",
       task_stack_used(s) < TASK_STACK / 2, (long)task_stack_used(s));
    task_yield();

    reset_world();
    s = task_spawn("Smash", t_smash, 0, TP_PAUSE);
    task_set_front(s);
    g_log_err = 0;
    task_yield();
    ck("a smashed canary stops the task on the way out", task_state(s) != TS_READY &&
       task_state(s) != TS_WAIT, task_state(s));
    ck("  ... the log says which and why", g_log_err == 1, g_log_err);
    task_yield(); task_yield();
    ck("  ... it is NEVER resumed", g_smashed_resumed == 0, g_smashed_resumed);
    ck("  ... and the front it held went back to its opener", task_front() == 0, task_front());

    /* ── the front pin: the lock and login screens ─────────────────── */
    reset_world();
    head("while the front is pinned, nothing comes forward");
    s1 = task_spawn("Shell", t_park, 0, TP_PAUSE);
    s2 = task_spawn("Music", t_loop, (void *)(intptr_t)4, TP_RUN);
    task_yield();
    task_pin_front();
    ck("task_set_front() refuses -- Alt+Tab cannot leave a lock screen",
       task_set_front(s1) == -1 && task_front() == 0, task_front());
    ck("  ... but a task behind still runs (the song plays on)",
       task_yield() == TY_RAN, 0);
    ck("  ... and the pin nests",
       (task_pin_front(), task_unpin_front(), task_front_pinned() == 1), task_front_pinned());
    task_unpin_front();
    ck("unpinned, it comes forward", task_set_front(s1) == 0 && task_front() == s1, task_front());

    /* ── ids and names ────────────────────────────────────────────── */
    reset_world();
    head("ids, names and states, as `ps` shows them");
    s1 = task_spawn("A name that is far too long for the table", t_park, 0, TP_PAUSE);
    s2 = task_spawn("Two", t_park, 0, TP_RUN);
    ck("a long name is cut to fit, terminated", strlen(task_name(s1)) == TASK_NAME - 1, 0);
    ck("ids count up and are found again",
       task_id(s2) == task_id(s1) + 1 && task_find_id(task_id(s2)) == s2, 0);
    ck("an id never handed out is not found", task_find_id(99) == -1, 0);
    ck("states read in front / running / paused",
       !strcmp(task_state_name(0), "in front") &&
       !strcmp(task_state_name(s2), "running") &&
       !strcmp(task_state_name(s1), "paused"), 0);
    task_yield();                         /* both park in wait_front  */
    ck("a TP_RUN task that is WAITING reads paused, not running",
       !strcmp(task_state_name(s2), "paused") && !task_running_behind(s2),
       task_state(s2));
    ck("task_set_policy() changes the CURRENT task only",
       (task_set_policy(TP_RUN), task_slot_policy(0) == TP_RUN && task_slot_policy(s1) == TP_PAUSE), 0);
    task_set_policy(TP_PAUSE);
    ck("refusal names are distinct words",
       strcmp(task_reason_name(TY_EBUSY), task_reason_name(TY_EPOLL)) &&
       strcmp(task_reason_name(TY_ELOCKED), task_reason_name(TY_ENOINIT)), 0);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
