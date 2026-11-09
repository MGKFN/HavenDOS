/* ════════════════════════════════════════════════════════════════════
   kernel/kpoll.c -- the tick sets flags; kernel_poll() does the work.

   The argument for the split, the mechanical rules that keep it, and
   why the refusals are named are all in include/kpoll.h.  What is here
   is the two halves and the one critical section between them.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/kpoll.h"
#include "../include/kpoll_asm.h"

typedef struct {
    const char *name;
    void      (*fn)(void);
    uint32_t    interval;      /* in ticks, never 0                     */
    uint32_t    next_due;      /* tick at which it becomes due          */
    uint32_t    runs;
    uint32_t    late;          /* due again while still flagged due     */
    volatile uint8_t due;      /* written by the tick, cleared by poll  */
} kpoll_client_t;

static kpoll_client_t g_cli[KPOLL_MAX_CLIENTS];
static int            g_ncli;
static int            g_ready;
static int            g_on = 1;
static int            g_in_poll;
static int            g_depth;
static volatile uint32_t g_ticks;
static uint32_t       g_refusals[KPOLL_REASONS];

void kpoll_init(void)
{
    g_ncli = 0;
    g_ticks = 0;
    g_in_poll = 0;
    g_depth = 0;
    g_on = 1;
    for(int i = 0; i < KPOLL_REASONS; i++) g_refusals[i] = 0;
    g_ready = 1;
}

int kpoll_register(const char *name, uint32_t interval_ms, void (*fn)(void))
{
    if(!g_ready || !fn || !name) return -1;
    if(g_ncli >= KPOLL_MAX_CLIENTS) return -1;

    /* Round UP to a whole tick, and never to zero.  An interval of 0
       would make next_due equal to the current tick for ever, so the
       client would run on every poll and nothing would say why -- which
       is the shape of bug that reads as "the shell got slow". */
    uint32_t t = (interval_ms + (1000u / KPOLL_HZ) - 1u) / (1000u / KPOLL_HZ);
    if(t == 0) t = 1;

    int i = g_ncli++;
    g_cli[i].name     = name;
    g_cli[i].fn       = fn;
    g_cli[i].interval = t;
    g_cli[i].next_due = g_ticks + t;
    g_cli[i].runs     = 0;
    g_cli[i].late     = 0;
    g_cli[i].due      = 0;
    return i;
}

/* ── INTERRUPT CONTEXT ───────────────────────────────────────────────
   Three writes and a compare, per client, over at most eight clients.
   No call of any kind: see the note in kpoll.h about pit_us().

   next_due advances HERE, not in kernel_poll(), and that is deliberate.
   If the main path owned it, a poll that was never reached -- a long
   install, a `wget` of a large file -- would leave next_due in the past
   and the client would fire on every subsequent poll until it caught
   up.  Owned by the tick, a client that is already due simply stays
   due, and `late` counts how often that happened, which is the only
   number that can tell "the work is slow" from "the poll is rare". */
void kpoll_tick(void)
{
    g_ticks++;
    if(!g_ready) return;
    for(int i = 0; i < g_ncli; i++){
        if((int32_t)(g_ticks - g_cli[i].next_due) >= 0){
            if(g_cli[i].due) g_cli[i].late++;
            g_cli[i].due = 1;
            g_cli[i].next_due = g_ticks + g_cli[i].interval;
        }
    }
}

uint32_t kpoll_ticks(void){ return g_ticks; }

void kpoll_enter(void){ g_depth++; }
void kpoll_leave(void){ if(g_depth > 0) g_depth--; }
int  kpoll_depth(void){ return g_depth; }

void kpoll_set_enabled(int on){ g_on = on ? 1 : 0; }
int  kpoll_enabled(void){ return g_on; }

static void refuse(int reason)
{
    if(reason > 0 && reason < KPOLL_REASONS) g_refusals[reason]++;
}

/* ── TASK CONTEXT ────────────────────────────────────────────────────
   The read-and-clear of the due flags is the one place in this file
   that races the tick, and it is done with interrupts off through
   include/kpoll_asm.h.  The WORK runs with interrupts back on: it
   issues control transfers and DHCP requests, and holding the machine
   with interrupts disabled across those would stop the clock that
   schedules them.

   So the snapshot is taken under the lock and the work is run outside
   it.  A client that becomes due during its own run is flagged again
   and picked up next time, which `late` counts. */
int kernel_poll(void)
{
    if(!g_ready){ refuse(KPOLL_ENOINIT); return KPOLL_ENOINIT; }
    if(!g_on)   { refuse(KPOLL_EOFF);    return KPOLL_EOFF;    }

    /* Reentrancy first, and refused rather than nested.  A client that
       calls kpoll_sleep_ms() -- which polls -- would otherwise run
       itself from inside itself, with whatever module buffer it is
       holding half-written. */
    if(g_in_poll){ refuse(KPOLL_EREENTER); return KPOLL_EREENTER; }

    /* And a subsystem mid-operation.  THIS IS NOT THE SAME CHECK.
       in_poll says "we are already in here"; depth says "somebody owns
       a shared buffer".  fs/fat16.c's writable borrow can be open
       across a keyboard wait, so the shell being idle does not mean
       the filesystem is. */
    if(g_depth > 0){ refuse(KPOLL_EBUSY); return KPOLL_EBUSY; }

    uint8_t run[KPOLL_MAX_CLIENTS];
    int     n = g_ncli;

    uint32_t f = kpoll_irq_save();
    for(int i = 0; i < n; i++){
        run[i] = g_cli[i].due;
        g_cli[i].due = 0;
    }
    g_in_poll = 1;
    kpoll_irq_restore(f);

    for(int i = 0; i < n; i++){
        if(!run[i]) continue;
        g_cli[i].runs++;
        g_cli[i].fn();
    }

    g_in_poll = 0;
    return KPOLL_RAN;
}

/* A sleep that polls.  Idle paths only -- tools/check_kpoll_sites.py
   refuses this symbol in drivers/, usb/, net/ and fs/.

   kernel/pit.c's sleep_ms() is unchanged and is the one a driver calls.
   The two look interchangeable and are not, which is why they have
   different names rather than a flag: a flag is something a caller
   gets wrong, a name is something a grep can refuse. */
extern void     sleep_ms(uint32_t ms);
extern uint32_t get_ticks(void);

void kpoll_sleep_ms(uint32_t ms)
{
    if(ms == 0){ kernel_poll(); return; }
    /* Chopped into short real sleeps with a poll between them, rather
       than one long sleep and a poll at the end: a 4-second DHCP wait
       that polls once at its end has not polled during the only four
       seconds that mattered. */
    uint32_t start = get_ticks();
    uint32_t want  = ms / 10u;          /* get_ticks() counts 10 ms units */
    if(want == 0) want = 1;
    for(;;){
        kernel_poll();
        if((get_ticks() - start) >= want) return;
        sleep_ms(10);
    }
}

int         kpoll_client_count(void){ return g_ncli; }
const char *kpoll_client_name(int i){
    return (i >= 0 && i < g_ncli) ? g_cli[i].name : "";
}
uint32_t kpoll_client_interval(int i){
    return (i >= 0 && i < g_ncli) ? g_cli[i].interval : 0;
}
uint32_t kpoll_client_runs(int i){
    return (i >= 0 && i < g_ncli) ? g_cli[i].runs : 0;
}
uint32_t kpoll_client_late(int i){
    return (i >= 0 && i < g_ncli) ? g_cli[i].late : 0;
}
uint32_t kpoll_refusals(int reason){
    return (reason >= 0 && reason < KPOLL_REASONS) ? g_refusals[reason] : 0;
}
const char *kpoll_reason_name(int reason)
{
    switch(reason){
        case KPOLL_RAN:      return "ran";
        case KPOLL_ENOINIT:  return "not initialised";
        case KPOLL_EREENTER: return "already polling";
        case KPOLL_EBUSY:    return "a driver holds a shared buffer";
        case KPOLL_EOFF:     return "switched off";
    }
    return "unknown";
}
