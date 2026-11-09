#include "../include/types.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/gfx.h"

/* VESA VBE via Multiboot framebuffer tag.
   We request the framebuffer from GRUB and use whatever we get. */

static uint32_t *fb_addr  = 0;
static uint32_t  fb_width = 0;
static uint32_t  fb_height= 0;
static uint32_t  fb_pitch = 0;
static int       fb_ready = 0;

/* Called from kernel_main with the MB1 framebuffer info if available */
void vesa_set_framebuffer(uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch){
    fb_addr   = (uint32_t*)addr;
    fb_width  = w;
    fb_height = h;
    fb_pitch  = pitch/4; /* in 32-bit words */
    fb_ready  = (addr != 0 && w > 0 && h > 0);
}

int vesa_try_enter(uint32_t width, uint32_t height){
    /* GRUB sets up VESA for us if we request it in the multiboot header.
       Since we use MB1 without a framebuffer tag, we can't request it
       at compile time. Instead we try to detect if GRUB gave us one. */
    (void)width; (void)height;
    return fb_ready;
}

void vesa_clear(uint32_t color){
    if(!fb_ready) return;
    for(uint32_t y=0;y<fb_height;y++)
        for(uint32_t x=0;x<fb_width;x++)
            fb_addr[y*fb_pitch+x]=color;
}

void vesa_putpixel(int x,int y,uint32_t color){
    if(!fb_ready||x<0||y<0||(uint32_t)x>=fb_width||(uint32_t)y>=fb_height)return;
    fb_addr[y*fb_pitch+x]=color;
}

void vesa_rect(int x,int y,int w,int h,uint32_t color){
    for(int dy=0;dy<h;dy++)
        for(int dx=0;dx<w;dx++)
            vesa_putpixel(x+dx,y+dy,color);
}

void vesa_desktop_run(void){
    /* Simple placeholder - just show what resolution we got */
    extern int keyboard_waitchar(void);
    extern void vga_init(void);
    extern void desktop_run(void);

    if(!fb_ready){
        vga_init();
        desktop_run();
        return;
    }
    /* Blue desktop with text */
    vesa_clear(0x00182848);
    /* White text in top-left */
    /* (no font renderer for VESA yet - show basic colored blocks) */
    vesa_rect(0,0,fb_width,24,0x00102030);
    vesa_rect(20,6,fb_width/3,12,0x0020A0FF);

    /* Wait then fall back to text */
    volatile int t=0; while(t++<5000000);
    vga_init();
    desktop_run();
}
