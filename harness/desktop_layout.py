#!/usr/bin/env python3
"""desktop_layout.py -- does the desktop chrome land correctly on a
console that is NOT 80x25?

WHY A HARNESS AND NOT A NATIVE TEST. Everything here is a coordinate,
and a native test that computes a coordinate from the same macro the
kernel uses agrees with itself by construction. The only judge that is
not ours is the screen, so this boots the real ISO, makes the console
bigger, drives the desktop and reads the cells back.

HOW THE BIG CONSOLE IS OBTAINED, and why not the obvious ways:

  * Not by setting gfxmode in grub.cfg. GRUB honours the mode in the
    kernel's own multiboot header (boot/boot.S) ABOVE gfxmode and
    gfxpayload, so that list never reaches the kernel -- measured in
    v0.8.8.7 by forcing it to 1920x1080 and still booting 1280x800.
  * Not by starving QEMU's VGA memory. That works, but downwards:
    2 MB gives a 640x480 panel, which is a TALLER grid and a narrower
    one, so it cannot show a column-anchoring bug at all.
  * By `resolution max` in the shell, which is a shipping command and
    changes the one thing under test -- the grid -- on the shipping
    kernel, with nothing else different.

This matters because the first version of this harness ran at
1280x800, which at the 16x32 readable cell is EXACTLY 80x25. Every
coordinate landed where its literal said it would and three real bugs
came back clean.

What it looks for, all three reported against v0.8.8.6:

  1. the notification drawer opens in the MIDDLE of the desktop
     instead of under the bell that opens it   (NC_X was a literal 55)
  2. the drawer stops short of the taskbar     (NC_BOT was a literal 23)
  3. a boxed app reads as a stranded 80x25 screen, not a window

Usage:  python3 harness/desktop_layout.py [iso]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu import Qemu, kill_stale

ISO = sys.argv[1] if len(sys.argv) > 1 else "havendos.iso"
OUT = "/tmp/hd_layout"

fails = []
notes = []


def check(name, ok, detail=""):
    print(("  PASS  " if ok else "  FAIL  ") + name)
    if detail and not ok:
        print("        " + detail.replace("\n", "\n        ")[:1600])
    if not ok:
        fails.append(name)


# The readback has to know the grid, and the grid is derived in the
# kernel -- so the harness derives it too, by trying candidates and
# keeping whichever produces real words. Reading a 160-column console
# at 80 columns does not fail; it returns plausible text sampled from
# the wrong cells ("0 4 : 2 3" for the clock), which is the kind of
# near-miss that costs an afternoon.
GRIDS = [(160, 50), (128, 48), (120, 33), (100, 37), (90, 30), (80, 30), (80, 25)]


def best_screen(q):
    best, bestn, bestg = "", -1, (80, 25)
    for c, r in GRIDS:
        q.set_grid(c, r)
        t = q.screen()
        n = len(re.findall(r"[A-Za-z]{3,}", t))
        if n > bestn:
            best, bestn, bestg = t, n, (c, r)
    q.set_grid(*bestg)
    return best, bestg


def typefield(q, s):
    for ch in s:
        q.type(ch)
        time.sleep(0.05)


def walk_wizard(q, user="tester"):
    """Fresh first boot -> desktop. Screen-driven, not a fixed key
    sequence: the wizard is seven steps and has miscounted them."""
    made = 0
    grid = (80, 25)
    for _ in range(90):
        s, grid = best_screen(q)
        if "HAVENDOS" in s and ("NOTIF" in s or "Tap icon" in s):
            return s, grid
        if "havendos:" in s or "havendos>" in s:
            return s, grid

        # login.c's first-run account screen, NOT the wizard's step-4
        # form. Two differences that matter to a script:
        #   * its field is labelled "Username:"; the wizard's says
        #     "Username  (3-15 characters)";
        #   * its inner form is a while(1) with NO ESCAPE. The footer
        #     advertises "ESC=Back", but that belongs to the outer
        #     chooser -- once Create Account is entered the only way
        #     out is an account that validates, and a blank password
        #     is refused here even though the wizard allows one.
        if "Create Account" in s and "Enter=Select" in s:
            typefield(q, "layoutqa")
            q.key("ret"); time.sleep(0.9)
            typefield(q, "Test@123")
            q.key("ret"); time.sleep(0.9)
            typefield(q, "Test@123")
            q.key("ret"); time.sleep(1.2)
            q.type("y"); time.sleep(1.6)
            continue

        if ("Create Account" in s or "Create your user account" in s) and made < 3:
            # NO leading backspaces. The other harness clears the field
            # with 20 of them first; here that walks focus backwards out
            # of the form and nothing typed afterwards lands, which
            # reads exactly like "typing does not work" and is not.
            typefield(q, user)
            q.key("ret"); time.sleep(0.9)
            pw = "" if made == 0 else "Test@123"
            if pw:
                typefield(q, pw)
            q.key("ret"); time.sleep(0.9)
            if pw:
                typefield(q, pw)
            q.key("ret"); time.sleep(1.2)
            if "Admin?" in best_screen(q)[0]:
                q.type("y"); time.sleep(1.2)
            made += 1
            continue

        if ("Scroll to the end" in s or "Terms" in s or "Policy" in s
                or "Licence" in s or "License" in s):
            q.key("end"); time.sleep(1.0); q.key("a"); time.sleep(1.3)
            continue

        if "Username" in s and made:
            typefield(q, user)
            q.key("ret"); time.sleep(1.0)
            q.key("ret"); time.sleep(2.0)
            continue

        q.key("ret"); time.sleep(1.0)
    return None, grid


def row_of(screen, needle):
    for i, line in enumerate(screen.split("\n")):
        if needle in line:
            return i, line
    return -1, ""


def main():
    kill_stale()
    q = Qemu(ISO, mem=512, extra=["-vga", "std"]).start()
    try:
        print("[1/5] booting and walking the setup wizard")
        s, (cols, rows) = walk_wizard(q)
        if s is None:
            check("reached the desktop", False, best_screen(q)[0])
            return 1
        print("      desktop reached at %dx%d" % (cols, rows))

        print("[2/5] widening the console from the Shell (`resolution max`)")
        q.key("ret"); time.sleep(2.5)              # icon 0 is the Shell
        s = best_screen(q)[0]
        if "havendos:" not in s and "#" not in s:
            check("the Shell opened", False, s)
            return 1
        q.line("resolution max"); time.sleep(2.5)
        # `resolution` asks "Keep this mode? Press ENTER to confirm"
        # and reverts after ten seconds if nobody does. Without this
        # the next command is typed INTO that prompt -- "layout" lost
        # its first three characters to it and the shell then reported
        # "'out' command not found", which is a confusing way to learn
        # that a confirmation exists.
        q.key("ret"); time.sleep(1.5)
        # NOT the `layout` audit here. It opens all 46 apps one at a
        # time and takes minutes; running it inside this harness meant
        # every check below ran while Tetris was still on screen. It
        # is a shell command and belongs in its own run.
        #
        # ESC is a key the shell READS, not a way out of it; the Shell
        # app ends on `exit`.
        q.line("exit"); time.sleep(3.0)            # back to the desktop
        for _ in range(6):
            if "NOTIF" in best_screen(q)[0]:
                break
            q.key("esc"); time.sleep(1.2)
        s, (cols, rows) = best_screen(q)
        print("      console is now %dx%d" % (cols, rows))
        check("the console is bigger than 80x25 (or this test proves nothing)",
              cols > 80 or rows > 25,
              "grid read back as %dx%d -- `resolution max` did not take, so "
              "every coordinate below would land where its literal says it "
              "does and the test would pass without testing anything"
              % (cols, rows))
        q.shot(OUT + "-desktop.ppm")


        print("[3/5] opening the notification drawer with N")
        q.key("n"); time.sleep(1.8)
        s = best_screen(q)[0]
        q.shot(OUT + "-notif.ppm")
        r, line = row_of(s, "Notifications")
        check("the drawer is on screen", r >= 0, s)
        if r >= 0:
            x = line.index("Notifications")
            check("the drawer is anchored to the right edge, not floating "
                  "mid-desktop",
                  x >= cols - 30,
                  "'Notifications' starts at column %d of %d. The bell that "
                  "opens it is painted at column %d, so the drawer opened "
                  "%d columns away from its own control."
                  % (x, cols, cols - 25, (cols - 25) - x))
            # And it should reach down to the taskbar rather than
            # stopping at the literal row 23.
            close_r, _ = row_of(s, "Any key=Close")
            check("the drawer reaches the taskbar instead of stopping at "
                  "row 23",
                  close_r >= rows - 4,
                  "its last row is %d of %d; the taskbar is row %d"
                  % (close_r, rows, rows - 1))
        q.key("esc"); time.sleep(1.8)       # the drawer closes on any key

        print("[4/5] opening a boxed app and looking for a window frame")
        # Via the START MENU, not the desktop icons. The desktop has
        # exactly ONE icon on it by default -- the Shell -- so Right
        # and Down had nowhere to move and Enter kept opening icon 0.
        # The Shell is also the one app in appreg marked fits=1: it
        # derives its rows from vga_rows() and is deliberately never
        # boxed, so opening it would have proved nothing either way.
        #
        # ESC opens the start menu (it is not a "cancel" on the
        # desktop). The FIRST category is Favorites and the only thing
        # in it is the Shell, which is why ESC-Enter-Down-Enter kept
        # opening the Shell and looking like broken navigation. Down
        # once moves to "Apps"; inside it, down once moves off the
        # Shell to the Text Editor, which lays out for 80x25 and
        # therefore gets boxed.
        q.key("esc");   time.sleep(1.2)
        q.key("down");  time.sleep(0.8)      # Favorites -> Apps
        q.key("ret");   time.sleep(1.2)      # open the Apps submenu
        q.key("down");  time.sleep(0.8)      # Shell -> Text Editor
        q.key("ret");   time.sleep(3.0)
        s = best_screen(q)[0]
        q.shot(OUT + "-app.ppm")
        check("the boxed app is given a window title bar saying how to "
              "leave", "ESC closes" in s, s)
        check("the boxed app is NOT the Shell (which is never boxed)",
              "HavenDOS Shell v" not in s,
              "icon navigation did not move off icon 0")
        q.key("esc"); time.sleep(2.5)       # the app's own ESC=Close

        print("[5/5] clicking the taskbar notification cluster")
        q.click(cols - 24, rows - 1)
        time.sleep(1.8)
        q.shot(OUT + "-bellclick.ppm")
        s = best_screen(q)[0]
        check("clicking the bell opens the drawer",
              "Notifications" in s,
              "clicked column %d of row %d and the drawer did not open"
              % (cols - 24, rows - 1))
    finally:
        q.stop()

    print()
    for n in notes:
        print(n)
    if fails:
        print("desktop_layout: %d FAILED -- %s" % (len(fails), ", ".join(fails)))
        return 1
    print("desktop_layout: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
