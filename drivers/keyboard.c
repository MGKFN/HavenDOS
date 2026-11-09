#include "../include/keys.h"
#include "../include/io.h"
#include "../include/types.h"
#include "../include/klog.h"

extern void irq_register(int, void(*)(void));
extern void vga_flush(void);
extern void mouse_feed(uint8_t data);
extern int  mouse_mid_packet(void);

#define KBD_BUF 256
/* Drained from the two read paths below: no interrupt, no timer,
   the same rule every driver in this tree follows. */
extern void usb_hid_poll(void);

static char kbd_buf[KBD_BUF];
static int  kbd_head=0, kbd_tail=0;
static int  shift=0, shift_l=0, shift_r=0, ctrl=0, caps=0, e0=0;

static const char sc_lo[128]={
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,
    0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};
static const char sc_hi[128]={
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,
    0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};

static void push_key(int c){
    if(c>0xFF){
        /* v0.7.12: the two bytes of an extended key were pushed with a
           separate full/empty test each.  If the ring filled between them the
           0x01 marker went in and the keycode did not, so the next ordinary
           character read was consumed as an arrow-key payload and every read
           after it was shifted by one.  Reserve both slots up front. */
        int n1=(kbd_head+1)%KBD_BUF;
        int n2=(n1+1)%KBD_BUF;
        if(n1==kbd_tail||n2==kbd_tail) return;   /* no room for the pair */
        kbd_buf[kbd_head]=0x01;            kbd_head=n1;
        kbd_buf[kbd_head]=(char)(c&0xFF);  kbd_head=n2;
    } else {
        int n=(kbd_head+1)%KBD_BUF; if(n==kbd_tail)return;
        kbd_buf[kbd_head]=(char)c; kbd_head=n;
    }
}

/* v0.8.5. The one way into this queue from outside the PS/2 path.
   usb/hid.c calls it, so a key from a USB keyboard and a key from a
   PS/2 keyboard are indistinguishable by the time anything reads them
   -- including the extended codes for the arrows, which usb/hid.c
   takes from the table below rather than inventing its own. */
void keyboard_inject(int c){ push_key(c); }

static void process_sc(uint8_t sc){
    if(sc==0xE0){e0=1;return;}
    /* 0xFA=ACK, 0xFF=reset, 0x00=error - discard these.
       NOTE: 0xAA is the BREAK scancode for left shift (0x2A|0x80)
       so we must NOT discard it here - it falls through to rel logic. */
    if(sc==0xFA||sc==0xFF||sc==0x00)return;
    int rel=(sc&0x80); sc&=0x7F;
    if(e0){
        e0=0;   /* always clear - even if we don't handle this e0 sequence */
        if(!rel){
            switch(sc){
                case 0x48: push_key(0x148); return; /* Up    (ext) */
                case 0x50: push_key(0x150); return; /* Down  (ext) */
                case 0x4B: push_key(0x14B); return; /* Left  (ext) */
                case 0x4D: push_key(0x14D); return; /* Right (ext) */
                case 0x47: push_key(0x147); return; /* Home  (ext) */
                case 0x4F: push_key(0x14F); return; /* End   (ext) */
                case 0x49: push_key(0x149); return; /* PgUp  (ext) */
                case 0x51: push_key(0x151); return; /* PgDn  (ext) */
                case 0x52: push_key(0x152); return; /* Ins   (ext) */
                case 0x53: push_key(0x153); return; /* Del   (ext) */
                case 0x1C: push_key('\n');  return; /* Num Enter   */
                case 0x1D: ctrl=1;          return; /* Right Ctrl  */
                case 0x35: push_key('/');   return; /* Num /       */
                case 0x38: return;                  /* Right Alt   */
                /* v0.8.8.4: both Windows keys, as one code. They used
                   to fall past this switch and be discarded, so the
                   key did nothing anywhere in the system. */
                case 0x5B: push_key(KEY_SUPER); return; /* Left GUI  */
                case 0x5C: push_key(KEY_SUPER); return; /* Right GUI */
            }
        } else {
            /* Break codes for extended modifier keys */
            if(sc==0x1D){ ctrl=0; return; }
            /* The GUI keys' releases, swallowed so a release is not
               mistaken for a second press. */
            if(sc==0x5B||sc==0x5C){ return; }
            /* E0 AA = right-shift release on some layouts - clear shift */
            if(sc==0x36||sc==0x2A){ shift_r=0; shift_l=0; shift=0; return; }
        }
        return;
    }
    /* Track left/right shift separately - explicit set/clear */
    if(sc==0x2A){shift_l=(rel?0:1);shift=(shift_l||shift_r);return;}
    if(sc==0x36){shift_r=(rel?0:1);shift=(shift_l||shift_r);return;}
    if(sc==0x1D){ctrl=(rel?0:1);return;}
    if(sc==0x3A&&!rel){caps=!caps;return;}
    if(!rel){
        switch(sc){
            case 0x3B: push_key(0x13B); return;
            case 0x3C: push_key(0x13C); return;
            case 0x3D: push_key(0x13D); return;
            case 0x3E: push_key(0x13E); return;
            case 0x48: push_key(0x148); return; /* Up    */
            case 0x50: push_key(0x150); return; /* Down  */
            case 0x4B: push_key(0x14B); return; /* Left  */
            case 0x4D: push_key(0x14D); return; /* Right */
            case 0x47: push_key(0x147); return; /* Home  */
            case 0x4F: push_key(0x14F); return; /* End   */
            case 0x49: push_key(0x149); return; /* PgUp  */
            case 0x51: push_key(0x151); return; /* PgDn  */
            case 0x52: push_key(0x152); return; /* Ins   */
            case 0x53: push_key(0x153); return; /* Del   */
        }
    }
    if(rel||sc==0||sc>=128)return;
    int us=shift;
    if(caps&&sc_lo[sc]>='a'&&sc_lo[sc]<='z')us=!us;
    char c=us?sc_hi[sc]:sc_lo[sc];
    if(!c)return;
    if(ctrl&&c>='a'&&c<='z')c=c-'a'+1;
    push_key((int)(unsigned char)c);
}

/* ── talking to the 8042, with the delay v0.8.5.3 paid for ──────────
   v0.8.5.2 and v0.8.5.3 were both one bug: two 8042 accesses with no
   I/O delay between them. A controller emulated in SMM -- which is
   what the HP has, because it has no real one -- had not updated its
   status register by the time the driver read it again.

   v0.8.5.3 fixed kbc_drain(), the runtime read path. It did not touch
   this function, which talked to the same controller through the same
   ports with six hand-rolled loops of the same shape:

       static void kbc_wait_wr(void){ int t=0x10000; while(t--&&(inb(0x64)&0x02)); }
       int t=0x10000; while(t--&&(inb(0x64)&0x01)) inb(0x60);
       t=0x10000; while(t--&&!(inb(0x64)&0x01));

   tests/test_kbdinit.c measured seven violations in here, and none in
   kbc_drain. Two mechanisms:

     - Touch 0x60, then read 0x64 with no delay: OBF still reads set,
       so the byte just taken is taken again. The doubling, exactly.
     - Read 0x64 twice with no delay: a polling loop reading a register
       that cannot have changed. On an emulated controller it can see a
       stale "input buffer empty" and return while the buffer is still
       full -- and the command byte written next is DROPPED. A dropped
       outb(0x64,0x60) or outb(0x60,cfg) means the config write never
       lands: no IRQ1, or scancode translation left off, on a machine
       whose only keyboard is this one.

   Six loops became these five helpers, so the next correction lands
   once. Every wait is still bounded -- a UEFI laptop may have no 8042
   at all, and an init that hangs is worse than one that gives up. */
/* v0.8.5.6: the bound lives in include/io.h beside the delay,
   because the two have to move together -- see the note there. */

/* v0.8.5.6: the delay itself now lives in include/io.h as
   i8042_delay(), because drivers/mouse.c needs exactly the same rule
   and had exactly the same bug. See the comment there. */
#define kbc_delay i8042_delay

/* The delay goes BEFORE each status read, not after it.
   Putting it after looks equivalent and is not: a helper that returns
   straight after reading 0x64 leaves the next helper free to read 0x64
   again with nothing in between, and two status reads either side of a
   call boundary are just as adjacent as two inside one loop. That left
   three violations after the first attempt at this fix. Delaying first
   makes the rule unconditional and states itself in one sentence:
   every read of 0x64 in this driver is preceded by a delay. */

/* Input buffer empty, so a command or data byte can be written. */
static int kbc_wait_write(void){
    for(int t=I8042_SPINS; t; t--){
        kbc_delay();
        if(!(inb(0x64)&0x02)) return 1;
    }
    return 0;
}

/* A byte is waiting in the output buffer. */
static int kbc_wait_read(void){
    for(int t=I8042_SPINS; t; t--){
        kbc_delay();
        if(inb(0x64)&0x01) return 1;
    }
    return 0;
}

/* Drain whatever is sitting in the output buffer. */
static void kbc_flush(void){
    for(int t=I8042_SPINS; t; t--){
        kbc_delay();
        if(!(inb(0x64)&0x01)) return;
        (void)inb(0x60);
    }
}

static void kbc_cmd(uint8_t c){ kbc_wait_write(); outb(0x64,c); }
static void kbc_send(uint8_t d){ kbc_wait_write(); outb(0x60,d); }

static uint8_t kbc_recv(void){
    if(!kbc_wait_read()) return 0xFF;
    return inb(0x60);
}

static void kbd_irq(void); /* forward declaration */
void keyboard_init(void){
    /* Reset all modifier state */
    shift=0; shift_l=0; shift_r=0; ctrl=0; caps=0; e0=0;
    kbd_head=0; kbd_tail=0;

    kbc_flush();

    /* Disable kbd port during init */
    kbc_cmd(0xAD);

    /* Read+modify config: enable IRQ1, keep translation ON */
    kbc_cmd(0x20);
    uint8_t cfg = kbc_recv();
    cfg |=  0x41;   /* bit0=IRQ1 enable, bit6=scancode translation ON */
    cfg &= ~0x20;   /* bit5=0: keyboard clock enabled (was ~0x10, wrong bit) */
    kbc_cmd(0x60);
    kbc_send(cfg);

    /* Enable keyboard port (clears inhibit) */
    kbc_cmd(0xAE);

    kbc_flush();

    /* Enable scanning */
    kbc_send(0xF4);
    (void)kbc_recv();            /* discard ACK */

    kbc_flush();

    irq_register(1, kbd_irq);

    /* Hard-reset all modifier state after IRQ is registered.
       UTM SE (USB→PS/2 emulation) sometimes sends shift make/break
       scancodes during GRUB handoff that arrive before kbd_irq is
       registered, leaving shift_l=1 stuck in the buffer flush above.
       Drain any remaining bytes and force-clear everything.           */
    kbc_flush();
    shift=0; shift_l=0; shift_r=0; ctrl=0; e0=0;
    kbd_head=0; kbd_tail=0;
}

/* Poll keyboard port, skip mouse bytes (bit5 of status = aux data).
   Also force-routes mid-packet bytes to mouse_feed regardless of bit5,
   because external mice (physical USB/BT via UTM SE → iPadOS → QEMU)
   sometimes lose bit5 on packet bytes 1 and 2 - those bytes then fall
   into process_sc and produce spurious characters (e.g. dx=0x12=18px
   right → scancode 'e').  Once mouse_cycle>0 we know the next bytes
   belong to the mouse regardless of what the status register says. */
/* ── v0.8.5.3: ONE drain, and a delay after the data port ───────────
 *
 * Two bugs lived in the pair of identical loops that used to be here,
 * and together they double every keystroke on a UEFI laptop.
 *
 * 1. NO DELAY BETWEEN THE DATA PORT AND THE STATUS PORT.
 *
 *    `inb(0x60)` then straight back to `inb(0x64)` to see whether more
 *    is waiting. A real 8042 clears the output-buffer-full bit as the
 *    data read completes, so the loop ends. A laptop with no PS/2
 *    controller at all does not have a real 8042: the firmware emulates
 *    one in SMM, and the status register is updated by a handler that
 *    has not finished running yet. OBF still reads as 1, the loop goes
 *    round, reads 0x60 again -- and an 8042 read with nothing new in it
 *    hands back THE SAME BYTE. One keypress, two scancodes.
 *
 *    The fix is the io_wait() every real 8042 driver has and this one
 *    never did. It costs about a microsecond and is invisible on
 *    hardware that was already correct, which is why QEMU never showed
 *    any of this.
 *
 * 2. TWO INDEPENDENT DRAINERS.
 *
 *    kbd_irq() is registered on IRQ1 and kbd_poll() is called from both
 *    keyboard_getchar() and keyboard_haschar(). Both read the same
 *    hardware FIFO with a check-then-act on OBF, so the poll can test
 *    the status, lose the byte to the interrupt, and then read 0x60
 *    anyway -- getting the last byte a second time.
 *
 *    So the poll now stands down while the interrupt is demonstrably
 *    doing the work, and takes over again if it goes quiet. One owner
 *    at a time.
 */
static int      g_irq_bytes;      /* bytes the IRQ path has delivered  */
static uint32_t g_irq_last;       /* ...and when, in ticks             */
extern uint32_t get_ticks(void);

int  kbd_diag;                    /* set by `kbddiag` on the cmdline   */

/* kbc_delay() is defined above, with the init helpers -- v0.8.5.3 put
   it here, where only this function could see it, which is how
   keyboard_init() kept the bug for three more releases. */

static void kbc_drain(int from_irq){
    uint8_t status;
    int guard = 64;               /* a stuck OBF must not hang the box */

    while((status=inb(0x64))&0x01){
        uint8_t sc=inb(0x60);
        kbc_delay();              /* let the controller update 0x64 */
        if(from_irq){ g_irq_bytes++; g_irq_last = get_ticks(); }
        if(kbd_diag)
            klog(KLOG_INFO, "kbd: %2x from %s", (uint32_t)sc,
                 from_irq ? "irq" : "poll");
        if((status&0x20)||mouse_mid_packet()){ mouse_feed(sc); }
        else process_sc(sc);
        if(--guard <= 0) break;
    }
}

static void kbd_poll(void){
    /* Stand down while the interrupt is doing the work. If it stops --
       masked, or a firmware that raises it once and then not again --
       take over after a second rather than leaving the machine with no
       keyboard at all. */
    if(g_irq_bytes > 0 && (get_ticks() - g_irq_last) < 100u) return;
    kbc_drain(0);
}

static void kbd_irq(void){ kbc_drain(1); }

/* ── v0.8.8.1: THE POLL MOVED DOWN HERE, AND THIS IS WHY ─────────────
 *
 * v0.8.8 put kernel_poll() in keyboard_waitchar()'s wait loop and
 * called that "the idle point". It is AN idle point. It is not the one
 * the desktop uses.
 *
 * Every interactive loop in shell/desktop.c reads with the NON-blocking
 * keyboard_getchar() and spins on `pause` so it can service the mouse
 * at the same time -- see the file manager at desktop.c:1941. So on the
 * desktop the bottom half never ran at all: no hot-plug, no DHCP
 * retry, no carrier, for as long as anybody was using the machine the
 * way it is meant to be used. Reported as "the polling still doesn't
 * really work", and it was right.
 *
 * Polling from here covers both: waitchar() calls getchar() in its
 * loop, so its own call is gone rather than doubled.
 *
 * NOT from keyboard_haschar(), deliberately. net/net_tick.c's DHCP
 * client calls haschar() to avoid starting a four-second blocking
 * request while somebody is typing -- and it is called FROM
 * kernel_poll(). Polling there would recurse into the reentrancy
 * guard on every DHCP poll, which is harmless but would make
 * `kpoll`'s "already polling" counter climb for a reason that is not
 * a bug, and that counter is the one the hardware test plan says
 * should stay at zero. A predicate should not have a side effect that
 * large anyway.
 *
 * It is safe to poll from a function called this often because
 * kernel_poll() refuses by itself -- it is two integer compares when
 * nothing is due, and it returns KPOLL_EBUSY while any subsystem holds
 * a shared buffer. This call site does not have to know whether it is
 * safe, which is the whole point of the guard. */
extern int kernel_poll(void);

int keyboard_getchar(void){
    kernel_poll();
    kbd_poll();
    usb_hid_poll();   /* USB keyboards and mice, same queues */
    if(kbd_head==kbd_tail)return -1;
    unsigned char c=(unsigned char)kbd_buf[kbd_tail];
    if(c==0x01){
        /* v0.7.12 FIX - the read side of the extended-key tear.  The old code
           advanced kbd_tail PAST the 0x01 marker and only then checked whether
           the payload byte had arrived; if it had not, it returned -1 with the
           marker already consumed.  The payload then showed up on the next
           call as a bare character, so an arrow key turned into a stray 'H' or
           'P' in whatever field had focus.  Peek first, commit only when both
           bytes are present. */
        int second=(kbd_tail+1)%KBD_BUF;
        if(second==kbd_head) return -1;          /* payload not here yet */
        unsigned char c2=(unsigned char)kbd_buf[second];
        kbd_tail=(second+1)%KBD_BUF;
        return 0x100|(int)c2;
    }
    kbd_tail=(kbd_tail+1)%KBD_BUF;
    return (int)c;
}

/* The blocking wait. It polls through keyboard_getchar(), which is
   called on every turn of the loop below -- see the note there. */
int keyboard_waitchar(void){
    vga_flush(); /* push any shadow-buffered content to real VGA before blocking */
    int c;
    int idle=0;
    while((c=keyboard_getchar())==-1){
        __asm__ volatile("pause");
        /* After ~2 seconds of no key activity, force-clear e0 and shift.
           On UTM SE TCG, USB key events can leave e0=1 or shift stuck
           if the break scancode was lost during a context switch.       */
        if(++idle > 2000000){
            idle=0;
            e0=0;
            /* Only clear shift if BOTH left AND right report stuck —
               avoids clearing a legitimately held shift during typing.
               Since we can't distinguish, we clear if both are set
               (impossible on a real 2-key physical press simultaneously). */
            if(shift_l && shift_r){ shift_l=0; shift_r=0; shift=0; }
        }
    }
    return c;
}

int keyboard_haschar(void){
    kbd_poll();
    usb_hid_poll();   /* USB keyboards and mice, same queues */
    return kbd_head!=kbd_tail;
}
int kbd_shift(void){return shift;}
int kbd_ctrl(void){return ctrl;}
