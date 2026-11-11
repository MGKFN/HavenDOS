#!/usr/bin/env python3
"""check_gfx_readonly.py -- the display code writes only where it says.

── 0.9.1: ONE FILE MAY WRITE, AND ONLY IN ONE PLACE ──────────────────

drivers/xegmbus.c reads a monitor's EDID over GMBUS, which means writing
the six GMBUS registers (0xC5100-0xC5120). That file is allowed GFX_WR
under three conditions this check enforces:

  - GFX_WR is used ONCE outside its own #define, inside gmbus_wr();
  - gmbus_wr()'s body still holds the range guard (it names both
    GMBUS_REG_FIRST and GMBUS_REG_LAST);
  - nothing else in the file writes: no volatile store outside the
    macro, no PCI config write, no port, no MSR, no memory type.

Everything else stays as 0.9.0 left it: drivers/xedisp.c,
shell/gfxcmd.c and the pure parser drivers/edid.c do not write at all.

── 0.9.0, unchanged below ────────────────────────────────────────────

0.9.0 is the first step of a display-engine driver for the HP's Iris Xe,
and the promise it makes is that it only LOOKS. The laptop's panel is
being scanned out by that engine while the command runs; a stray write
to a pipe, a plane or a power well can blank the only screen the machine
has, on a laptop with no serial port, and there would be no way to read
what happened.

So "read only" is not left to a comment. This refuses the build if
drivers/xedisp.c or shell/gfxcmd.c contains any of:

  GFX_WR                 a write seam -- 0.9.2 adds one, with a
                         read-back beside it, and this check changes
                         with it on purpose
  a store through a volatile pointer
                         `*(volatile uint32_t *)p = v` is a register
                         write however it is spelt
  pci_cfg_write*, pci_enable, pci_set_d0
                         config space and power state are writes too
  outb/outw/outl, wrmsr  so are ports and MSRs
  highfb_map( / highfb_wc_low(
                         the framebuffer mappings change memory TYPES;
                         only highfb_map_io(), which adds an uncached
                         window and changes nothing else, is allowed

Comments are blanked first (tools/csource.py), so the prose explaining
all of this cannot trip the check that enforces it.

Run:  python3 tools/check_gfx_readonly.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csource

FILES = ("drivers/xedisp.c", "shell/gfxcmd.c", "drivers/edid.c")
WRITER = "drivers/xegmbus.c"

RULES = [
    (re.compile(r"\bGFX_WR\b"), "a register write seam (GFX_WR)"),
    (re.compile(r"\(\s*volatile\b[^;]*?\)\s*[^;=]*?=(?!=)"),
     "a store through a volatile pointer"),
    (re.compile(r"\bpci_cfg_write(8|16|32)?\s*\("), "a PCI config write"),
    (re.compile(r"\bpci_enable\s*\("), "pci_enable(), a config write"),
    (re.compile(r"\bpci_set_d0\s*\("), "pci_set_d0(), a power-state change"),
    (re.compile(r"\bout[bwl]\s*\("), "a port write"),
    (re.compile(r"\bwrmsr\b"), "an MSR write"),
    (re.compile(r"\bhighfb_map\s*\("), "highfb_map(), a memory-type change"),
    (re.compile(r"\bhighfb_wc_low\s*\("), "highfb_wc_low(), a memory-type change"),
]


def scan(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    bad = []
    for num, line in enumerate(csource.code_lines(text), 1):
        # Comparisons are reads: `a == b`, `a != b`, `a <= b`, `a >= b`.
        probe = re.sub(r"[=!<>]=", "  ", line)
        for rx, what in RULES:
            if rx.search(probe):
                bad.append((num, what, text.splitlines()[num - 1].strip()))
    return bad


def scan_writer(path):
    """drivers/xegmbus.c: GFX_WR once, in gmbus_wr(), behind the guard."""
    text = open(path, encoding="utf-8", errors="replace").read()
    lines = text.splitlines()
    code = csource.code_lines(text)
    bad = []
    # the function's body, by line numbers
    start = next((i for i, l in enumerate(code)
                  if re.search(r"\bgmbus_wr\s*\(", l) and "static" in l), None)
    end = None
    if start is not None:
        end = next((i for i in range(start, len(code)) if code[i] == "}"), None)
    if start is None or end is None:
        return [(0, "gmbus_wr() is missing -- the only place a write may be", "")]
    body = "\n".join(code[start:end + 1])
    if "GMBUS_REG_FIRST" not in body or "GMBUS_REG_LAST" not in body:
        bad.append((start + 1, "gmbus_wr() lost its range guard", lines[start].strip()))
    uses = 0
    in_def = False
    for i, line in enumerate(code):
        probe = re.sub(r"[=!<>]=", "  ", line)
        is_def = bool(re.match(r"\s*#\s*(define|ifndef|endif)\b", line)) or in_def
        in_def = is_def and line.rstrip().endswith("\\")
        if re.search(r"\bGFX_WR\b", probe) and not is_def:
            if start <= i <= end:
                uses += 1
            else:
                bad.append((i + 1, "GFX_WR outside gmbus_wr()", lines[i].strip()))
        for rx, what in RULES:
            if what.startswith("a register write seam"):
                continue                       # GFX_WR handled above
            if what.startswith("a store through") and is_def:
                continue                       # the macro's own body
            if rx.search(probe):
                bad.append((i + 1, what, lines[i].strip()))
    if uses != 1:
        bad.append((start + 1, "gmbus_wr() uses GFX_WR %d times, not once" % uses,
                    lines[start].strip()))
    return bad


def main():
    if not os.path.isdir("drivers"):
        sys.stderr.write("run me from the source root\n")
        return 2
    found = []
    for f in FILES:
        if not os.path.exists(f):
            sys.stderr.write("check_gfx_readonly: %s is missing\n" % f)
            return 2
        for num, what, src in scan(f):
            found.append((f, num, what, src))
    if not os.path.exists(WRITER):
        sys.stderr.write("check_gfx_readonly: %s is missing\n" % WRITER)
        return 2
    for num, what, src in scan_writer(WRITER):
        found.append((WRITER, num, what, src))
    if found:
        print("check_gfx_readonly: the display code writes where it "
              "must not\n")
        for f, num, what, src in found:
            print("  %s:%d  %s\n      %s" % (f, num, what, src))
        print("\n  The panel is being scanned out by this engine while the")
        print("  command runs. A write is 0.9.2's job, behind `gfx on`, with")
        print("  a read-back -- not something to add to an inspector.")
        return 1
    print("check_gfx_readonly: ok, %s never write; %s writes only in "
          "gmbus_wr(), behind its range guard" % (", ".join(FILES), WRITER))
    return 0


if __name__ == "__main__":
    sys.exit(main())
