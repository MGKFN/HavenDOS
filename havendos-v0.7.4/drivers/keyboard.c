#include "../include/io.h"
#include "../include/types.h"

extern void irq_register(int, void(*)(void));
extern void vga_flush(void);
extern void mouse_feed(uint8_t data);
extern int  mouse_mid_packet(void);

#define KBD_BUF 256
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
        int n=(kbd_head+1)%KBD_BUF; if(n==kbd_tail)return;
        kbd_buf[kbd_head]=0x01; kbd_head=n;
        n=(kbd_head+1)%KBD_BUF; if(n==kbd_tail)return;
        kbd_buf[kbd_head]=(char)(c&0xFF); kbd_head=n;
    } else {
        int n=(kbd_head+1)%KBD_BUF; if(n==kbd_tail)return;
        kbd_buf[kbd_head]=(char)c; kbd_head=n;
    }
}

static void process_sc(uint8_t sc){
    if(sc==0xE0){e0=1;return;}
    /* 0xFA=ACK, 0xFF=reset, 0x00=error — discard these.
       NOTE: 0xAA is the BREAK scancode for left shift (0x2A|0x80)
       so we must NOT discard it here — it falls through to rel logic. */
    if(sc==0xFA||sc==0xFF||sc==0x00)return;
    int rel=(sc&0x80); sc&=0x7F;
    if(e0){
        e0=0;   /* always clear — even if we don't handle this e0 sequence */
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
            }
        } else {
            /* Break codes for extended modifier keys */
            if(sc==0x1D){ ctrl=0; return; }
            /* E0 AA = right-shift release on some layouts — clear shift */
            if(sc==0x36||sc==0x2A){ shift_r=0; shift_l=0; shift=0; return; }
        }
        return;
    }
    /* Track left/right shift separately — explicit set/clear */
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

static void kbc_wait_wr(void){ int t=0x10000; while(t--&&(inb(0x64)&0x02)); }
static void kbd_irq(void); /* forward declaration */
void keyboard_init(void){
    /* Reset all modifier state */
    shift=0; shift_l=0; shift_r=0; ctrl=0; caps=0; e0=0;
    kbd_head=0; kbd_tail=0;

    /* Flush */
    int t=0x10000; while(t--&&(inb(0x64)&0x01)) inb(0x60);

    /* Disable kbd port during init */
    kbc_wait_wr(); outb(0x64,0xAD);

    /* Read+modify config: enable IRQ1, keep translation ON */
    kbc_wait_wr(); outb(0x64,0x20);
    t=0x10000; while(t--&&!(inb(0x64)&0x01));
    uint8_t cfg=inb(0x60);
    cfg |=  0x41;   /* bit0=IRQ1 enable, bit6=scancode translation ON */
    cfg &= ~0x20;   /* bit5=0: keyboard clock enabled (was ~0x10, wrong bit) */
    kbc_wait_wr(); outb(0x64,0x60);
    kbc_wait_wr(); outb(0x60,cfg);

    /* Enable keyboard port (clears inhibit) */
    kbc_wait_wr(); outb(0x64,0xAE);

    /* Flush again */
    t=0x10000; while(t--&&(inb(0x64)&0x01)) inb(0x60);

    /* Enable scanning */
    kbc_wait_wr(); outb(0x60,0xF4);
    t=0x10000; while(t--&&!(inb(0x64)&0x01));
    inb(0x60); /* discard ACK */

    /* Flush */
    t=0x10000; while(t--&&(inb(0x64)&0x01)) inb(0x60);

    irq_register(1, kbd_irq);

    /* Hard-reset all modifier state after IRQ is registered.
       UTM SE (USB→PS/2 emulation) sometimes sends shift make/break
       scancodes during GRUB handoff that arrive before kbd_irq is
       registered, leaving shift_l=1 stuck in the buffer flush above.
       Drain any remaining bytes and force-clear everything.           */
    {
        int t=0x8000;
        while(t--&&(inb(0x64)&0x01)) inb(0x60);
    }
    shift=0; shift_l=0; shift_r=0; ctrl=0; e0=0;
    kbd_head=0; kbd_tail=0;
}

/* Poll keyboard port, skip mouse bytes (bit5 of status = aux data).
   Also force-routes mid-packet bytes to mouse_feed regardless of bit5,
   because external mice (physical USB/BT via UTM SE → iPadOS → QEMU)
   sometimes lose bit5 on packet bytes 1 and 2 — those bytes then fall
   into process_sc and produce spurious characters (e.g. dx=0x12=18px
   right → scancode 'e').  Once mouse_cycle>0 we know the next bytes
   belong to the mouse regardless of what the status register says. */
static void kbd_poll(void){
    uint8_t status;
    while((status=inb(0x64))&0x01){
        uint8_t sc=inb(0x60);
        if((status&0x20)||mouse_mid_packet()){ mouse_feed(sc); continue; }
        process_sc(sc);
    }
}

static void kbd_irq(void){
    uint8_t status;
    while((status=inb(0x64))&0x01){
        uint8_t sc=inb(0x60);
        if((status&0x20)||mouse_mid_packet()){ mouse_feed(sc); continue; }
        process_sc(sc);
    }
}

int keyboard_getchar(void){
    kbd_poll();
    if(kbd_head==kbd_tail)return -1;
    unsigned char c=(unsigned char)kbd_buf[kbd_tail];
    kbd_tail=(kbd_tail+1)%KBD_BUF;
    if(c==0x01){
        if(kbd_head==kbd_tail)return -1;
        unsigned char c2=(unsigned char)kbd_buf[kbd_tail];
        kbd_tail=(kbd_tail+1)%KBD_BUF;
        return 0x100|(int)c2;
    }
    return (int)c;
}

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
    return kbd_head!=kbd_tail;
}
int kbd_shift(void){return shift;}
int kbd_ctrl(void){return ctrl;}
