/* ================================================================
   shell/settings.c  --  the Settings app.  v0.9.5.

   Kyan: "settings isn't even useful btw. Do make settings make like an
   actual settings".  He was right. The old app (desktop.c) was five
   tabs in a fixed 72x21 window: a theme list, three checkboxes for
   built-in accounts nobody uses, two boot flags, a hostname and a
   screensaver number typed blind, and an About tab that said
   "HavenDOS 0.6.1". It could not change your password, your desktop
   icons, your console size, your prompt, your network address or
   anything about the accounts you actually created. Several things it
   did not offer had a setting in config.c already, and several things
   it did offer were only read on some paths.

   This is a sidebar of sections and a page for each, laid out for the
   console it is shown on. Everything on it is saved the moment it is
   changed, and every setting is read by the code it describes --
   that is the bar for an entry being here at all.

     Appearance  theme (with a preview), wallpaper, desktop info strip
     Display     console size: fit, max, a fixed size, or reset
     Desktop     which apps have desktop icons and which are favourites
     Shell       prompt and text colour
     Accounts    every account: password, role, delete, hide, auto-login,
                 and new accounts
     Network     address, DHCP, manual address, DNS
     Startup     boot log, POST screen, screensaver, hostname
     About       what this is

   Keys: Up/Down move, Enter or Right opens a section, Left or ESC
   goes back, ESC on the sidebar closes. Each page lists its own keys
   in the footer.
   ================================================================ */
#include "types.h"
#include "string.h"
#include "vga.h"
#include "version.h"
#include "keys.h"
#include "chrome.h"
#include "uimouse.h"
#include "theme.h"
#include "net.h"
#include "netdev.h"

extern int  keyboard_waitchar(void);
extern void sleep_ms(uint32_t);
extern int  atoi(const char*);
extern uint32_t pmm_total_blocks(void);

/* config.c */
extern void cfg_save(void);
extern int  cfg_desk_info(void);            extern void cfg_set_desk_info(int);
extern int  cfg_desk_small(void);           extern void cfg_set_desk_small(int);   /* v0.9.7 */
extern int  cfg_verbose_boot(void);         extern void cfg_set_verbose_boot(int);
extern int  cfg_post_screen(void);          extern void cfg_set_post_screen(int);
extern int  cfg_screensaver_timeout(void);  extern void cfg_set_screensaver_timeout(int);
extern int  cfg_autologin(void);            extern void cfg_set_autologin(int);
extern const char *cfg_autologin_user(void);extern void cfg_set_autologin_user(const char*);
extern int  cfg_autologin_timeout(void);    extern void cfg_set_autologin_timeout(int);
extern const char *cfg_hostname(void);      extern void cfg_set_hostname(const char*);
extern int  cfg_acct_hidden(int);           extern void cfg_set_acct_hidden(int,int);
extern const char *cfg_prompt(void);        extern void cfg_set_prompt(const char*);
/* login.c */
extern int         get_user_count(void);
extern const char *get_username(int);
extern int         get_user_admin(int);
extern int         get_user_current(void);
extern int         get_user_pwstate(int);
extern int         current_user_admin(void);
extern const char *current_username(void);
extern int         add_user(const char*, const char*, int);
extern int         delete_user(const char*);
extern int         passwd_user(const char*, const char*, const char*);
extern int         set_user_admin(const char*, int);
extern int         shell_verify_password(const char*, const char*);
extern int         user_last_login(const char*, char*, int, int*);
/* shell */
extern void shell_colors_get(int*, int*);
extern void shell_colors_set(int, int);
extern void cmd_resolution_v6(const char*);
extern int  shell_pref_cols(void);
extern int  shell_pref_rows(void);
/* desktop */
extern int         desktop_app_count(void);
extern const char *desktop_app_id(int);
extern const char *desktop_app_name(int);
extern void        desktop_pins_reload(void);
extern int         desktop_app_pinned(const char*, int);
extern int         desktop_app_pin_toggle(const char*, int);
/* net */
extern int  dhcp_request(void);

/* ── layout ───────────────────────────────────────────────────────── */
static int W, H;            /* the console                            */
static int PX, PW;          /* the page: first column and width       */
#define SIDE_W 16

enum { S_APPEAR, S_DISPLAY, S_DESKTOP, S_SHELL, S_ACCOUNTS, S_NETWORK,
       S_STARTUP, S_ABOUT, S_N };
static const char *sect_names[S_N] = {
    "Appearance","Display","Desktop","Terminal","Accounts","Network","Startup","About"
};
static const char *sect_keys[S_N] = {
    "appearance","display","desktop","shell","accounts","network","startup","about"
};

static char g_msg[96];      /* one line of feedback under the page    */
static vga_color_t g_msg_col;

static void say(vga_color_t c, const char *m){
    int i=0; while(m[i] && i<95){ g_msg[i]=m[i]; i++; } g_msg[i]=0; g_msg_col=c;
}

/* Selection colours that read on every theme and never blink: the
   accent with its bright bit dropped, as a background. */
static void sel_colors(vga_color_t *fg, vga_color_t *bg){
    int a = ui_theme()->accent & 7;
    if(a==0) a=VGA_LIGHT_GREY;
    *bg=(vga_color_t)a;
    *fg=(a==VGA_BLUE||a==VGA_RED||a==VGA_MAGENTA)?VGA_WHITE:VGA_BLACK;
}

static void put(int x, int y, const char *s, vga_color_t fg, vga_color_t bg){
    if(y<0||y>=H) return;
    for(; *s && x<W-1; s++, x++) vga_putchar_at(*s,x,y,fg,bg);
}
static void fill(int x, int y, int w, vga_color_t bg){
    for(int i=0;i<w && x+i<W;i++) vga_putchar_at(' ',x+i,y,VGA_WHITE,bg);
}
static void utoa_s(uint32_t v, char *o){
    char t[12]; int n=0; do{ t[n++]=(char)('0'+v%10); v/=10; }while(v);
    for(int i=0;i<n;i++) o[i]=t[n-1-i];
    o[n]=0;
}

/* A row on the page: label on the left, value right-aligned to a column,
   highlighted when selected. */
static void row_draw(int y, int sel, const char *label, const char *value, vga_color_t vcol){
    vga_color_t sf, sb; sel_colors(&sf,&sb);
    vga_color_t bg = sel ? sb : VGA_BLACK;
    fill(PX, y, PW, bg);
    put(PX+1, y, label, sel?sf:VGA_LIGHT_GREY, bg);
    if(value){
        int vx = PX + (PW>=56 ? 30 : PW-7);
        put(vx, y, value, sel?sf:vcol, bg);
    }
}
/* v0.9.6.3: a row that can be chosen is also a row that can be clicked */
static void row(int y, int sel, const char *label, const char *value, vga_color_t vcol){
    row_draw(y, sel, label, value, vcol);
    um_row(0, PX, y, PW, sel);
}
/* ...and a line of information is neither */
static void info(int y, const char *label, const char *value, vga_color_t vcol){
    row_draw(y, 0, label, value, vcol);
}

static void frame(int sect, int focus_page, const char *keys){
    const theme_t *th=ui_theme();
    W=vga_cols(); H=vga_rows();
    PX=SIDE_W+2; PW=W-PX-1;
    /* v0.9.6.3: the mouse. A page row needs the focus on the page
       first (Enter); a section is reached from the sidebar (ESC) by
       arrows, and opened with Enter. */
    um_frame();
    if(!focus_page){ int k='\n'; um_group_prefix(0,&k,1); }
    for(int i=0;i<S_N && 2+i<H-2;i++){
        int seq[UM_SEQ_MAX], n=0;
        if(focus_page) seq[n++]=27;
        for(int j=sect;j<i && n<UM_SEQ_MAX-1;j++) seq[n++]=KEY_DOWN;
        for(int j=sect;j>i && n<UM_SEQ_MAX-1;j--) seq[n++]=KEY_UP;
        seq[n++]='\n';
        um_keys(0,2+i,SIDE_W,1,seq,n);
    }
    for(int y=0;y<H;y++)
        vga_fill_row(y,' ',VGA_LIGHT_GREY,VGA_BLACK);
    vga_fill_row(0,' ',th->bar_fg,th->bar_bg);
    put(1,0,"Settings",th->bar_fg,th->bar_bg);
    chrome_note_header();                       /* v0.9.9: the window's title says it */
    if(W>=60) put(W-1-(int)strlen("HavenDOS " HAVENDOS_VER),0,"HavenDOS " HAVENDOS_VER,th->accent,th->bar_bg);
    vga_color_t sf, sb; sel_colors(&sf,&sb);
    for(int i=0;i<S_N && 2+i<H-2;i++){
        int on=(i==sect);
        vga_color_t bg = on ? (focus_page?VGA_BLACK:sb) : VGA_BLACK;
        fill(0,2+i,SIDE_W,bg);
        put(1,2+i,on?">":" ",on?th->accent:VGA_DARK_GREY,bg);
        put(3,2+i,sect_names[i], on?(focus_page?th->accent:sf):VGA_LIGHT_GREY, bg);
    }
    for(int y=1;y<H-1;y++) vga_putchar_at('|',SIDE_W,y,VGA_DARK_GREY,VGA_BLACK);
    put(PX,1,sect_names[sect],th->accent,VGA_BLACK);
    vga_fill_row(H-1,' ',VGA_LIGHT_GREY,VGA_DARK_GREY);
    put(1,H-1,focus_page?keys:"Up/Down choose  Enter open  ESC close",VGA_WHITE,VGA_DARK_GREY);
    if(g_msg[0]) put(PX+1,H-3,g_msg,g_msg_col,VGA_BLACK);
}

/* One line of input at the bottom of the page. secret hides it. Returns
   1 on Enter, 0 on ESC. */
static int ask(const char *prompt, char *buf, int max, int secret){
    int n=0; buf[0]=0;
    int y=H-3;
    for(;;){
        fill(PX,y,PW,VGA_BLACK);
        put(PX+1,y,prompt,VGA_WHITE,VGA_BLACK);
        int x=PX+2+(int)strlen(prompt);
        for(int i=0;i<n && x+i<W-1;i++) vga_putchar_at(secret?'*':buf[i],x+i,y,VGA_LIGHT_GREEN,VGA_BLACK);
        vga_putchar_at('_',x+n,y,VGA_LIGHT_GREEN,VGA_BLACK);
        vga_flush();
        int k=keyboard_waitchar();
        if(k==27) return 0;
        if(k=='\n'||k=='\r'){ buf[n]=0; return 1; }
        if(k=='\b'){ if(n>0) buf[--n]=0; continue; }
        if(k>=32 && k<127 && n<max-1){ buf[n++]=(char)k; buf[n]=0; }
    }
}

static int confirm(const char *q){
    int y=H-3;
    fill(PX,y,PW,VGA_BLACK);
    put(PX+1,y,q,VGA_YELLOW,VGA_BLACK);
    put(PX+3+(int)strlen(q),y,"Y / N",VGA_WHITE,VGA_BLACK);
    um_key(PX+3+(int)strlen(q),y,1,1,'y');      /* v0.9.6.3: click Y or N */
    um_key(PX+7+(int)strlen(q),y,1,1,'n');
    vga_flush();
    int k=keyboard_waitchar();
    return k=='y'||k=='Y';
}

/* ── Appearance ───────────────────────────────────────────────────── */
/* A miniature desktop in theme n: bar, wallpaper, a window. */
static void preview(int n, int x0, int y0, int w, int h){
    const theme_t *t=ui_theme_get(n);
    if(w<12||h<5) return;
    for(int x=0;x<w;x++) vga_putchar_at(' ',x0+x,y0,t->bar_fg,t->bar_bg);
    put(x0+1,y0,"HavenDOS",t->bar_fg,t->bar_bg);
    for(int y=1;y<h-1;y++){ for(int x=0;x<w;x++){
        char c=' '; vga_color_t fg=t->desk_bg;
        ui_pattern_at_theme(n,x,y,&c,&fg);
        vga_putchar_at(c,x0+x,y0+y,fg,t->desk_bg);
    } }
    put(x0+2,y0+2,"[>_] Shell",t->desk_fg,t->desk_bg);
    int wx=x0+(w>=30?14:w/3), wy=y0+3, ww=x0+w-2-wx, wh=h-5;
    if(ww>=10 && wh>=3){
        /* 0.9.5.1: a window as every app now draws one -- a filled title
           band over a plain body, no dashed frame. */
        vga_color_t tb=(vga_color_t)(t->accent&7); if(tb==t->win_bg) tb=t->title_bg;
        vga_color_t tf=(tb==VGA_BLACK||tb==VGA_BLUE||tb==VGA_RED||tb==VGA_MAGENTA)?VGA_WHITE:VGA_BLACK;
        for(int y=0;y<wh;y++) for(int x=0;x<ww;x++)
            vga_putchar_at(' ',wx+x,wy+y,y==0?tf:t->win_fg,y==0?tb:t->win_bg);
        put(wx+1,wy,t->name,tf,tb);
        if(ww>=20) put(wx+1,wy+2,"Text in a window",t->win_fg,t->win_bg);
    }
    for(int x=0;x<w;x++) vga_putchar_at(' ',x0+x,y0+h-1,t->bar_fg,t->bar_bg);
    put(x0+1,y0+h-1,"Start",t->accent,t->bar_bg);
}

static int page_appearance(int k, int *sel){
    int nt=ui_theme_count();
    int nrows=nt+3;                       /* themes, wallpaper, info strip, icon size */
    if(k==KEY_UP && *sel>0) (*sel)--;
    else if(k==KEY_DOWN && *sel<nrows-1) (*sel)++;
    else if(k=='\n'||k=='\r'||k==' '){
        if(*sel<nt){ ui_set_theme(*sel); say(VGA_LIGHT_GREEN,"Theme applied and saved."); }
        else if(*sel==nt){ ui_toggle_desk_pattern(); say(VGA_LIGHT_GREEN,ui_desk_pattern()?"Wallpaper on.":"Wallpaper off: plain desktop colour."); }
        else if(*sel==nt+1){ cfg_set_desk_info(!cfg_desk_info()); cfg_save(); say(VGA_LIGHT_GREEN,"Saved."); }
        else { cfg_set_desk_small(!cfg_desk_small()); cfg_save();
               say(VGA_LIGHT_GREEN,cfg_desk_small()?"Small desktop icons: one row each.":"Large desktop icons."); }
    }
    else if(k) return 0;

    int lw = PW>=60 ? 30 : PW;            /* the left column; preview right */
    int savePW=PW; PW=lw;
    int list_h = H-6;
    int top = (*sel < nt && *sel >= list_h-2) ? *sel-(list_h-3) : 0;
    if(top<0) top=0;
    int y=3;
    put(PX+1,2,"Theme",VGA_DARK_GREY,VGA_BLACK);
    for(int i=top;i<nt && y<H-6;i++,y++){
        char lab[40]; int p=0;
        lab[p++]= (i==ui_current_theme())?'*':' '; lab[p++]=' ';
        for(const char *c=ui_theme_name(i);*c&&p<38;c++) lab[p++]=*c;
        lab[p]=0;
        row(y,*sel==i,lab,0,VGA_WHITE);
    }
    y++;
    row(y++,*sel==nt,   "Wallpaper",          ui_desk_pattern()?"on":"off",VGA_WHITE);
    row(y++,*sel==nt+1, "Desktop info strip", cfg_desk_info()?"on":"off",VGA_WHITE);
    row(y++,*sel==nt+2, "Desktop icons",      cfg_desk_small()?"small":"large",VGA_WHITE);
    PW=savePW;
    if(PW>=60){
        int pv = (*sel<nt)?*sel:ui_current_theme();
        int px=PX+lw+3, pw=PW-lw-4, ph=H-8; if(ph>14) ph=14;
        preview(pv,px,3,pw,ph);
        if(*sel<nt && *sel!=ui_current_theme())
            put(px,3+ph,"preview -- Enter applies",VGA_DARK_GREY,VGA_BLACK);
    }
    if(ui_theme_count()<ui_theme_total())
        put(PX+1,H-2,"8 more themes: bpkg install themes-extra",VGA_DARK_GREY,VGA_BLACK);
    return 1;
}

/* ── Display ──────────────────────────────────────────────────────── */
static const struct { const char *label, *arg; } disp_opts[] = {
    {"Fit the screen (recommended)", "fit"},
    {"As much text as fits",         "max"},
    {"80 x 25",                      "80x25"},
    {"100 x 37",                     "100x37"},
    {"120 x 45",                     "120x45"},
    {"160 x 50",                     "160x50"},
    {"Reset to the default",         "reset"},
};
#define NDISP ((int)(sizeof disp_opts/sizeof disp_opts[0]))

/* v0.9.6.3: the HDMI monitor, two rows under the console sizes */
extern int  cfg_monitor_auto(void);  extern void cfg_set_monitor_auto(int);
extern int  cfg_monitor_fit(void);
extern int  gfx_monitor_refit(int fit);
extern int  gfx_monitor_lit(void);
extern const char *xm_fit_name(int fit);
extern const char *xm_why_name(int why);
#define XM_EREFUSED_  16     /* include/xemode.h XM_EREFUSED */
#define DISP_MON_AUTO (NDISP)
#define DISP_MON_FIT  (NDISP+1)

static int page_display(int k, int *sel){
    if(k==KEY_UP && *sel>0) (*sel)--;
    else if(k==KEY_DOWN && *sel<DISP_MON_FIT) (*sel)++;
    else if(*sel==DISP_MON_AUTO && (k=='\n'||k=='\r'||k==' '||k==KEY_LEFT||k==KEY_RIGHT)){
        cfg_set_monitor_auto(!cfg_monitor_auto()); cfg_save();
        say(VGA_LIGHT_GREEN, cfg_monitor_auto() ? "The monitor lights by itself when plugged in."
                                                : "The monitor lights only with `gfx on`.");
    }
    else if(*sel==DISP_MON_FIT && (k=='\n'||k=='\r'||k==' '||k==KEY_LEFT||k==KEY_RIGHT)){
        /* keep -> stretch -> keep; centre is `gfx fit centre` only */
        int f = cfg_monitor_fit()==2 ? 1 : 2, r;
        say(VGA_YELLOW, "Applying...");
        r = gfx_monitor_refit(f);
        say(r ? VGA_YELLOW : VGA_LIGHT_GREEN,
            r == XM_EREFUSED_ ? "Shown unscaled: scaling stopped. `dmesg` says why."
            : r ? xm_why_name(r)
            : gfx_monitor_lit() ? "Monitor picture changed." : "Saved for the monitor.");
    }
    else if(*sel<NDISP && (k=='\n'||k=='\r')){
        /* The resolution command is the one tested path into a mode
           change, with its revert-if-it-will-not-sync guard; use it. */
        vga_clear(); vga_set_cursor(0,0);
        vga_set_color(VGA_WHITE,VGA_BLACK);
        cmd_resolution_v6(disp_opts[*sel].arg);
        vga_set_color(VGA_DARK_GREY,VGA_BLACK);
        vga_puts("\n  Any key returns to Settings.");
        keyboard_waitchar();
        say(VGA_LIGHT_GREEN,"Console size saved.");
    }
    else if(k) return 0;
    char cur[32], a[8], b[8];
    utoa_s((uint32_t)shell_pref_cols(),a); utoa_s((uint32_t)shell_pref_rows(),b);
    int p=0;
    for(char *c=a;*c;c++) cur[p++]=*c;
    cur[p++]=' '; cur[p++]='x'; cur[p++]=' ';
    for(char *c=b;*c;c++) cur[p++]=*c;
    cur[p]=0;
    info(3,"Shell console",cur,VGA_WHITE);
    put(PX+1,4,"The desktop and apps adapt to the grid; older apps are",VGA_DARK_GREY,VGA_BLACK);
    put(PX+1,5,"centred in an 80x25 box on a bigger one.",VGA_DARK_GREY,VGA_BLACK);
    for(int i=0;i<NDISP;i++) row(7+i,*sel==i,disp_opts[i].label,0,VGA_WHITE);
    {
        int y = 8+NDISP;
        put(PX+1,y++,"HDMI monitor",VGA_DARK_GREY,VGA_BLACK);
        row(y++,*sel==DISP_MON_AUTO,"Light it by itself", cfg_monitor_auto()?"on":"off",VGA_WHITE);
        row(y++,*sel==DISP_MON_FIT, "Picture", cfg_monitor_fit()==2?"stretched":
                                              cfg_monitor_fit()==0?"centred":"fitted",VGA_WHITE);
    }
    return 1;
}

/* ── Desktop ──────────────────────────────────────────────────────── */
static int page_desktop(int k, int *sel){
    int n=desktop_app_count();
    if(k==KEY_UP && *sel>0) (*sel)--;
    else if(k==KEY_DOWN && *sel<n-1) (*sel)++;
    else if(k==KEY_PGDN){ *sel+=H-8; if(*sel>n-1) *sel=n-1; }
    else if(k==KEY_PGUP){ *sel-=H-8; if(*sel<0) *sel=0; }
    else if(k=='\n'||k=='\r'||k=='d'||k=='D'||k==' '){
        int r=desktop_app_pin_toggle(desktop_app_id(*sel),0);
        say(r<0?VGA_YELLOW:VGA_LIGHT_GREEN, r<0?"The desktop is full.":r?"Icon added to the desktop.":"Icon removed.");
    }
    else if(k=='f'||k=='F'){
        int r=desktop_app_pin_toggle(desktop_app_id(*sel),1);
        say(r<0?VGA_YELLOW:VGA_LIGHT_GREEN, r<0?"Favourites are full.":r?"Added to Favorites.":"Removed from Favorites.");
    }
    else if(k) return 0;
    put(PX+1,2,"Icon  Fav   App",VGA_DARK_GREY,VGA_BLACK);
    int vis=H-7, top=*sel-vis+1; if(top<0) top=0;
    for(int i=top, y=3; i<n && y<3+vis; i++, y++){
        char lab[48]; int p=0;
        const char *id=desktop_app_id(i);
        lab[p++]='['; lab[p++]=desktop_app_pinned(id,0)?'x':' '; lab[p++]=']'; lab[p++]=' '; lab[p++]=' ';
        lab[p++]='['; lab[p++]=desktop_app_pinned(id,1)?'*':' '; lab[p++]=']'; lab[p++]=' '; lab[p++]=' ';
        for(const char *c=desktop_app_name(i);*c&&p<46;c++) lab[p++]=*c;
        lab[p]=0;
        row(y,*sel==i,lab,0,VGA_WHITE);
    }
    return 1;
}

/* ── Shell ────────────────────────────────────────────────────────── */
static const char *prompts[] = {
    "\\u@havendos:\\w\\$ ", "\\w> ", "\\u:\\w\\$ ", "[\\t] \\w\\$ ", "\\$ ",
};
#define NPROMPT ((int)(sizeof prompts/sizeof prompts[0]))
static const char *colnames[16] = {
    "black","blue","green","cyan","red","magenta","brown","light grey",
    "dark grey","light blue","light green","light cyan","light red",
    "light magenta","yellow","white"
};

static int page_shell(int k, int *sel){
    int fg, bg; shell_colors_get(&fg,&bg);
    if(k==KEY_UP && *sel>0) (*sel)--;
    else if(k==KEY_DOWN && *sel<2) (*sel)++;
    else if(*sel==0 && (k=='\n'||k=='\r'||k==KEY_RIGHT||k==KEY_LEFT)){
        const char *cur=cfg_prompt(); int at=-1;
        for(int i=0;i<NPROMPT;i++) if(!strcmp(cur,prompts[i])) at=i;
        if(!cur[0]) at=0;
        at = (k==KEY_LEFT) ? (at<=0?NPROMPT-1:at-1) : (at+1)%NPROMPT;
        cfg_set_prompt(prompts[at]); cfg_save();
        say(VGA_LIGHT_GREEN,"Prompt saved -- the shell uses it from its next prompt.");
    }
    else if(*sel==1 && (k==KEY_RIGHT||k==KEY_LEFT||k=='\n'||k=='\r')){
        do{ fg = (k==KEY_LEFT) ? (fg+15)%16 : (fg+1)%16; }while(fg==VGA_BLACK);
        shell_colors_set(fg,VGA_BLACK); say(VGA_LIGHT_GREEN,"Saved.");
    }
    else if(*sel==2 && (k=='\n'||k=='\r')){
        cfg_set_prompt(""); cfg_save(); shell_colors_set(VGA_WHITE,VGA_BLACK);
        say(VGA_LIGHT_GREEN,"Prompt and colours back to the defaults.");
    }
    else if(k) return 0;
    shell_colors_get(&fg,&bg);
    const char *pp=cfg_prompt(); if(!pp[0]) pp=prompts[0];
    (void)bg;
    row(3,*sel==0,"Prompt",pp,VGA_WHITE);
    row(4,*sel==1,"Text colour",colnames[fg],(vga_color_t)fg);
    row(5,*sel==2,"Back to the defaults",0,VGA_WHITE);
    put(PX+1,8,"How it looks:",VGA_DARK_GREY,VGA_BLACK);
    put(PX+2,9,"kyan@havendos:HOME/kyan$ ",VGA_LIGHT_GREEN,VGA_BLACK);
    put(PX+27,9,"cat notes.txt",(vga_color_t)fg,VGA_BLACK);
    put(PX+1,11,"\\u user  \\h host  \\w folder  \\$ # or $  \\t time  \\d date",VGA_DARK_GREY,VGA_BLACK);
    put(PX+1,12,"Any other prompt: ps1 <format> in the shell.",VGA_DARK_GREY,VGA_BLACK);
    return 1;
}

/* ── Accounts ─────────────────────────────────────────────────────── */
static int page_accounts(int k, int *sel){
    int n=get_user_count();
    int me=get_user_current();
    int admin=current_user_admin();
    if(*sel>n) *sel=n;
    if(k==KEY_UP && *sel>0) (*sel)--;
    else if(k==KEY_DOWN && *sel<n) (*sel)++;
    else if(*sel==n && (k=='\n'||k=='\r'||k=='n'||k=='N')){
        if(!admin){ say(VGA_YELLOW,"Only an administrator can add accounts."); }
        else{
            char nm[16], pw[16], pw2[16];
            if(ask("New account name:",nm,16,0) && nm[0]){
                if(ask("Password (blank for none):",pw,16,1) && ask("Again:",pw2,16,1)){
                    if(strcmp(pw,pw2)) say(VGA_YELLOW,"The passwords did not match. Nothing changed.");
                    else if(add_user(nm,pw,0)) say(VGA_LIGHT_GREEN,"Account created, with a home folder.");
                    else say(VGA_YELLOW,"Could not: that name exists, or there are 16 accounts.");
                }
            }
        }
    }
    else if(*sel<n && (k=='p'||k=='P'||k=='\n'||k=='\r')){
        const char *nm=get_username(*sel);
        if(*sel!=me && !admin){ say(VGA_YELLOW,"You can change only your own password."); }
        else{
            char old[16]="", pw[16], pw2[16];
            int go=1;
            if(!admin){ go=ask("Current password:",old,16,1); }
            if(go && ask("New password:",pw,16,1) && ask("Again:",pw2,16,1)){
                if(strcmp(pw,pw2)) say(VGA_YELLOW,"The passwords did not match. Nothing changed.");
                else{
                    int r=passwd_user(nm,old,pw);
                    say(r==0?VGA_LIGHT_GREEN:VGA_YELLOW, r==0?"Password changed and saved.":r==-2?"That is not the current password.":"No such account.");
                }
            }
        }
    }
    else if(*sel<n && (k=='a'||k=='A')){
        int r=set_user_admin(get_username(*sel),!get_user_admin(*sel));
        say(r==0?VGA_LIGHT_GREEN:VGA_YELLOW, r==0?"Role changed.":r==-3?"That is the last administrator -- keeping it.":"Only an administrator can do that.");
    }
    else if(*sel<n && (k=='d'||k=='D'||k==KEY_DEL)){
        if(!admin) say(VGA_YELLOW,"Only an administrator can delete accounts.");
        else if(*sel==me) say(VGA_YELLOW,"You cannot delete the account you are using.");
        else if(confirm("Delete this account? Its home folder stays.")){
            int r=delete_user(get_username(*sel));
            say(r==0?VGA_LIGHT_GREEN:VGA_YELLOW, r==0?"Deleted.":r==-3?"The built-in admin account cannot be deleted.":"Could not delete it.");
            if(*sel>=get_user_count()) *sel=get_user_count();
        }
    }
    else if(*sel<n && *sel<3 && (k=='h'||k=='H')){
        if(!admin) say(VGA_YELLOW,"Only an administrator can do that.");
        else{ cfg_set_acct_hidden(*sel,!cfg_acct_hidden(*sel)); cfg_save(); say(VGA_LIGHT_GREEN,"Saved."); }
    }
    else if(*sel<n && (k=='l'||k=='L')){
        if(!admin) say(VGA_YELLOW,"Only an administrator can do that.");
        else{
            const char *nm=get_username(*sel);
            int on = cfg_autologin() && !strcmp(cfg_autologin_user(),nm);
            if(on){ cfg_set_autologin(0); say(VGA_LIGHT_GREEN,"Auto-login off: the login screen asks again."); }
            else { cfg_set_autologin(1); cfg_set_autologin_user(nm); say(VGA_YELLOW,"Auto-login on: this machine opens without a password."); }
            cfg_save();
        }
    }
    else if(k) return 0;

    put(PX+1,2,"Account         Role    Password  Last login",VGA_DARK_GREY,VGA_BLACK);
    int y=3;
    for(int i=0;i<n && y<H-5;i++,y++){
        char lab[80]; int p=0;
        const char *nm=get_username(i);
        lab[p++]=(i==me)?'*':' ';
        for(const char *c=nm;*c&&p<16;c++) lab[p++]=*c;
        while(p<17) lab[p++]=' ';
        for(const char *c=get_user_admin(i)?"admin":"user";*c;c++) lab[p++]=*c;
        while(p<25) lab[p++]=' ';
        int ps=get_user_pwstate(i);
        for(const char *c=ps==1?"set":ps==2?"DEFAULT":"none";*c;c++) lab[p++]=*c;
        while(p<35) lab[p++]=' ';
        char when[24];
        if(PW>=60){
            if(user_last_login(nm,when,sizeof when,0)) for(char *c=when;*c&&p<60;c++) lab[p++]=*c;
            else for(const char *c="never";*c;c++) lab[p++]=*c;
        }
        if(i<3 && cfg_acct_hidden(i)){ for(const char *c="  hidden";*c&&p<78;c++) lab[p++]=*c; }
        if(cfg_autologin() && !strcmp(cfg_autologin_user(),nm)){ for(const char *c="  auto";*c&&p<78;c++) lab[p++]=*c; }
        lab[p]=0;
        row(y,*sel==i,lab,0,VGA_WHITE);
        if(ps!=1 && *sel!=i) put(PX+26,y,ps==2?"DEFAULT":"none",VGA_YELLOW,VGA_BLACK);
    }
    row(y+1,*sel==n,"+ New account",0,VGA_WHITE);
    if(*sel<n && get_user_pwstate(*sel)==2)
        put(PX+1,H-2,"DEFAULT = the password in HavenDOS's source. Press P to change it.",VGA_YELLOW,VGA_BLACK);
    return 1;
}

/* ── Network ──────────────────────────────────────────────────────── */
static void ip_s(uint32_t ip, char *o){ ip_to_str(ip,o); }

static int page_network(int k, int *sel){
    if(k==KEY_UP && *sel>0) (*sel)--;
    else if(k==KEY_DOWN && *sel<2) (*sel)++;
    else if(k=='\n'||k=='\r'){
        if(netdev_count()<=0){ say(VGA_YELLOW,"There is no network adapter to configure."); }
        else if(*sel==0){
            put(PX+1,H-3,"Asking the network for an address...",VGA_WHITE,VGA_BLACK); vga_flush();
            int ok=dhcp_request();
            if(ok){ net_config_source_set(NET_CFG_DHCP); say(VGA_LIGHT_GREEN,"Got an address from DHCP."); }
            else say(VGA_YELLOW,"No DHCP server answered. The address is unchanged.");
        } else if(*sel==1){
            char a[20], g[20];
            if(ask("Address (e.g. 192.168.1.50):",a,20,0) && a[0] &&
               ask("Gateway:",g,20,0) && g[0]){
                uint32_t ip=ip_from_str(a), gw=ip_from_str(g);
                if(!ip||!gw) say(VGA_YELLOW,"That is not an address like 192.168.1.50.");
                else{ net_set_ip(ip,0x00FFFFFFu,gw); net_config_source_set(NET_CFG_MANUAL); say(VGA_LIGHT_GREEN,"Address set (mask 255.255.255.0)."); }
            }
        } else {
            char d[20];
            if(ask("DNS server:",d,20,0) && d[0]){
                uint32_t ip=ip_from_str(d);
                if(!ip) say(VGA_YELLOW,"That is not an address.");
                else{ net_set_dns(ip); say(VGA_LIGHT_GREEN,"DNS server set."); }
            }
        }
    }
    else if(k) return 0;

    char b[24];
    int y=3;
    if(netdev_count()<=0){
        put(PX+1,y++,"No network adapter found.",VGA_YELLOW,VGA_BLACK);
        put(PX+1,y++,"`lspci` and `usb eth` show what the machine has.",VGA_DARK_GREY,VGA_BLACK);
        y++;
    } else {
        info(y++,"Adapter",netdev_bus_name(0),VGA_WHITE);
        info(y++,"Link",netdev_link_up(0)?"up":"down",netdev_link_up(0)?VGA_WHITE:VGA_YELLOW);
        ip_s(net_get_ip(),b);  info(y++,"Address",b,VGA_WHITE);
        ip_s(net_get_gw(),b);  info(y++,"Gateway",b,VGA_WHITE);
        ip_s(net_get_dns(),b); info(y++,"DNS",b,VGA_WHITE);
        int src=net_config_source();
        info(y++,"Came from",src==NET_CFG_DHCP?"DHCP":src==NET_CFG_MANUAL?"set by hand":"fallback -- does not route",
            src==NET_CFG_FALLBACK?VGA_YELLOW:VGA_WHITE);
        y++;
    }
    row(y++,*sel==0,"Get an address from DHCP",0,VGA_WHITE);
    row(y++,*sel==1,"Set an address by hand",0,VGA_WHITE);
    row(y++,*sel==2,"Set the DNS server",0,VGA_WHITE);
    y++;
    put(PX+1,y,"Changes last until the next boot, which asks DHCP again.",VGA_DARK_GREY,VGA_BLACK);
    return 1;
}

/* ── Startup ──────────────────────────────────────────────────────── */
static const int ss_steps[] = { 0, 60, 300, 600, 1800 };
static const int al_steps[] = { 0, 3, 5, 10 };

static int page_startup(int k, int *sel){
    int admin=current_user_admin();
    if(k==KEY_UP && *sel>0) (*sel)--;
    else if(k==KEY_DOWN && *sel<4) (*sel)++;
    else if(k=='\n'||k=='\r'||k==KEY_RIGHT||k==KEY_LEFT||k==' '){
        if(!admin && *sel!=2){ say(VGA_YELLOW,"Startup settings are for administrators."); }
        else switch(*sel){
        case 0: cfg_set_verbose_boot(!cfg_verbose_boot()); cfg_save(); say(VGA_LIGHT_GREEN,"Saved: next boot."); break;
        case 1: cfg_set_post_screen(!cfg_post_screen());   cfg_save(); say(VGA_LIGHT_GREEN,"Saved: next boot."); break;
        case 2: {
            int cur=cfg_screensaver_timeout(), at=0;
            for(int i=0;i<5;i++) if(ss_steps[i]==cur) at=i;
            at = (k==KEY_LEFT) ? (at+4)%5 : (at+1)%5;
            cfg_set_screensaver_timeout(ss_steps[at]); cfg_save(); say(VGA_LIGHT_GREEN,"Saved.");
            break; }
        case 3: {
            int cur=cfg_autologin_timeout(), at=0;
            for(int i=0;i<4;i++) if(al_steps[i]==cur) at=i;
            at = (k==KEY_LEFT) ? (at+3)%4 : (at+1)%4;
            cfg_set_autologin_timeout(al_steps[at]); cfg_save(); say(VGA_LIGHT_GREEN,"Saved.");
            break; }
        case 4: {
            char h[24];
            if(ask("Hostname:",h,24,0) && h[0]){ cfg_set_hostname(h); cfg_save(); say(VGA_LIGHT_GREEN,"Hostname saved."); }
            break; }
        }
    }
    else if(k) return 0;
    char ss[24]; int t=cfg_screensaver_timeout();
    if(!t){ ss[0]='o'; ss[1]='f'; ss[2]='f'; ss[3]=0; }
    else { utoa_s((uint32_t)(t>=60?t/60:t),ss); int p=(int)strlen(ss); const char *u=t>=60?" min":" s"; while(*u) ss[p++]=*u++; ss[p]=0; }
    char al[24]; utoa_s((uint32_t)cfg_autologin_timeout(),al); { int p=(int)strlen(al); const char *u=" s"; while(*u) al[p++]=*u++; al[p]=0; }
    row(3,*sel==0,"Boot log",cfg_verbose_boot()?"scrolling log":"loading screen",VGA_WHITE);
    row(4,*sel==1,"Hardware summary at boot",cfg_post_screen()?"shown":"skipped",VGA_WHITE);
    row(5,*sel==2,"Screensaver after",ss,VGA_WHITE);
    row(6,*sel==3,"Auto-login countdown",al,VGA_WHITE);
    row(7,*sel==4,"Hostname",cfg_hostname(),VGA_WHITE);
    put(PX+1,9, cfg_autologin()?"Auto-login is on (Accounts, L to change).":"Auto-login is off (Accounts, L on an account).",VGA_DARK_GREY,VGA_BLACK);
    return 1;
}

/* ── About ────────────────────────────────────────────────────────── */
static int page_about(int k, int *sel){
    (void)sel;
    if(k && k!=KEY_UP && k!=KEY_DOWN) return 0;
    const theme_t *th=ui_theme();
    char mem[24]; utoa_s((pmm_total_blocks()*4u)/1024u,mem); { int p=(int)strlen(mem); const char *u=" MB"; while(*u) mem[p++]=*u++; mem[p]=0; }
    /* v0.9.7: the one version string (kernel/verinfo.c) */
    { extern const char *ver_display(void); put(PX+1,3,ver_display(),th->accent,VGA_BLACK); }
    put(PX+1,4,"Written from scratch in C and x86 assembly.",VGA_LIGHT_GREY,VGA_BLACK);
    info(6,"Made by",HAVENDOS_VENDOR,VGA_WHITE);
    info(7,"Kernel","x86 32-bit, Multiboot, GRUB 2",VGA_WHITE);
    info(8,"Memory",mem,VGA_WHITE);
    info(9,"Hostname",cfg_hostname(),VGA_WHITE);
    info(10,"Signed in as",current_username(),VGA_WHITE);
    info(12,"Source and releases","github.com/MGKFN/HavenDOS",VGA_WHITE);
    info(13,"Updates","`update` installs a new kernel",VGA_WHITE);
    info(14,"Everything else","`system`, `help`, `guide`",VGA_WHITE);
    put(PX+1,16,HAVENDOS_EA_LINE1,VGA_YELLOW,VGA_BLACK);
    return 1;
}

/* ── the loop ─────────────────────────────────────────────────────── */
static const char *page_keys(int s){
    switch(s){
    case S_APPEAR:   return "Up/Down choose  Enter apply  Left back";
    case S_DISPLAY:  return "Up/Down choose  Enter apply/change  Left back";
    case S_DESKTOP:  return "Enter/D desktop icon  F favourite  Left back";
    case S_SHELL:    return "Up/Down choose  Left/Right change  ESC back";
    case S_ACCOUNTS: return "P password  A role  D delete  H hide  L auto-login  ESC back";
    case S_NETWORK:  return "Up/Down choose  Enter do it  Left back";
    case S_STARTUP:  return "Up/Down choose  Enter/Left/Right change  ESC back";
    }
    return "Left or ESC back";
}

static int run_page(int s, int k, int *sel){
    switch(s){
    case S_APPEAR:   return page_appearance(k,sel);
    case S_DISPLAY:  return page_display(k,sel);
    case S_DESKTOP:  return page_desktop(k,sel);
    case S_SHELL:    return page_shell(k,sel);
    case S_ACCOUNTS: return page_accounts(k,sel);
    case S_NETWORK:  return page_network(k,sel);
    case S_STARTUP:  return page_startup(k,sel);
    case S_ABOUT:    return page_about(k,sel);
    }
    return 0;
}

void settings_open(const char *section){
    int sect=0, focus=0, sel[S_N];
    /* v0.9.6.5: the mouse is Settings' however it was opened -- the
       desktop's right-click menu and the Accounts shortcut call this
       inside the desktop's own task, where the mouse layer is off
       because the desktop reads the mouse itself. */
    int um_was=um_mode();
    um_set_mode(UM_ALL);
    for(int i=0;i<S_N;i++) sel[i]=0;
    sel[S_APPEAR]=ui_current_theme();
    g_msg[0]=0;
    desktop_pins_reload();
    if(section && section[0]){
        for(int i=0;i<S_N;i++) if(!strcmp(sect_keys[i],section)){ sect=i; focus=1; }
    }
    int key=0;
    for(;;){
        vga_begin_frame();
        frame(sect,focus,page_keys(sect));
        /* Pages take the key and draw; with focus on the sidebar they only draw. */
        int used = run_page(sect, focus ? key : 0, &sel[sect]);
        if(focus && key && !used){
            /* not the page's: navigation */
        }
        /* redraw once more so a page's state change shows this frame */
        if(focus && key && used){
            frame(sect,focus,page_keys(sect));
            run_page(sect,0,&sel[sect]);
        }
        vga_end_frame();
        vga_flush();

        key=keyboard_waitchar();
        if(key==KEY_RESIZE){ key=0; continue; }   /* v0.9.9: the window changed size: lay out again */
        if(!focus){
            g_msg[0]=0;
            if(key==27||key=='q'||key=='Q') break;
            if(key==KEY_UP)   { sect=(sect+S_N-1)%S_N; key=0; }
            else if(key==KEY_DOWN) { sect=(sect+1)%S_N; key=0; }
            else if(key=='\n'||key=='\r'||key==KEY_RIGHT){ focus=1; key=0; }
            else key=0;
        } else {
            if(key==27 || (key==KEY_LEFT && sect!=S_SHELL && sect!=S_STARTUP)){
                focus=0; key=0; g_msg[0]=0;
            }
        }
    }
    um_set_mode(um_was);
    vga_clear();
}

void app_settings_v2(void){ settings_open(""); }
