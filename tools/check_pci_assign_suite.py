#!/usr/bin/env python3
"""check_pci_assign_suite.py -- prove tests/test_pci_assign.c can fail.

drivers/pci_assign.c (v0.9.6.6) writes a BAR on real hardware: the HP's
two Serial-IO I2C controllers, which the firmware left at zero. Every
way it can be wrong lands on SOMEBODY ELSE -- a window on the xHCI is a
dead keyboard, one on the display a garbled screen, a window below 4 GB
can sit on the local APIC -- so each defect below is one of those, and
the suite has to catch it.

Its first run caught 10 of 12. The two that survived -- the overlap test
off by one at the end, and a trim that shortened a bridge window -- did
so because no range in the suite ended one byte into a candidate and no
BAR sat inside a bridge window. Both cases are in the suite now.

Run:  python3 tools/check_pci_assign_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "drivers/pci_assign.c"
TEST = "tests/test_pci_assign.c"
CC = ["gcc", "-std=gnu99", "-Wall", "-Wno-unused-function", "drivers/pci_bar.c"]

BUGS = [
    ("the overlap test is off by one at the end",
     "a window starting on the last byte of a BAR's bound counts as free, "
     "so it can be placed on the first page of the next device",
     """        if(a < r[i].end && r[i].base < a + size) return 1;""",
     """        if(a + 1 < r[i].end && r[i].base < a + size) return 1;"""),

    ("anchors below 4 GB are allowed",
     "below 4 GB live the local APIC, the I/O APIC, the HPET and the flash, "
     "none of them BARs -- a window placed there can land on any of them",
     """        if(used[i].base < GB4) continue;                    /* anchors: above 4 GB only */""",
     """        if(0) continue;"""),

    ("bounds are never trimmed at the next BAR",
     "the display's 16 MB register BAR is aligned to 128 GB, so its bound "
     "swallows the whole PCH and the window lands a gigabyte away from "
     "anything the firmware is known to decode",
     """    pci_trim_ranges(out, n);
    return n;""",
     """    return n;"""),

    ("the lowest address wins, not the closest",
     "on the HP the lowest free address is 1 GB past the display's "
     "aperture, far outside the cluster the firmware decodes",
     """        if(!best || dist < best_d || (dist == best_d && a < best)){ best = a; best_d = dist; }""",
     """        if(!best || a < best){ best = a; best_d = dist; }"""),

    ("a trim also shortens a bridge window",
     "a bridge window is exact; trimming it at a BAR inside it hands the "
     "rest of the window -- devices behind the bridge -- to the planner",
     """        if(!r[i].bar) continue;                /* a bridge window is exact */""",
     """        (void)0;"""),

    ("bridge windows are not collected",
     "whatever sits behind the root port (the NVMe) is invisible, and the "
     "window can land on the disk's registers",
     """            if(ml > mb){ out[n].base = mb; out[n].end = ml + 1; out[n].bar = 0; n++; }""",
     """            (void)ml; (void)mb;"""),

    ("memory decoding is left on while the BAR is written",
     "the device answers at all-ones and at a half-written address while "
     "it is being moved -- on top of whatever lives there",
     """    pci_cfg_write16(B, D, F, 0x04, (uint16_t)(cmd & ~0x0002u));   /* memory decode off */""",
     """    pci_cfg_write16(B, D, F, 0x04, (uint16_t)(cmd | 0x0002u));"""),

    ("the write is not read back",
     "a BAR that ignored the address is reported as assigned, decoding is "
     "turned on, and the driver reads somebody else's registers",
     """    if((rl & ~0xFu) != (uint32_t)a || rh != (uint32_t)(a >> 32)){""",
     """    if(0){"""),

    ("the table is not told",
     "pci_bar_mem64() still says zero, so the driver gives up on a window "
     "that exists -- and the second controller is planned on top of the first",
     """    pci_set_bar(d, n, rl);
    pci_set_bar(d, n + 1, rh);""",
     """    (void)rh;"""),

    ("the high half is not written",
     "the window is at the low 32 bits of the address -- below 4 GB, on "
     "top of whatever is there",
     """    pci_cfg_write32(B, D, F, (uint8_t)(reg + 4), (uint32_t)(a >> 32));
    rl = pci_cfg_read32(B, D, F, reg);""",
     """    rl = pci_cfg_read32(B, D, F, reg);"""),

    ("a 32-bit BAR is assigned anyway",
     "with no 64-bit high half, the planner's address is truncated to the "
     "bottom 4 GB",
     """    if(!is64) return PA_32BIT;""",
     """    (void)is64;"""),

    ("an already-assigned BAR is moved",
     "a device the firmware placed -- and may be using -- is moved under it",
     """    if((lo0 & ~0xFu) || hi0) return PA_ASSIGNED;""",
     """    (void)hi0;"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """static uint64_t lowbit(uint64_t v){ return v & (~v + 1); }""",
    """/* the lowest set bit */
static uint64_t lowbit(uint64_t v){ return v & (~v + 1); }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "pci_assign", SRC, TEST, CC, BUGS, HARMLESS, timeout=60,
        preamble="Proving tests/test_pci_assign.c can fail.\n"
                 "drivers/pci_assign.c writes a BAR on the HP; every defect "
                 "below lands on another device."))
