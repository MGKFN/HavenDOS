#!/usr/bin/env python3
"""window_apps.py -- v0.9.6.5: an app is a window; the taskbar stays.

On QEMU's 1280x800 at 128x50 (the HP runs 128x48), with a USB mouse:

  1. `open settings`: a title bar on row 0 saying "Settings" with _ [ ] X,
     and the taskbar on the last row with Start, a chip and the clock
  2. Settings itself starts on row 1 (its own "Settings" header there)
  3. the mouse works in it: the wheel moves the sidebar
  4. the full-screen button: title bar and taskbar gone; Alt+Enter brings
     them back
  5. minimize: the desktop is in front, Settings still open behind
  6. back to it, then the X: Settings closes (ESC, twice: sidebar, close)

The pointer is placed by counting out from the top-left corner and then
LOOKING for it on a screenshot and nudging, as mouse_apps.py does --
QEMU's HID queue drops counts when it fills.

Run:  python3 harness/window_apps.py [path/to.iso]   (foreground)
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
from mouse_apps import boot_to_shell, ev                # noqa: E402

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "havendos.iso")
COLS, ROWS = 128, 50
# 128x50 on 1280x800 is 8x16 cells -- 1024 px wide, centred, with a
# 128 px margin each side (vga_fb_app_grid picks the cell, not 1280/128)
CW, CH = 8, 16
X0 = (1280 - COLS * CW) // 2
CX = 320.0 / COLS                        # counts per column (MOUSE_SPAN 320)
CY = 320.0 * CH / (CW * COLS)            # counts per row: same pixels per count
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-1200:].replace("\n", "\n          "))
    return ok


def rel(q, axis, value):
    ev(q, [{"type": "rel", "data": {"axis": axis, "value": value}}])
    time.sleep(0.004 if abs(value) > 1 else 0.008)


def cells(q):
    from PIL import Image
    q.shot("/tmp/window_apps.ppm"); time.sleep(0.25)
    img = Image.open("/tmp/window_apps.ppm").convert("RGB")
    return {(x, y): (img.getpixel((X0 + x * CW + 1, y * CH + 1)), img.getpixel((X0 + x * CW + CW // 2, y * CH + CH // 2)))
            for y in range(ROWS) for x in range(COLS)}


def corner(q):
    for _ in range(60):
        rel(q, "x", -40)
    for _ in range(40):
        rel(q, "y", -40)
    time.sleep(0.3)


def place(q, x, y):
    from PIL import Image
    from arrowfind import tip_cell
    corner(q)
    q.shot("/tmp/window_apps_ref.ppm"); time.sleep(0.25)
    ref = Image.open("/tmp/window_apps_ref.ppm").copy()
    for _ in range(int(round(x * CX + CX / 2))):
        rel(q, "x", 1)
    for _ in range(int(round(y * CY + CY / 2))):
        rel(q, "y", 1)
    time.sleep(0.4)
    for _ in range(4):
        # v0.9.7.10: the pointer is an arrow; find its tip (arrowfind.py)
        q.shot("/tmp/window_apps_now.ppm"); time.sleep(0.25)
        at = tip_cell(Image.open("/tmp/window_apps_now.ppm"), ref, X0, 0, CW, CH)
        if at is None or at == (x, y):
            return at
        for _ in range(int(round(abs(x - at[0]) * CX))):
            rel(q, "x", 1 if x > at[0] else -1)
        for _ in range(int(round(abs(y - at[1]) * CY))):
            rel(q, "y", 1 if y > at[1] else -1)
        time.sleep(0.4)
    return None


def button(q, name="left", settle=1.0):
    ev(q, [{"type": "btn", "data": {"down": True, "button": name}}]); time.sleep(0.08)
    ev(q, [{"type": "btn", "data": {"down": False, "button": name}}]); time.sleep(settle)


def click(q, x, y, settle=1.2):
    at = place(q, x, y)
    if at != (x, y):
        print("          (pointer at %s, wanted %s)" % (at, (x, y)))
    button(q, "left", settle)


def scr(q):
    return q.screen(COLS, ROWS)


def lines(s):
    return s.split("\n")


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    print("an app is a window (0.9.6.5): title bar, buttons, taskbar")
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256, extra=["-device", "qemu-xhci,id=xhci",
                                         "-device", "usb-mouse,bus=xhci.0"]) as q:
        boot_to_shell(q)
        q.line("resolution %dx%d" % (COLS, ROWS)); time.sleep(1.5)
        q.key("ret"); time.sleep(1.0)
        q.line("open settings"); time.sleep(3.0)
        s = scr(q); L = lines(s)
        top, bot = L[0], L[ROWS - 1] if len(L) >= ROWS else ""
        check("row 0 is the title bar: 'Settings' and the X",
              "Settings" in top and re.search(r"\bX\s*$", top.rstrip()) is not None, top)
        check("the last row is the taskbar: Start, a Settings chip, the clock",
              "Start" in bot and "Settings" in bot and re.search(r"\d\d:\d\d", bot), bot)
        check("Settings itself starts below the title bar", "Settings" in L[1] and "Appearance" in s, L[1])

        # the mouse in Settings: the wheel moves the sidebar
        for _ in range(2):
            ev(q, [{"type": "btn", "data": {"down": True, "button": "wheel-down"}}])
            ev(q, [{"type": "btn", "data": {"down": False, "button": "wheel-down"}}])
            time.sleep(0.5)
        time.sleep(0.8)
        s = scr(q)
        check("the wheel works in Settings (Desktop's page is showing)",
              "Icon" in s and "Fav" in s, s)

        # full screen: the title bar's middle button
        top = lines(scr(q))[0]
        bx = top.rstrip().rfind("X")
        click(q, bx - 4, 0)
        s = scr(q); L = lines(s)
        check("full screen: no title bar, no taskbar", "Start" not in L[ROWS - 1] and
              not re.search(r"\bX\s*$", L[0].rstrip()), s)
        q.key("alt", "ret"); time.sleep(1.5)
        s = scr(q); L = lines(s)
        check("Alt+Enter: the title bar and taskbar are back",
              "Start" in L[ROWS - 1] and re.search(r"\bX\s*$", L[0].rstrip()), s)

        # minimize
        top = lines(scr(q))[0]
        bx = top.rstrip().rfind("X")
        click(q, bx - 8, 0, settle=2.5)
        s = scr(q)
        # the window was opened from the Shell, so minimizing it brings
        # forward the desktop -- whose title row is not Settings' bar
        check("minimize: Settings' window is gone from the screen",
              not re.search(r"Settings.*X\s*$", lines(s)[0].rstrip()) and "Appearance" not in s, s)
        q.key("alt", "tab"); time.sleep(1.0)       # back to the Shell, Settings behind
        q.key("ret"); time.sleep(1.5)
        q.line("ps"); time.sleep(1.2)
        s = scr(q)
        check("  ... and Settings is still open (ps)", "Settings" in s, s)
        q.line("open settings"); time.sleep(2.5)
        s = scr(q)
        check("open settings again brings THAT window back", "Settings" in lines(s)[0], lines(s)[0])

        # the X: ESC -- from the sidebar that closes Settings
        top = lines(scr(q))[0]
        bx = top.rstrip().rfind("X")
        click(q, bx, 0, settle=2.0)
        s = scr(q)
        if "Settings" in lines(s)[0] and "Appearance" in s:
            click(q, bx, 0, settle=2.0)                 # it was on a page: back, then close
            s = scr(q)
        check("the X closes Settings (as ESC does)", "Appearance" not in s, s)
        q.line("ps"); time.sleep(1.2)
        s = scr(q)
        check("  ... and it is gone from `ps`", "Settings" not in s, s)

    passed = sum(results)
    print("\n%d/%d checks passed" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
