#!/usr/bin/env python3
"""Fail the build if the command reference and the shell disagree.

Since 0.9.5 there is one command reference, shell/cmddb.c, read by `help`,
`man` and the desktop's Manual app. Before that there were two hand-kept
lists (help.c's table and man.c's 46 pages) and both rotted: help listed
`pong`, which was never a command, and man described a RAMFS the system
stopped using three releases earlier.

Ground truth is exec_cmd() in shell/shell.c: a command is real if the
dispatcher compares against it. Checked:

  1. Every name and alias in cmddb.c dispatches.        (nothing invented)
  2. Every dispatched name is in cmddb.c, as a name or
     an alias. Only EXEMPT is excused.                  (nothing missing)
  3. Every name in shell_cmds[] dispatches, and every
     dispatched name completes (bar `.` and `?`).       (Tab is honest)
  4. No name or alias appears twice.
  5. Every summary fits CMDDB_SUMMARY_MAX, so `help <topic>` never wraps
     on the narrowest console (64 columns).
  6. Every `see` word names an entry, every entry has a valid category
     and a non-empty synopsis, summary and description.
  7. help.c and man.c contain no command names of their own -- they read
     the table. (A string table creeping back is how the drift started.)
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
shell_c = (ROOT / "shell" / "shell.c").read_text()
db_c    = (ROOT / "shell" / "cmddb.c").read_text()
db_h    = (ROOT / "include" / "cmddb.h").read_text()

# `.` dispatches (as `source` with an argument) but it is chiefly pipe
# syntax; `source` documents both meanings.
EXEMPT = {"."}


def dispatched(src):
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


def c_strings(chunk):
    """Concatenate the adjacent C string literals in `chunk`."""
    out = []
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', chunk):
        out.append(m.group(1).encode().decode("unicode_escape"))
    return "".join(out)


def entries(src):
    """Parse cmddb[] into dicts. Each entry starts with
         { "name", "aliases"|NULL, CDB_X,
       and its seven text fields follow as runs of literals separated by
       top-level commas."""
    body = src[src.index("const cmddoc_t cmddb[] = {"):]
    body = body[:body.index("{ NULL, NULL, 0,")]
    # strip comments
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    out = []
    for m in re.finditer(r'\{\s*"([^"]+)",\s*(NULL|"[^"]*"),\s*(CDB_[A-Z]+),(.*?)\},\s*(?=\{|$)',
                         body, re.S):
        name, aliases, cat, rest = m.groups()
        # split rest on top-level commas (outside string literals)
        fields, cur, in_s, esc = [], "", False, False
        for ch in rest:
            if in_s:
                cur += ch
                if esc: esc = False
                elif ch == "\\": esc = True
                elif ch == '"': in_s = False
            elif ch == '"':
                in_s = True; cur += ch
            elif ch == ",":
                fields.append(cur); cur = ""
            else:
                cur += ch
        if cur.strip():
            fields.append(cur)
        fields = [f.strip() for f in fields]
        if len(fields) != 5:
            sys.exit("check_help: cannot parse the cmddb entry for %r (%d fields)"
                     % (name, len(fields)))
        def val(f):
            return None if f == "NULL" else c_strings(f)
        out.append(dict(name=name,
                        aliases=[] if aliases == "NULL" else aliases.strip('"').split(),
                        cat=cat,
                        synopsis=val(fields[0]), summary=val(fields[1]),
                        desc=val(fields[2]), examples=val(fields[3]), see=val(fields[4])))
    return out


def completions(src):
    m = re.search(r'static const char \*shell_cmds\[\]=\{(.*?)NULL\s*\};', src, re.S)
    if not m:
        sys.exit("check_help: cannot find shell_cmds[]")
    return set(re.findall(r'"([^"]+)"', m.group(1)))


disp = dispatched(shell_c)
db   = entries(db_c)
comp = completions(shell_c)
cats = set(re.findall(r"\b(CDB_[A-Z]+)\s*(?:=\s*0)?,", db_h)) - {"CDB_NCAT"}
smax = int(re.search(r"#define CMDDB_SUMMARY_MAX\s+(\d+)", db_h).group(1))

fail = []
names = []
for e in db:
    names.append(e["name"])
    names.extend(e["aliases"])
known = set(names)

dups = sorted({n for n in names if names.count(n) > 1})
if dups:
    fail.append("named twice in cmddb.c: " + ", ".join(dups))

ghosts = sorted(known - disp)
if ghosts:
    fail.append("cmddb.c documents names that do not dispatch: " + ", ".join(ghosts)
                + "\n    (the `pong` bug: a reference entry for something you "
                  "cannot type)")

missing = sorted(disp - known - EXEMPT)
if missing:
    fail.append("commands that work but are not in cmddb.c: " + ", ".join(missing)
                + "\n    (add an entry, or an alias to an existing one)")

ghost_comp = sorted(comp - disp)
if ghost_comp:
    fail.append("shell_cmds[] completes commands that do not dispatch: "
                + ", ".join(ghost_comp))
missing_comp = sorted(disp - comp - {".", "?"})
if missing_comp:
    fail.append("commands that work but do not tab-complete: " + ", ".join(missing_comp))

for e in db:
    n = e["name"]
    if e["cat"] not in cats:
        fail.append("%s: unknown category %s" % (n, e["cat"]))
    for f in ("synopsis", "summary", "desc"):
        if not e[f]:
            fail.append("%s: empty %s" % (n, f))
    if e["summary"] and len(e["summary"]) > smax:
        fail.append("%s: summary is %d characters, the limit is %d: %r"
                    % (n, len(e["summary"]), smax, e["summary"]))
    for w in (e["see"] or "").split():
        if w not in known:
            fail.append("%s: `see` names %r, which has no entry" % (n, w))
        if w == n:
            fail.append("%s: `see` names itself" % n)
    if e["examples"]:
        for line in e["examples"].split("\n"):
            if line and "\t" not in line:
                fail.append("%s: example %r has no tab before its comment" % (n, line))

# help.c and man.c must not grow their own lists again.
for fn in ("help.c", "man.c"):
    src = (ROOT / "shell" / fn).read_text()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    lits = set(re.findall(r'"([a-z][a-z0-9+-]*)"', src))
    own = sorted(lits & (disp - {"all", "help", "man", "guide"}))
    if own:
        fail.append("shell/%s spells out command names (%s) -- they belong in "
                    "cmddb.c" % (fn, ", ".join(own)))

# The guide's example lines ("> command\tcomment") must start with a real
# command. Capitalised words are desktop app names (page 2) and skipped.
guide = (ROOT / "shell" / "guide.c").read_text()
for line in re.findall(r'^"> ([^"\\]*?)(?:\\t|\\n)', guide, re.M):
    word = line.strip().split()[0] if line.strip() else ""
    if not word or word[0].isupper():
        continue
    if word in ("while", "endwhile", "if", "else", "endif"):     # script syntax
        continue
    if word not in disp:
        fail.append("shell/guide.c: an example starts with %r, which is not a command" % word)

if fail:
    print("check_help: the command reference and the shell disagree.\n")
    for f in fail:
        print("  - " + f)
    print()
    sys.exit(1)

print("check_help: %d commands dispatch, %d entries cover %d names, %d complete -- "
      "consistent" % (len(disp), len(db), len(known), len(comp)))
