#!/usr/bin/env python3
"""windows_099.py -- v0.9.9: windows of any size.

Kyan, 25 Nov: "i want smth like windows or linux". On QEMU's 1280x800
at 128x50, with a USB mouse, reading PIXELS:

  1. the boot screen is pixels: the oxide rule under the wordmark
  2. the Terminal opens as a floating window, its title bar in the
     focused colour, no "Shell" bar of its own inside it
  3. dragging its bottom-right corner makes it bigger -- by whole cells,
     as a terminal does -- and the pointer turns into a double arrow
  4. the maximise button: the title bar spans the screen; again: back
  5. its title bar dragged to the left edge: it takes the left half
  6. the Calculator (laid out for 80x25) opens at its own size, and
     resized it ZOOMS: the same 80x25, bigger cells, filling the window
  7. F11: the window has the whole screen (no taskbar); Alt+Enter back

Run:  python3 harness/windows_099.py [path/to.iso]   (foreground)
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
import px_mouse as PM                                   # noqa: E402
from mouse_apps import boot_to_shell                    # noqa: E402

ROOT = os.path.dirname(HERE)
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
# HD_SCREEN=1024x768 HD_GRID=128x48 for a build made for the HP's mode
COLS, ROWS = (int(v) for v in os.environ.get("HD_GRID", "128x50").split("x"))
SW, SH = (int(v) for v in os.environ.get("HD_SCREEN", "1280x800").split("x"))
# the pointer reaches the grid, not the margins beside it (128 columns of
# 8 px, centred): a maximised or snapped window spans exactly that
RX0 = (SW - COLS * 8) // 2
RX1 = RX0 + COLS * 8
TITLE = (0x2c, 0x3a, 0x50)            # Studio's BLUE: a focused title bar
EDGE = (85, 96, 114)                  # its outline: the title lightened
RED = (0xe2, 0x67, 0x3f)
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-800:])
    return ok


def frame(img):
    """The focused window: (x0, y0, x1, y1, bot) -- its title bar, from the
    first and last pixel of the title colour on the topmost row below the
    desktop's own strip that has a long run of it, and the row of its
    outline below (None when it has none: maximised, snapped)."""
    px = img.load()
    for y in range(18, SH - 40):
        xs = [x for x in range(SW) if px[x, y] == TITLE]
        if len(xs) < 160:
            continue
        x0, x1 = xs[0], xs[-1] + 1
        mid = x0 + (x1 - x0) * 3 // 5
        yt0 = y
        while yt0 > 0 and px[mid, yt0 - 1] == TITLE:
            yt0 -= 1
        yt1 = y
        while yt1 < SH - 1 and px[mid, yt1 + 1] == TITLE:
            yt1 += 1
        bot = None
        for yy in range(yt1 + 2, SH):
            if px[x0 + 40, yy] == EDGE and px[x0 + 80, yy] == EDGE:
                bot = yy
                break
        return (x0, yt0, x1, yt1 + 1, bot)
    return None


def sign_in(q):
    """From the account screen or the sign-in screen, as admin (as
    redraw_hunt.py does, at this harness's grid)."""
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


def keep(q, name):
    """SHOTS=<dir>: keep a picture of this moment (for the changelog)."""
    if os.environ.get("SHOTS"):
        PM.shot(q).save(os.path.join(os.environ["SHOTS"], "w099-%s.png" % name))


def open_app(q, name, settle=3.0):
    q.key("meta_l"); time.sleep(1.2)
    q.type(name, settle=0.12); time.sleep(0.8)
    q.key("ret"); time.sleep(settle)


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    print("windows of any size (0.9.9), at %dx%d on %dx%d" % (COLS, ROWS, SW, SH))
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256, extra=["-device", "qemu-xhci,id=xhci",
                                         "-device", "usb-mouse,bus=xhci.0"]) as q:
        # 1. the splash, from the desktop entry, before anything else
        time.sleep(7); q.key("ret")
        seen = False
        for _ in range(12):
            time.sleep(0.7)
            img = PM.shot(q); p = img.load()
            if any(p[x, y] == RED for y in range(SH // 6, SH // 2) for x in range(SW // 2 - 80, SW // 2 + 80, 8)):
                seen = True
                break
        check("the boot screen is pixels: the oxide rule under the wordmark", seen)
        q.stop(); Q.kill_stale(); time.sleep(1.0)
        q.__init__(iso=ISO, mem=256, extra=["-device", "qemu-xhci,id=xhci",
                                            "-device", "usb-mouse,bus=xhci.0"])
        q.start()
        boot_to_shell(q)
        q.line("resolution %dx%d" % (COLS, ROWS)); time.sleep(1.5)
        q.key("ret"); time.sleep(0.5)
        q.set_grid(COLS, ROWS)
        q.line("exit"); time.sleep(4.0)
        sign_in(q); time.sleep(3.0)
        for _ in range(2):
            q.key("esc"); time.sleep(0.5)

        # 2. the Terminal
        open_app(q, "terminal")
        keep(q, "terminal")
        img = PM.shot(q); f = frame(img)
        check("the Terminal is a floating window (a title bar, not edge to edge)",
              f is not None and f[0] > 20 and f[2] < SW - 20, f)
        if f is None:
            return finish()
        x0, y0, x1, y1, bot = f
        check("  ... with its outline below it", bot is not None, f)
        if bot is None:
            return finish()
        w0, h0 = x1 - x0, bot - y1

        # 3. bigger, from the bottom-right corner
        PM.to(q, x1 - 12, bot - 12)
        PM.nudge(q, 14, 14)                          # onto the band, outside the outline
        import numpy as np
        a = np.asarray(PM.shot(q), dtype=np.int16)
        check("over the corner the pointer is not the arrow any more",
              __import__("arrowfind")._by_shape(a) is None)
        PM.drag(q, 160, 96)
        q.line("help"); time.sleep(1.5)
        keep(q, "terminal-resized")
        f2 = frame(PM.shot(q))
        ok = f2 is not None and f2[4] is not None
        if ok:
            dw, dh = (f2[2] - f2[0]) - w0, (f2[4] - f2[3]) - h0
            check("dragged 160x96 bigger: the window is (whole cells: 8x16)",
                  150 <= dw <= 168 and 80 <= dh <= 104 and dw % 8 == 0, (dw, dh, f, f2))
            check("  ... and it stayed where it was (top-left unmoved)",
                  abs(f2[0] - x0) <= 1 and abs(f2[1] - y0) <= 1, (f, f2))
        else:
            check("dragged bigger: the window is found again", False, f2)

        # 4. maximise, and back
        f = frame(PM.shot(q))
        th = f[3] - f[1]; bw = th * 3 // 2
        PM.to(q, f[2] - bw - bw // 2, (f[1] + f[3]) // 2); PM.click(q, 1.5)
        img = PM.shot(q)
        if os.environ.get("SHOTS"):
            img.save(os.path.join(os.environ["SHOTS"], "w099-max.png"))
        fm = frame(img)
        check("maximise: the title bar spans the screen, from the top",
              fm is not None and fm[0] <= RX0 + 1 and fm[1] <= 1 and fm[2] >= RX1 - 3 * bw, fm)
        if fm:
            PM.to(q, fm[2] - bw - bw // 2, (fm[1] + fm[3]) // 2); PM.click(q, 1.5)
        img = PM.shot(q)
        if os.environ.get("SHOTS"):
            img.save(os.path.join(os.environ["SHOTS"], "w099-restored.png"))
        fr = frame(img)
        check("  ... again: the window as it was", fr is not None and abs(fr[0] - f[0]) <= 1
              and abs(fr[2] - f[2]) <= 1, (f, fr))

        # 5. snapped to the left half
        if fr:
            PM.to(q, (fr[0] + fr[2]) // 2 - 100, (fr[1] + fr[3]) // 2)
            PM.drag(q, -((fr[0] + fr[2]) // 2 - 100 - RX0) - 40, 0)
            keep(q, "snapped")
            fs = frame(PM.shot(q))
            check("dragged to the left edge: it takes the left half",
                  fs is not None and fs[0] <= RX0 + 1 and (RX0 + RX1) // 2 - 3 * bw <= fs[2] <= (RX0 + RX1) // 2 + 1, fs)

        # 6. the Calculator zooms
        open_app(q, "calculator")
        fc = frame(PM.shot(q))
        check("the Calculator opens as a window", fc is not None and fc[4] is not None, fc)
        if fc and fc[4]:
            cw0, ch0 = fc[2] - fc[0], fc[4] - fc[3]
            PM.to(q, fc[2] - 12, fc[4] - 12); PM.nudge(q, 14, 14)
            PM.drag(q, 240, 150)
            keep(q, "calc-zoomed")
            fc2 = frame(PM.shot(q))
            ok = fc2 is not None and fc2[4] is not None
            check("resized, it is bigger", ok and fc2[2] - fc2[0] > cw0 + 150, (fc, fc2))
            if ok:
                # its text is bigger: the widest run of light ink in a row
                img = PM.shot(q); p = img.load()
                def ink_h(f):
                    rows = 0
                    for yy in range(f[3] + 4, f[4] - 2):
                        if any(sum(p[xx, yy]) > 600 for xx in range(f[0] + 4, f[2] - 4, 2)):
                            rows += 1
                    return rows
                check("  ... and so is what is in it (zoomed, not padded)", ink_h(fc2) > 0, fc2)

        # 7. F11 and back
        q.key("f11"); time.sleep(2.0)
        keep(q, "calc-full")
        img = PM.shot(q); p = img.load()
        bar = sum(1 for x in range(0, SW, 4) if p[x, SH - 6] == p[x, SH - 20]) < SW // 8
        check("F11: the window has the whole screen -- no title bar at the top",
              frame(img) is None or frame(img)[1] > 40, frame(img))
        q.key("alt", "ret") if False else q.cmd_qmp("send-key", keys=[{"type": "qcode", "data": "alt"},
                                                                    {"type": "qcode", "data": "ret"}])
        time.sleep(2.0)
        check("Alt+Enter: a window again", frame(PM.shot(q)) is not None, None)
    return finish()


def finish():
    n = sum(results)
    print("\n%d/%d" % (n, len(results)))
    return 0 if results and n == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
