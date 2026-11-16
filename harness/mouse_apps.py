#!/usr/bin/env python3
"""mouse_apps.py -- v0.9.6.3: the mouse in Settings, on a real (emulated) mouse.

tests/test_uimouse.c proves what shell/uimouse.c turns a click into,
against a model screen. This proves the whole path on QEMU: a usb-mouse
on xHCI, through usb/hid.c and drivers/mouse.c, into keyboard_getchar()'s
hook, into Settings -- which until 0.9.6.3 could not be used with a mouse
at all:

  1. click "Display" in the sidebar: the Display page opens
  2. click the "Light it by itself" row twice: selected, then toggled
     (on -> off), and the setting reads off
  3. the scroll wheel moves the selection
  4. right-click goes back to the sidebar; clicking the hint "ESC close"
     in the bottom bar closes Settings
  5. the shell prompt is back, and a right-click there does nothing

The pointer is parked by slamming it into the top-left corner and
counting out: at 80x25 on QEMU's 1280x800 framebuffer drivers/mouse.c
moves one column per 4 counts and one row per 8.

Run:  python3 harness/mouse_apps.py [path/to.iso]   (foreground)
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
VER = re.search(r'HAVENDOS_VER_NUM\s+"([^"]+)"',
                open(os.path.join(os.path.dirname(HERE), "include", "version.h")).read()).group(1)


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-900:].replace("\n", "\n          "))
    return ok


def ev(q, events):
    q.cmd_qmp("input-send-event", events=events)


def rel(q, axis, value):
    ev(q, [{"type": "rel", "data": {"axis": axis, "value": value}}])
    time.sleep(0.004 if abs(value) > 1 else 0.008)


CX, CY = 4, 8          # counts per column / per row, at 80x25 on 1280x800


def park(q, x, y):
    """Into the top-left corner fast (the clamp makes overshoot free),
    then out ONE count at a time. QEMU merges the events a poll has not
    collected yet into one report, and a report over six counts is
    accelerated by drivers/mouse.c -- so steps of four, sent quickly,
    land a third further than counted."""
    for _ in range(60):
        rel(q, "x", -40)
    for _ in range(40):
        rel(q, "y", -40)
    time.sleep(0.3)
    for _ in range(x * CX):
        rel(q, "x", 1)
    for _ in range(y * CY):
        rel(q, "y", 1)
    time.sleep(0.4)


def _cells(q):
    """The screen as 80x25 cell signatures, from a screenshot."""
    from PIL import Image
    q.shot("/tmp/mouse_apps_cells.ppm"); time.sleep(0.25)
    img = Image.open("/tmp/mouse_apps_cells.ppm").convert("RGB")
    out = {}
    for y in range(25):
        for x in range(80):
            out[(x, y)] = (img.getpixel((x * 16 + 1, y * 32 + 1)),
                           img.getpixel((x * 16 + 8, y * 32 + 16)))
    return out


def find_pointer(q, ref):
    """Where the pointer is: the cells that differ from `ref`, a
    screenshot taken with the pointer in the corner, other than the
    corner itself."""
    now = _cells(q)
    diff = [c for c in now if now[c] != ref.get(c) and c != (0, 0)]
    return diff[0] if len(diff) == 1 else None


def place(q, x, y, tries=4):
    """park(), then LOOK: QEMU's USB HID queue drops counts when it
    fills, so a count-out lands a row or a column short now and then.
    Measure where the pointer went and nudge it by the difference."""
    park(q, 0, 0)
    ref = _cells(q)
    for _ in range(x * CX):
        rel(q, "x", 1)
    for _ in range(y * CY):
        rel(q, "y", 1)
    time.sleep(0.4)
    for _ in range(tries):
        at = find_pointer(q, ref)
        if at is None or at == (x, y):
            return at
        for _ in range(abs(x - at[0]) * CX):
            rel(q, "x", 1 if x > at[0] else -1)
        for _ in range(abs(y - at[1]) * CY):
            rel(q, "y", 1 if y > at[1] else -1)
        time.sleep(0.4)
    return find_pointer(q, ref)


def button(q, name, settle=0.7):
    ev(q, [{"type": "btn", "data": {"down": True, "button": name}}])
    time.sleep(0.08)
    ev(q, [{"type": "btn", "data": {"down": False, "button": name}}])
    time.sleep(settle)


def click(q, x, y, name="left", settle=0.9):
    at = place(q, x, y)
    if at != (x, y):
        print("          (pointer at %s, wanted %s)" % (at, (x, y)))
    button(q, name, settle)


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


def row_of(s, needle):
    for i, l in enumerate(s.splitlines()):
        if needle in l:
            return i, l
    return -1, ""


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    print("the mouse in Settings (0.9.6.3), usb-mouse on xHCI")
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256, extra=["-device", "qemu-xhci,id=xhci",
                                         "-device", "usb-mouse,bus=xhci.0"]) as q:
        boot_to_shell(q)
        q.line("resolution 80x25"); time.sleep(1.5)
        q.line("dmesg | grep -i mouse"); time.sleep(1.5)
        s = q.screen(80, 25)
        check("a USB mouse is bound", re.search(r"(?i)mouse", s) is not None, s)
        q.line("cls"); time.sleep(0.5)

        q.line("settings"); time.sleep(2.0)
        s = q.screen(80, 25)
        check("Settings is open", "Settings" in s and "Display" in s, s)
        r, _ = row_of(s, "Display")
        click(q, 4, r)
        s = q.screen(80, 25)
        check("click 'Display' in the sidebar: the Display page opens",
              "Shell console" in s and "Light it by itself" in s, s)

        r, line = row_of(s, "Light it by itself")
        before = "on" if re.search(r"Light it by itself\s+on", line) else "off"
        click(q, 30, r)                          # first click: select
        s = q.screen(80, 25)
        _, line = row_of(s, "Light it by itself")
        check("first click on 'Light it by itself' selects it, changes nothing",
              re.search(r"Light it by itself\s+" + before, line) is not None, s)
        button(q, "left")                        # second click: Enter
        s = q.screen(80, 25)
        _, line = row_of(s, "Light it by itself")
        after = "off" if before == "on" else "on"
        check("the second click toggles it: %s -> %s" % (before, after),
              re.search(r"Light it by itself\s+" + after, line) is not None, s)
        button(q, "left")                        # and back, so the disk is as it was
        time.sleep(0.5)

        # the wheel: from the selected monitor row up two rows. The text
        # readback has no colours, so the highlight is read off a
        # screenshot: the selection bar is the theme's accent behind the
        # row (16x32 cells at 80x25 on 1280x800).
        def highlighted(label):
            from PIL import Image
            q.shot("/tmp/mouse_apps.ppm"); time.sleep(0.3)
            img = Image.open("/tmp/mouse_apps.ppm").convert("RGB")
            r, _ = row_of(q.screen(80, 25), label)
            if r < 0:
                return False
            px = img.getpixel((60 * 16, r * 32 + 16))   # right of the text
            return px != (0, 0, 0)
        check("before the wheel: 'Light it by itself' is the selected row",
              highlighted("Light it by itself"))
        for _ in range(2):
            ev(q, [{"type": "btn", "data": {"down": True, "button": "wheel-up"}}])
            ev(q, [{"type": "btn", "data": {"down": False, "button": "wheel-up"}}])
            time.sleep(0.5)
        time.sleep(0.6)
        check("two notches up: '160 x 50' is selected, the monitor row is not",
              highlighted("160 x 50") and not highlighted("Light it by itself"))
        for _ in range(2):
            ev(q, [{"type": "btn", "data": {"down": True, "button": "wheel-down"}}])
            ev(q, [{"type": "btn", "data": {"down": False, "button": "wheel-down"}}])
            time.sleep(0.5)
        time.sleep(0.6)
        check("two notches down: back on the monitor row",
              highlighted("Light it by itself"))

        s = q.screen(80, 25)
        if "Shell console" not in s:
            q.line("settings display"); time.sleep(1.5)
        button(q, "right")
        s = q.screen(80, 25)
        check("right-click: back to the sidebar (its footer shows)",
              "ESC close" in s, s)
        r, line = row_of(s, "ESC close")
        col = line.find("ESC close")
        click(q, col + 1, r)
        s = q.screen(80, 25)
        check("click 'ESC close' in the bottom bar: Settings closes",
              "@havendos" in s and "Light it by itself" not in s, s)

        # the monitor's commands (QEMU has no Iris Xe: settings only)
        q.line("cls"); time.sleep(0.4)
        q.line("gfx fit stretch"); time.sleep(1.0)
        q.line("gfx fit"); time.sleep(1.0)
        s = q.screen(80, 25)
        check("`gfx fit stretch` is kept: `gfx fit` says stretched to fill",
              s.count("stretched to fill") >= 2, s)
        q.line("gfx fit keep"); time.sleep(0.8)
        q.line("gfx auto"); time.sleep(0.8)
        s = q.screen(80, 25)
        check("`gfx auto` reports the switch", "lights by itself" in s or "only by `gfx on`" in s, s)
        q.line("cls"); time.sleep(0.4)
        q.line("kpoll"); time.sleep(1.2)
        s = q.screen(80, 25)
        check("the monitor watcher is a kpoll client", "monitor" in s, s)

        button(q, "right")
        q.line("ver"); time.sleep(1.0)
        s = q.screen(80, 25)
        check("at the prompt a right-click does nothing; the shell still answers",
              VER in s, s)

    passed = sum(results)
    print("\n%d/%d checks passed" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
