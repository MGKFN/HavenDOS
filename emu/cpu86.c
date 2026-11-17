/* ════════════════════════════════════════════════════════════════════
   emu/cpu86.c -- the 8086/80186 interpreter behind Retro PC.  v0.9.7.

   include/emu86.h says what this is for and why it is an interpreter.
   This file is the whole instruction set of an 80186 in real mode --
   the 8086's, plus PUSHA/POPA, PUSH imm, IMUL imm, shifts by an
   immediate, ENTER/LEAVE, BOUND and INS/OUTS -- and the 386's
   additions that work on 16-bit registers: the 0F group (near Jcc,
   SETcc, MOVZX/MOVSX, BT/BTS/BTR/BTC, BSF/BSR, SHLD/SHRD, IMUL r16,
   LSS/LFS/LGS) and the FS and GS segment registers.

   WHY THE 386 PART. K-DOS Beta 1.1b was assembled for a 386: NASM
   turned its long conditional jumps into the 386's 0F 8x form, and
   the first version of this file -- a pure 80186 -- stopped on them
   the moment the shell ran a command. It also uses PUSHA and POPA,
   so an 8086 alone would not have got as far as the login.

   What it deliberately is NOT:
     - a 386 with 32-bit registers. The operand- and address-size
       prefixes (66h, 67h), protected-mode and system instructions
       stop it with EMU_FAULT and a sentence saying which instruction,
       at which address. Guessing would be worse: an instruction half
       understood corrupts the guest quietly.
     - an 8087. The ESC opcodes consume their operand and do nothing,
       which is what a PC with an empty coprocessor socket does -- and
       the standard FNINIT/FNSTSW probe then reads "no FPU", correctly.
     - cycle-accurate. Nothing old enough to care about cycles is on
       the list of things this exists to run.

   No floating point and no 64-bit division anywhere: the kernel links
   no libgcc. 32-by-16 division is native on the host.
   ════════════════════════════════════════════════════════════════════ */
#include "emu86.h"

#define AX e->r[R_AX]
#define CX e->r[R_CX]
#define DX e->r[R_DX]
#define BX e->r[R_BX]
#define SP e->r[R_SP]
#define BP e->r[R_BP]
#define SI e->r[R_SI]
#define DI e->r[R_DI]
#define ES e->sr[S_ES]
#define CS e->sr[S_CS]
#define SS e->sr[S_SS]
#define DS e->sr[S_DS]

/* ── memory ───────────────────────────────────────────────────────── */
uint8_t emu86_rd8(const emu86_t *e, uint32_t a){
    return e->mem[a & (EMU86_MEM - 1)];
}
uint16_t emu86_rd16(const emu86_t *e, uint32_t a){
    /* byte by byte: a word at 0xFFFFF wraps to 0, as on an 8086 */
    return (uint16_t)(emu86_rd8(e, a) | (emu86_rd8(e, a + 1) << 8));
}
void emu86_wr8(emu86_t *e, uint32_t a, uint8_t v){
    a &= EMU86_MEM - 1;
    if(a >= EMU86_ROM) return;           /* the BIOS ROM is read-only */
    e->mem[a] = v;
}
void emu86_wr16(emu86_t *e, uint32_t a, uint16_t v){
    emu86_wr8(e, a, (uint8_t)v);
    emu86_wr8(e, a + 1, (uint8_t)(v >> 8));
}

static uint8_t  rd8 (emu86_t *e, uint16_t s, uint16_t o){ return emu86_rd8(e, emu86_lin(s, o)); }
static uint16_t rd16(emu86_t *e, uint16_t s, uint16_t o){
    /* an offset of 0xFFFF wraps within the segment for the high byte */
    return (uint16_t)(rd8(e, s, o) | (rd8(e, s, (uint16_t)(o + 1)) << 8));
}
static void wr8 (emu86_t *e, uint16_t s, uint16_t o, uint8_t v){ emu86_wr8(e, emu86_lin(s, o), v); }
static void wr16(emu86_t *e, uint16_t s, uint16_t o, uint16_t v){
    wr8(e, s, o, (uint8_t)v); wr8(e, s, (uint16_t)(o + 1), (uint8_t)(v >> 8));
}

static uint8_t  fetch8 (emu86_t *e){ uint8_t v = rd8(e, CS, e->ip); e->ip++; return v; }
static uint16_t fetch16(emu86_t *e){ uint16_t v = rd16(e, CS, e->ip); e->ip += 2; return v; }

static void push(emu86_t *e, uint16_t v){ SP -= 2; wr16(e, SS, SP, v); }
static uint16_t pop(emu86_t *e){ uint16_t v = rd16(e, SS, SP); SP += 2; return v; }

void emu86_ret_flag(emu86_t *e, uint16_t flag, int on){
    uint16_t f = rd16(e, SS, (uint16_t)(SP + 4));
    f = on ? (uint16_t)(f | flag) : (uint16_t)(f & ~flag);
    wr16(e, SS, (uint16_t)(SP + 4), f);
}

/* ── flags ────────────────────────────────────────────────────────── */
static int parity(uint8_t v){
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return !(v & 1);
}
static void setf(emu86_t *e, uint16_t f, int on){
    if(on) e->flags |= f; else e->flags &= (uint16_t)~f;
}
static int getf(const emu86_t *e, uint16_t f){ return (e->flags & f) != 0; }

/* SF ZF PF from a result of width w (8 or 16) */
static void szp(emu86_t *e, uint32_t r, int w){
    uint32_t m = w == 8 ? 0xFFu : 0xFFFFu;
    setf(e, F_ZF, (r & m) == 0);
    setf(e, F_SF, (r & (w == 8 ? 0x80u : 0x8000u)) != 0);
    setf(e, F_PF, parity((uint8_t)r));
}

/* The eight ALU operations, in encoding order: ADD OR ADC SBB AND SUB
   XOR CMP. Returns the result; CMP's is discarded by the caller. */
static uint32_t alu(emu86_t *e, int op, uint32_t a, uint32_t b, int w){
    uint32_t m = w == 8 ? 0xFFu : 0xFFFFu, sb = w == 8 ? 0x80u : 0x8000u;
    uint32_t r, c;
    a &= m; b &= m;
    switch(op){
    case 0: case 2:                                    /* ADD, ADC */
        c = (op == 2 && getf(e, F_CF)) ? 1 : 0;
        r = a + b + c;
        setf(e, F_CF, r > m);
        setf(e, F_AF, ((a ^ b ^ r) & 0x10) != 0);
        setf(e, F_OF, ((r ^ a) & (r ^ b) & sb) != 0);
        break;
    case 3: case 5: case 7:                            /* SBB, SUB, CMP */
        c = (op == 3 && getf(e, F_CF)) ? 1 : 0;
        r = a - b - c;
        setf(e, F_CF, a < b + c);
        setf(e, F_AF, ((a ^ b ^ r) & 0x10) != 0);
        setf(e, F_OF, ((a ^ b) & (a ^ r) & sb) != 0);
        break;
    default:                                           /* OR AND XOR */
        r = op == 1 ? (a | b) : op == 4 ? (a & b) : (a ^ b);
        setf(e, F_CF, 0); setf(e, F_OF, 0); setf(e, F_AF, 0);
        break;
    }
    r &= m;
    szp(e, r, w);
    return r;
}

static uint32_t inc_dec(emu86_t *e, uint32_t a, int dec, int w){
    uint32_t m = w == 8 ? 0xFFu : 0xFFFFu, sb = w == 8 ? 0x80u : 0x8000u;
    uint32_t r = (dec ? a - 1 : a + 1) & m;            /* CF untouched */
    setf(e, F_AF, ((a ^ 1 ^ r) & 0x10) != 0);
    setf(e, F_OF, dec ? (a & m) == sb : r == sb);
    szp(e, r, w);
    return r;
}

/* ── ModRM ────────────────────────────────────────────────────────── */
typedef struct {
    int      mod, reg, rm;
    uint16_t seg, off;       /* the memory operand, when mod != 3 */
} modrm_t;

static void decode_modrm(emu86_t *e, modrm_t *m){
    uint8_t b = fetch8(e);
    uint16_t ea = 0;
    int bpbase = 0;
    m->mod = b >> 6; m->reg = (b >> 3) & 7; m->rm = b & 7;
    if(m->mod == 3) return;
    switch(m->rm){
    case 0: ea = (uint16_t)(BX + SI); break;
    case 1: ea = (uint16_t)(BX + DI); break;
    case 2: ea = (uint16_t)(BP + SI); bpbase = 1; break;
    case 3: ea = (uint16_t)(BP + DI); bpbase = 1; break;
    case 4: ea = SI; break;
    case 5: ea = DI; break;
    case 6:
        if(m->mod == 0) ea = fetch16(e);
        else { ea = BP; bpbase = 1; }
        break;
    case 7: ea = BX; break;
    }
    if(m->mod == 1) ea = (uint16_t)(ea + (int8_t)fetch8(e));
    else if(m->mod == 2) ea = (uint16_t)(ea + fetch16(e));
    m->off = ea;
    m->seg = e->seg >= 0 ? e->sr[e->seg] : (bpbase ? SS : DS);
}

static uint8_t *reg8p(emu86_t *e, int r){
    /* AL CL DL BL AH CH DH BH; little-endian on every host this builds on */
    return (uint8_t *)&e->r[r & 3] + (r >> 2);
}
static uint32_t rm_get(emu86_t *e, const modrm_t *m, int w){
    if(m->mod == 3) return w == 8 ? *reg8p(e, m->rm) : e->r[m->rm];
    return w == 8 ? rd8(e, m->seg, m->off) : rd16(e, m->seg, m->off);
}
static void rm_set(emu86_t *e, const modrm_t *m, int w, uint32_t v){
    if(m->mod == 3){
        if(w == 8) *reg8p(e, m->rm) = (uint8_t)v; else e->r[m->rm] = (uint16_t)v;
        return;
    }
    if(w == 8) wr8(e, m->seg, m->off, (uint8_t)v); else wr16(e, m->seg, m->off, (uint16_t)v);
}
static uint32_t reg_get(emu86_t *e, int r, int w){ return w == 8 ? *reg8p(e, r) : e->r[r]; }
static void reg_set(emu86_t *e, int r, int w, uint32_t v){
    if(w == 8) *reg8p(e, r) = (uint8_t)v; else e->r[r] = (uint16_t)v;
}

/* ── interrupts ───────────────────────────────────────────────────── */
static void do_int(emu86_t *e, uint8_t n){
    push(e, (uint16_t)(e->flags | F_FIXED));
    push(e, CS);
    push(e, e->ip);
    setf(e, F_IF, 0); setf(e, F_TF, 0);
    e->ip = emu86_rd16(e, (uint32_t)n * 4);
    CS    = emu86_rd16(e, (uint32_t)n * 4 + 2);
}

void emu86_irq(emu86_t *e, int line){ e->irq_pending |= (uint8_t)(1u << line); }
void emu86_eoi(emu86_t *e){
    /* a non-specific EOI ends the highest-priority line in service */
    for(int i = 0; i < 8; i++)
        if(e->irq_service & (1u << i)){ e->irq_service &= (uint8_t)~(1u << i); return; }
}

static int deliver_irq(emu86_t *e){
    if(!getf(e, F_IF)) return 0;
    for(int i = 0; i < 8; i++){
        uint8_t b = (uint8_t)(1u << i);
        if(e->irq_service & b) return 0;          /* higher one busy */
        if((e->irq_pending & b) && !(e->irq_mask & b)){
            e->irq_pending &= (uint8_t)~b;
            e->irq_service |= b;
            do_int(e, (uint8_t)(8 + i));
            return 1;
        }
    }
    return 0;
}

static int fault(emu86_t *e, const char *why){
    e->fault_cs = e->op_cs; e->fault_ip = e->op_ip;
    for(int i = 0; i < 4; i++) e->fault_op[i] = rd8(e, e->op_cs, (uint16_t)(e->op_ip + i));
    e->fault_why = why;
    return EMU_FAULT;
}

/* ── shifts and rotates (D0-D3, C0-C1) ────────────────────────────── */
static uint32_t shift(emu86_t *e, int op, uint32_t v, int n, int w){
    uint32_t m = w == 8 ? 0xFFu : 0xFFFFu, sb = w == 8 ? 0x80u : 0x8000u;
    int cf = getf(e, F_CF);
    v &= m;
    /* the 80186 masks the count to 5 bits, and so does this */
    n &= 0x1F;
    if(n == 0) return v;
    switch(op){
    case 0:                                            /* ROL */
        for(int i = 0; i < n; i++){ cf = (v & sb) != 0; v = ((v << 1) | (uint32_t)cf) & m; }
        setf(e, F_CF, cf); setf(e, F_OF, ((v & sb) != 0) ^ cf);
        return v;
    case 1:                                            /* ROR */
        for(int i = 0; i < n; i++){ cf = v & 1; v = (v >> 1) | (cf ? sb : 0); }
        setf(e, F_CF, cf); setf(e, F_OF, ((v ^ (v << 1)) & sb) != 0);
        return v;
    case 2:                                            /* RCL */
        for(int i = 0; i < n; i++){ int c = (v & sb) != 0; v = ((v << 1) | (uint32_t)cf) & m; cf = c; }
        setf(e, F_CF, cf); setf(e, F_OF, ((v & sb) != 0) ^ cf);
        return v;
    case 3:                                            /* RCR */
        /* OF from the LAST step: did the top bit change -- which is
           what a real 8088 leaves for a count above one */
        for(int i = 0; i < n; i++){
            uint32_t top = v & sb; int c = v & 1;
            v = (v >> 1) | (cf ? sb : 0); cf = c;
            setf(e, F_OF, (v & sb) != top);
        }
        setf(e, F_CF, cf);
        return v;
    case 4: case 6: {                                  /* SHL (SAL) */
        uint32_t r = v << n;
        setf(e, F_CF, n <= (w == 8 ? 8 : 16) ? (r & (m + 1)) != 0 : 0);
        r &= m;
        setf(e, F_OF, ((r & sb) != 0) ^ getf(e, F_CF));
        setf(e, F_AF, 0);
        szp(e, r, w);
        return r; }
    case 5: {                                          /* SHR */
        uint32_t r = n > 16 ? 0 : v >> (n - 1);
        setf(e, F_CF, r & 1);
        r >>= 1;
        setf(e, F_OF, n == 1 ? (v & sb) != 0 : 0);
        setf(e, F_AF, 0);
        szp(e, r, w);
        return r; }
    default: {                                         /* SAR */
        int32_t s = w == 8 ? (int8_t)v : (int16_t)v;
        if(n > 16) n = 16;
        s >>= (n - 1);
        setf(e, F_CF, s & 1);
        s >>= 1;
        uint32_t r = (uint32_t)s & m;
        setf(e, F_OF, 0);
        setf(e, F_AF, 0);
        szp(e, r, w);
        return r; }
    }
}

/* ── string instructions ──────────────────────────────────────────── */
static uint16_t src_seg(emu86_t *e){ return e->seg >= 0 ? e->sr[e->seg] : DS; }

static void string_op(emu86_t *e, uint8_t op){
    int w = (op & 1) ? 16 : 8, d = w / 8;
    int16_t step = getf(e, F_DF) ? (int16_t)-d : (int16_t)d;
    int rep = e->rep;
    int compare = (op >= 0xA6 && op <= 0xA7) || (op >= 0xAE);
    if(rep && CX == 0) return;
    for(;;){
        switch(op){
        case 0xA4: case 0xA5:                          /* MOVS */
            if(w == 8) wr8(e, ES, DI, rd8(e, src_seg(e), SI));
            else       wr16(e, ES, DI, rd16(e, src_seg(e), SI));
            SI += (uint16_t)step; DI += (uint16_t)step; break;
        case 0xA6: case 0xA7:                          /* CMPS */
            alu(e, 7, w == 8 ? rd8(e, src_seg(e), SI) : rd16(e, src_seg(e), SI),
                      w == 8 ? rd8(e, ES, DI) : rd16(e, ES, DI), w);
            SI += (uint16_t)step; DI += (uint16_t)step; break;
        case 0xAA: case 0xAB:                          /* STOS */
            if(w == 8) wr8(e, ES, DI, (uint8_t)AX); else wr16(e, ES, DI, AX);
            DI += (uint16_t)step; break;
        case 0xAC: case 0xAD:                          /* LODS */
            if(w == 8) AX = (uint16_t)((AX & 0xFF00) | rd8(e, src_seg(e), SI));
            else       AX = rd16(e, src_seg(e), SI);
            SI += (uint16_t)step; break;
        case 0xAE: case 0xAF:                          /* SCAS */
            alu(e, 7, w == 8 ? (AX & 0xFF) : AX, w == 8 ? rd8(e, ES, DI) : rd16(e, ES, DI), w);
            DI += (uint16_t)step; break;
        case 0x6C: case 0x6D: {                        /* INS (186) */
            uint8_t lo = e->host->in8(e->ctx, DX);
            if(w == 8) wr8(e, ES, DI, lo);
            else wr16(e, ES, DI, (uint16_t)(lo | (e->host->in8(e->ctx, (uint16_t)(DX + 1)) << 8)));
            DI += (uint16_t)step; break; }
        case 0x6E: case 0x6F: {                        /* OUTS (186) */
            if(w == 8) e->host->out8(e->ctx, DX, rd8(e, src_seg(e), SI));
            else { uint16_t v = rd16(e, src_seg(e), SI);
                   e->host->out8(e->ctx, DX, (uint8_t)v);
                   e->host->out8(e->ctx, (uint16_t)(DX + 1), (uint8_t)(v >> 8)); }
            SI += (uint16_t)step; break; }
        }
        if(!rep) return;
        CX--;
        if(CX == 0) return;
        if(compare){
            if(rep == 0xF3 && !getf(e, F_ZF)) return;  /* REPE  */
            if(rep == 0xF2 &&  getf(e, F_ZF)) return;  /* REPNE */
        }
    }
}

/* ── conditions for Jcc ───────────────────────────────────────────── */
static int cond(emu86_t *e, int c){
    int r;
    switch(c >> 1){
    case 0: r = getf(e, F_OF); break;
    case 1: r = getf(e, F_CF); break;
    case 2: r = getf(e, F_ZF); break;
    case 3: r = getf(e, F_CF) || getf(e, F_ZF); break;
    case 4: r = getf(e, F_SF); break;
    case 5: r = getf(e, F_PF); break;
    case 6: r = getf(e, F_SF) != getf(e, F_OF); break;
    default: r = getf(e, F_ZF) || (getf(e, F_SF) != getf(e, F_OF)); break;
    }
    return (c & 1) ? !r : r;
}

/* ── group 3: TEST NOT NEG MUL IMUL DIV IDIV ──────────────────────── */
static int group3(emu86_t *e, const modrm_t *m, int w){
    uint32_t v = rm_get(e, m, w);
    switch(m->reg){
    case 0: case 1:
        alu(e, 4, v, w == 8 ? fetch8(e) : fetch16(e), w); return 0;
    case 2: rm_set(e, m, w, ~v); return 0;
    case 3: {
        uint32_t r = alu(e, 5, 0, v, w);
        rm_set(e, m, w, r);
        setf(e, F_CF, (v & (w == 8 ? 0xFFu : 0xFFFFu)) != 0);
        return 0; }
    case 4:
        if(w == 8){
            uint16_t r = (uint16_t)((AX & 0xFF) * v); AX = r;
            setf(e, F_CF, (r >> 8) != 0); setf(e, F_OF, (r >> 8) != 0);
            szp(e, r & 0xFF, 8);
        } else {
            uint32_t r = (uint32_t)AX * v; AX = (uint16_t)r; DX = (uint16_t)(r >> 16);
            setf(e, F_CF, DX != 0); setf(e, F_OF, DX != 0);
            szp(e, AX, 16);
        }
        return 0;
    case 5:
        if(w == 8){
            int16_t r = (int16_t)((int8_t)AX * (int8_t)v); AX = (uint16_t)r;
            int ov = r != (int8_t)r;
            setf(e, F_CF, ov); setf(e, F_OF, ov);
            szp(e, (uint32_t)r & 0xFF, 8);
        } else {
            int32_t r = (int32_t)(int16_t)AX * (int16_t)v;
            AX = (uint16_t)r; DX = (uint16_t)((uint32_t)r >> 16);
            int ov = r != (int16_t)r;
            setf(e, F_CF, ov); setf(e, F_OF, ov);
            szp(e, AX, 16);
        }
        return 0;
    case 6:
        if(w == 8){
            if(v == 0 || AX / v > 0xFF) return -1;
            uint16_t q = (uint16_t)(AX / v), rr = (uint16_t)(AX % v);
            AX = (uint16_t)((rr << 8) | q);
        } else {
            uint32_t n = ((uint32_t)DX << 16) | AX;
            if(v == 0 || n / v > 0xFFFF) return -1;
            AX = (uint16_t)(n / v); DX = (uint16_t)(n % v);
        }
        return 0;
    default:
        if(w == 8){
            int32_t n = (int16_t)AX, d = (int8_t)v;
            if(d == 0) return -1;
            int32_t q = n / d, rr = n % d;
            if(q > 127 || q < -128) return -1;
            AX = (uint16_t)(((uint8_t)rr << 8) | (uint8_t)q);
        } else {
            int32_t n = (int32_t)(((uint32_t)DX << 16) | AX), d = (int16_t)v;
            if(d == 0) return -1;
            /* INT32_MIN / -1 overflows the host too; it is a #DE here anyway */
            if(n == (int32_t)0x80000000 && d == -1) return -1;
            int32_t q = n / d, rr = n % d;
            if(q > 32767 || q < -32768) return -1;
            AX = (uint16_t)q; DX = (uint16_t)rr;
        }
        return 0;
    }
}

static void imul3(emu86_t *e, int reg, uint32_t a, int16_t b){
    int32_t r = (int32_t)(int16_t)a * b;
    e->r[reg] = (uint16_t)r;
    int ov = r != (int16_t)r;
    setf(e, F_CF, ov); setf(e, F_OF, ov);
    szp(e, (uint16_t)r, 16);
}


/* ── the 0F group: the 386's additions on 16-bit registers ────────── */
static int bitscan(emu86_t *e, uint16_t v, int rev, int *out){
    if(!v){ setf(e, F_ZF, 1); return 0; }
    setf(e, F_ZF, 0);
    if(rev){ int i = 15; while(!(v & (1u << i))) i--; *out = i; }
    else   { int i = 0;  while(!(v & (1u << i))) i++; *out = i; }
    return 1;
}

/* BT BTS BTR BTC: 0 test, 1 set, 2 reset, 3 complement */
static void bitop(emu86_t *e, const modrm_t *m, int kind, int16_t bit, int from_reg){
    if(m->mod == 3){
        uint16_t v = e->r[m->rm], b = (uint16_t)(1u << (bit & 15));
        setf(e, F_CF, (v & b) != 0);
        if(kind == 1) v |= b; else if(kind == 2) v &= (uint16_t)~b; else if(kind == 3) v ^= b;
        if(kind) e->r[m->rm] = v;
        return;
    }
    /* a register bit offset reaches outside the word, by whole words;
       an immediate one does not */
    uint16_t off = m->off;
    if(from_reg) off = (uint16_t)(off + 2 * (bit >> 4));
    uint16_t v = rd16(e, m->seg, off), b = (uint16_t)(1u << (bit & 15));
    setf(e, F_CF, (v & b) != 0);
    if(kind == 1) v |= b; else if(kind == 2) v &= (uint16_t)~b; else if(kind == 3) v ^= b;
    if(kind) wr16(e, m->seg, off, v);
}

static int op0f(emu86_t *e){
    modrm_t m;
    uint8_t op = fetch8(e);

    if(op >= 0x80 && op <= 0x8F){                      /* Jcc rel16 */
        int16_t d = (int16_t)fetch16(e);
        if(cond(e, op & 0x0F)) e->ip = (uint16_t)(e->ip + d);
        return EMU_BUDGET;
    }
    if(op >= 0x90 && op <= 0x9F){                      /* SETcc r/m8 */
        decode_modrm(e, &m);
        rm_set(e, &m, 8, (uint32_t)cond(e, op & 0x0F));
        return EMU_BUDGET;
    }
    switch(op){
    case 0xA0: push(e, e->sr[S_FS]); return EMU_BUDGET;
    case 0xA1: e->sr[S_FS] = pop(e); return EMU_BUDGET;
    case 0xA8: push(e, e->sr[S_GS]); return EMU_BUDGET;
    case 0xA9: e->sr[S_GS] = pop(e); return EMU_BUDGET;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {      /* BT* r/m16, r16 */
        decode_modrm(e, &m);
        int kind = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
        bitop(e, &m, kind, (int16_t)e->r[m.reg], 1);
        return EMU_BUDGET; }
    case 0xBA: {                                       /* BT* r/m16, imm8 */
        decode_modrm(e, &m);
        if(m.reg < 4) return fault(e, "an undefined form of 0F BA");
        bitop(e, &m, m.reg - 4, (int16_t)(fetch8(e) & 15), 0);
        return EMU_BUDGET; }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {      /* SHLD SHRD */
        decode_modrm(e, &m);
        uint32_t dst = rm_get(e, &m, 16), src = e->r[m.reg];
        int n = ((op & 1) ? (CX & 0xFF) : fetch8(e)) & 0x1F;
        if(n == 0) return EMU_BUDGET;
        if(n > 16) return fault(e, "SHLD/SHRD by more than 16 on a 16-bit register");
        uint32_t r;
        if(op <= 0xA5){
            uint32_t w = (dst << 16) | src;
            setf(e, F_CF, (w >> (32 - n)) & 1);
            r = (w << n) >> 16;
        } else {
            uint32_t w = (src << 16) | dst;
            setf(e, F_CF, (w >> (n - 1)) & 1);
            r = (w >> n) & 0xFFFF;
        }
        r &= 0xFFFF;
        setf(e, F_OF, ((r ^ dst) & 0x8000) != 0);
        szp(e, r, 16);
        rm_set(e, &m, 16, r);
        return EMU_BUDGET; }
    case 0xAF: {                                       /* IMUL r16, r/m16 */
        decode_modrm(e, &m);
        imul3(e, m.reg, e->r[m.reg], (int16_t)rm_get(e, &m, 16));
        return EMU_BUDGET; }
    case 0xB2: case 0xB4: case 0xB5: {                 /* LSS LFS LGS */
        decode_modrm(e, &m);
        if(m.mod == 3) return fault(e, "LSS/LFS/LGS with a register operand");
        e->r[m.reg] = rd16(e, m.seg, m.off);
        e->sr[op == 0xB2 ? S_SS : op == 0xB4 ? S_FS : S_GS] = rd16(e, m.seg, (uint16_t)(m.off + 2));
        return EMU_BUDGET; }
    case 0xB6: case 0xBE: {                            /* MOVZX MOVSX r16, r/m8 */
        decode_modrm(e, &m);
        uint32_t v = rm_get(e, &m, 8);
        e->r[m.reg] = op == 0xB6 ? (uint16_t)v : (uint16_t)(int16_t)(int8_t)v;
        return EMU_BUDGET; }
    case 0xB7: case 0xBF:                              /* r16, r/m16: a move */
        decode_modrm(e, &m);
        e->r[m.reg] = (uint16_t)rm_get(e, &m, 16);
        return EMU_BUDGET;
    case 0xBC: case 0xBD: {                            /* BSF BSR */
        int i;
        decode_modrm(e, &m);
        if(bitscan(e, (uint16_t)rm_get(e, &m, 16), op == 0xBD, &i)) e->r[m.reg] = (uint16_t)i;
        return EMU_BUDGET; }
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x06:
    case 0x20: case 0x21: case 0x22: case 0x23:
        return fault(e, "a protected-mode or system instruction (0F group)");
    case 0x31: return fault(e, "RDTSC, a Pentium instruction");
    case 0xA2: return fault(e, "CPUID, a 486/Pentium instruction");
    default:
        return fault(e, "an 0F instruction this CPU does not have");
    }
}

/* ── one instruction ──────────────────────────────────────────────── */
static int step(emu86_t *e){
    modrm_t m;
    uint8_t op;
    e->seg = -1; e->rep = 0;
    e->op_cs = CS; e->op_ip = e->ip;

    for(;;){                                           /* prefixes */
        op = fetch8(e);
        if(op == 0x26){ e->seg = S_ES; continue; }
        if(op == 0x2E){ e->seg = S_CS; continue; }
        if(op == 0x36){ e->seg = S_SS; continue; }
        if(op == 0x3E){ e->seg = S_DS; continue; }
        if(op == 0x64){ e->seg = S_FS; continue; }
        if(op == 0x65){ e->seg = S_GS; continue; }
        if(op == 0xF2 || op == 0xF3){ e->rep = op; continue; }
        if(op == 0xF0) continue;                       /* LOCK */
        break;
    }

    /* the eight ALU groups, 00-3F, columns 0-5 */
    if(op < 0x40 && (op & 7) < 6){
        int o = op >> 3, w = (op & 1) ? 16 : 8;
        uint32_t a, b, r;
        switch(op & 7){
        case 0: case 1:                                /* r/m, reg */
            decode_modrm(e, &m);
            a = rm_get(e, &m, w); b = reg_get(e, m.reg, w);
            r = alu(e, o, a, b, w);
            if(o != 7) rm_set(e, &m, w, r);
            return EMU_BUDGET;
        case 2: case 3:                                /* reg, r/m */
            decode_modrm(e, &m);
            a = reg_get(e, m.reg, w); b = rm_get(e, &m, w);
            r = alu(e, o, a, b, w);
            if(o != 7) reg_set(e, m.reg, w, r);
            return EMU_BUDGET;
        case 4:                                        /* AL, imm8 */
            r = alu(e, o, AX & 0xFF, fetch8(e), 8);
            if(o != 7) AX = (uint16_t)((AX & 0xFF00) | r);
            return EMU_BUDGET;
        default:                                       /* AX, imm16 */
            r = alu(e, o, AX, fetch16(e), 16);
            if(o != 7) AX = (uint16_t)r;
            return EMU_BUDGET;
        }
    }

    switch(op){
    case 0x06: push(e, ES); break;
    case 0x07: ES = pop(e); break;
    case 0x0E: push(e, CS); break;
    case 0x16: push(e, SS); break;
    case 0x17: SS = pop(e); break;
    case 0x1E: push(e, DS); break;
    case 0x1F: DS = pop(e); break;

    case 0x0F:
        /* The BIOS trap, 0F FF nn -- only from the ROM segment, so a
           guest cannot reach the host by writing these three bytes. */
        if(CS == 0xF000 && rd8(e, CS, e->ip) == 0xFF){
            uint8_t vec = rd8(e, CS, (uint16_t)(e->ip + 1));
            int r;
            e->ip += 2;
            r = e->host->bios(e->ctx, e, vec);
            if(r == BIOS_WAIT){ e->ip = e->op_ip; return EMU_WAIT; }
            if(r == BIOS_REBOOT) return EMU_REBOOT;
            break;
        }
        /* On an 8086 this was POP CS; from the 286 it is the two-byte
           escape, and the 386's 16-bit additions live behind it. */
        return op0f(e);

    case 0x27: case 0x2F: {                            /* DAA, DAS */
        /* The 8086's rules, checked against a real 8088
           (tools/check_emu86_sst.py): the high-digit test is "above
           9Fh" when AF was set on entry and "above 99h" when it was
           not, and OF comes out of the adjustment as out of any
           addition. The 386 manual's version differs in both. */
        uint8_t al = (uint8_t)AX, old = al, adj = 0;
        int cf = getf(e, F_CF), af0 = getf(e, F_AF), af = af0;
        if((al & 0x0F) > 9 || af){ adj = 0x06; af = 1; }
        if(old > (af0 ? 0x9F : 0x99) || cf){ adj = (uint8_t)(adj | 0x60); cf = 1; }
        al = op == 0x27 ? (uint8_t)(old + adj) : (uint8_t)(old - adj);
        if(op == 0x27) setf(e, F_OF, (~(old ^ adj) & (old ^ al) & 0x80) != 0);
        else           setf(e, F_OF, ((old ^ adj) & (old ^ al) & 0x80) != 0);
        setf(e, F_CF, cf); setf(e, F_AF, af);
        AX = (uint16_t)((AX & 0xFF00) | al); szp(e, al, 8);
        break; }
    case 0x37: case 0x3F: {                            /* AAA, AAS */
        /* 8086: AL and AH are adjusted separately -- no carry from one
           into the other, which the 286 introduced */
        uint8_t al = (uint8_t)AX, ah = (uint8_t)(AX >> 8);
        if((al & 0x0F) > 9 || getf(e, F_AF)){
            if(op == 0x37){ al = (uint8_t)(al + 6); ah++; }
            else          { al = (uint8_t)(al - 6); ah--; }
            setf(e, F_AF, 1); setf(e, F_CF, 1);
        } else { setf(e, F_AF, 0); setf(e, F_CF, 0); }
        AX = (uint16_t)((ah << 8) | (al & 0x0F));
        break; }

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        e->r[op & 7] = (uint16_t)inc_dec(e, e->r[op & 7], 0, 16); break;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        e->r[op & 7] = (uint16_t)inc_dec(e, e->r[op & 7], 1, 16); break;
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        /* PUSH SP pushes SP after the decrement on an 8086 and 80186 */
        if(op == 0x54){ SP -= 2; wr16(e, SS, SP, SP); }
        else push(e, e->r[op & 7]);
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        e->r[op & 7] = pop(e); break;

    case 0x60: {                                       /* PUSHA */
        uint16_t sp = SP;
        push(e, AX); push(e, CX); push(e, DX); push(e, BX);
        push(e, sp); push(e, BP); push(e, SI); push(e, DI);
        break; }
    case 0x61:                                         /* POPA */
        DI = pop(e); SI = pop(e); BP = pop(e); (void)pop(e);
        BX = pop(e); DX = pop(e); CX = pop(e); AX = pop(e);
        break;
    case 0x62: {                                       /* BOUND */
        decode_modrm(e, &m);
        if(m.mod == 3) return fault(e, "BOUND with a register operand");
        int16_t v = (int16_t)e->r[m.reg];
        int16_t lo = (int16_t)rd16(e, m.seg, m.off);
        int16_t hi = (int16_t)rd16(e, m.seg, (uint16_t)(m.off + 2));
        if(v < lo || v > hi){ e->ip = e->op_ip; do_int(e, 5); }
        break; }
    case 0x63: return fault(e, "ARPL, a 286 protected-mode instruction");
    case 0x66: return fault(e, "a 32-bit register (the 386's 66h prefix)");
    case 0x67: return fault(e, "32-bit addressing (the 386's 67h prefix)");
    case 0x68: push(e, fetch16(e)); break;
    case 0x69: { decode_modrm(e, &m); uint32_t a = rm_get(e, &m, 16);
                 imul3(e, m.reg, a, (int16_t)fetch16(e)); break; }
    case 0x6A: push(e, (uint16_t)(int8_t)fetch8(e)); break;
    case 0x6B: { decode_modrm(e, &m); uint32_t a = rm_get(e, &m, 16);
                 imul3(e, m.reg, a, (int8_t)fetch8(e)); break; }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        string_op(e, op); break;

    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = (int8_t)fetch8(e);
        if(cond(e, op & 0x0F)) e->ip = (uint16_t)(e->ip + d);
        break; }

    case 0x80: case 0x81: case 0x82: case 0x83: {      /* group 1 */
        int w = (op & 1) ? 16 : 8;
        uint32_t b, r;
        decode_modrm(e, &m);
        if(op == 0x81) b = fetch16(e);
        else if(op == 0x83) b = (uint16_t)(int8_t)fetch8(e);
        else b = fetch8(e);
        r = alu(e, m.reg, rm_get(e, &m, w), b, w);
        if(m.reg != 7) rm_set(e, &m, w, r);
        break; }
    case 0x84: case 0x85: {
        int w = (op & 1) ? 16 : 8;
        decode_modrm(e, &m);
        alu(e, 4, rm_get(e, &m, w), reg_get(e, m.reg, w), w);
        break; }
    case 0x86: case 0x87: {
        int w = (op & 1) ? 16 : 8;
        decode_modrm(e, &m);
        uint32_t a = rm_get(e, &m, w);
        rm_set(e, &m, w, reg_get(e, m.reg, w));
        reg_set(e, m.reg, w, a);
        break; }
    case 0x88: case 0x89: { int w = (op & 1) ? 16 : 8;
        decode_modrm(e, &m); rm_set(e, &m, w, reg_get(e, m.reg, w)); break; }
    case 0x8A: case 0x8B: { int w = (op & 1) ? 16 : 8;
        decode_modrm(e, &m); reg_set(e, m.reg, w, rm_get(e, &m, w)); break; }
    case 0x8C:
        /* 4 and 5 are the 386's FS and GS; 6 and 7 are ES..DS again,
           as on every 8086 -- checked against a real 8088 */
        decode_modrm(e, &m);
        rm_set(e, &m, 16, e->sr[m.reg > 5 ? m.reg & 3 : m.reg]); break;
    case 0x8D:
        decode_modrm(e, &m);
        if(m.mod == 3) return fault(e, "LEA with a register operand");
        e->r[m.reg] = m.off; break;
    case 0x8E:
        decode_modrm(e, &m);
        { int sr = m.reg > 5 ? m.reg & 3 : m.reg;
          if(sr == S_CS) return fault(e, "MOV CS, which no 80186 allows");
          e->sr[sr] = (uint16_t)rm_get(e, &m, 16); }
        break;
    case 0x8F:
        decode_modrm(e, &m);
        { uint16_t v = pop(e); rm_set(e, &m, 16, v); }
        break;

    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96: case 0x97: {
        uint16_t t = AX; AX = e->r[op & 7]; e->r[op & 7] = t; break; }
    case 0x98: AX = (uint16_t)(int8_t)AX; break;
    case 0x99: DX = (AX & 0x8000) ? 0xFFFF : 0; break;
    case 0x9A: {
        uint16_t o = fetch16(e), s = fetch16(e);
        push(e, CS); push(e, e->ip); CS = s; e->ip = o; break; }
    case 0x9B: break;                                  /* WAIT */
    case 0x9C: push(e, (uint16_t)(e->flags | F_FIXED)); break;
    case 0x9D: e->flags = (uint16_t)((pop(e) & 0x0FD5) | F_FIXED); break;
    case 0x9E: e->flags = (uint16_t)((e->flags & 0xFF00) | ((AX >> 8) & 0xD5) | 0x02); break;
    case 0x9F: AX = (uint16_t)((AX & 0x00FF) | ((uint16_t)((e->flags & 0xD5) | 0x02) << 8)); break;

    case 0xA0: { uint16_t o = fetch16(e); AX = (uint16_t)((AX & 0xFF00) | rd8(e, src_seg(e), o)); break; }
    case 0xA1: { uint16_t o = fetch16(e); AX = rd16(e, src_seg(e), o); break; }
    case 0xA2: { uint16_t o = fetch16(e); wr8(e, src_seg(e), o, (uint8_t)AX); break; }
    case 0xA3: { uint16_t o = fetch16(e); wr16(e, src_seg(e), o, AX); break; }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(e, op); break;
    case 0xA8: alu(e, 4, AX & 0xFF, fetch8(e), 8); break;
    case 0xA9: alu(e, 4, AX, fetch16(e), 16); break;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        *reg8p(e, op & 7) = fetch8(e); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        e->r[op & 7] = fetch16(e); break;

    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int w = (op & 1) ? 16 : 8, n;
        decode_modrm(e, &m);
        if(op <= 0xC1) n = fetch8(e);
        else if(op <= 0xD1) n = 1;
        else n = CX & 0xFF;
        rm_set(e, &m, w, shift(e, m.reg, rm_get(e, &m, w), n, w));
        break; }

    case 0xC2: { uint16_t n = fetch16(e); e->ip = pop(e); SP += n; break; }
    case 0xC3: e->ip = pop(e); break;
    case 0xC4: case 0xC5:
        decode_modrm(e, &m);
        if(m.mod == 3) return fault(e, "LES/LDS with a register operand");
        e->r[m.reg] = rd16(e, m.seg, m.off);
        e->sr[op == 0xC4 ? S_ES : S_DS] = rd16(e, m.seg, (uint16_t)(m.off + 2));
        break;
    case 0xC6: case 0xC7: { int w = (op & 1) ? 16 : 8;
        decode_modrm(e, &m);
        rm_set(e, &m, w, w == 8 ? fetch8(e) : fetch16(e)); break; }
    case 0xC8: {                                       /* ENTER */
        uint16_t size = fetch16(e); uint8_t lvl = (uint8_t)(fetch8(e) & 0x1F);
        push(e, BP);
        uint16_t frame = SP;
        for(int i = 1; i < lvl; i++){ BP -= 2; push(e, rd16(e, SS, BP)); }
        if(lvl) push(e, frame);
        BP = frame; SP -= size;
        break; }
    case 0xC9: SP = BP; BP = pop(e); break;            /* LEAVE */
    case 0xCA: { uint16_t n = fetch16(e); e->ip = pop(e); CS = pop(e); SP += n; break; }
    case 0xCB: e->ip = pop(e); CS = pop(e); break;
    case 0xCC: do_int(e, 3); break;
    case 0xCD: { uint8_t n = fetch8(e); do_int(e, n); break; }
    case 0xCE: if(getf(e, F_OF)) do_int(e, 4); break;
    case 0xCF:
        e->ip = pop(e); CS = pop(e);
        e->flags = (uint16_t)((pop(e) & 0x0FD5) | F_FIXED);
        break;

    case 0xD4: {                                       /* AAM */
        uint8_t b = fetch8(e);
        if(!b){
            /* the 8088 sets the flags as for a zero result, then traps */
            setf(e, F_CF, 0); setf(e, F_AF, 0); setf(e, F_OF, 0);
            szp(e, 0, 8);
            do_int(e, 0); break;
        }
        uint8_t al = (uint8_t)AX;
        AX = (uint16_t)(((al / b) << 8) | (al % b));
        szp(e, AX & 0xFF, 8);
        break; }
    case 0xD5: {                                       /* AAD */
        uint8_t b = fetch8(e);
        uint8_t al = (uint8_t)((uint8_t)AX + (uint8_t)(AX >> 8) * b);
        AX = al; szp(e, al, 8);
        break; }
    case 0xD6: AX = (uint16_t)((AX & 0xFF00) | (getf(e, F_CF) ? 0xFF : 0)); break;  /* SALC */
    case 0xD7: AX = (uint16_t)((AX & 0xFF00) | rd8(e, src_seg(e), (uint16_t)(BX + (AX & 0xFF)))); break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        decode_modrm(e, &m);                           /* no 8087 fitted */
        break;

    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        int8_t d = (int8_t)fetch8(e);
        int go;
        if(op == 0xE3) go = CX == 0;
        else {
            CX--;
            go = CX != 0;
            if(op == 0xE0) go = go && !getf(e, F_ZF);
            if(op == 0xE1) go = go &&  getf(e, F_ZF);
        }
        if(go) e->ip = (uint16_t)(e->ip + d);
        break; }
    case 0xE4: AX = (uint16_t)((AX & 0xFF00) | e->host->in8(e->ctx, fetch8(e))); break;
    case 0xE5: { uint8_t p = fetch8(e);
        AX = (uint16_t)(e->host->in8(e->ctx, p) | (e->host->in8(e->ctx, (uint16_t)(p + 1)) << 8)); break; }
    case 0xE6: e->host->out8(e->ctx, fetch8(e), (uint8_t)AX); break;
    case 0xE7: { uint8_t p = fetch8(e);
        e->host->out8(e->ctx, p, (uint8_t)AX);
        e->host->out8(e->ctx, (uint16_t)(p + 1), (uint8_t)(AX >> 8)); break; }
    case 0xE8: { int16_t d = (int16_t)fetch16(e); push(e, e->ip); e->ip = (uint16_t)(e->ip + d); break; }
    case 0xE9: { int16_t d = (int16_t)fetch16(e); e->ip = (uint16_t)(e->ip + d); break; }
    case 0xEA: { uint16_t o = fetch16(e), s = fetch16(e); CS = s; e->ip = o; break; }
    case 0xEB: { int8_t d = (int8_t)fetch8(e); e->ip = (uint16_t)(e->ip + d); break; }
    case 0xEC: AX = (uint16_t)((AX & 0xFF00) | e->host->in8(e->ctx, DX)); break;
    case 0xED: AX = (uint16_t)(e->host->in8(e->ctx, DX) | (e->host->in8(e->ctx, (uint16_t)(DX + 1)) << 8)); break;
    case 0xEE: e->host->out8(e->ctx, DX, (uint8_t)AX); break;
    case 0xEF: e->host->out8(e->ctx, DX, (uint8_t)AX);
               e->host->out8(e->ctx, (uint16_t)(DX + 1), (uint8_t)(AX >> 8)); break;

    case 0xF4:                                         /* HLT */
        e->halted = 1;
        return getf(e, F_IF) ? EMU_IDLE : EMU_HALT;
    case 0xF5: setf(e, F_CF, !getf(e, F_CF)); break;
    case 0xF6: case 0xF7:
        decode_modrm(e, &m);
        if(group3(e, &m, (op & 1) ? 16 : 8) < 0){
            /* #DE: the 8086 and 80186 push the address of the NEXT
               instruction (the 286 changed that), so a handler that
               simply returns does not loop on the divide for ever. */
            do_int(e, 0);
        }
        break;
    case 0xF8: setf(e, F_CF, 0); break;
    case 0xF9: setf(e, F_CF, 1); break;
    case 0xFA: setf(e, F_IF, 0); break;
    case 0xFB: setf(e, F_IF, 1); break;
    case 0xFC: setf(e, F_DF, 0); break;
    case 0xFD: setf(e, F_DF, 1); break;
    case 0xFE:
        decode_modrm(e, &m);
        if(m.reg > 1) return fault(e, "an undefined form of FE");
        rm_set(e, &m, 8, inc_dec(e, rm_get(e, &m, 8), m.reg, 8));
        break;
    case 0xFF:
        decode_modrm(e, &m);
        switch(m.reg){
        case 0: case 1: rm_set(e, &m, 16, inc_dec(e, rm_get(e, &m, 16), m.reg, 16)); break;
        case 2: { uint16_t t = (uint16_t)rm_get(e, &m, 16); push(e, e->ip); e->ip = t; break; }
        case 3: {
            if(m.mod == 3) return fault(e, "CALL FAR with a register operand");
            uint16_t o = rd16(e, m.seg, m.off), s = rd16(e, m.seg, (uint16_t)(m.off + 2));
            push(e, CS); push(e, e->ip); CS = s; e->ip = o; break; }
        case 4: e->ip = (uint16_t)rm_get(e, &m, 16); break;
        case 5: {
            if(m.mod == 3) return fault(e, "JMP FAR with a register operand");
            uint16_t o = rd16(e, m.seg, m.off), s = rd16(e, m.seg, (uint16_t)(m.off + 2));
            CS = s; e->ip = o; break; }
        case 6:
            /* PUSH SP pushes the decremented SP on an 8086, by this
               encoding as well as by 54h */
            if(m.mod == 3 && m.rm == R_SP){ SP -= 2; wr16(e, SS, SP, SP); }
            else push(e, (uint16_t)rm_get(e, &m, 16));
            break;
        default: return fault(e, "an undefined form of FF");
        }
        break;

    default:
        return fault(e, "an opcode an 80186 does not have");
    }
    return EMU_BUDGET;
}

/* ── the public face ──────────────────────────────────────────────── */
void emu86_reset(emu86_t *e, uint8_t *mem, const emu86_host_t *h, void *ctx){
    for(int i = 0; i < 8; i++) e->r[i] = 0;
    for(int i = 0; i < 6; i++) e->sr[i] = 0;
    e->mem = mem; e->host = h; e->ctx = ctx;
    e->flags = F_FIXED;
    CS = 0xF000; e->ip = 0xFFF0;                       /* the reset vector */
    e->irq_pending = e->irq_service = e->irq_mask = 0;
    e->fault_why = 0; e->icount = 0; e->halted = 0;
}

int emu86_run(emu86_t *e, uint32_t budget){
    while(budget--){
        if(e->irq_pending && deliver_irq(e)) e->halted = 0;
        if(e->halted) return getf(e, F_IF) ? EMU_IDLE : EMU_HALT;
        int r = step(e);
        e->icount++;
        if(r != EMU_BUDGET) return r;
    }
    return EMU_BUDGET;
}
