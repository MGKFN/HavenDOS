/*
 * setup.c — HavenDOS v0.6.8 first-boot setup wizard
 *
 * Screens:
 *  1. Welcome
 *  2. Create account
 *  3. Choose theme
 *  4. Add-ons (all coming soon)
 *  5. Summary → Start
 */
#include "../include/vga.h"
#include "version.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/config.h"
#include "../include/legal.h"
/* v0.8.5.6: the theme functions were hand-declared as externs below
   rather than included, which is how this file came to call
   ui_set_theme() for a preview -- the header's comment says plainly
   which one is in-memory and that comment was not in view. The real
   declarations, and theme_t, come from here now. */
#include "../include/theme.h"

/* v0.7.5: cfg_* wrappers were called without declarations, so GCC assumed
   int(...) and the real prototypes were never checked. */
extern void cfg_set_setup_done(int v);
extern void cfg_set_acct_hidden(int idx, int hidden);
extern void cfg_set_autologin(int v);
extern void cfg_set_autologin_user(const char *u);
extern void cfg_save(void);

extern int         keyboard_getchar(void);
extern int         keyboard_waitchar(void);
extern void        sleep_ms(uint32_t);
extern void        vga_set_cursor(int,int);
extern int         add_user(const char*, const char*, int);
extern int         ui_theme_count(void);
extern const char *ui_theme_name(int);
extern void        ui_set_theme(int);
extern void        ui_draw_desktop(void);
extern int         bpkg_install(const char *id);
extern int         bpkg_registry_count(void);
extern const char *bpkg_pkg_id(int i);
extern const char *bpkg_pkg_name(int i);
extern const char *bpkg_pkg_desc(int i);

/* ── helpers ─────────────────────────────────────────────────── */
static void cls(void){
    for(int y=0;y<25;y++)
        for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
}
static void hdr(const char *title, int step){
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_CYAN);
    vga_puts_at(" HavenDOS Setup",0,0,VGA_BLACK,VGA_CYAN);
    vga_puts_at(title,28,0,VGA_BLACK,VGA_CYAN);
    /* Step indicator.  The terms and privacy screens are steps 1 and 2 and
       are drawn by legal.c, so this wizard's own screens start at 3 and the
       run is 7 screens long - counting to 6 made the last screen "7 of 6". */
    int shown = step + 3;
    char sb[16]; sb[0]='S';sb[1]='t';sb[2]='e';sb[3]='p';sb[4]=' ';
    sb[5]='0'+shown; sb[6]=' ';sb[7]='o';sb[8]='f';sb[9]=' ';sb[10]='7';sb[11]=0;
    vga_puts_at(sb,68,0,VGA_DARK_GREY,VGA_CYAN);
    for(int x=0;x<80;x++) vga_putchar_at('-',x,1,VGA_DARK_GREY,VGA_BLACK);
    /* Step progress bar */
    int filled=shown*76/7;
    for(int x=0;x<76;x++)
        vga_putchar_at(x<filled?'=':'.',x+2,1,
                       x<filled?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);
}
static void read_str_setup(char *buf, int max, int x, int y){
    int pos=0; buf[0]=0;
    vga_set_cursor(x,y);
    for(int i=0;i<max;i++) vga_putchar_at('_',x+i,y,VGA_DARK_GREY,VGA_BLACK);
    vga_set_cursor(x,y);
    while(1){
        int c=keyboard_waitchar();
        if(c=='\n'||c=='\r') break;
        if(c==27) break;
        if((c=='\b'||c==127)&&pos>0){
            pos--; buf[pos]=0;
            vga_putchar_at('_',x+pos,y,VGA_DARK_GREY,VGA_BLACK);
            vga_set_cursor(x+pos,y);
        } else if(c>=32&&c<127&&pos<max-1){
            buf[pos++]=(char)c; buf[pos]=0;
            vga_putchar_at((char)c,x+pos-1,y,VGA_WHITE,VGA_BLACK);
            vga_set_cursor(x+pos,y);
        }
    }
}
static void read_pw_setup(char *buf, int max, int x, int y){
    int pos=0; buf[0]=0;
    for(int i=0;i<max;i++) vga_putchar_at('_',x+i,y,VGA_DARK_GREY,VGA_BLACK);
    vga_set_cursor(x,y);
    while(1){
        int c=keyboard_waitchar();
        if(c=='\n'||c=='\r') break;
        if(c==27) break;
        if((c=='\b'||c==127)&&pos>0){
            pos--; buf[pos]=0;
            vga_putchar_at('_',x+pos,y,VGA_DARK_GREY,VGA_BLACK);
            vga_set_cursor(x+pos,y);
        } else if(c>=32&&c<127&&pos<max-1){
            buf[pos++]=(char)c; buf[pos]=0;
            vga_putchar_at('*',x+pos-1,y,VGA_LIGHT_CYAN,VGA_BLACK);
            vga_set_cursor(x+pos,y);
        }
    }
}

/* ── Screen 1: Welcome ───────────────────────────────────────── */
static void screen_welcome(void){
    cls(); hdr("Welcome",0);

    vga_puts_at("Welcome to HavenDOS " HAVENDOS_VER,20,5,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at("TechHaven Studios",30,6,VGA_DARK_GREY,VGA_BLACK);

    for(int x=10;x<70;x++) vga_putchar_at('-',x,8,VGA_DARK_GREY,VGA_BLACK);

    vga_puts_at("This setup wizard will help you configure your",14,10,VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts_at("HavenDOS installation. It only runs once.",17,11,VGA_LIGHT_GREY,VGA_BLACK);

    vga_puts_at("What we will set up:",20,14,VGA_WHITE,VGA_BLACK);
    vga_puts_at(". Your user account",22,15,VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts_at(". Desktop theme",22,16,VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts_at(". System add-ons",22,17,VGA_LIGHT_GREY,VGA_BLACK);

    for(int x=10;x<70;x++) vga_putchar_at('-',x,20,VGA_DARK_GREY,VGA_BLACK);

    /* Language note */
    vga_puts_at("Language: English (other languages coming soon)",14,22,VGA_DARK_GREY,VGA_BLACK);

    vga_puts_at("  Press any key to begin...  ",24,24,VGA_BLACK,VGA_LIGHT_GREEN);
    keyboard_waitchar();
}

/* ── Screen 2: Create Account ─────────────────────────────────── */
static int screen_account(char *out_name, char *out_pass, int *out_admin){
    char uname[16]=""; char upass[16]=""; char upass2[16]=""; int is_admin=1;

    while(1){
        cls(); hdr("Create Account",1);

        vga_puts_at("Create your user account",26,4,VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts_at("This will be your main account on HavenDOS.",17,5,VGA_DARK_GREY,VGA_BLACK);
        for(int x=10;x<70;x++) vga_putchar_at('-',x,7,VGA_DARK_GREY,VGA_BLACK);

        vga_puts_at("Username  (3-15 characters):",14,9,VGA_WHITE,VGA_BLACK);
        vga_puts_at("Password  (leave blank = no password):",14,12,VGA_WHITE,VGA_BLACK);
        vga_puts_at("Confirm password:",14,15,VGA_WHITE,VGA_BLACK);
        vga_puts_at("Account type:",14,18,VGA_WHITE,VGA_BLACK);
        vga_puts_at(is_admin?"Administrator":"Standard user",28,18,
                    is_admin?VGA_YELLOW:VGA_LIGHT_GREEN,VGA_BLACK);
        vga_puts_at("(Tab to toggle)",44,18,VGA_DARK_GREY,VGA_BLACK);

        /* Show current values */
        if(uname[0]) vga_puts_at(uname,43,9,VGA_LIGHT_CYAN,VGA_BLACK);

        vga_puts_at("  Enter username and press Enter at each field  ",14,22,VGA_DARK_GREY,VGA_BLACK);

        /* Read username */
        read_str_setup(uname,16,43,9);
        if(strlen(uname)<3){
            vga_puts_at("Username must be 3+ characters. Try again.",14,21,VGA_LIGHT_RED,VGA_BLACK);
            sleep_ms(1200); uname[0]=0; continue;
        }

        /* Read password */
        read_pw_setup(upass,16,43,12);

        /* Confirm password */
        if(upass[0]){
            read_pw_setup(upass2,16,43,15);
            if(strcmp(upass,upass2)!=0){
                vga_puts_at("Passwords do not match. Try again.",14,21,VGA_LIGHT_RED,VGA_BLACK);
                sleep_ms(1200); upass[0]=0; upass2[0]=0; continue;
            }
        }

        /* Admin toggle */
        vga_puts_at("Admin? Tab=toggle  Enter=confirm: ",14,20,VGA_WHITE,VGA_BLACK);
        while(1){
            int k=keyboard_waitchar();
            if(k=='\t'||k==' '){
                is_admin=!is_admin;
                vga_puts_at(is_admin?"Administrator":"Standard user",28,18,
                            is_admin?VGA_YELLOW:VGA_LIGHT_GREEN,VGA_BLACK);
                vga_puts_at("                ",44,18,VGA_BLACK,VGA_BLACK);
                vga_puts_at("(Tab to toggle)",44,18,VGA_DARK_GREY,VGA_BLACK);
            }
            else if(k=='\n'||k=='\r') break;
        }

        strncpy(out_name,uname,15); out_name[15]=0;
        strncpy(out_pass,upass,15); out_pass[15]=0;
        *out_admin=is_admin;
        return 1;
    }
}

/* ── Screen 3: Theme ──────────────────────────────────────────── */
static int screen_theme(void){
    int sel=0; int tc=ui_theme_count();
    while(1){
        cls(); hdr("Choose Theme",2);
        vga_puts_at("Choose your desktop theme",26,4,VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts_at("You can change this later in Settings.",20,5,VGA_DARK_GREY,VGA_BLACK);
        for(int x=10;x<70;x++) vga_putchar_at('-',x,7,VGA_DARK_GREY,VGA_BLACK);

        /* Theme list with live preview swatch */
        for(int i=0;i<tc;i++){
            int s=(i==sel);
            vga_color_t fg=s?VGA_BLACK:VGA_LIGHT_GREY;
            vga_color_t bg=s?VGA_LIGHT_GREY:VGA_BLACK;
            vga_putchar_at(s?'>':' ',16,9+i,fg,bg);
            char row[20]; int j=0;
            const char *nm=ui_theme_name(i);
            while(*nm&&j<18) row[j++]=*nm++;
            while(j<18) row[j++]=' ';
            row[18]=0;
            vga_puts_at(row,17,9+i,fg,bg);
        }

        /* ── The desktop sample ────────────────────────────────────
           v0.8.5.6: this panel used to be drawn with VGA_BLUE,
           VGA_CYAN, VGA_DARK_GREY and VGA_LIGHT_CYAN written out as
           literals, so it looked EXACTLY THE SAME for all ten themes.
           A preview that cannot change is worse than no preview: it
           tells you the choice does not matter.

           It also called ui_set_theme(sel) to "preview", and that
           writes THEME_FILE -- so arrowing down the list performed one
           disk write per keystroke during first-time setup.
           ui_load_preview() is the in-memory one, which is what this
           wants and what it is for.

           Every colour below now comes from the theme being previewed,
           and a sample window is drawn as well so win_bg, win_fg and
           the title colours are visible too -- choosing a theme by its
           taskbar alone is how "Retro" got a red title bar on a brown
           bar and nobody noticed until it was applied. */
        ui_load_preview(sel);
        theme_t *pv = ui_theme();

        vga_puts_at("Preview:",42,9,VGA_LIGHT_GREY,VGA_BLACK);

        /* Top status bar */
        for(int x=42;x<70;x++) vga_putchar_at(' ',x,10,pv->bar_fg,pv->bar_bg);
        vga_puts_at("HavenDOS",43,10,pv->bar_fg,pv->bar_bg);
        vga_puts_at("12:00",64,10,pv->accent,pv->bar_bg);

        /* Desktop, with the icon-dot pattern the real one uses */
        for(int y=11;y<18;y++)
            for(int x=42;x<70;x++)
                vga_putchar_at(((x+y)%8==0)?'.':' ',x,y,
                               pv->icon_fg,pv->desk_bg);

        /* A sample window: title row then body */
        for(int x=46;x<65;x++) vga_putchar_at(' ',x,13,pv->title_fg,pv->title_bg);
        vga_puts_at("Files",47,13,pv->title_fg,pv->title_bg);
        for(int y=14;y<17;y++)
            for(int x=46;x<65;x++)
                vga_putchar_at(' ',x,y,pv->win_fg,pv->win_bg);
        vga_puts_at("readme.txt",47,14,pv->win_fg,pv->win_bg);
        vga_puts_at("boot",47,15,pv->accent,pv->win_bg);

        /* Taskbar */
        for(int x=42;x<70;x++) vga_putchar_at(' ',x,18,pv->bar_fg,pv->bar_bg);
        vga_puts_at(" HAVENDOS",43,18,pv->accent,pv->bar_bg);
        vga_puts_at("user",64,18,pv->bar_fg,pv->bar_bg);

        vga_puts_at("Up-Down=Select  Enter=Confirm",22,22,VGA_DARK_GREY,VGA_BLACK);

        int c=keyboard_waitchar();
        if(c==0x148&&sel>0) sel--;
        if(c==0x150&&sel<tc-1) sel++;
        if(c=='\n'||c=='\r'){ ui_set_theme(sel);  return sel; }
    }
}

/* ── Screen 4: Package picker ─────────────────────────────────── */
static void screen_addons(void){
    int n = bpkg_registry_count();
    if(n > 10) n = 10;

    /* Selection state: 0=unselected 1=selected */
    int sel[10]; for(int i=0;i<n;i++) sel[i]=0;
    /* Select fortune, ascii-art, paint, music by default */
    for(int i=0;i<n&&i<4;i++) sel[i]=1;

    int cursor=0;

    while(1){
        cls(); hdr("Packages",3);

        vga_puts_at("Choose packages to install",26,3,VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts_at("All selected packages will be installed when setup finishes.",10,4,VGA_DARK_GREY,VGA_BLACK);
        for(int x=4;x<76;x++) vga_putchar_at('-',x,5,VGA_DARK_GREY,VGA_BLACK);

        /* Column headers */
        vga_puts_at(" ",5,6,VGA_DARK_GREY,VGA_BLACK);
        vga_puts_at("Package",8,6,VGA_DARK_GREY,VGA_BLACK);
        vga_puts_at("Description",30,6,VGA_DARK_GREY,VGA_BLACK);

        for(int i=0;i<n;i++){
            int is_cur=(i==cursor);
            vga_color_t bg=is_cur?VGA_DARK_GREY:VGA_BLACK;
            vga_color_t fg=is_cur?VGA_WHITE:VGA_LIGHT_GREY;

            /* Checkbox */
            vga_putchar_at('[',5,7+i,fg,bg);
            vga_putchar_at(sel[i]?'X':' ',6,7+i,sel[i]?VGA_LIGHT_GREEN:VGA_DARK_GREY,bg);
            vga_putchar_at(']',7,7+i,fg,bg);

            /* Name — pad to fixed width */
            const char *nm=bpkg_pkg_name(i);
            int nx=8;
            for(int k=0;nm[k]&&nx<28;k++,nx++)
                vga_putchar_at(nm[k],nx,7+i,fg,bg);
            while(nx<28){ vga_putchar_at(' ',nx,7+i,fg,bg); nx++; }

            /* Desc */
            const char *ds=bpkg_pkg_desc(i);
            int dx=29;
            for(int k=0;ds[k]&&dx<75;k++,dx++)
                vga_putchar_at(ds[k],dx,7+i,VGA_DARK_GREY,bg);
            while(dx<75){ vga_putchar_at(' ',dx,7+i,VGA_DARK_GREY,bg); dx++; }
        }

        /* Count selected */
        int nsel=0; for(int i=0;i<n;i++) nsel+=sel[i];
        char sc[24]="Selected: 0 / 00";
        sc[10]='0'+(nsel);
        sc[14]='0'+(n/10); sc[15]='0'+(n%10);
        vga_puts_at(sc,4,7+n+1,VGA_LIGHT_GREEN,VGA_BLACK);

        vga_puts_at("Up/Dn=Move  Space=Toggle  A=All  N=None  Enter=Install",13,23,VGA_DARK_GREY,VGA_BLACK);

        int c=keyboard_waitchar();
        if(c==0x148&&cursor>0)   cursor--;
        else if(c==0x150&&cursor<n-1) cursor++;
        else if(c==' ')         sel[cursor]=!sel[cursor];
        else if(c=='a'||c=='A'){ for(int i=0;i<n;i++) sel[i]=1; }
        else if(c=='n'||c=='N'){ for(int i=0;i<n;i++) sel[i]=0; }
        else if(c=='\n'||c=='\r') {
            /* Install selected packages */
            for(int i=0;i<n;i++){
                if(!sel[i]) continue;
                cls(); hdr("Installing",3);
                vga_puts_at("Installing packages...",29,8,VGA_LIGHT_CYAN,VGA_BLACK);
                vga_puts_at(bpkg_pkg_name(i),32,10,VGA_WHITE,VGA_BLACK);
                bpkg_install(bpkg_pkg_id(i));
                sleep_ms(150);
            }
            break;
        }
    }
}

/* ── Screen 5: Summary ────────────────────────────────────────── */
static void screen_summary(const char *uname, int is_admin, int theme_sel){
    /* 4, not 5: the screens are 0..4 here, 3..7 shown. 5 drew "Step 8 of
       7" on the last screen of every first boot (v0.9.4.1). */
    cls(); hdr("Ready",4);

    vga_puts_at("Setup complete. Here is your configuration:",17,4,VGA_LIGHT_CYAN,VGA_BLACK);
    for(int x=10;x<70;x++) vga_putchar_at('-',x,6,VGA_DARK_GREY,VGA_BLACK);

    vga_puts_at("Username:   ",16,8, VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts_at(uname,        28,8, VGA_WHITE,VGA_BLACK);

    vga_puts_at("Role:       ",16,9, VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts_at(is_admin?"Administrator":"Standard user",28,9,
                is_admin?VGA_YELLOW:VGA_LIGHT_GREEN,VGA_BLACK);

    vga_puts_at("Theme:      ",16,10,VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts_at(ui_theme_name(theme_sel),28,10,VGA_WHITE,VGA_BLACK);

    vga_puts_at("Auto-login: ",16,11,VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts_at("Disabled (enable in Settings)",28,11,VGA_DARK_GREY,VGA_BLACK);

    /* The label is 38 characters from column 16, which leaves column 55
       for the value -- and the value is 27 characters, so it ran to
       column 82 and the last two were clamped off the edge: "...in
       Setting". The row above gets this right at column 28 only because
       its label is shorter. Shortened to fit rather than moved, so the
       two values stay in line with each other.
       tools/check_screen_width.py now fails the build on this. */
    vga_puts_at("Default accounts (admin-bootos-guest):",16,12,VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts_at("Hidden (see Settings)",55,12,VGA_DARK_GREY,VGA_BLACK);

    for(int x=10;x<70;x++) vga_putchar_at('-',x,14,VGA_DARK_GREY,VGA_BLACK);

    vga_puts_at("You can change all of these in the Settings app.",15,16,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Your login password is required at each boot.",16,17,VGA_DARK_GREY,VGA_BLACK);

    vga_puts_at("  Press any key to start HavenDOS  ",21,24,VGA_BLACK,VGA_LIGHT_GREEN);
    keyboard_waitchar();
}

/* ── Main entry ──────────────────────────────────────────────── */
void run_setup_wizard(void){
    /* v0.7.11: legal first.  These used to appear mid-install, before the user
       had chosen anything and while a target drive was already selected -
       declining there meant abandoning a half-configured install.  Here the
       person actually using the machine reads them, and declining simply
       loops back to the welcome screen. */
    while(!legal_show_tos() || !legal_show_privacy()){
        cls();
        vga_puts_at("You must accept the Terms of Service and Privacy Policy",
                    12,11,VGA_YELLOW,VGA_BLACK);
        vga_puts_at("to use HavenDOS.  Press any key to review them again.",
                    13,13,VGA_LIGHT_GREY,VGA_BLACK);
        keyboard_waitchar();
    }
    screen_welcome();

    char uname[16]=""; char upass[16]=""; int is_admin=1;
    screen_account(uname,upass,&is_admin);

    /* Create the account (index 3 onwards — after built-in hidden ones) */
    add_user(uname, upass, is_admin);

    int theme_sel=screen_theme();

    screen_addons();
    screen_summary(uname,is_admin,theme_sel);

    /* Mark setup done, hide default accounts, save config */
    cfg_set_setup_done(1);
    cfg_set_acct_hidden(0,1); /* admin hidden */
    cfg_set_acct_hidden(1,1); /* bootos hidden */
    cfg_set_acct_hidden(2,1); /* guest hidden */
    cfg_set_autologin(0);
    cfg_set_autologin_user(uname);
    cfg_save();

    /* Brief wipe */
    for(int y=0;y<25;y++){
        for(int x=0;x<80;x++) vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
        sleep_ms(4);
    }
}
