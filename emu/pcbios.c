/* ════════════════════════════════════════════════════════════════════
   emu/pcbios.c -- the PC around emu/cpu86.c.  v0.9.7.

   include/pcbios.h has the overview. This file is the BIOS -- written
   as C behind a real interrupt vector table, so guest code that hooks
   INT 9 or INT 1Ch and chains to the old vector works the way it did
   on the hardware -- and the handful of chips an old boot sector
   expects to find: the 8259 (IRQ 0 and 1), the 8253's channel 0, the
   8042's data and status ports and the 6845's cursor registers.

   THE ROM. Every vector points into segment F000 at a stub:

       0F FF nn  CF          most services: the trap, then IRET
       FB 0F FF 16 CF        INT 16h: STI first, because the wait for a
                             key is satisfied by IRQ 1, and an INT has
                             just cleared IF -- without the STI a guest
                             waiting for a key would wait for ever
       0F FF 08 CD 1C CF     INT 08h: the tick, then the user hook 1Ch

   A service that has to wait (a key not pressed yet) returns BIOS_WAIT
   and the CPU re-runs the same trap later, so nothing here blocks:
   the window keeps drawing and HavenDOS keeps multitasking.

   WHAT IS NOT HERE. Graphics modes (a mode switch is honoured as a
   notice and the screen stays text), the serial and printer ports, a
   speaker, DOS (INT 21h). Each one that a guest asks for is named in
   the notice line rather than ignored silently.
   ════════════════════════════════════════════════════════════════════ */
#include "pcbios.h"

#define ROM_SEG    0xF000
#define STUB_BASE  0x1000          /* F000:1000 + vector * 8 */
#define DPT_OFF    0x0F00          /* the floppy parameter table */
#define BDA        0x400

/* the BDA fields used here */
#define BDA_EQUIP    0x10
#define BDA_MEMKB    0x13
#define BDA_KHEAD    0x1A
#define BDA_KTAIL    0x1C
#define BDA_KBUF     0x1E          /* 16 words, 0x1E..0x3D */
#define BDA_KEND     0x3E
#define BDA_MODE     0x49
#define BDA_COLS     0x4A
#define BDA_PAGESZ   0x4C
#define BDA_PAGEOFF  0x4E
#define BDA_CURPOS   0x50          /* 8 pages, col then row */
#define BDA_CURSHAPE 0x60
#define BDA_PAGE     0x62
#define BDA_CRTC     0x63
#define BDA_TICKS    0x6C
#define BDA_MIDNIGHT 0x70
#define BDA_NHD      0x75
#define BDA_KSTART   0x80
#define BDA_KSTOP    0x82
#define BDA_ROWS     0x84

#define TICKS_PER_DAY 0x1800B0u

static void copy(uint8_t *d, const uint8_t *s, uint32_t n){ while(n--) *d++ = *s++; }
static void fill(uint8_t *d, uint8_t v, uint32_t n){ while(n--) *d++ = v; }

static void say(pc_t *p, const char *a, int n, const char *b){
    /* "a" + two hex digits (if n >= 0) + "b", into the notice line */
    int i = 0;
    static const char hx[] = "0123456789ABCDEF";
    while(a && *a && i < PC_NOTICE_LEN - 1) p->notice[i++] = *a++;
    if(n >= 0 && i < PC_NOTICE_LEN - 3){
        p->notice[i++] = hx[(n >> 4) & 15]; p->notice[i++] = hx[n & 15];
    }
    while(b && *b && i < PC_NOTICE_LEN - 1) p->notice[i++] = *b++;
    p->notice[i] = 0;
    p->notices++;
}

static uint8_t  b8 (pc_t *p, uint32_t o){ return emu86_rd8(&p->cpu, BDA + o); }
static uint16_t b16(pc_t *p, uint32_t o){ return emu86_rd16(&p->cpu, BDA + o); }
static void wb8 (pc_t *p, uint32_t o, uint8_t v){ emu86_wr8(&p->cpu, BDA + o, v); }
static void wb16(pc_t *p, uint32_t o, uint16_t v){ emu86_wr16(&p->cpu, BDA + o, v); }

static uint8_t bcd(int v){ return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* ── disks ────────────────────────────────────────────────────────── */
void pc_geometry(uint32_t bytes, int floppy, int *c, int *h, int *s){
    if(floppy){
        /* the standard formats by size; anything else -- a bare boot
           image like K-DOS's 25 KB -- is put on a 1.44 MB disk, which
           is what it was copied onto to boot it */
        if(bytes == 368640)       { *c = 40; *h = 2; *s = 9;  }
        else if(bytes == 737280)  { *c = 80; *h = 2; *s = 9;  }
        else if(bytes == 1228800) { *c = 80; *h = 2; *s = 15; }
        else if(bytes == 2949120) { *c = 80; *h = 2; *s = 36; }
        else                      { *c = 80; *h = 2; *s = 18; }
        return;
    }
    *h = 16; *s = 63;
    *c = (int)(bytes / (16u * 63u * 512u));
    if(*c < 1) *c = 1;
}

/* drive -> its buffer and geometry, or 0 if there is no such drive */
static uint8_t *drive(pc_t *p, uint8_t dl, uint32_t *size, int *c, int *h, int *s){
    if(dl == 0x00 && p->fd){ *size = p->fd_size; *c = p->fd_c; *h = p->fd_h; *s = p->fd_s; return p->fd; }
    if(dl == 0x80 && p->hd){ *size = p->hd_size; *c = p->hd_c; *h = p->hd_h; *s = p->hd_s; return p->hd; }
    return 0;
}

static void ret_cf(pc_t *p, int err){ emu86_ret_flag(&p->cpu, F_CF, err); }

static void int13(pc_t *p){
    emu86_t *e = &p->cpu;
    uint8_t ah = (uint8_t)(e->r[R_AX] >> 8), al = (uint8_t)e->r[R_AX];
    uint8_t dl = (uint8_t)e->r[R_DX], dh = (uint8_t)(e->r[R_DX] >> 8);
    uint8_t cl = (uint8_t)e->r[R_CX], ch = (uint8_t)(e->r[R_CX] >> 8);
    uint32_t size; int C, H, S;
    uint8_t *d = drive(p, dl, &size, &C, &H, &S);
    uint8_t st = 0;
#ifdef PCBIOS_TRACE
    PCBIOS_TRACE("int13 ah=%02X al=%02X ch=%02X cl=%02X dh=%02X dl=%02X es:bx=%04X:%04X\n",
                 ah, al, ch, cl, dh, dl, e->sr[S_ES], e->r[R_BX]);
#endif

    switch(ah){
    case 0x00:                                         /* reset */
        st = d ? 0 : 0x01; break;
    case 0x01:                                         /* last status */
        e->r[R_AX] = (uint16_t)(p->last_status << 8);
        ret_cf(p, p->last_status != 0);
        return;
    case 0x02: case 0x03: case 0x04: {                 /* read write verify */
        int cyl = ch | ((cl & 0xC0) << 2), sec = cl & 0x3F, head = dh;
        uint32_t lba, n = al, off;
        uint16_t bx = e->r[R_BX];
        if(!d){ st = 0x80; break; }                    /* timeout: no drive */
        if(sec < 1 || sec > S || head >= H || cyl >= C || n == 0){ st = 0x04; break; }
        lba = ((uint32_t)cyl * (uint32_t)H + (uint32_t)head) * (uint32_t)S + (uint32_t)(sec - 1);
        if((lba + n) * 512u > size){ st = 0x04; break; }
        if(ah != 0x04){
            for(uint32_t i = 0; i < n * 512u; i++){
                off = lba * 512u + i;
                uint32_t a = emu86_lin(e->sr[S_ES], (uint16_t)(bx + i));
                if(ah == 0x02) emu86_wr8(e, a, d[off]);
                else           d[off] = emu86_rd8(e, a);
            }
            if(ah == 0x03 && dl == 0x80) p->hd_dirty = 1;
        }
        e->r[R_AX] = (uint16_t)(n);                    /* AH=0, AL=count */
        p->last_status = 0;
        ret_cf(p, 0);
        return; }
    case 0x08:                                         /* parameters */
        if(!d){ st = 0x01; break; }
        e->r[R_CX] = (uint16_t)((((C - 1) & 0xFF) << 8) | (((C - 1) >> 2) & 0xC0) | S);
        e->r[R_DX] = (uint16_t)(((H - 1) << 8) | (dl & 0x80 ? 1 : 1));
        if(!(dl & 0x80)){
            e->r[R_BX] = (uint16_t)((e->r[R_BX] & 0xFF00) | (S == 36 ? 5 : S == 18 ? 4 : S == 15 ? 2 : 3));
            e->sr[S_ES] = ROM_SEG; e->r[R_DI] = DPT_OFF;
        }
        e->r[R_AX] = 0; p->last_status = 0; ret_cf(p, 0);
        return;
    case 0x15:                                         /* drive type */
        if(!d){ e->r[R_AX] = 0; ret_cf(p, 0); return; }
        if(dl & 0x80){
            uint32_t sectors = size / 512u;
            e->r[R_AX] = 0x0300;
            e->r[R_CX] = (uint16_t)(sectors >> 16); e->r[R_DX] = (uint16_t)sectors;
        } else e->r[R_AX] = 0x0100;                    /* no change line */
        ret_cf(p, 0);
        return;
    case 0x16: st = 0; break;                          /* not changed */
    case 0x41: st = 0x01; break;                       /* no extensions */
    default:
        say(p, "INT 13h AH=", ah, "h is not provided");
        st = 0x01;
    }
    p->last_status = st;
    e->r[R_AX] = (uint16_t)((st << 8) | (st ? 0 : al));
    ret_cf(p, st != 0);
}

/* ── video ────────────────────────────────────────────────────────── */
static int cols(pc_t *p){ int c = b16(p, BDA_COLS); return c ? c : 80; }
static int rows(pc_t *p){ int r = b8(p, BDA_ROWS) + 1; return r > 1 ? r : 25; }
static uint32_t vram(pc_t *p){ return p->mode == 7 ? 0xB0000u : 0xB8000u; }
static uint32_t cell(pc_t *p, int page, int x, int y){
    return vram(p) + (uint32_t)page * b16(p, BDA_PAGESZ) + (uint32_t)(y * cols(p) + x) * 2u;
}
static void cursor_get(pc_t *p, int page, int *x, int *y){
    *x = b8(p, BDA_CURPOS + page * 2); *y = b8(p, BDA_CURPOS + page * 2 + 1);
}
static void cursor_set(pc_t *p, int page, int x, int y){
    wb8(p, BDA_CURPOS + page * 2, (uint8_t)x); wb8(p, BDA_CURPOS + page * 2 + 1, (uint8_t)y);
    if(page == b8(p, BDA_PAGE)){
        uint16_t pos = (uint16_t)(y * cols(p) + x);
        p->crtc[0x0E] = (uint8_t)(pos >> 8); p->crtc[0x0F] = (uint8_t)pos;
    }
}

static void scroll(pc_t *p, int up, int n, uint8_t attr, int r0, int c0, int r1, int c1){
    int page = b8(p, BDA_PAGE), C = cols(p), R = rows(p);
    if(r1 >= R) r1 = R - 1;
    if(c1 >= C) c1 = C - 1;
    if(r0 > r1 || c0 > c1) return;
    int h = r1 - r0 + 1;
    if(n == 0 || n > h) n = h;
    for(int i = 0; i < h; i++){
        int dst = up ? r0 + i : r1 - i;
        int src = up ? dst + n : dst - n;
        for(int x = c0; x <= c1; x++){
            uint32_t a = cell(p, page, x, dst);
            if((up && src <= r1) || (!up && src >= r0)){
                uint32_t b = cell(p, page, x, src);
                emu86_wr16(&p->cpu, a, emu86_rd16(&p->cpu, b));
            } else emu86_wr16(&p->cpu, a, (uint16_t)((attr << 8) | ' '));
        }
    }
}

static void teletype(pc_t *p, int page, uint8_t ch, int attr){
    int x, y, C = cols(p), R = rows(p);
    cursor_get(p, page, &x, &y);
    switch(ch){
    case 7:  break;                                    /* no speaker */
    case 8:  if(x > 0) x--; break;
    case 10: y++; break;
    case 13: x = 0; break;
    default: {
        uint32_t a = cell(p, page, x, y);
        emu86_wr8(&p->cpu, a, ch);
        if(attr >= 0) emu86_wr8(&p->cpu, a + 1, (uint8_t)attr);
        if(++x >= C){ x = 0; y++; }
    } }
    if(y >= R){
        /* the new line takes the attribute of the cell at the cursor */
        uint8_t at = emu86_rd8(&p->cpu, cell(p, page, x < C ? x : 0, R - 1) + 1);
        scroll(p, 1, 1, at, 0, 0, R - 1, C - 1);
        y = R - 1;
    }
    cursor_set(p, page, x, y);
}

static void set_mode(pc_t *p, int mode, int keep){
    int text = mode <= 3 || mode == 7;
    p->mode = text ? mode : 3;
    if(!text){
        say(p, "the program switched to video mode ", mode, "h: Retro PC shows text modes only");
    }
    wb8(p, BDA_MODE, (uint8_t)p->mode);
    wb16(p, BDA_COLS, (uint16_t)(p->mode <= 1 ? 40 : 80));
    wb16(p, BDA_PAGESZ, (uint16_t)(p->mode <= 1 ? 2048 : 4096));
    wb16(p, BDA_PAGEOFF, 0);
    wb8(p, BDA_PAGE, 0);
    wb8(p, BDA_ROWS, 24);
    wb16(p, BDA_CURSHAPE, 0x0607);
    p->crtc[0x0A] = 0x06; p->crtc[0x0B] = 0x07;
    for(int i = 0; i < 8; i++) cursor_set(p, i, 0, 0);
    if(!keep)
        for(uint32_t a = 0; a < 0x8000u; a += 2)
            emu86_wr16(&p->cpu, vram(p) + a, 0x0720);
}

static void int10(pc_t *p){
    emu86_t *e = &p->cpu;
    uint8_t ah = (uint8_t)(e->r[R_AX] >> 8), al = (uint8_t)e->r[R_AX];
    uint8_t bh = (uint8_t)(e->r[R_BX] >> 8), bl = (uint8_t)e->r[R_BX];
    int x, y;
    switch(ah){
    case 0x00: set_mode(p, al & 0x7F, al & 0x80); break;
    case 0x01:
        wb16(p, BDA_CURSHAPE, e->r[R_CX]);
        p->crtc[0x0A] = (uint8_t)(e->r[R_CX] >> 8); p->crtc[0x0B] = (uint8_t)e->r[R_CX];
        break;
    case 0x02: cursor_set(p, bh & 7, e->r[R_DX] & 0xFF, e->r[R_DX] >> 8); break;
    case 0x03:
        cursor_get(p, bh & 7, &x, &y);
        e->r[R_DX] = (uint16_t)((y << 8) | x);
        e->r[R_CX] = b16(p, BDA_CURSHAPE);
        break;
    case 0x05:
        wb8(p, BDA_PAGE, al & 7);
        wb16(p, BDA_PAGEOFF, (uint16_t)((al & 7) * b16(p, BDA_PAGESZ)));
        break;
    case 0x06: case 0x07:
        scroll(p, ah == 0x06, al, bh, e->r[R_CX] >> 8, e->r[R_CX] & 0xFF,
               e->r[R_DX] >> 8, e->r[R_DX] & 0xFF);
        break;
    case 0x08: {
        cursor_get(p, bh & 7, &x, &y);
        e->r[R_AX] = emu86_rd16(e, cell(p, bh & 7, x, y));
        break; }
    case 0x09: case 0x0A: {
        int n = e->r[R_CX], C = cols(p), R = rows(p);
        cursor_get(p, bh & 7, &x, &y);
        for(int i = 0; i < n; i++){
            int cx = x + i, cy = y + cx / C; cx %= C;
            if(cy >= R) break;
            uint32_t a = cell(p, bh & 7, cx, cy);
            emu86_wr8(e, a, al);
            if(ah == 0x09) emu86_wr8(e, a + 1, bl);
        }
        break; }
    case 0x0B: case 0x10: case 0x11: break;            /* palette, font */
    case 0x0E: teletype(p, b8(p, BDA_PAGE), al, -1); break;
    case 0x0F:
        e->r[R_AX] = (uint16_t)((cols(p) << 8) | p->mode);
        e->r[R_BX] = (uint16_t)((b8(p, BDA_PAGE) << 8) | bl);
        break;
    case 0x12:                                         /* EGA info */
        if(bl == 0x10) e->r[R_BX] = 0x0003;
        break;
    case 0x13: {                                       /* write string */
        int page = bh & 7, cnt = e->r[R_CX];
        uint16_t bp = e->r[R_BP];
        int ox, oy;
        cursor_get(p, page, &ox, &oy);
        cursor_set(p, page, e->r[R_DX] & 0xFF, e->r[R_DX] >> 8);
        for(int i = 0; i < cnt; i++){
            uint8_t ch = emu86_rd8(e, emu86_lin(e->sr[S_ES], bp++));
            int at = bl;
            if(al & 2) at = emu86_rd8(e, emu86_lin(e->sr[S_ES], bp++));
            teletype(p, page, ch, (ch == 7 || ch == 8 || ch == 10 || ch == 13) ? -1 : at);
        }
        if(!(al & 1)) cursor_set(p, page, ox, oy);
        break; }
    case 0x1A:                                         /* display code */
        if(al == 0){ e->r[R_AX] = 0x001A; e->r[R_BX] = 0x0008; }
        break;
    default:
        say(p, "INT 10h AH=", ah, "h is not provided");
    }
}

/* ── keyboard ─────────────────────────────────────────────────────── */
static int kbuf_put(pc_t *p, uint16_t v){
    uint16_t head = b16(p, BDA_KHEAD), tail = b16(p, BDA_KTAIL);
    uint16_t next = (uint16_t)(tail + 2);
    if(next >= b16(p, BDA_KSTOP)) next = b16(p, BDA_KSTART);
    if(next == head) return 0;                         /* full: beep, drop */
    wb16(p, tail, v);
    wb16(p, BDA_KTAIL, next);
    return 1;
}

static void int16(pc_t *p, int *wait){
    emu86_t *e = &p->cpu;
    uint8_t ah = (uint8_t)(e->r[R_AX] >> 8);
    uint16_t head = b16(p, BDA_KHEAD), tail = b16(p, BDA_KTAIL);
    switch(ah){
    case 0x00: case 0x10:
        if(head == tail){ *wait = 1; return; }
        e->r[R_AX] = b16(p, head);
        head = (uint16_t)(head + 2);
        if(head >= b16(p, BDA_KSTOP)) head = b16(p, BDA_KSTART);
        wb16(p, BDA_KHEAD, head);
        return;
    case 0x01: case 0x11:
        if(head == tail){ emu86_ret_flag(e, F_ZF, 1); return; }
        e->r[R_AX] = b16(p, head);
        emu86_ret_flag(e, F_ZF, 0);
        return;
    case 0x02: case 0x12:
        e->r[R_AX] = (uint16_t)(e->r[R_AX] & 0xFF00);
        return;
    case 0x05:                                         /* stuff a key */
        e->r[R_AX] = (uint16_t)(kbuf_put(p, e->r[R_CX]) ? 0 : 1);
        return;
    default:
        say(p, "INT 16h AH=", ah, "h is not provided");
    }
}

/* the default IRQ 1 handler: what the 8042 holds, into the buffer */
static void int09(pc_t *p){
    uint8_t sc = p->port60;
    if(!(sc & 0x80)) kbuf_put(p, (uint16_t)((sc << 8) | p->key_ascii));
    p->obf = 0;
    emu86_eoi(&p->cpu);
}

/* US layout. Index = scancode; unshifted and shifted. */
static const char k_low[] =
    "\0\x1b" "1234567890-=\b\t" "qwertyuiop[]\r\0" "asdfghjkl;'`\0\\" "zxcvbnm,./\0*\0 ";
static const char k_up[] =
    "\0\x1b" "!@#$%^&*()_+\b\t" "QWERTYUIOP{}\r\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?\0*\0 ";

static int scan_of(int ascii, uint8_t *sc){
    for(int i = 1; i < (int)sizeof(k_low) - 1; i++)
        if(k_low[i] == ascii || k_up[i] == ascii){ *sc = (uint8_t)i; return 1; }
    return 0;
}

void pc_key(pc_t *p, int key){
    uint8_t sc = 0, ascii = 0;
    int next;
    /* HavenDOS's Enter is '\n'; a PC's is CR, scancode 1Ch. Taken as
       Ctrl+J by the line below, it reached K-DOS as a line feed and
       the login never ended -- found by harness/retro_boot.py, since
       the native suite had been typing 13. Ctrl+J itself is the price. */
    if(key == '\n') key = '\r';
    if(key >= 0x100){                                  /* extended: 0x100|scancode */
        sc = (uint8_t)key;
        if(sc >= 0x80) return;                         /* HavenDOS-only codes */
    } else if(key >= 1 && key <= 26 && key != 8 && key != 9 && key != 13){
        ascii = (uint8_t)key;                          /* Ctrl+letter */
        scan_of('a' + key - 1, &sc);
    } else {
        ascii = (uint8_t)key;
        if(!scan_of(key, &sc)) return;
    }
    next = (p->kq_tail + 1) % 32;
    if(next == p->kq_head) return;
    p->keyq[p->kq_tail] = (uint16_t)((sc << 8) | ascii);
    p->kq_tail = next;
}

int pc_keys_waiting(const pc_t *p){ return p->kq_head != p->kq_tail || p->break_due; }

/* one scancode into the 8042 and IRQ 1, if the last one was taken */
static void kbd_feed(pc_t *p){
    if(p->obf || (p->cpu.irq_pending & 2) || (p->cpu.irq_service & 2)) return;
    if(p->break_due){
        p->port60 = (uint8_t)(p->break_due | 0x80); p->break_due = 0;
    } else if(p->kq_head != p->kq_tail){
        uint16_t k = p->keyq[p->kq_head];
        p->kq_head = (p->kq_head + 1) % 32;
        p->port60 = (uint8_t)(k >> 8); p->key_ascii = (uint8_t)k;
        p->break_due = (uint8_t)(k >> 8);
    } else return;
    p->obf = 1;
    emu86_irq(&p->cpu, 1);
}

/* ── time ─────────────────────────────────────────────────────────── */
static void int1a(pc_t *p){
    emu86_t *e = &p->cpu;
    uint8_t ah = (uint8_t)(e->r[R_AX] >> 8);
    int y, mo, d, h, mi, s;
    switch(ah){
    case 0x00: {
        uint32_t t = emu86_rd16(e, BDA + BDA_TICKS) | ((uint32_t)emu86_rd16(e, BDA + BDA_TICKS + 2) << 16);
        e->r[R_CX] = (uint16_t)(t >> 16); e->r[R_DX] = (uint16_t)t;
        e->r[R_AX] = b8(p, BDA_MIDNIGHT);
        wb8(p, BDA_MIDNIGHT, 0);
        return; }
    case 0x01:
        wb16(p, BDA_TICKS, e->r[R_DX]); wb16(p, BDA_TICKS + 2, e->r[R_CX]);
        return;
    case 0x02:
        p->host->rtc(p->hctx, &y, &mo, &d, &h, &mi, &s);
        e->r[R_CX] = (uint16_t)((bcd(h) << 8) | bcd(mi));
        e->r[R_DX] = (uint16_t)(bcd(s) << 8);
        ret_cf(p, 0);
        return;
    case 0x04:
        p->host->rtc(p->hctx, &y, &mo, &d, &h, &mi, &s);
        e->r[R_CX] = (uint16_t)((bcd(y / 100) << 8) | bcd(y % 100));
        e->r[R_DX] = (uint16_t)((bcd(mo) << 8) | bcd(d));
        ret_cf(p, 0);
        return;
    case 0x03: case 0x05: ret_cf(p, 0); return;        /* set: accepted, kept by the host's clock */
    default:
        say(p, "INT 1Ah AH=", ah, "h is not provided");
        ret_cf(p, 1);
    }
}

static void int08(pc_t *p){
    uint32_t t = emu86_rd16(&p->cpu, BDA + BDA_TICKS) |
                 ((uint32_t)emu86_rd16(&p->cpu, BDA + BDA_TICKS + 2) << 16);
    if(++t >= TICKS_PER_DAY){ t = 0; wb8(p, BDA_MIDNIGHT, 1); }
    wb16(p, BDA_TICKS, (uint16_t)t); wb16(p, BDA_TICKS + 2, (uint16_t)(t >> 16));
    emu86_eoi(&p->cpu);
}

/* ── the trap ─────────────────────────────────────────────────────── */
static int bios(void *ctx, emu86_t *e, uint8_t v){
    pc_t *p = (pc_t *)ctx;
    uint8_t ah = (uint8_t)(e->r[R_AX] >> 8);
    int wait = 0;
    switch(v){
    case 0x00: say(p, "the program divided by zero", -1, ""); break;
    case 0x08: int08(p); break;
    case 0x09: int09(p); break;
    case 0x10: int10(p); break;
    case 0x11: e->r[R_AX] = b16(p, BDA_EQUIP); break;
    case 0x12: e->r[R_AX] = b16(p, BDA_MEMKB); break;
    case 0x13: int13(p); break;
    case 0x15:
        if(ah == 0x88){ e->r[R_AX] = 0; ret_cf(p, 0); break; }   /* no extended memory */
        e->r[R_AX] = (uint16_t)((e->r[R_AX] & 0x00FF) | 0x8600);
        ret_cf(p, 1);
        break;
    case 0x16: int16(p, &wait); if(wait) return BIOS_WAIT; break;
    case 0x17: e->r[R_AX] = (uint16_t)((e->r[R_AX] & 0xFF) | 0x3000); break;  /* no printer */
    case 0x14: e->r[R_AX] = (uint16_t)((e->r[R_AX] & 0xFF) | 0x8000); break;  /* no serial  */
    case 0x18:
        say(p, "nothing left to boot (INT 18h)", -1, "");
        p->stop = 1;
        break;
    case 0x19: return BIOS_REBOOT;
    case 0x1A: int1a(p); break;
    case 0x1C: case 0x1B: case 0x05: case 0x01: case 0x03: case 0x04:
        break;                                         /* nobody listening */
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        emu86_eoi(e); break;                           /* spurious IRQs */
    case 0x21:
        say(p, "the program called DOS (INT 21h AH=", ah, "h); there is no DOS here");
        ret_cf(p, 1);
        break;
    default:
        say(p, "INT ", v, "h is not provided");
        ret_cf(p, 1);
    }
    return BIOS_DONE;
}

/* ── ports ────────────────────────────────────────────────────────── */
static uint8_t in8(void *ctx, uint16_t port){
    pc_t *p = (pc_t *)ctx;
    switch(port){
    case 0x20: return 0;
    case 0x21: return p->cpu.irq_mask;
    case 0x40: {                                       /* channel 0, counting down */
        uint16_t c = p->pit_latch;
        uint8_t r = (p->pit_lohi & 1) ? (uint8_t)(c >> 8) : (uint8_t)c;
        p->pit_lohi ^= 1;
        if(!(p->pit_lohi & 1)) p->pit_latch = (uint16_t)(p->pit_latch - 37);
        return r; }
    case 0x60: p->obf = 0; return p->port60;
    case 0x61: p->port61 ^= 0x10; return p->port61;   /* refresh toggles */
    case 0x64: return (uint8_t)(p->obf ? 0x1D : 0x1C);
    case 0x3D4: return p->crtc_idx;
    case 0x3D5: return p->crtc[p->crtc_idx & 31];
    case 0x3DA: p->retrace ^= 0x09; return p->retrace;
    case 0x3BA: p->retrace ^= 0x09; return p->retrace;
    default: return 0xFF;                              /* nothing there */
    }
}

static void out8(void *ctx, uint16_t port, uint8_t v){
    pc_t *p = (pc_t *)ctx;
    switch(port){
    case 0x20: if(v == 0x20) emu86_eoi(&p->cpu); break;
    case 0x21: p->cpu.irq_mask = v; break;
    case 0x43: p->pit_cmd = v; p->pit_lohi = 0; break;
    case 0x40: {
        /* a new divisor for the tick: games speed the timer up */
        static uint16_t lo;
        if(!(p->pit_lohi & 1)){ lo = v; p->pit_lohi |= 1; break; }
        p->pit_lohi &= (uint8_t)~1;
        uint32_t div = (uint32_t)lo | ((uint32_t)v << 8);
        if(div == 0) div = 65536;
        p->tick_us = div * 1000000u / 1193182u;
        if(p->tick_us < 1000) p->tick_us = 1000;      /* at most 1000 a second */
        break; }
    case 0x61: p->port61 = v; break;
    case 0x3D4: case 0x3B4: p->crtc_idx = v; break;
    case 0x3D5: case 0x3B5: p->crtc[p->crtc_idx & 31] = v; break;
    default: break;
    }
}

static const emu86_host_t cpu_host = { in8, out8, bios };

/* ── bring-up ─────────────────────────────────────────────────────── */
static void rom_put(pc_t *p, uint32_t off, const uint8_t *b, int n){
    copy(p->mem + 0xF0000u + off, b, (uint32_t)n);   /* the ROM: direct */
}

static void build_rom(pc_t *p){
    static const uint8_t dpt[11] = { 0xAF, 0x02, 0x25, 0x02, 0x12, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08 };
    fill(p->mem + 0xF0000u, 0xCF, 0x10000u);         /* IRET everywhere */
    for(int v = 0; v < 256; v++){
        uint32_t off = STUB_BASE + (uint32_t)v * 8u;
        uint8_t s[8]; int n = 0;
        if(v == 0x16) s[n++] = 0xFB;                   /* STI: see the header */
        s[n++] = 0x0F; s[n++] = 0xFF; s[n++] = (uint8_t)v;
        if(v == 0x08){ s[n++] = 0xCD; s[n++] = 0x1C; }
        s[n++] = 0xCF;
        rom_put(p, off, s, n);
        p->mem[v * 4 + 0] = (uint8_t)off; p->mem[v * 4 + 1] = (uint8_t)(off >> 8);
        p->mem[v * 4 + 2] = 0x00;         p->mem[v * 4 + 3] = 0xF0;
    }
    /* INT 1Eh is a pointer to the floppy parameters, not code */
    rom_put(p, DPT_OFF, dpt, 11);
    p->mem[0x1E * 4 + 0] = (uint8_t)DPT_OFF; p->mem[0x1E * 4 + 1] = (uint8_t)(DPT_OFF >> 8);
    /* the reset vector, the BIOS date, and the model byte (AT) */
    { static const uint8_t rv[5] = { 0xCD, 0x19, 0xF4, 0xEB, 0xFD };   /* INT 19h; HLT; JMP $-1 */
      rom_put(p, 0xFFF0, rv, 5); }
    rom_put(p, 0xFFF5, (const uint8_t *)"11/17/26", 8);
    p->mem[0xFFFFE] = 0xFC;
}

static void build_bda(pc_t *p){
    int y, mo, d, h, mi, s;
    fill(p->mem + BDA, 0, 256);
    wb16(p, BDA_EQUIP, 0x0021);         /* one floppy, 80x25 colour */
    wb16(p, BDA_MEMKB, 640);
    /* offsets within segment 40h, as the real BDA keeps them */
    wb16(p, BDA_KSTART, BDA_KBUF);
    wb16(p, BDA_KSTOP,  BDA_KEND);
    wb16(p, BDA_KHEAD,  BDA_KBUF);
    wb16(p, BDA_KTAIL,  BDA_KBUF);
    wb16(p, BDA_CRTC, 0x3D4);
    wb8(p, BDA_NHD, (uint8_t)(p->hd ? 1 : 0));
    p->host->rtc(p->hctx, &y, &mo, &d, &h, &mi, &s);
    {   /* the tick count since midnight, as the BIOS would have it */
        uint32_t secs = (uint32_t)(h * 3600 + mi * 60 + s);
        uint32_t t = secs * 18u + secs * 2065u / 10000u;
        wb16(p, BDA_TICKS, (uint16_t)t); wb16(p, BDA_TICKS + 2, (uint16_t)(t >> 16));
    }
}

int pc_init(pc_t *p, uint8_t *mem, const pc_host_t *host, void *hctx,
            uint8_t *fd, uint32_t fd_size, uint32_t fd_used,
            uint8_t *hd, uint32_t hd_size){
    fill((uint8_t *)p, 0, sizeof *p);
    p->mem = mem; p->host = host; p->hctx = hctx;
    p->fd = fd; p->fd_size = fd_size;
    p->hd = hd; p->hd_size = hd_size;
    if(fd) pc_geometry(fd_used, 1, &p->fd_c, &p->fd_h, &p->fd_s);
    if(hd) pc_geometry(hd_size, 0, &p->hd_c, &p->hd_h, &p->hd_s);
    /* a floppy's geometry must fit the buffer it is given */
    if(fd && (uint32_t)p->fd_c * (uint32_t)p->fd_h * (uint32_t)p->fd_s * 512u > fd_size)
        p->fd_c = (int)(fd_size / ((uint32_t)p->fd_h * (uint32_t)p->fd_s * 512u));
    return 0;
}

int pc_boot(pc_t *p){
    emu86_t *e = &p->cpu;
    fill(p->mem, 0, EMU86_MEM);
    emu86_reset(e, p->mem, &cpu_host, p);
    build_rom(p);
    build_bda(p);
    set_mode(p, 3, 0);
    p->tick_us = 54925; p->tick_acc_us = 0;
    p->last_ms = p->host->now_ms(p->hctx);
    p->kq_head = p->kq_tail = 0; p->obf = 0; p->break_due = 0;
    p->stop = 0;
    p->crtc_idx = 0; p->retrace = 0;

    if(!p->fd || p->fd_size < 512){
        say(p, "there is no disk to boot from", -1, "");
        return -1;
    }
    if(p->fd[510] != 0x55 || p->fd[511] != 0xAA){
        say(p, "the disk has no boot signature (55 AA at offset 510)", -1, "");
        return -1;
    }
    copy(p->mem + 0x7C00, p->fd, 512);
    e->sr[S_CS] = 0; e->ip = 0x7C00;
    e->sr[S_DS] = e->sr[S_ES] = e->sr[S_SS] = 0;
    e->r[R_SP] = 0x7C00;
    e->r[R_DX] = 0x0000;                               /* booted from A: */
    e->flags = F_FIXED | F_IF;
    return 0;
}

int pc_run(pc_t *p, uint32_t budget){
    uint32_t now = p->host->now_ms(p->hctx), ticks = 0;
    int r;

    /* the timer: as many ticks as the time that passed, but never a
       backlog -- after a pause behind another window the guest gets a
       few ticks, not a storm of them */
    p->tick_acc_us += (now - p->last_ms) * 1000u;
    p->last_ms = now;
    while(p->tick_acc_us >= p->tick_us && ticks < 4){ p->tick_acc_us -= p->tick_us; ticks++; }
    if(p->tick_acc_us > p->tick_us * 4) p->tick_acc_us = 0;
    if(ticks) emu86_irq(&p->cpu, 0);

    kbd_feed(p);
    r = emu86_run(&p->cpu, budget);
    if(r == EMU_REBOOT){
        say(p, "the program restarted the PC (INT 19h)", -1, "");
        return pc_boot(p) == 0 ? EMU_BUDGET : PC_STOPPED;
    }
    if(p->stop) return PC_STOPPED;
    /* after a key went in, offer the next one straight away */
    kbd_feed(p);
    return r;
}

void pc_screen(const pc_t *p, pc_screen_t *s){
    pc_t *q = (pc_t *)p;
    uint16_t pos = (uint16_t)((p->crtc[0x0E] << 8) | p->crtc[0x0F]);
    s->cols = cols(q); s->rows = rows(q);
    s->cells = p->mem + vram(q) + b16(q, BDA_PAGEOFF);
    s->cx = pos % s->cols; s->cy = pos / s->cols;
    /* CRTC register 0Ah bit 5, or a start line below the end line, hides it */
    s->cursor_on = !(p->crtc[0x0A] & 0x20) && (p->crtc[0x0A] & 0x1F) <= (p->crtc[0x0B] & 0x1F);
    s->text = 1;
}
