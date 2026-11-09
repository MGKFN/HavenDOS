/* ════════════════════════════════════════════════════════════════════
   shell/crashcmd.c -- the `crash` command.  v0.8.8.4.

   THIS COMMAND EXISTS TO MAKE THE RECOVERY PATH TESTABLE.

   kernel/appguard.c catches a CPU exception raised inside an app and
   returns to the desktop instead of painting KERNEL PANIC. That is a
   piece of code which, by construction, only ever runs when something
   has already gone wrong -- so if it is never deliberately triggered
   it ships having never executed once.

   That is precisely the shape of the bug that cost v0.8.8.1 its
   Miller-Rabin: `for(r = 0; r < 1; r++)` sat in the tree passing every
   test, because no test ever asked for more than one round. A recovery
   path with no way to provoke it is worse, because the first time it
   runs is the first time it is needed.

   So: `crash` faults on purpose, in each of the ways an app can.

   ── WHY THIS IS NOT A HIDDEN COMMAND ────────────────────────────────

   It is in `help` and it tab-completes. A command that crashes the
   program it is typed into is alarming, and the way to handle that is
   to say so plainly before doing it, not to hide the command and hope.
   Each form prints what it is about to do and what should happen, so
   the screen says whether the result was the intended one.

   ── WHAT IT PROVES, AND WHAT IT CANNOT ──────────────────────────────

   Typed in the Shell app, the Shell was launched through
   desktop_run_app(), so the guard is armed and the fault should come
   back to the desktop with "Program stopped". That tests the whole
   path: the exception, isr_handler()'s hand-off, the stack switch, the
   return, and the desktop's redraw.

   Typed at the BOOT CONSOLE -- the shell before the desktop starts --
   nothing armed a guard, and it panics. That is not a bug and the
   command says so up front: there is no desktop to go back to, and a
   fake recovery that returned into a frame nobody saved would be the
   more dangerous answer.

   `crash df` is excluded from the guard on purpose. A double fault
   means the CPU could not deliver an earlier exception, usually
   because the stack has gone, and the handler that would recover has
   no stack to run on either. include/appguard.h says so; this is how
   you watch it be true.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/appguard.h"

static void up(const char *s){ vga_puts(s); }

static void putu(uint32_t v){
    char b[12]; int n=0;
    if(!v) b[n++]='0';
    while(v){ b[n++]=(char)('0'+v%10); v/=10; }
    while(n) vga_putchar(b[--n]);
}

/* ── the faults ──────────────────────────────────────────────────────
   Each is written so the compiler cannot fold it away. A divide by a
   literal zero is a compile error, and a store to a literal null is
   undefined behaviour the optimiser is entitled to delete -- so the
   values go through volatile. The point is to reach the CPU. */

static void fault_divide(void)
{
    volatile int zero = 0;
    volatile int n    = 1;
    n = n / zero;                 /* #DE, exception 0 */
    (void)n;
}

static void fault_null(void)
{
    volatile uint32_t *p = (volatile uint32_t *)0;
    *p = 0xDEAD;                  /* #PF, exception 14 -- page 0 is
                                     deliberately left unmapped */
}

static void fault_opcode(void)
{
    /* ud2: the one instruction the architecture guarantees will always
       raise #UD. Not a random byte sequence, which might decode into
       something that runs. */
    __asm__ __volatile__("ud2");
}

static void fault_wild(void)
{
    /* A call through a wild pointer, which is what a corrupted vtable
       or a freed callback actually looks like. It faults on
       INSTRUCTION FETCH rather than on a data access, which is a
       different path through the CPU than `crash null` takes.

       ── WHY NOT 0xF0000000, WHICH THIS USED TO BE ───────────────────

       FOUND BY BOOTING IT. The first version called 0xF0000000 on the
       reasoning that it is far above installed RAM. It did not fault.
       This kernel has ONE page directory with a FLAT 4 GB IDENTITY
       MAPPING -- include/appguard.h says so in the course of
       explaining why this is not a sandbox -- so 0xF0000000 is a
       perfectly valid linear address. The call went there, read
       whatever the chipset returns for unbacked physical memory, and
       executed it. What happened next was not a clean exception and
       not reproducible.

       An address being outside RAM does not make it unmapped. The
       first page IS unmapped, which is what makes `crash null` work,
       so a wild call belongs in it too -- not at 0, so that this is
       visibly a different vector from the null write rather than the
       same one spelled differently. */
    void (*fn)(void) = (void (*)(void))0x00000010u;
    fn();
}

static void fault_stack(void)
{
    /* ── THE ONE THE GUARD CANNOT SAVE YOU FROM ──────────────────────

       MEASURED, in harness/appguard_boot.py. This was written
       expecting a double fault, on the reasoning that once the stack
       is gone the CPU has nowhere to push an exception frame. What
       actually happens on this machine is worse and more instructive.

       The stack walks DOWN through mapped memory for a long way
       before it reaches anything unmapped, and everything it passes
       through is written with zeroes: the heap, the console's shadow
       buffer, whatever else is below it. By the time any fault
       arrives, the damage is done. The observed result is a screen of
       corrupted glyphs and a machine that stops responding -- no
       panic, no recovery window, nothing to read.

       That is precisely the row include/appguard.h calls NOT caught:

         "a write to kernel memory that SUCCEEDS. If an app corrupts
          the heap, the VFS table or another app's state and then
          returns normally, the guard never fires and the damage is
          already done."

       This is kept, not removed, because it is the only thing in the
       tree that demonstrates that limit instead of asserting it. It
       is labelled as what it is: a vector that requires a reboot. */
    __asm__ __volatile__(
        "1: pushl $0\n"
        "   jmp 1b\n");
}

static void explain_where(void)
{
    if(appguard_armed()){
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        up("  The app guard is armed for \"");
        up(appguard_name());
        up("\".\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("  This should close this app and return to the desktop,\n");
        up("  which will say the program stopped. The machine keeps\n");
        up("  running and 'dmesg' afterwards has the exception in it.\n");
    } else {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        up("  NO GUARD IS ARMED -- this will panic.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("  You are at the boot console, not in an app launched from\n");
        up("  the desktop, so there is no saved frame to return to.\n");
        up("  That is the intended behaviour, not a failure: start the\n");
        up("  desktop, open the Shell from it, and type this there to\n");
        up("  see the recovery instead.\n");
    }
    up("\n");
}

static void usage(void)
{
    up("crash -- fault on purpose, to test the app guard\n\n");
    up("  crash where     is a guard armed right now? faults nothing\n");
    up("  crash div       divide by zero          (#DE)\n");
    up("  crash null      write through a null    (#PF)\n");
    up("  crash opcode    an invalid instruction  (#UD)\n");
    up("  crash wild      call a wild pointer     (#PF)\n");
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    up("  crash stack     run the stack off its end\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    up("                  -- THIS ONE NEEDS A REBOOT. It overwrites\n");
    up("                     memory below the stack on its way down,\n");
    up("                     and the guard catches faults, not\n");
    up("                     damage. Measured: no panic, no recovery,\n");
    up("                     a corrupted screen and a wedged machine.\n");
    up("                     It is here because it is the only thing\n");
    up("                     that DEMONSTRATES that limit.\n\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    up("  The recovery path only ever runs when something has already\n");
    up("  gone wrong, so without this it would ship never having run.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    up("\n  recovered so far this boot: ");
    putu(appguard_crash_count());
    up("\n");
}

void cmd_crash(const char *arg1)
{
    if(!arg1 || !arg1[0]){ usage(); return; }

    /* `where` answers the question without faulting. It exists because
       the warning printed by a faulting form is immediately painted
       over by the panic screen it warned about -- so the only way to
       READ it is a form that stops there. */
    if(!strcmp(arg1,"where")){
        explain_where();
        up("  recovered so far this boot: ");
        putu(appguard_crash_count());
        up("\n");
        return;
    }

    if(!strcmp(arg1,"div")){
        up("Dividing by zero.\n\n"); explain_where(); fault_divide();
    } else if(!strcmp(arg1,"null")){
        up("Writing through a null pointer.\n\n"); explain_where(); fault_null();
    } else if(!strcmp(arg1,"opcode")){
        up("Executing ud2.\n\n"); explain_where(); fault_opcode();
    } else if(!strcmp(arg1,"wild")){
        up("Calling through 0xF0000000.\n\n"); explain_where(); fault_wild();
    } else if(!strcmp(arg1,"stack")){
        up("Running the stack off its end.\n\n");
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("  THE GUARD CANNOT CATCH THIS AND YOU WILL NEED TO REBOOT.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("  The stack overwrites memory on its way down, so the\n");
        up("  damage is done before any fault arrives. Expect a\n");
        up("  corrupted screen and a machine that stops answering --\n");
        up("  not a panic, and not a recovery.\n\n");
        fault_stack();
    } else {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        up("crash: no such fault: "); up(arg1); up("\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        usage();
        return;
    }

    /* Reached only if the fault did not happen, which would itself be
       news: it means the compiler folded it away or the CPU did not
       raise. Said out loud rather than returning quietly. */
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    up("\ncrash: the fault did NOT happen -- this is a problem with the\n");
    up("test, not good news. Report it.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}
