/*
 * ui.c — HavenDOS v0.6.8
 * Modern TUI: taskbar, themes, corner clock, box-drawing windows
 */
#include "../include/theme.h"
#include "version.h"
#include "../include/vga.h"
extern int keyboard_waitchar(void);
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

/* Preview a theme in-memory only — no disk write; used by Settings live preview
 *
 * IT OVERWRITES cur_theme. So after previewing, ui_current_theme() is the
 * PREVIEWED theme, not the one that was active when the picker opened, and
 *
 *     ui_set_theme(ui_current_theme());     // "restore"
 *
 * does not restore anything -- it commits the preview to disk. That was the
 * Settings Themes tab for several releases: ESC and Tab both wrote whichever
 * theme the cursor happened to be resting on. A caller that previews must
 * save the entry value ITSELF, before the first preview, and pass it back to
 * this function (not to ui_set_theme) to cancel.
 *
 * There are three theme pickers in this tree -- shell/setup.c,
 * shell/desktop.c and shell/apps_extra.c -- and before v0.8.5.6 all three
 * handled this differently and two were wrong. If a fourth appears, unify
 * them instead.
 */
void ui_load_preview(int n){
    if(n>=0&&n<nthemes_active()) cur_theme=n;
}

/* ================================================================
   TASKBAR — slim, modern, HavenDOS branding
   ================================================================ */

/* ── App taskbar (top bar only) — used by all apps ────────────── */
void ui_draw_taskbar(const char *title){
    theme_t *th=ui_theme();
    const int W=vga_cols();
    for(int x=0;x<W;x++) vga_putchar_at(' ',x,0,th->bar_fg,th->bar_bg);
    /* Left: back/close hint */
    vga_puts_at(" ESC=Close",0,0,th->bar_fg,th->bar_bg);
    /* Center: app title */
    if(title&&title[0]){
        int tlen=(int)strlen(title);
        int tx=(W-tlen)/2; if(tx<12)tx=12;
        vga_puts_at(title,tx,0,th->accent,th->bar_bg);
    }
    /* Right: HH:MM */
    uint8_t h,m,s2; rtc_get_time(&h,&m,&s2);
    char tb[6];
    tb[0]='0'+h/10;tb[1]='0'+h%10;tb[2]=':';
    tb[3]='0'+m/10;tb[4]='0'+m%10;tb[5]=0;
    vga_puts_at(tb,W-6,0,th->accent,th->bar_bg);
}

/* ── Desktop two-bar layout ─────────────────────────────────────
   Row  0 : thin top status bar  (version . HavenDOS . clock)
   Row 24 : main taskbar (start button + title + sysinfo)
   ────────────────────────────────────────────────────────────── */
void ui_draw_desktop_bars(void){
    theme_t *th=ui_theme();
    /* v0.7.18: derived from the active mode instead of a hard-coded 80x25,
       so the bar sits on the real bottom row and the right-hand cluster is
       anchored to the real right edge. At 80x25 the arithmetic reproduces
       the previous fixed columns exactly. */
    const int W=vga_cols();
    const int BAR_Y=vga_rows()-1;

    /* Row 0 — top status bar */
    for(int x=0;x<W;x++) vga_putchar_at(' ',x,0,th->bar_fg,th->bar_bg);
    vga_puts_at("HavenDOS " HAVENDOS_VER,1,0,th->bar_fg,th->bar_bg);
    uint8_t h,m,s2; rtc_get_time(&h,&m,&s2);
    char tb[9];
    tb[0]='0'+h/10;tb[1]='0'+h%10;tb[2]=':';
    tb[3]='0'+m/10;tb[4]='0'+m%10;tb[5]=':';
    tb[6]='0'+s2/10;tb[7]='0'+s2%10;tb[8]=0;
    /* Row 0 time removed — desktop tick writes date | time to row 0 */

    /* Bottom taskbar */
    for(int x=0;x<W;x++) vga_putchar_at(' ',x,BAR_Y,th->bar_fg,th->bar_bg);
    vga_puts_at("  HAVENDOS    ",0,BAR_Y,th->bar_bg,th->accent);
    vga_putchar_at('.',14,BAR_Y,th->accent,th->bar_bg);
    const char *un=current_username();
    int ulen=(int)strlen(un);
    vga_puts_at(un,17,BAR_Y,th->accent,th->bar_bg);
    if(current_user_admin()) vga_puts_at(".admin",17+ulen,BAR_Y,th->bar_fg,th->bar_bg);
    /* Right-hand cluster: [!] NOTIF (n)  HH:MM:SS, anchored to the right edge.
       At W=80 these land on 55/59/65/71, exactly where they were. */
    const int CLOCK_X=W-9, NCNT_X=W-15, NTXT_X=W-21, BELL_X=W-25;
    extern int notif_get_unread(void);
    int nunu = notif_get_unread();
    vga_puts_at("[!]",BELL_X,BAR_Y,nunu>0?th->accent:th->bar_fg,th->bar_bg);
    if(nunu > 0){
        /* v0.7.5 FIX: was char nc[4] with nc[4]=0 on the >9 path — a
           one-byte stack overflow that clobbered the adjacent local. */
        char nc[5]; nc[0]='('; nc[1]='0'+nunu; nc[2]=')'; nc[3]=0; nc[4]=0;
        if(nunu>9){ nc[1]='9'; nc[2]='+'; nc[3]=')'; nc[4]=0; }
        vga_puts_at("NOTIF",NTXT_X,BAR_Y,th->accent,th->bar_bg);
        vga_puts_at(nc,NCNT_X,BAR_Y,th->accent,th->bar_bg);
    } else {
        vga_puts_at("NOTIF",NTXT_X,BAR_Y,th->bar_fg,th->bar_bg);
        vga_puts_at("   ",NCNT_X,BAR_Y,th->bar_fg,th->bar_bg); /* clear old count */
    }
    vga_puts_at(tb,CLOCK_X,BAR_Y,th->accent,th->bar_bg);
}

/* The notification cluster's hit box, from the SAME arithmetic that
   paints it above. It is a function and not a pair of constants
   because the two copies had already drifted: desktop.c tested
   columns 55..63 while this paints 55..66 at W=80, so clicking the
   unread count did nothing even on the console both were written for.
   v0.8.8.7. */
int ui_taskbar_notif_hit(int x, int y)
{
    const int W = vga_cols();
    if(y != vga_rows() - 1) return 0;
    return x >= W - 25 && x <= W - 13;
}

/* ── Corner clock — updates top status bar clock only ────────── */
   /* NOTE: nothing calls this. The desktop repaints in full every
      second (see the v0.7.6 note in desktop.c) precisely because
      partial updates left stale cells. The column is derived anyway
      rather than left at a literal 71, so that a future caller does
      not reintroduce the bug this release is fixing. */
void ui_draw_corner_clock(void){
    uint8_t h,m,s2; rtc_get_time(&h,&m,&s2);
    char tb[9];
    tb[0]='0'+h/10;tb[1]='0'+h%10;tb[2]=':';
    tb[3]='0'+m/10;tb[4]='0'+m%10;tb[5]=':';
    tb[6]='0'+s2/10;tb[7]='0'+s2%10;tb[8]=0;
    theme_t *th=ui_theme();
    vga_puts_at(tb,vga_cols()-9,0,th->accent,th->bar_bg);
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

/* ── a pager, for output longer than the screen ──────────────────────
 *
 * v0.8.5. Added because `lspci -vv` on the test laptop prints 17
 * devices with their BARs -- about sixty lines -- and scrolled the top
 * of it off a screen that could not be scrolled back. The only way to
 * read it was to photograph the display mid-scroll, which is not a
 * diagnostic tool.
 *
 * The page height comes from vga_rows() rather than a constant. The
 * pager in shell/help.c is hardcoded to 20 lines, which is already
 * wrong on the 90x60 modes this kernel has had since v0.7.12 and will
 * be wrong on every grid 0.8.8 adds. Deriving it is the same rule the
 * desktop chrome has followed since v0.7.18.
 *
 * SPACE continues and Q stops. Both are named on the prompt, because a
 * pager that says "press any key" eats the first character of whatever
 * the reader types next -- which is how scripted runs of this shell
 * have lost commands before.
 */
static int g_page_lines;

void ui_page_reset(void){ g_page_lines = 0; }

int ui_page_line(void)
{
    int rows = vga_rows();
    if (rows < 6) rows = 25;              /* a grid too small to page */
    if (++g_page_lines < rows - 2) return 1;

    g_page_lines = 0;
    vga_set_color(VGA_BLACK, VGA_DARK_GREY);
    vga_puts(" -- more -- SPACE continues, Q stops -- ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    {
        int c = keyboard_waitchar();
        vga_puts("\r                                         \r");
        if (c == 'q' || c == 'Q' || c == 27) return 0;
    }
    return 1;
}
