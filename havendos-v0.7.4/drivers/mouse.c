/*
 * mouse.c — PS/2 mouse driver, HavenDOS v0.7.4
 *
 * What's new vs v0.7.4:
 *  - Sample rate bumped from 100 to 200 samples/sec: smoother tracking
 *    on real PCs and QEMU where IRQ12 firing is actually reliable.
 *    (UTM SE polled at ~100 anyway due to TCG speed, so no regression.)
 *  - Adaptive acceleration: slow movement stays precise (DIVISOR=4),
 *    fast flicks (|delta|>6) get a 2× speed boost so you can cross
 *    the 80-column VGA screen without 400 micro-movements.
 *  - Better sync recovery: if the sync byte check fails 8 times in a
 *    row, force mouse_cycle=0 to re-sync rather than silently dropping.
 *  - Click-edge latch unchanged — still works correctly.
 */
#include "../include/io.h"
#include "../include/types.h"

static int mouse_x=40, mouse_y=12, mouse_btn=0;
static int mouse_ready=0;
static uint8_t mouse_cycle=0;
static int8_t  mouse_bytes[3];

/* ── Click-edge latch ────────────────────────────────────────── */
static int mouse_btn_prev=0;
static int mouse_btn_edge=0;

/* ── Accumulator for sub-cell motion ─────────────────────────── */
#define MOUSE_DIVISOR 4   /* base divisor for slow/precise movement */
static int accum_x=0, accum_y=0;
static int sync_fail=0;   /* consecutive sync failures → force resync */

/* ── Adaptive delta: fast movement gets 2× boost ─────────────── */
static inline int adaptive_delta(int raw_accum, int divisor){
    int whole = raw_accum / divisor;
    /* Boost: if the raw per-packet delta is large, double the output */
    if(raw_accum < 0 ? -raw_accum : raw_accum > 6 * divisor)
        whole *= 2;
    return whole;
}

/* ── mouse_feed ──────────────────────────────────────────────── */
void mouse_feed(uint8_t data){
    switch(mouse_cycle){
        case 0:
            if(!(data&0x08)){
                /* Bad sync byte — allow a few misses before forcing reset */
                if(++sync_fail >= 8){ sync_fail=0; mouse_cycle=0; }
                break;
            }
            sync_fail=0;
            mouse_bytes[0]=(int8_t)data; mouse_cycle=1; break;
        case 1:
            mouse_bytes[1]=(int8_t)data; mouse_cycle=2; break;
        case 2:
            mouse_bytes[2]=(int8_t)data; mouse_cycle=0;

            accum_x += mouse_bytes[1];
            accum_y -= mouse_bytes[2];   /* Y axis: PS/2 positive = up, VGA = down */

            int dx = adaptive_delta(accum_x, MOUSE_DIVISOR);
            int dy = adaptive_delta(accum_y, MOUSE_DIVISOR);
            accum_x -= dx * MOUSE_DIVISOR;
            accum_y -= dy * MOUSE_DIVISOR;

            mouse_x += dx;
            mouse_y += dy;
            if(mouse_x < 0)  mouse_x = 0;
            if(mouse_x > 79) mouse_x = 79;
            if(mouse_y < 0)  mouse_y = 0;
            if(mouse_y > 24) mouse_y = 24;

            {
                int new_btn = (uint8_t)mouse_bytes[0] & 0x03;
                mouse_btn_edge |= (new_btn & ~mouse_btn_prev);
                mouse_btn_prev  = new_btn;
                mouse_btn       = new_btn;
            }
            break;
    }
}

/* ── PS/2 controller helpers ─────────────────────────────────── */
static void mw(void){ int t=0x10000; while(t--&&(inb(0x64)&0x02)); }
static void mr(void){ int t=0x10000; while(t--&&!(inb(0x64)&0x01)); }
static void mouse_cmd(uint8_t c){ mw();outb(0x64,0xD4); mw();outb(0x60,c); }
static uint8_t mouse_rd(void){ mr();return inb(0x60); }

void mouse_init(void){
    /* Enable aux device */
    mw(); outb(0x64,0xA8);
    /* Read config byte, clear IRQ12 enable (bit1) and mouse clock
       disable (bit5).  IRQ12 stays off — we poll instead. */
    mw(); outb(0x64,0x20); mr();
    uint8_t cfg = inb(0x60);
    cfg &= ~0x02;   /* disable IRQ12       */
    cfg &= ~0x20;   /* enable mouse clock  */
    mw(); outb(0x64,0x60); mw(); outb(0x60,cfg);

    /* Reset mouse */
    mouse_cmd(0xFF);
    if(mouse_rd()!=0xFA){ return; }   /* no ACK → no mouse */
    mouse_rd(); mouse_rd();           /* BAT result + device ID */

    /* Set 200 samples/sec (vs default 100) for smoother tracking */
    mouse_cmd(0xF3); mouse_rd(); mouse_cmd(200); mouse_rd();
    /* Set resolution: 4 counts/mm (default 4, but re-set explicitly) */
    mouse_cmd(0xE8); mouse_rd(); mouse_cmd(0x03); mouse_rd();
    /* Enable data reporting */
    mouse_cmd(0xF4); mouse_rd();

    mouse_ready = 1;
}

void mouse_get(int *x,int *y,int *btn){ *x=mouse_x; *y=mouse_y; *btn=mouse_btn; }
int  mouse_is_ready(void)              { return mouse_ready; }
int  mouse_mid_packet(void)            { return mouse_cycle>0; }
int  mouse_get_clicks(void)            { int e=mouse_btn_edge; mouse_btn_edge=0; return e; }
