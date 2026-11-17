#!/usr/bin/env python3
"""tools/check_emu86_sst.py -- emu/cpu86.c against a real 8088.  v0.9.7.

Runs the SingleStepTests 8088 suite (github.com/SingleStepTests/8088,
recorded from a physical CPU) through Retro PC's interpreter. See
tests/emu86_sst.c for why: a CPU checked only by its author's own tests
agrees with its author.

  python3 tools/check_emu86_sst.py [--cache DIR] [--limit N] [OPCODE...]

Downloads into the cache once (a few hundred MB), then converts each
opcode file to tests/emu86_sst.c's record format and runs it.

WHAT IS COMPARED, AND WHAT IS NOT. The interpreter is an 80186 with
the 386's 16-bit additions; the suite is an 8088. Where the two
processors genuinely differ, the 8088's answer is not this CPU's
answer, and those tests are skipped here rather than counted as
passes -- the list below says which and why. Undefined flags are
masked out with the suite's own metadata.json.

A deliberate act, like the injection harnesses: it needs the network
and minutes. Not part of `make test`.
"""
import argparse, gzip, json, os, struct, subprocess, sys, urllib.error, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = "https://raw.githubusercontent.com/SingleStepTests/8088/main/v2/"
REGS = ["ax","bx","cx","dx","cs","ss","ds","es","sp","bp","si","di","ip","flags"]

# Opcodes where an 80186 is not an 8088. Skipped, with the reason.
DIFFERS = {
    "0F": "POP CS on an 8088; the two-byte escape here",
    **{"%02X" % o: "a Jcc alias on an 8088; 80186/386 instructions here" for o in range(0x60, 0x70)},
    "C0": "RET imm alias on an 8088; shift by immediate on an 80186",
    "C1": "RET alias on an 8088; shift by immediate on an 80186",
    "C8": "RETF imm alias on an 8088; ENTER on an 80186",
    "C9": "RETF alias on an 8088; LEAVE on an 80186",
}
# Group forms that differ, by opcode.reg.
DIFFERS_REG = {nm: "SETMO/SETMOC on an 8088 (undocumented); SHL on an 80186 and after"
               for nm in ("D0.6", "D1.6", "D2.6", "D3.6")}
# Within an opcode: tests whose 8088 behaviour is not an 80186's.
def skip_test(op, t):
    b = t["bytes"]
    i = 0
    while b[i] in (0x26, 0x2E, 0x36, 0x3E, 0xF0, 0xF2, 0xF3): i += 1
    pre = b[:i]
    base = op.split(".")[0]
    if base in ("D2", "D3"):
        # the 8088 shifts by all of CL; the 80186 masks it to five bits
        cl = t["initial"]["regs"]["cx"] & 0xFF
        if cl & 0xE0: return True
    if base in ("8C", "8E") and ((b[i + 1] >> 3) & 7) in (4, 5):
        # ES/CS again on an 8088; the 386's FS and GS here
        return True
    if base in ("F6", "F7") and (0xF2 in pre or 0xF3 in pre):
        # an 8088 quirk: REP before MUL/IMUL/DIV/IDIV negates the result
        return True
    if op in ("F6.6", "F6.7", "F7.6", "F7.7"):
        # a divide error: the flags the 8088 pushes are whatever its
        # microcode left, and the 8086 also refuses a quotient of exactly
        # -128 / -32768, which the 80186 allows
        # a DIV that traps pushes three words; one that does not never
        # touches SP -- a surer sign than where IP ended up
        if "sp" in t["final"]["regs"]: return True
    return False

def fetch(cache, name):
    """The suite names a group's file "80.3"; three single-form groups
    (8F, C6, C7) are named by the opcode alone. None if not there."""
    for nm in ([name, name.split(".")[0]] if "." in name else [name]):
        p = os.path.join(cache, nm + ".json.gz")
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            try:
                with urllib.request.urlopen(BASE + nm + ".json.gz", timeout=120) as r:
                    data = r.read()
            except urllib.error.HTTPError:
                continue
            with open(p + ".part", "wb") as f: f.write(data)
            os.replace(p + ".part", p)
        with open(p, "rb") as f:
            if f.read(2) == b"\x1f\x8b": return p
    return None

def convert(src, dst, op, mask, limit):
    tests = json.load(gzip.open(src))
    n = skipped = 0
    with open(dst, "wb") as f:
        for t in tests:
            if limit and n >= limit: break
            if skip_test(op, t): skipped += 1; continue
            ri = t["initial"]["regs"]; rf = dict(ri); rf.update(t["final"]["regs"])
            f.write(struct.pack("<14H", *[ri[k] for k in REGS]))
            f.write(struct.pack("<H", len(t["initial"]["ram"])))
            for a, v in t["initial"]["ram"]: f.write(struct.pack("<IB", a, v))
            f.write(struct.pack("<14H", *[rf[k] for k in REGS]))
            f.write(struct.pack("<H", len(t["final"]["ram"])))
            for a, v in t["final"]["ram"]: f.write(struct.pack("<IB", a, v))
            f.write(struct.pack("<H", mask))
            nm = t["name"].encode()[:255]
            f.write(struct.pack("<B", len(nm)) + nm)
            n += 1
    return n, skipped

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default=os.path.join(os.path.expanduser("~"), ".cache", "sst8088"))
    ap.add_argument("--limit", type=int, default=0, help="tests per opcode (0: all)")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("ops", nargs="*")
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)

    meta = json.load(open(fetch_meta(a.cache)))["opcodes"]
    names = []
    for op, v in sorted(meta.items()):
        subs = [(f"{op}.{r}", rv) for r, rv in sorted(v["reg"].items())] if "reg" in v else [(op, v)]
        for nm, rv in subs:
            if rv.get("status") not in ("normal", "undocumented"): continue
            if a.ops and nm not in a.ops and op not in a.ops: continue
            names.append((nm, rv))

    exe = "/tmp/hd_emu86_sst"
    cc = ["gcc", "-O2", "-std=gnu99", "-DEMU86_ROM=0x100000u", "-I", os.path.join(ROOT, "include"),
          "-o", exe, os.path.join(ROOT, "tests", "emu86_sst.c"), os.path.join(ROOT, "emu", "cpu86.c")]
    if subprocess.call(cc) != 0: sys.exit("check_emu86_sst: the runner did not build")

    total_p = total_t = 0; failed = []; skipped_ops = []
    for nm, rv in names:
        base = nm.split(".")[0]
        if base in DIFFERS: skipped_ops.append((nm, DIFFERS[base])); continue
        if nm in DIFFERS_REG: skipped_ops.append((nm, DIFFERS_REG[nm])); continue
        mask = rv.get("flags-mask", 0xFFFF)
        src = fetch(a.cache, nm)
        if not src: skipped_ops.append((nm, "not in the suite (the 8088 suite has no test for it)")); continue
        vec = os.path.join(a.cache, nm + ".bin")
        n, sk = convert(src, vec, nm, mask, a.limit)
        out = subprocess.run([exe, vec] + (["-v"] if a.v else []), capture_output=True, text=True).stdout
        last = out.strip().splitlines()[-1]
        p, t = map(int, last.split()[0].split("/"))
        total_p += p; total_t += t
        mark = "ok  " if p == t else "FAIL"
        print(f"  {mark} {nm:5s} {p}/{t}" + (f"   ({sk} skipped: 8088-only behaviour)" if sk else ""))
        if p != t:
            failed.append(nm)
            print("\n".join(out.strip().splitlines()[:-1]))
    print()
    for nm, why in skipped_ops: print(f"  skip {nm:5s} {why}")
    print(f"\ncheck_emu86_sst: {total_p}/{total_t} instructions match the 8088"
          + (f"; {len(failed)} opcodes differ: {' '.join(failed)}" if failed else ""))
    sys.exit(1 if failed else 0)

def fetch_meta(cache):
    p = os.path.join(cache, "metadata.json")
    if not os.path.exists(p):
        with urllib.request.urlopen(BASE + "metadata.json", timeout=60) as r, open(p, "wb") as f:
            f.write(r.read())
    return p

if __name__ == "__main__":
    main()
