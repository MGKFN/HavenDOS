#pragma once
/* ================================================================
   include/machine.h -- resetting and powering off the machine
   ================================================================
   Reported from the HP on 20 October: `reboot` and `poweroff` both
   hang. Neither is a crash. Both were a single write to a port that
   exists only in an emulator, followed by `for(;;) cli; hlt` -- so
   on a machine where the write does nothing, the halt is reached and
   the screen keeps saying "Rebooting HavenDOS... Please wait." for
   ever.

   `reboot` pulsed the 8042 reset line, outb(0x64,0xFE). That laptop
   has no 8042: no PS/2 port of either kind, and the controller is
   emulated in SMM -- the same emulation that doubled every keystroke
   until v0.8.5.3. An emulated register has no reset line behind it.

   `poweroff` wrote 0x604 (QEMU's ACPI PM1a_CNT), 0xB004 (Bochs and
   older QEMU) and 0x4004 (VirtualBox). Every one is emulator
   specific. On real silicon those addresses belong to something else,
   or to nothing.

   That sequence was written out in NINE places -- six resets and
   three power-offs, across five files -- and not one of the nine had
   heard of port 0xCF9, the register that would have worked on that
   laptop. This is the same shape as the bus-label bug that
   v0.8.5.4 finished killing after four releases of correcting it one
   site at a time, so the fix is these two functions and
   tools/check_machine_reset.py, which fails the build if a tenth
   copy appears.

   Neither function returns on hardware that can be reset or powered
   off. Both return under a native test, which is how every fallback
   stage gets exercised: see include/machine_asm.h.
   ================================================================ */
#ifndef MACHINE_H
#define MACHINE_H

/* Reset the machine. Four methods, in this order: a platform reset
   through the PCI reset control register at 0xCF9, the same register
   again with FULL_RST for a power cycle, the 8042 pulse, then a triple
   fault. Never returns -- if all four fail it says so on screen and
   parks. */
void machine_reset(void);

/* Power the machine off. Tries the three emulator ACPI shutdown
   ports. There is no hardware path yet -- that is ACPI proper: find
   the RSDP, parse the FADT for PM1a_CNT_BLK, parse the DSDT for the
   \_S5 sleep type, then write SLP_TYP|SLP_EN -- so on real silicon
   this parks with a message telling the person to hold the power
   button, rather than hanging under a screen that claims it is
   shutting down. */
void machine_poweroff(void);

#endif
