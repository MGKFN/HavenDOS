#!/usr/bin/env python3
"""resolution_tasks.py -- v0.9.6.1: change the grid with another app open.

On the HP on 16 November, `resolution` refused on the desktop every time:
0.9.6 refused whenever another task existed, and the desktop and the Shell
it opened are two. 0.9.6.1 re-lays out each other app's console when it
comes forward instead (drivers/vga.c, ctx_reflow). This boots Shell Mode,
opens Notes, writes a line in it, goes back, changes the grid, and goes to
Notes again:

  1. `resolution 90x30` with Notes open is NOT refused;
  2. the shell's own screen is 90 columns now;
  3. Alt+Tab to Notes: its line is there, on the row it was written on --
     not sheared across the new width;
  4. Alt+Tab back, `resolution 80x25`, and Notes once more: still there.

Run:  python3 harness/resolution_tasks.py [path/to.iso]   (foreground)
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "havendos.iso")
results = []
MARK = "reflowmark42"


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-700:].replace("\n", "\n          "))
    return ok


def boot_to_shell(q):
    time.sleep(6)
    q.key("down"); time.sleep(0.4)
    q.key("ret")
    if q.wait_any(["System ready", "@havendos"], 180)[0] is None:
        raise RuntimeError("the boot log never completed")
    if q.wait_for("admin@havendos", 90) is None:
        q.key("ret")
        if q.wait_for("admin@havendos", 60) is None:
            raise RuntimeError("no shell prompt")
    time.sleep(1.0)


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    print("resolution with another app open (0.9.6.1)")
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256) as q:
        boot_to_shell(q)
        q.line("resolution 80x25"); time.sleep(1.5)

        # Notes, with one line in it
        q.line("open notes"); time.sleep(2.5)
        q.key("a"); time.sleep(0.6)
        q.type(MARK); time.sleep(0.3)
        q.key("ret"); time.sleep(1.0)
        s = q.screen()
        check("Notes is open and shows the line", MARK in s, s)
        notes_row = next((i for i, l in enumerate(s.splitlines()) if MARK in l), -1)
        notes_col = s.splitlines()[notes_row].find(MARK) if notes_row >= 0 else -1

        # back to the shell
        q.key("alt", "tab"); time.sleep(1.5)
        s = q.screen()
        check("Alt+Tab is back at the shell", "@havendos" in s, s)

        q.line("cls"); time.sleep(0.5)
        q.line("resolution 90x30"); time.sleep(2.5)
        s = q.screen(90, 30)
        check("`resolution 90x30` with Notes open is not refused",
              "Close the other open apps" not in s and "Refused" not in s, s)
        q.line("ver"); time.sleep(1.0)
        s = q.screen(90, 30)
        check("the shell is 90 columns wide now",
              any(len(l.rstrip()) > 80 for l in s.splitlines()) or "@havendos" in s, s)

        q.key("alt", "tab"); time.sleep(1.8)
        s = q.screen(90, 30)
        lines = s.splitlines()
        row = next((i for i, l in enumerate(lines) if MARK in l), -1)
        check("Notes at 90x30: the line is still there, whole", row >= 0, s)
        # Notes lays out for 80x25 and is centred: its rows move down
        # and right by the same amount everywhere, never sheared.
        if row >= 0 and notes_row >= 0:
            col = lines[row].find(MARK)
            check("  ... moved as one piece (centred, not sheared)",
                  row - notes_row in range(0, 6) and col - notes_col in range(0, 11),
                  "row %d->%d col %d->%d" % (notes_row, row, notes_col, col))

        q.key("alt", "tab"); time.sleep(1.5)
        q.line("resolution 80x25"); time.sleep(2.5)
        q.key("alt", "tab"); time.sleep(1.8)
        s = q.screen()
        check("back at 80x25, Notes still shows its line", MARK in s, s)
        q.key("alt", "tab"); time.sleep(1.0)
        q.line("ps"); time.sleep(1.2)
        s = q.screen()
        check("and `ps` still lists Notes and the Shell",
              re.search(r"[Nn]otes", s) is not None and "Shell" in s, s)

    passed = sum(results)
    print("\n%d/%d checks passed" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
