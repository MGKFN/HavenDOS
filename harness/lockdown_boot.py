#!/usr/bin/env python3
"""lockdown_boot.py -- boot the current ISO and exercise `usb lockdown`.

── WHAT THIS CAN AND CANNOT ANSWER ────────────────────────────────────

QEMU HAS NO IPHONE. Nothing here can say whether lockdownd answers
QueryType; only the HP can, and docs/LOCKDOWN-HARDWARE-TEST.md is the
five minutes that settle it.

What this CAN answer, and what no suite can:

  - the kernel still boots with usb/lockdown.c linked into it
  - `usb lockdown` DISPATCHES, rather than falling through to the plain
    bus listing. v0.8.6.6's lesson: `usb cfg` on a kernel too old to
    have it printed a perfectly normal screen and looked like success,
    and that cost a round trip to the laptop
  - it is DOCUMENTED on the help screen and on `usb`'s own usage list
  - with no Apple device on the bus it REFUSES PROMPTLY and says which
    step refused -- it does not spend its twenty-thousand-poll budget
    waiting for a device that is not there, and it does not hang

That last one is the reason this exists. Every wait in this kernel is a
bounded poll because nothing runs on a timer, and a command that burns
its whole budget before saying anything is indistinguishable, at the
keyboard, from one that has wedged.

Run:  python3 harness/lockdown_boot.py [path/to.iso]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu import Qemu, kill_stale                            # noqa: E402

ROOT = os.path.dirname(HERE)
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")


def version():
    """Read it from include/version.h, never type it out.

    The first version of this file compared the screen against a
    LITERAL and failed the moment the version was bumped -- which is
    the whole reason tools/make_version.py --check refuses version
    literals anywhere outside that header. A harness is not exempt
    from the rule it exists to protect. (Prose in a comment is fine;
    a comparison is not.)
    """
    import re
    h = open(os.path.join(ROOT, "include", "version.h"),
             encoding="utf-8").read()
    m = re.search(r'#define\s+HAVENDOS_VER_NUM\s+"([^"]+)"', h)
    if not m:
        sys.exit("lockdown_boot: cannot read HAVENDOS_VER_NUM")
    return m.group(1)


VER = version()

fails = []
total = [0]


def ck(what, ok, detail=""):
    total[0] += 1
    if ok:
        print(f"    ok   {what}")
    else:
        fails.append(what)
        print(f"    FAIL {what}" + (f"\n         {detail}" if detail else ""))


def main():
    if not os.path.exists(ISO):
        sys.exit(f"lockdown_boot: no ISO at {ISO} -- run make first")

    print(f"Booting {os.path.basename(ISO)} to a shell, headless.\n")
    kill_stale()
    q = Qemu(ISO, disk=None, usb=True).start()
    try:
        time.sleep(6)
        # GRUB entry 2 is Shell Mode. Entry 1 is the desktop, which needs
        # a key to reach a prompt and is not what this is testing.
        q.key("down")
        time.sleep(0.4)
        q.key("ret")

        # Shell Mode goes straight through: there is no "press any key"
        # POST screen on this entry, which is what harness/boot.py waits
        # for and why that one does not drive this. Wait for the boot log
        # to finish and then for the prompt itself.
        if q.wait_for("System ready", 180) is None:
            raise RuntimeError("the boot log never completed")
        if q.wait_for("admin@havendos", 90) is None:
            # The shell banner may need a key on some entries.
            q.key("ret")
            if q.wait_for("admin@havendos", 60) is None:
                raise RuntimeError("the shell prompt never appeared")
        time.sleep(1.0)

        def run(cmd, settle=2.5):
            q.line(cmd)
            time.sleep(settle)
            return q.screen()

        print("\n  the kernel this release built actually boots")
        ck("it reached a shell prompt", True)

        scr = run("ver")
        ck(f"and reports v{VER}", VER in scr, scr[-400:])

        print("\n  `usb lockdown` is documented, not just dispatched")
        # check_help.py checks the top-level command; `usb lockdown` is a
        # named argument, and v0.8.6.6 is why an argument that is not
        # documented is worth a check of its own.
        scr = run("usb nosuchoption")
        ck("usb's usage list names it",
           "lockdown" in scr, scr[-600:])
        ck("...and still refuses an argument it does not know",
           "no such option" in scr.lower(), scr[-600:])

        print("\n  `usb lockdown` DISPATCHES rather than falling through")
        t0 = time.time()
        scr = run("usb lockdown", settle=4.0)
        took = time.time() - t0
        # The plain bus listing is what a kernel too old to have this
        # command would print, and it looks like success. The word
        # "lockdownd" appears only on the real screen.
        ck("it ran the lockdown command, not the bus listing",
           "lockdownd" in scr or "62078" in scr, scr[-700:])

        print("\n  ...and with no Apple device it names THAT, not the framing")
        # v0.8.7.7: the first version of this command printed the
        # generic failure here, which sends the reader to doubt the TCP
        # framing when the framing was never reached. Three causes
        # behind one screen is the x_control() mistake in the shell.
        ck("it says the interface is not on the bus",
           "No Apple Mobile Device interface" in scr, scr[-700:])
        ck("...and NOT that the TCP framing is the suspect",
           "framing is the suspect" not in scr, scr[-700:])
        ck("and it tells you how to look for the interface",
           "FF/FE/02" in scr, scr[-700:])
        # A bounded poll that spends its whole budget looks, at the
        # keyboard, exactly like a wedge. There is nothing to poll for
        # here -- the interface is not on the bus -- so this must come
        # back immediately.
        ck(f"and came back promptly ({took:.1f}s), not after a full "
           f"poll budget", took < 20.0, f"took {took:.1f}s")

        print("\n  the shell is still answering afterwards")
        scr = run("ver")
        ck("a command after it still works", VER in scr, scr[-300:])

        print("\n  and `usb mux` still behaves the same way")
        scr = run("usb mux", settle=4.0)
        ck("it refuses with no phone attached",
           "No Apple Mobile Device" in scr or "cannot reach" in scr,
           scr[-700:])

        print("\n  the log names which step refused")
        scr = run("dmesg", settle=3.0)
        ck("dmesg still works after both commands",
           len(scr.strip()) > 0, scr[-300:])

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
