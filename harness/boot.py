"""Boot HavenDOS to a shell prompt and run commands, headless."""
import sys, time
sys.path.insert(0, '/home/claude/havendos/harness')
from qemu import Qemu, kill_stale

ISO = "/home/claude/havendos/HavenDOS-0.7.20-source/HavenDOS-0.7.21.iso"
DISK = "/home/claude/havendos/disk.img"

def boot_shell(iso=None, disk=None, usbstick=None, audio=False, verbose=True,
               extra=None):
    kill_stale()
    q = Qemu(iso or ISO, disk=disk or DISK, usbstick=usbstick, audio=audio,
             extra=extra).start()
    time.sleep(6)
    q.key("down"); time.sleep(0.4); q.key("ret")      # GRUB entry 1: Shell Mode
    if q.wait_for("Press any key", 120) is None:
        raise RuntimeError("POST screen never appeared")
    q.key("ret")
    if q.wait_for("System ready - press any key", 90) is None:
        raise RuntimeError("boot log never completed")
    q.key("ret")
    if q.wait_for("admin@havendos", 60) is None:
        raise RuntimeError("shell prompt never appeared")
    time.sleep(0.5)
    return q

def run(q, cmd, settle=2.0):
    """Run one shell command and return the screen afterwards."""
    q.line(cmd)
    time.sleep(settle)
    return q.screen()

def run_scroll(q, cmd, pages=1, settle=2.0):
    """Run a command whose output may need paging."""
    out = [run(q, cmd, settle)]
    for _ in range(pages - 1):
        q.key("spc"); time.sleep(0.8)
        out.append(q.screen())
    return out
