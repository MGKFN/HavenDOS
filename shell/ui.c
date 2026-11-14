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
   THEMES -- redone for v0.9.5

   The set this replaces was ten palettes from 0.6.x that differed in
   the colour of a bar and nothing else: every one of the twenty had a
   BLACK desktop, so switching theme changed the taskbar and left the
   screen looking the same. Kyan's photo of `theme` made the point --
   "themes old, and are never changed since we added them".

   Now each theme is a whole look: a desktop colour, a wallpaper drawn
   from one of nine shapes, and text colours picked to be readable on
   that desktop (desk_fg, a new field -- the desktop used to draw its
   text in bar_fg, which only worked because every desktop was black).

   Backgrounds stay in colours 0-7. In VGA text mode colours 8-15 as a
   background mean "blink" unless the attribute controller is told
   otherwise, and the firmware's text mode is not ours to reprogram on
   every machine; the framebuffer console does not care either way.

   The first twelve are on every install; the last eight come with the
   themes-extra package. The theme is saved by NAME now (".theme"), so
   reordering this table can never silently swap someone's theme again;
   a file from an older release holds a number and goes through
   legacy_map[] once.
   ================================================================ */
#define C(x) VGA_##x
static theme_t themes[]={
 /* bar_bg bar_fg  desk_bg  win_bg win_fg  title_bg title_fg  accent  icon_bg icon_fg
      name            desk_fg   wallpaper                         */
 {C(BLACK),C(WHITE),       C(BLUE),   C(BLACK),C(LIGHT_GREY), C(CYAN),   C(LIGHT_CYAN),  C(LIGHT_CYAN),   C(LIGHT_CYAN),   C(WHITE),
   "Haven",          C(WHITE),      UI_PAT_GRID,   '.', C(LIGHT_BLUE)},
 {C(BLACK),C(LIGHT_CYAN),  C(BLACK),  C(BLACK),C(LIGHT_GREY), C(BLACK),  C(LIGHT_CYAN),  C(CYAN),         C(CYAN),         C(LIGHT_CYAN),
   "Midnight",       C(LIGHT_GREY), UI_PAT_GRID,   '.', C(DARK_GREY)},
 {C(BLACK),C(LIGHT_GREEN), C(BLACK),  C(BLACK),C(LIGHT_GREEN),C(BLACK),  C(LIGHT_GREEN), C(GREEN),        C(GREEN),        C(LIGHT_GREEN),
   "Terminal",       C(LIGHT_GREEN),UI_PAT_RAIN,   '.', C(GREEN)},
 {C(BLACK),C(YELLOW),      C(BLACK),  C(BLACK),C(YELLOW),     C(BLACK),  C(YELLOW),      C(YELLOW),       C(BROWN),        C(YELLOW),
   "Amber CRT",      C(YELLOW),     UI_PAT_HATCH,  '.', C(BROWN)},
 {C(BLUE), C(WHITE),       C(CYAN),   C(BLACK),C(LIGHT_GREY), C(BLUE),   C(WHITE),       C(LIGHT_CYAN),   C(BLUE),         C(WHITE),
   "Ocean",          C(BLACK),      UI_PAT_WAVE,   '~', C(LIGHT_CYAN)},
 {C(BLACK),C(LIGHT_GREEN), C(GREEN),  C(BLACK),C(LIGHT_GREY), C(GREEN),  C(LIGHT_GREEN), C(YELLOW),       C(YELLOW),       C(WHITE),
   "Forest",         C(WHITE),      UI_PAT_TREES,  '^', C(LIGHT_GREEN)},
 {C(BLACK),C(YELLOW),      C(RED),    C(BLACK),C(LIGHT_GREY), C(RED),    C(YELLOW),      C(YELLOW),       C(YELLOW),       C(WHITE),
   "Sunset",         C(WHITE),      UI_PAT_BANDS,  '-', C(LIGHT_RED)},
 {C(BLACK),C(LIGHT_MAGENTA),C(MAGENTA),C(BLACK),C(LIGHT_GREY),C(MAGENTA), C(WHITE),       C(LIGHT_MAGENTA),C(LIGHT_MAGENTA),C(WHITE),
   "Grape",          C(WHITE),      UI_PAT_SCATTER,'+', C(LIGHT_MAGENTA)},
 {C(BLACK),C(WHITE),       C(BLACK),  C(BLACK),C(LIGHT_GREY), C(BLACK),  C(LIGHT_BLUE),  C(LIGHT_BLUE),   C(LIGHT_BLUE),   C(WHITE),
   "Slate",          C(LIGHT_GREY), UI_PAT_COLUMNS,'|', C(DARK_GREY)},
 {C(BLACK),C(WHITE),       C(LIGHT_GREY),C(BLACK),C(LIGHT_GREY),C(BLACK), C(LIGHT_RED),   C(RED),          C(BLUE),         C(BLACK),
   "Paper",          C(BLACK),      UI_PAT_GRID,   '.', C(DARK_GREY)},
 {C(BLACK),C(YELLOW),      C(BROWN),  C(BLACK),C(LIGHT_GREY), C(BROWN),  C(YELLOW),      C(YELLOW),       C(YELLOW),       C(WHITE),
   "Retro",          C(WHITE),      UI_PAT_CHECK,  '.', C(YELLOW)},
 {C(BLACK),C(WHITE),       C(BLACK),  C(BLACK),C(WHITE),      C(BLACK),  C(YELLOW),      C(YELLOW),       C(WHITE),        C(WHITE),
   "High Contrast",  C(WHITE),      UI_PAT_NONE,   ' ', C(BLACK)},
 /* ── themes-extra ── */
 {C(BLACK),C(LIGHT_MAGENTA),C(BLACK), C(BLACK),C(LIGHT_CYAN), C(BLACK),  C(LIGHT_MAGENTA),C(LIGHT_CYAN),  C(MAGENTA),      C(LIGHT_CYAN),
   "Neon",           C(LIGHT_CYAN), UI_PAT_COLUMNS,'|', C(MAGENTA)},
 {C(BLUE), C(WHITE),       C(BLUE),   C(BLACK),C(WHITE),      C(BLUE),   C(WHITE),       C(WHITE),        C(LIGHT_CYAN),   C(WHITE),
   "Arctic",         C(WHITE),      UI_PAT_SCATTER,'*', C(WHITE)},
 {C(BLACK),C(LIGHT_RED),   C(BLACK),  C(BLACK),C(LIGHT_GREY), C(RED),    C(WHITE),       C(LIGHT_RED),    C(RED),          C(YELLOW),
   "Volcano",        C(LIGHT_RED),  UI_PAT_TREES,  '^', C(RED)},
 {C(BLACK),C(YELLOW),      C(BROWN),  C(BLACK),C(YELLOW),     C(BROWN),  C(YELLOW),      C(YELLOW),       C(YELLOW),       C(WHITE),
   "Desert",         C(YELLOW),     UI_PAT_WAVE,   '~', C(YELLOW)},
 {C(BLACK),C(LIGHT_CYAN),  C(CYAN),   C(BLACK),C(LIGHT_GREY), C(CYAN),   C(WHITE),       C(LIGHT_CYAN),   C(BLUE),         C(WHITE),
   "Lagoon",         C(BLACK),      UI_PAT_SCATTER,'o', C(WHITE)},
 {C(BLACK),C(LIGHT_GREY),  C(BLACK),  C(BLACK),C(LIGHT_GREY), C(BLACK),  C(WHITE),       C(WHITE),        C(DARK_GREY),    C(LIGHT_GREY),
   "Mono",           C(LIGHT_GREY), UI_PAT_HATCH,  '.', C(DARK_GREY)},
 {C(BLACK),C(YELLOW),      C(BLUE),   C(BLACK),C(LIGHT_GREY), C(BLUE),   C(YELLOW),      C(YELLOW),       C(YELLOW),       C(WHITE),
   "Royal",          C(YELLOW),     UI_PAT_CHECK,  '+', C(YELLOW)},
 {C(BLACK),C(LIGHT_RED),   C(MAGENTA),C(BLACK),C(LIGHT_GREY), C(MAGENTA),C(WHITE),       C(LIGHT_RED),    C(LIGHT_RED),    C(WHITE),
   "Rose",           C(WHITE),      UI_PAT_GRID,   '.', C(LIGHT_RED)},
};
#undef C
#define NTHEMES_BASE  12
#define NTHEMES_ALL   ((int)(sizeof(themes)/sizeof(themes[0])))
static int cur_theme    = 0;
static int g_extra_themes = 0;   /* set to 1 when themes-extra is installed */
static int g_pattern_on   = 1;

/* Old index (0.6 to 0.9.4) to the new theme most like it. */
static const uint8_t legacy_map[20] = {
    1,  /* Midnight Cyan  -> Midnight      */
    0,  /* Classic Blue   -> Haven         */
    2,  /* Terminal Green -> Terminal      */
    3,  /* Amber          -> Amber CRT     */
    8,  /* Dark           -> Slate         */
    13, /* Arctic         -> Arctic        */
    12, /* Neon           -> Neon          */
    5,  /* Mint           -> Forest        */
    6,  /* Sunset         -> Sunset        */
    10, /* Retro          -> Retro         */
    14, /* Blood          -> Volcano       */
    17, /* Monochrome     -> Mono          */
    4,  /* Ocean          -> Ocean         */
    7,  /* Purple Haze    -> Grape         */
    11, /* Ghost          -> High Contrast */
    15, /* Desert         -> Desert        */
    18, /* Sapphire       -> Royal         */
    2,  /* Matrix Gold    -> Terminal      */
    14, /* Volcano        -> Volcano       */
    13, /* Ice            -> Arctic        */
};

extern int vfs_write(const char*, const char*, uint32_t);
extern int vfs_read(const char*, char*, uint32_t);

#define THEME_FILE ".theme"

static int nthemes_active(void){ return g_extra_themes ? NTHEMES_ALL : NTHEMES_BASE; }

theme_t    *ui_theme(void)          { return &themes[cur_theme]; }
const theme_t *ui_theme_get(int n)  { return (n>=0&&n<NTHEMES_ALL)?&themes[n]:&themes[0]; }
int         ui_theme_base_count(void){ return NTHEMES_BASE; }
int         ui_theme_total(void)     { return NTHEMES_ALL; }
void        ui_set_theme(int n)     {
    int max = nthemes_active();
    if(n>=0&&n<max){
        cur_theme=n;
        const char *nm=themes[n].name;
        char buf[32]; int k=0;
        while(nm[k]&&k<30){ buf[k]=nm[k]; k++; }
        buf[k++]='\n';
        vfs_write(THEME_FILE,buf,(uint32_t)k);
    }
}
int         ui_theme_count(void)    { return nthemes_active(); }
int         ui_current_theme(void)  { return cur_theme; }
const char *ui_theme_name(int n)    { int max=nthemes_active(); return (n>=0&&n<max)?themes[n].name:"?"; }

/* ── The wallpaper ─────────────────────────────────────────────── */
static int pat_cell(const theme_t *t, int x, int y, char *c, vga_color_t *fg){
    int hit=0;
    char ch=t->pat_ch;
    switch(t->pat_kind){
    case UI_PAT_GRID:    hit = (x%8==0 && y%4==0); break;
    case UI_PAT_RAIN:
        if((x*3+y*7)%17==0) hit=1;
        else if((x*7+y*2)%31==0){ *c=(char)('0'+((x+y)%10)); *fg=VGA_DARK_GREY; return 1; }
        break;
    case UI_PAT_HATCH:   hit = ((x+y)%12==0 || (x-y+240)%12==0); break;
    case UI_PAT_WAVE:    hit = ((y*4 + ((x/5)%2)*2) % 10 == 0 && x%5 != 4); break;
    case UI_PAT_TREES:   hit = ((x*7+y*13)%37==0); break;
    case UI_PAT_BANDS:   hit = (y%6==0 && (x/3)%2==0); break;
    case UI_PAT_SCATTER: hit = ((x*5+y*11)%53==0); break;
    case UI_PAT_COLUMNS:
        if(x%16==0){ if(y%5==0){ *c='+'; *fg=t->accent; return 1; } hit=1; }
        break;
    case UI_PAT_CHECK:   hit = ((x/4+y/2)%2==0 && x%4==0 && y%2==0); break;
    default: return 0;
    }
    if(!hit) return 0;
    *c=ch; *fg=t->pat_fg;
    return 1;
}
int ui_pattern_at_theme(int n, int x, int y, char *c, vga_color_t *fg){
    return pat_cell(ui_theme_get(n), x, y, c, fg);
}
int ui_pattern_at(int x, int y, char *c, vga_color_t *fg){
    if(!g_pattern_on) return 0;
    return pat_cell(&themes[cur_theme], x, y, c, fg);
}
int  ui_desk_pattern(void){ return g_pattern_on; }
void ui_set_desk_pattern(int on){
    extern void cfg_set_desk_pattern(int); extern void cfg_save(void);
    g_pattern_on = on ? 1 : 0;
    cfg_set_desk_pattern(g_pattern_on); cfg_save();
}
void ui_toggle_desk_pattern(void){ ui_set_desk_pattern(!g_pattern_on); }

void ui_unlock_extra_themes(void)  { g_extra_themes=1; }
int  ui_extra_themes_unlocked(void){ return g_extra_themes; }

void ui_load_theme(void){
    extern int bpkg_is_installed(const char*);
    extern int cfg_desk_pattern(void);
    if(bpkg_is_installed("themes-extra")) g_extra_themes=1;
    g_pattern_on = cfg_desk_pattern();
    char buf[40];
    int r=vfs_read(THEME_FILE, buf, sizeof(buf)-1);
    if(r<=0) return;
    buf[r]=0;
    while(r>0&&(buf[r-1]=='\n'||buf[r-1]=='\r'||buf[r-1]==' ')) buf[--r]=0;
    int max=nthemes_active();
    if(buf[0]>='0'&&buf[0]<='9'){
        /* A number: written by 0.9.4 or older, indexing the old table. */
        int n=buf[0]-'0';
        if(buf[1]>='0'&&buf[1]<='9') n=n*10+(buf[1]-'0');
        int m=(n>=0&&n<20)?legacy_map[n]:0;
        if(m>=max) m=(n==5)?0:1;   /* an extra the package no longer unlocks */
        ui_set_theme(m);           /* rewrite it by name, once */
        return;
    }
    for(int i=0;i<max;i++)
        if(strcmp(themes[i].name,buf)==0){ cur_theme=i; return; }
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
   DESKTOP BACKGROUND -- the theme's colour and wallpaper
   ================================================================ */
void ui_draw_desktop(void){
    theme_t *th=ui_theme();
    int W=vga_cols(), H=vga_rows();
    for(int y=1;y<H-1;y++){
        for(int x=0;x<W;x++){
            char c=' ';
            vga_color_t fg=th->desk_bg;
            ui_pattern_at(x,y,&c,&fg);
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
    /* Keep cursor glyph in content area only: below the menu bar on
       row 0 and above the taskbar on the bottom row.
       v0.8.9.6: the bottom was `23` written out -- row 23 of an 80x25
       screen. On a high-resolution grid of 50 or 67 rows that is
       halfway down the glass, and the pointer stopped there however
       far the mouse moved. Reported on the HP as a cursor that would
       not go past the middle of the screen. */
    {
        int bottom = vga_rows() - 2;
        if(bottom < 1) bottom = 1;
        if(my<1) my=1;
        if(my>bottom) my=bottom;
    }
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

/* ── v0.8.9.6: AN IDLE WAIT THAT KEEPS LISTENING TO THE MOUSE ─────────
   The desktop's idle was `sleep_ms(20)`, and a USB or I2C pointer is
   only read when somebody asks where it is -- so it was read once per
   pass and its reports queued behind the sleep. A boot-protocol report
   holds at most 127 counts per axis, so a fast movement that piled up
   for a whole slow pass was clipped, and the pointer lagged the hand.

   This waits the same total time in 4 ms slices, pumping the pointer
   in each, and returns early the moment something happened -- the
   pointer moved, a button changed, a key arrived -- so the caller
   redraws now rather than at the end of the wait. The same 50 passes
   a second when nothing is happening; far fewer missed reports when
   something is. */
extern int  keyboard_haschar(void);
extern void sleep_ms(uint32_t);
void ui_idle(uint32_t ms)
{
    int x0, y0, b0, x, y, b;
    uint32_t done = 0;
    if(!mouse_is_ready()){ sleep_ms(ms); return; }
    mouse_get(&x0, &y0, &b0);
    while(done < ms){
        uint32_t step = (ms - done) < 4u ? (ms - done) : 4u;
        sleep_ms(step);
        done += step;
        mouse_get(&x, &y, &b);      /* pumps USB and I2C */
        if(x != x0 || y != y0 || b != b0) return;
        if(keyboard_haschar()) return;
    }
}

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
