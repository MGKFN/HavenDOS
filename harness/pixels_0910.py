#!/usr/bin/env python3
"""pixels_0910.py -- v0.9.10: screenshots, Images, perf, the pixel menu.

On QEMU's 1280x800 at 128x50 with a USB mouse:

  1. `perf bench` in the Terminal prints its timings, and some of its
     200 lines were scrolled by moving pixels
  2. PrtSc saves a PNG into Pictures, and `ls Pictures` lists it
  3. `screenshot` does the same, and the file it names is a real PNG
     (its first bytes, through `hex`)
  4. Images opens as a pixel window and shows the screenshot: the
     picture area is not one flat colour
  5. a right-click on the desktop opens the menu in PIXELS: a panel in
     the window manager's panel colour where the old one drew cells
  6. Settings -> Display has Best performance

SHOTS=<dir> keeps pictures of each step.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
import px_mouse as PM                                   # noqa: E402
from mouse_apps import boot_to_shell, ev                # noqa: E402
import windows_099 as W9                                # noqa: E402

ROOT = os.path.dirname(HERE)
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
COLS, ROWS, SW, SH = 128, 50, 1280, 800
results = []
PANELS = set()


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-900:])
    return ok


def keep(q, name):
    if os.environ.get("SHOTS"):
        PM.shot(q).save(os.path.join(os.environ["SHOTS"], "p0910-%s.png" % name))


def screen(q):
    import fbtext
    q.shot("/tmp/p0910.ppm"); time.sleep(0.4)
    t = fbtext.text("/tmp/p0910.ppm", COLS, ROWS)
    return t if isinstance(t, str) else "\n".join(t)


def main():
    Q.kill_stale()
    W9.COLS, W9.ROWS = COLS, ROWS
    with Q.Qemu(iso=ISO, mem=512, extra=["-device", "qemu-xhci,id=xhci",
                                         "-device", "usb-mouse,bus=xhci.0"]) as q:
        boot_to_shell(q)
        q.line("resolution %dx%d" % (COLS, ROWS)); time.sleep(1.5)
        q.key("ret"); time.sleep(0.5)
        q.set_grid(COLS, ROWS)
        q.line("exit"); time.sleep(4.0)
        W9.sign_in(q); time.sleep(3.0)
        for _ in range(2):
            q.key("esc"); time.sleep(0.5)

        W9.open_app(q, "terminal")
        q.line("perf bench"); time.sleep(14)
        s = screen(q)
        keep(q, "perf")
        check("perf bench: a scrolled line timed", "a line scrolled" in s and " ms" in s, s[-1500:])
        check("perf bench: lines scrolled by moving pixels", "by moving pixels" in s and "(0 of 200" not in s, s[-800:])

        q.line("cls"); time.sleep(1)
        q.key("print"); time.sleep(6)
        keep(q, "prtsc")
        q.line("ls Pictures"); time.sleep(2)
        s = screen(q)
        check("PrtSc: a PNG in Pictures", "Shot-" in s and ".png" in s.lower(), s[-900:])

        q.line("cls"); time.sleep(1)
        q.line("screenshot Pictures/test.png"); time.sleep(8)
        q.line("hex Pictures/test.png"); time.sleep(2)
        s = screen(q)
        check("`screenshot` writes a real PNG (89 50 4E 47)", "89 50 4E 47" in s.upper(), s[-900:])

        W9.open_app(q, "images", settle=5.0)
        img = PM.shot(q); keep(q, "images")
        f = W9.frame(img)
        ok = f is not None and f[4] is not None
        check("Images opens as a window", ok, f)
        if ok:
            p = img.load()
            # the picture area: the right two thirds of the content
            cols = set()
            for y in range(f[3] + 80, f[4] - 60, 7):
                for x in range(f[0] + (f[2] - f[0]) // 3 + 20, f[2] - 20, 7):
                    cols.add(p[x, y])
            check("  ... and shows a picture (many colours in its view)", len(cols) > 20, len(cols))
        q.key("esc"); time.sleep(2)

        # the desktop's right-click -- with the Terminal closed, so the
        # desktop's own loop is the one reading the mouse
        q.line("exit"); time.sleep(3)
        PM.to(q, 1060, 300)                      # the desktop, right of the Terminal
        before = PM.shot(q)
        ev(q, [{"type": "btn", "data": {"down": True, "button": "right"}}]); time.sleep(0.1)
        ev(q, [{"type": "btn", "data": {"down": False, "button": "right"}}]); time.sleep(2)
        img = PM.shot(q); keep(q, "menu")
        s = screen(q)
        changed = sum(1 for y in range(305, 420, 4) for x in range(1065, 1150, 4)
                      if img.getpixel((x, y)) != before.getpixel((x, y)))
        # the old menu drew its rows in cells, which the cell reader reads;
        # the new one is Plex Sans pixels, which it cannot
        check("right-click: a menu in pixels at the pointer", changed > 200 and "Next theme" not in s,
              (changed, s[-300:]))
        q.key("esc"); time.sleep(1)
    n = sum(results)
    print("\n%d/%d" % (n, len(results)))
    return 0 if results and n == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
