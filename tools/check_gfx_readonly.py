#!/usr/bin/env python3
"""check_gfx_readonly.py -- the display code writes only where it says.

── 0.9.2: A SECOND WRITER, AND THE ONE THAT LIGHTS A SCREEN ──────────

drivers/xemode.c programs pipe B, transcoder B, DDI B, combo PHY B and a
free DPLL to light the HDMI monitor. It gets the same treatment as
xegmbus.c, with a different guard:

  - GFX_WR is used ONCE outside its own #define, inside xm_wr();
  - xm_wr() calls xm_allowed() BEFORE its GFX_WR -- the allow-list is
    asked first, not consulted afterwards;
  - xm_allowed() names no register of pipe A's path: no offset in
    transcoder A (0x60000-0x60FFF), pipe A (0x70000-0x70FFF), DDI A's
    buffer (0x64000) or combo PHY A (0x162000-0x162FFF). A list that
    cannot name them cannot allow them;
  - nothing else in the file writes.

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
# 0.9.2: (file, the one function that writes, a check on its body)
WRITERS = (
    ("drivers/xegmbus.c", "gmbus_wr"),
    ("drivers/xemode.c", "xm_wr"),
)
PIPE_A = ((0x60000, 0x61000, "transcoder A"), (0x70000, 0x71000, "pipe A"),
          (0x64000, 0x64004, "DDI A's buffer"), (0x162000, 0x163000, "combo PHY A"))

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


def func_span(code, name, need_static=True):
    """(first, last) line index of a function's definition, or None."""
    start = next((i for i, l in enumerate(code)
                  if re.match(r"\s*(static\s+)?\w[\w\s\*]*\b%s\s*\(" % name, l)
                  and (not need_static or "static" in l)
                  and not l.rstrip().endswith(";")), None)
    if start is None:
        return None
    end = next((i for i in range(start, len(code)) if code[i] == "}"), None)
    if end is None:
        return None
    return start, end


def guard_problems(path, fn, code, lines, start, end):
    """What the writing function must do before it writes."""
    body = "\n".join(code[start:end + 1])
    bad = []
    if fn == "gmbus_wr":
        if "GMBUS_REG_FIRST" not in body or "GMBUS_REG_LAST" not in body:
            bad.append((start + 1, "gmbus_wr() lost its range guard", lines[start].strip()))
    elif fn == "xm_wr":
        ask = body.find("xm_allowed(")
        wrt = body.find("GFX_WR")
        if ask < 0:
            bad.append((start + 1, "xm_wr() no longer asks xm_allowed()", lines[start].strip()))
        elif wrt >= 0 and wrt < ask:
            bad.append((start + 1, "xm_wr() writes before it asks xm_allowed()",
                        lines[start].strip()))
        span = func_span(code, "xm_allowed", need_static=False)
        if span is None:
            bad.append((0, "xm_allowed() is missing -- the allow-list", ""))
        else:
            for i in range(span[0], span[1] + 1):
                for m in re.finditer(r"(?<![\w])0[xX]([0-9A-Fa-f]+)[uUlL]*", code[i]):
                    v = int(m.group(1), 16)
                    for lo, hi, what in PIPE_A:
                        if lo <= v < hi:
                            bad.append((i + 1, "xm_allowed() names %s (0x%X)" % (what, v),
                                        lines[i].strip()))
    return bad


def scan_writer(path, fn="gmbus_wr"):
    """A writer: GFX_WR once, inside `fn`, behind its guard."""
    text = open(path, encoding="utf-8", errors="replace").read()
    lines = text.splitlines()
    code = csource.code_lines(text)
    bad = []
    span = func_span(code, fn)
    if span is None:
        return [(0, "%s() is missing -- the only place a write may be" % fn, "")]
    start, end = span
    bad += guard_problems(path, fn, code, lines, start, end)
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
                bad.append((i + 1, "GFX_WR outside %s()" % fn, lines[i].strip()))
        for rx, what in RULES:
            if what.startswith("a register write seam"):
                continue                       # GFX_WR handled above
            if what.startswith("a store through") and is_def:
                continue                       # the macro's own body
            if rx.search(probe):
                bad.append((i + 1, what, lines[i].strip()))
    if uses != 1:
        bad.append((start + 1, "%s() uses GFX_WR %d times, not once" % (fn, uses),
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
    for path, fn in WRITERS:
        if not os.path.exists(path):
            sys.stderr.write("check_gfx_readonly: %s is missing\n" % path)
            return 2
        for num, what, src in scan_writer(path, fn):
            found.append((path, num, what, src))
    if found:
        print("check_gfx_readonly: the display code writes where it "
              "must not\n")
        for f, num, what, src in found:
            print("  %s:%d  %s\n      %s" % (f, num, what, src))
        print("\n  The panel is being scanned out by this engine while the")
        print("  command runs. Writes belong in xm_wr() (behind xm_allowed())")
        print("  or gmbus_wr() -- not in an inspector, and not anywhere else.")
        return 1
    print("check_gfx_readonly: ok, %s never write; %s; xm_allowed() "
          "names nothing of pipe A's" % (", ".join(FILES),
          ", ".join("%s writes only in %s()" % (p, f) for p, f in WRITERS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
