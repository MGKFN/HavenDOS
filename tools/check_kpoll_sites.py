#!/usr/bin/env python3
"""check_kpoll_sites.py -- where periodic work may be run from.

v0.8.8 "Helios" gave HavenDOS a tick.  include/kpoll.h has the argument
for why the tick sets flags and a separate kernel_poll() does the work:
every layer in this tree shares module-level buffers and none of it is
reentrant, so work run from the wrong place corrupts a transfer that is
already in progress.  The symptom is a once-an-hour impossibility, and
v0.8.7.5's shared event ring is the record of what one of those costs --
one unanswered DHCP write broke descriptor reads on a webcam, and it
took a release to find.

A DESIGN THAT RELIES ON NOBODY CALLING THE WRONG FUNCTION IS A COMMENT.
This is the rule.  Three things are refused:

  1. kernel_poll() called from a file not on the allowlist.  There are
     very few places in a kernel where nothing is in progress, and they
     are nameable: the keyboard's idle wait, and the polling sleep.

  2. kpoll_sleep_ms() anywhere under drivers/, usb/, net/ or fs/.
     kernel/pit.c's sleep_ms() is the driver-safe wait and is unchanged.
     The two look interchangeable and are not, which is exactly why
     they are two names rather than one function with a flag: a flag is
     something a caller gets wrong, a name is something a grep refuses.

  3. kpoll_register() called from a file not on the allowlist.  A
     client registered from inside a driver is a client nobody can find
     by reading one file, and `kpoll` printing a task nobody can locate
     is the boot log's old problem in a new screen.

A line that genuinely needs an exception spells  kpoll-site-ok  in a
comment on it, through tools/csource.py's has_reason() -- the same
marker mechanism check_bus_label.py and check_blk_dispatch.py use, and
for the same reason: an exception is a visible decision.

    python3 tools/check_kpoll_sites.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csource

OPT_OUT = "kpoll-site-ok"

SCAN_DIRS = ("kernel", "drivers", "shell", "fs", "net", "usb")

# Where the kernel is genuinely between operations.
#
#   drivers/keyboard.c   keyboard_waitchar()'s wait loop. The kernel is
#                        waiting for a person; by definition nothing is
#                        in progress. Every shell and app read path in
#                        the tree goes through it, which is why the list
#                        is this short.
#   kernel/kpoll.c       kpoll_sleep_ms() is the polling sleep itself.
#
# shell/shell.c is deliberately NOT here. Its prompt reads through
# keyboard_waitchar(), so a second call site would add no coverage and
# would widen the permission for nothing -- and an allowlist entry that
# nothing uses is an entry somebody later treats as precedent.
POLL_CALLERS = {
    "drivers/keyboard.c",
    "kernel/kpoll.c",
}

# Registration lives in one place so that the list of everything this
# machine does on a timer can be read in one file -- plus the two client
# files, each of which registers only its own.
REGISTER_CALLERS = {
    "kernel/kpoll.c",     # the implementation
    "kernel/main.c",      # the one place registration is called from
    "net/net_tick.c",
    "usb/hotplug.c",
}

# A polling sleep in any of these is the bug this check exists for.
NO_POLLING_SLEEP = ("drivers", "usb", "net", "fs")

CALL_POLL  = re.compile(r"\bkernel_poll\s*\(")
CALL_SLEEP = re.compile(r"\bkpoll_sleep_ms\s*\(")
CALL_REG   = re.compile(r"\bkpoll_register\s*\(")


def scan(root="."):
    bad = []
    for d in SCAN_DIRS:
        path = os.path.join(root, d)
        if not os.path.isdir(path):
            continue
        for name in sorted(os.listdir(path)):
            if not name.endswith(".c"):
                continue
            rel = "%s/%s" % (d, name)
            text = open(os.path.join(path, name), encoding="utf-8",
                        errors="replace").read()
            # Comments blanked, and extern declarations too: naming a
            # function is not calling it, and kpoll.h's own callers
            # declare it locally rather than including the header.
            code = csource.blank(text, strip_externs=True)
            raw = text.splitlines()
            for n, line in enumerate(code.splitlines(), 1):
                window = raw[n - 1] if n - 1 < len(raw) else ""
                if csource.has_reason(window, OPT_OUT):
                    continue
                if CALL_POLL.search(line) and rel not in POLL_CALLERS:
                    bad.append((rel, n, "kernel_poll() outside the "
                                "allowlist", raw[n - 1].strip()))
                if CALL_REG.search(line) and rel not in REGISTER_CALLERS:
                    bad.append((rel, n, "kpoll_register() outside the "
                                "allowlist", raw[n - 1].strip()))
                if CALL_SLEEP.search(line) and d in NO_POLLING_SLEEP:
                    bad.append((rel, n, "a POLLING sleep in a driver -- "
                                "sleep_ms() is the one to use",
                                raw[n - 1].strip()))
    return bad


def main():
    bad = scan()
    if not bad:
        print("check_kpoll_sites: ok, periodic work runs only from %d "
              "named place(s)" % len(POLL_CALLERS))
        return 0
    print("check_kpoll_sites: FAILED\n")
    for f, n, why, text in bad:
        print("  %s:%d  %s" % (f, n, why))
        print("      %s" % text[:100])
    print("""
include/kpoll.h has the argument.  kernel_poll() runs periodic work, and
the work reaches the USB core, the block layer and the network stack --
every one of which keeps a module-level buffer that is not reentrant.
It may be called only where nothing is in progress:

  %s

A driver that wants to wait calls sleep_ms(), which polls the PIT and
nothing else.  If a call here genuinely is at an idle point, add the
file above and say why; if one line genuinely needs it, mark the line
  kpoll-site-ok: <reason>""" % "\n  ".join(sorted(POLL_CALLERS)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
