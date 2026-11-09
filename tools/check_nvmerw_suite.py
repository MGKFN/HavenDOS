#!/usr/bin/env python3
"""check_nvmerw_suite.py -- prove tests/test_nvmerw.c can fail.

nvme_write() is the function that lets HavenDOS write to the disk in a
modern laptop. The dangerous bug in it is not a crash: it is a 4096-byte
block written without being read first, which replaces the seven sectors
beside the one being written and reports success.

── v0.8.6: THIS HARNESS HAD NOT BUILT SINCE v0.8.5.1 ──────────────────

v0.8.5.1 moved the 64-bit BAR decoder out of drivers/pci.c into
drivers/pci_bar.c -- "a stub is not the function", after a stubbed
pci_bar_mem() made it impossible for tests/test_xhci.c to find the
truncation that stopped the xHCI starting. The Makefile learned to link
drivers/pci_bar.c into the four suites that need it.

This file's own compiler line did not, so every run since has ended in

    undefined reference to `pci_bar_mem64'

and reported nothing at all. An injection harness that cannot build is a
harness that has proved nothing, and it is the same disease v0.8.5.6
found in `make test`: a check nobody reads the output of. The 20 October
handoff's table of harness results does not list this one, which is how
it went unnoticed for five releases.

tools/check_harnesses.py is in the `make` checks now and refuses a
harness whose baseline does not build.

Run:  python3 tools/check_nvmerw_suite.py
"""
import os
import subprocess
import sys
import tempfile

SRC = "drivers/nvme.c"
TEST = "tests/test_nvmerw.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-no-pie", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch",
      # The same file the Makefile links into this suite. Four suites LINK
      # drivers/pci_bar.c rather than stubbing it, and this harness has to
      # build the suite the same way `make test` does or it is building a
      # different program.
      "drivers/pci_bar.c"]

BUGS = [
    ("a write-protected namespace is written anyway",
     "NSATTR bit 0 ignored: every write fails, mid-partition-table",
     "    if(d->ro)  return -1;      /* the namespace said so at identify time */",
     "    ;"),

    ("NSATTR is never read, so no namespace is ever protected",
     "the flag exists and is always clear, which is worse than not having it",
     "    return (idns[99] & 0x01u) ? 1 : 0;",
     "    return 0;"),

    ("the wrong NSATTR bit is tested",
     "bit 1 is a shared-namespace hint, not write protection",
     "    return (idns[99] & 0x01u) ? 1 : 0;",
     "    return (idns[99] & 0x02u) ? 1 : 0;"),

    ("the wrong byte of Identify Namespace is read",
     "byte 98 is the tail of NMIC and NAWUPF, not NSATTR",
     "    return (idns[99] & 0x01u) ? 1 : 0;",
     "    return (idns[98] & 0x01u) ? 1 : 0;"),

    # NOT INJECTED, and this is a real gap rather than a redundant guard,
    # so it is written down as one.
    #
    # The line is, inside the controller bring-up:
    #     if(!nvme_ns_parse(dma_buf[slot], &d->nsze, &d->lba_bytes, &d->ro))
    # and it is the join between a parse this suite tests thoroughly and
    # the device fields every read and write is computed from. Replacing
    # it with hardcoded values is not caught, because this harness builds
    # its device by hand (attach()) and never runs bring-up: that needs a
    # controller reset with CSTS tracking CC, an admin queue, Identify
    # Controller, Identify Namespace and two Create I/O Queue commands,
    # none of which is simulated here.
    #
    # The risk is small -- one assignment, from a tested function into
    # fields whose use is tested -- but it is untested, and saying so is
    # better than an injection reporting a MISS that means "we chose not
    # to test this".
    #
    # tests/test_ahci.c shows the shape of the fix: a controller that
    # executes the commands, built in process memory behind
    # include/mmio.h. Doing the same for NVMe bring-up is on the handoff.

    ("FLBAS is not masked to four bits",
     "a drive with the metadata bit set selects a format entry past the table",
     "    flbas = idns[26] & 0x0Fu;",
     "    flbas = idns[26];"),

    ("FLBAS is ignored and format zero is always read",
     "the right answer on most drives and the wrong one on the rest",
     "    lbads = idns[128 + (uint32_t)flbas*4u + 2u];",
     "    lbads = idns[128 + 2u];"),

    ("an unreasonable block size is taken at face value",
     "a 64 KB block against a 4 KB bounce buffer",
     "    if(lbads < 9 || lbads > 12) lbads = 9;",
     "    ;"),

    ("NSZE is read as four bytes instead of eight",
     "every namespace over 2 TB reports a fraction of its size",
     "    for(i = 0; i < 8; i++) nsze |= (uint64_t)idns[i] << (8*i);",
     "    for(i = 0; i < 4; i++) nsze |= (uint64_t)idns[i] << (8*i);"),

    ("a namespace that does not exist is accepted",
     "zero blocks, and every bounds check against it passes",
     "    if(!nsze) return 0;",
     "    ;"),

    ("nvme_writable reports a protected namespace as writable",
     "the installer offers the disk and finds out part-way through",
     "    return g_dev[idx].ok && !g_dev[idx].ro;",
     "    return g_dev[idx].ok;"),

    ("a partial block is written without being read first",
     "THE HAZARD: seven sectors of somebody's filesystem, gone, reported ok",
     "        if(skip != 0 || have != per){\n"
     "            nvme_sqe_t r; memset(&r, 0, sizeof r);",
     "        if(0){\n"
     "            nvme_sqe_t r; memset(&r, 0, sizeof r);"),

    # Not injected: dropping the `skip != 0` half of the read-before-write
    # condition. It is redundant, and provably so rather than probably:
    # have is computed as `per - skip` and then only ever clamped DOWN, so
    # skip != 0 implies have < per implies have != per. The second half of
    # the condition is true wherever the first is.
    #
    # It stays because it says what the two cases are -- a block we do not
    # start at the beginning of, and a block we do not fill to the end --
    # and those are the two reasons to read, not one. If `have` ever stops
    # being derived from `skip`, the redundancy goes with it.

    ("the read-before-write ignores a short tail",
     "a block whose start is written keeps nothing after it",
     "        if(skip != 0 || have != per){",
     "        if(skip != 0){"),

    ("the write opcode is the read opcode",
     "every write is a read, and nothing is ever stored",
     "        c.cdw0  = 0x01u;                        /* Write                 */",
     "        c.cdw0  = 0x02u;"),

    ("the read-before-write uses the write opcode",
     "the block is written with stale buffer contents before it is patched",
     "            r.cdw0  = 0x02u;                    /* Read                  */",
     "            r.cdw0  = 0x01u;"),

    ("the destination offset inside the block is ignored",
     "every sector written lands at the start of its block",
     "        memcpy(dma_buf[idx] + skip*512u, in + done*512u, have*512u);",
     "        memcpy(dma_buf[idx], in + done*512u, have*512u);"),

    ("the source offset is ignored",
     "the first sector of the caller's buffer written over and over",
     "        memcpy(dma_buf[idx] + skip*512u, in + done*512u, have*512u);",
     "        memcpy(dma_buf[idx] + skip*512u, in, have*512u);"),

    ("the block number's high half is dropped",
     "a namespace over 2 TB wraps to the start of itself",
     "        c.cdw0  = 0x01u;                        /* Write                 */\n"
     "        c.nsid  = d->nsid;\n"
     "        c.prp1  = (uint64_t)(uint32_t)dma_buf[idx];\n"
     "        c.cdw10 = (uint32_t)block;\n"
     "        c.cdw11 = (uint32_t)(block >> 32);",
     "        c.cdw0  = 0x01u;\n"
     "        c.nsid  = d->nsid;\n"
     "        c.prp1  = (uint64_t)(uint32_t)dma_buf[idx];\n"
     "        c.cdw10 = (uint32_t)block;\n"
     "        c.cdw11 = 0;"),

    ("the write runs past the end of the namespace",
     "a command the controller refuses, issued anyway",
     "        if(block >= d->nsze) return -1;\n\n"
     "        uint32_t have = per - skip;",
     "        ;\n\n"
     "        uint32_t have = per - skip;"),

    ("a failed command is reported as success",
     "the installer believes the kernel is on the disk",
     "        if(submit_wait(d, 1, &c, 0) != 0) return -1;\n\n"
     "        done += have;",
     "        submit_wait(d, 1, &c, 0);\n\n"
     "        done += have;"),

    ("a failed read-before-write is ignored",
     "the block is patched into whatever the bounce buffer held",
     "            if(submit_wait(d, 1, &r, 0) != 0) return -1;",
     "            submit_wait(d, 1, &r, 0);"),

    ("more is copied than the chunk holds",
     "the caller's buffer read past its end, into the next block's data",
     "        uint32_t have = per - skip;\n"
     "        if(have > count - done) have = count - done;\n\n"
     "        /* Partial block: read it before changing part of it. */",
     "        uint32_t have = per - skip;\n\n"
     "        /* Partial block: read it before changing part of it. */"),

    ("the arguments are not checked",
     "a null buffer and a zero count reach the queue",
     "int nvme_write(int idx, uint64_t lba, uint32_t count, const void *buf)\n"
     "{\n"
     "    if(idx < 0 || idx >= g_count || !buf || !count) return -1;",
     "int nvme_write(int idx, uint64_t lba, uint32_t count, const void *buf)\n"
     "{\n"
     "    if(idx < 0 || idx >= g_count) return -1;"),

    ("a controller that never came up is written to",
     "commands posted to a queue the controller is not reading",
     "    if(!d->ok) return -1;\n"
     "    if(d->ro)  return -1;      /* the namespace said so at identify time */\n\n"
     "    /* No 64-bit divide",
     "    if(d->ro)  return -1;      /* the namespace said so at identify time */\n\n"
     "    /* No 64-bit divide"),

    ("sectors-per-block is taken as one, whatever the namespace says",
     "a 4 KB namespace addressed as though its blocks were 512 bytes",
     "    uint32_t per = d->lba_bytes / 512u;\n"
     "    if(!per) return -1;",
     "    uint32_t per = 1;"),

    ("the block number is the sector number",
     "every write to a 4 KB namespace lands eight times too far in",
     "        for(uint32_t k = 0; k < per_shift; k++) block >>= 1;\n"
     "        uint32_t skip  = (uint32_t)abs & (per - 1u);\n"
     "        if(block >= d->nsze) return -1;\n\n"
     "        uint32_t have = per - skip;",
     "        uint32_t skip  = (uint32_t)abs & (per - 1u);\n"
     "        if(block >= d->nsze) return -1;\n\n"
     "        uint32_t have = per - skip;"),
]


def build_and_run(timeout):
    exe = os.path.join(tempfile.gettempdir(), "hd_tnvrw_inj")
    b = subprocess.run(CC + ["-o", exe, TEST], capture_output=True, text=True)
    if b.returncode != 0:
        return "build-failed", b.stderr
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "hang", ""
    return ("pass" if r.returncode == 0 else "fail"), r.stdout


def main():
    if not os.path.exists(SRC):
        sys.stderr.write("run me from the source root\n")
        return 2
    original = open(SRC, encoding="utf-8").read()

    print("Injecting %d bugs into nvme_write() in %s.\n"
          "This is the function that writes to the disk in a modern laptop.\n"
          % (len(BUGS), SRC))

    st, out = build_and_run(600)
    if st != "pass":
        print("  the UNMODIFIED tree does not pass (%s) -- fix that first" % st)
        print(out[-2500:])
        return 2
    print("  baseline: the unmodified driver passes\n")

    missed, stale = [], []
    try:
        for i, (label, why, old, new) in enumerate(BUGS, 1):
            n = original.count(old)
            if n != 1:
                stale.append((label, n))
                print("  %2d. %-58s INJECTION STALE (%d)" % (i, label, n))
                continue
            open(SRC, "w", encoding="utf-8").write(original.replace(old, new, 1))
            st, _ = build_and_run(300)
            if st == "pass":
                missed.append((label, why))
                verdict = "MISSED"
            elif st == "build-failed":
                verdict = "caught (did not compile)"
            elif st == "hang":
                verdict = "caught (hung)"
            else:
                verdict = "caught"
            print("  %2d. %-58s %s" % (i, label, verdict))
    finally:
        open(SRC, "w", encoding="utf-8").write(original)

    print()
    if stale:
        print("  %d injection(s) no longer match the source:" % len(stale))
        for label, n in stale:
            print("    - %s (%d matches)" % (label, n))
    if missed:
        print("  %d bug(s) the suite did NOT catch:" % len(missed))
        for label, why in missed:
            print("    - %s\n        %s" % (label, why))
        return 1
    if stale:
        return 1
    print("  all %d caught" % len(BUGS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
