#!/usr/bin/env python3
"""make_retropc_dist.py -- the two Retro PC downloads.  v0.9.7.5.

From the programs `make retropc` built in dist/retropc:

  dist/RetroPC-<ver>-windows.zip          RetroPC/RetroPC.exe
                                          RetroPC/README.txt  (CRLF)
                                          RetroPC/RETRO/      (empty, ready)
  dist/RetroPC-<ver>-linux-x86_64.tar.gz  RetroPC/retropc, retropc-headless,
                                          README.txt, RETRO/

Refuses (exit 1) if a program is missing, or if it was built from a
different version.h than the one in the tree -- a download named
0.9.7.5 holding a 0.9.7.4 program is the v0.7.20 bug in a new place.
"""
import io, os, re, subprocess, sys, tarfile, time, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "dist", "retropc")


def die(m):
    sys.exit("make_retropc_dist: " + m)


def main():
    h = open(os.path.join(ROOT, "include", "version.h")).read()
    ver = re.search(r'#define\s+HAVENDOS_VER_NUM\s+"([^"]+)"', h).group(1)
    exe = os.path.join(SRC, "RetroPC.exe")
    lin = os.path.join(SRC, "retropc")
    hl = os.path.join(SRC, "retropc-headless")
    for p in (exe, lin, hl):
        if not os.path.exists(p):
            die(f"{os.path.relpath(p, ROOT)} is missing -- run `make retropc`")
    for p in (exe, lin, hl):
        if ver.encode() not in open(p, "rb").read():
            die(f"{os.path.relpath(p, ROOT)} does not carry version {ver}: it is stale")
    got = subprocess.run([hl, "--version"], capture_output=True, text=True).stdout.strip()
    if got != f"Retro PC, HavenDOS {ver}":
        die(f"retropc-headless --version says {got!r}, expected {ver}")

    readme = open(os.path.join(ROOT, "retropc", "README.txt")).read().replace("@VER@", ver)
    now = time.localtime()[:6]

    zp = os.path.join(ROOT, "dist", f"RetroPC-{ver}-windows.zip")
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
        def add(name, data, mode=0o644):
            zi = zipfile.ZipInfo(name, now)
            zi.external_attr = mode << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, data)
        add("RetroPC/RetroPC.exe", open(exe, "rb").read(), 0o755)
        add("RetroPC/README.txt", readme.replace("\n", "\r\n").replace("./retropc", "retropc"))
        add("RetroPC/RETRO/", b"", 0o755 | 0o040000)
        add("RetroPC/RETRO/PUT-IMAGES-HERE.txt",
            "Floppy images (.IMG) and CD images (.ISO) in this folder are listed in Retro PC.\r\n"
            "The old PCs' hard disks are kept here too, as .HDD files.\r\n")

    tp = os.path.join(ROOT, "dist", f"RetroPC-{ver}-linux-x86_64.tar.gz")
    with tarfile.open(tp, "w:gz") as t:
        def add(name, data=None, mode=0o644, path=None):
            ti = tarfile.TarInfo(name)
            ti.mtime = time.time()
            if data is None and path is None:
                ti.type = tarfile.DIRTYPE; ti.mode = 0o755
                t.addfile(ti); return
            if path:
                data = open(path, "rb").read()
            ti.size = len(data); ti.mode = mode
            t.addfile(ti, io.BytesIO(data))
        add("RetroPC")
        add("RetroPC/retropc", path=lin, mode=0o755)
        add("RetroPC/retropc-headless", path=hl, mode=0o755)
        add("RetroPC/README.txt", readme.encode())
        add("RetroPC/RETRO")
        add("RetroPC/RETRO/PUT-IMAGES-HERE.txt",
            b"Floppy images (.IMG) and CD images (.ISO) in this folder are listed in Retro PC.\n"
            b"The old PCs' hard disks are kept here too, as .HDD files.\n")

    for p in (zp, tp):
        print(f"make_retropc_dist: {os.path.relpath(p, ROOT)}  {os.path.getsize(p)} bytes")


main()
