#!/usr/bin/env python3
"""check_gfx_suite.py -- prove tests/test_gfx.c can fail.

drivers/xedisp.c HAS NEVER SEEN AN IRIS XE, and no emulator has one:
QEMU's machines have no Intel display engine at all. Between this code
and the HP there is the suite and nothing else, so the suite's worth is
exactly the set of mistakes it can be shown to notice.

── WHAT THE MISTAKES ARE ABOUT ────────────────────────────────────────

Three families, each of which would put a wrong photograph in front of
the person deciding what 0.9.2 writes:

  the gates    a register read in a power well that is off returns
               zero. Zero is a plausible value for almost everything
               here -- "plane off", "PLL off", "nothing plugged in" --
               so a gate that is wrong does not look wrong. It looks
               like a quieter machine. The model counts dark reads and
               sets the engine's own unclaimed flag, and the suite
               checks the count.
  the window   kernel/highfb.c has eight windows above 4 GB for the
               life of the machine. A command that maps on every run
               takes one each time.
  the tables   the VBT says which port the HDMI socket is wired to,
               through four renumberings (DVO port, DDC pin, AUX
               channel, and display 13 moving Type-C to F..I). An
               off-by-one there aims 0.9.1 at the wrong connector.

── THE FIXTURE'S OWN FIRST BUG ───────────────────────────────────────

The first version counted reads per 4 KB page. DDI A and B (0x64000,
0x64100) share a page with the Type-C buffers (0x64300+), so "no
Type-C buffer was read" failed on reads of A and B -- the suite could
not tell the registers on either side of a gate apart. It counts per
register now. That is in the suite's comments too, where the next
person will see it.

── AND WHAT THIS HARNESS FOUND ON ITS FIRST RUN ──────────────────────

The suite passed 172 of 172 before this ran, and this reported FOUR
misses out of 31. Three were holes in the suite, one in the harness:

  "detect off" was asserted with has(), which any row satisfies -- and
      every Type-C row on the HP says "detect off". The suite could not
      fail on DDI B. Per-port checks read the port's own line now.
  the MIPI block was zero-filled, so a walk using the wrong size crept
      through zero-length "blocks" three bytes at a time and landed on
      block 2 regardless. The payload is junk now.
  no child was DVI-only, so "DVI is called HDMI" had nothing to break.
  the "line too long" injection made a 77-character line, which fits.
      That was the harness's mistake, not the suite's; it is 90 now.

Same shapes as every release before it: a check that cannot fail, and
a fixture that cannot produce the case.

Run:  python3 tools/check_gfx_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "drivers/xedisp.c"
TEST = "tests/test_gfx.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    # ── the gates ──────────────────────────────────────────────────
    ("a pipe's planes are read whenever the core is awake",
     "PW_B, PW_C and PW_D are off on the HP as the firmware leaves it. "
     "Reading their planes returns zero, which the report would print "
     "as 'plane off' -- true by accident, and the engine's unclaimed "
     "flag set behind it",
     """        if(!r->core || !well_on(r, pipe_well(p))) continue;""",
     """        if(!r->core) continue;"""),

    ("transcoders B-D are gated on the core, not their own wells",
     "transcoder A is in PW_1; B, C and D are in PW_B, PW_C, PW_D. "
     "Treating them like A reads three transcoders in the dark",
     """    return t == 0 ? r->core : well_on(r, PW_A + t);""",
     """    return r->core;"""),

    ("the Type-C buffers are read without PW_2",
     "Type-C lanes are in PW_2, which the firmware leaves off when "
     "nothing is plugged into a USB-C port",
     """            if(!well_on(r, PW_2)) continue;   /* Type-C lanes: PW_2 */""",
     """            /* PW_2 not checked */"""),

    ("PIPESRC is read on the transcoder's gate",
     "PIPESRC sits at a transcoder address but is a PIPE register: "
     "transcoder A is powered by PW_1, PIPESRC A by PW_A",
     """        if(well_on(r, pipe_well(t)))
            tt->src = rd(dev, r, R_PIPESRC(t));""",
     """        tt->src = rd(dev, r, R_PIPESRC(t));"""),

    ("a well's REQUEST bit is taken for its STATE",
     "each well has two bits: request (2*idx+1) and state (2*idx). A "
     "well the firmware asked for is not necessarily on, and one a "
     "driver did not ask for may be -- the state bit is the hardware",
     """#define PW_STATE(idx)   (1u << ((idx) * 2))""",
     """#define PW_STATE(idx)   (2u << ((idx) * 2))"""),

    ("the 2 MB span is not enforced",
     "the GTT is above the registers in BAR0, and the window maps "
     "exactly 2 MB; a read past it is somebody else's memory",
     """    if(off >= GFX_MMIO_SPAN || (off & 3u)){ r->out_of_span++; return 0; }""",
     """    if(0){ r->out_of_span++; return 0; }"""),

    ("the unclaimed flag is not read first",
     "without the BEFORE value, a flag the firmware left set looks "
     "exactly like a gate this code got wrong",
     """    r->fpga_before = rd(dev, r, R_FPGA_DBG);""",
     """    r->fpga_before = 0;"""),

    ("a flag already set is blamed on this pass",
     "the firmware may leave FPGA_DBG set; clearing it is a write. "
     "Calling that an unclaimed read of ours sends the next person "
     "hunting for a gate that is fine",
     """    if(!(r->fpga_before & FPGA_NOCLAIM) && (r->fpga_after & FPGA_NOCLAIM)){""",
     """    if(r->fpga_after & FPGA_NOCLAIM){"""),

    # ── opening, and the window ────────────────────────────────────
    ("an unknown device ID is read anyway",
     "these offsets are display 13's. On Tiger Lake or Meteor Lake a "
     "register here can mean something else, and the report would "
     "present it with complete confidence",
     """    if(!known_id(d->device)){ dev->why = GFX_EUNKNOWN; return dev->why; }""",
     """    (void)known_id;"""),

    ("memory decoding off is not noticed",
     "with bit 1 of the command register clear every read is master "
     "abort -- all ones -- and turning it on is a write",
     """    if(!(dev->pcicmd & 0x0002u)){ dev->why = GFX_EMEMOFF; return dev->why; }""",
     """    /* memory decode not checked */"""),

    ("BAR0 is mapped afresh on every run",
     "highfb has eight windows for the life of the machine and the "
     "framebuffer, xHCI and I2C already hold some. The fifth `gfx` "
     "would fail with EMAP and the sixth would never get there",
     """    if(g_map_virt && g_map_hi == dev->bar0_hi && g_map_lo == dev->bar0_lo){""",
     """    if(0){"""),

    ("the whole 16 MB BAR is mapped, GTT and all",
     "a window over the GTT is a window over the page tables the "
     "panel is being scanned out through",
     """        if(!highfb_map_io(dev->bar0_hi, dev->bar0_lo, GFX_MMIO_SPAN, &virt)""",
     """        if(!highfb_map_io(dev->bar0_hi, dev->bar0_lo, 0x1000000u, &virt)"""),

    ("a dead function is not recognised",
     "a GPU in D3 reads all ones, and every decoded field would be "
     "'on', 'locked', 'enabled' -- the most confident wrong report "
     "this command could produce",
     """    if(fuse == 0xFFFFFFFFu && pw == 0xFFFFFFFFu){""",
     """    if(0){"""),

    # ── decoding ───────────────────────────────────────────────────
    ("the DDI select in TRANS_DDI_FUNC_CTL is not de-biased",
     "on display 12+ the field holds port + 1, with 0 meaning none; "
     "reading it raw says the panel is on DDI B",
     """    return v ? (int)v - 1 : GFX_PORT_NONE;""",
     """    return v ? (int)v : GFX_PORT_NONE;"""),

    ("the CD2X divider 1.5 is taken as 2",
     "a CDCLK reported a quarter low is a 0.9.2 that picks the wrong "
     "minimum for a mode the clock can actually drive",
     """    case 1: return vco / 3u;                   /* / 2 / 1.5           */""",
     """    case 1: return vco / 4u;                   /* / 2 / 1.5           */"""),

    ("the 19.2 MHz reference is taken as 38.4",
     "DSSM's field picks one of three crystals; the wrong one doubles "
     "every clock derived from it",
     """    case 1: ref = 19200; break;""",
     """    case 1: ref = 38400; break;"""),

    ("hot-plug reads the neighbouring port's live bit",
     "SDEISR has one bit per port from 16. Off by one, the HDMI port "
     "reports the panel's connector, which is always present",
     """        int live = (r->sdeisr >> (16 + port)) & 1;""",
     """        int live = (r->sdeisr >> (15 + port)) & 1;"""),

    ("a disabled detector is reported as 'nothing'",
     "the firmware may not enable hot-plug detection. 'nothing' then "
     "means 'not looking', and a person would unplug and replug a "
     "cable that was fine",
     """        if(!en)       Ls(l, "detect off");""",
     """        if(0)         Ls(l, "detect off");"""),

    ("the framebuffer check compares low halves only",
     "two addresses 4 GB apart agree in their low 32 bits",
     """        if(fb_w && dev->bar2_hi == fb_hi &&""",
     """        if(fb_w &&"""),

    ("a report line runs past the screen",
     "on the HP a cut line is a register value with its last digits "
     "missing, in a photograph that is the only evidence",
     """        say(GFX_T_TEXT, "  Pipe B and transcoder B are free for the monitor.");""",
     """        say(GFX_T_TEXT, "  Pipe B and transcoder B are free for the external monitor, and so are DPLL1 and PW_B.");"""),

    # ── the VBT ────────────────────────────────────────────────────
    ("the MIPI sequence block is walked by its 16-bit size",
     "from version 3 that block carries a 32-bit size; the 16-bit "
     "field is not its length, and walking by it lands mid-block and "
     "never finds the child devices (intel_bios.c _get_blocksize)",
     """        if(id == 53 && idx + 8u <= bdb_size && bdb[idx + 3] >= 3)""",
     """        if(0)"""),

    ("display 13's Type-C renumbering is off by one",
     "the VBT calls TC1 'F' on display 13. Map it to TC2 and 0.9.1 "
     "talks to an empty socket",
     """    case 14: case 13: return GFX_PORT_TC1;       /* HDMIF, DPF        */""",
     """    case 14: case 13: return GFX_PORT_TC1 + 1;   /* HDMIF, DPF        */"""),

    ("the DDC pin is taken as the GMBUS pin",
     "ADL-P remaps them: VBT DDC 3 is GMBUS pin 9. Unmapped, 0.9.1 "
     "would clock an I2C read out of a pin with nothing on it",
     """    case 3: return 9;  case 4: return 10;          /* TC1, TC2       */""",
     """    case 3: return 3;  case 4: return 4;           /* TC1, TC2       */"""),

    ("the LSPCON bit is read on any BDB version",
     "before 192 that bit is part of another field. An LSPCON wrongly "
     "reported sends 0.9.1 to AUX on a port that speaks I2C",
     """        k->lspcon  = (out->bdb_version >= 192) && (k->flags1 & 0x04u);""",
     """        k->lspcon  = (k->flags1 & 0x04u) != 0;"""),

    ("DVI-only is called HDMI",
     "a connector with NOT_HDMI set cannot carry HDMI infoframes or "
     "audio; calling it HDMI promises things the wire cannot do",
     """        k->is_hdmi = (type & DT_TMDS) && !(type & DT_NOT_HDMI);""",
     """        k->is_hdmi = (type & DT_TMDS) != 0;"""),

    ("empty child slots are kept",
     "a VBT has fixed slots and most are zero; kept, they crowd the "
     "eight-entry table and push real connectors out",
     """        if(!type) continue;                     /* an empty slot   */""",
     """        /* empty slots kept */"""),

    ("the child table is not capped",
     "twelve children into an eight-entry array is a write past it",
     """    for(i = 0; i < n && out->nchild < GFX_VBT_MAX_CHILD; i++){""",
     """    for(i = 0; i < n; i++){"""),

    ("a 2.1 RVDA is taken as absolute",
     "from OpRegion 2.1 RVDA is relative to the OpRegion; as an "
     "absolute address it points into the first pages of RAM",
     """            int      rel = (major > 2 || minor >= 1);""",
     """            int      rel = 0;"""),

    ("an RVDA pointing inside the OpRegion is believed",
     "i915 warns on exactly this; it is a table that is lying, and "
     "following it parses a mailbox as a VBT",
     """            if(!rel || (at > rv_lo && rv_lo >= OPR_SIZE)){""",
     """            if(!rel || at > rv_lo){"""),

    ("an RVDA above 4 GB is truncated",
     "the low half of a 64-bit address is an unrelated address",
     """        if(rv_lo && !rv_hi && rvds && rvds <= VBT_MAX){""",
     """        if(rv_lo && rvds && rvds <= VBT_MAX){"""),

    ("the mailbox 4 length ignores the ASLE-ext mailbox",
     "with mailbox 5 present the VBT area ends at 0x1C00, and parsing "
     "past it reads mailbox 5 as table",
     """    len = ((mboxes & MBOX_ASLE_EXT) ? OPR_ASLE_EXT : OPR_SIZE) - OPR_VBT_OFF;""",
     """    len = OPR_SIZE - OPR_VBT_OFF;"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the driver changed",
    """    case GFX_OK:       return "ok";""",
    """    /* the one that means nothing is wrong */
    case GFX_OK:       return "ok";""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "gfx", SRC, TEST, CC + ["-fsanitize=address,undefined"], BUGS,
        HARMLESS, timeout=120,
        preamble="Proving tests/test_gfx.c can fail.\n"
                 "drivers/xedisp.c has never seen an Iris Xe, and no\n"
                 "emulator has one. This suite and the HP's photographs\n"
                 "are the only two things between it and real silicon."))
