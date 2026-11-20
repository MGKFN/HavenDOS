#!/usr/bin/env python3
"""features_097.py -- 0.9.7's features, end to end in QEMU.

  1. The USB mouse runs in REPORT protocol (its wheel), from its own
     report descriptor.
  2. `update channel` says which channel this system follows.
  3. E-2: in the editor, a mouse DRAG selects text, Ctrl+C copies it, and
     Ctrl+V at the Shell's prompt pastes it -- one clipboard for every app.
  4. E-3: Files' right-click opens its menu (Rename ... New folder).
  5. E-4: the taskbar's pin (Files, by default) opens Files.

Run:  python3 harness/features_097.py [path/to.iso]   (foreground)
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
import window_apps as W                                 # noqa: E402
from mouse_apps import boot_to_shell, ev                # noqa: E402

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "havendos.iso")
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-900:].replace("\n", "\n          "))
    return ok


def grep_dmesg(q, pat):
    q.line("dmesg | grep -i %s" % pat); time.sleep(2.0)
    seen = []
    for _ in range(12):
        s = q.screen(128, 50)
        seen.append(s)
        if "-- more --" not in s:
            break
        q.key("spc"); time.sleep(0.8)
    return "\n".join(l.strip() for s in seen for l in s.split("\n")
                     if pat.lower() in l.lower() and "grep" not in l)


def find(lines, needle):
    for y, l in enumerate(lines):
        x = l.find(needle)
        if x >= 0:
            return x, y
    return None


def drag(q, x0, y, x1):
    W.place(q, x0, y)
    ev(q, [{"type": "btn", "data": {"down": True, "button": "left"}}]); time.sleep(0.4)
    for _ in range(int(round((x1 - x0) * W.CX))):
        W.rel(q, "x", 1)
    time.sleep(0.6)
    ev(q, [{"type": "btn", "data": {"down": False, "button": "left"}}]); time.sleep(0.8)


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    print("0.9.7: report-protocol mouse, update channel, the editor's clipboard, "
          "Files' menu, taskbar pins")
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256, extra=["-device", "qemu-xhci,id=xhci",
                                         "-device", "usb-mouse,bus=xhci.0"]) as q:
        boot_to_shell(q)
        q.line("resolution 128x50"); time.sleep(1.5)
        q.key("ret"); time.sleep(1.0)

        s = grep_dmesg(q, "mouse")
        check("the USB mouse is in report protocol, with its wheel",
              "report protocol, wheel" in s, s)
        q.line("update channel"); time.sleep(1.2)
        s = q.screen(128, 50)
        check("`update channel`: this Beta follows the Beta channel (CH20)",
              "follows the Beta channel (CH20)" in s, s[-600:])

        # E-2: select with the mouse, copy, paste at the prompt
        q.line("open edit"); time.sleep(3.0)
        q.key("ret"); time.sleep(1.5)          # the picker: a new file
        q.type("hello world"); time.sleep(0.8)
        L = W.lines(W.scr(q))
        at = find(L, "hello world")
        check("the editor shows what was typed", at is not None, "\n".join(L[:30]))
        if at:
            drag(q, at[0], at[1], at[0] + 5)
            s = W.scr(q)
            check("a mouse drag made a selection (the status bar says so)", "SELECTION" in s, s[-1200:])
            q.key("ctrl", "c"); time.sleep(0.6)
            q.key("esc"); time.sleep(1.0)
            q.key("q"); time.sleep(2.0)
            q.key("ctrl", "v"); time.sleep(1.0)
            L = W.lines(W.scr(q))
            prompt = [l for l in L if "@havendos" in l]
            check("Ctrl+V at the Shell's prompt pastes what the editor copied",
                  bool(prompt) and prompt[-1].rstrip().endswith("hello"), prompt[-3:])
            q.key("ret"); time.sleep(1.0)

        # E-4 then E-3: the pin opens Files, whose right-click has its menu
        q.line("echo hi > NOTE.TXT"); time.sleep(1.0)
        q.line("open 2048"); time.sleep(3.0)
        L = W.lines(W.scr(q))
        check("under an app, the taskbar shows Files' pin beside Start",
              L[49][8:12] == " FM ", repr(L[49][:30]))
        W.click(q, 9, 49, settle=3.0)
        L = W.lines(W.scr(q))
        check("clicking the pin opens Files", any("File Manager" in l for l in L), "\n".join(L[:14]))
        at = find(L, "NOTE.TXT")
        check("Files lists NOTE.TXT", at is not None, "\n".join(L[:30]))
        if at:
            W.place(q, at[0] + 2, at[1])
            W.button(q, "right", settle=1.5)
            s = W.scr(q)
            check("a right-click on it opens the menu: Open ... Rename ... New folder",
                  "Rename" in s and "New folder" in s and "Delete" in s, s[-1500:])
            # Rename, from the menu: the file on disk is what is checked
            q.key("down"); time.sleep(0.4)
            q.key("ret"); time.sleep(1.0)
            for _ in range(10):
                q.key("backspace"); time.sleep(0.05)
            q.type("memo.txt"); time.sleep(0.4)     # no Shift: QEMU drops some shifted keys
            q.key("ret"); time.sleep(2.5)
            W.corner(q); time.sleep(0.8)     # the pointer is a '.' cell: off the list first
            s = W.scr(q)
            check("Rename from the menu renames it: MEMO.TXT listed, NOTE.TXT gone",
                  "MEMO.TXT" in s.upper() and "NOTE.TXT" not in s.upper(), s[-1500:])
        q.key("esc"); time.sleep(1.5)

    passed = sum(results)
    print("\n%d/%d checks passed" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
