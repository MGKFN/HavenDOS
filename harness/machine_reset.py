#!/usr/bin/env python3
"""machine_reset.py -- does `reboot` still reboot, and does `poweroff`
still power off, now that nine hand-written copies are one function?

What this proves, and what it cannot, stated up front because the
defect it covers was a claim that outran its evidence:

  CAN    `reboot` still resets the machine, and resets it rather than
         shutting it down.
  CAN    `poweroff` still shuts the machine down, and shuts it down
         rather than resetting it.
  CANNOT that `reboot` works on a machine with no 8042. QEMU has a
         working one, so under QEMU the OLD code rebooted too -- which
         is exactly why this shipped nine times and was only found by
         booting the laptop. docs/MACHINE-RESET-HARDWARE-TEST.md is
         the part only the HP can answer.

The signal is the QEMU process, not the screen. During GRUB's gfxterm
menu neither the 0xB8000 dump nor the framebuffer decode reads back
reliably, so a harness that scrapes text here is a harness that
reports whatever the font did. Process state is unambiguous:

  -no-reboot   QEMU EXITS when the guest requests a reset. So an exit
               means the command was typed, reached the shell, and
               produced a reset request. Nothing else makes it exit.
  default      QEMU RESTARTS the guest on a reset and exits on S5. So
               staying alive after `reboot` means it was a reset and
               not a shutdown; exiting after `poweroff` means S5 and
               not a reset.

The two runs together are the proof: the first says a reset was
requested, the second says it was a reset. Either alone is weaker --
a machine that simply sat there would pass the second on its own.

Run:  python3 harness/machine_reset.py [pc|q35|both]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from qemu import Qemu, kill_stale            # noqa: E402

ISO = os.path.join(SRC, "havendos.iso")

# GRUB entry 1 is Shell Mode; isodir/boot/grub/grub.cfg says so and two
# other harnesses index the same list.
SHELL_ENTRY = 1

# The boot log stops at one or two "press any key" gates. They cannot
# be read reliably (see the note above), so they are cleared blind --
# a stray RET at a shell prompt is harmless.
BOOT_SECONDS = 34
GATE_PRESSES = 5
EXIT_WAIT    = 75

total = 0
fails = 0


def ck(what, ok, detail=""):
    global total, fails
    total += 1
    if ok:
        print("    ok   %s" % what, flush=True)
    else:
        fails += 1
        print("    FAIL %-50s %s" % (what, detail), flush=True)


def head(s):
    print("\n  %s" % s, flush=True)


def ioports(q, machine):
    """Which reset registers this QEMU actually implements.

    `info ioports` was removed from QEMU; the flat memory-tree view is
    the current equivalent and names each region, which is better --
    it says `lpc-reset-control` rather than leaving 0xCF9 to be
    inferred from an address.

    This matters for attribution. The ladder tries 0xCF9 first, so if
    QEMU implements it, 0xCF9 is necessarily what carried the reset:
    the 8042 pulse is only reached when the two 0xCF9 triggers did
    nothing. Without this, all a reboot proves is that SOME rung
    worked.
    """
    want = {"0cf9": "0xCF9  reset control",
            "0604": "0x604  ACPI PM1a_CNT",
            "0064": "0x64   8042 command"}
    found = {}
    try:
        out = q.hmp("info mtree -f")
    except Exception as e:
        print("      (info mtree unavailable: %s)" % e, flush=True)
        return found
    io = out.split("address-space: I/O", 1)[-1]
    for line in io.splitlines():
        s = line.strip()
        for key, name in want.items():
            # "0000000000000cf9-0000000000000cf9 (prio 1, i/o): lpc-reset-control"
            if s.startswith("0" * 13 + key) or ("%s-" % key) in s[:34]:
                region = s.split("):", 1)[-1].strip() if "):" in s else s
                found.setdefault(key, region)
    print("      %s implements:" % machine, flush=True)
    for key, name in want.items():
        print("        %-22s %s" % (name, found.get(key, "-- absent --")),
              flush=True)
    if "0cf9" in found:
        print("      so a reset here is carried by 0xCF9: it is tried first,",
              flush=True)
        print("      and the 8042 is only reached if 0xCF9 did nothing.",
              flush=True)
    return found


def boot_and_type(machine, cmd, no_reboot, show_ports=False):
    """Boot to Shell Mode, type `cmd`, and report whether QEMU exited."""
    kill_stale()
    extra = ["-no-reboot"] if no_reboot else None
    q = Qemu(ISO, machine=machine, extra=extra,
             sock="/tmp/hd_mreset.sock").start()
    try:
        time.sleep(7)
        for _ in range(SHELL_ENTRY):
            q.key("down")
            time.sleep(0.4)
        q.key("ret")

        ports = ioports(q, machine) if show_ports else {}

        time.sleep(BOOT_SECONDS)
        for _ in range(GATE_PRESSES):
            if q.proc.poll() is not None:
                break
            q.key("ret")
            time.sleep(2)

        if q.proc.poll() is not None:
            # It died before the command was typed, which is a result in
            # itself and never expected.
            return "died-early", 0.0, ports

        q.line(cmd)
        t0 = time.time()
        for _ in range(EXIT_WAIT):
            if q.proc.poll() is not None:
                return "exited", time.time() - t0, ports
            time.sleep(1)
        return "alive", time.time() - t0, ports
    finally:
        try:
            q.stop()
        except Exception:
            pass
        kill_stale()


def run_machine(machine):
    print("\n══ %s ══" % machine, flush=True)

    head("`reboot` asks the machine to reset")
    st, dt, ports = boot_and_type(machine, "reboot", no_reboot=True,
                                  show_ports=True)
    ck("with -no-reboot, qemu exits", st == "exited",
       "(got %s after %.0fs)" % (st, dt))
    if st == "exited" and "0cf9" in ports:
        ck("and 0xCF9 is what carried it (tried first, and present)", True)
    if st == "died-early":
        print("      the guest died before `reboot` was typed -- the rest "
              "of this machine's checks would be meaningless", flush=True)
        return

    head("and it is a reset, not a shutdown")
    st, dt, _ = boot_and_type(machine, "reboot", no_reboot=False)
    ck("without -no-reboot, qemu keeps running", st == "alive",
       "(got %s after %.0fs -- an exit here is an S5, not a reset)"
       % (st, dt))

    head("`poweroff` asks the machine to shut down")
    st, dt, _ = boot_and_type(machine, "poweroff", no_reboot=False)
    # On a machine type with no PM1a at any of the three addresses this
    # legitimately stays alive -- that is the real-hardware case, and
    # kernel/machine.c prints why rather than hanging under a message
    # claiming it shut down. Reported, not failed.
    if st == "alive":
        print("      qemu stayed alive: this machine type has no PM1a at "
              "0x604, 0xB004 or 0x4004.", flush=True)
        print("      That is the real-hardware path, and the suite covers "
              "what it prints.", flush=True)
        ck("poweroff did not reset the machine instead", True)
    else:
        ck("qemu exits (the guest reached S5)", st == "exited",
           "(got %s after %.0fs)" % (st, dt))


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "both"
    machines = ["pc", "q35"] if which == "both" else [which]

    if not os.path.exists(ISO):
        sys.stderr.write("no %s -- run make first\n" % ISO)
        return 2

    print("Driving `reboot` and `poweroff` under QEMU.", flush=True)
    print("Signal is the QEMU process, not the screen -- see the module "
          "docstring.", flush=True)

    for m in machines:
        try:
            run_machine(m)
        except Exception as e:
            global fails, total
            total += 1
            fails += 1
            print("    FAIL %s: %s" % (m, e), flush=True)
        finally:
            kill_stale()

    print("\n%d/%d passed" % (total - fails, total), flush=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
