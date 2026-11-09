#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/appguard.h -- an app crash does not take the system with it.
   v0.8.8.4.

   WHAT THIS IS

   Before this, every CPU exception went to isr_handler(), which painted
   KERNEL PANIC and halted. A divide by zero in the calculator, a null
   dereference in the editor, a bad opcode anywhere: the machine
   stopped and the only way out was the power button.

   Now the desktop arms a guard before it calls an app and disarms it
   after. If a fault happens while the guard is armed, isr_handler()
   does not panic: it restores the desktop's own stack and returns
   there, and the desktop says which app stopped and keeps running.

   ── WHAT THIS IS NOT, AND THE DISTINCTION MATTERS ───────────────────

   THIS IS NOT MEMORY PROTECTION. There is no per-task address space in
   this kernel -- one page directory, one flat 4 GB mapping, everything
   in ring 0 -- so an app can write anywhere it likes and nothing stops
   it. What this catches is the fault AFTER the damage:

     caught        a divide by zero, a jump through a null or wild
                   pointer, an invalid opcode, a read or write to an
                   unmapped page, a stack that ran off its end
     NOT caught    a write to kernel memory that SUCCEEDS. If an app
                   corrupts the heap, the VFS table or another app's
                   state and then returns normally, the guard never
                   fires and the damage is already done. The machine
                   carries on and misbehaves later, somewhere else.

   So this turns most crashes from "reboot the machine" into "that app
   stopped", and it is not a sandbox. Real isolation needs a page
   directory per task and user-mode segments, which is a release of its
   own and a large one.

   ── AND ONE THING IT DELIBERATELY DOES NOT TRY ──────────────────────

   A double fault is not recoverable here. #DF means the CPU could not
   deliver an earlier exception -- usually because the stack is gone --
   and the handler that would run has no stack to run on either. It
   still panics, because a recovery that needs a working stack cannot
   be the answer to not having one. A task gate with its own stack
   would fix that, and until then the panic is honest.

   ── THE STATE A RECOVERED FAULT LEAVES BEHIND ───────────────────────

   The app was in the middle of something. Anything it had half-done
   stays half-done: an open file handle is still open, a borrowed
   filesystem driver is still borrowed. appguard_recover() is where the
   things that MUST be put back go, and the list is deliberately short
   and explicit rather than "clean up everything", because a cleanup
   that guesses is how a recovery corrupts more than the crash did.
   ════════════════════════════════════════════════════════════════════ */

/* ── WHY appguard_enter IS A MACRO AND NOT A FUNCTION ────────────────

   FOUND BY BOOTING IT, in harness/appguard_boot.py. The first version
   of this was an ordinary function:

       int appguard_enter(const char *name) {
           ...
           if(appguard_setjmp()) return 1;
           return 0;
       }

   It armed correctly, the fault was caught, the stack was restored --
   and the machine then panicked with "Invalid Opcode at EIP 0x7".

   The reason is that appguard_enter RETURNS before the app runs. The
   stack pointer it saved points into its own frame, and the moment it
   returns, that frame is dead memory. desktop_run_app() then calls the
   app, whose frame is allocated at the same addresses -- so by the
   time the app faults, the saved frame has been written all over.

   Restoring esp to it and returning there lands in the tail of
   appguard_enter, whose epilogue pops the callee-saved registers back
   off the stack -- from slots the app has since overwritten -- and
   then returns to whatever the first four of those bytes happen to be.
   Here that was 0x7.

   Restoring ebx/esi/edi/ebp from the jump buffer does not help: the
   epilogue pops over them again straight afterwards.

   So setjmp has to run in a frame that is STILL ALIVE while the app
   runs, which means the caller's. That is the same reason the C
   standard requires setjmp itself to be a macro and makes calling it
   through a function pointer undefined. desktop_run_app() stays on the
   stack for as long as the app does, so the save belongs there.

   ── WHAT THE TWO HALVES DO ──────────────────────────────────────────

   appguard_arm()    records the name and sets the armed flag. Returns
                     1 if it armed, 0 if it REFUSED because a guard was
                     already armed -- in which case no save is made and
                     the outer guard is left exactly as it was, so a
                     fault still returns somewhere real.
   appguard_setjmp() the save itself, which must be expanded into the
                     caller.

   Returns 0 the first time. Returns 1 when a fault has brought control
   back here, exactly like setjmp, and the caller must then NOT call
   the app again. */
int  appguard_arm(const char *name);
int  appguard_setjmp(void);

#define appguard_enter(name) \
    (appguard_arm(name) ? appguard_setjmp() : 0)

/* Disarm. Must be called on the normal path, or the next fault
   anywhere in the system is attributed to an app that already
   finished and jumps into a stack frame that no longer exists. */
void appguard_leave(void);

/* Is a guard armed right now? isr_handler() asks before it panics. */
int  appguard_armed(void);

/* The app that was running, for the message. Valid after
   appguard_enter() returns 1. */
const char *appguard_name(void);

/* Which exception, and where. Valid after appguard_enter() returns 1. */
uint32_t    appguard_exception(void);
uint32_t    appguard_eip(void);
const char *appguard_exception_name(void);

/* How many times anything has been recovered this boot. A machine that
   is recovering repeatedly is a machine with a real problem, and the
   desktop says so after the third. */
uint32_t    appguard_crash_count(void);

/* Called from isr_handler(), which has already decided the guard is
   armed. Does not return. */
void appguard_fault(uint32_t exc_no, uint32_t eip) __attribute__((noreturn));
