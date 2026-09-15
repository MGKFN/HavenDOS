#include "../include/types.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/gfx.h"

#define M13_W    320
#define M13_H    200
#define M13_FB   ((uint8_t*)0xA0000)

static uint8_t backbuf[M13_W * M13_H];

/* Coordinate flip flags - set after detecting display orientation */
static int flip_x = 0;
static int flip_y = 0;

/* ---- Color palette indices ---- */
#define C_BLACK    0
#define C_DARKGREY 8
#define C_GREY     7
#define C_WHITE    15
#define C_BLUE     1
#define C_LBLUE    9
#define C_CYAN     3
#define C_LCYAN    11
#define C_GREEN    2
#define C_LGREEN   10
#define C_RED      4
#define C_LRED     12
#define C_YELLOW   14
#define C_MAGENTA  5
#define C_BROWN    6
#define C_TASKBAR  16
#define C_DESK     17
#define C_WINBG    18
#define C_ACCENT   19
#define C_SHADOW   20
#define C_WINBDR   21
#define C_TITLEBG  22
#define C_TITLEFG  23
#define C_ICONFG   24
#define C_ICONBG   25
#define C_SELBG    26
#define C_SELFG    27
#define C_MENUBD   28
#define C_MENUBG   29
#define C_BTN      30
#define C_BTNHI    31

static void set_palette(void){
    static const uint8_t cga[16][3]={
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},
        {42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},
        {63,21,21},{63,21,63},{63,63,21},{63,63,63}
    };
    outb(0x3C8,0);
    for(int i=0;i<16;i++){
        outb(0x3C9,cga[i][0]);
        outb(0x3C9,cga[i][1]);
        outb(0x3C9,cga[i][2]);
    }
    outb(0x3C8,16);
    /* C_TASKBAR  */ outb(0x3C9,10);outb(0x3C9,12);outb(0x3C9,22);
    /* C_DESK     */ outb(0x3C9,14);outb(0x3C9,18);outb(0x3C9,32);
    /* C_WINBG    */ outb(0x3C9,52);outb(0x3C9,52);outb(0x3C9,54);
    /* C_ACCENT   */ outb(0x3C9,10);outb(0x3C9,32);outb(0x3C9,63);
    /* C_SHADOW   */ outb(0x3C9,5); outb(0x3C9,5); outb(0x3C9,8);
    /* C_WINBDR   */ outb(0x3C9,30);outb(0x3C9,38);outb(0x3C9,55);
    /* C_TITLEBG  */ outb(0x3C9,8); outb(0x3C9,20);outb(0x3C9,50);
    /* C_TITLEFG  */ outb(0x3C9,63);outb(0x3C9,63);outb(0x3C9,63);
    /* C_ICONFG   */ outb(0x3C9,63);outb(0x3C9,63);outb(0x3C9,63);
    /* C_ICONBG   */ outb(0x3C9,12);outb(0x3C9,22);outb(0x3C9,45);
    /* C_SELBG    */ outb(0x3C9,10);outb(0x3C9,35);outb(0x3C9,63);
    /* C_SELFG    */ outb(0x3C9,63);outb(0x3C9,63);outb(0x3C9,63);
    /* C_MENUBD   */ outb(0x3C9,25);outb(0x3C9,32);outb(0x3C9,50);
    /* C_MENUBG   */ outb(0x3C9,45);outb(0x3C9,47);outb(0x3C9,52);
    /* C_BTN      */ outb(0x3C9,35);outb(0x3C9,40);outb(0x3C9,55);
    /* C_BTNHI    */ outb(0x3C9,15);outb(0x3C9,40);outb(0x3C9,63);
}

int mode13_try_enter(void){
    outb(0x3C4,0x00);outb(0x3C5,0x03);
    outb(0x3C4,0x01);outb(0x3C5,0x01);
    outb(0x3C4,0x02);outb(0x3C5,0x0F);
    outb(0x3C4,0x03);outb(0x3C5,0x00);
    outb(0x3C4,0x04);outb(0x3C5,0x0E);
    outb(0x3C2,0x63);
    outb(0x3D4,0x11);outb(0x3D5,0x0E);
    static const uint8_t crtc[]={
        0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,
        0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
        0x9C,0x0E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF
    };
    for(int i=0;i<25;i++){outb(0x3D4,i);outb(0x3D5,crtc[i]);}
    static const uint8_t gc[]={0,0,0,0,0,0x40,0x05,0x0F,0xFF};
    for(int i=0;i<9;i++){outb(0x3CE,i);outb(0x3CF,gc[i]);}
    static const uint8_t ac[]={
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
        0x41,0x00,0x0F,0x00,0x00
    };
    inb(0x3DA);
    for(int i=0;i<21;i++){outb(0x3C0,i);outb(0x3C0,ac[i]);}
    outb(0x3C0,0x20);
    set_palette();

    /* Write a test pattern to detect orientation:
       Write a marker pixel at top-left logical position (0,0)
       then check if it appears at framebuffer offset 0 or end */
    M13_FB[0] = 0xFF;  /* should be top-left */
    M13_FB[M13_W*M13_H-1] = 0xFE; /* bottom-right */

    /* We can't read back easily on UTM SE, so use cmdline flag instead.
       Default: assume UTM SE flips both axes based on observed behavior */
    /* flip_x and flip_y are set by mode13_set_flip() from main */

    /* Clear the test pixels */
    M13_FB[0] = 0;
    M13_FB[M13_W*M13_H-1] = 0;

    /* Re-enable keyboard after VGA mode switch.
       Some emulators (UTM SE) need this after register writes. */
    {
        int t;
        /* Wait for KBC ready */
        t=0x10000; while(t--&&(inb(0x64)&0x02));
        /* Re-enable keyboard port */
        outb(0x64, 0xAE);
        /* Flush any pending bytes */
        t=0x10000; while(t--&&(inb(0x64)&0x01)) inb(0x60);
        /* Re-enable scanning */
        t=0x10000; while(t--&&(inb(0x64)&0x02));
        outb(0x60, 0xF4);
        t=0x10000; while(t--&&!(inb(0x64)&0x01));
        if(inb(0x64)&0x01) inb(0x60); /* discard ACK */
    }

    return 1;
}

void mode13_set_flip(int fx, int fy){ flip_x=fx; flip_y=fy; }

void mode13_clear(uint8_t color){
    memset(backbuf,color,M13_W*M13_H);
}

/* Core putpixel with flip correction */
void mode13_putpixel(int x,int y,uint8_t color){
    if(x<0||x>=M13_W||y<0||y>=M13_H)return;
    int rx = flip_x ? (M13_W-1-x) : x;
    int ry = flip_y ? (M13_H-1-y) : y;
    if(rx<0||rx>=M13_W||ry<0||ry>=M13_H)return; /* safety */
    backbuf[ry*M13_W+rx]=color;
}

void mode13_rect(int x,int y,int w,int h,uint8_t color){
    /* Use memset for rows when no flip_x for speed */
    if(!flip_x){
        for(int dy=0;dy<h;dy++){
            int ry = flip_y ? (M13_H-1-(y+dy)) : (y+dy);
            if(ry<0||ry>=M13_H)continue;
            int rx = x; if(rx<0)rx=0;
            int rw = w; if(rx+rw>M13_W)rw=M13_W-rx;
            if(rw>0) memset(backbuf+ry*M13_W+rx, color, rw);
        }
    } else {
        for(int dy=0;dy<h;dy++)
            for(int dx=0;dx<w;dx++)
                mode13_putpixel(x+dx,y+dy,color);
    }
}

static void hline(int x,int y,int w,uint8_t c){
    if(!flip_x){
        int ry = flip_y ? (M13_H-1-y) : y;
        if(ry<0||ry>=M13_H)return;
        int rx=x; if(rx<0)rx=0;
        int rw=w; if(rx+rw>M13_W)rw=M13_W-rx;
        if(rw>0) memset(backbuf+ry*M13_W+rx, c, rw);
    } else {
        for(int i=0;i<w;i++) mode13_putpixel(x+i,y,c);
    }
}
static void vline(int x,int y,int h,uint8_t c){
    for(int i=0;i<h;i++) mode13_putpixel(x,y+i,c);
}

/* 8x8 bitmap font */
static const uint8_t font8x8[96][8]={
    {0,0,0,0,0,0,0,0},{0x18,0x3C,0x3C,0x18,0x18,0,0x18,0},
    {0x36,0x36,0,0,0,0,0,0},{0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0},
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0},{0,0x63,0x33,0x18,0x0C,0x66,0x63,0},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0},{0x06,0x06,0x03,0,0,0,0,0},
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0},{0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0},
    {0,0x66,0x3C,0xFF,0x3C,0x66,0,0},{0,0x0C,0x0C,0x3F,0x0C,0x0C,0,0},
    {0,0,0,0,0,0x0C,0x0C,0x06},{0,0,0,0x3F,0,0,0,0},
    {0,0,0,0,0,0x0C,0x0C,0},{0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0},
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0},{0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0},{0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0},
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0},{0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0},
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0},{0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0},{0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0},
    {0,0x0C,0x0C,0,0x0C,0x0C,0,0},{0,0x0C,0x0C,0,0x0C,0x0C,0x06,0},
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0},{0,0,0x3F,0,0x3F,0,0,0},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0},{0x1E,0x33,0x30,0x18,0x0C,0,0x0C,0},
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0},{0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0},{0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0},{0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0},
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0},{0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0},{0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0},{0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0},
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0},{0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0},{0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0},{0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0},
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0},{0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0},{0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0},{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0},
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0},{0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0},{0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0},
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0},{0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0},
    {0x08,0x1C,0x36,0x63,0,0,0,0},{0,0,0,0,0,0,0,0xFF},
    {0x0C,0x0C,0x18,0,0,0,0,0},{0,0,0x1E,0x30,0x3E,0x33,0x6E,0},
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0},{0,0,0x1E,0x33,0x03,0x33,0x1E,0},
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0},{0,0,0x1E,0x33,0x3f,0x03,0x1E,0},
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0},{0,0,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0},{0x0C,0,0x0E,0x0C,0x0C,0x0C,0x1E,0},
    {0x30,0,0x30,0x30,0x30,0x33,0x33,0x1E},{0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0},
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0},{0,0,0x33,0x7F,0x7F,0x6B,0x63,0},
    {0,0,0x1F,0x33,0x33,0x33,0x33,0},{0,0,0x1E,0x33,0x33,0x33,0x1E,0},
    {0,0,0x3B,0x66,0x66,0x3E,0x06,0x0F},{0,0,0x6E,0x33,0x33,0x3E,0x30,0x78},
    {0,0,0x3B,0x6E,0x66,0x06,0x0F,0},{0,0,0x3E,0x03,0x1E,0x30,0x1F,0},
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0},{0,0,0x33,0x33,0x33,0x33,0x6E,0},
    {0,0,0x33,0x33,0x33,0x1E,0x0C,0},{0,0,0x63,0x6B,0x7F,0x7F,0x36,0},
    {0,0,0x63,0x36,0x1C,0x36,0x63,0},{0,0,0x33,0x33,0x33,0x3E,0x30,0x1F},
    {0,0,0x3F,0x19,0x0C,0x26,0x3F,0},{0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0},
    {0x18,0x18,0x18,0,0x18,0x18,0x18,0},{0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0},
    {0x6E,0x3B,0,0,0,0,0,0},{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
};

void mode13_text(int x,int y,const char *s,uint8_t fg,uint8_t bg){
    int ox=x;
    while(*s){
        if(*s=='\n'){x=ox;y+=10;s++;continue;}
        int idx=(unsigned char)*s-32;
        if(idx>=0&&idx<96){
            for(int row=0;row<8;row++){
                uint8_t bits=font8x8[idx][row];
                for(int col=0;col<8;col++){
                    mode13_putpixel(x+col,y+row,(bits>>(7-col))&1?fg:bg);
                }
            }
        }
        x+=8; s++;
    }
}

static void mode13_flip(void){
    memcpy(M13_FB,backbuf,M13_W*M13_H);
}

/* ---- UI helpers ---- */
__attribute__((unused)) static void draw_window(int x,int y,int w,int h,const char *title){
    mode13_rect(x+2,y+2,w,h,C_SHADOW);
    mode13_rect(x,y,w,h,C_WINBG);
    mode13_rect(x,y,w,16,C_TITLEBG);
    hline(x,y,w,C_ACCENT);
    hline(x,y+h-1,w,C_WINBDR);
    vline(x,y,h,C_WINBDR);
    vline(x+w-1,y,h,C_WINBDR);
    if(title){
        int tx=x+(w-(int)strlen(title)*8)/2;
        if(tx<x+2)tx=x+2;
        mode13_text(tx,y+4,title,C_TITLEFG,C_TITLEBG);
    }
    /* X button */
    mode13_rect(x+w-14,y+2,12,12,C_RED);
    mode13_text(x+w-11,y+4,"x",C_WHITE,C_RED);
}

static void draw_taskbar(const char *title, uint32_t ticks){
    mode13_rect(0,0,M13_W,18,C_TASKBAR);
    hline(0,17,M13_W,C_WINBDR);
    hline(0,0,M13_W,C_ACCENT);
    mode13_text(4,5,"BOOT OS",C_WHITE,C_TASKBAR);
    vline(58,2,14,C_WINBDR);
    if(title){
        int tx=M13_W/2-(int)strlen(title)*4;
        mode13_text(tx,5,title,C_WHITE,C_TASKBAR);
    }
    int s=(ticks/100)%60,m=(ticks/6000)%60,h=((ticks/360000)+12)%24;
    char tb[10];
    tb[0]='0'+h/10;tb[1]='0'+h%10;tb[2]=':';
    tb[3]='0'+m/10;tb[4]='0'+m%10;tb[5]=':';
    tb[6]='0'+s/10;tb[7]='0'+s%10;tb[8]=0;
    mode13_text(M13_W-66,5,tb,C_WHITE,C_TASKBAR);
    /* Status bar */
    mode13_rect(0,M13_H-12,M13_W,12,C_TASKBAR);
    hline(0,M13_H-12,M13_W,C_WINBDR);
    mode13_text(4,M13_H-9,"ESC=Menu  Arrows=Nav  Enter=Open",C_GREY,C_TASKBAR);
}

static void draw_desktop_bg(void){
    /* Fast checkerboard - write whole rows at once */
    for(int y=18;y<M13_H-12;y++){
        int fy = flip_y ? (M13_H-1-y) : y;
        uint8_t *row = backbuf + fy*M13_W;
        if(!flip_x){
            for(int x=0;x<M13_W;x++)
                row[x] = (x/4+y/4)%2==0 ? C_DESK : C_TASKBAR;
        } else {
            for(int x=0;x<M13_W;x++)
                row[M13_W-1-x] = (x/4+y/4)%2==0 ? C_DESK : C_TASKBAR;
        }
    }
}

typedef struct{const char*name;const char*icon;}m13app_t;
static m13app_t m13apps[]={
    {"Shell",">_"},{"Calc","+-"},{"Editor","Ed"},
    {"Files","FM"},{"Info","Si"},{"Clock","Cl"},
    {"Todo","Td"},{"Settings","St"},{"Snake","Sn"},{"Power","Pw"},
};
#define M13_NAPP 10

static void draw_icons(int sel){
    for(int i=0;i<M13_NAPP;i++){
        int col=i%2,row=i/2;
        int x=6+col*56,y=22+row*32;
        int s=(i==sel);
        uint8_t ibg=s?C_SELBG:C_ICONBG;
        mode13_rect(x,y,48,22,ibg);
        hline(x,y,48,s?C_ACCENT:C_WINBDR);
        hline(x,y+21,48,s?C_ACCENT:C_WINBDR);
        vline(x,y,22,s?C_ACCENT:C_WINBDR);
        vline(x+47,y,22,s?C_ACCENT:C_WINBDR);
        mode13_text(x+16,y+7,m13apps[i].icon,C_WHITE,ibg);
        int nl=strlen(m13apps[i].name);
        mode13_text(x+(48-nl*8)/2,y+24,m13apps[i].name,s?C_ACCENT:C_GREY,C_DESK);
    }
}

static void draw_infopanel(uint32_t ticks){
    extern uint32_t pmm_free_blocks(void);
    extern uint32_t pmm_total_blocks(void);
    extern const char *current_username(void);
    int px=122,py=22,pw=194,ph=78;
    mode13_rect(px+2,py+2,pw,ph,C_SHADOW);
    mode13_rect(px,py,pw,ph,C_WINBG);
    mode13_rect(px,py,pw,14,C_TITLEBG);
    hline(px,py,pw,C_ACCENT);
    mode13_text(px+4,py+3,"System Info",C_WHITE,C_TITLEBG);
    uint32_t f=pmm_free_blocks(),t=pmm_total_blocks();
    char rb[24]; utoa(f*4,rb,10); strcat(rb," KB free");
    mode13_text(px+4,py+18,"RAM:",C_GREY,C_WINBG);
    mode13_text(px+36,py+18,rb,C_LGREEN,C_WINBG);
    int bw=pw-12,used=t>0?(int)((t-f)*bw/t):0;
    mode13_rect(px+4,py+30,bw,6,C_DARKGREY);
    mode13_rect(px+4,py+30,used,6,C_ACCENT);
    char up[20]; utoa(ticks/100,up,10); strcat(up,"s");
    mode13_text(px+4,py+40,"Uptime:",C_GREY,C_WINBG);
    mode13_text(px+60,py+40,up,C_WHITE,C_WINBG);
    mode13_text(px+4,py+52,"User:",C_GREY,C_WINBG);
    mode13_text(px+44,py+52,current_username(),C_LGREEN,C_WINBG);
    mode13_text(px+4,py+64,"v0.3.0",C_GREY,C_WINBG);
}

static void draw_startmenu(int sel){
    int mx=0,mw=116,mh=M13_NAPP*13+20;
    int my=M13_H-12-mh;
    mode13_rect(mx+2,my+2,mw,mh,C_SHADOW);
    mode13_rect(mx,my,mw,mh,C_MENUBG);
    mode13_rect(mx,my,mw,14,C_TITLEBG);
    hline(mx,my,mw,C_ACCENT);
    mode13_text(mx+4,my+3,"Start Menu",C_WHITE,C_TITLEBG);
    for(int i=0;i<M13_NAPP;i++){
        int iy=my+15+i*13;
        int s=(i==sel);
        if(s)mode13_rect(mx+1,iy,mw-2,13,C_SELBG);
        mode13_text(mx+4,iy+3,m13apps[i].icon,s?C_WHITE:C_ACCENT,s?C_SELBG:C_MENUBG);
        mode13_text(mx+20,iy+3,m13apps[i].name,s?C_WHITE:C_BLACK,s?C_SELBG:C_MENUBG);
    }
}

/* Keyboard in graphics mode - pure port polling, no IRQ needed */
static int gfx_getchar(void){
    extern int keyboard_getchar(void);
    return keyboard_getchar();
}
static int gfx_waitchar(void){
    int c;
    while((c=gfx_getchar())==-1)
        __asm__ volatile("pause");
    return c;
}

void mode13_desktop_run(void){
    extern uint32_t get_ticks(void);
    extern void shell_run(void);
    extern void vga_init(void);

    int menu_open=0,menu_sel=0,icon_sel=0,needs_draw=1;

    while(1){
        if(needs_draw){
            uint32_t t=get_ticks();
            mode13_clear(C_DESK);
            draw_desktop_bg();
            draw_taskbar("Desktop",t);
            draw_icons(menu_open?-1:icon_sel);
            draw_infopanel(t);
            if(menu_open) draw_startmenu(menu_sel);
            mode13_flip();
            needs_draw=0;
        }

        int c=gfx_waitchar();
        needs_draw=1;

        if(c==27){ menu_open=!menu_open; menu_sel=0; }
        else if(menu_open){
            if(c==0x148&&menu_sel>0)          menu_sel--;
            if(c==0x150&&menu_sel<M13_NAPP-1) menu_sel++;
            if(c=='\n'||c=='\r'){
                menu_open=0;
                if(menu_sel==0){
                    /* Restore text mode fully before shell */
                    vga_init();
                    vga_clear();
                    vga_set_color(VGA_WHITE, VGA_BLACK);
                    shell_run();
                    /* Re-enter graphics mode */
                    mode13_try_enter();
                    mode13_clear(C_DESK);
                } else {
                    /* Simple app placeholder - minimal rendering to avoid crash */
                    mode13_clear(C_DESK);
                    mode13_rect(60,70,200,60,C_WINBG);
                    mode13_rect(60,70,200,14,C_TITLEBG);
                    mode13_text(80,75,m13apps[menu_sel].name,C_WHITE,C_TITLEBG);
                    mode13_text(68,94,"Coming soon!",C_BLACK,C_WINBG);
                    mode13_text(68,106,"Press ESC",C_GREY,C_WINBG);
                    mode13_flip();
                    while(gfx_waitchar()!=27);
                }
                needs_draw=1;
            }
        } else {
            if(c==0x148&&icon_sel>=2)            icon_sel-=2;
            if(c==0x150&&icon_sel+2<M13_NAPP)   icon_sel+=2;
            if(c==0x14B&&icon_sel%2>0)           icon_sel--;
            if(c==0x14D&&icon_sel%2==0&&icon_sel+1<M13_NAPP) icon_sel++;
            if(c=='\n'||c=='\r'){
                if(icon_sel==0){
                    vga_init();
                    vga_clear();
                    vga_set_color(VGA_WHITE, VGA_BLACK);
                    shell_run();
                    mode13_try_enter();
                    mode13_clear(C_DESK);
                } else {
                    mode13_clear(C_DESK);
                    mode13_rect(60,70,200,60,C_WINBG);
                    mode13_rect(60,70,200,14,C_TITLEBG);
                    mode13_text(80,75,m13apps[icon_sel].name,C_WHITE,C_TITLEBG);
                    mode13_text(68,94,"Coming soon!",C_BLACK,C_WINBG);
                    mode13_text(68,106,"Press ESC",C_GREY,C_WINBG);
                    mode13_flip();
                    while(gfx_waitchar()!=27);
                }
                needs_draw=1;
            }
        }
    }
}
