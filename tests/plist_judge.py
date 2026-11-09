#!/usr/bin/env python3
"""The external judge for tests/test_plist.c.

Python's plistlib is a real, independent implementation of the XML
property list format -- it is what `plutil` is to macOS and what
nothing in this tree is. It is the judge for two reasons:

  1. There IS no plist reader in HavenDOS, so our writer cannot be
     checked against our reader even in principle.
  2. If there were one, a writer and reader built from the same
     misunderstanding would agree with each other perfectly and be
     rejected by the phone, which is the exact failure this suite
     exists to catch.

Usage: plist_judge.py <plist-file> <expression-file>

The expression arrives in a FILE rather than on the command line, and
that is not fussiness. The suite shells out to run this, so an
expression passed as an argument has to survive shell quoting -- and
the very first value worth testing is a string containing both kinds
of quote, which is exactly what shell quoting cannot carry. One check
("quotes are left alone") failed for that reason and for no other:
the plist was perfect and the TEST was unrunnable. A file has no
quoting rules at all.
  exit 0  the expression is true
  exit 1  the expression is false
  exit 2  the file does not parse as a property list at all
  exit 3  the expression itself blew up (a bad test, not a bad plist)

Exit 2 is distinct on purpose: "lockdownd would reject this" and
"the value came back wrong" are different findings and the suite
prints which one it got.
"""
import plistlib, sys

def main():
    try:
        with open(sys.argv[1], "rb") as f:
            p = plistlib.load(f)
    except Exception as e:
        sys.stderr.write("UNPARSEABLE: %s: %s\n" % (type(e).__name__, e))
        return 2
    # p goes in GLOBALS, not locals. A generator expression inside
    # eval() gets its own scope, and that scope can see the globals
    # mapping but NOT the locals mapping -- so `all(p[k] == ... for k
    # in ...)` raises NameError on p when p is passed as a local.
    # Found by this suite failing with exit 3 (bad expression) on
    # exactly the three checks that used a comprehension.
    try:
        expr = open(sys.argv[2], "r").read()
        v = eval(expr, {"p": p, "__builtins__": {
            "len": len, "all": all, "any": any, "list": list,
            "dict": dict, "bytes": bytes, "range": range,
            "type": type, "int": int, "str": str, "sorted": sorted,
            "isinstance": isinstance, "bool": bool, "True": True,
            "False": False, "set": set, "chr": chr, "ord": ord,
        }})
    except Exception as e:
        sys.stderr.write("BAD EXPRESSION: %s: %s\n  %s\n"
                         % (type(e).__name__, e, sys.argv[2]))
        return 3
    if not v:
        sys.stderr.write("FALSE: %s\nparsed: %r\n" % (expr, p))
        return 1
    return 0

sys.exit(main())
