#!/usr/bin/env python3
"""check_ahci_suite.py -- prove tests/test_ahci.c can fail.

tests/test_ahci.c passed on its first run, which in this tree has meant
very little: the FAT32 suite reported 168/168 while missing ten of
twenty-six injected bugs, the ESP suite 74/74 while missing eight of
sixteen, the FAT16 formatter 81/81 while missing six of twenty-one, and
the additive installer 48/48 while missing nine of twenty-one.

This driver writes to the disk in a modern laptop, through a path that
has never run on real silicon. The suite over it had better be able to
notice when it is wrong.

Run:  python3 tools/check_ahci_suite.py
"""
import os
import subprocess
import sys
import tempfile

SRC = "drivers/ahci.c"
TEST = "tests/test_ahci.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-no-pie", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    # ── the command header ──────────────────────────────────────────
    ("a write does not set the W bit",
     "THE HAZARD: the controller transfers the other way and reports success",
     "    h->flags = (uint16_t)((sizeof(uint32_t) == 4 ? 5u : 5u)   /* CFL in dwords */\n"
     "                          | (write ? 0x40u : 0u));",
     "    h->flags = 5u;"),

    ("every command sets the W bit",
     "a read that writes the buffer's old contents onto the disk",
     "                          | (write ? 0x40u : 0u));",
     "                          | 0x40u);"),

    ("the command FIS length is wrong",
     "the controller reads five dwords of a FIS that says it is shorter",
     "    h->flags = (uint16_t)((sizeof(uint32_t) == 4 ? 5u : 5u)   /* CFL in dwords */",
     "    h->flags = (uint16_t)((sizeof(uint32_t) == 4 ? 4u : 4u)   /* CFL in dwords */"),

    ("the scatter-gather list is never declared",
     "PRDTL zero: the controller moves no data and reports success",
     "    h->prdtl = bytes ? 1 : 0;",
     "    h->prdtl = 0;"),

    ("the command table address is not set",
     "the controller follows a null pointer to find the command",
     "    h->ctba  = (uint32_t)(uintptr_t)t;",
     "    h->ctba  = 0;"),

    # ── the PRDT ────────────────────────────────────────────────────
    ("the byte count is not zero-based",
     "an off-by-one that transfers 513 bytes into a 512-byte buffer",
     "        t->prdt[0].dbc  = (bytes - 1u) & 0x003FFFFFu;",
     "        t->prdt[0].dbc  = bytes & 0x003FFFFFu;"),

    ("the data buffer address is not set",
     "the transfer goes to physical address zero",
     "        t->prdt[0].dba  = (uint32_t)(uintptr_t)buf;",
     "        t->prdt[0].dba  = 0;"),

    # ── the command FIS ─────────────────────────────────────────────
    ("the FIS is not marked as a command",
     "bit 7 of byte 1 clear makes it a control FIS, which transfers nothing",
     "    f[1] = 0x80;                     /* C: this is a command, not control */",
     "    f[1] = 0x00;"),

    ("the FIS type is not a Host-to-Device Register FIS",
     "the controller does not recognise it",
     "    f[0] = FIS_H2D;",
     "    f[0] = 0x34;"),

    ("LBA mode is not selected",
     "bit 6 of the device register clear means CHS addressing",
     "    f[7] = 0x40;                     /* LBA mode                          */",
     "    f[7] = 0x00;"),

    ("the top sixteen bits of the LBA are dropped",
     "exactly what drivers/ata.c does: every access above 2 TB lands elsewhere",
     "    f[9] = (uint8_t)(lba >> 32);\n"
     "    f[10]= (uint8_t)(lba >> 40);",
     "    f[9] = 0;\n"
     "    f[10]= 0;"),

    ("LBA[31:24] is dropped",
     "the 8 GiB truncation, which this tree has already shipped once",
     "    f[8] = (uint8_t)(lba >> 24);",
     "    f[8] = 0;"),

    ("the sector count is never set",
     "zero sectors, which on ATA means 65536 of them",
     "    f[12]= (uint8_t)(count     );\n"
     "    f[13]= (uint8_t)(count >> 8);",
     "    f[12]= 0;\n"
     "    f[13]= 0;"),

    ("the read and write opcodes are swapped",
     "every read writes and every write reads",
     "    if(!run_cmd(idx, ATA_READ_DMA, lba, 1, g_dma[idx], 512, 0)) return 0;",
     "    if(!run_cmd(idx, ATA_WRITE_DMA, lba, 1, g_dma[idx], 512, 0)) return 0;"),

    # ── the port ────────────────────────────────────────────────────
    ("the port is not stopped before its command list is replaced",
     "writing PxCLB with PxCMD.CR set is indeterminate by specification",
     "    if(!port_stop(abar, port)) return 0;",
     "    ;"),

    ("port_stop does not wait for the engines to stop",
     "the registers are written while the controller is still reading them",
     "    for(int i = 0; i < 1000; i++){\n"
     "        cmd = pr(abar, port, PxCMD);\n"
     "        if(!(cmd & (CMD_CR | CMD_FR))) return 1;\n"
     "        sleep_ms(1);\n"
     "    }\n"
     "    return 0;",
     "    return 1;"),

    ("only the first implemented port is brought up",
     "what drivers/ata.c does, and why a two-disk machine shows one disk",
     "        port_init(abar, (int)port);",
     "        if(port_init(abar, (int)port)) break;"),

    ("ports that are not implemented are probed anyway",
     "registers read from a port the controller does not have",
     "        if(!(pi & (1u << port))) continue;",
     "        ;"),

    ("a port with no device is accepted",
     "DET 3 means a device is present and communicating; anything else is not",
     "    if(det != 3u || ipm != 1u) return 0;      /* nothing there, or asleep */",
     "    ;"),

    ("the device signature is not checked",
     "a port multiplier or an enclosure bridge taken for a disk",
     "    if(sig != SIG_ATA && sig != SIG_ATAPI) return 0;",
     "    ;"),

    ("an optical drive is treated as a disk",
     "READ DMA EXT against a device that does not implement it",
     "    d->atapi = (sig == SIG_ATAPI);",
     "    d->atapi = 0;"),

    # ── IDENTIFY ────────────────────────────────────────────────────
    ("LBA48 support is assumed rather than read from word 83",
     "words 100..103 are meaningless on a drive that does not support it",
     "    if(id16(id, 83) & 0x0400u){",
     "    if(1){"),

    ("the capacity words are read in the wrong order",
     "word 100 is the low half, not the high",
     "        d->sectors = (uint64_t)id16(id, 100)\n"
     "                   | ((uint64_t)id16(id, 101) << 16)\n"
     "                   | ((uint64_t)id16(id, 102) << 32)\n"
     "                   | ((uint64_t)id16(id, 103) << 48);",
     "        d->sectors = (uint64_t)id16(id, 103)\n"
     "                   | ((uint64_t)id16(id, 102) << 16)\n"
     "                   | ((uint64_t)id16(id, 101) << 32)\n"
     "                   | ((uint64_t)id16(id, 100) << 48);"),

    ("IDENTIFY's result is not checked before the drive is accepted",
     "a drive of zero sectors, against which every bounds check passes",
     "    return d->sectors != 0;",
     "    return 1;"),

    ("the model string is not byte-swapped",
     "ATA stores it big-endian within each word; it reads as gibberish",
     "        d->model[i*2]     = (char)id[27*2 + i*2 + 1];\n"
     "        d->model[i*2 + 1] = (char)id[27*2 + i*2];",
     "        d->model[i*2]     = (char)id[27*2 + i*2];\n"
     "        d->model[i*2 + 1] = (char)id[27*2 + i*2 + 1];"),

    # ── the bounds and the refusals ─────────────────────────────────
    ("a read past the end of the disk is issued",
     "a command the drive refuses, or worse, wraps",
     "    if(g_drv[idx].sectors && lba >= g_drv[idx].sectors) return 0;\n"
     "    if(!run_cmd(idx, ATA_READ_DMA, lba, 1, g_dma[idx], 512, 0)) return 0;",
     "    if(!run_cmd(idx, ATA_READ_DMA, lba, 1, g_dma[idx], 512, 0)) return 0;"),

    ("a write past the end of the disk is issued",
     "the same, on the path where it matters",
     "    if(g_drv[idx].sectors && lba >= g_drv[idx].sectors) return 0;\n"
     "    memcpy(g_dma[idx], buf, 512);",
     "    memcpy(g_dma[idx], buf, 512);"),

    ("an optical drive is readable as a disk",
     "ATAPI needs a packet command, not READ DMA EXT",
     "    if(!g_drv[idx].ok || g_drv[idx].atapi) return 0;\n"
     "    if(g_drv[idx].sectors && lba >= g_drv[idx].sectors) return 0;\n"
     "    if(!run_cmd(idx, ATA_READ_DMA",
     "    if(!g_drv[idx].ok) return 0;\n"
     "    if(g_drv[idx].sectors && lba >= g_drv[idx].sectors) return 0;\n"
     "    if(!run_cmd(idx, ATA_READ_DMA"),

    ("the drive index is not range-checked",
     "a read through a pointer past the end of the drive table",
     "    if(idx < 0 || idx >= g_n || !buf) return 0;\n"
     "    if(!g_drv[idx].ok || g_drv[idx].atapi) return 0;\n"
     "    if(g_drv[idx].sectors && lba >= g_drv[idx].sectors) return 0;\n"
     "    if(!run_cmd(idx, ATA_READ_DMA",
     "    if(!buf) return 0;\n"
     "    if(!g_drv[idx].ok || g_drv[idx].atapi) return 0;\n"
     "    if(g_drv[idx].sectors && lba >= g_drv[idx].sectors) return 0;\n"
     "    if(!run_cmd(idx, ATA_READ_DMA",

     ),

    # ── completion ──────────────────────────────────────────────────
    ("the task file error bit is not checked",
     "a command the drive rejected, reported as success",
     "            if(tfd & TFD_ERR) return 0;",
     "            ;"),

    ("the command is never issued",
     "PxCI untouched: the loop below waits for a bit that was never set",
     "    pw(d->abar, d->port, PxCI, 1u);",
     "    ;"),

    ("the bus master bit is not set",
     "a controller that cannot do DMA, which is the only way AHCI moves data",
     "    pci_enable(p, 1);                          /* memory space + bus master */",
     "    pci_enable(p, 0);"),
]


def build_and_run(timeout):
    exe = os.path.join(tempfile.gettempdir(), "hd_tahci_inj")
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

    print("Injecting %d real AHCI bugs into %s.\n"
          "This driver writes to the disk in a modern laptop.\n" % (len(BUGS), SRC))

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
