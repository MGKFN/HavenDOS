#!/usr/bin/env python3
"""helios_boot.py -- boot the current ISO and look at what v0.8.8 added.

── WHY A BOOT HARNESS AND NOT JUST THE SUITES ─────────────────────────

BOOT IT. A SCREEN IS A DELIVERABLE AND IT CAN BE WRONG.  That is this
tree's rule and it was earned: harness/lockdown_boot.py found that
`usb lockdown` printed TCP-framing guidance on a machine with no iPhone
at all, sending the reader to doubt a framing that was never reached.
No suite checks what a shell screen says, and in the shell
x_control()'s mistake costs a trip to the laptop.

v0.8.8 adds two things with 295 checks between them and NEITHER of
those checks can answer the questions below, because all of them are
about a running machine:

  THE TICK.  tests/test_kpoll.c drives kpoll_tick() by hand -- which is
  what lets it produce the interleavings -- and therefore proves
  nothing whatever about IRQ0 actually arriving.  Every release before
  this one assumed the timer was running without anything depending on
  it; kernel/idt.c's own header records that irq_common_stub was
  missing its `push %esp` for an unknown number of releases, so
  irq_handler() read r->int_no out of the saved DS selector and "the
  kernel was picking a function pointer out of an array by lottery, on
  every timer tick."  Nothing noticed, because nothing depended on it.
  SOMETHING DEPENDS ON IT NOW.  `kpoll` prints the tick count, and a
  count that climbs is the first evidence this project has ever had
  that IRQ0 reaches a handler.

  THE CONSOLE.  QEMU's default is 1280x800, which fbc_fit()'s own
  header calls the one size where the grid asked for and the panel
  divided by the glyph agree -- so it is the one panel on which every
  v0.8.8 console change could be a no-op and look perfect.  What this
  can check is that `resolution` SETS a grid the text hardware cannot
  express, that the shell still answers afterwards at that grid, and
  that it comes back.

Run:  python3 harness/helios_boot.py [path/to.iso]
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu import Qemu, kill_stale                            # noqa: E402

ROOT = os.path.dirname(HERE)
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")


def version():
    """From include/version.h, never typed out.

    lockdown_boot.py compared the screen against a version LITERAL and
    broke on the first bump; make_version.py --check refuses literals
    in C for the same reason.  A harness is not exempt from the rule it
    protects.
    """
    h = open(os.path.join(ROOT, "include", "version.h"),
             encoding="utf-8").read()
    m = re.search(r'#define\s+HAVENDOS_VER_NUM\s+"([^"]+)"', h)
    if not m:
        sys.exit("helios_boot: cannot read HAVENDOS_VER_NUM")
    return m.group(1)


def codename():
    h = open(os.path.join(ROOT, "include", "version.h"),
             encoding="utf-8").read()
    m = re.search(r'#define\s+HAVENDOS_CODENAME\s+"([^"]+)"', h)
    return m.group(1) if m else ""


VER = version()
CODE = codename()

fails = []
total = [0]


def ck(what, ok, detail=""):
    total[0] += 1
    if ok:
        print(f"    ok   {what}")
    else:
        fails.append(what)
        print(f"    FAIL {what}" + (f"\n         {detail}" if detail else ""))


def num_after(scr, label):
    """The first integer following `label` on the screen."""
    i = scr.find(label)
    if i < 0:
        return None
    m = re.search(r"(\d+)", scr[i + len(label):i + len(label) + 60])
    return int(m.group(1)) if m else None


def main():
    if not os.path.exists(ISO):
        sys.exit(f"helios_boot: no ISO at {ISO} -- run make first")

    print(f"Booting {os.path.basename(ISO)} to a shell, headless.\n")
    kill_stale()
    q = Qemu(ISO, disk=None, usb=True).start()
    try:
        time.sleep(6)
        q.key("down")          # GRUB entry 2: Shell Mode
        time.sleep(0.4)
        q.key("ret")

        if q.wait_for("System ready", 180) is None:
            raise RuntimeError("the boot log never completed")
        if q.wait_for("admin@havendos", 90) is None:
            q.key("ret")
            if q.wait_for("admin@havendos", 60) is None:
                raise RuntimeError("the shell prompt never appeared")
        time.sleep(1.0)

        def waiting(scr):
            """Is the pager prompt the LAST thing on screen?

            Not "does the screen contain -- more --": the prompt is
            printed in place and stays visible after a key consumes it,
            so a substring test reports a pager that closed pages ago.
            That is what sent a stray 'q' to the command line.
            """
            lines = [ln for ln in scr.split("\n") if ln.strip()]
            return bool(lines) and "-- more --" in lines[-1]

        def run(cmd, settle=2.5, slow=False):
            """Type a command and read the screen it produced.

            `slow` spaces the keystrokes out.  At 100x50 on a 1280x800
            panel the framebuffer console repaints 5000 cells of
            8x16 physical pixels, and a command typed at the harness's
            usual rate loses its first characters -- `resolution reset`
            arrived as `ution reset` twice.  A person types at about
            five characters a second and will not see it; this types at
            twenty.  Recorded rather than worked around silently,
            because "the console is slower at a big grid" is a real
            property of this release and somebody should know it before
            they write the next harness.
            """
            q.line(cmd, settle=0.2 if slow else 0.05)
            time.sleep(settle)
            return q.screen()

        def run_paged(cmd, settle=3.0, pages=12):
            """Run a command and page all the way through it.

            TWO commands in this harness page: `dmesg` and `help`,
            with two different prompt strings ("-- more -- (any key, Q
            to stop)" and "-- more -- press any key --"). Both are
            caught by looking for "-- more --" as the LAST thing on
            screen.

            `dmesg` is paged -- a full boot is about forty lines and the
            screen holds twenty-five -- and the first version of this
            harness ignored that.  It read the first page, failed to
            find a line that is near the END of the log, and then typed
            the NEXT command into the open pager, so `kpoll` was
            swallowed and reported as not dispatching.  Four failures
            from one unhandled prompt.

            Returns every page joined, so a check can look for a line
            wherever it fell.
            """
            q.line(cmd)
            time.sleep(settle)
            pile = [q.screen()]
            for _ in range(pages):
                if not waiting(pile[-1]):
                    break
                q.key("spc")             # the pager key is spc, not space
                time.sleep(0.8)
                pile.append(q.screen())
            # If it is STILL waiting, page once more rather than typing
            # a letter: a 'q' sent at a prompt that has already closed
            # lands on the command line, and the next command became
            # `qkpoll` -- four failures from one stray character.
            if waiting(pile[-1]):
                q.key("spc")
                time.sleep(0.8)
                pile.append(q.screen())
            return "\n".join(pile)

        print("\n  the kernel this release built actually boots")
        ck("it reached a shell prompt", True)
        scr = run("ver")
        ck(f"and reports v{VER}", VER in scr, scr[-400:])
        # `ver` prints the number and not the codename; neofetch is
        # where HAVENDOS_CODENAME reaches a screen (shell/neofetch.c),
        # and the boot splash, which is gone by now.
        scr = run("neofetch", settle=3.5)
        ck(f"and neofetch names the codename {CODE}", CODE in scr,
           scr[-500:])

        # ── THE BOOT LOG'S IRQ CLAIM ────────────────────────────────
        print("\n  the boot log no longer claims an IRQ nothing serves")
        scr = run_paged("dmesg", settle=3.5)
        ck("it says the mouse is polled",
           "mouse polled" in scr, scr[-900:])
        ck("...and does NOT say 'mouse on IRQ12'",
           "mouse on IRQ12" not in scr, scr[-900:])
        ck("while the keyboard's IRQ1, which IS registered, is still named",
           "IRQ1" in scr, scr[-900:])
        ck("and the boot log records the periodic tasks",
           "kpoll" in scr, scr[-900:])

        # ── THE TICK ────────────────────────────────────────────────
        print("\n  `kpoll`: and IRQ0 really does arrive")
        scr = run("kpoll", settle=2.5)
        ck("the command dispatches rather than falling through",
           "Periodic work" in scr, scr[-900:])
        t1 = num_after(scr, "IRQ0 ticks")
        ck("it prints a tick count", t1 is not None, scr[-900:])
        ck("...which is not zero -- IRQ0 reaches a handler",
           bool(t1) and t1 > 0, f"ticks={t1}")
        ck("it lists the registered tasks",
           "net dhcp" in scr and "usb ports" in scr, scr[-900:])
        ck("and the refusals are broken out by reason, not totalled",
           "a driver holds a shared buffer" in scr, scr[-900:])

        # THE one claim no suite can make. Wait, ask again, and the
        # count must have MOVED -- a static non-zero number would be a
        # counter incremented once during init.
        print("\n  ...and the tick keeps arriving")
        before = t1 or 0
        time.sleep(4.0)
        scr = run("kpoll", settle=2.5)
        t2 = num_after(scr, "IRQ0 ticks")
        ck("the tick count advanced over four seconds",
           bool(t2) and t2 > before, f"{before} -> {t2}")
        # 100 Hz, so four seconds is about 400 ticks. Generous bounds:
        # this is a TCG host under load, and the claim being tested is
        # "the clock is roughly right", not "the clock is accurate".
        if t2 and before:
            d = t2 - before
            ck(f"...by roughly four seconds' worth at 100 Hz ({d} ticks)",
               100 < d < 3000, f"delta={d}")

        print("\n  the work really ran, not merely became due")
        ck("at least one task has a non-zero run count",
           re.search(r"(net dhcp|net carrier|usb ports)\s+\d+ms\s+[1-9]",
                     scr) is not None, scr[-900:])
        ck("and 'already polling' stayed at zero",
           re.search(r"^\s*0\s+already polling", scr, re.M) is not None,
           scr[-900:])

        print("\n  `kpoll off` and `kpoll on`")
        scr = run("kpoll off")
        ck("off is acknowledged", "off" in scr.lower(), scr[-400:])
        scr = run("kpoll")
        ck("...and reported as off", "Enabled    : no" in scr, scr[-900:])
        scr = run("kpoll on")
        ck("on is acknowledged", "on" in scr.lower(), scr[-400:])
        scr = run("kpoll")
        ck("...and reported as on", "Enabled    : yes" in scr, scr[-900:])

        # ── THE CONSOLE ─────────────────────────────────────────────
        print("\n  `resolution`: what this panel can hold")
        scr = run("resolution", settle=2.5)
        ck("it names the panel in pixels",
           "Panel" in scr or "720x400" in scr, scr[-900:])
        ck("and the largest grid, or the four text modes",
           "Largest" in scr or "90x60" in scr, scr[-900:])

        # A GRID NO VGA CARD CAN EXPRESS. 100x50 is not one of the four
        # text modes and is inside every v0.8.8 limit, so on a
        # framebuffer console it must be TAKEN -- and on VGA text
        # memory it must be refused BY NAME. Which console this is was
        # established by the listing above.
        fb = "Panel" in scr
        print("\n  a grid the text hardware cannot express"
              f"   (console: {'framebuffer' if fb else 'VGA text'})")
        if fb:
            q.line("resolution 100x50")
            time.sleep(2.0)
            # ── AND THE READBACK HAS TO FOLLOW THE GRID ──────────────
            # The screen is 100x50 cells from the moment the mode is
            # taken, and the decoder samples at the scale and margin
            # the driver chose. Reading it at 80x25 gives ASCII art,
            # which is exactly what the first run of this harness
            # reported -- a working feature read as a broken screen.
            q.set_grid(100, 50)
            time.sleep(1.0)
            scr = q.screen()
            ck("100x50 was accepted and the confirm prompt is up",
               "Keep this mode" in scr, scr[-900:])
            q.key("ret")                  # confirm-or-revert wants ENTER
            time.sleep(2.0)
            scr = q.screen()
            ck("...and confirming keeps it",
               "Mode set to 100x50" in scr, scr[-900:])

            print("\n  the shell still answers at a grid no CRTC has")
            scr = run("ver")
            ck("a command after the mode change still works",
               VER in scr, scr[-600:])
            scr = run("resolution", settle=2.5)
            ck("...and `resolution` reports 100x50",
               "100x50" in scr, scr[-900:])
            # 50 rows really are addressable: write enough lines to
            # reach the bottom of a grid the old cap could not hold,
            # and the last one has to be there.
            print("\n  a grid past every limit, refused by name")
            # BEFORE the long screen below, not after it. The first
            # version ran this after `help` -- fifty rows of output --
            # and typed into a console still drawing them: "resol" was
            # eaten and the shell reported `ution` not found, which
            # reads exactly like a command that does not exist.
            scr = run("resolution 999x999", settle=3.0)
            ck("it is refused", "Refused" in scr, scr[-1400:])
            ck("...as 'outside the grid limits', not as a text-mode problem",
               "outside the grid limits" in scr, scr[-1400:])
            scr = run("resolution", settle=2.5)
            ck("...and the console is still 100x50 after the refusal",
               "Current : 100x50" in scr, scr[-1400:])

            # `help` is the longest screen in the system, so it is what
            # proves there are rows below the 25th to print on. Last,
            # because it fills the grid -- and PAGED, which is the
            # whole lesson of this harness's first four runs: `help`
            # stops with "-- more -- press any key --" and the next
            # command's first three characters were consumed as the
            # keys that dismissed it. `resolution reset` arrived as
            # `ution reset`, and the shell reported a command that does
            # not exist -- which reads nothing like an unread prompt.
            # Two different pagers, two different prompt strings, one
            # mistake.
            scr = run_paged("help", settle=5.0)
            ck("...and the grid is really 50 rows deep",
               len([ln for ln in scr.split("\n") if ln.strip()]) > 25,
               str(len(scr.split("\n"))))
            time.sleep(1.5)

            print("\n  and it comes back")
            # A blank line first, and a wait: the fifty-row `help`
            # screen above is still being painted cell by cell, and a
            # command typed into that loses its first characters. Twice
            # in this harness's first three runs.
            q.line("")
            time.sleep(2.0)
            q.line("resolution reset", settle=0.2)
            time.sleep(3.0)
            q.set_grid(80, 25)
            time.sleep(0.5)
            scr = q.screen()
            ck("`resolution reset` returns to stock 80x25",
               "80x25" in scr, scr[-700:])
        else:
            scr = run("resolution 100x50", settle=2.5)
            ck("100x50 was refused on VGA text memory",
               "Refused" in scr, scr[-900:])
            ck("...by name, saying it is not a VGA text mode",
               "not a VGA text mode" in scr, scr[-900:])
            ck("...and the advice names the four modes",
               "90x60" in scr, scr[-900:])

            print("\n  a grid past every limit, refused by name")
            scr = run("resolution 999x999", settle=2.5)
            ck("it is refused", "Refused" in scr, scr[-900:])
            ck("...and the reason is printed",
               "not a VGA text mode" in scr, scr[-900:])

        print("\n  the shell is still answering at the end of all that")
        scr = run("ver")
        ck("a command after everything still works", VER in scr, scr[-400:])

    finally:
        q.stop()

    print(f"\n{total[0] - len(fails)}/{total[0]} passed")
    if fails:
        print("\nfailed:")
        for f in fails:
            print(f"  - {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
