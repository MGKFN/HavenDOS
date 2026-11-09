#!/usr/bin/env python3
"""make_version.py — propagate include/version.h into the ISO tree.

    python3 tools/make_version.py include/version.h isodir

Rewrites isodir/version.txt and the menu-entry titles in
isodir/boot/grub/grub.cfg so the boot menu, the version file the updater
reads off the ISO, and the running kernel can never disagree again.

Before v0.7.18 all three were typed by hand and had drifted: a v0.7.17 build
shipped a GRUB menu reading v0.7.11 and a version.txt reading v0.7.11, while
fs/update.c believed it was running 0.5.9.14.
"""
import re
import sys
import os


def read_version(header):
    src = open(header, encoding="utf-8").read()
    m = re.search(r'#define\s+HAVENDOS_VER_NUM\s+"([^"]+)"', src)
    if not m:
        sys.exit("make_version.py: HAVENDOS_VER_NUM not found in %s" % header)
    return m.group(1)



# ──────────────────────────────────────────────────────────────────────
#  --check : fail the build if an OS version literal is typed anywhere
#            outside this file's single source of truth.
#
#  v0.7.18 claimed to do this.  It did not — it only GENERATED the ISO
#  files, and four stale literals survived into the shipped v0.7.18 zip:
#  $VER at the shell prompt read 0.7.11, havendos_install_version_str()
#  returned 0.7.4, the mode-13 info panel drew v0.3.0 beside live RAM and
#  uptime, and version.h's own comment carried "0.7.18" which the Makefile
#  banner's unanchored sed then printed alongside the real version.
#
#  A line that legitimately contains something version-shaped (a package
#  version, an IP address) opts out with the marker  version-literal-ok
# ──────────────────────────────────────────────────────────────────────

SCAN_DIRS = ("kernel", "drivers", "fs", "net", "shell", "include", "tests")
SOURCE_OF_TRUTH = os.path.join("include", "version.h")
OPT_OUT = "version-literal-ok"
# A whole file opts out with this marker. Exactly one thing needs it: the test
# whose SUBJECT is version strings, which necessarily contains dozens of them.
# Per-line markers there would bury the test in noise and make it harder to
# read than the bug it guards.
OPT_OUT_FILE = "version-literal-file-ok"

# A quoted literal that is ENTIRELY an OS-shaped version: 0.N.N or 0.N.N.N,
# optionally v-prefixed.  Anchored inside the quotes so "192.168.0.1" and
# free prose do not match.
LIT = re.compile(r'"(v?0\.\d+\.\d+(?:\.\d+)?)"')


def check_drift(root="."):
    bad = []
    for d in SCAN_DIRS:
        dpath = os.path.join(root, d)
        if not os.path.isdir(dpath):
            continue
        for dirpath, _dirnames, filenames in os.walk(dpath):
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                fp = os.path.join(dirpath, fn)
                rel = os.path.relpath(fp, root)
                if rel.replace("\\", "/") == SOURCE_OF_TRUTH.replace("\\", "/"):
                    continue
                try:
                    text = open(fp, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                if OPT_OUT_FILE in text:
                    continue
                lines = text.split("\n")
                for i, line in enumerate(lines, 1):
                    if OPT_OUT in line:
                        continue
                    for m in LIT.finditer(line):
                        bad.append((rel, i, m.group(1), line.strip()))
    if bad:
        sys.stderr.write(
            "\nmake_version.py --check: hardcoded OS version literal(s) found.\n"
            "Use HAVENDOS_VER / HAVENDOS_VER_NUM from include/version.h, or add\n"
            "the marker '%s' to the line if it is not an OS version,\n"
            "or '%s' anywhere in a file whose subject is version strings.\n\n"
            % (OPT_OUT, OPT_OUT_FILE))
        for rel, ln, lit, text in bad:
            sys.stderr.write('  %s:%d  "%s"\n      %s\n' % (rel, ln, lit, text[:96]))
        sys.stderr.write("\n%d literal(s).\n" % len(bad))
        return 1
    print("make_version: --check clean, no hardcoded version literals")
    return 0


LIT_RE = re.compile(r'"(?:[^"\\]|\\.)*"')


def strip_comments(text):
    """Blank out comments, keeping line structure and string literals intact.

    Needed because a comment may legitimately quote a word with an accent in
    it -- drivers/vga.c explains the font work using the word "cafe" spelled
    properly -- and a comment is never drawn on screen.
    """
    out = []
    i, n = 0, len(text)
    in_line = in_block = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            else:
                out.append(" ")
            i += 1
        elif in_block:
            if c == "*" and nxt == "/":
                in_block = False
                out.append("  ")
                i += 2
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
        elif c == "/" and nxt == "/":
            in_line = True
            out.append("  ")
            i += 2
        elif c == "/" and nxt == "*":
            in_block = True
            out.append("  ")
            i += 2
        elif c in '"\'':
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def check_ascii(root="."):
    """Refuse non-ASCII inside a user-visible string literal.

    VGA text mode is one byte per cell over code page 437. A UTF-8 em dash is
    three bytes, so it is drawn as three wrong glyphs: the update manager
    printed "havendos.bak GCo first install path". 23 literals across the tree
    had this. Comments are fine -- they are never drawn.
    """
    bad = []
    for d in SCAN_DIRS:
        dpath = os.path.join(root, d)
        if not os.path.isdir(dpath):
            continue
        for dirpath, _dirnames, filenames in os.walk(dpath):
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                fp = os.path.join(dirpath, fn)
                rel = os.path.relpath(fp, root)
                try:
                    text = open(fp, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                code = strip_comments(text)
                for i, line in enumerate(code.split("\n"), 1):
                    if OPT_OUT in line:
                        continue
                    for s in LIT_RE.findall(line):
                        nb = [c for c in s if ord(c) > 127]
                        if nb:
                            bad.append((rel, i, "".join(sorted(set(nb))),
                                        line.strip()))
    if bad:
        sys.stderr.write(
            "\nmake_version.py --check: non-ASCII in a string literal.\n"
            "VGA text mode is CP437, one byte per cell -- a UTF-8 character is\n"
            "drawn as several wrong glyphs. Use plain ASCII, or add '%s'\n"
            "to the line if the string never reaches the screen.\n\n" % OPT_OUT)
        for rel, ln, chars, text in bad:
            sys.stderr.write("  %s:%d  contains %s\n      %s\n"
                             % (rel, ln, chars, text[:92]))
        sys.stderr.write("\n%d literal(s).\n" % len(bad))
        return 1
    print("make_version: --check clean, string literals are ASCII")
    return 0


def write_theme(isodir, ver):
    """Rewrite the version label in the GRUB theme.

    This caption used to be painted into background.png, so the version on
    the first screen anyone sees could not be regenerated and had been stale
    since v0.5.9.9 -- and no literal scan could ever have found it, because
    it was pixels.  As a label it is text, and it is rewritten here."""
    path = os.path.join(isodir, "boot", "grub", "themes", "havendos", "theme.txt")
    if not os.path.exists(path):
        return
    src = open(path, encoding="utf-8").read()
    new = re.sub(r'(text\s*=\s*)"HavenDOS v[0-9][^"]*"',
                 r'\1"HavenDOS v%s"' % ver, src)
    if new != src:
        open(path, "w", encoding="utf-8").write(new)
        print("make_version: theme.txt -> v%s" % ver)


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--check":
        sys.exit(check_drift(".") or check_ascii("."))
    if len(sys.argv) != 3:
        sys.exit("usage: make_version.py <include/version.h> <isodir>\n"
                 "       make_version.py --check")
    header, isodir = sys.argv[1], sys.argv[2]
    num = read_version(header)
    disp = "v" + num

    vt = os.path.join(isodir, "version.txt")
    with open(vt, "w", encoding="utf-8") as f:
        f.write(disp + "\n")
    print("make_version: %s -> %s" % (vt, disp))

    cfg = os.path.join(isodir, "boot", "grub", "grub.cfg")
    if os.path.exists(cfg):
        src = open(cfg, encoding="utf-8").read()
        # Only rewrite the version token inside menuentry titles.
        new, n = re.subn(r'v0\.[0-9]+\.[0-9]+(?:\.[0-9]+)*', disp, src)
        if new != src:
            open(cfg, "w", encoding="utf-8").write(new)
        print("make_version: %s -> %s (%d titles)" % (cfg, disp, n))

    write_theme(isodir, num)


if __name__ == "__main__":
    main()
