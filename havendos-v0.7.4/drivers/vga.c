#include "../include/vga.h"
#include "../include/io.h"
#include "../include/string.h"
#include <stdarg.h>

#define VGA_MEM ((uint16_t*)0xB8000)
static int cur_x=0, cur_y=0;
static vga_color_t cur_fg=VGA_LIGHT_GREY, cur_bg=VGA_BLACK;

/* ── Shadow buffer — anti-flicker double-buffering ──────────────────
   ARCHITECTURE:
   vga_write()  — write-through: shadow + real VGA together, skip if
                  cell unchanged.  Fast path for all normal drawing.

   vga_clear()  — shadow-only: blanks the shadow, does NOT touch real
                  VGA.  This eliminates the classic "white flash":
                  without this fix, clearing VGA then redrawing took
                  1-2 full 14ms VGA refresh cycles on PC hardware and
                  accelerated VM backends (VirtualBox, VMware, QEMU
                  KVM/HVF) — visible as obvious flicker.  On UTM SE
                  TCG the bug was invisible because the software
                  renderer committed every frame atomically.

   vga_flush()  — diffs shadow against real VGA and writes only cells
                  that differ.  Called at the END of full_draw() after
                  the entire new frame is in the shadow buffer.  This
                  means VGA is updated from old frame → new frame in
                  one pass; the user never sees a blank intermediate
                  state.  Also called from keyboard_waitchar() to push
                  any buffered content before blocking.

   vga_shadow_write() / vga_shadow_read() — cursor bypass: the mouse
                  cursor must read and write individual cells without
                  going through the normal path, so it uses these
                  direct accessors that update both shadow and VGA.   */
static uint16_t shadow[VGA_HEIGHT*VGA_WIDTH];

static uint16_t make_entry(char c, vga_color_t fg, vga_color_t bg){
    return (uint16_t)(uint8_t)c | ((uint16_t)((bg<<4)|fg)<<8);
}

/* Write-through: shadow + VGA together, skip if unchanged */
static inline void vga_write(int x, int y, uint16_t entry){
    /* BUG FIX: bounds check — out-of-bounds y (e.g. from long theme lists
       in Settings) wrote past shadow[] into kernel heap/stack, hanging the
       system. Silently discard any write outside the 80×25 VGA grid. */
    if((unsigned)x >= VGA_WIDTH || (unsigned)y >= VGA_HEIGHT) return;
    int idx=y*VGA_WIDTH+x;
    if(shadow[idx]!=entry){
        shadow[idx]=entry;
        VGA_MEM[idx]=entry;
    }
}

/* Cursor bypass — writes to both shadow and VGA directly */
void vga_shadow_write(int x, int y, uint16_t val){
    int idx=y*VGA_WIDTH+x;
    shadow[idx]=val;
    VGA_MEM[idx]=val;
}
uint16_t vga_shadow_read(int x, int y){ return shadow[y*VGA_WIDTH+x]; }

/* Sync shadow → VGA.  Called at the end of full_draw() after all UI
   elements have been drawn into the shadow buffer.  Any cell that
   differs between shadow and real VGA (including background cells
   left blank by the shadow-only vga_clear()) gets written here in
   one pass, so the screen updates atomically from the user's
   perspective — no intermediate blank frame is ever visible.       */
void vga_flush(void){
    uint16_t *vmem=VGA_MEM;
    for(int i=0;i<VGA_HEIGHT*VGA_WIDTH;i++)
        if(vmem[i]!=shadow[i]) vmem[i]=shadow[i];
}

static void update_hw_cursor(void){
    uint16_t pos=(uint16_t)(cur_y*VGA_WIDTH+cur_x);
    outb(0x3D4,14); outb(0x3D5,(uint8_t)(pos>>8));
    outb(0x3D4,15); outb(0x3D5,(uint8_t)(pos&0xFF));
}

void vga_init(void){
    cur_x=0;cur_y=0;cur_fg=VGA_LIGHT_GREY;cur_bg=VGA_BLACK;
    for(int i=0;i<VGA_HEIGHT*VGA_WIDTH;i++) shadow[i]=0xFFFF;
    vga_clear(); vga_flush(); update_hw_cursor();
}

/* Shadow-only clear: writes to shadow buffer ONLY, never touches real VGA.
   This is the key anti-flicker fix for PC VM environments (VirtualBox,
   VMware, QEMU with hardware-accelerated VGA):
     - Old behaviour: clear → VGA blank (visible white flash) → redraw
     - New behaviour: clear → shadow blank → redraw → vga_flush() diffs
       only the cells that are STILL blank after the new frame is drawn.
   On UTM SE TCG the difference was invisible (TCG renders every frame
   in one shot), but on accelerated PC VGA the blank frame was visible
   for 1-2 full VGA refresh cycles (~14ms each) = obvious flicker.
   vga_flush() at the end of full_draw() handles any cells that stayed
   blank (background areas not covered by UI elements). */
void vga_clear(void){
    uint16_t blank=make_entry(' ',cur_fg,cur_bg);
    for(int i=0;i<VGA_HEIGHT*VGA_WIDTH;i++) shadow[i]=blank;
    cur_x=0;cur_y=0;update_hw_cursor();
}

void vga_scroll(void){
    for(int y=0;y<VGA_HEIGHT-1;y++)
        for(int x=0;x<VGA_WIDTH;x++)
            vga_write(x,y,shadow[(y+1)*VGA_WIDTH+x]);
    for(int x=0;x<VGA_WIDTH;x++)
        vga_write(x,VGA_HEIGHT-1,make_entry(' ',cur_fg,cur_bg));
    cur_y=VGA_HEIGHT-1;
}
void vga_putchar(char c){
    if(c=='\n'){cur_x=0;cur_y++;if(cur_y>=VGA_HEIGHT)vga_scroll();update_hw_cursor();return;}
    if(c=='\r'){cur_x=0;update_hw_cursor();return;}
    if(c=='\b'){if(cur_x>0){cur_x--;vga_write(cur_x,cur_y,make_entry(' ',cur_fg,cur_bg));}update_hw_cursor();return;}
    if(c=='\t'){int ns=(cur_x+8)&~7;while(cur_x<ns&&cur_x<VGA_WIDTH)vga_write(cur_x++,cur_y,make_entry(' ',cur_fg,cur_bg));update_hw_cursor();return;}
    vga_write(cur_x,cur_y,make_entry(c,cur_fg,cur_bg));
    if(++cur_x>=VGA_WIDTH){cur_x=0;cur_y++;}
    if(cur_y>=VGA_HEIGHT)vga_scroll();
    update_hw_cursor();
}
void vga_puts(const char *s){ while(*s)vga_putchar(*s++); }
void vga_set_color(vga_color_t fg, vga_color_t bg){ cur_fg=fg; cur_bg=bg; }
void vga_set_cursor(int x, int y){ cur_x=x;cur_y=y;update_hw_cursor(); }
void vga_get_cursor(int *x, int *y){ *x=cur_x;*y=cur_y; }
void vga_putchar_at(char c, int x, int y, vga_color_t fg, vga_color_t bg){
    vga_write(x,y,make_entry(c,fg,bg));
}
void vga_puts_at(const char *s, int x, int y, vga_color_t fg, vga_color_t bg){
    while(*s&&x<VGA_WIDTH)vga_putchar_at(*s++,x++,y,fg,bg);
}
void vga_fill_row(int y, char c, vga_color_t fg, vga_color_t bg){
    for(int x=0;x<VGA_WIDTH;x++)vga_putchar_at(c,x,y,fg,bg);
}
void vga_printf(const char *fmt,...){
    va_list args; va_start(args,fmt); char buf[32];
    while(*fmt){
        if(*fmt=='%'){fmt++;
            switch(*fmt){
                case 'd':{ int v=va_arg(args,int); itoa(v,buf,10); vga_puts(buf); break;}
                case 'u':{ uint32_t v=va_arg(args,uint32_t); utoa(v,buf,10); vga_puts(buf); break;}
                case 'x':{ uint32_t v=va_arg(args,uint32_t); utoa(v,buf,16); vga_puts(buf); break;}
                case 's':{ char *s=va_arg(args,char*); vga_puts(s?s:"(null)"); break;}
                case 'c':{ vga_putchar((char)va_arg(args,int)); break;}
                case '%': vga_putchar('%'); break;
            }
        } else vga_putchar(*fmt);
        fmt++;
    }
    va_end(args);
}
void vga_getchar_at(int x, int y, uint8_t *ch, uint8_t *attr){
    uint16_t v=shadow[y*VGA_WIDTH+x];
    *ch=(uint8_t)(v&0xFF); *attr=(uint8_t)(v>>8);
}
void vga_putraw_at(uint8_t ch, uint8_t attr, int x, int y){
    vga_write(x,y,(uint16_t)((attr<<8)|ch));
}
