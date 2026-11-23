/* ════════════════════════════════════════════════════════════════════
   emu/pc386.c -- the PC around emu/cpu386.c, and the loader that does
   GRUB's job.  v0.9.7.2.  include/pc386.h says what is in it.
   ════════════════════════════════════════════════════════════════════ */
#include "pc386.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

static int  memcmp_(const char *a, const char *b, int n){ for(int i = 0; i < n; i++) if(a[i] != b[i]) return 1; return 0; }
static void scopy(char *d, const char *s, int max){ int i = 0; for(; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static int  up(int ch){ return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch; }
static void say(pc386_t *p, const char *a, const char *b){
    int k = 0;
    for(; *a && k < PC386_NOTICE_LEN - 1; a++) p->notice[k++] = *a;
    for(; b && *b && k < PC386_NOTICE_LEN - 1; b++) p->notice[k++] = *b;
    p->notice[k] = 0;
}

/* ── ISO 9660 ─────────────────────────────────────────────────────── */
static uint32_t le32(const uint8_t *b){ return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint16_t le16(const uint8_t *b){ return (uint16_t)(b[0] | (b[1] << 8)); }

/* does a directory record's name equal this path component? Case is
   ignored and the ";1" version and a trailing dot are not part of it */
static int name_is(const uint8_t *nm, int nlen, const char *want, int wlen){
    int l = nlen;
    for(int i = 0; i < nlen; i++) if(nm[i] == ';'){ l = i; break; }
    if(l > 0 && nm[l - 1] == '.') l--;
    if(l != wlen) return 0;
    for(int i = 0; i < l; i++) if(up(nm[i]) != up(want[i])) return 0;
    return 1;
}

int pc386_iso_read(const uint8_t *iso, uint32_t size, const char *path, const uint8_t **data){
    const uint8_t *pvd, *rec;
    uint32_t lba, len;
    if(size < 17u * 2048u) return -1;
    pvd = iso + 16u * 2048u;
    if(pvd[0] != 1 || pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0') return -1;
    rec = pvd + 156;
    lba = le32(rec + 2); len = le32(rec + 10);
    while(*path == '/') path++;
    while(*path){
        int wl = 0, found = 0;
        while(path[wl] && path[wl] != '/') wl++;
        for(uint32_t off = 0; off < len; ){
            const uint8_t *r;
            uint32_t at = lba * 2048u + off;
            if(at + 34 > size) return -1;
            r = iso + at;
            if(r[0] == 0){ off = (off / 2048u + 1u) * 2048u; continue; }   /* to the next sector */
            if(r[0] >= 34 && name_is(r + 33, r[32], path, wl)){
                lba = le32(r + 2); len = le32(r + 10);
                found = 1;
                break;
            }
            off += r[0];
        }
        if(!found) return -1;
        path += wl;
        while(*path == '/') path++;
    }
    if((uint64_t)lba * 2048u + len > size) return -1;
    *data = iso + lba * 2048u;
    return (int)len;
}

/* ── grub.cfg: the menu entries and their multiboot lines ─────────── */
static const char *skipsp(const char *s, const char *e){ while(s < e && (*s == ' ' || *s == '\t')) s++; return s; }

int pc386_iso_menu(const uint8_t *iso, uint32_t size, pc386_entry_t *out, int max, const char **why){
    const uint8_t *cfg;
    int n = pc386_iso_read(iso, size, "/boot/grub/grub.cfg", &cfg);
    if(n < 0) n = pc386_iso_read(iso, size, "/boot/grub/menu.lst", &cfg);
    if(n < 0){
        *why = (size >= 17u * 2048u && iso[16u * 2048u + 1] == 'C') ?
               "a CD with no boot menu on it (no /boot/grub/grub.cfg) -- nothing on it starts"
             : "not a CD image (no ISO 9660 volume descriptor)";
        return -1;
    }
    return pc386_cfg_menu((const char *)cfg, n, out, max, why);
}

int pc386_cfg_menu(const char *cfg, int n, pc386_entry_t *out, int max, const char **why){
    const char *s, *e;
    pc386_entry_t *cur = 0;
    int cnt = 0;
    s = cfg; e = s + n;
    while(s < e){
        const char *l = s, *le;
        while(s < e && *s != '\n') s++;
        le = s; if(s < e) s++;
        if(le > l && le[-1] == '\r') le--;
        l = skipsp(l, le);
        if(le - l > 9 && !memcmp_(l, "menuentry", 9)){
            if(cnt >= max) break;
            cur = &out[cnt++];
            memset(cur, 0, sizeof *cur);
            l = skipsp(l + 9, le);
            if(l < le && (*l == '"' || *l == '\'')){
                char q = *l++; int k = 0;
                while(l < le && *l != q && k < (int)sizeof cur->title - 1) cur->title[k++] = *l++;
                cur->title[k] = 0;
            }
        } else if(cur && !cur->path[0] && le - l > 9 && !memcmp_(l, "multiboot", 9)){
            int k = 0;
            cur->mb2 = (l[9] == '2');
            l = skipsp(l + 9 + (cur->mb2 ? 1 : 0), le);
            while(l < le && *l != ' ' && *l != '\t' && k < (int)sizeof cur->path - 1) cur->path[k++] = *l++;
            cur->path[k] = 0;
            l = skipsp(l, le);
            k = 0;
            while(l < le && k < (int)sizeof cur->args - 1) cur->args[k++] = *l++;
            while(k > 0 && (cur->args[k - 1] == ' ' || cur->args[k - 1] == '\t')) k--;
            cur->args[k] = 0;
        }
    }
    /* entries with no multiboot line (chainloaders, "reboot") are dropped */
    {
        int k = 0;
        for(int i = 0; i < cnt; i++) if(out[i].path[0]) out[k++] = out[i];
        cnt = k;
    }
    if(!cnt){ *why = "its boot menu starts nothing this PC can load (no multiboot kernel)"; return -1; }
    return cnt;
}

/* ── the 8259s ────────────────────────────────────────────────────── */
static void pic_update(pc386_t *p){
    if(p->pic[1].irr & ~p->pic[1].imr) p->pic[0].irr |= 4; else p->pic[0].irr &= (uint8_t)~4;
    p->cpu.intr = (p->pic[0].irr & ~p->pic[0].imr) != 0;
}
static void irq(pc386_t *p, int line){
    if(line < 8) p->pic[0].irr |= (uint8_t)(1u << line);
    else p->pic[1].irr |= (uint8_t)(1u << (line - 8));
    pic_update(p);
}
static int pic_pick(pic_t *c){
    uint8_t req = c->irr & ~c->imr;
    for(int i = 0; i < 8; i++){
        if(c->isr & (1u << i)) return -1;               /* something above it is in service */
        if(req & (1u << i)) return i;
    }
    return -1;
}
static int intack(void *ctx){
    pc386_t *p = (pc386_t *)ctx;
    int i = pic_pick(&p->pic[0]), v = -1;
    if(i < 0){ p->cpu.intr = 0; return -1; }
    if(i == 2){
        int j = pic_pick(&p->pic[1]);
        if(j < 0){ p->pic[0].irr &= (uint8_t)~4; pic_update(p); return -1; }
        p->pic[1].irr &= (uint8_t)~(1u << j);
        p->pic[1].isr |= (uint8_t)(1u << j);
        p->pic[0].isr |= 4;
        v = p->pic[1].base + j;
    } else {
        p->pic[0].irr &= (uint8_t)~(1u << i);
        p->pic[0].isr |= (uint8_t)(1u << i);
        v = p->pic[0].base + i;
    }
    pic_update(p);
    return v;
}
static void pic_out(pc386_t *p, int which, int port1, uint8_t v){
    pic_t *c = &p->pic[which];
    if(!port1){
        if(v & 0x10){ c->init = 2; c->icw4 = v & 1; c->imr = 0; c->isr = 0; c->irr = 0; return; }
        if((v & 0x18) == 0x08){ if((v & 3) == 2) c->read_isr = 0; if((v & 3) == 3) c->read_isr = 1; return; }
        if(v & 0x20){                                   /* EOI */
            if(v & 0x40) c->isr &= (uint8_t)~(1u << (v & 7));
            else for(int i = 0; i < 8; i++) if(c->isr & (1u << i)){ c->isr &= (uint8_t)~(1u << i); break; }
        }
    } else {
        if(c->init == 2){ c->base = v & 0xF8; c->init = 3; return; }
        if(c->init == 3){ c->init = c->icw4 ? 4 : 0; return; }
        if(c->init == 4){ c->init = 0; return; }
        c->imr = v;
    }
    pic_update(p);
}

/* ── the 8042: one output buffer, fed from a queue ────────────────── */
static void kbc_push(pc386_t *p, uint8_t b, int aux){
    int next = (p->oq_tail + 1) % 64;
    if(next == p->oq_head) return;
    p->outq[p->oq_tail] = b; p->outq_aux[p->oq_tail] = (uint8_t)aux;
    p->oq_tail = next;
}
static void kbc_feed(pc386_t *p){
    if(p->obf) return;
    if(p->oq_head == p->oq_tail && p->kq_head != p->kq_tail && !p->kbd_disabled){
        kbc_push(p, p->keyq[p->kq_head], 0);
        p->kq_head = (p->kq_head + 1) % 64;
    }
    if(p->oq_head == p->oq_tail) return;
    p->ob = p->outq[p->oq_head]; p->obf_aux = p->outq_aux[p->oq_head];
    p->oq_head = (p->oq_head + 1) % 64;
    p->obf = 1;
    if(p->obf_aux){ if(p->kbc_ccb & 2) irq(p, 12); }
    else if(p->kbc_ccb & 1) irq(p, 1);
}
static void kbd_write(pc386_t *p, uint8_t v){
    if(p->kbd_next){ p->kbd_next = 0; kbc_push(p, 0xFA, 0); return; }   /* the data byte of ED/F0/F3 */
    switch(v){
    case 0xFF: kbc_push(p, 0xFA, 0); kbc_push(p, 0xAA, 0); break;
    case 0xF2: kbc_push(p, 0xFA, 0); kbc_push(p, 0xAB, 0); kbc_push(p, 0x83, 0); break;
    case 0xEE: kbc_push(p, 0xEE, 0); break;
    case 0xED: case 0xF0: case 0xF3: p->kbd_next = v; kbc_push(p, 0xFA, 0); break;
    default:   kbc_push(p, 0xFA, 0);
    }
}
static void aux_write(pc386_t *p, uint8_t v){
    if(p->aux_next){ p->aux_next = 0; kbc_push(p, 0xFA, 1); return; }
    switch(v){
    case 0xFF: kbc_push(p, 0xFA, 1); kbc_push(p, 0xAA, 1); kbc_push(p, 0x00, 1); break;
    case 0xF2: kbc_push(p, 0xFA, 1); kbc_push(p, 0x00, 1); break;
    case 0xE9: kbc_push(p, 0xFA, 1); kbc_push(p, 0, 1); kbc_push(p, 2, 1); kbc_push(p, 100, 1); break;
    case 0xF3: case 0xE8: p->aux_next = v; kbc_push(p, 0xFA, 1); break;
    case 0xEB: kbc_push(p, 0xFA, 1); kbc_push(p, 0x08, 1); kbc_push(p, 0, 1); kbc_push(p, 0, 1); break;
    default:   kbc_push(p, 0xFA, 1);
    }
}

/* ── the 8254 ─────────────────────────────────────────────────────── */
/* 32-bit arithmetic only: the kernel has no libgcc for 64-bit division.
   One count of the 8254 is 838 ns. */
static uint32_t pit_period_us(const pc386_t *p){
    uint32_t r = p->pit_reload[0] ? p->pit_reload[0] : 65536u;
    return r * 838u / 1000u;
}
static uint16_t pit_count(pc386_t *p, int ch){
    uint32_t r = p->pit_reload[ch] ? p->pit_reload[ch] : 65536u;
    uint32_t counts = p->host->now_ms(p->hctx) * 1193u;
    /* a counter that moves with real time, for kernels that read it back */
    return (uint16_t)(r - counts % r);
}

/* ── CMOS ─────────────────────────────────────────────────────────── */
static uint8_t bcd(int v){ return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static uint8_t cmos_read(pc386_t *p){
    int y, mo, d, h, mi, s;
    uint32_t ext = p->mem_size > 0x100000u ? (p->mem_size - 0x100000u) / 1024u : 0;
    if(ext > 0xFFFF) ext = 0xFFFF;
    p->host->rtc(p->hctx, &y, &mo, &d, &h, &mi, &s);
    switch(p->cmos_idx & 0x7F){
    case 0x00: return bcd(s);
    case 0x02: return bcd(mi);
    case 0x04: return bcd(h);
    case 0x06: return 1;
    case 0x07: return bcd(d);
    case 0x08: return bcd(mo);
    case 0x09: return bcd(y % 100);
    case 0x32: return bcd(y / 100);
    case 0x0A: return 0x26;
    case 0x0B: return 0x02;
    case 0x0D: return 0x80;
    case 0x15: return 0x80; case 0x16: return 0x02;       /* 640 KB */
    case 0x17: case 0x30: return (uint8_t)ext;
    case 0x18: case 0x31: return (uint8_t)(ext >> 8);
    }
    return 0;
}

/* ── ports ────────────────────────────────────────────────────────── */
static uint32_t port_in(void *ctx, uint16_t port, int size){
    pc386_t *p = (pc386_t *)ctx;
    switch(port){
    case 0x20: case 0xA0: { pic_t *c = &p->pic[port == 0xA0]; return c->read_isr ? c->isr : c->irr; }
    case 0x21: case 0xA1: return p->pic[port == 0xA1].imr;
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        uint16_t v = p->pit_latched[ch] ? p->pit_latch[ch] : pit_count(p, ch);
        if(p->pit_access[ch] == 1){ p->pit_latched[ch] = 0; return v & 0xFF; }
        if(p->pit_access[ch] == 2){ p->pit_latched[ch] = 0; return v >> 8; }
        if(!p->pit_lohi_r[ch]){ p->pit_lohi_r[ch] = 1; return v & 0xFF; }
        p->pit_lohi_r[ch] = 0; p->pit_latched[ch] = 0; return v >> 8; }
    case 0x60: {
        uint8_t v = p->ob;
        p->obf = 0;
        kbc_feed(p);
        return v; }
    case 0x64: return (uint32_t)((p->obf ? 1 : 0) | 4 | (p->obf && p->obf_aux ? 0x20 : 0));
    case 0x61: p->port61 ^= 0x10; return (uint32_t)(p->port61 | 0x20);
    case 0x71: return cmos_read(p);
    case 0x92: return 0x02;
    case 0x3DA: case 0x3BA: p->retrace ^= 0x09; p->attr_flip = 0; return p->retrace;
    case 0x3D5: return p->crtc[p->crtc_idx & 31];
    case 0x3D4: return p->crtc_idx;
    case 0x3CF: return p->gc[p->gc_idx & 15];
    case 0x3C5: return p->seq[p->seq_idx & 7];
    case 0x3C1: return p->attr[p->attr_idx & 31];
    case 0x3CC: return 0x67;
    case 0x3FD: case 0x2FD: case 0x3ED: case 0x2ED: return 0x60;   /* COM: always ready, nothing received */
    case 0xCFC: case 0xCFD: case 0xCFE: case 0xCFF: return 0xFFFFFFFFu;   /* no PCI devices */
    case 0xCF8: return p->pci_addr;
    case 0x1F7: case 0x177: case 0x3F6: case 0x376: return 0;     /* no disks */
    }
    (void)size;
    return 0xFFFFFFFFu;
}

static void port_out(void *ctx, uint16_t port, uint32_t v, int size){
    pc386_t *p = (pc386_t *)ctx;
    uint8_t b = (uint8_t)v;
    (void)size;
    switch(port){
    case 0x20: case 0x21: pic_out(p, 0, port & 1, b); return;
    case 0xA0: case 0xA1: pic_out(p, 1, port & 1, b); return;
    case 0x43: {
        int ch = b >> 6;
        if(ch == 3) return;                             /* read-back: not kept */
        if(((b >> 4) & 3) == 0){ p->pit_latch[ch] = pit_count(p, ch); p->pit_latched[ch] = 1; p->pit_lohi_r[ch] = 0; return; }
        p->pit_access[ch] = (b >> 4) & 3; p->pit_mode[ch] = (b >> 1) & 7;
        p->pit_lohi_w[ch] = 0; p->pit_lohi_r[ch] = 0;
        return; }
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        if(p->pit_access[ch] == 1) p->pit_reload[ch] = (uint16_t)((p->pit_reload[ch] & 0xFF00) | b);
        else if(p->pit_access[ch] == 2) p->pit_reload[ch] = (uint16_t)((p->pit_reload[ch] & 0xFF) | (b << 8));
        else if(!p->pit_lohi_w[ch]){ p->pit_reload[ch] = (uint16_t)((p->pit_reload[ch] & 0xFF00) | b); p->pit_lohi_w[ch] = 1; }
        else { p->pit_reload[ch] = (uint16_t)((p->pit_reload[ch] & 0xFF) | (b << 8)); p->pit_lohi_w[ch] = 0; }
        if(ch == 0) p->pit_acc_us = 0;
        return; }
    case 0x61: p->port61 = b & 0x0F; return;
    case 0x60:
        if(p->kbc_next == 0x60){ p->kbc_ccb = b; p->kbc_next = 0; return; }
        if(p->kbc_next == 0xD4){ p->kbc_next = 0; aux_write(p, b); kbc_feed(p); return; }
        if(p->kbc_next == 0xD1){ p->kbc_next = 0; if(!(b & 1)) { p->stop = 1; say(p, "the system reset itself (8042 output port)", 0); } return; }
        kbd_write(p, b); kbc_feed(p);
        return;
    case 0x64:
        switch(b){
        case 0x20: kbc_push(p, p->kbc_ccb, 0); kbc_feed(p); return;
        case 0x60: case 0xD4: case 0xD1: p->kbc_next = b; return;
        case 0xA7: p->aux_enabled = 0; return;
        case 0xA8: p->aux_enabled = 1; return;
        case 0xA9: case 0xAB: kbc_push(p, 0x00, 0); kbc_feed(p); return;
        case 0xAA: kbc_push(p, 0x55, 0); kbc_feed(p); return;
        case 0xAD: p->kbd_disabled = 1; return;
        case 0xAE: p->kbd_disabled = 0; kbc_feed(p); return;
        case 0xFE: p->stop = 1; say(p, "the system reset itself (it asked the keyboard controller)", 0); return;
        }
        return;
    case 0x70: p->cmos_idx = b; return;
    case 0x3D4: case 0x3B4: p->crtc_idx = b; return;
    case 0x3D5: case 0x3B5: p->crtc[p->crtc_idx & 31] = b; return;
    case 0x3CE: p->gc_idx = b; return;
    case 0x3CF: p->gc[p->gc_idx & 15] = b; return;
    case 0x3C4: p->seq_idx = b; return;
    case 0x3C5: p->seq[p->seq_idx & 7] = b; return;
    case 0x3C0:
        if(!p->attr_flip) p->attr_idx = b & 31; else p->attr[p->attr_idx & 31] = b;
        p->attr_flip ^= 1;
        return;
    case 0xCF8: p->pci_addr = v; return;
    case 0xCF9: if(b & 4){ p->stop = 1; say(p, "the system reset itself", 0); } return;
    case 0x604: case 0xB004: case 0x4004:
        if((v & 0xFFFF) == 0x2000 || (v & 0x3C00) == 0x2000 || (v & 0x2000)){ p->stop = 2; say(p, "it powered the PC off", 0); }
        return;
    }
}

static const cpu386_host_t g_host386 = { port_in, port_out, intack };

/* ── keys: HavenDOS codes to set-1 scancodes ──────────────────────── */
static const char k_low[] =
    "\0\x1b" "1234567890-=\b\t" "qwertyuiop[]\n\0" "asdfghjkl;'`\0\\" "zxcvbnm,./\0*\0 ";
static const char k_up[] =
    "\0\x1b" "!@#$%^&*()_+\b\t" "QWERTYUIOP{}\n\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?\0*\0 ";

static void kq(pc386_t *p, uint8_t b){
    int next = (p->kq_tail + 1) % 64;
    if(next == p->kq_head) return;
    p->keyq[p->kq_tail] = b; p->kq_tail = next;
}

void pc386_key(pc386_t *p, int key){
    int sc = -1, shift = 0, ctrl = 0;
    if(key == '\r') key = '\n';
    if(key >= 0x100){
        sc = key & 0xFF;
        if(sc >= 0x80) return;
        /* v0.9.7.5: the function keys are not extended keys. F1-F10
           (3B-44) and F11/F12 (57, 58) come without the E0 prefix on a
           real keyboard; with it, a kernel that reads E0 as "the next
           code is a cursor key" saw F1 as nothing at all. */
        if((sc >= 0x3B && sc <= 0x44) || sc == 0x57 || sc == 0x58){
            kq(p, (uint8_t)sc); kq(p, (uint8_t)(sc | 0x80));
        } else {
            kq(p, 0xE0); kq(p, (uint8_t)sc); kq(p, 0xE0); kq(p, (uint8_t)(sc | 0x80));
        }
        kbc_feed(p);
        return;
    }
    if(key >= 1 && key <= 26 && key != 8 && key != 9 && key != 10){ ctrl = 1; key = 'a' + key - 1; }
    for(int i = 1; i < (int)sizeof(k_low) - 1; i++){
        if(k_low[i] == key){ sc = i; break; }
        if(k_up[i] == key){ sc = i; shift = 1; break; }
    }
    if(sc < 0) return;
    if(ctrl) kq(p, 0x1D);
    if(shift) kq(p, 0x2A);
    kq(p, (uint8_t)sc); kq(p, (uint8_t)(sc | 0x80));
    if(shift) kq(p, 0xAA);
    if(ctrl) kq(p, 0x9D);
    kbc_feed(p);
}

/* ── the machine ──────────────────────────────────────────────────── */
int pc386_init(pc386_t *p, uint8_t *mem, uint32_t mem_size, const pc386_host_t *host, void *hctx){
    memset(p, 0, sizeof *p);
    p->mem = mem; p->mem_size = mem_size;
    p->host = host; p->hctx = hctx;
    memset(mem, 0, mem_size);
    cpu386_reset(&p->cpu, mem, mem_size, &g_host386, p);
    p->pic[0].base = 0x08; p->pic[1].base = 0x70;
    p->pic[0].imr = 0xFF; p->pic[1].imr = 0xFF;
    p->kbc_ccb = 0x45;                                   /* kbd IRQ on, translation on */
    /* v0.9.7.5: the attribute controller's mode register as a VGA BIOS
       leaves it for mode 3 -- bit 3 set, so attribute bit 7 blinks. A
       kernel that clears it for 16 background colours can be told
       apart from one that never touched it, which reads back 0x0C. */
    p->attr[0x10] = 0x0C;
    p->pit_last_ms = host->now_ms(hctx);
    /* the BIOS left a blank 80x25 screen and its cursor at the top */
    for(uint32_t i = 0; i < 80u * 25u; i++){ mem[0xB8000 + i * 2] = ' '; mem[0xB8000 + i * 2 + 1] = 0x07; }
    p->crtc[0x0A] = 0x0D; p->crtc[0x0B] = 0x0E;
    return 0;
}

static uint32_t rd32le(const uint8_t *b){ return le32(b); }

int pc386_boot(pc386_t *p, const uint8_t *iso, uint32_t size, const pc386_entry_t *e){
    const uint8_t *k;
    int klen;
    if(e->mb2){ say(p, "this is a 64-bit Multiboot2 kernel; Retro PC's CPU is 32-bit", 0); return -1; }
    klen = pc386_iso_read(iso, size, e->path, &k);
    if(klen < 0){ say(p, "the kernel is not on the CD: ", e->path); return -1; }
    return pc386_boot_kernel(p, k, klen, e);
}

int pc386_boot_kernel(pc386_t *p, const uint8_t *k, int klen, const pc386_entry_t *e){
    uint32_t mbi = 0x9000, cmd = 0x9800, name = 0x9B00, mmap = 0x9C00;
    int hdr = -1;
    if(e->mb2){ say(p, "this is a 64-bit Multiboot2 kernel; Retro PC's CPU is 32-bit", 0); return -1; }
    /* the Multiboot header, in the first 8 KB on a 4-byte boundary */
    for(int i = 0; i + 12 <= klen && i < 8192; i += 4)
        if(rd32le(k + i) == 0x1BADB002u && rd32le(k + i) + rd32le(k + i + 4) + rd32le(k + i + 8) == 0){ hdr = i; break; }
    if(hdr < 0){ say(p, "no Multiboot header in ", e->path); return -1; }

    if(klen >= 52 && k[0] == 0x7F && k[1] == 'E' && k[2] == 'L' && k[3] == 'F' && !(rd32le(k + hdr + 4) & 0x10000u)){
        uint32_t phoff, phnum, phsz;
        if(k[4] != 1 || le16(k + 18) != 3){ say(p, "not a 32-bit x86 kernel: ", e->path); return -1; }
        p->entry = rd32le(k + 24);
        phoff = rd32le(k + 28); phsz = le16(k + 42); phnum = le16(k + 44);
        for(uint32_t i = 0; i < phnum; i++){
            const uint8_t *ph = k + phoff + i * phsz;
            uint32_t off, pa, fsz, msz;
            if(phoff + (i + 1) * phsz > (uint32_t)klen) break;
            if(rd32le(ph) != 1) continue;                /* PT_LOAD */
            off = rd32le(ph + 4); pa = rd32le(ph + 12); fsz = rd32le(ph + 16); msz = rd32le(ph + 20);
            if(pa + msz > p->mem_size || pa + msz < pa || off + fsz > (uint32_t)klen){
                say(p, "the kernel does not fit in Retro PC's memory", 0); return -1;
            }
            memcpy(p->mem + pa, k + off, fsz);
            memset(p->mem + pa + fsz, 0, msz - fsz);
        }
    } else if(rd32le(k + hdr + 4) & 0x10000u){          /* the a.out kludge: addresses in the header */
        uint32_t ha = rd32le(k + hdr + 12), la = rd32le(k + hdr + 16), le_ = rd32le(k + hdr + 20),
                 be = rd32le(k + hdr + 24), off = (uint32_t)hdr - (ha - la), ln = le_ ? le_ - la : (uint32_t)klen - off;
        if(be > p->mem_size || la + ln > p->mem_size || off + ln > (uint32_t)klen){ say(p, "the kernel does not fit in Retro PC's memory", 0); return -1; }
        memcpy(p->mem + la, k + off, ln);
        if(be > la + ln) memset(p->mem + la + ln, 0, be - la - ln);
        p->entry = rd32le(k + hdr + 28);
    } else { say(p, "the kernel is neither ELF nor a Multiboot image: ", e->path); return -1; }

    /* the Multiboot information, as GRUB would leave it */
    {
        char line[180]; int l = 0;
        const char *a = e->path;
        while(*a && l < 170) line[l++] = *a++;
        if(e->args[0] && l < 170){ line[l++] = ' '; for(a = e->args; *a && l < 178; a++) line[l++] = *a; }
        line[l] = 0;
        memcpy(p->mem + cmd, line, (size_t)l + 1);
        memcpy(p->mem + name, "HavenDOS Retro PC", 18);
    }
    {
        uint8_t *m = p->mem + mbi;
        uint32_t upper = (p->mem_size - 0x100000u) / 1024u;
        uint32_t ents[4][3] = { { 0, 0x9FC00, 1 }, { 0x9FC00, 0x400, 2 }, { 0xE0000, 0x20000, 2 },
                                { 0x100000, p->mem_size - 0x100000u, 1 } };
        memset(m, 0, 128);
        #define W32(o, v) do{ uint32_t _v = (v); memcpy(m + (o), &_v, 4); }while(0)
        W32(0, 0x1 | 0x4 | 0x40 | 0x200);
        W32(4, 639); W32(8, upper);
        W32(16, cmd);
        W32(44, 4 * 24); W32(48, mmap);
        W32(64, name);
        for(int i = 0; i < 4; i++){
            uint8_t *x = p->mem + mmap + i * 24;
            uint32_t sz = 20, hi = 0;
            memcpy(x, &sz, 4); memcpy(x + 4, &ents[i][0], 4); memcpy(x + 8, &hi, 4);
            memcpy(x + 12, &ents[i][1], 4); memcpy(x + 16, &hi, 4); memcpy(x + 20, &ents[i][2], 4);
        }
        #undef W32
    }
    scopy(p->kernel, e->path, sizeof p->kernel);
    cpu386_flat(&p->cpu, p->entry);
    p->cpu.r[X_EAX] = 0x2BADB002u;
    p->cpu.r[X_EBX] = mbi;
    p->cpu.r[X_ESP] = 0x7FF0;
    return 0;
}

/* The budget is run in slices, and the timer ticks that real time has
   owed since the last call are handed out one per slice -- each one
   only once the last has been taken, as an edge-triggered 8259 would.
   A guest that counts ticks stays in step with the clock on the wall
   over a second, if not over every millisecond. */
int pc386_run(pc386_t *p, uint32_t budget){
    uint32_t now = p->host->now_ms(p->hctx), dt = now - p->pit_last_ms, per = pit_period_us(p);
    int r = X86_BUDGET;
    p->pit_last_ms = now;
    if(per < 100) per = 100;                             /* 10 kHz at most */
    if(dt > 200) dt = 200;                               /* a stall is not a burst of ticks */
    p->pit_acc_us += dt * 1000u;
    if(p->pit_acc_us > per * 64u) p->pit_acc_us = per * 64u;
    while(budget){
        uint32_t slice = budget < 20000u ? budget : 20000u;
        if(p->pit_acc_us >= per && !(p->pic[0].irr & 1)){ irq(p, 0); p->pit_acc_us -= per; }
        kbc_feed(p);
        r = cpu386_run(&p->cpu, slice);
        budget -= slice;
        if(p->stop) return PC386_STOPPED;
        if(r == X86_FAULT){ say(p, "it crashed: ", p->cpu.fault_why); return r; }
        if(r != X86_BUDGET){
            /* idle: give it the ticks it is waiting for now, not next call */
            if(r == X86_IDLE && p->pit_acc_us >= per && !(p->pic[0].irr & 1)) continue;
            return r;
        }
    }
    return r;
}

void pc386_screen(const pc386_t *p, pc386_screen_t *s){
    uint16_t pos = (uint16_t)((p->crtc[0x0E] << 8) | p->crtc[0x0F]);
    s->cells = p->mem + 0xB8000;
    s->cols = 80; s->rows = 25;
    s->cx = pos % 80; s->cy = pos / 80;
    s->cursor_on = !(p->crtc[0x0A] & 0x20) && pos < 2000;
    s->graphics = (p->gc[6] & 1) != 0;
}
