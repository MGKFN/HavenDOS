/* ════════════════════════════════════════════════════════════════════
   test_machine.c -- resetting and powering off, against a machine that
   refuses to die.

   `reboot` and `poweroff` hung the HP. Reported 20 October. Nine sites
   wrote the same two sequences by hand and not one of them knew about
   port 0xCF9, so on a laptop with no 8042 and no emulator ACPI ports
   both commands reached `for(;;) cli; hlt` and sat under "Rebooting
   HavenDOS... Please wait." for ever.

   What makes that bug hard to test is that a working reset STOPS
   EXECUTION. There is no return value and no state afterwards: the
   only observable behaviour is the sequence of port writes, the order
   they come in, and which of them are reached when the ones before do
   nothing.

   So that is what is checked here. include/io.h's IO_H seam supplies
   recording accessors and include/machine_asm.h's MACHINE_ASM_H seam
   supplies a triple fault that notes it was asked for instead of
   faulting, and a park that returns. kernel/machine.c is then
   #included and runs unmodified -- the real escalation, not a copy of
   it. Because nothing here actually resets, every fallback stage is
   reached in a single run and the whole ladder is visible at once.

   The simulated machine is deliberately UNKIND, which is the lesson
   0.8.5 learned ten times and 20 October learned four more. In
   particular it can hold the 8042 input buffer full for ever, which
   is what an SMM-emulated controller does under load -- and what the
   unbounded `1: inb $0x64; testb $2; jnz 1b` loop this replaces would
   have hung inside, before reaching the reset write at all. That hang
   is indistinguishable from a failed reset when you are looking at a
   laptop, and it is check 'a stuck 8042 does not hang the wait'.

   The log records EVENTS, not every port touch. The first version of
   this file logged each io_wait() as its own entry, and settle() does
   ten thousand of them -- so the log overflowed inside stage 1 and
   every later stage silently vanished. 19 of 43 checks "failed" on
   correct code. Delays are counted instead, and each event carries
   the running count, which is what the settle checks read.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-56s got %ld\n", what, got); }
}

/* ── a machine that records instead of resetting ──────────────────── */
#define IO_H

/* Only events worth ordering go in the log: port writes, status
   reads, the triple fault, the park. Each carries the number of
   io_wait()s that had happened when it was recorded, so "did stage 1
   settle before stage 2" is a subtraction rather than a scan. */
#define IOLOG_MAX 256
typedef struct { char kind; uint16_t port; uint32_t val; uint32_t waits; } io_rec_t;
static io_rec_t iolog[IOLOG_MAX];
static int      ion;
static uint32_t nwait;

/* What inb(0x64) reports. Bit 1 set means "input buffer full", i.e.
   the controller is not ready for a command. kbc_stuck leaves it set
   for ever, which is the case that used to hang. */
static int kbc_stuck;
static int kbc_reads;

#define KBC_READS_LOGGED 8

static void rec(char kind, uint16_t port, uint32_t val){
    if(ion < IOLOG_MAX){
        iolog[ion].kind  = kind;
        iolog[ion].port  = port;
        iolog[ion].val   = val;
        iolog[ion].waits = nwait;
    }
    ion++;
}

static void outb(uint16_t p, uint8_t v){ rec('b', p, v); }
static void outw(uint16_t p, uint16_t v){ rec('w', p, v); }
static void outl(uint16_t p, uint32_t v){ rec('l', p, v); }
static inline void io_wait(void){ nwait++; }

static uint8_t inb(uint16_t p){
    if(p == 0x64){
        kbc_reads++;
        /* Logged only for the first few: a stuck controller is read a
           hundred thousand times and the checks need the first two. */
        if(kbc_reads <= KBC_READS_LOGGED) rec('r', p, 0);
        /* Ready on the third look unless stuck -- so the wait loop is
           exercised rather than satisfied immediately. */
        return (kbc_stuck || kbc_reads < 3) ? 0x02 : 0x00;
    }
    rec('r', p, 0);
    return 0xFF;
}
static uint16_t inw(uint16_t p){ rec('r', p, 0); return 0xFFFF; }
static uint32_t inl(uint16_t p){ rec('r', p, 0); return 0xFFFFFFFFu; }

/* ── the two instructions a test cannot execute ───────────────────── */
#define MACHINE_ASM_H
static int tripled, parked;
static void machine_triple_fault(void){ tripled++; rec('t', 0, 0); }
static void machine_park(void){ parked++; rec('p', 0, 0); }

/* ── the screen ───────────────────────────────────────────────────── */
#include "../include/vga.h"
#define MSG_MAX 16
static char msgs[MSG_MAX][128];
static int  nmsg;
static int  msg_at_io;          /* ion when the first message was drawn */

void vga_puts_at(const char *s, int x, int y, vga_color_t fg, vga_color_t bg){
    (void)x; (void)y; (void)fg; (void)bg;
    if(nmsg == 0) msg_at_io = ion;
    if(nmsg < MSG_MAX){
        snprintf(msgs[nmsg], sizeof msgs[nmsg], "%s", s);
        nmsg++;
    }
}

/* ── the code under test, compiled as it ships ────────────────────── */
#include "../kernel/machine.c"

/* ── helpers over the recorded log ────────────────────────────────── */
static void reset_log(void){
    ion = 0; nmsg = 0; msg_at_io = -1;
    tripled = parked = 0;
    kbc_reads = 0; nwait = 0;
    memset(iolog, 0, sizeof iolog);
}

static int logged(void){ return ion < IOLOG_MAX ? ion : IOLOG_MAX; }

/* Index of the n-th (1-based) write of val to port, or -1. */
static int find_write(uint16_t port, uint32_t val, int nth){
    int seen = 0;
    for(int i = 0; i < logged(); i++){
        if(iolog[i].kind != 'b' && iolog[i].kind != 'w' && iolog[i].kind != 'l')
            continue;
        if(iolog[i].port == port && iolog[i].val == val){
            if(++seen == nth) return i;
        }
    }
    return -1;
}

static int find_mark(char kind){
    for(int i = 0; i < logged(); i++)
        if(iolog[i].kind == kind) return i;
    return -1;
}

static int count_write(uint16_t port){
    int n = 0;
    for(int i = 0; i < logged(); i++)
        if(iolog[i].port == port &&
           (iolog[i].kind == 'b' || iolog[i].kind == 'w' || iolog[i].kind == 'l'))
            n++;
    return n;
}

/* io_wait()s that happened between two logged events. */
static long waits_between(int a, int b){
    if(a < 0 || b < 0 || a >= logged() || b >= logged()) return -1;
    return (long)iolog[b].waits - (long)iolog[a].waits;
}

/* The n-th (1-based) logged read of the 8042 status port. */
static int find_kbc_read(int nth){
    int seen = 0;
    for(int i = 0; i < logged(); i++)
        if(iolog[i].kind == 'r' && iolog[i].port == 0x64 && ++seen == nth)
            return i;
    return -1;
}

int main(void)
{
    /* ════════════════════════════════════════════════════════════════
       reset: the ladder, in order
       ════════════════════════════════════════════════════════════════ */
    head("every reset method is tried, in the order that works");
    reset_log();
    kbc_stuck = 0;
    machine_reset();
    {
        int warm  = find_write(0xCF9, 0x06, 1);
        int hard  = find_write(0xCF9, 0x0E, 1);
        int pulse = find_write(0x64,  0xFE, 1);
        int trip  = find_mark('t');
        int park  = find_mark('p');

        ck("0xCF9 warm reset is attempted",     warm  >= 0, warm);
        ck("0xCF9 full reset is attempted",     hard  >= 0, hard);
        ck("the 8042 pulse is attempted",       pulse >= 0, pulse);
        ck("a triple fault is attempted",       trip  >= 0, trip);
        ck("the machine is parked at the end",  park  >= 0, park);

        /* The order is the whole point: 0xCF9 is the register that
           exists on the HP, so it must be tried BEFORE the 8042 that
           does not. A ladder in the other order is the shipped bug
           with extra steps. */
        ck("warm 0xCF9 precedes the full one",  warm >= 0 && hard > warm, hard);
        ck("0xCF9 precedes the 8042 pulse",     hard >= 0 && pulse > hard, pulse);
        ck("the 8042 pulse precedes the triple fault",
                                                pulse >= 0 && trip > pulse, trip);
        ck("the triple fault precedes the park",
                                                trip >= 0 && park > trip, park);
    }

    head("0xCF9 is triggered on an edge, so it is armed first");
    {
        /* RST_CPU (bit 2) triggers on 0 -> 1. Writing 0x06 into a
           register that already holds 0x06 does nothing whatsoever,
           which is why each trigger is preceded by a write with the
           bit low. Drop the arming write and this register silently
           stops working on the second attempt. */
        int arm1  = find_write(0xCF9, 0x02, 1);
        int warm  = find_write(0xCF9, 0x06, 1);
        int arm2  = find_write(0xCF9, 0x02, 2);
        int hard  = find_write(0xCF9, 0x0E, 1);
        ck("the warm trigger is armed first", arm1 >= 0 && warm > arm1, arm1);
        ck("the full trigger is armed too",   arm2 >= 0 && hard > arm2, arm2);
        ck("nothing sits between arm and trigger",
           arm1 >= 0 && warm == arm1 + 1 && arm2 >= 0 && hard == arm2 + 1,
           warm - arm1);
        ck("the edge is given a moment to be seen",
           waits_between(arm1, warm) >= 1 && waits_between(arm2, hard) >= 1,
           waits_between(arm1, warm));
        ck("nothing is written to 0xCF9 beyond those four",
           count_write(0xCF9) == 4, count_write(0xCF9));
    }

    head("a reset is given time to happen before the next is tried");
    {
        /* Without a settle, stage 2 is reached while stage 1 is still
           in flight and the machine is blamed for ignoring a reset it
           was in the middle of performing. */
        int warm  = find_write(0xCF9, 0x06, 1);
        int arm2  = find_write(0xCF9, 0x02, 2);
        int hard  = find_write(0xCF9, 0x0E, 1);
        int pulse = find_write(0x64,  0xFE, 1);
        int trip  = find_mark('t');
        ck("the warm reset settles before the full one",
           waits_between(warm, arm2) > 1000, waits_between(warm, arm2));
        ck("the full reset settles before the 8042",
           waits_between(hard, pulse) > 1000, waits_between(hard, pulse));
        ck("the 8042 settles before the triple fault",
           waits_between(pulse, trip) > 1000, waits_between(pulse, trip));
    }

    head("a reset does not power the machine off instead");
    ck("no QEMU PM1a write",  count_write(0x604)  == 0, count_write(0x604));
    ck("no Bochs PM1a write", count_write(0xB004) == 0, count_write(0xB004));
    ck("no VBox PM1a write",  count_write(0x4004) == 0, count_write(0x4004));

    head("the failure is reported, and only once everything was tried");
    {
        int park = find_mark('p');
        ck("something is printed",            nmsg >= 1, nmsg);
        ck("the power button is mentioned",
           nmsg >= 2 && strstr(msgs[1], "power button") != NULL, nmsg);
        /* Printed BEFORE any attempt is the shipped lie: the old
           poweroff said "It is now safe to power off." and then did
           not power off. */
        ck("nothing is printed before the attempts",
           msg_at_io > find_mark('t'), msg_at_io);
        /* msg_at_io is the index the NEXT event will take, so a park
           landing exactly there is a park immediately after the
           message -- which is what should happen. */
        ck("the park comes after the message", park >= msg_at_io, park);
    }

    /* ════════════════════════════════════════════════════════════════
       reset: the 8042 wait, which is where the old code hung
       ════════════════════════════════════════════════════════════════ */
    head("the 8042 is waited for, politely");
    reset_log();
    kbc_stuck = 0;
    machine_reset();
    {
        int r1 = find_kbc_read(1);
        int r2 = find_kbc_read(2);
        ck("the status register is read at all", r1 >= 0, r1);
        ck("it is read more than once",          r2 >= 0, r2);
        /* v0.8.5.3: two reads of an SMM-emulated controller with no
           I/O delay between them return the same stale value. That is
           the bug that doubled every keystroke, and the old reset
           loop had exactly the same shape. */
        ck("there is an io_wait between consecutive reads",
           waits_between(r1, r2) >= 1, waits_between(r1, r2));
        ck("the pulse follows the reads",
           find_write(0x64, 0xFE, 1) > r2, find_write(0x64, 0xFE, 1));
    }

    head("a stuck 8042 does not hang the wait");
    reset_log();
    kbc_stuck = 1;                  /* input buffer full for ever */
    machine_reset();                /* must RETURN, not spin      */
    {
        /* If this check is reached at all, the wait was bounded. The
           old code was `1: inb $0x64; testb $2, %al; jnz 1b` with no
           exit -- on a controller that never reports ready, it hung
           here, before the reset write. From the outside that is
           identical to a reset that did not work. */
        ck("the wait returned", 1, 0);
        ck("it gave up rather than spinning for ever",
           kbc_reads > 1000, kbc_reads);
        ck("the reset command is sent anyway",
           find_write(0x64, 0xFE, 1) >= 0, find_write(0x64, 0xFE, 1));
        ck("the ladder continues past it",  find_mark('t') >= 0, find_mark('t'));
        ck("the machine is still parked",   parked == 1, parked);
    }

    /* ════════════════════════════════════════════════════════════════
       poweroff
       ════════════════════════════════════════════════════════════════ */
    head("poweroff tries every emulator it has ever run under");
    reset_log();
    machine_poweroff();
    {
        int q = find_write(0x604,  0x2000, 1);
        int b = find_write(0xB004, 0x2000, 1);
        int v = find_write(0x4004, 0x3400, 1);
        ck("QEMU PM1a_CNT gets SLP_TYP=5|SLP_EN",  q >= 0, q);
        ck("Bochs PM1a_CNT gets the same",         b >= 0, b);
        ck("VirtualBox gets its own value",        v >= 0, v);
        ck("they are tried in that order",  q >= 0 && b > q && v > b, v);
        ck("each is given time to take effect",
           waits_between(q, b) > 1000 && waits_between(b, v) > 1000,
           waits_between(q, b));
        ck("the machine is parked afterwards", parked == 1, parked);
    }

    head("poweroff does not reset the machine instead");
    {
        /* A shutdown that reboots is worse than a shutdown that fails:
           the person walks away from a machine they believe is off and
           it comes back up. */
        ck("no 0xCF9 write",        count_write(0xCF9) == 0, count_write(0xCF9));
        ck("no 8042 reset pulse",
           find_write(0x64, 0xFE, 1) < 0, find_write(0x64, 0xFE, 1));
        ck("no triple fault",       tripled == 0, tripled);
    }

    head("poweroff tells the truth when it could not");
    {
        ck("something is printed",      nmsg >= 1, nmsg);
        ck("it does not claim success",
           nmsg >= 1 && strstr(msgs[0], "safe to power off") == NULL, nmsg);
        ck("the power button is mentioned",
           nmsg >= 2 && strstr(msgs[1], "power button") != NULL, nmsg);
        ck("nothing is printed before the attempts",
           msg_at_io > find_write(0x4004, 0x3400, 1), msg_at_io);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
