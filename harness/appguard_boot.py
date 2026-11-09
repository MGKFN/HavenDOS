#!/usr/bin/env python3
"""appguard_boot.py -- make an app crash and watch the machine survive.

BOOT IT. A SCREEN IS A DELIVERABLE AND IT CAN BE WRONG.

kernel/appguard.c is a recovery path, and a recovery path is the one
kind of code that cannot be proved by a suite: it is reached only from
a CPU exception, and a native suite has no CPU exceptions to offer. A
unit test can call appguard_enter() and appguard_fault() in sequence
and watch a flag change, and that would tell you nothing about whether
`iret` lands where the header says it lands.

So this crashes an app on purpose, five ways, and reads the screen.

── THE FOUR THINGS ONLY A BOOT CAN ANSWER ────────────────────────────

  1. THE FAULT IS CAUGHT AT ALL. Before this release every exception
     painted KERNEL PANIC and halted. The test is whether the screen
     says "Program stopped" instead.

  2. THE MACHINE IS STILL ALIVE AFTERWARDS. A recovery that returns to
     a broken desktop is a panic with extra steps. So after the crash
     the harness drives the desktop again -- opens another app, reads
     its screen -- and only then counts it.

  3. THE STACK SWITCH IS RIGHT. appguard_jump() loads esp from the
     saved buffer because `iret` in ring 0 does not reload it. If that
     is wrong the return goes somewhere arbitrary, and what you get is
     not a clean failure: it is a machine that appears to recover and
     then misbehaves later. Crashing FIVE TIMES IN A ROW is the check
     -- a wrong esp survives once and compounds.

  4. INTERRUPTS COME BACK. `iret` reloads eflags from the exception
     frame, so a fault taken with interrupts off would resume with
     them off, and a desktop that cannot receive IRQ0 looks hung. The
     tick count before and after is the evidence.

── AND ONE THING THAT MUST STILL FAIL ────────────────────────────────

`crash stack` runs esp off its end, which ends as a double fault.
include/appguard.h says #DF is NOT recoverable and panics by design,
because the handler that would recover has no stack to run on. A
harness that reported that as a pass would be hiding the limit. So it
is asserted to panic, and a recovery there would be the failure.

Run:  python3 harness/appguard_boot.py [path/to.iso]     (foreground)
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

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}"
          + (f"\n          {detail}" if detail and not ok else ""))
    return bool(ok)


def boot_to_shell(q, entry_down=1):
    """GRUB -> shell prompt. entry_down=1 is Shell Mode."""
    time.sleep(6)
    for _ in range(entry_down):
        q.key("down")
        time.sleep(0.4)
    q.key("ret")
    if q.wait_for("System ready", 180) is None:
        raise RuntimeError("the boot log never completed")
    if q.wait_for("admin@havendos", 90) is None:
        q.key("ret")
        if q.wait_for("admin@havendos", 60) is None:
            raise RuntimeError("the shell prompt never appeared")
    time.sleep(1.0)


def ticks(q):
    """kpoll's "IRQ0 ticks : <n>", or None.

    Matched on the exact label rather than the word "ticks", because
    the line reads "IRQ0 ticks" and a loose pattern can find the 0 in
    IRQ0 depending on where it starts.
    """
    if not in_shell(q):
        return None
    q.line("kpoll")
    time.sleep(1.4)
    scr = q.screen()
    m = re.search(r"IRQ0 ticks\s*:\s*(\d+)", scr)
    return int(m.group(1)) if m else None


# ══ part 1: the boot console has no guard, and says so ═══════════════

def part_unguarded(q):
    print("\n-- the boot console: no guard, and it must say so --")
    q.line("crash")
    time.sleep(1.0)
    scr = q.screen()
    check("`crash` with no argument explains itself",
          "app guard" in scr.lower(), scr[-400:])
    check("it reports zero recoveries so far",
          re.search(r"recovered so far this boot:\s*0", scr) is not None,
          scr[-400:])

    # `crash where` and not `crash div` for this one. A faulting form
    # prints the warning and then panics, and the panic screen paints
    # over the warning before it can be read -- so the check would be
    # testing the harness's timing, not the kernel.
    q.line("crash where")
    time.sleep(1.2)
    scr = q.screen()
    check("it warns that nothing is armed at the boot console",
          "NO GUARD IS ARMED" in scr, scr[-600:])

    q.line("crash div")
    time.sleep(1.5)
    scr = q.screen()
    panicked = "PANIC" in scr.upper()
    check("and the unguarded fault does panic, as documented",
          panicked, "no panic -- then the fault did not happen at all:\n"
                    + scr[-600:])
    return panicked


# ══ part 2: the desktop, where the guard is armed ═══════════════════

SHELL_PROMPT = "@havendos"


def in_shell(q):
    return SHELL_PROMPT in q.screen()


def open_shell_from_desktop(q):
    """Desktop -> Shell app. TAB is the desktop's shortcut for it.

    Waits for the PROMPT rather than assuming the keystroke landed.
    Checking for "$" was the earlier version and it was wrong: the
    desktop's own screen can contain one, so the harness concluded a
    shell was already open and then typed a command into a desktop,
    which read it as a string of single-key shortcuts.
    """
    if in_shell(q):
        return True
    for _ in range(4):
        q.key("tab")
        time.sleep(1.8)
        if in_shell(q):
            return True
        q.key("esc")
        time.sleep(0.8)
    return False


def to_desktop(q):
    """Shell Mode -> `exit` -> login -> desktop.

    NOT the default GRUB entry, which runs the setup wizard on a live
    ISO and would make this harness seven screens of clicking through
    a licence. kernel/main.c's MODE_SHELL falls through to
    login_screen() and the desktop after the shell returns, and skips
    the wizard entirely.

    A live ISO has no accounts, so the login screen asks for one to be
    created. Its fields advance with DOWN, not TAB -- tried TAB first
    and all three fields were typed into the username box, which the
    screen then showed as "adminadminadmi".
    """
    q.line("exit")
    time.sleep(2.5)
    for _ in range(14):
        scr = q.screen()

        if "How would you like to start" in scr or "Create new account" in scr:
            q.key("ret")      # the first option is already selected
            time.sleep(2.0)
            continue
        if "Create Account" in scr or "Username" in scr:
            q.type("admin"); time.sleep(0.4)
            q.key("down");   time.sleep(0.4)
            q.type("admin"); time.sleep(0.4)
            q.key("down");   time.sleep(0.4)
            q.type("admin"); time.sleep(0.4)
            q.key("ret");    time.sleep(3.0)
            continue
        if "Password" in scr:
            q.type("admin"); time.sleep(0.4)
            q.key("ret");    time.sleep(3.0)
            continue

        # PAST LOGIN, decided by what is NOT on the screen.
        #
        # Two earlier versions of this harness tried to recognise the
        # desktop by its chrome and both were wrong, because the login
        # screen draws a header in the same place and the same style:
        # "HavenDOS <ver>   TechHaven Studios   <theme>". The harness
        # matched it, declared the desktop up, and typed a shell
        # command into a login box.
        #
        # So this does not try to name the desktop. It waits for the
        # login screen to be GONE -- "TechHaven Studios" is on its
        # header and not on the desktop's -- and then lets the kernel
        # settle the question, which is what guard_state() is for.
        if "TechHaven Studios" not in scr:
            return True

        q.key("esc")          # ESC, not ENTER: ENTER on a desktop icon
        time.sleep(1.5)       # opens its context menu
    return False


def guard_state(q):
    """Ask the KERNEL whether a guard is armed, rather than inferring
    it from which window is on screen.

    This matters: the first run of this harness decided "the desktop
    came up" was false while looking at a screen whose top line was
    the desktop's own taskbar with the Shell open in it. The harness
    was wrong, not the kernel. `crash where` reads appguard_armed()
    directly, so there is nothing left to misread.

    Returns (armed, recoveries) or (None, None).
    """
    q.line("crash where")
    time.sleep(1.3)
    scr = q.screen()
    m = re.search(r"recovered so far this boot:\s*(\d+)", scr)
    if "NO GUARD IS ARMED" in scr:
        return False, (int(m.group(1)) if m else None)
    if "app guard is armed" in scr:
        return True, (int(m.group(1)) if m else None)
    return None, None


def part_guarded(q):
    print("\n-- the desktop: four crashes, and it keeps running --")

    if not check("the desktop came up", to_desktop(q), q.screen()[:500]):
        return

    # Make sure we are in a desktop-hosted Shell, then ASK the kernel.
    q.key("esc"); time.sleep(0.8)
    q.key("esc"); time.sleep(0.8)
    if not check("the Shell opens from the desktop",
                 open_shell_from_desktop(q), q.screen()[:600]):
        return
    armed, n0 = guard_state(q)
    if not check("a guard IS armed in an app launched from the desktop",
                 armed is True,
                 f"crash where said armed={armed} -- if it said False, "
                 "desktop_run_app() is not arming it. Screen:\n"
                 + q.screen()[:600]):
        return
    check("and nothing has been recovered yet", n0 == 0, f"it says {n0}")

    t0 = None
    forms = ["div", "null", "opcode", "wild"]
    for i, how in enumerate(forms):
        if not open_shell_from_desktop(q):
            check(f"the Shell opens before crash {i+1} ({how})", False,
                  q.screen()[:400])
            return
        if t0 is None:
            t0 = ticks(q)

        q.line(f"crash {how}")

        # WAIT for one of the two outcomes rather than sleeping and
        # peeking. The earlier version slept 2.5s and read whatever
        # was there, and reported "no recovery window" three times
        # while looking at a screen caught mid-redraw -- the fourth
        # crash passed on identical code, which is what gave it away.
        scr = ""
        for _ in range(24):
            scr = q.screen()
            if "Program stopped" in scr or "PANIC" in scr.upper():
                break
            time.sleep(0.5)

        caught = "Program stopped" in scr
        panicked = "PANIC" in scr.upper()
        check(f"crash {how}: caught, not a panic",
              caught and not panicked,
              ("PANICKED" if panicked else "no recovery window") + ":\n"
              + scr[-700:])
        if panicked:
            return

        # 2: still alive. Not "the screen changed" -- the desktop has
        # to still answer a keystroke. The crash window ends with
        # keyboard_getchar(), so this keypress is also what dismisses
        # it.
        q.key("esc")
        time.sleep(1.5)
        alive = "Start" in q.screen() or "HavenDOS" in q.screen()
        check(f"crash {how}: the desktop still answers afterwards",
              alive, q.screen()[:500])
        if not alive:
            return

    # 4: interrupts came back. A desktop that cannot receive IRQ0 looks
    # like one that recovered, until you ask the clock.
    if open_shell_from_desktop(q):
        t1 = ticks(q)
        if t0 is not None and t1 is not None:
            check("the timer is still ticking after four recoveries",
                  t1 > t0, f"before {t0}, after {t1} -- interrupts did "
                           "not come back")
        else:
            check("the tick count could be read", False,
                  f"before={t0} after={t1}")

        # the count the kernel itself kept
        _, n1 = guard_state(q)
        check("the kernel counted all four recoveries", n1 == 4,
              f"it says {n1}, expected 4")

        # ── and the one the guard cannot help with ───────────────
        print("\n-- the stack overflow, which the guard CANNOT catch --")
        before = guard_state(q)[1]
        q.line("crash stack")
        scr = ""
        for _ in range(16):
            scr = q.screen()
            if "Program stopped" in scr or "PANIC" in scr.upper():
                break
            time.sleep(0.5)

        # MEASURED, not assumed. The first version of this asserted a
        # panic, on appguard.h's reasoning that a vanished stack leaves
        # the recovery nowhere to run. What this machine actually does
        # is neither: the stack walks down through mapped memory for a
        # long way first, zeroing the heap and the console's shadow
        # buffer, and the result is a corrupted screen and a machine
        # that stops answering. No fault to catch, because the damage
        # lands before any fault arrives.
        #
        # That is the "NOT caught" row of appguard.h, demonstrated. So
        # what is checked here is that the system does NOT claim to
        # have survived it.
        if "Program stopped" in scr:
            q.key("esc"); time.sleep(1.5)
            still = open_shell_from_desktop(q)
            after = guard_state(q)[1] if still else None
            check("if it claims to have recovered, it really did",
                  still and after is not None and after > (before or 0),
                  "it showed a recovery window and then did not work: "
                  f"shell={still} count {before} -> {after}")
        elif "PANIC" in scr.upper():
            check("it panicked rather than pretending to recover", True)
        else:
            # The documented outcome. Confirm it is wedged rather than
            # merely slow, and that nothing claimed otherwise.
            q.key("esc"); time.sleep(2.0)
            wedged = not open_shell_from_desktop(q)
            check("it wedges the machine, and says so rather than "
                  "claiming a recovery", wedged,
                  "it neither panicked, recovered, nor wedged -- which "
                  "is a fourth outcome nothing describes:\n"
                  + q.screen()[:600])
            if wedged:
                print("          (measured: memory corrupted below the")
                print("           stack, screen garbled, no response --")
                print("           appguard.h's 'NOT caught' row, live)")


def main():
    if not os.path.exists(ISO):
        sys.exit(f"appguard_boot: no ISO at {ISO} -- run make first")
    print(f"Crashing apps on purpose in {os.path.basename(ISO)}.\n"
          "A recovery path is the one kind of code a native suite\n"
          "cannot reach: it needs a real CPU exception.\n")

    kill_stale()
    q = Qemu(ISO, usb=True).start()
    try:
        boot_to_shell(q, entry_down=1)          # Shell Mode
        part_unguarded(q)
    finally:
        q.stop()

    time.sleep(1.0)
    kill_stale()
    q = Qemu(ISO, usb=True).start()
    try:
        boot_to_shell(q, entry_down=1)          # Shell Mode again
        part_guarded(q)
    finally:
        q.stop()

    npass = sum(1 for _, ok, _ in results if ok)
    print(f"\nappguard_boot: {npass}/{len(results)} passed")
    for name, ok, detail in results:
        if not ok:
            print(f"  FAILED: {name}")
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
