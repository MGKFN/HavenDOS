#!/usr/bin/env python3
"""redraw_hunt.py -- what each screen leaves behind, on the HP's size of grid.

Kyan, 22 Nov, with a photo of the sign-in screen drawn over the Start
menu on 0.9.7: "those kind of bugs are everywhere".

The cause in the photo was a screen laid out for 80x25 on a 128x48
display: it painted its own 80x25 and left the rest of the old screen
where it was. QEMU's harnesses mostly ran 80x25, the one grid where
that cannot show. So this runs 128x50 and LOOKS:

  1. `exit` from the Shell: the account screen covers the whole grid
     (its bottom strip is on the last row; none of the Shell is left)
  2. signing in reaches the desktop
  3. Start -> Lock: the sign-in screen covers the whole grid, nothing
     of the Start menu is left (Kyan's photo)
  4. signing in again: the desktop comes back as it was
  5. the Shell's `cls` clears all of it and puts the prompt at the top
  6. the Shell's `lock` is the same whole-screen sign-in screen

Apps opened from the desktop run in a viewport (include/vga.h) and
cannot leave anything outside it; what could were the screens that run
with none -- these six.

Run:  python3 harness/redraw_hunt.py [path/to.iso]   (foreground)
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
import layout_audit as LA                               # noqa: E402
from mouse_apps import boot_to_shell                    # noqa: E402

ROOT = os.path.dirname(HERE)
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
COLS, ROWS = 128, 50
SKIP = dict(LA.SKIP)
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-1500:].replace("\n", "\n          "))
    return ok


def lines(q):
    s = q.screen(COLS, ROWS).split("\n")
    return (s + [""] * ROWS)[:ROWS]


def body(L):
    # rows 1..ROWS-2: the top bar and the taskbar both carry a clock
    return [l.rstrip() for l in L[1:ROWS - 1]]


def same(x, y):
    """Equal but for the clocks (any digit) and the pointer (a '.' cell)."""
    x, y = x.ljust(len(y)), y.ljust(len(x))
    for a, b in zip(x, y):
        if a == b or "." in (a, b) or (a.isdigit() and b.isdigit()):
            continue
        return False
    return True


def diff(a, b):
    out = []
    for i, (x, y) in enumerate(zip(a, b)):
        if not same(x, y):
            out.append("row %2d was: %s\n       now: %s" % (i + 1, x[:110], y[:110]))
    return out


def startmenu_lock_px(sw=1280, sh=800):
    """The lock button's centre, as shell/startmenu.c lays the menu out
    (geometry()) for this grid: 8x16 cells at 128x50 on 1280x800, so the
    title bar and taskbar are two rows (32 px) and the font 16 px."""
    ch = sh // ROWS if sh // ROWS in (16, 32, 48) else 16
    th = ch if ch >= 24 else 2 * ch
    tr = 1 if ch >= 24 else 2
    bh = sh - (ROWS - tr) * ch
    fp = max(12, min(24, th // 2))
    pad = fp
    pw = min(fp * 40, sw - 2 * pad)
    ph = min(fp * 34, sh - bh - 2 * pad)
    gx0 = (sw - COLS * (8 * (ch // 16))) // 2      # the grid's left edge
    pw = min(fp * 40, COLS * (8 * (ch // 16)) - 2 * pad)
    px0, py0 = gx0 + pad // 2, sh - bh - pad // 2 - ph
    foot = fp * 3
    fy = py0 + ph - foot
    bw = foot - fp // 2
    bx = px0 + pw - pad - 2 * (bw + fp // 4)
    return bx + bw // 2, fy + fp // 4 + bw // 2


def sign_in(q):
    """From the account screen or the sign-in screen, as admin."""
    for _ in range(12):
        s = q.screen(COLS, ROWS)
        if "How would you like to start" in s:
            q.key("down"); time.sleep(0.4); q.key("ret"); time.sleep(2.0); continue
        if "Password:" in s:
            q.type("Admin@123", settle=0.2); time.sleep(0.6); q.key("ret"); time.sleep(4.0); continue
        if "Enter=Select" in s:
            q.key("ret"); time.sleep(1.5); continue
        return True
    return False


def _unused_open_and_close(q, index, name, topnames, base):
    """Start -> All Apps -> name; look while it is open; close; look again."""
    if not LA.open_menu(q):
        return ["the start menu would not open"], base
    if not LA.walk_to(q, topnames, 0, "All Apps", len(topnames) + 8):
        return ["never selected All Apps"], base
    q.key("ret"); time.sleep(1.0)
    if not LA.walk_to(q, LA.NAMES, LA.SUB_X, name.strip(), index + 60):
        return ["never selected it"], base
    q.key("ret")
    for _ in range(16):
        time.sleep(0.25)
        if not LA.menu_is_open(q):
            break
    time.sleep(1.6)
    bugs = []
    L = lines(q)
    if any("Favorites" in l for l in L):
        bugs.append("while open, the Start menu is still on screen:\n" +
                    "\n".join(l.rstrip() for l in L if l.strip())[-900:])
    for k in ("esc", "q", "esc", "spc", "esc"):
        q.key(k); time.sleep(0.45)
    time.sleep(1.2)
    after = body(lines(q))
    d = diff(base, after)
    if d:
        bugs.append("after closing, the desktop differs in %d row(s):\n%s" %
                    (len(d), "\n".join(d[:6])))
    return bugs, after


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    apps = LA.apps_from_source()
    LA.NAMES[:] = sorted((a[1] for a in apps), key=len, reverse=True)
    src = open(os.path.join(ROOT, "shell", "desktop.c")).read()
    tm = src[src.index("static const top_item_t topmenu[]={"):]
    tm = tm[:tm.index("};")]
    topnames = sorted([t.strip() for t in re.findall(r'\{\s*"([^"]*)"', tm)], key=len, reverse=True)

    print("what each screen leaves behind, at %dx%d" % (COLS, ROWS))
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256, extra=["-device", "qemu-xhci,id=xhci",
                                         "-device", "usb-mouse,bus=xhci.0"]) as q:
        boot_to_shell(q)
        q.line("resolution %dx%d" % (COLS, ROWS)); time.sleep(1.5)
        q.key("ret"); time.sleep(0.5)
        q.set_grid(COLS, ROWS)
        for i in range(30):
            q.line("echo SHELL-LEFTOVER-%d" % i); time.sleep(0.12)
        q.line("exit"); time.sleep(4.0)
        L = lines(q)
        check("`exit`: the account screen's bottom strip is on the last row",
              "Enter=Select" in L[ROWS - 1], repr(L[ROWS - 1][:60]))
        check("`exit`: nothing of the Shell is left on screen",
              not any("SHELL-LEFTOVER" in l for l in L),
              "\n".join(l.rstrip() for l in L if "SHELL-LEFTOVER" in l)[:400])

        check("signing in as admin reaches the desktop", sign_in(q))
        time.sleep(2.0)
        # v0.9.8: ESC on the desktop opens the Start menu and ESC in it
        # closes it -- an even number of presses leaves it shut
        for _ in range(2):
            q.key("esc"); time.sleep(0.5)
        base = body(lines(q))

        if True:
            # Start -> Lock, Kyan's photo. v0.9.8: the Start menu is pixels
            # (shell/startmenu.c); its lock button is at the right of its
            # foot, where startmenu_geometry() says.
            import window_apps as W
            q.key("meta_l"); time.sleep(1.5)
            lx, ly = startmenu_lock_px()
            W.click_px(q, lx, ly, settle=3.0)
            L = lines(q)
            check("Start -> Lock: the sign-in screen's strip is on the last row",
                  "Enter=Select" in L[ROWS - 1], "\n".join(l.rstrip() for l in L[-6:]))
            check("Start -> Lock: nothing of the desktop is left (the photo)",
                  not any(w in l for l in L for w in ("RAM free", "LIVE -")),
                  "\n".join(l.rstrip() for l in L[ROWS - 16:]))
            check("signing in again reaches the desktop", sign_in(q))
            time.sleep(2.0)
            d = diff(base, body(lines(q)))
            check("the desktop is as it was before Lock", not d, "\n".join(d[:6]))
            base = body(lines(q))

        # 5/6: the Shell's own `cls` and `lock`, on the whole grid
        # v0.9.8: the desktop's first icon (the Shell) is a tile of pixels
        # over cells 1..15 of rows 2..3 -- clicked there, opened with Enter
        import window_apps as W
        W.click(q, 4, 2, settle=0.6); q.key("ret"); time.sleep(3.0)
        L = lines(q)
        check("the Shell opens from its desktop icon", any("@havendos" in l for l in L),
              "\n".join(l.rstrip() for l in L if l.strip())[-600:])
        for i in range(40):
            q.line("echo CLS-LEFTOVER-%d" % i); time.sleep(0.1)
        q.line("cls"); time.sleep(2.5)
        L = lines(q)
        check("`cls`: nothing it was meant to clear is left (all 128x50)",
              not any("CLS-LEFTOVER" in l for l in L),
              "\n".join(l.rstrip() for l in L if "CLS-LEFTOVER" in l)[:400])
        prompt = [y for y, l in enumerate(L) if "@havendos" in l]
        # v0.9.9: the Terminal is a floating window -- its first line is
        # a few rows down the screen, under its title bar; the prompt is
        # that first line, and the only one
        check("`cls`: the next prompt is the window's first line", prompt and prompt[0] <= 6
              and len(prompt) == 1, prompt)
        q.line("lock"); time.sleep(3.0)
        L = lines(q)
        check("`lock`: the sign-in screen, whole-screen (its strip on the last row)",
              "Enter=Select" in L[ROWS - 1], "\n".join(l.rstrip() for l in L[-5:]))
        check("signing in from `lock` returns to the Shell", sign_in(q) and
              any("@havendos" in l for l in lines(q)),
              "\n".join(l.rstrip() for l in lines(q) if l.strip())[-500:])

    print("\n%d/%d" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
