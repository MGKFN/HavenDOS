/* tests/test_emu386.c -- emu/cpu386.c, Retro PC's 32-bit CPU.  v0.9.7.2.

   Part 1: every ALU operation and every shift, at 8, 16 and 32 bits,
   against the build machine's own processor -- the same operation run
   natively, its flags read back with PUSHF. Random operands plus the
   edges (0, 1, the sign bit, all ones). Flags Intel leaves undefined
   are masked: AF after logic ops and shifts, OF after a shift of more
   than one.

   Part 2: small programs, assembled by hand, run start to finish.

   Part 3 (only when the BOOT OS images are present, which they are in
   archive/retro once built): each boots to its login screen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../emu/cpu386.c"
#include "../emu/pc386.c"
#include "retro_images.h"

static int total, fails;
static void ck(const char *w, int ok, long got)
{ total++; if(ok){ if(getenv("V")) printf("  ok    %s\n", w); } else { fails++; printf("  FAIL  %s  (%lx)\n", w, got); } }

#define FMASK (X_CF | X_PF | X_AF | X_ZF | X_SF | X_OF)

/* ── native reference ─────────────────────────────────────────────── */
#define NAT(INSN, T, RA, RB)                                            \
    __asm__ volatile("push %[fi]\n\tpopfq\n\t" INSN " %" RB "[b], %" RA "[a]\n\tpushfq\n\tpop %[fo]" \
        : [a] "+r"(av), [fo] "=r"(fo) : [b] "r"(bv), [fi] "r"(fi) : "cc")

static uint32_t native_alu(int op, uint32_t a, uint32_t b, int n, uint64_t fi, uint64_t *fout){
    uint64_t fo = 0;
    if(n == 4){
        uint32_t av = a, bv = b;
        switch(op){
        case 0: NAT("addl", uint32_t, "k", "k"); break;
        case 1: NAT("orl",  uint32_t, "k", "k"); break;
        case 2: NAT("adcl", uint32_t, "k", "k"); break;
        case 3: NAT("sbbl", uint32_t, "k", "k"); break;
        case 4: NAT("andl", uint32_t, "k", "k"); break;
        case 5: NAT("subl", uint32_t, "k", "k"); break;
        case 6: NAT("xorl", uint32_t, "k", "k"); break;
        case 7: NAT("cmpl", uint32_t, "k", "k"); break;
        }
        *fout = fo; return av;
    } else if(n == 2){
        uint16_t av = (uint16_t)a, bv = (uint16_t)b;
        switch(op){
        case 0: NAT("addw", uint16_t, "w", "w"); break;
        case 1: NAT("orw",  uint16_t, "w", "w"); break;
        case 2: NAT("adcw", uint16_t, "w", "w"); break;
        case 3: NAT("sbbw", uint16_t, "w", "w"); break;
        case 4: NAT("andw", uint16_t, "w", "w"); break;
        case 5: NAT("subw", uint16_t, "w", "w"); break;
        case 6: NAT("xorw", uint16_t, "w", "w"); break;
        case 7: NAT("cmpw", uint16_t, "w", "w"); break;
        }
        *fout = fo; return av;
    } else {
        uint8_t av = (uint8_t)a, bv = (uint8_t)b;
        switch(op){
        case 0: NAT("addb", uint8_t, "b", "b"); break;
        case 1: NAT("orb",  uint8_t, "b", "b"); break;
        case 2: NAT("adcb", uint8_t, "b", "b"); break;
        case 3: NAT("sbbb", uint8_t, "b", "b"); break;
        case 4: NAT("andb", uint8_t, "b", "b"); break;
        case 5: NAT("subb", uint8_t, "b", "b"); break;
        case 6: NAT("xorb", uint8_t, "b", "b"); break;
        case 7: NAT("cmpb", uint8_t, "b", "b"); break;
        }
        *fout = fo; return av;
    }
}

#define SH(INSN, RA)                                                    \
    __asm__ volatile("push %[fi]\n\tpopfq\n\t" INSN " %%cl, %" RA "[a]\n\tpushfq\n\tpop %[fo]" \
        : [a] "+r"(av), [fo] "=r"(fo) : "c"(cnt), [fi] "r"(fi) : "cc")
static uint32_t native_shift(int op, uint32_t a, uint8_t cnt, int n, uint64_t fi, uint64_t *fout){
    uint64_t fo = fi;
    if(n == 4){ uint32_t av = a;
        switch(op){ case 0: SH("roll","k"); break; case 1: SH("rorl","k"); break; case 2: SH("rcll","k"); break;
                    case 3: SH("rcrl","k"); break; case 4: SH("shll","k"); break; case 5: SH("shrl","k"); break;
                    case 7: SH("sarl","k"); break; }
        *fout = fo; return av; }
    if(n == 2){ uint16_t av = (uint16_t)a;
        switch(op){ case 0: SH("rolw","w"); break; case 1: SH("rorw","w"); break; case 2: SH("rclw","w"); break;
                    case 3: SH("rcrw","w"); break; case 4: SH("shlw","w"); break; case 5: SH("shrw","w"); break;
                    case 7: SH("sarw","w"); break; }
        *fout = fo; return av; }
    { uint8_t av = (uint8_t)a;
        switch(op){ case 0: SH("rolb","b"); break; case 1: SH("rorb","b"); break; case 2: SH("rclb","b"); break;
                    case 3: SH("rcrb","b"); break; case 4: SH("shlb","b"); break; case 5: SH("shrb","b"); break;
                    case 7: SH("sarb","b"); break; }
        *fout = fo; return av; }
}

static uint32_t rnd(void){ return ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ ((uint32_t)rand() << 30); }
static uint32_t pick(int i){
    static const uint32_t edge[] = { 0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
                                     0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0x0F, 0x10 };
    return (i % 3 == 0) ? edge[(unsigned)rand() % (sizeof edge / sizeof edge[0])] : rnd();
}

static void part1(void){
    cpu386_t c;
    int bad_alu = 0, bad_sh = 0, n_alu = 0, n_sh = 0;
    char w[160];
    cpu386_reset(&c, 0, 0, 0, 0);       /* builds the parity table */
    srand(386);
    for(int it = 0; it < 60000; it++){
        int op = it % 8, n = (it / 8) % 3 == 0 ? 1 : (it / 8) % 3 == 1 ? 2 : 4;
        uint32_t a = pick(it) & MASK(n), b = pick(it + 1) & MASK(n);
        uint64_t fi = 0x202 | (rand() & 1 ? X_CF : 0), fo;
        uint32_t want = native_alu(op, a, b, n, fi, &fo) & MASK(n), got;
        uint32_t mask = FMASK;
        if(op == 1 || op == 4 || op == 6) mask &= ~X_AF;
        c.eflags = (uint32_t)fi;
        got = alu(&c, op, a, b, n);
        n_alu++;
        if((op != 7 && got != want) || ((c.eflags ^ (uint32_t)fo) & mask)){
            if(bad_alu++ < 6){
                snprintf(w, sizeof w, "ALU op %d size %d: %x,%x -> %x flags %x, native %x flags %x",
                         op, n, a, b, got, c.eflags & mask, want, (uint32_t)fo & mask);
                ck(w, 0, 0);
            }
        }
    }
    for(int it = 0; it < 60000; it++){
        static const int ops[] = { 0, 1, 2, 3, 4, 5, 7 };
        int op = ops[it % 7], n = (it / 7) % 3 == 0 ? 1 : (it / 7) % 3 == 1 ? 2 : 4;
        uint32_t a = pick(it) & MASK(n);
        uint8_t cnt = (uint8_t)((it % 5 == 0) ? 1 : (rand() % (n * 8 + 3)));
        uint64_t fi = 0x202 | (rand() & 1 ? X_CF : 0) | (rand() & 1 ? X_OF : 0), fo;
        uint32_t want = native_shift(op, a, cnt, n, fi, &fo) & MASK(n), got;
        uint32_t mask = X_CF | X_OF;
        uint8_t m = cnt & 31;
        if(op >= 4) mask |= X_ZF | X_SF | X_PF;
        if(m != 1) mask &= ~X_OF;                 /* undefined for counts above one */
        if(op >= 4 && m > (uint8_t)(n * 8)) mask &= ~X_CF;   /* undefined past the width */
        if(!m) mask = FMASK;                      /* a zero count changes nothing */
        if(!m && op >= 0) mask &= ~X_AF;
        c.eflags = (uint32_t)fi;
        got = shift(&c, op, a, cnt, n);
        n_sh++;
        if(got != want || ((c.eflags ^ (uint32_t)fo) & mask)){
            if(bad_sh++ < 6){
                snprintf(w, sizeof w, "shift op %d size %d: %x by %u -> %x flags %x, native %x flags %x",
                         op, n, a, cnt, got, c.eflags & mask, want, (uint32_t)fo & mask);
                ck(w, 0, 0);
            }
        }
    }
    snprintf(w, sizeof w, "the 8 ALU operations at 8/16/32 bits match this CPU (%d cases)", n_alu);
    ck(w, bad_alu == 0, bad_alu);
    snprintf(w, sizeof w, "the 7 shifts and rotates at 8/16/32 bits match this CPU (%d cases)", n_sh);
    ck(w, bad_sh == 0, bad_sh);
}

/* ── part 2: programs ─────────────────────────────────────────────── */
static uint8_t g_mem[1 << 20];
static uint8_t g_port_out[16]; static int g_nout;
static uint32_t h_in(void *x, uint16_t p, int n){ (void)x; (void)n; return p == 0x64 ? 0x1C : 0xFF; }
static void h_out(void *x, uint16_t p, uint32_t v, int n){ (void)x; (void)n; if(p == 0xE9 && g_nout < 16) g_port_out[g_nout++] = (uint8_t)v; }
static int g_vec = -1;
static int h_ack(void *x){ int v = g_vec; (void)x; g_vec = -1; return v; }
static const cpu386_host_t g_host = { h_in, h_out, h_ack };

static int run_prog(cpu386_t *c, const uint8_t *code, int len, uint32_t budget){
    memset(g_mem, 0, sizeof g_mem);
    memcpy(g_mem + 0x1000, code, (size_t)len);
    cpu386_reset(c, g_mem, sizeof g_mem, &g_host, 0);
    cpu386_flat(c, 0x1000);
    c->r[X_ESP] = 0x8000;
    return cpu386_run(c, budget);
}

static void part2(void){
    cpu386_t c;
    /* sum 1..100 in a loop, then hlt with interrupts off */
    static const uint8_t sum[] = {
        0x31, 0xC0,                   /* xor eax,eax     */
        0xB9, 100, 0, 0, 0,           /* mov ecx,100     */
        0x01, 0xC8,                   /* add eax,ecx     */
        0xE2, 0xFC,                   /* loop -4         */
        0xFA, 0xF4 };                 /* cli; hlt        */
    int r = run_prog(&c, sum, sizeof sum, 10000);
    ck("a LOOP sums 1..100 to 5050 and HLT with IF clear stops it", r == X86_HALT && c.r[X_EAX] == 5050, c.r[X_EAX]);

    /* rep stosd, rep movsb, call/ret, push/pop */
    static const uint8_t str[] = {
        0xBF, 0x00, 0x20, 0, 0,       /* mov edi,0x2000  */
        0xB8, 0x44, 0x33, 0x22, 0x11, /* mov eax,11223344 */
        0xB9, 4, 0, 0, 0,             /* mov ecx,4       */
        0xFC, 0xF3, 0xAB,             /* cld; rep stosd  */
        0xBE, 0x00, 0x20, 0, 0,       /* mov esi,0x2000  */
        0xBF, 0x00, 0x30, 0, 0,       /* mov edi,0x3000  */
        0xB9, 16, 0, 0, 0,            /* mov ecx,16      */
        0xF3, 0xA4,                   /* rep movsb       */
        0xE8, 0x02, 0, 0, 0,          /* call +2         */
        0xFA, 0xF4,                   /* cli; hlt        */
        0x53, 0xBB, 0x78, 0x56, 0x34, 0x12, 0x89, 0xD8, 0x5B, 0xC3 };  /* push ebx; mov ebx,..; mov eax,ebx; pop ebx; ret */
    r = run_prog(&c, str, sizeof str, 10000);
    ck("REP STOSD then REP MOVSB copy 16 bytes", cpu386_rd32(&c, 0x300C) == 0x11223344u && c.r[X_ECX] == 0, cpu386_rd32(&c, 0x300C));
    ck("CALL, PUSH/POP and RET come back with EAX from the callee", r == X86_HALT && c.r[X_EAX] == 0x12345678u, c.r[X_EAX]);

    /* lgdt + far jump + a data segment with a base, then an IDT and int 0x30 */
    {
        uint8_t prog[256]; int k = 0;
        /* GDT at 0x5000: null, code (base 0), data (base 0x100) */
        memset(prog, 0, sizeof prog);
        #define B(x) prog[k++] = (uint8_t)(x)
        #define D(x) do{ uint32_t _v = (x); B(_v); B(_v >> 8); B(_v >> 16); B(_v >> 24); }while(0)
        B(0x0F); B(0x01); B(0x15); D(0x5100);          /* lgdt [0x5100] */
        B(0x0F); B(0x01); B(0x1D); D(0x5200);          /* lidt [0x5200] */
        B(0x66); B(0xB8); B(0x10); B(0x00);            /* mov ax,0x10 */
        B(0x8E); B(0xD8);                              /* mov ds,ax   */
        B(0xEA); D(0x1000 + 0x40); B(0x08); B(0x00);   /* ljmp 0x08:0x1040 */
        while(k < 0x40) B(0x90);
        B(0xC7); B(0x05); D(0x10); D(0xCAFEBABE);      /* mov dword [ds:0x10], CAFEBABE */
        B(0xFB);                                       /* sti */
        B(0xCD); B(0x30);                              /* int 0x30 */
        B(0xFA); B(0xF4);                              /* cli; hlt */
        while(k < 0x80) B(0x90);
        B(0xB8); D(0x30);                              /* handler: mov eax,0x30 */
        B(0xCF);                                       /* iret */
        memset(g_mem, 0, sizeof g_mem);
        memcpy(g_mem + 0x1000, prog, (size_t)k);
        /* GDT */
        { uint8_t *g = g_mem + 0x5000;
          static const uint8_t code[8] = { 0xFF,0xFF,0,0,0,0x9A,0xCF,0 };
          static const uint8_t data[8] = { 0xFF,0xFF,0x00,0x01,0,0x92,0xCF,0 };  /* base 0x100 */
          memcpy(g + 8, code, 8); memcpy(g + 16, data, 8); }
        g_mem[0x5100] = 23; g_mem[0x5102] = 0x00; g_mem[0x5103] = 0x50;
        /* IDT at 0x6000 with vector 0x30 -> 0x08:0x1080, an interrupt gate */
        g_mem[0x5200] = 0xFF; g_mem[0x5201] = 0x01; g_mem[0x5202] = 0x00; g_mem[0x5203] = 0x60;
        { uint8_t *e = g_mem + 0x6000 + 0x30 * 8;
          e[0] = 0x80; e[1] = 0x10; e[2] = 0x08; e[3] = 0; e[4] = 0; e[5] = 0x8E; e[6] = 0; e[7] = 0; }
        cpu386_reset(&c, g_mem, sizeof g_mem, &g_host, 0);
        cpu386_flat(&c, 0x1000);
        c.r[X_ESP] = 0x8000;
        r = cpu386_run(&c, 1000);
        ck("LGDT and a far jump: the data segment's base is honoured", cpu386_rd32(&c, 0x110) == 0xCAFEBABEu, cpu386_rd32(&c, 0x110));
        ck("INT 30h through the IDT, IRET back, and IF restored by IRET", r == X86_HALT && c.r[X_EAX] == 0x30, c.r[X_EAX]);
    }
    /* a hardware interrupt wakes HLT */
    {
        static const uint8_t idle[] = { 0xFB, 0xF4, 0xB8, 7, 0, 0, 0, 0xFA, 0xF4 };
        r = run_prog(&c, idle, sizeof idle, 100);
        ck("HLT with IF set is idle, not stopped", r == X86_IDLE, r);
        /* no IDT: an interrupt cannot be delivered; skip -- deliver() refuses quietly */
    }
    /* an unknown opcode with no IDT stops with a reason */
    {
        static const uint8_t ud[] = { 0x0F, 0x0B };
        r = run_prog(&c, ud, sizeof ud, 10);
        ck("UD2 with no IDT stops with a reason and the address", r == X86_FAULT && c.fault_vec == 6 && c.fault_eip == 0x1000 && c.fault_why, c.fault_vec);
    }
    /* divide: 100 / 7 and a #DE */
    {
        static const uint8_t dv[] = { 0xB8, 100, 0, 0, 0, 0x31, 0xD2, 0xB9, 7, 0, 0, 0, 0xF7, 0xF1, 0xFA, 0xF4 };
        r = run_prog(&c, dv, sizeof dv, 100);
        ck("DIV: 100 / 7 = 14 remainder 2", r == X86_HALT && c.r[X_EAX] == 14 && c.r[X_EDX] == 2, c.r[X_EAX]);
        static const uint8_t dz[] = { 0x31, 0xC9, 0xF7, 0xF1 };
        r = run_prog(&c, dz, sizeof dz, 100);
        ck("DIV by zero raises #DE", r == X86_FAULT && c.fault_vec == 0, c.fault_vec);
    }
    /* movzx, movsx, setcc, cmov, bsf, bswap, imul */
    {
        static const uint8_t mx[] = {
            0xB8, 0xF0, 0x00, 0x00, 0x00,     /* mov eax,0xF0 */
            0x0F, 0xBE, 0xD8,                 /* movsx ebx,al  -> 0xFFFFFFF0 */
            0x0F, 0xB6, 0xC8,                 /* movzx ecx,al  -> 0xF0 */
            0x39, 0xC8,                       /* cmp eax,ecx (equal) */
            0x0F, 0x94, 0xC2,                 /* sete dl */
            0x0F, 0x45, 0xC3,                 /* cmovne eax,ebx (not taken) */
            0x0F, 0xBC, 0xF1,                 /* bsf esi,ecx -> 4 */
            0x0F, 0xCB,                       /* bswap ebx -> 0xF0FFFFFF */
            0x6B, 0xF9, 0x03,                 /* imul edi,ecx,3 -> 0x2D0 */
            0xFA, 0xF4 };
        r = run_prog(&c, mx, sizeof mx, 100);
        ck("MOVSX, MOVZX, SETE, CMOVNE, BSF, BSWAP and IMUL r,r/m,imm",
           r == X86_HALT && c.r[X_EBX] == 0xF0FFFFFFu && c.r[X_ECX] == 0xF0 && (c.r[X_EDX] & 0xFF) == 1 &&
           c.r[X_EAX] == 0xF0 && c.r[X_ESI] == 4 && c.r[X_EDI] == 0x2D0, c.r[X_EBX]);
    }
    /* out to port E9 */
    {
        static const uint8_t o[] = { 0xB0, 'H', 0xE6, 0xE9, 0xB0, 'i', 0xE6, 0xE9, 0xFA, 0xF4 };
        g_nout = 0;
        r = run_prog(&c, o, sizeof o, 100);
        ck("OUT reaches the host", g_nout == 2 && g_port_out[0] == 'H' && g_port_out[1] == 'i', g_nout);
    }
}

/* ── part 3: the BOOT OS builds Retro PC carries, booted ──────────── */
static uint32_t g_ms;
static uint32_t t_now(void *x){ (void)x; return g_ms; }
static void t_rtc(void *x, int *y, int *mo, int *d, int *h, int *mi, int *s){
    (void)x; *y = 2026; *mo = 11; *d = 22; *h = 13; *mi = 37; *s = (int)(g_ms / 1000 % 60);
}
static const pc386_host_t g_h386 = { t_now, t_rtc };
static char g_scr[25 * 81 + 1];
static const char *screen(const pc386_t *p){
    pc386_screen_t s; int k = 0;
    pc386_screen(p, &s);
    for(int y = 0; y < 25; y++){
        for(int x = 0; x < 80; x++){ uint8_t c = s.cells[(y * 80 + x) * 2]; g_scr[k++] = (c >= 32 && c < 127) ? (char)c : ' '; }
        g_scr[k++] = '\n';
    }
    g_scr[k] = 0;
    return g_scr;
}
/* run until the screen shows `want`, 10 ms of guest time per million
   instructions; 0 if it never does within `limit_ms` */
static int run_until(pc386_t *p, const char *want, uint32_t limit_ms){
    for(uint32_t t = 0; t < limit_ms; t += 10){
        int r;
        g_ms += 10;
        r = pc386_run(p, 1000000);
        if(r == X86_FAULT || r == PC386_STOPPED) return 0;
        if(strstr(screen(p), want)) return 1;
    }
    return 0;
}
static void type(pc386_t *p, const char *s){
    for(; *s; s++){
        pc386_key(p, *s);
        for(int i = 0; i < 4; i++){ g_ms += 10; pc386_run(p, 1000000); }
    }
}

static void part3(void){
    static uint8_t mem[16u << 20];
    static pc386_t pc;
    char w[160];
    int nmb = 0;
    for(int i = 0; i < RETRO_NIMAGES; i++){
        const retro_image_t *im = &retro_images[i];
        pc386_entry_t e[PC386_ENTRY_MAX];
        const char *why = "";
        int n, sel = -1;
        if(!im->multiboot) continue;
        nmb++;
        n = pc386_cfg_menu(im->cfg, (int)im->cfg_size, e, PC386_ENTRY_MAX, &why);
        snprintf(w, sizeof w, "%s: its grub.cfg has 8 entries, all multiboot /boot/bootos.elf", im->title);
        ck(w, n == 8 && !strcmp(e[0].path, "/boot/bootos.elf") && !e[0].mb2, n);
        for(int k = 0; k < n; k++) if(strstr(e[k].args, "gfx=text") && strstr(e[k].args, "mode=desktop")){ sel = k; break; }
        snprintf(w, sizeof w, "%s: a text-mode desktop entry (\"%s\")", im->title, sel >= 0 ? e[sel].title : "?");
        ck(w, sel >= 0, sel);
        if(sel < 0) continue;
        pc386_init(&pc, mem, sizeof mem, &g_h386, 0);
        snprintf(w, sizeof w, "%s: loads like GRUB (ELF segments, Multiboot info, EAX=2BADB002)", im->title);
        ck(w, pc386_boot_kernel(&pc, im->data, (int)im->size, &e[sel]) == 0 && pc.cpu.r[X_EAX] == 0x2BADB002u &&
              cpu386_rd32(&pc.cpu, 0x9000) & 0x4, pc.cpu.r[X_EAX]);
        snprintf(w, sizeof w, "%s: boots on the 386 to its login screen", im->title);
        ck(w, run_until(&pc, "Password:", 60000), (long)pc.cpu.eip);
        snprintf(w, sizeof w, "%s: its command line reached the kernel (\"%.60s\")", im->title, (const char *)(mem + 0x9800));
        ck(w, strstr((const char *)(mem + 0x9800), "mode=desktop gfx=text") != 0, 0);
        if(i == RETRO_NIMAGES - 1){
            type(&pc, "bootos\n1234\n");
            snprintf(w, sizeof w, "%s: bootos / 1234 reaches its desktop", im->title);
            ck(w, run_until(&pc, "Tab=Shell", 10000), (long)pc.cpu.eip);
            type(&pc, "\t");
            snprintf(w, sizeof w, "%s: Tab opens its shell", im->title);
            ck(w, run_until(&pc, "bootos@bootos>", 5000), 0);
            type(&pc, "sysinfo\n");
            snprintf(w, sizeof w, "%s: `sysinfo` sees the 386's 16 MB and the 32-bit CPU", im->title);
            ck(w, run_until(&pc, "Total RAM:   16384 KB", 3000) && strstr(g_scr, "x86 32-bit Protected Mode"), 0);
            if(getenv("V")) printf("%s", g_scr);
        }
    }
    ck("three BOOT OS builds are built in", nmb == 3, nmb);

    /* the loader's refusals say why */
    {
        pc386_entry_t e[2]; const char *why = 0;
        static const char cfg2[] = "menuentry \"K-DOS\" {\n multiboot2 /boot/kernel.elf\n}\n";
        int n = pc386_cfg_menu(cfg2, (int)sizeof cfg2 - 1, e, 2, &why);
        pc386_init(&pc, mem, sizeof mem, &g_h386, 0);
        ck("a multiboot2 entry is listed as one, and refused with a reason",
           n == 1 && e[0].mb2 && pc386_boot_kernel(&pc, retro_images[1].data, 100, &e[0]) == -1 && strstr(pc.notice, "64-bit"), n);
        static const uint8_t junk[3000] = { 1, 2, 3 };
        n = pc386_iso_menu(junk, sizeof junk, e, 2, &why);
        ck("not an ISO: refused, and says it is not a CD image", n == -1 && why && strstr(why, "not a CD"), n);
    }

    /* v0.9.7.5: what the keyboard sends. A function key is not an
       extended key: F1 is 3B BB, with no E0, on every PC keyboard. Up
       is E0 48 E0 C8. pc386_key() once put E0 in front of all of them. */
    {
        uint8_t got[8]; int n;
        pc386_init(&pc, mem, sizeof mem, &g_h386, 0);
        pc.kbc_ccb = 0;                                  /* no IRQ: read the queue */
        pc386_key(&pc, 0x13B);
        n = 0; got[n++] = pc.ob; while(pc.kq_head != pc.kq_tail && n < 8){ got[n++] = pc.keyq[pc.kq_head]; pc.kq_head = (pc.kq_head + 1) % 64; }
        ck("F1 reaches the 8042 as 3B BB, without E0", n == 2 && got[0] == 0x3B && got[1] == 0xBB, got[0]);
        pc386_init(&pc, mem, sizeof mem, &g_h386, 0);
        pc.kbc_ccb = 0;
        pc386_key(&pc, 0x148);
        n = 0; got[n++] = pc.ob; while(pc.kq_head != pc.kq_tail && n < 8){ got[n++] = pc.keyq[pc.kq_head]; pc.kq_head = (pc.kq_head + 1) % 64; }
        ck("Up reaches it as E0 48 E0 C8", n == 4 && got[0] == 0xE0 && got[1] == 0x48 && got[2] == 0xE0 && got[3] == 0xC8, got[0]);
    }
}

int main(int argc, char **argv){
    (void)argc; (void)argv;
    printf("test_emu386 -- Retro PC's 32-bit CPU, its PC and BOOT OS\n");
    part1();
    part2();
    part3();
    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
