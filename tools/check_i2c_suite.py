#!/usr/bin/env python3
"""check_i2c_suite.py -- prove tests/test_i2c.c can fail.

drivers/i2c.c HAS NEVER SEEN AN I2C CONTROLLER. It was written from
the Synopsys DesignWare register map, for a machine that is not here,
and the first time it runs against real silicon will be on Kyan's HP.
That is precisely the position tests/test_usbmux.c was in when it
passed 79 of 79 against a sixteen-byte header the phone has never
used.

So the suite's value is not "it passes". It is that the simulated core
is awkward in the specific ways the hardware is awkward, and that
every one of those is checked. This harness is what says so.

── THE ONE IT ALREADY CAUGHT, BEFORE ANY INJECTION ───────────────────

The first version of the suite reached the driver through a
VGA_MEM_PTR-style seam: a macro returning a `volatile uint32_t *`.
That works for the console, because the console's memory IS memory --
writing to it is a write and nothing else happens.

An I2C controller is not memory. Writing IC_DATA_CMD puts a byte on a
wire. With a pointer seam every write the driver made landed in the
scratch word the macro handed back, the LPSS reset was never released,
and 31 of 41 checks failed in a way that looked like a broken driver.
The seam is two hooks now, I2C_RD and I2C_WR.

── WHAT THESE INJECTIONS ARE ABOUT ───────────────────────────────────

Four things about this controller are easy to get wrong in a way that
works on a bench and fails on a laptop:

  the LPSS reset    Intel holds the core in reset out of a cold boot
                    and every register reads zero until it is
                    released. A driver that skips it sees a BAR full
                    of zeroes, which is indistinguishable from a
                    mapping that failed -- the exact shape of the bug
                    that cost v0.8.5 its USB.
  IC_TAR while enabled  the hardware IGNORES it. A driver that does
                    not disable first addresses whatever was there
                    before, and on a fresh core that is address zero
                    -- the general-call address, which every device
                    on the bus listens to.
  the sticky abort  a NAK latches, and a scan does 112 in a row. One
                    that is not cleared makes every address after the
                    first failure report as absent.
  the repeated start  a stop between the register number and the read
                    releases the bus, and another master may move the
                    register pointer before the read happens.

── AND THE SCAN HAS TO BE NON-DESTRUCTIVE ────────────────────────────

It probes with a zero-length READ. A write to an unknown device can
do anything, and the whole reason a scan is acceptable at all is that
it writes nothing. An injection that turns it into a write is in the
list below.

Run:  python3 tools/check_i2c_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "drivers/i2c.c"
TEST = "tests/test_i2c.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    ("the LPSS reset is never released",
     "Intel holds the DesignWare core in reset out of a cold boot and "
     "every register reads back zero until it is released. A driver "
     "that skips it sees a BAR full of zeroes, which looks exactly "
     "like a mapping that failed -- and that is the bug that cost "
     "v0.8.5 its USB",
     """    wr(b, LPSS_RESET, LPSS_RESET_RELEASE);""",
     """    /* reset not released */"""),

    ("the core is not asked what it is",
     "class 0C/80 is 'serial bus controller, other' and plenty of "
     "things report it. IC_COMP_TYPE is how a DesignWare core "
     "identifies itself, and writing this register map into "
     "something else is worse than doing nothing",
     """    t = rd(b, IC_COMP_TYPE);
    if(t != DW_COMP_TYPE) return 0;""",
     """    t = rd(b, IC_COMP_TYPE);
    (void)t;"""),

    ("the FIFO depth is assumed instead of read",
     "IC_COMP_PARAM_1 says how deep it is, and cores ship with 16 and "
     "with 32. A driver that believes 32 on a core with 16 overruns "
     "it by sixteen bytes, and the bytes it loses are the middle of "
     "a transfer",
     """    param = rd(b, IC_COMP_PARAM_1);
    b->txfifo = ((param >> 16) & 0xFFu) + 1u;""",
     """    param = rd(b, IC_COMP_PARAM_1);
    (void)param;
    b->txfifo = 32u;"""),

    ("a transfer longer than the FIFO is truncated instead of refused",
     "a short write that reports success is a register half written, "
     "and on a touchpad that is a mode nobody chose. There is no DMA "
     "here, so the only correct answer is a named refusal",
     """    if(len > b->txfifo) return I2C_ETOOLONG;

    r = set_target(b, addr);
    if(r != I2C_OK) return r;
    (void)rd(b, IC_CLR_INTR);

    for(i = 0; i < len; i++){
        uint32_t cmd = buf[i];""",
     """    if(len > b->txfifo) len = b->txfifo;

    r = set_target(b, addr);
    if(r != I2C_OK) return r;
    (void)rd(b, IC_CLR_INTR);

    for(i = 0; i < len; i++){
        uint32_t cmd = buf[i];"""),

    ("the target address is set with the core still enabled",
     "the hardware IGNORES a write to IC_TAR while it is running, so "
     "the transfer goes to whatever address was there before. On a "
     "freshly reset core that is zero -- the general-call address, "
     "which every device on the bus listens to",
     """    disable(b);
    wr(b, IC_TAR, (uint32_t)(addr & 0x7Fu));""",
     """    wr(b, IC_TAR, (uint32_t)(addr & 0x7Fu));"""),

    ("a NAK is not told apart from a timeout",
     "the whole of i2c_scan() rests on this. If every failure is one "
     "code, a scan reports either all 112 addresses or none of them, "
     "and the one thing this release exists to find out is which "
     "addresses answer",
     """    if(src & (ABRT_7B_ADDR_NOACK | ABRT_TXDATA_NOACK)) return I2C_ENAK;""",
     """    if(src & (ABRT_7B_ADDR_NOACK | ABRT_TXDATA_NOACK)) return I2C_ETIMEOUT;"""),

    ("the abort is never cleared",
     "a NAK latches in hardware. A scan does 112 probes in a row, so "
     "one uncleared abort makes every address after the first "
     "failure report as absent -- and the first failure is 0x08",
     """    uint32_t src = rd(b, IC_TX_ABRT_SOURCE);
    (void)rd(b, IC_CLR_TX_ABRT);""",
     """    uint32_t src = rd(b, IC_TX_ABRT_SOURCE);"""),

    ("write-then-read puts a STOP between the halves",
     "a stop releases the bus, and another master may address the "
     "same device before the read -- after which the register "
     "pointer is whatever they left it at. The repeated start is the "
     "only reason CON_RESTART_EN is set",
     """        if(i == 0) cmd |= CMD_RESTART;""",
     """        if(i == 0) cmd |= CMD_STOP;"""),

    ("the probe WRITES to the address instead of reading",
     "the only reason a bus scan is acceptable is that it writes "
     "nothing. A write to an unknown device can do anything -- set a "
     "mode, start a firmware update, latch a configuration byte. A "
     "zero-length read cannot",
     """    return i2c_read(bus, addr, 0, 0);""",
     """    { uint8_t z = 0; return i2c_write(bus, addr, &z, 1); }"""),

    ("the scan probes the reserved addresses",
     "0x00-0x07 and 0x78-0x7F are reserved by the I2C specification. "
     "0x00 is the GENERAL CALL address and every device on the bus "
     "listens to it, so probing it is not harmless and the answer it "
     "gives is not about one device",
     """    for(a = 0x08; a <= 0x77; a++){""",
     """    for(a = 0x00; a <= 0x7F; a++){"""),

    ("a bus index out of range is accepted",
     "g_bus[] is four entries and the index comes from a shell "
     "argument. Reading past it is whatever follows in bss, and the "
     "mmio field of that is a pointer the driver then writes through",
     """    if(bus < 0 || bus >= g_nbus) return I2C_ENOBUS;
    b = &g_bus[bus];
    if(!b->ready) return I2C_ENOTREADY;
    if(len > b->txfifo) return I2C_ETOOLONG;

    r = set_target(b, addr);
    if(r != I2C_OK) return r;
    (void)rd(b, IC_CLR_INTR);

    for(i = 0; i < len; i++){
        uint32_t cmd = buf[i];""",
     """    if(bus < 0) return I2C_ENOBUS;
    b = &g_bus[bus];
    if(!b->ready) return I2C_ENOTREADY;
    if(len > b->txfifo) return I2C_ETOOLONG;

    r = set_target(b, addr);
    if(r != I2C_OK) return r;
    (void)rd(b, IC_CLR_INTR);

    for(i = 0; i < len; i++){
        uint32_t cmd = buf[i];"""),

    ("a 64-bit BAR is taken as its low half",
     "the HP puts this controller's registers at 0x60_xxxxxxxx. "
     "pci_bar_mem() would hand back the low dword and every register "
     "read would come from whatever sits 33 MB up. That is not a "
     "hypothetical: it is what v0.8.5's xHCI did, and it got a "
     "nonsense CAPLENGTH and correctly refused to start",
     """        lo = pci_bar_mem64(d, 0, &hi);
        if(!lo && !hi){""",
     """        lo = pci_bar_mem64(d, 0, &hi);
        hi = 0;
        if(!lo && !hi){"""),

    ("a BAR that will not map is used anyway",
     "highfb_map_io() returns SUCCESS and writes the address out. "
     "Reading its return value as the address gives 1 as a pointer, "
     "which is registers read out of the interrupt vector table -- "
     "and this file had exactly that mistake before it first built",
     """            if(!highfb_map_io(hi, lo, 0x1000u, &virt) || !virt){""",
     """            highfb_map_io(hi, lo, 0x1000u, &virt);
            if(0){"""),

    # ── v0.8.9.6 ──────────────────────────────────────────────────────
    ("a controller left in D3 is never powered up",
     "every register reads FFFFFFFF, bring_up() says 'not a DesignWare "
     "core', and the trackpad behind it is never looked for",
     """            int was = pci_set_d0(d, sleep_ms);""",
     """            int was = 0;"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the driver changed",
    """int              i2c_bus_count(void)       { return g_nbus; }""",
    """/* how many controllers came up */
int              i2c_bus_count(void)       { return g_nbus; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "i2c", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        preamble="Proving tests/test_i2c.c can fail.\n"
                 "drivers/i2c.c has never seen an I2C controller --\n"
                 "it was written from the DesignWare register map for\n"
                 "a machine that is not here. The suite's worth is that\n"
                 "the simulated core is awkward in the ways the real one\n"
                 "is, and this is what says so."))
