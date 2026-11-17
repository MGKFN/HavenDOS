/* ════════════════════════════════════════════════════════════════════
   tests/test_emu86.c -- Retro PC: the CPU, the BIOS, and K-DOS.  v0.9.7.

   emu/cpu86.c and emu/pcbios.c compiled natively, against the bytes
   that ship: K-DOS Beta 1.1b comes from include/retro_images.h, the
   header the kernel itself is built with.

   WHERE THE EXPECTED ANSWERS COME FROM.
     - K-DOS's screens: QEMU, with SeaBIOS, booting the same image and
       given the same keys (17 Nov 2026). That includes its quirks:
       `taskman` leaves the screen half-drawn and returns to the login
       on the next key, and `save` writes the four bytes of its own
       input buffer to head 1 sector 10 (LBA 72) -- both reproduced by
       a real BIOS, so both are K-DOS and both are checked here as
       behaviour, not "fixed".
     - The 8086 core: tools/check_emu86_sst.py runs it against a real
       8088's recorded results (2.5 million instructions). That suite
       cannot see the 80186 and 386 additions, so they are here.
     - The guest programs below were assembled with NASM; the source is
       beside each one.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu86.h"
#include "pcbios.h"
#include "retro_images.h"

static int total, fails;
static void check(const char *what, int ok){
    total++;
    if(!ok){ fails++; printf("  FAIL %s\n", what); }
}

/* ── a bare CPU, for instruction tests ────────────────────────────── */
static uint8_t cmem[EMU86_MEM];
static int bios_calls;
static uint8_t port_last_out; static uint16_t port_last_addr;
static uint8_t c_in8(void *c, uint16_t p){ (void)c; return (uint8_t)(p & 0xFF); }
static void    c_out8(void *c, uint16_t p, uint8_t v){ (void)c; port_last_addr = p; port_last_out = v; }
static int     c_bios(void *c, emu86_t *e, uint8_t v){ (void)c; (void)e; (void)v; bios_calls++; return BIOS_DONE; }
static const emu86_host_t chost = { c_in8, c_out8, c_bios };

/* load code at 0000:1000, stack at 0000:FFFE, run to HLT */
static int cpu_run(emu86_t *e, const uint8_t *code, int n){
    memset(cmem, 0, sizeof cmem);
    memcpy(cmem + 0x1000, code, (size_t)n);
    emu86_reset(e, cmem, &chost, 0);
    e->sr[S_CS] = 0; e->ip = 0x1000; e->r[R_SP] = 0xFFFE;
    return emu86_run(e, 100000);
}
static uint16_t w(uint32_t a){ return (uint16_t)(cmem[a] | (cmem[a + 1] << 8)); }

/* NASM: org 0x1000 */
static const uint8_t g_pusha[] = { 0xFA, 0xB8, 0x01, 0x00, 0xB9, 0x02, 0x00, 0xBA, 0x03, 0x00, 0xBB, 0x04, 0x00, 0xBD, 0x05, 0x00, 0xBE, 0x06, 0x00, 0xBF, 0x07, 0x00, 0x60, 0xB8, 0x00, 0x00, 0xB9, 0x00, 0x00, 0xBA, 0x00, 0x00, 0xBB, 0x00, 0x00, 0xBD, 0x00, 0x00, 0xBE, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x61, 0xF4 };
/* cli; mov bp,1111h; enter 8,0; mov ax,sp; mov bx,bp; leave; hlt */
static const uint8_t g_enter[] = { 0xFA, 0xBD, 0x11, 0x11, 0xC8, 0x08, 0x00, 0x00, 0x89, 0xE0, 0x89, 0xEB, 0xC9, 0xF4 };
/* cli; mov bx,-300; imul cx,bx,100; pushf; pop dx; imul si,bx,7; hlt */
static const uint8_t g_imul[] = { 0xFA, 0xBB, 0xD4, 0xFE, 0x6B, 0xCB, 0x64, 0x9C, 0x5A, 0x6B, 0xF3, 0x07, 0xF4 };
/* cli; mov ax,1; shl ax,33; mov bx,8001h; rol bx,4; hlt */
static const uint8_t g_shlimm[] = { 0xFA, 0xB8, 0x01, 0x00, 0xC1, 0xE0, 0x21, 0xBB, 0x01, 0x80, 0xC1, 0xC3, 0x04, 0xF4 };
/* cli; xor ax,ax; stc; jc near t1; mov ax,0BADh; t1: inc ax; clc; jc near t2; add ax,10h; t2: hlt */
static const uint8_t g_jcc16[] = { 0xFA, 0x31, 0xC0, 0xF9, 0x0F, 0x82, 0x03, 0x00, 0xB8, 0xAD, 0x0B, 0x40, 0xF8, 0x0F, 0x82, 0x03, 0x00, 0x83, 0xC0, 0x10, 0xF4 };
/* cli; mov ax,5; cmp ax,7; setl bl; setg bh; setz cl; hlt */
static const uint8_t g_setcc[] = { 0xFA, 0xB8, 0x05, 0x00, 0x83, 0xF8, 0x07, 0x0F, 0x9C, 0xC3, 0x0F, 0x9F, 0xC7, 0x0F, 0x94, 0xC1, 0xF4 };
/* cli; mov bl,80h; movzx ax,bl; movsx cx,bl; hlt */
static const uint8_t g_movzx[] = { 0xFA, 0xB3, 0x80, 0x0F, 0xB6, 0xC3, 0x0F, 0xBE, 0xCB, 0xF4 };
/* cli; mov ax,10h; bt ax,4; setc dl; bts ax,0; btr ax,4;
   mov word [600h],0; mov word [602h],0; mov cx,20; bts word [600h],cx; mov si,[602h]; hlt */
static const uint8_t g_bt[] = { 0xFA, 0xB8, 0x10, 0x00, 0x0F, 0xBA, 0xE0, 0x04, 0x0F, 0x92, 0xC2, 0x0F, 0xBA, 0xE8, 0x00, 0x0F, 0xBA, 0xF0, 0x04, 0xC7, 0x06, 0x00, 0x06, 0x00, 0x00, 0xC7, 0x06, 0x02, 0x06, 0x00, 0x00, 0xB9, 0x14, 0x00, 0x0F, 0xAB, 0x0E, 0x00, 0x06, 0x8B, 0x36, 0x02, 0x06, 0xF4 };
/* cli; mov ax,600h; bsf bx,ax; bsr cx,ax; xor dx,dx; bsf si,dx; setz dl; hlt */
static const uint8_t g_bsf[] = { 0xFA, 0xB8, 0x00, 0x06, 0x0F, 0xBC, 0xD8, 0x0F, 0xBD, 0xC8, 0x31, 0xD2, 0x0F, 0xBC, 0xF2, 0x0F, 0x94, 0xC2, 0xF4 };
/* cli; mov ax,1234h; mov bx,0ABCDh; shld ax,bx,4; mov cx,1234h; shrd cx,bx,4; hlt */
static const uint8_t g_shld[] = { 0xFA, 0xB8, 0x34, 0x12, 0xBB, 0xCD, 0xAB, 0x0F, 0xA4, 0xD8, 0x04, 0xB9, 0x34, 0x12, 0x0F, 0xAC, 0xD9, 0x04, 0xF4 };
/* cli; mov word [700h],10h; mov word [702h],2000h; lfs si,[700h];
   mov byte [fs:10h],5Ah; mov ax,2000h; mov ds,ax; mov al,[10h]; hlt */
static const uint8_t g_lfs[] = { 0xFA, 0xC7, 0x06, 0x00, 0x07, 0x10, 0x00, 0xC7, 0x06, 0x02, 0x07, 0x00, 0x20, 0x0F, 0xB4, 0x36, 0x00, 0x07, 0x64, 0xC6, 0x06, 0x10, 0x00, 0x5A, 0xB8, 0x00, 0x20, 0x8E, 0xD8, 0xA0, 0x10, 0x00, 0xF4 };
/* cli; nop; db 66h; mov ax,1; hlt */
static const uint8_t g_op66[] = { 0xFA, 0x90, 0x66, 0xB8, 0x01, 0x00, 0xF4 };
/* cli; cpuid; hlt */
static const uint8_t g_cpuid[] = { 0xFA, 0x0F, 0xA2, 0xF4 };
/* cli; db 0Fh,0FFh,10h (the BIOS trap, from the wrong segment); hlt */
static const uint8_t g_trapforge[] = { 0xFA, 0x0F, 0xFF, 0x10, 0xF4 };
/* cli; mov word [0],after; mov word [2],0; xor bl,bl; mov ax,5; div bl; nop; hlt;
   after: mov dx,0D0D0h; pop cx; hlt */
static const uint8_t g_divzero[] = { 0xFA, 0xC7, 0x06, 0x00, 0x00, 0x16, 0x10, 0xC7, 0x06, 0x02, 0x00, 0x00, 0x00, 0x30, 0xDB, 0xB8, 0x05, 0x00, 0xF6, 0xF3, 0x90, 0xF4, 0xBA, 0xD0, 0xD0, 0x59, 0xF4 };
/* cli; mov word [800h],10; mov word [802h],20; mov word [20],h; mov word [22],0;
   mov ax,15; bound ax,[800h]; mov ax,25; bi: bound ax,[800h]; hlt; h: mov dx,0B0B0h; pop cx; hlt */
static const uint8_t g_bound[] = { 0xFA, 0xC7, 0x06, 0x00, 0x08, 0x0A, 0x00, 0xC7, 0x06, 0x02, 0x08, 0x14, 0x00, 0xC7, 0x06, 0x14, 0x00, 0x28, 0x10, 0xC7, 0x06, 0x16, 0x00, 0x00, 0x00, 0xB8, 0x0F, 0x00, 0x62, 0x06, 0x00, 0x08, 0xB8, 0x19, 0x00, 0x62, 0x06, 0x00, 0x08, 0xF4, 0xBA, 0xB0, 0xB0, 0x59, 0xF4 };
/* cli; pushf; pop ax; hlt */
static const uint8_t g_pushf[] = { 0xFA, 0x9C, 0x58, 0xF4 };

static void test_cpu(void){
    emu86_t e;
    int r;
    printf("  the 80186 and the 386's 16-bit additions\n");

    r = cpu_run(&e, g_pusha, sizeof g_pusha);
    check("PUSHA/POPA: runs to HLT", r == EMU_HALT);
    check("PUSHA/POPA: every register comes back",
          e.r[R_AX] == 1 && e.r[R_CX] == 2 && e.r[R_DX] == 3 && e.r[R_BX] == 4 &&
          e.r[R_BP] == 5 && e.r[R_SI] == 6 && e.r[R_DI] == 7 && e.r[R_SP] == 0xFFFE);
    check("PUSHA pushes the SP from before it, in the 4th slot",
          w(0xFFFE - 10) == 0xFFFE && w(0xFFFE - 2) == 1 && w(0xFFFE - 16) == 7);

    r = cpu_run(&e, g_enter, sizeof g_enter);
    check("ENTER 8,0: SP is 8 below the saved BP", e.r[R_AX] == 0xFFFE - 2 - 8);
    check("ENTER: BP is the frame, at the saved BP", e.r[R_BX] == 0xFFFC && w(0xFFFC) == 0x1111);
    check("LEAVE restores BP and SP", e.r[R_BP] == 0x1111 && e.r[R_SP] == 0xFFFE);

    r = cpu_run(&e, g_imul, sizeof g_imul);
    check("IMUL r,r/m,imm8: -300 * 100 = -30000", e.r[R_CX] == (uint16_t)-30000);
    check("IMUL imm: no overflow at -30000, CF and OF clear", (e.r[R_DX] & (F_CF | F_OF)) == 0);
    check("IMUL imm8 sign-extends: -300*7 = -2100", e.r[R_SI] == (uint16_t)-2100);

    r = cpu_run(&e, g_shlimm, sizeof g_shlimm);
    check("SHL by immediate 33 is SHL by 1 (the 80186 masks the count)", e.r[R_AX] == 2);
    check("ROL by immediate 4", e.r[R_BX] == 0x0018);

    r = cpu_run(&e, g_jcc16, sizeof g_jcc16);
    check("0F 82 (JC rel16) taken jumps over, not-taken falls through",
          r == EMU_HALT && e.r[R_AX] == 0x11);

    r = cpu_run(&e, g_setcc, sizeof g_setcc);
    check("SETL/SETG/SETZ", (e.r[R_BX] & 0xFF) == 1 && (e.r[R_BX] >> 8) == 0 && (e.r[R_CX] & 0xFF) == 0);

    r = cpu_run(&e, g_movzx, sizeof g_movzx);
    check("MOVZX zero-extends, MOVSX sign-extends", e.r[R_AX] == 0x0080 && e.r[R_CX] == 0xFF80);

    r = cpu_run(&e, g_bt, sizeof g_bt);
    check("BT copies the bit to CF", (e.r[R_DX] & 0xFF) == 1);
    check("BTS sets, BTR clears", e.r[R_AX] == 0x0001);
    check("BTS with a register offset of 20 reaches the next word", e.r[R_SI] == 0x0010 && w(0x600) == 0);

    r = cpu_run(&e, g_bsf, sizeof g_bsf);
    check("BSF and BSR find the lowest and highest bits", e.r[R_BX] == 9 && e.r[R_CX] == 10);
    check("BSF of zero sets ZF", (e.r[R_DX] & 0xFF) == 1);

    r = cpu_run(&e, g_shld, sizeof g_shld);
    check("SHLD ax,bx,4", e.r[R_AX] == 0x234A);
    check("SHRD cx,bx,4", e.r[R_CX] == 0xD123);

    r = cpu_run(&e, g_lfs, sizeof g_lfs);
    check("LFS loads FS and the register", e.sr[S_FS] == 0x2000 && e.r[R_SI] == 0x0010);
    check("an FS: override writes through FS", (e.r[R_AX] & 0xFF) == 0x5A && cmem[0x20010] == 0x5A);

    r = cpu_run(&e, g_pushf, sizeof g_pushf);
    check("PUSHF has bits 12-15 set: an 8086/80186 to anything that asks",
          (e.r[R_AX] & 0xF000) == 0xF000);

    r = cpu_run(&e, g_divzero, sizeof g_divzero);
    check("DIV by zero raises INT 0", e.r[R_DX] == 0xD0D0);
    check("...and returns to the instruction AFTER the DIV (8086/80186)", e.r[R_CX] == 0x1014);

    r = cpu_run(&e, g_bound, sizeof g_bound);
    check("BOUND in range does nothing; out of range raises INT 5", e.r[R_DX] == 0xB0B0);
    check("...with the return address on the BOUND itself", e.r[R_CX] == 0x1023);

    printf("  what it refuses, and how it says so\n");
    r = cpu_run(&e, g_op66, sizeof g_op66);
    check("a 66h prefix stops it", r == EMU_FAULT);
    check("...at the instruction, not after it", e.fault_cs == 0 && e.fault_ip == 0x1002 && e.fault_op[0] == 0x66);
    check("...and the reason names the 32-bit register", e.fault_why && strstr(e.fault_why, "32-bit"));
    r = cpu_run(&e, g_cpuid, sizeof g_cpuid);
    check("CPUID stops it, by name", r == EMU_FAULT && strstr(e.fault_why, "CPUID"));

    bios_calls = 0;
    r = cpu_run(&e, g_trapforge, sizeof g_trapforge);
    check("the BIOS trap from outside segment F000 is not a BIOS call", bios_calls == 0 && r == EMU_FAULT);

    /* the ROM is read-only to the guest */
    memset(cmem, 0, sizeof cmem);
    emu86_reset(&e, cmem, &chost, 0);
    emu86_wr8(&e, 0xF1234, 0x77); emu86_wr8(&e, 0xEFFFF, 0x66);
    check("a write into F0000-FFFFF is ignored; below it is not", cmem[0xF1234] == 0 && cmem[0xEFFFF] == 0x66);
    check("addresses wrap at 1 MB, as with A20 off", emu86_lin(0xFFFF, 0x0010) == 0x00000);

    /* HLT with IF set waits for an IRQ; with IF clear it is the end */
    {   static const uint8_t idle[] = { 0xFB, 0xF4, 0xB8, 0x34, 0x12, 0xFA, 0xF4 };   /* sti; hlt; mov ax,1234h; cli; hlt */
        memset(cmem, 0, sizeof cmem);
        memcpy(cmem + 0x1000, idle, sizeof idle);
        cmem[0x20 + 0] = 0x00; cmem[0x20 + 1] = 0x20;                         /* INT 8 -> 0000:2000 */
        cmem[0x2000] = 0xCF;                                                 /* IRET */
        emu86_reset(&e, cmem, &chost, 0);
        e.sr[S_CS] = 0; e.ip = 0x1000; e.r[R_SP] = 0xFFFE;
        r = emu86_run(&e, 100);
        check("HLT with IF set is idle, not stopped", r == EMU_IDLE);
        emu86_irq(&e, 0);
        r = emu86_run(&e, 100);
        check("an IRQ wakes it and it carries on after the HLT", r == EMU_HALT && e.r[R_AX] == 0x1234);
        check("the IRQ stays in service until an EOI", e.irq_service == 1);
        emu86_irq(&e, 0);
        check("a second IRQ 0 is held while the first is in service", e.irq_pending == 1);
        emu86_eoi(&e);
        check("EOI ends it", e.irq_service == 0);
    }

    /* the 8259's rule, with the CPU running: a handler that has not
       sent its EOI is not entered again by its own next IRQ.
       sti; l: hlt; jmp l -- and at 0000:2000, inc word [500h]; iret */
    {   static const uint8_t spin[] = { 0xFB, 0xF4, 0xEB, 0xFD };
        static const uint8_t isr[]  = { 0xFF, 0x06, 0x00, 0x05, 0xCF };
        memset(cmem, 0, sizeof cmem);
        memcpy(cmem + 0x1000, spin, sizeof spin);
        memcpy(cmem + 0x2000, isr, sizeof isr);
        cmem[0x20] = 0x00; cmem[0x21] = 0x20;
        emu86_reset(&e, cmem, &chost, 0);
        e.sr[S_CS] = 0; e.ip = 0x1000; e.r[R_SP] = 0xFFFE;
        emu86_run(&e, 50);
        emu86_irq(&e, 0); emu86_run(&e, 50);
        emu86_irq(&e, 0); emu86_run(&e, 50);
        emu86_irq(&e, 0); emu86_run(&e, 50);
        check("IRQ 0 raised three times without an EOI: the handler ran once", w(0x500) == 1);
        emu86_eoi(&e); emu86_run(&e, 50);
        check("...and the held one is delivered after the EOI", w(0x500) == 2);
    }
}

/* ── a whole PC ───────────────────────────────────────────────────── */
static uint32_t T;
static uint32_t now(void *c){ (void)c; return T; }
static void rtc(void *c, int *y, int *mo, int *d, int *h, int *mi, int *s){
    (void)c; *y = 2026; *mo = 11; *d = 17; *h = 10; *mi = 45; *s = 30;
}
static const pc_host_t phost = { now, rtc };
static uint8_t fd[PC_FD_BYTES], hd[PC_HD_BYTES], *pmem;
static pc_t pc;

static int pc_go(int frames){
    int r = EMU_BUDGET;
    for(int i = 0; i < frames; i++){
        T += 5;
        r = pc_run(&pc, 20000);
        if(r == EMU_FAULT || r == EMU_HALT || r == PC_STOPPED) return r;
    }
    return r;
}
/* what HavenDOS's keyboard drivers deliver, '\n' for Enter included --
   the suite once typed 13 for it, and so could not see that pc_key()
   took HavenDOS's Enter for Ctrl+J */
static void type(const char *s){ for(; *s; s++){ pc_key(&pc, *s); pc_go(20); } pc_go(50); }

static void row(int y, char *out){
    pc_screen_t s; pc_screen(&pc, &s);
    int n = 0;
    for(int x = 0; x < s.cols; x++) out[n++] = (char)s.cells[(y * s.cols + x) * 2];
    while(n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}
static int row_is(int y, const char *want){ char b[100]; row(y, b); return strcmp(b, want) == 0; }
static int screen_has(const char *want){
    for(int y = 0; y < 25; y++){ char b[100]; row(y, b); if(strstr(b, want)) return 1; }
    return 0;
}

static int boot_bytes(const uint8_t *code, int n){
    memset(fd, 0, sizeof fd);
    memcpy(fd, code, (size_t)n);
    fd[510] = 0x55; fd[511] = 0xAA;
    pc_init(&pc, pmem, &phost, 0, fd, PC_FD_BYTES, 512, hd, PC_HD_BYTES);
    return pc_boot(&pc);
}
static int boot_kdos(void){
    memset(fd, 0, sizeof fd);
    memcpy(fd, retro_images[0].data, retro_images[0].size);
    pc_init(&pc, pmem, &phost, 0, fd, PC_FD_BYTES, retro_images[0].size, hd, PC_HD_BYTES);
    return pc_boot(&pc);
}

static void test_kdos(void){
    char b[100];
    pc_screen_t s;
    int r;
    printf("  K-DOS Beta 1.1b, against QEMU with a real BIOS\n");
    check("the shipped image is the archived one", retro_images[0].size == 25600 &&
          strcmp(retro_images[0].id, "KDOS11B") == 0);
    memset(hd, 0, sizeof hd);
    check("it boots", boot_kdos() == 0);
    r = pc_go(100);
    check("and waits for a key rather than stopping", r == EMU_WAIT);
    check("the login screen", row_is(0, "K-DOS-Beta-1.1b SECURITY") && row_is(1, "User:"));
    pc_screen(&pc, &s);
    check("the cursor after 'User: '", s.cx == 6 && s.cy == 1 && s.cursor_on);

    type("admin\n"); type("nope\n");
    check("a wrong password: LOGIN FAILED! and the prompt again",
          row_is(2, "LOGIN FAILED!") && row_is(3, "User:"));

    type("admin\n"); type("1234\n");
    check("admin / 1234 opens the shell", row_is(1, "K-DOS-Beta-1.1b Active Shell.") && row_is(3, "K:\\SYSTEM32>"));
    row(24, b);
    check("the command bar on row 24", strstr(b, "TIME | CALC | USER | TASKMAN") != 0);
    pc_screen(&pc, &s);
    check("...drawn black on grey (attribute 70h)", s.cells[(24 * 80 + 2) * 2 + 1] == 0x70);

    type("help\n");
    check("help lists the ten commands", row_is(4, "Commands: time, calc, user, taskman, sfc, fdisk, format, save, load, cls"));
    type("time\n");
    check("time reads the clock through INT 1Ah (BCD)", row_is(5, "K:\\SYSTEM32> time10:45"));
    type("xyz\n");
    check("an unknown command: ?", row_is(7, "?"));

    /* save after time: `time` leaves the clock's seconds in DH and
       `save` never sets DH, so it asks for head 30h of a 16-head disk.
       The write fails and K-DOS says [OK] regardless. QEMU's disk is
       empty after the same keys. */
    type("user\n"); type("1\n"); type("abc\n");
    check("user: a new password, [OK]", screen_has("New Pass: abc [OK]"));
    memset(hd, 0, sizeof hd); pc.hd_dirty = 0;
    type("save\n");
    {   int any = 0; for(int i = 0; i < (int)sizeof hd; i++) any |= hd[i];
        check("save after time: [OK] on screen, nothing on the disk -- as QEMU",
              screen_has("save [OK]") && !any && !pc.hd_dirty); }

    /* the same keys QEMU was given: admin, 1234, user, 1, abc, save */
    memset(hd, 0, sizeof hd);
    boot_kdos(); pc_go(100); type("admin\n"); type("1234\n");
    type("user\n"); type("1\n"); type("abc\n"); type("save\n");
    check("save says [OK]", screen_has("save [OK]"));
    check("save writes drive 80h, and says so to the host", pc.hd_dirty == 1);
    check("...at head 1 sector 10 (LBA 72): its input buffer, 'save', as QEMU's disk shows",
          memcmp(hd + 72 * 512, "save", 4) == 0);
    {   int other = 0;
        for(int i = 0; i < (int)sizeof hd; i++) if(hd[i] && (i < 72 * 512 || i >= 72 * 512 + 4)) other++;
        check("...and nothing else on the disk", other == 0); }
    check("the floppy is still the image it booted from",
          memcmp(fd, retro_images[0].data, retro_images[0].size) == 0);

    type("format\n"); type("y");
    check("format: [OK]", screen_has("Format Sector 10? (y):  [OK]"));
    check("format clears what save wrote", hd[72 * 512] == 0);

    /* the taskman quirk, screen for screen as QEMU draws it */
    boot_kdos(); pc_go(100); type("admin\n"); type("1234\n"); type("help\n"); type("time\n"); type("taskman\n");
    check("taskman: the prompt cut to K:\\SYSTEM3, as on a real BIOS", row_is(5, "K:\\SYSTEM3"));
    check("taskman: its line, as on a real BIOS", row_is(6, "K:\\SYSTEM3  TASKMGR: KERNEL.SYS [RUNNING]"));
    type("s");
    check("taskman: the next key goes back to the login, as on a real BIOS",
          row_is(0, "K-DOS-Beta-1.1b SECURITY") && row_is(1, "User:"));
    check("K-DOS never asked for anything the BIOS does not provide", pc.notices == 0);
}

/* NASM, org 7C00h, each a boot sector */
/* cli; xor ax,ax; mov ds,ax; mov word [70h],h; mov word [72h],0; sti; l: hlt; jmp l; h: inc word [500h]; iret */
static const uint8_t b_hook1c[] = { 0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0xC7, 0x06, 0x70, 0x00, 0x15, 0x7C, 0xC7, 0x06, 0x72, 0x00, 0x00, 0x00, 0xFB, 0xF4, 0xEB, 0xFD, 0xFF, 0x06, 0x00, 0x05, 0xCF };
/* hooks INT 9 (reads port 60h, counts makes, chains to the old vector), then INT 16h AH=0 */
static const uint8_t b_hook9[] = { 0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0xA1, 0x24, 0x00, 0xA3, 0x04, 0x05, 0xA1, 0x26, 0x00, 0xA3, 0x06, 0x05, 0xC7, 0x06, 0x24, 0x00, 0x27, 0x7C, 0xC7, 0x06, 0x26, 0x00, 0x00, 0x00, 0xFB, 0xB4, 0x00, 0xCD, 0x16, 0xA3, 0x08, 0x05, 0xFA, 0xF4, 0x50, 0xE4, 0x60, 0xA8, 0x80, 0x75, 0x04, 0xFF, 0x06, 0x02, 0x05, 0x58, 0xFF, 0x2E, 0x04, 0x05 };
/* thirty lines through INT 10h AH=0Eh: A..^ each followed by CR LF */
static const uint8_t b_tty[] = { 0xFA, 0xB9, 0x1E, 0x00, 0xB3, 0x41, 0x88, 0xD8, 0xB4, 0x0E, 0xCD, 0x10, 0xB0, 0x0D, 0xCD, 0x10, 0xB0, 0x0A, 0xCD, 0x10, 0xFE, 0xC3, 0xE2, 0xEE, 0xF4 };
/* cli; mov ax,13h; int 10h; hlt */
static const uint8_t b_mode13[] = { 0xFA, 0xB8, 0x13, 0x00, 0xCD, 0x10, 0xF4 };
/* cli; mov ah,9; int 21h; hlt */
static const uint8_t b_dos[] = { 0xFA, 0xB4, 0x09, 0xCD, 0x21, 0xF4 };
/* INT 13h: drive 81h; cylinder 80; head 1 of the floppy; AH=8 -> flags/AX at 510h..51Fh */
static const uint8_t b_int13[] = { 0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0x8E, 0xC0, 0xB8, 0x01, 0x02, 0xB9, 0x01, 0x00, 0xBA, 0x81, 0x00, 0xBB, 0x00, 0x06, 0xCD, 0x13, 0x9C, 0x8F, 0x06, 0x10, 0x05, 0xA3, 0x12, 0x05, 0xB8, 0x01, 0x02, 0xB9, 0x01, 0x50, 0xBA, 0x00, 0x00, 0xCD, 0x13, 0x9C, 0x8F, 0x06, 0x14, 0x05, 0xA3, 0x16, 0x05, 0xB8, 0x01, 0x02, 0xB9, 0x01, 0x00, 0xBA, 0x00, 0x01, 0xBB, 0x00, 0x06, 0xCD, 0x13, 0x9C, 0x8F, 0x06, 0x18, 0x05, 0xA3, 0x1A, 0x05, 0xB4, 0x08, 0xB2, 0x00, 0xCD, 0x13, 0x89, 0x0E, 0x1C, 0x05, 0x89, 0x16, 0x1E, 0x05, 0xF4 };
/* cli; mov ah,1; int 16h; pushf; pop word [520h]; hlt */
static const uint8_t b_peek[] = { 0xFA, 0xB4, 0x01, 0xCD, 0x16, 0x9C, 0x8F, 0x06, 0x20, 0x05, 0xF4 };
/* int 19h; hlt */
static const uint8_t b_reboot[] = { 0xCD, 0x19, 0xF4 };

static uint16_t pw(uint32_t a){ return (uint16_t)(pmem[a] | (pmem[a + 1] << 8)); }

static void test_bios(void){
    int r;
    printf("  the BIOS, the timer and the keyboard\n");

    check("a disk with no boot signature is refused, with the reason",
          (memset(fd, 0, 512), pc_init(&pc, pmem, &phost, 0, fd, PC_FD_BYTES, 512, hd, PC_HD_BYTES),
           pc_boot(&pc) == -1 && strstr(pc.notice, "55 AA")));

    boot_bytes(b_hook1c, sizeof b_hook1c);
    {   uint32_t t0 = (uint32_t)pw(0x46C) | ((uint32_t)pw(0x46E) << 16);
        check("the BDA tick count starts at the time of day (10:45:30)", t0 > 700000 && t0 < 720000);
        r = pc_go(1);
        check("HLT with interrupts on: idle, not stopped", r == EMU_IDLE);
        for(int i = 0; i < 60; i++){ T += 55; pc_run(&pc, 1000); }
        uint32_t t1 = (uint32_t)pw(0x46C) | ((uint32_t)pw(0x46E) << 16);
        check("the timer ticks the BDA count about 18 times a second", t1 - t0 >= 55 && t1 - t0 <= 65);
        check("and calls a hooked INT 1Ch each time", pw(0x500) == t1 - t0);
    }

    boot_bytes(b_hook9, sizeof b_hook9);
    r = pc_go(10);
    check("INT 16h with no key: waits", r == EMU_WAIT);
    pc_key(&pc, 'x');
    pc_go(10);
    check("HavenDOS's Enter ('\\n') is a PC's Enter: CR, scancode 1Ch",
          (pc_key(&pc, '\n'), pc.keyq[(pc.kq_tail + 31) % 32] == ((0x1C << 8) | 13)));
    check("a key arrives through IRQ 1 and the guest's own INT 9 sees it on port 60h", pw(0x502) == 1);
    check("...and chaining to the BIOS's INT 9 puts it in the buffer for INT 16h",
          pw(0x508) == (uint16_t)((0x2D << 8) | 'x'));

    boot_bytes(b_peek, sizeof b_peek);
    pc_go(5);
    check("INT 16h AH=1 with nothing waiting: ZF set", (pw(0x520) & F_ZF) != 0);

    boot_bytes(b_tty, sizeof b_tty);
    pc_go(20);
    check("teletype: thirty lines scroll the screen", row_is(23, "^") && row_is(0, "G"));
    {   pc_screen_t s; pc_screen(&pc, &s);
        check("teletype: the cursor at the start of the last row", s.cx == 0 && s.cy == 24); }

    boot_bytes(b_int13, sizeof b_int13);
    pc_go(10);
    check("INT 13h on a drive that is not there: CF and AH=80h (timeout)",
          (pw(0x510) & F_CF) && (pw(0x512) >> 8) == 0x80);
    check("INT 13h past the last cylinder: CF and AH=04h", (pw(0x514) & F_CF) && (pw(0x516) >> 8) == 0x04);
    check("INT 13h head 1 of the floppy reads (AL=1, CF clear)", !(pw(0x518) & F_CF) && pw(0x51A) == 1);
    check("INT 13h AH=8: a 1.44 MB floppy is 80 cylinders, 18 sectors, 2 heads",
          pw(0x51C) == ((79 << 8) | 18) && (pw(0x51E) >> 8) == 1);

    boot_bytes(b_mode13, sizeof b_mode13);
    pc_go(5);
    check("a graphics mode is named in the notice, not ignored", strstr(pc.notice, "13h") != 0);
    boot_bytes(b_dos, sizeof b_dos);
    pc_go(5);
    check("a DOS call is named in the notice", strstr(pc.notice, "DOS") != 0);

    boot_bytes(b_reboot, sizeof b_reboot);
    r = pc_run(&pc, 100);
    check("INT 19h restarts the PC from the floppy", r == EMU_BUDGET && strstr(pc.notice, "INT 19h") != 0);
}

int main(void){
    pmem = calloc(1, EMU86_MEM);
    test_cpu();
    test_kdos();
    test_bios();
    printf("%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
