#!/usr/bin/env python3
"""Boot an image under real UEFI firmware (OVMF) and screenshot it.

WHY THIS EXISTS. Until now the UEFI side of the ISO was argued about
rather than tested: "the loader is a PE32+ with machine 0x8664" is a
property of a file, not evidence that firmware will boot it. OVMF is
firmware. If it does not find a loader, this harness shows the same
boot-manager screen a real machine would show, and that is a failure
you can see rather than one you reason about.

  uefi_boot.py <image> <out-prefix> [--hdd] [--secs=N] [--keys=a,b@12,c]

A key may carry its own wait: "ret@14" presses Enter and waits 14s
before the screenshot, which is what booting a kernel needs and what
a fixed 2.5s pause was quietly too short for.
"""
import os, socket, subprocess, sys, time

CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"

img  = sys.argv[1]
out  = sys.argv[2]
args = sys.argv[3:]
hdd  = "--hdd" in args
secs = 22
keys = []
for a in args:
    if a.startswith("--secs="): secs = int(a.split("=",1)[1])
    if a.startswith("--keys="): keys = [k for k in a.split("=",1)[1].split(",") if k]

vars_copy = out + "-vars.fd"
subprocess.run(["cp", VARS, vars_copy], check=True)

sock = out + "-mon.sock"
if os.path.exists(sock): os.unlink(sock)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(sock); srv.listen(1)

media = ["-drive", f"file={img},format=raw,if=ide,media=disk"] if hdd \
        else ["-cdrom", img]

q = subprocess.Popen(
    ["qemu-system-x86_64",
     "-machine", "q35",
     "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={CODE}",
     "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}",
     *media, "-m", "1024", "-display", "none",
     "-monitor", f"unix:{sock}"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

conn, _ = srv.accept()
conn.settimeout(0.4)          # short: the monitor often sends nothing back

def mon(cmd):
    conn.sendall((cmd + "\n").encode())
    time.sleep(0.10)
    try:
        while conn.recv(65536): pass
    except socket.timeout:
        pass

def shot(tag):
    p = f"{out}-{tag}.ppm"
    mon("screendump " + p)
    time.sleep(0.8)
    print(f"  {tag:12s} {os.path.getsize(p) if os.path.exists(p) else 'MISSING'}")

time.sleep(secs)
shot("boot")
for i, k in enumerate(keys):
    wait = 2.5
    if "@" in k:
        k, w = k.split("@", 1)
        wait = float(w)
    mon("sendkey " + k)
    time.sleep(wait)
    shot(f"k{i}_{k}")

mon("quit"); time.sleep(0.4)
q.terminate()
try: q.wait(timeout=5)
except Exception: q.kill()
print("done")
