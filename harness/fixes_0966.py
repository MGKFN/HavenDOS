#!/usr/bin/env python3
"""fixes_0966.py -- what Kyan's 0.9.6.5 test on the HP found, in QEMU.

  1. The Shell's X closes it. It arrived as ESC, and the Shell's ESC
     clears the line, so on the HP the X did nothing.
  2. Full screen FILLS for an 80x25 app: the console becomes 80x25
     (QEMU's 1280x800 takes it at a whole scale; the HP's 1024x768 at
     12x24, which tests/test_console.c proves). Alt+Enter brings the
     window back at the person's grid.
  3. A mouse unplugged and plugged back in is bound again ("USB mouse"
     after the hot-plug rescan); before 0.9.6.6 the rescan never re-ran
     usb_hid_init() and the mouse never moved again.
  4. A USB stick plugged in after that comes back as a disk.

Run:  python3 harness/fixes_0966.py [path/to.iso]   (foreground)
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
import window_apps as W                                 # noqa: E402
from mouse_apps import boot_to_shell                    # noqa: E402

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "havendos.iso")
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-900:].replace("\n", "\n          "))
    return ok


def dmesg(q, pat):
    """Every line of `dmesg | grep -i pat`, across pages. dmesg pages its
    own output before the pipe sees it once the log is longer than the
    screen (recorded in HANDOFF), so the pages are walked with the space
    bar and each screen kept; only the matching lines are returned."""
    q.line("dmesg | grep -i %s" % pat); time.sleep(2.0)
    seen = []
    for _ in range(12):
        s = q.screen(128, 50)
        seen.append(s)
        if "-- more --" not in s:
            break
        q.key("spc"); time.sleep(0.8)
    lines = []
    for s in seen:
        for l in s.split("\n"):
            if pat.lower() in l.lower() and l.strip() not in lines and "grep" not in l:
                lines.append(l.strip())
    return "\n".join(lines)


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    print("0.9.6.6: the Shell's X, full screen that fills, mice and sticks plugged back in")
    for n in (1, 2):
        os.system("rm -f /tmp/f966_stick%d.img; dd if=/dev/zero of=/tmp/f966_stick%d.img bs=1M count=32 "
                  "status=none; mkfs.vfat -n STICK%d /tmp/f966_stick%d.img >/dev/null" % (n, n, n, n))
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256, extra=["-device", "qemu-xhci,id=xhci",
                                         "-device", "usb-mouse,bus=xhci.0,id=m1",
                                         "-drive", "if=none,id=stk1,file=/tmp/f966_stick1.img,format=raw",
                                         "-drive", "if=none,id=stk2,file=/tmp/f966_stick2.img,format=raw"]) as q:
        boot_to_shell(q)
        q.line("resolution 128x50"); time.sleep(1.5)
        q.key("ret"); time.sleep(1.0)

        # 1. the Shell's X
        q.line("open shell"); time.sleep(3.0)
        L = W.lines(W.scr(q))
        check("an opened Shell has a title bar", L[0].startswith("  Shell") and L[0].rstrip().endswith("X"), L[0])
        check("  ... which no longer promises 'ESC closes'", "ESC closes" not in L[0], L[0])
        bx = L[0].rstrip().rfind("X")
        W.click(q, bx, 0, settle=2.5)
        L = W.lines(W.scr(q))
        check("its X closes it: the boot console is back", L[0].startswith("admin@havendos") or
              "Shell" not in L[0], L[0])

        # 2. full screen fills
        q.line("open 2048"); time.sleep(3.0)
        L = W.lines(W.scr(q))
        ty = [i for i, l in enumerate(L) if "2048" in l and l.rstrip().endswith("X")]
        check("2048 opens boxed, with its title bar", bool(ty), "\n".join(L[:15]))
        if ty:
            bx = L[ty[0]].rstrip().rfind("X")
            W.click(q, bx - 4, ty[0], settle=2.5)
            s = q.screen(80, 25).split("\n")
            check("full screen: the console is 80x25 and the game starts on row 0",
                  s[0].lstrip().startswith("2048") and "Score" in s[0], s[0])
            q.key("alt", "ret"); time.sleep(2.0)
            L = W.lines(W.scr(q))
            check("Alt+Enter: back at 128x50, boxed, with the taskbar",
                  any("2048" in l and l.rstrip().endswith("X") for l in L) and "Start" in L[49], L[49])
            q.key("q"); time.sleep(1.5)

        # 3 and 4. plugged back in
        q.cmd_qmp("device_add", driver="usb-storage", bus="xhci.0", drive="stk1", id="s1"); time.sleep(5)
        q.cmd_qmp("device_del", id="m1"); time.sleep(5)
        q.cmd_qmp("device_del", id="s1"); time.sleep(5)
        q.cmd_qmp("device_add", driver="usb-mouse", bus="xhci.0", id="m2"); time.sleep(5)
        s = dmesg(q, 'mouse')
        # the boot binds it once; the rescan after the replug must bind it
        # again -- before 0.9.6.6 it never did, and the count stayed at 1
        check("the mouse plugged back in is bound again (a second 'USB mouse' line)",
              s.count("USB mouse") >= 2, s)
        q.cmd_qmp("device_add", driver="usb-storage", bus="xhci.0", drive="stk2", id="s2"); time.sleep(6)
        s = dmesg(q, 'NICs')
        last = [l for l in s.split("\n") if "after hot-plug" in l]
        check("  ... and a stick plugged in after it is a disk again",
              bool(last) and "2 devices, 1 disk" in last[-1], s)
        s = dmesg(q, 'mouse')
        check("  ... with the mouse bound again after that rescan too", s.count("USB mouse") >= 3, s)

    passed = sum(results)
    print("\n%d/%d checks passed" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
