/* ════════════════════════════════════════════════════════════════════
   emu/cpu386.c -- the 32-bit CPU of Retro PC.  v0.9.7.2.
   include/emu386.h says what it is and what it is not.

   One instruction at a time: prefixes, then one switch on the opcode.
   Flags are computed when an instruction sets them, not lazily -- a
   little slower, and every flag can be compared with the real CPU's
   right after the instruction, which is what tests/test_emu386.c does.

   Exceptions: an instruction that faults sets c->exc and returns; the
   loop puts EIP back at its first byte and delivers the exception
   through the IDT, as the real CPU does for a fault. Nothing here can
   fault on a memory access -- there is no paging and segments are not
   limit-checked -- so the only ways in are #DE, #UD and the INTs.
   ════════════════════════════════════════════════════════════════════ */
#include "emu386.h"
/* the three it uses, declared here rather than through include/string.h,
   so tests/test_emu386.c can compile this file beside the C library's */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);

#define EXC_DE 0
#define EXC_UD 6
#define EXC_DF 8
#define EXC_GP 13

/* ── memory ───────────────────────────────────────────────────────── */
uint8_t cpu386_rd8(const cpu386_t *c, uint32_t a){
    return a < c->mem_size ? c->mem[a] : 0xFF;
}
static uint16_t rd16(const cpu386_t *c, uint32_t a){
    if(a + 1 < c->mem_size && a + 1 > a){ uint16_t v; memcpy(&v, c->mem + a, 2); return v; }
    return (uint16_t)(cpu386_rd8(c, a) | (cpu386_rd8(c, a + 1) << 8));
}
uint32_t cpu386_rd32(const cpu386_t *c, uint32_t a){
    if(a + 3 < c->mem_size && a + 3 > a){ uint32_t v; memcpy(&v, c->mem + a, 4); return v; }
    return (uint32_t)rd16(c, a) | ((uint32_t)rd16(c, a + 2) << 16);
}
void cpu386_wr8(cpu386_t *c, uint32_t a, uint8_t v){
    if(a < c->mem_size) c->mem[a] = v;
}
static void wr16(cpu386_t *c, uint32_t a, uint16_t v){
    if(a + 1 < c->mem_size && a + 1 > a){ memcpy(c->mem + a, &v, 2); return; }
    cpu386_wr8(c, a, (uint8_t)v); cpu386_wr8(c, a + 1, (uint8_t)(v >> 8));
}
void cpu386_wr32(cpu386_t *c, uint32_t a, uint32_t v){
    if(a + 3 < c->mem_size && a + 3 > a){ memcpy(c->mem + a, &v, 4); return; }
    wr16(c, a, (uint16_t)v); wr16(c, a + 2, (uint16_t)(v >> 16));
}
static uint32_t rdn(const cpu386_t *c, uint32_t a, int n){
    return n == 1 ? cpu386_rd8(c, a) : n == 2 ? rd16(c, a) : cpu386_rd32(c, a);
}
static void wrn(cpu386_t *c, uint32_t a, uint32_t v, int n){
    if(n == 1) cpu386_wr8(c, a, (uint8_t)v);
    else if(n == 2) wr16(c, a, (uint16_t)v);
    else cpu386_wr32(c, a, v);
}

static inline uint32_t MASK(int n){ return n == 4 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFu : 0xFFu; }
static inline uint32_t SIGN(int n){ return n == 4 ? 0x80000000u : n == 2 ? 0x8000u : 0x80u; }
static int32_t  sext(uint32_t v, int n){
    return n == 1 ? (int32_t)(int8_t)v : n == 2 ? (int32_t)(int16_t)v : (int32_t)v;
}

/* ── fetching ─────────────────────────────────────────────────────── */
static inline uint8_t f8(cpu386_t *c){
    uint32_t a = c->base[X_CS] + c->eip++;
    return a < c->mem_size ? c->mem[a] : 0xFF;
}
static inline uint16_t f16(cpu386_t *c){ uint16_t v = rd16(c, c->base[X_CS] + c->eip); c->eip += 2; return v; }
static inline uint32_t f32(cpu386_t *c){ uint32_t v = cpu386_rd32(c, c->base[X_CS] + c->eip); c->eip += 4; return v; }
static uint32_t fimm(cpu386_t *c, int n){ return n == 1 ? f8(c) : n == 2 ? f16(c) : f32(c); }

/* ── registers by size ────────────────────────────────────────────── */
static inline uint32_t getr(const cpu386_t *c, int r, int n){
    if(n == 1) return r < 4 ? (c->r[r] & 0xFF) : ((c->r[r - 4] >> 8) & 0xFF);
    if(n == 2) return c->r[r] & 0xFFFF;
    return c->r[r];
}
static inline void setr(cpu386_t *c, int r, uint32_t v, int n){
    if(n == 1){
        if(r < 4) c->r[r] = (c->r[r] & ~0xFFu) | (v & 0xFF);
        else      c->r[r - 4] = (c->r[r - 4] & ~0xFF00u) | ((v & 0xFF) << 8);
    } else if(n == 2) c->r[r] = (c->r[r] & ~0xFFFFu) | (v & 0xFFFF);
    else c->r[r] = v;
}

/* ── flags ────────────────────────────────────────────────────────── */
#define ARITH (X_CF | X_PF | X_AF | X_ZF | X_SF | X_OF)
/* PF for each low byte, built once */
static uint8_t g_pf[256];
static int     g_pf_ready;
static void pf_init(void){
    for(int i = 0; i < 256; i++){
        int v = i; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
        g_pf[i] = (v & 1) ? 0 : X_PF;
    }
    g_pf_ready = 1;
}
static inline uint32_t szp(uint32_t r, int n){
    uint32_t f;
    r &= MASK(n);
    f = g_pf[r & 0xFF];
    if(!r) f |= X_ZF;
    if(r & SIGN(n)) f |= X_SF;
    return f;
}
static inline void setf(cpu386_t *c, uint32_t which, uint32_t val){
    c->eflags = (c->eflags & ~which) | (val & which);
}
static inline int CF(const cpu386_t *c){ return (c->eflags & X_CF) != 0; }

/* the eight ALU operations, op = the /digit of 80-83 */
static uint32_t alu(cpu386_t *c, int op, uint32_t a, uint32_t b, int n){
    uint32_t m = MASK(n), s = SIGN(n), r = 0, f = 0;
    uint64_t wide;
    a &= m; b &= m;
    switch(op){
    case 0: case 2: {                                   /* add, adc */
        uint32_t ci = (op == 2) ? (uint32_t)CF(c) : 0;
        wide = (uint64_t)a + b + ci; r = (uint32_t)wide & m;
        if(wide > m) f |= X_CF;
        if((a ^ r) & (b ^ r) & s) f |= X_OF;
        if((a ^ b ^ r) & 0x10) f |= X_AF;
        break; }
    case 3: case 5: case 7: {                           /* sbb, sub, cmp */
        uint32_t bi = (op == 3) ? (uint32_t)CF(c) : 0;
        r = (a - b - bi) & m;
        if((uint64_t)a < (uint64_t)b + bi) f |= X_CF;
        if((a ^ b) & (a ^ r) & s) f |= X_OF;
        if((a ^ b ^ r) & 0x10) f |= X_AF;
        break; }
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    case 6: r = a ^ b; break;
    }
    f |= szp(r, n);
    setf(c, ARITH, f);
    return r;
}
uint32_t cpu386_alu(cpu386_t *c, int op, uint32_t a, uint32_t b, int size){
    if(!g_pf_ready) pf_init();
    return alu(c, op, a, b, size);
}

static uint32_t incdec(cpu386_t *c, uint32_t a, int dec, int n){
    uint32_t keep = c->eflags & X_CF;
    uint32_t r = alu(c, dec ? 5 : 0, a, 1, n);
    c->eflags = (c->eflags & ~X_CF) | keep;
    return r;
}

/* shifts and rotates, op = the /digit of C0/C1/D0-D3 */
static uint32_t shift(cpu386_t *c, int op, uint32_t a, uint8_t cnt, int n){
    int bits = n * 8;
    uint32_t m = MASK(n), s = SIGN(n), r = a & m, f;
    cnt &= 31;
    if(!cnt) return r;
    switch(op){
    case 0: {                                           /* rol */
        int k = cnt % bits;
        r = k ? ((r << k) | (r >> (bits - k))) & m : r;
        setf(c, X_CF, (r & 1) ? X_CF : 0);
        setf(c, X_OF, (((r & s) != 0) ^ ((r & 1) != 0)) ? X_OF : 0);
        return r; }
    case 1: {                                           /* ror */
        int k = cnt % bits;
        r = k ? ((r >> k) | (r << (bits - k))) & m : r;
        setf(c, X_CF, (r & s) ? X_CF : 0);
        setf(c, X_OF, (((r & s) != 0) ^ ((r & (s >> 1)) != 0)) ? X_OF : 0);
        return r; }
    case 2: case 3: {                                   /* rcl, rcr */
        int k = cnt % (bits + 1);
        uint64_t v = r, cf = (uint64_t)CF(c);
        for(int i = 0; i < k; i++){
            if(op == 2){ uint64_t out = (v >> (bits - 1)) & 1; v = ((v << 1) | cf) & m; cf = out; }
            else       { uint64_t out = v & 1; v = (v >> 1) | (cf << (bits - 1)); cf = out; }
        }
        r = (uint32_t)v & m;
        setf(c, X_CF, cf ? X_CF : 0);
        if(op == 2) setf(c, X_OF, (((r & s) != 0) ^ (cf != 0)) ? X_OF : 0);
        else        setf(c, X_OF, (((r & s) != 0) ^ ((r & (s >> 1)) != 0)) ? X_OF : 0);
        return r; }
    case 4: case 6: {                                   /* shl, sal */
        uint32_t out = (cnt <= (uint8_t)bits) ? ((uint32_t)(((uint64_t)r << (cnt - 1)) >> (bits - 1)) & 1) : 0;
        r = cnt < 32 ? (uint32_t)(((uint64_t)r << cnt) & m) : 0;
        f = szp(r, n) | (out ? X_CF : 0);
        if(((r & s) != 0) ^ (out != 0)) f |= X_OF;
        setf(c, ARITH, f);
        return r; }
    case 5: {                                           /* shr */
        uint32_t out = (cnt <= (uint8_t)bits) ? (uint32_t)(((uint64_t)r >> (cnt - 1)) & 1) : 0;
        f = (a & s) ? X_OF : 0;
        r = (uint32_t)((uint64_t)r >> cnt) & m;
        f |= szp(r, n) | (out ? X_CF : 0);
        setf(c, ARITH, f);
        return r; }
    case 7: {                                           /* sar */
        int64_t v = sext(r, n);
        uint32_t out = (uint32_t)((v >> (cnt - 1 < 63 ? cnt - 1 : 63)) & 1);
        r = (uint32_t)(v >> (cnt < 63 ? cnt : 63)) & m;
        f = szp(r, n) | (out ? X_CF : 0);
        setf(c, ARITH, f);
        return r; }
    }
    return r;
}
uint32_t cpu386_shift(cpu386_t *c, int op, uint32_t a, uint8_t n, int size){
    if(!g_pf_ready) pf_init();
    return shift(c, op, a, n, size);
}

static int cond(const cpu386_t *c, int cc){
    uint32_t f = c->eflags;
    int r;
    switch(cc >> 1){
    case 0: r = (f & X_OF) != 0; break;
    case 1: r = (f & X_CF) != 0; break;
    case 2: r = (f & X_ZF) != 0; break;
    case 3: r = (f & (X_CF | X_ZF)) != 0; break;
    case 4: r = (f & X_SF) != 0; break;
    case 5: r = (f & X_PF) != 0; break;
    case 6: r = ((f & X_SF) != 0) != ((f & X_OF) != 0); break;
    default: r = (f & X_ZF) || (((f & X_SF) != 0) != ((f & X_OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}

/* ── the stack ────────────────────────────────────────────────────── */
static int ss32(const cpu386_t *c){ (void)c; return 1; }   /* flat: a 32-bit stack */
static void push(cpu386_t *c, uint32_t v, int n){
    if(ss32(c)){ c->r[X_ESP] -= (uint32_t)n; wrn(c, c->base[X_SS] + c->r[X_ESP], v, n); }
}
static uint32_t pop(cpu386_t *c, int n){
    uint32_t v = rdn(c, c->base[X_SS] + c->r[X_ESP], n);
    c->r[X_ESP] += (uint32_t)n;
    return v;
}

/* ── segments: the base from the GDT, and nothing else ────────────── */
static uint32_t desc_base(const cpu386_t *c, uint16_t sel){
    uint32_t a;
    if((sel & ~3u) == 0) return 0;
    a = c->gdt_base + (sel & ~7u);
    return (uint32_t)rd16(c, a + 2) | ((uint32_t)cpu386_rd8(c, a + 4) << 16) |
           ((uint32_t)cpu386_rd8(c, a + 7) << 24);
}
static void load_seg(cpu386_t *c, int s, uint16_t sel){
    c->sel[s] = sel;
    c->base[s] = (c->cr0 & 1) ? desc_base(c, sel) : ((uint32_t)sel << 4);
}

/* ── interrupts ───────────────────────────────────────────────────── */
/* 1 delivered; 0 no usable gate */
static int deliver(cpu386_t *c, int vec, int has_err, uint32_t err){
    uint32_t g, off, typ;
    uint16_t sel;
    if(!(c->cr0 & 1) || (uint32_t)vec * 8 + 7 > c->idt_lim) return 0;
    g = c->idt_base + (uint32_t)vec * 8;
    off = (uint32_t)rd16(c, g) | ((uint32_t)rd16(c, g + 6) << 16);
    sel = rd16(c, g + 2);
    typ = cpu386_rd8(c, g + 5);
    if(!(typ & 0x80)) return 0;                         /* not present */
    push(c, c->eflags, 4);
    push(c, c->sel[X_CS], 4);
    push(c, c->eip, 4);
    if(has_err) push(c, err, 4);
    c->eflags &= ~X_TF;
    if((typ & 0x0F) == 0x0E || (typ & 0x0F) == 0x06) c->eflags &= ~X_IF;  /* interrupt gate */
    load_seg(c, X_CS, sel);
    c->eip = off;
    return 1;
}

static void raise(cpu386_t *c, int vec){ if(c->exc < 0){ c->exc = vec; c->exc_has_err = 0; c->exc_err = 0; } }
static void raise_err(cpu386_t *c, int vec, uint32_t err){
    if(c->exc < 0){ c->exc = vec; c->exc_has_err = 1; c->exc_err = err; }
}

/* ── ModRM ────────────────────────────────────────────────────────── */
typedef struct { int mod, reg, rm; uint32_t ea; } mrm_t;

static uint32_t lin(const cpu386_t *c, int defseg, uint32_t off){
    int s = c->seg >= 0 ? c->seg : defseg;
    if(c->a16) off &= 0xFFFF;
    return c->base[s] + off;
}

static void modrm(cpu386_t *c, mrm_t *m){
    uint8_t b = f8(c);
    uint32_t off = 0;
    int defseg = X_DS;
    m->mod = b >> 6; m->reg = (b >> 3) & 7; m->rm = b & 7;
    if(m->mod == 3){ m->ea = 0; return; }
    if(c->a16){
        static const int bx[8] = { X_EBX, X_EBX, X_EBP, X_EBP, -1, -1, X_EBP, X_EBX };
        static const int ix[8] = { X_ESI, X_EDI, X_ESI, X_EDI, X_ESI, X_EDI, -1, -1 };
        if(m->mod == 0 && m->rm == 6) off = f16(c);
        else {
            if(bx[m->rm] >= 0) off += c->r[bx[m->rm]] & 0xFFFF;
            if(ix[m->rm] >= 0) off += c->r[ix[m->rm]] & 0xFFFF;
            if(m->rm == 2 || m->rm == 3 || m->rm == 6) defseg = X_SS;
            if(m->mod == 1) off += (uint32_t)(int32_t)(int8_t)f8(c);
            else if(m->mod == 2) off += f16(c);
        }
        m->ea = lin(c, defseg, off & 0xFFFF);
        return;
    }
    if(m->rm == 4){                                     /* SIB */
        uint8_t sib = f8(c);
        int sc = sib >> 6, idx = (sib >> 3) & 7, bas = sib & 7;
        if(bas == 5 && m->mod == 0) off = f32(c);
        else { off = c->r[bas]; if(bas == X_ESP || bas == X_EBP) defseg = X_SS; }
        if(idx != 4) off += c->r[idx] << sc;
    } else if(m->rm == 5 && m->mod == 0){
        off = f32(c);
    } else {
        off = c->r[m->rm];
        if(m->rm == X_EBP) defseg = X_SS;
    }
    if(m->mod == 1) off += (uint32_t)(int32_t)(int8_t)f8(c);
    else if(m->mod == 2) off += f32(c);
    m->ea = lin(c, defseg, off);
}
static uint32_t rm_get(cpu386_t *c, const mrm_t *m, int n){
    return m->mod == 3 ? getr(c, m->rm, n) : rdn(c, m->ea, n);
}
static void rm_set(cpu386_t *c, const mrm_t *m, uint32_t v, int n){
    if(m->mod == 3) setr(c, m->rm, v, n); else wrn(c, m->ea, v, n);
}

/* ── ports ────────────────────────────────────────────────────────── */
static uint32_t pin(cpu386_t *c, uint16_t port, int n){
    return c->host && c->host->in ? c->host->in(c->ctx, port, n) & MASK(n) : MASK(n);
}
static void pout(cpu386_t *c, uint16_t port, uint32_t v, int n){
    if(c->host && c->host->out) c->host->out(c->ctx, port, v & MASK(n), n);
}

/* ── string instructions ──────────────────────────────────────────── */
static uint32_t cnt_reg(const cpu386_t *c){ return c->a16 ? (c->r[X_ECX] & 0xFFFF) : c->r[X_ECX]; }
static void cnt_set(cpu386_t *c, uint32_t v){
    if(c->a16) c->r[X_ECX] = (c->r[X_ECX] & 0xFFFF0000u) | (v & 0xFFFF); else c->r[X_ECX] = v;
}
static uint32_t idx(const cpu386_t *c, int r){ return c->a16 ? (c->r[r] & 0xFFFF) : c->r[r]; }
static void idx_add(cpu386_t *c, int r, int32_t d){
    if(c->a16) c->r[r] = (c->r[r] & 0xFFFF0000u) | ((c->r[r] + (uint32_t)d) & 0xFFFF);
    else c->r[r] += (uint32_t)d;
}

static void string_op(cpu386_t *c, uint8_t op, int n){
    int32_t d = (c->eflags & X_DF) ? -n : n;
    int rep = c->rep;
    int srcseg = c->seg >= 0 ? c->seg : X_DS;
    if(rep && !cnt_reg(c)) return;
    /* fast paths for the two every kernel's memset and memcpy reach */
    if(rep && !c->a16 && !(c->eflags & X_DF)){
        uint32_t cnt = c->r[X_ECX], bytes = cnt * (uint32_t)n;
        uint32_t di = c->base[X_ES] + c->r[X_EDI];
        if(op == 0xAA || op == 0xAB){
            if(bytes / (uint32_t)n == cnt && di + bytes <= c->mem_size && di + bytes >= di){
                uint32_t v = c->r[X_EAX] & MASK(n);
                if(n == 1) memset(c->mem + di, (int)v, bytes);
                else for(uint32_t i = 0; i < cnt; i++) wrn(c, di + i * (uint32_t)n, v, n);
                c->r[X_EDI] += bytes; c->r[X_ECX] = 0;
                return;
            }
        } else if(op == 0xA4 || op == 0xA5){
            uint32_t si = c->base[srcseg] + c->r[X_ESI];
            if(bytes / (uint32_t)n == cnt && di + bytes <= c->mem_size && si + bytes <= c->mem_size &&
               di + bytes >= di && si + bytes >= si && (di <= si || di >= si + bytes)){
                memmove(c->mem + di, c->mem + si, bytes);
                c->r[X_EDI] += bytes; c->r[X_ESI] += bytes; c->r[X_ECX] = 0;
                return;
            }
        }
    }
    for(;;){
        uint32_t a, b;
        switch(op){
        case 0xA4: case 0xA5:                           /* movs */
            wrn(c, c->base[X_ES] + idx(c, X_EDI), rdn(c, c->base[srcseg] + idx(c, X_ESI), n), n);
            idx_add(c, X_ESI, d); idx_add(c, X_EDI, d); break;
        case 0xA6: case 0xA7:                           /* cmps */
            a = rdn(c, c->base[srcseg] + idx(c, X_ESI), n);
            b = rdn(c, c->base[X_ES] + idx(c, X_EDI), n);
            alu(c, 7, a, b, n);
            idx_add(c, X_ESI, d); idx_add(c, X_EDI, d); break;
        case 0xAA: case 0xAB:                           /* stos */
            wrn(c, c->base[X_ES] + idx(c, X_EDI), getr(c, X_EAX, n), n);
            idx_add(c, X_EDI, d); break;
        case 0xAC: case 0xAD:                           /* lods */
            setr(c, X_EAX, rdn(c, c->base[srcseg] + idx(c, X_ESI), n), n);
            idx_add(c, X_ESI, d); break;
        case 0xAE: case 0xAF:                           /* scas */
            alu(c, 7, getr(c, X_EAX, n), rdn(c, c->base[X_ES] + idx(c, X_EDI), n), n);
            idx_add(c, X_EDI, d); break;
        case 0x6C: case 0x6D:                           /* ins */
            wrn(c, c->base[X_ES] + idx(c, X_EDI), pin(c, (uint16_t)c->r[X_EDX], n), n);
            idx_add(c, X_EDI, d); break;
        case 0x6E: case 0x6F:                           /* outs */
            pout(c, (uint16_t)c->r[X_EDX], rdn(c, c->base[srcseg] + idx(c, X_ESI), n), n);
            idx_add(c, X_ESI, d); break;
        }
        if(!rep) return;
        cnt_set(c, cnt_reg(c) - 1);
        if(!cnt_reg(c)) return;
        if(op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF){
            int z = (c->eflags & X_ZF) != 0;
            if(rep == 0xF3 && !z) return;               /* repe */
            if(rep == 0xF2 && z) return;                /* repne */
        }
    }
}

/* ── multiply and divide ──────────────────────────────────────────── */
static void mul1(cpu386_t *c, uint32_t v, int sgn, int n){
    uint32_t f;
    if(n == 1){
        uint16_t r = sgn ? (uint16_t)((int16_t)(int8_t)c->r[X_EAX] * (int16_t)(int8_t)v)
                         : (uint16_t)((c->r[X_EAX] & 0xFF) * (v & 0xFF));
        setr(c, X_EAX, r, 2);
        f = sgn ? ((int16_t)r != (int8_t)r) : (r >> 8) != 0;
    } else if(n == 2){
        uint32_t r = sgn ? (uint32_t)((int32_t)(int16_t)c->r[X_EAX] * (int32_t)(int16_t)v)
                         : (c->r[X_EAX] & 0xFFFF) * (v & 0xFFFF);
        setr(c, X_EAX, r, 2); setr(c, X_EDX, r >> 16, 2);
        f = sgn ? ((int32_t)r != (int16_t)r) : (r >> 16) != 0;
    } else {
        uint64_t r = sgn ? (uint64_t)((int64_t)(int32_t)c->r[X_EAX] * (int64_t)(int32_t)v)
                         : (uint64_t)c->r[X_EAX] * v;
        c->r[X_EAX] = (uint32_t)r; c->r[X_EDX] = (uint32_t)(r >> 32);
        f = sgn ? ((int64_t)r != (int32_t)r) : (r >> 32) != 0;
    }
    setf(c, X_CF | X_OF, f ? (X_CF | X_OF) : 0);
}
static uint32_t imul2(cpu386_t *c, uint32_t a, uint32_t b, int n){
    int64_t r = (int64_t)sext(a, n) * (int64_t)sext(b, n);
    uint32_t t = (uint32_t)r & MASK(n);
    int over = (int64_t)sext(t, n) != r;
    setf(c, X_CF | X_OF, over ? (X_CF | X_OF) : 0);
    return t;
}
/* 64 by 32 bits, unsigned, for a kernel with no libgcc to call. The
   caller has made sure the quotient fits in 32 bits. Shift and subtract:
   slow, and DIV is rare enough in the old builds for that not to show. */
static uint32_t udiv64(uint64_t x, uint32_t d, uint32_t *rem){
    uint32_t hi = (uint32_t)(x >> 32), lo = (uint32_t)x, q = 0;
    for(int i = 31; i >= 0; i--){
        int carry = (hi & 0x80000000u) != 0;
        hi = (hi << 1) | (lo >> 31); lo <<= 1;
        q <<= 1;
        if(carry || hi >= d){ hi -= d; q |= 1; }
    }
    *rem = hi;
    return q;
}

static void div1(cpu386_t *c, uint32_t v, int sgn, int n){
    v &= MASK(n);
    if(!v){ raise(c, EXC_DE); return; }
    if(n == 1){
        uint16_t ax = (uint16_t)c->r[X_EAX];
        if(sgn){
            int16_t q = (int16_t)((int16_t)ax / (int8_t)v), rr = (int16_t)((int16_t)ax % (int8_t)v);
            if(q > 127 || q < -128){ raise(c, EXC_DE); return; }
            setr(c, X_EAX, (uint8_t)q | ((uint32_t)(uint8_t)rr << 8), 2);
        } else {
            uint16_t q = (uint16_t)(ax / v), rr = (uint16_t)(ax % v);
            if(q > 0xFF){ raise(c, EXC_DE); return; }
            setr(c, X_EAX, q | ((uint32_t)rr << 8), 2);
        }
    } else if(n == 2){
        uint32_t x = (c->r[X_EAX] & 0xFFFF) | ((c->r[X_EDX] & 0xFFFF) << 16);
        if(sgn){
            int32_t q = (int32_t)x / (int16_t)v, rr = (int32_t)x % (int16_t)v;
            if(q > 32767 || q < -32768){ raise(c, EXC_DE); return; }
            setr(c, X_EAX, (uint32_t)q, 2); setr(c, X_EDX, (uint32_t)rr, 2);
        } else {
            uint32_t q = x / v, rr = x % v;
            if(q > 0xFFFF){ raise(c, EXC_DE); return; }
            setr(c, X_EAX, q, 2); setr(c, X_EDX, rr, 2);
        }
    } else {
        uint64_t x = (uint64_t)c->r[X_EAX] | ((uint64_t)c->r[X_EDX] << 32);
        uint32_t q, rr;
        if(sgn){
            int neg_x = (int64_t)x < 0, neg_d = (int32_t)v < 0;
            uint64_t ax = neg_x ? (uint64_t)0 - x : x;
            uint32_t ad = neg_d ? (uint32_t)0 - v : v;
            if((uint32_t)(ax >> 32) >= ad){ raise(c, EXC_DE); return; }
            q = udiv64(ax, ad, &rr);
            if(neg_x != neg_d){ if(q > 0x80000000u){ raise(c, EXC_DE); return; } q = (uint32_t)0 - q; }
            else if(q > 0x7FFFFFFFu){ raise(c, EXC_DE); return; }
            if(neg_x) rr = (uint32_t)0 - rr;
        } else {
            if(c->r[X_EDX] >= v){ raise(c, EXC_DE); return; }
            q = udiv64(x, v, &rr);
        }
        c->r[X_EAX] = q; c->r[X_EDX] = rr;
    }
}

/* ── far transfers and IRET ───────────────────────────────────────── */
static void iret(cpu386_t *c){
    int n = c->o16 ? 2 : 4;
    uint32_t ip = pop(c, n), cs = pop(c, n) & 0xFFFF, fl = pop(c, n);
    if(n == 2) fl = (c->eflags & 0xFFFF0000u) | fl;
    c->eip = ip;
    load_seg(c, X_CS, (uint16_t)cs);
    c->eflags = (fl & 0x003F7FD5u) | 2u;
}

/* ── 0F xx ────────────────────────────────────────────────────────── */
static void op0f(cpu386_t *c){
    uint8_t op = f8(c);
    int n = c->o16 ? 2 : 4;
    mrm_t m;
    if(op >= 0x80 && op <= 0x8F){                       /* jcc near */
        int32_t d = c->o16 ? (int16_t)f16(c) : (int32_t)f32(c);
        if(cond(c, op & 15)) c->eip += (uint32_t)d;
        if(c->o16) c->eip &= 0xFFFF;
        return;
    }
    if(op >= 0x90 && op <= 0x9F){ modrm(c, &m); rm_set(c, &m, (uint32_t)cond(c, op & 15), 1); return; }
    if(op >= 0x40 && op <= 0x4F){                       /* cmovcc */
        modrm(c, &m);
        { uint32_t v = rm_get(c, &m, n); if(cond(c, op & 15)) setr(c, m.reg, v, n); }
        return;
    }
    if(op >= 0xC8){ c->r[op & 7] = (c->r[op & 7] >> 24) | ((c->r[op & 7] >> 8) & 0xFF00u) |
                                   ((c->r[op & 7] << 8) & 0xFF0000u) | (c->r[op & 7] << 24); return; }
    switch(op){
    case 0x00:                                          /* sldt str lldt ltr verr verw */
        modrm(c, &m);
        if(m.reg == 0 || m.reg == 1) rm_set(c, &m, 0, 2);
        return;
    case 0x01:
        modrm(c, &m);
        switch(m.reg){
        case 0: wr16(c, m.ea, c->gdt_lim); cpu386_wr32(c, m.ea + 2, c->gdt_base); return;
        case 1: wr16(c, m.ea, c->idt_lim); cpu386_wr32(c, m.ea + 2, c->idt_base); return;
        case 2: c->gdt_lim = rd16(c, m.ea); c->gdt_base = cpu386_rd32(c, m.ea + 2) & (c->o16 ? 0xFFFFFFu : 0xFFFFFFFFu); return;
        case 3: c->idt_lim = rd16(c, m.ea); c->idt_base = cpu386_rd32(c, m.ea + 2) & (c->o16 ? 0xFFFFFFu : 0xFFFFFFFFu); return;
        case 4: rm_set(c, &m, c->cr0 & 0xFFFF, 2); return;
        case 6: c->cr0 = (c->cr0 & ~0xFu) | (rm_get(c, &m, 2) & 0xF) | (c->cr0 & 1); return;
        case 7: return;                                 /* invlpg: no TLB here */
        }
        raise(c, EXC_UD); return;
    case 0x06: c->cr0 &= ~8u; return;                   /* clts */
    case 0x08: case 0x09: return;                       /* invd, wbinvd */
    case 0x0B: raise(c, EXC_UD); return;                /* ud2 */
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        modrm(c, &m); return;                           /* prefetch, nop r/m */
    case 0x20: modrm(c, &m);                            /* mov r32, crN */
        c->r[m.rm] = m.reg == 0 ? c->cr0 : m.reg == 2 ? c->cr2 : m.reg == 3 ? c->cr3 : m.reg == 4 ? c->cr4 : 0;
        return;
    case 0x22: modrm(c, &m);                            /* mov crN, r32 */
        if(m.reg == 0) c->cr0 = c->r[m.rm] | 0x10u;
        else if(m.reg == 2) c->cr2 = c->r[m.rm];
        else if(m.reg == 3) c->cr3 = c->r[m.rm];
        else if(m.reg == 4) c->cr4 = c->r[m.rm];
        return;
    case 0x21: modrm(c, &m); c->r[m.rm] = 0; return;    /* debug registers: none */
    case 0x23: modrm(c, &m); return;
    case 0x30: return;                                  /* wrmsr: accepted, kept nowhere */
    case 0x31: c->r[X_EAX] = (uint32_t)c->tsc; c->r[X_EDX] = (uint32_t)(c->tsc >> 32); return;
    case 0x32: c->r[X_EAX] = 0; c->r[X_EDX] = 0; return;    /* rdmsr */
    case 0xA0: push(c, c->sel[X_FS], n); return;
    case 0xA1: load_seg(c, X_FS, (uint16_t)pop(c, n)); return;
    case 0xA8: push(c, c->sel[X_GS], n); return;
    case 0xA9: load_seg(c, X_GS, (uint16_t)pop(c, n)); return;
    case 0xA2: {                                        /* cpuid */
        uint32_t leaf = c->r[X_EAX];
        if(leaf == 0){
            c->r[X_EAX] = 1;
            c->r[X_EBX] = 0x65766148u; c->r[X_EDX] = 0x7465526Eu; c->r[X_ECX] = 0x43506F72u; /* "HavenRetroPC" */
        } else if(leaf == 1){
            c->r[X_EAX] = 0x00000633u;                  /* family 6 */
            c->r[X_EBX] = 0; c->r[X_ECX] = 0;
            c->r[X_EDX] = (1u << 4) | (1u << 8) | (1u << 15);   /* TSC, CX8, CMOV -- no FPU */
        } else if(leaf == 0x80000000u){
            c->r[X_EAX] = 0x80000000u; c->r[X_EBX] = c->r[X_ECX] = c->r[X_EDX] = 0;  /* no long mode */
        } else { c->r[X_EAX] = c->r[X_EBX] = c->r[X_ECX] = c->r[X_EDX] = 0; }
        return; }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {       /* bt bts btr btc r/m, r */
        uint32_t bit, a, v, mask;
        int32_t off;
        modrm(c, &m);
        bit = getr(c, m.reg, n);
        if(m.mod == 3){ off = 0; bit &= (uint32_t)(n * 8 - 1); a = 0; }
        else {
            off = (n == 4) ? ((int32_t)bit >> 5) * 4 : ((int32_t)(int16_t)bit >> 4) * 2;
            bit &= (uint32_t)(n * 8 - 1);
            a = m.ea + (uint32_t)off;
        }
        v = m.mod == 3 ? getr(c, m.rm, n) : rdn(c, a, n);
        mask = 1u << bit;
        setf(c, X_CF, (v & mask) ? X_CF : 0);
        if(op == 0xA3) return;
        v = op == 0xAB ? (v | mask) : op == 0xB3 ? (v & ~mask) : (v ^ mask);
        if(m.mod == 3) setr(c, m.rm, v, n); else wrn(c, a, v, n);
        return; }
    case 0xBA: {                                        /* bt* r/m, imm8 */
        uint32_t v, mask;
        modrm(c, &m);
        v = rm_get(c, &m, n);
        mask = 1u << (f8(c) & (n * 8 - 1));
        setf(c, X_CF, (v & mask) ? X_CF : 0);
        if(m.reg == 4) return;
        if(m.reg < 4){ raise(c, EXC_UD); return; }
        v = m.reg == 5 ? (v | mask) : m.reg == 6 ? (v & ~mask) : (v ^ mask);
        rm_set(c, &m, v, n);
        return; }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {       /* shld, shrd */
        uint32_t a, b, r; uint8_t cnt; int bits = n * 8;
        modrm(c, &m);
        a = rm_get(c, &m, n); b = getr(c, m.reg, n);
        cnt = (op == 0xA4 || op == 0xAC) ? f8(c) : (uint8_t)c->r[X_ECX];
        cnt &= 31;
        if(!cnt) return;
        if(op < 0xAC){
            uint64_t w = ((uint64_t)a << bits) | b;
            r = (uint32_t)((w << cnt) >> bits) & MASK(n);
            setf(c, X_CF, ((w >> (bits - cnt)) >> bits) & 1 ? X_CF : 0);
        } else {
            uint64_t w = ((uint64_t)b << bits) | a;
            r = (uint32_t)(w >> cnt) & MASK(n);
            setf(c, X_CF, (w >> (cnt - 1)) & 1 ? X_CF : 0);
        }
        setf(c, X_ZF | X_SF | X_PF, szp(r, n));
        setf(c, X_OF, ((r ^ a) & SIGN(n)) ? X_OF : 0);
        rm_set(c, &m, r, n);
        return; }
    case 0xAF: modrm(c, &m); setr(c, m.reg, imul2(c, getr(c, m.reg, n), rm_get(c, &m, n), n), n); return;
    case 0xB0: case 0xB1: {                             /* cmpxchg */
        int w = op == 0xB0 ? 1 : n;
        uint32_t d;
        modrm(c, &m);
        d = rm_get(c, &m, w);
        alu(c, 7, getr(c, X_EAX, w), d, w);
        if(c->eflags & X_ZF) rm_set(c, &m, getr(c, m.reg, w), w);
        else setr(c, X_EAX, d, w);
        return; }
    case 0xC0: case 0xC1: {                             /* xadd */
        int w = op == 0xC0 ? 1 : n;
        uint32_t d, s;
        modrm(c, &m);
        d = rm_get(c, &m, w); s = getr(c, m.reg, w);
        setr(c, m.reg, d, w);
        rm_set(c, &m, alu(c, 0, d, s, w), w);
        return; }
    case 0xB2: case 0xB4: case 0xB5: {                  /* lss lfs lgs */
        modrm(c, &m);
        { uint32_t off = rdn(c, m.ea, n); uint16_t sel = rd16(c, m.ea + (uint32_t)n);
          load_seg(c, op == 0xB2 ? X_SS : op == 0xB4 ? X_FS : X_GS, sel); setr(c, m.reg, off, n); }
        return; }
    case 0xB6: case 0xB7: modrm(c, &m); setr(c, m.reg, rm_get(c, &m, op == 0xB6 ? 1 : 2), n); return;
    case 0xBE: case 0xBF: modrm(c, &m); setr(c, m.reg, (uint32_t)sext(rm_get(c, &m, op == 0xBE ? 1 : 2), op == 0xBE ? 1 : 2), n); return;
    case 0xBC: case 0xBD: {                             /* bsf, bsr */
        uint32_t v; int i;
        modrm(c, &m);
        v = rm_get(c, &m, n);
        if(!v){ c->eflags |= X_ZF; return; }
        c->eflags &= ~X_ZF;
        if(op == 0xBC){ for(i = 0; !(v & (1u << i)); i++); }
        else { for(i = n * 8 - 1; !(v & (1u << i)); i--); }
        setr(c, m.reg, (uint32_t)i, n);
        return; }
    }
    raise(c, EXC_UD);
}

/* ── one instruction ──────────────────────────────────────────────── */
static void step(cpu386_t *c){
    uint8_t op;
    int n;
    mrm_t m;

    c->op_eip = c->eip;
    c->seg = -1; c->rep = 0; c->o16 = 0; c->a16 = 0;
    for(int k = 0; ; k++){
        op = f8(c);
        if((op & 0xE6) != 0x26 && (op & 0xFC) != 0x64 && (op & 0xFC) != 0xF0) break;   /* not a prefix */
        if(k > 14){ raise(c, EXC_GP); return; }
        switch(op){
        case 0x26: c->seg = X_ES; continue;
        case 0x2E: c->seg = X_CS; continue;
        case 0x36: c->seg = X_SS; continue;
        case 0x3E: c->seg = X_DS; continue;
        case 0x64: c->seg = X_FS; continue;
        case 0x65: c->seg = X_GS; continue;
        case 0x66: c->o16 = 1; continue;
        case 0x67: c->a16 = 1; continue;
        case 0xF0: continue;
        case 0xF2: case 0xF3: c->rep = op; continue;
        }
        break;
    }
    n = c->o16 ? 2 : 4;

    switch(op){
    case 0x00 ... 0x05: case 0x08 ... 0x0D: case 0x10 ... 0x15: case 0x18 ... 0x1D:
    case 0x20 ... 0x25: case 0x28 ... 0x2D: case 0x30 ... 0x35: case 0x38 ... 0x3D: {
        int aop = op >> 3, w = (op & 1) ? n : 1;
        uint32_t a, b, r;
        switch(op & 7){
        case 0: case 1: modrm(c, &m); a = rm_get(c, &m, w); b = getr(c, m.reg, w);
            r = alu(c, aop, a, b, w); if(aop != 7) rm_set(c, &m, r, w); return;
        case 2: case 3: modrm(c, &m); a = getr(c, m.reg, w); b = rm_get(c, &m, w);
            r = alu(c, aop, a, b, w); if(aop != 7) setr(c, m.reg, r, w); return;
        default: a = getr(c, X_EAX, w); b = fimm(c, w);
            r = alu(c, aop, a, b, w); if(aop != 7) setr(c, X_EAX, r, w); return;
        }
    }
    case 0x40 ... 0x47: setr(c, op & 7, incdec(c, getr(c, op & 7, n), 0, n), n); return;
    case 0x48 ... 0x4F: setr(c, op & 7, incdec(c, getr(c, op & 7, n), 1, n), n); return;
    case 0x50 ... 0x57: push(c, getr(c, op & 7, n), n); return;
    case 0x58 ... 0x5F: { uint32_t v = pop(c, n); setr(c, op & 7, v, n); return; }
    case 0x70 ... 0x7F: {
        int32_t d = (int8_t)f8(c);
        if(cond(c, op & 15)) c->eip += (uint32_t)d;
        return; }
    case 0x91 ... 0x97: {
        uint32_t t = getr(c, op & 7, n); setr(c, op & 7, getr(c, X_EAX, n), n); setr(c, X_EAX, t, n); return; }
    case 0xB0 ... 0xB7: setr(c, op & 7, f8(c), 1); return;
    case 0xB8 ... 0xBF: setr(c, op & 7, fimm(c, n), n); return;
    case 0xD8 ... 0xDF: modrm(c, &m); return;           /* x87: none here */
    case 0x06: push(c, c->sel[X_ES], n); return;
    case 0x07: load_seg(c, X_ES, (uint16_t)pop(c, n)); return;
    case 0x0E: push(c, c->sel[X_CS], n); return;
    case 0x16: push(c, c->sel[X_SS], n); return;
    case 0x17: load_seg(c, X_SS, (uint16_t)pop(c, n)); return;
    case 0x1E: push(c, c->sel[X_DS], n); return;
    case 0x1F: load_seg(c, X_DS, (uint16_t)pop(c, n)); return;
    case 0x0F: op0f(c); return;
    case 0x27: case 0x2F: {                             /* daa, das */
        uint8_t al = (uint8_t)c->r[X_EAX], old = al; int cf = CF(c), af = (c->eflags & X_AF) != 0;
        uint32_t f = 0;
        if((al & 0x0F) > 9 || af){ al = (uint8_t)(op == 0x27 ? al + 6 : al - 6); f |= X_AF; }
        if(old > 0x99 || cf){ al = (uint8_t)(op == 0x27 ? al + 0x60 : al - 0x60); f |= X_CF; }
        setr(c, X_EAX, al, 1);
        setf(c, ARITH & ~X_OF, f | szp(al, 1));
        return; }
    case 0x37: case 0x3F: {                             /* aaa, aas */
        uint32_t ax = c->r[X_EAX] & 0xFFFF;
        if((ax & 0x0F) > 9 || (c->eflags & X_AF)){
            ax = op == 0x37 ? ax + 0x106 : ax - 0x106;
            c->eflags |= X_AF | X_CF;
        } else c->eflags &= ~(X_AF | X_CF);
        setr(c, X_EAX, ax & 0xFF0F, 2);
        return; }
    case 0x60: {                                        /* pusha */
        uint32_t sp = c->r[X_ESP];
        for(int i = 0; i < 8; i++) push(c, i == X_ESP ? sp : getr(c, i, n), n);
        return; }
    case 0x61:                                          /* popa */
        for(int i = 7; i >= 0; i--){ uint32_t v = pop(c, n); if(i != X_ESP) setr(c, i, v, n); }
        return;
    case 0x68: push(c, fimm(c, n), n); return;
    case 0x6A: push(c, (uint32_t)(int32_t)(int8_t)f8(c), n); return;
    case 0x69: case 0x6B: {
        uint32_t b;
        modrm(c, &m);
        b = op == 0x69 ? fimm(c, n) : (uint32_t)(int32_t)(int8_t)f8(c);
        setr(c, m.reg, imul2(c, rm_get(c, &m, n), b, n), n);
        return; }
    case 0x6C: case 0x6E: string_op(c, op, 1); return;
    case 0x6D: case 0x6F: string_op(c, op, n); return;
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int w = (op & 1) ? n : 1;
        uint32_t a, b, r;
        modrm(c, &m);
        a = rm_get(c, &m, w);
        b = op == 0x83 ? (uint32_t)(int32_t)(int8_t)f8(c) : fimm(c, w);
        r = alu(c, m.reg, a, b, w);
        if(m.reg != 7) rm_set(c, &m, r, w);
        return; }
    case 0x84: case 0x85: { int w = (op & 1) ? n : 1;
        modrm(c, &m); alu(c, 4, rm_get(c, &m, w), getr(c, m.reg, w), w); return; }
    case 0x86: case 0x87: { int w = (op & 1) ? n : 1; uint32_t t;
        modrm(c, &m); t = rm_get(c, &m, w); rm_set(c, &m, getr(c, m.reg, w), w); setr(c, m.reg, t, w); return; }
    case 0x88: modrm(c, &m); rm_set(c, &m, getr(c, m.reg, 1), 1); return;
    case 0x89: modrm(c, &m); rm_set(c, &m, getr(c, m.reg, n), n); return;
    case 0x8A: modrm(c, &m); setr(c, m.reg, rm_get(c, &m, 1), 1); return;
    case 0x8B: modrm(c, &m); setr(c, m.reg, rm_get(c, &m, n), n); return;
    case 0x8C: modrm(c, &m);
        if(m.mod == 3) setr(c, m.rm, c->sel[m.reg % 6], n); else wr16(c, m.ea, c->sel[m.reg % 6]);
        return;
    case 0x8D: {                                        /* lea: the offset, not the linear address */
        int saved = c->seg; uint32_t b0;
        c->seg = X_DS;
        b0 = c->base[X_DS];
        modrm(c, &m);
        setr(c, m.reg, m.ea - b0, n);
        c->seg = saved;
        return; }
    case 0x8E: modrm(c, &m); load_seg(c, m.reg % 6, (uint16_t)rm_get(c, &m, 2)); return;
    case 0x8F: { uint32_t v = pop(c, n); modrm(c, &m); rm_set(c, &m, v, n); return; }
    case 0x90: return;
    case 0x98: if(c->o16) setr(c, X_EAX, (uint32_t)(int32_t)(int8_t)c->r[X_EAX], 2);
               else c->r[X_EAX] = (uint32_t)(int32_t)(int16_t)c->r[X_EAX];
               return;
    case 0x99: if(c->o16) setr(c, X_EDX, (c->r[X_EAX] & 0x8000) ? 0xFFFF : 0, 2);
               else c->r[X_EDX] = (c->r[X_EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
               return;
    case 0x9A: {                                        /* call far */
        uint32_t off = fimm(c, n); uint16_t sel = f16(c);
        push(c, c->sel[X_CS], n); push(c, c->eip, n);
        load_seg(c, X_CS, sel); c->eip = off;
        return; }
    case 0x9B: return;                                  /* wait */
    case 0x9C: push(c, (c->eflags & 0x00FCFFFFu) | 2u, n); return;
    case 0x9D: { uint32_t f = pop(c, n);
        if(n == 2) f = (c->eflags & 0xFFFF0000u) | f;
        c->eflags = (f & 0x003F7FD5u) | 2u;
        return; }
    case 0x9E: setf(c, X_SF | X_ZF | X_AF | X_PF | X_CF, (c->r[X_EAX] >> 8) & 0xFF); return;
    case 0x9F: setr(c, X_EAX, ((c->eflags & 0xD5u) | 2u) << 8 | (c->r[X_EAX] & 0xFF), 2); return;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        int w = (op & 1) ? n : 1;
        uint32_t off = c->a16 ? f16(c) : f32(c), a = lin(c, X_DS, off);
        if(op < 0xA2) setr(c, X_EAX, rdn(c, a, w), w); else wrn(c, a, getr(c, X_EAX, w), w);
        return; }
    case 0xA4: case 0xA6: case 0xAA: case 0xAC: case 0xAE: string_op(c, op, 1); return;
    case 0xA5: case 0xA7: case 0xAB: case 0xAD: case 0xAF: string_op(c, op, n); return;
    case 0xA8: alu(c, 4, c->r[X_EAX], f8(c), 1); return;
    case 0xA9: alu(c, 4, c->r[X_EAX], fimm(c, n), n); return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int w = (op & 1) ? n : 1;
        uint8_t cnt;
        modrm(c, &m);
        cnt = op <= 0xC1 ? f8(c) : op <= 0xD1 ? 1 : (uint8_t)c->r[X_ECX];
        rm_set(c, &m, shift(c, m.reg, rm_get(c, &m, w), cnt, w), w);
        return; }
    case 0xC2: { uint16_t k = f16(c); c->eip = pop(c, n); c->r[X_ESP] += k; return; }
    case 0xC3: c->eip = pop(c, n); return;
    case 0xC4: case 0xC5: {
        modrm(c, &m);
        { uint32_t off = rdn(c, m.ea, n); uint16_t sel = rd16(c, m.ea + (uint32_t)n);
          load_seg(c, op == 0xC4 ? X_ES : X_DS, sel); setr(c, m.reg, off, n); }
        return; }
    case 0xC6: modrm(c, &m); rm_set(c, &m, f8(c), 1); return;
    case 0xC7: modrm(c, &m); rm_set(c, &m, fimm(c, n), n); return;
    case 0xC8: {                                        /* enter */
        uint16_t sz = f16(c); uint8_t lvl = (uint8_t)(f8(c) & 31);
        uint32_t fp;
        push(c, c->r[X_EBP], n);
        fp = c->r[X_ESP];
        for(int i = 1; i < lvl; i++){ c->r[X_EBP] -= (uint32_t)n; push(c, rdn(c, c->base[X_SS] + c->r[X_EBP], n), n); }
        if(lvl) push(c, fp, n);
        setr(c, X_EBP, fp, n);
        c->r[X_ESP] -= sz;
        return; }
    case 0xC9: c->r[X_ESP] = c->r[X_EBP]; setr(c, X_EBP, pop(c, n), n); return;
    case 0xCA: case 0xCB: {                             /* retf */
        uint16_t k = op == 0xCA ? f16(c) : 0;
        uint32_t ip = pop(c, n); uint16_t cs = (uint16_t)pop(c, n);
        load_seg(c, X_CS, cs); c->eip = ip; c->r[X_ESP] += k;
        return; }
    case 0xCC: if(!deliver(c, 3, 0, 0)) raise(c, EXC_GP); return;
    case 0xCD: { uint8_t v = f8(c); if(!deliver(c, v, 0, 0)) raise_err(c, EXC_GP, (uint32_t)v * 8 + 2); return; }
    case 0xCE: if((c->eflags & X_OF) && !deliver(c, 4, 0, 0)) raise(c, EXC_GP); return;
    case 0xCF: iret(c); return;
    case 0xD4: { uint8_t b = f8(c), al = (uint8_t)c->r[X_EAX];
        if(!b){ raise(c, EXC_DE); return; }
        setr(c, X_EAX, ((uint32_t)(al / b) << 8) | (al % b), 2);
        setf(c, X_ZF | X_SF | X_PF, szp(al % b, 1)); return; }
    case 0xD5: { uint8_t b = f8(c);
        uint8_t al = (uint8_t)(((c->r[X_EAX] >> 8) & 0xFF) * b + (c->r[X_EAX] & 0xFF));
        setr(c, X_EAX, al, 2);
        setf(c, X_ZF | X_SF | X_PF, szp(al, 1)); return; }
    case 0xD6: setr(c, X_EAX, CF(c) ? 0xFF : 0, 1); return;
    case 0xD7: setr(c, X_EAX, cpu386_rd8(c, lin(c, X_DS, (c->a16 ? (c->r[X_EBX] & 0xFFFF) : c->r[X_EBX]) + (c->r[X_EAX] & 0xFF))), 1); return;
    case 0xE0: case 0xE1: case 0xE2: {                  /* loopne loope loop */
        int32_t d = (int8_t)f8(c);
        uint32_t cx = cnt_reg(c) - 1;
        cnt_set(c, cx);
        if(cx && (op == 0xE2 || (op == 0xE1) == ((c->eflags & X_ZF) != 0))) c->eip += (uint32_t)d;
        return; }
    case 0xE3: { int32_t d = (int8_t)f8(c); if(!cnt_reg(c)) c->eip += (uint32_t)d; return; }
    case 0xE4: setr(c, X_EAX, pin(c, f8(c), 1), 1); return;
    case 0xE5: setr(c, X_EAX, pin(c, f8(c), n), n); return;
    case 0xE6: pout(c, f8(c), c->r[X_EAX], 1); return;
    case 0xE7: pout(c, f8(c), c->r[X_EAX], n); return;
    case 0xE8: { int32_t d = c->o16 ? (int16_t)f16(c) : (int32_t)f32(c); push(c, c->eip, n); c->eip += (uint32_t)d; return; }
    case 0xE9: { int32_t d = c->o16 ? (int16_t)f16(c) : (int32_t)f32(c); c->eip += (uint32_t)d; return; }
    case 0xEA: { uint32_t off = fimm(c, n); uint16_t sel = f16(c); load_seg(c, X_CS, sel); c->eip = off; return; }
    case 0xEB: { int32_t d = (int8_t)f8(c); c->eip += (uint32_t)d; return; }
    case 0xEC: setr(c, X_EAX, pin(c, (uint16_t)c->r[X_EDX], 1), 1); return;
    case 0xED: setr(c, X_EAX, pin(c, (uint16_t)c->r[X_EDX], n), n); return;
    case 0xEE: pout(c, (uint16_t)c->r[X_EDX], c->r[X_EAX], 1); return;
    case 0xEF: pout(c, (uint16_t)c->r[X_EDX], c->r[X_EAX], n); return;
    case 0xF4: c->halted = 1; return;
    case 0xF5: c->eflags ^= X_CF; return;
    case 0xF6: case 0xF7: {
        int w = (op & 1) ? n : 1;
        uint32_t v;
        modrm(c, &m);
        switch(m.reg){
        case 0: case 1: alu(c, 4, rm_get(c, &m, w), fimm(c, w), w); return;
        case 2: rm_set(c, &m, ~rm_get(c, &m, w), w); return;
        case 3: v = rm_get(c, &m, w); rm_set(c, &m, alu(c, 5, 0, v, w), w);
                setf(c, X_CF, (v & MASK(w)) ? X_CF : 0); return;
        case 4: mul1(c, rm_get(c, &m, w), 0, w); return;
        case 5: mul1(c, rm_get(c, &m, w), 1, w); return;
        case 6: div1(c, rm_get(c, &m, w), 0, w); return;
        case 7: div1(c, rm_get(c, &m, w), 1, w); return;
        }
        return; }
    case 0xF8: c->eflags &= ~X_CF; return;
    case 0xF9: c->eflags |= X_CF; return;
    case 0xFA: c->eflags &= ~X_IF; return;
    case 0xFB: c->eflags |= X_IF; return;
    case 0xFC: c->eflags &= ~X_DF; return;
    case 0xFD: c->eflags |= X_DF; return;
    case 0xFE:
        modrm(c, &m);
        if(m.reg > 1){ raise(c, EXC_UD); return; }
        rm_set(c, &m, incdec(c, rm_get(c, &m, 1), m.reg, 1), 1);
        return;
    case 0xFF:
        modrm(c, &m);
        switch(m.reg){
        case 0: case 1: rm_set(c, &m, incdec(c, rm_get(c, &m, n), m.reg, n), n); return;
        case 2: { uint32_t t = rm_get(c, &m, n); push(c, c->eip, n); c->eip = t; return; }
        case 3: { uint32_t off = rdn(c, m.ea, n); uint16_t sel = rd16(c, m.ea + (uint32_t)n);
                  push(c, c->sel[X_CS], n); push(c, c->eip, n); load_seg(c, X_CS, sel); c->eip = off; return; }
        case 4: c->eip = rm_get(c, &m, n); return;
        case 5: { uint32_t off = rdn(c, m.ea, n); uint16_t sel = rd16(c, m.ea + (uint32_t)n);
                  load_seg(c, X_CS, sel); c->eip = off; return; }
        case 6: push(c, rm_get(c, &m, n), n); return;
        }
        raise(c, EXC_UD); return;
    }
    raise(c, EXC_UD);
}

/* ── the loop ─────────────────────────────────────────────────────── */
void cpu386_reset(cpu386_t *c, uint8_t *mem, uint32_t mem_size,
                  const cpu386_host_t *h, void *ctx){
    memset(c, 0, sizeof *c);
    c->mem = mem; c->mem_size = mem_size;
    c->host = h; c->ctx = ctx;
    c->eflags = 2;
    c->exc = -1;
    c->fault_vec = -1;
    if(!g_pf_ready) pf_init();
}

void cpu386_flat(cpu386_t *c, uint32_t eip){
    c->cr0 = 0x11;                  /* PE, ET */
    c->gdt_base = 0; c->gdt_lim = 0;
    for(int s = 0; s < 6; s++){ c->sel[s] = s == X_CS ? 0x10 : 0x18; c->base[s] = 0; }
    c->eip = eip;
    c->eflags = 2;
}

/* an exception or interrupt that cannot be delivered ends the run, as a
   triple fault resets a real machine -- and says what it was */
static int take_exception(cpu386_t *c){
    int v = c->exc;
    c->exc = -1;
    c->eip = c->op_eip;
    if(deliver(c, v, c->exc_has_err, c->exc_err)){ c->nested = 0; return 0; }
    c->fault_eip = c->op_eip;
    c->fault_vec = v;
    c->fault_why = v == EXC_UD ? "an instruction this CPU does not have" :
                   v == EXC_DE ? "a division by zero, with no handler" :
                                 "an exception with no handler in the IDT (a triple fault)";
    return 1;
}

int cpu386_run(cpu386_t *c, uint32_t budget){
    while(budget--){
        if(c->intr && (c->eflags & X_IF) && c->host && c->host->intack){
            int v = c->host->intack(c->ctx);
            if(v >= 0){ c->halted = 0; deliver(c, v, 0, 0); }
        }
        if(c->halted) return (c->eflags & X_IF) ? X86_IDLE : X86_HALT;
        step(c);
        c->icount++;
        c->tsc += 3;
        if(c->exc >= 0 && take_exception(c)) return X86_FAULT;
    }
    return X86_BUDGET;
}
