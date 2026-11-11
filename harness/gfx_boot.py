"""gfx_boot.py -- boot the current ISO and exercise `gfx`.

── WHAT THIS CAN AND CANNOT ANSWER ────────────────────────────────────

QEMU HAS NO IRIS XE. Its display at 00:02.0 is the Bochs/QEMU "std"
adapter (1234:1111), and no machine type QEMU offers has an Intel
display engine. So nothing here can say whether a single register
offset in drivers/xedisp.c is right; only the HP can, and
docs/GFX-INSPECTOR-HARDWARE-TEST.md is the five minutes that settle it.

What this CAN answer, and no suite can:

  - the kernel still boots with drivers/xedisp.c linked into it
  - `gfx` DISPATCHES -- the word is not swallowed by another command
  - on a machine with no Intel display it says THAT, by name, and
    touches nothing: no BAR is mapped for a device that is not there
  - `gfx regs` and `gfx vbt` refuse the same way, and an unknown
    argument prints the usage rather than running the inspector
  - the shell answers afterwards

Run:  python3 harness/gfx_boot.py [path/to.iso]
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
    """From include/version.h, never typed out (see lockdown_boot.py)."""
    h = open(os.path.join(ROOT, "include", "version.h"),
             encoding="utf-8").read()
    m = re.search(r'#define\s+HAVENDOS_VER_NUM\s+"([^"]+)"', h)
    if not m:
        sys.exit("gfx_boot: cannot read HAVENDOS_VER_NUM")
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
        sys.exit(f"gfx_boot: no ISO at {ISO} -- run make first")

    print(f"Booting {os.path.basename(ISO)} to a shell, headless.\n")
    kill_stale()
    q = Qemu(ISO, disk=None, usb=True).start()
    try:
        time.sleep(6)
        q.key("down")                     # GRUB entry 2: Shell Mode
        time.sleep(0.4)
        q.key("ret")
        # The prompt is the thing waited for. "System ready" scrolls off
        # the top before the first poll on a fast host, and waiting for a
        # line that is no longer on screen fails a boot that worked --
        # the first run of this harness did exactly that.
        if q.wait_for("admin@havendos", 240) is None:
            q.key("ret")
            if q.wait_for("admin@havendos", 60) is None:
                raise RuntimeError("the shell prompt never appeared")
        time.sleep(1.0)

        def run(cmd, settle=3.0):
            q.line(cmd)
            time.sleep(settle)
            return q.screen()

        print("\n  the kernel this release built actually boots")
        scr = run("ver")
        ck(f"it reports v{VER}", VER in scr, scr[-400:])

        print("\n  `gfx` dispatches, and on a machine with no Iris Xe says so")
        scr = run("gfx")
        ck("it ran the inspector (its header is on screen)",
           "nothing is written" in scr, scr[-900:])
        ck("it names the absence, not a generic failure",
           "No Intel display controller at 00:02.0" in scr, scr[-900:])
        ck("and says why that is expected here",
           "QEMU" in scr, scr[-900:])
        ck("and does not go on to print power wells it never read",
           "PW_1" not in scr, scr[-900:])

        print("\n  the other two screens refuse the same way")
        scr = run("gfx regs")
        ck("`gfx regs` says there is nothing to list",
           "none: no Intel display controller" in scr, scr[-700:])
        scr = run("gfx vbt")
        ck("`gfx vbt` says there is no OpRegion",
           "gave no OpRegion" in scr, scr[-700:])

        print("\n  `gfx edid` (0.9.1) refuses without an engine, writing nothing")
        scr = run("gfx edid")
        ck("it says there is no display engine to read through",
           "No display engine to read through" in scr, scr[-700:])

        print("\n  an argument it does not know prints the usage")
        scr = run("gfx bogus")
        ck("the usage names all four forms",
           "gfx regs" in scr and "gfx vbt" in scr and "gfx edid" in scr and
           "Read only" in scr,
           scr[-700:])

        print("\n  the shell is still answering afterwards")
        scr = run("ver")
        ck("a command after it still works", VER in scr, scr[-300:])
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
