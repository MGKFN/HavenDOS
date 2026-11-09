#!/usr/bin/env python3
"""layout_audit.py -- which apps draw outside the screen, and by how much.

Kyan, after running beta 5 on the HP:

    "some apps still go off the screen and aren't show fully maybe bc
     of 16:10???"

THERE ARE FORTY-SIX APPS. Guessing which ones is how a release fixes
the wrong three and ships the report still open, and this project has
done that before -- "the polling still doesn't rlly work" went two
releases because the answer was assumed instead of measured.

So this measures. v0.8.8.5 counts every write the console clips in
vga_write() -- the one function every cell goes through -- and
shell/desktop.c resets the counter before launching an app and logs a
line afterwards naming the app and the worst coordinate it reached.
This harness opens every app in turn and then reads the log.

── WHY IT NAVIGATES THE START MENU ───────────────────────────────────

Because the reporting lives in desktop_run_app(), and that is the only
path that knows which app is running. Many apps are also shell
commands, but the shell is `fits=1` and runs with no viewport and no
per-app accounting, so running `paint` from a prompt measures nothing.

"All Apps" is a flat list in registry order (reg_fill(), TOP_ALL), so
app number N is N presses of DOWN. That is the only reliable way to
address forty-six apps without a launcher command that does not exist.

── WHAT IS SKIPPED, AND WHY ──────────────────────────────────────────

  power      "Power and Reboot" -- it reboots the machine, which ends
             the audit and loses the log.
  install    "Install HavenDOS" -- it writes to disks. In a live
             session it would be writing to QEMU's, which is this
             harness's own ISO, but it is skipped on principle: an
             audit must not be able to install an operating system.

Both are recorded as skipped rather than quietly left out, because a
list of forty-four results where forty-six were expected is a list
somebody has to go and count.

Run:  python3 harness/layout_audit.py [path/to.iso]     (foreground)
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

SKIP = {"power": "it reboots the machine",
        "install": "it writes to disks"}

# Longest first, so "System Info" cannot match before "System Info Plus".
NAMES = []


def apps_from_source():
    """The registry, in the order 'All Apps' lists it."""
    src = open(os.path.join(ROOT, "shell", "desktop.c")).read()
    i = src.index("static const appreg_t appreg[]={")
    j = src.index("};", i)
    rows = re.findall(
        r'\{"([a-z0-9]+)",\s*"([^"]+)",\s*"..",\s*\w+,\s*CAT_\w+(?:,\s*(\d))?\s*\}',
        src[i:j])
    return [(a, b, int(c or 0)) for a, b, c in rows]


def boot_to_desktop(q):
    time.sleep(6)
    q.key("down"); time.sleep(0.4)      # GRUB entry 2: Shell Mode
    q.key("ret")
    if q.wait_for("System ready", 180) is None:
        raise RuntimeError("the boot log never completed")
    if q.wait_for("admin@havendos", 90) is None:
        q.key("ret")
        if q.wait_for("admin@havendos", 60) is None:
            raise RuntimeError("the shell prompt never appeared")
    time.sleep(1.0)

    q.line("exit")
    time.sleep(2.5)
    for _ in range(14):
        scr = q.screen()
        if "How would you like to start" in scr or "Create new account" in scr:
            q.key("ret"); time.sleep(2.0); continue
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
        if "TechHaven Studios" not in scr:
            return True
        q.key("esc"); time.sleep(1.5)
    return False


MENU_MARK = "Favorites"          # only the start menu's top panel draws this

# The flyout's '>' cursor sits at column SUB_X, which is MENU_W. Read
# from the source so a change to the panel width cannot quietly make
# this harness read the wrong column.
def _menu_w():
    src = open(os.path.join(ROOT, "shell", "desktop.c")).read()
    m = re.search(r'^#define MENU_W\s+(\d+)', src, re.M)
    if not m:
        sys.exit("layout_audit: MENU_W is gone from shell/desktop.c")
    return int(m.group(1))


SUB_X = _menu_w()


def menu_is_open(q):
    return MENU_MARK in q.screen()


def open_menu(q):
    """Get the start menu open, from WHATEVER the desktop is showing.

    Not one keypress. The desktop has four menu states and the Windows
    key toggles: from a context menu it CLOSES that and opens nothing.
    A probe run found the desktop sitting under a context menu left by
    the login sequence, so the single meta_l closed that, seven DOWNs
    moved the icon selection, and ENTER launched the Shell -- while
    the harness believed it was walking a flyout.

    So: press and check, up to a few times. Self-correcting beats
    counting.
    """
    for _ in range(6):
        if menu_is_open(q):
            return True
        q.key("meta_l")
        time.sleep(0.8)
    return False


def row_selection(q, names, column):
    """The entry marked '>' at `column`, matched against `names`.

    The column matters: when the flyout is open BOTH menus draw a
    cursor, and the top menu's " Settings" would otherwise match the
    app called "Settings". draw_submenu() puts the flyout's cursor at
    SUB_X and the top panel's at column 1, so the column tells them
    apart exactly.
    """
    for line in q.screen().split("\n"):
        if len(line) <= column or line[column] != ">":
            continue
        after = line[column + 1:].lstrip()
        for nm in names:
            if after.startswith(nm):
                return nm
    return None


def walk_to(q, names, column, target, limit):
    """Press DOWN until the selection IS `target`, reading it back.

    ── WHY IT READS RATHER THAN COUNTS ───────────────────────────────

    The first version pressed DOWN a fixed number of times at 90 ms
    intervals and trusted the count. It does not hold: the desktop
    repaints all 2000 cells and flushes on every keypress, so keys
    sent faster than it can draw are DROPPED. A probe that asked for
    app 14 opened app 7 -- exactly half the presses had landed -- and
    the audit had been opening whichever app it reached while
    reporting the name it had asked for.
    """
    for _ in range(limit):
        if row_selection(q, names, column) == target:
            return True
        q.key("down")
        time.sleep(0.30)
    return False


def open_app(q, index, name, topnames):
    """Start menu -> All Apps -> `name`. Verified at every step."""
    if not open_menu(q):
        return "the start menu would not open"

    before = q.screen()

    # Column 0: draw_menu() puts the top panel's cursor at column 0 and
    # the name at column 1 -- the name itself carries a leading space.
    # Column 1 was the first guess and read None every time, because
    # column 1 is that space. Every row also has a second '>' at
    # MENU_W-2, which is why the column is matched exactly rather than
    # searched for.
    if not walk_to(q, topnames, 0, "All Apps", len(topnames) + 8):
        return "never selected All Apps (on %r)" % row_selection(
            q, topnames, 0)
    q.key("ret"); time.sleep(1.0)

    if not walk_to(q, NAMES, SUB_X, name.strip(), index + 60):
        return "never selected it (on %r)" % row_selection(q, NAMES, SUB_X)

    q.key("ret")

    # ── DID IT ACTUALLY OPEN? ─────────────────────────────────────────
    # The start menu's marker disappearing is the signal: an app takes
    # the whole screen, so "Favorites" goes away.
    opened = False
    for _ in range(16):
        time.sleep(0.25)
        if not menu_is_open(q):
            opened = True
            break
    if not opened:
        return "nothing opened -- the menu is still up"

    time.sleep(1.4)
    if q.screen() == before:
        return "the screen did not change"

    # Out again. Several keys because apps differ: ESC for most, q for
    # the games, a key for anything waiting on one.
    for k in ("esc", "q", "esc", "spc", "esc"):
        q.key(k); time.sleep(0.45)
    time.sleep(0.6)
    return None


def main():
    if not os.path.exists(ISO):
        sys.exit(f"layout_audit: no ISO at {ISO} -- run make first")

    apps = apps_from_source()
    NAMES[:] = sorted((a[1] for a in apps), key=len, reverse=True)
    print(f"Auditing {len(apps)} apps in {os.path.basename(ISO)}.\n"
          "Every write the console clips is counted in vga_write() and\n"
          "logged per app by desktop_run_app(). This opens each one and\n"
          "then reads the log -- a measurement, not a theory.\n")

    # How many DOWN presses reach "All Apps": read off topmenu[] in the
    # source rather than counted by hand, so adding a category does
    # not silently make this harness audit the wrong list.
    src = open(os.path.join(ROOT, "shell", "desktop.c")).read()
    tm = src[src.index("static const top_item_t topmenu[]={"):]
    tm = tm[:tm.index("};")]
    topnames = [t.strip() for t in re.findall(r'\{\s*"([^"]*)"', tm)]
    topnames = sorted(topnames, key=len, reverse=True)
    if "All Apps" not in topnames:
        sys.exit("layout_audit: topmenu[] has no 'All Apps' row any more")
    print(f"  top menu: {len(topnames)} rows, cursor at column 0; "
          f"flyout cursor at column {SUB_X}\n")

    kill_stale()
    q = Qemu(ISO, usb=True).start()
    try:
        if not boot_to_desktop(q):
            raise RuntimeError("never reached the desktop")
        print("  at the desktop\n")
        failed = []

        for n, (aid, name, fits) in enumerate(apps):
            if aid in SKIP:
                print(f"  {n:3d} {name:<18} SKIPPED -- {SKIP[aid]}")
                continue
            why = open_app(q, n, name, topnames)
            if why:
                failed.append((name, why))
                print(f"  {n:3d} {name:<18} NOT OPENED -- {why}")
            else:
                print(f"  {n:3d} {name:<18} opened"
                      + ("  (fits=1, no viewport)" if fits else ""))

        # ── and now the log ──────────────────────────────────────────
        for _ in range(3):
            q.key("esc"); time.sleep(0.4)
        q.key("tab"); time.sleep(2.0)         # the Shell
        if "@havendos" not in q.screen():
            raise RuntimeError("could not reopen the shell to read dmesg")

        # ── THE AUDIT IS ONLY WORTH ITS COVERAGE ─────────────────────
        if failed:
            print(f"\n  WARNING: {len(failed)} app(s) never opened. The "
                  "result below is\n  not an audit of those, and a clean "
                  "result is not a clean result.")
        else:
            print(f"\n  all {len(apps) - len(SKIP)} audited apps really "
                  "opened")

        print("\n── what drew outside its console ──────────────────────")
        q.line("dmesg | grep layout")
        time.sleep(2.5)
        found = []
        for _ in range(40):
            scr = q.screen()
            for line in scr.split("\n"):
                m = re.search(r"layout: (.+?) drew (\d+) cell\(s\) outside "
                              r"its (\d+)x(\d+) console, as far as "
                              r"column (-?\d+) row (-?\d+)", line)
                if m and m.group(1) not in [f[0] for f in found]:
                    found.append((m.group(1), int(m.group(2)),
                                  int(m.group(5)), int(m.group(6))))
            if "-- more --" in scr:
                q.key("spc"); time.sleep(1.0)
            else:
                break

        if not found:
            print("  nothing. Every app that opened stayed inside its "
                  "console.")
        else:
            print(f"  {len(found)} app(s) drew outside:\n")
            for nm, cells, mx, my in sorted(found, key=lambda f: -f[1]):
                print(f"    {nm:<20} {cells:>6} cell(s)   "
                      f"worst column {mx}, row {my}")
        return 0 if not found else 1
    finally:
        q.stop()


if __name__ == "__main__":
    sys.exit(main())
