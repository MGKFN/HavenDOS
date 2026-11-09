/* ════════════════════════════════════════════════════════════════════
   kernel/machine.c -- the one place that resets or powers off the box

   See include/machine.h for why this file exists. In short: nine
   copies of a one-line emulator trick, none of which worked on the
   laptop HavenDOS now dual-boots on.

   Everything here reaches hardware through include/io.h and the two
   instructions in include/machine_asm.h, both of which a native test
   replaces -- so tests/test_machine.c runs THIS escalation rather
   than a copy of it, and sees every stage including the ones only
   reached because the stage before did nothing.
   ════════════════════════════════════════════════════════════════════ */
#include "types.h"
#include "io.h"
#include "machine_asm.h"
#include "machine.h"
#include "vga.h"

/* ── stage 1: the PCI reset control register ──────────────────────────
   Port 0xCF9, RST_CNT, in the LPC bridge on every Intel PCH and the
   equivalent on AMD's FCH. Present on essentially every x86 chipset
   since the 1990s, and -- the point for the HP -- it is a real
   register in real silicon, not something SMM pretends to have.

     bit 1  SYS_RST   0 = reset the CPU only (INIT), 1 = reset the
                      whole platform
     bit 2  RST_CPU   0 -> 1 is what triggers it
     bit 3  FULL_RST  1 = power-cycle the platform rather than simply
                      resetting it

   So RST_WARM below (0x06) is SYS_RST | RST_CPU: a platform reset,
   not a CPU-only one. RST_HARD (0x0E) adds FULL_RST and power-cycles.
   The names are the usual shorthand; the bits are what matters.

   RST_CPU triggers on the EDGE, so the trigger bit has to be low
   before it is raised. That is why stage 1 is two writes and not
   one: a single write of 0x06 to a register already holding 0x06
   does nothing at all. */
#define RST_CNT       0xCF9
#define RST_ARM       0x02      /* SYS_RST set, RST_CPU still low   */
#define RST_WARM      0x06      /* SYS_RST | RST_CPU -- go          */
#define RST_HARD      0x0E      /* FULL_RST as well -- power cycle  */

/* ── stage 2: the 8042 pulse ──────────────────────────────────────────
   Kept, because it is what works on the older machines this tree
   still supports and in emulators. Two things about the version it
   replaces, both of which matter on a machine whose 8042 is a
   fiction maintained by SMM:

   1. It waited for the input buffer with an UNBOUNDED spin:
          1: inb $0x64; testb $2, %al; jnz 1b
      If status bit 1 never clears -- and on an emulated controller
      under load it may not -- that loop hangs BEFORE the reset write
      is ever reached. The hang then looks exactly like a failed
      reset while actually being a failed wait. Bounded here.

   2. It read the status port back to back with no I/O delay. That is
      the v0.8.5.3 bug, the one that doubled every keystroke: an SMM
      emulated controller hands back a stale register when the read
      follows too closely. io_wait() between reads, as the keyboard
      driver now does. */
#define KBC_DATA      0x60
#define KBC_STATUS    0x64
#define KBC_CMD       0x64
#define KBC_IN_FULL   0x02      /* status bit 1: input buffer full  */
#define KBC_PULSE_RST 0xFE      /* pulse the reset line             */

/* ── poweroff: the three emulator ports ───────────────────────────────
   ACPI PM1a_CNT with SLP_TYP for S5 and SLP_EN set. The addresses
   are hard-coded per emulator because finding the real one means
   parsing ACPI tables, which is a release of its own and not this
   one. Unchanged from what the nine sites wrote; what is new is what
   happens when they do nothing. */
#define PM1A_QEMU     0x604
#define PM1A_BOCHS    0xB004
#define PM1A_VBOX     0x4004
#define SLP_S5        0x2000    /* SLP_TYP=5 << 10 | SLP_EN         */
#define SLP_S5_VBOX   0x3400    /* VirtualBox wants its own value   */

/* ── how long to wait for a reset to actually happen ──────────────────
   A chipset reset is not instantaneous: the write returns and some
   microseconds later the machine stops executing. Without a wait,
   stage 2 would be reached while stage 1 was still in progress, and
   the log would blame the wrong one.

   io_wait() is a write to port 0x80, the conventional ~1 us dummy
   I/O delay, so this is roughly 10 ms. The PIT is not used because
   sleep_ms() needs interrupts and a caller may have disabled them,
   and because this file is compiled into a native test where there
   is no timer at all. */
#define SETTLE_IOWAITS 10000

static void settle(void)
{
    for(int i = 0; i < SETTLE_IOWAITS; i++)
        io_wait();
}

/* Wait for the 8042 to accept a command. Returns 1 if the input
   buffer drained, 0 if it never did -- and the caller writes the
   reset command anyway, because a controller that will not report
   itself ready may still have a reset line, and by this point there
   is nothing to lose. */
#define KBC_SPINS 100000

static int kbc_input_clear(void)
{
    for(int i = 0; i < KBC_SPINS; i++){
        if(!(inb(KBC_STATUS) & KBC_IN_FULL))
            return 1;
        io_wait();
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
   machine_reset
   ════════════════════════════════════════════════════════════════════ */
void machine_reset(void)
{
    /* 1. PCI reset control: a platform reset. */
    outb(RST_CNT, RST_ARM);
    io_wait();
    outb(RST_CNT, RST_WARM);
    settle();

    /* 2. PCI reset control again, this time with FULL_RST so the
          platform is power-cycled. Some chipsets ignore the plain
          reset requested above but honour this one. */
    outb(RST_CNT, RST_ARM);
    io_wait();
    outb(RST_CNT, RST_HARD);
    settle();

    /* 3. The 8042 pulse. */
    kbc_input_clear();
    outb(KBC_CMD, KBC_PULSE_RST);
    settle();

    /* 4. Triple fault. Needs no chipset co-operation, so it is the
          one method that cannot be absent -- only prevented, by a
          hypervisor that catches it. */
    machine_triple_fault();
    settle();

    /* 5. Still here. Say so rather than sitting under "Please wait."
          for ever, which is the whole defect this file fixes. */
    vga_puts_at("Reset failed: no method this machine supports.",
                4, 16, VGA_LIGHT_RED, VGA_BLACK);
    vga_puts_at("Hold the power button to switch off.",
                4, 17, VGA_WHITE, VGA_BLACK);
    machine_park();
}

/* ════════════════════════════════════════════════════════════════════
   machine_poweroff
   ════════════════════════════════════════════════════════════════════ */
void machine_poweroff(void)
{
    outw(PM1A_QEMU,  SLP_S5);
    settle();
    outw(PM1A_BOCHS, SLP_S5);
    settle();
    outw(PM1A_VBOX,  SLP_S5_VBOX);
    settle();

    /* Real silicon reaches here every time: all three ports above are
       emulator inventions. Telling the truth about that is the point
       -- the screens that called this used to print "It is now safe
       to power off." BEFORE trying, so a machine still drawing power
       with its fans running said it was off. */
    vga_puts_at("Power off is not supported on this machine yet.",
                4, 16, VGA_YELLOW, VGA_BLACK);
    vga_puts_at("Hold the power button to switch off.",
                4, 17, VGA_WHITE, VGA_BLACK);
    machine_park();
}
