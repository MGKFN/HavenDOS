#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/kpoll.h -- periodic work, as a BOTTOM HALF

   v0.8.8 "Helios".  The phrase this file exists to delete appears in
   six places in HANDOFF.md and three in the source:

       "nothing in this kernel runs on a timer"

   It was never quite true.  IRQ0 has been unmasked since there was an
   IDT (kernel/idt.c writes 0xFC to port 0x21), irq_handler() has fired
   at 100 Hz, and pit_tick() has incremented a counter and done nothing
   else.  The timer was running the whole time; nothing was hung off it.

   ── WHY NOT JUST CALL THE WORK FROM THE IRQ ─────────────────────────

   Because every layer in this tree shares module-level buffers and not
   one of them is reentrant:

       usb/xhci_hc.c    g_bounce, g_inctx
       usb/core.c       g_cfgbuf  -- its own comment calls it borrowed
       usb/ethernet.c   g_rx, g_tx, g_ctl
       net/ethernet.c   eth_rx_buf, and a static frame inside eth_send()
       fs/fat16.c, fat32.c, esp.c   their sector buffers

   An IRQ0 that called net_poll() while the shell was part-way through
   usb_control() would corrupt that transfer.  The symptom is a
   once-an-hour impossibility, and v0.8.7.5's shared event ring is this
   tree's record of what those cost: one unanswered DHCP write broke
   descriptor reads on a webcam, and it took a release to find.

   So the split is the ordinary one.  kpoll_tick() runs in interrupt
   context, counts, and sets due flags.  kernel_poll() runs the work,
   from points chosen deliberately: the shell's idle loop, and
   kpoll_sleep_ms().

   ── AND THE RULE THAT KEEPS IT THAT WAY IS MECHANICAL ───────────────

   A design that depends on "do not call kernel_poll() from a driver"
   is a comment.  tools/check_kpoll_sites.py is the rule:

     - kernel_poll() may be called only from the files it names.
     - kpoll_sleep_ms() may not appear in drivers/, usb/, net/ or fs/
       at all -- sleep_ms() is the driver-safe wait and stays exactly
       as it was, polling the PIT and nothing else.
     - the subsystem entry points it names must bracket themselves
       with kpoll_enter()/kpoll_leave(), so that a kernel_poll()
       reached from inside one of them refuses instead of reentering.

   The third is the belt to the first's braces, and it is not
   redundant: fs/fat16.c's writable BORROW can be open across a
   keyboard wait, so "the shell is idle" and "no subsystem is
   mid-operation" are genuinely different statements.

   ── EVERY REFUSAL HAS A NAME ────────────────────────────────────────

   kernel_poll() returns a KPOLL_* code, never a bare zero.  0.8.7.7
   spent a release on what a nameless refusal costs: remove a length
   guard from mux_recv() and it returns len-hdr, which for a 4-byte
   length is -4, which is USB_ENODEV, "nothing on the port" -- and the
   next person goes to check whether the phone is plugged in.  "The
   poll did not run" has four causes here and they are four different
   bugs.
   ════════════════════════════════════════════════════════════════════ */

/* What kernel_poll() did.  KPOLL_RAN covers "nothing was due", which
   is the overwhelmingly common answer and is not a refusal. */
#define KPOLL_RAN       0
#define KPOLL_ENOINIT   1   /* kpoll_init() has not run                 */
#define KPOLL_EREENTER  2   /* already inside kernel_poll()              */
#define KPOLL_EBUSY     3   /* a kpoll_enter() section is open           */
#define KPOLL_EOFF      4   /* switched off -- safe mode, or `kpoll off` */
#define KPOLL_REASONS   5

#define KPOLL_MAX_CLIENTS 8

/* The tick rate the due arithmetic assumes, and the one pit_init() is
   called with.  Named here because an interval in MILLISECONDS has to
   be converted to ticks somewhere, and a second copy of the number is
   how v0.7.5's pit_us() came to disagree with pit_init(). */
#define KPOLL_HZ 100

void kpoll_init(void);

/* Register periodic work.  `name` must be a string literal with
   static lifetime -- it is kept, not copied, and `kpoll` prints it.
   `interval_ms` is rounded UP to a whole tick, minimum one tick, so
   an interval of 1 ms does not silently become "every tick of a clock
   that cannot express it".  Returns the client index, or -1. */
int  kpoll_register(const char *name, uint32_t interval_ms, void (*fn)(void));

/* ── INTERRUPT CONTEXT.  Called from irq_handler() for IRQ0 and from
   nowhere else.  Counts, sets due flags, returns.  It must not call
   anything: in particular not get_ticks(), because pit_us() mutates
   last_pit_val and us_frac and is itself non-reentrant -- an IRQ0 that
   read the polled clock would corrupt the clock it was trying to
   read. */
void kpoll_tick(void);

/* ── TASK CONTEXT.  Runs whatever is due.  See the refusal codes. */
int  kernel_poll(void);

/* A shared-buffer section.  Nests. */
void kpoll_enter(void);
void kpoll_leave(void);
int  kpoll_depth(void);

/* A sleep that polls.  IDLE PATHS ONLY -- kernel/pit.c's sleep_ms() is
   the one a driver calls and it does not poll, which is the whole
   reason this is a separate function rather than a flag. */
void kpoll_sleep_ms(uint32_t ms);

/* ── what `kpoll` prints, and what the suite reads ─────────────────── */
uint32_t    kpoll_ticks(void);          /* ticks since kpoll_init()      */
int         kpoll_client_count(void);
const char *kpoll_client_name(int i);
uint32_t    kpoll_client_interval(int i);   /* in ticks                  */
uint32_t    kpoll_client_runs(int i);
uint32_t    kpoll_client_late(int i);   /* became due again before it ran */
uint32_t    kpoll_refusals(int reason);
const char *kpoll_reason_name(int reason);
void        kpoll_set_enabled(int on);
int         kpoll_enabled(void);
