/*
 * ui.c — HavenDOS v0.6.8
 * Modern TUI: taskbar, themes, corner clock, box-drawing windows
 */
#include "../include/theme.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/rtc.h"

extern uint32_t get_ticks(void);
extern void mouse_get(int*,int*,int*);
extern int  mouse_is_ready(void);
extern const char *current_username(void);
extern int         current_user_admin(void);

/* ================================================================
   THEMES  — 10 modern palettes
   ================================================================ */
static theme_t themes[]={
 /* bar_bg         bar_fg       desk_bg       win_bg         win_fg
    title_bg       title_fg     accent        icon_bg        icon_fg   name */
 /* ── Base 10 themes (always available) ─────────────────────────── */
 {VGA_DARK_GREY,  VGA_CYAN,    VGA_BLACK,    VGA_BLACK,     VGA_LIGHT_GREY,
  VGA_DARK_GREY,  VGA_CYAN,    VGA_CYAN,     VGA_DARK_GREY, VGA_CYAN,    "Midnight Cyan"},
 {VGA_BLUE,       VGA_WHITE,   VGA_BLACK,    VGA_BLACK,     VGA_LIGHT_GREY,
  VGA_BLUE,       VGA_WHITE,   VGA_LIGHT_CYAN,VGA_BLUE,     VGA_WHITE,   "Classic Blue"},
 {VGA_BLACK,      VGA_LIGHT_GREEN,VGA_BLACK, VGA_BLACK,     VGA_LIGHT_GREEN,
  VGA_BLACK,      VGA_LIGHT_GREEN,VGA_GREEN, VGA_BLACK,     VGA_LIGHT_GREEN,"Terminal Green"},
 {VGA_BLACK,      VGA_YELLOW,  VGA_BLACK,    VGA_BLACK,     VGA_YELLOW,
  VGA_BLACK,      VGA_YELLOW,  VGA_YELLOW,   VGA_BLACK,     VGA_YELLOW,  "Amber"},
 {VGA_DARK_GREY,  VGA_WHITE,   VGA_BLACK,    VGA_BLACK,     VGA_LIGHT_GREY,
  VGA_DARK_GREY,  VGA_WHITE,   VGA_LIGHT_GREY,VGA_DARK_GREY,VGA_WHITE,   "Dark"},
 {VGA_LIGHT_BLUE, VGA_WHITE,   VGA_BLACK,    VGA_BLACK,     VGA_WHITE,
  VGA_LIGHT_BLUE, VGA_WHITE,   VGA_LIGHT_CYAN,VGA_LIGHT_BLUE,VGA_WHITE,  "Arctic"},
 {VGA_BLACK,      VGA_MAGENTA, VGA_BLACK,    VGA_BLACK,     VGA_LIGHT_CYAN,
  VGA_BLACK,      VGA_MAGENTA, VGA_MAGENTA,  VGA_BLACK,     VGA_CYAN,    "Neon"},
 {VGA_GREEN,      VGA_BLACK,   VGA_BLACK,    VGA_BLACK,     VGA_LIGHT_GREY,
  VGA_GREEN,      VGA_BLACK,   VGA_LIGHT_GREEN,VGA_GREEN,   VGA_BLACK,   "Mint"},
 {VGA_RED,        VGA_WHITE,   VGA_BLACK,    VGA_BLACK,     VGA_LIGHT_GREY,
  VGA_RED,        VGA_WHITE,   VGA_LIGHT_RED, VGA_RED,      VGA_WHITE,   "Sunset"},
 {VGA_BROWN,      VGA_WHITE,   VGA_BLACK,    VGA_BLACK,     VGA_LIGHT_GREY,
  VGA_RED,        VGA_WHITE,   VGA_RED,      VGA_BROWN,     VGA_WHITE,   "Retro"},
 /* ── Extra 10 themes (unlocked by themes-extra package) ─────────── */
 {VGA_BLACK,      VGA_LIGHT_RED,VGA_BLACK,   VGA_BLACK,     VGA_LIGHT_RED,
  VGA_BLACK,      VGA_LIGHT_RED,VGA_RED,     VGA_BLACK,     VGA_LIGHT_RED,"Blood"},
 {VGA_LIGHT_GREY, VGA_BLACK,   VGA_BLACK,    VGA_DARK_GREY, VGA_BLACK,
  VGA_LIGHT_GREY, VGA_BLACK,   VGA_DARK_GREY,VGA_LIGHT_GREY,VGA_BLACK,   "Monochrome"},
 {VGA_CYAN,       VGA_BLACK,   VGA_BLACK,    VGA_BLACK,     VGA_CYAN,
  VGA_CYAN,       VGA_BLACK,   VGA_LIGHT_CYAN,VGA_CYAN,     VGA_BLACK,   "Ocean"},
 {VGA_DARK_GREY,  VGA_MAGENTA, VGA_BLACK,    VGA_BLACK,     VGA_MAGENTA,
  VGA_DARK_GREY,  VGA_MAGENTA, VGA_LIGHT_MAGENTA,VGA_DARK_GREY,VGA_MAGENTA,"Purple Haze"},
 {VGA_BLACK,      VGA_WHITE,   VGA_BLACK,    VGA_BLACK,     VGA_WHITE,
  VGA_BLACK,      VGA_WHITE,   VGA_WHITE,    VGA_DARK_GREY, VGA_WHITE,   "Ghost"},
 {VGA_BROWN,      VGA_YELLOW,  VGA_BLACK,    VGA_BLACK,     VGA_YELLOW,
  VGA_BROWN,      VGA_YELLOW,  VGA_YELLOW,   VGA_BROWN,     VGA_YELLOW,  "Desert"},
 {VGA_BLUE,       VGA_CYAN,    VGA_BLACK,    VGA_BLACK,     VGA_CYAN,
  VGA_BLUE,       VGA_CYAN,    VGA_LIGHT_CYAN,VGA_BLUE,     VGA_CYAN,    "Sapphire"},
 {VGA_GREEN,      VGA_YELLOW,  VGA_BLACK,    VGA_BLACK,     VGA_YELLOW,
  VGA_GREEN,      VGA_YELLOW,  VGA_LIGHT_GREEN,VGA_GREEN,   VGA_YELLOW,  "Matrix Gold"},
 {VGA_DARK_GREY,  VGA_LIGHT_RED,VGA_BLACK,   VGA_BLACK,     VGA_LIGHT_RED,
  VGA_RED,        VGA_WHITE,   VGA_LIGHT_RED,VGA_DARK_GREY, VGA_LIGHT_RED,"Volcano"},
 {VGA_LIGHT_BLUE, VGA_BLACK,   VGA_BLACK,    VGA_BLACK,     VGA_LIGHT_BLUE,
  VGA_LIGHT_BLUE, VGA_BLACK,   VGA_CYAN,     VGA_LIGHT_BLUE,VGA_BLACK,   "Ice"},
};
#define NTHEMES_BASE  10
#define NTHEMES_EXTRA 20   /* total when themes-extra installed */
#define NTHEMES_ALL   20
static int cur_theme    = 0;
static int g_extra_themes = 0;   /* set to 1 when themes-extra is installed */

extern int vfs_write(const char*, const char*, uint32_t);
extern int vfs_read(const char*, char*, uint32_t);

#define THEME_FILE ".theme"

static int nthemes_active(void){ return g_extra_themes ? NTHEMES_ALL : NTHEMES_BASE; }

theme_t    *ui_theme(void)          { return &themes[cur_theme]; }
void        ui_set_theme(int n)     {
    int max = nthemes_active();
    if(n>=0&&n<max){
        cur_theme=n;
        /* store as two-char decimal so we can handle n>=10 */
        char buf[6];
        if(n<10){ buf[0]='0'+n; buf[1]='\n'; buf[2]=0; vfs_write(THEME_FILE,buf,2); }
        else    { buf[0]='0'+n/10; buf[1]='0'+n%10; buf[2]='\n'; buf[3]=0; vfs_write(THEME_FILE,buf,3); }
    }
}
int         ui_theme_count(void)    { return nthemes_active(); }
int         ui_current_theme(void)  { return cur_theme; }
const char *ui_theme_name(int n)    { int max=nthemes_active(); return (n>=0&&n<max)?themes[n].name:"?"; }

void ui_unlock_extra_themes(void)  { g_extra_themes=1; }
int  ui_extra_themes_unlocked(void){ return g_extra_themes; }

void ui_load_theme(void){
    extern int bpkg_is_installed(const char*);
    if(bpkg_is_installed("themes-extra")) g_extra_themes=1;
    char buf[8];
    int r=vfs_read(THEME_FILE, buf, sizeof(buf)-1);
    if(r>0){
        buf[r]=0;
        int n=0;
        if(buf[1]>='0'&&buf[1]<='9') n=(buf[0]-'0')*10+(buf[1]-'0');
        else n=buf[0]-'0';
        int max=nthemes_active();
        if(n>=0&&n<max) cur_theme=n;
    }
}

/* Preview a theme in-memory only — no disk write; used by Settings live preview */
void ui_load_preview(int n){
    if(n>=0&&n<nthemes_active()) cur_theme=n;
}

/* ================================================================
   TASKBAR — slim, modern, HavenDOS branding
   ================================================================ */

/* ── App taskbar (top bar only) — used by all apps ────────────── */
void ui_draw_taskbar(const char *title){
    theme_t *th=ui_theme();
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,th->bar_fg,th->bar_bg);
    /* Left: back/close hint */
    vga_puts_at(" ESC=Close",0,0,th->bar_fg,th->bar_bg);
    /* Center: app title */
    if(title&&title[0]){
        int tlen=(int)strlen(title);
        int tx=(80-tlen)/2; if(tx<12)tx=12;
        vga_puts_at(title,tx,0,th->accent,th->bar_bg);
    }
    /* Right: HH:MM */
    uint8_t h,m,s2; rtc_get_time(&h,&m,&s2);
    char tb[6];
    tb[0]='0'+h/10;tb[1]='0'+h%10;tb[2]=':';
    tb[3]='0'+m/10;tb[4]='0'+m%10;tb[5]=0;
    vga_puts_at(tb,74,0,th->accent,th->bar_bg);
}

/* ── Desktop two-bar layout ─────────────────────────────────────
   Row  0 : thin top status bar  (version . HavenDOS . clock)
   Row 24 : main taskbar (start button + title + sysinfo)
   ────────────────────────────────────────────────────────────── */
void ui_draw_desktop_bars(void){
    theme_t *th=ui_theme();

    /* Row 0 — top status bar */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,th->bar_fg,th->bar_bg);
    vga_puts_at("HavenDOS v0.7.4",1,0,th->bar_fg,th->bar_bg);
    uint8_t h,m,s2; rtc_get_time(&h,&m,&s2);
    char tb[9];
    tb[0]='0'+h/10;tb[1]='0'+h%10;tb[2]=':';
    tb[3]='0'+m/10;tb[4]='0'+m%10;tb[5]=':';
    tb[6]='0'+s2/10;tb[7]='0'+s2%10;tb[8]=0;
    /* Row 0 time removed — desktop tick writes date | time to row 0 */

    /* Row 24 — main taskbar */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,th->bar_fg,th->bar_bg);
    vga_puts_at("  HAVENDOS    ",0,24,th->bar_bg,th->accent);
    vga_putchar_at('.',14,24,th->accent,th->bar_bg);
    const char *un=current_username();
    int ulen=(int)strlen(un);
    vga_puts_at(un,17,24,th->accent,th->bar_bg);
    if(current_user_admin()) vga_puts_at(".admin",17+ulen,24,th->bar_fg,th->bar_bg);
    /* Notification bell indicator col 55 — show unread count if any */
    extern int notif_get_unread(void);
    int nunu = notif_get_unread();
    vga_puts_at("[!]",55,24,nunu>0?th->accent:th->bar_fg,th->bar_bg);
    if(nunu > 0){
        char nc[4]; nc[0]='('; nc[1]='0'+nunu; nc[2]=')'; nc[3]=0;
        if(nunu>9){ nc[1]='9'; nc[2]='+'; nc[3]=')'; nc[4]=0; }
        vga_puts_at("NOTIF",59,24,th->accent,th->bar_bg);
        vga_puts_at(nc,65,24,th->accent,th->bar_bg);
    } else {
        vga_puts_at("NOTIF",59,24,th->bar_fg,th->bar_bg);
        vga_puts_at("   ",65,24,th->bar_fg,th->bar_bg); /* clear old count */
    }
    vga_puts_at(tb,71,24,th->accent,th->bar_bg);
}

/* ── Corner clock — updates top status bar clock only ────────── */
void ui_draw_corner_clock(void){
    uint8_t h,m,s2; rtc_get_time(&h,&m,&s2);
    char tb[9];
    tb[0]='0'+h/10;tb[1]='0'+h%10;tb[2]=':';
    tb[3]='0'+m/10;tb[4]='0'+m%10;tb[5]=':';
    tb[6]='0'+s2/10;tb[7]='0'+s2%10;tb[8]=0;
    theme_t *th=ui_theme();
    vga_puts_at(tb,71,0,th->accent,th->bar_bg);
    /* Row 24 taskbar time is updated on full redraw only — no duplicate here */
}

/* ================================================================
   DESKTOP BACKGROUND
   Per-theme background using only safe ASCII chars (UTM SE safe).
   Themes 0,1,5,7,8,9 = plain black.
   Themes 2,4 = sparse dots '.', Themes 3 = sparse '+', 6 = sparse '|'
   ================================================================ */
void ui_draw_desktop(void){
    theme_t *th=ui_theme();
    for(int y=1;y<24;y++){
        for(int x=0;x<80;x++){
            char c=' ';
            vga_color_t fg=th->desk_bg;
            switch(cur_theme){
            case 0: /* Midnight Cyan — sparse dots in a grid */
                if(x%8==0&&y%4==0){ c='.'; fg=VGA_DARK_GREY; }
                break;
            case 1: /* Classic Blue — horizontal rule every 6 rows */
                if(y%6==0){ c='-'; fg=VGA_BLUE; }
                break;
            case 2: /* Terminal Green — matrix rain dots */
                if((x*3+y*7)%17==0){ c='.'; fg=VGA_GREEN; }
                else if((x*7+y*2)%31==0){ c='0'+((x+y)%10); fg=VGA_DARK_GREY; }
                break;
            case 3: /* Amber — diagonal hatching */
                if((x+y)%12==0||(x-y+80)%12==0){ c='.'; fg=VGA_BROWN; }
                break;
            case 4: /* Dark — subtle corner brackets */
                if((x%20==0&&y%10==0)||(x%20==1&&y%10==0)||(x%20==0&&y%10==1))
                    { c='.'; fg=VGA_DARK_GREY; }
                break;
            case 5: /* Arctic — sparse snowflakes */
                if((x*5+y*11)%53==0){ c='*'; fg=VGA_LIGHT_BLUE; }
                else if((x*11+y*3)%47==0){ c='.'; fg=VGA_DARK_GREY; }
                break;
            case 6: /* Neon — vertical bar accents */
                if(x%16==0){ c='|'; fg=VGA_DARK_GREY; }
                if(x%16==0&&y%5==0){ c='+'; fg=VGA_MAGENTA; }
                break;
            case 7: /* Mint — horizontal wave */
                { int wave=(x%16<8)?0:1; if((y+wave)%6==0){ c='-'; fg=VGA_GREEN; } }
                break;
            case 8: /* Sunset — diagonal rays */
                if((x+y*2)%19==0){ c='/'; fg=VGA_RED; }
                break;
            case 9: /* Retro — checkerboard dots */
                if((x/4+y/2)%2==0&&x%4==0&&y%2==0){ c='.'; fg=VGA_BROWN; }
                break;
            }
            vga_putchar_at(c,x,y,fg,th->desk_bg);
        }
    }
}

/* ================================================================
   WINDOW — double-line border, accent title
   ================================================================ */
void ui_draw_window(int x,int y,int w,int h,const char *title){
    theme_t *th=ui_theme();

    /* Top border */
    vga_putchar_at('.',x,y,th->accent,th->win_bg);
    for(int i=1;i<w-1;i++) vga_putchar_at('-',x+i,y,th->accent,th->win_bg);
    vga_putchar_at('.',x+w-1,y,th->accent,th->win_bg);

    /* Title in center of top border */
    if(title&&title[0]){
        int tlen=(int)strlen(title);
        int tx=x+(w-tlen-2)/2; if(tx<x+1)tx=x+1;
        vga_putchar_at(' ',tx-1,y,th->title_fg,th->win_bg);
        vga_puts_at(title,tx,y,th->title_fg,th->win_bg);
        vga_putchar_at(' ',tx+tlen,y,th->title_fg,th->win_bg);
    }

    /* Sides + fill */
    for(int i=1;i<h-1;i++){
        vga_putchar_at('|',x,    y+i,th->accent,th->win_bg);
        for(int j=1;j<w-1;j++) vga_putchar_at(' ',x+j,y+i,th->win_fg,th->win_bg);
        vga_putchar_at('|',x+w-1,y+i,th->accent,th->win_bg);
    }

    /* Bottom border */
    vga_putchar_at('.',x,    y+h-1,th->accent,th->win_bg);
    for(int i=1;i<w-1;i++) vga_putchar_at('-',x+i,y+h-1,th->accent,th->win_bg);
    vga_putchar_at('.',x+w-1,y+h-1,th->accent,th->win_bg);
}

/* ================================================================
   MOUSE CURSOR
   ================================================================ */
static int last_mx=-1,last_my=-1;
static uint16_t saved_char=0;

void ui_draw_cursor(void){
    if(!mouse_is_ready())return;
    int mx,my,mb; mouse_get(&mx,&my,&mb);
    /* Keep cursor glyph in content area only */
    if(my<1) my=1;
    if(my>23) my=23;
    if(mx==last_mx&&my==last_my)return;
    /* Restore old cell from shadow (ground truth), then save new
       cell from shadow and draw cursor to both shadow+VGA directly
       so it's immediately visible without waiting for vga_flush(). */
    if(last_mx>=0&&last_my>=0)
        vga_shadow_write(last_mx,last_my,saved_char);
    saved_char=vga_shadow_read(mx,my);
    uint8_t bg=mb?VGA_RED:VGA_LIGHT_CYAN;
    vga_shadow_write(mx,my,(uint16_t)'.'|((uint16_t)((bg<<4)|VGA_WHITE)<<8));
    last_mx=mx; last_my=my;
}

void ui_erase_cursor(void){
    if(last_mx<0||last_my<0)return;
    vga_shadow_write(last_mx,last_my,saved_char);
    last_mx=-1; last_my=-1;
}

void ui_force_cursor_redraw(void){ last_mx=-1; last_my=-1; }
