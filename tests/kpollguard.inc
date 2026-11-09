/* ════════════════════════════════════════════════════════════════════
   tests/kpollguard.inc -- kpoll_enter()/kpoll_leave() for a suite that
                           links a guarded module but is not about
                           periodic work.

   v0.8.8 brackets six subsystem entry points -- usb_control(),
   usb_enumerate(), blk_read(), blk_write(), eth_send() and eth_poll()
   -- so that kernel_poll() refuses while one of them is open.  See
   include/kpoll.h and tools/check_kpoll_guard.py.

   Thirteen existing suites compile one of those modules.  None of them
   is about the tick, and linking kernel/kpoll.c into each would drag
   in a polling sleep that wants get_ticks() and sleep_ms() -- which
   several of those suites already define, differently, for their own
   purposes.

   ── WHY IT COUNTS INSTEAD OF DOING NOTHING ──────────────────────────

   An empty stub would compile and prove nothing.  This tree's name for
   that is the mock-that-cannot-refuse, and include/usb.h has warned
   about it since v0.8.5; tests/test_usbnet.c's monument to it accepted
   a transfer of any length, so a driver that could not move a single
   frame passed 110 checks.

   So the depth is real here.  A suite including this can assert
   kpollguard_depth() == 0 after the work it drives, and an unbalanced
   bracket in the module under test shows up as a number rather than as
   nothing at all.  It is cheap enough that there is no reason not to:
   the whole mechanism is one int.

   This is NOT a copy of kernel/kpoll.c's logic in any sense that can
   drift -- the two functions there are `g_depth++` and `if(g_depth>0)
   g_depth--`, and the real pair, the real refusal codes and the real
   critical section are all under test in tests/test_kpoll.c, which
   compiles kernel/kpoll.c itself.
   ════════════════════════════════════════════════════════════════════ */
#ifndef KPOLLGUARD_INC
#define KPOLLGUARD_INC

static int kpollguard_d;
static int kpollguard_max;
static long kpollguard_enters;

void kpoll_enter(void){
    kpollguard_enters++;
    if(++kpollguard_d > kpollguard_max) kpollguard_max = kpollguard_d;
}
void kpoll_leave(void){ if(kpollguard_d > 0) kpollguard_d--; }
int  kpoll_depth(void){ return kpollguard_d; }

/* ── and kernel_poll(), for a suite that compiles a POLL SITE ────────
   drivers/keyboard.c calls it from keyboard_waitchar()'s wait loop --
   the one idle point in the kernel, and the only file besides
   kernel/kpoll.c that tools/check_kpoll_sites.py permits to call it.
   A suite that compiles that driver needs the symbol.

   Counting, again, and for a better reason than symmetry: a suite can
   now assert that the idle loop POLLS AT ALL. That claim has no screen
   and no return value -- the loop calls it and ignores the result --
   so a stub that counted nothing would leave the one thing worth
   checking about the call site unobservable. */
static long kpollguard_polls;
int kernel_poll(void){ kpollguard_polls++; return 0; }

/* For a suite that wants to assert the bracketing it just exercised. */
static int  kpollguard_depth(void){ return kpollguard_d; }
static long kpollguard_count(void){ return kpollguard_enters; }
static long kpollguard_poll_count(void){ return kpollguard_polls; }

#endif
