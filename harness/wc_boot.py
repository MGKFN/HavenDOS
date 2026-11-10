#!/usr/bin/env python3
"""wc_boot.py -- boot the ISO with write-combining on, BIOS and UEFI.

v0.8.9.6 turns paging on for EVERY machine with a linear framebuffer,
not only the ones whose framebuffer is above 4 GB, so the low-
framebuffer path -- which is QEMU's, both BIOS and UEFI -- now runs
with paging and a PAT entry it never had before. A suite can check the
page tables; only a boot can show the kernel still runs on them.

What this CANNOT show is the speed. QEMU's framebuffer is host memory
and ignores the memory type, so write-combining and uncached draw
exactly as fast here. The HP is the only judge of that.

Run:  python3 harness/wc_boot.py [path/to.iso]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu import Qemu, kill_stale                            # noqa: E402

ROOT = os.path.dirname(HERE)
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
OVMF = "/usr/share/OVMF/OVMF_CODE_4M.fd"

fails, total = [], [0]


def ck(what, ok, detail=""):
    total[0] += 1
    print(("    ok   " if ok else "    FAIL ") + what
          + ("" if ok or not detail else "\n         " + detail))
    if not ok:
        fails.append(what)


def to_shell(q):
    time.sleep(6)
    q.key("down")                      # GRUB entry 2: Shell Mode
    time.sleep(0.4)
    q.key("ret")
    # The prompt, not "System ready": the shell clears the boot log off
    # the screen as it starts, so on a fast boot that line is never
    # there to be seen.
    if q.wait_for("@havendos", 240) is None:
        q.key("ret")
        if q.wait_for("@havendos", 60) is None:
            raise RuntimeError("the shell prompt never appeared")
    time.sleep(1.0)


def last_line(scr):
    """The last non-empty line: where a pager prompt or a shell prompt
    actually is. The screen's final row is often blank."""
    lines = [l for l in scr.splitlines() if l.strip()]
    return lines[-1] if lines else ""


def dmesg_text(q):
    """Every page of `dmesg`, joined. Paged with the space bar only --
    any other key is a command name at a prompt."""
    q.type("dmesg\n")
    time.sleep(1.5)
    seen = []
    for _ in range(40):
        scr = q.screen()
        seen.append(scr)
        if "-- more --" not in last_line(scr):
            break
        q.key("spc")
        time.sleep(0.6)
    return "\n".join(seen)


def run(label, **kw):
    print(f"\n  {label}")
    kill_stale()
    q = Qemu(ISO, disk=None, usb=True, **kw).start()
    try:
        to_shell(q)
        ck("it boots to a shell with paging on", True)
        log = dmesg_text(q)
        ck("the boot log says the framebuffer is write-combining",
           "write-combining (PAT)" in log,
           "no 'Framebuffer memory: write-combining (PAT)' line in dmesg")
        # Out of the pager only if it is still open: a `q` at a prompt
        # becomes the first letter of the next command.
        if "-- more --" in last_line(q.screen()):
            q.key("q")
            time.sleep(0.8)
        q.type("ver\n")
        time.sleep(5)    # the first command after a pager is slow to show,
                         # in 0.8.9.5 exactly as here
        scr = q.screen()
        ck("and the shell still answers afterwards",
           "0.8.9" in scr, "\n".join(scr.splitlines()[-8:]))
        q.type("i2chid\n")
        time.sleep(1.5)
        scr = q.screen()
        ck("`i2chid` with no controller says so and how to find out why",
           "dmesg" in scr and "I2C" in scr, scr[-400:])
    finally:
        q.stop()


def main():
    if not os.path.exists(ISO):
        sys.exit(f"wc_boot: no ISO at {ISO} -- run make first")
    run("BIOS, i386, framebuffer below 4 GB")
    if os.path.exists(OVMF):
        run("UEFI, x86_64 with OVMF, framebuffer below 4 GB",
            binary="qemu-system-x86_64", mem=512,
            extra=["-drive", f"if=pflash,format=raw,readonly=on,file={OVMF}"])
    else:
        print("\n  (no OVMF; UEFI not tried)")
    print(f"\n{total[0] - len(fails)}/{total[0]} passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
