#!/usr/bin/env python3
"""check_nic_dispatch.py -- one file chooses the NIC a frame goes to.

── WHY THIS EXISTS ────────────────────────────────────────────────────

v0.8.6 "Argo" ended with an instruction to its successor, in its own
words:

  > Carry this into 0.8.7. USB Ethernet is the next bus, and the lesson
  > of this release is that guarding the LABEL was never enough -- the
  > thing that chooses has to be one place too, and a grep has to be
  > able to see it. 0.8.7 adds no block device, but it does add a NIC,
  > and net/ethernet.c has not been grepped for the same shape.

net/ethernet.c was clean. It calls nic_send() and nic_recv() and names
no driver, and always has.

The shape was one layer below it, in net/net.c, and it was the same
shape exactly -- three switches on one `static int nic_type`:

    net/net.c   net_get_mac()   if(nic_type==1) rtl8139 else e1000
    net/net.c   nic_send()      if(nic_type==1) rtl8139 else e1000
    net/net.c   nic_recv()      if(nic_type==1) rtl8139 else e1000

plus a fourth place deciding the probe order inside net_init(), and the
numbering -- 0=none 1=rtl8139 2=e1000 -- recorded as a COMMENT above
the variable rather than as anything a compiler or a grep could check.

That is three of the four copies fs/blkdev.c was created to delete. The
fourth, an order decided in one function and depended on by another, is
the one that made the installer call a disk NVMe while the filesystem
bound itself to the ATA one, and nothing on any screen could say so.

Adding a USB NIC to that costs four edits. Forgetting the one in
net_get_mac() produces a machine that sends ARP replies claiming a MAC
address of 00:00:00:00:00:00 and cannot be told why -- the frames go
out, the counters go up, nothing comes back. So the table was written
BEFORE the driver, which is the order 0.8.6 wishes it had used, and this
check is what keeps it that way.

── WHAT IT REFUSES ───────────────────────────────────────────────────

A call to any NIC driver's send, receive or MAC function from outside
net/netdev.c and outside the driver that owns it. Those calls ARE the
dispatch: a file that makes one has chosen a NIC, and only one file is
allowed to.

Everything above the NIC layer calls netdev_send() / netdev_recv() /
netdev_mac() with an index, or nic_send() / nic_recv() / net_get_mac()
for whichever NIC is active, and names no driver at all.

A line may opt out with `nic-dispatch-ok: <reason>` in a comment on it
or within the six above, WITH a reason. A bare marker is how an
exemption becomes a habit -- and an exemption added to silence a bug in
a checker is one that will hide a real finding later.

Run:  python3 tools/check_nic_dispatch.py
"""
import os
import re
import sys

OWNER = "net/netdev.c"

# The per-driver entry points. Calling one of these IS choosing a NIC.
#
# init() is deliberately NOT here: bringing a controller up is not
# choosing where a frame goes, and netdev_scan() has to call it. The
# functions that move frames and the function that answers "who are we"
# are the three that must have one caller, because those are the three
# net/net.c had a switch for.
FUNCS = (
    "rtl8139_send", "rtl8139_recv", "rtl8139_get_mac",
    "e1000_send", "e1000_recv", "e1000_get_mac",
)

# A driver may call its own. Nothing else is listed, and that is the
# point: if this set ever needs an entry, the question to ask first is
# why something above the NIC layer knows which NIC it has.
EXEMPT = {
    OWNER,
    "drivers/rtl8139.c",
    "drivers/e1000.c",
}

# What counts as a reason after the marker lives in
# tools/csource.py, beside the comment-stripping, for the reason
# its own header gives: all three checks tested it with `\s*\S`,
# and in C the character after a bare marker is the `*` of the
# `*/` that closes the comment. A bare marker silenced all three.

# Telling code from comments lives in tools/csource.py, once -- see its
# header for what the obvious way to do it gets wrong, and what that
# cost in v0.8.6.1.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csource import code_lines, has_reason                    # noqa: E402


def main():
    if not os.path.isdir("shell"):
        sys.stderr.write("run me from the source root\n")
        return 2
    if not os.path.exists(OWNER):
        print("check_nic_dispatch: %s is missing" % OWNER)
        return 1

    call = re.compile(r"\b(%s)\s*\(" % "|".join(FUNCS))
    bad = []
    checked = 0

    for root, dirs, files in os.walk("."):
        dirs[:] = [d for d in dirs
                   if d not in (".git", "isodir", "tests", "harness",
                                "tools", "manual", "docs", "rescue",
                                "__pycache__")]
        for fn in files:
            if not fn.endswith(".c"):
                continue
            rel = os.path.relpath(os.path.join(root, fn), ".").replace("\\", "/")
            if rel in EXEMPT:
                continue
            checked += 1
            raw = open(os.path.join(root, fn), encoding="utf-8",
                       errors="replace").read()
            lines = raw.splitlines()
            code = code_lines(raw, strip_externs=True)
            for n, line in enumerate(lines, 1):
                code_line = code[n - 1] if n <= len(code) else ""
                m = call.search(code_line)
                if not m:
                    continue
                window = "\n".join(lines[max(0, n - 7):n])
                if has_reason(window, "nic-dispatch-ok"):
                    continue
                bad.append((rel, n, m.group(1), line.strip()))

    if bad:
        print("check_nic_dispatch: a NIC is chosen outside %s\n" % OWNER)
        for rel, n, f, line in bad:
            print("  %s:%d  %s()" % (rel, n, f))
            print("      %s" % line[:72])
        print("\n  Use netdev_send(i, data, len) / netdev_recv(i, buf, max) /")
        print("  netdev_mac(i, out) from include/netdev.h -- they take a NIC")
        print("  index, not a driver -- or nic_send() / nic_recv() /")
        print("  net_get_mac() for whichever NIC is active.")
        print("\n  Three functions in net/net.c used to switch on a driver")
        print("  number, and a fourth place decided what the numbers meant.")
        print("  If this call genuinely belongs below the NIC layer, add the")
        print("  file to EXEMPT here and say why, or mark the line")
        print("  `nic-dispatch-ok: <reason>`.")
        return 1

    print("check_nic_dispatch: ok, %d files choose no NIC; %s owns the "
          "dispatch for %d driver entry points"
          % (checked, OWNER, len(FUNCS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
