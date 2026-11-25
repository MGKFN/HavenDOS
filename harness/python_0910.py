#!/usr/bin/env python3
"""python_0910.py -- v0.9.10: MicroPython in HavenDOS.

On QEMU's 1280x800 at 128x50:

  1. `python file.py` runs a file, and `import` finds a module next to it
  2. a missing file is a one-line message, not a crash
  3. the prompt: arithmetic, big integers, floats, math, a comprehension
  4. time.sleep(1) takes about a second; random, sys
  5. input() reads a line
  6. Ctrl+C stops `while True: pass` with KeyboardInterrupt
  7. runaway recursion is a RuntimeError, not a crashed kernel
  8. an exception prints a traceback and the prompt comes back
  9. Ctrl+D goes back to the shell, and the shell still works
 10. Python from the Start menu opens as a window with the prompt in it
 11. its X closes it, and it opens again

SHOTS=<dir> keeps pictures of each step.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
import px_mouse as PM                                   # noqa: E402
from mouse_apps import boot_to_shell                    # noqa: E402
import windows_099 as W9                                # noqa: E402

ROOT = os.path.dirname(HERE)
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
COLS, ROWS = 128, 50
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-900:])
    return ok


def keep(q, name):
    if os.environ.get("SHOTS"):
        PM.shot(q).save(os.path.join(os.environ["SHOTS"], "py0910-%s.png" % name))


def screen(q):
    import fbtext
    q.shot("/tmp/py0910.ppm"); time.sleep(0.4)
    t = fbtext.text("/tmp/py0910.ppm", COLS, ROWS)
    return t if isinstance(t, str) else "\n".join(t)


def slow(q):
    """Python's prompt types back every key; 50 ms a key overruns it and a
    key comes out twice. The rule is >= 80 ms a key."""
    orig = q.line
    q.line = lambda s, settle=0.09, pause=0.35: orig(s, settle=settle, pause=pause)


def main():
    Q.kill_stale()
    W9.COLS, W9.ROWS = COLS, ROWS
    with Q.Qemu(iso=ISO, mem=512, extra=["-device", "qemu-xhci,id=xhci",
                                         "-device", "usb-mouse,bus=xhci.0"]) as q:
        boot_to_shell(q)
        slow(q)
        q.line("resolution %dx%d" % (COLS, ROWS)); time.sleep(1.5)
        q.key("ret"); time.sleep(0.5)
        q.set_grid(COLS, ROWS)

        q.line("cls"); time.sleep(1)
        q.line("echo def sq(n): return n*n > mymod.py"); time.sleep(1)
        q.line("echo import mymod > t.py"); time.sleep(1)
        q.line("echo print('sq', mymod.sq(12)) >> t.py"); time.sleep(1)
        q.line("python t.py"); time.sleep(3)
        q.line("python nope.py"); time.sleep(2)
        s = screen(q); keep(q, "file")
        check("python t.py runs, and imports mymod.py", "sq 144" in s, s[-600:])
        check("a missing file: one line", "no such file: nope.py" in s, s[-600:])

        q.line("cls"); time.sleep(1)
        q.line("python"); time.sleep(3)
        q.line("print(6*7, 2**100, 7/2)"); time.sleep(1)
        q.line("import math, time, random, sys"); time.sleep(1)
        q.line("print(math.sqrt(16), [x*x for x in range(5)], sys.platform)"); time.sleep(1)
        q.line("t=time.ticks_ms(); time.sleep(1); d=time.ticks_diff(time.ticks_ms(), t); print('slept', 900 < d < 1500)")
        time.sleep(3)
        q.line("print('dice', random.randint(1, 6) in range(1, 7))"); time.sleep(1)
        q.line("n = input('name? ')"); time.sleep(1); q.line("Kyan"); time.sleep(1)
        q.line("print('hi', n)"); time.sleep(1)
        s = screen(q); keep(q, "repl")
        check("the prompt: ints, big ints, floats", "42 1267650600228229401496703205376 3.5" in s, s[-900:])
        check("math, a comprehension, sys", "4.0 [0, 1, 4, 9, 16] havendos" in s, s[-900:])
        check("time.sleep(1) is about a second", "slept True" in s, s[-900:])
        check("random", "dice True" in s, s[-900:])
        check("input() reads a line", "hi Kyan" in s, s[-900:])

        q.line("cls"); time.sleep(1)                 # a NameError, not the shell's cls
        q.line("while True: pass"); time.sleep(0.4); q.key("ret"); time.sleep(2)
        q.key("ctrl", "c"); time.sleep(2)
        s = screen(q)
        check("Ctrl+C stops a loop", "KeyboardInterrupt" in s, s[-900:])
        q.line("def f(n): return f(n + 1)"); time.sleep(0.4); q.key("ret"); time.sleep(1)
        q.line("f(0)"); time.sleep(4)
        q.line("print('still', 'here')"); time.sleep(1)
        s = screen(q); keep(q, "stops")
        check("runaway recursion is a RuntimeError", "maximum recursion depth" in s, s[-900:])
        check("... and the prompt comes back", "still here" in s, s[-900:])

        q.key("ctrl", "d"); time.sleep(2)
        q.line("echo back in the shell"); time.sleep(1)
        s = screen(q)
        check("Ctrl+D: back to the shell", "back in the shell" in s.split("still here")[-1], s[-600:])

        # the Start menu's Python, in a window
        q.line("exit"); time.sleep(4.0)
        W9.sign_in(q); time.sleep(3.0)
        W9.open_app(q, "python", settle=4.0)
        q.type("print(3*14)", settle=0.1); q.key("ret"); time.sleep(2)
        img = PM.shot(q); keep(q, "window")
        f = W9.frame(img)
        s = screen(q)
        check("Python opens as a window", f is not None, f)
        check("... with the prompt working in it", "42" in s and "MicroPython" in s, s[-600:])

        # its X closes it (ESC + a close request: the prompt leaves), and
        # it opens again -- an interpreter left marked busy would refuse
        if f:
            PM.to(q, f[2] - 24, (f[1] + f[3]) // 2); PM.click(q); time.sleep(2.5)
            img = PM.shot(q); keep(q, "closed")
            check("its X closes it", W9.frame(img) is None, W9.frame(img))
            W9.open_app(q, "python", settle=4.0)
            q.type("print('again')", settle=0.1); q.key("ret"); time.sleep(2)
            s = screen(q); keep(q, "again")
            check("... and it opens again", "again" in s and "already open" not in s, s[-600:])

    n = sum(results)
    print("\n%d/%d" % (n, len(results)))
    sys.exit(0 if n == len(results) else 1)


if __name__ == "__main__":
    main()
