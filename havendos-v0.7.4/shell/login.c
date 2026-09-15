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
#include "../include/io.h"
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
#define AUDIT_MAX 512
static char audit_buf[AUDIT_MAX];
static int  audit_len = 0;

static void audit(const char *user, const char *event){
    if(audit_len>=AUDIT_MAX-40) audit_len=0;
    const char *parts[]={"[",user,"] ",event,"\n"};
    for(int p=0;p<5;p++){
        const char *s=parts[p];
        while(*s&&audit_len<AUDIT_MAX-1) audit_buf[audit_len++]=*s++;
    }
    audit_buf[audit_len]=0;
    vfs_write(".loginlog",audit_buf,audit_len);
}

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

    /* Full background with per-theme pattern (rows 1-23) */
    for(int y=1;y<24;y++){
        for(int x=0;x<80;x++){
            char c=' ';
            vga_color_t fg=th->desk_bg;
            if(t==2||t==4){
                if((x*3+y*7)%29==0){ c='.'; fg=VGA_DARK_GREY; }
            } else if(t==3){
                if((x*5+y*3)%41==0){ c='+'; fg=VGA_BROWN; }
            } else if(t==6){
                if((x+y*2)%37==0){ c='|'; fg=VGA_DARK_GREY; }
            }
            vga_putchar_at(c,x,y,fg,th->desk_bg);
        }
    }

    /* Top strip: thin status bar */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,th->bar_fg,th->bar_bg);
    vga_puts_at(" HavenDOS v0.6.8",0,0,th->accent,th->bar_bg);
    vga_puts_at("TechHaven Studios",32,0,th->bar_fg,th->bar_bg);
    /* Theme name top-right */
    const char *tn = ui_theme_name(ui_current_theme());
    int tnlen = (int)strlen(tn);
    vga_puts_at(tn, 79-tnlen, 0, th->accent, th->bar_bg);

    /* Bottom strip: hint bar */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,th->bar_fg,th->bar_bg);
    vga_puts_at(" Enter=Select  Up/Down=Switch  ESC=Back",0,24,th->bar_fg,th->bar_bg);
}

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
    int tx = (80 - 9) / 2;   /* centre "HH : MM" with spaces */
    char wide[12];
    wide[0]=ts[0]; wide[1]=' '; wide[2]=ts[1]; wide[3]=' ';
    wide[4]=ts[2]; wide[5]=' ';
    wide[6]=ts[3]; wide[7]=' '; wide[8]=ts[4];
    wide[9]=0;
    vga_puts_at(wide, tx, 3, th->accent, th->desk_bg);

    /* Date below */
    int dx = (80-(int)strlen(ds))/2;
    vga_puts_at(ds, dx, 5, th->bar_fg, th->desk_bg);
}

/* ================================================================
   USER TILES — Win11 bottom-left style
   Each tile: 18 wide × 3 tall, stacked bottom-left
   Selected tile gets accent background + arrow indicator
   sel/mouse_hover index into vis_users[], not users[]
   ================================================================ */
#define TILE_X      2
#define TILE_W      20
#define TILE_H      3
#define TILE_BASE   23

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

static void draw_tiles(int sel, int mouse_hover){
    theme_t *th = ui_theme();

    for(int i=0;i<vis_count&&i<6;i++){
        int ui=vis_users[i]; /* real user index */
        int ty = TILE_BASE - i*TILE_H - (i>0?i:0);
        if(ty < 7) break;

        int is_sel   = (i==sel);
        int is_hover = (i==mouse_hover && !is_sel);

        vga_color_t bg = is_sel   ? th->accent  :
                         is_hover ? th->bar_bg   : th->desk_bg;
        vga_color_t fg = is_sel   ? th->bar_bg   :
                         is_hover ? th->accent   : th->bar_fg;

        for(int row=0;row<TILE_H;row++)
            for(int x=TILE_X;x<TILE_X+TILE_W;x++)
                vga_putchar_at(' ',x,ty-TILE_H+1+row,fg,bg);

        vga_putchar_at('[', TILE_X+1, ty-1, fg, bg);
        vga_putchar_at(users[ui].name[0], TILE_X+2, ty-1, fg, bg);
        vga_putchar_at(']', TILE_X+3, ty-1, fg, bg);
        vga_puts_at(users[ui].name, TILE_X+5, ty-1, fg, bg);

        if(users[ui].admin){
            int nlen=(int)strlen(users[ui].name);
            vga_puts_at("[A]", TILE_X+5+nlen+1, ty-1,
                        is_sel ? th->bar_bg : VGA_YELLOW, bg);
        }
        if(is_sel)
            vga_putchar_at('>', TILE_X+TILE_W-2, ty-1, th->bar_bg, bg);

        if(i<vis_count-1&&i<5){
            int sep_y=ty-TILE_H;
            if(sep_y>=7)
                for(int x=TILE_X;x<TILE_X+TILE_W;x++)
                    vga_putchar_at('-',x,sep_y,VGA_DARK_GREY,th->desk_bg);
        }
    }
}

/* ================================================================
   PASSWORD FIELD — centred on screen, appears after tile select
   ================================================================ */
static void draw_pw_panel(int user_idx, int attempts){
    theme_t *th = ui_theme();

    /* Panel: 32 wide, 7 tall, centred */
    int px=24, py=9, pw=32, ph=7;
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
        vga_putchar_at('-',x,py+3,VGA_DARK_GREY,th->win_bg);

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
static void read_pw(char *buf, int max, int px, int py){
    int pos=0; buf[0]=0;
    int fx=px+12, fy=py+4;
    vga_set_cursor(fx,fy);
    uint8_t last_s=0xFF;
    while(1){
        /* Keep the login clock ticking while waiting for keystrokes */
        uint8_t ch2,cm2,cs2; rtc_get_time(&ch2,&cm2,&cs2);
        if(cs2!=last_s){ last_s=cs2; draw_clock(); vga_set_cursor(fx+pos,fy); }

        int c=keyboard_getchar();
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

static void first_boot_creator(void){
    int sel=0;
    while(1){
        /* Theme bg */
        draw_theme_bg();
        draw_clock();

        ui_draw_window(20,7,40,14,"Welcome to HavenDOS");
        vga_puts_at("How would you like to start?",26,9,VGA_LIGHT_GREY,VGA_BLACK);

        theme_t *th=ui_theme();

        /* Option 0 */
        {
            int s=(sel==0);
            vga_color_t bg=s?th->accent:VGA_BLACK;
            vga_color_t fg=s?th->bar_bg:VGA_DARK_GREY;
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,11,fg,bg);
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,12,fg,bg);
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,13,fg,bg);
            vga_puts_at(s?" > Create new account   ":" * Create new account   ",
                        23,11,s?th->bar_bg:VGA_LIGHT_GREY,bg);
            vga_puts_at("   Set your username and password",23,12,
                        s?th->bar_bg:VGA_DARK_GREY,bg);
            vga_puts_at("   Recommended for personal use",23,13,
                        s?th->bar_bg:VGA_DARK_GREY,bg);
        }

        for(int x=23;x<58;x++) vga_putchar_at('-',x,15,VGA_DARK_GREY,VGA_BLACK);

        /* Option 1 */
        {
            int s=(sel==1);
            vga_color_t bg=s?th->accent:VGA_BLACK;
            vga_color_t fg=s?th->bar_bg:VGA_DARK_GREY;
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,16,fg,bg);
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,17,fg,bg);
            for(int x=23;x<58;x++) vga_putchar_at(' ',x,18,fg,bg);
            vga_puts_at(s?" > Use existing account ":" * Use existing account ",
                        23,16,s?th->bar_bg:VGA_LIGHT_GREY,bg);
            vga_puts_at("   admin / bootos / guest",23,17,
                        s?th->bar_bg:VGA_DARK_GREY,bg);
            vga_puts_at("   Login with default accounts",23,18,
                        s?th->bar_bg:VGA_DARK_GREY,bg);
        }

        int c=keyboard_waitchar();
        if(c==0x148&&sel>0) sel--;
        if(c==0x150&&sel<1) sel++;
        if(c=='\n'||c=='\r'){
            if(sel==1) return;

            char nu[16]="",np[16]="",np2[16]="";
            const char *err=0;
            while(1){
                draw_theme_bg(); draw_clock();
                ui_draw_window(22,7,36,14,"Create Account");
                vga_puts_at("Username:",25,10,VGA_LIGHT_GREY,VGA_BLACK);
                vga_puts_at("Password:",25,12,VGA_LIGHT_GREY,VGA_BLACK);
                vga_puts_at("Confirm: ",25,14,VGA_LIGHT_GREY,VGA_BLACK);
                vga_puts_at("Admin?  Y=Yes  N=No",25,16,VGA_DARK_GREY,VGA_BLACK);
                for(int x=25;x<55;x++) vga_putchar_at('-',x,11,VGA_DARK_GREY,VGA_BLACK);
                for(int x=25;x<55;x++) vga_putchar_at('-',x,13,VGA_DARK_GREY,VGA_BLACK);
                for(int x=25;x<55;x++) vga_putchar_at('-',x,15,VGA_DARK_GREY,VGA_BLACK);
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

                draw_theme_bg(); draw_clock();
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
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
    int l=(int)strlen(msg);
    int sx=(80-l)/2; if(sx<2)sx=2;
    vga_puts_at(msg,sx,y,col,VGA_BLACK);
}

void login_welcome(const char *username, int first_boot){
    for(int y=0;y<25;y++)
        for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    /* Line 1: big centred greeting */
    if(first_boot){
        welcome_line(4, "Welcome to HavenDOS", VGA_LIGHT_CYAN);
        welcome_line(5, "v0.7.0  TechHaven Studios", VGA_DARK_GREY);
    } else {
        char msg[64];
        strncpy(msg, "Welcome back, ", sizeof(msg)-1);
        msg[sizeof(msg)-1] = 0;
        strncat(msg, username, sizeof(msg)-1-strlen(msg));
        welcome_line(4, msg, VGA_LIGHT_CYAN);
        welcome_line(5, "HavenDOS v0.7.0", VGA_DARK_GREY);
    }

    /* Home dir hint line */
    char home_hint[64];
    strncpy(home_hint, "Home: ~/  (HOME/", sizeof(home_hint)-1);
    strncat(home_hint, username, sizeof(home_hint)-1-strlen(home_hint));
    strncat(home_hint, "/)", sizeof(home_hint)-1-strlen(home_hint));
    welcome_line(6, home_hint, VGA_DARK_GREY);

    /* Divider */
    for(int x=20;x<60;x++) vga_putchar_at('-',x,7,VGA_DARK_GREY,VGA_BLACK);
    sleep_ms(400);

    /* Hardware / init lines */
    typedef struct { const char *msg; vga_color_t col; int ms; } wl;

    extern uint32_t pmm_total_blocks(void);
    extern int      virtio_drive_count(void);
    extern uint64_t virtio_drive_sectors(int);
    extern int      rtl8139_found(void);
    extern int      e1000_found(void);

    char ram_s[32]; char rm[8];
    utoa(pmm_total_blocks()*4/1024,rm,10);
    strcpy(ram_s,rm); strcat(ram_s,"MB RAM OK");

    char vio_s[40];
    int vd=virtio_drive_count();
    if(vd>0){ char ms[12]; utoa((uint32_t)(virtio_drive_sectors(0)/2048),ms,10);
        strcpy(vio_s,"Disk: "); strcat(vio_s,ms); strcat(vio_s,"MB VirtIO mounted"); }
    else { strcpy(vio_s,"Disk: not found"); }

    char nic_s[40];
    if(rtl8139_found())    strcpy(nic_s,"Network: RTL8139 ready");
    else if(e1000_found()) strcpy(nic_s,"Network: e1000 ready");
    else                   strcpy(nic_s,"Network: not available");

    wl lines[]={
        {"Initialising system...",  VGA_LIGHT_GREY, 300},
        {ram_s,                     VGA_LIGHT_GREEN, 200},
        {vio_s,                     vd>0?VGA_LIGHT_GREEN:VGA_YELLOW, 200},
        {nic_s,                     (rtl8139_found()||e1000_found())?VGA_LIGHT_GREEN:VGA_YELLOW, 200},
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
    for(int y=0;y<25;y++){
        for(int x=0;x<80;x++) vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
        sleep_ms(6);
    }
}

/* ================================================================
   MAIN LOGIN SCREEN
   ================================================================ */
void login_screen(void){
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
                for(int y=0;y<25;y++) for(int x=0;x<80;x++)
                    vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
                vga_puts_at("HavenDOS v0.6.8",32,10,VGA_DARK_GREY,VGA_BLACK);
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

    if(!login_done){
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
            (void)c;
            int ui=vis_users[user_sel];
            draw_pw_panel(ui,attempts);
            upass[0]=0;
            read_pw(upass,15,24,9);

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
                for(int x=0;x<80;x++)
                    vga_putchar_at(' ',x,12,VGA_WHITE,VGA_RED);
                vga_puts_at("  Too many failed attempts. Rebooting in 3s...  ",
                            16,12,VGA_WHITE,VGA_RED);
                sleep_ms(3000);
                outb(0x64,0xFE);
                for(;;) __asm__ volatile("cli;hlt");
            }
            draw_pw_panel(ui,attempts);
        }
    }
}
