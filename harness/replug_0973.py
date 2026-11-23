#!/usr/bin/env python3
"""replug_0973.py -- unplugging and plugging back in, while HavenDOS runs.

Kyan, 22 Nov: "usb devices plugging back in out still an pain point for
this os that kind of stuff needs fixed". fixes_0966.py proved a replugged
mouse is BOUND again (a log line). This proves it WORKS again, and adds
the cases a person actually does:

  1. the USB mouse moves the pointer (the `mouse` command reads it)
  2. unplugged, plugged back in a few seconds later: it moves it again
  3. unplugged and plugged straight back in, inside one poll interval:
     it moves it again (the case a connected-ports bitmap cannot see)
  4. a stick: /USB lists it; pulled, /USB is gone; back, /USB lists it
  5. a stick pulled and put straight back: /USB still lists it
  6. a USB keyboard unplugged and back: what it types reaches the Shell

All on xHCI, the HP's controller, with QEMU's device_del/device_add.

Run:  python3 harness/replug_0973.py [path/to.iso]   (foreground)
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
from mouse_apps import boot_to_shell, ev                # noqa: E402
from usb_storage import make_stick                      # noqa: E402

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
WORK = "/tmp/hd_replug"
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-1200:].replace("\n", "\n          "))
    return ok


def pos(q):
    q.line("cls"); time.sleep(0.8)
    q.line("mouse"); time.sleep(1.2)
    m = re.findall(r"mouse: x (\d+)\s+y (\d+)", q.screen())
    return (int(m[-1][0]), int(m[-1][1])) if m else None


def nudge(q, dx, dy):
    for _ in range(abs(dx) // 4):
        ev(q, [{"type": "rel", "data": {"axis": "x", "value": 4 if dx > 0 else -4}}]); time.sleep(0.02)
    for _ in range(abs(dy) // 4):
        ev(q, [{"type": "rel", "data": {"axis": "y", "value": 4 if dy > 0 else -4}}]); time.sleep(0.02)
    time.sleep(0.6)


def moves(q, label):
    a = pos(q)
    nudge(q, 60, 40)
    b = pos(q)
    nudge(q, -60, -40)
    c = pos(q)
    return check(label, a is not None and b is not None and c is not None and (b != a or c != b),
                 "%s -> %s -> %s" % (a, b, c))


def usb_lists(q):
    q.line("cls"); time.sleep(0.8)
    q.line("ls /USB"); time.sleep(2.5)
    return "ARGO.TXT" in q.screen()


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    print("plugging back in: mouse, stick and keyboard, slow and quick, on xHCI")
    os.makedirs(WORK, exist_ok=True)
    # One image per plug-in: QEMU deletes a -drive with the device it
    # backs, so the stick put back is the same contents in a new drive.
    import shutil
    stick = make_stick(os.path.join(WORK, "stick.img"), mb=64, fat=32)
    for n in (2, 3):
        shutil.copy(stick, os.path.join(WORK, "stick%d.img" % n))
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256, extra=[
            "-device", "qemu-xhci,id=xhci",
            "-device", "usb-mouse,bus=xhci.0,id=m1",
            "-drive", "if=none,id=stk,file=%s,format=raw" % stick,
            "-drive", "if=none,id=stk2,file=%s,format=raw" % os.path.join(WORK, "stick2.img"),
            "-drive", "if=none,id=stk3,file=%s,format=raw" % os.path.join(WORK, "stick3.img"),
            "-device", "usb-storage,bus=xhci.0,drive=stk,id=s1"]) as q:
        boot_to_shell(q)
        moves(q, "the USB mouse moves the pointer")

        q.cmd_qmp("device_del", id="m1"); time.sleep(4)
        q.cmd_qmp("device_add", driver="usb-mouse", bus="xhci.0", id="m2"); time.sleep(5)
        moves(q, "unplugged, back a few seconds later: it moves the pointer again")

        q.cmd_qmp("device_del", id="m2"); time.sleep(0.15)
        q.cmd_qmp("device_add", driver="usb-mouse", bus="xhci.0", id="m3"); time.sleep(6)
        moves(q, "unplugged and straight back in (under a second): it still moves it")

        check("the stick is /USB", usb_lists(q))
        q.cmd_qmp("device_del", id="s1"); time.sleep(5)
        q.line("cls"); time.sleep(0.8)
        q.line("ls /USB"); time.sleep(2.5)
        check("pulled: /USB is gone", "not a folder" in q.screen(), q.screen()[-400:])
        q.cmd_qmp("device_add", driver="usb-storage", bus="xhci.0", drive="stk2", id="s2"); time.sleep(6)
        check("put back: /USB lists it again", usb_lists(q))
        q.cmd_qmp("device_del", id="s2"); time.sleep(0.15)
        q.cmd_qmp("device_add", driver="usb-storage", bus="xhci.0", drive="stk3", id="s3"); time.sleep(7)
        check("pulled and straight back: /USB still lists it", usb_lists(q))
        moves(q, "and the mouse still works after all of that")

        # v0.9.7.4. Kyan on the HP: "works once after that nothing all usb
        # stuff gone". More replugs than the controller has slots (8),
        # alternating slow and quick, spaced past the storm window.
        for i in range(10):
            old, new = "m%d" % (3 + i), "m%d" % (4 + i)
            q.cmd_qmp("device_del", id=old)
            time.sleep(0.15 if i % 2 else 3)
            q.cmd_qmp("device_add", driver="usb-mouse", bus="xhci.0", id=new)
            time.sleep(6)
        moves(q, "TEN MORE REPLUGS (more than the 8 slots): the mouse still moves")
        check("...and the stick is still /USB", usb_lists(q))

        q.cmd_qmp("device_add", driver="usb-kbd", bus="xhci.0", id="k1"); time.sleep(5)
        q.cmd_qmp("device_del", id="k1"); time.sleep(4)
        q.cmd_qmp("device_add", driver="usb-kbd", bus="xhci.0", id="k2"); time.sleep(5)
        q.line("cls"); time.sleep(0.8)
        # QMP send-key goes to the focused keyboard; with a USB keyboard
        # attached last, QEMU routes keys to it
        q.line("echo replugged-keyboard"); time.sleep(1.5)
        check("a USB keyboard plugged back in types into the Shell",
              "replugged-keyboard" in q.screen().split("echo replugged-keyboard")[-1], q.screen()[-400:])

    print("\n%d/%d" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
