#!/usr/bin/env python3
"""check_task_sites.py -- where a task switch may be asked for.

v0.9.6. include/task.h makes one argument and this check is what keeps
it true: a switch is only ever asked for where kernel_poll() is ALREADY
allowed to run -- the keyboard's read, the polling sleep, the desktop's
idle wait -- and task_yield() refuses for the same reasons kernel_poll()
does. That makes a switch exactly as safe as the polling that already
happens at the same instruction.

The argument fails the moment a driver calls task_yield() itself. A
driver mid-transfer holds a module buffer; if it yields, another task
can start the next transfer on the same buffer, and kpoll_depth() only
catches the six entry points that bracket themselves. So, as with
kpoll_sleep_ms():

  1. No call into the task layer anywhere under drivers/, usb/, net/
     or fs/. The input drivers reach it only through the two pointers
     task_init() registers (keyboard_set_task_hooks(),
     mouse_set_front_check()), which they call at the one point each
     where a switch is safe -- and which this check therefore does not
     look for.

  2. task_switch_stack() -- the assembly itself -- is called from
     kernel/task.c and nowhere else. A second caller would switch
     without saving the console or the guard.

  3. No sleep_ms() in an app the registry marks TK_RUN. Such an app
     keeps running behind the front, and every sleep_ms() it makes
     holds the app IN front up for that long; kpoll_sleep_ms() yields.
     The registry rows are read out of shell/desktop.c and the app's
     function body is found by name.

A line that genuinely needs an exception spells  task-site-ok  in a
comment on it (tools/csource.py's has_reason()).

    python3 tools/check_task_sites.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csource

OPT_OUT = "task-site-ok"
NO_TASK_CALLS = ("drivers", "usb", "net", "fs")
CALL_TASK = re.compile(r"\btask_(yield|wait_front|spawn|exit|kill|set_front|"
                       r"lock|unlock|set_policy)\s*\(")
CALL_SWITCH = re.compile(r"\btask_switch_stack\s*\(")
SWITCH_HOME = "kernel/task.c"


def code_lines(rel):
    text = open(rel, encoding="utf-8", errors="replace").read()
    code = csource.blank(text, strip_externs=True)
    return text.splitlines(), code.splitlines()


def scan_dirs(bad):
    for d in ("kernel", "drivers", "shell", "fs", "net", "usb"):
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".c"):
                continue
            rel = "%s/%s" % (d, name)
            raw, code = code_lines(rel)
            for n, line in enumerate(code, 1):
                window = raw[n - 1] if n - 1 < len(raw) else ""
                if csource.has_reason(window, OPT_OUT):
                    continue
                if d in NO_TASK_CALLS and CALL_TASK.search(line):
                    bad.append((rel, n, "a call into the task layer from "
                                "driver code", window.strip()))
                if CALL_SWITCH.search(line) and rel != SWITCH_HOME:
                    bad.append((rel, n, "task_switch_stack() outside "
                                + SWITCH_HOME, window.strip()))


def function_body(text, fname):
    """The body of `void fname(void){ ... }` by brace counting, or None."""
    m = re.search(r"\bvoid\s+" + re.escape(fname) + r"\s*\(\s*void\s*\)\s*\{",
                  text)
    if not m:
        return None, 0
    i = m.end()
    depth = 1
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[m.end():i], text[:m.start()].count("\n") + 1


def scan_run_apps(bad):
    desk = open("shell/desktop.c", encoding="utf-8").read()
    m = re.search(r"static const appreg_t appreg\[\]=\{(.*?)\n\};", desk, re.S)
    if not m:
        bad.append(("shell/desktop.c", 0, "cannot find appreg[]", ""))
        return 0
    rows = re.findall(r'\{"[^"]+",\s*"([^"]+)",\s*"..",\s*(\w+),\s*CAT_\w+'
                      r'\s*,\s*\w+\s*,\s*TK_RUN\s*\}', m.group(1))
    seen = 0
    for label, fn in rows:
        # The function may be a thin wrapper (appx_music runs the music
        # package); follow one level of `run_package_app("x", ...)`.
        targets = [fn]
        for d in ("shell",):
            for name in sorted(os.listdir(d)):
                if not name.endswith(".c"):
                    continue
                rel = "%s/%s" % (d, name)
                text = open(rel, encoding="utf-8", errors="replace").read()
                body, line0 = function_body(text, fn)
                if body is None:
                    continue
                pk = re.search(r'run_package_app\(\s*"(\w+)"', body)
                if pk:
                    targets.append("pkg_" + pk.group(1))
        for t in targets:
            for name in sorted(os.listdir("shell")):
                if not name.endswith(".c"):
                    continue
                rel = "shell/" + name
                text = open(rel, encoding="utf-8", errors="replace").read()
                body, line0 = function_body(text, t)
                if body is None:
                    continue
                seen += 1
                blanked = csource.blank(body, strip_externs=True)
                raw = body.splitlines()
                for n, line in enumerate(blanked.splitlines()):
                    if re.search(r"(?<!kpoll_)\bsleep_ms\s*\(", line):
                        w = raw[n] if n < len(raw) else ""
                        if csource.has_reason(w, OPT_OUT):
                            continue
                        bad.append((rel, line0 + n, "sleep_ms() in %s, which "
                                    "keeps running behind the front (TK_RUN) "
                                    "-- use kpoll_sleep_ms()" % label, w.strip()))
    return len(rows)


def main():
    if not os.path.isdir("kernel"):
        sys.stderr.write("run me from the source root\n")
        return 2
    bad = []
    scan_dirs(bad)
    nrun = scan_run_apps(bad)
    if bad:
        print("check_task_sites: %d problem(s)" % len(bad))
        for rel, n, why, text in bad:
            print("  %s:%d  %s\n      %s" % (rel, n, why, text))
        print("\n  include/task.h: a switch is only as safe as the place it"
              "\n  is asked for. Mark a real exception with  %s ." % OPT_OUT)
        return 1
    print("check_task_sites: ok, no task calls in driver code, the switch "
          "has one caller, %d background app(s) wait with kpoll_sleep_ms()"
          % nrun)
    return 0


if __name__ == "__main__":
    sys.exit(main())
