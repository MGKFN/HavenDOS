/*
 * login.c — HavenDOS v0.6.8
 * Win11-style login:
 *   - Full theme-aware background (desk_bg colour + pattern)
 *   - Live clock centred near top
 *   - User tiles bottom-left (like Win11 user switcher)
 *   - Selected user expands: centred password field appears
 *   - Mouse-aware tile highlighting
 */
#include "../include/vga.h"
#include "../include/uimouse.h"
#include "../include/task.h"
#include "../include/klog.h"
extern void notif_push(const char *);   /* v0.9.6 */
#include "version.h"
#include "../include/io.h"
#include "../include/machine.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/theme.h"
#include "../include/rtc.h"

extern int  keyboard_waitchar(void);
extern int  keyboard_getchar(void);
extern void ui_draw_window(int,int,int,int,const char*);
extern int  vfs_write(const char*, const char*, uint32_t);
extern int  vfs_read(const char*, char*, uint32_t);
extern void vfs_set_user(const char*, int);
extern void sleep_ms(uint32_t);
extern void setup_user_home(const char *username);
extern void shell_set_home(const char *username);
extern void ui_load_theme(void);
extern void mouse_get(int*,int*,int*);
extern int  mouse_is_ready(void);

/* ── Password hashing (djb2) ──────────────────────────────────── */
static uint32_t pw_hash(const char *s){
    uint32_t h=5381;
    while(*s) h=((h<<5)+h)^(uint8_t)*s++;
    return h;
}

/* ── User database ────────────────────────────────────────────── */
#define MAX_USERS 16
typedef struct {
    char     name[16];
    uint32_t pw_hash;
    int      admin;
} user_t;

static user_t users[MAX_USERS]={
    {"admin",  0, 1},
    {"bootos", 0, 0},
    {"guest",  0, 0},
};
static int user_count    = 3;
static int logged_in_idx = 0;
static int login_done    = 0;
static int login_inited  = 0;

/* ── Audit log ────────────────────────────────────────────────── */
/* v0.9.5: "[kyan] login OK", with no time, in a 512-byte buffer that
   started empty every boot and overwrote the file -- so `log` showed
   this boot's logins and nothing else, and nothing could say when an
   account was last used. Now each line carries the date and time, the
   file is read back at boot and appended to, and the oldest whole lines
   go when it passes AUDIT_MAX.

     2026-11-14 10:32  kyan  login OK

   Lines in the old "[user] event" form are kept until they rotate out;
   user_last_login() reads both. */
#define AUDIT_MAX 2048
static char audit_buf[AUDIT_MAX];
static int  audit_len = 0;
static int  audit_loaded = 0;

static void audit_load(void){
    if(audit_loaded) return;
    audit_loaded=1;
    int r=vfs_read(".loginlog",audit_buf,AUDIT_MAX-1);
    audit_len = (r>0) ? r : 0;
    audit_buf[audit_len]=0;
}

static void put2(char *d, int v){ d[0]=(char)('0'+(v/10)%10); d[1]=(char)('0'+v%10); }

static void audit(const char *user, const char *event){
    audit_load();
    char line[96]; int n=0;
    uint8_t h=0,mi=0,se=0,day=0,mon=0; uint16_t yr=0;
    rtc_get_time(&h,&mi,&se); rtc_get_date(&day,&mon,&yr);
    put2(line+n,yr/100); n+=2; put2(line+n,yr%100); n+=2; line[n++]='-';
    put2(line+n,mon); n+=2; line[n++]='-'; put2(line+n,day); n+=2; line[n++]=' ';
    put2(line+n,h); n+=2; line[n++]=':'; put2(line+n,mi); n+=2;
    line[n++]=' '; line[n++]=' ';
    for(const char *p=user; *p && n<40; p++) line[n++]=*p;
    line[n++]=' '; line[n++]=' ';
    for(const char *p=event; *p && n<94; p++) line[n++]=*p;
    line[n++]='\n';

    if(audit_len+n >= AUDIT_MAX){
        /* Drop whole lines from the front until this one fits. */
        int cut = audit_len + n - (AUDIT_MAX - 1);
        while(cut < audit_len && audit_buf[cut-1] != '\n') cut++;
        if(cut > audit_len) cut = audit_len;
        memmove(audit_buf, audit_buf+cut, (size_t)(audit_len-cut));
        audit_len -= cut;
    }
    memcpy(audit_buf+audit_len, line, (size_t)n);
    audit_len += n;
    audit_buf[audit_len]=0;
    vfs_write(".loginlog",audit_buf,(uint32_t)audit_len);
}

/* Does this log line record `name` logging in? Fills `when` (16 chars,
   "YYYY-MM-DD HH:MM") for the new format, "" for the old. */
static int log_line_is_login(const char *l, int len, const char *name, char *when){
    int nl=(int)strlen(name);
    when[0]=0;
    if(len>=18 && l[0]>='0' && l[0]<='9' && l[4]=='-'){
        const char *u=l+18;
        if(len-18 < nl+2 || strncmp(u,name,(size_t)nl) || u[nl]!=' ') return 0;
        const char *ev=u+nl; while(*ev==' ') ev++;
        if(strncmp(ev,"login OK",8) && strncmp(ev,"auto-login",10) &&
           strncmp(ev,"account created + login",23)) return 0;
        memcpy(when,l,16); when[16]=0;
        return 1;
    }
    if(len>nl+2 && l[0]=='[' && !strncmp(l+1,name,(size_t)nl) && l[1+nl]==']'){
        const char *ev=l+nl+3;
        return !strncmp(ev,"login OK",8) || !strncmp(ev,"auto-login",10)
            || !strncmp(ev,"account created",15);
    }
    return 0;
}

/* The most recent login for `name`: 1 and `out` filled ("YYYY-MM-DD
   HH:MM", or "before 0.9.5" for an undated line), 0 if none on record.
   `count` gets how many logins the log holds. */
int user_last_login(const char *name, char *out, int max, int *count){
    audit_load();
    int found=0, c=0;
    char when[20], best[20]; best[0]=0;
    const char *p=audit_buf;
    while(*p){
        const char *e=p; while(*e && *e!='\n') e++;
        if(log_line_is_login(p,(int)(e-p),name,when)){
            c++; found=1;
            int k=0; const char *w = when[0] ? when : "before 0.9.5";
            while(w[k] && k<19){ best[k]=w[k]; k++; } best[k]=0;
        }
        p = *e ? e+1 : e;
    }
    if(count) *count=c;
    if(found && out && max>0){ int k=0; while(best[k] && k<max-1){ out[k]=best[k]; k++; } out[k]=0; }
    return found;
}

/* The whole log, for `log` and Settings > Accounts. */
const char *audit_text(void){ audit_load(); return audit_buf; }

static void accounts_load(void);
static void accounts_save(void);

void login_init(void){
    if(login_inited) return;
    login_inited=1;
    users[0].pw_hash=pw_hash("Admin@123");
    users[1].pw_hash=pw_hash("BootOS2026");
    users[2].pw_hash=pw_hash("");
    ui_load_theme();
    accounts_load();
}

/* ── Public API ───────────────────────────────────────────────── */
const char *current_username(void)   { return users[logged_in_idx].name;  }
int         current_user_admin(void) { return users[logged_in_idx].admin; }
int         get_user_count(void)     { return user_count; }
const char *get_username(int i)      { return i<user_count?users[i].name:"";  }
int         get_user_admin(int i)    { return i<user_count?users[i].admin:0;  }
int         get_user_current(void)   { return logged_in_idx; }
/* 0 no password, 1 a password of the user's own, 2 the built-in account
   still on the password printed in this source file -- which anyone who
   has read it can type. `users` says so in yellow. */
int get_user_pwstate(int i){
    if(i<0||i>=user_count) return 0;
    if(users[i].pw_hash==pw_hash("")) return 0;
    if(i==0 && users[i].pw_hash==pw_hash("Admin@123"))  return 2;
    if(i==1 && users[i].pw_hash==pw_hash("BootOS2026")) return 2;
    return 1;
}
int get_user_hidden(int i){
    extern int cfg_acct_hidden(int);
    return (i>=0 && i<3) ? cfg_acct_hidden(i) : 0;
}
/* Admins only; refuses to leave the machine without one. */
int set_user_admin(const char *name, int admin){
    if(!current_user_admin()) return -2;
    int at=-1, admins=0;
    for(int i=0;i<user_count;i++){
        if(users[i].admin) admins++;
        if(strcmp(users[i].name,name)==0) at=i;
    }
    if(at<0) return -1;
    if(!admin && users[at].admin && admins<=1) return -3;
    users[at].admin = admin ? 1 : 0;
    audit(current_username(), admin ? "made an admin" : "made a standard user");
    accounts_save();
    return 0;
}

/* ── Account persistence ──────────────────────────────────────── */
#define ACCOUNTS_FILE ".accounts"

static void accounts_save(void){
    static char buf[MAX_USERS * 48];
    int p=0;
    for(int i=0;i<user_count;i++){
        const char *n=users[i].name;
        while(*n && p<(int)sizeof(buf)-4) buf[p++]=*n++;
        buf[p++]=':';
        char hs[12]; uint32_t hv=users[i].pw_hash;
        hs[10]=0;
        for(int d=9;d>=0;d--){ hs[d]='0'+hv%10; hv/=10; }
        const char *hp=hs;
        while(*hp && p<(int)sizeof(buf)-4) buf[p++]=*hp++;
        buf[p++]=':';
        buf[p++]=users[i].admin?'1':'0';
        buf[p++]='\n';
    }
    vfs_write(ACCOUNTS_FILE, buf, (uint32_t)p);
}

static void accounts_load(void){
    static char buf[MAX_USERS * 48];
    int r=vfs_read(ACCOUNTS_FILE, buf, sizeof(buf)-1);
    if(r<=0) return;
    buf[r]=0;
    users[0].pw_hash=pw_hash("Admin@123");
    users[1].pw_hash=pw_hash("BootOS2026");
    users[2].pw_hash=pw_hash("");
    user_count=3;
    char *line=buf;
    int idx=0;
    while(*line && idx<MAX_USERS){
        char *end=line;
        while(*end && *end!='\n') end++;
        char save=*end; *end=0;
        char *sep1=line; while(*sep1 && *sep1!=':') sep1++;
        if(*sep1==':'){
            *sep1=0;
            char *sep2=sep1+1; while(*sep2 && *sep2!=':') sep2++;
            if(*sep2==':'){
                *sep2=0;
                const char *nm=line;
                uint32_t hv=0;
                for(const char *d=sep1+1;*d>='0'&&*d<='9';d++) hv=hv*10+(*d-'0');
                int adm=(*(sep2+1)=='1');
                int found=-1;
                for(int i=0;i<user_count;i++)
                    if(strcmp(users[i].name,nm)==0){found=i;break;}
                if(found>=0){
                    users[found].pw_hash=hv;
                    users[found].admin=adm;
                } else if(user_count<MAX_USERS){
                    strncpy(users[user_count].name,nm,15);
                    users[user_count].pw_hash=hv;
                    users[user_count].admin=adm;
                    user_count++;
                }
            }
        }
        *end=save;
        line=(*end)?end+1:end;
        idx++;
    }
}

int add_user(const char *name, const char *pass, int admin){
    if(user_count>=MAX_USERS) return 0;
    if(strlen(name)<1||strlen(name)>15) return 0;
    for(int i=0;i<user_count;i++)
        if(strcmp(users[i].name,name)==0) return 0;
    strncpy(users[user_count].name,name,15);
    users[user_count].pw_hash=pw_hash(pass);
    users[user_count].admin=admin;
    user_count++;
    audit(current_username(),"added user");
    accounts_save();
    setup_user_home(name);     /* v0.9.5: HOME/<name>, as setup does */
    return 1;
}

int delete_user(const char *name){
    if(!current_user_admin()) return -2;
    for(int i=0;i<user_count;i++){
        if(strcmp(users[i].name,name)==0){
            if(i==0) return -3;
            for(int j=i;j<user_count-1;j++) users[j]=users[j+1];
            user_count--;
            audit(current_username(),"deleted user");
            accounts_save();
            return 0;
        }
    }
    return -1;
}

/* Public verify function used by shell lock command */
int shell_verify_password(const char *username, const char *pw) {
    uint32_t hv = pw_hash(pw);
    for (int i = 0; i < user_count; i++) {
        if (!strcmp(users[i].name, username))
            return (users[i].pw_hash == hv) ? 1 : 0;
    }
    return 0;
}

int passwd_user(const char *name, const char *oldpass, const char *newpass){
    for(int i=0;i<user_count;i++){
        if(strcmp(users[i].name,name)==0){
            if(!current_user_admin())
                if(pw_hash(oldpass)!=users[i].pw_hash) return -2;
            users[i].pw_hash=pw_hash(newpass);
            audit(name,"password changed");
            /* v0.9.5: and saved. It never was: `passwd` reported
               "Password changed." and the old password came back at
               the next boot. */
            accounts_save();
            return 0;
        }
    }
    return -1;
}

/* ================================================================
   THEME-AWARE BACKGROUND
   Fills rows 0-24 with desk_bg colour + theme pattern.
   Row 0 and 24 get the bar_bg colour (thin status strips).
   ================================================================ */
static void draw_theme_bg(void){
    theme_t *th = ui_theme();
    int t = ui_current_theme();
    /* v0.9.7.1: the WHOLE screen. This drew rows 0-24 and columns 0-79
       and nothing else, so on the HP's 128x48 the Start menu, the
       taskbar and whatever the Shell had printed stayed visible below
       and beside the sign-in screen -- Kyan's photo of 22 Nov. A dialog
       box is centred in an 80x25 viewport by lg_box(); the background
       is drawn with none. */
    vga_viewport_clear();
    int W = vga_cols(), H = vga_rows();

    /* Full background with the theme's wallpaper -- the same function the
       desktop draws with, so the two can no longer disagree (they did:
       this screen patterned themes 2, 3, 4 and 6, the desktop all ten). */
    for(int y=1;y<H-1;y++){
        for(int x=0;x<W;x++){
            char c=' ';
            vga_color_t fg=th->desk_bg;
            ui_pattern_at(x,y,&c,&fg);
            vga_putchar_at(c,x,y,fg,th->desk_bg);
        }
    }
    (void)t;

    /* Top strip: thin status bar */
    for(int x=0;x<W;x++) vga_putchar_at(' ',x,0,th->bar_fg,th->bar_bg);
    vga_puts_at(" HavenDOS " HAVENDOS_VER,0,0,th->accent,th->bar_bg);
    vga_puts_at("TechHaven Studios",(W-17)/2,0,th->bar_fg,th->bar_bg);
    /* Theme name top-right */
    const char *tn = ui_theme_name(ui_current_theme());
    int tnlen = (int)strlen(tn);
    vga_puts_at(tn, W-1-tnlen, 0, th->accent, th->bar_bg);

    /* Bottom strip: hint bar */
    for(int x=0;x<W;x++) vga_putchar_at(' ',x,H-1,th->bar_fg,th->bar_bg);
    vga_puts_at(" Enter=Select  Up/Down=Switch  ESC=Back",0,H-1,th->bar_fg,th->bar_bg);
}

/* v0.9.7.1: the account dialogs were laid out for 80x25 at absolute
   positions. lg_box() gives them an 80x25 box in the middle of the
   screen -- the same viewport an app gets -- and draw_theme_bg() takes
   it away again before it paints. */
static void lg_box(void){ vga_viewport_centre(80, 25); }
static int pw_px(void){ int x = (vga_cols() - 32) / 2; return x < 0 ? 0 : x; }
static int pw_py(void){ int y = (vga_rows() - 7) / 2;  return y < 7 ? 7 : y; }

/* ================================================================
   CLOCK — centred, large-ish, drawn near top of screen
   Shows HH:MM  Day DD Mon YYYY
   ================================================================ */
static void draw_clock(void){
    theme_t *th = ui_theme();
    uint8_t h,m,s2,day,mon; uint16_t yr;
    rtc_get_time(&h,&m,&s2);
    rtc_get_date(&day,&mon,&yr);

    /* Time string: HH:MM */
    char ts[6];
    ts[0]='0'+h/10; ts[1]='0'+h%10;
    ts[2]=':';
    ts[3]='0'+m/10; ts[4]='0'+m%10;
    ts[5]=0;

    /* Date string: Mon DD */
    const char *months[]={"Jan","Feb","Mar","Apr","May","Jun",
                           "Jul","Aug","Sep","Oct","Nov","Dec"};
    const char *mname=(mon>=1&&mon<=12)?months[mon-1]:"???";
    char ds[16];
    ds[0]=mname[0]; ds[1]=mname[1]; ds[2]=mname[2];
    ds[3]=' ';
    ds[4]='0'+day/10; ds[5]='0'+day%10;
    ds[6]=0;

    /* Draw time large (doubled-width via spacing) */
    int tx = (vga_cols() - 9) / 2;   /* centre "HH : MM" with spaces */
    char wide[12];
    wide[0]=ts[0]; wide[1]=' '; wide[2]=ts[1]; wide[3]=' ';
    wide[4]=ts[2]; wide[5]=' ';
    wide[6]=ts[3]; wide[7]=' '; wide[8]=ts[4];
    wide[9]=0;
    vga_puts_at(wide, tx, 3, th->desk_fg, th->desk_bg);

    /* Date below */
    int dx = (vga_cols()-(int)strlen(ds))/2;
    vga_puts_at(ds, dx, 5, th->desk_fg, th->desk_bg);
}

/* ================================================================
   USER TILES — Win11 bottom-left style
   Each tile: 18 wide × 3 tall, stacked bottom-left
   Selected tile gets accent background + arrow indicator
   sel/mouse_hover index into vis_users[], not users[]
   ================================================================ */
#define TILE_X      2
#define TILE_H      3

/* v0.7.19 — TILE_W was a fixed 20 and TILE_BASE a fixed 23.

   The width was the worse of the two.  The name is drawn at TILE_X+5 and the
   selection marker at TILE_X+TILE_W-2, so with TILE_W=20 the marker landed on
   column 20 while a 15-character name ran to column 21 — the marker was
   written INTO the name, replacing its 14th character.  Setup's own prompt
   says "Username (3-15 characters)", so it invites people straight into the
   fault: an account created as "kyanhoekstra123" appeared on the login screen
   as "kyanhoekstra1>3".  The [A] administrator badge, drawn after the name,
   ran off the end of the tile entirely for the same reason.

   TILE_BASE=23 is the bottom row of an 80x25 screen; in 80x50 or 90x60 the
   tiles were stranded in the middle of the display. */

static int tile_w(void);
static int tile_base(void);
#define TILE_W      (tile_w())
#define TILE_BASE   (tile_base())

/* Visible user list — rebuilt each time the login screen starts */
static int vis_users[MAX_USERS]; /* maps visible index → users[] index */
static int vis_count = 0;

static void build_vis_list(void){
    extern int cfg_acct_hidden(int);
    vis_count=0;
    /* First 3 slots are built-in accounts; check their hidden flag */
    for(int i=0;i<user_count;i++){
        /* Built-in accounts: admin(0) bootos(1) guest(2) */
        if(i<3 && cfg_acct_hidden(i)) continue;
        vis_users[vis_count++]=i;
    }
    /* Always show at least admin if nothing visible */
    if(vis_count==0){ vis_users[0]=0; vis_count=1; }
}

/* Wide enough for the longest name actually on screen, plus the badge and the
   marker, and never wider than the display. */
static int tile_w(void)
{
    int longest = 0;
    for(int i=0;i<vis_count&&i<6;i++){
        int n=(int)strlen(users[vis_users[i]].name);
        if(n>longest) longest=n;
    }
    /* "[k] " + name + " [A]" + " >" — the +11 is what keeps the marker clear
       of the badge and the badge clear of the name. */
    int w = longest + 11;
    if(w < 20) w = 20;
    int max = vga_cols() - TILE_X - 1;
    if(w > max) w = max;
    return w;
}

/* The tiles stack upward from just above the bottom of whatever mode is
   active, not from row 23. */
static int tile_base(void)
{
    int b = vga_rows() - 2;
    if(b < 10) b = 10;
    return b;
}

static void draw_tiles(int sel, int mouse_hover){
    theme_t *th = ui_theme();
    const int TW = TILE_W;          /* evaluate once: it walks the user list */

    for(int i=0;i<vis_count&&i<6;i++){
        int ui=vis_users[i]; /* real user index */
        int ty = TILE_BASE - i*TILE_H - (i>0?i:0);
        if(ty < 7) break;

        int is_sel   = (i==sel);
        int is_hover = (i==mouse_hover && !is_sel);

        vga_color_t sfg, sbg; ui_sel_colors(&sfg, &sbg);
        vga_color_t bg = is_sel   ? sbg          :
                         is_hover ? th->bar_bg   : th->desk_bg;
        vga_color_t fg = is_sel   ? sfg          :
                         is_hover ? th->accent   : th->bar_fg;

        for(int row=0;row<TILE_H;row++)
            for(int x=TILE_X;x<TILE_X+TW;x++)
                vga_putchar_at(' ',x,ty-TILE_H+1+row,fg,bg);

        vga_putchar_at('[', TILE_X+1, ty-1, fg, bg);
        vga_putchar_at(users[ui].name[0], TILE_X+2, ty-1, fg, bg);
        vga_putchar_at(']', TILE_X+3, ty-1, fg, bg);

        /* The marker sits at TILE_X+TW-2 and the badge just after the name, so
           the name may use everything up to the badge.  On a display too narrow
           for the whole name it is cut rather than drawn over the furniture. */
        int nlen = (int)strlen(users[ui].name);
        int name_room = TW - 10;             /* "[k] " ... " [A] >" */
        if(name_room < 1) name_room = 1;
        if(nlen > name_room) nlen = name_room;
        for(int k=0;k<nlen;k++)
            vga_putchar_at(users[ui].name[k], TILE_X+5+k, ty-1, fg, bg);

        if(users[ui].admin)
            vga_puts_at("[A]", TILE_X+5+nlen+1, ty-1,
                        is_sel ? th->bar_bg : VGA_YELLOW, bg);
        if(is_sel)
            vga_putchar_at('>', TILE_X+TW-2, ty-1, th->bar_bg, bg);

        if(i<vis_count-1&&i<5){
            int sep_y=ty-TILE_H;
            if(sep_y>=7)
                for(int x=TILE_X;x<TILE_X+TW;x++)
                    vga_putchar_at(' ',x,sep_y,VGA_DARK_GREY,th->desk_bg);
        }
    }
}

/* ================================================================
   PASSWORD FIELD — centred on screen, appears after tile select
   ================================================================ */
static void draw_pw_panel(int user_idx, int attempts){
    theme_t *th = ui_theme();

    /* Panel: 32 wide, 7 tall, centred -- on the screen it is on (v0.9.7.1) */
    int px=pw_px(), py=pw_py(), pw=32, ph=7;
    ui_draw_window(px,py,pw,ph,"Sign in");

    /* Username */
    vga_puts_at("User:", px+2, py+2, th->bar_fg, th->win_bg);
    vga_puts_at(users[user_idx].name, px+8, py+2, th->accent, th->win_bg);
    if(users[user_idx].admin)
        vga_puts_at("[admin]",
                    px+8+(int)strlen(users[user_idx].name)+1,
                    py+2, VGA_YELLOW, th->win_bg);

    /* Divider */
    for(int x=px+1;x<px+pw-1;x++)
        vga_putchar_at(' ',x,py+3,VGA_DARK_GREY,th->win_bg);

    /* Password label + input underline */
    vga_puts_at("Password:", px+2, py+4, th->bar_fg, th->win_bg);
    for(int x=px+12;x<px+pw-2;x++)
        vga_putchar_at('_',x,py+4,VGA_DARK_GREY,th->win_bg);

    /* Hint */
    vga_puts_at("ESC = back to users",px+2,py+5,VGA_DARK_GREY,th->win_bg);

    /* Wrong password */
    if(attempts>0){
        for(int x=px+1;x<px+pw-1;x++)
            vga_putchar_at(' ',x,py+6,VGA_WHITE,VGA_RED);
        vga_puts_at("Wrong password",px+9,py+6,VGA_WHITE,VGA_RED);
    }
}

/* Read password into buf at centred position */
/* v0.9.7.1: `first` is the key that was already read when the panel
   came up. The loop below the panel used to read one key, throw it
   away -- `(void)c` -- and only then start this function, so the FIRST
   character of every password typed straight after choosing a user
   was lost and "Wrong password" followed. Found in QEMU by a harness
   typing Admin@123 and getting eight stars. -1 means none. */
static void read_pw(char *buf, int max, int px, int py, int first){
    int pos=0; buf[0]=0;
    int fx=px+12, fy=py+4;
    vga_set_cursor(fx,fy);
    uint8_t last_s=0xFF;
    while(1){
        /* Keep the login clock ticking while waiting for keystrokes */
        uint8_t ch2,cm2,cs2; rtc_get_time(&ch2,&cm2,&cs2);
        if(cs2!=last_s){ last_s=cs2; draw_clock(); vga_set_cursor(fx+pos,fy); }

        int c=first; first=-1;
        if(c==-1) c=keyboard_getchar();
        if(c==-1){ sleep_ms(20); continue; }
        if(c=='\n'||c=='\r') break;
        if((c=='\b'||c==127)&&pos>0){
            pos--; buf[pos]=0;
            vga_putchar_at('_',fx+pos,fy,VGA_DARK_GREY,VGA_BLACK);
            vga_set_cursor(fx+pos,fy);
        } else if(c==27){ buf[0]=0; return; }
        else if(c>=32&&c<127&&pos<max-1){
            buf[pos++]=c; buf[pos]=0;
            vga_putchar_at('*',fx+pos-1,fy,ui_theme()->accent,VGA_BLACK);
            vga_set_cursor(fx+pos,fy);
        }
    }
}

static int try_login(const char *name, const char *pass){
    for(int i=0;i<user_count;i++)
        if(strcmp(users[i].name,name)==0&&pw_hash(pass)==users[i].pw_hash)
            return i;
    return -1;
}

/* ================================================================
   FIRST-BOOT ACCOUNT CREATOR — keep existing, just theme it
   ================================================================ */
static void read_field_plain(char *buf, int max, int x, int y, int hidden){
    int pos=0; buf[0]=0;
    vga_set_cursor(x,y);
    while(1){
        int c=keyboard_waitchar();
        if(c=='\n'||c=='\r') break;
        if((c=='\b'||c==127)&&pos>0){
            pos--; buf[pos]=0;
            vga_putchar_at(' ',x+pos,y,VGA_WHITE,VGA_BLACK);
            vga_set_cursor(x+pos,y);
        } else if(c==27){ buf[0]=0; return; }
        else if(c>=32&&c<127&&pos<max-1){
            buf[pos++]=c; buf[pos]=0;
            vga_putchar_at(hidden?'*':c,x+pos-1,y,
                           ui_theme()->accent,VGA_BLACK);
        }
    }
}

/* v0.9.6.1: has anyone made an account here? See the call in login_screen. */
static int has_own_account(void){
    for(int i=0;i<user_count;i++)
        if(strcmp(users[i].name,"admin") && strcmp(users[i].name,"bootos") &&
           strcmp(users[i].name,"guest"))
            return 1;
    return 0;
}

static void first_boot_creator(void){
    int sel=0;
    while(1){
        /* Theme bg */
        draw_theme_bg();
        draw_clock();
        lg_box();

        ui_draw_window(20,7,40,14,"Welcome to HavenDOS");
        vga_puts_at("How would you like to start?",26,9,VGA_LIGHT_GREY,VGA_BLACK);

        /* Option 0 */
        {
            int s=(sel==0);
            vga_color_t sfg,sbg; ui_sel_colors(&sfg,&sbg);
            vga_color_t bg=s?sbg:VGA_BLACK;
            vga_color_t fg=s?sfg:VGA_DARK_GREY;
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,11,fg,bg);
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,12,fg,bg);
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,13,fg,bg);
            vga_puts_at(s?" > Create new account   ":" * Create new account   ",
                        23,11,s?sfg:VGA_LIGHT_GREY,bg);
            vga_puts_at("   Set your username and password",23,12,
                        s?sfg:VGA_DARK_GREY,bg);
            vga_puts_at("   Recommended for personal use",23,13,
                        s?sfg:VGA_DARK_GREY,bg);
        }

        for(int x=23;x<58;x++) vga_putchar_at(' ',x,15,VGA_DARK_GREY,VGA_BLACK);

        /* Option 1 */
        {
            int s=(sel==1);
            vga_color_t sfg,sbg; ui_sel_colors(&sfg,&sbg);
            vga_color_t bg=s?sbg:VGA_BLACK;
            vga_color_t fg=s?sfg:VGA_DARK_GREY;
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,16,fg,bg);
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,17,fg,bg);
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,18,fg,bg);
            vga_puts_at(s?" > Use existing account ":" * Use existing account ",
                        23,16,s?sfg:VGA_LIGHT_GREY,bg);
            vga_puts_at("   admin / bootos / guest",23,17,
                        s?sfg:VGA_DARK_GREY,bg);
            vga_puts_at("   Login with default accounts",23,18,
                        s?sfg:VGA_DARK_GREY,bg);
        }

        int c=keyboard_waitchar();
        if(c==0x148&&sel>0) sel--;
        if(c==0x150&&sel<1) sel++;
        if(c=='\n'||c=='\r'){
            if(sel==1) return;

            char nu[16]="",np[16]="",np2[16]="";
            const char *err=0;
            while(1){
                draw_theme_bg(); draw_clock(); lg_box();
                ui_draw_window(22,7,36,14,"Create Account");
                vga_puts_at("Username:",25,10,VGA_LIGHT_GREY,VGA_BLACK);
                vga_puts_at("Password:",25,12,VGA_LIGHT_GREY,VGA_BLACK);
                vga_puts_at("Confirm: ",25,14,VGA_LIGHT_GREY,VGA_BLACK);
                vga_puts_at("Admin?  Y=Yes  N=No",25,16,VGA_DARK_GREY,VGA_BLACK);
                for(int x=25;x<55;x++) vga_putchar_at(' ',x,11,VGA_DARK_GREY,VGA_BLACK);
                for(int x=25;x<55;x++) vga_putchar_at(' ',x,13,VGA_DARK_GREY,VGA_BLACK);
                for(int x=25;x<55;x++) vga_putchar_at(' ',x,15,VGA_DARK_GREY,VGA_BLACK);
                if(err) vga_puts_at(err,24,18,VGA_LIGHT_RED,VGA_BLACK);

                nu[0]=0; read_field_plain(nu,15,25,11,0);
                if(strlen(nu)<3){ err="Min 3 chars for username"; continue; }
                int dup=0;
                for(int i=0;i<user_count;i++)
                    if(strcmp(users[i].name,nu)==0){ dup=1; break; }
                if(dup){ err="Username already exists"; continue; }

                np[0]=0; read_field_plain(np,15,25,13,1);
                if(strlen(np)<3){ err="Min 3 chars for password"; continue; }

                np2[0]=0; read_field_plain(np2,15,25,15,1);
                if(strcmp(np,np2)!=0){ err="Passwords do not match"; continue; }

                vga_puts_at("Make admin? [Y/N]: ",24,17,VGA_WHITE,VGA_BLACK);
                int ac=keyboard_waitchar();
                int is_admin=(ac=='y'||ac=='Y');

                strncpy(users[user_count].name,nu,15);
                users[user_count].pw_hash=pw_hash(np);
                users[user_count].admin=is_admin;
                logged_in_idx=user_count;
                user_count++;
                accounts_save();
                vfs_set_user(users[logged_in_idx].name,users[logged_in_idx].admin);
                audit(users[logged_in_idx].name,"account created + login");
                setup_user_home(users[logged_in_idx].name);
                shell_set_home(users[logged_in_idx].name);
                login_done=1;

                draw_theme_bg(); draw_clock(); lg_box();
                ui_draw_window(27,10,26,5,"Account Created");
                vga_puts_at("Welcome,",30,12,VGA_LIGHT_GREY,VGA_BLACK);
                vga_puts_at(nu,39,12,ui_theme()->accent,VGA_BLACK);
                vga_puts_at("Loading desktop...",30,13,VGA_DARK_GREY,VGA_BLACK);
                sleep_ms(1000);
                return;
            }
        }
    }
}

/* ================================================================
   LOGIN WELCOME SEQUENCE
   Called after successful authentication.
   first_boot=1: "Welcome to HavenDOS" + hardware init lines.
   first_boot=0: "Welcome back, <user>" + short system check.
   ================================================================ */
static void welcome_line(int y, const char *msg, vga_color_t col){
    int W=vga_cols();
    for(int x=0;x<W;x++) vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
    int l=(int)strlen(msg);
    int sx=(W-l)/2; if(sx<2)sx=2;
    vga_puts_at(msg,sx,y,col,VGA_BLACK);
}

void login_welcome(const char *username, int first_boot){
    vga_viewport_clear();   /* v0.9.7.1: the whole screen, not a dialog's box */
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_clear();

    /* Line 1: big centred greeting */
    if(first_boot){
        welcome_line(4, "Welcome to HavenDOS", VGA_LIGHT_CYAN);
        welcome_line(5, HAVENDOS_VER "  TechHaven Studios", VGA_DARK_GREY);
    } else {
        char msg[64];
        strncpy(msg, "Welcome back, ", sizeof(msg)-1);
        msg[sizeof(msg)-1] = 0;
        strncat(msg, username, sizeof(msg)-1-strlen(msg));
        welcome_line(4, msg, VGA_LIGHT_CYAN);
        welcome_line(5, "HavenDOS " HAVENDOS_VER, VGA_DARK_GREY);
    }

    /* Home dir hint line */
    char home_hint[64];
    strncpy(home_hint, "Home: ~/  (HOME/", sizeof(home_hint)-1);
    strncat(home_hint, username, sizeof(home_hint)-1-strlen(home_hint));
    strncat(home_hint, "/)", sizeof(home_hint)-1-strlen(home_hint));
    welcome_line(6, home_hint, VGA_DARK_GREY);

    /* Divider */
    for(int x=(vga_cols()-40)/2;x<(vga_cols()+40)/2;x++) vga_putchar_at(' ',x,7,VGA_DARK_GREY,VGA_BLACK);
    sleep_ms(400);

    /* Hardware / init lines */
    typedef struct { const char *msg; vga_color_t col; int ms; } wl;

    extern uint32_t pmm_total_blocks(void);
    extern int      vfs_using_disk(void);
    extern int      netdev_count(void);

    char ram_s[32]; char rm[8];
    utoa(pmm_total_blocks()*4/1024,rm,10);
    strcpy(ram_s,rm); strcat(ram_s,"MB RAM OK");

    /* 0.9.5.1: these two lines said "VirtIO mounted" or "Disk: not found",
       and named only two network cards -- on an installed IDE, NVMe or
       USB system both were wrong. They now say what is known. */
    int on_disk = vfs_using_disk();
    const char *vio_s = on_disk ? "Disk: mounted" : "Disk: none -- a live session";
    int nics = netdev_count();
    const char *nic_s = nics>0 ? "Network: adapter ready" : "Network: no adapter";

    wl lines[]={
        {"Initialising system...",  VGA_LIGHT_GREY, 300},
        {ram_s,                     VGA_LIGHT_GREEN, 200},
        {vio_s,                     on_disk?VGA_LIGHT_GREEN:VGA_YELLOW, 200},
        {nic_s,                     nics>0?VGA_LIGHT_GREEN:VGA_YELLOW, 200},
        {"Loading desktop...",      VGA_LIGHT_GREY, 250},
        {"Almost done...",          VGA_CYAN,        350},
    };
    int nl=(int)(sizeof(lines)/sizeof(lines[0]));

    for(int i=0;i<nl;i++){
        welcome_line(9+i, lines[i].msg, lines[i].col);
        sleep_ms(lines[i].ms);
    }

    /* Brief pause then wipe */
    sleep_ms(300);
    vga_clear(); vga_flush();
}

/* ================================================================
   MAIN LOGIN SCREEN
   ================================================================ */
/* ── v0.9.6: THE LOGIN SCREEN AND OPEN APPS ─────────────────────────
   login_screen() is also the lock screen (the start menu's Lock) and
   Switch user, and it can run with apps open behind it. Two rules:

   1. Nothing comes forward while it is up. It reads the keyboard, so
      Alt+Tab reaches it; without the pin that walks straight past the
      lock into whatever was open.
   2. If a DIFFERENT user signs in, the previous user's apps are
      closed. Otherwise the next person is one Alt+Tab from somebody
      else's shell, signed in as them -- an administrator's, as often
      as not. The same user signing back in finds everything as it was.

   login_screen_inner() is the screen itself, unchanged. */
static void login_screen_inner(void);
void login_screen(void){
    um_set_mode(0);   /* v0.9.6.3: the login screen reads the mouse itself */
    char before[32];
    strncpy(before, current_username(), sizeof before - 1);
    before[sizeof before - 1] = 0;

    /* v0.9.7.1: the login screen is the whole screen. Whatever box the
       caller was drawing in (the Power app's, say) is put back after. */
    extern int  chrome_pause(void);
    extern void chrome_resume(int);
    int paused = chrome_pause();      /* no window taskbar over this screen */
    int vpa = vga_viewport_active(), vx=0, vy=0, vw=0, vh=0;
    if(vpa) vga_viewport_get(&vx, &vy, &vw, &vh);
    vga_viewport_clear();

    task_pin_front();
    login_screen_inner();
    task_unpin_front();

    vga_viewport_clear();
    if(vpa) vga_viewport_set(vx, vy, vw, vh);
    chrome_resume(paused);

    if(task_ready() && before[0] && strcmp(before, current_username())){
        int closed = 0;
        for(int s = 1; s < TASK_MAX; s++){
            if(s == task_current()) continue;
            if(task_state(s) != TS_READY && task_state(s) != TS_WAIT) continue;
            if(task_kill(s) == 0) closed++;
        }
        if(closed){
            char m[48]; int n = 0;
            const char *a = "Closed apps left open by ";
            while(*a && n < 30) m[n++] = *a++;
            for(int i = 0; before[i] && n < 47; i++) m[n++] = before[i];
            m[n] = 0;
            notif_push(m);
            klog(KLOG_INFO, "login: %d app(s) of %s closed for %s",
                 closed, before, current_username());
        }
    }
}

static void login_screen_inner(void){
    login_init();

    /* Auto-login check */
    extern int cfg_autologin(void);
    extern const char *cfg_autologin_user(void);
    extern int cfg_autologin_timeout(void);
    if(cfg_autologin()){
        int timeout=cfg_autologin_timeout();
        const char *au=cfg_autologin_user();
        /* find the user first */
        int aidx=-1;
        for(int i=0;i<user_count;i++)
            if(strcmp(users[i].name,au)==0){ aidx=i; break; }
        if(aidx>=0){
            if(timeout>0){
                /* Show brief countdown on a dark screen */
                vga_viewport_clear();
                vga_set_color(VGA_WHITE,VGA_BLACK); vga_clear();
                lg_box();
                vga_puts_at("HavenDOS " HAVENDOS_VER,32,10,VGA_DARK_GREY,VGA_BLACK);
                char umsg[48]="Auto-login as: ";
                strcat(umsg,au);
                vga_puts_at(umsg,32,12,VGA_LIGHT_GREY,VGA_BLACK);
                for(int t=timeout;t>0;t--){
                    char ts[24]="Logging in in   s...";
                    char ns[4]; utoa((uint32_t)t,ns,10);
                    ts[13]=ns[0]; ts[14]=(ns[1]?ns[1]:' ');
                    vga_puts_at(ts,32,13,VGA_DARK_GREY,VGA_BLACK);
                    /* poll keyboard — any key cancels auto-login */
                    extern int keyboard_haschar(void);
                    extern int keyboard_getchar(void);
                    int cancelled=0;
                    for(int ms=0;ms<1000;ms+=50){
                        extern void sleep_ms(uint32_t);
                        sleep_ms(50);
                        if(keyboard_haschar()){ keyboard_getchar(); cancelled=1; break; }
                    }
                    if(cancelled){ aidx=-1; break; }
                }
            }
            if(aidx>=0){
                logged_in_idx=aidx;
                vfs_set_user(users[aidx].name,users[aidx].admin);
                audit(users[aidx].name,"auto-login");
                setup_user_home(users[aidx].name);
                shell_set_home(users[aidx].name);
                login_welcome(users[aidx].name,0);
                return;
            }
        }
    }

    /* v0.9.6.1: "Welcome / Create new account" is for a machine nobody
       has made an account on. It showed on EVERY boot: login_done is
       RAM, zero at power-on, so the test was always true -- even after
       setup had made one. Now it asks whether any account besides the
       three built in (admin, bootos, guest) exists, which is what "first
       boot" meant. */
    if(!login_done && !has_own_account()){
        first_boot_creator();
        if(login_done) return;
    }
    login_done=0;

    /* Build visible user list respecting hidden account settings */
    build_vis_list();

    int user_sel=0, attempts=0;
    int pw_mode=0;
    char upass[16];

    draw_theme_bg(); draw_clock();
    draw_tiles(user_sel,-1);

    uint8_t last_m=0xFF;
    int     last_hover=-1;

    while(1){
        uint8_t ch,cm,cs2;
        rtc_get_time(&ch,&cm,&cs2);
        if(cm!=last_m){ last_m=cm; draw_clock(); }

        int hover=-1;
        if(!pw_mode && mouse_is_ready()){
            int mx,my,mb; mouse_get(&mx,&my,&mb);
            for(int i=0;i<vis_count&&i<6;i++){
                int ty=TILE_BASE - i*TILE_H - (i>0?i:0);
                if(ty<7) break;
                if(my>=(ty-TILE_H+1)&&my<=ty&&mx>=TILE_X&&mx<TILE_X+TILE_W){
                    hover=i;
                    if(mb&1){
                        user_sel=i; attempts=0; last_hover=-1;
                        draw_theme_bg(); draw_clock();
                        draw_tiles(user_sel,-1);
                        int ui=vis_users[user_sel];
                        if(users[ui].pw_hash==pw_hash("")){ goto do_guest_login; }
                        pw_mode=1;
                        draw_pw_panel(ui,0);
                        break;
                    }
                }
            }
            if(hover!=last_hover){ last_hover=hover; draw_tiles(user_sel,hover); }
        }

        int c=keyboard_getchar();
        if(c==-1){ sleep_ms(16); continue; }

        if(!pw_mode){
            if(c==0x148 && user_sel<vis_count-1){
                user_sel++; attempts=0;
                draw_theme_bg(); draw_clock(); draw_tiles(user_sel,-1);
            } else if(c==0x150 && user_sel>0){
                user_sel--; attempts=0;
                draw_theme_bg(); draw_clock(); draw_tiles(user_sel,-1);
            } else if(c=='\n'||c=='\r'){
                int ui=vis_users[user_sel];
                if(users[ui].pw_hash==pw_hash("")){
                    do_guest_login:;
                    int gi=vis_users[user_sel];
                    logged_in_idx=gi;
                    vfs_set_user(users[gi].name,users[gi].admin);
                    audit(users[gi].name,"login OK (no password)");
                    setup_user_home(users[gi].name);
                    shell_set_home(users[gi].name);
                    login_welcome(users[gi].name,0);
                    return;
                }
                pw_mode=1; attempts=0;
                draw_pw_panel(ui,0);
            }
        } else {
            int ui=vis_users[user_sel];
            draw_pw_panel(ui,attempts);
            upass[0]=0;
            read_pw(upass,15,pw_px(),pw_py(),c);

            if(!upass[0]){
                pw_mode=0; attempts=0;
                draw_theme_bg(); draw_clock();
                draw_tiles(user_sel,-1);
                last_hover=-1; last_m=0xFF;
                continue;
            }

            int idx=try_login(users[ui].name,upass);
            if(idx>=0){
                logged_in_idx=idx;
                vfs_set_user(users[idx].name,users[idx].admin);
                audit(users[idx].name,"login OK");
                setup_user_home(users[idx].name);
                shell_set_home(users[idx].name);
                login_welcome(users[idx].name,0);
                return;
            }

            audit(users[ui].name,"login FAILED");
            attempts++;
            if(attempts>=3){
                int W=vga_cols(), my=vga_rows()/2;
                for(int x=0;x<W;x++)
                    vga_putchar_at(' ',x,my,VGA_WHITE,VGA_RED);
                vga_puts_at("  Too many failed attempts. Rebooting in 3s...  ",
                            (W-48)/2,my,VGA_WHITE,VGA_RED);
                sleep_ms(3000);
                machine_reset();
            }
            draw_pw_panel(ui,attempts);
        }
    }
}
