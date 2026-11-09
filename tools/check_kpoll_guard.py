#!/usr/bin/env python3
"""check_kpoll_guard.py -- the brackets round a shared buffer.

v0.8.8's periodic work runs from the keyboard's idle loop, and
tools/check_kpoll_sites.py keeps it there.  That is the braces.  This is
the belt, and it is not redundant: "the shell is idle" and "no subsystem
is mid-operation" are different statements, because fs/fat16.c's
writable BORROW can be open across a keyboard wait.  A poll that ran
then would call blk_scan() -- which rebuilds the block layer's table --
while an index into that table was live.

So each subsystem entry point below brackets itself with
kpoll_enter()/kpoll_leave(), and kernel_poll() refuses while the depth
is non-zero, with its own named refusal code.

── WHY THE SHAPE IS A WRAPPER AND WHY THAT IS WHAT IS CHECKED ─────────

An enter/leave pair added to a function BODY has to be paired on every
return path, and these functions have several.  So the body is not
edited at all: it is renamed to <name>_inner and a four-line wrapper
takes its place.  One entry, one exit, nothing to miss, and the pairing
is one line a reader can see.

This check therefore asserts the SHAPE rather than counting tokens:

  1. a static <name>_inner exists,
  2. the public <name> exists and its body calls kpoll_enter(),
     <name>_inner and kpoll_leave(), in that order,
  3. the body of the public <name> contains no `return` before the
     kpoll_leave() -- which is the one way a wrapper can still leak,
  4. and per file, kpoll_enter() and kpoll_leave() appear the same
     number of times.

(4) is the cheap global sanity check and (3) is the specific one.  Both
are here because v0.8.6's lesson was that a guard is only as good as
the thing that refuses to let it rot: three releases running, this tree
found a dispatch one layer below where the previous release guessed it
would be.

── WHY THESE FIVE AND NOT EVERYTHING ──────────────────────────────────

They are the entry points to the three subsystems the poll clients can
reach, and each owns module-level state:

  usb/core.c     usb_control     g_cfgbuf, and the back-ends' g_bounce
                                 and g_inctx below it
  usb/core.c     usb_enumerate   REBUILDS the device table
  fs/blkdev.c    blk_read        the sector buffers under it
  fs/blkdev.c    blk_write       likewise, and blk_scan() renumbers the
                                 table these index into
  net/ethernet.c eth_send        a static frame inside the function
  net/ethernet.c eth_poll        eth_rx_buf, handed to arp_handle() and
                                 ip_handle(), which reply

Not usb_bulk/usb_intr_poll: both are reached only from a driver that is
already inside one of the above, or from a poll client's own work, and
bracketing a function whose only callers are already bracketed buys
nothing but a longer list.  That is a judgement, it is written here
rather than left implicit, and if a third poll client ever calls one of
them directly this list is where it gets added.

── AND ONE COPY OF THE RESCAN SEQUENCE ────────────────────────────────

usb/hotplug.c owns usb_rescan_all().  `usb rescan` in shell/usb.c used
to spell the seven calls out, and the ORDER is the whole content -- each
step exists because of a specific bug.  Two copies would drift, which is
what tools/check_diskname_sync.py refuses one layer down.  So a second
spelling-out of the sequence is refused here.

    python3 tools/check_kpoll_guard.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csource

# (file, public entry point)
GUARDED = [
    ("usb/core.c",     "usb_control"),
    ("usb/core.c",     "usb_enumerate"),
    ("fs/blkdev.c",    "blk_read"),
    ("fs/blkdev.c",    "blk_write"),
    ("net/ethernet.c", "eth_send"),
    ("net/ethernet.c", "eth_poll"),
]

# The rescan sequence belongs to usb/hotplug.c alone.  A file that calls
# all three of these is spelling it out again.
RESCAN_CALLS = ("usb_enumerate", "msc_rescan", "blk_scan", "usbnet_probe")
RESCAN_OWNER = "usb/hotplug.c"
RESCAN_SCAN_DIRS = ("shell", "kernel", "fs", "net", "usb", "drivers")


def body_of(code, name):
    """The brace-balanced body of the first definition of `name`.

    Matches a definition, not a call: the name is followed by a
    parenthesised list and then a brace, with no semicolon between.
    """
    pat = re.compile(r"\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{")
    m = pat.search(code)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    for j in range(i, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return code[i:j + 1]
    return None


def check_guards(root="."):
    bad = []
    for rel, name in GUARDED:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            bad.append((rel, "%s: the file is not there" % name))
            continue
        code = csource.blank(open(path, encoding="utf-8",
                                  errors="replace").read())

        if not re.search(r"\bstatic\b[^;{}]*\b" + re.escape(name)
                         + r"_inner\s*\(", code):
            bad.append((rel, "%s has no static %s_inner -- the body "
                        "should be renamed, not edited" % (name, name)))
            continue

        body = body_of(code, name)
        if body is None:
            bad.append((rel, "%s: no definition found" % name))
            continue

        # The wrapper must not also be the inner function: body_of()
        # finds the first definition, and the inner one is defined
        # first in all six files.  Ask for the one that is not static.
        pub = re.search(r"(?<!static )\b" + re.escape(name)
                        + r"\s*\([^;{]*\)\s*\{", code)
        if pub:
            i = pub.end() - 1
            depth = 0
            for j in range(i, len(code)):
                if code[j] == "{":
                    depth += 1
                elif code[j] == "}":
                    depth -= 1
                    if depth == 0:
                        body = code[i:j + 1]
                        break

        i_enter = body.find("kpoll_enter")
        i_inner = body.find(name + "_inner")
        i_leave = body.find("kpoll_leave")
        if i_enter < 0:
            bad.append((rel, "%s does not call kpoll_enter()" % name))
            continue
        if i_leave < 0:
            bad.append((rel, "%s does not call kpoll_leave()" % name))
            continue
        if not (i_enter < i_inner < i_leave):
            bad.append((rel, "%s: the order must be kpoll_enter(), "
                        "%s_inner(), kpoll_leave()" % (name, name)))
            continue
        # A return before the leave is the one leak a wrapper can have.
        before = body[:i_leave]
        if re.search(r"\breturn\b", before):
            bad.append((rel, "%s returns before kpoll_leave() -- the "
                        "depth would never come back down" % name))

    # Per-file pairing.
    for rel in sorted({r for r, _ in GUARDED}):
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            continue
        code = csource.blank(open(path, encoding="utf-8",
                                  errors="replace").read())
        ne = len(re.findall(r"\bkpoll_enter\s*\(", code))
        nl = len(re.findall(r"\bkpoll_leave\s*\(", code))
        if ne != nl:
            bad.append((rel, "%d kpoll_enter() and %d kpoll_leave()"
                        % (ne, nl)))
    return bad


def check_rescan_copies(root="."):
    bad = []
    for d in RESCAN_SCAN_DIRS:
        path = os.path.join(root, d)
        if not os.path.isdir(path):
            continue
        for name in sorted(os.listdir(path)):
            if not name.endswith(".c"):
                continue
            rel = "%s/%s" % (d, name)
            if rel == RESCAN_OWNER:
                continue
            code = csource.blank(open(os.path.join(path, name),
                                      encoding="utf-8",
                                      errors="replace").read(),
                                 strip_externs=True)
            hits = [c for c in RESCAN_CALLS
                    if re.search(r"\b" + c + r"\s*\(", code)]
            if len(hits) == len(RESCAN_CALLS):
                bad.append((rel, "a second copy of the rescan sequence "
                            "(%s) -- call usb_rescan_all()"
                            % ", ".join(hits)))
    return bad


def main():
    bad = check_guards() + check_rescan_copies()
    if not bad:
        print("check_kpoll_guard: ok, %d entry point(s) bracketed, one "
              "copy of the rescan sequence" % len(GUARDED))
        return 0
    print("check_kpoll_guard: FAILED\n")
    for rel, why in bad:
        print("  %s  %s" % (rel, why))
    print("""
The shape each entry point has to keep:

    static int foo_inner(args) { ...the unchanged body... }

    int foo(args)
    {
        int r;
        kpoll_enter();
        r = foo_inner(args);
        kpoll_leave();
        return r;
    }

include/kpoll.h says why.  A poll reached from inside one of these
would reuse a module-level buffer that is already in use, and the
symptom of that is a once-an-hour impossibility.""")
    return 1


if __name__ == "__main__":
    sys.exit(main())
