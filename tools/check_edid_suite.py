#!/usr/bin/env python3
"""check_edid_suite.py -- prove tests/test_edid.c can fail.

0.9.1 is the first release in the display series that WRITES, and what
it writes is the GMBUS controller: six registers, 0xC5100-0xC5120. QEMU
has no Intel display engine, so drivers/xegmbus.c meets real silicon for
the first time on the HP. Between the two there is this suite.

Two families of mistake matter:

  the transfer   a GMBUS read that does not release the pin, does not
                 clear a latched NAK, issues a command after GMBUS0
                 failed to hold the pin, or tramples a transfer already
                 in flight. Each of those leaves the controller in a
                 state the NEXT read (or the firmware) inherits.
  the parse      an EDID decoded wrong is a mode 0.9.2 will program into
                 pipe B. A sync offset off by a nibble is a monitor that
                 shows nothing, and nothing on screen to say why.

Defects are planted in BOTH drivers/xegmbus.c and drivers/edid.c (the
fifth element names the file when it is not the first).

── WHAT IT FOUND ON ITS FIRST RUN ─────────────────────────────────────

95 of 95 passed, and this reported THREE misses out of 24 -- all in
the suite, none in the harness:

  no test timing had a sync offset past 255, so the top two bits of
      the horizontal offset were never read. CEA-861's 1080p24 (front
      porch 638) is in the suite now.
  every test timing had equal h and v polarity, so reading hsync from
      the vsync bit was invisible. The 144 Hz test mode is +h -v and
      both are checked.
  the "block present but not announced" test copied block 0 into the
      second slot -- which is not a CTA block and is ignored whether
      the count is obeyed or not. It is a valid CTA block now.

The same shapes as every release: a fixture that cannot produce the
case, and a check that would pass either way.

Run:  python3 tools/check_edid_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "drivers/xegmbus.c"
PARSER = "drivers/edid.c"
TEST = "tests/test_edid.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function",
      "-fsanitize=address,undefined"]

BUGS = [
    # ── the transfer ──────────────────────────────────────────────
    ("the pin is not released after a transfer",
     "GMBUS0 left holding pin 2 means the next user of the controller -- "
     "the firmware on the next boot, or i915 under Linux -- starts on "
     "a pin it did not choose",
     """    gmbus_wr(dev, tr, G0, 0);              /* release the pin          */""",
     """    /* pin left held */"""),

    ("a NAK's latched status is not cleared",
     "SATOER latches. Without SW_CLR_INT every later transfer fails "
     "immediately, and the retry that rescues a sleepy monitor never "
     "had a chance",
     """        gmbus_wr(dev, tr, G1, G1_SW_CLR_INT);""",
     """        /* latch left set */"""),

    ("there is no retry on a first NAK",
     "i915 retries once because monitors coming out of standby miss "
     "the first address. Without it, a monitor that is simply waking "
     "up reads as 'nothing answered'",
     """    if(r == GMBUS_ENAK){
        tr->retried = 1;""",
     """    if(0){
        tr->retried = 1;"""),

    ("the GMBUS0 read-back is not compared",
     "if the pin did not take, the command runs on whatever pin GMBUS0 "
     "does hold -- possibly the panel's. The read-back is the one "
     "check that stops that",
     """    if(gmbus_wr(dev, tr, G0, reg0) != reg0){""",
     """    if((gmbus_wr(dev, tr, G0, reg0), 0)){"""),

    ("a transfer already in flight is trampled",
     "the firmware may still be mid-transfer. Starting a new command "
     "under it corrupts both",
     """    if(gm_rd(dev, G2) & G2_ACTIVE){""",
     """    if(0){"""),

    ("a NAK is mistaken for data",
     "SATOER checked after HW_RDY instead of before: a target that "
     "never answered reads as a stream of whatever GMBUS3 holds",
     """        if(s & G2_SATOER){ tr->status = s; return GMBUS_ENAK; }""",
     """        if(0){ tr->status = s; return GMBUS_ENAK; }"""),

    ("the command is a plain read, not an INDEX read",
     "without CYCLE_INDEX the offset byte is never sent, so block 1 is "
     "read from wherever the monitor's pointer happens to be -- "
     "usually block 0 again",
     """    gmbus_wr(dev, tr, G1, G1_SW_RDY | G1_CYCLE_INDEX | G1_CYCLE_WAIT |""",
     """    gmbus_wr(dev, tr, G1, G1_SW_RDY | G1_CYCLE_WAIT |"""),

    ("the address is not shifted into bits 7:1",
     "0x50 in bits 6:0 is 0x28 on the wire -- not the EDID address",
     """                          ((uint32_t)addr << G1_ADDR_SHIFT) | G1_READ);""",
     """                          (uint32_t)addr | G1_READ);"""),

    ("only three bytes are taken from each GMBUS3 read",
     "the register carries four; dropping one shifts every byte after "
     "it and the checksum fails -- or worse, a block happens to sum",
     """        for(k = 0; k < 4 && got < len; k++){""",
     """        for(k = 0; k < 3 && got < len; k++){"""),

    ("block 1 is never read",
     "a 144 Hz mode lives in the CTA extension; without it the screen "
     "offers the monitor's slowest mode only",
     """    if(buf[126] >= 1){""",
     """    if(0){"""),

    ("pin 3 is accepted",
     "ADL-P maps DDC to pins 1, 2 and 9-12. Pin 3 is DDI C, which this "
     "chip does not have; a transfer there clocks wires to nothing",
     """    return pin == 1 || pin == 2 || (pin >= 9 && pin <= 12);""",
     """    return pin == 1 || pin == 2 || pin == 3 || (pin >= 9 && pin <= 12);"""),

    # the guard itself is the build check's job (check_gfx_readonly
    # refuses a gmbus_wr() without it) AND the suite's -- both:
    ("gmbus_wr() loses its upper bound",
     "the guard is the whole reason a write in this file cannot reach "
     "a pipe or a power well; without its top half, 0x45404 and "
     "0x70180 are fair game",
     """    if(off < GMBUS_REG_FIRST || off > GMBUS_REG_LAST || (off & 3u)){""",
     """    if(off < GMBUS_REG_FIRST || (off & 3u)){"""),

    # ── the parse ─────────────────────────────────────────────────
    ("the horizontal sync offset loses its top two bits",
     "byte 11 bits 7:6 are bits 9:8 of the offset; a wide mode's sync "
     "lands in the wrong place and the monitor sees no signal",
     """    m->hso     = (uint16_t)(d[8] | ((d[11] & 0xC0) << 2));""",
     """    m->hso     = (uint16_t)(d[8]);""", PARSER),

    ("the vertical sync offset takes the low nibble",
     "byte 10 holds offset in 7:4 and width in 3:0; swapping them is "
     "a frame that never syncs",
     """    m->vso     = (uint16_t)((d[10] >> 4) | ((d[11] & 0x0C) << 2));""",
     """    m->vso     = (uint16_t)((d[10] & 0x0F) | ((d[11] & 0x0C) << 2));""", PARSER),

    ("hsync polarity is read from the vsync bit",
     "a negative-sync mode (1024x768) driven positive: some monitors "
     "cope, some show nothing",
     """        m->hpos = (d[17] >> 1) & 1u;""",
     """        m->hpos = (d[17] >> 2) & 1u;""", PARSER),

    ("the manufacturer ID is read little-endian",
     "'GSM' becomes nonsense, and the screen's one plain-language "
     "confirmation of WHICH monitor answered is wrong",
     """    e->mfg[0] = (char)('A' - 1 + ((id >> 10) & 0x1F));""",
     """    e->mfg[0] = (char)('A' - 1 + ((id >> 11) & 0x1F));""", PARSER),

    ("the year is not offset from 1990",
     "a monitor made in 2024 reported as made in 34",
     """    e->year    = (uint16_t)(1990u + buf[17]);""",
     """    e->year    = (uint16_t)(buf[17]);""", PARSER),

    ("a bad extension checksum is accepted",
     "a block corrupted on the wire would offer modes made of noise",
     """        if(!sum_ok(x)){ e->bad_ext++; continue; }""",
     """        (void)sum_ok;""", PARSER),

    ("blocks past the announced count are parsed",
     "byte 126 is the count; anything after it in the buffer is not "
     "this monitor's",
     """    if(nblocks > 1u + e->extensions) nblocks = 1u + e->extensions;""",
     """    /* announced count ignored */""", PARSER),

    ("the mode table is not capped",
     "four blocks can carry 22 timings and there are twelve slots",
     """    if(e->nmodes >= EDID_MAX_MODES) return;""",
     """    /* no cap */""", PARSER),

    ("repeated timings are kept",
     "CTA blocks routinely repeat the preferred mode; listed twice, "
     "and the cap fills with copies",
     """        if(same_timing(&e->modes[i], m)) return;""",
     """        (void)same_timing;""", PARSER),

    ("the name runs past its terminator",
     "the 0x0A ends the string; past it is padding, printed as part of "
     "the name",
     """        if(c == 0x0A || c == 0) break;""",
     """        if(c == 0) break;""", PARSER),

    ("the EDID 1.4 range offset is ignored",
     "a 240 Hz monitor reports a 285 Hz top as 30, and a mode the "
     "monitor accepts looks out of range",
     """            e->vmax = (uint16_t)(d[6] + ((d[4] & 0x02) ? 255u : 0u));""",
     """            e->vmax = (uint16_t)(d[6]);""", PARSER),

    ("refresh divides in the wrong order",
     "frames before lines truncates the line rate to whole kHz and "
     "60.00 Hz reads as 59.xx",
     """    lines = pclk_khz * 1000u / ht;""",
     """    lines = (pclk_khz / ht) * 1000u;""", PARSER),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the driver changed",
    """    case GMBUS_OK:       return "ok";""",
    """    /* nothing went wrong */
    case GMBUS_OK:       return "ok";""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "edid", SRC, TEST, CC, BUGS, HARMLESS, timeout=180,
        files=(SRC, PARSER, TEST),
        checks=(["python3", "tools/check_gfx_readonly.py"],),
        preamble="Proving tests/test_edid.c can fail.\n"
                 "drivers/xegmbus.c is the first code in the display series\n"
                 "that writes, and nothing but the HP can run it for real."))
