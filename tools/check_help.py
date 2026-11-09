#!/usr/bin/env python3
"""Fail the build if the help screen and the shell disagree.

HavenDOS already refuses to build when a version literal is typed outside
include/version.h (make_version.py --check) and when the boot path describes
machine state in a string literal (check_boot_log.py).  This is the same idea
applied to the command reference, and it exists because the reference had
quietly rotted: it documented `pong`, which has never been a shell command,
it named the bpkg package id `h-lang` instead of the `h` command, and eleven
working commands were missing from it.

Ground truth is exec_cmd() in shell/shell.c.  A command is real if the
dispatcher compares against it.  Three things are checked:

  1. Every name in shell/help.c's tables dispatches.      (no invented commands)
  2. Every name in shell_cmds[] dispatches.               (no invented completions)
  3. Every dispatched name appears in help.c, except the
     documented exemptions below.                         (nothing undocumented)

Exemptions are listed explicitly so adding one is a visible decision rather
than a silent omission.
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
shell_c = (ROOT / "shell" / "shell.c").read_text()
help_c  = (ROOT / "shell" / "help.c").read_text()

# Names that dispatch but are deliberately not in the reference.
EXEMPT = {
    ".",         # pipe operator, documented as syntax ("cmd . cmd")
    "?",         # alias for help, documented on the help line itself
    "source",    # documented
    "copy", "type", "version", "host", "hplusplus", "resolution",
    "quit", "poweroff", "htop", "dir", "rm", "mv", "notepad", "clear",
    "dns", "curl", "tlsinfo", "boot", "time",
}
# Those aliases all appear inside a slash-separated entry
# ("cat / type <f>", "ver / version"), which the parser below splits out,
# so in practice only the first two matter.  Kept explicit anyway: an
# alias that loses its entry should show up here as a decision, not vanish.


def dispatched(src):
    """Every name exec_cmd() compares the command against."""
    lines = src.split("\n")
    starts = [i for i, l in enumerate(lines)
              if l.startswith("static void exec_cmd(") and l.rstrip().endswith("{")]
    if not starts:
        sys.exit("check_help: cannot find the exec_cmd() definition")
    st = starts[0]
    depth = 0
    for i in range(st, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if i > st and depth <= 0:
            en = i
            break
    else:
        sys.exit("check_help: exec_cmd() is not brace-balanced")
    return set(re.findall(r'strcmp\(cmd,\s*"([^"]+)"\)', "\n".join(lines[st:en + 1])))


def documented(src):
    """Command names in help.c's category tables.

    An entry is "name  description", with the name allowing slash-separated
    aliases and <arg> placeholders: "nslookup / dns / host  Resolve a name".
    """
    # "cmd" is a metavariable, not a command: the three syntax entries read
    # "cmd > file", "cmd >> file" and "cmd . cmd".  Skipping it here rather
    # than exempting it keeps EXEMPT a list of real commands.
    META = {"cmd"}
    names = set()
    for entry in re.findall(r'^\s*"([a-z0-9?][^"]*)"\s*,\s*$', src, re.M):
        head = entry.split("  ")[0]
        for part in head.split("/"):
            part = part.strip()
            if not part:
                continue
            word = part.split()[0]
            if word.startswith("<") or word.startswith("[") or word in META:
                continue
            names.add(word)
    return names


def completions(src):
    m = re.search(r'static const char \*shell_cmds\[\]=\{(.*?)NULL\s*\};', src, re.S)
    if not m:
        sys.exit("check_help: cannot find shell_cmds[]")
    return set(re.findall(r'"([^"]+)"', m.group(1)))


disp = dispatched(shell_c)
doc  = documented(help_c)
comp = completions(shell_c)

fail = []

ghosts = sorted(doc - disp)
if ghosts:
    fail.append("help.c documents commands that do not dispatch: " + ", ".join(ghosts)
                + "\n    (this is the `pong` bug: a help entry for an app that is "
                  "desktop-only)")

ghost_comp = sorted(comp - disp)
if ghost_comp:
    fail.append("shell_cmds[] completes commands that do not dispatch: "
                + ", ".join(ghost_comp))

missing = sorted(disp - doc - EXEMPT)
if missing:
    fail.append("commands that work but are undocumented: " + ", ".join(missing)
                + "\n    (document them in shell/help.c, or add them to EXEMPT "
                  "in this script with a reason)")

missing_comp = sorted(disp - comp - {".", "?"})
if missing_comp:
    fail.append("commands that work but do not tab-complete: " + ", ".join(missing_comp))

if fail:
    print("check_help: the command reference and the shell disagree.\n")
    for f in fail:
        print("  - " + f)
    print()
    sys.exit(1)

print("check_help: %d commands dispatch, %d documented, %d complete -- consistent"
      % (len(disp), len(doc), len(comp)))
