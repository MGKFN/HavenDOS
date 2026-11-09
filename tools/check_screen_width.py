#!/usr/bin/env python3
"""check_screen_width.py -- text written off the edge of the screen.

vga_puts_at(s, col, row, fg, bg) writes s starting at col. The console is
80 columns wide today and drivers/vga.c clamps a write at the edge, so a
string that starts at column 42 and is 72 characters long is not an error
and does not look like one -- it looks like a sentence that stops in the
middle of a word, on the screen that tells somebody which sectors of their
disk are about to be written.

That is exactly what the drive picker did in the first version of the
additive installer: the Notes column begins at 42 and
havendos_install_add_strerror() returns strings up to 72 characters, so
"no unallocated space on this disk -- shrink a partition in Windows first"
would have appeared as "no unallocated space on this d".

This checks the cases that can be checked without running anything: a
string literal with a literal column. Computed columns and runtime strings
are not checked -- they are reported as a count, so that the number of
unexamined sites is at least visible.

Run:  python3 tools/check_screen_width.py [cols]
"""
import os
import re
import sys

# vga_puts_at("...", COL, ...) and vga_puts_at("..." "...", COL, ...)
LIT = re.compile(
    r'vga_puts_at\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\s*,\s*(\d+)\s*,',
    re.S)
NONLIT = re.compile(r'vga_puts_at\s*\(\s*(?!\s*")', re.S)

ESCAPES = {'n': 1, 't': 1, 'r': 1, '0': 1, '\\': 1, '"': 1, "'": 1}


def literal_len(chunk):
    """Length of a C string literal, concatenated pieces included."""
    total = 0
    for piece in re.findall(r'"((?:[^"\\]|\\.)*)"', chunk):
        i = 0
        while i < len(piece):
            if piece[i] == '\\' and i + 1 < len(piece):
                i += 2
            else:
                i += 1
            total += 1
    return total


def main():
    cols = int(sys.argv[1]) if len(sys.argv) > 1 else 80
    roots = ["shell", "kernel", "drivers", "fs", "net"]
    over, checked, skipped = [], 0, 0

    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, _, names in os.walk(root):
            for name in sorted(names):
                if not name.endswith(".c"):
                    continue
                path = os.path.join(dirpath, name)
                src = open(path, encoding="utf-8", errors="replace").read()
                for m in LIT.finditer(src):
                    checked += 1
                    n = literal_len(m.group(1))
                    col = int(m.group(2))
                    if col + n > cols:
                        line = src.count("\n", 0, m.start()) + 1
                        text = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
                        over.append((path, line, col, n, col + n,
                                     "".join(text)[:46]))
                skipped += len(NONLIT.findall(src))

    if over:
        sys.stderr.write(
            "check_screen_width: %d literal(s) run past column %d:\n"
            % (len(over), cols))
        for path, line, col, n, end, text in over:
            sys.stderr.write("  %s:%d  col %d + %d = %d  \"%s\"\n"
                             % (path, line, col, n, end, text))
        sys.stderr.write(
            "  A clamped string looks like a sentence that stops mid-word,\n"
            "  not like a bug. Shorten it, start it further left, or split\n"
            "  it across two rows.\n")
        return 1

    print("check_screen_width: %d literal placements fit %d columns "
          "(%d computed sites not checked)" % (checked, cols, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
