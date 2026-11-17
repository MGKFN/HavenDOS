#!/usr/bin/env python3
"""retro_boot.py -- v0.9.7: K-DOS Beta 1.1b, inside HavenDOS, in QEMU.

tests/test_emu86.c boots K-DOS natively on the build machine. This is
the same thing on the real kernel: the ISO's live shell, `retro`, the
list, Enter, and then K-DOS's own screens read back off the framebuffer
-- an 8086 emulator running in a 32-bit kernel running in QEMU.

  1. `retro`: the Retro PC list, with K-DOS Beta 1.1b built in
  2. Enter: K-DOS's login screen, in the window
  3. admin / 1234: K-DOS's shell and its command bar
  4. help and time answer as they do under a real BIOS
  5. F4 leaves, and HavenDOS's shell is back

Run:  python3 harness/retro_boot.py [path/to.iso]   (foreground)
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
from mouse_apps import boot_to_shell                    # noqa: E402

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "havendos.iso")
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-2400:].replace("\n", "\n          "))
    return ok


def keys(q, s):
    for ch in s:
        if ch == "\n":
            q.key("ret")
        else:
            q.type(ch)
        time.sleep(0.12)
    time.sleep(0.8)


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    print("Retro PC: K-DOS Beta 1.1b inside HavenDOS")
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256) as q:
        boot_to_shell(q)
        g = q.screen()
        cols = max(len(l) for l in g.splitlines()) if g else 80
        q.line("retro"); time.sleep(2.5)
        s = q.screen()
        check("`retro` opens the list, K-DOS built in",
              "Retro PC" in s and "K-DOS Beta 1.1b" in s, s)
        check("the list says where its disk lives", "RETRO/KDOS11B" in s, s)

        q.key("ret"); time.sleep(3.0)
        s = q.screen()
        check("Enter boots it: K-DOS's login screen", "K-DOS-Beta-1.1b SECURITY" in s and "User:" in s, s)

        keys(q, "admin\n"); keys(q, "1234\n")
        s = q.screen()
        check("admin / 1234: K-DOS's shell", "Active Shell." in s and "K:\\SYSTEM32>" in s, s)
        check("its command bar", "TIME | CALC | USER | TASKMAN" in s, s)

        keys(q, "help\n")
        s = q.screen()
        check("help", "Commands: time, calc, user, taskman, sfc, fdisk" in s, s)
        keys(q, "time\n")
        s = q.screen()
        check("time: an hour and minutes off the RTC through INT 1Ah",
              any(l.find("K:\\SYSTEM32> time") >= 0 and len(l.split("time", 1)[1].strip()) >= 4
                  for l in s.splitlines()), s)

        q.key("f4"); time.sleep(2.0)
        s = q.screen()
        check("F4 leaves the old PC, back to the list", "Retro PC" in s and "Active Shell." not in s, s)
        q.type("q"); time.sleep(2.0)
        # page with the space bar until the pager stops (build notes:
        # any other key is a command name at the prompt)
        q.line("dmesg"); time.sleep(2.0)
        seen = ""
        for _ in range(20):
            s = q.screen(); seen += s
            if "retro: booting K-DOS Beta 1.1b" in s or "-- more --" not in s:
                break
            q.key("spc"); time.sleep(1.0)
        check("the kernel log says what it booted", "retro: booting K-DOS Beta 1.1b" in seen, s)

    print("\n%d/%d" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
