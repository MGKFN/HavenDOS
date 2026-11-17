/* ════════════════════════════════════════════════════════════════════
   tests/emu86_sst.c -- emu/cpu86.c against the 8088 itself.  v0.9.7.

   The SingleStepTests 8088 suite (github.com/SingleStepTests/8088) is
   ten thousand instructions per opcode, each recorded from a real
   8088: the registers and memory before, and after. Nothing in it was
   written by this project, which is the point -- a CPU judged only by
   tests its own author wrote agrees with itself, not with Intel.

   tools/check_emu86_sst.py downloads the suite, turns each opcode's
   JSON into the flat records read below, builds this file and runs
   it. It is a deliberate act, like the injection harnesses, not part
   of `make test`: it needs the network and several hundred megabytes.

   Record format (little-endian), one per test:
     u16 regs_before[14]      ax bx cx dx cs ss ds es sp bp si di ip flags
     u16 nram, then nram x { u32 addr, u8 value }
     u16 regs_after[14]
     u16 nram, then nram x { u32 addr, u8 value }
     u16 flag_mask            undefined flags cleared, from metadata.json
     u8  name_len, name
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu86.h"

static uint8_t mem[EMU86_MEM];

static uint8_t  in8(void *c, uint16_t p){ (void)c; (void)p; return 0xFF; }
static void     out8(void *c, uint16_t p, uint8_t v){ (void)c; (void)p; (void)v; }
static int      bios(void *c, emu86_t *e, uint8_t v){ (void)c; (void)e; (void)v; return BIOS_DONE; }
static const emu86_host_t host = { in8, out8, bios };

static int rd(FILE *f, void *p, size_t n){ return fread(p, 1, n, f) == n; }

/* the suite's register order -> this CPU's */
static void load(emu86_t *e, const uint16_t r[14]){
    e->r[R_AX] = r[0]; e->r[R_BX] = r[1]; e->r[R_CX] = r[2]; e->r[R_DX] = r[3];
    e->sr[S_CS] = r[4]; e->sr[S_SS] = r[5]; e->sr[S_DS] = r[6]; e->sr[S_ES] = r[7];
    e->r[R_SP] = r[8]; e->r[R_BP] = r[9]; e->r[R_SI] = r[10]; e->r[R_DI] = r[11];
    e->ip = r[12]; e->flags = r[13];
}
static void save(const emu86_t *e, uint16_t r[14]){
    r[0] = e->r[R_AX]; r[1] = e->r[R_BX]; r[2] = e->r[R_CX]; r[3] = e->r[R_DX];
    r[4] = e->sr[S_CS]; r[5] = e->sr[S_SS]; r[6] = e->sr[S_DS]; r[7] = e->sr[S_ES];
    r[8] = e->r[R_SP]; r[9] = e->r[R_BP]; r[10] = e->r[R_SI]; r[11] = e->r[R_DI];
    r[12] = e->ip; r[13] = e->flags;
}
static const char *RN[14] = { "ax","bx","cx","dx","cs","ss","ds","es","sp","bp","si","di","ip","flags" };

int main(int argc, char **argv){
    long pass = 0, fail = 0;
    int shown = 0, verbose = argc > 2;
    FILE *f = fopen(argv[1], "rb");
    if(!f){ perror(argv[1]); return 2; }
    for(;;){
        uint16_t before[14], after[14], got[14], n, mask;
        uint32_t addr; uint8_t val, nl; char name[256];
        static uint32_t waddr[4096]; static uint8_t wval[4096];
        uint16_t nw;
        emu86_t e;
        if(!rd(f, before, sizeof before)) break;
        memset(mem, 0, sizeof mem);
        if(!rd(f, &n, 2)) break;
        for(int i = 0; i < n; i++){ rd(f, &addr, 4); rd(f, &val, 1); mem[addr & 0xFFFFF] = val; }
        rd(f, after, sizeof after);
        rd(f, &nw, 2);
        for(int i = 0; i < nw; i++){ rd(f, &waddr[i], 4); rd(f, &wval[i], 1); }
        rd(f, &mask, 2);
        rd(f, &nl, 1); rd(f, name, nl); name[nl] = 0;

        emu86_reset(&e, mem, &host, 0);
        load(&e, before);
        int r = emu86_run(&e, 1);
        save(&e, got);

        int bad = r == EMU_FAULT;
        for(int i = 0; i < 14 && !bad; i++){
            uint16_t m = i == 13 ? mask : 0xFFFF;
            if((got[i] & m) != (after[i] & m)) bad = 1;
        }
        for(int i = 0; i < nw && !bad; i++)
            if(mem[waddr[i] & 0xFFFFF] != wval[i]) bad = 1;
        if(!bad){ pass++; continue; }
        fail++;
        if(shown < (verbose ? 20 : 3)){
            shown++;
            printf("    FAIL %s%s\n", name, r == EMU_FAULT ? "  (stopped: fault)" : "");
            for(int i = 0; i < 14; i++){
                uint16_t m = i == 13 ? mask : 0xFFFF;
                if((got[i] & m) != (after[i] & m))
                    printf("         %-5s got %04X want %04X (before %04X)\n", RN[i], got[i], after[i], before[i]);
            }
            for(int i = 0; i < nw; i++)
                if(mem[waddr[i] & 0xFFFFF] != wval[i])
                    printf("         [%05X] got %02X want %02X\n", waddr[i], mem[waddr[i] & 0xFFFFF], wval[i]);
        }
    }
    fclose(f);
    printf("  %ld/%ld\n", pass, pass + fail);
    return fail ? 1 : 0;
}
