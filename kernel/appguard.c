/* ════════════════════════════════════════════════════════════════════
   kernel/appguard.c -- catching an app's fault and getting back.
   v0.8.8.4.

   include/appguard.h says what this does and, more importantly, what
   it does not: it is fault RECOVERY, not memory protection.

   ── HOW THE RETURN ACTUALLY HAPPENS ─────────────────────────────────

   A minimal setjmp/longjmp pair, written out rather than pulled in,
   because this kernel has no libc and the whole thing is six moves.

   appguard_setjmp() saves the return address, esp, ebp and the
   callee-saved registers -- EXPANDED INTO desktop_run_app(), whose
   frame stays on the stack for as long as the app does. It cannot be
   called from a helper that returns first; appguard.h records at
   length what happened when it was.

   isr_handler() then calls appguard_fault(), which jumps.

   THE ESP AT THAT MOMENT IS WRONG and it does not matter: `iret` in
   ring 0 does not reload esp, so appguard_jump lands on the exception
   stack. Its first instruction loads esp from the saved buffer, which
   is the desktop's stack, and from there it is an ordinary return.
   That is the only reason this can be done without a task gate.

   `iret` DOES reload eflags from the frame, so the interrupt flag
   comes back as the app had it. If the app faulted inside a critical
   section with interrupts off, the recovery would resume with them
   off -- so appguard_jump sets them explicitly. A desktop that cannot
   receive IRQ0 is a desktop that looks hung, which is the thing this
   file exists to prevent.

   ── WHY THE JUMP BUFFER IS NOT A STACK OF THEM ──────────────────────

   One guard, not nested guards. An app that launches another app
   would overwrite the outer save, and the recovery would return to a
   frame that is no longer there. appguard_enter() REFUSES to arm a
   second time and says so, rather than quietly replacing the first --
   and the desktop is the only caller, so it cannot happen today. It
   is checked because "it cannot happen today" is the sentence that
   precedes it happening.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/appguard.h"
#include "../include/klog.h"

/* 0:eip 1:esp 2:ebp 3:ebx 4:esi 5:edi
   NOT static: the two assembly stubs below reference it by symbol
   name, and a file-scope static has no symbol for them to reach.
   Declared nowhere else, so it is private in practice -- the
   compiler just cannot be told that and still emit the label. */
uint32_t appguard_jb[6];
static int         g_armed = 0;
static const char *g_name = "(none)";
static uint32_t    g_exc = 0, g_eip = 0;
static uint32_t    g_crashes = 0;

static const char *exc_names[] = {
    "divide by zero",        "debug",                "NMI",
    "breakpoint",            "overflow",             "bound range",
    "invalid opcode",        "device not available", "double fault",
    "coprocessor overrun",   "invalid TSS",          "segment not present",
    "stack fault",           "general protection",   "page fault",
    "reserved"
};

/* ── the save, in assembly, because it is about esp ─────────────────
   A C function cannot save its own caller's stack pointer usefully:
   by the time it runs, its own frame is on top and the compiler may
   have moved things. This is what setjmp is for and this is all
   setjmp is. */
__asm__(
".globl appguard_setjmp\n"
"appguard_setjmp:\n"
"   movl  (%esp), %eax\n"          /* the return address            */
"   movl  %eax, appguard_jb+0\n"
"   movl  %esp, appguard_jb+4\n"
"   movl  %ebp, appguard_jb+8\n"
"   movl  %ebx, appguard_jb+12\n"
"   movl  %esi, appguard_jb+16\n"
"   movl  %edi, appguard_jb+20\n"
"   xorl  %eax, %eax\n"            /* 0: this is the first time     */
"   ret\n"
);

/* ── and the jump back ──────────────────────────────────────────────
   Entered from the exception's `iret` with the WRONG esp, which the
   first instruction fixes. Sets the interrupt flag because the
   faulting context may have had it clear. */
__asm__(
".globl appguard_jump\n"
"appguard_jump:\n"
"   movl  appguard_jb+4,  %esp\n"
"   movl  appguard_jb+8,  %ebp\n"
"   movl  appguard_jb+12, %ebx\n"
"   movl  appguard_jb+16, %esi\n"
"   movl  appguard_jb+20, %edi\n"
"   movl  appguard_jb+0,  %eax\n"
"   movl  %eax, (%esp)\n"          /* put the return address back   */
"   sti\n"
"   movl  $1, %eax\n"              /* 1: we came back from a fault  */
"   ret\n"
);

extern void appguard_jump(void) __attribute__((noreturn));

/* Only the flag and the name. THE SAVE IS NOT HERE and must not be:
   see the long note in include/appguard.h for what happened when it
   was. appguard_enter() is a macro so that appguard_setjmp() runs in
   desktop_run_app()'s frame, which is still on the stack while the app
   is running -- this function's frame is not. */
int appguard_arm(const char *name)
{
    if(g_armed){
        /* Refused rather than replaced. Returning 0 means no save is
           made, so the OUTER guard stays armed and pointing at a frame
           that is still live -- a fault then still lands somewhere
           real. Replacing it would leave the recovery returning into
           the inner app's frame after that app had gone. */
        klog(KLOG_WARN, "appguard: already armed for %s, refusing to "
             "nest %s", g_name, name ? name : "(null)");
        return 0;
    }
    g_name  = name ? name : "(unnamed)";
    g_armed = 1;
    return 1;
}

void appguard_leave(void)
{
    g_armed = 0;
    g_name  = "(none)";
}

int         appguard_armed(void)       { return g_armed; }
const char *appguard_name(void)        { return g_name; }
uint32_t    appguard_exception(void)   { return g_exc; }
uint32_t    appguard_eip(void)         { return g_eip; }
uint32_t    appguard_crash_count(void) { return g_crashes; }

const char *appguard_exception_name(void)
{
    return (g_exc < 16) ? exc_names[g_exc] : "unknown exception";
}

void appguard_fault(uint32_t exc_no, uint32_t eip)
{
    g_exc = exc_no;
    g_eip = eip;
    g_crashes++;

    /* DISARM BEFORE JUMPING. A fault inside the recovery path with the
       guard still armed jumps to the same place again, for ever, and a
       machine in that state looks identical to one that has hung --
       which is exactly the symptom this file was written to remove. */
    g_armed = 0;

    /* The log is written here rather than by the desktop, because the
       desktop may itself be the thing that is broken and `dmesg` after
       a reboot is the only record either way. */
    klog(KLOG_WARN, "appguard: %s faulted -- %s at EIP %8x",
         g_name, appguard_exception_name(), eip);

    appguard_jump();
}
