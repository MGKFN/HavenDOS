/* ════════════════════════════════════════════════════════════════════
   kernel/task.c -- cooperative tasks.  v0.9.6.

   include/task.h has the argument: what a task is, why switching is
   cooperative, where a switch may happen and why it is refused where
   it is. What is here is the table, the switch, and the rules for who
   is in front.

   ── THE SWITCH ──────────────────────────────────────────────────────

   task_switch_stack() pushes the four callee-saved registers and
   EFLAGS on the stack it is leaving, stores esp, loads the other
   task's esp, pops the same five, and returns -- on the other stack,
   into whatever that task was doing when it last called this. Nothing
   else needs saving, because a switch only ever happens inside a C
   call: eax/ecx/edx and the x87 and SSE registers are caller-saved in
   the i386 ABI, so no compiler keeps anything live in them across
   task_yield(). That is the one big thing cooperative switching buys.

   EFLAGS is saved so each task keeps its own interrupt flag. Every
   switch point runs with interrupts on today; saving it costs two
   instructions and means a future caller with them off cannot hand
   that state to an unrelated task.

   There is nothing privileged in it, so tests/test_task.c compiles
   this file -m32 and runs REAL switches on REAL stacks in user space.
   pushfl/popfl in ring 3 leave IF alone rather than faulting.

   ── A NEW TASK'S FIRST SWITCH ───────────────────────────────────────

   task_spawn() lays out the stack as if the task had called
   task_switch_stack() itself, with task_trampoline() as the return
   address -- so the first switch "returns" into the trampoline, which
   calls the entry point and then task_exit(). See stack_build().
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/task.h"
#include "../include/vga.h"
#include "../include/appguard.h"
#include "../include/kpoll.h"
#include "../include/klog.h"
#include "../include/wm.h"

extern void    *kmalloc(size_t size);
extern void     kfree(void *p);
extern uint32_t get_ticks(void);
extern void     mouse_clamp_to_grid(void);
extern void     keyboard_set_task_hooks(int (*gate)(void), void (*sw)(int));
extern void     mouse_set_front_check(int (*fn)(void));
extern void     ui_set_yield(int (*fn)(void));

#define FILL 0x5A5A5A5Au      /* unused stack, for the high-water mark */

typedef struct {
    int         state;
    int         policy;
    int         opener;
    uint32_t    id;
    char        name[TASK_NAME];
    uint32_t    esp;
    uint32_t   *stack;         /* NULL for slot 0: the boot stack       */
    uint16_t   *cells;         /* NULL for slot 0: the boot console     */
    vga_ctx_t   vga;
    appguard_ctx_t guard;
    void      (*entry)(void *);
    void       *arg;
    void      (*cleanup)(void);
    uint32_t    switches;
    uint32_t    born;
    uint32_t    front_seq;
} task_t;

static task_t   g_t[TASK_MAX];
static int      g_init;
static int      g_cur;
static int      g_front;
static int      g_lock;
static int      g_pin;          /* see task_pin_front()            */
static uint32_t g_next_id;
static uint32_t g_seq;
static uint32_t g_refused[TY_REASONS];
static void   (*g_switcher)(int back);

/* ── the switch ─────────────────────────────────────────────────────
   void task_switch_stack(uint32_t *save, uint32_t load); */
__asm__(
".text\n"
".globl task_switch_stack\n"
"task_switch_stack:\n"
"   movl  4(%esp), %eax\n"         /* where to store this esp         */
"   movl  8(%esp), %edx\n"         /* the esp to load                 */
"   pushl %ebp\n"
"   pushl %ebx\n"
"   pushl %esi\n"
"   pushl %edi\n"
"   pushfl\n"
"   movl  %esp, (%eax)\n"
"   movl  %edx, %esp\n"
"   popfl\n"
"   popl  %edi\n"
"   popl  %esi\n"
"   popl  %ebx\n"
"   popl  %ebp\n"
"   ret\n"
);
extern void task_switch_stack(uint32_t *save, uint32_t load);

static int alive(int s)
{
    return s >= 0 && s < TASK_MAX &&
           (g_t[s].state == TS_READY || g_t[s].state == TS_WAIT);
}

static int runnable(int s)
{
    if(g_t[s].state == TS_READY) return 1;
    if(g_t[s].state == TS_WAIT && s == g_front) return 1;
    return 0;
}

/* Free what dead tasks held. Never the current task: it is still
   standing on its stack. */
static void reap(void)
{
    for(int s = 1; s < TASK_MAX; s++){
        if(g_t[s].state != TS_DEAD || s == g_cur) continue;
        if(g_t[s].cells){
            vga_forget_cells(g_t[s].cells);
            kfree(g_t[s].cells);
        }
        if(g_t[s].stack) kfree(g_t[s].stack);
        g_t[s].cells = 0;
        g_t[s].stack = 0;
        g_t[s].state = TS_FREE;
    }
}

static int canary_ok(int s)
{
    if(!g_t[s].stack) return 1;
    for(int i = 0; i < TASK_CANARY_N; i++)
        if(g_t[s].stack[i] != TASK_CANARY) return 0;
    return 1;
}

static int pick_next(void)
{
    for(int k = 1; k <= TASK_MAX; k++){
        int s = (g_cur + k) % TASK_MAX;
        if(runnable(s)) return s;
    }
    return g_cur;
}

static void switch_to(int next)
{
    int prev = g_cur;
    task_t *p = &g_t[prev], *n = &g_t[next];

    vga_ctx_save(&p->vga);
    appguard_ctx_save(&p->guard);

    /* ── THE CANARY ──────────────────────────────────────────────────
       There is no guard page under a task's stack, so an overflow
       writes into whatever the heap put there. It is caught here, on
       the way out, and the task is not run again: it may have been
       running on corrupted locals for some time already, and resuming
       it is how the corruption spreads. What it already overwrote
       below its stack is not repaired -- that is the honest limit,
       and the log line says so. */
    if(!canary_ok(prev)){
        klog(KLOG_ERR, "task: %s overran its %u KB stack and was stopped "
             "-- memory below it may be damaged", p->name,
             TASK_STACK / 1024u);
        p->state = TS_DEAD;
        if(g_front == prev){
            int to = alive(p->opener) ? p->opener : 0;
            p->vga.visible = 0;
            g_front = to;
            g_t[to].vga.visible = 1;
            g_t[to].front_seq = ++g_seq;
        }
    }

    g_cur = next;
    n->switches++;
    vga_ctx_load(&n->vga);
    appguard_ctx_load(&n->guard);
    if(next == g_front) mouse_clamp_to_grid();
    task_switch_stack(&p->esp, n->esp);
    /* Back in `prev`, some time later. Whoever switched to us has
       already loaded our console and guard. */
}

static void task_trampoline(void)
{
    task_t *t = &g_t[g_cur];
    t->entry(t->arg);
    task_exit();
}

/* Lay the new stack out as task_switch_stack() would have left it:
   EFLAGS, edi, esi, ebx, ebp, then the "return address" -- the
   trampoline -- and a dummy slot above it standing in for the
   trampoline's own return address, which it never uses. The dummy
   sits so that the trampoline is entered with esp+4 on a 16-byte
   boundary, as the i386 ABI promises a called function. */
static uint32_t stack_build(uint32_t *base, uint32_t bytes)
{
    uint32_t top = ((uint32_t)(uintptr_t)base + bytes) & ~15u;
    uint32_t *sp = (uint32_t *)(uintptr_t)(top - 4);
    *sp   = 0;                                     /* dummy return   */
    *--sp = (uint32_t)(uintptr_t)task_trampoline;  /* ret            */
    *--sp = 0;                                     /* ebp            */
    *--sp = 0;                                     /* ebx            */
    *--sp = 0;                                     /* esi            */
    *--sp = 0;                                     /* edi            */
    *--sp = 0x202;                                 /* EFLAGS, IF on  */
    return (uint32_t)(uintptr_t)sp;
}

/* ── the gates the input drivers call ───────────────────────────────
   keyboard_getchar() asks before it reads: 1 = this task may have the
   key, 0 = it is behind the front and gets "no key". Calling it is
   also the switch point -- see include/task.h. */
static int key_gate(void)
{
    if(!g_init) return 1;
    if(g_cur == g_front){
        task_yield();               /* give the others a turn          */
        return g_cur == g_front;
    }
    if(g_t[g_cur].policy == TP_PAUSE) task_wait_front();
    else                              task_yield();
    return g_cur == g_front;
}

static int front_check(void){ return !g_init || g_cur == g_front; }

void task_init(void)
{
    for(int s = 0; s < TASK_MAX; s++){
        g_t[s].state = TS_FREE;
        g_t[s].stack = 0;
        g_t[s].cells = 0;
    }
    for(int r = 0; r < TY_REASONS; r++) g_refused[r] = 0;

    /* Slot 0 is the context we are running in now. Its console is
       the boot console, and vga_ctx_save() captures it on the first
       switch out; until then the live state IS its state. */
    task_t *z = &g_t[0];
    z->state = TS_READY;
    z->policy = TP_PAUSE;
    z->opener = -1;
    z->id = 0;
    {
        const char *n = "Desktop";
        int i = 0;
        while(n[i] && i < TASK_NAME - 1){ z->name[i] = n[i]; i++; }
        z->name[i] = 0;
    }
    z->switches = 1;
    z->born = get_ticks();
    z->front_seq = 1;
    z->cleanup = 0;
    vga_ctx_save(&z->vga);
    appguard_ctx_save(&z->guard);

    g_cur = 0;
    g_front = 0;
    g_lock = 0;
    g_pin = 0;
    g_next_id = 1;
    g_seq = 1;
    g_init = 1;

    keyboard_set_task_hooks(key_gate, task_switcher_run);
    mouse_set_front_check(front_check);
    kpoll_set_yield(task_yield);
    ui_set_yield(task_yield);
}

int task_ready(void){ return g_init; }

int task_spawn(const char *name, void (*entry)(void *), void *arg, int policy)
{
    if(!g_init || !entry) return -1;
    reap();

    int s;
    for(s = 1; s < TASK_MAX; s++) if(g_t[s].state == TS_FREE) break;
    if(s >= TASK_MAX){
        klog(KLOG_WARN, "task: no free slot for %s (%d tasks open)",
             name ? name : "?", TASK_MAX);
        return -1;
    }

    uint32_t  *stk = (uint32_t *)kmalloc(TASK_STACK);
    uint16_t  *cel = (uint16_t *)kmalloc(VGA_CELLS_MAX * sizeof(uint16_t));
    if(!stk || !cel){
        if(stk) kfree(stk);
        if(cel) kfree(cel);
        klog(KLOG_WARN, "task: no memory for %s (%u KB stack + console)",
             name ? name : "?",
             (TASK_STACK + VGA_CELLS_MAX * 2u) / 1024u);
        return -1;
    }
    for(uint32_t i = 0; i < TASK_STACK / 4u; i++) stk[i] = FILL;
    for(int i = 0; i < TASK_CANARY_N; i++) stk[i] = TASK_CANARY;

    task_t *t = &g_t[s];
    t->stack = stk;
    t->cells = cel;
    t->state = TS_READY;
    t->policy = (policy == TP_RUN) ? TP_RUN : TP_PAUSE;
    t->opener = g_cur;
    t->id = g_next_id++;
    {
        int i = 0;
        if(name) while(name[i] && i < TASK_NAME - 1){ t->name[i] = name[i]; i++; }
        t->name[i] = 0;
    }
    t->entry = entry;
    t->arg = arg;
    t->cleanup = 0;
    t->switches = 0;
    t->born = get_ticks();
    t->front_seq = 0;
    vga_ctx_init(&t->vga, cel);
    appguard_ctx_clear(&t->guard);
    t->esp = stack_build(stk, TASK_STACK);
    return s;
}

static int refuse(int why){ g_refused[why]++; return why; }

int task_yield(void)
{
    if(!g_init)            return refuse(TY_ENOINIT);
    if(g_lock)             return refuse(TY_ELOCKED);
    if(kpoll_depth() > 0)  return refuse(TY_EBUSY);
    if(kpoll_in_poll())    return refuse(TY_EPOLL);

    reap();
    int next = pick_next();
    if(next == g_cur) return TY_ALONE;
    switch_to(next);
    return TY_RAN;
}

int task_wait_front(void)
{
    if(!g_init) return 1;
    int me = g_cur;
    if(g_front == me) return 1;

    g_t[me].state = TS_WAIT;
    while(g_front != me){
        /* Anything but "another task ran" means nobody else can run
           right now -- a refusal, or (which would be a bug) a front
           that is not runnable. Waiting on would spin for ever, so
           give up and let the caller carry on with "no key". */
        if(task_yield() != TY_RAN) break;
    }
    g_t[me].state = TS_READY;
    return g_front == me;
}

int task_set_front(int s)
{
    if(!g_init || !alive(s)) return -1;
    if(g_pin && s != g_front) return -1;
    int old = g_front;
    if(old == s) return 0;

    /* Hide the old front. If it is the task running this, its console
       stops painting now and it carries on drawing into its own
       buffer until it next yields. */
    if(old == g_cur) vga_set_visible(0);
    else             g_t[old].vga.visible = 0;

    g_front = s;
    g_t[s].front_seq = ++g_seq;
    if(s == g_cur){ vga_set_visible(1); mouse_clamp_to_grid(); }
    else            g_t[s].vga.visible = 1;   /* painted when switched in */
    vga_wm_front(s);                          /* v0.9.8: its window comes up */
    return 0;
}

/* Hand the front on from a task that is going away. */
static void front_from(int s)
{
    if(g_front != s) return;
    int to = alive(g_t[s].opener) ? g_t[s].opener : 0;
    if(to == s) to = 0;
    if(s == g_cur) vga_set_visible(0);
    else           g_t[s].vga.visible = 0;
    g_front = to;
    g_t[to].front_seq = ++g_seq;
    if(to == g_cur){ vga_set_visible(1); mouse_clamp_to_grid(); }
    else             g_t[to].vga.visible = 1;
    vga_wm_front(to);                         /* v0.9.8 */
}

static void reparent(int s)
{
    for(int k = 0; k < TASK_MAX; k++)
        if(k != s && alive(k) && g_t[k].opener == s)
            g_t[k].opener = g_t[s].opener;
}

void task_exit(void)
{
    int me = g_cur;
    if(me == 0){
        /* Slot 0 is the boot context and has nowhere to go. Nothing
           calls this from there; if something ever does, say so and
           stay put rather than switching off a stack that the next
           task would then free. */
        klog(KLOG_ERR, "task: slot 0 asked to exit -- refused");
        for(;;) task_yield();
    }
    if(g_lock){
        klog(KLOG_WARN, "task: %s ended inside task_lock() -- released",
             g_t[me].name);
        g_lock = 0;
    }
    reparent(me);
    front_from(me);
    g_t[me].state = TS_DEAD;

    int next = pick_next();
    if(next == me) next = 0;       /* cannot happen: slot 0 is alive */
    switch_to(next);
    for(;;){}                      /* never resumed: it is DEAD     */
}

int task_kill(int s)
{
    if(!g_init || s <= 0 || s >= TASK_MAX || s == g_cur || !alive(s))
        return -1;
    /* A task that is not running is parked at a switch point, and a
       switch is refused while any driver holds a shared buffer -- so
       it holds none, and nothing at module level is half done. What it
       may still hold is its own: heap it allocated, a socket it opened.
       The cleanup hook is for that; most apps have none, and what they
       allocated stays allocated. */
    if(g_t[s].cleanup) g_t[s].cleanup();
    reparent(s);
    front_from(s);
    g_t[s].state = TS_DEAD;
    reap();
    return 0;
}

/* ── THE FRONT PIN ───────────────────────────────────────────────────
   The lock screen and the login screen read the keyboard, so Alt+Tab
   arrives there like anywhere else -- and switching out of a lock
   screen to the shell behind it is not a convenience, it is the lock
   not working. While the front is pinned, nothing can be brought
   forward: task_set_front() refuses and the switcher says so. Tasks
   behind keep running (a song plays on under the lock screen), which
   is the difference from task_lock(). A task ending still hands the
   front on -- front_from() does not ask. */
void task_pin_front(void)   { g_pin++; }
void task_unpin_front(void) { if(g_pin > 0) g_pin--; }
int  task_front_pinned(void){ return g_pin; }

void task_lock(void)   { g_lock++; }
void task_unlock(void) { if(g_lock > 0) g_lock--; }
int  task_locked(void) { return g_lock; }

void task_set_policy(int p){ if(g_init) g_t[g_cur].policy = (p == TP_RUN) ? TP_RUN : TP_PAUSE; }
int  task_policy(void)     { return g_init ? g_t[g_cur].policy : TP_PAUSE; }
void task_set_cleanup(void (*fn)(void)){ if(g_init) g_t[g_cur].cleanup = fn; }
void task_set_name(const char *name)
{
    if(!g_init || !name) return;
    int i = 0;
    while(name[i] && i < TASK_NAME - 1){ g_t[g_cur].name[i] = name[i]; i++; }
    g_t[g_cur].name[i] = 0;
}

int task_current(void){ return g_cur; }
int task_front(void)  { return g_front; }
int task_is_front(void){ return !g_init || g_cur == g_front; }

int task_count(void)
{
    int n = 0;
    for(int s = 0; s < TASK_MAX; s++) if(alive(s)) n++;
    return n;
}

int task_state(int s){ return (s >= 0 && s < TASK_MAX) ? g_t[s].state : TS_FREE; }
int task_slot_policy(int s){ return alive(s) ? g_t[s].policy : TP_PAUSE; }
uint32_t task_id(int s){ return alive(s) ? g_t[s].id : 0; }
const char *task_name(int s){ return alive(s) ? g_t[s].name : ""; }
int task_opener(int s){ return alive(s) ? g_t[s].opener : -1; }
uint32_t task_switches(int s){ return alive(s) ? g_t[s].switches : 0; }
uint32_t task_front_seq(int s){ return alive(s) ? g_t[s].front_seq : 0; }
uint32_t task_age_ticks(int s){ return alive(s) ? get_ticks() - g_t[s].born : 0; }
uint32_t task_stack_size(int s){ return (alive(s) && g_t[s].stack) ? TASK_STACK : 0; }

uint32_t task_stack_used(int s)
{
    if(!alive(s) || !g_t[s].stack) return 0;
    uint32_t i = TASK_CANARY_N;
    while(i < TASK_STACK / 4u && g_t[s].stack[i] == FILL) i++;
    return TASK_STACK - i * 4u;
}

int task_find_id(uint32_t id)
{
    for(int s = 0; s < TASK_MAX; s++) if(alive(s) && g_t[s].id == id) return s;
    return -1;
}

uint32_t task_refusals(int r){ return (r >= 0 && r < TY_REASONS) ? g_refused[r] : 0; }

const char *task_reason_name(int r)
{
    switch(r){
        case TY_RAN:     return "ran";
        case TY_ALONE:   return "nothing else to run";
        case TY_ENOINIT: return "not initialised";
        case TY_ELOCKED: return "switching is locked";
        case TY_EBUSY:   return "a driver holds a shared buffer";
        case TY_EPOLL:   return "inside kernel_poll";
    }
    return "unknown";
}

/* Running behind the front: runnable AND set to keep running. A task
   whose policy is TP_RUN but which is waiting -- the shell waiting for
   the app it opened -- is not running anything, and saying so was the
   first thing the switcher got wrong in QEMU. */
int task_running_behind(int s)
{
    return alive(s) && s != g_front && g_t[s].state == TS_READY &&
           g_t[s].policy == TP_RUN;
}

const char *task_state_name(int s)
{
    if(!alive(s)) return "gone";
    if(s == g_front) return "in front";
    if(task_running_behind(s)) return "running";
    return "paused";
}

void task_set_switcher(void (*fn)(int back)){ g_switcher = fn; }
void task_switcher_run(int back){ if(g_switcher) g_switcher(back); }
