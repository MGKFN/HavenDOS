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
/* v0.8.5.6: the pointer clamp needs the active grid. Declared rather
   than pulling in all of vga.h, which would drag the colour enum into
   a driver that draws nothing. */
extern int vga_cols(void);
extern int vga_rows(void);

static int mouse_x=40, mouse_y=12, mouse_btn=0;

/* ── v0.8.8.1: "IS THERE A POINTING DEVICE" IS NOT "DID PS/2 ACK" ─────
 *
 * This was one flag, `mouse_ready`, set in exactly one place: the end
 * of mouse_init(), i.e. the PS/2 reset having been acknowledged.
 *
 * usb/hid.c synthesises PS/2-shaped packets and pushes them through
 * mouse_feed(), so a USB mouse updates mouse_x and mouse_y perfectly
 * well -- and never touched that flag. Every consumer gates on it:
 *
 *     shell/ui.c:317        if(!mouse_is_ready()) return;   <- no cursor
 *     shell/desktop.c:890   if(mouse_is_ready()) ui_draw_cursor();
 *     shell/desktop.c:1474  if(mouse_is_ready()){ ...clicks... }
 *     shell/chat.c:1741     if(mouse_is_ready() && mouse_get_clicks())
 *
 * So on a laptop whose trackpad does not answer the PS/2 reset -- which
 * is the HP, reported 31 October -- a working USB mouse is tracked and
 * INVISIBLE. Both pointers dead, one cause.
 *
 * It is the bus-dispatch mistake this tree has now fixed three times in
 * other layers: one driver's private flag answering a question about
 * the whole subsystem. net/net.c held `nic_type` and three switches on
 * it; four screens each worked out a disk's bus label independently.
 * The rule each time was that the thing which DECIDES has to be one
 * place a grep can see.
 *
 * So there are two sources and one answer. Each source says only what
 * it knows, and mouse_is_ready() is the only function that combines
 * them. A third kind of pointing device adds a source here and changes
 * nothing anywhere else.
 */
static int ps2_ready=0;      /* the PS/2 reset was acknowledged      */
static int usb_pointers=0;   /* USB HID mice currently registered    */
static int i2c_pointers=0;   /* I2C-HID trackpads (v0.8.8.11)        */
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
    /* v0.7.17 FIX — precedence bug that made the pointer asymmetric.
     *
     * The old test was:
     *     if(raw_accum < 0 ? -raw_accum : raw_accum > 6 * divisor)
     *
     * `>` binds tighter than `?:`, so that parses as
     *     if( raw_accum < 0 ? (-raw_accum) : (raw_accum > 6*divisor) )
     *
     * For a NEGATIVE accumulator the condition is `-raw_accum`, a positive
     * number, which is always true — so every leftward and upward movement
     * got the 2x boost regardless of speed, while rightward and downward
     * movement was boosted only when genuinely fast.  The pointer moved
     * twice as far left as right for the same hand motion, which reads as
     * the mouse "drifting" or "fighting you" and is why precise clicking
     * has always been awkward.
     *
     * Take the magnitude first, then compare. */
    int mag = raw_accum < 0 ? -raw_accum : raw_accum;
    if(mag > 6 * divisor) whole *= 2;
    return whole;
}

/* ── mouse_apply ──────────────────────────────────────────────────────
   The accumulator, the acceleration, the clamp and the button edge --
   ONE copy, reached by every pointing device there is.

   This was the body of mouse_feed()'s case 2 and was lifted out
   unchanged in v0.8.8.11, when an I2C trackpad became a third source
   of movement. This file's own header says why it must not be copied:
   "a second copy of that arithmetic is a second place for the
   v0.7.17 acceleration-precedence bug to live." A third source would
   have been a third place.

   dx and dy are already in VGA's sense: positive x is right, positive
   y is DOWN. A caller whose hardware disagrees flips the sign on the
   way in, which is what mouse_feed() does for PS/2's inverted Y.
   `new_btn` is a PS/2-style mask: 1 left, 2 right. */
static void mouse_apply(int raw_dx, int raw_dy, int new_btn)
{
    int dx, dy;

    accum_x += raw_dx;
    accum_y += raw_dy;

    dx = adaptive_delta(accum_x, MOUSE_DIVISOR);
    dy = adaptive_delta(accum_y, MOUSE_DIVISOR);
    accum_x -= dx * MOUSE_DIVISOR;
    accum_y -= dy * MOUSE_DIVISOR;

    mouse_x += dx;
    mouse_y += dy;
    /* v0.8.5.6: clamped to the ACTIVE grid, not to 80x25. These were
       79 and 24 written out, so on 80x50, 90x60 or the framebuffer
       console the pointer could not be moved past column 79 or row
       24 -- most of the screen was unreachable by mouse. */
    {
        int maxx = vga_cols() - 1, maxy = vga_rows() - 1;
        if(maxx < 0) maxx = 0;
        if(maxy < 0) maxy = 0;
        if(mouse_x < 0)     mouse_x = 0;
        if(mouse_x > maxx)  mouse_x = maxx;
        if(mouse_y < 0)     mouse_y = 0;
        if(mouse_y > maxy)  mouse_y = maxy;
    }

    mouse_btn_edge |= (new_btn & ~mouse_btn_prev);
    mouse_btn_prev  = new_btn;
    mouse_btn       = new_btn;
}

/* An I2C-HID trackpad, through the same arithmetic. drivers/i2c_hid.c
   has already turned absolute finger positions into deltas -- that
   part belongs there, because only it knows when the finger was
   lifted -- and what arrives here is movement in VGA's sense. */
void mouse_feed_i2c(int dx, int dy, int buttons)
{
    mouse_apply(dx, dy, buttons & 3);
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

            /* Y axis: PS/2 positive = up, VGA = down. */
            mouse_apply(mouse_bytes[1], -mouse_bytes[2],
                        (uint8_t)mouse_bytes[0] & 0x03);
            break;
    }
}

/* ── PS/2 controller helpers ─────────────────────────────────── */
/* v0.8.5.6: i8042_delay() before every status read.
   These two loops were the v0.8.5.3 bug, untouched. That release found
   that two 8042 accesses with no I/O delay between them make an SMM
   emulated controller serve a stale status register -- which doubled
   every keystroke -- and fixed kbc_drain() in drivers/keyboard.c. It
   did not fix keyboard_init(), and it did not fix this file, which
   drives the SAME controller through the SAME two ports.

   For the mouse the stale read costs differently: mr() returning early
   means mouse_rd() reads 0x60 before a byte is there and gets the
   previous one, so a three-byte movement packet is parsed one byte out
   of phase -- which reads as a pointer that jumps. mw() returning early
   means the 0xD4 prefix or the command byte after it is dropped, and a
   mouse that misses its ACK is a mouse this driver gives up on
   (`if(mouse_rd()!=0xFA) return;`).

   The delay goes BEFORE the read, not after -- see include/io.h. */
static void mw(void){
    for(int t=I8042_SPINS; t; t--){ i8042_delay(); if(!(inb(0x64)&0x02)) return; }
}
static void mr(void){
    for(int t=I8042_SPINS; t; t--){ i8042_delay(); if(inb(0x64)&0x01) return; }
}
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

    ps2_ready = 1;
}

/* ── the USB side's half of the answer ───────────────────────────────
 *
 * usb/hid.c calls these as it rebuilds its table. The pair mirrors
 * usbnet_release()/usbnet_probe() around a rescan, and for the same
 * reason: usb_hid_init() sets its device count back to zero and walks
 * the bus again, so a count kept here and never reset would survive a
 * mouse being unplugged and keep claiming a pointer that is gone.
 *
 * A count rather than a flag, because two mice is a thing people do
 * and unplugging one of them must not take the pointer away. */
void mouse_usb_forget_all(void){ usb_pointers = 0; }
void mouse_usb_attach(void)    { usb_pointers++; }
/* A trackpad does not come and go, so there is no forget for it: an
   I2C device is soldered down and a count that only rises is the
   truth about one. */
void mouse_i2c_attach(void)    { i2c_pointers++; }
int  mouse_i2c_count(void)     { return i2c_pointers; }

/* ── v0.8.8.11: READING THE POINTER PUMPS THE USB MOUSE ───────────────
 *
 * PS/2 pushes: a packet arrives at port 0x60 and mouse_feed() is
 * called from the interrupt path, so the cached position is current
 * whenever anybody looks at it.
 *
 * USB does not. usb_hid_poll() has to go and ask, and it was called
 * from exactly two places -- drivers/keyboard.c's two read paths. So
 * on a machine whose only pointer is a USB mouse, the pointer moved
 * only when something read the KEYBOARD.
 *
 * ── THE HANG THAT MADE THIS URGENT ───────────────────────────────────
 *
 * shell/desktop.c, Notepad's right-click menu:
 *
 *     if(mb&2){                                  // right button down
 *         do { mouse_get(&mx,&my,&mb); } while(mb&2);   // wait for release
 *
 * Nothing in that loop reads the keyboard, so with a USB mouse
 * nothing polled USB, so mouse_btn never changed, so the loop never
 * ended -- and it reads no keys either, so there was no way out of
 * it. A right-click in Notepad froze the machine. mouse_is_ready()
 * returns true for a USB-only pointer (`ps2_ready || usb_pointers`),
 * which is correct and is what made the block reachable.
 *
 * That is also exactly how a working mouse gets reported as a broken
 * one: the mouse was fine, Notepad was not.
 *
 * Fixing the one loop would have left the next one to be written.
 * Draining USB whenever anybody asks where the pointer IS makes the
 * cached position mean the same thing for both buses, which is what
 * every caller already assumed.
 *
 * ── REENTRANCY ───────────────────────────────────────────────────────
 *
 * usb_hid_poll() -> mouse_report() -> mouse_feed() does not call back
 * in here today, and the guard is here so that it cannot start to:
 * a mouse_get() reached from inside a USB transfer would recurse
 * through the host controller, which is the one thing the polled
 * design has no way to survive.
 */
extern void usb_hid_poll(void);
extern void i2c_hid_poll(void);
static int mouse_in_sync = 0;

void mouse_sync(void)
{
    /* Both polled buses, for the same reason: neither pushes. */
    if(usb_pointers <= 0 && i2c_pointers <= 0) return;
    if(mouse_in_sync) return;          /* see above */
    mouse_in_sync = 1;
    if(usb_pointers > 0) usb_hid_poll();
    if(i2c_pointers > 0) i2c_hid_poll();
    mouse_in_sync = 0;
}

void mouse_get(int *x,int *y,int *btn){
    mouse_sync();
    *x=mouse_x; *y=mouse_y; *btn=mouse_btn;
}

/* ── pull the pointer back onto the current LOGICAL grid ──────────────
   The clamp in mouse_feed() runs when a packet arrives, so it is the
   right thing in the steady state and does nothing at the moment the
   grid changes under it. v0.8.8.4 made that moment common: entering an
   app sets an 80x25 viewport on a wider panel, and a pointer last seen
   at column 85 then sits outside the app's console until the mouse is
   moved. An app hit-testing against a coordinate outside its own box
   finds the wrong cell or nothing at all.

   Called by whoever changes the grid. Cheap, and idempotent. */
void mouse_clamp_to_grid(void)
{
    int maxx = vga_cols() - 1, maxy = vga_rows() - 1;
    if(maxx < 0) maxx = 0;
    if(maxy < 0) maxy = 0;
    if(mouse_x > maxx) mouse_x = maxx;
    if(mouse_y > maxy) mouse_y = maxy;
    if(mouse_x < 0) mouse_x = 0;
    if(mouse_y < 0) mouse_y = 0;
}

/* THE one answer. Nothing else in the tree may ask a driver directly:
   the question is "can the user point at something", and neither
   driver knows that on its own. */
int  mouse_is_ready(void)              { return ps2_ready
                                                || usb_pointers > 0
                                                || i2c_pointers > 0; }

/* And the two halves, named separately -- for the BOOT LOG, which must
   report what actually happened rather than what was attempted. A log
   line that says "mouse polled" on a machine with no mouse is exactly
   what tools/check_boot_log.py exists to refuse, and kernel/main.c
   printed one unconditionally for every release up to and including
   v0.8.8. */
int  mouse_ps2_ready(void)             { return ps2_ready; }
int  mouse_usb_count(void)             { return usb_pointers; }
int  mouse_mid_packet(void)            { return mouse_cycle>0; }
/* Pumped for the same reason mouse_get() is: a caller that only ever
   asks "was there a click" is a caller that never reads the keyboard,
   and shell/chat.c is one. */
int  mouse_get_clicks(void)            { mouse_sync();
                                         { int e=mouse_btn_edge; mouse_btn_edge=0; return e; } }
