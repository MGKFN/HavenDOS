#!/usr/bin/env python3
"""Install the current ISO to a blank disk and run the first-boot wizard, so
the result is a system that boots straight to a login.  The installed system
runs the kernel the installer copied, NOT the one in the ISO you boot next
time - so any kernel change needs this before it can be tested from disk."""
import sys, time, shutil
sys.path.insert(0, '/home/claude/havendos/harness')
import capture as C
from qemu import Qemu, kill_stale


def build():
    C.blank_disk(C.BLANK)
    kill_stale()
    q = Qemu(C.ISO, disk=C.BLANK, usbstick=C.STICK).start()
    try:
        C.waitfor(q, "Installer", timeout=150)
        q.key("end"); time.sleep(0.9); q.key("a"); time.sleep(1.3)
        q.key("end"); time.sleep(0.9); q.key("a"); time.sleep(1.5)
        q.key("ret"); time.sleep(1.5)
        q.line("yes"); time.sleep(1.5)
        if not C.waitfor(q, "Rebooting", timeout=150, poll=0.3):
            raise SystemExit("install never finished")
    finally:
        q.stop()
    shutil.copy(C.BLANK, C.DISK)

    q = C.boot_disk()
    try:
        # first_run() photographs the screens it walks through; this helper is
        # used to rebuild a test disk, not to capture, and it was overwriting
        # the install-media shots every time it ran.
        C.shot = lambda *a, **k: None
        C.first_run(q)
        if not C.waitfor(q, "havendos:", timeout=90):
            print("  (did not reach a shell prompt; wizard state may differ)")
    finally:
        q.stop()
    print("ready:", C.DISK)


if __name__ == "__main__":
    build()
