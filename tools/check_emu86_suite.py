#!/usr/bin/env python3
"""check_emu86_suite.py -- prove tests/test_emu86.c can fail.  v0.9.7.

Retro PC's suite passed 83 of 83 on its first run, which in this tree
is the least trustworthy result there is: it was written the same day
as the code, by the same hand.

TWO OUTSIDE REFERENCES ALREADY JUDGE MOST OF IT. tools/check_emu86_sst.py
runs the 8086 core against 2.5 million instructions recorded from a
real 8088, and K-DOS's screens and disk writes in the suite are the
ones QEMU with a real BIOS produced for the same keys. So the defects
planted here are aimed at what neither reference can see:

  the 80186 and 386 additions   the 8088 suite has no PUSHA, no ENTER,
                                no 0F group, no FS -- the very things
                                K-DOS needed that an 8086 lacks
  the refusals                  a 66h prefix half-understood, a BIOS
                                trap forged from guest memory, a write
                                into the ROM: each one a way for an old
                                program to do something it should not
  the BIOS                      the STI in INT 16h, the head in a CHS
                                address, the EOI at the end of IRQ 1 --
                                the details a real BIOS gets right and
                                a guest depends on without knowing

── WHAT IT FOUND ON ITS FIRST RUN ────────────────────────────────────

26 of 27. "an IRQ is delivered while the same one is in service" was
MISSED: the suite raised a second IRQ 0 and checked that it was held
PENDING -- which it is, with or without the 8259's rule -- and never
ran the CPU to see whether it was DELIVERED. The check that catches it
now runs a spinning guest, raises the line three times without an EOI,
counts the handler's entries (one), sends the EOI and counts again
(two). The wire, not the outcome, again.

Run:  python3 tools/check_emu86_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "emu/cpu86.c"
BIOS = "emu/pcbios.c"
TEST = "tests/test_emu86.c"
CC = ["gcc", "-std=gnu99", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "emu/cpu86.c", "emu/pcbios.c"]

BUGS = [
    ("PUSHA pushes the SP it has reached, not the one before it",
     "the 80186 pushes the ORIGINAL SP in the fourth slot; code that "
     "reads its own frame after PUSHA gets an address 6 bytes off",
     "        push(e, sp); push(e, BP); push(e, SI); push(e, DI);",
     "        push(e, SP); push(e, BP); push(e, SI); push(e, DI);"),

    ("POPA loads SP from the stack",
     "POPA discards that slot; loading it moves the stack under the "
     "program's feet -- the next RET returns somewhere else",
     "        DI = pop(e); SI = pop(e); BP = pop(e); (void)pop(e);",
     "        DI = pop(e); SI = pop(e); BP = pop(e); SP = pop(e);"),

    ("ENTER does not reserve the locals",
     "the frame is set up and the space for the locals is not taken, "
     "so the next PUSH lands on the function's own variables",
     "        BP = frame; SP -= size;",
     "        BP = frame;"),

    ("a shift by immediate takes its count from CL",
     "C0/C1 carry the count in the instruction; using CL turns every "
     "SHL AX,4 into a shift by whatever CL held",
     "        if(op <= 0xC1) n = fetch8(e);",
     "        if(op <= 0xC1){ (void)fetch8(e); n = CX & 0xFF; }"),

    ("0F 8x reads an 8-bit displacement",
     "the exact instruction K-DOS needed: a near Jcc with the wrong "
     "length jumps into the middle of the next instruction",
     "        int16_t d = (int16_t)fetch16(e);\n        if(cond(e, op & 0x0F)) e->ip = (uint16_t)(e->ip + d);\n        return EMU_BUDGET;",
     "        int16_t d = (int8_t)fetch8(e);\n        if(cond(e, op & 0x0F)) e->ip = (uint16_t)(e->ip + d);\n        return EMU_BUDGET;"),

    ("SETcc stores the opposite",
     "every SETcc a compiler emits for a comparison gives the other answer",
     "        rm_set(e, &m, 8, (uint32_t)cond(e, op & 0x0F));",
     "        rm_set(e, &m, 8, (uint32_t)!cond(e, op & 0x0F));"),

    ("MOVSX zero-extends",
     "signed bytes become large positive numbers: -1 reads as 255",
     "(uint16_t)(int16_t)(int8_t)v;",
     "(uint16_t)(uint8_t)v;"),

    ("BT with a register offset stays inside the first word",
     "a bitmap indexed by a register past bit 15 tests and sets the "
     "wrong bit, quietly",
     "    if(from_reg) off = (uint16_t)(off + 2 * (bit >> 4));",
     "    (void)from_reg;"),

    ("BSR scans from the bottom",
     "BSR finds the highest set bit; from the bottom it is BSF again",
     "op == 0xBD, &i",
     "op == 0xBC, &i"),

    ("SHRD shifts the wrong way",
     "the double shifts are how 16-bit code moves values across a "
     "register pair; one direction wrong corrupts both halves",
     "            r = (w >> n) & 0xFFFF;",
     "            r = (w << n) & 0xFFFF;"),

    ("LFS loads DS",
     "the far pointer lands in the wrong segment register, and every "
     "access the program then makes through DS goes to the wrong place",
     "S_SS : op == 0xB4 ? S_FS : S_GS",
     "S_SS : op == 0xB4 ? S_DS : S_GS"),

    ("the FS: override is ignored",
     "a prefix that changes nothing reads and writes through DS",
     "        if(op == 0x64){ e->seg = S_FS; continue; }",
     "        if(op == 0x64){ continue; }"),

    ("a 66h prefix is skipped instead of refused",
     "the instruction after it runs with 16-bit operands and the wrong "
     "length: the guest is corrupted and nothing says so",
     '    case 0x66: return fault(e, "a 32-bit register (the 386\'s 66h prefix)");',
     "    case 0x66: break;"),

    ("the BIOS trap is honoured from any segment",
     "three bytes in guest memory would then reach the host. The trap "
     "is only the BIOS's because only segment F000 can execute it",
     "        if(CS == 0xF000 && rd8(e, CS, e->ip) == 0xFF){",
     "        if(rd8(e, CS, e->ip) == 0xFF){"),

    ("the BIOS ROM is writable",
     "a program that scribbles on F000 rewrites the vectors' stubs and "
     "the next INT runs whatever it wrote",
     "    if(a >= EMU86_ROM) return;           /* the BIOS ROM is read-only */",
     "    /* (no ROM) */"),

    ("an IRQ is delivered while the same one is in service",
     "a timer handler interrupted by its own next tick re-enters itself; "
     "a real 8259 holds the line until the EOI",
     "        if(e->irq_service & b) return 0;          /* higher one busy */",
     "        /* (no in-service check) */"),

    ("a divide error returns to the DIV itself",
     "the 286's rule, not the 80186's: a handler that just returns "
     "divides by zero again, for ever",
     "               simply returns does not loop on the divide for ever. */\n            do_int(e, 0);",
     "               simply returns does not loop on the divide for ever. */\n            e->ip = e->op_ip; do_int(e, 0);"),

    ("PUSHF clears bits 12-15",
     "the 386's flags: software that asks which CPU it has then decides "
     "it is on a 286 or later and may use instructions this one refuses",
     "    case 0x9C: push(e, (uint16_t)(e->flags | F_FIXED)); break;",
     "    case 0x9C: push(e, (uint16_t)((e->flags | 2) & 0x0FFF)); break;"),

    ("INT 16h without its STI",
     "the wait for a key is satisfied by IRQ 1, and the INT has just "
     "cleared IF: a program waiting for a key waits for ever",
     "        if(v == 0x16) s[n++] = 0xFB;                   /* STI: see the header */",
     "        /* (no STI) */",
     BIOS),

    ("INT 13h ignores the head",
     "K-DOS's save goes to head 1; without the head it overwrites "
     "sector 10 of head 0 instead",
     "        lba = ((uint32_t)cyl * (uint32_t)H + (uint32_t)head) * (uint32_t)S + (uint32_t)(sec - 1);",
     "        lba = ((uint32_t)cyl * (uint32_t)H) * (uint32_t)S + (uint32_t)(sec - 1);",
     BIOS),

    ("drive 80h is the floppy",
     "the pretend hard disk was the point: a guest's writes to 80h "
     "would land on its own boot image",
     "    if(dl == 0x80 && p->hd){ *size = p->hd_size; *c = p->hd_c; *h = p->hd_h; *s = p->hd_s; return p->hd; }",
     "    if(dl == 0x80 && p->hd){ *size = p->fd_size; *c = p->fd_c; *h = p->fd_h; *s = p->fd_s; return p->fd; }",
     BIOS),

    ("teletype does not scroll",
     "the cursor walks off the bottom of the screen and writes into "
     "the next page of video memory, invisibly",
     "    if(y >= R){\n        /* the new line takes the attribute of the cell at the cursor */",
     "    if(0){\n        /* the new line takes the attribute of the cell at the cursor */",
     BIOS),

    ("the timer does not count",
     "INT 1Ah AH=0 and every program timing itself off the BDA count "
     "sees the clock stopped",
     "    if(++t >= TICKS_PER_DAY){ t = 0; wb8(p, BDA_MIDNIGHT, 1); }",
     "    if(t >= TICKS_PER_DAY){ t = 0; wb8(p, BDA_MIDNIGHT, 1); }",
     BIOS),

    ("the clock comes back in binary, not BCD",
     "INT 1Ah AH=2 is BCD by definition; K-DOS's time prints the "
     "nibbles as digits",
     "        e->r[R_CX] = (uint16_t)((bcd(h) << 8) | bcd(mi));",
     "        e->r[R_CX] = (uint16_t)((h << 8) | mi);",
     BIOS),

    ("break codes go into the key buffer",
     "every key typed arrives twice, the second time as its release",
     "    if(!(sc & 0x80)) kbuf_put(p, (uint16_t)((sc << 8) | p->key_ascii));",
     "    kbuf_put(p, (uint16_t)((sc << 8) | p->key_ascii));",
     BIOS),

    ("the BIOS's INT 9 sends no EOI",
     "IRQ 1 stays in service after the first key and no second key "
     "ever arrives",
     "    if(!(sc & 0x80)) kbuf_put(p, (uint16_t)((sc << 8) | p->key_ascii));\n    p->obf = 0;\n    emu86_eoi(&p->cpu);",
     "    if(!(sc & 0x80)) kbuf_put(p, (uint16_t)((sc << 8) | p->key_ascii));\n    p->obf = 0;",
     BIOS),

    ("HavenDOS's Enter reaches the old PC as Ctrl+J",
     "the bug harness/retro_boot.py found on the first real boot: K-DOS "
     "got a line feed for every Enter and its login never ended",
     "    if(key == '\\n') key = '\\r';",
     "    /* (Enter as is) */",
     BIOS),

    ("a disk with no boot signature boots",
     "a blank or damaged image runs whatever zeros decode to, instead "
     "of saying why it will not start",
     "    if(p->fd[510] != 0x55 || p->fd[511] != 0xAA){",
     "    if(0){",
     BIOS),
]

HARMLESS = (
    "the segment registers are cleared in a different order",
    "three assignments of the same value to three different registers "
    "in another order change nothing at all",
    "    e->sr[S_DS] = e->sr[S_ES] = e->sr[S_SS] = 0;",
    "    e->sr[S_SS] = e->sr[S_ES] = e->sr[S_DS] = 0;",
    BIOS)

if __name__ == "__main__":
    sys.exit(inject.run("emu86", SRC, TEST, CC, BUGS, HARMLESS,
                        timeout=60, files=(SRC, BIOS, TEST)))
