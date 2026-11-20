#!/usr/bin/env python3
"""release_names.py -- what a release is called, from include/version.h.

v0.9.7, THS-RM-1004 sections 9-11. One place decides, for the Release
workflow, the bundle and the manifest:

    the 0.9.x line (PRODUCT 0)        HavenDOS 1 on (PRODUCT >= 1)
    tag    v0.9.7                     27100.1000
    iso    HavenDOS-0.9.7.iso         HavenDOS-1-27H1.iso
    title  HavenDOS 0.9.7             HavenDOS 1, version 27H1 (OS Build 27100.1000)
    pre    yes (CH20 Beta)            yes below CH50 Stable; no on Stable

From 0.9.7 every release below Stable is a GitHub PRE-release, so
"latest" on GitHub is only ever a Stable release (RM-1004 s.10).

    python3 tools/release_names.py [include/version.h]       # shell assignments
    python3 tools/release_names.py --json [include/version.h]
    python3 tools/release_names.py --selftest
"""
import json
import re
import sys

CHANNELS = {0: "Experimental", 10: "Development", 20: "Beta",
            30: "Public Beta", 40: "Release Preview", 50: "Stable"}


def fields(src):
    def num(name, default=None):
        m = re.search(r'^#define\s+%s\s+(\d+)' % name, src, re.M)
        if not m:
            if default is None:
                sys.exit("release_names: no %s in version.h" % name)
            return default
        return int(m.group(1))

    def text(name, default=None):
        m = re.search(r'^#define\s+%s\s+"([^"]*)"' % name, src, re.M)
        if not m:
            if default is None:
                sys.exit("release_names: no %s in version.h" % name)
            return default
        return m.group(1)

    return dict(ver=text("HAVENDOS_VER_NUM"), product=num("HAVENDOS_PRODUCT", 0),
                release=text("HAVENDOS_RELEASE", ""), build=num("HAVENDOS_BUILD", 0),
                revision=num("HAVENDOS_REVISION", 0), channel=num("HAVENDOS_CHANNEL_ID", 20),
                level=num("HAVENDOS_LEVEL", 1), codename=text("HAVENDOS_CODENAME", ""))


def names(f):
    ch = f["channel"]
    if ch not in CHANNELS:
        sys.exit("release_names: HAVENDOS_CHANNEL_ID %d is not a channel" % ch)
    if f["product"] < 1:
        return dict(ver=f["ver"], tag="v" + f["ver"], iso="HavenDOS-%s.iso" % f["ver"],
                    title="HavenDOS %s" % f["ver"], prerelease=ch < 50,
                    channel=ch, channel_name=CHANNELS[ch])
    bn = "%d.%04d" % (f["build"], f["revision"])
    if f["ver"] != bn:
        sys.exit("release_names: HAVENDOS_VER_NUM %s is not BUILD.REVISION %s -- "
                 "from HavenDOS 1 they are one number" % (f["ver"], bn))
    if not re.match(r"^\d\dH[12]$", f["release"]):
        sys.exit("release_names: RELEASE %r is not YYH1 or YYH2" % f["release"])
    title = "HavenDOS %d, version %s (OS Build %s%s)" % (
        f["product"], f["release"], bn, "" if ch == 50 else ", " + CHANNELS[ch])
    return dict(ver=f["ver"], tag=bn, iso="HavenDOS-%d-%s.iso" % (f["product"], f["release"]),
                title=title, prerelease=ch < 50, channel=ch, channel_name=CHANNELS[ch])


def selftest():
    ok = True

    def eq(what, got, want):
        nonlocal ok
        good = got == want
        ok &= good
        print("  %s  %s%s" % ("ok  " if good else "FAIL", what, "" if good else "  (%r)" % (got,)))

    v097 = '#define HAVENDOS_VER_NUM    "0.9.7"\n#define HAVENDOS_PRODUCT 0\n#define HAVENDOS_CHANNEL_ID 20\n'
    n = names(fields(v097))
    eq("0.9.7: tag v0.9.7", n["tag"], "v0.9.7")
    eq("0.9.7: ISO HavenDOS-0.9.7.iso", n["iso"], "HavenDOS-0.9.7.iso")
    eq("0.9.7: a pre-release (Beta)", n["prerelease"], True)
    hd1 = ('#define HAVENDOS_VER_NUM "27100.1000"\n#define HAVENDOS_PRODUCT 1\n'
           '#define HAVENDOS_RELEASE "27H1"\n#define HAVENDOS_BUILD 27100\n'
           '#define HAVENDOS_REVISION 1000\n#define HAVENDOS_CHANNEL_ID %d\n')
    n = names(fields(hd1 % 50))
    eq("27100.1000 Stable: tag 27100.1000", n["tag"], "27100.1000")
    eq("  ... ISO HavenDOS-1-27H1.iso", n["iso"], "HavenDOS-1-27H1.iso")
    eq("  ... not a pre-release: 'latest' on GitHub", n["prerelease"], False)
    eq("  ... titled as THS-RM-1004 says", n["title"], "HavenDOS 1, version 27H1 (OS Build 27100.1000)")
    n = names(fields(hd1 % 40))
    eq("27100.1000 Release Preview: a pre-release", n["prerelease"], True)
    eq("  ... with its channel in the title", n["title"],
       "HavenDOS 1, version 27H1 (OS Build 27100.1000, Release Preview)")
    try:
        names(fields(hd1.replace('"27100.1000"', '"27100.1001"') % 50))
        eq("a VER_NUM that is not BUILD.REVISION is refused", False, True)
    except SystemExit:
        eq("a VER_NUM that is not BUILD.REVISION is refused", True, True)
    return 0 if ok else 1


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    header = args[0] if args else "include/version.h"
    n = names(fields(open(header, encoding="utf-8").read()))
    if "--json" in sys.argv:
        print(json.dumps(n))
        return
    for k, v in n.items():
        if isinstance(v, bool):
            v = "true" if v else "false"
        print("%s=%s" % (k.upper(), "'" + str(v).replace("'", "") + "'"))


if __name__ == "__main__":
    main()
