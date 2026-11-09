#!/usr/bin/env python3
"""External judge for tests/test_hid_rd.c.

`hid_parser` is an independent Python implementation of the HID report
descriptor grammar. It is the judge because the alternative -- working
the bit offsets out by hand from the HID specification and typing them
into the test -- is the same person making the same mistake twice, and
the mistake would be invisible: a descriptor parsed wrongly still
yields plausible-looking offsets.

Input: a file of lines

    <name> <hex-of-descriptor> <field;field;...>

where each field the C parser produced is page:usage:offset:size, with
an "a" suffix on an ARRAY slot. Padding and constant items are not
fields and are not listed.

A name may be prefixed "skip:<reason>:" -- the two cases where this
judge is not the authority are listed in tests/test_hid_rd.c, and both
are recorded there rather than quietly dropped.

ARRAYS are compared by their FIRST SLOT only. hid_parser reports a
six-slot keycode array as one ArrayItem of one slot's width, and this
parser reports six fields; neither is wrong, they are different
summaries of the same bits, and the first slot's offset and width is
the part both must agree on.

Compared against hid_parser's own view of the same bytes, ignoring its
PaddingItems. Exit 0 if every descriptor agrees, 1 otherwise.
Prints the number of descriptors and fields examined, so the suite can
assert the judge was not silently idle.
"""
import sys, warnings

try:
    import hid_parser
except ImportError:
    sys.stderr.write("hid_parser is not installed\n")
    sys.exit(77)

warnings.simplefilter("ignore")


def want_fields(desc):
    r = hid_parser.ReportDescriptor(desc)
    out = []
    for rid in r.input_report_ids:
        try:
            items = r.get_input_items(rid) if rid is not None \
                    else r.get_input_items()
        except Exception:
            continue
        for it in items:
            if isinstance(it, hid_parser.PaddingItem):
                continue
            usage = getattr(it, "usage", None)
            if usage is None:
                # An ArrayItem names the whole usage RANGE it selects
                # from rather than one usage. Its first usage is the
                # range's minimum, which is what the C side records.
                us = getattr(it, "usages", None)
                if not us:
                    continue
                first = list(us)[0]
                out.append((int(first.page), int(first.usage),
                            int(it.offset), int(it.size)))
                continue
            out.append((int(usage.page), int(usage.usage),
                        int(it.offset), int(it.size)))
    return out


def main():
    ndesc = nfield = 0
    bad = []
    for line in open(sys.argv[1]):
        line = line.strip()
        if not line:
            continue
        parts = line.split(" ")
        name, hexs = parts[0], parts[1]
        if name.startswith("skip:"):
            continue
        got_s = parts[2] if len(parts) > 2 else ""
        desc = bytes.fromhex(hexs)
        got = []
        arrays = []
        for f in got_s.split(";"):
            if not f:
                continue
            isarr = f.endswith("a")
            if isarr:
                f = f[:-1]
            p, u, o, z = f.split(":")
            t = (int(p, 16), int(u, 16), int(o), int(z))
            if isarr:
                arrays.append(t)
            else:
                got.append(t)
        # One entry per array RUN: the slots of a run are contiguous
        # and the judge compares the first of each.
        arrays.sort(key=lambda t: t[2])
        prev = None
        for t in arrays:
            if prev is not None and t[2] == prev[2] + prev[3] \
               and t[0] == prev[0] and t[1] == prev[1]:
                prev = t
                continue
            got.append(t)
            prev = t
        try:
            want = want_fields(desc)
        except Exception as e:
            bad.append("%s: hid_parser could not read it: %s: %s"
                       % (name, type(e).__name__, e))
            continue
        ndesc += 1
        nfield += len(want)
        # Order is not compared -- a set, because two parsers may walk
        # report ids in a different order. WHAT each field is, and
        # WHERE it sits, is compared exactly.
        if sorted(got) != sorted(want):
            only_got = [x for x in got if x not in want]
            only_want = [x for x in want if x not in got]
            bad.append("%s:\n   ours only:  %s\n   theirs only: %s"
                       % (name, only_got[:6], only_want[:6]))
    print("%d %d" % (ndesc, nfield))
    for b in bad[:6]:
        sys.stderr.write(b + "\n")
    if bad:
        sys.stderr.write("%d descriptor(s) disagree\n" % len(bad))
        return 1
    return 0


sys.exit(main())
