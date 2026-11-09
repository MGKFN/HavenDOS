#!/usr/bin/env python3
"""check_harnesses.py -- every injection harness still builds its suite.

── WHY THIS EXISTS ────────────────────────────────────────────────────

v0.8.5.1 moved the 64-bit BAR decoder out of drivers/pci.c into
drivers/pci_bar.c, and the Makefile learned to link drivers/pci_bar.c
into the four suites that need it. tools/check_nvmerw_suite.py's own
compiler line did not, so every run of it since has ended in

    undefined reference to `pci_bar_mem64'

and reported nothing at all. Found in v0.8.6, five releases later, by
running all fifteen harnesses in a row for the first time.

An injection harness is the thing you trust INSTEAD OF THINKING about
whether a suite can fail. One that cannot build has proved nothing, and
one nobody runs cannot tell you it is broken -- which is the same disease
v0.8.5.6 found in `make test`, where 31 suites ended in `| tail -1` and a
failing suite exited 0. The 20 October handoff's table of harness results
does not list nvmerw at all, which is how it went unnoticed.

── WHY IT ONLY BUILDS ────────────────────────────────────────────────

Running all fifteen harnesses properly takes several minutes: each one
compiles and runs its suite once per injection, and there are 350-odd
injections between them. That is a thing to run deliberately, not on
every `make`.

Building each harness's baseline is seconds, and it is the failure that
actually happened. So this is the cheap half, in the build, and the
harnesses themselves stay a deliberate run.

Each harness exposes its compiler line as CC and its suite as TEST, and
any extra sources it links as EXTRA or LINK -- several suites LINK a real
driver rather than stubbing it ("a stub is not the function", v0.8.5.1),
and a harness that builds without those files is building a different
program from the one `make test` builds. That difference is exactly what
went wrong in check_nvmerw_suite.py.

A harness that exposes none of them is listed in SPECIAL with the reason,
so one cannot escape this check by not having the right variable names.

Run:  python3 tools/check_harnesses.py
"""
import glob
import importlib.util
import os
import subprocess
import sys
import tempfile

# Harnesses whose shape this check cannot read automatically, and why.
SPECIAL = {
    # check_hub_suite.py drives two source files and names them CORE and
    # HUB; its CC and TEST are still readable, so it needs no exception.
}


def load(path):
    name = os.path.splitext(os.path.basename(path))[0]
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def main():
    if not os.path.isdir("tools"):
        sys.stderr.write("run me from the source root\n")
        return 2

    harnesses = sorted(glob.glob("tools/check_*_suite.py"))
    if not harnesses:
        print("check_harnesses: no injection harnesses found at all, which "
              "is itself the answer")
        return 1

    bad, skipped, ok = [], [], 0
    for path in harnesses:
        base = os.path.basename(path)
        if base in SPECIAL:
            skipped.append((base, SPECIAL[base]))
            continue
        try:
            mod = load(path)
        except Exception as e:                       # noqa: BLE001
            bad.append((base, "will not even import: %s" % e))
            continue

        cc = getattr(mod, "CC", None)
        test = getattr(mod, "TEST", None)
        # LINK, where a harness has one, is the WHOLE source list
        # including the suite; EXTRA is the suite plus these.
        link = getattr(mod, "LINK", None)
        extra = getattr(mod, "EXTRA", None) or []
        if not cc or not (test or link):
            bad.append((base, "exposes no CC/TEST, so this check cannot "
                              "see what it builds -- add it to SPECIAL "
                              "with a reason, or name them"))
            continue
        sources = list(link) if link else ([test] + list(extra))
        missing = [f for f in sources
                   if f.endswith((".c", ".S")) and not os.path.exists(f)]
        if missing:
            bad.append((base, "these sources do not exist: %s"
                              % ", ".join(missing)))
            continue

        exe = os.path.join(tempfile.gettempdir(), "hd_harn_" + base)
        r = subprocess.run(list(cc) + ["-o", exe] + sources,
                           capture_output=True, text=True)
        if r.returncode != 0:
            tail = [l for l in r.stderr.splitlines()
                    if "error" in l or "undefined" in l][-3:]
            bad.append((base, "its baseline does not build:\n        "
                              + "\n        ".join(tail or ["(see above)"])))
            continue
        ok += 1

    for base, why in skipped:
        print("check_harnesses: %s skipped -- %s" % (base, why))

    if bad:
        print("check_harnesses: %d harness(es) cannot run:\n" % len(bad))
        for base, why in bad:
            print("  %s\n      %s" % (base, why))
        print("\n  A harness that does not build has proved nothing, and a")
        print("  harness nobody runs cannot tell you it is broken. Build it")
        print("  the same way the Makefile builds that suite -- the two")
        print("  compiler lines drifting apart is how this check came to")
        print("  exist (tools/check_nvmerw_suite.py, five releases).")
        return 1

    print("check_harnesses: ok, %d injection harness(es) build their suites"
          % ok)
    return 0


if __name__ == "__main__":
    sys.exit(main())
