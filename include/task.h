#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/task.h -- cooperative tasks.  v0.9.6.

   WHAT THIS IS

   Until 0.9.6 HavenDOS ran one program at a time. The desktop CALLED
   an app, and the app owned the machine until it returned. `ps` said
   so, and so did the System app. Kyan asked for both halves of the
   0.8.x promise:

     - tasks that switch when an app waits for a key or yields, so
       work can carry on in the background;
     - switching between open apps with Alt+Tab, each keeping its
       state.

   So each app now runs as a TASK: its own stack, its own console,
   its own crash guard. One task is in FRONT -- it owns the screen,
   the keyboard and the mouse buttons. The others are behind it, and
   each one is either PAUSED (the default: it stops at its next
   keyboard read and waits to be brought back) or RUNNING (it keeps
   going and draws into its own console, which nobody sees until it
   comes to the front).

   ── WHY COOPERATIVE, AND WHERE A SWITCH CAN HAPPEN ──────────────────

   Preemption -- switching from IRQ0 -- is out, for the reason
   include/kpoll.h gives for not doing WORK from IRQ0: every layer in
   this tree keeps module-level buffers and none of them is
   reentrant. A task switched out half way through usb_control()
   while another task starts one corrupts both transfers.

   So a switch happens only at the points where kernel_poll() is
   ALREADY allowed to run, and is refused for the same reasons:

     keyboard_getchar()    every interactive loop in the system reads
                           the keyboard; this is where kernel_poll()
                           has run since 0.8.8.1
     kpoll_sleep_ms()      the idle wait; already polls, and is
                           already refused in drivers/, usb/, net/ and
                           fs/ by tools/check_kpoll_sites.py

   and task_yield() refuses -- returns a named code, switches nothing
   -- when kpoll_depth() > 0 (a driver holds a shared buffer), when
   kernel_poll() is running, or inside task_lock(). That is the whole
   argument: a switch is exactly as safe as the polling that already
   happens at the same instruction, because it is refused for exactly
   the same reasons. tools/check_task_sites.py keeps task_yield() out
   of drivers/, usb/, net/ and fs/ the way the kpoll check keeps
   kpoll_sleep_ms() out.

   sleep_ms() does NOT switch and must not. It is the driver-safe
   wait. An app that wants to keep running in the background waits
   with kpoll_sleep_ms() instead -- see the Stopwatch and the Music
   Player.

   ── WHAT A TASK HAS OF ITS OWN ──────────────────────────────────────

     a stack         TASK_STACK bytes from the kernel heap, with a
                     canary at the bottom that is checked on every
                     switch. There is no guard page -- paging is not on
                     on every machine -- so an overflow is noticed, not
                     prevented. Noticed loudly: the task is stopped and
                     the log says which.
     a console       its own cell buffer, cursor, colour, viewport and
                     frame depth (vga_ctx_t). Only the task in front
                     paints the screen; the others draw into their own
                     buffers. Bringing a task to the front repaints
                     from its buffer, so an app comes back exactly as
                     it was left, with no redraw of its own.
     a crash guard   appguard's jump buffer, so a fault in one task
                     stops that task and nothing else.

   What it does NOT have, and the distinction is the same one
   include/appguard.h draws: an address space. Every task can still
   write anywhere. This is task switching, not protection.

   ── TASK 0 ──────────────────────────────────────────────────────────

   Slot 0 is the context the kernel booted on -- the desktop, or the
   shell in Shell Mode. It has the boot stack and the boot console and
   is never freed. Everything else is spawned.

   ── THE FRONT, AND WHO GETS IT BACK ─────────────────────────────────

   When a task in front ends, the front goes back to the task that
   opened it if that is still alive, and to slot 0 if not. The person
   always lands somewhere that exists.

   task_wait_front() is how an opener waits: the desktop calls it
   after opening an app, and it returns when the desktop is in front
   again -- because the app finished, or because somebody switched
   back to the desktop with the app still open. desktop_run_app()
   therefore still "returns when the desktop gets the screen back",
   which is what every one of its callers was written against.
   ════════════════════════════════════════════════════════════════════ */

#define TASK_MAX        8          /* slot 0 included                   */
#define TASK_STACK      (96u*1024u)
#define TASK_NAME       24
#define TASK_CANARY     0x7A5CA9E5u
#define TASK_CANARY_N   16         /* words of canary at the stack base */

/* States. A DEAD task is still in its slot until somebody who is not
   running on its stack frees it -- see task_reap(). */
#define TS_FREE   0
#define TS_READY  1    /* runnable                                       */
#define TS_WAIT   2    /* waiting to be in front; runnable only then     */
#define TS_DEAD   3

/* What a task does when it is not in front. */
#define TP_PAUSE  0    /* stop at the next keyboard read until in front  */
#define TP_RUN    1    /* keep running; keyboard reads return "no key"   */

/* What task_yield() did. TY_RAN and TY_ALONE are not refusals. */
#define TY_RAN      0  /* another task ran and we are back               */
#define TY_ALONE    1  /* nothing else was runnable                      */
#define TY_ENOINIT  2  /* task_init() has not run                        */
#define TY_ELOCKED  3  /* inside task_lock()                             */
#define TY_EBUSY    4  /* kpoll_depth() > 0: a driver holds a buffer     */
#define TY_EPOLL    5  /* inside kernel_poll()                           */
#define TY_REASONS  6

void task_init(void);
int  task_ready(void);

/* Start a task. `entry(arg)` runs on a new stack; when it returns the
   task ends. The task is NOT brought to the front -- the caller does
   that with task_set_front() if it wants to. Returns the slot, or -1
   (no free slot, or no memory for the stack or the console). The
   caller must then run the work some other way: the desktop falls
   back to calling the app directly, exactly as before 0.9.6. */
int  task_spawn(const char *name, void (*entry)(void *), void *arg, int policy);

/* Give the other runnable tasks a turn. Returns a TY_* code. */
int  task_yield(void);

/* Block until this task is in front. Returns 1 if it is, 0 if a
   switch was refused on the way (the caller should carry on and try
   again later -- getchar returns "no key"). */
int  task_wait_front(void);

/* The current task ends. Does not return. */
void task_exit(void) __attribute__((noreturn));

/* End another task. Not slot 0 and not the caller. Its cleanup hook
   runs first, in the caller's context. Returns 0, or -1. */
int  task_kill(int slot);

/* Bring a task to the front. The switch to it happens at the caller's
   next yield; until then the caller keeps running, drawing into its
   own (now hidden) console. Returns 0, or -1. */
int  task_set_front(int slot);

/* Nothing comes forward while pinned -- the lock and login screens.
   Tasks behind keep running. Nests. */
void task_pin_front(void);
void task_unpin_front(void);
int  task_front_pinned(void);

/* No switching inside: the installer, update, uninstall. Nests. */
void task_lock(void);
void task_unlock(void);
int  task_locked(void);

/* Per-task settings for the CURRENT task. */
void task_set_policy(int policy);
int  task_policy(void);
void task_set_cleanup(void (*fn)(void));   /* run if it is killed */
void task_set_name(const char *name);

/* Who is who. */
int         task_current(void);
int         task_front(void);
int         task_is_front(void);           /* the current task      */
int         task_count(void);              /* live tasks, slot 0 in */
int         task_state(int slot);          /* TS_*                  */
int         task_slot_policy(int slot);
uint32_t    task_id(int slot);             /* the number `ps` shows */
const char *task_name(int slot);
int         task_opener(int slot);
uint32_t    task_switches(int slot);       /* times it was run      */
uint32_t    task_age_ticks(int slot);
uint32_t    task_stack_used(int slot);     /* high-water, bytes     */
uint32_t    task_stack_size(int slot);
uint32_t    task_front_seq(int slot);      /* MRU stamp, 0 = never  */
int         task_find_id(uint32_t id);     /* slot for an id, or -1 */
uint32_t    task_refusals(int reason);
const char *task_reason_name(int reason);
const char *task_state_name(int slot);     /* "in front", "running"...*/
int         task_running_behind(int slot); /* runnable, behind, TP_RUN */

/* The task switcher hook. drivers/keyboard.c calls it when Alt+Tab
   arrives; shell/taskswitch.c supplies it. `back` is 1 for
   Shift+Alt+Tab. */
void task_set_switcher(void (*fn)(int back));
void task_switcher_run(int back);
