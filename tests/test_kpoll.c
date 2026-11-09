/* ════════════════════════════════════════════════════════════════════
   test_kpoll.c -- the tick sets flags, kernel_poll() does the work,
                   and every refusal has a name.

   v0.8.8 "Helios" gives HavenDOS a bottom half.  include/kpoll.h has
   the argument; the short version is that every layer in this tree
   shares module-level buffers, none of it is reentrant, and work run
   from the wrong place corrupts a transfer already in flight.  The
   symptom of that is a once-an-hour impossibility, and v0.8.7.5's
   shared event ring is this project's record of what one costs: an
   unanswered DHCP write broke descriptor reads on a webcam, and it
   took a release to find.

   ── WHAT MAKES THIS SUITE ABLE TO SAY ANYTHING ──────────────────────

   kernel/kpoll.c is COMPILED here, through include/kpoll_asm.h's seam,
   so the real read-and-clear runs.  And because kpoll_tick() is called
   BY HAND rather than by a timer, this suite can produce the exact
   interleaving the cli protects against -- a tick landing between the
   read of a due flag and its clear -- which a suite running against a
   real 100 Hz interrupt could not do reliably and could not do at all
   from ring 3.

   The seam is counted, not discarded: "the critical section was
   entered" is one of the things under test, and a save/restore pair
   that is not balanced is a kernel that comes back from a poll with
   interrupts off.

   ── WHAT IS COUNTED, AND WHY IT IS NOT THE RETURN VALUE ─────────────

   This tree's standing rule is to count the WIRE rather than the
   outcome, because two different failures reach a caller as the same
   number.  Here the "wire" is the CLIENT: every client below counts
   its own calls, and every refusal test asserts that the call count
   DID NOT MOVE -- not merely that kernel_poll() returned non-zero.

   A client that runs when it should not and a client that is refused
   both leave kernel_poll() returning something; only one of them has
   touched a shared buffer.

   ── AND THE REFUSALS ARE ASSERTED BY CODE ───────────────────────────

   0.8.7.7 spent a release on what a nameless refusal costs: strip a
   length guard from mux_recv() and it returns len-hdr, which for a
   4-byte length is -4, which is USB_ENODEV -- "nothing on the port" --
   and the next person goes to check whether the phone is plugged in.
   "The poll did not run" has four causes here and four different
   remedies, so every refusal test below names the exact KPOLL_* code
   and the refusal COUNTER for that reason.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-54s got %ld\n", what, got); }
}

/* ── the kpoll_asm.h seam ────────────────────────────────────────────
   Counted, and it also lets a client plant a tick "during" the
   critical section -- see sneaky_client() below. */
#define KPOLL_ASM_H
#include "../include/types.h"
static int irq_saves, irq_restores, irq_depth, irq_depth_max;
static uint32_t kpoll_irq_save(void){
    irq_saves++; irq_depth++;
    if(irq_depth > irq_depth_max) irq_depth_max = irq_depth;
    return 0x202u;                       /* a plausible EFLAGS with IF set */
}
static void kpoll_irq_restore(uint32_t f){
    irq_restores++; irq_depth--;
    (void)f;
}

/* ── the two edges kpoll.c reaches for ───────────────────────────────
   get_ticks() is DRIVEN by this suite rather than read from a PIT, so
   kpoll_sleep_ms()'s loop is deterministic. */
static uint32_t sim_ticks10;             /* 10 ms units, as get_ticks() is */
static long     sleep_calls, sleep_total_ms;
uint32_t get_ticks(void){ return sim_ticks10; }
void sleep_ms(uint32_t ms){
    sleep_calls++;
    sleep_total_ms += ms;
    sim_ticks10 += ms / 10u ? ms / 10u : 1u;
}

#include "../kernel/kpoll.c"

/* ── the clients.  Each counts its own calls: the WIRE. ──────────────── */
static long calls_a, calls_b, calls_c;
static void client_a(void){ calls_a++; }
static void client_b(void){ calls_b++; }
static void client_c(void){ calls_c++; }

/* A client that tries to poll from inside itself -- which is what a
   client calling kpoll_sleep_ms() amounts to. */
static long reentry_rc, reentry_calls;
static void client_reenter(void){
    reentry_calls++;
    reentry_rc = kernel_poll();
}

/* A client that opens a critical section and polls inside it: both
   guards are live at once, and only one of them can be the reason. */
static long nested_rc;
static void client_enter_then_poll(void){
    kpoll_enter();
    nested_rc = kernel_poll();
    kpoll_leave();
}

/* A client that becomes due again while it is running, by driving the
   tick from inside its own work. */
static long selfdue_calls;
static void client_self_due(void){
    selfdue_calls++;
    for(int i = 0; i < 300; i++) kpoll_tick();   /* 3 seconds of ticks */
}

/* A client that plants a tick in the middle of the snapshot, by being
   called from the loop that runs after the critical section closed.
   This is the ordinary case and it must be picked up NEXT poll, not
   lost. */
static long late_setter_calls;
static void client_late_setter(void){ late_setter_calls++; }

static void reset_counts(void){
    calls_a = calls_b = calls_c = 0;
    reentry_rc = reentry_calls = 0;
    nested_rc = 0;
    selfdue_calls = 0;
    late_setter_calls = 0;
}

static void ticks(int n){ for(int i = 0; i < n; i++) kpoll_tick(); }

static void clients_main(void);

int main(void)
{
    printf("\ntest_kpoll -- the tick, the bottom half, and the refusals\n");

    /* ════════════════════════════════════════════════════════════════
       1. BEFORE kpoll_init(): A NAMED REFUSAL, NOT A CRASH
       kernel/main.c calls kpoll_init() before irq_enable_basic(), and
       the ordering is written down there -- but "safe because the
       callee checks" has to actually be true.
       ════════════════════════════════════════════════════════════════ */
    head("before kpoll_init(): refused by name, and the tick is harmless");
    ck("kernel_poll() refuses", kernel_poll() == KPOLL_ENOINIT, kernel_poll());
    ck("  ... as 'not initialised'",
       !strcmp(kpoll_reason_name(KPOLL_ENOINIT), "not initialised"), 0);
    /* A tick before init must not write into a structure about to be
       zeroed, and must not fault. */
    ticks(5);
    ck("a tick before init does not register anything",
       kpoll_client_count() == 0, kpoll_client_count());
    ck("kpoll_register() before init refuses",
       kpoll_register("early", 1000, client_a) == -1, 0);
    ck("  ... and registered nothing", kpoll_client_count() == 0,
       kpoll_client_count());

    kpoll_init();
    ck("after kpoll_init() the tick count starts at zero",
       kpoll_ticks() == 0, kpoll_ticks());
    ck("  ... and the refusal counters are clear",
       kpoll_refusals(KPOLL_ENOINIT) == 0, kpoll_refusals(KPOLL_ENOINIT));

    /* ════════════════════════════════════════════════════════════════
       2. THE INTERVAL ARITHMETIC
       An interval in milliseconds has to become a whole number of
       ticks, and v0.7.5's pit_us() is this tree's record of what a
       second copy of a divisor costs: pit_us() hardcoded the divisor
       while pit_init() accepted any hz, so any call but pit_init(100)
       silently corrupted every timing calculation in the OS.
       ════════════════════════════════════════════════════════════════ */
    head("milliseconds to ticks: rounded UP, and never to zero");
    struct { uint32_t ms; uint32_t want; const char *what; } iv[] = {
        { 1000, 100, "1000 ms is 100 ticks at 100 Hz" },
        {   10,   1, "10 ms is one tick" },
        {    1,   1, "1 ms rounds UP to one tick, not down to zero" },
        {    0,   1, "0 ms becomes one tick, not every poll for ever" },
        {   15,   2, "15 ms rounds up to two ticks" },
        { 2000, 200, "2000 ms is 200 ticks" },
    };
    for(unsigned i = 0; i < sizeof iv / sizeof iv[0]; i++){
        kpoll_init();
        int h = kpoll_register("x", iv[i].ms, client_a);
        ck(iv[i].what, kpoll_client_interval(h) == iv[i].want,
           kpoll_client_interval(h));
    }
    ck("KPOLL_HZ is the rate pit_init() is called with", KPOLL_HZ == 100,
       KPOLL_HZ);

    /* ════════════════════════════════════════════════════════════════
       3. THE TICK SETS FLAGS AND DOES NO WORK
       This is the whole design, and it is the one thing a suite can
       assert directly that no amount of reading can.
       ════════════════════════════════════════════════════════════════ */
    head("the tick does NOT run the work -- the entire point");
    kpoll_init();
    reset_counts();
    kpoll_register("a", 1000, client_a);
    ticks(1000);                        /* ten seconds of interrupts */
    ck("ten seconds of ticks arrived", kpoll_ticks() == 1000, kpoll_ticks());
    ck("and the client was never called", calls_a == 0, calls_a);
    ck("  ... nor was the critical section entered by the tick",
       irq_saves == 0, irq_saves);
    ck("one kernel_poll() then calls it exactly once",
       (kernel_poll() == KPOLL_RAN) && calls_a == 1, calls_a);
    ck("  ... and a second poll with no tick between does not",
       (kernel_poll() == KPOLL_RAN) && calls_a == 1, calls_a);

    /* ════════════════════════════════════════════════════════════════
       4. next_due IS OWNED BY THE TICK, AND WHY THAT MATTERS
       If the main path owned it, a poll that was never reached -- a
       long install, a big download -- would leave next_due in the past
       and the client would fire on every subsequent poll until it
       caught up.  Owned by the tick, a client that is already due
       stays due exactly once, and `late` counts the misses.
       ════════════════════════════════════════════════════════════════ */
    head("a poll that never happens does not become a burst");
    kpoll_init();
    reset_counts();
    kpoll_register("a", 1000, client_a);
    ticks(10000);                       /* a hundred seconds unattended */
    ck("a hundred intervals went by", kpoll_ticks() == 10000, kpoll_ticks());
    kernel_poll();
    ck("the client runs ONCE, not a hundred times", calls_a == 1, calls_a);
    ck("  ... and `late` recorded the 99 it missed",
       kpoll_client_late(0) == 99, kpoll_client_late(0));
    /* late is the number that tells "the work is slow" from "the poll
       is rare", which is the question a screen cannot answer from runs
       alone. */
    ck("  ... while `runs` says 1", kpoll_client_runs(0) == 1,
       kpoll_client_runs(0));

    /* And after catching up, the next interval is a fresh one rather
       than an immediate re-fire. */
    reset_counts();
    ticks(99);
    kernel_poll();
    ck("99 more ticks is not yet due", calls_a == 0, calls_a);
    ticks(1);
    kernel_poll();
    ck("the 100th tick makes it due", calls_a == 1, calls_a);

    /* ════════════════════════════════════════════════════════════════
       5. SEVERAL CLIENTS, EACH ON ITS OWN INTERVAL
       ════════════════════════════════════════════════════════════════ */
    head("three clients, three intervals, independent");
    kpoll_init();
    reset_counts();
    kpoll_register("a", 1000, client_a);    /* 1 s  */
    kpoll_register("b", 2000, client_b);    /* 2 s  */
    kpoll_register("c", 5000, client_c);    /* 5 s  */
    ck("three registered", kpoll_client_count() == 3, kpoll_client_count());
    for(int sec = 1; sec <= 10; sec++){
        ticks(100);
        kernel_poll();
    }
    ck("the 1 s client ran ten times", calls_a == 10, calls_a);
    ck("the 2 s client ran five times", calls_b == 5, calls_b);
    ck("the 5 s client ran twice", calls_c == 2, calls_c);
    ck("and none of them is late", kpoll_client_late(0) == 0 &&
       kpoll_client_late(1) == 0 && kpoll_client_late(2) == 0,
       kpoll_client_late(0) * 100 + kpoll_client_late(1) * 10 +
       kpoll_client_late(2));
    /* Names are KEPT, not copied -- the header says so, and `kpoll`
       prints them. */
    ck("the names come back", !strcmp(kpoll_client_name(0), "a") &&
       !strcmp(kpoll_client_name(2), "c"), 0);
    ck("an index that is not a client gives an empty name, not a crash",
       !strcmp(kpoll_client_name(99), "") &&
       !strcmp(kpoll_client_name(-1), ""), 0);
    ck("  ... and zero for its counters",
       kpoll_client_runs(99) == 0 && kpoll_client_interval(-1) == 0, 0);

    /* The table is bounded, and a full table refuses rather than
       writing past it. */
    head("the client table is bounded and says so");
    kpoll_init();
    {
        int ok = 1, last = 0;
        for(int i = 0; i < KPOLL_MAX_CLIENTS; i++)
            if(kpoll_register("x", 1000, client_a) < 0) ok = 0;
        ck("KPOLL_MAX_CLIENTS fit", ok == 1, ok);
        last = kpoll_register("one too many", 1000, client_a);
        ck("one more is refused", last == -1, last);
        ck("  ... and the count did not move",
           kpoll_client_count() == KPOLL_MAX_CLIENTS, kpoll_client_count());
        /* A refused registration must not have been half-written: the
           last valid slot still holds the client that owns it. */
        ck("  ... and the last slot still has its own client",
           !strcmp(kpoll_client_name(KPOLL_MAX_CLIENTS - 1), "x"), 0);
    }
    ck("a null function is refused",
       kpoll_register("null", 1000, 0) == -1, 0);
    ck("a null name is refused",
       kpoll_register(0, 1000, client_a) == -1, 0);

    /* ════════════════════════════════════════════════════════════════
       6. THE CRITICAL SECTION
       ════════════════════════════════════════════════════════════════ */
    head("the read-and-clear runs with interrupts off, and restores them");
    kpoll_init();
    reset_counts();
    irq_saves = irq_restores = irq_depth = irq_depth_max = 0;
    kpoll_register("a", 1000, client_a);
    ticks(100);
    kernel_poll();
    ck("one save", irq_saves == 1, irq_saves);
    ck("one restore", irq_restores == 1, irq_restores);
    ck("balanced -- a poll does not return with interrupts off",
       irq_depth == 0, irq_depth);
    ck("never nested", irq_depth_max == 1, irq_depth_max);
    /* THE WORK RUNS OUTSIDE THE LOCK.  A client issues control
       transfers and DHCP requests; holding the machine with interrupts
       disabled across those would stop the clock that schedules them.
       Checked by having the client observe the depth. */
    ck("and the client ran with interrupts back on",
       calls_a == 1, calls_a);
    {
        /* Directly: a client that reads the depth. */
        static int seen_depth;
        void probe(void){ seen_depth = irq_depth; }
        kpoll_init();
        irq_depth = 0;
        kpoll_register("probe", 1000, probe);
        ticks(100);
        kernel_poll();
        ck("the client saw interrupts enabled while it ran",
           seen_depth == 0, seen_depth);
    }
    /* A poll with nothing due still takes the lock once -- the flags
       have to be read to find that out -- and still balances. */
    head("a poll with nothing due still balances the lock");
    kpoll_init();
    irq_saves = irq_restores = irq_depth = 0;
    kpoll_register("a", 1000, client_a);
    kernel_poll();
    ck("one save, one restore, nothing due",
       irq_saves == 1 && irq_restores == 1 && irq_depth == 0,
       irq_saves * 100 + irq_restores);
    /* And a REFUSED poll takes it zero times: the refusals all happen
       before the lock, which is what keeps a refusal cheap enough to
       be in the keyboard's inner loop. */
    kpoll_set_enabled(0);
    irq_saves = 0;
    ck("a refused poll does not take the lock at all",
       (kernel_poll() == KPOLL_EOFF) && irq_saves == 0, irq_saves);
    kpoll_set_enabled(1);

    /* ════════════════════════════════════════════════════════════════
       7. A TICK LANDING DURING THE SNAPSHOT IS NOT LOST
       The race the lock exists for, produced on purpose.  A due flag
       set after its snapshot was taken must be picked up on the NEXT
       poll rather than cleared unseen.
       ════════════════════════════════════════════════════════════════ */
    head("a tick during the run is picked up next poll, not lost");
    kpoll_init();
    reset_counts();
    kpoll_register("setter", 1000, client_late_setter);
    kpoll_register("b", 1000, client_b);
    ticks(100);
    kernel_poll();
    ck("both ran", late_setter_calls == 1 && calls_b == 1,
       late_setter_calls * 10 + calls_b);
    /* Now plant a full interval of ticks from INSIDE a client's work,
       i.e. after the snapshot was taken and the flags cleared. */
    kpoll_init();
    reset_counts();
    kpoll_register("self-due", 1000, client_self_due);
    ticks(100);
    kernel_poll();
    ck("the client ran once", selfdue_calls == 1, selfdue_calls);
    kernel_poll();
    ck("  ... and the ticks it planted make it due again next poll",
       selfdue_calls == 2, selfdue_calls);

    /* ════════════════════════════════════════════════════════════════
       8. THE FOUR REFUSALS, EACH BY CODE AND BY COUNTER
       And the client call count must NOT MOVE -- the wire, not the
       return value.
       ════════════════════════════════════════════════════════════════ */
    head("switched off: refused by name, and nothing ran");
    kpoll_init();
    reset_counts();
    kpoll_register("a", 1000, client_a);
    ticks(100);
    kpoll_set_enabled(0);
    ck("kpoll_enabled() says off", kpoll_enabled() == 0, 0);
    /* ONE call, into a variable.  The first version of this line read
       `ck(..., kernel_poll() == KPOLL_EOFF, kernel_poll())` -- which
       polls TWICE, because the `got` argument is evaluated too, and
       then asserted a refusal count of 2.  It passed.  A fixture whose
       own reporting changes the thing it measures is a fixture that
       cannot be reasoned about, and this one would have hidden a
       double-counted refusal exactly. */
    { int rc = kernel_poll();
      ck("refused as KPOLL_EOFF", rc == KPOLL_EOFF, rc); }
    ck("  ... THE CLIENT DID NOT RUN", calls_a == 0, calls_a);
    ck("  ... and the counter for that reason moved by one",
       kpoll_refusals(KPOLL_EOFF) == 1, kpoll_refusals(KPOLL_EOFF));
    ck("  ... and no other reason's counter moved",
       kpoll_refusals(KPOLL_EBUSY) == 0 &&
       kpoll_refusals(KPOLL_EREENTER) == 0 &&
       kpoll_refusals(KPOLL_ENOINIT) == 0, 0);
    kpoll_set_enabled(1);
    ck("switched back on, the still-due client runs",
       (kernel_poll() == KPOLL_RAN) && calls_a == 1, calls_a);

    head("a driver holds a shared buffer: refused, and nothing ran");
    kpoll_init();
    reset_counts();
    kpoll_register("a", 1000, client_a);
    ticks(100);
    kpoll_enter();
    ck("the depth is one", kpoll_depth() == 1, kpoll_depth());
    { int rc = kernel_poll();
      ck("refused as KPOLL_EBUSY", rc == KPOLL_EBUSY, rc); }
    ck("  ... THE CLIENT DID NOT RUN", calls_a == 0, calls_a);
    ck("  ... and the EBUSY counter moved by one",
       kpoll_refusals(KPOLL_EBUSY) == 1, kpoll_refusals(KPOLL_EBUSY));
    /* Nesting: usb_control() inside usb_enumerate() is the real case. */
    kpoll_enter();
    ck("it nests", kpoll_depth() == 2, kpoll_depth());
    ck("still refused at depth 2", kernel_poll() == KPOLL_EBUSY, 0);
    kpoll_leave();
    ck("one leave does not open it", kpoll_depth() == 1, kpoll_depth());
    ck("  ... and the poll is still refused",
       kernel_poll() == KPOLL_EBUSY, 0);
    kpoll_leave();
    ck("the matching leave opens it", kpoll_depth() == 0, kpoll_depth());
    ck("  ... and the client, still due, runs now",
       (kernel_poll() == KPOLL_RAN) && calls_a == 1, calls_a);
    /* An unmatched leave must not take the depth negative -- which
       would turn a missing kpoll_enter() somewhere into a poll that can
       never refuse again. */
    kpoll_leave(); kpoll_leave(); kpoll_leave();
    ck("extra leaves cannot drive the depth below zero",
       kpoll_depth() == 0, kpoll_depth());
    kpoll_enter();
    ck("  ... and one enter still refuses afterwards",
       kernel_poll() == KPOLL_EBUSY, 0);
    kpoll_leave();

    head("already polling: a client that polls from inside itself");
    kpoll_init();
    reset_counts();
    kpoll_register("reenter", 1000, client_reenter);
    kpoll_register("b", 2000, client_b);
    ticks(200);
    ck("the outer poll runs", kernel_poll() == KPOLL_RAN, 0);
    ck("  ... the client ran once", reentry_calls == 1, reentry_calls);
    ck("  ... and ITS poll was refused as KPOLL_EREENTER",
       reentry_rc == KPOLL_EREENTER, reentry_rc);
    ck("  ... the EREENTER counter moved",
       kpoll_refusals(KPOLL_EREENTER) == 1,
       kpoll_refusals(KPOLL_EREENTER));
    /* The important consequence: the OTHER client, which was also due,
       ran exactly once -- from the outer poll -- and not twice. */
    ck("  ... and the other due client ran once, not twice",
       calls_b == 1, calls_b);
    ck("after the outer poll returns, polling works again",
       (kernel_poll() == KPOLL_RAN), 0);

    /* BOTH GUARDS AT ONCE, and which one answers.  This is the check
       that says in_poll and depth are not the same test wearing two
       names: a client that opens a critical section and then polls is
       refused for being INSIDE A POLL, because that test comes first
       and is the stronger statement. */
    head("both guards live at once -- and the first one names it");
    kpoll_init();
    reset_counts();
    kpoll_register("enter-then-poll", 1000, client_enter_then_poll);
    ticks(100);
    kernel_poll();
    ck("refused as EREENTER, not EBUSY -- in_poll is checked first",
       nested_rc == KPOLL_EREENTER, nested_rc);
    ck("  ... and the depth came back down", kpoll_depth() == 0,
       kpoll_depth());

    /* ════════════════════════════════════════════════════════════════
       9. kpoll_sleep_ms: A SLEEP THAT POLLS
       And the thing that makes it worth having: it polls DURING the
       wait, not once at the end.
       ════════════════════════════════════════════════════════════════ */
    head("the polling sleep polls during the wait, not after it");
    kpoll_init();
    reset_counts();
    sleep_calls = sleep_total_ms = 0;
    sim_ticks10 = 0;
    kpoll_register("a", 100, client_a);       /* every 10 ms = 1 tick */
    /* Ticks arrive as sleep_ms advances the simulated clock, so drive
       them from the sleep edge: every 10 ms of sleep is one tick. */
    ck("a zero sleep still polls once",
       (kpoll_sleep_ms(0), sleep_calls == 0), sleep_calls);
    {
        /* 500 ms, in 10 ms steps: 50 real sleeps and 51 polls. */
        long before = sleep_calls;
        kpoll_sleep_ms(500);
        ck("500 ms is spent in short sleeps, not one long one",
           sleep_calls - before >= 40, sleep_calls - before);
        ck("  ... and the total slept is about 500 ms",
           sleep_total_ms >= 480 && sleep_total_ms <= 520, sleep_total_ms);
    }
    /* A client due during the sleep has to have been called before the
       sleep returns -- that is the whole difference from sleep_ms(). */
    head("a client due during a polling sleep runs before it returns");
    kpoll_init();
    reset_counts();
    sim_ticks10 = 0; sleep_calls = 0;
    kpoll_register("a", 100, client_a);
    ticks(10);                                /* already due */
    kpoll_sleep_ms(200);
    ck("the client ran during the sleep", calls_a >= 1, calls_a);
    /* And the contrast, which is the reason sleep_ms() was left alone:
       the driver-safe sleep does NOT poll. */
    reset_counts();
    kpoll_init();
    kpoll_register("a", 100, client_a);
    ticks(10);
    sleep_ms(200);
    ck("plain sleep_ms() does NOT poll -- drivers call that one",
       calls_a == 0, calls_a);

    /* ════════════════════════════════════════════════════════════════
       10. THE REASON NAMES
       `kpoll` prints these, and a screen that says "unknown" for a
       real code is the boot log's old problem in a new place.
       ════════════════════════════════════════════════════════════════ */
    head("every refusal code has a name, and nothing says 'unknown'");
    for(int r = 0; r < KPOLL_REASONS; r++){
        const char *n = kpoll_reason_name(r);
        ck("a code in range has a real name",
           n && *n && strcmp(n, "unknown") != 0, r);
    }
    ck("a code out of range says 'unknown' rather than reading off the end",
       !strcmp(kpoll_reason_name(KPOLL_REASONS), "unknown") &&
       !strcmp(kpoll_reason_name(-1), "unknown"), 0);
    ck("and the four refusals are four DIFFERENT names",
       strcmp(kpoll_reason_name(KPOLL_ENOINIT),
              kpoll_reason_name(KPOLL_EREENTER)) &&
       strcmp(kpoll_reason_name(KPOLL_EREENTER),
              kpoll_reason_name(KPOLL_EBUSY)) &&
       strcmp(kpoll_reason_name(KPOLL_EBUSY),
              kpoll_reason_name(KPOLL_EOFF)), 0);
    ck("KPOLL_RAN is zero, so a caller can test for it",
       KPOLL_RAN == 0, KPOLL_RAN);
    ck("  ... and every refusal is non-zero",
       KPOLL_ENOINIT && KPOLL_EREENTER && KPOLL_EBUSY && KPOLL_EOFF, 0);
    ck("refusals(out of range) is 0, not a read off the end",
       kpoll_refusals(-1) == 0 && kpoll_refusals(KPOLL_REASONS) == 0, 0);

    /* ════════════════════════════════════════════════════════════════
       11. THE TICK COUNTER WRAPS, AND THE DUE TEST SURVIVES IT
       g_ticks is a uint32_t at 100 Hz, so it wraps after about 497
       days.  The comparison is written as a signed difference for that
       reason, and a suite that never tested it would be trusting a
       cast.  497 days is not a hypothetical for a machine that is
       never switched off.
       ════════════════════════════════════════════════════════════════ */
    head("the tick counter wraps and the due arithmetic holds");
    kpoll_init();
    reset_counts();
    /* Drive g_ticks to just below the wrap.  Done through the real
       tick so that next_due is computed the way it will be in life. */
    /* 0xFFFFFFC0 + 100 ticks crosses zero.  0xFFFFFF00 does NOT -- the
       first version of this used it, next_due came out as 0xFFFFFF64,
       and the test "next_due wrapped" was asserting something that had
       not happened.  A fixture value that cannot produce the condition
       is the shape 0.8.7's harnesses found four times, and arithmetic
       is where it is easiest to write. */
    g_ticks = 0xFFFFFFC0u;
    kpoll_register("a", 1000, client_a);      /* next_due wraps round */
    ck("next_due really wrapped past zero",
       g_cli[0].next_due < 0x1000u, (long)g_cli[0].next_due);
    ticks(99);
    kernel_poll();
    ck("not due one tick early, across the wrap", calls_a == 0, calls_a);
    ticks(1);
    kernel_poll();
    ck("due on the right tick, across the wrap", calls_a == 1, calls_a);
    ck("  ... and it did not fire a hundred times",
       kpoll_client_late(0) == 0, kpoll_client_late(0));

    /* ════════════════════════════════════════════════════════════════
       12. THE CLIENTS.  usb/hotplug.c and net/net_tick.c are compiled
       below this main(), so the sections for them run from
       clients_main(), called here.
       ════════════════════════════════════════════════════════════════ */
    clients_main();

    /* tools/run_suite.py reads this exact line and refuses a suite that
       does not print one, or that comes in under its floor -- which is
       what caught 621 silently-absent checks at v0.8.5.6. */
    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}

/* ════════════════════════════════════════════════════════════════════
   THE CLIENTS

   The mechanism above is the half a reader can check by reading.  The
   clients are the half that will actually be wrong, because each one
   is a small state machine about hardware that comes and goes.

   Both are COMPILED here, not modelled: net/net_tick.c and
   usb/hotplug.c as they ship, against stubbed edges that COUNT.  The
   standing rule in this tree is to count the wire rather than the
   outcome, so every stub below records what it was asked to do and the
   assertions are about those counts -- "the bus was walked once" is a
   different statement from "the rescan returned a device".
   ════════════════════════════════════════════════════════════════════ */

#include "../include/usb.h"
#include "../include/netdev.h"
#include "../include/net.h"

/* ── the edges, all counting ─────────────────────────────────────────── */
static long klogs, klog_warns;
void klog(int level, const char *fmt, ...){
    (void)fmt;
    klogs++;
    if(level == 1) klog_warns++;          /* KLOG_WARN, see include/klog.h */
}
uint32_t klog_now_ms(void){ return sim_ticks10 * 10u; }

/* net edges */
static int  sim_cfg_src = NET_CFG_FALLBACK;
static int  sim_active  = 0;
static int  sim_ncount  = 1;
static int  sim_link[4] = { 1, 1, 1, 1 };
static long dhcp_calls;
static int  dhcp_will_succeed;
static int  sim_haschar;

int      net_config_source(void){ return sim_cfg_src; }
void     net_config_source_set(int s){ sim_cfg_src = s; }
int      netdev_active(void){ return sim_active; }
int      netdev_count(void){ return sim_ncount; }
/* Modelled on net/netdev.c's own body, including the bounds check --
   a fixture looser than the real function would let a client pass that
   the kernel would refuse, and one STRICTER would fail a correct
   client.  Model what it does, in both directions. */
static long link_asks, link_asks_badindex;
int      netdev_link_up(int i){
    link_asks++;
    if(i < 0 || i >= sim_ncount){ link_asks_badindex++; return 0; }
    return (i < 4) ? sim_link[i] : 0;
}
const char *netdev_bus_name(int i){ (void)i; return "sim NIC"; }
int      keyboard_haschar(void){ return sim_haschar; }
int      dhcp_request(void){
    dhcp_calls++;
    if(dhcp_will_succeed) sim_cfg_src = NET_CFG_DHCP;
    return dhcp_will_succeed;
}

#include "../net/net_tick.c"

/* usb edges.  usb_rescan_all() calls seven things in a fixed order and
   the ORDER is the whole content of that function -- so the stubs
   record the order, not just the counts. */
static long  rescan_steps;
static char  rescan_order[64];
static int   rescan_order_n;
static long  enumerates, scans, probes, releases, forgets, closes;
static void note(char c){
    rescan_steps++;
    if(rescan_order_n < (int)sizeof rescan_order - 1)
        rescan_order[rescan_order_n++] = c;
    rescan_order[rescan_order_n] = 0;
}
int  msc_rescan(void){ note('m'); return 1; }
int  blk_scan(void){ note('b'); scans++; return 2; }
void usbnet_release(void){ note('r'); releases++; }
int  usbnet_probe(void){ note('p'); probes++; return 1; }
void lockdown_forget(void){ note('l'); forgets++; }
void usbmux_close(void){ note('x'); closes++; }
int  usb_enumerate(void){ note('e'); enumerates++; return 3; }
int  usb_hub_count(void){ return 0; }
int  usb_hub_devices(void){ return 0; }

/* Two simulated controllers, with a settable connected-port bitmap.
   Reads are COUNTED: "the poll is one MMIO read per root port" is a
   claim usb/hotplug.c's header makes, and a client that walked the
   whole bus every second instead would still pass every behavioural
   check below. */
static uint32_t sim_ports[2];
static int      sim_nports[2] = { 4, 4 };
static long     port_reads;
static int      sim_nhc = 1;

static int sim_port_count(struct usb_hc *h){ return sim_nports[h->index]; }
static long port_reads_hc[2];
static int sim_port_connected(struct usb_hc *h, int p){
    port_reads++;
    if(h->index >= 0 && h->index < 2) port_reads_hc[h->index]++;
    return (sim_ports[h->index] >> p) & 1u;
}
/* Designated, and it stays designated: a positional initialiser for a
   struct of function pointers silently rebinds everything after an
   inserted member, which include/usb.h warns about. */
static const usb_hc_ops_t SIM_OPS = {
    .port_count     = sim_port_count,
    .port_connected = sim_port_connected,
};
/* A controller that cannot report its ports at all -- a back-end
   without the optional ops.  It must contribute nothing rather than
   reading as "every port empty", which would make a device already
   plugged in look like a removal on the first poll. */
static const usb_hc_ops_t MUTE_OPS = { .port_count = 0 };
/* And the dangerous asymmetry the interface permits: a back-end that
   can COUNT its root ports and cannot say what is on them.  Both ops
   are optional in include/usb.h and nothing requires them to arrive
   together, so a client that checks only one of them dereferences a
   null function pointer -- and a fixture whose mute controller lacks
   BOTH can never produce that, which is why
   tools/check_hotplug_suite.py's injection into the guard was
   MISSED. */
static const usb_hc_ops_t HALF_OPS = {
    .port_count     = sim_port_count,
    .port_connected = 0,
};

static usb_hc_t sim_hc[2];
int             usb_hc_count(void){ return sim_nhc; }
const usb_hc_t *usb_hc(int i){ return (i >= 0 && i < 2) ? &sim_hc[i] : 0; }
const char     *usb_hc_name(const usb_hc_t *h){ (void)h; return "simHC"; }

#include "../usb/hotplug.c"

static void clients_main(void)
{
    /* ════════════════════════════════════════════════════════════════
       12. usb/hotplug.c -- the root-port bitmap
       ════════════════════════════════════════════════════════════════ */
    head("hot-plug: the baseline is not an arrival");
    sim_hc[0].ops = &SIM_OPS; sim_hc[0].index = 0;
    sim_hc[1].ops = &SIM_OPS; sim_hc[1].index = 1;
    sim_nhc = 1;
    sim_ports[0] = 0x5;                    /* ports 0 and 2 occupied */
    kpoll_init();
    rescan_steps = 0; rescan_order_n = 0; enumerates = 0;
    usb_kpoll_register();
    ck("registering took a reading", port_reads > 0, port_reads);
    ck("  ... and did NOT walk the bus",
       enumerates == 0, enumerates);
    ck("  ... and registered one client",
       kpoll_client_count() == 1, kpoll_client_count());
    ck("  ... at one second", kpoll_client_interval(0) == 100,
       kpoll_client_interval(0));

    head("hot-plug: nothing changes, nothing happens");
    ticks(100); kernel_poll();
    ck("the poll ran but did not rescan", enumerates == 0, enumerates);
    ticks(100); kernel_poll();
    ck("  ... still not, a second later", enumerates == 0, enumerates);

    head("hot-plug: a device arrives on port 1");
    sim_ports[0] = 0x7;
    ticks(100); kernel_poll();
    ck("the bus was walked once", enumerates == 1, enumerates);
    ck("  ... and the change was counted",
       usb_hotplug_changes() == 1, usb_hotplug_changes());
    ck("  ... and one rescan recorded", usb_hotplug_rescans() == 1,
       usb_hotplug_rescans());

    /* THE ORDER.  Every step is in usb/hotplug.c's header with the bug
       that put it there; a different order is a different program. */
    ck("the sequence ran in order: release, forget, close, enumerate, "
       "msc, blk, probe",
       !strcmp(rescan_order, "rlxembp"), 0);

    head("hot-plug: ONE plug is ONE rescan");
    /* The bitmap is re-read after the rescan, deliberately, because
       enumeration resets ports and a reading taken mid-change can
       disagree with the settled one.  Without that re-read, this next
       poll would rescan again. */
    ticks(100); kernel_poll();
    ck("the next poll does not rescan again", enumerates == 1, enumerates);
    ticks(300); kernel_poll();
    ck("  ... nor three seconds later", enumerates == 1, enumerates);

    head("hot-plug: a device is removed");
    sim_ports[0] = 0x3;                    /* port 2 gone */
    ticks(100); kernel_poll();
    ck("a removal rescans too", enumerates == 2, enumerates);
    ck("  ... and is counted as a change",
       usb_hotplug_changes() == 2, usb_hotplug_changes());

    head("hot-plug: two ports moving at once is still one rescan");
    sim_ports[0] = 0x0;                    /* both gone */
    ticks(100); kernel_poll();
    ck("one bus walk", enumerates == 3, enumerates);
    ck("  ... and two more changes counted",
       usb_hotplug_changes() == 4, usb_hotplug_changes());

    head("hot-plug: a second controller is watched too");
    sim_nhc = 2;
    sim_ports[1] = 0;
    ticks(100); kernel_poll();             /* baseline for hc1 */
    long e = enumerates;
    sim_ports[1] = 0x2;
    ticks(100); kernel_poll();
    ck("a change on controller 1 rescans", enumerates == e + 1,
       enumerates - e);

    head("hot-plug: a back-end with no port ops contributes nothing");
    sim_hc[1].ops = &MUTE_OPS;
    usb_hotplug_baseline();
    e = enumerates;
    ticks(100); kernel_poll();
    ck("a mute controller does not read as a mass removal",
       enumerates == e, enumerates - e);

    head("hot-plug: a back-end that counts ports but cannot read them");
    /* The asymmetry include/usb.h permits and nothing forbids: a
       port_count with no port_connected.  A client that guards only
       one of the two calls a null function pointer, and the mute
       controller above cannot produce that because it has neither. */
    sim_hc[1].ops = &HALF_OPS;
    sim_ports[1] = 0xF;                    /* and it claims four ports */
    usb_hotplug_baseline();
    e = enumerates;
    {
        long reads = port_reads_hc[1];
        ticks(100); kernel_poll();
        ck("it is read as nothing connected, not as a crash",
           enumerates == e, enumerates - e);
        /* THE WIRE, per controller.  A tautology here ("1, 0") would
           have thrown away the whole value of this check -- and
           counting ALL reads would have measured controller 0's four
           ports instead, which is what the first version did and why
           it read "got 4". */
        ck("  ... and not one of ITS four ports was read",
           port_reads_hc[1] == reads, port_reads_hc[1] - reads);
        ck("  ... while the working controller's were",
           port_reads > reads, port_reads);
    }
    sim_hc[1].ops = &SIM_OPS;
    sim_nhc = 1;

    head("hot-plug: a controller with more root ports than the bitmap");
    /* 32 bits, and a controller claiming more has its extras ignored.
       Said out loud rather than silently truncated: reading 32 of 40
       ports is the kind of truncation that looks like a dead socket. */
    sim_nports[0] = 40;
    usb_hotplug_baseline();
    ck("the truncation is counted, not silent",
       usb_hotplug_truncated() > 0, usb_hotplug_truncated());
    sim_nports[0] = 4;

    head("hot-plug: a NIC that arrives gets the DHCP sequence reset");
    /* The retry sequence has to have MOVED first.  The first version
       of this check called net_dhcp_retry_reset() and then asserted
       the count was zero, which it already was -- so the injection
       that deleted the reset was MISSED.  A fixture that cannot
       produce the before-state cannot test the transition. */
    kpoll_init();
    sim_cfg_src = NET_CFG_FALLBACK;
    sim_link[0] = 1; sim_haschar = 0; dhcp_will_succeed = 0;
    net_kpoll_register();
    sim_ports[0] = 0x1;
    usb_kpoll_register();
    for(int i = 0; i < 45; i++){ ticks(100); kernel_poll(); }
    ck("two retries have been spent", net_dhcp_tries() == 2,
       net_dhcp_tries());
    sim_ports[0] = 0x3;                    /* a device arrives */
    ticks(100); kernel_poll();
    ck("  ... and the arrival put the sequence back at the start",
       net_dhcp_tries() == 0, net_dhcp_tries());

    /* ════════════════════════════════════════════════════════════════
       13. net/net_tick.c -- the DHCP retry
       ════════════════════════════════════════════════════════════════ */
    head("dhcp retry: not while the address was negotiated");
    kpoll_init();
    sim_cfg_src = NET_CFG_DHCP;
    dhcp_calls = 0;
    net_kpoll_register();
    ck("two net clients registered", kpoll_client_count() == 2,
       kpoll_client_count());
    for(int i = 0; i < 100; i++){ ticks(100); kernel_poll(); }
    ck("a hundred seconds and no request was sent", dhcp_calls == 0,
       dhcp_calls);

    head("dhcp retry: nor while the address was set by hand");
    kpoll_init();
    sim_cfg_src = NET_CFG_MANUAL;
    dhcp_calls = 0;
    net_kpoll_register();
    for(int i = 0; i < 100; i++){ ticks(100); kernel_poll(); }
    ck("still nothing", dhcp_calls == 0, dhcp_calls);

    head("dhcp retry: on the fallback, it asks -- with backoff");
    kpoll_init();
    sim_cfg_src = NET_CFG_FALLBACK;
    sim_active = 0; sim_link[0] = 1; sim_haschar = 0;
    dhcp_will_succeed = 0;
    dhcp_calls = 0;
    net_kpoll_register();
    /* The first attempt is 10 s in, not immediately: the boot already
       made one. */
    for(int i = 0; i < 9; i++){ ticks(100); kernel_poll(); }
    ck("nothing in the first nine seconds", dhcp_calls == 0, dhcp_calls);
    ticks(100); kernel_poll();
    ck("the first retry is at ten seconds", dhcp_calls == 1, dhcp_calls);
    /* and the second is 30 s after that, not 10 */
    for(int i = 0; i < 29; i++){ ticks(100); kernel_poll(); }
    ck("the second is not 10 s later", dhcp_calls == 1, dhcp_calls);
    ticks(100); kernel_poll();
    ck("  ... it is 30 s later", dhcp_calls == 2, dhcp_calls);

    head("dhcp retry: it gives up, and says so exactly once");
    /* Run far past the last backoff step and count the warnings. A
       line printed every interval buries the one that matters --
       0.8.7.8's lesson, and `dmesg | grep` is how every hardware
       question in this tree has been answered. */
    klog_warns = 0;
    for(int i = 0; i < 1200; i++){ ticks(100); kernel_poll(); }
    ck("it stopped at five attempts", dhcp_calls == 5, dhcp_calls);
    ck("  ... and said so once, not on every poll",
       klog_warns == 1, klog_warns);
    ck("  ... and reports that the sequence is finished",
       net_dhcp_tries() == -1, net_dhcp_tries());

    head("dhcp retry: an answer stops it immediately");
    kpoll_init();
    sim_cfg_src = NET_CFG_FALLBACK;
    dhcp_calls = 0; dhcp_will_succeed = 1;
    net_kpoll_register();
    for(int i = 0; i < 200; i++){ ticks(100); kernel_poll(); }
    ck("exactly one request was needed", dhcp_calls == 1, dhcp_calls);
    ck("  ... and the sequence is finished", net_dhcp_tries() == -1,
       net_dhcp_tries());

    head("dhcp retry: no NIC, no request -- and the ORDER of the guards");
    kpoll_init();
    sim_cfg_src = NET_CFG_FALLBACK;
    sim_active = -1;
    dhcp_calls = 0; dhcp_will_succeed = 0;
    link_asks_badindex = 0;
    net_kpoll_register();
    for(int i = 0; i < 200; i++){ ticks(100); kernel_poll(); }
    ck("nothing was asked through a NIC that is not there",
       dhcp_calls == 0, dhcp_calls);
    /* ── THE WIRE, NOT THE OUTCOME ────────────────────────────────────
       netdev_link_up(-1) returns 0, so the CARRIER guard also stops
       this -- which means "no request was sent" passes with the no-NIC
       check deleted, and tools/check_nettick_suite.py's injection into
       it was MISSED. A property masked by a guard somewhere else, the
       shape four of test_netdev's seven holes had.
       So the check is that netdev_link_up was never ASKED about index
       -1: the NIC guard has to come first. */
    ck("  ... and the link was never asked about a NIC index of -1",
       link_asks_badindex == 0, link_asks_badindex);
    sim_active = 0;

    head("dhcp retry: no carrier, no four-second wait");
    kpoll_init();
    sim_cfg_src = NET_CFG_FALLBACK;
    sim_link[0] = 0;
    dhcp_calls = 0;
    net_kpoll_register();
    for(int i = 0; i < 200; i++){ ticks(100); kernel_poll(); }
    ck("a NIC reporting no carrier is not asked",
       dhcp_calls == 0, dhcp_calls);
    /* And the carrier coming up is what starts it. */
    sim_link[0] = 1;
    for(int i = 0; i < 20; i++){ ticks(100); kernel_poll(); }
    ck("  ... and it asks once the link comes up", dhcp_calls >= 1,
       dhcp_calls);

    head("dhcp retry: a keystroke waiting defers the attempt");
    /* dhcp_request() blocks for about four seconds and this runs from
       the keyboard's idle loop.  Deferring does not make the freeze
       shorter; it makes it land when nobody is typing. */
    kpoll_init();
    sim_cfg_src = NET_CFG_FALLBACK;
    sim_link[0] = 1;
    dhcp_calls = 0;
    sim_haschar = 1;
    net_kpoll_register();
    for(int i = 0; i < 60; i++){ ticks(100); kernel_poll(); }
    ck("nothing was sent while a key was waiting", dhcp_calls == 0,
       dhcp_calls);
    ck("  ... and the deferrals were counted",
       net_dhcp_deferred() > 0, net_dhcp_deferred());
    sim_haschar = 0;
    /* 200 ticks, because this client's poll interval is two seconds.
       One second was not enough and the first version of this check
       read "got 0" -- a fixture that cannot reach the state it
       asserts, which is the same shape as the wrap test above. */
    ticks(200); kernel_poll();
    ck("  ... and it goes as soon as the keyboard is quiet",
       dhcp_calls == 1, dhcp_calls);

    /* ── the carrier client ─────────────────────────────────────────── */
    head("carrier: the first reading is not an event");
    kpoll_init();
    sim_ncount = 2; sim_link[0] = 1; sim_link[1] = 0;
    sim_cfg_src = NET_CFG_DHCP;            /* keep dhcp quiet */
    net_kpoll_register();
    {
        uint32_t before = net_carrier_events();
        ticks(100); kernel_poll();
        ck("two NICs read, no events", net_carrier_events() == before,
           net_carrier_events() - before);
    }

    head("carrier: a transition is reported once");
    {
        uint32_t before = net_carrier_events();
        sim_link[1] = 1;
        ticks(100); kernel_poll();
        ck("one event", net_carrier_events() == before + 1,
           net_carrier_events() - before);
        ticks(300); kernel_poll();
        ck("  ... and not repeated while it stays up",
           net_carrier_events() == before + 1,
           net_carrier_events() - before);
        sim_link[1] = 0;
        ticks(100); kernel_poll();
        ck("  ... and going down is another event",
           net_carrier_events() == before + 2,
           net_carrier_events() - before);
    }

    head("carrier: a link coming back asks for an address again");
    /* An iPhone hotspot switched off and on hands out a different
       subnet, and the address we hold came from the old one.  The
       symptom is `dns: resolution failed` over a link reporting itself
       up, which is 26 October on the HP exactly.

       THE RETRY SEQUENCE HAS TO HAVE MOVED FIRST.  The first version
       of this block spent twenty polls with a keystroke pending --
       which DEFERS an attempt without advancing the counter -- and
       then asserted the counter was zero, which it had never left.
       The injection that deleted the reset was MISSED.  Same vacuous
       shape as the hot-plug version of this check, found in the same
       run. */
    kpoll_init();
    sim_cfg_src = NET_CFG_FALLBACK;
    sim_ncount = 2; sim_active = 0;
    sim_link[0] = 1;                       /* the active NIC can be asked */
    sim_link[1] = 0;                       /* the other one is down      */
    sim_haschar = 0; dhcp_will_succeed = 0;
    dhcp_calls = 0;
    net_kpoll_register();
    for(int i = 0; i < 45; i++){ ticks(100); kernel_poll(); }
    ck("two real attempts have been spent", net_dhcp_tries() == 2,
       net_dhcp_tries());
    sim_link[1] = 1;                       /* and NIC 1's link comes up  */
    ticks(100); kernel_poll();
    ck("  ... and the carrier client put the sequence back to the start",
       net_dhcp_tries() == 0, net_dhcp_tries());

    head("carrier: a NIC count change forgets the remembered state");
    /* netdev_unregister() COMPACTS the table, so index 1 after an
       unplug is a different NIC from index 1 before it.  Remembering
       its carrier across that is the v0.8.7 pinned-index bug in a new
       place: a NIC that happens to differ from its predecessor would
       report a transition it never made. */
    kpoll_init();
    sim_cfg_src = NET_CFG_DHCP;
    sim_ncount = 2; sim_link[0] = 1; sim_link[1] = 1;
    net_kpoll_register();
    ticks(100); kernel_poll();
    {
        uint32_t before = net_carrier_events();
        sim_ncount = 1;                    /* NIC 1 unplugged */
        sim_link[0] = 0;                   /* and the survivor differs */
        ticks(100); kernel_poll();
        ck("the count change produced no phantom transition",
           net_carrier_events() == before, net_carrier_events() - before);
        /* ... and the next change IS seen, so forgetting did not
           disable the client. */
        sim_link[0] = 1;
        ticks(100); kernel_poll();
        ck("  ... and a real transition after it is still seen",
           net_carrier_events() == before + 1,
           net_carrier_events() - before);
    }
}
