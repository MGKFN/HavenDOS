#!/usr/bin/env python3
"""tools/vendor_micropython.py -- refresh third_party/micropython from a
MicroPython checkout.

HavenDOS builds MicroPython from the files in third_party/micropython,
with no MicroPython build tools: the generated headers (the qstr table,
the module list, the compressed error strings) are made once, here, and
committed. Run this when updating MicroPython or after changing
third_party/micropython/port/mpconfigport.h (which changes the qstrs):

    git clone https://github.com/micropython/micropython ~/mpy
    python3 tools/vendor_micropython.py ~/mpy

It copies the port into the checkout as ports/havendos, builds it there
with MicroPython's own makefiles, and copies back every .c that build
compiled, the headers, and build/genhdr.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST = os.path.join(ROOT, "third_party", "micropython")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    mpy = os.path.abspath(sys.argv[1])
    port = os.path.join(mpy, "ports", "havendos")
    if os.path.exists(port):
        shutil.rmtree(port)
    shutil.copytree(os.path.join(DST, "port"), port,
                    ignore=shutil.ignore_patterns("build", "Makefile.havendos"))
    subprocess.check_call(["make", "-C", port, "-j8"])
    build = os.path.join(port, "build")
    srcs = []
    for dp, _, fs in os.walk(build):
        for f in fs:
            if f.endswith(".o"):
                rel = os.path.relpath(os.path.join(dp, f), build)[:-2] + ".c"
                srcs.append(rel)
    srcs.sort()
    for d in ("py", "shared", "lib", "genhdr", "extmod"):
        p = os.path.join(DST, d)
        if os.path.exists(p):
            shutil.rmtree(p)
    copied = []
    for rel in srcs:
        if rel in ("havendos_port.c", "lib.c"):
            continue
        os.makedirs(os.path.dirname(os.path.join(DST, rel)), exist_ok=True)
        shutil.copy(os.path.join(mpy, rel), os.path.join(DST, rel))
        copied.append(rel)
    # every header the sources might include
    for sub in ("py", "shared/readline", "shared/runtime", "lib/libm", "extmod"):
        sd = os.path.join(mpy, sub)
        for f in os.listdir(sd):
            if f.endswith(".h"):
                os.makedirs(os.path.join(DST, sub), exist_ok=True)
                shutil.copy(os.path.join(sd, f), os.path.join(DST, sub, f))
    os.makedirs(os.path.join(DST, "genhdr"), exist_ok=True)
    for f in ("qstrdefs.generated.h", "moduledefs.h", "mpversion.h",
              "root_pointers.h", "compressed.data.h"):
        shutil.copy(os.path.join(build, "genhdr", f), os.path.join(DST, "genhdr", f))
    # the banner says the release, not a bare commit hash
    import re
    cfg = open(os.path.join(mpy, "py", "mpconfig.h")).read()
    ver = ".".join(re.search(r"MICROPY_VERSION_%s (\d+)" % k, cfg).group(1) for k in ("MAJOR", "MINOR", "MICRO"))
    pre = re.search(r"MICROPY_VERSION_PRERELEASE (\d+)", cfg).group(1) != "0"
    vh = os.path.join(DST, "genhdr", "mpversion.h")
    txt = re.sub(r'MICROPY_GIT_TAG "[^"]*"', 'MICROPY_GIT_TAG "v%s%s"' % (ver, "-preview" if pre else ""), open(vh).read())
    open(vh, "w").write(txt)
    shutil.copy(os.path.join(mpy, "LICENSE"), os.path.join(DST, "LICENSE"))
    with open(os.path.join(DST, "SOURCES"), "w") as fh:
        fh.write("\n".join(copied) + "\n")
    rev = subprocess.check_output(["git", "-C", mpy, "log", "-1", "--format=%H %cd"]).decode().strip()
    with open(os.path.join(DST, "VERSION"), "w") as fh:
        fh.write(rev + "\n")
    shutil.rmtree(port)
    print("vendored %d sources from %s" % (len(copied), rev))


if __name__ == "__main__":
    main()
