#include "../include/io.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/theme.h"
#include "../include/rtc.h"
#include "../include/toast.h"

extern int         keyboard_getchar(void);
extern int         keyboard_waitchar(void);
extern int         keyboard_haschar(void);
extern void        mouse_get(int*,int*,int*);
extern int         mouse_is_ready(void);
extern int         mouse_get_clicks(void);
extern void        ui_draw_taskbar(const char*);
extern void        ui_draw_desktop(void);
extern void        ui_draw_window(int,int,int,int,const char*);
extern void        ui_draw_cursor(void);
extern void        ui_erase_cursor(void);
extern void        ui_force_cursor_redraw(void);
extern void        shell_run(void);
extern void        sleep_ms(uint32_t);
extern uint32_t    get_ticks(void);
extern uint32_t    pmm_free_blocks(void);
extern uint32_t    pmm_total_blocks(void);
extern int         vfs_write(const char*,const char*,uint32_t);
extern int         vfs_read(const char*,char*,uint32_t);
extern void        vfs_list(void(*)(const char*,int,uint32_t));
extern int         vfs_delete(const char*);
extern int         vfs_exists(const char*);
extern int         vfs_rename(const char*,const char*);
extern int         vfs_using_disk(void);
extern int         vfs_using_fat16(void);
extern const char *current_username(void);
extern int         current_user_admin(void);
extern int         get_user_count(void);
extern const char *get_username(int);
extern int         get_user_admin(int);
extern int         add_user(const char*, const char*, int);
extern void        login_screen(void);
extern void        busy_wait(volatile uint32_t);
extern void        app_tetris(void);
extern void        screensaver_run_matrix(void);
extern void        screensaver_run_stars(void);
extern void        screensaver_reset(void);
extern void        screensaver_check(void);
extern void        music_startup(void);
extern void        speaker_beep(uint32_t, uint32_t);
/* Reminder system */
void               reminders_save(void);
void               reminders_load(void);
void               reminders_check(void);
/* Notification center */
void               notif_push(const char*);

/* ---- App declarations ---- */
static void app_shell(void);
static void app_calculator(void);
static void app_texteditor(void);
static void app_filemanager(void);
static void app_sysinfo(void);
static void app_sysmonitor(void);
static void app_clock(void);
static void app_todo(void);
static void app_settings(void);
static void app_snake(void);
static void app_tetris_wrap(void);
static void app_screensaver(void);
static void app_power(void);
static void notif_center(void);
static void app_boot_ide(void);
static void app_install(void);
static void app_bpkg(void);

/* ================================================================
   APPS & SUBMENU SYSTEM
   ================================================================ */
typedef struct { const char *name; const char *icon; void(*fn)(void); } app_t;

/* Submenu entry: name + action */
typedef struct { const char *name; void(*fn)(void); } menu_item_t;

/* Top-level menu entries can either launch an app directly or open a submenu */
typedef struct {
    const char *name;
    const char *icon;
    void(*fn)(void);          /* NULL if has submenu */
    menu_item_t *sub;         /* NULL if direct app  */
    int nsub;
} top_item_t;

/* ---- Forward declarations of all app functions ---- */
static void app_shell(void);
static void app_texteditor(void);
static void app_calculator(void);
static void app_todo(void);
static void app_filemanager(void);
static void app_sysinfo(void);
static void app_sysmonitor(void);
static void app_clock(void);
static void app_snake(void);
static void app_tetris_wrap(void);
static void app_screensaver(void);
static void app_settings(void);
static void app_power(void);
static void app_pong(void);
static void app_hexviewer(void);
static void app_logviewer(void);
static void app_diskinfo(void);
static void app_netinfo(void);
static void app_usermanager(void);
static void notif_center(void);
static void app_bpkg(void);
static void app_dashboard(void);
static void app_pipes(void);
static void app_matrix(void);

/* ---- Submenus ---- */
static menu_item_t sub_apps[]={
    {"Shell",           app_shell},
    {"Text Editor",     app_texteditor},
    {"Calculator",      app_calculator},
    {"Todo List",       app_todo},
    {"HavenCode",        app_boot_ide},
    {"Hex Viewer",      app_hexviewer},
};
static menu_item_t sub_system[]={
    {"File Manager",    app_filemanager},
    {"System Info",     app_sysinfo},
    {"Dashboard",       app_dashboard},
    {"System Monitor",  app_sysmonitor},
    {"Clock",           app_clock},
    {"Disk Info",       app_diskinfo},
    {"Network Info",    app_netinfo},
    {"User Manager",    app_usermanager},
    {"Log Viewer",      app_logviewer},
};
static menu_item_t sub_fun[]={
    {"Snake",           app_snake},
    {"Tetris",          app_tetris_wrap},
    {"Pong",            app_pong},
    {"Matrix",          app_matrix},
    {"Pipes",           app_pipes},
    {"Screensaver",     app_screensaver},
};
static menu_item_t sub_settings[]={
    {"Settings",        app_settings},
    {"Power / Reboot",  app_power},
};

#define NSUB_APPS     6
#define NSUB_SYSTEM   9
#define NSUB_FUN      6
#define NSUB_SETTINGS 2

/* ---- Top-level menu ---- */
static top_item_t topmenu[]={
    {" Apps",      ">_", NULL, sub_apps,     NSUB_APPS},
    {" System",    "Sy", NULL, sub_system,   NSUB_SYSTEM},
    {" Fun",       ":-", NULL, sub_fun,      NSUB_FUN},
    {" Settings",  "St", NULL, sub_settings, NSUB_SETTINGS},
};
#define NTOP 4

/* ---- Icon grid (same apps, for desktop) ---- */
static app_t apps[]={
    {"Shell",        ">_", app_shell},
    {"Text Editor",  "Ed", app_texteditor},
    {"Calculator",   "c=", app_calculator},
    {"Reminders",    "Rm", app_todo},
    {"HavenCode",     "Hc", app_boot_ide},
    {"File Manager", "FM", app_filemanager},
    {"System Info",  "Si", app_sysinfo},
    {"Sys Monitor",  "Mn", app_sysmonitor},
    {"Clock",        "Cl", app_clock},
    {"Snake",        "Sn", app_snake},
    {"Tetris",       "Te", app_tetris_wrap},
    {"Screensaver",  "~~", app_screensaver},
    {"Settings",     "St", app_settings},
    {"Power",        "Pw", app_power},
    {"Pong",         "||", app_pong},
    {"Install",      "In", app_install},
    {"bpkg",         "Pk", app_bpkg},
};
#define NAPP 17

/* ================================================================
   DESKTOP DRAW
   ================================================================ */

/* Icons: 2-column grid with scroll support.
 * scroll_row = first app-row to display (0-based).
 * Each app row is TILE_STEP=4 screen rows tall.
 * Visible content: rows 2..20 = 19 screen rows → 4 app-rows visible. */
#define ICON_TILE_STEP  4   /* screen rows per app row (icon+name+dot+gap) */
#define ICON_ROWS_VIS   4   /* how many app-rows fit on screen             */

static int icon_scroll_row = 0; /* file-scope so hit-test can read it */

static void draw_icons(int sel, int menu_open){
    theme_t *th=ui_theme();

    /* Compute scroll offset so selected icon is always visible */
    int sel_approw = sel / 2;
    if(sel_approw < icon_scroll_row) icon_scroll_row = sel_approw;
    if(sel_approw >= icon_scroll_row + ICON_ROWS_VIS) icon_scroll_row = sel_approw - ICON_ROWS_VIS + 1;

    /* ── Clear the entire icon area first (rows 2..20, cols 0..33) ── */
    for(int sy=2; sy<=20; sy++)
        for(int sx=0; sx<35; sx++)
            vga_putchar_at(' ', sx, sy, th->desk_bg, th->desk_bg);

    /* ── Draw visible app rows ──────────────────────────────────── */
    for(int i=0; i<NAPP; i++){
        int approw = i / 2;
        int col    = i % 2;

        /* Skip rows not in view */
        if(approw < icon_scroll_row) continue;
        int vis_row = approw - icon_scroll_row;
        if(vis_row >= ICON_ROWS_VIS) break;

        int x = 1 + col * 17;
        int y = 2 + vis_row * ICON_TILE_STEP;

        int s=(i==sel&&!menu_open);
        vga_color_t ibg=s?th->accent:th->icon_bg;
        vga_color_t ifg=s?th->icon_bg:th->icon_fg;
        vga_color_t nfg=s?th->accent:th->bar_fg;

        /* Row 0: dash-border with icon letters */
        vga_puts_at("---------------",x,y,th->bar_fg,th->desk_bg);
        vga_putchar_at('-',x,y,th->accent,th->desk_bg);
        vga_putchar_at(apps[i].icon[0],x+1,y,ifg,ibg);
        vga_putchar_at(apps[i].icon[1],x+2,y,ifg,ibg);
        vga_putchar_at('-',x+3,y,th->accent,th->desk_bg);

        /* Row 1: app name (up to 14 chars) */
        char nm[15]; int nl=(int)strlen(apps[i].name);
        for(int j=0;j<14;j++) nm[j]=(j<nl)?apps[i].name[j]:' ';
        nm[14]=0;
        vga_puts_at(nm,x,y+1,nfg,th->desk_bg);

        /* Row 2: subtle dot separator */
        vga_putchar_at('.',x+6,y+2,th->icon_bg,th->desk_bg);
    }

    /* ── Scroll indicator (right gutter) if list is longer than screen ── */
    int total_rows = (NAPP+1)/2;
    if(total_rows > ICON_ROWS_VIS){
        vga_putchar_at(icon_scroll_row>0?'^':' ', 34, 2, th->bar_fg, th->desk_bg);
        vga_putchar_at((icon_scroll_row+ICON_ROWS_VIS)<total_rows?'v':' ',
                       34, 2+ICON_ROWS_VIS*ICON_TILE_STEP-1, th->bar_fg, th->desk_bg);
    }
}

/* Info strip along the bottom of the content area */
static void draw_infopanel(void){
    theme_t *th=ui_theme();
    uint32_t f=pmm_free_blocks(),t=pmm_total_blocks();
    (void)t;
    char rb[8]; utoa((f*4)/1024,rb,10); strcat(rb,"M");
    uint32_t up=get_ticks()/100;
    char us[12];
    us[0]='0'+(up/3600/10)%10; us[1]='0'+(up/3600)%10; us[2]=':';
    us[3]='0'+((up%3600)/60/10)%10; us[4]='0'+((up%3600)/60)%10; us[5]=':';
    us[6]='0'+((up%60)/10)%10; us[7]='0'+(up%60)%10; us[8]=0;
    /* Separator line at row 21 */
    for(int x=0;x<80;x++) vga_putchar_at('-',x,21,th->icon_bg,th->desk_bg);
    /* Info row at row 22 */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,22,th->desk_bg,th->desk_bg);
    vga_puts_at("RAM:",1,22,th->bar_fg,th->desk_bg);
    vga_puts_at(rb,5,22,VGA_LIGHT_GREEN,th->desk_bg);
    vga_puts_at("Up:",11,22,th->bar_fg,th->desk_bg);
    vga_puts_at(us,14,22,VGA_LIGHT_GREY,th->desk_bg);
    /* Right side hint */
    vga_puts_at("Tap icon to open  Right-tap for menu",44,22,th->icon_bg,th->desk_bg);
}

/* ================================================================
   SUBMENU SYSTEM
   menu_state: 0=closed, 1=top level open, 2=submenu open
   top_sel: which top item highlighted
   sub_sel: which submenu item highlighted
   ================================================================ */
static int menu_state=0; /* 0=closed 1=top 2=sub 3=context(right-click) */
static int top_sel=0, sub_sel=0;
static int ctx_x=0, ctx_y=0, ctx_sel=0;

/* ================================================================
   START MENU — rises from bottom-left above the taskbar
   Layout (rows 13-23, anchored to row 24 taskbar):
     Row 13        : top border
     Row 14        : username
     Row 15        : "Logged in"
     Row 16        : separator
     Rows 17-20    : NTOP category items
     Row 21        : separator
     Row 22        : Lock
     Row 23        : bottom border
   Submenu flies out to the right (cols MENU_W..MENU_W+SUB_W)
   ================================================================ */
#define MENU_W      22    /* left panel width                  */
#define MENU_TOP    13    /* top border row                    */
#define MENU_BOT    23    /* bottom border row                 */
#define MENU_ITEM_Y 17    /* first category item row           */
#define MENU_LOCK_Y 22    /* lock item row                     */
#define SUB_W       26    /* submenu flyout width              */
#define SUB_X       MENU_W

static void draw_topmenu(void){
    theme_t *th=ui_theme();
    const char *uname=current_username();

    /* Left panel background */
    for(int y=MENU_TOP;y<=MENU_BOT;y++)
        for(int x=0;x<MENU_W;x++)
            vga_putchar_at(' ',x,y,th->win_fg,th->win_bg);

    /* Top border */
    vga_putchar_at('.',0,MENU_TOP,th->accent,th->win_bg);
    for(int x=1;x<MENU_W-1;x++) vga_putchar_at('-',x,MENU_TOP,th->accent,th->win_bg);
    vga_putchar_at('.',MENU_W-1,MENU_TOP,th->accent,th->win_bg);

    /* User header */
    vga_putchar_at(' ',0,14,th->win_fg,th->win_bg);
    vga_puts_at(uname,1,14,th->accent,th->win_bg);
    vga_putchar_at(' ',0,15,th->win_fg,th->win_bg);
    vga_puts_at("Logged in",1,15,th->bar_fg,th->win_bg);
    if(current_user_admin()){
        vga_puts_at(".admin",1+(int)strlen(uname),14,th->bar_fg,th->win_bg);
    }

    /* Separator after header */
    for(int x=0;x<MENU_W;x++) vga_putchar_at('-',x,16,th->icon_bg,th->win_bg);

    /* Category items */
    for(int i=0;i<NTOP;i++){
        int iy=MENU_ITEM_Y+i;
        int s=(i==top_sel);
        vga_color_t bg=s?th->accent:th->win_bg;
        vga_color_t fg=s?th->win_bg:th->win_fg;
        vga_putchar_at(s?'>':' ',0,iy,fg,bg);
        char row[22]; int j=0;
        const char *n=topmenu[i].name;
        while(*n&&j<MENU_W-4) row[j++]=*n++;
        while(j<MENU_W-4) row[j++]=' ';
        row[j]=0;
        vga_puts_at(row,1,iy,fg,bg);
        /* Flyout arrow */
        vga_putchar_at('>',MENU_W-2,iy,s?th->win_bg:th->icon_bg,bg);
        vga_putchar_at(' ',MENU_W-1,iy,fg,bg);
    }

    /* Separator before lock */
    for(int x=0;x<MENU_W;x++) vga_putchar_at('-',x,21,th->icon_bg,th->win_bg);

    /* Lock item */
    int ls=(menu_state==4); /* state 4 = lock highlighted */
    vga_color_t lbg=ls?th->accent:th->win_bg;
    vga_color_t lfg=ls?th->win_bg:th->win_fg;
    vga_putchar_at(ls?'>':' ',0,MENU_LOCK_Y,lfg,lbg);
    vga_puts_at("Lock                ",1,MENU_LOCK_Y,lfg,lbg);

    /* Bottom border */
    vga_putchar_at('.',0,MENU_BOT,th->accent,th->win_bg);
    for(int x=1;x<MENU_W-1;x++) vga_putchar_at('-',x,MENU_BOT,th->accent,th->win_bg);
    vga_putchar_at('.',MENU_W-1,MENU_BOT,th->accent,th->win_bg);
}

static int sub_scroll = 0; /* scroll offset for submenu items */

static void draw_submenu(void){
    theme_t *th=ui_theme();
    menu_item_t *sub=topmenu[top_sel].sub;
    int nsub=topmenu[top_sel].nsub;
    int vis = MENU_BOT - MENU_ITEM_Y - 1; /* visible rows available */

    /* Keep sub_scroll so sub_sel is always visible */
    if(sub_sel < sub_scroll) sub_scroll = sub_sel;
    if(sub_sel >= sub_scroll + vis) sub_scroll = sub_sel - vis + 1;
    if(sub_scroll < 0) sub_scroll = 0;

    /* Submenu occupies same rows as left panel, to the right */
    for(int y=MENU_TOP;y<=MENU_BOT;y++)
        for(int x=SUB_X;x<SUB_X+SUB_W;x++)
            vga_putchar_at(' ',x,y,th->win_fg,th->win_bg);

    /* Top border */
    vga_putchar_at('.',SUB_X,MENU_TOP,th->accent,th->win_bg);
    for(int x=SUB_X+1;x<SUB_X+SUB_W-1;x++) vga_putchar_at('-',x,MENU_TOP,th->accent,th->win_bg);
    vga_putchar_at('.',SUB_X+SUB_W-1,MENU_TOP,th->accent,th->win_bg);

    /* Category label */
    vga_puts_at(topmenu[top_sel].name,SUB_X+1,14,th->accent,th->win_bg);
    for(int x=SUB_X;x<SUB_X+SUB_W;x++) vga_putchar_at('-',x,16,th->icon_bg,th->win_bg);

    /* Items — rendered with scroll offset */
    for(int i=0;i<vis;i++){
        int idx = i + sub_scroll;
        if(idx >= nsub) break;
        int iy=MENU_ITEM_Y+i;
        int s=(idx==sub_sel);
        vga_color_t bg=s?th->accent:th->win_bg;
        vga_color_t fg=s?th->win_bg:th->win_fg;
        vga_putchar_at(s?'>':' ',SUB_X,iy,fg,bg);
        char row[26]; int j=0;
        const char *n=sub[idx].name;
        while(*n&&j<SUB_W-3) row[j++]=*n++;
        while(j<SUB_W-3) row[j++]=' ';
        row[j]=0;
        vga_puts_at(row,SUB_X+1,iy,fg,bg);
        vga_putchar_at(' ',SUB_X+SUB_W-1,iy,fg,bg);
    }

    /* Scroll indicators */
    if(sub_scroll > 0)
        vga_putchar_at('^', SUB_X+SUB_W-2, MENU_ITEM_Y, th->accent, th->win_bg);
    if(sub_scroll + vis < nsub)
        vga_putchar_at('v', SUB_X+SUB_W-2, MENU_ITEM_Y+vis-1, th->accent, th->win_bg);

    /* Bottom border */
    vga_putchar_at('.',SUB_X,MENU_BOT,th->accent,th->win_bg);
    for(int x=SUB_X+1;x<SUB_X+SUB_W-1;x++) vga_putchar_at('-',x,MENU_BOT,th->accent,th->win_bg);
    vga_putchar_at('.',SUB_X+SUB_W-1,MENU_BOT,th->accent,th->win_bg);
}

/* ================================================================
   RIGHT-CLICK QUICK-ACCESS MENU  (v0.7.4)
   Tiny 2-item flyout (Settings / Display) anchored at the click
   point, clamped to stay on-screen. Reuses the exact border
   glyphs already proven on UTM SE in draw_submenu() above rather
   than introducing any new extended character.
   ================================================================ */
#define CTXM_W 14
#define CTXM_H 4
#define CTXM_NITEMS 2
static const char *ctxm_labels[CTXM_NITEMS]={"Settings","Display"};

/* Runs the action for a selected context-menu item. Shared by
   both the mouse-click and Enter-key activation paths so the
   behavior can't drift between the two. */
static void ctx_menu_activate(int sel){
    if(sel==0){
        app_settings();
    } else {
        int nt=(ui_current_theme()+1)%ui_theme_count();
        ui_set_theme(nt);
        char msg[32]="Theme: ";
        strcat(msg,ui_theme_name(nt));
        toast_info(msg);
    }
}

static void draw_contextmenu(void){
    theme_t *th=ui_theme();
    int x=ctx_x, y=ctx_y;

    /* Top border */
    vga_putchar_at('.',x,y,th->accent,th->win_bg);
    for(int i=1;i<CTXM_W-1;i++) vga_putchar_at('-',x+i,y,th->accent,th->win_bg);
    vga_putchar_at('.',x+CTXM_W-1,y,th->accent,th->win_bg);

    /* Items */
    for(int i=0;i<CTXM_NITEMS;i++){
        int s=(i==ctx_sel);
        vga_color_t bg=s?th->accent:th->win_bg;
        vga_color_t fg=s?th->win_bg:th->win_fg;
        vga_putchar_at('|',x,y+1+i,th->accent,th->win_bg);
        vga_putchar_at(s?'>':' ',x+1,y+1+i,fg,bg);
        char row[CTXM_W-3]; int j=0;
        const char *n=ctxm_labels[i];
        while(*n&&j<CTXM_W-4) row[j++]=*n++;
        while(j<CTXM_W-4) row[j++]=' ';
        row[j]=0;
        vga_puts_at(row,x+2,y+1+i,fg,bg);
        vga_putchar_at('|',x+CTXM_W-1,y+1+i,th->accent,th->win_bg);
    }

    /* Bottom border */
    vga_putchar_at('.',x,y+1+CTXM_NITEMS,th->accent,th->win_bg);
    for(int i=1;i<CTXM_W-1;i++) vga_putchar_at('-',x+i,y+1+CTXM_NITEMS,th->accent,th->win_bg);
    vga_putchar_at('.',x+CTXM_W-1,y+1+CTXM_NITEMS,th->accent,th->win_bg);
}

static void full_draw(int icon_sel, int m_state, int t_sel, int s_sel){
    (void)t_sel; (void)s_sel;
    ui_erase_cursor();
    ui_draw_desktop();
    ui_draw_desktop_bars();
    draw_icons(icon_sel, m_state!=0);
    draw_infopanel();
    if(m_state==1||m_state==2) draw_topmenu();
    if(m_state==2) draw_submenu();
    if(m_state==3) draw_contextmenu();
    if(mouse_is_ready()) ui_draw_cursor();
    vga_flush();
}

static void app_texteditor_load(const char *filename);

/* ================================================================
   APPS
   ================================================================ */
static void app_shell(void){ vga_clear(); shell_run(); }

/* ── Calculator ─────────────────────────────────────────────────── */
#define CALC_HIST 12
#define CALC_HIST_FILE ".calc_history"

static char calc_hist[CALC_HIST][80];
static int  calc_hist_count = 0;
static int  calc_hist_loaded = 0;

static void calc_load_history(void){
    if(calc_hist_loaded) return;
    calc_hist_loaded=1;
    char buf[CALC_HIST*82+4];
    int n=vfs_read(CALC_HIST_FILE,buf,sizeof(buf)-1);
    if(n<=0) return;
    buf[n]=0; calc_hist_count=0;
    char *line=buf;
    while(*line && calc_hist_count<CALC_HIST){
        char *end=line; while(*end&&*end!='\n')end++;
        char sv=*end; *end=0;
        if(line[0]){
            strncpy(calc_hist[calc_hist_count],line,79);
            calc_hist[calc_hist_count][79]=0;
            calc_hist_count++;
        }
        *end=sv; line=end; if(*line=='\n')line++;
    }
}

static void calc_save_history(void){
    char buf[CALC_HIST*82+4]; int bp=0;
    for(int i=0;i<calc_hist_count;i++){
        for(int j=0;calc_hist[i][j];j++) buf[bp++]=calc_hist[i][j];
        buf[bp++]='\n';
    }
    buf[bp]=0;
    vfs_write(CALC_HIST_FILE,buf,(uint32_t)bp);
}

static void calc_push_history(const char *expr, int result){
    /* Build "expr = result" */
    char entry[80]; char rs[20];
    strncpy(entry,expr,60); entry[60]=0;
    strcat(entry," = ");
    itoa(result,rs,10); strcat(entry,rs);
    /* Shift if full */
    if(calc_hist_count>=CALC_HIST){
        for(int i=0;i<CALC_HIST-1;i++) strcpy(calc_hist[i],calc_hist[i+1]);
        calc_hist_count=CALC_HIST-1;
    }
    strncpy(calc_hist[calc_hist_count++],entry,79);
    calc_save_history();
}

/* Recursive descent evaluator: handles +,-,*,/,% and () */
static const char *g_ep;
static int calc_parse_expr(void);
static int calc_parse_term(void);
static int calc_parse_factor(void);

static void calc_skip_ws(void){ while(*g_ep==' ')g_ep++; }

static int calc_parse_factor(void){
    calc_skip_ws();
    int neg=0;
    if(*g_ep=='-'){ neg=1; g_ep++; }
    else if(*g_ep=='+') g_ep++;
    calc_skip_ws();
    int v=0;
    if(*g_ep=='('){
        g_ep++;
        v=calc_parse_expr();
        calc_skip_ws();
        if(*g_ep==')') g_ep++;
    } else {
        while(*g_ep>='0'&&*g_ep<='9'){ v=v*10+(*g_ep-'0'); g_ep++; }
    }
    return neg?-v:v;
}

static int calc_parse_term(void){
    int v=calc_parse_factor();
    while(1){
        calc_skip_ws();
        if(*g_ep=='*'){ g_ep++; int r=calc_parse_factor(); v*=r; }
        else if(*g_ep=='/'){ g_ep++; int r=calc_parse_factor(); v=r?v/r:0; }
        else if(*g_ep=='%'){ g_ep++; int r=calc_parse_factor(); v=r?v%r:0; }
        else break;
    }
    return v;
}

static int calc_parse_expr(void){
    int v=calc_parse_term();
    while(1){
        calc_skip_ws();
        if(*g_ep=='+'){ g_ep++; v+=calc_parse_term(); }
        else if(*g_ep=='-'){ g_ep++; v-=calc_parse_term(); }
        else break;
    }
    return v;
}

static int calc_eval(const char *expr, int *ok){
    g_ep=expr; *ok=1;
    int v=calc_parse_expr();
    calc_skip_ws();
    if(*g_ep&&*g_ep!='='){ *ok=0; return 0; }
    return v;
}

static void app_calculator(void){
    calc_load_history();
    int hist_sel=-1; /* -1=typing, >=0=browsing history */
    char buf[64]=""; int pos=0;
    char result_str[32]=""; int has_result=0;

    while(1){
        vga_clear(); ui_draw_taskbar("Calculator");
        ui_draw_window(16,2,48,21,"Calculator  v0.7.4");

        /* History panel */
        vga_puts_at("History",17,4,VGA_LIGHT_CYAN,VGA_BLACK);
        for(int x=17;x<64;x++) vga_putchar_at('-',x,5,VGA_DARK_GREY,VGA_BLACK);
        int show=calc_hist_count>10?10:calc_hist_count;
        for(int i=0;i<show;i++){
            int idx=calc_hist_count-show+i;
            int s=(hist_sel==idx);
            vga_color_t fg=s?VGA_BLACK:VGA_LIGHT_GREY;
            vga_color_t bg=s?VGA_LIGHT_GREY:VGA_BLACK;
            char row[47]; strncpy(row,calc_hist[idx],46); row[46]=0;
            /* Pad */
            int rl=(int)strlen(row);
            while(rl<46) row[rl++]=' '; row[46]=0;
            vga_puts_at(row,17,6+i,fg,bg);
        }
        if(!calc_hist_count)
            vga_puts_at("(no history yet)",17,8,VGA_DARK_GREY,VGA_BLACK);

        /* Divider */
        for(int x=17;x<64;x++) vga_putchar_at('-',x,17,VGA_DARK_GREY,VGA_BLACK);

        /* Input line */
        vga_puts_at("Expression:",17,18,VGA_WHITE,VGA_BLACK);
        char disp[48]; strncpy(disp,buf,46); disp[46]=0;
        int dl=(int)strlen(disp);
        while(dl<46) disp[dl++]=' '; disp[46]=0;
        vga_puts_at(disp,17,19,VGA_LIGHT_CYAN,VGA_BLACK);
        vga_set_cursor(17+pos,19);

        /* Result */
        if(has_result){
            vga_puts_at("= ",17,20,VGA_WHITE,VGA_BLACK);
            vga_puts_at(result_str,19,20,VGA_LIGHT_GREEN,VGA_BLACK);
        }

        vga_puts_at("Up-Dn=history  Enter=calc  Ctrl+C=clear  ESC=close",17,22,VGA_DARK_GREY,VGA_BLACK);

        int c=keyboard_waitchar();
        if(c==27){ return; }
        if(c==3){ /* Ctrl+C — clear */
            buf[0]=0; pos=0; has_result=0; result_str[0]=0; hist_sel=-1; continue;
        }
        if(c==0x148){ /* Up — browse history */
            if(calc_hist_count>0){
                if(hist_sel<0) hist_sel=calc_hist_count-1;
                else if(hist_sel>0) hist_sel--;
                /* Copy expression part (before " = ") to input */
                strncpy(buf,calc_hist[hist_sel],62);
                /* Truncate at " = " */
                char *eq=buf; while(*eq&&!(eq[0]==' '&&eq[1]=='=')){eq++;}
                if(*eq) *eq=0;
                pos=(int)strlen(buf);
                has_result=0;
            }
            continue;
        }
        if(c==0x150){ /* Down — browse history */
            if(hist_sel>=0){
                hist_sel++;
                if(hist_sel>=calc_hist_count){ hist_sel=-1; buf[0]=0; pos=0; }
                else {
                    strncpy(buf,calc_hist[hist_sel],62);
                    char *eq=buf; while(*eq&&!(eq[0]==' '&&eq[1]=='=')){eq++;}
                    if(*eq) *eq=0;
                    pos=(int)strlen(buf);
                }
                has_result=0;
            }
            continue;
        }
        if(c=='\n'||c=='\r'){
            if(!buf[0]) continue;
            int ok=0; int r=calc_eval(buf,&ok);
            if(ok){
                itoa(r,result_str,10);
                has_result=1;
                calc_push_history(buf,r);
            } else {
                strcpy(result_str,"Syntax error");
                has_result=1;
            }
            continue;
        }
        if((c=='\b'||c==127)&&pos>0){
            pos--; buf[pos]=0; has_result=0; hist_sel=-1; continue;
        }
        if(c>=32&&c<127&&pos<62){
            buf[pos++]=c; buf[pos]=0; has_result=0; hist_sel=-1;
        }
    }
}

/* ═══════════════════════════════════════════════════════════
   NOTEPAD — Full screen text editor (v0.7.4)
   Layout: row 0 = taskbar, row 1 = title bar,
           rows 2-22 = content (21 lines visible),
           row 23 = status bar, row 24 = hint bar
   New in v0.7.4:
     - Selection with Shift+arrows (highlighted in reverse)
     - Ctrl+C copy selection, Ctrl+X cut selection, Ctrl+V paste
     - Right-click context menu: Copy / Cut / Paste / Select All
     - Multi-line clipboard (up to ED_CLIP_LINES lines)
   ═══════════════════════════════════════════════════════════ */
#define ED_ROWS        200   /* max lines in buffer            */
#define ED_COLS        79    /* max chars per line             */
#define ED_VROWS       21    /* visible rows on screen         */
#define ED_VCOLS       74    /* visible cols (after line nums) */
#define ED_LNPAD       5     /* line number gutter width       */
#define ED_CLIP_LINES  64    /* max lines in clipboard         */

static char  ed_buf[ED_ROWS][ED_COLS];
static char  ed_filename[48];
static int   ed_dirty;
static int   ed_insert;

/* ── Selection state ─────────────────────────────────────── */
static int ed_sel_active;    /* 1 if selection exists          */
static int ed_sel_sr;        /* selection start row            */
static int ed_sel_sc;        /* selection start col            */
static int ed_sel_er;        /* selection end   row            */
static int ed_sel_ec;        /* selection end   col            */

/* ── Multi-line clipboard ─────────────────────────────────── */
static char ed_clip[ED_CLIP_LINES][ED_COLS];
static int  ed_clip_lines;   /* number of lines in clipboard   */

/* ── Helpers ──────────────────────────────────────────────── */
static int ed_last_row(void){
    int r=0;
    for(int i=0;i<ED_ROWS;i++) if(ed_buf[i][0]) r=i;
    return r;
}

/* Normalise selection so sr/sc <= er/ec */
static void ed_sel_norm(int *sr,int *sc,int *er,int *ec){
    if(*sr>*er||((*sr)==(*er)&&*sc>*ec)){
        int tr=*sr; *sr=*er; *er=tr;
        int tc=*sc; *sc=*ec; *ec=tc;
    }
}

/* Is position (row,col) inside the current selection? */
static int ed_in_sel(int row,int col){
    if(!ed_sel_active) return 0;
    int sr=ed_sel_sr,sc=ed_sel_sc,er=ed_sel_er,ec=ed_sel_ec;
    ed_sel_norm(&sr,&sc,&er,&ec);
    if(row<sr||row>er) return 0;
    if(row==sr&&col<sc) return 0;
    if(row==er&&col>=ec) return 0;
    return 1;
}

/* ── Draw the full editor screen ────────────────────────────
   Highlights selected region in reverse (cyan bg / black fg) */
static void ed_redraw(int cur_row, int cur_col, int scroll_row, int scroll_col)
{
    /* Title bar */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,1,VGA_WHITE,VGA_BLUE);
    vga_puts_at(" Notepad",0,1,VGA_WHITE,VGA_BLUE);
    if(ed_filename[0]) vga_puts_at(ed_filename,12,1,VGA_YELLOW,VGA_BLUE);
    else               vga_puts_at("[Untitled]",12,1,VGA_LIGHT_GREY,VGA_BLUE);
    if(ed_dirty)       vga_putchar_at('*',11,1,VGA_LIGHT_RED,VGA_BLUE);
    vga_puts_at(ed_insert?"[INS]":"[OVR]",73,1,VGA_LIGHT_CYAN,VGA_BLUE);
    if(ed_sel_active)  vga_puts_at("[SEL]",67,1,VGA_YELLOW,VGA_BLUE);

    /* Content area */
    for(int vr=0;vr<ED_VROWS;vr++){
        int br = vr + scroll_row;
        /* Line number gutter */
        char ln[6]="     ";
        if(br<ED_ROWS){
            char tmp[6]; utoa((uint32_t)(br+1),tmp,10);
            int tl=(int)strlen(tmp);
            for(int i=0;i<tl&&i<4;i++) ln[3-tl+1+i]=tmp[i];
        }
        vga_color_t lgc = (br==cur_row)?VGA_LIGHT_CYAN:VGA_DARK_GREY;
        vga_puts_at(ln, 0, 2+vr, lgc, VGA_BLACK);
        vga_putchar_at('|', ED_LNPAD-1, 2+vr, VGA_DARK_GREY, VGA_BLACK);

        /* Line content — char by char so we can highlight selection */
        for(int dc=0;dc<ED_VCOLS;dc++){
            int bc = dc + scroll_col;
            char ch = (br<ED_ROWS && bc<ED_COLS) ? ed_buf[br][bc] : 0;
            if(!ch) ch=' ';
            int in_s = ed_in_sel(br,bc);
            vga_color_t fg = in_s ? VGA_BLACK   : (br==cur_row?VGA_WHITE:VGA_LIGHT_GREY);
            vga_color_t bg = in_s ? VGA_CYAN     : VGA_BLACK;
            vga_putchar_at(ch, ED_LNPAD+dc, 2+vr, fg, bg);
        }
    }

    /* Status bar */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,23,VGA_BLACK,VGA_CYAN);
    char pos[32]; char rn[8]; char cn[8];
    utoa((uint32_t)(cur_row+1),rn,10); utoa((uint32_t)(cur_col+1),cn,10);
    strcpy(pos,"Ln:"); strcat(pos,rn); strcat(pos," Col:"); strcat(pos,cn);
    vga_puts_at(pos,1,23,VGA_BLACK,VGA_CYAN);
    if(ed_filename[0]) vga_puts_at(ed_filename,20,23,VGA_BLACK,VGA_CYAN);
    else               vga_puts_at("Untitled",20,23,VGA_BLACK,VGA_CYAN);
    vga_puts_at(ed_dirty?"*Modified*":"          ",40,23,VGA_BLACK,VGA_CYAN);
    if(ed_sel_active)  vga_puts_at("SELECTION",52,23,VGA_BLUE,VGA_YELLOW);

    /* Hint bar */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("^S=Save ^W=SaveAs ^Q=Quit ^C=Copy ^X=Cut ^V=Paste ^A=All",1,24,VGA_DARK_GREY,VGA_BLACK);

    /* Cursor */
    int scr_c = cur_col - scroll_col;
    int scr_r = cur_row - scroll_row;
    if(scr_c>=0&&scr_c<ED_VCOLS&&scr_r>=0&&scr_r<ED_VROWS)
        vga_set_cursor(ED_LNPAD+scr_c, 2+scr_r);
}

/* ── Basic edit operations ───────────────────────────────── */
static void ed_insert_char(int row, int col, char c){
    int len=(int)strlen(ed_buf[row]);
    if(len>=ED_COLS-2) return;
    for(int i=len;i>=col;i--) ed_buf[row][i+1]=ed_buf[row][i];
    ed_buf[row][col]=c;
    ed_dirty=1;
}
static void ed_delete_char(int row, int col){
    int len=(int)strlen(ed_buf[row]);
    if(col>=len) return;
    for(int i=col;i<len;i++) ed_buf[row][i]=ed_buf[row][i+1];
    ed_dirty=1;
}
static void ed_join_lines(int row){
    if(row>=ED_ROWS-1) return;
    int la=(int)strlen(ed_buf[row]);
    int lb=(int)strlen(ed_buf[row+1]);
    if(la+lb<ED_COLS-1){
        strcat(ed_buf[row],ed_buf[row+1]);
        for(int i=row+1;i<ED_ROWS-1;i++) strcpy(ed_buf[i],ed_buf[i+1]);
        ed_buf[ED_ROWS-1][0]=0;
        ed_dirty=1;
    }
}
static void ed_split_line(int row, int col){
    if(row>=ED_ROWS-2) return;
    for(int i=ED_ROWS-1;i>row+1;i--) strcpy(ed_buf[i],ed_buf[i-1]);
    strcpy(ed_buf[row+1], ed_buf[row]+col);
    ed_buf[row][col]=0;
    ed_dirty=1;
}

/* ── Clipboard operations ────────────────────────────────── */

/* Copy selection into clipboard. Returns 1 if something was copied. */
static int ed_copy_sel(void){
    if(!ed_sel_active) return 0;
    int sr=ed_sel_sr,sc=ed_sel_sc,er=ed_sel_er,ec=ed_sel_ec;
    ed_sel_norm(&sr,&sc,&er,&ec);
    for(int i=0;i<ED_CLIP_LINES;i++) ed_clip[i][0]=0;
    ed_clip_lines=0;
    for(int r=sr;r<=er&&ed_clip_lines<ED_CLIP_LINES;r++){
        int from=(r==sr)?sc:0;
        int to  =(r==er)?ec:(int)strlen(ed_buf[r]);
        int n=to-from; if(n<0)n=0;
        if(n>=ED_COLS) n=ED_COLS-1;
        for(int i=0;i<n;i++) ed_clip[ed_clip_lines][i]=ed_buf[r][from+i];
        ed_clip[ed_clip_lines][n]=0;
        ed_clip_lines++;
    }
    return 1;
}

/* Delete selected text, return new cursor position */
static void ed_delete_sel(int *crow,int *ccol){
    if(!ed_sel_active) return;
    int sr=ed_sel_sr,sc=ed_sel_sc,er=ed_sel_er,ec=ed_sel_ec;
    ed_sel_norm(&sr,&sc,&er,&ec);
    if(sr==er){
        /* Single line — remove chars sc..ec */
        int len=(int)strlen(ed_buf[sr]);
        for(int i=sc;i<len;i++) ed_buf[sr][i]=ed_buf[sr][i+(ec-sc)<len-i?ec-sc:0];
        /* safer: memmove-style */
        int rmv=ec-sc; if(rmv<0)rmv=0;
        for(int i=sc;i<len-rmv;i++) ed_buf[sr][i]=ed_buf[sr][i+rmv];
        for(int i=len-rmv;i<len;i++) ed_buf[sr][i]=0;
    } else {
        /* Multi-line: keep head of sr up to sc, keep tail of er from ec */
        /* Merge sr tail with er tail */
        ed_buf[sr][sc]=0;
        strncat(ed_buf[sr], ed_buf[er]+ec, ED_COLS-sc-1);
        /* Delete lines sr+1 .. er */
        int del=er-sr;
        for(int i=sr+1;i<ED_ROWS-del;i++) strcpy(ed_buf[i],ed_buf[i+del]);
        for(int i=ED_ROWS-del;i<ED_ROWS;i++) ed_buf[i][0]=0;
    }
    *crow=sr; *ccol=sc;
    ed_sel_active=0;
    ed_dirty=1;
}

/* Paste clipboard at cursor position */
static void ed_paste(int *crow,int *ccol){
    if(ed_clip_lines==0) return;
    if(ed_sel_active) ed_delete_sel(crow,ccol);
    int r=*crow, c=*ccol;
    if(ed_clip_lines==1){
        /* Single-line paste: insert inline */
        int n=(int)strlen(ed_clip[0]);
        for(int i=0;i<n;i++){ ed_insert_char(r,c+i,ed_clip[0][i]); }
        *ccol=c+n;
    } else {
        /* Multi-line: split current line, insert clip lines */
        ed_split_line(r,c);
        /* First clip line appends to r */
        int n0=(int)strlen(ed_clip[0]);
        for(int i=0;i<n0;i++) ed_insert_char(r,(int)strlen(ed_buf[r]),ed_clip[0][i]);
        /* Middle lines: insert new rows */
        for(int cl=1;cl<ed_clip_lines-1;cl++){
            /* shift rows down */
            for(int i=ED_ROWS-1;i>r+cl+1;i--) strcpy(ed_buf[i],ed_buf[i-1]);
            strncpy(ed_buf[r+cl],ed_clip[cl],ED_COLS-1);
        }
        /* Last clip line prepends to what was r+1 */
        int last=ed_clip_lines-1;
        int nr=r+last;
        if(nr<ED_ROWS){
            char tmp[ED_COLS]; strncpy(tmp,ed_buf[nr],ED_COLS-1); tmp[ED_COLS-1]=0;
            strncpy(ed_buf[nr],ed_clip[last],ED_COLS-1);
            strncat(ed_buf[nr],tmp,ED_COLS-1-(int)strlen(ed_buf[nr]));
            *crow=nr; *ccol=(int)strlen(ed_clip[last]);
        }
    }
    ed_dirty=1;
}

/* ── Right-click context menu ────────────────────────────── */
/* Shows a small popup at a fixed position, returns chosen action:
   0=nothing, 1=copy, 2=cut, 3=paste, 4=select all             */
static int ed_ctx_menu(void){
    /* Draw menu at col 10, row 8 — clear area around it */
    const char *items[]={"Copy","Cut","Paste","Select All"};
    int n=4;
    int mx=20, my=8, mw=14;
    /* Draw box */
    for(int x=mx;x<mx+mw;x++) vga_putchar_at(' ',x,my-1,VGA_WHITE,VGA_DARK_GREY);
    for(int i=0;i<n;i++){
        for(int x=mx;x<mx+mw;x++) vga_putchar_at(' ',x,my+i,VGA_WHITE,VGA_DARK_GREY);
        vga_puts_at(items[i],mx+1,my+i,VGA_WHITE,VGA_DARK_GREY);
    }
    for(int x=mx;x<mx+mw;x++) vga_putchar_at(' ',x,my+n,VGA_WHITE,VGA_DARK_GREY);

    int sel=0;
    while(1){
        /* Highlight current item */
        for(int i=0;i<n;i++){
            vga_color_t bg=(i==sel)?VGA_BLUE:VGA_DARK_GREY;
            vga_color_t fg=(i==sel)?VGA_WHITE:VGA_LIGHT_GREY;
            for(int x=mx;x<mx+mw;x++) vga_putchar_at(' ',x,my+i,fg,bg);
            vga_puts_at(items[i],mx+1,my+i,fg,bg);
        }
        int k=keyboard_waitchar();
        if(k==0x148&&sel>0) sel--;           /* Up   */
        else if(k==0x150&&sel<n-1) sel++;    /* Down */
        else if(k=='\n'||k=='\r') return sel+1;
        else if(k==27) return 0;             /* ESC  */
    }
}

/* ── Save prompt ─────────────────────────────────────────── */
static void ed_save_prompt(void){
    if(!ed_filename[0]){
        for(int x=0;x<80;x++) vga_putchar_at(' ',x,23,VGA_BLACK,VGA_CYAN);
        vga_puts_at("Save as: ",0,23,VGA_BLACK,VGA_CYAN);
        vga_set_cursor(9,23);
        int sp=0; char name[48]; name[0]=0;
        while(1){
            int k=keyboard_waitchar();
            if(k=='\n'||k=='\r') break;
            if(k==27){return;}
            if(k=='\b'&&sp>0){sp--;name[sp]=0;vga_putchar_at(' ',9+sp,23,VGA_BLACK,VGA_CYAN);vga_set_cursor(9+sp,23);}
            else if(k>=32&&k<127&&sp<46){name[sp++]=k;name[sp]=0;vga_putchar_at(k,9+sp-1,23,VGA_BLACK,VGA_CYAN);vga_set_cursor(9+sp,23);}
        }
        if(name[0]) strncpy(ed_filename,name,47);
        else return;
    }
    static char fbuf[ED_ROWS*(ED_COLS+2)];
    int p=0;
    for(int r=0;r<ED_ROWS;r++){
        int l=(int)strlen(ed_buf[r]);
        for(int i=0;i<l;i++) fbuf[p++]=ed_buf[r][i];
        fbuf[p++]='\n';
        if(r==ed_last_row()) break;
    }
    int wr_result = vfs_write(ed_filename,fbuf,p);
    if(wr_result >= 0){
        ed_dirty=0;
        for(int x=0;x<80;x++) vga_putchar_at(' ',x,23,VGA_BLACK,VGA_CYAN);
        vga_puts_at("Saved.",0,23,VGA_BLACK,VGA_CYAN);
        if(!vfs_using_fat16())
            vga_puts_at("(RAMFS - lost on reboot, no persistent disk)",10,23,VGA_YELLOW,VGA_CYAN);
        sleep_ms(1400);
    } else {
        /* Save genuinely failed — do NOT clear the dirty flag, and
           make this loud rather than silently pretending it worked.
           Held on screen deliberately longer than a success message
           since this is the case the person actually needs to see. */
        for(int x=0;x<80;x++) vga_putchar_at(' ',x,23,VGA_BLACK,VGA_RED);
        vga_puts_at(" SAVE FAILED - changes NOT written to disk! Try again. ",0,23,VGA_WHITE,VGA_RED);
        sleep_ms(2200);
    }
}

/* ── Main editor loop ────────────────────────────────────── */
static void app_texteditor_run(void){
    vga_clear();
    ui_draw_taskbar("Notepad");
    int crow=0,ccol=0,srow=0,scol=0;
    ed_sel_active=0;
    ed_redraw(crow,ccol,srow,scol);

    while(1){
        /* Poll mouse for right-click context menu */
        extern void mouse_get(int*,int*,int*);
        extern int  mouse_is_ready(void);
        if(mouse_is_ready()){
            int mx,my,mb; mouse_get(&mx,&my,&mb);
            if(mb&2){   /* right button */
                /* Wait for release */
                do { mouse_get(&mx,&my,&mb); } while(mb&2);
                int act=ed_ctx_menu();
                if(act==1){                        /* Copy */
                    ed_copy_sel();
                } else if(act==2){                 /* Cut  */
                    ed_copy_sel();
                    ed_delete_sel(&crow,&ccol);
                } else if(act==3){                 /* Paste */
                    ed_paste(&crow,&ccol);
                } else if(act==4){                 /* Select All */
                    ed_sel_active=1;
                    ed_sel_sr=0; ed_sel_sc=0;
                    int lr=ed_last_row();
                    ed_sel_er=lr; ed_sel_ec=(int)strlen(ed_buf[lr]);
                }
                ed_redraw(crow,ccol,srow,scol);
                continue;
            }
        }

        int c=keyboard_waitchar();
        extern int kbd_shift(void);
        int shift=kbd_shift();

        /* ── Ctrl combos (handled before nav so they always fire) ── */
        if(c==('a'&0x1F)){                   /* Ctrl+A select all */
            ed_sel_active=1;
            ed_sel_sr=0; ed_sel_sc=0;
            int lr=ed_last_row();
            ed_sel_er=lr; ed_sel_ec=(int)strlen(ed_buf[lr]);
        } else if(c==('c'&0x1F)){            /* Ctrl+C copy */
            ed_copy_sel();
        } else if(c==('s'&0x1F)){            /* Ctrl+S save */
            ed_save_prompt();
        } else if(c==('w'&0x1F)){            /* Ctrl+W save as (new name) */
            /* Clear filename so ed_save_prompt always asks for a new name */
            char old_fname[48];
            strncpy(old_fname, ed_filename, 47); old_fname[47]=0;
            ed_filename[0] = 0;
            ed_save_prompt();
            /* If the user cancelled (ESC in the prompt), restore old name */
            if(!ed_filename[0]) strncpy(ed_filename, old_fname, 47);
        } else if(c==('q'&0x1F)||c==27){     /* Ctrl+Q / ESC quit */
            if(ed_dirty){
                for(int x=0;x<80;x++) vga_putchar_at(' ',x,23,VGA_BLACK,VGA_RED);
                vga_puts_at(" Unsaved! Q=Discard  S=Save  ESC=Cancel",0,23,VGA_WHITE,VGA_RED);
                int k=keyboard_waitchar();
                if(k==('q'&0x1F)||k=='q'||k=='Q') break;
                else if(k==('s'&0x1F)||k=='s'||k=='S'){ ed_save_prompt(); break; }
            } else break;
        } else if(c==('x'&0x1F)){            /* Ctrl+X cut */
            if(ed_sel_active){
                ed_copy_sel();
                ed_delete_sel(&crow,&ccol);
            } else {
                for(int i=0;i<ED_CLIP_LINES;i++) ed_clip[i][0]=0;
                ed_clip_lines=1;
                strncpy(ed_clip[0],ed_buf[crow],ED_COLS-1);
                for(int i=crow;i<ED_ROWS-1;i++) strcpy(ed_buf[i],ed_buf[i+1]);
                ed_buf[ED_ROWS-1][0]=0;
                if(crow>ed_last_row()&&crow>0) crow--;
                ccol=0; ed_dirty=1;
            }
        } else if(c==('v'&0x1F)){            /* Ctrl+V paste */
            ed_paste(&crow,&ccol);
        } else if(c==('n'&0x1F)){            /* Ctrl+N new */
            for(int i=0;i<ED_ROWS;i++) memset(ed_buf[i],0,ED_COLS);
            ed_filename[0]=0; ed_dirty=0; ed_sel_active=0;
            crow=0; ccol=0; srow=0; scol=0;
        }
        /* ── Navigation ─────────────────────────────────────────── */
        else if(c==0x148){                   /* Up */
            if(crow>0){ crow--;
              int l=(int)strlen(ed_buf[crow]); if(ccol>l) ccol=l; }
            if(shift){ if(!ed_sel_active){ed_sel_active=1;ed_sel_sr=crow+1;ed_sel_sc=ccol;}
                        ed_sel_er=crow; ed_sel_ec=ccol; }
            else ed_sel_active=0;
        } else if(c==0x150){                 /* Down */
            if(crow<ed_last_row()&&crow<ED_ROWS-1){ crow++;
              int l=(int)strlen(ed_buf[crow]); if(ccol>l) ccol=l; }
            if(shift){ if(!ed_sel_active){ed_sel_active=1;ed_sel_sr=crow-1;ed_sel_sc=ccol;}
                        ed_sel_er=crow; ed_sel_ec=ccol; }
            else ed_sel_active=0;
        } else if(c==0x14B){                 /* Left */
            int pr=crow, pc=ccol;
            if(ccol>0) ccol--;
            else if(crow>0){ crow--; ccol=(int)strlen(ed_buf[crow]); }
            if(shift){ if(!ed_sel_active){ed_sel_active=1;ed_sel_sr=pr;ed_sel_sc=pc;}
                        ed_sel_er=crow; ed_sel_ec=ccol; }
            else ed_sel_active=0;
        } else if(c==0x14D){                 /* Right */
            int pr=crow, pc=ccol;
            int l=(int)strlen(ed_buf[crow]);
            if(ccol<l) ccol++;
            else if(crow<ED_ROWS-1){ crow++; ccol=0; }
            if(shift){ if(!ed_sel_active){ed_sel_active=1;ed_sel_sr=pr;ed_sel_sc=pc;}
                        ed_sel_er=crow; ed_sel_ec=ccol; }
            else ed_sel_active=0;
        } else if(c==0x147){                 /* Home — start of line */
            if(shift){ if(!ed_sel_active){ed_sel_active=1;ed_sel_sr=crow;ed_sel_sc=ccol;}
                        ccol=0; ed_sel_er=crow; ed_sel_ec=0; }
            else { ccol=0; ed_sel_active=0; }
        } else if(c==0x14F){                 /* End — end of line */
            int l=(int)strlen(ed_buf[crow]);
            if(shift){ if(!ed_sel_active){ed_sel_active=1;ed_sel_sr=crow;ed_sel_sc=ccol;}
                        ccol=l; ed_sel_er=crow; ed_sel_ec=l; }
            else { ccol=l; ed_sel_active=0; }
        } else if(c==0x149){                 /* PgUp */
            int pr=crow;
            crow-=ED_VROWS; if(crow<0) crow=0;
            int l=(int)strlen(ed_buf[crow]); if(ccol>l) ccol=l;
            if(shift){ if(!ed_sel_active){ed_sel_active=1;ed_sel_sr=pr;ed_sel_sc=ccol;}
                        ed_sel_er=crow; ed_sel_ec=ccol; }
            else ed_sel_active=0;
        } else if(c==0x151){                 /* PgDn */
            int pr=crow; int lr=ed_last_row();
            crow+=ED_VROWS; if(crow>lr) crow=lr;
            int l=(int)strlen(ed_buf[crow]); if(ccol>l) ccol=l;
            if(shift){ if(!ed_sel_active){ed_sel_active=1;ed_sel_sr=pr;ed_sel_sc=ccol;}
                        ed_sel_er=crow; ed_sel_ec=ccol; }
            else ed_sel_active=0;
        } else if(c==0x152){                 /* Ins toggle */
            ed_insert^=1;
        }
        /* ── Editing keys ────────────────────────────────────────── */
        else if(c=='\n'||c=='\r'){
            if(ed_sel_active) ed_delete_sel(&crow,&ccol);
            ed_split_line(crow,ccol); crow++; ccol=0;
        } else if(c=='\b'){
            if(ed_sel_active){ ed_delete_sel(&crow,&ccol); }
            else if(ccol>0){ ccol--; ed_delete_char(crow,ccol); }
            else if(crow>0){ crow--; ccol=(int)strlen(ed_buf[crow]); ed_join_lines(crow); }
        } else if(c==0x153){                 /* Del key */
            if(ed_sel_active){ ed_delete_sel(&crow,&ccol); }
            else {
                int l=(int)strlen(ed_buf[crow]);
                if(ccol<l) ed_delete_char(crow,ccol);
                else ed_join_lines(crow);
            }
        } else if(c>=32&&c<127){
            if(ed_sel_active) ed_delete_sel(&crow,&ccol);
            if(ed_insert) ed_insert_char(crow,ccol,c);
            else { if(ccol<ED_COLS-1){ed_buf[crow][ccol]=(char)c; ed_dirty=1;} }
            ccol++;
        }

        /* Collapse selection if start==end */
        if(ed_sel_active&&ed_sel_sr==ed_sel_er&&ed_sel_sc==ed_sel_ec)
            ed_sel_active=0;

        /* Scroll to keep cursor visible */
        if(crow<srow) srow=crow;
        if(crow>=srow+ED_VROWS) srow=crow-ED_VROWS+1;
        if(ccol<scol) scol=ccol;
        if(ccol>=scol+ED_VCOLS) scol=ccol-ED_VCOLS+1;

        ed_redraw(crow,ccol,srow,scol);
    }
    vga_clear();
}

/* ---- Notepad launch picker: New vs Open existing ---- */
#define ED_PICK_MAX 24
static char ed_pick_names[ED_PICK_MAX][32];
static uint32_t ed_pick_sizes[ED_PICK_MAX];
static int  ed_pick_count;

static void ed_pick_collect_cb(const char *name, int is_dir, uint32_t size){
    if(is_dir) return;                 /* files only */
    if(ed_pick_count>=ED_PICK_MAX) return;
    strncpy(ed_pick_names[ed_pick_count], name, 31);
    ed_pick_sizes[ed_pick_count]=size;
    ed_pick_count++;
}

/* Draws the New/Open picker and returns a filename to load,
   or NULL (via *want_new=1) if the person chose New/blank,
   or NULL with *want_new=0 if they cancelled entirely (ESC). */
static const char *ed_launch_picker(int *want_new){
    extern void vfs_list(void(*)(const char*,int,uint32_t));
    static char chosen[32];
    ed_pick_count=0;
    vfs_list(ed_pick_collect_cb);

    /* No saved files at all — nothing to open, so just go straight
       to a blank document instead of showing an empty picker. */
    if(ed_pick_count==0){ *want_new=1; return NULL; }

    vga_clear();
    ui_draw_taskbar("Notepad");
    vga_puts_at(" Notepad", 0, 1, VGA_WHITE, VGA_BLUE);
    for(int x=8;x<80;x++) vga_putchar_at(' ',x,1,VGA_WHITE,VGA_BLUE);

    int sel=0; /* 0 = "New blank document", 1..N = files */
    int total = ed_pick_count+1;

    while(1){
        for(int y=3;y<3+total && y<24;y++) vga_puts_at("                                                        ",2,y,VGA_WHITE,VGA_BLACK);

        vga_puts_at("What would you like to open?", 2, 3, VGA_LIGHT_CYAN, VGA_BLACK);

        vga_color_t fg0 = (sel==0)?VGA_BLACK:VGA_WHITE;
        vga_color_t bg0 = (sel==0)?VGA_CYAN :VGA_BLACK;
        for(int x=2;x<60;x++) vga_putchar_at(' ',x,5,fg0,bg0);
        vga_puts_at("[New]  Blank document", 3, 5, fg0, bg0);

        for(int i=0;i<ed_pick_count;i++){
            int y=7+i;
            if(y>23) break;
            vga_color_t fg=(sel==i+1)?VGA_BLACK:VGA_LIGHT_GREY;
            vga_color_t bg=(sel==i+1)?VGA_CYAN :VGA_BLACK;
            for(int x=2;x<60;x++) vga_putchar_at(' ',x,y,fg,bg);
            vga_puts_at(ed_pick_names[i], 3, y, fg, bg);
            char sz[16]; utoa(ed_pick_sizes[i],sz,10);
            vga_puts_at(sz, 40, y, fg, bg);
            vga_puts_at("bytes", 48, y, fg, bg);
        }

        vga_puts_at("Up/Down=select  Enter=open  Del=delete  Esc=cancel", 2, 24, VGA_DARK_GREY, VGA_BLACK);

        int k=keyboard_waitchar();
        if(k==0x148){ if(sel>0) sel--; }
        else if(k==0x150){ if(sel<total-1) sel++; }
        else if((k==127||k==0x153) && sel>0){
            /* Delete the highlighted file (not the New entry) */
            extern int vfs_delete(const char*);
            vga_puts_at("  Delete this file? Y/N  ", 2, 22, VGA_BLACK, VGA_LIGHT_RED);
            int confirm=keyboard_waitchar();
            if(confirm=='y'||confirm=='Y'){
                int rc=vfs_delete(ed_pick_names[sel-1]);
                if(rc<0){
                    vga_puts_at("  DELETE FAILED - file still present  ", 2, 22, VGA_WHITE, VGA_RED);
                    sleep_ms(1400);
                } else {
                    /* Refresh the list in place */
                    ed_pick_count=0;
                    vfs_list(ed_pick_collect_cb);
                    total=ed_pick_count+1;
                    if(sel>=total) sel=total-1;
                }
            }
            for(int x=2;x<60;x++) vga_putchar_at(' ',x,22,VGA_WHITE,VGA_BLACK);
        }
        else if(k=='\n'||k=='\r'){
            if(sel==0){ *want_new=1; return NULL; }
            strncpy(chosen, ed_pick_names[sel-1], 31);
            *want_new=0;
            return chosen;
        } else if(k==27){
            *want_new=-1; /* signal: cancelled, don't open editor at all */
            return NULL;
        }
    }
}

static void app_texteditor(void){
    int want_new=1;
    const char *pick = ed_launch_picker(&want_new);

    if(want_new==-1) return; /* person cancelled — back to desktop */

    for(int i=0;i<ED_ROWS;i++) memset(ed_buf[i],0,ED_COLS);
    ed_filename[0]=0; ed_dirty=0; ed_insert=1;
    ed_sel_active=0; ed_clip_lines=0;

    if(!want_new && pick){
        strncpy(ed_filename,pick,47);
        static char fbuf[ED_ROWS*(ED_COLS+2)];
        int r=vfs_read(ed_filename,fbuf,sizeof(fbuf)-1);
        if(r>0){
            fbuf[r]=0; int row=0,col=0;
            for(int i=0;i<r&&row<ED_ROWS;i++){
                if(fbuf[i]=='\n'){row++;col=0;}
                else if(fbuf[i]>=32&&col<ED_COLS-1){ed_buf[row][col++]=fbuf[i];ed_buf[row][col]=0;}
            }
        }
    }

    app_texteditor_run();
    /* Reinit keyboard so desktop loop gets clean state */
    extern void keyboard_init(void);
    extern void irq_enable_basic(void);
    extern void irq_enable_mouse(void);
    extern int  mouse_get_clicks(void);
    keyboard_init();
    irq_enable_basic();
    irq_enable_mouse();
    mouse_get_clicks(); /* flush stale click edge */
}

static void app_texteditor_load(const char *filename){
    for(int i=0;i<ED_ROWS;i++) memset(ed_buf[i],0,ED_COLS);
    strncpy(ed_filename,filename,47); ed_dirty=0; ed_insert=1;
    ed_sel_active=0; ed_clip_lines=0;
    static char fbuf[ED_ROWS*(ED_COLS+2)];
    int r=vfs_read(filename,fbuf,sizeof(fbuf)-1);
    if(r>0){
        fbuf[r]=0; int row=0,col=0;
        for(int i=0;i<r&&row<ED_ROWS;i++){
            if(fbuf[i]=='\n'){row++;col=0;}
            else if(fbuf[i]>=32&&col<ED_COLS-1){ed_buf[row][col++]=fbuf[i];ed_buf[row][col]=0;}
        }
    }
    app_texteditor_run();
    extern void keyboard_init(void);
    extern void irq_enable_basic(void);
    extern void irq_enable_mouse(void);
    extern int  mouse_get_clicks(void);
    keyboard_init();
    irq_enable_basic();
    irq_enable_mouse();
    mouse_get_clicks(); /* flush stale click edge */
}

/* Called from shell — open editor with optional filename */
void shell_launch_editor(const char *fname){
    if(fname && fname[0]) app_texteditor_load(fname);
    else app_texteditor();
}

/* ---- File manager state ---- */
#define FM_MAX 32
static char fm_names[FM_MAX][32];
static int  fm_dirs[FM_MAX];
static uint32_t fm_sizes[FM_MAX];
static int  fm_count;
static char fm_cwd[64] = "";  /* current path in file manager */

extern void vfs_list_dir(const char*, void(*)(const char*,int,uint32_t));
extern int  vfs_is_dir(const char*);

static void fm_collect_cb(const char *name, int is_dir, uint32_t size){
    if(fm_count>=FM_MAX) return;
    strncpy(fm_names[fm_count], name, 31);
    fm_dirs[fm_count]  = is_dir;
    fm_sizes[fm_count] = size;
    fm_count++;
}

static void fm_draw(int sel, int scroll){
    theme_t *th=ui_theme();
    vga_clear(); ui_draw_taskbar("File Manager");
    ui_draw_window(2,2,76,20,"File Manager");

    /* Path breadcrumb */
    vga_puts_at(fm_cwd[0]?fm_cwd:"/", 5, 3, VGA_LIGHT_CYAN, VGA_BLACK);

    /* Back button — visible and tappable when inside a subdir */
    if(fm_cwd[0]){
        vga_puts_at(" Back ", 69, 3, th->bar_bg, th->accent);
    } else {
        vga_puts_at("      ", 69, 3, VGA_BLACK, VGA_BLACK);
    }

    /* Column headers */
    vga_puts_at("Name",                        5, 4, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_at("Type",                        34,4, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_at("Size",                        42,4, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_at("-------------------------------",5,5,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("----",                        34,5, VGA_DARK_GREY, VGA_BLACK);
    vga_puts_at("--------",                    42,5, VGA_DARK_GREY, VGA_BLACK);

    /* Show ".." entry if not at root */
    int row_offset = 0;
    if(fm_cwd[0]){
        int is_sel = (sel == -1);
        vga_color_t bg  = is_sel ? th->accent   : VGA_BLACK;
        vga_color_t fg  = is_sel ? th->win_bg   : VGA_LIGHT_CYAN;
        vga_puts_at("..                         ", 5, 6, fg, bg);
        vga_puts_at("UP  ", 34, 6, is_sel?th->win_bg:VGA_CYAN, bg);
        row_offset = 1;
    }

    /* List — rows 6+offset to 19 */
    const int VISIBLE = fm_cwd[0] ? 13 : 14;
    for(int i=0;i<VISIBLE;i++){
        int idx=scroll+i;
        int vy=6+row_offset+i;
        vga_puts_at("                                                                          ",4,vy,VGA_BLACK,VGA_BLACK);
        if(idx>=fm_count) continue;
        int s=(idx==sel);
        vga_color_t bg=s?th->accent:VGA_BLACK;
        vga_color_t nfg=s?th->win_bg:(fm_dirs[idx]?VGA_LIGHT_CYAN:VGA_LIGHT_GREEN);
        vga_color_t tfg=s?th->win_bg:VGA_CYAN;
        vga_color_t sfg=s?th->win_bg:VGA_YELLOW;
        char nm[28]; int nl=strlen(fm_names[idx]);
        for(int j=0;j<27;j++) nm[j]=j<nl?fm_names[idx][j]:' '; nm[27]=0;
        vga_puts_at(nm,5,vy,nfg,bg);
        vga_puts_at(fm_dirs[idx]?"DIR  ":"file ",34,vy,tfg,bg);
        if(!fm_dirs[idx]){
            char sb[12]; utoa(fm_sizes[idx],sb,10); strcat(sb," B");
            vga_puts_at(sb,42,vy,sfg,bg);
        }
    }

    if(fm_count>VISIBLE){
        char sb[8]; utoa(sel+1,sb,10); strcat(sb,"/");
        char sc2[8]; utoa(fm_count,sc2,10); strcat(sb,sc2);
        vga_puts_at(sb,66,21,VGA_DARK_GREY,VGA_BLACK);
    }
    if(fm_count==0 && !fm_cwd[0])
        vga_puts_at("(empty)",30,12,VGA_DARK_GREY,VGA_BLACK);
    else if(fm_count==0)
        vga_puts_at("(empty directory)",28,12,VGA_DARK_GREY,VGA_BLACK);

    vga_puts_at("Tap=Open  Del=Delete  Bksp=Up  ESC=Close  Mouse supported",5,21,VGA_DARK_GREY,VGA_BLACK);
}

static void app_filemanager(void){
    int sel=0, scroll=0;
    fm_cwd[0]=0;   /* always start at root */
    const int VISIBLE=14;

reload:
    fm_count=0;
    vfs_list_dir(fm_cwd, fm_collect_cb);
    if(sel>=fm_count) sel=fm_count>0?fm_count-1:0;
    if(sel<0) sel=0;

    while(1){
        int effective_visible = fm_cwd[0] ? VISIBLE-1 : VISIBLE;
        int row_offset        = fm_cwd[0] ? 1 : 0;
        if(sel<scroll) scroll=sel;
        if(sel>=scroll+effective_visible) scroll=sel-effective_visible+1;

        fm_draw(sel, scroll);
        if(mouse_is_ready()) ui_draw_cursor();
        vga_flush();

        /* ── Input poll loop — keyboard OR mouse ──────────────── */
        int c=-1;
        while(c==-1){
            /* Mouse: cursor tracking + click */
            if(mouse_is_ready()){
                int mx,my,mb; mouse_get(&mx,&my,&mb);
                static int flm_lx=-1,flm_ly=-1;
                if(mx!=flm_lx||my!=flm_ly){ ui_draw_cursor(); flm_lx=mx;flm_ly=my; }

                int mc=mouse_get_clicks();
                if(mc&1){
                    /* Back button: cols 69-74, row 3, only when in a subdir */
                    if(fm_cwd[0] && my==3 && mx>=69 && mx<=74){
                        c=8; /* go up */
                    }
                    /* ".." row (row 6 when inside a subdir) */
                    else if(fm_cwd[0] && my==6 && mx>=3 && mx<77){
                        c=8; /* treat as Backspace → go up */
                    } else {
                        /* File list: rows 6+row_offset .. 6+row_offset+effective_visible-1 */
                        int list_y0=6+row_offset;
                        int idx=(my-list_y0)+scroll;
                        if(my>=list_y0 && my<list_y0+effective_visible &&
                           mx>=3 && mx<77 &&
                           idx>=0 && idx<fm_count){
                            sel=idx;
                            c='\n'; /* single-tap → open/navigate */
                        }
                    }
                }
            }
            /* Non-blocking keyboard */
            if(c==-1){ int k=keyboard_getchar(); if(k!=-1) c=k; }
            __asm__ volatile("pause");
        }
        /* ─────────────────────────────────────────────────────── */

        if(c==27) { fm_cwd[0]=0; return; }

        /* Backspace or left arrow = go up */
        if((c==8||c==0x14b) && fm_cwd[0]){
            int l=(int)strlen(fm_cwd);
            while(l>0&&fm_cwd[l-1]!='/') l--;
            if(l>0) l--;
            fm_cwd[l]=0;
            sel=0; scroll=0;
            goto reload;
        }

        if(c==0x148){ if(sel>0) sel--; else if(fm_cwd[0]){ } continue; }
        if(c==0x150 && sel<fm_count-1) { sel++; continue; }
        if(c==0x149) { sel-=effective_visible; if(sel<0)sel=0; continue; }
        if(c==0x151) { sel+=effective_visible; if(sel>=fm_count)sel=fm_count-1; continue; }

        /* Enter — also triggered by mouse single-tap via c='\n' */
        if(c=='\n'||c=='\r'){
            if(fm_dirs[sel]){
                char newpath[64];
                if(fm_cwd[0]){
                    strncpy(newpath,fm_cwd,47); strcat(newpath,"/");
                    int dl=strlen(newpath);
                    strncpy(newpath+dl,fm_names[sel],63-dl);
                } else {
                    strncpy(newpath,fm_names[sel],63);
                }
                strncpy(fm_cwd,newpath,63); fm_cwd[63]=0;
                sel=0; scroll=0;
                goto reload;
            } else if(fm_count>0){
                char fullpath[64];
                if(fm_cwd[0]){
                    strncpy(fullpath,fm_cwd,47); strcat(fullpath,"/");
                    int dl=strlen(fullpath);
                    strncpy(fullpath+dl,fm_names[sel],63-dl);
                } else {
                    strncpy(fullpath,fm_names[sel],63);
                }
                app_texteditor_load(fullpath);
                goto reload;
            }
            continue;
        }

        /* Delete — files only, not dirs */
        if((c==127||c=='d'||c=='D') && fm_count>0 && !fm_dirs[sel]){
            vga_puts_at("  Delete this file? Y/N  ",20,21,VGA_BLACK,VGA_LIGHT_RED);
            int k=keyboard_waitchar();
            if(k=='y'||k=='Y'){
                char fullpath[64];
                if(fm_cwd[0]){
                    strncpy(fullpath,fm_cwd,47); strcat(fullpath,"/");
                    int dl=strlen(fullpath);
                    strncpy(fullpath+dl,fm_names[sel],63-dl);
                } else {
                    strncpy(fullpath,fm_names[sel],63);
                }
                int del_rc = vfs_delete(fullpath);
                if(del_rc<0){
                    vga_puts_at("  DELETE FAILED - file still present  ",20,21,VGA_WHITE,VGA_RED);
                    sleep_ms(1600);
                } else {
                    if(sel>=fm_count-1 && sel>0) sel--;
                }
            }
            goto reload;
        }
    }
}

static void app_sysinfo(void){
    vga_clear(); ui_draw_taskbar("System Info");
    ui_draw_window(6,2,68,19,"System Information");
    int y=4;
    #define ROW(l,v,c) vga_puts_at(l,10,y,VGA_LIGHT_GREY,VGA_BLACK);vga_puts_at(v,32,y,c,VGA_BLACK);y++;
    char rb[24],tb[24],up[24];
    ROW("OS:",          "HavenDOS v0.7.4",        VGA_LIGHT_CYAN);
    ROW("Architecture:","x86 32-bit Protected",  VGA_WHITE);
    ROW("Bootloader:",  "GRUB2 Multiboot1",       VGA_WHITE);
    ROW("Display:",     "80x25 VGA Text Mode",    VGA_WHITE);
    y++;
    uint32_t f=pmm_free_blocks(),t=pmm_total_blocks();
    utoa(f*4,rb,10); strcat(rb," KB free");
    utoa(t*4,tb,10); strcat(tb," KB total");
    ROW("Free RAM:",    rb, VGA_LIGHT_GREEN);
    ROW("Total RAM:",   tb, VGA_YELLOW);
    utoa(get_ticks()/100,up,10); strcat(up,"s");
    ROW("Uptime:",      up, VGA_WHITE);
    y++;
    ROW("User:",        current_username(),  VGA_LIGHT_GREEN);
    ROW("Admin:",       current_user_admin()?"Yes":"No", current_user_admin()?VGA_LIGHT_GREEN:VGA_LIGHT_GREY);
    ROW("Drivers:",     "VGA, PS/2 KB+Mouse",VGA_WHITE);
    ROW("Filesystem:",  "RAMFS (64 slots, dirs supported)",  VGA_WHITE);
    vga_puts_at("Any key...",34,20,VGA_DARK_GREY,VGA_BLACK);
    keyboard_waitchar();
}

/* System Monitor - live updating */
static void app_sysmonitor(void){
    vga_clear(); ui_draw_taskbar("System Monitor");
    ui_draw_window(4,2,72,20,"System Monitor  [ESC=Close]");
    vga_puts_at("CPU Activity",8,4,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at("Memory",8,9,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at("Uptime",8,14,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at("Ticks",44,4,VGA_LIGHT_CYAN,VGA_BLACK);

    int bar_pos=0;
    while(1){
        if(keyboard_haschar()&&keyboard_getchar()==27)return;

        /* Fake CPU activity bar (animates) */
        bar_pos=(bar_pos+1)%60;
        vga_puts_at("                                                    ",8,6,VGA_DARK_GREY,VGA_BLACK);
        for(int i=0;i<bar_pos;i++)
            vga_putchar_at('#',8+i,6,
                i<20?VGA_LIGHT_GREEN:i<45?VGA_YELLOW:VGA_RED,VGA_BLACK);

        /* Memory bar */
        uint32_t f=pmm_free_blocks(),t=pmm_total_blocks();
        int used=t>0?(int)((t-f)*60/t):0;
        for(int i=0;i<60;i++)
            vga_putchar_at('#',8+i,11,
                i<used?(i<40?VGA_LIGHT_GREEN:VGA_YELLOW):VGA_DARK_GREY,VGA_BLACK);
        char rb[16],tb[16];
        utoa(f*4,rb,10); strcat(rb,"KB free");
        utoa(t*4,tb,10); strcat(tb,"KB total");
        vga_puts_at("                    ",8,12,VGA_BLACK,VGA_BLACK);
        vga_puts_at(rb,8,12,VGA_LIGHT_GREEN,VGA_BLACK);
        vga_puts_at(" / ",8+(int)strlen(rb),12,VGA_LIGHT_GREY,VGA_BLACK);
        vga_puts_at(tb,11+(int)strlen(rb),12,VGA_YELLOW,VGA_BLACK);

        /* Uptime */
        uint32_t ticks=get_ticks();
        char up[32];
        uint32_t secs=ticks/100,mins=secs/60,hrs=mins/60;
        secs%=60;mins%=60;
        up[0]='0'+hrs/10;up[1]='0'+hrs%10;up[2]=':';
        up[3]='0'+mins/10;up[4]='0'+mins%10;up[5]=':';
        up[6]='0'+secs/10;up[7]='0'+secs%10;up[8]=0;
        vga_puts_at(up,8,15,VGA_WHITE,VGA_BLACK);

        /* Tick counter */
        char tc[16]; utoa(ticks,tc,10);
        vga_puts_at("          ",44,6,VGA_BLACK,VGA_BLACK);
        vga_puts_at(tc,44,6,VGA_LIGHT_CYAN,VGA_BLACK);

        sleep_ms(100);
    }
}

static void app_clock(void){
    vga_clear(); ui_draw_taskbar("Clock");
    ui_draw_window(22,5,36,14,"Clock");
    vga_puts_at("ESC to close",33,19,VGA_DARK_GREY,VGA_BLACK);
    while(1){
        uint8_t h,m,s; rtc_get_time(&h,&m,&s);
        /* HH:MM:SS */
        char buf[9];
        buf[0]='0'+h/10;buf[1]='0'+h%10;buf[2]=':';
        buf[3]='0'+m/10;buf[4]='0'+m%10;buf[5]=':';
        buf[6]='0'+s/10;buf[7]='0'+s%10;buf[8]=0;
        vga_puts_at(buf,31,9,VGA_LIGHT_CYAN,VGA_BLACK);
        /* Blinking separator (safe char: dot) */
        vga_putchar_at(s%2?'.':' ',31+8,9,VGA_YELLOW,VGA_BLACK);
        /* Uptime from ticks */
        uint32_t up=get_ticks()/100;
        char us[12];
        us[0]='0'+(up/3600/10)%10;us[1]='0'+(up/3600)%10;us[2]=':';
        us[3]='0'+((up%3600)/60/10)%10;us[4]='0'+((up%3600)/60)%10;us[5]=':';
        us[6]='0'+((up%60)/10)%10;us[7]='0'+(up%60)%10;us[8]=0;
        vga_puts_at("Uptime:",26,12,VGA_DARK_GREY,VGA_BLACK);
        vga_puts_at(us,34,12,VGA_DARK_GREY,VGA_BLACK);
        vga_puts_at("Time from CMOS RTC",26,14,VGA_DARK_GREY,VGA_BLACK);
        if(keyboard_haschar()&&keyboard_getchar()==27)return;
        sleep_ms(500);
    }
}

/* Reminder storage now in reminders_save/load — see below */
/* ═══════════════════════════════════════════════════════════════
   REMINDERS — persistent, due-date aware, past-due alerts
   File format (.reminders): one line per reminder:
     STATUS|DUE_YYYY-MM-DD HH:MM|TEXT
   STATUS: 0=pending 1=done
   DUE: "0000-00-00 00:00" = no due date
   ═══════════════════════════════════════════════════════════════ */
#define REM_MAX  24
#define REM_TEXT 56

typedef struct {
    char text[REM_TEXT];
    int  done;
    /* due date (0 = not set) */
    int  due_y, due_mo, due_d;
    int  due_h, due_min;
} reminder_t;

static reminder_t reminders[REM_MAX];
static int        rem_count = 0;
static int        rem_alerted[REM_MAX]; /* 1 = alert already fired this session */

#define REM_FILE ".reminders"

void reminders_save(void){
    /* Format each reminder as one text line */
    char buf[REM_MAX * 80 + 4];
    int  bp = 0;
    for(int i = 0; i < rem_count; i++){
        reminder_t *r = &reminders[i];
        /* STATUS */
        buf[bp++] = r->done ? '1' : '0';
        buf[bp++] = '|';
        /* DUE: YYYY-MM-DD HH:MM */
        char tmp[6];
        /* year */
        utoa((uint32_t)(r->due_y), tmp, 10);
        if(r->due_y < 1000){ buf[bp++]='0'; }
        if(r->due_y < 100 ) { buf[bp++]='0'; }
        if(r->due_y < 10  ) { buf[bp++]='0'; }
        for(int j=0;tmp[j];j++) buf[bp++]=tmp[j];
        buf[bp++]='-';
        if(r->due_mo<10) buf[bp++]='0';
        utoa((uint32_t)r->due_mo,tmp,10); for(int j=0;tmp[j];j++) buf[bp++]=tmp[j];
        buf[bp++]='-';
        if(r->due_d<10) buf[bp++]='0';
        utoa((uint32_t)r->due_d,tmp,10);  for(int j=0;tmp[j];j++) buf[bp++]=tmp[j];
        buf[bp++]=' ';
        if(r->due_h<10) buf[bp++]='0';
        utoa((uint32_t)r->due_h,tmp,10);  for(int j=0;tmp[j];j++) buf[bp++]=tmp[j];
        buf[bp++]=':';
        if(r->due_min<10) buf[bp++]='0';
        utoa((uint32_t)r->due_min,tmp,10);for(int j=0;tmp[j];j++) buf[bp++]=tmp[j];
        buf[bp++]='|';
        /* TEXT */
        for(int j=0;r->text[j]&&j<REM_TEXT-1;j++) buf[bp++]=r->text[j];
        buf[bp++]='\n';
    }
    buf[bp]=0;
    vfs_write(REM_FILE, buf, (uint32_t)bp);
}

void reminders_load(void){
    char buf[REM_MAX * 80 + 4];
    int n = vfs_read(REM_FILE, buf, sizeof(buf)-1);
    if(n <= 0) return;
    buf[n] = 0;
    rem_count = 0;
    char *line = buf;
    while(*line && rem_count < REM_MAX){
        /* Find end of line */
        char *end = line;
        while(*end && *end != '\n') end++;
        char saved = *end; *end = 0;

        reminder_t *r = &reminders[rem_count];
        memset(r, 0, sizeof(*r));
        rem_alerted[rem_count] = 0;

        /* Parse: STATUS|DUE|TEXT */
        char *p = line;
        r->done = (*p == '1') ? 1 : 0;
        p++;
        if(*p == '|') p++;
        /* DUE: YYYY-MM-DD HH-MM */
        r->due_y   = atoi(p); p+=5;
        r->due_mo  = atoi(p); p+=3;
        r->due_d   = atoi(p); p+=3;
        r->due_h   = atoi(p); p+=3;
        r->due_min = atoi(p); p+=3;
        if(*p == '|') p++;
        /* TEXT */
        int j=0;
        while(*p && j < REM_TEXT-1) r->text[j++] = *p++;
        r->text[j] = 0;

        if(r->text[0]) rem_count++;

        *end = saved;
        line = end;
        if(*line == '\n') line++;
    }
}

/* Check for past-due reminders — called every real second from desktop loop */
void reminders_check(void){
    if(rem_count == 0) return;
    uint8_t d,mo,h,m,s; uint16_t yr16;
    rtc_get_date(&d,&mo,&yr16);
    rtc_get_time(&h,&m,&s);
    int yr = (int)yr16;

    for(int i=0;i<rem_count;i++){
        reminder_t *r = &reminders[i];
        if(r->done || rem_alerted[i]) continue;
        if(r->due_y == 0) continue; /* no due date set */
        /* Past-due? */
        int past = 0;
        if(yr > r->due_y) past=1;
        else if(yr == r->due_y){
            if((int)mo > r->due_mo) past=1;
            else if((int)mo == r->due_mo){
                if((int)d > r->due_d) past=1;
                else if((int)d == r->due_d){
                    if((int)h > r->due_h) past=1;
                    else if((int)h == r->due_h && (int)m >= r->due_min) past=1;
                }
            }
        }
        if(past){
            rem_alerted[i] = 1;
            char alert[72]; strcpy(alert,"DUE: "); strcat(alert,r->text);
            toast_warning(alert);
            notif_push(alert);
        }
    }
}

/* Parse "YYYY-MM-DD HH:MM" from input string */
static int parse_due(const char *s, int *y, int *mo, int *d, int *h, int *mi){
    /* Accept: YYYY-MM-DD or YYYY-MM-DD HH:MM */
    if(strlen(s) < 10) return 0;
    *y  = atoi(s);
    *mo = atoi(s+5);
    *d  = atoi(s+8);
    *h  = 0; *mi = 0;
    if(strlen(s) >= 16){
        *h  = atoi(s+11);
        *mi = atoi(s+14);
    }
    if(*y < 2024 || *mo < 1 || *mo > 12 || *d < 1 || *d > 31) return 0;
    return 1;
}

static void app_todo(void){
    int sel=0;
    while(1){
        vga_clear(); ui_draw_taskbar("Reminders");
        ui_draw_window(10,2,60,20,"Reminders  -  A=Add  D=Delete  Space=Done  ESC=Close");

        /* Column headers */
        vga_puts_at("  Done  Task                              Due            ",11,4,VGA_LIGHT_CYAN,VGA_BLACK);
        for(int x=11;x<71;x++) vga_putchar_at('-',x,5,VGA_DARK_GREY,VGA_BLACK);

        for(int i=0;i<rem_count&&i<13;i++){
            reminder_t *r=&reminders[i];
            int s=(i==sel);
            vga_color_t fg=s?VGA_BLACK:(r->done?VGA_DARK_GREY:VGA_WHITE);
            vga_color_t bg=s?VGA_LIGHT_GREY:VGA_BLACK;

            /* Selection indicator */
            vga_putchar_at(s?'>':' ',11,6+i,fg,bg);
            /* Done marker */
            vga_putchar_at(r->done?'X':' ',13,6+i,r->done?VGA_DARK_GREY:VGA_LIGHT_GREEN,bg);
            vga_putchar_at(' ',14,6+i,fg,bg);
            /* Text (36 chars) */
            char row[37]; int j=0;
            const char *t=r->text;
            while(*t&&j<36)row[j++]=*t++;
            while(j<36)row[j++]=' '; row[36]=0;
            vga_puts_at(row,15,6+i,fg,bg);
            /* Due date */
            if(r->due_y>0){
                char due[18];
                char tmp[8];
                /* YYYY-MM-DD */
                utoa((uint32_t)r->due_y,tmp,10); strcpy(due,tmp); strcat(due,"-");
                if(r->due_mo<10) strcat(due,"0");
                utoa((uint32_t)r->due_mo,tmp,10); strcat(due,tmp); strcat(due,"-");
                if(r->due_d<10) strcat(due,"0");
                utoa((uint32_t)r->due_d,tmp,10); strcat(due,tmp);
                /* Check past-due for colour */
                vga_color_t dc=rem_alerted[i]?VGA_LIGHT_RED:VGA_YELLOW;
                vga_puts_at(due,52,6+i,r->done?VGA_DARK_GREY:dc,bg);
            } else {
                vga_puts_at("          ",52,6+i,VGA_DARK_GREY,bg);
            }
        }
        if(!rem_count)vga_puts_at("(empty - press A to add a reminder)",18,10,VGA_DARK_GREY,VGA_BLACK);

        /* Hint bar */
        vga_puts_at("A=Add  D=Delete  Space=Toggle done  Up-Down=Select  S=Save",11,20,VGA_DARK_GREY,VGA_BLACK);

        int c=keyboard_waitchar();
        if(c==27){ reminders_save(); return; }
        if(c==0x148&&sel>0) sel--;
        if(c==0x150&&sel<rem_count-1) sel++;
        if(c==' '&&rem_count>0){
            reminders[sel].done=!reminders[sel].done;
            if(reminders[sel].done) rem_alerted[sel]=1;
            reminders_save();
        }
        if((c=='s'||c=='S')) reminders_save();

        if((c=='a'||c=='A')&&rem_count<REM_MAX){
            /* Add new reminder */
            vga_puts_at("Task: ",11,21,VGA_WHITE,VGA_BLACK);
            char tbuf[REM_TEXT]=""; int tp=0; vga_set_cursor(17,21);
            for(int x=17;x<68;x++) vga_putchar_at('_',x,21,VGA_DARK_GREY,VGA_BLACK);
            vga_set_cursor(17,21);
            while(1){int k=keyboard_waitchar();
                if(k=='\n'||k==27)break;
                if(k=='\b'&&tp>0){tp--;tbuf[tp]=0;
                    vga_putchar_at('_',16+tp,21,VGA_DARK_GREY,VGA_BLACK);
                    vga_set_cursor(17+tp,21);}
                else if(k>=32&&k<127&&tp<REM_TEXT-2){
                    tbuf[tp++]=k;tbuf[tp]=0;
                    vga_putchar_at(k,16+tp,21,VGA_WHITE,VGA_BLACK);
                    vga_set_cursor(17+tp,21);}
            }
            if(!tbuf[0]) continue;

            /* Ask for due date */
            vga_puts_at("Due (YYYY-MM-DD HH:MM or blank): ",11,22,VGA_WHITE,VGA_BLACK);
            char dbuf[20]=""; int dp=0; vga_set_cursor(44,22);
            for(int x=44;x<65;x++) vga_putchar_at('_',x,22,VGA_DARK_GREY,VGA_BLACK);
            vga_set_cursor(44,22);
            while(1){int k=keyboard_waitchar();
                if(k=='\n'||k==27)break;
                if(k=='\b'&&dp>0){dp--;dbuf[dp]=0;
                    vga_putchar_at('_',43+dp,22,VGA_DARK_GREY,VGA_BLACK);
                    vga_set_cursor(44+dp,22);}
                else if(k>=32&&k<127&&dp<18){
                    dbuf[dp++]=k;dbuf[dp]=0;
                    vga_putchar_at(k,43+dp,22,VGA_WHITE,VGA_BLACK);
                    vga_set_cursor(44+dp,22);}
            }

            reminder_t *r=&reminders[rem_count];
            memset(r,0,sizeof(*r));
            strncpy(r->text,tbuf,REM_TEXT-1);
            r->done=0;
            parse_due(dbuf,&r->due_y,&r->due_mo,&r->due_d,&r->due_h,&r->due_min);
            rem_alerted[rem_count]=0;
            rem_count++;
            reminders_save();
        }

        if((c=='d'||c=='D')&&rem_count>0){
            for(int i=sel;i<rem_count-1;i++){
                reminders[i]=reminders[i+1];
                rem_alerted[i]=rem_alerted[i+1];
            }
            rem_count--;
            if(sel>=rem_count&&sel>0) sel--;
            reminders_save();
        }
    }
}

static void read_str(char *buf, int max, int x, int y){
    int pos=0; buf[0]=0; vga_set_cursor(x,y);
    /* clear field */
    for(int i=0;i<max;i++) vga_putchar_at('_',x+i,y,VGA_DARK_GREY,VGA_BLACK);
    vga_set_cursor(x,y);
    while(1){
        int c=keyboard_waitchar();
        if(c=='\n'||c=='\r') break;
        if(c==27){buf[0]=0;break;}
        if((c=='\b'||c==127)&&pos>0){pos--;buf[pos]=0;
            vga_putchar_at('_',x+pos,y,VGA_DARK_GREY,VGA_BLACK);
            vga_set_cursor(x+pos,y);}
        else if(c>=32&&c<127&&pos<max-1){
            buf[pos++]=c;buf[pos]=0;
            vga_putchar_at(c,x+pos-1,y,VGA_WHITE,VGA_BLACK);
        }
    }
}

static void app_settings(void){
    extern int         cfg_autologin(void);
    extern void        cfg_set_autologin(int);
    extern const char *cfg_autologin_user(void);
    extern void        cfg_set_autologin_user(const char*);
    extern int         cfg_verbose_boot(void);
    extern void        cfg_set_verbose_boot(int);
    extern int         cfg_post_screen(void);
    extern void        cfg_set_post_screen(int);
    extern int         cfg_screensaver_timeout(void);
    extern void        cfg_set_screensaver_timeout(int);
    extern int         cfg_autologin_timeout(void);
    extern void        cfg_set_autologin_timeout(int);
    extern const char *cfg_hostname(void);
    extern void        cfg_set_hostname(const char*);
    extern int         cfg_acct_hidden(int);
    extern void        cfg_set_acct_hidden(int,int);
    extern void        cfg_save(void);
    extern uint32_t    pmm_free_blocks(void);
    extern uint32_t    pmm_total_blocks(void);

    /* Tabs: Themes | Accounts | Boot | System | About */
    const char *tabs[]={"Themes","Accounts","Boot","System","About"};
    int ntabs=5, tab=0, sel=0;

    while(1){
        vga_clear(); ui_draw_taskbar("Settings");
        ui_draw_window(4,2,72,21,"Settings  v0.7.4");

        /* Tab bar */
        int tx=6;
        for(int t=0;t<ntabs;t++){
            int active=(t==tab);
            vga_putchar_at(' ',tx,4,active?VGA_BLACK:VGA_DARK_GREY,active?VGA_CYAN:VGA_BLACK);
            vga_puts_at(tabs[t],tx+1,4,active?VGA_BLACK:VGA_LIGHT_GREY,active?VGA_CYAN:VGA_BLACK);
            vga_putchar_at(' ',tx+1+(int)strlen(tabs[t]),4,active?VGA_BLACK:VGA_DARK_GREY,active?VGA_CYAN:VGA_BLACK);
            tx+=2+(int)strlen(tabs[t])+1;
        }
        for(int x=6;x<75;x++) vga_putchar_at('-',x,5,VGA_DARK_GREY,VGA_BLACK);
        vga_puts_at("Tab=Next tab  Up-Down=Select  Enter=Apply  ESC=Close",6,22,VGA_DARK_GREY,VGA_BLACK);

        /* ── Tab 0: Themes ── */
        if(tab==0){
            /* BUG FIX: cap visible rows to 12 so we never draw past row 20.
               Use scroll offset so all themes are accessible via Up/Down. */
            #define THEME_VISIBLE 12
            static int theme_scroll = 0;
            int tc=ui_theme_count();
            /* Clamp scroll so sel is always visible */
            if(sel < theme_scroll) theme_scroll = sel;
            if(sel >= theme_scroll + THEME_VISIBLE) theme_scroll = sel - THEME_VISIBLE + 1;
            if(theme_scroll < 0) theme_scroll = 0;

            vga_puts_at("Select desktop theme:",7,6,VGA_WHITE,VGA_BLACK);
            if(theme_scroll > 0)
                vga_puts_at("^ more",62,6,VGA_DARK_GREY,VGA_BLACK);
            else
                vga_puts_at("      ",62,6,VGA_BLACK,VGA_BLACK);

            for(int i=0;i<THEME_VISIBLE;i++){
                int ti = i + theme_scroll;
                if(ti >= tc) break;
                int s=(ti==sel);
                vga_putchar_at(s?'>':' ',7,8+i,s?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);
                /* Pad to clear old text */
                char padded[20]; int pi=0;
                const char *nm=ui_theme_name(ti);
                while(*nm&&pi<19) padded[pi++]=*nm++;
                while(pi<19) padded[pi++]=' ';
                padded[pi]=0;
                vga_puts_at(padded,9,8+i,s?VGA_WHITE:VGA_LIGHT_GREY,VGA_BLACK);
            }
            if(theme_scroll + THEME_VISIBLE < tc)
                vga_puts_at("v more",62,8+THEME_VISIBLE-1,VGA_DARK_GREY,VGA_BLACK);

            /* Preview label — always at fixed row 21 inside window */
            vga_puts_at("Preview: ",7,21,VGA_DARK_GREY,VGA_BLACK);
            char pnm[20]; int pi2=0;
            const char *pn=ui_theme_name(sel);
            while(*pn&&pi2<19) pnm[pi2++]=*pn++;
            while(pi2<19) pnm[pi2++]=' ';
            pnm[pi2]=0;
            vga_puts_at(pnm,16,21,ui_theme()->accent,VGA_BLACK);

            int c=keyboard_waitchar();
            if(c==27){ ui_set_theme(ui_current_theme()); return; }
            if(c=='\t'){ui_set_theme(ui_current_theme());tab=(tab+1)%ntabs;sel=0;theme_scroll=0;continue;}
            if(c==0x148&&sel>0){ sel--; ui_load_preview(sel); }
            if(c==0x150&&sel<tc-1){ sel++; ui_load_preview(sel); }
            if(c=='\n'||c=='\r'){ ui_set_theme(sel); }
        }

        /* ── Tab 1: Accounts ── */
        else if(tab==1){
            vga_puts_at("Account visibility on login screen:",7,6,VGA_WHITE,VGA_BLACK);
            /* Built-in accounts: indices 0,1,2 = admin,bootos,guest */
            const char *bnames[]={"admin","bootos","guest"};
            for(int i=0;i<3;i++){
                int s=(sel==i);
                int hidden=cfg_acct_hidden(i);
                vga_putchar_at(s?'>':' ',7,8+i,s?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);
                vga_puts_at(bnames[i],9,8+i,VGA_WHITE,VGA_BLACK);
                vga_puts_at(hidden?"  [HIDDEN]  ":"  [VISIBLE] ",17,8+i,
                            hidden?VGA_YELLOW:VGA_LIGHT_GREEN,VGA_BLACK);
                vga_puts_at("Enter to toggle",30,8+i,VGA_DARK_GREY,VGA_BLACK);
            }

            for(int x=6;x<75;x++) vga_putchar_at('-',x,12,VGA_DARK_GREY,VGA_BLACK);

            /* Auto-login */
            int al=cfg_autologin();
            vga_puts_at("Auto-login:",7,13,VGA_WHITE,VGA_BLACK);
            vga_puts_at(al?"ENABLED ":"DISABLED",19,13,al?VGA_LIGHT_GREEN:VGA_YELLOW,VGA_BLACK);
            vga_puts_at("(sel row 3 + Enter to toggle)",29,13,VGA_DARK_GREY,VGA_BLACK);

            if(al){
                vga_puts_at("Auto-login user:",7,14,VGA_WHITE,VGA_BLACK);
                vga_puts_at(cfg_autologin_user(),24,14,VGA_LIGHT_CYAN,VGA_BLACK);
            }

            vga_puts_at("Note: hidden accounts can still log in via shell.",7,16,VGA_DARK_GREY,VGA_BLACK);
            vga_puts_at("Wizard-created accounts are always visible.",7,17,VGA_DARK_GREY,VGA_BLACK);

            int c=keyboard_waitchar();
            if(c==27)return;
            if(c=='\t'){tab=(tab+1)%ntabs;sel=0;continue;}
            if(c==0x148&&sel>0)sel--;
            if(c==0x150&&sel<3)sel++;
            if((c=='\n'||c=='\r')&&sel<3){
                cfg_set_acct_hidden(sel,!cfg_acct_hidden(sel));
                cfg_save();
            }
            if((c=='\n'||c=='\r')&&sel==3){
                cfg_set_autologin(!cfg_autologin());
                cfg_save();
            }
        }

        /* ── Tab 2: Boot ── */
        else if(tab==2){
            vga_puts_at("Boot configuration:",7,6,VGA_WHITE,VGA_BLACK);

            typedef struct{const char *lbl; int val;} bopt;
            bopt bopts[]={
                {"POST screen on boot",  cfg_post_screen()},
                {"Verbose boot log",     cfg_verbose_boot()},
            };
            int nbopts=2;
            for(int i=0;i<nbopts;i++){
                int s=(i==sel);
                vga_putchar_at(s?'>':' ',7,8+i,s?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);
                vga_puts_at(bopts[i].lbl,9,8+i,VGA_WHITE,VGA_BLACK);
                vga_puts_at(bopts[i].val?"[ON] ":"[OFF]",40,8+i,
                            bopts[i].val?VGA_LIGHT_GREEN:VGA_YELLOW,VGA_BLACK);
            }
            vga_puts_at("Changes take effect on next boot.",7,14,VGA_DARK_GREY,VGA_BLACK);

            int c=keyboard_waitchar();
            if(c==27)return;
            if(c=='\t'){tab=(tab+1)%ntabs;sel=0;continue;}
            if(c==0x148&&sel>0)sel--;
            if(c==0x150&&sel<nbopts-1)sel++;
            if(c=='\n'||c=='\r'){
                if(sel==0){cfg_set_post_screen(!cfg_post_screen());cfg_save();}
                if(sel==1){cfg_set_verbose_boot(!cfg_verbose_boot());cfg_save();}
            }
        }

        /* ── Tab 3: System ── */
        else if(tab==3){
            vga_puts_at("System settings:",7,6,VGA_WHITE,VGA_BLACK);
            vga_puts_at("Hostname:",7,8,VGA_WHITE,VGA_BLACK);
            vga_puts_at(cfg_hostname(),18,8,VGA_LIGHT_CYAN,VGA_BLACK);
            vga_puts_at("Screensaver timeout (seconds):",7,10,VGA_WHITE,VGA_BLACK);
            char tsbuf[8]; itoa(cfg_screensaver_timeout(),tsbuf,10);
            vga_puts_at(tsbuf,38,10,VGA_LIGHT_CYAN,VGA_BLACK);
            vga_puts_at("Auto-login delay  (seconds):",7,12,VGA_WHITE,VGA_BLACK);
            char albuf[8]; itoa(cfg_autologin_timeout(),albuf,10);
            vga_puts_at(albuf,36,12,VGA_LIGHT_CYAN,VGA_BLACK);
            vga_puts_at("(0=instant, any key cancels countdown)",7,13,VGA_DARK_GREY,VGA_BLACK);
            vga_puts_at("H=Hostname  T=Screensaver  A=Auto-login delay",7,18,VGA_DARK_GREY,VGA_BLACK);

            int c=keyboard_waitchar();
            if(c==27)return;
            if(c=='\t'){tab=(tab+1)%ntabs;sel=0;continue;}
            if(c=='h'||c=='H'){
                vga_puts_at("New hostname: ",7,15,VGA_WHITE,VGA_BLACK);
                char hn[24]="";
                read_str(hn,23,22,15);
                if(hn[0]){cfg_set_hostname(hn);cfg_save();}
            }
            if(c=='t'||c=='T'){
                vga_puts_at("Screensaver timeout (sec): ",7,15,VGA_WHITE,VGA_BLACK);
                char ts[8]="";
                read_str(ts,7,34,15);
                if(ts[0]){cfg_set_screensaver_timeout(atoi(ts));cfg_save();}
            }
            if(c=='a'||c=='A'){
                vga_puts_at("Auto-login delay (sec, 0=instant): ",7,15,VGA_WHITE,VGA_BLACK);
                char als[8]="";
                read_str(als,7,42,15);
                if(als[0]){cfg_set_autologin_timeout(atoi(als));cfg_save();}
            }
        }

        /* ── Tab 4: About ── */
        else if(tab==4){
            vga_puts_at("HavenDOS v0.7.4",7,6,VGA_LIGHT_CYAN,VGA_BLACK);
            vga_puts_at("TechHaven Studios  2026",7,7,VGA_DARK_GREY,VGA_BLACK);
            for(int x=6;x<75;x++) vga_putchar_at('-',x,9,VGA_DARK_GREY,VGA_BLACK);

            uint32_t free_mb=pmm_free_blocks()*4/1024;
            uint32_t total_mb=pmm_total_blocks()*4/1024;
            char fb[8]; utoa(free_mb,fb,10);
            char tb2[8]; utoa(total_mb,tb2,10);
            vga_puts_at("RAM (free / total):",7,10,VGA_WHITE,VGA_BLACK);
            vga_puts_at(fb,27,10,VGA_LIGHT_GREEN,VGA_BLACK);
            vga_puts_at("MB /",33,10,VGA_WHITE,VGA_BLACK);
            vga_puts_at(tb2,38,10,VGA_LIGHT_GREEN,VGA_BLACK);
            vga_puts_at("MB",44,10,VGA_WHITE,VGA_BLACK);

            vga_puts_at("Kernel:",7,11,VGA_WHITE,VGA_BLACK);
            vga_puts_at("x86 32-bit  Multiboot1  GRUB2",15,11,VGA_LIGHT_GREY,VGA_BLACK);
            vga_puts_at("Build:",7,12,VGA_WHITE,VGA_BLACK);
            vga_puts_at("HavenDOS 0.6.1  GCC -m32 -O2",15,12,VGA_LIGHT_GREY,VGA_BLACK);
            vga_puts_at("Hostname:",7,13,VGA_WHITE,VGA_BLACK);
            vga_puts_at(cfg_hostname(),17,13,VGA_LIGHT_GREY,VGA_BLACK);
            vga_puts_at("User:",7,14,VGA_WHITE,VGA_BLACK);
            vga_puts_at(current_username(),13,14,VGA_LIGHT_GREY,VGA_BLACK);
            vga_puts_at(current_user_admin()?"  [admin]":"  [user]",13+(int)strlen(current_username()),14,
                        current_user_admin()?VGA_YELLOW:VGA_LIGHT_GREY,VGA_BLACK);

            for(int x=6;x<75;x++) vga_putchar_at('-',x,16,VGA_DARK_GREY,VGA_BLACK);
            vga_puts_at("github.com-MGKFN-HavenDOS-updates",7,17,VGA_DARK_GREY,VGA_BLACK);
            vga_puts_at("techhavenstudios.lovable.app",7,18,VGA_DARK_GREY,VGA_BLACK);

            int c=keyboard_waitchar();
            if(c==27)return;
            if(c=='\t'){tab=(tab+1)%ntabs;sel=0;continue;}
        }
    }
}

#define GW 36
#define GH 17
static void app_snake(void){
    static int sx[400],sy[400];
    int sl=3,dx=1,dy=0,dead=0,sc=0,fx,fy;
    for(int i=0;i<sl;i++){sx[i]=12-i;sy[i]=8;}
    fx=(int)((get_ticks()*13+5)%GW); fy=(int)((get_ticks()*7+3)%GH);
    vga_clear(); ui_draw_taskbar("Snake");
    /* Quick tick sanity check */
    {
        uint32_t t0=get_ticks();
        volatile int w=500000; while(w--) __asm__ volatile("pause");
        if(get_ticks()==t0){
            /* Ticks not advancing - PIT issue, show warning */
            vga_set_color(VGA_YELLOW,VGA_BLACK);
            vga_puts_at("WARNING: Timer ticks not advancing.",5,5,VGA_YELLOW,VGA_BLACK);
            vga_puts_at("Game may run at wrong speed. Press any key.",5,6,VGA_WHITE,VGA_BLACK);
            keyboard_waitchar();
        }
    }
    /* Double-line border */
    vga_putchar_at('.',21,2,VGA_GREEN,VGA_BLACK);
    vga_putchar_at('.',21+GW+1,2,VGA_GREEN,VGA_BLACK);
    vga_putchar_at('.',21,GH+3,VGA_GREEN,VGA_BLACK);
    vga_putchar_at('.',21+GW+1,GH+3,VGA_GREEN,VGA_BLACK);
    for(int x=1;x<=GW;x++){vga_putchar_at('-',21+x,2,VGA_GREEN,VGA_BLACK);vga_putchar_at('-',21+x,GH+3,VGA_GREEN,VGA_BLACK);}
    for(int y=3;y<=GH+2;y++){vga_putchar_at('|',21,y,VGA_GREEN,VGA_BLACK);vga_putchar_at('|',21+GW+1,y,VGA_GREEN,VGA_BLACK);}
    vga_puts_at("Arrows=move  Q=quit",22,1,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Score: 0   ",50,1,VGA_WHITE,VGA_BLACK);
    /* High score display */
    vga_puts_at("Best: 0    ",2,4,VGA_YELLOW,VGA_BLACK);
    static int best_score=0;
    while(!dead){
        /* Each iteration = one frame = 120ms. Read keys then move. */
        sleep_ms(120);
        int c=keyboard_getchar();
        if(c=='q'||c=='Q'||c==27)break;
        if(c==0x148&&dy==0){dx=0;dy=-1;}
        if(c==0x150&&dy==0){dx=0;dy=1;}
        if(c==0x14B&&dx==0){dx=-1;dy=0;}
        if(c==0x14D&&dx==0){dx=1;dy=0;}
        {
        int nx=sx[0]+dx,ny=sy[0]+dy;
        if(nx<0||nx>=GW||ny<0||ny>=GH){dead=1;break;}
        for(int i=0;i<sl;i++)if(sx[i]==nx&&sy[i]==ny){dead=1;break;}
        if(dead)break;
        vga_putchar_at(' ',22+sx[sl-1],3+sy[sl-1],VGA_BLACK,VGA_BLACK);
        for(int i=sl-1;i>0;i--){sx[i]=sx[i-1];sy[i]=sy[i-1];}
        sx[0]=nx;sy[0]=ny;
        if(nx==fx&&ny==fy){
            sc+=10;sl++;
            if(sc>best_score)best_score=sc;
            fx=(int)((get_ticks()*17)%GW);fy=(int)((get_ticks()*11)%GH);
        }
        for(int i=0;i<sl;i++)
            vga_putchar_at(i?'o':'@',22+sx[i],3+sy[i],i?VGA_LIGHT_GREEN:VGA_WHITE,VGA_BLACK);
        vga_putchar_at('*',22+fx,3+fy,VGA_RED,VGA_BLACK);
        char sb[12]; utoa(sc,sb,10);
        vga_puts_at(sb,57,1,VGA_YELLOW,VGA_BLACK);
        char bs[12]; utoa(best_score,bs,10);
        vga_puts_at(bs,8,4,VGA_YELLOW,VGA_BLACK);
        } /* end frame */
    }
    if(dead)vga_puts_at(" GAME OVER! Press any key ",24,10,VGA_WHITE,VGA_RED);
    else vga_puts_at(" Press any key... ",24,10,VGA_DARK_GREY,VGA_BLACK);
    keyboard_waitchar();
}

static void app_tetris_wrap(void){
    vga_clear();
    app_tetris();
}

static void app_screensaver(void){
    vga_clear();
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts_at("Screensaver",34,10,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at("1. Matrix Rain",32,12,VGA_WHITE,VGA_BLACK);
    vga_puts_at("2. Starfield",32,13,VGA_WHITE,VGA_BLACK);
    vga_puts_at("3. Pipes",32,14,VGA_WHITE,VGA_BLACK);
    vga_puts_at("ESC. Cancel",32,15,VGA_DARK_GREY,VGA_BLACK);
    extern int keyboard_waitchar(void);
    int c=keyboard_waitchar();
    if(c=='1') screensaver_run_matrix();
    if(c=='2') screensaver_run_stars();
    if(c=='3') screensaver_run_pipes();
}

extern void screensaver_run_pipes(void);

static void app_matrix(void){
    vga_clear();
    screensaver_run_matrix();
    vga_clear(); toast_clear();
}

static void app_pipes(void){
    vga_clear();
    screensaver_run_pipes();
    vga_clear(); toast_clear();
}

static void app_dashboard(void){
    /* Launch the dashboard — same as typing 'dashboard' in shell */
    extern void shell_run_cmd(const char *cmd);
    shell_run_cmd("dashboard");
}

/* ── Install App ─────────────────────────────────────────────── */
static void install_progress_gui(int step, int total, const char *msg){
    theme_t *th=ui_theme();
    /* Progress bar row 17, cols 27..57 (30 chars wide) */
    int bar_w=30;
    int filled = (total>0) ? (step*bar_w/total) : 0;
    char bar[31];
    for(int i=0;i<bar_w;i++) bar[i]=(i<filled)?'=':(i==filled?'>':'.');
    bar[bar_w]=0;
    vga_puts_at(bar, 27, 17, th->accent, th->win_bg);
    /* Status line */
    char line[41];
    int ml=(int)strlen(msg); if(ml>38) ml=38;
    for(int i=0;i<38;i++) line[i]=(i<ml)?msg[i]:' ';
    line[38]=0;
    vga_puts_at(line, 27, 16, VGA_WHITE, th->win_bg);
    /* Step counter */
    char sc[16]; char sa[8]; char sb[8];
    utoa(step,sa,10); utoa(total,sb,10);
    int pi=0;
    for(int i=0;sa[i];i++) sc[pi++]=sa[i];
    sc[pi++]='/';
    for(int i=0;sb[i];i++) sc[pi++]=sb[i];
    sc[pi]=0;
    vga_puts_at(sc, 27, 18, VGA_DARK_GREY, th->win_bg);
}

/* ── Notification Center ──────────────────────────────────────────────────── */
#define NC_X    55
#define NC_W    25
#define NC_TOP   1
#define NC_BOT  23
#define NOTIF_MAX 8
static char notif_msgs[NOTIF_MAX][48];
static int  notif_count  = 0;
static int  notif_unread = 0;

int notif_get_count(void)  { return notif_count; }
int notif_get_unread(void) { return notif_unread; }

void notif_push(const char *msg){
    if(notif_count < NOTIF_MAX){
        int j=0;
        while(msg[j]&&j<47){notif_msgs[notif_count][j]=msg[j];j++;}
        notif_msgs[notif_count][j]=0;
        notif_count++;
    } else {
        for(int i=0;i<NOTIF_MAX-1;i++)
            for(int j=0;j<48;j++) notif_msgs[i][j]=notif_msgs[i+1][j];
        int j=0;
        while(msg[j]&&j<47){notif_msgs[NOTIF_MAX-1][j]=msg[j];j++;}
        notif_msgs[NOTIF_MAX-1][j]=0;
    }
    notif_unread++;
}

static void notif_center(void){
    theme_t *th=ui_theme();

    /* Seed initial notifications on first open */
    if(notif_count == 0){
        notif_push("HavenDOS v0.7.4 running");
        notif_push("System boot OK");
        if(vfs_using_disk())  notif_push("VirtIO disk mounted");
        if(vfs_using_fat16()) notif_push("FAT16 filesystem OK");
        notif_push("Press N to open again");
    }

    /* Mark all as read when center is opened */
    notif_unread = 0;

    /* Draw panel */
    for(int y=NC_TOP;y<=NC_BOT;y++)
        for(int x=NC_X;x<NC_X+NC_W;x++)
            vga_putchar_at(' ',x,y,th->win_fg,th->win_bg);

    vga_putchar_at('.',NC_X,NC_TOP,th->accent,th->win_bg);
    for(int x=NC_X+1;x<NC_X+NC_W-1;x++)
        vga_putchar_at('-',x,NC_TOP,th->accent,th->win_bg);
    vga_putchar_at('.',NC_X+NC_W-1,NC_TOP,th->accent,th->win_bg);

    vga_puts_at(" Notifications  ",NC_X+1,NC_TOP+1,th->accent,th->win_bg);
    for(int x=NC_X;x<NC_X+NC_W;x++)
        vga_putchar_at('-',x,NC_TOP+2,th->icon_bg,th->win_bg);

    int row=NC_TOP+3;
    if(notif_count==0){
        vga_puts_at(" No notifications",NC_X,row,th->icon_bg,th->win_bg);
    } else {
        for(int i=notif_count-1; i>=0&&row<NC_BOT-1; i--,row++){
            vga_putchar_at('.',NC_X,row,th->accent,th->win_bg);
            char line[24]; int j=0;
            while(notif_msgs[i][j]&&j<NC_W-3){line[j]=notif_msgs[i][j];j++;}
            while(j<NC_W-2) line[j++]=' ';
            line[j]=0;
            vga_puts_at(line,NC_X+1,row,th->win_fg,th->win_bg);
        }
    }

    for(int x=NC_X;x<NC_X+NC_W;x++)
        vga_putchar_at('-',x,NC_BOT-1,th->icon_bg,th->win_bg);
    vga_puts_at(" Any key=Close ",NC_X+1,NC_BOT,th->icon_bg,th->win_bg);

    notif_unread=0;

    while(1){
        int k=keyboard_getchar();
        if(k==-1){sleep_ms(16);continue;}
        break;
    }
}

/* ── BOOT IDE App ─────────────────────────────────────────────── */
static void app_boot_ide(void){
    extern void boot_repl(void);
    extern int  boot_run_file(const char*);
    extern int  bpkg_is_installed(const char*);

    vga_clear();
    ui_draw_taskbar("HavenCode");

    theme_t *th = ui_theme();
    ui_draw_window(0,1,80,23,"HavenCode  v0.2.0  -  TechHaven Studios");

    /* Always show option screen — check boot-lang at run-time only */
    int boot_installed = bpkg_is_installed("h-lang") || bpkg_is_installed("boot-lang");

    vga_puts_at("Welcome to HavenCode",28,5,th->accent,VGA_BLACK);
    vga_puts_at("H is TechHaven's scripting language (H, H++, H#)",14,7,VGA_LIGHT_GREY,VGA_BLACK);

    if(!boot_installed){
        vga_puts_at("h-lang not installed  (bpkg install h-lang)",18,9,VGA_YELLOW,VGA_BLACK);
    }

    vga_puts_at("1  Interactive REPL  (H interactive mode)",13,11,boot_installed?VGA_WHITE:VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("2  Run a .h / .boot file  (enter filename)",13,13,boot_installed?VGA_WHITE:VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("3  Language reference and examples",13,15,VGA_WHITE,VGA_BLACK);
    vga_puts_at("ESC  Close",13,17,VGA_DARK_GREY,VGA_BLACK);

    int c=keyboard_waitchar();
    if(c==27) return;

    /* Options 1 and 2 need boot-lang installed */
    if((c=='1'||c=='2')&&!boot_installed){
        vga_puts_at("Install boot-lang via bpkg first.  Press any key.",14,20,VGA_LIGHT_RED,VGA_BLACK);
        keyboard_waitchar();
        return;
    }

    if(c=='1'){
        vga_clear();
        vga_set_color(VGA_WHITE,VGA_BLACK);
        boot_repl();
        vga_puts("\n(Press any key to return to desktop)\n");
        keyboard_waitchar();
        return;
    }

    if(c=='2'){
        vga_clear();
        ui_draw_taskbar("HavenCode - Run File");
        vga_puts_at("Enter filename (.boot):",2,3,VGA_WHITE,VGA_BLACK);
        char fname[48]=""; int fp=0;
        vga_puts_at("                                        ",26,3,VGA_DARK_GREY,VGA_BLACK);
        vga_set_cursor(26,3);
        while(1){
            int k=keyboard_waitchar();
            if(k==27||k=='\n'||k=='\r') break;
            if((k=='\b'||k==127)&&fp>0){ fp--; fname[fp]=0; vga_putchar_at('_',25+fp,3,VGA_DARK_GREY,VGA_BLACK); vga_set_cursor(26+fp,3); }
            else if(k>=32&&k<127&&fp<46){ fname[fp++]=k; fname[fp]=0; vga_putchar_at(k,25+fp-1,3,VGA_WHITE,VGA_BLACK); vga_set_cursor(26+fp,3); }
        }
        if(fname[0]){
            vga_puts_at("Running: ",2,5,VGA_LIGHT_CYAN,VGA_BLACK);
            vga_puts_at(fname,11,5,VGA_WHITE,VGA_BLACK);
            vga_set_cursor(0,7);
            vga_set_color(VGA_WHITE,VGA_BLACK);
            boot_run_file(fname);
            vga_puts("\n\n(Press any key to return to desktop)\n");
            keyboard_waitchar();
        }
        return;
    }

    if(c=='3'){
        vga_clear();
        ui_draw_taskbar("HavenCode - Language Reference");
        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts("\n  -- H Language Reference  (H / H++ / H#) --\n\n");
        vga_set_color(VGA_DARK_GREY,VGA_BLACK);
        vga_puts("  H is TechHaven's scripting language for HavenDOS.\n");
        vga_puts("  H++ adds OOP syntax.  H# is the IDE-integrated dialect.\n\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);

        /* Example 1: Hello World */
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("  Example 1: Hello World\n");
        vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        vga_puts("    println \"Hello, HavenDOS!\"\n\n");

        /* Example 2: Count */
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("  Example 2: Countdown\n");
        vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        vga_puts("    var i = 5\n");
        vga_puts("    while i > 0 do\n");
        vga_puts("      println i\n");
        vga_puts("      i = i - 1\n");
        vga_puts("    end\n");
        vga_puts("    println \"Blast off!\"\n\n");

        /* Example 3: File I/O */
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("  Example 3: File I/O\n");
        vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        vga_puts("    writefile \"~/hello.txt\" \"Hello!\"\n");
        vga_puts("    readfile  \"~/hello.txt\" content\n");
        vga_puts("    println content\n\n");

        /* Example 4: Input */
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("  Example 4: Greet user\n");
        vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        vga_puts("    print \"Your name: \"\n");
        vga_puts("    input name\n");
        vga_puts("    println \"Hello, \" + name + \"!\"\n\n");

        vga_set_color(VGA_DARK_GREY,VGA_BLACK);
        vga_puts("  Save as .h or .boot files, run with option 2.\n");
        vga_puts("  Use the Text Editor to write H programs.\n\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        vga_puts("  (Press any key to return)");
        keyboard_waitchar();
        return;
    }
}

/* ── bpkg GUI App ────────────────────────────────────────────────── */
static void app_bpkg(void){
    extern int         bpkg_registry_count(void);
    extern const char *bpkg_pkg_id(int);
    extern const char *bpkg_pkg_name(int);
    extern const char *bpkg_pkg_version(int);
    extern const char *bpkg_pkg_desc(int);
    extern int         bpkg_pkg_size(int);
    extern int         bpkg_is_installed(const char*);
    extern int         bpkg_install(const char*);
    extern int         bpkg_remove(const char*);

    int sel=0, tab=0; /* tab 0=available, tab 1=installed */
    int total=bpkg_registry_count();

    while(1){
        vga_clear(); ui_draw_taskbar("bpkg Package Manager");
        ui_draw_window(4,2,72,21,"bpkg  -  HavenDOS Package Manager  v0.7.4");

        /* Tab bar */
        vga_puts_at(tab==0?" Available ":"  Available",6,4,
                    tab==0?VGA_BLACK:VGA_LIGHT_GREY,tab==0?VGA_CYAN:VGA_BLACK);
        vga_puts_at(tab==1?" Installed ":"  Installed",18,4,
                    tab==1?VGA_BLACK:VGA_LIGHT_GREY,tab==1?VGA_CYAN:VGA_BLACK);
        for(int x=6;x<75;x++) vga_putchar_at('-',x,5,VGA_DARK_GREY,VGA_BLACK);

        if(tab==0){
            /* Available packages */
            vga_puts_at("ID              Version  Size    Description",7,6,VGA_LIGHT_CYAN,VGA_BLACK);
            for(int x=7;x<74;x++) vga_putchar_at('-',x,7,VGA_DARK_GREY,VGA_BLACK);

            for(int i=0;i<total&&i<12;i++){
                int s=(i==sel);
                int inst=bpkg_is_installed(bpkg_pkg_id(i));
                vga_color_t fg=s?VGA_BLACK:VGA_LIGHT_GREY;
                vga_color_t bg=s?VGA_LIGHT_GREY:VGA_BLACK;

                /* Build padded id */
                char idrow[17]; int j=0;
                const char *id=bpkg_pkg_id(i);
                while(*id&&j<16)idrow[j++]=*id++;
                while(j<16)idrow[j++]=' '; idrow[16]=0;

                vga_putchar_at(s?'>':' ',6,8+i,fg,bg);
                vga_puts_at(idrow,7,8+i,fg,bg);
                vga_puts_at(bpkg_pkg_version(i),24,8+i,s?VGA_BLACK:VGA_DARK_GREY,bg);

                /* Size */
                char sz[8]; utoa((uint32_t)bpkg_pkg_size(i),sz,10);
                vga_puts_at(sz,33,8+i,fg,bg);
                vga_puts_at("KB",33+(int)strlen(sz),8+i,fg,bg);

                /* Description (truncated) */
                char desc[28]; int di=0;
                const char *d=bpkg_pkg_desc(i);
                while(*d&&di<26)desc[di++]=*d++;
                while(di<26)desc[di++]=' '; desc[26]=0;
                vga_puts_at(desc,40,8+i,fg,bg);

                /* Status badge */
                if(inst) vga_puts_at("[OK]",68,8+i,VGA_LIGHT_GREEN,bg);
            }

            /* Detail panel for selected */
            for(int x=6;x<75;x++) vga_putchar_at('-',x,21,VGA_DARK_GREY,VGA_BLACK);
            vga_puts_at(bpkg_pkg_name(sel),7,22,VGA_WHITE,VGA_BLACK);
            vga_puts_at(bpkg_is_installed(bpkg_pkg_id(sel))
                        ?"  [installed]  I=Remove":"  [not installed]  I=Install",
                        7+(int)strlen(bpkg_pkg_name(sel)),22,
                        bpkg_is_installed(bpkg_pkg_id(sel))?VGA_LIGHT_GREEN:VGA_YELLOW,VGA_BLACK);

            vga_puts_at("Up-Down=Select  Tab=Installed  I=Install-Remove  ESC=Close",6,23,VGA_DARK_GREY,VGA_BLACK);

            int c=keyboard_waitchar();
            if(c==27) return;
            if(c=='\t'){tab=1;sel=0;continue;}
            if(c==0x148&&sel>0) sel--;
            if(c==0x150&&sel<total-1) sel++;
            if(c=='i'||c=='I'){
                const char *pid=bpkg_pkg_id(sel);
                if(bpkg_is_installed(pid)){
                    bpkg_remove(pid);
                    notif_push("bpkg: package removed");
                } else {
                    int r=bpkg_install(pid);
                    if(r==0){
                        extern const char *bpkg_install_msg(const char*);
                        notif_push(bpkg_install_msg(pid));
                    } else if(r==-3){
                        vga_puts_at("Package DB full",30,23,VGA_LIGHT_RED,VGA_BLACK);
                        sleep_ms(1200);
                    }
                }
            }
        } else {
            /* Installed packages */
            extern int bpkg_installed_count(void);
            extern const char *bpkg_installed_id(int);
            int ic=bpkg_installed_count();
            vga_puts_at("Installed packages:",7,6,VGA_WHITE,VGA_BLACK);
            for(int x=7;x<74;x++) vga_putchar_at('-',x,7,VGA_DARK_GREY,VGA_BLACK);

            if(!ic){
                vga_puts_at("No packages installed.",7,10,VGA_DARK_GREY,VGA_BLACK);
                vga_puts_at("Press Tab to browse available packages.",7,11,VGA_DARK_GREY,VGA_BLACK);
            } else {
                for(int i=0;i<ic&&i<12;i++){
                    int s=(i==sel);
                    vga_color_t fg=s?VGA_BLACK:VGA_LIGHT_GREEN;
                    vga_color_t bg=s?VGA_LIGHT_GREY:VGA_BLACK;
                    vga_putchar_at(s?'>':' ',6,8+i,fg,bg);
                    vga_puts_at(bpkg_installed_id(i),7,8+i,fg,bg);
                }
            }

            vga_puts_at("Tab=Available  R=Remove selected  ESC=Close",6,23,VGA_DARK_GREY,VGA_BLACK);

            int c=keyboard_waitchar();
            if(c==27) return;
            if(c=='\t'){tab=0;sel=0;continue;}
            if(c==0x148&&sel>0)sel--;
            if(c==0x150&&sel<ic-1)sel++;
            if((c=='r'||c=='R')&&ic>0&&sel<ic){
                bpkg_remove(bpkg_installed_id(sel));
                notif_push("bpkg: package removed");
                if(sel>=ic-1&&sel>0)sel--;
            }
        }
    }
}

static void app_install(void){
    extern int      virtio_scan_drives(void);
    extern int      virtio_drive_count(void);
    extern uint64_t virtio_drive_sectors(int);
    extern uint8_t  virtio_drive_bus(int);
    extern uint8_t  virtio_drive_dev(int);
    extern int      havendos_install_to_drive(int, void(*)(int,int,const char*));
    extern int      keyboard_waitchar(void);

    theme_t *th=ui_theme();
    vga_clear();
    ui_draw_taskbar("Install HavenDOS");
    ui_draw_window(20, 4, 40, 22, "HavenDOS Installer");

    /* Scan drives */
    vga_puts_at("Scanning for VirtIO drives...", 22, 8, VGA_DARK_GREY, th->win_bg);
    int n = virtio_scan_drives();

    if(n == 0){
        vga_puts_at("No VirtIO drives found.", 22, 10, VGA_LIGHT_RED, th->win_bg);
        vga_puts_at("Attach a VirtIO disk and reboot.", 22, 11, VGA_DARK_GREY, th->win_bg);
        vga_puts_at("Press any key to return.", 22, 14, VGA_DARK_GREY, th->win_bg);
        keyboard_waitchar(); return;
    }

    /* Drive list */
    vga_puts_at("Available VirtIO drives:", 22, 8, VGA_WHITE, th->win_bg);
    for(int i=0;i<n&&i<8;i++){
        uint64_t sects = virtio_drive_sectors(i);
        uint32_t mb    = (uint32_t)(sects / 2048);
        char line[41]; char ns[4]; char mbs[12];
        utoa(i, ns, 10); utoa(mb, mbs, 10);
        /* build: "[0] VirtIO Bus:0 Dev:5  64MB" */
        int pi=0;
        line[pi++]='['; line[pi++]=ns[0]; line[pi++]=']'; line[pi++]=' ';
        const char *lbl="VirtIO  Bus:";
        for(int j=0;lbl[j];j++) line[pi++]=lbl[j];
        char bs[4]; utoa(virtio_drive_bus(i),bs,10);
        for(int j=0;bs[j];j++) line[pi++]=bs[j];
        line[pi++]=' '; line[pi++]='D'; line[pi++]='e'; line[pi++]='v'; line[pi++]=':';
        char ds[4]; utoa(virtio_drive_dev(i),ds,10);
        for(int j=0;ds[j];j++) line[pi++]=ds[j];
        line[pi++]=' '; line[pi++]=' ';
        for(int j=0;mbs[j];j++) line[pi++]=mbs[j];
        line[pi++]='M'; line[pi++]='B';
        if(i==0){ line[pi++]=' '; line[pi++]='*'; }
        line[pi]=0;
        vga_color_t fg=(i==0)?th->accent:VGA_WHITE;
        vga_puts_at(line, 22, 9+i, fg, th->win_bg);
    }

    /* Drive picker */
    int yp=9+n+1;
    vga_puts_at("Drive number (0-", 22, yp, VGA_WHITE, th->win_bg);
    char nmx[4]; utoa(n-1,nmx,10);
    vga_puts_at(nmx, 38, yp, VGA_WHITE, th->win_bg);
    vga_puts_at(") or Q=cancel:", 39, yp, VGA_WHITE, th->win_bg);
    vga_puts_at("> ", 22, yp+1, th->accent, th->win_bg);

    char inp[4]; int ii=0; inp[0]=0;
    while(1){
        int k=keyboard_waitchar();
        if(k=='\n'||k=='\r') break;
        if(k=='q'||k=='Q'){ inp[0]='Q'; inp[1]=0; break; }
        if(k==0x1B){ inp[0]='Q'; inp[1]=0; break; }
        if(k>='0'&&k<='9'&&ii<2){
            inp[ii++]=(char)k; inp[ii]=0;
            vga_putchar_at((char)k, 24+ii, yp+1, th->accent, th->win_bg);
        }
    }

    if(inp[0]=='Q'||inp[0]==0){
        vga_puts_at("Cancelled.          ", 22, yp+2, VGA_DARK_GREY, th->win_bg);
        sleep_ms(800); return;
    }

    int target=atoi(inp);
    if(target<0||target>=n){
        vga_puts_at("Invalid drive number.", 22, yp+2, VGA_LIGHT_RED, th->win_bg);
        sleep_ms(1200); return;
    }

    /* Warning + confirm */
    uint64_t tsects=virtio_drive_sectors(target);
    uint32_t tmb=(uint32_t)(tsects/2048);
    (void)tmb; /* size shown in drive list above */
    vga_puts_at("WARNING: Drive will be WIPED.", 22, yp+2, VGA_LIGHT_RED, th->win_bg);
    vga_puts_at("Type YES to confirm:", 22, yp+3, VGA_WHITE, th->win_bg);
    vga_puts_at("> ", 22, yp+4, th->accent, th->win_bg);

    char conf[8]; int ci=0; conf[0]=0;
    while(1){
        int k=keyboard_waitchar();
        if(k=='\n'||k=='\r') break;
        if(k==0x1B){ conf[0]=0; break; }
        if(k=='\b'&&ci>0){
            ci--; conf[ci]=0;
            vga_putchar_at(' ', 24+ci, yp+4, th->accent, th->win_bg);
        } else if(k>='A'&&k<='Z'&&ci<7){
            conf[ci++]=(char)k; conf[ci]=0;
            vga_putchar_at((char)k, 23+ci, yp+4, th->accent, th->win_bg);
        } else if(k>='a'&&k<='z'&&ci<7){
            char uc=(char)(k-32);
            conf[ci++]=uc; conf[ci]=0;
            vga_putchar_at(uc, 23+ci, yp+4, th->accent, th->win_bg);
        }
    }

    if(strcmp(conf,"YES")!=0){
        vga_puts_at("Cancelled.          ", 22, yp+5, VGA_DARK_GREY, th->win_bg);
        sleep_ms(800); return;
    }

    /* ── Run installer ── */
    /* Clear window body for progress display */
    for(int sy=13;sy<=20;sy++)
        for(int sx=21;sx<=59;sx++)
            vga_putchar_at(' ', sx, sy, th->win_bg, th->win_bg);
    vga_puts_at("Installing HavenDOS...", 27, 14, VGA_WHITE, th->win_bg);
    vga_puts_at("Do not power off.", 27, 15, VGA_DARK_GREY, th->win_bg);

    int rc = havendos_install_to_drive(target, install_progress_gui);

    /* Result */
    for(int sy=13;sy<=20;sy++)
        for(int sx=21;sx<=59;sx++)
            vga_putchar_at(' ', sx, sy, th->win_bg, th->win_bg);

    if(rc==0){
        vga_puts_at("Install complete.", 27, 14, VGA_LIGHT_GREEN, th->win_bg);
        vga_puts_at("1. Shutdown  2. Keep running", 27, 16, VGA_WHITE, th->win_bg);
        int c=keyboard_waitchar();
        if(c=='1'){
            vga_puts_at("Shutting down...", 27, 18, VGA_DARK_GREY, th->win_bg);
            sleep_ms(600);
            outw(0x604,0x2000); outw(0xB004,0x2000); outw(0x4004,0x3400);
            for(;;)__asm__ volatile("cli;hlt");
        }
    } else {
        char erc[8]; utoa(-rc, erc, 10);
        vga_puts_at("Install FAILED (err -", 27, 14, VGA_LIGHT_RED, th->win_bg);
        vga_puts_at(erc, 48, 14, VGA_LIGHT_RED, th->win_bg);
        vga_puts_at(")", 49, 14, VGA_LIGHT_RED, th->win_bg);
        const char *reason="unknown";
        if(rc==-1) reason="no drive";
        else if(rc==-2) reason="too small";
        else if(rc==-3) reason="MBR write fail";
        else if(rc==-4) reason="GRUB embed fail";
        else if(rc==-5) reason="FAT16 format fail";
        else if(rc==-6) reason="mkdir fail";
        vga_puts_at(reason, 27, 15, VGA_DARK_GREY, th->win_bg);
        vga_puts_at("Press any key.", 27, 17, VGA_DARK_GREY, th->win_bg);
        keyboard_waitchar();
    }
}


static void app_power(void){
    vga_clear(); ui_draw_taskbar("Power");
    ui_draw_window(25,7,30,12,"Power Options");
    vga_puts_at("1. Reboot",          27,10,VGA_WHITE,VGA_BLACK);
    vga_puts_at("2. Shutdown",        27,11,VGA_WHITE,VGA_BLACK);
    vga_puts_at("3. Switch user",     27,12,VGA_WHITE,VGA_BLACK);
    vga_puts_at("ESC. Cancel",        27,14,VGA_DARK_GREY,VGA_BLACK);
    int c=keyboard_waitchar();
    if(c=='1'){
        vga_clear();
        vga_puts_at("  Rebooting HavenDOS...  ", 28, 12, VGA_BLACK, VGA_LIGHT_CYAN);
        vga_puts_at("Please wait.",             34, 14, VGA_DARK_GREY, VGA_BLACK);
        sleep_ms(800); outb(0x64,0xFE);
        for(;;)__asm__ volatile("cli;hlt");
    }
    if(c=='2'){
        vga_clear();
        vga_puts_at("  Shutting down HavenDOS...  ", 26, 12, VGA_BLACK, VGA_YELLOW);
        vga_puts_at("It is now safe to power off.", 26, 14, VGA_DARK_GREY, VGA_BLACK);
        sleep_ms(600);
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        outw(0x4004, 0x3400);
        for(;;)__asm__ volatile("cli;hlt");
    }
    if(c=='3'){
        login_screen();
    }
}

/* ================================================================
   DESKTOP MAIN LOOP
   ================================================================ */
void desktop_run(void){
    int icon_sel=0, needs_draw=1;
    int last_mouse_x=-1, last_mouse_y=-1;
    menu_state=0; top_sel=0; sub_sel=0;
    screensaver_reset();
    music_startup();
    uint8_t last_rtc_s=0xFF;  /* track RTC seconds for clock update */
    /* Welcome toast + notification */
    toast_success("Welcome to HavenDOS v0.7.4!");
    notif_push("HavenDOS v0.7.4 started");
    while(1){
        if(needs_draw){
            full_draw(icon_sel,menu_state,top_sel,sub_sel);
            needs_draw=0;
        }

        /* Refresh clock + infopanel every real second via RTC */
        {
            uint8_t ch,cm,cs; rtc_get_time(&ch,&cm,&cs);
            if(cs!=last_rtc_s){
                last_rtc_s=cs;
                reminders_check();
                if(menu_state==0){
                    draw_infopanel();
                    char ts[9]; rtc_time_str(ts);
                    char ds[9]; rtc_date_str(ds);
                    theme_t *th=ui_theme();
                    vga_puts_at(ds,60,0,th->bar_fg,th->bar_bg);
                    vga_puts_at(" | ",68,0,VGA_DARK_GREY,th->bar_bg);
                    vga_puts_at(ts,71,0,th->bar_fg,th->bar_bg);
                    if(mouse_is_ready()){
                        int mx,my,mb; mouse_get(&mx,&my,&mb);
                        if((my==22&&mx<27)||my==0){
                            ui_force_cursor_redraw();
                            ui_draw_cursor();
                        }
                    }
                }
            }
        }
        toast_tick();

        screensaver_check();

        /* Mouse click handling — works alongside keyboard */
        if(mouse_is_ready()){
            int mx,my,mb; mouse_get(&mx,&my,&mb);

            int mb_click = mouse_get_clicks();

            /* Cursor tracking — runs regardless of menu state */
            if(mx!=last_mouse_x||my!=last_mouse_y){
                ui_draw_cursor();
                last_mouse_x=mx; last_mouse_y=my;
            }

            /* ── Hover highlighting ────────────────────────────────── */
            if(menu_state==1||menu_state==2){
                /* Left panel categories */
                if(mx<MENU_W&&my>=MENU_ITEM_Y&&my<MENU_ITEM_Y+NTOP){
                    int h=my-MENU_ITEM_Y;
                    if(h!=top_sel||menu_state==1){
                        top_sel=h; sub_sel=0; menu_state=2; needs_draw=1;
                    }
                }
                /* Submenu hover */
                if(menu_state==2){
                    int nsub=topmenu[top_sel].nsub;
                    if(mx>=SUB_X&&mx<SUB_X+SUB_W&&my>=MENU_ITEM_Y&&my<MENU_ITEM_Y+nsub){
                        int h=my-MENU_ITEM_Y;
                        if(h!=sub_sel){sub_sel=h;needs_draw=1;}
                    }
                }
            }
            /* Context menu hover */
            if(menu_state==3){
                if(mx>=ctx_x&&mx<ctx_x+CTXM_W&&my>=ctx_y+1&&my<ctx_y+1+CTXM_NITEMS){
                    int h=my-(ctx_y+1);
                    if(h!=ctx_sel){ctx_sel=h;needs_draw=1;}
                }
            }

            if(mb_click&1){
                screensaver_reset(); needs_draw=1;
                if(menu_state==0){
                    /* Icon hit test — accounts for scroll offset */
                    int clicked=-1;
                    for(int i=0;i<NAPP;i++){
                        int approw=i/2, icol=i%2;
                        int vis_row=approw-icon_scroll_row;
                        if(vis_row<0||vis_row>=ICON_ROWS_VIS) continue;
                        int ix=1+icol*17, iy=2+vis_row*ICON_TILE_STEP;
                        if(mx>=ix&&mx<ix+15&&my>=iy&&my<iy+2){
                            clicked=i; break;
                        }
                    }
                    if(clicked>=0) { icon_sel=clicked; apps[icon_sel].fn(); }
                    /* Start button: row 24, cols 0-13 */
                    if(my==24&&mx<=13){ menu_state=1; top_sel=0; sub_sel=0; }
                    /* Notification center: row 24, cols 55-63 */
                    if(my==24&&mx>=55&&mx<=63){ notif_center(); needs_draw=1; }
                } else if(menu_state==1||menu_state==2){
                    /* Category item click */
                    if(mx<MENU_W&&my>=MENU_ITEM_Y&&my<MENU_ITEM_Y+NTOP){
                        top_sel=my-MENU_ITEM_Y; menu_state=2; sub_sel=0;
                    }
                    /* Submenu item click */
                    else if(menu_state==2&&mx>=SUB_X&&mx<SUB_X+SUB_W&&
                            my>=MENU_ITEM_Y&&my<MENU_ITEM_Y+topmenu[top_sel].nsub){
                        sub_sel=my-MENU_ITEM_Y; menu_state=0;
                        topmenu[top_sel].sub[sub_sel].fn();
                    }
                    /* Lock click */
                    else if(mx<MENU_W&&my==MENU_LOCK_Y){
                        menu_state=0; needs_draw=1;
                        login_screen();
                    }
                    /* Click outside → close */
                    else if(my==24||my<MENU_TOP){ menu_state=0; }
                } else if(menu_state==3){
                    if(mx>=ctx_x&&mx<ctx_x+CTXM_W&&my>=ctx_y+1&&my<ctx_y+1+CTXM_NITEMS){
                        ctx_sel=my-(ctx_y+1); menu_state=0;
                        ctx_menu_activate(ctx_sel);
                    } else {
                        menu_state=0;
                    }
                }
            }
            if(mb_click&2){
                screensaver_reset(); needs_draw=1;
                if(menu_state==0){
                    ctx_x=mx; ctx_y=my; ctx_sel=0;
                    if(ctx_x+CTXM_W>80) ctx_x=80-CTXM_W;
                    if(ctx_x<0) ctx_x=0;
                    if(ctx_y+CTXM_H>23) ctx_y=23-CTXM_H;
                    if(ctx_y<1) ctx_y=1;
                    menu_state=3;
                } else if(menu_state==3){
                    menu_state=0;
                } else {
                    menu_state=0;
                }
            }
        }

        int c=keyboard_getchar();
        if(c==-1){
            sleep_ms(20); /* ~50fps polling, low CPU */
            continue;
        }
        needs_draw=1;
        screensaver_reset();

        if(menu_state==0){
            /* Desktop: no menu open */
            if(c==27){ menu_state=1; top_sel=0; sub_sel=0; }
            else if(c=='\t')    app_shell();
            else if(c=='n'||c=='N') { notif_center(); needs_draw=1; }
            else if(c==0x148&&icon_sel>=2)  icon_sel-=2;
            else if(c==0x150&&icon_sel+2<NAPP) icon_sel+=2;
            else if(c==0x14B&&icon_sel%2>0) icon_sel--;
            else if(c==0x14D&&icon_sel%2==0&&icon_sel+1<NAPP) icon_sel++;
            else if(c=='\n'||c=='\r')       apps[icon_sel].fn();
        }
        else if(menu_state==1){
            /* Main panel: up/down through categories */
            if(c==27)                         { menu_state=0; }
            else if(c==0x148&&top_sel>0)      { top_sel--; sub_sel=0; sub_scroll=0; }
            else if(c==0x150&&top_sel<NTOP-1) { top_sel++; sub_sel=0; sub_scroll=0; }
            else if(c==0x14D||c=='\n'||c=='\r'){ menu_state=2; sub_sel=0; sub_scroll=0; } /* right/enter = open submenu */
        }
        else if(menu_state==2){
            /* Submenu flyout */
            int nsub=topmenu[top_sel].nsub;
            if(c==27)                          { menu_state=0; }
            else if(c==0x14B)                  { menu_state=1; } /* left = back to categories */
            else if(c==0x148&&sub_sel>0)       sub_sel--;
            else if(c==0x150&&sub_sel<nsub-1)  sub_sel++;
            else if(c=='\n'||c=='\r'){
                menu_state=0;
                topmenu[top_sel].sub[sub_sel].fn();
            }
        }
        else if(menu_state==3){
            /* Context menu (right-click quick access) */
            if(c==27)                                  { menu_state=0; }
            else if(c==0x148&&ctx_sel>0)                ctx_sel--;
            else if(c==0x150&&ctx_sel<CTXM_NITEMS-1)    ctx_sel++;
            else if(c=='\n'||c=='\r'){
                menu_state=0;
                ctx_menu_activate(ctx_sel);
            }
        }
    }
}
/* ================================================================
   PONG
   ================================================================ */
static void app_pong(void){
    #define PG_X1 2
    #define PG_X2 77
    #define PG_Y1 2
    #define PG_Y2 22
    #define PG_H  (PG_Y2-PG_Y1+1)
    #define PAD_H 4

    vga_clear(); ui_draw_taskbar("Pong");
    for(int x=PG_X1;x<=PG_X2;x++){
        vga_putchar_at('-',x,PG_Y1,VGA_WHITE,VGA_BLACK);
        vga_putchar_at('-',x,PG_Y2,VGA_WHITE,VGA_BLACK);
    }
    vga_puts_at("W/S=Left  Arrows=Right  Q=Quit  First to 7 wins",15,0,VGA_DARK_GREY,VGA_BLACK);

    int lpy=PG_Y1+PG_H/2-PAD_H/2;
    int rpy=PG_Y1+PG_H/2-PAD_H/2;
    int lsc=0, rsc=0;
    int bx=(PG_X1+PG_X2)/2*10, by=(PG_Y1+PG_Y2)/2*10;
    int bdx=7, bdy=4;
    int last_bx=-1, last_by=-1;
    int last_lpy=-1, last_rpy=-1;
    uint32_t last=get_ticks();

    while(1){
        int c=keyboard_getchar();
        if(c=='q'||c=='Q'||c==27) break;
        if(c=='w'||c=='W'){ if(lpy>PG_Y1+1) lpy--; }
        if(c=='s'||c=='S'){ if(lpy+PAD_H<PG_Y2) lpy++; }
        if(c==0x148){       if(rpy>PG_Y1+1) rpy--; }
        if(c==0x150){       if(rpy+PAD_H<PG_Y2) rpy++; }

        if(get_ticks()-last < 3){ __asm__ volatile("pause"); continue; }
        last=get_ticks();

        /* Erase */
        if(last_bx>=0) vga_putchar_at(' ',last_bx,last_by,VGA_BLACK,VGA_BLACK);
        if(last_lpy>=0) for(int i=0;i<PAD_H;i++) vga_putchar_at(' ',PG_X1+2,last_lpy+i,VGA_BLACK,VGA_BLACK);
        if(last_rpy>=0) for(int i=0;i<PAD_H;i++) vga_putchar_at(' ',PG_X2-2,last_rpy+i,VGA_BLACK,VGA_BLACK);

        bx+=bdx; by+=bdy;
        int ibx=bx/10, iby=by/10;

        if(iby<=PG_Y1){ by=(PG_Y1+1)*10; bdy=(bdy<0?-bdy:bdy); }
        if(iby>=PG_Y2){ by=(PG_Y2-1)*10; bdy=-(bdy<0?-bdy:bdy); }

        if(ibx<=PG_X1+3 && iby>=lpy && iby<lpy+PAD_H){
            bx=(PG_X1+4)*10; bdx=(bdx<0?-bdx:bdx)+1;
            if(bdx>14) bdx=14;
            bdy=(iby-lpy-PAD_H/2)*2;
            if(bdy==0) bdy=1;
        }
        if(ibx>=PG_X2-3 && iby>=rpy && iby<rpy+PAD_H){
            bx=(PG_X2-4)*10; bdx=-((bdx<0?-bdx:bdx)+1);
            if(bdx<-14) bdx=-14;
            bdy=(iby-rpy-PAD_H/2)*2;
            if(bdy==0) bdy=-1;
        }

        if(ibx<=PG_X1){
            rsc++;
            bx=(PG_X1+PG_X2)/2*10; by=(PG_Y1+PG_Y2)/2*10;
            bdx=7; bdy=4;
            sleep_ms(300);
        }
        if(ibx>=PG_X2){
            lsc++;
            bx=(PG_X1+PG_X2)/2*10; by=(PG_Y1+PG_Y2)/2*10;
            bdx=-7; bdy=4;
            sleep_ms(300);
        }

        /* Draw paddles */
        for(int i=0;i<PAD_H;i++){
            vga_putchar_at('|',PG_X1+2,lpy+i,VGA_LIGHT_CYAN,VGA_BLACK);
            vga_putchar_at('|',PG_X2-2,rpy+i,VGA_LIGHT_RED,VGA_BLACK);
        }
        ibx=bx/10; iby=by/10;
        vga_putchar_at('o',ibx,iby,VGA_YELLOW,VGA_BLACK);

        /* Centre divider */
        for(int y=PG_Y1+1;y<PG_Y2;y+=2)
            vga_putchar_at(':',( PG_X1+PG_X2)/2,y,VGA_DARK_GREY,VGA_BLACK);

        /* Score */
        char sc[4]; itoa(lsc,sc,10);
        vga_puts_at(sc,36,1,VGA_LIGHT_CYAN,VGA_BLACK);
        vga_putchar_at('-',38,1,VGA_WHITE,VGA_BLACK);
        itoa(rsc,sc,10);
        vga_puts_at(sc,40,1,VGA_LIGHT_RED,VGA_BLACK);

        last_bx=ibx; last_by=iby;
        last_lpy=lpy; last_rpy=rpy;

        if(lsc>=7||rsc>=7) break;
    }
    const char *win = lsc>rsc ? "Left Player Wins!" : rsc>lsc ? "Right Player Wins!" : "Draw!";
    int wx=40-(int)strlen(win)/2;
    vga_puts_at(win,wx,12,VGA_WHITE,VGA_RED);
    vga_puts_at("Press any key...",32,14,VGA_DARK_GREY,VGA_BLACK);
    while(keyboard_getchar()==-1) __asm__ volatile("pause");
    keyboard_waitchar();
    #undef PG_X1
    #undef PG_X2
    #undef PG_Y1
    #undef PG_Y2
    #undef PG_H
    #undef PAD_H
}

/* ================================================================
   NEW SYSTEM APP WRAPPERS
   ================================================================ */
static void app_hexviewer(void){
    vga_clear(); ui_draw_taskbar("Hex Viewer");
    ui_draw_window(2,2,76,20,"Hex Viewer  -  Enter filename, ESC to cancel");
    vga_puts_at("File: ",5,4,VGA_LIGHT_GREY,VGA_BLACK);
    char fname[32]=""; int fpos=0;
    vga_set_cursor(11,4);
    while(1){
        int c=keyboard_waitchar();
        if(c=='\n'||c=='\r') break;
        if(c==27) return;
        if((c=='\b'||c==127)&&fpos>0){ fpos--; fname[fpos]=0; vga_putchar('\b'); }
        else if(c>=32&&c<127&&fpos<30){ fname[fpos++]=c; fname[fpos]=0; vga_putchar(c); }
    }
    if(!fname[0]) return;
    static char buf[512];
    int r=vfs_read(fname,buf,511);
    if(r<0){ vga_puts_at("File not found!",5,6,VGA_RED,VGA_BLACK); keyboard_waitchar(); return; }
    for(int row=0;row<r/16+1&&row<14;row++){
        int base=row*16;
        /* Offset */
        char ob[5];
        ob[0]="0123456789ABCDEF"[(base>>12)&0xF];
        ob[1]="0123456789ABCDEF"[(base>>8)&0xF];
        ob[2]="0123456789ABCDEF"[(base>>4)&0xF];
        ob[3]="0123456789ABCDEF"[base&0xF];
        ob[4]=0;
        vga_puts_at(ob,4,6+row,VGA_DARK_GREY,VGA_BLACK);
        vga_putchar_at(':',8,6+row,VGA_DARK_GREY,VGA_BLACK);
        for(int i=0;i<16;i++){
            int idx=base+i;
            if(idx<r){
                unsigned char byte=(unsigned char)buf[idx];
                char hb[3];
                hb[0]="0123456789ABCDEF"[byte>>4];
                hb[1]="0123456789ABCDEF"[byte&0xF];
                hb[2]=0;
                vga_puts_at(hb,10+i*3,6+row,VGA_LIGHT_GREEN,VGA_BLACK);
            }
        }
        vga_putchar_at('|',59,6+row,VGA_DARK_GREY,VGA_BLACK);
        for(int i=0;i<16;i++){
            int idx=base+i;
            char ch=(idx<r&&buf[idx]>=32&&buf[idx]<127)?buf[idx]:'.';
            vga_putchar_at(ch,60+i,6+row,VGA_WHITE,VGA_BLACK);
        }
        vga_putchar_at('|',76,6+row,VGA_DARK_GREY,VGA_BLACK);
    }
    char rb[12]; itoa(r,rb,10); strcat(rb," bytes");
    vga_puts_at(rb,4,21,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Any key...",60,21,VGA_DARK_GREY,VGA_BLACK);
    keyboard_waitchar();
}

static void app_logviewer(void){
    vga_clear(); ui_draw_taskbar("Log Viewer");
    ui_draw_window(2,2,76,20,"Login Audit Log");
    if(!current_user_admin()){
        vga_puts_at("Admin access required.",5,12,VGA_RED,VGA_BLACK);
        keyboard_waitchar(); return;
    }
    static char lb[1024];
    int lr=vfs_read(".loginlog",lb,1023);
    if(lr<=0){ vga_puts_at("No log entries yet.",5,12,VGA_DARK_GREY,VGA_BLACK); }
    else {
        lb[lr]=0;
        char *p=lb; int row=4;
        while(*p&&row<22){
            char *end=p; while(*end&&*end!='\n') end++;
            char saved=*end; *end=0;
            vga_puts_at(p,4,row,VGA_LIGHT_GREY,VGA_BLACK);
            *end=saved; p=end+(*end?1:0); row++;
        }
    }
    vga_puts_at("Any key...",60,22,VGA_DARK_GREY,VGA_BLACK);
    keyboard_waitchar();
}

static void app_diskinfo(void){
    vga_clear(); ui_draw_taskbar("Disk Info");
    ui_draw_window(4,2,72,21,"Storage Information  -  HavenDOS v0.7.4");
    int y=4;
    #define DROW(l,v,c) vga_puts_at(l,8,y,VGA_LIGHT_GREY,VGA_BLACK);vga_puts_at(v,30,y,c,VGA_BLACK);y++;
    /* RAMFS */
    DROW("Primary Storage:", "RAMFS (in-memory)", VGA_LIGHT_GREEN);
    DROW("Max Files:",        "32 files",          VGA_WHITE);
    DROW("Max File Size:",    "4 KB per file",     VGA_WHITE);
    y++;
    /* VirtIO disk */
    int vready=virtio_blk_ready();
    DROW("VirtIO Block:",   vready?"Detected":"Not detected", vready?VGA_LIGHT_GREEN:VGA_DARK_GREY);
    if(vready){
        uint32_t secs=(uint32_t)virtio_blk_sectors();
        uint32_t mb=secs/2048;
        char szb[16]; char tmp[12]; utoa(mb,tmp,10); strncpy(szb,tmp,8); strncat(szb," MB",15);
        DROW("Disk Size:",    szb,              VGA_WHITE);
        DROW("Sector Size:",  "512 bytes",      VGA_WHITE);
        char sec_s[16]; utoa(secs,sec_s,10);
        DROW("Total Sectors:",sec_s,            VGA_DARK_GREY);
        DROW("Filesystem:",   "FAT16 (HavenDOS partition)", VGA_WHITE);
        DROW("Interface:",    "VirtIO BLK (UTM SE)", VGA_LIGHT_CYAN);
        DROW("PCI ID:",       "VID:1AF4 DID:1001",   VGA_DARK_GREY);
    } else {
        DROW("Disk Size:",    "---",            VGA_DARK_GREY);
        DROW("Filesystem:",   "---",            VGA_DARK_GREY);
        DROW("Note:",         "Attach VirtIO disk in UTM SE", VGA_YELLOW);
    }
    y++;
    DROW("FAT12 (ATA):",  "Legacy fallback", VGA_DARK_GREY);
    vga_puts_at("Any key...",60,22,VGA_DARK_GREY,VGA_BLACK);
    keyboard_waitchar();
    #undef DROW
}

static void app_netinfo(void){
    vga_clear(); ui_draw_taskbar("Network Info");
    ui_draw_window(4,2,72,21,"Network Information  -  HavenDOS v0.7.4");
    int y=4;
    #define NROW(l,v,c) vga_puts_at(l,8,y,VGA_LIGHT_GREY,VGA_BLACK);vga_puts_at(v,30,y,c,VGA_BLACK);y++;
    /* Live NIC status */
    int ready=net_ready();
    NROW("NIC Status:",  ready?"Online":"Offline", ready?VGA_LIGHT_GREEN:VGA_LIGHT_RED);
    if(ready){
        char ip_s[20]; ip_to_str(net_get_ip(),ip_s);
        char gw_s[20]; ip_to_str(net_get_gw(),gw_s);
        char ms_s[20]; ip_to_str(net_get_mask(),ms_s);
        char dns_s[20]; ip_to_str(net_get_dns(),dns_s);
        uint8_t mac[6]; net_get_mac(mac);
        char mac_s[20];
        char *mp=mac_s;
        for(int i=0;i<6;i++){
            char h[3]; h[0]="0123456789ABCDEF"[mac[i]>>4]; h[1]="0123456789ABCDEF"[mac[i]&0xF]; h[2]=0;
            *mp++=h[0]; *mp++=h[1]; if(i<5)*mp++=':';
        }
        *mp=0;
        NROW("IP Address:", ip_s,  VGA_WHITE);
        NROW("Subnet Mask:",ms_s,  VGA_WHITE);
        NROW("Gateway:",    gw_s,  VGA_WHITE);
        NROW("DNS Server:", dns_s, VGA_WHITE);
        NROW("MAC Address:",mac_s, VGA_LIGHT_CYAN);
    } else {
        NROW("IP Address:", "---",           VGA_DARK_GREY);
        NROW("Subnet:",     "---",           VGA_DARK_GREY);
        NROW("Gateway:",    "---",           VGA_DARK_GREY);
        NROW("DNS Server:", "---",           VGA_DARK_GREY);
        NROW("MAC Address:","---",           VGA_DARK_GREY);
    }
    y++;
    NROW("TCP/IP Stack:", "Active (v0.7.4)",     VGA_LIGHT_GREEN);
    NROW("Protocols:",    "ARP ICMP UDP TCP DNS", VGA_WHITE);
    NROW("HTTP/HTTPS:",   "wget curl (TLS 1.2)", VGA_WHITE);
    NROW("BOOT Chat:",    "TCP:7700 (stub)",      VGA_YELLOW);
    vga_puts_at("Any key...",60,22,VGA_DARK_GREY,VGA_BLACK);
    keyboard_waitchar();
    #undef NROW
}

static void app_usermanager(void){
    vga_clear(); ui_draw_taskbar("User Manager");
    ui_draw_window(5,2,70,20,"User Manager");
    vga_puts_at("Username",        8,4,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at("Role",           30,4,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at("Status",         44,4,VGA_LIGHT_CYAN,VGA_BLACK);
    for(int i=0;i<68;i++) vga_putchar_at('-',6+i,5,VGA_DARK_GREY,VGA_BLACK);
    for(int i=0;i<get_user_count()&&i<14;i++){
        const char *un=get_username(i);
        int isme=(strcmp(un,current_username())==0);
        vga_puts_at(un,8,6+i,isme?VGA_LIGHT_GREEN:VGA_WHITE,VGA_BLACK);
        vga_puts_at(get_user_admin(i)?"Admin":"User",30,6+i,
            get_user_admin(i)?VGA_YELLOW:VGA_LIGHT_GREY,VGA_BLACK);
        vga_puts_at(isme?"[current]":"",44,6+i,VGA_CYAN,VGA_BLACK);
    }
    if(current_user_admin())
        vga_puts_at("A=Add  D=Delete  ESC=Back",8,21,VGA_DARK_GREY,VGA_BLACK);
    else
        vga_puts_at("ESC=Back",8,21,VGA_DARK_GREY,VGA_BLACK);

    int c=keyboard_waitchar();
    if((c=='a'||c=='A')&&current_user_admin()){
        char nu[16]="",np[16]="";
        vga_puts_at("New username: ",8,22,VGA_WHITE,VGA_BLACK);
        int pi=0; vga_set_cursor(22,22);
        while(1){int c2=keyboard_waitchar();if(c2=='\n'||c2=='\r')break;
            if((c2=='\b'||c2==127)&&pi>0){pi--;nu[pi]=0;vga_putchar('\b');}
            else if(c2>=32&&c2<127&&pi<14){nu[pi++]=c2;nu[pi]=0;vga_putchar(c2);}
        }
        vga_puts_at("Password:     ",8,22,VGA_WHITE,VGA_BLACK);
        pi=0; vga_set_cursor(22,22);
        while(1){int c2=keyboard_waitchar();if(c2=='\n'||c2=='\r')break;
            if((c2=='\b'||c2==127)&&pi>0){pi--;np[pi]=0;vga_putchar('\b');}
            else if(c2>=32&&c2<127&&pi<14){np[pi++]=c2;np[pi]=0;vga_putchar('*');}
        }
        if(nu[0]&&np[0]) add_user(nu,np,0);
    }
}
