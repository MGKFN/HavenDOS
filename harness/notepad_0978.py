#!/usr/bin/env python3
"""notepad_0978.py -- Notepad in 0.9.7.8, driven with real key presses.

Kyan, 24 Nov: wrap to a new line at the edge of the screen, more than 200
lines, and "CTRL + A doesnt work ... CTRL + X same story". Checked here
through QEMU's keyboard, so the keys go through the 8042 and
drivers/keyboard.c exactly as on a laptop:

  * a sentence longer than the screen wraps onto line 2, word by word;
  * Ctrl+A selects everything, Ctrl+X cuts it, Ctrl+V puts it back;
  * Ctrl+X on part of ONE line removes exactly that part (0.9.7.7
    scrambled the line);
  * the cursor can go past line 200.

Run:  python3 harness/notepad_0978.py [path/to.iso]
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

fails, total = [], [0]


def ck(what, ok, detail=""):
    total[0] += 1
    print(("    ok   " if ok else "    FAIL ") + what
          + ("" if ok or not detail else "\n         " + detail))
    if not ok:
        fails.append(what)


def to_shell(q):
    time.sleep(6)
    q.key("down")                      # GRUB entry 1: Shell Mode
    time.sleep(0.4)
    q.key("ret")
    if q.wait_for("@havendos", 240) is None:
        q.key("ret")
        if q.wait_for("@havendos", 60) is None:
            raise RuntimeError("the shell prompt never appeared")
    time.sleep(1.0)


def text_line(scr, n):
    """The text of buffer line n as Notepad shows it: '   12| text'."""
    for line in scr.splitlines():
        m = re.match(r"^\s*(\d+)\|(.*)$", line)
        if m and int(m.group(1)) == n:
            return m.group(2).rstrip()
    return None


WORDS = ("the quick brown fox jumps over the lazy dog while seven "
         "amber lamps glow on a desk at tech haven studios tonight")


def main():
    if not os.path.exists(ISO):
        sys.exit(f"notepad_0978: no ISO at {ISO} -- run make first")
    kill_stale()
    q = Qemu(ISO, disk=None, usb=False).start()
    try:
        to_shell(q)
        q.type("edit /NOTE.TXT\n")
        time.sleep(3)
        scr = q.screen()
        ck("Notepad opens", "Ln:" in scr, scr[-300:])

        # ── wrapping ──────────────────────────────────────────────────
        q.type(WORDS, settle=0.08)
        time.sleep(1.5)
        scr = q.screen()
        l1, l2 = text_line(scr, 1), text_line(scr, 2)
        ck("a sentence longer than the screen goes on to line 2",
           bool(l2), f"line 1: {l1!r}\n         line 2: {l2!r}")
        ck("line 1 stops before the edge (73 characters at 80x25)",
           l1 is not None and len(l1) <= 73, repr(l1))
        ck("the break is between words, not inside one",
           l1 is not None and l2 is not None
           and (l1 + " " + l2).split() == WORDS.split(),
           f"{l1!r} / {l2!r}")

        # ── Ctrl+A, Ctrl+X, Ctrl+V ───────────────────────────────────
        q.key("ctrl", "a")
        time.sleep(1.0)
        scr = q.screen()
        ck("Ctrl+A selects (the status bar says SELECTION)",
           "SELECTION" in scr, scr.splitlines()[-2] if scr else "")
        q.key("ctrl", "x")
        time.sleep(1.0)
        scr = q.screen()
        ck("Ctrl+X cuts all of it: line 1 is empty",
           (text_line(scr, 1) or "") == "" and not text_line(scr, 2),
           f"{text_line(scr, 1)!r} / {text_line(scr, 2)!r}")
        q.key("ctrl", "v")
        time.sleep(1.0)
        scr = q.screen()
        back = (text_line(scr, 1) or "") + " " + (text_line(scr, 2) or "")
        ck("Ctrl+V puts it back, word for word",
           back.split() == WORDS.split(), repr(back))

        # ── a cut inside one line ────────────────────────────────────
        q.key("ctrl", "n")
        time.sleep(0.8)
        q.type("hello world", settle=0.08)
        for _ in range(5):
            q.key("shift", "left", settle=0.08)
        time.sleep(0.5)
        q.key("ctrl", "x")
        time.sleep(1.0)
        scr = q.screen()
        ck("Ctrl+X of 'world' leaves 'hello ' (0.9.7.7 scrambled the line)",
           (text_line(scr, 1) or "") == "hello", repr(text_line(scr, 1)))

        # ── past line 200 ────────────────────────────────────────────
        q.key("ctrl", "n")
        time.sleep(0.8)
        for _ in range(230):
            q.key("ret", settle=0.01)
        time.sleep(1.5)
        q.type("end", settle=0.08)
        time.sleep(1.0)
        scr = q.screen()
        ck("the cursor reaches line 231 (was capped at 200)",
           "Ln:231" in scr and text_line(scr, 231) == "end",
           [l for l in scr.splitlines() if "Ln:" in l][:1])

        q.key("ctrl", "q")
        time.sleep(0.6)
        q.key("q")
        time.sleep(1.0)
    finally:
        q.stop()
    print(f"\n{total[0] - len(fails)}/{total[0]} passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
