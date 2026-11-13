#include "../include/klog.h"
#include "../include/keys.h"
#include "../include/io.h"
#include "../include/instdisk.h"
#include "version.h"
#include "../include/vga.h"
#include "../include/appguard.h"
#include "../include/machine.h"
extern void screensaver_run_pipes(void);  /* v0.7.5: was used before declaration */
#include "../include/virtio.h"   /* v0.7.5: was implicit — virtio_blk_sectors() returns uint64_t */
#include "../include/net.h"      /* v0.7.5: net_get_ip/gw/mask/dns/mac, ip_to_str */
#include "../include/string.h"
#include "../include/types.h"
#include "../include/theme.h"
#include "../include/rtc.h"
#include "../include/toast.h"

extern int         keyboard_getchar(void);
extern void        mouse_clamp_to_grid(void);
extern int         keyboard_waitchar(void);
extern int         keyboard_haschar(void);
extern void        mouse_get(int*,int*,int*);
extern int         mouse_is_ready(void);
extern int         mouse_get_clicks(void);
extern void        ui_draw_taskbar(const char*);
extern int         toast_tick_expired(void);   /* v0.7.6 */
extern void        ui_draw_desktop(void);
extern void        ui_draw_window(int,int,int,int,const char*);
extern void        ui_draw_cursor(void);
extern void        ui_erase_cursor(void);
extern void        ui_force_cursor_redraw(void);
extern void        shell_run(void);
extern void        sleep_ms(uint32_t);
extern void        ui_idle(uint32_t);
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
/* ================================================================
   THE APP REGISTRY  --  one table, several views
   ================================================================

   Until 0.8.1 there were THREE app tables in this file: four
   menu_item_t submenu arrays, a top_item_t topmenu[] pointing at
   them, and an app_t apps[] for the icon grid.  Nothing tied them
   together, so they drifted: Install and bpkg were on the icon grid
   and in no menu at all, and a dozen things that existed only as
   shell commands (paint, music, calendar, quiz, lspci, usb, dmesg,
   BOOT Chat, the guide) were in neither.

   There is one table now.  Categories, All Apps, Favorites and the
   desktop grid are all *views* over it, computed by reg_fill().
   Adding an app is one line in one place, and it cannot appear in
   one view while being forgotten in another.

   The `id` is the stable key written into .favorites and .desktop.
   Renaming a `name` is cosmetic; changing an `id` orphans a pin, so
   ids are append-only in practice.
   ================================================================ */

/* Apps defined later in this file.  (The old duplicate forward-declaration
   block lived inside the tables this registry replaced.) */
static void app_pong(void);
static void app_hexviewer(void);
static void app_logviewer(void);
static void app_diskinfo(void);
static void app_netinfo(void);
static void app_usermanager(void);
static void app_dashboard(void);
static void app_pipes(void);
static void app_matrix(void);

/* Wrappers for things that were shell-only.  See shell/apps_extra.c
   for why each one does or does not pause on exit. */
extern void appx_paint(void);
extern void appx_quiz(void);
extern void appx_hacker(void);
extern void appx_music(void);
extern void appx_calendar(void);
extern void appx_sysinfo_plus(void);
extern void appx_fortune(void);
extern void appx_ascii_art(void);
extern void appx_pci(void);
extern void appx_usb(void);
extern void appx_bootlog(void);
extern void appx_selftest(void);
extern void appx_neofetch(void);
extern void appx_chat(void);
extern void appx_guide(void);
extern void appx_manual(void);
extern void appx_legal(void);
extern void appx_stars(void);
extern void appx_themes(void);
extern void app_mines(void);

/* Real categories.  The start menu adds Favorites above these and
   All Apps below them; neither is a category an app can belong to. */
typedef enum {
    CAT_APPS=0, CAT_SYSTEM, CAT_HARDWARE, CAT_GAMES, CAT_MEDIA,
    CAT_SETTINGS, NREALCAT
} appcat_t;

typedef struct {
    const char *id;        /* stable key for .favorites / .desktop   */
    const char *name;      /* menu label                             */
    const char *icon;      /* exactly 2 chars, safe VGA set only     */
    void      (*fn)(void);
    uint8_t     cat;
    /* ── DOES THIS APP LAY OUT FOR THE REAL GRID? ────────────────────
       0 for all of them unless stated, because all of them place text
       at absolute columns and rows for an 80x25 console -- v0.7.18's
       header says so and there are forty-three.

       An app with fits=0 is given an 80x25 VIEWPORT (see
       include/vga.h): it is told it has 80x25, its coordinates are
       translated into a centred box, and it lands in the middle of a
       big panel instead of the corner. It is not modified at all.

       fits=1 means the app derives its layout from vga_rows() and
       vga_cols() and should be handed the whole screen. Setting it on
       an app that does NOT is the one way to make this worse than it
       was: the app would draw an 80x25 layout into the top-left of
       the panel, which is the old behaviour, with no viewport to
       centre it. So it is set per app, deliberately, and only after
       looking. */
    uint8_t     fits;
} appreg_t;

static const appreg_t appreg[]={
    /* ---- Apps ------------------------------------------------- */
    /* fits=1: shell/shell_v076.c derives its row count from
       vga_rows() -- "The shell's preferred row count" -- so it uses
       the whole panel and must not be boxed. It is the only one in
       this table that does. */
    {"shell",     "Shell",            ">_", app_shell,          CAT_APPS, 1},
    {"edit",      "Text Editor",      "Ed", app_texteditor,     CAT_APPS},
    {"calc",      "Calculator",       "c=", app_calculator,     CAT_APPS},
    {"files",     "File Manager",     "FM", app_filemanager,    CAT_APPS},
    {"todo",      "Reminders",        "Rm", app_todo,           CAT_APPS},
    {"havencode", "HavenCode",        "Hc", app_boot_ide,       CAT_APPS},
    {"hex",       "Hex Viewer",       "Hx", app_hexviewer,      CAT_APPS},
    {"paint",     "Paint",            "Pt", appx_paint,         CAT_APPS},
    {"calendar",  "Calendar",         "Ca", appx_calendar,      CAT_APPS},
    {"chat",      "BOOT Chat",        "Ch", appx_chat,          CAT_APPS},
    {"asciiart",  "ASCII Art",        "Aa", appx_ascii_art,     CAT_APPS},

    /* ---- System ----------------------------------------------- */
    {"sysinfo",   "System Info",      "Si", app_sysinfo,        CAT_SYSTEM},
    {"sysinfop",  "System Info Plus", "S+", appx_sysinfo_plus,  CAT_SYSTEM},
    {"dashboard", "Dashboard",        "Db", app_dashboard,      CAT_SYSTEM},
    {"sysmon",    "System Monitor",   "Mn", app_sysmonitor,     CAT_SYSTEM},
    {"clock",     "Clock",            "Cl", app_clock,          CAT_SYSTEM},
    {"neofetch",  "Neofetch",         "Nf", appx_neofetch,      CAT_SYSTEM},
    {"notif",     "Notifications",    "Nt", notif_center,       CAT_SYSTEM},
    {"users",     "User Manager",     "Um", app_usermanager,    CAT_SYSTEM},
    {"audit",     "Login Audit Log",  "Al", app_logviewer,      CAT_SYSTEM},
    {"bpkg",      "Package Manager",  "Pk", app_bpkg,           CAT_SYSTEM},
    {"install",   "Install HavenDOS", "In", app_install,        CAT_SYSTEM},
    {"selftest",  "Self Test",        "Se", appx_selftest,      CAT_SYSTEM},

    /* ---- Hardware --------------------------------------------- */
    {"disk",      "Disk Info",        "Dk", app_diskinfo,       CAT_HARDWARE},
    {"net",       "Network Info",     "Nw", app_netinfo,        CAT_HARDWARE},
    {"pci",       "PCI Devices",      "Pc", appx_pci,           CAT_HARDWARE},
    /* bus-label-ok: the name of the USB Devices app in the start menu,
       not a disk's bus. Labelling a disk anywhere in this file still goes
       through blk_bus_name() -- see tools/check_bus_label.py, and see
       v0.8.5.3 for what this file got wrong when it did its own. */
    {"usb",       "USB Devices",      "Ub", appx_usb,           CAT_HARDWARE},
    {"bootlog",   "Boot Log",         "Bl", appx_bootlog,       CAT_HARDWARE},

    /* ---- Games ------------------------------------------------ */
    {"snake",     "Snake",            "Sn", app_snake,          CAT_GAMES},
    {"tetris",    "Tetris",           "Te", app_tetris_wrap,    CAT_GAMES},
    {"pong",      "Pong",             "||", app_pong,           CAT_GAMES},
    {"quiz",      "Quiz",             "Qz", appx_quiz,          CAT_GAMES},
    /* fits=1: shell/mines.c sizes its board from vga_cols()/vga_rows(),
       so a bigger console is a bigger minefield and it must NOT be
       boxed into 80x25. The only app besides the shell that does. */
    {"mines",     "Minefield",        "Mf", app_mines,          CAT_GAMES, 1},

    /* ---- Media ------------------------------------------------ */
    {"music",     "Music Player",     "Mu", appx_music,         CAT_MEDIA},
    {"matrix",    "Matrix",           "Mx", app_matrix,         CAT_MEDIA},
    {"stars",     "Starfield",        "**", appx_stars,         CAT_MEDIA},
    {"pipes",     "Pipes",            "Pi", app_pipes,          CAT_MEDIA},
    {"screensvr", "Screensaver",      "~~", app_screensaver,    CAT_MEDIA},
    {"hacker",    "Hacker Mode",      "Hk", appx_hacker,        CAT_MEDIA},
    {"fortune",   "Fortune",          "Fo", appx_fortune,       CAT_MEDIA},

    /* ---- Settings --------------------------------------------- */
    {"settings",  "Settings",         "St", app_settings,       CAT_SETTINGS},
    {"themes",    "Themes",           "Th", appx_themes,        CAT_SETTINGS},
    {"guide",     "User Guide",       "Gu", appx_guide,         CAT_SETTINGS},
    {"manual",    "Manual",           "Ma", appx_manual,        CAT_SETTINGS},
    {"legal",     "Terms and Privacy","Lg", appx_legal,         CAT_SETTINGS},
    {"power",     "Power and Reboot", "Pw", app_power,          CAT_SETTINGS},
};
#define NREG ((int)(sizeof(appreg)/sizeof(appreg[0])))

/* ---- Top-level start menu rows ------------------------------- */
#define TOP_FAV 0
#define TOP_CAT 1
#define TOP_ALL 2

typedef struct { const char *name; uint8_t kind; uint8_t cat; } top_item_t;

static const top_item_t topmenu[]={
    {" Favorites", TOP_FAV, 0},
    {" Apps",      TOP_CAT, CAT_APPS},
    {" System",    TOP_CAT, CAT_SYSTEM},
    {" Hardware",  TOP_CAT, CAT_HARDWARE},
    {" Games",     TOP_CAT, CAT_GAMES},
    {" Media",     TOP_CAT, CAT_MEDIA},
    {" Settings",  TOP_CAT, CAT_SETTINGS},
    {" All Apps",  TOP_ALL, 0},
};
#define NTOP ((int)(sizeof(topmenu)/sizeof(topmenu[0])))

/* ================================================================
   PINS  --  .favorites and .desktop
   ================================================================
   Kyan's call for 0.8.1: the desktop ships with Shell and nothing
   else, and the user decides what joins it.  That needs the choice
   to survive a reboot, so both lists are plain newline-separated id
   files in the VFS.

   An id that is no longer in the registry is dropped on load rather
   than rejected, so removing an app cannot leave a pin that points
   nowhere and crashes on Enter.
   ================================================================ */
#define PIN_MAX    24
#define PIN_IDLEN  16

static char fav_ids[PIN_MAX][PIN_IDLEN];   static int fav_n=0;
static char desk_ids[PIN_MAX][PIN_IDLEN];  static int desk_n=0;

static int reg_find(const char *id){
    for(int i=0;i<NREG;i++) if(strcmp(appreg[i].id,id)==0) return i;
    return -1;
}

static void pin_load(const char *path, char ids[][PIN_IDLEN], int *n,
                     const char *dflt){
    static char buf[512];
    *n=0;
    int r=vfs_read(path,buf,sizeof buf-1);
    if(r<=0){
        /* First boot, or the file was deleted: seed the default. */
        if(dflt&&dflt[0]&&reg_find(dflt)>=0){
            strncpy(ids[0],dflt,PIN_IDLEN-1); ids[0][PIN_IDLEN-1]=0; *n=1;
        }
        return;
    }
    buf[r]=0;
    char *p=buf;
    while(*p&&*n<PIN_MAX){
        char *e=p; while(*e&&*e!='\n'&&*e!='\r') e++;
        char saved=*e; *e=0;
        if(*p&&reg_find(p)>=0){            /* drop ids we no longer have */
            strncpy(ids[*n],p,PIN_IDLEN-1); ids[*n][PIN_IDLEN-1]=0; (*n)++;
        }
        *e=saved;
        p=e; while(*p=='\n'||*p=='\r') p++;
    }
}

static void pin_save(const char *path, char ids[][PIN_IDLEN], int n){
    static char buf[512];
    int o=0;
    for(int i=0;i<n;i++){
        int l=(int)strlen(ids[i]);
        if(o+l+1>=(int)sizeof buf) break;
        for(int j=0;j<l;j++) buf[o++]=ids[i][j];
        buf[o++]='\n';
    }
    buf[o]=0;
    vfs_write(path,buf,(uint32_t)o);
}

static int pin_index(char ids[][PIN_IDLEN], int n, const char *id){
    for(int i=0;i<n;i++) if(strcmp(ids[i],id)==0) return i;
    return -1;
}

/* Returns 1 if the app is now pinned, 0 if it was just unpinned,
   -1 if the list is full. */
static int pin_toggle(const char *path, char ids[][PIN_IDLEN], int *n,
                      const char *id){
    int at=pin_index(ids,*n,id);
    if(at>=0){
        for(int i=at;i<*n-1;i++)
            strncpy(ids[i],ids[i+1],PIN_IDLEN);
        (*n)--;
        pin_save(path,ids,*n);
        return 0;
    }
    if(*n>=PIN_MAX) return -1;
    strncpy(ids[*n],id,PIN_IDLEN-1); ids[*n][PIN_IDLEN-1]=0; (*n)++;
    pin_save(path,ids,*n);
    return 1;
}

/* The filenames are deliberately 8.3-legal, NOT dotfiles.
   fat16.c's name_to_83() converts ".desktop" to a BLANK basename with
   extension "DES": its first loop stops on the leading dot having
   written nothing, and the extension loop then consumes the rest.  A
   directory entry with an empty name is not findable, so the write
   appeared to succeed and every read came back empty -- the pins
   reverted to the seeded default on the next load.  (".loginlog", used
   by the audit log, has the same latent problem on a FAT16 install;
   left alone here because changing it would orphan existing logs, but
   it is worth a separate look.) */
#define PIN_FILE_FAV   "FAVORITE.CFG"
#define PIN_FILE_DESK  "DESKTOP.CFG"

static void pins_load(void){
    pin_load(PIN_FILE_FAV, fav_ids, &fav_n, "shell");
    pin_load(PIN_FILE_DESK,desk_ids,&desk_n,"shell");
}

/* ---- Views ---------------------------------------------------
   reg_fill() writes registry indices for one start-menu row into
   `out` and returns the count.  This is the only place a view is
   computed, so a category and All Apps cannot disagree about what
   exists. */
static int reg_fill(int top, int *out){
    int n=0;
    if(topmenu[top].kind==TOP_FAV){
        for(int i=0;i<fav_n;i++){
            int r=reg_find(fav_ids[i]);
            if(r>=0) out[n++]=r;
        }
    } else if(topmenu[top].kind==TOP_ALL){
        for(int i=0;i<NREG;i++) out[n++]=i;
    } else {
        for(int i=0;i<NREG;i++)
            if(appreg[i].cat==topmenu[top].cat) out[n++]=i;
    }
    return n;
}

/* Desktop icon grid: whatever the user pinned, nothing more. */
static int desk_fill(int *out){
    int n=0;
    for(int i=0;i<desk_n;i++){
        int r=reg_find(desk_ids[i]);
        if(r>=0) out[n++]=r;
    }
    return n;
}

/* ================================================================
   DESKTOP DRAW
   ================================================================ */

/* Icons: 2-column grid with scroll support.
 * scroll_row = first app-row to display (0-based).
 * Each app row is TILE_STEP=4 screen rows tall.
 * Visible content: rows 2..20 = 19 screen rows → 4 app-rows visible. */
#define ICON_TILE_STEP  4   /* screen rows per app row (icon+name+dot+gap) */
#define ICON_PITCH     17   /* screen columns per app column             */


/* v0.7.18 — the desktop chrome used to be nailed to rows 21-24 and 80
   columns, so desktop_run() force-switched the display to 80x25 and the
   shell put the user's mode back afterwards.  The chrome now derives its
   geometry from the active mode.  At 80x25 every expression below evaluates
   to the number it replaced, so the familiar layout is unchanged; at 80x50
   or 90x60 the bars sit on the real bottom rows and the icon grid grows.

   Apps launched FROM the desktop still lay themselves out for 80x25 and
   render in the top-left of a larger screen -- converting those is a
   separate pass and is not claimed here. */
#define D_ROWS        vga_rows()
#define D_COLS        vga_cols()
#define TASKBAR_Y     (D_ROWS-1)   /* 24 at 25 rows */
#define INFO_SEP_Y    (D_ROWS-4)   /* 21            */
#define INFO_Y        (D_ROWS-3)   /* 22            */
#define ICON_TOP      2
#define ICON_BOT      (INFO_SEP_Y-1)
#define ICON_ROWS_VIS ((ICON_BOT-ICON_TOP+1)/ICON_TILE_STEP)

/* ── HOW MANY ICON COLUMNS FIT, v0.8.8.2 ─────────────────────────────
   This was the literal 2, in five places: `i/2` and `i%2` in
   draw_icons(), in icon_hit(), in the context-menu anchor, and in
   the arrow-key moves. Two columns of a 17-cell pitch is 34 cells,
   so on an 80-column console the right 45 columns of the desktop
   were empty and on a 120-column one it was 85 -- which is most of
   what "some parts unoptimized for the screen sizes" means.

   The chrome around it already scaled: TASKBAR_Y, INFO_Y and
   ICON_ROWS_VIS all come from vga_rows() and vga_cols(). The icon
   FIELD did not.

   One function, because icon_hit() and draw_icons() disagreeing
   about where an icon is means clicking one and launching another --
   the same reason reg_fill() is the only place a menu view is
   computed. */
static int icon_cols(void){
    int c = (D_COLS - 1) / ICON_PITCH;
    if(c < 1) c = 1;                 /* a console too narrow for one
                                        still gets one, clipped      */
    return c;
}


static int icon_scroll_row = 0; /* file-scope so hit-test can read it */

/* The icon grid is the user's own shortlist now, not a fixed table.
   0.8.1: HavenDOS used to ship seventeen icons chosen here, which is
   a decision that belongs to whoever is using the machine.  The grid
   is .desktop -- Shell on a fresh install, and whatever the user
   pins from the start menu after that.  An empty grid says so and
   says where the apps are, because a blank left half of the screen
   otherwise looks like a failed draw. */
static int  icon_items[PIN_MAX];
static int  icon_count=0;

static void icons_refresh(void){ icon_count=desk_fill(icon_items); }

/* Which grid slot is at (mx,my), or -1.  Lifted out of the click handler
   so the left-click and right-click paths cannot disagree about where an
   icon is -- the same reason reg_fill() is the only place a menu view is
   computed. */
static int icon_hit(int mx,int my){
    for(int i=0;i<icon_count;i++){
        int nc=icon_cols();
        int approw=i/nc, icol=i%nc;
        int vis_row=approw-icon_scroll_row;
        if(vis_row<0||vis_row>=ICON_ROWS_VIS) continue;
        int ix=1+icol*ICON_PITCH, iy=2+vis_row*ICON_TILE_STEP;
        if(mx>=ix&&mx<ix+15&&my>=iy&&my<iy+2) return i;
    }
    return -1;
}

static void draw_icons(int sel, int menu_open){
    theme_t *th=ui_theme();

    int rows_vis=ICON_ROWS_VIS;
    int ncols=icon_cols();
    int field_w=ncols*ICON_PITCH;      /* the icon area, in cells */

    /* Keep the selection in view */
    int sel_approw = sel / ncols;
    if(sel_approw < icon_scroll_row) icon_scroll_row = sel_approw;
    if(sel_approw >= icon_scroll_row + rows_vis)
        icon_scroll_row = sel_approw - rows_vis + 1;
    if(icon_scroll_row<0) icon_scroll_row=0;

    /* Clear the icon area */
    /* Clear exactly the field, which is now as wide as the console
       allows rather than a hardcoded 35. */
    for(int sy=ICON_TOP; sy<=ICON_BOT; sy++)
        for(int sx=0; sx<=field_w && sx<D_COLS; sx++)
            vga_putchar_at(' ', sx, sy, th->desk_bg, th->desk_bg);

    if(!icon_count){
        vga_puts_at("No desktop shortcuts.",2,3,th->bar_fg,th->desk_bg);
        vga_puts_at("Windows key or ESC opens",2,5,th->bar_fg,th->desk_bg);
        vga_puts_at("the start menu;",2,6,th->bar_fg,th->desk_bg);
        vga_puts_at("press D on an app there",2,7,th->bar_fg,th->desk_bg);
        vga_puts_at("to put it here.",2,8,th->bar_fg,th->desk_bg);
        return;
    }

    for(int i=0; i<icon_count; i++){
        int approw = i / ncols;
        int col    = i % ncols;

        if(approw < icon_scroll_row) continue;
        int vis_row = approw - icon_scroll_row;
        if(vis_row >= rows_vis) break;

        const appreg_t *a=&appreg[icon_items[i]];
        int x = 1 + col * ICON_PITCH;
        int y = 2 + vis_row * ICON_TILE_STEP;

        int s=(i==sel&&!menu_open);
        vga_color_t ibg=s?th->accent:th->icon_bg;
        vga_color_t ifg=s?th->icon_bg:th->icon_fg;
        vga_color_t nfg=s?th->accent:th->bar_fg;

        vga_puts_at("---------------",x,y,th->bar_fg,th->desk_bg);
        vga_putchar_at('-',x,y,th->accent,th->desk_bg);
        vga_putchar_at(a->icon[0],x+1,y,ifg,ibg);
        vga_putchar_at(a->icon[1],x+2,y,ifg,ibg);
        vga_putchar_at('-',x+3,y,th->accent,th->desk_bg);

        char nm[15]; int nl=(int)strlen(a->name);
        for(int j=0;j<14;j++) nm[j]=(j<nl)?a->name[j]:' ';
        nm[14]=0;
        vga_puts_at(nm,x,y+1,nfg,th->desk_bg);

        vga_putchar_at('.',x+6,y+2,th->icon_bg,th->desk_bg);
    }

    int total_rows = (icon_count + ncols - 1)/ncols;
    if(total_rows > rows_vis){
        int bar_x = field_w < D_COLS ? field_w : D_COLS-1;
        vga_putchar_at(icon_scroll_row>0?'^':' ', bar_x, 2,
                       th->bar_fg, th->desk_bg);
        vga_putchar_at((icon_scroll_row+rows_vis)<total_rows?'v':' ',
                       bar_x, 2+rows_vis*ICON_TILE_STEP-1,
                       th->bar_fg, th->desk_bg);
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
    /* Separator line */
    for(int x=0;x<D_COLS;x++) vga_putchar_at('-',x,INFO_SEP_Y,th->icon_bg,th->desk_bg);
    /* Info row */
    for(int x=0;x<D_COLS;x++) vga_putchar_at(' ',x,INFO_Y,th->desk_bg,th->desk_bg);
    vga_puts_at("RAM:",1,INFO_Y,th->bar_fg,th->desk_bg);
    vga_puts_at(rb,5,INFO_Y,VGA_LIGHT_GREEN,th->desk_bg);
    vga_puts_at("Up:",11,INFO_Y,th->bar_fg,th->desk_bg);
    vga_puts_at(us,14,INFO_Y,VGA_LIGHT_GREY,th->desk_bg);
    /* Right side hint, right-anchored so it lands correctly in any width.
       A live session says so here instead: someone who pins an app to the
       desktop and finds it gone after a reboot should have been told why
       before they did it, not after. */
    {
        extern int boot_is_live(void);
        const char *hint = boot_is_live()
            ? "LIVE - no disk is touched, nothing is saved"
            : "Tap icon to open  Right-tap for menu";
        int hx=D_COLS-(int)strlen(hint)-1; if(hx<20) hx=20;
        vga_puts_at(hint,hx,INFO_Y,
                    boot_is_live()?VGA_YELLOW:th->icon_bg, th->desk_bg);
    }
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
   START MENU  --  rises from bottom-left above the taskbar
   ================================================================

   Geometry, 0.8.1
   ---------------
   The panel used to be nailed to a fixed eleven rows -- MENU_BOT-10
   -- with the category rows at 17..20 and the Lock row at 22.  Two
   consequences, both now gone:

     * A fifth category landed on the separator at row 21.  The panel
       height derives from NTOP now, so the menu grew from four rows
       to eight without a single magic number changing.  At the old
       NTOP=4 the expression still evaluates to MENU_BOT-10, so the
       familiar layout is byte-identical.

     * The username, the two separators and the Lock row were drawn
       at literal rows 14, 15, 16 and 21 while the panel itself was
       computed from vga_rows().  At 80x25 those agree.  At 80x50 or
       90x60 the panel sits at the bottom of a taller screen and the
       header rendered in the MIDDLE OF THE DESKTOP, a third of the
       way up, with nothing around it.  Every row is MENU_TOP- or
       MENU_BOT-relative now, so the panel is one object at any mode.

   The submenu gets its own top at SUB_TOP, independent of the left
   panel, because the left panel is only as tall as it needs to be
   for NTOP while the submenu may hold all 45 apps.  That is about
   eighteen visible rows at 80x25, which is every category except
   All Apps with no scrolling at all.
   ================================================================ */
#define MENU_W      22                   /* left panel width            */
#define MENU_BOT    (TASKBAR_Y-1)        /* bottom border, above the bar */
#define MENU_TOP    (MENU_BOT-(NTOP+6))  /* border+user+logged+sep
                                            +NTOP+sep+lock+border       */
#define MENU_ITEM_Y (MENU_TOP+4)         /* first category row          */
#define MENU_LOCK_Y (MENU_BOT-1)         /* lock row                    */
#define MENU_SEP_Y  (MENU_LOCK_Y-1)      /* separator above lock        */

#define SUB_W       32                   /* flyout width incl. markers  */
#define SUB_X       MENU_W
#define SUB_TOP     2                    /* flyout grows up to row 2    */
#define SUB_ITEM_Y  (SUB_TOP+3)          /* border+label+sep            */
#define SUB_VIS     (MENU_BOT-SUB_ITEM_Y)/* visible item rows           */

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
    vga_puts_at(uname,1,MENU_TOP+1,th->accent,th->win_bg);
    if(current_user_admin())
        vga_puts_at(".admin",1+(int)strlen(uname),MENU_TOP+1,th->bar_fg,th->win_bg);
    vga_puts_at("Logged in",1,MENU_TOP+2,th->bar_fg,th->win_bg);

    /* Separator after header */
    for(int x=0;x<MENU_W;x++)
        vga_putchar_at('-',x,MENU_TOP+3,th->icon_bg,th->win_bg);

    /* Category rows */
    for(int i=0;i<NTOP;i++){
        int iy=MENU_ITEM_Y+i;
        int s=(i==top_sel);
        vga_color_t bg=s?th->accent:th->win_bg;
        vga_color_t fg=s?th->win_bg:th->win_fg;
        vga_putchar_at(s?'>':' ',0,iy,fg,bg);
        char row[MENU_W]; int j=0;
        const char *n=topmenu[i].name;
        while(*n&&j<MENU_W-4) row[j++]=*n++;
        while(j<MENU_W-4) row[j++]=' ';
        row[j]=0;
        vga_puts_at(row,1,iy,fg,bg);
        vga_putchar_at('>',MENU_W-2,iy,s?th->win_bg:th->icon_bg,bg);
        vga_putchar_at(' ',MENU_W-1,iy,fg,bg);
    }

    /* Separator before lock */
    for(int x=0;x<MENU_W;x++)
        vga_putchar_at('-',x,MENU_SEP_Y,th->icon_bg,th->win_bg);

    /* Lock row */
    int ls=(menu_state==4);
    vga_color_t lbg=ls?th->accent:th->win_bg;
    vga_color_t lfg=ls?th->win_bg:th->win_fg;
    vga_putchar_at(ls?'>':' ',0,MENU_LOCK_Y,lfg,lbg);
    { char row[MENU_W]; int j=0;
      const char *n="Lock";
      while(*n&&j<MENU_W-2) row[j++]=*n++;
      while(j<MENU_W-2) row[j++]=' ';
      row[j]=0;
      vga_puts_at(row,1,MENU_LOCK_Y,lfg,lbg); }

    /* Bottom border */
    vga_putchar_at('.',0,MENU_BOT,th->accent,th->win_bg);
    for(int x=1;x<MENU_W-1;x++) vga_putchar_at('-',x,MENU_BOT,th->accent,th->win_bg);
    vga_putchar_at('.',MENU_W-1,MENU_BOT,th->accent,th->win_bg);
}

static int sub_scroll=0;        /* first visible item index */
static int sub_items[64];       /* registry indices for the open category */
static int sub_count=0;

/* Recompute the open category's item list.  Called whenever top_sel
   changes or a pin is toggled, never from inside the draw -- a view
   that rebuilds itself while being painted is how a selection index
   ends up pointing past the end of its own list. */
static void sub_refresh(void){
    sub_count=reg_fill(top_sel,sub_items);
    if(sub_sel>=sub_count) sub_sel=sub_count?sub_count-1:0;
    if(sub_sel<0) sub_sel=0;
    if(sub_scroll>sub_count-1) sub_scroll=0;
}

static void draw_submenu(void){
    theme_t *th=ui_theme();

    /* Keep the selection in view */
    if(sub_sel<sub_scroll) sub_scroll=sub_sel;
    if(sub_sel>=sub_scroll+SUB_VIS) sub_scroll=sub_sel-SUB_VIS+1;
    if(sub_scroll<0) sub_scroll=0;

    for(int y=SUB_TOP;y<=MENU_BOT;y++)
        for(int x=SUB_X;x<SUB_X+SUB_W;x++)
            vga_putchar_at(' ',x,y,th->win_fg,th->win_bg);

    /* Top border */
    vga_putchar_at('.',SUB_X,SUB_TOP,th->accent,th->win_bg);
    for(int x=SUB_X+1;x<SUB_X+SUB_W-1;x++)
        vga_putchar_at('-',x,SUB_TOP,th->accent,th->win_bg);
    vga_putchar_at('.',SUB_X+SUB_W-1,SUB_TOP,th->accent,th->win_bg);

    /* Category label, with a count so an empty Favorites reads as
       empty rather than broken */
    vga_puts_at(topmenu[top_sel].name,SUB_X+1,SUB_TOP+1,th->accent,th->win_bg);
    { char cnt[8]; int v=sub_count,j=0;
      if(!v) cnt[j++]='0';
      else { char t[8]; int k=0; while(v){t[k++]=(char)('0'+v%10);v/=10;}
             while(k) cnt[j++]=t[--k]; }
      cnt[j]=0;
      vga_puts_at(cnt,SUB_X+SUB_W-2-(int)strlen(cnt),SUB_TOP+1,
                  th->bar_fg,th->win_bg); }
    for(int x=SUB_X;x<SUB_X+SUB_W;x++)
        vga_putchar_at('-',x,SUB_TOP+2,th->icon_bg,th->win_bg);

    if(!sub_count){
        vga_puts_at("Nothing pinned yet.",SUB_X+2,SUB_ITEM_Y+1,
                    th->bar_fg,th->win_bg);
        vga_puts_at("Press F on any app to",SUB_X+2,SUB_ITEM_Y+3,
                    th->bar_fg,th->win_bg);
        vga_puts_at("add it here.",SUB_X+2,SUB_ITEM_Y+4,
                    th->bar_fg,th->win_bg);
    }

    /* Items.  Two marker columns on the right: D = on the desktop,
       F = a favorite.  Favorites itself does not show F, which would
       be true of every row there and carry no information. */
    for(int i=0;i<SUB_VIS;i++){
        int idx=i+sub_scroll;
        if(idx>=sub_count) break;
        int r=sub_items[idx];
        int iy=SUB_ITEM_Y+i;
        int s=(idx==sub_sel);
        vga_color_t bg=s?th->accent:th->win_bg;
        vga_color_t fg=s?th->win_bg:th->win_fg;
        vga_putchar_at(s?'>':' ',SUB_X,iy,fg,bg);
        /* A leading space: the '>' cursor sits at SUB_X and the name began
           at SUB_X+1, so a selected row read ">Paint".  The left panel gets
           away without this only because its own labels are written with a
           leading space in topmenu[]. */
        char row[SUB_W]; int j=0;
        const char *n=appreg[r].name;
        row[j++]=' ';
        while(*n&&j<SUB_W-5) row[j++]=*n++;
        while(j<SUB_W-5) row[j++]=' ';
        row[j]=0;
        vga_puts_at(row,SUB_X+1,iy,fg,bg);

        int on_desk=pin_index(desk_ids,desk_n,appreg[r].id)>=0;
        int is_fav =pin_index(fav_ids,fav_n,appreg[r].id)>=0;
        vga_putchar_at(on_desk?'D':' ',SUB_X+SUB_W-4,iy,
                       s?th->win_bg:th->accent,bg);
        vga_putchar_at((is_fav&&topmenu[top_sel].kind!=TOP_FAV)?'F':' ',
                       SUB_X+SUB_W-3,iy,s?th->win_bg:th->accent,bg);
        vga_putchar_at(' ',SUB_X+SUB_W-2,iy,fg,bg);
        vga_putchar_at(' ',SUB_X+SUB_W-1,iy,fg,bg);
    }

    /* Scroll indicators */
    if(sub_scroll>0)
        vga_putchar_at('^',SUB_X+SUB_W-2,SUB_ITEM_Y,th->accent,th->win_bg);
    if(sub_scroll+SUB_VIS<sub_count)
        vga_putchar_at('v',SUB_X+SUB_W-2,SUB_ITEM_Y+SUB_VIS-1,
                       th->accent,th->win_bg);

    /* Bottom border, with the pin keys spelled out on it -- the only
       place the user finds out D and F do anything */
    vga_putchar_at('.',SUB_X,MENU_BOT,th->accent,th->win_bg);
    for(int x=SUB_X+1;x<SUB_X+SUB_W-1;x++)
        vga_putchar_at('-',x,MENU_BOT,th->accent,th->win_bg);
    vga_putchar_at('.',SUB_X+SUB_W-1,MENU_BOT,th->accent,th->win_bg);
    vga_puts_at(" D=desktop F=fav M=menu ",SUB_X+2,MENU_BOT,
                th->bar_fg,th->win_bg);
}

/* ================================================================
   RIGHT-CLICK QUICK-ACCESS MENU  (v0.7.4)
   Tiny 2-item flyout (Settings / Display) anchored at the click
   point, clamped to stay on-screen. Reuses the exact border
   glyphs already proven on UTM SE in draw_submenu() above rather
   than introducing any new extended character.
   ================================================================ */
/* 0.8.1: the menu is built for whatever was right-clicked.
   It used to be a fixed Settings/Display pair wherever you clicked, so
   the only way to unpin a desktop shortcut was the `-` key, which is not
   something anyone finds without being told.  Right-clicking an icon or
   a start-menu entry now offers the pin actions on the thing under the
   cursor; right-clicking bare desktop still gives the original two. */
#define CTXM_W      20
#define CTXM_MAX     5
#define CTXM_H      (ctx_nitems+2)

/* What a row does.  Numbered rather than a function pointer because two
   of them need the app the menu was opened on. */
#define CTXA_SETTINGS  0
#define CTXA_DISPLAY   1
#define CTXA_OPEN      2
#define CTXA_DESKTOP   3
#define CTXA_FAVORITE  4

static void desktop_run_app(void (*fn)(void), const char *name, int fits);  /* with the main loop */

static int         ctx_app = -1;        /* registry index, or -1 for bare desktop */
static const char *ctx_labels[CTXM_MAX];
static int         ctx_action[CTXM_MAX];
static int         ctx_nitems = 0;

/* Build the menu for a registry index (or -1).  Called at open time, and
   again after a pin toggle so the label flips with the state it reports. */
static void ctx_build(int app){
    ctx_app   = app;
    ctx_nitems = 0;
    if(app >= 0 && app < NREG){
        int on_desk = pin_index(desk_ids,desk_n,appreg[app].id) >= 0;
        int is_fav  = pin_index(fav_ids, fav_n, appreg[app].id) >= 0;
        ctx_labels[ctx_nitems]="Open";
        ctx_action[ctx_nitems++]=CTXA_OPEN;
        ctx_labels[ctx_nitems]= on_desk ? "Remove shortcut" : "Add to desktop";
        ctx_action[ctx_nitems++]=CTXA_DESKTOP;
        ctx_labels[ctx_nitems]= is_fav  ? "Unfavorite"      : "Add to favorites";
        ctx_action[ctx_nitems++]=CTXA_FAVORITE;
    }
    ctx_labels[ctx_nitems]="Settings";
    ctx_action[ctx_nitems++]=CTXA_SETTINGS;
    ctx_labels[ctx_nitems]="Display";
    ctx_action[ctx_nitems++]=CTXA_DISPLAY;
}

/* Runs the action for a selected context-menu item. Shared by
   both the mouse-click and Enter-key activation paths so the
   behavior can't drift between the two.
   Returns 1 if the menu should stay open (a pin toggle, so the user can
   see the label flip and keep going), 0 if it should close. */
static int ctx_menu_activate(int sel){
    if(sel < 0 || sel >= ctx_nitems) return 0;
    switch(ctx_action[sel]){
    case CTXA_SETTINGS:
        app_settings();
        return 0;
    case CTXA_DISPLAY: {
        int nt=(ui_current_theme()+1)%ui_theme_count();
        ui_set_theme(nt);
        char msg[32]="Theme: ";
        strcat(msg,ui_theme_name(nt));
        toast_info(msg);
        return 0;
    }
    case CTXA_OPEN:
        if(ctx_app>=0) desktop_run_app(appreg[ctx_app].fn, appreg[ctx_app].name, appreg[ctx_app].fits);
        return 0;
    case CTXA_DESKTOP: {
        if(ctx_app<0) return 0;
        int r=pin_toggle(PIN_FILE_DESK,desk_ids,&desk_n,appreg[ctx_app].id);
        icons_refresh();
        if(r<0) toast_warning("Desktop is full");
        else    toast_info(r?"Added to desktop":"Removed from desktop");
        ctx_build(ctx_app);          /* the label it just acted on has flipped */
        return 1;
    }
    case CTXA_FAVORITE: {
        if(ctx_app<0) return 0;
        int r=pin_toggle(PIN_FILE_FAV,fav_ids,&fav_n,appreg[ctx_app].id);
        if(r<0) toast_warning("Favorites is full");
        else    toast_info(r?"Added to favorites":"Removed from favorites");
        sub_refresh();               /* Favorites is a view of this list */
        ctx_build(ctx_app);
        return 1;
    }
    }
    return 0;
}

static void draw_contextmenu(void){
    theme_t *th=ui_theme();
    int x=ctx_x, y=ctx_y;

    /* Top border */
    vga_putchar_at('.',x,y,th->accent,th->win_bg);
    for(int i=1;i<CTXM_W-1;i++) vga_putchar_at('-',x+i,y,th->accent,th->win_bg);
    vga_putchar_at('.',x+CTXM_W-1,y,th->accent,th->win_bg);

    /* Items */
    for(int i=0;i<ctx_nitems;i++){
        int s=(i==ctx_sel);
        vga_color_t bg=s?th->accent:th->win_bg;
        vga_color_t fg=s?th->win_bg:th->win_fg;
        vga_putchar_at('|',x,y+1+i,th->accent,th->win_bg);
        vga_putchar_at(s?'>':' ',x+1,y+1+i,fg,bg);
        char row[CTXM_W-3]; int j=0;
        const char *n=ctx_labels[i];
        while(*n&&j<CTXM_W-4) row[j++]=*n++;
        while(j<CTXM_W-4) row[j++]=' ';
        row[j]=0;
        vga_puts_at(row,x+2,y+1+i,fg,bg);
        vga_putchar_at('|',x+CTXM_W-1,y+1+i,th->accent,th->win_bg);
    }

    /* Bottom border */
    vga_putchar_at('.',x,y+1+ctx_nitems,th->accent,th->win_bg);
    for(int i=1;i<CTXM_W-1;i++) vga_putchar_at('-',x+i,y+1+ctx_nitems,th->accent,th->win_bg);
    vga_putchar_at('.',x+CTXM_W-1,y+1+ctx_nitems,th->accent,th->win_bg);
}

static void full_draw(int icon_sel, int m_state, int t_sel, int s_sel){
    (void)t_sel; (void)s_sel;
    vga_begin_frame();          /* v0.7.6: compose the whole frame off-screen */
    ui_erase_cursor();
    ui_draw_desktop();
    ui_draw_desktop_bars();
    draw_icons(icon_sel, m_state!=0);
    draw_infopanel();
    if(m_state==1||m_state==2) draw_topmenu();
    if(m_state==2) draw_submenu();
    if(m_state==3) draw_contextmenu();
    if(mouse_is_ready()) ui_draw_cursor();
    vga_end_frame();            /* commit in a single pass */
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
        ui_draw_window(16,2,48,21,"Calculator  " HAVENDOS_VER);

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
            while(rl<46) row[rl++]=' ';
            row[46]=0;
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
        while(dl<46) disp[dl++]=' ';
        disp[46]=0;
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
/* v0.7.19 — these were compile-time 21 and 74, pinned to 80x25.  In 80x50,
   90x30 or 90x60 the editor therefore drew a 21x74 box in the top-left of the
   screen and left the rest of it holding whatever was there before, which is
   the "desktop apps render top-left instead of centred" report.
   ed_vrows()/ed_vcols() reproduce 21 and 74 EXACTLY at 80x25, so the familiar
   layout does not shift; tests/test_edgeom.c checks that. */
#define ED_VROWS       (ed_vrows())
#define ED_VCOLS       (ed_vcols())
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

/* Rows 0 and 1 are the desktop status bar and the editor title bar; the last
   two rows are the status and hint bars.  Everything between is text. */
int ed_vrows(void)
{
    int r = vga_rows() - 4;
    if(r < 4)       r = 4;              /* a 8-row mode would be unusable */
    if(r > ED_ROWS) r = ED_ROWS;
    return r;
}

/* The line-number gutter takes ED_LNPAD, and the last cell is left alone. */
int ed_vcols(void)
{
    int c = vga_cols() - ED_LNPAD - 1;
    if(c < 16)      c = 16;
    if(c > ED_COLS) c = ED_COLS;        /* the buffer is only this wide */
    return c;
}
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
    /* Title bar.  W and the two bar rows come from the active mode; at 80x25
       they evaluate to the columns this code used to hard-code. */
    const int W  = vga_cols();
    const int SB = vga_rows() - 2;      /* status bar */
    const int HB = vga_rows() - 1;      /* hint bar   */
    for(int x=0;x<W;x++) vga_putchar_at(' ',x,1,VGA_WHITE,VGA_BLUE);
    vga_puts_at(" Notepad",0,1,VGA_WHITE,VGA_BLUE);
    if(ed_filename[0]) vga_puts_at(ed_filename,12,1,VGA_YELLOW,VGA_BLUE);
    else               vga_puts_at("[Untitled]",12,1,VGA_LIGHT_GREY,VGA_BLUE);
    if(ed_dirty)       vga_putchar_at('*',11,1,VGA_LIGHT_RED,VGA_BLUE);
    vga_puts_at(ed_insert?"[INS]":"[OVR]",W-7,1,VGA_LIGHT_CYAN,VGA_BLUE);
    if(ed_sel_active)  vga_puts_at("[SEL]",W-13,1,VGA_YELLOW,VGA_BLUE);

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
    for(int x=0;x<W;x++) vga_putchar_at(' ',x,SB,VGA_BLACK,VGA_CYAN);
    char pos[32]; char rn[8]; char cn[8];
    utoa((uint32_t)(cur_row+1),rn,10); utoa((uint32_t)(cur_col+1),cn,10);
    strcpy(pos,"Ln:"); strcat(pos,rn); strcat(pos," Col:"); strcat(pos,cn);
    vga_puts_at(pos,1,SB,VGA_BLACK,VGA_CYAN);
    if(ed_filename[0]) vga_puts_at(ed_filename,20,SB,VGA_BLACK,VGA_CYAN);
    else               vga_puts_at("Untitled",20,SB,VGA_BLACK,VGA_CYAN);
    vga_puts_at(ed_dirty?"*Modified*":"          ",40,SB,VGA_BLACK,VGA_CYAN);
    if(ed_sel_active)  vga_puts_at("SELECTION",52,SB,VGA_BLUE,VGA_YELLOW);

    /* Hint bar */
    for(int x=0;x<W;x++) vga_putchar_at(' ',x,HB,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("^S=Save ^W=SaveAs ^Q=Quit ^C=Copy ^X=Cut ^V=Paste ^A=All",1,HB,VGA_DARK_GREY,VGA_BLACK);

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
        int pr = vga_rows() - 2;
        for(int x=0;x<vga_cols();x++) vga_putchar_at(' ',x,pr,VGA_BLACK,VGA_CYAN);
        vga_puts_at("Save as: ",0,pr,VGA_BLACK,VGA_CYAN);
        vga_set_cursor(9,pr);
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
        int pr = vga_rows() - 2;
        for(int x=0;x<vga_cols();x++) vga_putchar_at(' ',x,pr,VGA_BLACK,VGA_CYAN);
        vga_puts_at("Saved.",0,pr,VGA_BLACK,VGA_CYAN);
        if(!vfs_using_disk())   /* v0.9.3: was !fat16, wrong on FAT32 */
            vga_puts_at("(RAMFS - lost on reboot, no persistent disk)",10,23,VGA_YELLOW,VGA_CYAN);
        sleep_ms(1400);
    } else {
        /* Save genuinely failed — do NOT clear the dirty flag, and
           make this loud rather than silently pretending it worked.
           Held on screen deliberately longer than a success message
           since this is the case the person actually needs to see. */
        int pr = vga_rows() - 2;
        for(int x=0;x<vga_cols();x++) vga_putchar_at(' ',x,pr,VGA_BLACK,VGA_RED);
        vga_puts_at(" SAVE FAILED - changes NOT written to disk! Try again. ",0,pr,VGA_WHITE,VGA_RED);
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
                /* ── WAIT FOR RELEASE, BUT NOT FOREVER ───────────────
                   This was `do { mouse_get(&mx,&my,&mb); } while(mb&2);`
                   and it was an unbounded wait on a hardware event with
                   no way out of it. With a USB mouse it never ended at
                   all, because nothing in the loop polled USB -- fixed
                   in drivers/mouse.c, where mouse_get() now drains it.

                   The bound stays anyway. A mouse unplugged with its
                   button down, or a packet lost mid-report, leaves a
                   button stuck down, and a loop that waits for a thing
                   that is not coming is a frozen machine either way.
                   Two seconds at 100 Hz, and a keypress gets out. */
                int guard = 200;
                do {
                    mouse_get(&mx,&my,&mb);
                    if(keyboard_getchar() == 27) break;   /* ESC */
                    sleep_ms(10);
                } while((mb&2) && --guard > 0);
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
                int pr = vga_rows() - 2;
                for(int x=0;x<vga_cols();x++) vga_putchar_at(' ',x,pr,VGA_BLACK,VGA_RED);
                vga_puts_at(" Unsaved! Q=Discard  S=Save  ESC=Cancel",0,pr,VGA_WHITE,VGA_RED);
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
    for(int x=8;x<vga_cols();x++) vga_putchar_at(' ',x,1,VGA_WHITE,VGA_BLUE);

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
        for(int j=0;j<27;j++) nm[j]=j<nl?fm_names[idx][j]:' ';
        nm[27]=0;
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
    ROW("OS:",          "HavenDOS " HAVENDOS_VER,        VGA_LIGHT_CYAN);
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
            while(j<36)row[j++]=' ';
            row[36]=0;
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

    /* v0.8.5.6: the theme as it was BEFORE any preview.
       ui_load_preview() overwrites cur_theme, so ui_current_theme()
       cannot be used to undo a preview -- it returns the previewed
       value. This tab used to cancel with
           ui_set_theme(ui_current_theme())
       which reads the clobbered value and WRITES IT TO DISK, so ESC
       committed the theme you were last hovering over instead of
       cancelling. Tab did the same. shell/apps_extra.c's picker got
       this right by saving the entry value first; this one did not.
       Three separate theme pickers, three different behaviours. */
    int theme_start = ui_current_theme();
    /* And open on the active theme rather than on theme 0, which is the
       other half of the same bug: starting at 0 meant one Down keypress
       previewed theme 1, and ESC then committed it. */
    sel = theme_start;

    while(1){
        vga_clear(); ui_draw_taskbar("Settings");
        ui_draw_window(4,2,72,21,"Settings  " HAVENDOS_VER);

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
            /* Cancel restores in memory and writes nothing. */
            if(c==27){ ui_load_preview(theme_start); return; }
            if(c=='\t'){
                ui_load_preview(theme_start);
                tab=(tab+1)%ntabs;
                sel=(tab==0)?ui_current_theme():0;
                theme_scroll=0;
                continue;
            }
            if(c==0x148&&sel>0){ sel--; ui_load_preview(sel); }
            if(c==0x150&&sel<tc-1){ sel++; ui_load_preview(sel); }
            /* Enter is the only thing that commits, and it moves the
               baseline so a later ESC reverts to what was applied
               rather than to what was active on entry. */
            if(c=='\n'||c=='\r'){ ui_set_theme(sel); theme_start=sel; }
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
            /* The POST screen is gone -- it announced a power-on self test
               that the real BIOS had already performed before GRUB loaded
               the kernel.  What is left is one honest switch. */
            bopt bopts[]={
                {"Verbose boot log",     cfg_verbose_boot()},
            };
            int nbopts=1;
            for(int i=0;i<nbopts;i++){
                int s=(i==sel);
                vga_putchar_at(s?'>':' ',7,8+i,s?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);
                vga_puts_at(bopts[i].lbl,9,8+i,VGA_WHITE,VGA_BLACK);
                vga_puts_at(bopts[i].val?"[ON] ":"[OFF]",40,8+i,
                            bopts[i].val?VGA_LIGHT_GREEN:VGA_YELLOW,VGA_BLACK);
            }
            vga_puts_at("The log prints as each driver starts. 'dmesg' replays it",
                        7,12,VGA_DARK_GREY,VGA_BLACK);
            vga_puts_at("either way; 'quiet' on the GRUB line silences it.",
                        7,13,VGA_DARK_GREY,VGA_BLACK);
            vga_puts_at("Changes take effect on next boot.",7,15,VGA_DARK_GREY,VGA_BLACK);

            int c=keyboard_waitchar();
            if(c==27)return;
            if(c=='\t'){tab=(tab+1)%ntabs;sel=0;continue;}
            if(c==0x148&&sel>0)sel--;
            if(c==0x150&&sel<nbopts-1)sel++;
            if(c=='\n'||c=='\r'){
                if(sel==0){cfg_set_verbose_boot(!cfg_verbose_boot());cfg_save();}
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
            vga_puts_at("HavenDOS " HAVENDOS_VER,7,6,VGA_LIGHT_CYAN,VGA_BLACK);
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
        /* v0.9.4.1: waits FOR a tick, up to a bound, instead of for a
           fixed 500 thousand pauses. A pause is ten cycles on some CPUs and
           a hundred on others, so the old loop could finish inside one
           10 ms tick on a fast machine and call a working timer broken --
           it did, in every 1.0 promo run, under QEMU. 200 million pauses
           is seconds on anything; a timer that has not ticked by then is
           really stopped. */
        uint32_t t0=get_ticks();
        volatile uint32_t w=200000000u;
        while(w && get_ticks()==t0){ w--; __asm__ volatile("pause"); }
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
/* v0.9.4: the progress bar that lived here went with the app's own
   whole-disk installer; app_install() hands over to the ISO's wizard,
   which draws its own. */

/* ── Notification Center ──────────────────────────────────────────────────
   v0.8.8.7: THE GEOMETRY IS DERIVED NOW.

   NC_X was 55 and NC_BOT was 23. Both are correct for the 80x25
   console they were written against and wrong on every console that
   is not 80x25. On the HP's framebuffer grid -- 128x48 -- the drawer
   opened at column 55 of 128 and stopped at row 23 of 48: floating in
   the middle of the desktop, 48 columns clear of the right edge, with
   the taskbar bell that opens it nowhere near it.

   Reported as "notification center goes in middle", which is exactly
   what it did, and is the same family as the 64x24 app bug: a number
   that was once a measurement of the screen and became a constant.

   At 80x25 these expressions evaluate to 55 and 23, so the console
   every app was designed against is byte-for-byte unchanged. */
#define NC_W    25
#define NC_X    ((D_COLS > NC_W) ? (D_COLS - NC_W) : 0)
#define NC_TOP   1
#define NC_BOT  (TASKBAR_Y - 1)
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
        notif_push("HavenDOS " HAVENDOS_VER " running");
        notif_push("System boot OK");
        /* v0.8.5.4: this said "VirtIO disk mounted" for whatever disk
           had been mounted, which on a laptop is an NVMe one. There is
           no disk index to hand here -- vfs_using_disk() is a boolean --
           so it says the true thing it knows rather than guessing a bus. */
        if(vfs_using_disk())  notif_push("Disk mounted");
        if(vfs_using_fat16()) notif_push("FAT16 filesystem OK");
        else { extern int vfs_using_fat32(void);
               if(vfs_using_fat32()) notif_push("FAT32 filesystem OK"); }
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
    /* v0.7.19: was a fixed 80x23 window at (0,1), which in a 90x60 mode was a
       small box in the corner. */
    /* v0.2.0 is the H language's own version, the same one bpkg.c reports for
       the h-lang package -- NOT the OS version.   version-literal-ok */
    ui_draw_window(0,1,vga_cols(),vga_rows()-2,
                   "HavenCode  v0.2.0  -  TechHaven Studios");

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
        ui_draw_window(4,2,72,21,"bpkg  -  HavenDOS Package Manager  " HAVENDOS_VER);

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
                while(j<16)idrow[j++]=' ';
                idrow[16]=0;

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
                while(di<26)desc[di++]=' ';
                desc[26]=0;
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

/* The same installer backs the desktop icon and the shell's `install`
   command.  There used to be three of these - this one, the shell's, and the
   ISO's - and only the ISO's had learned about disks that are not VirtIO. */
void installer_run(void){ app_install(); }

static void app_install(void){
    /* The unified scan lives in iso_installer.c and covers both buses; this
       app used to run its own VirtIO-only one and hand the installer a VirtIO
       index, which is why it announced "No VirtIO drives found" on a machine
       whose IDE disk the ISO installer had no trouble with. */
    /* The disk interface comes from include/instdisk.h now, not from a
       handful of extern declarations written out here. That header is
       what makes one bus-name function reachable from every screen;
       before it existed there was nowhere to put one. (v0.8.5.4) */
    extern int      havendos_install_to_drive(int, void(*)(int,int,const char*));
    extern int      havendos_install_image_available(void);
    extern int      keyboard_waitchar(void);

    theme_t *th=ui_theme();
    const int WX=14, WY=3, WW=52, WH=20;    /* window box */
    const int CX=WX+2;                       /* content left edge */

    #define INST_ROW(r) (WY+2+(r))
    #define INST_WIPE(r0,r1) do{ for(int _y=INST_ROW(r0);_y<=INST_ROW(r1);_y++) \
        for(int _x=WX+1;_x<WX+WW-1;_x++) \
            vga_putchar_at(' ',_x,_y,th->win_fg,th->win_bg); }while(0)

    vga_clear();
    ui_draw_taskbar("Install HavenDOS");
    ui_draw_window(WX, WY, WW, WH, "Install HavenDOS " HAVENDOS_VER);

    /* An installed HavenDOS runs the pass-1 kernel, which carries no image to
       install.  Saying so here costs the user nothing; discovering it at step
       7 of 9 cost them the disk. */
    { extern int boot_is_live(void);
      if(boot_is_live()){
        vga_puts_at("This is a live session.",CX,INST_ROW(1),
                    VGA_YELLOW,th->win_bg);
        vga_puts_at("Nothing is mounted and nothing is written to",CX,INST_ROW(3),th->win_fg,th->win_bg);
        vga_puts_at("any disk, so whatever else is installed on",CX,INST_ROW(4),th->win_fg,th->win_bg);
        vga_puts_at("this machine cannot be affected.",CX,INST_ROW(5),th->win_fg,th->win_bg);
        vga_puts_at("To install, reboot and choose",CX,INST_ROW(7),th->win_fg,th->win_bg);
        vga_puts_at("\"Install to Disk\" from the boot menu.",CX,INST_ROW(8),th->win_fg,th->win_bg);
        vga_puts_at("No disk has been touched.",CX,INST_ROW(10),VGA_LIGHT_GREEN,th->win_bg);
        vga_puts_at("Press any key to return.",CX,INST_ROW(12),VGA_DARK_GREY,th->win_bg);
        keyboard_waitchar(); return;
      } }

    if(!havendos_install_image_available()){
        vga_puts_at("This copy cannot install HavenDOS.",CX,INST_ROW(1),
                    VGA_YELLOW,th->win_bg);
        vga_puts_at("There is no kernel image in this kernel and",CX,INST_ROW(3),th->win_fg,th->win_bg);
        vga_puts_at("no readable /boot/havendos.elf on the",CX,INST_ROW(4),th->win_fg,th->win_bg);
        vga_puts_at("system volume to copy.",CX,INST_ROW(5),th->win_fg,th->win_bg);
        vga_puts_at("Boot the HavenDOS ISO to install or",CX,INST_ROW(7),th->win_fg,th->win_bg);
        vga_puts_at("reinstall; it starts the installer itself.",CX,INST_ROW(8),th->win_fg,th->win_bg);
        vga_puts_at("Your disk has not been touched.",CX,INST_ROW(10),VGA_LIGHT_GREEN,th->win_bg);
        vga_puts_at("Press any key to return.",CX,INST_ROW(12),VGA_DARK_GREY,th->win_bg);
        keyboard_waitchar(); return;
    }

    /* ── v0.9.4 "Prometheus": THE ISO'S INSTALLER, FROM HERE ─────────
       This app used to carry its own whole-disk installer: a disk list,
       a YES prompt, havendos_install_to_drive(). It could not install
       beside Windows (the additive path never reached it), it labelled
       every non-ATA disk "(VirtIO)", and it could only ever run on the
       ISO, because an installed system had nothing to install. Now an
       installed system installs its own /boot/havendos.elf, so the app
       hands over to the one installer -- the same previews, the same
       refusals (including the disk it is running from), both paths. */
    {
        extern void iso_install_wizard(void);
        extern void havendos_install_image_release(void);
        iso_install_wizard();
        havendos_install_image_release();
    }
    #undef INST_ROW
    #undef INST_WIPE
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
        sleep_ms(800);
        machine_reset();
    }
    if(c=='2'){
        vga_clear();
        vga_puts_at("  Shutting down HavenDOS...  ", 26, 12, VGA_BLACK, VGA_YELLOW);
        /* No "It is now safe to power off." before the attempt -- see
           the same removal in shell/shell.c. */
        sleep_ms(600);
        machine_poweroff();
    }
    if(c=='3'){
        login_screen();
    }
}

/* Apps lay themselves out for 80x25 and draw in the top-left corner.  On
   any larger grid -- 80x50, 90x60, or the framebuffer console -- the desktop
   they were launched from stays visible around and below them, which reads
   as "the app opened but the desktop is still there".
 
   Clearing before and redrawing after costs one screen paint and removes the
   whole class of complaint.  Making each app derive its own geometry from
   vga_rows()/vga_cols(), the way the desktop chrome already does, is the
   real fix and a separate pass -- this stops it looking broken meanwhile. */
/* ── AN APP CRASH DOES NOT TAKE THE DESKTOP WITH IT, v0.8.8.4 ────────
   Every CPU exception used to reach isr_handler(), which painted
   KERNEL PANIC and halted -- so a divide by zero in the calculator
   stopped the machine. appguard_enter() arms a guard; if the app
   faults, control comes back HERE with a 1 and the desktop says which
   app stopped.

   include/appguard.h has the limits in full. The short version: this
   catches the fault, not the damage. An app that corrupts kernel
   memory and then returns normally is never caught, because nothing
   in this kernel stops it writing there in the first place. */
static void desktop_app_crashed(void);

/* ── the box an unconverted app is given ─────────────────────────────
   80x25, because that is what every app in the registry was written
   for. Not a guess: v0.7.18's header states it and the layouts show
   it -- `ui_draw_window(4,2,72,20,...)`, a 60-cell bar from column 8,
   a hint line at row 23. */
#define APP_VIEW_COLS 80
#define APP_VIEW_ROWS 25

/* The surround, painted ONCE before the viewport is set, so the app's
   box reads as a window on a desk rather than as a screen that failed
   to finish drawing. Must run with the viewport off -- it is the only
   thing that ever draws outside the box.

   v0.8.8.7: IT IS A WINDOW NOW, NOT A DOTTED RECTANGLE.

   v0.8.8.6 ringed the box with '.' and called it done. On the HP that
   reads as an 80x25 screen stranded in the middle of a larger one --
   "apps now go top left or center" -- because nothing on screen says
   the small region is deliberate. A dotted rule is a mark; a title
   bar with the app's name in it is a window, and the difference is
   entirely in whether the person can tell which thing they are
   looking at.

   So: a title bar above the box carrying the app's name and how to
   leave, a frame around it, and a shadow down the right and bottom.
   Everything outside is the theme's desktop background, as before. */
static void desktop_app_surround(int vx, int vy, int vw, int vh,
                                 const char *name)
{
    theme_t *th = ui_theme();
    int pw = vga_phys_cols(), ph = vga_phys_rows();
    int x, y;
    int tb = vy - 1;                    /* the title bar row          */
    int fx0 = vx - 1, fx1 = vx + vw;    /* frame columns              */
    int fy1 = vy + vh;                  /* frame bottom row           */

    /* Clear the whole panel to the desktop first; every piece below
       then paints over a known background rather than over whatever
       the last app left, which is what made the old surround flicker
       on a repaint. */
    for(y = 0; y < ph; y++)
        for(x = 0; x < pw; x++){
            if(x >= vx && x < vx + vw && y >= vy && y < vy + vh) continue;
            vga_putchar_at(' ', x, y, th->desk_bg, th->desk_bg);
        }

    /* Shadow, one cell right and one row down, drawn BEFORE the frame
       so the frame's corners sit on top of it. */
    for(y = tb + 1; y <= fy1 + 1; y++)
        if(y >= 0 && y < ph && fx1 + 1 < pw)
            vga_putchar_at(' ', fx1 + 1, y, th->desk_bg, VGA_DARK_GREY);
    for(x = fx0 + 1; x <= fx1 + 1; x++)
        if(x >= 0 && x < pw && fy1 + 1 < ph)
            vga_putchar_at(' ', x, fy1 + 1, th->desk_bg, VGA_DARK_GREY);

    /* Side and bottom frame. */
    for(y = (tb >= 0 ? tb : vy); y <= fy1; y++){
        if(y < 0 || y >= ph) continue;
        if(fx0 >= 0)  vga_putchar_at('|', fx0, y, th->accent, th->desk_bg);
        if(fx1 < pw)  vga_putchar_at('|', fx1, y, th->accent, th->desk_bg);
    }
    for(x = fx0; x <= fx1; x++)
        if(x >= 0 && x < pw && fy1 < ph)
            vga_putchar_at('-', x, fy1, th->accent, th->desk_bg);
    if(fx0 >= 0 && fy1 < ph) vga_putchar_at('+', fx0, fy1, th->accent, th->desk_bg);
    if(fx1 < pw && fy1 < ph) vga_putchar_at('+', fx1, fy1, th->accent, th->desk_bg);

    /* Title bar. If there is no row above the box -- a panel exactly
       25 rows tall, which vga_viewport_centre can still produce -- it
       is skipped rather than drawn over the app's first line. */
    if(tb >= 0){
        const char *hint = "ESC closes";
        int hl = (int)strlen(hint);
        int hx = fx1 - hl;
        for(x = fx0; x <= fx1; x++)
            if(x >= 0 && x < pw)
                vga_putchar_at(' ', x, tb, th->bar_bg, th->accent);
        if(name && name[0] && fx0 + 2 < pw)
            vga_puts_at(name, fx0 + 2, tb, th->bar_bg, th->accent);
        /* Only when it cannot collide with the name. */
        if(hx > fx0 + 3 + (name ? (int)strlen(name) : 0) && hx + hl <= fx1)
            vga_puts_at(hint, hx, tb, th->bar_bg, th->accent);
    }
}

static void desktop_run_app(void (*fn)(void), const char *name, int fits)
{
    int boxed = 0;

    if(!fn) return;

    /* ── BOX IT IF IT CANNOT USE THE SCREEN ───────────────────────────
       An app that lays out for 80x25 gets an 80x25 console in the
       middle of the panel. include/vga.h says why the transform is in
       the driver rather than in forty-three rewritten apps.

       Only when the panel is actually bigger: at 80x25 the viewport
       would be the whole screen and setting one would be a no-op with
       extra steps. And vga_viewport_centre() refuses a box that will
       not fit, so a panel smaller than 80x25 -- which the framebuffer
       console can produce, 640x480 at a 16x32 cell is 40x15 -- falls
       back to the app having the whole thing, exactly as before. */
    if(!fits &&
       (vga_phys_cols() > APP_VIEW_COLS || vga_phys_rows() > APP_VIEW_ROWS)){
        int vx, vy, vw, vh;
        if(vga_viewport_centre(APP_VIEW_COLS, APP_VIEW_ROWS)){
            boxed = 1;
            vga_viewport_get(&vx, &vy, &vw, &vh);
            vga_viewport_clear();                 /* paint the surround */
            desktop_app_surround(vx, vy, vw, vh, name);
            vga_viewport_centre(APP_VIEW_COLS, APP_VIEW_ROWS);
            mouse_clamp_to_grid();
        }
    }

    vga_clear();
    vga_set_cursor(0,0);

    /* ── MEASURE WHAT THIS APP DRAWS OUTSIDE ITS BOX ──────────────────
       Reset after the clear, so the clear's own writes are not counted
       -- vga_clear() inside a viewport writes exactly the cells it is
       allowed to and clips nothing, but the full-screen clear on the
       way IN to a boxed app happens before the viewport is set and is
       not this app's doing either way. */
    vga_clip_reset();

    if(appguard_enter(name ? name : "an app")){
        /* A fault brought us back. The app is NOT called again. */
        desktop_app_crashed();
        if(boxed){ vga_viewport_clear(); mouse_clamp_to_grid(); }
        vga_clear();
        return;
    }
    fn();
    appguard_leave();

    /* ── AND SAY SO, IN THE LOG ───────────────────────────────────────
       Reported rather than drawn on screen: the app has just closed
       and the desktop is about to repaint, so a toast would be gone
       before it was read -- and the person who needs this is reading
       `dmesg` afterwards, not watching.

       One line per app that overflowed, naming it and the worst
       coordinate. That turns "some apps go off the screen" into a
       list. */
    {
        uint32_t clipped = 0;
        int mx = -1, my = -1;
        vga_clip_stats(&clipped, &mx, &my);
        if(clipped){
            klog(KLOG_WARN, "layout: %s drew %u cell(s) outside its "
                            "%dx%d console, as far as column %d row %d",
                 name ? name : "an app", clipped,
                 vga_cols(), vga_rows(), mx, my);
        }
    }

    /* The screen goes back to the desktop BEFORE it is cleared, or the
       clear would only blank the app's box and leave the surround --
       and then the desktop would draw its taskbar inside the last
       app's window. */
    if(boxed){ vga_viewport_clear(); mouse_clamp_to_grid(); }
    vga_clear();
}

/* The screen a person sees instead of a panic. Deliberately not red
   and not alarming: the system is fine, one program is not. */
static void desktop_app_crashed(void)
{
    theme_t *th = ui_theme();
    int w = 54, h = 11;
    int x = (D_COLS - w) / 2, y = (D_ROWS - h) / 2;
    if(x < 0) x = 0;
    if(y < 0) y = 0;

    vga_clear();
    ui_draw_window(x, y, w, h, "Program stopped");
    vga_puts_at(appguard_name(), x + 2, y + 2, th->accent, th->win_bg);
    vga_puts_at("stopped unexpectedly.", x + 3 + (int)strlen(appguard_name()),
                y + 2, th->win_fg, th->win_bg);

    {
        char line[72];
        int n = 0;
        const char *why = appguard_exception_name();
        const char *pre = "Cause: ";
        for(int i = 0; pre[i] && n < (int)sizeof line - 1; i++)
            line[n++] = pre[i];
        for(int i = 0; why[i] && n < (int)sizeof line - 1; i++)
            line[n++] = why[i];
        line[n] = 0;
        vga_puts_at(line, x + 2, y + 4, th->bar_fg, th->win_bg);
    }

    vga_puts_at("The desktop is still running. Nothing else was",
                x + 2, y + 6, th->win_fg, th->win_bg);
    vga_puts_at("closed, and your files are untouched.",
                x + 2, y + 7, th->win_fg, th->win_bg);

    /* AFTER THE THIRD ONE, SAY SOMETHING DIFFERENT. A machine that
       keeps recovering is a machine with a real problem, and a
       reassuring message repeated five times stops being honest. */
    if(appguard_crash_count() >= 3){
        char c[48];
        int n = 0;
        const char *pre = "This is crash ";
        uint32_t v = appguard_crash_count();
        char d[8]; int dn = 0;
        for(int i = 0; pre[i]; i++) c[n++] = pre[i];
        if(v == 0) d[dn++] = '0';
        while(v){ d[dn++] = (char)('0' + v % 10); v /= 10; }
        while(dn) c[n++] = d[--dn];
        {
            const char *post = " since boot -- consider a reboot.";
            for(int i = 0; post[i] && n < (int)sizeof c - 1; i++)
                c[n++] = post[i];
        }
        c[n] = 0;
        vga_puts_at(c, x + 2, y + 9, VGA_YELLOW, th->win_bg);
    } else {
        vga_puts_at("Press any key.", x + 2, y + 9, th->bar_fg, th->win_bg);
    }

    vga_flush();

    /* ── DRAIN FIRST, THEN WAIT. ──────────────────────────────────────
       FOUND BY BOOTING IT, in harness/appguard_boot.py.

       This used to be a bare keyboard_getchar(), and it returned
       IMMEDIATELY every time. The app that just faulted had unread
       keystrokes sitting in the buffer -- at minimum the ENTER that
       ran the command which crashed it -- and this window consumed one
       of them and vanished. The recovery was working perfectly and the
       notice was on screen for less than a frame, so from the outside
       an app crash looked like the app simply closing with no
       explanation at all.

       The harness read the desktop one second after a crash and
       reported "no recovery window" four times in a row, which is the
       only reason this was found: nothing about it is visible from the
       code, and a person would have reported it as "it just quits".

       So: throw away what the dead app left behind, hold the notice
       for a moment in case a key is already on its way down, and only
       then wait for a key that the person actually meant. */
    while(keyboard_haschar()) (void)keyboard_getchar();

    {
        extern uint32_t get_ticks(void);        /* 10 ms units */
        extern int      kernel_poll(void);
        uint32_t until = get_ticks() + 60u;     /* ~600 ms */
        while((int32_t)(get_ticks() - until) < 0){
            if(keyboard_haschar()) (void)keyboard_getchar();
            kernel_poll();   /* kpoll-site-ok: the notice waits for a
                                person, and the clock, the DHCP retries
                                and hotplug must not stop while it does */
        }
    }

    (void)keyboard_waitchar();
}

/* ════════════════════════════════════════════════════════════════════
   THE LAYOUT AUDIT -- `layout` in the shell.   v0.8.8.5

   Kyan, after running beta 5 on the HP:

       "some apps still go off the screen and aren't show fully maybe
        bc of 16:10???"

   THERE ARE FORTY-SIX APPS and guessing which three is how a release
   fixes the wrong ones. v0.8.8.5 counts every write the console clips
   -- vga_write() is the one function every cell goes through -- so
   the machine can answer the question itself, on the panel it is
   actually running on.

   ── WHY IT IS IN THE KERNEL AND NOT IN A HARNESS ────────────────────

   The first attempt drove the start menu from harness/layout_audit.py.
   It took six minutes, and it was wrong three times over before it was
   right: the desktop repaints 2000 cells on every keypress, so DOWN
   presses sent faster than it draws are DROPPED and a request for app
   14 opened app 7. Then the Windows key closed a context menu instead
   of opening the start menu. Then the cursor column was read as 1
   when draw_menu() puts it at 0.

   Each of those was a harness bug reported as a result. None of them
   is a risk here, because nothing is being navigated: the audit walks
   appreg[] directly.

   It also runs ON THE HP, which a harness never will.

   ── HOW AN APP THAT WAITS FOR A KEY IS MADE TO RETURN ───────────────

   Every app blocks on the keyboard. So the audit FILLS THE KEY BUFFER
   before calling each one: drivers/keyboard.c holds 256 and
   keyboard_inject() is the same entry usb/hid.c uses, so an injected
   key is indistinguishable from a typed one.

   ESC and 'q' only. Not ENTER and not SPACE: those CONFIRM things --
   a dialog's default button, an installer's "proceed" -- and an audit
   that can answer a confirmation prompt is an audit that can do
   something it was not asked to.

   ── AND TWO APPS ARE NOT AUDITED ────────────────────────────────────

   "Install HavenDOS" writes to disks and "Power and Reboot" ends the
   session. Both are skipped by id and both are PRINTED as skipped,
   because a table of forty-four where forty-six were expected is a
   table somebody has to go and count.
   ════════════════════════════════════════════════════════════════════ */
extern void keyboard_inject(int c);

static int audit_skipped(const char *id)
{
    /* ── "shell" IS WHY THE FIRST RUN AUDITED ONE APP ─────────────────
       MEASURED. The first version skipped only install and power, so
       the audit opened app 0 -- the Shell -- which is a NESTED SHELL.
       The injected ESC and 'q' keys became characters typed at its
       prompt, it waited for `exit`, and the audit sat inside it for
       ever. `dmesg` held exactly one line, "layout audit: Shell", and
       the screen showed a shell prompt with sixty q's on it, which is
       indistinguishable from the audit having finished.

       That one log line is the whole reason this was diagnosable, and
       it is why the name is written BEFORE the app runs.

       Nothing is lost by skipping it: the Shell is fits=1, so it is
       handed the whole console and has nothing to draw outside. */
    return strcmp(id, "shell")   == 0 ||
           strcmp(id, "install") == 0 ||
           strcmp(id, "power")   == 0;
}

static void audit_fill_keys(void)
{
    /* Just under the 256 the ring holds, so the last few cannot be
       dropped by a full buffer. ESC first because most apps take it. */
    for(int i = 0; i < 120; i++){
        keyboard_inject(27);
        keyboard_inject('q');
    }
}

static void audit_drain_keys(void)
{
    while(keyboard_haschar()) (void)keyboard_getchar();
}

void desktop_layout_audit(void)
{
    int over = 0, done = 0, skipped = 0;

    vga_clear();
    vga_set_cursor(0,0);
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("Layout audit\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("Opening every app and counting what it draws outside its\n");
    vga_puts("console. This takes a moment and the screen will flicker.\n\n");
    vga_flush();
    for(volatile int d = 0; d < 20000000; d++){ }

    for(int i = 0; i < NREG; i++){
        uint32_t clipped = 0;
        int mx = -1, my = -1;
        int boxed = 0;

        if(audit_skipped(appreg[i].id)){ skipped++; continue; }
        if(!appreg[i].fn) continue;

        /* Named in the log BEFORE it runs. If an app does not return
           -- because it wants a key this audit will not send -- the
           last line in `dmesg` is the one that says which. */
        klog(KLOG_INFO, "layout audit: %s", appreg[i].name);

        if(!appreg[i].fits &&
           (vga_phys_cols() > APP_VIEW_COLS ||
            vga_phys_rows() > APP_VIEW_ROWS)){
            boxed = vga_viewport_centre(APP_VIEW_COLS, APP_VIEW_ROWS);
        }

        vga_clear();
        vga_clip_reset();
        audit_fill_keys();
        appreg[i].fn();
        audit_drain_keys();
        vga_clip_stats(&clipped, &mx, &my);

        if(boxed) vga_viewport_clear();
        if(clipped){
            over++;
            klog(KLOG_WARN, "layout: %s drew %u cell(s) outside its %dx%d "
                            "console, as far as column %d row %d",
                 appreg[i].name, clipped,
                 boxed ? APP_VIEW_COLS : vga_phys_cols(),
                 boxed ? APP_VIEW_ROWS : vga_phys_rows(), mx, my);
        }
        done++;
    }

    vga_viewport_clear();

    /* ── THE BUFFER MUST BE EMPTY BEFORE THE SUMMARY ──────────────────
       Two hundred and forty keys were injected per app and the apps
       only consumed the ones they needed. Left in the ring they do
       three things, all of them bad: they dismiss this summary before
       it can be read, they are echoed at the shell prompt afterwards,
       and -- measured on the first run -- they corrupt the NEXT
       command typed, so `dmesg | grep layout` came back empty and the
       audit looked like it had found nothing.

       Same shape as the crash notice being closed by the dead app's
       unread ENTER and the loading screen being dropped by GRUB's.
       Third time in one release: a key that was meant for something
       else is not an answer to a question that has not been asked. */
    audit_drain_keys();

    vga_clear();
    vga_set_cursor(0,0);
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("Layout audit\n\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_printf("  %d app(s) opened, %d skipped (shell, install, power)\n",
               done, skipped);
    if(over == 0){
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("  none of them drew outside its console\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
    } else {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_printf("  %d app(s) drew outside their console\n", over);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        vga_puts("  'dmesg | grep layout' names each one and the\n");
        vga_puts("  furthest cell it reached.\n");
    }
    vga_puts("\n  The console is ");
    { char b[8]; itoa(vga_phys_cols(), b, 10); vga_puts(b); }
    vga_puts("x");
    { char b[8]; itoa(vga_phys_rows(), b, 10); vga_puts(b); }
    vga_puts(".\n");
    if(vga_phys_cols() <= APP_VIEW_COLS && vga_phys_rows() <= APP_VIEW_ROWS)
        vga_puts("  No app was boxed: the console is not bigger than\n"
                 "  80x25, so every app had the whole screen.\n"
                 "  'resolution fit' makes the grid match the panel.\n");
    vga_puts("\nPress any key.\n");
    vga_flush();
    audit_drain_keys();          /* nothing injected may answer this */
    (void)keyboard_waitchar();
    audit_drain_keys();          /* and the shell gets a clean prompt */
}

/* ================================================================
   DESKTOP MAIN LOOP
   ================================================================ */
void desktop_run(void){
    /* v0.7.18: the chrome is derived from vga_rows()/vga_cols() now, so the
       desktop no longer force-switches the display and the user's chosen mode
       survives opening it.  (Apps launched from here still lay out for 80x25
       and draw in the top-left of a larger screen.) */

    int icon_sel=0, needs_draw=1;
    int last_mouse_x=-1, last_mouse_y=-1;
    menu_state=0; top_sel=0; sub_sel=0; sub_scroll=0;
    /* 0.8.1: the icon grid and Favorites are the user's lists now, read
       from .desktop and .favorites.  Loaded before the first draw, since
       draw_icons() reads icon_count. */
    pins_load();
    icons_refresh();
    sub_refresh();
    screensaver_reset();
    music_startup();
    uint8_t last_rtc_s=0xFF;  /* track RTC seconds for clock update */
    /* Welcome toast + notification */
    toast_success("Welcome to HavenDOS " HAVENDOS_VER "!");
    notif_push("HavenDOS " HAVENDOS_VER " started");
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
                    /* v0.7.6: the desktop no longer repaints itself in pieces.
                     *
                     * Every release since v0.7.4 has tried to keep the screen
                     * current by redrawing only the parts that changed each
                     * second - the clock, the infopanel, later the bars.  That
                     * is what produced the reports of individual elements
                     * blinking out: any cell a partial update did not cover,
                     * and that the shadow buffer believed was already correct,
                     * stayed wrong on screen until something unrelated forced
                     * a full redraw.  Each fix moved the symptom to a
                     * different set of cells rather than removing it, because
                     * the incremental path itself is the problem.
                     *
                     * Now the tick simply requests a full frame.  full_draw()
                     * repaints all 2000 cells into the shadow and commits once
                     * via vga_end_frame(), and the flush only writes cells
                     * that genuinely differ - so a full repaint costs about
                     * the same at the VGA end as the partial one did, while
                     * making a stale cell structurally impossible.
                     *
                     * This also fixes the two clocks disagreeing: row 24 was
                     * only ever painted by full_draw(), so the taskbar clock
                     * froze at the last full redraw while row 0 kept ticking.
                     */
                    needs_draw=1;
                }
            }
        }
        /* v0.7.6: toast_blank() clears row 22 and flushes, but nothing ever
           restored what was underneath - toast.c's own header admits it
           ("Desktop redraws naturally cover it on next frame"), except there
           is no next frame unless the user moves the mouse.  An expired toast
           left a blank stripe across the desktop until then. */
        if(toast_tick_expired()) needs_draw=1;

        screensaver_check();

        /* Mouse click handling — works alongside keyboard */
        if(mouse_is_ready()){
            int mx,my,mb; mouse_get(&mx,&my,&mb);

            int mb_click = mouse_get_clicks();

            /* Cursor tracking — runs regardless of menu state.
               Must flush after draw so cursor appears in same frame
               as the rest of the shadow content, not one frame late. */
            if(mx!=last_mouse_x||my!=last_mouse_y){
                vga_begin_frame();
                ui_draw_cursor();
                vga_end_frame();
                last_mouse_x=mx; last_mouse_y=my;
            }

            /* ── Hover highlighting ────────────────────────────────── */
            if(menu_state==1||menu_state==2){
                /* Left panel categories */
                if(mx<MENU_W&&my>=MENU_ITEM_Y&&my<MENU_ITEM_Y+NTOP){
                    int h=my-MENU_ITEM_Y;
                    if(h!=top_sel||menu_state==1){
                        top_sel=h; sub_sel=0; sub_scroll=0;
                        menu_state=2; sub_refresh(); needs_draw=1;
                    }
                }
                /* Submenu hover.  This used to test my<MENU_ITEM_Y+nsub and
                   take the row as the item index outright, ignoring
                   sub_scroll -- so once a category was scrolled, hovering
                   and clicking selected the wrong app.  System already had
                   nine items in a five-row window, so it was reachable. */
                if(menu_state==2){
                    if(mx>=SUB_X&&mx<SUB_X+SUB_W&&
                       my>=SUB_ITEM_Y&&my<SUB_ITEM_Y+SUB_VIS){
                        int h=sub_scroll+(my-SUB_ITEM_Y);
                        if(h<sub_count&&h!=sub_sel){sub_sel=h;needs_draw=1;}
                    }
                }
            }
            /* Context menu hover */
            if(menu_state==3){
                if(mx>=ctx_x&&mx<ctx_x+CTXM_W&&my>=ctx_y+1&&my<ctx_y+1+ctx_nitems){
                    int h=my-(ctx_y+1);
                    if(h!=ctx_sel){ctx_sel=h;needs_draw=1;}
                }
            }

            if(mb_click&1){
                screensaver_reset(); needs_draw=1;
                if(menu_state==0){
                    /* Icon hit test — one implementation, see icon_hit() */
                    int clicked=icon_hit(mx,my);
                    if(clicked>=0){
                        icon_sel=clicked;
                        desktop_run_app(appreg[icon_items[icon_sel]].fn, appreg[icon_items[icon_sel]].name,
                                        appreg[icon_items[icon_sel]].fits);
                        needs_draw=1;
                    }
                    /* Start button: row 24, cols 0-13 */
                    if(my==TASKBAR_Y&&mx<=13){
                        menu_state=1; top_sel=0; sub_sel=0; sub_scroll=0;
                        sub_refresh();
                    }
                    /* Notification center: row 24, cols 55-63 */
                    /* v0.8.8.7: was `mx>=55&&mx<=63`, the drawn extent
                       of the cluster at W=80 minus the unread count --
                       and simply the wrong part of the screen at any
                       other width. ui.c owns the arithmetic now, so the
                       hit box and the paint cannot drift apart. */
                    if(ui_taskbar_notif_hit(mx,my)){ notif_center(); needs_draw=1; }
                } else if(menu_state==1||menu_state==2){
                    /* Category item click */
                    if(mx<MENU_W&&my>=MENU_ITEM_Y&&my<MENU_ITEM_Y+NTOP){
                        top_sel=my-MENU_ITEM_Y; menu_state=2;
                        sub_sel=0; sub_scroll=0; sub_refresh();
                    }
                    /* Submenu item click -- scroll-aware, see the hover
                       path above for what this used to get wrong */
                    else if(menu_state==2&&mx>=SUB_X&&mx<SUB_X+SUB_W&&
                            my>=SUB_ITEM_Y&&my<SUB_ITEM_Y+SUB_VIS){
                        int h=sub_scroll+(my-SUB_ITEM_Y);
                        if(h<sub_count){
                            sub_sel=h; menu_state=0;
                            desktop_run_app(appreg[sub_items[sub_sel]].fn, appreg[sub_items[sub_sel]].name,
                                        appreg[sub_items[sub_sel]].fits);
                            icons_refresh(); needs_draw=1;
                        }
                    }
                    /* Lock click */
                    else if(mx<MENU_W&&my==MENU_LOCK_Y){
                        menu_state=0; needs_draw=1;
                        login_screen();
                    }
                    /* Click outside → close */
                    else if(my==TASKBAR_Y||
                            (my<MENU_TOP&&!(menu_state==2&&
                                            mx>=SUB_X&&mx<SUB_X+SUB_W&&
                                            my>=SUB_TOP))){ menu_state=0; }
                } else if(menu_state==3){
                    if(mx>=ctx_x&&mx<ctx_x+CTXM_W&&my>=ctx_y+1&&my<ctx_y+1+ctx_nitems){
                        ctx_sel=my-(ctx_y+1);
                        /* A pin toggle leaves the menu up so the label can be
                           seen to flip and a second pin is one click away;
                           anything that takes over the screen closes it. */
                        if(!ctx_menu_activate(ctx_sel)) menu_state=0;
                    } else {
                        menu_state=0;
                    }
                }
            }
            if(mb_click&2){
                screensaver_reset(); needs_draw=1;
                if(menu_state==0||menu_state==2){
                    /* What is under the cursor decides what the menu offers:
                       a desktop shortcut, a start-menu entry, or neither. */
                    int app=-1;
                    if(menu_state==0){
                        int hit=icon_hit(mx,my);
                        if(hit>=0){ app=icon_items[hit]; icon_sel=hit; }
                    } else if(mx>=SUB_X&&mx<SUB_X+SUB_W&&
                              my>=SUB_ITEM_Y&&my<SUB_ITEM_Y+SUB_VIS){
                        int h=sub_scroll+(my-SUB_ITEM_Y);
                        if(h<sub_count){ app=sub_items[h]; sub_sel=h; }
                    }
                    /* A context menu over the flyout would need a second
                       state to keep the flyout painted underneath, so the
                       menu closes behind it -- which is what a context menu
                       does everywhere else anyway. */
                    ctx_build(app);
                    ctx_x=mx; ctx_y=my; ctx_sel=0;
                    if(ctx_x+CTXM_W>D_COLS) ctx_x=D_COLS-CTXM_W;
                    if(ctx_x<0) ctx_x=0;
                    if(ctx_y+CTXM_H>TASKBAR_Y) ctx_y=TASKBAR_Y-CTXM_H;
                    if(ctx_y<1) ctx_y=1;
                    menu_state=3;
                } else {
                    menu_state=0;
                }
            }
        }

        int c=keyboard_getchar();
        if(c==-1){
            /* v0.8.9.6: listening to the pointer while it waits, and
               back early when it moves -- see ui_idle(). */
            ui_idle(20);
            continue;
        }
        needs_draw=1;
        screensaver_reset();

        /* ── THE WINDOWS KEY ──────────────────────────────────────────
           Handled HERE, above the per-state branches, so it means the
           same thing from everywhere: open the start menu, or close it
           if it is already open. That is what it does on the system
           every person reaching for this key has used.

           Above the branches and with a `continue`, not inside them,
           for two reasons. Putting it in the menu_state==0 branch
           would make it work on the desktop and nowhere else, so it
           could not CLOSE the menu it opened. And falling through
           after changing menu_state would hand the same keystroke to
           the branch for the state it had just switched to, which
           would act on it a second time.

           The draw happens at the top of this loop, so `continue`
           repaints rather than skipping a frame. */
        if(c==KEY_SUPER){
            if(menu_state==0){
                menu_state=1; top_sel=0; sub_sel=0; sub_scroll=0;
                sub_refresh();
            } else {
                menu_state=0;
            }
            continue;
        }

        if(menu_state==0){
            /* Desktop: no menu open */
            if(c==27){ menu_state=1; top_sel=0; sub_sel=0; sub_scroll=0; sub_refresh(); }
            else if(c=='\t')    { /* fits=1: the shell derives its layout from vga_rows(),
                           same as its registry entry says. */
                        desktop_run_app(app_shell, "Shell", 1); needs_draw=1; }
            else if(c=='n'||c=='N') { notif_center(); needs_draw=1; }
            else if(c=='m'||c=='M'){
                /* The context menu had no keyboard route at all -- it was
                   right-click only, on an OS that runs perfectly well with
                   no mouse attached and whose mouse is unreliable under TCG.
                   M opens it on the selected icon, anchored beside it. */
                ctx_build(icon_count ? icon_items[icon_sel] : -1);
                int nc2=icon_cols();
                int approw=icon_sel/nc2, icol=icon_sel%nc2;
                ctx_x = 1 + icol*ICON_PITCH;
                ctx_y = 2 + (approw-icon_scroll_row)*ICON_TILE_STEP + 1;
                if(ctx_x+CTXM_W>D_COLS) ctx_x=D_COLS-CTXM_W;
                if(ctx_x<0) ctx_x=0;
                if(ctx_y+CTXM_H>TASKBAR_Y) ctx_y=TASKBAR_Y-CTXM_H;
                if(ctx_y<1) ctx_y=1;
                ctx_sel=0; menu_state=3;
            }
            /* Up and down move a whole ROW, left and right one column
               -- and a row is however many columns fit, not two. */
            else if(c==0x148&&icon_sel>=icon_cols()) icon_sel-=icon_cols();
            else if(c==0x150&&icon_sel+icon_cols()<icon_count)
                icon_sel+=icon_cols();
            else if(c==0x14B&&icon_sel%icon_cols()>0) icon_sel--;
            else if(c==0x14D&&icon_sel%icon_cols()!=icon_cols()-1
                    &&icon_sel+1<icon_count) icon_sel++;
            else if((c=='-'||c==0x153)&&icon_count>0){
                /* Unpin the selected shortcut.  The app itself is never
                   removed -- it is still in the start menu, which is the
                   whole point of the grid being a shortlist. */
                const char *id=appreg[icon_items[icon_sel]].id;
                pin_toggle(PIN_FILE_DESK,desk_ids,&desk_n,id);
                icons_refresh();
                if(icon_sel>=icon_count) icon_sel=icon_count?icon_count-1:0;
                toast_info("Removed from desktop");
            }
            else if(c=='\n'||c=='\r'){
                if(icon_count>0){
                    desktop_run_app(appreg[icon_items[icon_sel]].fn, appreg[icon_items[icon_sel]].name,
                                        appreg[icon_items[icon_sel]].fits);
                    needs_draw=1;
                }
            }
        }
        else if(menu_state==1){
            /* Main panel: up/down through categories */
            if(c==27)                         { menu_state=0; }
            else if(c==0x148&&top_sel>0)      { top_sel--; sub_sel=0; sub_scroll=0; sub_refresh(); }
            else if(c==0x150&&top_sel<NTOP-1) { top_sel++; sub_sel=0; sub_scroll=0; sub_refresh(); }
            else if(c==0x14D||c=='\n'||c=='\r'){ menu_state=2; sub_sel=0; sub_scroll=0; sub_refresh(); }
        }
        else if(menu_state==2){
            /* Submenu flyout */
            if(c==27)                              { menu_state=0; }
            else if(c==0x14B)                      { menu_state=1; }
            else if(c==0x148&&sub_sel>0)             sub_sel--;
            else if(c==0x150&&sub_sel<sub_count-1)   sub_sel++;
            else if(c==0x149){ sub_sel-=SUB_VIS; if(sub_sel<0) sub_sel=0; }
            else if(c==0x151){ sub_sel+=SUB_VIS;
                               if(sub_sel>sub_count-1) sub_sel=sub_count?sub_count-1:0; }
            else if((c=='m'||c=='M')&&sub_count>0){
                ctx_build(sub_items[sub_sel]);
                ctx_x = SUB_X + 2;
                ctx_y = SUB_ITEM_Y + (sub_sel - sub_scroll) + 1;
                if(ctx_x+CTXM_W>D_COLS) ctx_x=D_COLS-CTXM_W;
                if(ctx_x<0) ctx_x=0;
                if(ctx_y+CTXM_H>TASKBAR_Y) ctx_y=TASKBAR_Y-CTXM_H;
                if(ctx_y<1) ctx_y=1;
                ctx_sel=0; menu_state=3;
            }
            else if((c=='d'||c=='D')&&sub_count>0){
                const char *id=appreg[sub_items[sub_sel]].id;
                int r=pin_toggle(PIN_FILE_DESK,desk_ids,&desk_n,id);
                icons_refresh();
                if(r<0) toast_warning("Desktop is full");
                else    toast_info(r?"Added to desktop":"Removed from desktop");
            }
            else if((c=='f'||c=='F')&&sub_count>0){
                const char *id=appreg[sub_items[sub_sel]].id;
                int r=pin_toggle(PIN_FILE_FAV,fav_ids,&fav_n,id);
                /* Favorites is itself a view, so unpinning from inside it
                   shortens the list being displayed -- refresh before the
                   next draw reads sub_sel against a stale count. */
                sub_refresh();
                if(r<0) toast_warning("Favorites is full");
                else    toast_info(r?"Added to favorites":"Removed from favorites");
            }
            else if(c=='\n'||c=='\r'){
                if(sub_count>0){
                    menu_state=0;
                    desktop_run_app(appreg[sub_items[sub_sel]].fn, appreg[sub_items[sub_sel]].name,
                                        appreg[sub_items[sub_sel]].fits);
                    icons_refresh(); needs_draw=1;
                }
            }
        }
        else if(menu_state==3){
            /* Context menu (right-click quick access) */
            if(c==27)                                  { menu_state=0; }
            else if(c==0x148&&ctx_sel>0)                ctx_sel--;
            else if(c==0x150&&ctx_sel<ctx_nitems-1)     ctx_sel++;
            else if(c=='\n'||c=='\r'){
                if(!ctx_menu_activate(ctx_sel)) menu_state=0;
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
    ui_draw_window(4,2,72,21,"Storage Information  -  HavenDOS " HAVENDOS_VER);
    int y=4;
    #define DROW(l,v,c) vga_puts_at(l,8,y,VGA_LIGHT_GREY,VGA_BLACK);vga_puts_at(v,30,y,c,VGA_BLACK);y++;
    /* RAMFS */
    DROW("Primary Storage:", "RAMFS (in-memory)", VGA_LIGHT_GREEN);
    DROW("Max Files:",        "32 files",          VGA_WHITE);
    DROW("Max File Size:",    "4 KB per file",     VGA_WHITE);
    y++;
    /* VirtIO disk */
    int vready=virtio_blk_ready();
    /* bus-label-ok: this row reports the VirtIO block DRIVER, gated on
       virtio_blk_ready() -- it is not labelling a disk's bus. The panel
       below it describes only VirtIO, which is its own gap; see the
       handoff. */
    DROW("VirtIO Block:",   vready?"Detected":"Not detected", vready?VGA_LIGHT_GREEN:VGA_DARK_GREY);
    if(vready){
        uint32_t secs=(uint32_t)virtio_blk_sectors();
        uint32_t mb=secs/2048;
        char szb[16]; char tmp[12]; utoa(mb,tmp,10); strncpy(szb,tmp,8); strncat(szb," MB",15);
        DROW("Disk Size:",    szb,              VGA_WHITE);
        DROW("Sector Size:",  "512 bytes",      VGA_WHITE);
        char sec_s[16]; utoa(secs,sec_s,10);
        DROW("Total Sectors:",sec_s,            VGA_DARK_GREY);
        {   extern int vfs_using_fat32(void);
            DROW("Filesystem:", vfs_using_fat32() ? "FAT32 (HavenDOS partition)"
                                                  : "FAT16 (HavenDOS partition)",
                 VGA_WHITE); }
        /* bus-label-ok: inside if(vready), so VirtIO really is there. */
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
    ui_draw_window(4,2,72,21,"Network Information  -  HavenDOS " HAVENDOS_VER);
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
    NROW("TCP/IP Stack:", "Active (" HAVENDOS_VER ")",     VGA_LIGHT_GREEN);
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
