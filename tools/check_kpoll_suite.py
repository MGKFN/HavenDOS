#!/usr/bin/env python3
"""check_kpoll_suite.py -- prove tests/test_kpoll.c can fail.

tests/test_kpoll.c passed 140 of 140 on its first run after two
fixture bugs of its own were found (a `got` argument that polled a
second time, and a wrap test whose starting value could not wrap).
That is the least trustworthy result this tree recognises.

kernel/kpoll.c is eighty lines and most of it is counters, which makes
it exactly the kind of file a suite can pass without testing: every
behavioural claim in it is about an ORDER or an INTERLEAVING rather
than about a return value.  The injections below are each a different
one of those:

  - "the tick does the work" is the whole design undone, and it would
    look completely fine on a machine with no USB device: the symptom
    is a corrupted transfer under load, which is the once-an-hour
    impossibility v0.8.7.5 spent a release finding.
  - "next_due is owned by the poll" turns a long install into a burst
    of a hundred catch-up runs.
  - "the due flag is cleared after the work" loses a tick that lands
    during the work -- the race the critical section is for.
  - "in_poll is cleared before the work" is the same class, inside the
    reentrancy guard.
  - "the refusals share a counter" is x_control() returning (3) for
    six causes, in the screen that exists to tell them apart.
  - "the due comparison is unsigned" is correct for 497 days.

Run:  python3 tools/check_kpoll_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "kernel/kpoll.c"
TEST = "tests/test_kpoll.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

CHECKS = [["python3", "tools/check_kpoll_sites.py"],
          ["python3", "tools/check_kpoll_guard.py"]]

BUGS = [
    ("the tick runs the work itself",
     "the entire design undone. Every layer in this tree shares "
     "module-level buffers and none is reentrant, so an IRQ0 that "
     "calls net_poll() while the shell is part-way through "
     "usb_control() corrupts that transfer. On an idle machine it "
     "looks perfect",
     """        if((int32_t)(g_ticks - g_cli[i].next_due) >= 0){
            if(g_cli[i].due) g_cli[i].late++;
            g_cli[i].due = 1;
            g_cli[i].next_due = g_ticks + g_cli[i].interval;
        }""",
     """        if((int32_t)(g_ticks - g_cli[i].next_due) >= 0){
            g_cli[i].next_due = g_ticks + g_cli[i].interval;
            g_cli[i].runs++;
            g_cli[i].fn();
        }"""),

    ("next_due is advanced by the poll instead of the tick",
     "a poll that was never reached -- a long install, a big download "
     "-- leaves next_due in the past, so the client fires on every "
     "subsequent poll until it catches up. A hundred seconds "
     "unattended becomes a hundred DHCP attempts in a row",
     """            if(g_cli[i].due) g_cli[i].late++;
            g_cli[i].due = 1;
            g_cli[i].next_due = g_ticks + g_cli[i].interval;""",
     """            if(g_cli[i].due) g_cli[i].late++;
            g_cli[i].due = 1;"""),

    ("the due flag is cleared AFTER the work runs",
     "the race the critical section exists for. A tick that lands "
     "while the client is running sets the flag, and the clear "
     "afterwards throws it away -- so a client that happens to take "
     "longer than its interval silently stops being periodic",
     """    uint32_t f = kpoll_irq_save();
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
    }""",
     """    uint32_t f = kpoll_irq_save();
    for(int i = 0; i < n; i++) run[i] = g_cli[i].due;
    g_in_poll = 1;
    kpoll_irq_restore(f);

    for(int i = 0; i < n; i++){
        if(!run[i]) continue;
        g_cli[i].runs++;
        g_cli[i].fn();
        g_cli[i].due = 0;
    }"""),

    ("the read-and-clear runs with interrupts ON",
     "a tick between the read of a flag and its clear loses that "
     "flag. One in a few thousand polls, for ever, and the only "
     "symptom is a client that occasionally skips an interval",
     """    uint32_t f = kpoll_irq_save();
    for(int i = 0; i < n; i++){
        run[i] = g_cli[i].due;
        g_cli[i].due = 0;
    }
    g_in_poll = 1;
    kpoll_irq_restore(f);""",
     """    for(int i = 0; i < n; i++){
        run[i] = g_cli[i].due;
        g_cli[i].due = 0;
    }
    g_in_poll = 1;"""),

    ("the reentrancy guard is never armed",
     "a client that calls kpoll_sleep_ms() runs itself from inside "
     "itself, with whatever module buffer it holds half-written",
     """    g_in_poll = 1;
    kpoll_irq_restore(f);""",
     """    kpoll_irq_restore(f);"""),

    ("in_poll is cleared before the work rather than after",
     "the guard is present, pairs correctly, and protects nothing. "
     "Same shape as the one above and much harder to see",
     """    for(int i = 0; i < n; i++){
        if(!run[i]) continue;
        g_cli[i].runs++;
        g_cli[i].fn();
    }

    g_in_poll = 0;
    return KPOLL_RAN;""",
     """    g_in_poll = 0;

    for(int i = 0; i < n; i++){
        if(!run[i]) continue;
        g_cli[i].runs++;
        g_cli[i].fn();
    }

    return KPOLL_RAN;"""),

    ("the busy depth is not checked",
     "'the shell is idle' and 'no subsystem is mid-operation' are "
     "different statements: fs/fat16.c's writable borrow can be open "
     "across a keyboard wait, and a poll then calls blk_scan(), which "
     "rebuilds the table an index into it is live on",
     """    if(g_depth > 0){ refuse(KPOLL_EBUSY); return KPOLL_EBUSY; }""",
     """    /* depth not checked */"""),

    ("in_poll and depth are checked in the other order",
     "both guards are live when a client opens a critical section and "
     "then polls, and only one of them is the true reason. Reporting "
     "EBUSY there sends the reader to look for a driver that is "
     "holding a buffer, when what happened is a client polling itself",
     """    if(g_in_poll){ refuse(KPOLL_EREENTER); return KPOLL_EREENTER; }""",
     """    if(g_depth > 0){ refuse(KPOLL_EBUSY); return KPOLL_EBUSY; }
    if(g_in_poll){ refuse(KPOLL_EREENTER); return KPOLL_EREENTER; }"""),

    ("kpoll_leave can drive the depth below zero",
     "one unmatched leave anywhere and the poll can never refuse "
     "again -- the guard reads as present and is permanently open",
     """void kpoll_leave(void){ if(g_depth > 0) g_depth--; }""",
     """void kpoll_leave(void){ g_depth--; }"""),

    ("every refusal increments the same counter",
     "x_control() returning (3) for six causes, in the one screen "
     "that exists to tell the four apart. 'a driver holds a buffer' "
     "is normal and expected; 'already polling' should never happen",
     """static void refuse(int reason)
{
    if(reason > 0 && reason < KPOLL_REASONS) g_refusals[reason]++;
}""",
     """static void refuse(int reason)
{
    (void)reason;
    g_refusals[KPOLL_EBUSY]++;
}"""),

    ("an interval rounds DOWN to ticks",
     "1 ms becomes zero ticks, next_due stays equal to the current "
     "tick for ever, and the client runs on every poll. The symptom "
     "is 'the shell got slow' and nothing says why",
     """    uint32_t t = (interval_ms + (1000u / KPOLL_HZ) - 1u) / (1000u / KPOLL_HZ);
    if(t == 0) t = 1;""",
     """    uint32_t t = interval_ms / (1000u / KPOLL_HZ);"""),

    ("the client table is not bounded",
     "a ninth registration writes past g_cli[]. The array is "
     "followed by g_ncli and g_ready, so the first thing it corrupts "
     "is the count that bounds it",
     """    if(g_ncli >= KPOLL_MAX_CLIENTS) return -1;""",
     """    /* unbounded */"""),

    ("the due comparison is unsigned",
     "correct for 497 days. g_ticks is a uint32_t at 100 Hz, and "
     "after it wraps every client's next_due is in the far future, so "
     "nothing periodic ever runs again -- on a machine that is never "
     "switched off, which is what a desktop OS is",
     """        if((int32_t)(g_ticks - g_cli[i].next_due) >= 0){""",
     """        if(g_ticks >= g_cli[i].next_due){"""),

    ("the polling sleep polls once, at the end",
     "a four-second DHCP wait that polls after its four seconds has "
     "not polled during the only four seconds that mattered",
     """    for(;;){
        kernel_poll();
        if((get_ticks() - start) >= want) return;
        sleep_ms(10);
    }""",
     """    sleep_ms(ms);
    kernel_poll();
    (void)start; (void)want;"""),

    ("`late` is not counted",
     "the only number that can tell 'the work is slow' from 'the poll "
     "is rare'. runs alone cannot: a client that ran once in a "
     "hundred intervals and a client that ran once in one look "
     "identical",
     """            if(g_cli[i].due) g_cli[i].late++;""",
     """            /* late not counted */"""),

    ("kpoll_init() does not clear the refusal counters",
     "they are static, so this only shows after a reinit -- which is "
     "what every section of the suite does, and what a `kpoll reset` "
     "would do. A counter that carries over reads as refusals that "
     "never happened",
     """    for(int i = 0; i < KPOLL_REASONS; i++) g_refusals[i] = 0;""",
     """    /* counters not cleared */"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """uint32_t kpoll_ticks(void){ return g_ticks; }""",
    """/* ticks since kpoll_init(), as the IRQ counted them */
uint32_t kpoll_ticks(void){ return g_ticks; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "kpoll", SRC, TEST, CC, BUGS, HARMLESS, timeout=60, checks=CHECKS,
        preamble="Proving tests/test_kpoll.c can fail.\n"
                 "kernel/kpoll.c is eighty lines of counters whose every "
                 "claim is about an order or an interleaving."))
