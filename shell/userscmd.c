/* ================================================================
   shell/userscmd.c  --  `users` and `log`.  v0.9.5.

   `users` printed a name and the word admin or user -- and on 0.9.4
   not even that, because vga_printf did not understand %-16s and the
   photo Kyan sent read "16s admin" on every line. Even fixed, it told
   you nothing you could act on.

   Now it answers the questions you have when you look at accounts:
   which one am I, which can log in without a password, which still
   have the default password printed in shell/login.c, where is each
   home folder, when was each last used, and which are hidden from the
   login screen. `users <name>` shows one account and its recent
   logins. `log` pages the login log, which now has times on it.
   ================================================================ */
#include "types.h"
#include "vga.h"
#include "string.h"

extern int  keyboard_waitchar(void);
extern int         get_user_count(void);
extern const char *get_username(int);
extern int         get_user_admin(int);
extern int         get_user_current(void);
extern int         get_user_pwstate(int);
extern int         get_user_hidden(int);
extern int         current_user_admin(void);
extern const char *current_username(void);
extern int         user_last_login(const char *name, char *out, int max, int *count);
extern const char *audit_text(void);
extern int         vfs_is_dir(const char *name);
extern int         vfs_exists(const char *name);
static int home_exists(const char *h){ return vfs_is_dir(h) || vfs_exists(h); }
extern int         cfg_autologin(void);
extern const char *cfg_autologin_user(void);

static void col(vga_color_t fg){ vga_set_color(fg, VGA_BLACK); }
static void pad(int n){ while(n-- > 0) vga_putchar(' '); }
static void putw(const char *s, int w){ int n=(int)strlen(s); vga_puts(s); pad(w-n); }

static int find_user(const char *name){
    for(int i=0;i<get_user_count();i++)
        if(!strcmp(get_username(i),name)) return i;
    return -1;
}

static void home_of(int i, char *out){
    const char *n=get_username(i);
    int k=0; const char *h="HOME/";
    while(*h) out[k++]=*h++;
    while(*n && k<40) out[k++]=*n++;
    out[k]=0;
}

static void show_one(int i){
    const char *nm=get_username(i);
    char home[48]; home_of(i,home);
    char when[24]; int logins=0;
    int seen=user_last_login(nm,when,sizeof when,&logins);
    int ps=get_user_pwstate(i);

    col(VGA_LIGHT_CYAN); vga_puts("  "); vga_puts(nm);
    if(i==get_user_current()){ col(VGA_DARK_GREY); vga_puts("   (you)"); }
    vga_putchar('\n');
    col(VGA_DARK_GREY); vga_puts("  Role        "); col(VGA_WHITE);
    vga_puts(get_user_admin(i)?"administrator":"standard user"); vga_putchar('\n');
    col(VGA_DARK_GREY); vga_puts("  Password    ");
    if(ps==0){ col(VGA_YELLOW); vga_puts("none -- anyone can log in as this account\n"); }
    else if(ps==2){ col(VGA_YELLOW); vga_puts("THE DEFAULT, which is published -- change it: passwd "); vga_puts(nm); vga_putchar('\n'); }
    else { col(VGA_WHITE); vga_puts("set\n"); }
    col(VGA_DARK_GREY); vga_puts("  Home        "); col(VGA_WHITE);
    vga_puts(home); if(!home_exists(home)){ col(VGA_DARK_GREY); vga_puts("  (not created yet)"); }
    vga_putchar('\n');
    col(VGA_DARK_GREY); vga_puts("  Login list  "); col(VGA_WHITE);
    vga_puts(get_user_hidden(i)?"hidden (built-in account; Settings > Accounts shows it)\n":"shown\n");
    if(cfg_autologin() && !strcmp(cfg_autologin_user(),nm)){
        col(VGA_DARK_GREY); vga_puts("  Auto-login  "); col(VGA_WHITE); vga_puts("yes, at boot\n");
    }
    col(VGA_DARK_GREY); vga_puts("  Last login  "); col(VGA_WHITE);
    if(seen) vga_printf("%s   (%d in the log)\n", when, logins);
    else vga_puts("none on record\n");

    /* Its lines from the log, newest last, at most eight. */
    const char *log=audit_text();
    int nl=(int)strlen(nm), total=0;
    for(const char *p=log; *p; ){
        const char *e=p; while(*e && *e!='\n') e++;
        if((e-p)>18+nl && !strncmp(p+18,nm,(size_t)nl) && p[18+nl]==' ') total++;
        p = *e ? e+1 : e;
    }
    if(total){
        int skip = total>8 ? total-8 : 0, k=0;
        vga_putchar('\n'); col(VGA_DARK_GREY); vga_puts("  Recent activity\n");
        for(const char *p=log; *p; ){
            const char *e=p; while(*e && *e!='\n') e++;
            if((e-p)>18+nl && !strncmp(p+18,nm,(size_t)nl) && p[18+nl]==' '){
                if(k++ >= skip){
                    col(VGA_LIGHT_GREY); vga_puts("    ");
                    for(const char *c=p; c<e; c++) vga_putchar(*c);
                    vga_putchar('\n');
                }
            }
            p = *e ? e+1 : e;
        }
    }
    col(VGA_WHITE);
}

void cmd_users(const char *arg){
    if(arg && arg[0]){
        int i=find_user(arg);
        if(i<0){ col(VGA_LIGHT_RED); vga_printf("users: no account called '%s'\n",arg); col(VGA_WHITE); return; }
        show_one(i);
        return;
    }
    int wide = vga_cols() >= 80;
    col(VGA_LIGHT_CYAN);
    vga_puts("  "); putw("User",16); putw("Role",8); putw("Password",11);
    if(wide) putw("Home",18);
    vga_puts("Last login\n");
    col(VGA_DARK_GREY);
    vga_puts("  "); putw("----",16); putw("----",8); putw("--------",11);
    if(wide) putw("----",18);
    vga_puts("----------\n");

    int weak=0;
    for(int i=0;i<get_user_count();i++){
        const char *nm=get_username(i);
        int me = (i==get_user_current());
        col(me?VGA_LIGHT_GREEN:VGA_DARK_GREY); vga_puts(me?"* ":"  ");
        col(me?VGA_WHITE:VGA_LIGHT_GREY); putw(nm,16);
        col(get_user_admin(i)?VGA_LIGHT_CYAN:VGA_LIGHT_GREY); putw(get_user_admin(i)?"admin":"user",8);
        int ps=get_user_pwstate(i);
        if(ps==1){ col(VGA_LIGHT_GREY); putw("set",11); }
        else { col(VGA_YELLOW); putw(ps==2?"DEFAULT":"none",11); if(ps==2) weak++; }
        if(wide){
            char home[48]; home_of(i,home);
            if(home_exists(home)){ col(VGA_LIGHT_GREY); putw(home,18); }
            else { col(VGA_DARK_GREY); putw("-",18); }
        }
        char when[24]; int n=0;
        if(user_last_login(nm,when,sizeof when,&n)){ col(VGA_LIGHT_GREY); vga_puts(when); }
        else { col(VGA_DARK_GREY); vga_puts("never"); }
        if(get_user_hidden(i)){ col(VGA_DARK_GREY); vga_puts("  hidden"); }
        vga_putchar('\n');
    }
    vga_putchar('\n');
    col(VGA_DARK_GREY);
    vga_printf("  %d account%s. * is you (%s, %s).\n", get_user_count(),
               get_user_count()==1?"":"s", current_username(),
               current_user_admin()?"administrator":"standard user");
    if(weak){
        col(VGA_YELLOW);
        vga_puts("  DEFAULT means the password printed in HavenDOS's own source --\n"
                 "  anyone can use it. passwd <user> changes it; deluser removes\n"
                 "  the account.\n");
    }
    col(VGA_DARK_GREY);
    vga_puts("  users <name> shows one account; Settings > Accounts manages them.\n");
    col(VGA_WHITE);
}

void cmd_loginlog(void){
    if(!current_user_admin()){ vga_puts("Admin only.\n"); return; }
    const char *log=audit_text();
    if(!log[0]){ vga_puts("The login log is empty.\n"); return; }
    col(VGA_LIGHT_CYAN); vga_puts("  Login log, oldest first\n"); col(VGA_LIGHT_GREY);
    int lines=1;
    for(const char *p=log; *p; ){
        const char *e=p; while(*e && *e!='\n') e++;
        vga_puts("  ");
        for(const char *c=p; c<e; c++) vga_putchar(*c);
        vga_putchar('\n');
        if(++lines >= vga_rows()-2 && *e && e[1]){
            vga_set_color(VGA_BLACK, VGA_LIGHT_GREY);
            vga_puts(" -- more -- any key, Q stops ");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            int k=keyboard_waitchar();
            vga_puts("\r                              \r");
            if(k=='q'||k=='Q'||k==27) break;
            lines=0;
        }
        p = *e ? e+1 : e;
    }
    col(VGA_WHITE);
}
