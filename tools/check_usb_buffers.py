#!/usr/bin/env python3
"""check_usb_buffers.py -- a driver must not ask for more than the
back-end can carry, and the simulator must not be kinder than the real one.

── WHY THIS EXISTS ────────────────────────────────────────────────────

usb/xhci_hc.c copies every transfer through a fixed buffer and REFUSES
anything longer, before the controller is touched:

    x_data()       len > sizeof g_bounce[0]   -> USB_EPARAM
    x_intr_poll()  len > XHCI_ABUF_SZ         -> USB_EPARAM

Both were sized for what this kernel used to move. g_bounce was 1024
because the largest thing was a 512-byte sector. XHCI_ABUF_SZ was 64
because the only asynchronous IN was a keyboard report. Both were right
for three releases.

v0.8.7 added a driver that moves ETHERNET FRAMES -- 1514 bytes out,
1516 in. Neither fitted. The first `usb eth` on the HP:

    carrier 0   rx 0/0/19440   tx 0/48

Nineteen thousand four hundred and forty receive failures, forty-eight
refused frames, and not one byte requested from the device. The driver
was correct; it was refused by its own back-end, locally, every time.

tests/test_usbnet.c did not catch it because its simulated controller
accepted a transfer of any length -- A BACK-END MORE PERMISSIVE THAN
THE REAL ONE, which is the same mistake as a mock that cannot refuse.
The suite enforces both bounds now, so this cannot pass again.

That leaves the numbers, in three files, which is where things drift.

── WHAT IT REFUSES ───────────────────────────────────────────────────

  1. A driver transfer-length constant larger than the back-end bound
     it will be refused by.
  2. A simulated bound in tests/ that does not EQUAL the kernel's.
     Not "at least" -- equal. A simulator that is stricter would fail
     working code, and one that is kinder is how this shipped.

Run:  python3 tools/check_usb_buffers.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csource import blank                                     # noqa: E402

XHCI   = "usb/xhci_hc.c"
SUITE  = "tests/test_usbnet.c"

# Driver constants, and which bound each one meets.
#   file, constant, which bound ("sync" | "async"), what it is
DRIVERS = [
    ("usb/ethernet.c", "IPHETH_TX_LEN", "sync",
     "the padded transmit, sent with usb_bulk()"),
    ("usb/ethernet.c", "IPHETH_RX_LEN", "async",
     "the receive, read with usb_bulk_poll()"),
    ("usb/ethernet.c", "IPHETH_CTRL_BUF", "sync",
     "the vendor control requests"),
    ("usb/usbmux.c", "MUX_BUF", "sync",
     "a usbmux packet, sent with usb_bulk()"),
    ("usb/usbmux.c", "MUX_BUF", "async",
     "a usbmux packet, read with usb_bulk_poll()"),
]


def source(path):
    return blank(open(path, encoding="utf-8", errors="replace").read())


# A constant may be written in terms of other constants --
# IPHETH_RX_LEN is (IPHETH_TX_LEN + IPHETH_RX_SKIP) -- so the table is
# read once and the value resolved from it. Only integers, the names in
# that table, and + - * / ( ) are accepted; anything else is reported
# rather than guessed at.
SAFE = re.compile(r'^[0-9A-Za-z_+\-*/() \t]+$')


def defines(text):
    out = {}
    for m in re.finditer(r'^\s*#\s*define\s+([A-Za-z_]\w*)\s+(.+?)\s*$',
                         text, re.M):
        out[m.group(1)] = m.group(2)
    return out


def define(text, name, _depth=0):
    table = text if isinstance(text, dict) else defines(text)
    if name not in table or _depth > 8:
        return None
    expr = table[name]
    if not SAFE.match(expr):
        return None
    try:
        names = set(re.findall(r'[A-Za-z_]\w*', expr))
        env = {}
        for n in names:
            v = define(table, n, _depth + 1)
            if v is None:
                return None
            env[n] = v
        return int(eval(expr, {"__builtins__": {}}, env))
    except Exception:
        return None


def main():
    if not os.path.isdir("usb"):
        sys.stderr.write("run me from the source root\n")
        return 2
    for f in (XHCI, SUITE):
        if not os.path.exists(f):
            print("check_usb_buffers: %s is missing" % f)
            return 1

    x = source(XHCI)

    m = re.search(r'g_bounce\s*\[\s*XHCI_MAX_HC\s*\]\s*\[\s*(\d+)\s*\]', x)
    if not m:
        print("check_usb_buffers: cannot find g_bounce's size in %s" % XHCI)
        return 1
    sync = int(m.group(1))

    asyn = define(x, "XHCI_ABUF_SZ")
    if asyn is None:
        print("check_usb_buffers: cannot find XHCI_ABUF_SZ in %s" % XHCI)
        return 1

    bound = {"sync": (sync, "sizeof g_bounce[0]", "x_data"),
             "async": (asyn, "XHCI_ABUF_SZ", "x_intr_poll")}

    bad = []
    for path, name, kind, what in DRIVERS:
        if not os.path.exists(path):
            continue
        v = define(source(path), name)
        if v is None:
            bad.append("%s: %s is not a plain #define any more" % (path, name))
            continue
        lim, limname, fn = bound[kind]
        if v > lim:
            bad.append(
                "%s: %s is %d, but %s() refuses anything over %s = %d\n"
                "      (%s)" % (path, name, v, fn, limname, lim, what))

    # The simulator must agree exactly.
    s = source(SUITE)
    for const, kind in (("SIM_SYNC_MAX", "sync"), ("SIM_ASYNC_MAX", "async")):
        v = define(s, const)
        lim, limname, _ = bound[kind]
        if v is None:
            bad.append("%s: %s is missing -- the simulated controller "
                       "must carry the real bound" % (SUITE, const))
        elif v != lim:
            bad.append(
                "%s: %s is %d but the kernel's %s is %d\n"
                "      A simulated back-end kinder than the real one is how "
                "v0.8.7 shipped." % (SUITE, const, v, limname, lim))

    if bad:
        print("check_usb_buffers: a transfer will be refused by its own "
              "back-end\n")
        for b in bad:
            print("  %s" % b)
        print("\n  Raise the back-end buffer in %s, or lower the driver's" % XHCI)
        print("  constant. Whichever moves, the simulated bound in")
        print("  %s moves with it -- the suite covers the" % SUITE)
        print("  behaviour and this check covers the numbers.")
        return 1

    print("check_usb_buffers: ok, sync %d / async %d bytes; %d driver "
          "constant(s) fit, and the simulator carries both bounds"
          % (sync, asyn, len(DRIVERS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
