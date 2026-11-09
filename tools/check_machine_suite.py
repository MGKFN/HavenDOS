#!/usr/bin/env python3
"""check_machine_suite.py -- prove tests/test_machine.c can fail.

A suite that passes on its first run is the least trustworthy result
available, and this one is over a function with no return value and no
state afterwards: on real hardware machine_reset() either stops the
machine or does not, and everything the suite inspects is a recording
made by replaced accessors. A suite like that can very easily be
checking its own stubs.

So every defect below is planted in kernel/machine.c and the suite has
to notice. Most of them are not hypothetical:

  - "only the 8042" is the shipped bug, nine times over, the one that
    hung `reboot` on the HP on 20 October.
  - "no io_wait between status reads" is v0.8.5.3 exactly: an SMM
    emulated controller handing back a stale register, which doubled
    every keystroke for three releases.
  - "the wait spins for ever" is the loop this file replaced -- and it
    hangs rather than failing, which is why the harness treats a
    timeout as a catch.
  - "the message is printed first" is the lie the old poweroff told:
    "It is now safe to power off." on a machine whose fans were still
    running.

Every file is restored before every injection. The first version of
check_xhci_suite.py restored only the file it was about to damage, so
once a bug was injected that file stayed broken for every injection
after it and eleven of them "caught" the leftover -- one of which was a
real miss hiding in the pile. A harness you check instead of thinking
has to be checked itself.

Run:  python3 tools/check_machine_suite.py
"""
import os
import subprocess
import sys
import tempfile

SRC  = "kernel/machine.c"
TEST = "tests/test_machine.c"
FILES = (SRC, TEST)
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

# A real reset takes ~10 ms of io_wait() in SETTLE_IOWAITS; the suite
# runs several of them plus a 100000-spin stuck-controller case, so a
# healthy run is well under a second. Anything past this is the
# unbounded-loop class of bug.
TIMEOUT = 30

BUGS = [
    # Anchored on code, not on the comment above it: the first version
    # of this injection quoted the "/* 1. PCI reset control, warm. */"
    # line and went STALE the moment that comment was reworded. The
    # harness reported it rather than passing it, which is the whole
    # point of the stale check -- but an injection that breaks when a
    # comment is edited is an injection that will be broken most of the
    # time.
    ("only the 8042 is tried",
     "the shipped bug, nine times over: the HP has no 8042, so reboot "
     "reached the halt and sat under 'Please wait.' for ever",
     """    outb(RST_CNT, RST_ARM);
    io_wait();
    outb(RST_CNT, RST_WARM);
    settle();
""",
     """"""),

    ("the 8042 is tried before 0xCF9",
     "the right methods in the wrong order. On the HP the 8042 write "
     "does nothing and the ladder still works, so only the ORDER check "
     "can see this -- and the order is the entire fix",
     """    /* 3. The 8042 pulse. */
    kbc_input_clear();
    outb(KBC_CMD, KBC_PULSE_RST);
    settle();
""",
     """    /* 3. nothing */
"""),

    ("the warm trigger is never armed",
     "RST_CPU triggers on 0 -> 1, so writing 0x06 into a register "
     "already holding 0x06 does nothing. Without the arming write the "
     "register silently stops working on the second attempt",
     """    outb(RST_CNT, RST_ARM);
    io_wait();
    outb(RST_CNT, RST_WARM);""",
     """    outb(RST_CNT, RST_WARM);"""),

    ("the full trigger is never armed",
     "the same edge bug on stage 2, which is the stage that matters on "
     "a chipset that ignores a warm reset",
     """    outb(RST_CNT, RST_ARM);
    io_wait();
    outb(RST_CNT, RST_HARD);""",
     """    outb(RST_CNT, RST_HARD);"""),

    ("the warm reset forgets SYS_RST",
     "0x04 is RST_CPU alone. Some chipsets do nothing at all without "
     "the type bit, and the write still looks like a reset attempt",
     "#define RST_WARM      0x06",
     "#define RST_WARM      0x04"),

    ("the full reset is the warm one again",
     "stage 2 becomes a duplicate of stage 1, so a chipset that "
     "ignores a warm reset gets asked twice and the full reset is "
     "never actually requested",
     "#define RST_HARD      0x0E",
     "#define RST_HARD      0x06"),

    ("the 8042 wait spins for ever",
     "the loop this file replaced. On a controller whose status "
     "register never clears -- an SMM emulation under load -- it hangs "
     "BEFORE the reset write, which from the outside is identical to a "
     "reset that did not work",
     "    for(int i = 0; i < KBC_SPINS; i++){",
     "    for(;;){"),

    ("the 8042 status is read with no I/O delay",
     "v0.8.5.3 exactly: an emulated controller hands back the same "
     "register when the read follows too closely. That bug doubled "
     "every keystroke for three releases and io_wait() had been in "
     "include/io.h, unused, the whole time",
     """        if(!(inb(KBC_STATUS) & KBC_IN_FULL))
            return 1;
        io_wait();""",
     """        if(!(inb(KBC_STATUS) & KBC_IN_FULL))
            return 1;"""),

    ("a stuck 8042 skips the reset write",
     "giving up on the wait is right; giving up on the write is not. A "
     "controller that will not report itself ready may still have a "
     "reset line behind it",
     """    kbc_input_clear();
    outb(KBC_CMD, KBC_PULSE_RST);""",
     """    if(kbc_input_clear())
        outb(KBC_CMD, KBC_PULSE_RST);"""),

    ("the triple fault is gone",
     "the last resort, and the only method that needs no chipset "
     "co-operation at all",
     """    machine_triple_fault();
    settle();""",
     """    settle();"""),

    ("nothing settles between stages",
     "stage 2 is then reached while stage 1 is still in flight, and "
     "the machine is blamed for ignoring a reset it was in the middle "
     "of performing",
     "#define SETTLE_IOWAITS 10000",
     "#define SETTLE_IOWAITS 0"),

    ("the reset failure is announced before it is attempted",
     "this is the shipped lie in its exact shape: a screen that says "
     "what happened before anything has happened",
     """    machine_triple_fault();
    settle();

    /* 5. Still here. Say so rather than sitting under "Please wait."
          for ever, which is the whole defect this file fixes. */
    vga_puts_at("Reset failed: no method this machine supports.",""",
     """    vga_puts_at("Reset failed: no method this machine supports.","""),

    ("reset falls through without parking",
     "returns into a caller that believed it would never come back, "
     "and execution carries on in a shell that has just told the "
     "person the machine is restarting",
     """                4, 17, VGA_WHITE, VGA_BLACK);
    machine_park();
}

/* ════════════════════════════════════════════════════════════════════
   machine_poweroff""",
     """                4, 17, VGA_WHITE, VGA_BLACK);
}

/* ════════════════════════════════════════════════════════════════════
   machine_poweroff"""),

    ("poweroff resets the machine instead",
     "worse than a shutdown that fails: the person walks away from a "
     "machine they believe is off and it comes back up",
     """    outw(PM1A_QEMU,  SLP_S5);
    settle();""",
     """    outw(PM1A_QEMU,  SLP_S5);
    settle();
    outb(RST_CNT, RST_ARM);
    io_wait();
    outb(RST_CNT, RST_WARM);
    settle();"""),

    ("reset powers the machine off instead",
     "`reboot` that shuts down. The machine does stop, so every "
     "liveness check still passes -- only the port it stopped through "
     "tells you",
     """    /* 3. The 8042 pulse. */
    kbc_input_clear();""",
     """    /* 3. The 8042 pulse. */
    outw(PM1A_QEMU, SLP_S5);
    settle();
    kbc_input_clear();"""),

    ("only QEMU's power-off port is written",
     "Bochs and VirtualBox then hang on a command that worked in "
     "QEMU, which is how an emulator-only mechanism goes unnoticed",
     """    outw(PM1A_BOCHS, SLP_S5);
    settle();
    outw(PM1A_VBOX,  SLP_S5_VBOX);
    settle();""",
     """"""),

    ("VirtualBox gets QEMU's sleep value",
     "0x2000 is not what VirtualBox's PM1a_CNT wants, and a wrong "
     "value writes just as successfully as a right one",
     "#define SLP_S5_VBOX   0x3400",
     "#define SLP_S5_VBOX   0x2000"),

    ("poweroff still claims the machine is off",
     "the message the nine sites printed -- before trying, and then "
     "regardless of the outcome",
     """    vga_puts_at("Power off is not supported on this machine yet.",
                4, 16, VGA_YELLOW, VGA_BLACK);""",
     """    vga_puts_at("It is now safe to power off.",
                4, 16, VGA_YELLOW, VGA_BLACK);"""),

    ("the power button is never mentioned",
     "the person is left with a screen that says something failed and "
     "no way to act on it, which is most of the original complaint",
     """    vga_puts_at("Hold the power button to switch off.",
                4, 17, VGA_WHITE, VGA_BLACK);
    machine_park();
}

/* ════════════════════════════════════════════════════════════════════
   machine_poweroff""",
     """    machine_park();
}

/* ════════════════════════════════════════════════════════════════════
   machine_poweroff"""),

    ("poweroff parks without saying anything",
     "a blank halt, which is exactly what the person reported: a "
     "machine sitting there with no explanation",
     """    vga_puts_at("Power off is not supported on this machine yet.",
                4, 16, VGA_YELLOW, VGA_BLACK);
    vga_puts_at("Hold the power button to switch off.",
                4, 17, VGA_WHITE, VGA_BLACK);
    machine_park();""",
     """    machine_park();"""),
]


def build_and_run():
    exe = os.path.join(tempfile.gettempdir(), "hd_tmach_inj")
    b = subprocess.run(CC + ["-o", exe, TEST], capture_output=True, text=True)
    if b.returncode != 0:
        return "build-failed", b.stderr
    try:
        r = subprocess.run([exe], capture_output=True, text=True,
                           timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return "hang", ""
    return ("pass" if r.returncode == 0 else "fail"), r.stdout


def main():
    if not os.path.exists(SRC):
        sys.stderr.write("run me from the source root\n")
        return 2
    pristine = {f: open(f, encoding="utf-8").read() for f in FILES}

    print("Injecting %d reset/poweroff defects into %s.\n"
          "Four of them have shipped; one of them shipped nine times.\n"
          % (len(BUGS), SRC))

    st, out = build_and_run()
    if st != "pass":
        print("  the UNMODIFIED suite does not pass (%s) -- fix that first" % st)
        print(out[-3000:])
        return 2
    print("  baseline: the unmodified suite passes\n")

    missed, stale = [], []
    try:
        for i, (label, why, old, new) in enumerate(BUGS, 1):
            # Every file back to pristine before every injection, not
            # just the one about to be damaged.
            for f, text in pristine.items():
                open(f, "w", encoding="utf-8").write(text)

            n = pristine[SRC].count(old)
            if n != 1:
                stale.append((label, n))
                print("  %2d. %-54s INJECTION STALE (%d matches)" % (i, label, n))
                continue
            open(SRC, "w", encoding="utf-8").write(
                pristine[SRC].replace(old, new, 1))

            st, _ = build_and_run()
            if st == "pass":
                missed.append((label, why))
                verdict = "MISSED"
            elif st == "build-failed":
                verdict = "caught (did not compile)"
            elif st == "hang":
                verdict = "caught (hung)"
            else:
                verdict = "caught"
            print("  %2d. %-54s %s" % (i, label, verdict))
    finally:
        for f, text in pristine.items():
            open(f, "w", encoding="utf-8").write(text)

    print()
    if stale:
        print("  %d injection(s) no longer match the source:" % len(stale))
        for label, n in stale:
            print("    - %s (%d matches)" % (label, n))
    if missed:
        print("  %d defect(s) the suite did NOT catch:" % len(missed))
        for label, why in missed:
            print("    - %s\n        %s" % (label, why))
        return 1
    if stale:
        return 1
    print("  all %d caught" % len(BUGS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
