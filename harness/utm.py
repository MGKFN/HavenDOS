"""
Reproduce UTM's USB topologies in QEMU and see what HavenDOS makes of them.

The device lines are UTM's own, from
Configuration/UTMQemuConfiguration+Arguments.swift:

  USB 2.0 ->  ich9-usb-ehci1,id=usb-controller-0
              ich9-usb-uhci1,masterbus=usb-controller-0.0,firstport=0
              ich9-usb-uhci2,masterbus=usb-controller-0.0,firstport=2
              ich9-usb-uhci3,masterbus=usb-controller-0.0,firstport=4
  USB 3.0 ->  nec-usb-xhci,id=usb-controller-0      (on PC machines)

A drive whose Interface is USB becomes  -device usb-storage,drive=...
"""
import sys, time
sys.path.insert(0, '/home/claude/havendos/harness')
from qemu import Qemu, kill_stale
from boot import ISO, DISK

STICK = "/home/claude/havendos/usbstick.img"

def usb2_args(i=0):
    return ["-device", f"ich9-usb-ehci1,id=usb-controller-{i}",
            "-device", f"ich9-usb-uhci1,masterbus=usb-controller-{i}.0,firstport=0",
            "-device", f"ich9-usb-uhci2,masterbus=usb-controller-{i}.0,firstport=2",
            "-device", f"ich9-usb-uhci3,masterbus=usb-controller-{i}.0,firstport=4"]

def usb3_args(i=0):
    return ["-device", f"nec-usb-xhci,id=usb-controller-{i}"]

def stick_args(extra=""):
    return ["-drive", f"if=none,id=stick,file={STICK},format=raw",
            "-device", "usb-storage,drive=stick,removable=true" + extra]

def boot(extra, label):
    kill_stale()
    q = Qemu(ISO, disk=DISK, usb=False, extra=extra).start()
    time.sleep(6); q.key("down"); time.sleep(0.4); q.key("ret")
    if q.wait_for("Press any key", 150) is None:
        raise RuntimeError(label + ": POST never appeared")
    q.key("ret")
    if q.wait_for("System ready - press any key", 120) is None:
        raise RuntimeError(label + ": boot log never finished")
    q.key("ret")
    if q.wait_for("admin@havendos", 90) is None:
        raise RuntimeError(label + ": no shell prompt")
    time.sleep(0.5)
    return q

def run(q, cmd, settle=2.5):
    q.line(cmd); time.sleep(settle); return q.screen()

def body(screen, cmd):
    """Just the output of the last command, without the banner."""
    lines = screen.splitlines()
    out, seen = [], False
    for l in lines:
        if l.strip().endswith("# " + cmd) or l.strip().endswith("#" + cmd):
            seen = True; out = []; continue
        if seen: out.append(l)
    return "\n".join([l for l in out if l.strip()][:22])
