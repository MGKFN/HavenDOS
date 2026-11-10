/* ════════════════════════════════════════════════════════════════════
   test_kbdinit.c -- the 8042 is talked to with a delay between every
   access, during INITIALISATION as well as at runtime.

   v0.8.5.2 and v0.8.5.3 were both this bug. Every keystroke arrived
   twice on the HP, and the cause was two 8042 accesses with no I/O
   delay between them: an 8042 emulated in SMM -- which is what that
   laptop has, because it has no real one -- had not updated its status
   register by the time the driver read it again, so the driver saw the
   same byte waiting and read it twice. `io_wait()` had been sitting in
   include/io.h, unused, the whole time.

   v0.8.5.3 fixed kbc_drain(), the RUNTIME read path, and added
   kbc_delay() there. It did not touch keyboard_init(), which talks to
   the same controller through the same ports and had six loops of the
   same shape:

       static void kbc_wait_wr(void){ int t=0x10000; while(t--&&(inb(0x64)&0x02)); }
       int t=0x10000; while(t--&&(inb(0x64)&0x01)) inb(0x60);
       t=0x10000; while(t--&&!(inb(0x64)&0x01));

   all reading 0x64 and 0x60 back to back with nothing in between. On
   the controller that doubled keystrokes, a stale status read means
   kbc_wait_wr() can return while the input buffer is still full -- and
   the command byte written next is then dropped. A dropped
   `outb(0x64,0x60)` or `outb(0x60,cfg)` means the config write never
   lands: no IRQ1, or scancode translation left off.

   WHAT THIS SUITE CHECKS is a structural invariant, not a guess about
   any particular firmware's timing. Two rules, and only two, because
   the first version of this suite checked "no two 8042 accesses
   adjacent" and that over-reported: reading 0x64, seeing a byte is
   waiting, and immediately reading 0x60 is the correct way to use the
   controller and is not a defect. The rules that matter are:

     RULE 1  After touching 0x60, do not read 0x64 without an
             io_wait(). The status register has not caught up, so OBF
             still reads set and the byte just taken is taken again.
             That is the keystroke doubling of v0.8.5.3 exactly, and
             kbc_drain() already obeys this -- its kbc_delay() sits
             between reading the data port and the next status read,
             which is the right place.

     RULE 2  Do not read 0x64 twice in a row without an io_wait().
             A polling loop that does is reading a register that
             cannot have changed, and on an emulated controller it can
             see a stale "input buffer empty" and return while the
             buffer is still full -- so the command byte written next
             is dropped. A dropped outb(0x64,0x60) or outb(0x60,cfg)
             means the config write never lands: no IRQ1, or scancode
             translation left off.

   Both are objective, need no model of SMM timing, and fail on code
   that has the bug.

   drivers/keyboard.c is COMPILED here through include/io.h's IO_H
   seam, so the sequence under test is the shipping one. The simulated
   controller answers plausibly enough for keyboard_init() to run to
   completion; what is asserted is the ACCESS PATTERN, which is the
   thing that was wrong.
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

/* ── the access log ───────────────────────────────────────────────── */
#define LOGMAX 4096
/* Port accesses one init may spend on a controller that never
   answers. ~1 us each, so this is about 0.15 s. */
#define BUDGET 150000
enum { A_RD, A_WR, A_WAIT };
typedef struct { int kind; uint16_t port; uint8_t val; } acc_t;
static acc_t log_[LOGMAX];
static int logn;

static void rec(int kind, uint16_t port, uint8_t val){
    if(logn < LOGMAX){ log_[logn].kind=kind; log_[logn].port=port;
                       log_[logn].val=val; }
    logn++;
}

/* ── a simulated 8042, just plausible enough to finish init ───────── */
#define QMAX 32
static uint8_t q[QMAX];
static int qh, qt;
static uint8_t sim_cfg = 0x45;
static int expect_cfg_data;          /* a 0x60 command is awaiting data */
static int expect_mouse_cmd;
static int sim_mouse_present;   /* 0 is the HP: the aux port never answers */
static int cmds_seen;
static int sim_dead;            /* answers nothing, ever */
/* The seam supplies the bound too, as it supplies the delay. */
#define I8042_SPINS 10000

static void qpush(uint8_t b){
    if(sim_dead) return;
    if(((qt+1)%QMAX)!=qh){ q[qt]=b; qt=(qt+1)%QMAX; }
}
static int  qempty(void){ return qh==qt; }
static uint8_t qpop(void){ uint8_t b=q[qh]; qh=(qh+1)%QMAX; return b; }

#define IO_H
static inline void io_wait(void){ rec(A_WAIT, 0x80, 0); }
/* Part of the io.h seam too, since defining IO_H skips the real one.
   Four io_waits, as the shipping version is -- the checker treats any
   recorded wait as a delay, so the count does not matter here, only
   that one happened. */
static inline void i8042_delay(void){
    io_wait(); io_wait(); io_wait(); io_wait();
}

static void outb(uint16_t p, uint8_t v){
    rec(A_WR, p, v);
    if(p == 0x64){
        cmds_seen++;
        switch(v){
            case 0x20: qpush(sim_cfg); break;     /* read config */
            case 0x60: expect_cfg_data = 1; break;/* write config */
            case 0xAD: case 0xAE: break;          /* disable/enable port */
            case 0xA8: break;                     /* enable aux port     */
            case 0xD4: expect_mouse_cmd = 1; break; /* next 0x60 is aux  */
            default: break;
        }
    } else if(p == 0x60){
        if(expect_cfg_data){ sim_cfg = v; expect_cfg_data = 0; }
        else if(expect_mouse_cmd){
            /* ── v0.8.8.1: THE AUX PATH, MODELLED ─────────────────────
               This stub did not know 0xD4 at all, so no mouse command
               was ever acknowledged and mouse_init() always bailed at
               its reset. The existing test above worked around it by
               PRE-LOADING the queue with the bytes it expected -- and
               that works only as long as nothing checks the outcome,
               because mouse_init() reads the config byte first and
               eats the first pre-loaded byte as the config.

               A fixture that answers is a different thing from a
               fixture that has the answers queued: the first can be
               told to refuse. sim_mouse_present = 0 is the HP. */
            expect_mouse_cmd = 0;
            if(sim_mouse_present){
                qpush(0xFA);                      /* ACK             */
                if(v == 0xFF){ qpush(0xAA); qpush(0x00); }  /* BAT, id */
            }
        }
        else if(v == 0xF4)  qpush(0xFA);          /* enable scanning -> ACK */
    }
}
static void outw(uint16_t p, uint16_t v){ (void)p; (void)v; }
static void outl(uint16_t p, uint32_t v){ (void)p; (void)v; }

/* v0.8.8: a key that arrives LATE.  keyboard_waitchar()'s loop only
   goes round if the queue is empty when it first looks, and the one
   thing worth checking about that loop is that it polls the bottom half
   while it waits.  So the key is pushed after a few status reads --
   i.e. from inside the wait -- rather than before the call, which is
   what the first version of that check did and why it read "got 0". */
static int sim_feed_after;
static uint8_t sim_feed_code;

static uint8_t inb(uint16_t p){
    rec(A_RD, p, 0);
    if(sim_dead) return 0x00;                    /* nothing, ever */
    if(p == 0x64 && sim_feed_after > 0 && --sim_feed_after == 0)
        qpush(sim_feed_code);
    if(p == 0x64) return (uint8_t)(qempty() ? 0x00 : 0x01);  /* IBF clear */
    if(p == 0x60) return qempty() ? 0x00 : qpop();
    return 0xFF;
}
static uint16_t inw(uint16_t p){ rec(A_RD,p,0); return 0xFFFF; }
static uint32_t inl(uint16_t p){ rec(A_RD,p,0); return 0xFFFFFFFFu; }

/* ── the edges keyboard.c reaches for ─────────────────────────────── */
static int irqs_registered;
void irq_register(int n, void (*fn)(void)){ (void)n; (void)fn; irqs_registered++; }
void klog(int level, const char *fmt, ...){ (void)level; (void)fmt; }
uint32_t get_ticks(void){ return 0; }
void usb_hid_poll(void){ }
/* v0.8.8.11: drivers/mouse.c pumps both polled pointer buses. */
void i2c_hid_poll(void){ }
void vga_flush(void){ }
/* mouse.c clamps the pointer to the ACTIVE grid now, so it asks. */
static int sim_cols = 80, sim_rows = 25;
int vga_cols(void){ return sim_cols; }
int vga_rows(void){ return sim_rows; }
/* v0.8.9.6: and the size of a cell in pixels, so the speed can be a
   fraction of the screen rather than a count of cells. */
static int sim_cw = 8, sim_ch = 16;
void vga_cell_px(int *w, int *h){ if(w) *w = sim_cw; if(h) *h = sim_ch; }

/* v0.8.8: kernel_poll(), which keyboard_waitchar() now calls from
   its wait loop -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

/* ── the driver, as it ships ──────────────────────────────────────── */
#include "../drivers/keyboard.c"
/* The mouse shares the controller, and shared the bug. */
#include "../drivers/mouse.c"

/* ── the invariant ────────────────────────────────────────────────── */
static int is_8042(uint16_t p){ return p == 0x60 || p == 0x64; }

/* Violations of RULE 1 and RULE 2 (see the header). Returns the count
   and reports the first few, because the useful output is WHERE. */
static int delay_violations(int report)
{
    int bad = 0, shown = 0;
    int prev = -1;                       /* index of the last 8042 access */
    for(int i = 0; i < logn && i < LOGMAX; i++){
        if(log_[i].kind == A_WAIT){ prev = -1; continue; }
        if(!is_8042(log_[i].port)) continue;
        if(prev >= 0){
            int touched_data   = (log_[prev].port == 0x60);
            int reads_status   = (log_[i].kind == A_RD && log_[i].port == 0x64);
            int status_twice   = (log_[prev].kind == A_RD &&
                                  log_[prev].port == 0x64 && reads_status);
            const char *rule = NULL;
            if(touched_data && reads_status)      rule = "RULE 1";
            else if(status_twice)                 rule = "RULE 2";
            if(rule){
                bad++;
                if(report && shown < 6){
                    shown++;
                    printf("      %s: %s 0x%02X then read 0x64, no io_wait\n",
                           rule,
                           log_[prev].kind == A_RD ? "read " : "write",
                           log_[prev].port);
                }
            }
        }
        prev = i;
    }
    return bad;
}

static void reset_log(void){
    logn = 0; qh = qt = 0; cmds_seen = 0; sim_dead = 0;
    expect_cfg_data = 0; sim_cfg = 0x45;
    memset(log_, 0, sizeof log_);
}

int main(void)
{
    head("keyboard_init() reaches the controller at all");
    reset_log();
    keyboard_init();
    ck("it issued 8042 commands",      cmds_seen > 0, cmds_seen);
    ck("it registered the IRQ handler", irqs_registered == 1, irqs_registered);
    ck("it did not run away",           logn < LOGMAX, logn);

    head("and it waits between accesses -- the v0.8.5.3 rule");
    {
        int bad = delay_violations(1);
        ck("the controller is given time to update its status",
           bad == 0, bad);
    }

    head("the config write actually landed");
    {
        /* If a command byte is dropped because the driver wrote it while
           the input buffer was still full, the controller never sees the
           config write. Checked through the simulator's own copy rather
           than by reading the driver's intent. */
        ck("IRQ1 is enabled in the controller config",
           (sim_cfg & 0x01) != 0, sim_cfg);
        ck("scancode translation is left on",
           (sim_cfg & 0x40) != 0, sim_cfg);
        ck("the keyboard clock is not disabled",
           (sim_cfg & 0x20) == 0, sim_cfg);
    }

    head("the runtime read path keeps the rule too (v0.8.5.3's own fix)");
    {
        reset_log();
        qpush(0x1E);                      /* 'a' make */
        qpush(0x9E);                      /* 'a' break */
        keyboard_getchar();
        int bad = delay_violations(1);
        ck("draining scancodes keeps the rules",
           bad == 0, bad);
    }

    head("every wait is bounded, so a dead controller cannot hang the box");
    {
        /* Nothing is ever queued and the status never shows a byte, so
           every wait-for-data loop must give up on its own. Reaching
           this check at all is the proof. */
        reset_log();
        sim_dead = 1;
        keyboard_init();
        ck("init returned against a silent controller", 1, 0);
        /* Judged as a boot-time budget, not against the log's capacity:
           overflowing a 4096-entry log is not a defect, but spending a
           second of boot on a controller that is not there is. At
           roughly a microsecond per port access this is a ~0.15 s
           ceiling. The bound and the delay in include/io.h are sized
           together for exactly this reason. */
        ck("and within a sane boot-time budget", logn < BUDGET, logn);
    }

    head("the mouse shares the controller, and shared the bug");
    {
        /* drivers/mouse.c drives the SAME 8042 through the SAME two
           ports, and v0.8.5.3's fix never reached it. For the mouse a
           stale status read costs differently: mr() returning early
           makes mouse_rd() read 0x60 before a byte is there and return
           the previous one, so a three-byte movement packet is parsed
           one byte out of phase -- a pointer that jumps. */
        reset_log();
        qpush(0xFA);                  /* ACK for the reset        */
        qpush(0xAA);                  /* BAT result               */
        qpush(0x00);                  /* device id                */
        for(int i = 0; i < 12; i++) qpush(0xFA);   /* later ACKs  */
        mouse_init();
        ck("mouse_init talked to the controller", logn > 0, logn);
        ck("it did not run away",                 logn < LOGMAX, logn);
        int bad = delay_violations(1);
        ck("and every status read is preceded by a delay", bad == 0, bad);
    }

    head("the pointer can reach the whole screen, not just 80x25");
    {
        /* These were 79 and 24 written out, so on any larger grid the
           pointer stopped partway across the screen. */
        reset_log();
        sim_cols = 90; sim_rows = 60;
        mouse_init();
        /* Drive a long way right and down: 40 packets of +100,+100. */
        for(int i = 0; i < 40; i++){
            mouse_feed(0x08);          /* byte 0: always-1 bit, no sign */
            mouse_feed(100);           /* dx */
            mouse_feed(100);           /* dy, positive = up in PS/2 */
        }
        int mx = -1, my = -1, mb = 0;
        mouse_get(&mx, &my, &mb);
        ck("x can exceed 79 on a 90-column grid", mx > 79, mx);
        ck("x is clamped to the real last column", mx <= 89, mx);
        ck("y stays inside the real grid", my >= 0 && my <= 59, my);

        /* And back the other way, to the real origin. */
        for(int i = 0; i < 80; i++){
            mouse_feed(0x08 | 0x30);   /* both sign bits: dx,dy negative */
            mouse_feed(156);           /* -100 */
            mouse_feed(156);
        }
        mouse_get(&mx, &my, &mb);
        ck("and clamps at zero rather than going negative",
           mx >= 0 && my >= 0, mx);
        sim_cols = 80; sim_rows = 25;
    }

    /* ── v0.8.9.6: SPEED IS A FRACTION OF THE SCREEN ────────────────
       Four counts per CELL made a 240-column grid three times slower
       to cross than 80x25 -- "heel sloom" on the HP at high
       resolution -- and moved the pointer twice as many pixels down as
       across, because a cell is twice as tall as it is wide. */
    {
        int mx, my, mb;
        /* Small reports, five counts each, so the acceleration (which
           starts above six) is not what is being measured. */
#define FEED(dx, dy, n) for(int i_ = 0; i_ < (n); i_++){ \
            mouse_feed((uint8_t)(0x08 | ((dx) < 0 ? 0x10 : 0) | ((dy) > 0 ? 0x20 : 0))); \
            mouse_feed((uint8_t)((dx) & 0xFF)); \
            mouse_feed((uint8_t)((-(dy)) & 0xFF)); }
        /* Home to the top-left: clamped there whatever the grid. */
#define HOME() FEED(-5, -5, 400)

        head("the same hand movement crosses the same share of any grid");
        sim_cols = 80;  sim_rows = 25; sim_cw = 8; sim_ch = 16;
        HOME(); FEED(5, 0, 32);                     /* 160 counts */
        mouse_get(&mx, &my, &mb);
        ck("80 columns: half the span is half the width", mx == 40, mx);
        sim_cols = 240; sim_rows = 67; sim_cw = 8; sim_ch = 16;
        HOME(); FEED(5, 0, 32);
        mouse_get(&mx, &my, &mb);
        ck("240 columns: the same movement is still half the width",
           mx == 120, mx);
        sim_cols = 160; sim_rows = 50; sim_cw = 8; sim_ch = 16;
        HOME(); FEED(5, 0, 64);                     /* 320 counts */
        mouse_get(&mx, &my, &mb);
        ck("160 columns: the whole span reaches the last column",
           mx == 159, mx);

        head("a count is the same number of pixels on both axes");
        /* 80x25 on 1280x800: 16x32 cells. 40 counts across is 10
           cells = 160 px; 40 counts down must be 160 px = 5 rows. */
        sim_cols = 80; sim_rows = 25; sim_cw = 16; sim_ch = 32;
        HOME(); FEED(5, 5, 8);
        mouse_get(&mx, &my, &mb);
        ck("40 counts across is 10 columns (160 px)", mx == 10, mx);
        ck("40 counts down is 5 rows (160 px), not 10", my == 5, my);

        head("left and right are mirror images");
        sim_cols = 80; sim_rows = 25; sim_cw = 8; sim_ch = 16;
        HOME(); FEED(5, 0, 32);                     /* to column 40 */
        FEED(3, 0, 37);                             /* 111 counts right */
        FEED(-3, 0, 37);                            /* 111 counts left  */
        mouse_get(&mx, &my, &mb);
        ck("there and back again ends where it started", mx == 40, mx);
        /* The v0.7.17 bug in a new place would show here: a fast
           movement left must be boosted exactly as much as one right. */
        FEED(20, 0, 2);                             /* +20 columns */
        FEED(-20, 0, 2);
        mouse_get(&mx, &my, &mb);
        ck("and so does a fast one, acceleration and all", mx == 40, mx);
        head("pushing against an edge leaves no debt behind");
        HOME();
        FEED(-5, -5, 50);                           /* keep pushing */
        FEED(5, 0, 1);                              /* 5 counts back */
        FEED(5, 0, 1);                              /* 10: 2.5 cells */
        mouse_get(&mx, &my, &mb);
        ck("the first movement off the edge moves at once", mx == 2, mx);
#undef HOME
#undef FEED
        sim_cols = 80; sim_rows = 25; sim_cw = 8; sim_ch = 16;
    }

    head("a silent controller does not hang the mouse either");
    {
        reset_log();
        sim_dead = 1;                 /* no PS/2 port at all, like the HP */
        mouse_init();
        ck("mouse_init returned", 1, 0);
        ck("and within a sane boot-time budget", logn < BUDGET, logn);
    }

    /* ════════════════════════════════════════════════════════════════
       v0.8.8.1: "IS THERE A POINTING DEVICE" IS ONE QUESTION WITH TWO
       SOURCES

       It was one flag set by the PS/2 reset acknowledging, and every
       consumer in the tree gated on it -- so on a laptop whose
       trackpad does not answer, a working USB mouse was tracked and
       invisible. Reported from the HP on 31 October as "both dead",
       which is the clue: one cause, not two.

       The same mistake net/net.c made with `nic_type` and four screens
       made with the disk bus label. What is asserted here is that
       NEITHER SOURCE CAN ANSWER ALONE.
       ════════════════════════════════════════════════════════════════ */
    head("a pointer is ready if EITHER driver has one");
    {
        /* The HP: the aux port never acknowledges. */
        sim_dead = 1;
        mouse_usb_forget_all();
        mouse_init();
        ck("a trackpad that does not ACK leaves PS/2 not ready",
           mouse_ps2_ready() == 0, mouse_ps2_ready());
        ck("  ... and with no USB mouse either, no pointer",
           mouse_is_ready() == 0, mouse_is_ready());

        /* THE BUG, in one check: a USB mouse arrives and the cursor
           has to appear. Before v0.8.8.1 this stayed 0 for ever. */
        mouse_usb_attach();
        ck("a USB mouse alone makes a pointer ready",
           mouse_is_ready() == 1, mouse_is_ready());
        ck("  ... without claiming the PS/2 port answered",
           mouse_ps2_ready() == 0, mouse_ps2_ready());
        ck("  ... and it is counted, not flagged",
           mouse_usb_count() == 1, mouse_usb_count());

        /* Two mice is a thing people do, and unplugging one must not
           take the pointer away. A flag could not express that. */
        mouse_usb_attach();
        ck("a second USB mouse counts", mouse_usb_count() == 2,
           mouse_usb_count());

        /* usb_hid_init() resets its table and re-walks the bus, so the
           count must reset with it -- otherwise a mouse that has been
           unplugged keeps claiming a cursor. Same reason
           usbnet_release() runs before usb_enumerate(). */
        mouse_usb_forget_all();
        ck("a rescan that finds nothing takes the pointer away",
           mouse_is_ready() == 0, mouse_is_ready());
        ck("  ... and the count really went to zero",
           mouse_usb_count() == 0, mouse_usb_count());

        /* And the other direction: a PS/2 mouse that DOES answer is a
           pointer with no USB mouse anywhere.  The simulated controller
           ANSWERS now rather than having its answers queued ahead of
           it -- see the note in outb(). */
        sim_dead = 0;
        qh = qt = 0;
        sim_mouse_present = 1;
        mouse_init();
        ck("a trackpad that answers makes a pointer ready on its own",
           mouse_is_ready() == 1, mouse_is_ready());
        ck("  ... and the USB count is still zero",
           mouse_usb_count() == 0, mouse_usb_count());
        sim_mouse_present = 0;
        qh = qt = 0;                  /* leave no bytes for the next test */
    }

    /* ── v0.8.8: THE IDLE LOOP POLLS ─────────────────────────────────
       keyboard_waitchar() is the kernel's one idle point and the only
       driver file tools/check_kpoll_sites.py lets call kernel_poll().
       The call has no return value anybody reads and no screen, so
       without a counting stub there is nothing in the tree that could
       notice it being deleted -- and "nothing in this kernel runs on a
       timer" would quietly be true again.

       The wait loop is driven here by feeding it a key AFTER a few
       turns, so it has to go round at least once. */
    /* ── v0.8.8.1: AND THE NON-BLOCKING READ POLLS TOO ───────────────
       v0.8.8 polled only from keyboard_waitchar(). Every interactive
       loop in shell/desktop.c uses the NON-blocking keyboard_getchar()
       so it can service the mouse at the same time, so on the desktop
       the bottom half never ran at all. This is the check that would
       have caught it. */
    head("the non-blocking read polls the bottom half");
    {
        long before = kpollguard_poll_count();
        int c = keyboard_getchar();          /* nothing queued: -1 */
        ck("it returned 'no key'", c == -1, c);
        ck("and still polled -- this is the desktop's path",
           kpollguard_poll_count() > before,
           kpollguard_poll_count() - before);
    }
    /* And haschar deliberately does NOT: net/net_tick.c calls it from
       inside kernel_poll(), and recursing there would make `kpoll`'s
       "already polling" counter climb for something that is not a
       bug. */
    head("...but the predicate does not, on purpose");
    {
        long before = kpollguard_poll_count();
        (void)keyboard_haschar();
        ck("keyboard_haschar() has no side effect that large",
           kpollguard_poll_count() == before,
           kpollguard_poll_count() - before);
    }

    head("the keyboard's idle wait polls the bottom half");
    {
        long before = kpollguard_poll_count();
        /* Queue a key so waitchar returns rather than spinning for
           ever; the loop still runs its body first because the queue
           is read at the top. */
        sim_dead = 0;
        sim_feed_code  = 0x1E;                /* 'a' make code */
        sim_feed_after = 6;                   /* arrives DURING the wait */
        int c = keyboard_waitchar();
        ck("the wait returned a key", c == 'a', c);
        ck("and kernel_poll() was reached at least once",
           kpollguard_poll_count() > before,
           kpollguard_poll_count() - before);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
