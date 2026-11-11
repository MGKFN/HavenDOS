"""hid_boot.py -- boot the ISO with a USB keyboard and mouse on xHCI.

v0.9.2.1. usb/hid.c now surveys every interface of a HID device and
binds each boot keyboard and boot mouse it finds. tests/test_usbhid.c
proves that against a simulated controller; this proves the change
did not break the plain case on QEMU's real xHCI emulation:

  - a usb-kbd and a usb-mouse behind qemu-xhci are both found, and the
    log names the INTERFACE each was bound on (new in 0.9.2.1)
  - the shell still answers afterwards

QEMU has no multi-interface receiver to offer, so the receiver case is
the suite's and the HP's.

Run:  python3 harness/hid_boot.py [path/to.iso]
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

fails = []
total = [0]


def ck(what, ok, detail=""):
    total[0] += 1
    if ok:
        print(f"    ok   {what}")
    else:
        fails.append(what)
        print(f"    FAIL {what}" + (f"\n         {detail}" if detail else ""))


def version():
    h = open(os.path.join(ROOT, "include", "version.h"), encoding="utf-8").read()
    return re.search(r'#define\s+HAVENDOS_VER_NUM\s+"([^"]+)"', h).group(1)


def main():
    ver = version()
    print(f"Booting {os.path.basename(ISO)} with usb-kbd + usb-mouse on xHCI.\n")
    kill_stale()
    q = Qemu(ISO, disk=None, usb=False,
             extra=["-device", "qemu-xhci,id=xhci",
                    "-device", "usb-kbd,bus=xhci.0",
                    "-device", "usb-mouse,bus=xhci.0"]).start()
    try:
        time.sleep(6)
        q.key("down")
        time.sleep(0.4)
        q.key("ret")
        if q.wait_for("admin@havendos", 240) is None:
            q.key("ret")
            if q.wait_for("admin@havendos", 60) is None:
                raise RuntimeError("the shell prompt never appeared")
        time.sleep(1.0)

        def run(cmd, settle=3.0):
            q.line(cmd)
            time.sleep(settle)
            return q.screen()

        print("\n  both devices are bound, each on its interface")
        q.line("dmesg")
        time.sleep(3)
        seen = q.screen()
        # page through the log, collecting it
        for _ in range(12):
            if "USB keyboard" in seen and "USB mouse" in seen:
                break
            q.key("spc")
            time.sleep(0.8)
            seen += q.screen()
        if "more --" in q.screen()[-400:]:
            q.key("q")                    # leave the pager, if still in it
            time.sleep(0.8)
        ck("the keyboard is bound, on interface 0",
           re.search(r"USB keyboard \S+ interface 0", seen) is not None, seen[-900:])
        ck("the mouse is bound, on interface 0",
           re.search(r"USB mouse \S+ interface 0", seen) is not None, seen[-900:])

        print("\n  the shell is still answering")
        scr = run("ver")
        ck(f"it reports v{ver}", ver in scr, scr[-300:])
    finally:
        q.stop()

    print(f"\n{total[0] - len(fails)}/{total[0]} passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
