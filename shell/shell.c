#include "../include/io.h"
#include "../include/uimouse.h"
#include "version.h"
#include "../include/vga.h"
#include "../include/task.h"
#include "../include/machine.h"
#include "../include/nvme.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/rtc.h"
#include "../include/toast.h"
#include "../include/theme.h"
#include "../include/http.h"   /* v0.7.6: ONE http_resp_t, shared with net/http.c */

extern const char *current_username(void);
extern int current_user_admin(void);
extern int get_user_count(void);
extern const char *get_username(int);
extern int get_user_admin(int);
extern int add_user(const char*, const char*, int);
extern int delete_user(const char*);
extern int passwd_user(const char*, const char*, const char*);
extern int keyboard_getchar(void);
extern int chrome_take_close(void);   /* shell/chrome.c, v0.9.6.6 */
extern int keyboard_waitchar(void);
extern void screensaver_run_matrix(void);
extern void screensaver_run_stars(void);
extern void screensaver_run_pipes(void);
extern void speaker_beep(uint32_t, uint32_t);
extern void music_boot_jingle(void);
extern int vfs_read(const char*,char*,uint32_t);
extern int vfs_write(const char*,const char*,uint32_t);
extern int vfs_exists(const char*);
extern void vfs_list(void(*)(const char*,int,uint32_t));
extern void vfs_list_dir(const char*,void(*)(const char*,int,uint32_t));
extern int vfs_is_dir(const char*);
extern int vfs_delete(const char*);
extern int vfs_mkdir(const char*);
extern int vfs_rename(const char*,const char*);
extern int vfs_using_disk(void);
extern int vfs_using_fat16(void);
extern uint32_t pmm_free_blocks(void);
extern uint32_t pmm_total_blocks(void);
extern void ui_draw_taskbar(const char*);
extern void ui_draw_corner_clock(void);
extern void ui_set_theme(int);
extern int ui_theme_count(void);
extern const char *ui_theme_name(int);
extern void sleep_ms(uint32_t);
extern uint32_t get_ticks(void);
extern int ramfs_chmod(const char*,int);
extern int ramfs_chown(const char*,const char*);
extern void cmd_update(void);
extern void cmd_rollback(void);

/* ================================================================
   HavenDOS v0.7.11 - Shell
   New in 0.5.9.10: color ls, tree, which, guide, help overhaul,
                   command polish + consistent error messages
   ================================================================ */

/* ---- Pipe / redirect buffer ---- */
#define PIPE_BUF_SIZE 8192
static char pipe_buf[PIPE_BUF_SIZE];
static int  pipe_buf_len  = 0;
static int  pipe_mode     = 0;
static int  pipe_read_pos = 0;

/* Current working directory — empty string = root */
static char shell_cwd[64] = "";

/* Per-user home directory — set at login, used for ~ expansion */
static char shell_home[64] = "HOME";

/* Called from login.c after login to set this user's home dir */
void shell_set_home(const char *username) {
    strncpy(shell_home, "HOME/", 5);
    strncat(shell_home, username, sizeof(shell_home)-6);
    shell_home[sizeof(shell_home)-1] = 0;
    /* Start shell in user's home directory */
    strncpy(shell_cwd, shell_home, sizeof(shell_cwd)-1);
    shell_cwd[sizeof(shell_cwd)-1] = 0;
}

/* Build a full RAMFS path from CWD + name.
   Result is written into dst (must be at least 64 bytes).
   If name starts with / it is used as-is (absolute).
   If name starts with ~ it expands to the user's home directory. */
void make_path(char *dst, const char *name) {
    if(!name || !name[0]) { dst[0]=0; return; }
    /* Tilde expansion: ~ alone = home, ~/... = home/... */
    if(name[0]=='~') {
        strncpy(dst, shell_home, 63); dst[63]=0;
        if(name[1]=='/') {
            int dl=(int)strlen(dst);
            strncpy(dst+dl, name+1, 63-dl);
            dst[63]=0;
        }
        return;
    }
    if(name[0]=='/') { strncpy(dst, name+1, 63); dst[63]=0; return; }
    if(shell_cwd[0]) {
        strncpy(dst, shell_cwd, 47); dst[47]=0;
        strcat(dst, "/");
        int dl = (int)strlen(dst);
        strncpy(dst+dl, name, 63-dl);
    } else {
        strncpy(dst, name, 63);
    }
    dst[63]=0;
}

static void pipe_putchar(char c) {
    if(pipe_buf_len < PIPE_BUF_SIZE-1)
        pipe_buf[pipe_buf_len++] = c;
}
static void pipe_puts(const char *s) {
    while(*s) pipe_putchar(*s++);
}

/* ---- Output abstraction ---- */
static void out_char(char c) {
    if(pipe_mode) pipe_putchar(c);
    else vga_putchar(c);
}
static void out_str(const char *s) {
    if(pipe_mode) pipe_puts(s);
    else vga_puts(s);
}
static void out_int(int v) {
    char b[16]; itoa(v,b,10); out_str(b);
}
static void out_uint(uint32_t v) {
    char b[16]; utoa(v,b,10); out_str(b);
}

/* ================================================================
   PS1 PROMPT CUSTOMISATION
   Tokens: \u=user \h=hostname \w=path \$=$/# \t=time \d=date \n=newline
   ================================================================ */
static char ps1_format[64] = "\\u@havendos:\\w\\$ ";

static void ps1_render(char *out, int maxlen) {
    const char *f = ps1_format;
    int i = 0;
    while(*f && i < maxlen-1) {
        if(*f == '\\' && *(f+1)) {
            f++;
            switch(*f) {
                case 'u': {
                    const char *u = current_username();
                    while(*u && i < maxlen-1) out[i++] = *u++;
                    break;
                }
                case 'h': {
                    const char *h = "havendos";
                    while(*h && i < maxlen-1) out[i++] = *h++;
                    break;
                }
                case 'w': {
                    /* Show CWD, abbreviating the user's home directory as ~.
                       v0.7.6 FIX: this branch had never executed before,
                       because the broken strncmp() could not return 0 for a
                       prefix.  The moment v0.7.5 fixed strncmp it started
                       running — and it was wrong: `shell_cwd + hl - 1` points
                       at the LAST CHARACTER OF THE HOME NAME, not at the
                       slash after it, so `HOME/kyan/docs` rendered as
                       "~n/docs".  Off by one.  */
                    int hl = (int)strlen(shell_home);
                    const char *w;
                    if(shell_cwd[0] && strcmp(shell_cwd, shell_home)==0){
                        w = "~";
                    } else if(shell_cwd[0]
                              && strncmp(shell_cwd, shell_home, (size_t)hl)==0
                              && shell_cwd[hl]=='/'){
                        if(i<maxlen-1) out[i++]='~';
                        w = shell_cwd + hl;          /* -> "/docs" */
                    } else {
                        w = shell_cwd[0] ? shell_cwd : "/";
                    }
                    while(*w && i < maxlen-1) out[i++] = *w++;
                    break;
                }
                case '$': {
                    out[i++] = current_user_admin() ? '#' : '$';
                    break;
                }
                case 't': {
                    uint8_t h2,m2,s2;
                    rtc_get_time(&h2,&m2,&s2);
                    if(i+8 < maxlen) {
                        out[i++]='0'+h2/10; out[i++]='0'+h2%10; out[i++]=':';
                        out[i++]='0'+m2/10; out[i++]='0'+m2%10; out[i++]=':';
                        out[i++]='0'+s2/10; out[i++]='0'+s2%10;
                    }
                    break;
                }
                case 'd': {
                    uint8_t day,month; uint16_t year;
                    rtc_get_date(&day,&month,&year);
                    if(i+10 < maxlen) {
                        out[i++]='0'+day/10;   out[i++]='0'+day%10;   out[i++]='/';
                        out[i++]='0'+month/10; out[i++]='0'+month%10; out[i++]='/';
                        out[i++]='0'+(year/1000)%10; out[i++]='0'+(year/100)%10;
                        out[i++]='0'+(year/10)%10;   out[i++]='0'+year%10;
                    }
                    break;
                }
                case 'n': {
                    out[i++] = '\n';
                    break;
                }
                default:
                    out[i++] = '\\';
                    out[i++] = *f;
                    break;
            }
            f++;
        } else {
            out[i++] = *f++;
        }
    }
    out[i] = 0;
}

/* ---- Env vars ---- */
#define MAX_ENV 32
static char env_keys[MAX_ENV][16];
static char env_vals[MAX_ENV][64];
static int  env_count = 0;

void env_set(const char *k, const char *v) {
    for(int i=0;i<env_count;i++)
        if(strcmp(env_keys[i],k)==0){strncpy(env_vals[i],v,63);return;}
    if(env_count<MAX_ENV){
        strncpy(env_keys[env_count],k,15);
        strncpy(env_vals[env_count],v,63);
        env_count++;
    }
}
const char *env_get(const char *k) {
    for(int i=0;i<env_count;i++)
        if(strcmp(env_keys[i],k)==0) return env_vals[i];
    return "";
}

/* ---- Aliases ---- */
#define MAX_ALIAS 16
static char alias_keys[MAX_ALIAS][16];
static char alias_vals[MAX_ALIAS][64];
static int  alias_count = 0;

static void alias_set(const char *k, const char *v) {
    for(int i=0;i<alias_count;i++)
        if(strcmp(alias_keys[i],k)==0){strncpy(alias_vals[i],v,63);return;}
    if(alias_count<MAX_ALIAS){
        strncpy(alias_keys[alias_count],k,15);
        strncpy(alias_vals[alias_count],v,63);
        alias_count++;
    }
}
static const char *alias_get(const char *k) {
    for(int i=0;i<alias_count;i++)
        if(strcmp(alias_keys[i],k)==0) return alias_vals[i];
    return 0;
}
static int alias_del(const char *k) {
    for(int i=0;i<alias_count;i++){
        if(strcmp(alias_keys[i],k)==0){
            for(int j=i;j<alias_count-1;j++){
                strcpy(alias_keys[j],alias_keys[j+1]);
                strcpy(alias_vals[j],alias_vals[j+1]);
            }
            alias_count--;
            return 0;
        }
    }
    return -1;
}

/* ---- Persistent aliases — saved to ETC/ALIASES.TXT ---- */
#define ALIAS_FILE "ETC/ALIASES.TXT"

static void alias_save(void){
    char buf[1024]; int p=0;
    for(int i=0;i<alias_count&&p<1000;i++){
        const char *k=alias_keys[i],*v=alias_vals[i];
        while(*k&&p<1020) buf[p++]=*k++;
        buf[p++]='=';
        while(*v&&p<1020) buf[p++]=*v++;
        buf[p++]='\n';
    }
    vfs_write(ALIAS_FILE,buf,p);
}

static void alias_load(void){
    char buf[1024]; int r=vfs_read(ALIAS_FILE,buf,1023);
    if(r<=0) return;
    buf[r]=0;
    char *p=buf;
    while(*p){
        char *end=p; while(*end&&*end!='\n') end++;
        char saved=*end; *end=0;
        char *eq=strchr(p,'=');
        if(eq&&eq>p){
            char k[16],v[64];
            int kl=(int)(eq-p); if(kl>15)kl=15;
            strncpy(k,p,kl); k[kl]=0;
            strncpy(v,eq+1,63); v[63]=0;
            /* v0.9.5: `h` used to be a built-in alias for help, which
               made the H language's own command unreachable -- typing
               `h` opened the help screen. The default is gone; a copy
               saved by an older release is dropped here so the command
               comes back without anyone editing ETC/ALIASES.TXT. */
            if(!strcmp(k,"h")&&!strcmp(v,"help")) k[0]=0;
            if(k[0]&&v[0]) alias_set(k,v);
        }
        p=end; if(saved) p++; else break;
    }
}

/* ---- History ---- */
#define MAX_HIST 50
#define HIST_FILE ".shell_history"
static char history[MAX_HIST][128];
static int  hist_count=0, hist_pos=0;
static int  hist_loaded=0;

static void hist_load(void){
    if(hist_loaded) return;
    hist_loaded=1;
    extern int vfs_read(const char*, char*, uint32_t);
    char buf[MAX_HIST*130+4];
    int n=vfs_read(HIST_FILE,buf,sizeof(buf)-1);
    if(n<=0) return;
    buf[n]=0; hist_count=0;
    char *line=buf;
    while(*line&&hist_count<MAX_HIST){
        char *end=line; while(*end&&*end!='\n')end++;
        char sv=*end; *end=0;
        if(line[0]){ strncpy(history[hist_count++],line,127); }
        *end=sv; line=(*end)?end+1:end;
    }
    hist_pos=hist_count;
}

static void hist_save(void){
    extern int vfs_write(const char*, const char*, uint32_t);
    char buf[MAX_HIST*130+4]; int bp=0;
    int start=(hist_count>MAX_HIST)?hist_count-MAX_HIST:0;
    for(int i=start;i<hist_count;i++){
        const char *s=history[i%MAX_HIST];
        while(*s) buf[bp++]=*s++;
        buf[bp++]='\n';
    }
    buf[bp]=0;
    vfs_write(HIST_FILE,buf,(uint32_t)bp);
}

static void hist_add(const char *cmd) {
    if(!cmd[0]) return;
    if(hist_count>0 && strcmp(history[(hist_count-1)%MAX_HIST],cmd)==0){
        hist_pos=hist_count; return;
    }
    strncpy(history[hist_count%MAX_HIST],cmd,127);
    hist_count++;
    hist_pos=hist_count;
    hist_save();
}

/* ---- Tab completion ---- */
#define MAX_FILENAMES 64
#define MAX_FNAME_LEN 24
static char tab_names[MAX_FILENAMES][MAX_FNAME_LEN];
static int  tab_count=0;
static void tab_add(const char *name);
static void tab_collect(const char *name, int is_dir, uint32_t size){
    (void)is_dir;(void)size;
    tab_add(name);
}

/* v0.7.6: this table had EIGHT duplicated entries (find, grep, wc, cp, stat,
   hex, fortune, screensaver).  Harmless while strncmp() was broken, because
   tab completion never matched anything at all.  Once v0.7.5 fixed strncmp,
   typing "gre<Tab>" found *two* matches for "grep", so tab_complete() took the
   ambiguous path and refused to complete a command that has exactly one
   spelling.  De-duplicated, and tab_complete() now also rejects duplicates at
   match time so a filename that shares a command's name cannot resurrect the
   same bug.

   0.8.1: regenerated from exec_cmd() itself rather than maintained by hand.
   It had drifted the same way help had -- it completed `pong`, which does not
   dispatch, and did not complete ten commands that do, `disk` among them.
   tools/check_help.py now fails the build if this and the dispatcher disagree.
   `.` (the pipe operator) and `?` (the help alias) are deliberately absent:
   neither is something anyone tab-completes. */
static const char *shell_cmds[]={
    "2048","breakout","convert","memory","notes","stopwatch","tictactoe","ttt",
    "adduser","alias","ascii-art","banner","base64","beep","boot","bpkg",
    "calc","calendar","cat","cd","chat","chmod","chown","clear",
    "cls","color","copy","cowsay","cp","crash","curl","dashboard","date",
    "del","deluser","df","diff","dir","disk","dmesg","dns",
    "echo","edit","env","exfat","exit","export","fat16","fat32","fg","find","fortune","gfx","grep",
    "guide","h","hacker","hash","head","help","hex","history",
    "kill","kpoll",
    "host","hplusplus","htop","i2c","i2chid","ifconfig","install","jobs","lock","log","ls",
    "uninstall",
    "layout","lspci","man","matrix","mem","migrate","mkdir","motd","music","mv",
    "neofetch","netstat","notepad","notify","nslookup","open","paint","pair","passwd","ping",
    "pipes","play","poweroff","ps","ps1","pwd","quit","quiz",
    "reboot","rename","repeat","res","resolution","retro","rm","rollback","rsa","run",
    "screensaver","script","selftest","seq","set","settings","shutdown","snake","sort",
    "source","stars","stat","sysinfo","sysinfo-plus","sysmon","system","tail","tcpsend","tcptest",
    "tee","theme","time","timer","tls","tlsinfo","todo","touch",
    "tree","type","udpsend","unalias","uniq","unset","unzip","update",
    "lockdown","uptime","usb","users","ver","version","watch","wc","wget",
    "which","whoami","write","yes","zip",
    NULL
};

/* v0.7.6: tab_collect used to be fed by vfs_list(), which only ever lists the
   ROOT directory.  After `cd HOME/kyan`, completing a filename offered root
   entries that do not exist here and hid the ones that do.  Now driven from
   vfs_list_dir(shell_cwd, ...) so it completes what `ls` actually shows. */
static int tab_have(const char *name){
    for(int i=0;i<tab_count;i++)
        if(strcmp(tab_names[i],name)==0) return 1;
    return 0;
}
static void tab_add(const char *name){
    if(tab_count>=MAX_FILENAMES) return;
    if(tab_have(name)) return;                 /* suppress duplicates */
    strncpy(tab_names[tab_count],name,MAX_FNAME_LEN-1);
    tab_names[tab_count][MAX_FNAME_LEN-1]=0;
    tab_count++;
}

static const char *tab_complete(const char *prefix, int is_arg){
    int plen=(int)strlen(prefix);
    if(!plen) return 0;

    tab_count=0;
    if(!is_arg){
        for(int i=0;shell_cmds[i];i++)
            if(strncmp(shell_cmds[i],prefix,(size_t)plen)==0)
                tab_add(shell_cmds[i]);
    }
    /* Filenames from the CURRENT directory, not the root. */
    vfs_list_dir(shell_cwd, tab_collect);

    const char *match=0; int matches=0;
    for(int i=0;i<tab_count;i++)
        if(strncmp(tab_names[i],prefix,(size_t)plen)==0){match=tab_names[i];matches++;}

    if(matches==1) return match;
    if(matches>1){
        vga_putchar('\n');
        int col=0;
        for(int i=0;i<tab_count;i++){
            if(strncmp(tab_names[i],prefix,(size_t)plen)==0){
                vga_set_color(is_arg?VGA_LIGHT_GREEN:VGA_LIGHT_CYAN,VGA_BLACK);
                vga_puts(tab_names[i]);
                vga_set_color(VGA_DARK_GREY,VGA_BLACK); vga_puts("  ");
                col++;
                if(col%5==0) vga_putchar('\n');
            }
        }
        vga_set_color(VGA_WHITE,VGA_BLACK);
        if(col%5!=0) vga_putchar('\n');
    }
    return 0;
}

/* ---- Line input ----------------------------------------------------------
 * v0.7.6: rewritten.  The old reader only understood printable characters,
 * Backspace, Tab and Up/Down — the cursor could not move.  Mistyping the first
 * character of a long command meant deleting the whole line.  The keyboard
 * driver has been emitting Left/Right/Home/End/Delete (0x14B/0x14D/0x147/
 * 0x14F/0x153) since v0.7.4; nothing consumed them.
 *
 *   Left / Right   move by one character      Ctrl+A / Ctrl+E  start / end
 *   Home / End     jump to start / end        Ctrl+U / Ctrl+K  kill to start / end
 *   Delete         delete under cursor        Ctrl+W           delete word left
 *   Ctrl+R         reverse history search     Ctrl+L           clear screen
 *
 * Insertion happens at the cursor, so editing mid-line works properly.
 * ------------------------------------------------------------------------ */

static int cur_row(void){ int x,y; vga_get_cursor(&x,&y); return y; }

/* Repaint the editable part of the line and park the hardware cursor at `pos`.
   VGA text mode gives us no way to query the cursor column reliably across all
   the emulated BIOSes we run on, so we track the prompt width ourselves. */
static void line_redraw(const char *prompt, const char *buf, int pos, int *shown){
    int plen = (int)strlen(prompt);
    int blen = (int)strlen(buf);
    vga_set_cursor(0, cur_row());
    vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
    vga_puts(prompt);
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts(buf);
    /* erase whatever the previous, longer line left behind */
    for(int i=blen;i<*shown;i++) vga_putchar(' ');
    *shown = blen;
    vga_set_cursor(plen+pos, cur_row());
}

static int hist_search(char *buf, int max, const char *prompt){
    char pat[64]; int pl=0; pat[0]=0;
    int found=-1;
    while(1){
        /* draw the search prompt on the current line */
        vga_set_cursor(0, cur_row());
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("(reverse-search)`");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        vga_puts(pat);
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("': ");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        if(found>=0) vga_puts(history[found%MAX_HIST]);
        for(int i=0;i<40;i++) vga_putchar(' ');

        int c=-1;
        while(c==-1){ c=keyboard_getchar(); if(c==-1){toast_tick();sleep_ms(10);} }

        if(c=='\n'||c=='\r'){
            if(found>=0){ strncpy(buf,history[found%MAX_HIST],max-1); buf[max-1]=0; }
            vga_set_cursor(0, cur_row());
            for(int i=0;i<79;i++) vga_putchar(' ');
            vga_set_cursor(0, cur_row());
            (void)prompt;
            return found>=0 ? 1 : 0;
        }
        if(c==27){                                   /* ESC cancels */
            vga_set_cursor(0, cur_row());
            for(int i=0;i<79;i++) vga_putchar(' ');
            vga_set_cursor(0, cur_row());
            return 0;
        }
        if(c=='\b'||c==127){ if(pl>0) pat[--pl]=0; }
        else if(c>=32&&c<127&&pl<63){ pat[pl++]=(char)c; pat[pl]=0; }
        else continue;

        /* newest-first scan for a substring match */
        found=-1;
        if(pl){
            int total = hist_count<MAX_HIST?hist_count:MAX_HIST;
            for(int k=1;k<=total;k++){
                int idx=hist_count-k;
                if(idx<0) break;
                if(strstr(history[idx%MAX_HIST],pat)){ found=idx; break; }
            }
        }
    }
}

void shell_read_line(char *buf, int max, const char *prompt){
    int plen=(int)strlen(prompt);
    vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
    vga_puts(prompt);
    vga_set_color(VGA_WHITE,VGA_BLACK);

    int pos=0, len=0, shown=0;
    buf[0]=0;

    /* Keep the whole line on one row: prompt + text must fit 80 columns. */
    if(max > 79-plen) max = 79-plen;
    if(max < 8) max = 8;

    while(1){
        int c=-1;
        while(c==-1){ c=keyboard_getchar(); if(c==-1){toast_tick();sleep_ms(10);} }

        if(c=='\n'||c=='\r'){ vga_putchar('\n'); buf[len]=0; return; }
        /* v0.9.6.6: the window's X arrives as ESC, and ESC here does
           nothing, so on the HP the Shell's X did nothing. It is `exit`. */
        if(c==27 && chrome_take_close()){
            vga_putchar('\n'); strncpy(buf,"exit",max-1); buf[max-1]=0; return;
        }

        /* ---- cursor movement ---- */
        if(c==0x14B){ if(pos>0){pos--; vga_set_cursor(plen+pos,cur_row());} continue; } /* Left  */
        if(c==0x14D){ if(pos<len){pos++; vga_set_cursor(plen+pos,cur_row());} continue; } /* Right */
        if(c==0x147||c==1){ pos=0;   vga_set_cursor(plen,cur_row());     continue; }   /* Home / ^A */
        if(c==0x14F||c==5){ pos=len; vga_set_cursor(plen+len,cur_row()); continue; }   /* End  / ^E */

        /* ---- deletion ---- */
        if(c=='\b'||c==127){                                   /* Backspace */
            if(pos>0){
                for(int i=pos-1;i<len;i++) buf[i]=buf[i+1];
                pos--; len--; buf[len]=0;
                line_redraw(prompt,buf,pos,&shown);
            }
            continue;
        }
        if(c==0x153){                                          /* Delete    */
            if(pos<len){
                for(int i=pos;i<len;i++) buf[i]=buf[i+1];
                len--; buf[len]=0;
                line_redraw(prompt,buf,pos,&shown);
            }
            continue;
        }
        if(c==21){                                             /* Ctrl+U    */
            if(pos>0){
                int rest=len-pos;
                for(int i=0;i<rest;i++) buf[i]=buf[pos+i];
                len=rest; pos=0; buf[len]=0;
                line_redraw(prompt,buf,pos,&shown);
            }
            continue;
        }
        if(c==11){ len=pos; buf[len]=0;                        /* Ctrl+K    */
                   line_redraw(prompt,buf,pos,&shown); continue; }
        if(c==23){                                             /* Ctrl+W    */
            int e=pos; while(e>0&&buf[e-1]==' ') e--;
            while(e>0&&buf[e-1]!=' ') e--;
            if(e<pos){
                int rest=len-pos;
                for(int i=0;i<rest;i++) buf[e+i]=buf[pos+i];
                len=e+rest; pos=e; buf[len]=0;
                line_redraw(prompt,buf,pos,&shown);
            }
            continue;
        }
        if(c==12){                                             /* Ctrl+L    */
            vga_clear(); vga_set_cursor(0,0);
            shown=0; line_redraw(prompt,buf,pos,&shown);
            continue;
        }
        if(c==18){                                             /* Ctrl+R    */
            if(hist_search(buf,max,prompt)){ len=(int)strlen(buf); pos=len; }
            shown=0; line_redraw(prompt,buf,pos,&shown);
            continue;
        }

        /* ---- history ---- */
        if(c==0x148){                                          /* Up        */
            if(hist_pos>0){
                hist_pos--;
                strncpy(buf,history[hist_pos%MAX_HIST],max-1); buf[max-1]=0;
                len=(int)strlen(buf); pos=len;
                line_redraw(prompt,buf,pos,&shown);
            }
            continue;
        }
        if(c==0x150){                                          /* Down      */
            if(hist_pos<hist_count){
                hist_pos++;
                if(hist_pos<hist_count){ strncpy(buf,history[hist_pos%MAX_HIST],max-1); buf[max-1]=0; }
                else buf[0]=0;
                len=(int)strlen(buf); pos=len;
                line_redraw(prompt,buf,pos,&shown);
            }
            continue;
        }

        /* ---- tab completion (now completes the word under the cursor) ---- */
        if(c=='\t'){
            buf[len]=0;
            int ws=pos; while(ws>0&&buf[ws-1]!=' ') ws--;
            char word[64]; int wl=pos-ws;
            if(wl>63) wl=63;
            for(int i=0;i<wl;i++) word[i]=buf[ws+i];
            word[wl]=0;
            if(!wl) continue;
            const char *comp=tab_complete(word,ws!=0);
            if(comp){
                int cl=(int)strlen(comp);
                int delta=cl-wl;
                if(len+delta<max){
                    /* shift the tail, then splice the completion in */
                    for(int i=len;i>=pos;i--) buf[i+delta]=buf[i];
                    for(int i=0;i<cl;i++) buf[ws+i]=comp[i];
                    len+=delta; pos+=delta; buf[len]=0;
                }
            }
            shown=0;
            vga_putchar('\n');
            line_redraw(prompt,buf,pos,&shown);
            continue;
        }

        /* ---- printable: INSERT at the cursor ---- */
        if(c>=32&&c<127&&len<max-1){
            for(int i=len;i>pos;i--) buf[i]=buf[i-1];
            buf[pos]=(char)c;
            len++; pos++; buf[len]=0;
            if(pos==len){                    /* fast path: appending */
                vga_putchar((char)c);
                if(len>shown) shown=len;
            } else {
                line_redraw(prompt,buf,pos,&shown);
            }
        }
    }
}

/* ---- ls callback — color coded ---- */
/* locked flag fetched via ramfs_get_admin_only */
extern int ramfs_get_admin_only(const char *name);

/* v0.7.6: vfs_list_dir() hands the callback the LEAF name ("NOTES.TXT"), but
   ramfs_get_admin_only() indexes by full path ("HOME/kyan/NOTES.TXT").  Inside
   any subdirectory the lookup silently missed and every locked file rendered as
   an ordinary green file with no [L] marker.  Rebuild the full path first. */
static void ls_callback(const char *name, int is_dir, uint32_t size){
    char sbuf[12];
    char full[64]; make_path(full,name);
    int locked = (!is_dir) && ramfs_get_admin_only(full);
    /* color: cyan=dir, red=locked, green=normal file */
    if(is_dir)        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    else if(locked)   vga_set_color(VGA_LIGHT_RED,  VGA_BLACK);
    else              vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(name);
    int pad = 22-(int)strlen(name); while(pad-->0) vga_putchar(' ');
    if(is_dir){
        vga_set_color(VGA_CYAN,VGA_BLACK);
        vga_puts("  DIR");
    } else {
        /* right-align size in a 10-char field */
        utoa(size,sbuf,10);
        int slen=(int)strlen(sbuf);
        vga_set_color(locked?VGA_LIGHT_RED:VGA_LIGHT_GREY,VGA_BLACK);
        int pad2=8-slen; while(pad2-->0) vga_putchar(' ');
        vga_puts(sbuf);
        vga_puts(" B");
        if(locked){ vga_set_color(VGA_RED,VGA_BLACK); vga_puts(" [L]"); }
    }
    vga_putchar('\n');
}

static void ls_pipe_callback(const char *name, int is_dir, uint32_t size){
    (void)is_dir;(void)size;
    out_str(name); out_char('\n');
}

/* ---- tree: collect all entries then print with safe-char branches ---- */
#define MAX_TREE_FILES 64
static char tree_names[MAX_TREE_FILES][32];
static int  tree_is_dir[MAX_TREE_FILES];
static int  tree_count=0;
static void tree_collect(const char *name, int is_dir, uint32_t size){
    (void)size;
    if(tree_count<MAX_TREE_FILES){
        strncpy(tree_names[tree_count],name,31);
        tree_is_dir[tree_count]=is_dir;
        tree_count++;
    }
}

static void cmd_tree(void){
    tree_count=0;
    vfs_list_dir(shell_cwd, tree_collect);   /* v0.7.6: was root-only */
    vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts(shell_cwd[0]?shell_cwd:"/");
    vga_puts("\n");
    for(int i=0;i<tree_count;i++){
        int last=(i==tree_count-1);
        vga_set_color(VGA_DARK_GREY,VGA_BLACK);
        /* safe-char branch: use +- for branch, last item uses \\- */
        if(last) vga_puts(".--");
        else     vga_puts("+--");
        vga_putchar(' ');
        if(tree_is_dir[i]) vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        else {
            char tf[64]; make_path(tf,tree_names[i]);
            int lk=ramfs_get_admin_only(tf);
            vga_set_color(lk?VGA_LIGHT_RED:VGA_LIGHT_GREEN,VGA_BLACK);
        }
        vga_puts(tree_names[i]);
        if(tree_is_dir[i]){ vga_set_color(VGA_CYAN,VGA_BLACK); vga_puts("/"); }
        vga_putchar('\n');
    }
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    char cbuf[8]; itoa(tree_count,cbuf,10);
    vga_puts(cbuf); vga_puts(" item(s)\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

/* ---- which: check if a command is known ---- */
static void cmd_which(const char *name){
    if(!name||!name[0]){ vga_puts("Usage: which <command>\n"); return; }
    for(int i=0;shell_cmds[i];i++){
        if(strcmp(shell_cmds[i],name)==0){
            vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
            vga_puts(name);
            vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
            vga_puts(" is a built-in shell command\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            return;
        }
    }
    /* check aliases */
    const char *av=alias_get(name);
    if(av){
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts(name);
        vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        vga_puts(" is aliased to '");
        vga_puts(av);
        vga_puts("'\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        return;
    }
    /* check if it's a .bsh script in VFS */
    char scriptname[32]; strncpy(scriptname,name,27); strcat(scriptname,".BSH");
    if(vfs_exists(scriptname)||vfs_exists(name)){
        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts(name);
        vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        vga_puts(" is a script in /\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        return;
    }
    vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
    vga_puts(name);
    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts(": not found\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

/* ================================================================
   CALCULATOR
   ================================================================ */
static const char *calc_p;
static int calc_expr(void);
static int calc_number(void){
    while(*calc_p==' ') calc_p++;
    if(*calc_p=='('){
        calc_p++;
        int v=calc_expr();
        if(*calc_p==')') calc_p++;
        return v;
    }
    int neg=0;
    if(*calc_p=='-'){neg=1;calc_p++;}
    int v=0;
    while(*calc_p>='0'&&*calc_p<='9') v=v*10+(*calc_p++)-'0';
    return neg?-v:v;
}
/* v0.9.4.1: '^' is a power, right-associative (2^3^2 = 2^9), binding
   tighter than * and /. It used to be an unknown character, and an unknown
   character simply ended the expression: "(2 + 3) * 7 ^ 2" printed 35. */
static int calc_power(void){
    int b=calc_number();
    while(*calc_p==' ') calc_p++;
    if(*calc_p=='^'){
        calc_p++;
        int e=calc_power(), v=1;
        if(e<0) return 0;
        while(e-- > 0) v*=b;
        return v;
    }
    return b;
}
static int calc_term(void){
    int v=calc_power();
    while(1){
        while(*calc_p==' ') calc_p++;
        if(*calc_p=='*'||*calc_p=='/'||*calc_p=='%'){
            char op=*calc_p++;
            int r=calc_power();
            if(op=='*') v*=r;
            else if(op=='/') v=(r?v/r:0);
            else v=(r?v%r:0);
        } else break;
    }
    return v;
}
static int calc_expr(void){
    int v=calc_term();
    while(1){
        while(*calc_p==' ') calc_p++;
        if(*calc_p=='+'||*calc_p=='-'){
            char op=*calc_p++;
            int r=calc_term();
            if(op=='+') v+=r; else v-=r;
        } else break;
    }
    return v;
}
/* Where parsing stopped, if it stopped before the end: anything left is a
   character the calculator does not understand, and the answer says so
   rather than printing the value of the part before it. */
static const char *calc_left;
static int do_calc(const char *expr){
    int v;
    calc_p=expr;
    v=calc_expr();
    while(*calc_p==' ') calc_p++;
    calc_left = *calc_p ? calc_p : 0;
    return v;
}

/* ================================================================
   WC / HEAD / TAIL
   ================================================================ */
static void buf_wc(const char *buf, int len, int *lines, int *words, int *chars){
    int l=0,w=0,c=0,inw=0;
    for(int i=0;i<len;i++){
        char ch=buf[i]; c++;
        if(ch=='\n'||ch=='\r') l++;
        if(ch==' '||ch=='\t'||ch=='\n'||ch=='\r') inw=0;
        else { if(!inw){w++;inw=1;} }
    }
    *lines=l;*words=w;*chars=c;
}

static void print_lines_out(const char *buf, int len, int n, int from_end){
    int total=0;
    for(int i=0;i<len;i++) if(buf[i]=='\n') total++;
    int skip=from_end?(total-n>0?total-n:0):0;
    int show=n, skipped=0;
    const char *p=buf;
    while(p<buf+len&&show>0){
        const char *end=p;
        while(end<buf+len&&*end!='\n') end++;
        if(skipped>=skip){
            while(p<=end&&p<buf+len) out_char(*p++);
            if(p<buf+len&&*p=='\n'){out_char('\n');p++;}
            show--;
        } else {
            p=end; if(p<buf+len&&*p=='\n') p++;
            skipped++;
        }
    }
}

/* ================================================================
   SORT
   ================================================================ */
#define MAX_SORT_LINES 256
#define MAX_SORT_LINE  128
static char sort_lines[MAX_SORT_LINES][MAX_SORT_LINE];
static int  sort_count=0;

static void sort_buf(const char *buf, int len, int reverse, int unique){
    sort_count=0;
    const char *p=buf;
    while(p<buf+len&&sort_count<MAX_SORT_LINES){
        const char *end=p;
        while(end<buf+len&&*end!='\n') end++;
        int llen=(int)(end-p);
        if(llen>MAX_SORT_LINE-1) llen=MAX_SORT_LINE-1;
        strncpy(sort_lines[sort_count],p,llen);
        sort_lines[sort_count][llen]=0;
        sort_count++;
        p=end+(*end=='\n'?1:0);
    }
    for(int i=1;i<sort_count;i++){
        char tmp[MAX_SORT_LINE]; strcpy(tmp,sort_lines[i]);
        int j=i-1;
        while(j>=0){
            int cmp=strcmp(sort_lines[j],tmp);
            if(reverse?cmp<0:cmp>0){strcpy(sort_lines[j+1],sort_lines[j]);j--;}
            else break;
        }
        strcpy(sort_lines[j+1],tmp);
    }
    for(int i=0;i<sort_count;i++){
        if(unique&&i>0&&strcmp(sort_lines[i],sort_lines[i-1])==0) continue;
        out_str(sort_lines[i]); out_char('\n');
    }
}

/* ================================================================
   FIND
   ================================================================ */
/* v0.9.5: find was "names in this folder that end with the pattern if
   it starts with *, else contain it" -- `find *note*` found nothing,
   and nothing below the current folder was ever searched. Now it is a
   real glob (* and ?, any case) over the current folder and every
   folder below it, breadth first. A pattern with no wildcard matches
   anywhere in a name, which is what the old substring rule was for.

   Only FAT32 and the RAM filesystem list subfolders; FAT16 and FAT12
   list their root whatever folder they are asked for (see
   vfs_list_dir), so on those there is one level and no descent --
   descending would list the root forever. */
#define FIND_QMAX 48
static char find_q[FIND_QMAX][64];
static int  find_qt, find_found, find_trunc;
static char find_dir[64];
static const char *find_pat;

static char find_lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }

static int glob_ci(const char *p, const char *s){
    const char *star=0, *back=0;
    while(*s){
        if(*p=='?' || (*p && *p!='*' && find_lc(*p)==find_lc(*s))){ p++; s++; continue; }
        if(*p=='*'){ star=p++; back=s; continue; }
        if(star){ p=star+1; s=++back; continue; }
        return 0;
    }
    while(*p=='*') p++;
    return *p==0;
}

static void find_cb(const char *name, int is_dir, uint32_t size){
    (void)size;
    if(!name||!name[0]||!strcmp(name,".")||!strcmp(name,"..")) return;
    char full[64]; int n=0;
    for(const char *d=find_dir; *d && n<62; d++) full[n++]=*d;
    if(n && n<62) full[n++]='/';
    for(const char *d=name; *d && n<63; d++) full[n++]=*d;
    full[n]=0;
    if(glob_ci(find_pat,name)){
        out_char('/'); out_str(full);
        if(is_dir) out_char('/');
        out_char('\n');
        find_found++;
    }
    if(is_dir){
        if(find_qt<FIND_QMAX){ strncpy(find_q[find_qt],full,63); find_q[find_qt][63]=0; find_qt++; }
        else find_trunc=1;
    }
}

static void do_find(const char *pattern){
    extern int vfs_using_fat32(void);
    char pat[48]; int n=0;
    int wild=0;
    for(const char *c=pattern;*c;c++) if(*c=='*'||*c=='?') wild=1;
    if(!wild) pat[n++]='*';
    for(const char *c=pattern;*c&&n<45;c++) pat[n++]=*c;
    if(!wild) pat[n++]='*';
    pat[n]=0;
    find_pat=pat; find_found=0; find_trunc=0; find_qt=0;

    int deep = vfs_using_fat32() || !vfs_using_disk();
    strncpy(find_q[find_qt++], shell_cwd, 63);
    for(int qh=0; qh<find_qt; qh++){
        strncpy(find_dir, find_q[qh], 63); find_dir[63]=0;
        vfs_list_dir(find_dir, find_cb);
        if(!deep) break;
    }
    if(pipe_mode) return;
    if(!find_found) vga_printf("No names matching '%s' here or below.\n",pattern);
    else{
        vga_set_color(VGA_DARK_GREY,VGA_BLACK);
        vga_printf("%d found\n",find_found);
        vga_set_color(VGA_WHITE,VGA_BLACK);
    }
    if(find_trunc){
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_printf("(stopped descending after %d folders -- cd lower down to search deeper)\n",FIND_QMAX);
        vga_set_color(VGA_WHITE,VGA_BLACK);
    }
}

/* ================================================================
   DIFF
   ================================================================ */
static void do_diff(const char *f1, const char *f2){
    char b1[4096],b2[4096];
    int r1=vfs_read(f1,b1,4095); if(r1<0){vga_printf("Not found: %s\n",f1);return;}
    int r2=vfs_read(f2,b2,4095); if(r2<0){vga_printf("Not found: %s\n",f2);return;}
    b1[r1]=0; b2[r2]=0;
    char *l1=b1, *l2=b2;
    int lnum=1,diffs=0;
    while(*l1||*l2){
        char *e1=l1; while(*e1&&*e1!='\n') e1++;
        char *e2=l2; while(*e2&&*e2!='\n') e2++;
        char s1=*e1,s2=*e2; *e1=0;*e2=0;
        if(strcmp(l1,l2)!=0){
            vga_set_color(VGA_YELLOW,VGA_BLACK);
            vga_printf("%d< %s\n",lnum,l1);
            vga_printf("%d> %s\n",lnum,l2);
            vga_set_color(VGA_WHITE,VGA_BLACK);
            diffs++;
        }
        *e1=s1;*e2=s2;
        l1=e1+(*e1?1:0); l2=e2+(*e2?1:0);
        lnum++;
    }
    if(!diffs) vga_puts("Files are identical\n");
    else vga_printf("%d difference(s)\n",diffs);
}

/* ================================================================
   TEE
   ================================================================ */
static void do_tee(const char *fname, const char *data, int dlen, int append){
    if(append){
        char existing[4096]; int er=vfs_read(fname,existing,4095);
        if(er>0){
            if(er+dlen<4095){
                memcpy(existing+er,data,dlen);
                vfs_write(fname,existing,er+dlen);
            }
        } else vfs_write(fname,data,dlen);
    } else {
        vfs_write(fname,data,dlen);
    }
    for(int i=0;i<dlen;i++) vga_putchar(data[i]);
}

/* ================================================================
   NET COMMANDS
   ================================================================ */
extern void rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s);
extern void rtc_get_date(uint8_t *day, uint8_t *month, uint16_t *year);

static void cmd_date(void){
    uint8_t day,month; uint16_t year;
    rtc_get_date(&day,&month,&year);
    char buf[12];
    buf[0]='0'+day/10;   buf[1]='0'+day%10;   buf[2]='/';
    buf[3]='0'+month/10; buf[4]='0'+month%10; buf[5]='/';
    buf[6]='0'+(year/1000)%10; buf[7]='0'+(year/100)%10;
    buf[8]='0'+(year/10)%10;   buf[9]='0'+year%10; buf[10]=0;
    out_str(buf); out_char('\n');
}

static void cmd_time_rtc(void){
    uint8_t h,m,s;
    rtc_get_time(&h,&m,&s);
    char buf[10];
    buf[0]='0'+h/10; buf[1]='0'+h%10; buf[2]=':';
    buf[3]='0'+m/10; buf[4]='0'+m%10; buf[5]=':';
    buf[6]='0'+s/10; buf[7]='0'+s%10; buf[8]=0;
    out_str(buf); out_char('\n');
}

static void cmd_adduser(const char *name, const char *pass){
    if(!current_user_admin()){out_str("Admin only.\n");return;}
    if(!name[0]||!pass[0]){out_str("Usage: adduser <name> <password>\n");return;}
    int r=add_user(name,pass,0);
    if(r==1) { out_str("User '"); out_str(name); out_str("' created.\n"); }
    else       out_str("Failed (duplicate or limit reached).\n");
}

/* v0.9.5: the shell's own colours, kept and saved. Before this `color`
   lasted exactly until the next command reset to white on black. */
static int sh_fg = VGA_WHITE, sh_bg = VGA_BLACK;
void shell_colors_load(void){
    extern int cfg_shell_fg(void); extern int cfg_shell_bg(void);
    extern void cfg_load(void);
    cfg_load();
    int f=cfg_shell_fg(), b=cfg_shell_bg();
    sh_fg = (f>0) ? f : VGA_WHITE;
    sh_bg = VGA_BLACK;   /* text colour only -- see cmd_color */
    (void)b;
}
void shell_colors_set(int fg, int bg){
    extern void cfg_set_shell_colors(int,int); extern void cfg_save(void);
    if(fg<0||fg>15) fg=VGA_WHITE;
    if(bg<0||bg>7)  bg=VGA_BLACK;
    sh_fg=fg; sh_bg=bg;
    cfg_set_shell_colors(fg,bg); cfg_save();
    vga_set_color((vga_color_t)fg,(vga_color_t)bg);
}
void shell_colors_get(int *fg, int *bg){ *fg=sh_fg; *bg=sh_bg; }

static void cmd_color(const char *fg_s, const char *bg_s){
    if(fg_s[0] && !strcmp(fg_s,"reset")){
        shell_colors_set(VGA_WHITE,VGA_BLACK);
        vga_puts("Colours back to white on black.\n");
        return;
    }
    if(!fg_s[0]){
        vga_puts("Usage: color <0-15>   or   color reset\n");
        vga_puts("  0=Black 1=Blue 2=Green 3=Cyan 4=Red 5=Magenta\n");
        vga_puts("  6=Brown 7=LGrey 8=DGrey 9=LBlue 10=LGreen\n");
        vga_puts("  11=LCyan 12=LRed 13=LMagenta 14=Yellow 15=White\n");
        return;
    }
    int fg=atoi(fg_s); if(fg<0||fg>15) fg=7;
    /* Text colour only. A background did not survive: most commands
       draw their own highlights on black, so a blue background came out
       as stripes. Better a setting that holds than one that half does. */
    if(bg_s[0] && atoi(bg_s)!=0){
        vga_puts("color sets the text colour; the background stays black, because\n"
                 "most commands draw on black and anything else came out striped.\n");
    }
    if(fg==0){ vga_puts("Black text on black would be invisible.\n"); return; }
    shell_colors_set(fg,VGA_BLACK);
    vga_printf("Text colour set to %d (saved). 'color reset' undoes it.\n",fg);
}

static void cmd_cowsay(const char *msg){
    if(!msg||!msg[0]) msg="Moo!";
    int len=(int)strlen(msg);
    vga_putchar(' '); vga_putchar('+');
    for(int i=0;i<len+2;i++) vga_putchar('-');
    vga_puts("+\n | "); vga_puts(msg); vga_puts(" |\n");
    vga_putchar(' '); vga_putchar('+');
    for(int i=0;i<len+2;i++) vga_putchar('-');
    vga_puts("+\n");
    vga_puts("        \\   ^__^\n");
    vga_puts("         \\  (oo)\\_______\n");
    vga_puts("            (__)\\       )\\/\\\n");
    vga_puts("                ||----w |\n");
    vga_puts("                ||     ||\n");
}

/* ================================================================
   BANNER COMMAND - big ASCII art letters
   ================================================================ */
/* 5-wide x 7-tall font for A-Z, 0-9, space */
static const char *banner_font[37][7] = {
    /* A */
    {" ### "," # # ","#####","#   #","#   #","     ","     "},
    /* B */
    {"#### ","#   #","#### ","#   #","#### ","     ","     "},
    /* C */
    {" ####","#    ","#    ","#    "," ####","     ","     "},
    /* D */
    {"#### ","#   #","#   #","#   #","#### ","     ","     "},
    /* E */
    {"#####","#    ","#### ","#    ","#####","     ","     "},
    /* F */
    {"#####","#    ","#### ","#    ","#    ","     ","     "},
    /* G */
    {" ####","#    ","# ###","#   #"," ####","     ","     "},
    /* H */
    {"#   #","#   #","#####","#   #","#   #","     ","     "},
    /* I */
    {"#####","  #  ","  #  ","  #  ","#####","     ","     "},
    /* J */
    {"#####","   # ","   # ","#  # "," ##  ","     ","     "},
    /* K */
    {"#   #","#  # ","###  ","#  # ","#   #","     ","     "},
    /* L */
    {"#    ","#    ","#    ","#    ","#####","     ","     "},
    /* M */
    {"#   #","## ##","# # #","#   #","#   #","     ","     "},
    /* N */
    {"#   #","##  #","# # #","#  ##","#   #","     ","     "},
    /* O */
    {" ### ","#   #","#   #","#   #"," ### ","     ","     "},
    /* P */
    {"#### ","#   #","#### ","#    ","#    ","     ","     "},
    /* Q */
    {" ### ","#   #","# # #","#  ##"," ## #","     ","     "},
    /* R */
    {"#### ","#   #","#### ","#  # ","#   #","     ","     "},
    /* S */
    {" ####","#    "," ### ","    #","#### ","     ","     "},
    /* T */
    {"#####","  #  ","  #  ","  #  ","  #  ","     ","     "},
    /* U */
    {"#   #","#   #","#   #","#   #"," ### ","     ","     "},
    /* V */
    {"#   #","#   #","#   #"," # # ","  #  ","     ","     "},
    /* W */
    {"#   #","#   #","# # #","## ##","#   #","     ","     "},
    /* X */
    {"#   #"," # # ","  #  "," # # ","#   #","     ","     "},
    /* Y */
    {"#   #"," # # ","  #  ","  #  ","  #  ","     ","     "},
    /* Z */
    {"#####","   # ","  #  "," #   ","#####","     ","     "},
    /* 0 */
    {" ### ","#  ##","# # #","##  #"," ### ","     ","     "},
    /* 1 */
    {"  #  "," ##  ","  #  ","  #  ","#####","     ","     "},
    /* 2 */
    {" ### ","#   #","  ## "," #   ","#####","     ","     "},
    /* 3 */
    {"#### ","    #"," ### ","    #","#### ","     ","     "},
    /* 4 */
    {"#   #","#   #","#####","    #","    #","     ","     "},
    /* 5 */
    {"#####","#    ","#### ","    #","#### ","     ","     "},
    /* 6 */
    {" ### ","#    ","#### ","#   #"," ### ","     ","     "},
    /* 7 */
    {"#####","   # ","  #  "," #   ","#    ","     ","     "},
    /* 8 */
    {" ### ","#   #"," ### ","#   #"," ### ","     ","     "},
    /* 9 */
    {" ### ","#   #"," ####","    #"," ### ","     ","     "},
    /* space */
    {"     ","     ","     ","     ","     ","     ","     "},
};

static int banner_char_idx(char c) {
    if(c>='a'&&c<='z') c=c-'a'+'A';
    if(c>='A'&&c<='Z') return c-'A';
    if(c>='0'&&c<='9') return 26+(c-'0');
    return 36; /* space */
}

static void cmd_banner(const char *text) {
    if(!text||!text[0]) { vga_puts("Usage: banner <text>\n"); return; }
    int len=(int)strlen(text);
    if(len>12) len=12; /* cap at 12 chars to fit 80 cols */

    for(int row=0;row<5;row++){
        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        for(int ci=0;ci<len;ci++){
            int idx=banner_char_idx(text[ci]);
            vga_puts(banner_font[idx][row]);
            vga_putchar(' ');
        }
        vga_putchar('\n');
    }
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

/* ================================================================
   FORTUNE - random quotes on demand
   ================================================================ */
static const char *fortunes[]={
    "\"The best way to predict the future is to invent it.\" - Alan Kay",
    "\"Any sufficiently advanced technology is indistinguishable from magic.\" - Clarke",
    "\"First, solve the problem. Then, write the code.\" - John Johnson",
    "\"Make it work, make it right, make it fast.\" - Kent Beck",
    "\"Simplicity is the soul of efficiency.\" - Austin Freeman",
    "\"Talk is cheap. Show me the code.\" - Linus Torvalds",
    "\"The computer was born to solve problems that did not exist before.\" - Bill Gates",
    "\"It's not a bug, it's an undocumented feature.\" - Anonymous",
    "\"There are only two kinds of programming languages: bad ones and good ones nobody uses.\"",
    "\"Walking on water and developing software from a spec are easy if both are frozen.\"",
    "\"The best tool for the job is the one you already know.\" - Unknown",
    "\"Debugging is twice as hard as writing the code in the first place.\" - Kernighan",
    "\"In theory, theory and practice are the same. In practice, they are not.\"",
    "\"HavenDOS: built from scratch, one byte at a time.\" - TechHaven Studios",
    "\"Keep it simple, keep it fast, keep it yours.\" - TechHaven Studios",
    NULL
};

static void cmd_fortune(void) {
    int count=0;
    while(fortunes[count]) count++;
    int idx=(int)(get_ticks()%count);
    vga_set_color(VGA_YELLOW,VGA_BLACK);
    vga_puts(fortunes[idx]);
    vga_putchar('\n');
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

/* ================================================================
   NET COMMANDS
   ================================================================ */
extern int      net_ready(void);
extern uint32_t net_get_ip(void);
extern uint32_t net_get_mask(void);
extern uint32_t net_get_gw(void);
extern void     net_get_mac(uint8_t mac[6]);
extern void     net_set_ip(uint32_t,uint32_t,uint32_t);
/* v0.8.7.2: where the address came from. The declarations here follow
   this file's own convention of externing the net API rather than
   including net.h, which pulls in the whole stack. */
#define NET_CFG_FALLBACK 0
#define NET_CFG_DHCP     1
#define NET_CFG_MANUAL   2
extern int      net_config_source(void);
extern void     net_config_source_set(int);
extern uint32_t ip_from_str(const char *);
extern void     ip_to_str(uint32_t, char *);
extern void     mac_to_str(const uint8_t *, char *);
extern int      icmp_ping(uint32_t, uint16_t);
extern int      udp_send(uint32_t,uint16_t,uint16_t,const void*,uint16_t);
extern uint32_t dns_resolve(const char *);
extern uint32_t net_get_dns(void);
extern void     net_set_dns(uint32_t);
extern int      dhcp_request(void);

/* v0.8.9.9: declared ahead -- `ifconfig dhcp` reports these now too. */
#include "../include/netdev.h"
extern uint32_t dhcp_stat_sent(void), dhcp_stat_offers(void),
                dhcp_stat_acks(void), dhcp_stat_naks(void);
static void net_explain_send(void);
static void cmd_ifconfig(const char *a1, const char *a2){
    if(!net_ready()){
        out_str("No NIC detected.\n"); return;
    }
    /* ifconfig set <ip> <gw>
       v0.7.6: arg1 is the bare token "set" (4 bytes incl. NUL), so the old
       ip_from_str(a1+4) parsed whatever followed the string in memory.  The IP
       comes from arg2, which parse_cmd_args() fills with the rest of the line. */
    if(!strcmp(a1,"set")){
        if(!a2[0]){ out_str("Usage: ifconfig set <ip> <gw>\n"); return; }
        char ipbuf[20]; int k=0;
        const char *q=a2; while(*q==' ')q++;
        while(*q&&*q!=' '&&k<19) ipbuf[k++]=*q++;
        ipbuf[k]=0;
        while(*q==' ')q++;
        if(!ipbuf[0]||!*q){ out_str("Usage: ifconfig set <ip> <gw>\n"); return; }
        net_set_ip(ip_from_str(ipbuf), ip_from_str("255.255.255.0"), ip_from_str(q));
        net_config_source_set(NET_CFG_MANUAL);
        out_str("IP updated.\n"); return;
    }
    /* ifconfig dns <ip> */
    if(!strcmp(a1,"dns")){
        if(!a2[0]){ out_str("Usage: ifconfig dns <ip>\n"); return; }
        net_set_dns(ip_from_str(a2));
        out_str("DNS updated.\n"); return;
    }
    /* ifconfig dhcp */
    if(!strcmp(a1,"dhcp")){
        out_str("Running DHCP...\n");
        if(dhcp_request()){
            char buf[20];
            out_str("DHCP OK. IP: ");
            ip_to_str(net_get_ip(),buf); out_str(buf);
            out_str("  GW: "); ip_to_str(net_get_gw(),buf); out_str(buf);
            out_str("\n");
        } else {
            out_str("DHCP failed - still on the built-in fallback address.\n");
            /* v0.8.9.9: how far it got, every time, not only after a
               failed ping. */
            out_str("  asked "); out_int((int)dhcp_stat_sent());
            out_str(", offered "); out_int((int)dhcp_stat_offers());
            out_str(", acknowledged "); out_int((int)dhcp_stat_acks());
            out_str(".\n");
            net_explain_send();
            out_str("Nothing will route until a server answers, or you\n");
            out_str("set an address by hand with 'ifconfig set'.\n");
        }
        return;
    }
    /* ifconfig (show) */
    char buf[20]; uint8_t mac[6];
    net_get_mac(mac);
    vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
    out_str("eth0:\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    out_str("  MAC:  "); mac_to_str(mac,buf); out_str(buf); out_str("\n");
    out_str("  IP:   "); ip_to_str(net_get_ip(),buf); out_str(buf);
    out_str(" / "); ip_to_str(net_get_mask(),buf); out_str(buf); out_str("\n");
    out_str("  GW:   "); ip_to_str(net_get_gw(),buf); out_str(buf); out_str("\n");
    out_str("  DNS:  "); ip_to_str(net_get_dns(),buf); out_str(buf); out_str("\n");
    /* ── v0.8.7.2: WHERE the address came from, not just what it is ──
       The compile-time default is QEMU's -- 10.0.2.15 behind a gateway
       at 10.0.2.2 -- and on a real network it is an address nobody has
       heard of. Printing it under "Status: UP" is a machine calling
       itself configured while every lookup goes to a DNS server
       reached through a gateway that is not there.

       On the HP, that is the whole of "dns: resolution failed". */
    switch(net_config_source()){
    case NET_CFG_DHCP:
        out_str("  Status: UP, address from DHCP\n");
        break;
    case NET_CFG_MANUAL:
        out_str("  Status: UP, address set by hand\n");
        break;
    default:
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        out_str("  Status: LINK ONLY -- this address is the built-in\n");
        out_str("          fallback, not one this network gave us.\n");
        out_str("          Nothing will route. Run: ifconfig dhcp\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        break;
    }
    out_str("\n  ifconfig set <ip> <gw>  |  ifconfig dns <ip>  |  ifconfig dhcp\n");
}

/* ── v0.8.9.8: WHY NOTHING ANSWERED, IN WORDS ────────────────────────
   This used to print "try 'ping 10.0.2.2' for the QEMU gateway" after
   every failed ping, on every machine. On the HP with an iPhone that
   advice was worse than none: 10.0.2.2 exists only inside QEMU, and the
   reason nothing answered was that DHCP had never given this machine
   an address on the phone's network at all. Say which case it is. */
extern uint32_t dhcp_stat_sent(void), dhcp_stat_offers(void),
                dhcp_stat_acks(void), dhcp_stat_naks(void);
/* v0.8.9.9: why the DISCOVER never left, in words. Printed when DHCP
   asked nobody -- the HP's "0 asked" screen, which had nothing after
   it. The NIC is named, its link state given, and the error the stack
   got from it. */
extern uint32_t dhcp_stat_send_fail(void);
extern int      dhcp_last_send_err(void);
static void net_explain_send(void)
{
    int a = netdev_active();
    if(dhcp_stat_send_fail() == 0) return;
    out_str("  The request never left this machine: the network card\n"
            "  refused it ");
    out_int((int)dhcp_stat_send_fail());
    out_str(dhcp_stat_send_fail() == 1 ? " time" : " times");
    out_str(" (error "); out_int(dhcp_last_send_err()); out_str(").\n");
    if(a < 0){
        out_str("  There is no active network card at all.\n");
        return;
    }
    out_str("  Card: "); out_str(netdev_bus_name(a));
    out_str(netdev_link_up(a) ? ", link up" : ", link DOWN");
    out_str(". 'usb eth' shows its counters.\n");
}

static void net_explain(void)
{
    char ip[20], gw[20];
    ip_to_str(net_get_ip(), ip);
    ip_to_str(net_get_gw(), gw);
    if(net_config_source() == NET_CFG_FALLBACK){
        out_str("  No address from DHCP: this machine is on the built-in\n"
                "  "); out_str(ip); out_str(", which only exists inside QEMU.\n");
        out_str("  DHCP so far: "); out_int((int)dhcp_stat_sent());
        out_str(" asked, "); out_int((int)dhcp_stat_offers());
        out_str(" offered, "); out_int((int)dhcp_stat_acks());
        out_str(" acknowledged");
        if(dhcp_stat_naks()){ out_str(", "); out_int((int)dhcp_stat_naks()); out_str(" refused"); }
        out_str(".\n");
        net_explain_send();
        out_str("  'ifconfig dhcp' asks again.\n");
    } else {
        out_str("  This machine is "); out_str(ip);
        out_str(", gateway "); out_str(gw);
        out_str(". 'ping "); out_str(gw);
        out_str("' tests the first hop.\n");
    }
}

static void cmd_ping(const char *a1){
    if(!net_ready()){ out_str("No NIC.\n"); return; }
    if(!a1[0]){ out_str("Usage: ping <host|ip>\n"); return; }
    /* DNS resolve if not a bare IP */
    uint32_t dest;
    int is_ip=1;
    for(int i=0;a1[i];i++) if(!(a1[i]>='0'&&a1[i]<='9')&&a1[i]!='.') is_ip=0;
    if(is_ip) dest=ip_from_str(a1);
    else {
        out_str("Resolving "); out_str(a1); out_str("...\n");
        dest=dns_resolve(a1);
        if(!dest){ out_str("DNS failed.\n"); net_explain(); return; }
        char buf[20]; ip_to_str(dest,buf);
        out_str("-> "); out_str(buf); out_str("\n");
    }
    out_str("PING "); out_str(a1); out_str(" - 4 packets:\n");
    int replied=0;
    for(int i=0;i<4;i++){
        int ms=icmp_ping(dest,(uint16_t)i);
        if(ms>=0){ out_str("  seq="); out_int(i); out_str(" time="); out_int(ms); out_str("ms\n"); replied++; }
        else if(ms==-1){ out_str("  seq="); out_int(i); out_str(" timeout\n"); }
        else { out_str("  ARP failed - host unreachable\n"); break; }
        /* v0.9.6: between echoes, let the other tasks run -- a `ping &`
           job otherwise holds the machine for all four. Nothing of the
           network is held here: icmp_ping() has returned. */
        task_yield();
    }
    if(!replied) net_explain();
}

static void cmd_dns(const char *a1){
    if(!net_ready()){ out_str("No NIC.\n"); return; }
    if(!a1[0]){ out_str("Usage: dns <hostname>\n"); return; }
    out_str("Resolving: "); out_str(a1); out_str("\n");
    uint32_t ip=dns_resolve(a1);
    if(ip){ char buf[20]; ip_to_str(ip,buf); out_str("  -> "); out_str(buf); out_str("\n"); }
    else out_str("  DNS resolution failed.\n");
}

/* Progress bar for wget/curl */
/* ── Human-readable byte size formatter ─────────────────────────── */
/* Writes "1.2 MB", "512 KB", "47 B" etc. into buf (at least 16 bytes) */
static void format_bytes(int bytes, char *buf) {
    if(bytes < 0) bytes = 0;
    if(bytes < 1024) {
        utoa((uint32_t)bytes, buf, 10);
        strncat(buf, " B", 3);
    } else if(bytes < 1024*1024) {
        /* KB with one decimal */
        uint32_t kb  = (uint32_t)bytes / 1024;
        uint32_t dec = ((uint32_t)bytes % 1024) * 10 / 1024;
        utoa(kb, buf, 10);
        int l = (int)strlen(buf);
        buf[l++] = '.'; buf[l++] = '0'+dec; buf[l] = 0;
        strncat(buf, " KB", 4);
    } else {
        /* MB with one decimal */
        uint32_t mb  = (uint32_t)bytes / (1024*1024);
        uint32_t dec = ((uint32_t)bytes % (1024*1024)) * 10 / (1024*1024);
        utoa(mb, buf, 10);
        int l = (int)strlen(buf);
        buf[l++] = '.'; buf[l++] = '0'+dec; buf[l] = 0;
        strncat(buf, " MB", 4);
    }
}

static void wget_progress(int done, int total){
    char dbuf[16], tbuf[16];
    format_bytes(done, dbuf);
    if(total>0) {
        format_bytes(total, tbuf);
        int pct=done*100/total;
        int bar=pct*30/100;
        out_str("\r  [");
        for(int i=0;i<30;i++) out_char(i<bar?'#':'.');
        out_str("] ");
        out_int(pct); out_str("%  ");
        out_str(dbuf); out_str(" / "); out_str(tbuf);
        out_str("   ");
    } else {
        out_str("\r  "); out_str(dbuf); out_str(" received...   ");
    }
}

static void cmd_wget(const char *url, const char *outfile){
    if(!net_ready()){ out_str("No NIC.\n"); return; }
    if(!url[0]){ out_str("Usage: wget <url> [filename]\n"); return; }

    /* Determine output filename */
    char fname[64];
    if(outfile && outfile[0]){
        strncpy(fname,outfile,63); fname[63]=0;
    } else {
        /* use last component of URL path */
        const char *p=url; const char *last=url;
        while(*p){ if(*p=='/') last=p+1; p++; }
        strncpy(fname,last[0]?last:"index.html",63); fname[63]=0;
        if(!fname[0]) { strncpy(fname,"index.html",63); }
    }

    out_str("GET "); out_str(url); out_str("\n");
    out_str("Saving to: "); out_str(fname); out_str("\n");

    /* v0.7.6: this used to declare a PRIVATE http_resp_t with a completely
       different field order from include/http.h, then pass it through a
       `void *` extern so the compiler could not object.  net/http.c read the
       progress function pointer from 124 bytes past the end of this stack
       object and called it. */
    http_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.body           = 0;
    resp.body_max       = 0;
    resp.file_path      = fname;
    resp.follow_redirect= 1;
    resp.progress       = wget_progress;

    int status=http_get(url,&resp);
    out_str("\n");
    if(status>=200&&status<300){
        char szb[16]; format_bytes(resp.body_len, szb);
        out_str("Saved "); out_str(szb); out_str(" to "); out_str(fname); out_str("\n");
    } else if(status<0){
        out_str("Error: "); out_str(http_strerror(status)); out_str("\n");
    } else {
        out_str("HTTP "); out_int(status); out_str("\n");
    }
}

static void cmd_curl(const char *url){
    if(!net_ready()){ out_str("No NIC.\n"); return; }
    if(!url[0]){ out_str("Usage: curl <url>\n"); return; }

    static uint8_t body[65536];

    http_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.body           = body;
    resp.body_max       = 65535;
    resp.file_path      = 0;      /* v0.7.6: real NULL, so http.c keeps the
                                     body in memory.  It used to be a char[128]
                                     array field that could never be NULL, so
                                     every response went to the file branch and
                                     curl printed nothing at all. */
    resp.follow_redirect= 1;

    out_str("GET "); out_str(url); out_str("\n---\n");
    int status=http_get(url,&resp);
    if(status>0){
        if(resp.body_len<0) resp.body_len=0;
        body[resp.body_len]=0;
        out_str((char*)body);
        char szb[16]; format_bytes(resp.body_len, szb);
        out_str("\n---\nHTTP "); out_int(status);
        out_str("  "); out_str(szb); out_str("\n");
    } else {
        out_str("Error: "); out_str(http_strerror(status)); out_str("\n");
    }
}

static void cmd_udpsend(const char *a1, const char *a2){
    if(!net_ready()){ out_str("No NIC.\n"); return; }
    if(!a1[0]){ out_str("Usage: udpsend <ip> <port> <message>\n"); return; }
    char ip_str[16]; int pi=0;
    const char *p=a1;
    while(*p&&*p!=' '&&pi<15) ip_str[pi++]=*p++;
    ip_str[pi]=0; while(*p==' ')p++;
    uint16_t port=(uint16_t)atoi(p);
    const char *msg=a2[0]?a2:"hello";
    uint32_t dest=ip_from_str(ip_str);
    int r=udp_send(dest,9000,port,msg,(uint16_t)strlen(msg));
    if(r==0) out_str("Sent.\n");
    else if(r==-2) out_str("ARP timeout.\n");
    else out_str("Send failed.\n");
}

static void cmd_netstat(void){
    if(!net_ready()){ out_str("No NIC detected.\n"); return; }
    char buf[20]; uint8_t mac[6]; net_get_mac(mac);
    out_str("Network:\n  Status: UP\n  MAC:    ");
    mac_to_str(mac,buf); out_str(buf);
    out_str("\n  IP:     "); ip_to_str(net_get_ip(),buf); out_str(buf);
    out_str("\n  GW:     "); ip_to_str(net_get_gw(),buf); out_str(buf);
    out_str("\n  DNS:    "); ip_to_str(net_get_dns(),buf); out_str(buf); out_str("\n");
}

/* ================================================================
   COMMAND EXECUTOR FORWARD DECLARATIONS
   ================================================================ */
/* v0.7.4 new commands (shell_v072.c) */
void cmd_motd_v2(const char *arg1, const char *arg2);
void cmd_df_v2(void);
void cmd_neofetch_v2(void);
void cmd_screensaver_v2(const char *arg1);
void cmd_source_v2(const char *arg1);
void cmd_watch_v2(const char *interval_s, const char *cmd);
void cmd_script_v2(const char *filename);
void cmd_time_v2(const char *cmd, const char *a1);
void cmd_yes_v2(const char *str);
/* v0.7.20 new commands (lspci.c) */
void cmd_lspci(const char *arg1);
/* v0.7.21 new commands (usb.c) */
void cmd_usb(const char *arg1);
/* Phase A new command (fat32cmd.c) -- FAT32 volumes, read only */
void cmd_fat32(const char *arg1, const char *arg2);
void cmd_fat16(const char *arg1, const char *arg2);
void cmd_exfat(const char *arg1, const char *arg2);
void cmd_rsa(const char *arg1, const char *arg2);
void cmd_i2c(const char *arg1, const char *arg2);
void cmd_i2chid(const char *arg);
void cmd_gfx(const char *arg, const char *arg2);
void cmd_pair(const char *arg);
void cmd_crash(const char *arg1);
void desktop_layout_audit(void);
/* v0.7.6 new commands (shell_v076.c) */
void cmd_selftest_v6(const char *arg1);
void cmd_netstat_stats_v6(void);
void cmd_netstat_reset_v6(void);
void cmd_tlsinfo_v6(void);
void cmd_resolution_v6(const char *arg1);
void cmd_kpoll_v6(const char *arg1);
/* v0.7.14: the real BOOT Chat client lives in shell/chat.c — the v0.7.11
   `chat` stub that used to sit in this file has been removed. */
void cmd_bootchat(const char *arg1);
void cmd_nslookup_v6(const char *arg1);
int  shell_pref_rows(void);
int  shell_pref_cols(void);
/* v0.7.4 new commands (shell_v073.c) */
void cmd_base64_v3(const char *flag, const char *arg);
void cmd_hash_v3(const char *arg1, const char *arg2);
void cmd_todo_v3(const char *subcmd, const char *arg);
void cmd_lock_v3(void);
void cmd_wc_v3(const char *path);
void cmd_zip_v3(const char *src, const char *dst);
void cmd_unzip_v3(const char *src, const char *dst);

static void exec_cmd(const char *cmd, const char *arg1, const char *arg2);
/* Public wrapper for shell_v072.c */
void exec_cmd_public(const char *cmd, const char *a1, const char *a2) {
    exec_cmd(cmd, a1, a2);
}
void shell_run_script(const char *filename);
void clear_anim(void);
extern void cmd_man(const char *cmd);

/* ================================================================
   $VAR EXPANSION — shared by interactive shell and BSH scripts
   ================================================================ */
static void expand_vars(const char *line, char *out, int maxlen){
    int ei=0;
    for(int i=0;line[i]&&ei<maxlen-1;){
        if(line[i]=='$'){
            i++;
            char vname[16]; int vi=0;
            while(line[i]&&(line[i]=='_'||
                  (line[i]>='A'&&line[i]<='Z')||
                  (line[i]>='a'&&line[i]<='z')||
                  (line[i]>='0'&&line[i]<='9'))&&vi<15)
                vname[vi++]=line[i++];
            vname[vi]=0;
            const char *val=env_get(vname);
            if(val) while(*val&&ei<maxlen-1) out[ei++]=*val++;
        } else {
            out[ei++]=line[i++];
        }
    }
    out[ei]=0;
}

/* Is this string a pure integer (optional leading -)? */
static int is_int_str(const char *s){
    if(!*s) return 0;
    if(*s=='-') s++;
    if(!*s) return 0;
    while(*s){ if(*s<'0'||*s>'9') return 0; s++; }
    return 1;
}

/* Does this expanded value look like an arithmetic expression
   (digits, spaces, + - * / % ( ) only, at least one operator)? */
static int looks_arith(const char *s){
    int has_op=0,has_digit=0;
    for(const char *p=s;*p;p++){
        char c=*p;
        if(c>='0'&&c<='9'){has_digit=1;continue;}
        if(c==' '||c=='('||c==')'){continue;}
        if(c=='+'||c=='-'||c=='*'||c=='/'||c=='%'){has_op=1;continue;}
        return 0; /* contains something else — not pure arithmetic */
    }
    return has_op&&has_digit;
}

/* ================================================================
   BSH CONDITION EVALUATOR
   if/while support: ==  !=  <=  >=  <  >   plus truthy fallback
   Operands are var-expanded before comparison.
   ================================================================ */
static int eval_cond(const char *cond_raw){
    char cond[128];
    expand_vars(cond_raw,cond,128);

    /* trim leading/trailing spaces */
    char *s=cond; while(*s==' ')s++;
    int len=(int)strlen(s); while(len>0&&s[len-1]==' '){s[len-1]=0;len--;}

    /* find operator — check 2-char ops first */
    const char *ops2[]={"==","!=","<=",">=",0};
    const char *op=0; char *opPos=0; int oplen=0;
    for(int i=0;ops2[i];i++){
        char *p=strstr(s,ops2[i]);
        if(p){opPos=p;op=ops2[i];oplen=2;break;}
    }
    if(!opPos){
        for(char *p=s;*p;p++){
            if(*p=='<'||*p=='>'){opPos=p;op=(*p=='<')?"<":">";oplen=1;break;}
        }
    }

    if(!opPos){
        /* truthy: non-empty and not "0" */
        return s[0]!=0 && strcmp(s,"0")!=0;
    }

    char left[64],right[64];
    int ll=(int)(opPos-s); if(ll>63)ll=63;
    strncpy(left,s,ll); left[ll]=0;
    while(ll>0&&left[ll-1]==' '){left[--ll]=0;}
    char *rp=opPos+oplen; while(*rp==' ')rp++;
    strncpy(right,rp,63); right[63]=0;
    int rl=(int)strlen(right); while(rl>0&&right[rl-1]==' '){right[--rl]=0;}

    if(is_int_str(left)&&is_int_str(right)){
        int lv=atoi(left), rv=atoi(right);
        if(!strcmp(op,"=="))return lv==rv;
        if(!strcmp(op,"!="))return lv!=rv;
        if(!strcmp(op,"<")) return lv<rv;
        if(!strcmp(op,">")) return lv>rv;
        if(!strcmp(op,"<="))return lv<=rv;
        if(!strcmp(op,">="))return lv>=rv;
    } else {
        int c=strcmp(left,right);
        if(!strcmp(op,"=="))return c==0;
        if(!strcmp(op,"!="))return c!=0;
        if(!strcmp(op,"<")) return c<0;
        if(!strcmp(op,">")) return c>0;
        if(!strcmp(op,"<="))return c<=0;
        if(!strcmp(op,">="))return c>=0;
    }
    return 0;
}
/* ── parse one "cmd arg1 arg2" token from a string ── */
static void parse_cmd_args(const char *src,
                            char *cmd,  /* out, 64 */
                            char *arg1, /* out, 64 */
                            char *arg2  /* out, 256 — rest of line */) {
    cmd[0]=arg1[0]=arg2[0]=0;
    const char *p=src;
    while(*p==' ')p++;
    int i=0; while(*p&&*p!=' '&&i<63){cmd[i++]=*p++;} cmd[i]=0;
    while(*p==' ')p++;
    i=0; while(*p&&*p!=' '&&i<63){arg1[i++]=*p++;} arg1[i]=0;
    while(*p==' ')p++;
    i=0; while(*p&&i<255){arg2[i++]=*p++;} arg2[i]=0;
}

/* ── split line on unescaped '.' pipe character ── */
#define MAX_PIPE_STAGES 8
static int split_pipe_stages(char *line, char *stages[], int max){
    int n=0;
    stages[n++]=line;
    for(char *p=line+1;*p;p++){
        /* Use '.' as pipe char (safe char set) — but only when flanked by spaces */
        if(*p=='.'&&*(p-1)==' '&&*(p+1)==' '){
            *p=0; /* terminate left segment */
            if(n<max) stages[n++]=p+2; /* right segment starts after '. ' */
        }
    }
    return n;
}

static void run_pipeline(char *line){
    /* ── check for redirect first (takes priority over pipes for final output) ── */
    /* v0.7.6: the first of these two loops dereferenced *(p-1) with p==line,
       reading one byte BEFORE the buffer, and its result was then thrown away
       and recomputed by the second loop anyway.  Deleted. */
    char *redir_pos=0;
    int   redir_append=0;
    for(char *p=line;*p;p++){
        if(*p=='>'&&*(p+1)=='>'){redir_pos=p;redir_append=1;break;}
        if(*p=='>'){redir_pos=p;redir_append=0;break;}
    }

    char redir_fname[64]="";
    if(redir_pos){
        char *fn=redir_pos+(redir_append?2:1);
        while(*fn==' ')fn++;
        {   /* v0.7.6: `ls > out.txt` inside a subdir wrote to the root. */
            char tmpn[64]; strncpy(tmpn,fn,63); tmpn[63]=0;
            int tl=(int)strlen(tmpn); while(tl>0&&tmpn[tl-1]==' ') tmpn[--tl]=0;
            if(tmpn[0]) make_path(redir_fname,tmpn); else redir_fname[0]=0;
        }
        /* trim trailing spaces from left side */
        char *e=redir_pos-1; while(e>line&&*e==' '){*e=0;e--;}
        *redir_pos=0;
    }

    /* ── split into pipe stages ── */
    char *stages[MAX_PIPE_STAGES];
    int   ns=split_pipe_stages(line,stages,MAX_PIPE_STAGES);

    if(ns==1&&!redir_fname[0]){
        /* Fast path: no pipe, no redirect */
        char cmd[64],a1[64],a2[256];
        parse_cmd_args(stages[0],cmd,a1,a2);
        exec_cmd(cmd,a1,a2);
        return;
    }

    /* ── multi-stage pipe chain ── */
    /* Run stage 0 into pipe_buf, then feed into stage 1 ... etc. */
    /* We keep a secondary buffer to swap */
    static char pipe_buf2[PIPE_BUF_SIZE];

    for(int s=0;s<ns;s++){
        char cmd[64],a1[64],a2[256];
        parse_cmd_args(stages[s],cmd,a1,a2);

        int is_last=(s==ns-1);

        if(s==0){
            /* First stage: run into pipe_buf */
            pipe_buf_len=0; pipe_read_pos=0; pipe_mode=1;
            exec_cmd(cmd,a1,a2);
            pipe_mode=0;
            pipe_buf[pipe_buf_len]=0;
        } else if(!is_last){
            /* Middle stage: pipe_buf -> exec -> pipe_buf2, then swap */
            /* Copy pipe_buf into pipe_buf2 as "input" available via pipe_read */
            memcpy(pipe_buf2,pipe_buf,pipe_buf_len+1);
            int saved_len=pipe_buf_len;
            /* Set up pipe_buf as output, pipe_buf2 as input-readable */
            /* We pass the pipe input as arg2 override for commands that support it */
            pipe_buf_len=0; pipe_read_pos=0; pipe_mode=1;
            /* For filter commands, set a flag that input comes from pipe */
            /* Simplest approach: for known filters run directly on pipe_buf2 */
            if(!strcmp(cmd,"grep")){
                char *p2=pipe_buf2; int lnum=1,found=0;
                while(p2<pipe_buf2+saved_len){
                    char *end=p2;
                    while(end<pipe_buf2+saved_len&&*end!='\n')end++;
                    char saved_c=*end;*end=0;
                    if(a1[0]&&strstr(p2,a1)){
                        out_str(p2);out_char('\n');found=1;
                    }
                    *end=saved_c;
                    p2=end+(*end?1:0); lnum++;
                }
                (void)found;
            } else if(!strcmp(cmd,"sort")){
                memcpy(pipe_buf,pipe_buf2,saved_len);
                pipe_buf_len=saved_len;
                pipe_mode=0;
                sort_buf(pipe_buf,saved_len,!strcmp(a1,"-r"),0);
                /* sort_buf outputs directly — we need to recapture */
                /* For chained sort we just pass through for now */
                memcpy(pipe_buf,pipe_buf2,saved_len);
                pipe_buf_len=saved_len;
            } else if(!strcmp(cmd,"head")||!strcmp(cmd,"tail")){
                int n=10;
                if(a1[0]=='-')n=atoi(a1+1);
                else if(a1[0])n=atoi(a1);
                if(n<=0)n=10;
                memcpy(pipe_buf,pipe_buf2,saved_len);pipe_buf_len=saved_len;
                pipe_mode=0;
                /* re-enable pipe_mode for output */
                pipe_buf_len=0;pipe_mode=1;
                print_lines_out(pipe_buf2,saved_len,n,!strcmp(cmd,"tail"));
            } else {
                /* Unknown middle stage: just pass pipe_buf through */
                memcpy(pipe_buf,pipe_buf2,saved_len);
                pipe_buf_len=saved_len;
            }
            pipe_mode=0;
            pipe_buf[pipe_buf_len]=0;
        } else {
            /* Last stage: consume pipe_buf, output to screen or file */
            if(!strcmp(cmd,"grep")){
                char *p2=pipe_buf; int lnum=1,found=0;
                while(p2<pipe_buf+pipe_buf_len){
                    char *end=p2;
                    while(end<pipe_buf+pipe_buf_len&&*end!='\n')end++;
                    char saved_c=*end;*end=0;
                    if(a1[0]&&strstr(p2,a1)){
                        vga_set_color(VGA_YELLOW,VGA_BLACK);
                        vga_printf("%d: ",lnum);
                        vga_set_color(VGA_WHITE,VGA_BLACK);
                        vga_puts(p2);vga_putchar('\n');found=1;
                    }
                    *end=saved_c;
                    p2=end+(*end?1:0);lnum++;
                }
                if(!found)vga_printf("No matches for '%s'\n",a1);
            } else if(!strcmp(cmd,"wc")){
                int l,w,c; buf_wc(pipe_buf,pipe_buf_len,&l,&w,&c);
                vga_printf("  Lines: %d   Words: %d   Chars: %d\n",l,w,c);
            } else if(!strcmp(cmd,"sort")){
                sort_buf(pipe_buf,pipe_buf_len,!strcmp(a1,"-r"),0);
            } else if(!strcmp(cmd,"uniq")){
                sort_buf(pipe_buf,pipe_buf_len,0,1);
            } else if(!strcmp(cmd,"head")){
                int n=10;
                if(a1[0]=='-')n=atoi(a1+1);
                else if(a1[0])n=atoi(a1);
                if(n<=0)n=10;
                print_lines_out(pipe_buf,pipe_buf_len,n,0);
            } else if(!strcmp(cmd,"tail")){
                int n=10;
                if(a1[0]=='-')n=atoi(a1+1);
                else if(a1[0])n=atoi(a1);
                if(n<=0)n=10;
                print_lines_out(pipe_buf,pipe_buf_len,n,1);
            } else if(!strcmp(cmd,"tee")){
                if(!a1[0]){vga_puts("tee: need filename\n");return;}
                char fp[64];make_path(fp,a1);
                do_tee(fp,pipe_buf,pipe_buf_len,0);
            } else if(!strcmp(cmd,"cat")||!strcmp(cmd,"less")){
                for(int i=0;i<pipe_buf_len;i++) vga_putchar(pipe_buf[i]);
                vga_putchar('\n');
            } else {
                /* Generic: run the cmd with pipe_mode set so it can read */
                pipe_read_pos=0; pipe_mode=1;
                exec_cmd(cmd,a1,a2);
                pipe_mode=0;
            }
        }
    }

    /* Handle redirect of final pipe output */
    if(redir_fname[0]){
        if(redir_append){
            char existing[4096];int er=vfs_read(redir_fname,existing,4095);
            if(er>0&&er+pipe_buf_len<4095){
                memcpy(existing+er,pipe_buf,pipe_buf_len);
                vfs_write(redir_fname,existing,er+pipe_buf_len);
            } else vfs_write(redir_fname,pipe_buf,pipe_buf_len);
        } else {
            vfs_write(redir_fname,pipe_buf,pipe_buf_len);
        }
        vga_printf("Written to %s (%d bytes)\n",redir_fname,pipe_buf_len);
    }
    return;

}

/* ================================================================
   SNAKE GAME
   ================================================================ */
#define SN_W 36
#define SN_H 16
static void run_snake(void){
    int sx[200],sy[200],slen=3,sdx=1,sdy=0,dead=0,score=0;
    int fx,fy;
    for(int i=0;i<slen;i++){sx[i]=10-i;sy[i]=8;}
    fx=20+(int)((get_ticks()*13)%10);
    fy=4+(int)((get_ticks()*7)%8);
    vga_clear(); toast_clear();
    ui_draw_taskbar("Snake");
    for(int x=0;x<SN_W+2;x++){
        vga_putchar_at('#',22+x,2,VGA_GREEN,VGA_BLACK);
        vga_putchar_at('#',22+x,SN_H+3,VGA_GREEN,VGA_BLACK);
    }
    for(int y=3;y<SN_H+3;y++){
        vga_putchar_at('#',22,y,VGA_GREEN,VGA_BLACK);
        vga_putchar_at('#',22+SN_W+1,y,VGA_GREEN,VGA_BLACK);
    }
    vga_puts_at("Arrows=move  Q=quit",22,SN_H+4,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Score: 0    ",22,1,VGA_YELLOW,VGA_BLACK);
    uint32_t last=get_ticks();
    while(!dead){
        int c=keyboard_getchar();
        if(c=='q'||c=='Q'||c==27) break;
        if(c==0x148&&sdy==0){sdx=0;sdy=-1;}
        if(c==0x150&&sdy==0){sdx=0;sdy=1;}
        if(c==0x14B&&sdx==0){sdx=-1;sdy=0;}
        if(c==0x14D&&sdx==0){sdx=1;sdy=0;}
        if(get_ticks()-last<8){__asm__ volatile("hlt");continue;}
        last=get_ticks();
        int nx=sx[0]+sdx,ny=sy[0]+sdy;
        if(nx<0||nx>=SN_W||ny<0||ny>=SN_H){dead=1;break;}
        for(int i=0;i<slen;i++) if(sx[i]==nx&&sy[i]==ny){dead=1;break;}
        if(dead) break;
        vga_putchar_at(' ',23+sx[slen-1],3+sy[slen-1],VGA_BLACK,VGA_BLACK);
        for(int i=slen-1;i>0;i--){sx[i]=sx[i-1];sy[i]=sy[i-1];}
        sx[0]=nx;sy[0]=ny;
        if(nx==fx&&ny==fy){
            score+=10;slen++;
            fx=(int)((get_ticks()*17)%SN_W);
            fy=(int)((get_ticks()*11)%SN_H);
        }
        for(int i=0;i<slen;i++)
            vga_putchar_at(i?'o':'@',23+sx[i],3+sy[i],VGA_LIGHT_GREEN,VGA_BLACK);
        vga_putchar_at('*',23+fx,3+fy,VGA_RED,VGA_BLACK);
        char sbuf[12];utoa(score,sbuf,10);
        vga_puts_at("Score: ",22,1,VGA_YELLOW,VGA_BLACK);
        vga_puts_at(sbuf,29,1,VGA_WHITE,VGA_BLACK);
    }
    if(dead) vga_puts_at("  GAME OVER! Press any key  ",24,SN_H/2+3,VGA_WHITE,VGA_RED);
    else     vga_puts_at("  Press any key             ",24,SN_H/2+3,VGA_WHITE,VGA_BLACK);
    keyboard_waitchar();
}

/* ================================================================
   HELP
   ================================================================ */
/* ================================================================
   GUIDE — built-in interactive user guide
   ================================================================ */
/* ================================================================
   HELP — categorized command reference
   ================================================================ */
/* cmd_help() now lives in shell/help.c, corrected against exec_cmd().
   It was 150 lines of string tables inside this file and had drifted:
   a documented `pong` that never dispatched, `h-lang` for `h`, and
   eleven working commands missing.  See that file's header. */
extern void cmd_help(void);

/* ================================================================
   STAT — show file metadata
   ================================================================ */
static void cmd_stat(const char *arg1) {
    if (!arg1 || !arg1[0]) { vga_puts("Usage: stat <file>\n"); return; }
    char fp[64]; make_path(fp, arg1);
    if (!vfs_exists(fp)) { vga_printf("stat: %s: no such file\n", fp); return; }
    int isdir = vfs_is_dir(fp);
    char buf[4096]; int sz = 0;
    if (!isdir) { sz = vfs_read(fp, buf, 4095); if (sz < 0) sz = 0; }
    int locked = (!isdir) && ramfs_get_admin_only(fp);
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  File: "); vga_set_color(VGA_WHITE, VGA_BLACK); vga_puts(fp); vga_putchar('\n');
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_printf("  Type: %s\n", isdir ? "directory" : "regular file");
    if (!isdir) vga_printf("  Size: %d bytes\n", sz);
    vga_printf("  Mode: %s\n", locked ? "admin-only [locked]" : "public");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* ================================================================
   HEX — hexdump a file or string
   ================================================================ */
static void cmd_hex(const char *arg1, const char *arg2) {
    if (!arg1 || !arg1[0]) { vga_puts("Usage: hex <file>  OR  hex -s <string>\n"); return; }
    char buf[512]; int len = 0;
    if (!strcmp(arg1, "-s")) {
        /* hexdump a string literal */
        const char *s = arg2[0] ? arg2 : "hello";
        len = (int)strlen(s);
        if (len > 511) len = 511;
        for (int i = 0; i < len; i++) buf[i] = s[i];
    } else {
        char fp[64]; make_path(fp, arg1);
        int r = vfs_read(fp, buf, 511);
        if (r < 0) { vga_printf("hex: %s: not found\n", fp); return; }
        len = r;
    }
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("Offset   00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F  ASCII\n");
    vga_puts("------   -----------------------------------------------  ----------------\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    for (int row = 0; row < len; row += 16) {
        /* offset */
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        char ob[5];
        ob[0] = "0123456789ABCDEF"[(row>>12)&0xF];
        ob[1] = "0123456789ABCDEF"[(row>>8) &0xF];
        ob[2] = "0123456789ABCDEF"[(row>>4) &0xF];
        ob[3] = "0123456789ABCDEF"[ row     &0xF];
        ob[4] = 0;
        vga_puts(ob); vga_puts("     ");
        /* hex bytes */
        for (int i = 0; i < 16; i++) {
            int idx = row + i;
            if (idx < len) {
                unsigned char b = (unsigned char)buf[idx];
                char hb[3];
                hb[0] = "0123456789ABCDEF"[b>>4];
                hb[1] = "0123456789ABCDEF"[b&0xF];
                hb[2] = 0;
                vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
                vga_puts(hb);
                vga_set_color(VGA_DARK_GREY, VGA_BLACK);
                vga_putchar(' ');
            } else {
                vga_puts("   ");
            }
            if (i == 7) vga_putchar(' ');
        }
        /* ASCII */
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("  ");
        for (int i = 0; i < 16; i++) {
            int idx = row + i;
            if (idx < len) {
                unsigned char b = (unsigned char)buf[idx];
                vga_set_color((b >= 32 && b < 127) ? VGA_WHITE : VGA_DARK_GREY, VGA_BLACK);
                vga_putchar((b >= 32 && b < 127) ? (char)b : '.');
            } else {
                vga_putchar(' ');
            }
        }
        vga_set_color(VGA_WHITE, VGA_BLACK);
        vga_putchar('\n');
    }
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_printf("  %d bytes\n", len);
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* ================================================================
   TCPTEST — test a TCP connection to host:port
   ================================================================ */
extern int tcp_connect(uint32_t ip, uint16_t port);
extern int tcp_send(int fd, const void *data, uint16_t len);
extern int tcp_recv(int fd, void *buf, uint16_t maxlen, uint32_t timeout_ms);
extern void tcp_close(int fd);
extern int tcp_connected(int fd);

static void cmd_tcptest(const char *host, const char *port_s) {
    if (!net_ready()) { out_str("No NIC.\n"); return; }
    if (!host[0] || !port_s[0]) {
        out_str("Usage: tcptest <host|ip> <port>\n");
        out_str("  Example: tcptest 10.0.2.2 80\n");
        return;
    }
    /* Resolve host */
    uint32_t ip;
    int is_ip = 1;
    for (int i = 0; host[i]; i++) if (!(host[i]>='0' && host[i]<='9') && host[i]!='.') is_ip=0;
    if (is_ip) ip = ip_from_str(host);
    else {
        out_str("Resolving "); out_str(host); out_str("...\n");
        ip = dns_resolve(host);
        if (!ip) { out_str("DNS failed.\n"); return; }
        char ips[20]; ip_to_str(ip, ips);
        out_str("-> "); out_str(ips); out_str("\n");
    }
    uint16_t port = (uint16_t)atoi(port_s);
    char ips[20]; ip_to_str(ip, ips);
    out_str("Connecting to "); out_str(ips);
    out_str(":"); out_int(port); out_str("...\n");

    int fd = tcp_connect(ip, port);
    if (fd < 0) {
        out_str("Connect failed ("); out_int(fd); out_str(")\n");
        return;
    }
    if (!tcp_connected(fd)) {
        out_str("TCP handshake timeout.\n");
        tcp_close(fd);
        return;
    }
    out_str("Connected! fd="); out_int(fd); out_str("\n");

    /* Send HTTP HEAD to see if it's an HTTP server */
    char req[128];
    strncpy(req, "HEAD / HTTP/1.0\r\nHost: ", 127);
    strncat(req, host, 127);
    strncat(req, "\r\n\r\n", 127);
    tcp_send(fd, req, (uint16_t)strlen(req));

    char rbuf[256];
    int r = tcp_recv(fd, rbuf, 255, 2000);
    if (r > 0) {
        rbuf[r] = 0;
        out_str("Response ("); out_int(r); out_str(" bytes):\n");
        /* Print first line only */
        int i = 0;
        while (i < r && rbuf[i] != '\n' && i < 80) { out_char(rbuf[i]); i++; }
        out_char('\n');
    } else {
        out_str("No response (port open, not HTTP or silent).\n");
    }
    tcp_close(fd);
    out_str("Connection closed.\n");
}

/* ================================================================
   TCPSEND — send raw data over TCP and show response
   ================================================================ */
static void cmd_tcpsend(const char *host, const char *port_s) {
    if (!net_ready()) { out_str("No NIC.\n"); return; }
    if (!host[0] || !port_s[0]) {
        out_str("Usage: tcpsend <host|ip> <port>\n");
        out_str("  Then type data line by line. Empty line sends. ESC quits.\n");
        return;
    }
    uint32_t ip;
    int is_ip = 1;
    for (int i = 0; host[i]; i++) if (!(host[i]>='0' && host[i]<='9') && host[i]!='.') is_ip=0;
    if (is_ip) ip = ip_from_str(host);
    else {
        out_str("Resolving "); out_str(host); out_str("...\n");
        ip = dns_resolve(host);
        if (!ip) { out_str("DNS failed.\n"); return; }
    }
    uint16_t port = (uint16_t)atoi(port_s);
    char ips[20]; ip_to_str(ip, ips);
    out_str("Connecting to "); out_str(ips); out_str(":"); out_int(port); out_str("...\n");

    int fd = tcp_connect(ip, port);
    if (fd < 0 || !tcp_connected(fd)) {
        out_str("Connect failed.\n");
        if (fd >= 0) tcp_close(fd);
        return;
    }
    out_str("Connected. Type data (empty line = send, ESC = quit):\n");

    char line[256]; int lpos = 0;
    while (1) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("> ");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        lpos = 0; line[0] = 0;
        while (1) {
            int c = -1;
            while (c == -1) c = keyboard_getchar();
            if (c == 27) { goto tcpsend_done; }
            if (c == '\n' || c == '\r') { vga_putchar('\n'); break; }
            if ((c == '\b' || c == 127) && lpos > 0) { lpos--; line[lpos] = 0; vga_putchar('\b'); }
            else if (c >= 32 && c < 127 && lpos < 255) { line[lpos++] = (char)c; line[lpos] = 0; vga_putchar((char)c); }
        }
        if (!lpos) {
            /* Empty line = receive any pending data */
            char rbuf[512];
            int r = tcp_recv(fd, rbuf, 511, 500);
            if (r > 0) {
                rbuf[r] = 0;
                vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
                out_str("<<< "); out_str(rbuf);
                vga_set_color(VGA_WHITE, VGA_BLACK);
            } else {
                out_str("(no data)\n");
            }
            continue;
        }
        line[lpos++] = '\r'; line[lpos++] = '\n'; line[lpos] = 0;
        int r = tcp_send(fd, line, (uint16_t)lpos);
        if (r < 0) { out_str("Send failed.\n"); break; }
    }
tcpsend_done:
    tcp_close(fd);
    out_str("Disconnected.\n");
}


/* ================================================================
   EXEC_CMD
   ================================================================ */
/* ----------------------------------------------------------------
   dmesg, as a function.
   Lifted out of exec_cmd() so the desktop's "Boot Log" app replays
   the log through this exact pager rather than carrying a second
   copy of it.  Three installers that each had their own disk scan
   is what a second copy costs (see CHANGELOG 0.8 Beta), so there is
   one of these and both callers use it.
   ---------------------------------------------------------------- */
void cmd_dmesg_pager(void){

        /* This used to print its own hardcoded copy of the boot story, with
           timestamps -- [0.000] through [0.011] -- that nothing had measured
           and that were simply typed one millisecond apart.  It replays the
           kernel log now, which is the same buffer the boot screen printed
           live, rendered by the same function so the two cannot drift. */
        extern int  klog_count(void);
        extern int  klog_dropped(void);
        extern void klog_print_line(int);
        int total=klog_count();
        /* A pager starts on a clean screen: otherwise the shell banner and
           the command line are still there and the first page scrolls off
           the top, which is the part worth reading. */
        vga_clear(); vga_set_cursor(0,0);
        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts("[dmesg] kernel log, " HAVENDOS_VER "  ");
        { char n[12]; utoa((uint32_t)total,n,10); vga_puts(n); }
        vga_puts(" lines\n");
        vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        /* Paged, because a full boot is about thirty lines and the screen
           holds twenty-five -- an unpaged dump scrolls the beginning away,
           and the beginning is where a boot that went wrong went wrong. */
        int per=vga_rows()-2, shown=0;
        extern int klog_gap_at(void);
        for(int i=0;i<total;i++){
            /* v0.8.9.6: the boot is pinned and the rest is a ring, so a
               long session has a gap between them. Said where it is. */
            if(i==klog_gap_at()){
                vga_set_color(VGA_YELLOW,VGA_BLACK);
                vga_printf("(... %d older lines rotated out after the boot ...)\n",
                           klog_dropped());
                vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
                shown++;
            }
            klog_print_line(i);
            if(++shown>=per && i<total-1){
                shown=0;
                vga_set_color(VGA_DARK_GREY,VGA_BLACK);
                vga_puts("-- more -- (any key, Q to stop)");
                int c=keyboard_waitchar();
                vga_puts("\n");
                vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
                if(c=='q'||c=='Q'||c==27) break;
            }
        }
    
}

static void exec_cmd(const char *cmd, const char *arg1, const char *arg2){
    if(!cmd||!cmd[0]) return;

    /* v0.9.5: one System app (shell/sysapp.c) for what used to be a
       full-screen dashboard drawn right here, three desktop apps and the
       `sysinfo` text dump. Every old name opens it. */
    if(!strcmp(cmd,"system")||!strcmp(cmd,"dashboard")||!strcmp(cmd,"htop")||
       !strcmp(cmd,"sysmon")||!strcmp(cmd,"sysinfo")||!strcmp(cmd,"sysinfo-plus")){
        extern void app_system_open(const char*);
        app_system_open(arg1);
        toast_clear();
    }

    else if(!strcmp(cmd,"help")||!strcmp(cmd,"?")){
        extern void cmd_help_arg(const char*);
        cmd_help_arg(arg1);
    }

    else if(!strcmp(cmd,"ver")||!strcmp(cmd,"version")){
        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts("HavenDOS " HAVENDOS_VER " " HAVENDOS_CHANNEL " \"" HAVENDOS_CODENAME "\"  (32-bit x86)\n");
        vga_set_color(VGA_DARK_GREY,VGA_BLACK);
        vga_puts(HAVENDOS_VENDOR "\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
    }

    else if(!strcmp(cmd,"cls")||!strcmp(cmd,"clear")){
        clear_anim();
    }

    else if(!strcmp(cmd,"run")){
        if(!arg1[0]){vga_puts("Usage: run <script.bsh>\n");return;}
        shell_run_script(arg1);
    }

    /* ---- PS1 COMMAND ---- */
    else if(!strcmp(cmd,"ps1")){
        if(!arg1[0]){
            vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
            vga_puts("Current PS1: ");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            vga_puts(ps1_format); vga_putchar('\n');
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_puts("Tokens: \\u=user  \\h=host  \\w=path  \\$=$/# \\t=time  \\d=date  \\n=newline\n");
            vga_puts("Example: ps1 \\u@\\h:\\w\\$ \n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
        } else {
            /* Accept arg1 + rest of line */
            char newps1[64];
            strncpy(newps1,arg1,31);
            if(arg2[0]){ strncat(newps1," ",63); strncat(newps1,arg2,63); }
            if(!strcmp(newps1,"reset")) strcpy(newps1,"\\u@havendos:\\w\\$ ");
            strncpy(ps1_format,newps1,63);
            { extern void cfg_set_prompt(const char*); extern void cfg_save(void);
              cfg_set_prompt(ps1_format); cfg_save(); }
            vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
            vga_puts("Prompt updated and saved. 'ps1 reset' restores the default.\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
        }
    }

    /* ---- FORTUNE ---- */
    else if(!strcmp(cmd,"fortune")){
        cmd_fortune();
    }

    /* ---- BANNER ---- */
    else if(!strcmp(cmd,"banner")){
        /* Combine arg1 + arg2 for multi-word banner */
        char btxt[32];
        strncpy(btxt,arg1,15);
        if(arg2[0]){ strncat(btxt," ",31); strncat(btxt,arg2,15); }
        cmd_banner(btxt);
    }

    else if(!strcmp(cmd,"ls")||!strcmp(cmd,"dir")){
        if(pipe_mode) vfs_list_dir(shell_cwd, ls_pipe_callback);
        else {
            /* color legend header */
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            /* show current path */
            vga_puts(shell_cwd[0] ? shell_cwd : "/");
            vga_puts("   ");
            vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);  vga_puts("DIR ");
            vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK); vga_puts("FILE ");
            vga_set_color(VGA_LIGHT_RED,VGA_BLACK);   vga_puts("LOCKED");
            vga_putchar('\n');
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);

            vga_putchar('\n');
            vfs_list_dir(shell_cwd, ls_callback);
            vga_set_color(VGA_WHITE,VGA_BLACK);
        }
    }

    else if(!strcmp(cmd,"cat")||!strcmp(cmd,"type")){
        if(!arg1[0]){vga_puts("Usage: cat <file>\n");return;}
        char fp[64]; make_path(fp,arg1);
        char buf[4096];int r=vfs_read(fp,buf,4095);
        if(r<0){vga_printf("Not found: %s\n",fp);return;}
        buf[r]=0;
        out_str(buf); if(!pipe_mode) vga_putchar('\n');
    }

    else if(!strcmp(cmd,"touch")){
        if(!arg1[0]){vga_puts("Usage: touch <file>\n");return;}
        char fp[64]; make_path(fp,arg1);
        if(vfs_exists(fp)) vga_printf("Exists: %s\n",fp);
        else{vfs_write(fp,"",0);vga_printf("Created: %s\n",fp);}
    }

    else if(!strcmp(cmd,"head")){
        int n=10; const char *fname=arg1;
        if(arg1[0]=='-'){n=atoi(arg1+1);if(n<=0)n=10;fname=arg2;}
        else if(arg2[0]){int t=atoi(arg2);if(t>0)n=t;}
        if(!fname||!fname[0]){vga_puts("Usage: head [-n] <file>\n");return;}
        char fp[64]; make_path(fp,fname);
        char buf[4096];int r=vfs_read(fp,buf,4095);
        if(r<0){vga_printf("Not found: %s\n",fp);return;}
        buf[r]=0; print_lines_out(buf,r,n,0);
    }

    else if(!strcmp(cmd,"tail")){
        int n=10; const char *fname=arg1;
        if(arg1[0]=='-'){n=atoi(arg1+1);if(n<=0)n=10;fname=arg2;}
        else if(arg2[0]){int t=atoi(arg2);if(t>0)n=t;}
        if(!fname||!fname[0]){vga_puts("Usage: tail [-n] <file>\n");return;}
        char fp[64]; make_path(fp,fname);
        char buf[4096];int r=vfs_read(fp,buf,4095);
        if(r<0){vga_printf("Not found: %s\n",fp);return;}
        buf[r]=0; print_lines_out(buf,r,n,1);
    }

    else if(!strcmp(cmd,"wc")){
        if(!arg1[0]){vga_puts("Usage: wc <file>\n");return;}
        char fp[64]; make_path(fp,arg1);
        char buf[4096];int r=vfs_read(fp,buf,4095);
        if(r<0){vga_printf("Not found: %s\n",fp);return;}
        int l,w,c; buf_wc(buf,r,&l,&w,&c);
        if(pipe_mode){
            out_str(fp);out_char(' ');
            out_int(l);out_char(' ');out_int(w);out_char(' ');out_int(c);out_char('\n');
        } else {
            vga_printf("  Lines: %d   Words: %d   Chars: %d   File: %s\n",l,w,c,fp);
        }
    }

    else if(!strcmp(cmd,"sort")){
        if(!arg1[0]){vga_puts("Usage: sort [-r] <file>\n");return;}
        int rev=0; const char *fname=arg1;
        if(!strcmp(arg1,"-r")){rev=1;fname=arg2;}
        char fp[64]; make_path(fp,fname);
        char buf[4096];int r=vfs_read(fp,buf,4095);
        if(r<0){vga_printf("Not found: %s\n",fp);return;}
        sort_buf(buf,r,rev,0);
    }

    else if(!strcmp(cmd,"uniq")){
        if(!arg1[0]){vga_puts("Usage: uniq <file>\n");return;}
        char fp[64]; make_path(fp,arg1);
        char buf[4096];int r=vfs_read(fp,buf,4095);
        if(r<0){vga_printf("Not found: %s\n",fp);return;}
        sort_buf(buf,r,0,1);
    }

    else if(!strcmp(cmd,"find")){
        if(!arg1[0]){vga_puts("Usage: find <pattern>  (* glob ok)\n");return;}
        do_find(arg1);
    }

    else if(!strcmp(cmd,"diff")){
        if(!arg1[0]||!arg2[0]){vga_puts("Usage: diff <f1> <f2>\n");return;}
        char fp1[64],fp2[64]; make_path(fp1,arg1); make_path(fp2,arg2);
        do_diff(fp1,fp2);
    }

    else if(!strcmp(cmd,"tee")){
        if(!arg1[0]){vga_puts("Usage: tee <file>\n");return;}
        char fp[64]; make_path(fp,arg1);
        if(pipe_buf_len>0) do_tee(fp,pipe_buf,pipe_buf_len,0);
        else vga_puts("tee: no input (use with pipe)\n");
    }

    else if(!strcmp(cmd,"grep")){
        /* v0.7.11: strlen(arg1) was recomputed for every character of every
           line scanned - O(n*m) in the pattern length for no reason. */
        const int plen_cached=(int)strlen(arg1);
        if(!arg1[0]||!arg2[0]){vga_puts("Usage: grep <pattern> <file>\n");return;}
        char fp[64]; make_path(fp,arg2);
        char gbuf[4096];int gr=vfs_read(fp,gbuf,4095);
        if(gr<0){vga_printf("Not found: %s\n",fp);return;}
        gbuf[gr]=0;
        char *ln=gbuf;int found=0,lnum=1;
        while(*ln){
            char *end=ln;while(*end&&*end!='\n')end++;
            char saved=*end;*end=0;
            char *p2=ln;
            while(*p2){
                if(strncmp(p2,arg1,(size_t)plen_cached)==0){
                    if(pipe_mode){out_str(ln);out_char('\n');}
                    else{
                        vga_set_color(VGA_YELLOW,VGA_BLACK);
                        vga_printf("%d: ",lnum);
                        vga_set_color(VGA_WHITE,VGA_BLACK);
                        vga_puts(ln);vga_putchar('\n');
                    }
                    found=1;break;
                }
                p2++;
            }
            *end=saved;
            ln=end+(*end?1:0); lnum++;
        }
        if(!found&&!pipe_mode) vga_printf("No matches for '%s'\n",arg1);
    }

    else if(!strcmp(cmd,"write")){
        if(!arg1[0]||!arg2[0]){vga_puts("Usage: write <file> <text>\n");return;}
        char fp[64]; make_path(fp,arg1);
        vfs_write(fp,arg2,strlen(arg2));
        vga_printf("Written to %s (%u bytes)\n",fp,(uint32_t)strlen(arg2));
    }

    else if(!strcmp(cmd,"mkdir")){
        if(!arg1[0]){vga_puts("Usage: mkdir <name>\n");return;}
        char fp[64]; make_path(fp,arg1);
        vfs_mkdir(fp)<0?vga_printf("Failed: %s\n",fp):vga_printf("Created: %s\n",fp);
    }

    else if(!strcmp(cmd,"cd")){
        if(!arg1[0]||!strcmp(arg1,"~")){
            strncpy(shell_cwd,shell_home,63); shell_cwd[63]=0;
        } else if(!strcmp(arg1,"/")){
            shell_cwd[0]=0;

        } else if(!strcmp(arg1,"..")){
            if(shell_cwd[0]){
                int l=(int)strlen(shell_cwd);
                while(l>0&&shell_cwd[l-1]!='/') l--;
                if(l>0) l--;
                shell_cwd[l]=0;
            }
        } else {
            char target[64]; make_path(target,arg1);
            if(vfs_is_dir(target)){
                strncpy(shell_cwd,target,63); shell_cwd[63]=0;
            } else if(vfs_exists(target)){
                vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
                vga_puts("  cd: not a directory: "); vga_puts(arg1); vga_putchar('\n');
                vga_set_color(VGA_WHITE,VGA_BLACK);
            } else {
                vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
                vga_puts("  cd: no such directory: "); vga_puts(arg1); vga_putchar('\n');
                vga_set_color(VGA_WHITE,VGA_BLACK);
            }
        }
    }

    else if(!strcmp(cmd,"pwd")){
        vga_puts(shell_cwd[0] ? shell_cwd : "/");
        vga_putchar('\n');
    }

    else if(!strcmp(cmd,"del")||!strcmp(cmd,"rm")){
        if(!arg1[0]){vga_puts("Usage: del <file>\n");return;}
        char fp[64]; make_path(fp,arg1);
        vfs_delete(fp)<0?vga_printf("Not found: %s\n",fp):vga_printf("Deleted: %s\n",fp);
    }

    else if(!strcmp(cmd,"rename")||!strcmp(cmd,"mv")){
        if(!arg1[0]||!arg2[0]){vga_puts("Usage: rename <old> <new>\n");return;}
        char fp1[64],fp2[64]; make_path(fp1,arg1); make_path(fp2,arg2);
        vfs_rename(fp1,fp2)<0?vga_puts("Failed\n"):vga_printf("%s -> %s\n",fp1,fp2);
    }

    else if(!strcmp(cmd,"cp")||!strcmp(cmd,"copy")){
        if(!arg1[0]||!arg2[0]){vga_puts("Usage: cp <src> <dst>\n");return;}
        char fp1[64],fp2[64]; make_path(fp1,arg1); make_path(fp2,arg2);
        char cbuf[4096];int cr=vfs_read(fp1,cbuf,4095);
        if(cr<0){vga_printf("Not found: %s\n",fp1);return;}
        cbuf[cr]=0;vfs_write(fp2,cbuf,cr);
        vga_printf("Copied %s -> %s (%d bytes)\n",fp1,fp2,cr);
    }

    else if(!strcmp(cmd,"mem")){
        uint32_t f=pmm_free_blocks(),t=pmm_total_blocks();
        if(pipe_mode){
            out_str("Free:");out_uint(f*4);out_str("KB Total:");out_uint(t*4);out_str("KB\n");
        } else {
            vga_printf("Free: %u KB  Used: %u KB  Total: %u KB\n",f*4,(t-f)*4,t*4);
        }
    }

    else if(!strcmp(cmd,"uptime")){
        uint32_t t=get_ticks()/100;
        vga_printf("Uptime: %u:%02u:%02u\n",t/3600,(t%3600)/60,t%60);
    }

    else if(!strcmp(cmd,"disk")){
        extern int         virtio_blk_ready(void);
        extern uint64_t    virtio_blk_sectors(void);
        extern const char *virtio_blk_log(void);
        extern int         fat16_detected(void);
        extern int         ata_present(void);
        int vready=virtio_blk_ready();
        /* v0.9.3: "a disk is mounted and it is not FAT16" meant FAT12 until
           there was a third kind. On a FAT32 install this printed "ATA
           FAT12 : present" on an NVMe laptop that has no ATA at all. */
        extern int vfs_using_fat32(void);
        int has_ata=vfs_using_disk() && !vfs_using_fat16() && !vfs_using_fat32();
        extern int boot_is_live(void);
        int has_fat16=fat16_detected();
        /* fat16_on_ata() is gone from this screen: it only ever meant
           "not VirtIO", and the two lines that used it now ask
           blk_bus_name() for the bus the volume is actually on. */
        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts("Storage status:\n");
        if(boot_is_live()){
            vga_set_color(VGA_YELLOW,VGA_BLACK);
            /* v0.8.6: this said "no disk is probed, mounted or
               written", which stopped being true when USB mass storage
               started being probed on live media -- a stick is not the
               machine, and it is the reason somebody boots the ISO. The
               guarantee live mode actually makes is about the disks that
               were in the laptop already, so that is what it says. */
            vga_puts("  LIVE SESSION - this machine's own disks are not "
                     "probed, mounted or\n"
                     "                 written. A USB stick you plug in "
                     "is yours and is listed.\n");
            vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        }
        /* NVMe, and what is actually on it.  Read-only, and said so plainly:
           someone looking at their own Windows disk listed here should be
           able to see at a glance that HavenDOS cannot write to it. */
        {
            extern int         nvme_count(void);
            extern uint64_t    nvme_sectors(int);
            extern const char *nvme_model(int);
            extern int         inst_disk_scan(void);
            extern int         inst_disk_is_nvme(int);
            extern int         inst_disk_has_gpt(int);
            extern int         inst_disk_count(void);
            int nn = nvme_count();
            if(nn <= 0){
                vga_puts("  NVMe       : not found\n");
            } else {
                for(int i = 0; i < nn; i++){
                    vga_puts("  NVMe ns");
                    { char n[4]; n[0]=(char)('0'+i); n[1]=0; vga_puts(n); }
                    vga_puts("   : ");
                    vga_puts(nvme_model(i));
                    vga_puts("  ");
                    { char mb[16]; utoa((uint32_t)(nvme_sectors(i)>>11),mb,10);
                      vga_puts(mb); vga_puts(" MB"); }
                    /* This used to be the literal string "[read-only]",
                       printed for every namespace whether or not it was.
                       It dated from before nvme_write() existed and it told
                       every owner of a real NVMe machine that their disk
                       could not be written -- on the one screen they would
                       check before trying. Ask the driver. */
                    if(nvme_writable(i)){
                        vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
                        vga_puts("  [writable]\n");
                    } else {
                        vga_set_color(VGA_YELLOW,VGA_BLACK);
                        vga_puts("  [write protected by the namespace]\n");
                    }
                    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
                }
                /* The partition table, if there is one.
                   This used to read "install refuses this disk (it would
                   destroy the table)", which was true only of the whole-disk
                   installer. The additive installer KEEPS the table -- that
                   is the entire point of it -- so the old line told every
                   dual-boot machine the opposite of the truth. */
                int nd = inst_disk_scan();
                for(int i = 0; i < nd; i++){
                    if(!inst_disk_is_nvme(i)) continue;
                    if(inst_disk_has_gpt(i)){
                        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
                        vga_puts("  Partitions : GPT - kept. HavenDOS "
                                 "installs into free space beside it.\n");
                        vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
                    } else {
                        vga_puts("  Partitions : no GPT found\n");
                    }
                }
            }
        }
        /* VirtIO */
        if(vready){
            uint64_t secs=virtio_blk_sectors();
            uint32_t mb=(uint32_t)(secs/2048);
            char mb_s[12]; utoa(mb,mb_s,10);
            vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
            vga_puts("  VirtIO BLK : present  ");
            vga_puts(mb_s); vga_puts("MB  (UTM SE VID:1AF4 DID:1001)\n");
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_puts("  Driver log : ");
            vga_puts(virtio_blk_log()); vga_puts("\n");
        } else {
            vga_set_color(VGA_YELLOW,VGA_BLACK);
            vga_puts("  VirtIO BLK : not found\n");
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            /* Only a machine with no disk at all needs telling to attach one.
               Saying it on a machine running off IDE reads as a fault. */
            /* Only when there is genuinely no disk on ANY bus. It used to
               fire whenever VirtIO was absent and no FAT16/ATA was found,
               which on an NVMe laptop printed "attach a drive" directly
               under the drive it had just identified by model name. */
            /* v0.8.6: through fs/blkdev.c, which knows every bus. This
               listed four by name and would have had to learn a fifth --
               which is the shape of the bug that cost four releases. */
            { extern int blk_count(void);
              extern int blk_scan(void);
              if(!has_fat16 && !has_ata && blk_count()==0 && blk_scan()==0)
                  vga_puts("  (no disk on any bus)\n"); }
        }
        /* ── USB disks, v0.8.6 ────────────────────────────────────────
           Listed separately from the fixed disks because they are a
           different thing to a person: a stick is something they put in
           and take out. fs/blkdev.c registers them after every fixed
           disk so one cannot renumber the disk the installer is about to
           write to, and this screen says which is which. */
        {
            extern int         msc_count(void);
            extern uint64_t    msc_sectors(int);
            extern int         msc_readonly(int);
            extern int         msc_removable(int);
            extern uint32_t    msc_blocksize(int);
            extern const char *msc_vendor(int);
            extern const char *msc_product(int);
            int nu = msc_count();
            if(nu <= 0){
                vga_set_color(VGA_DARK_GREY,VGA_BLACK);
                vga_puts("  USB disks  : none\n");
            } else {
                for(int i = 0; i < nu; i++){
                    vga_set_color(VGA_LIGHT_MAGENTA,VGA_BLACK);
                    vga_puts("  USB disk ");
                    { char n[4]; n[0]=(char)('0'+i); n[1]=0; vga_puts(n); }
                    vga_puts(" : ");
                    vga_puts(msc_vendor(i));
                    vga_puts(" ");
                    vga_puts(msc_product(i));
                    vga_puts("  ");
                    { char mb[16];
                      utoa((uint32_t)(msc_sectors(i)/2048),mb,10);
                      vga_puts(mb); vga_puts(" MB"); }
                    if(msc_readonly(i)){
                        vga_set_color(VGA_YELLOW,VGA_BLACK);
                        /* WHY, not just that it is. A stick is read-only
                           for three different reasons and only one of
                           them is something the person can change. */
                        if(msc_blocksize(i) != 512){
                            vga_puts("  [read-only: ");
                            { char b[12];
                              utoa(msc_blocksize(i),b,10); vga_puts(b); }
                            vga_puts("-byte blocks]\n");
                        } else {
                            vga_puts("  [write protected]\n");
                        }
                    } else {
                        vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
                        vga_puts(msc_removable(i) ? "  [writable, removable]\n"
                                                  : "  [writable]\n");
                    }
                    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
                }
                vga_set_color(VGA_DARK_GREY,VGA_BLACK);
                vga_puts("  A stick plugged in after boot needs "
                         "'usb rescan'.\n");
            }
            vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        }
        /* FAT16, on whichever bus it was actually found */
        vga_set_color(has_fat16?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);
        /* v0.8.6: this line said "ATA/IDE" or "VirtIO", from
           fat16_on_ata(), which only ever meant "not VirtIO". So an
           installed system on an NVMe laptop -- the machine this is
           developed on -- reported its own volume as mounted on ATA/IDE,
           on the screen somebody checks to find out where their disk is.
           The fourth place this exact bug has been found, and the first
           one where the fix is to ASK: fs/blkdev.c knows which disk the
           volume is on and what that bus is called. */
        if(has_fat16){
            extern int         fat16_dev(void);
            extern const char *blk_bus_name(int);
            vga_puts("  FAT16 part : mounted on ");
            vga_puts(blk_bus_name(fat16_dev()));
            vga_puts(" (persistent)\n");
        } else {
            vga_puts("  FAT16 part : not mounted\n");
        }
        /* FAT32, when that is what got mounted.  The numbers come from the
           driver, which read them off the volume a moment ago. */
        {
            extern int      vfs_using_fat32(void);
            extern uint32_t fat32_total_clusters(void);
            extern uint32_t fat32_free_clusters(void);
            extern uint32_t fat32_cluster_bytes(void);
            extern int  fat32_sys_begin(void);
            extern void fat32_sys_end(void);
            extern int  blk_boot_dev(void);
            extern const char *blk_bus_name(int);
            int has_fat32 = vfs_using_fat32();
            vga_set_color(has_fat32?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);
            /* The system volume's numbers, even if `fat32` is holding a
               stick open: the driver lends the slot back for this. */
            if(has_fat32 && fat32_sys_begin()){
                char cl[12], kb[12];
                utoa(fat32_total_clusters(),cl,10);
                utoa(fat32_cluster_bytes()/1024,kb,10);
                vga_puts("  FAT32 part : mounted on ");
                vga_puts(blk_bus_name(blk_boot_dev()));
                vga_puts(", ");
                vga_puts(cl); vga_puts(" clusters of ");
                vga_puts(kb); vga_puts("KB\n");
                uint32_t fr = fat32_free_clusters();
                vga_set_color(VGA_DARK_GREY,VGA_BLACK);
                if(fr == 0xFFFFFFFFu){
                    /* FSInfo says it does not know, so neither do we.  A
                       number invented here would be the same class of thing
                       as a boot log that describes machine state in a
                       string literal. */
                    vga_puts("  Free space : unknown (FSInfo carries no count)\n");
                } else {
                    char mb_s[12];
                    utoa((uint32_t)(((uint64_t)fr*fat32_cluster_bytes())>>20),mb_s,10);
                    vga_puts("  Free space : "); vga_puts(mb_s); vga_puts("MB\n");
                }
                fat32_sys_end();
            } else {
                vga_puts("  FAT32 part : not mounted\n");
            }
        }
        /* ATA FAT12, the floppy-style fallback */
        vga_set_color(has_ata?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);
        vga_puts(has_ata?"  ATA FAT12 : present\n":"  ATA FAT12 : not present\n");
        /* Filesystem */
        vga_set_color(VGA_WHITE,VGA_BLACK);
        {
            extern int vfs_using_fat32(void);
            if(has_fat16){
                extern int         fat16_dev(void);
                extern const char *blk_bus_name(int);
                vga_puts("  Active FS  : FAT16 on ");
                vga_puts(blk_bus_name(fat16_dev()));
                vga_puts(" (persistent across reboots)\n");
            }
            else if(vfs_using_fat32())
                               vga_puts("  Active FS  : FAT32 (persistent across reboots)\n");
            else if(has_ata)   vga_puts("  Active FS  : FAT12 on ATA (persistent)\n");
            else               vga_puts("  Active FS  : RAMFS (volatile in-memory)\n");
        }
        vga_set_color(VGA_DARK_GREY,VGA_BLACK);
        if(boot_is_live())
            vga_puts("  Tip        : this is live media; reboot and pick "
                     "'Install to Disk' to\n"
                     "               set up persistent storage\n");
        else if(!vfs_using_disk())
            vga_puts("  Tip        : run 'install' to set up persistent storage\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
    }

    else if(!strcmp(cmd,"install")){
        /* One installer, reached two ways.  The copy that used to live here
           scanned VirtIO only and told anyone with an IDE disk to attach one,
           which is exactly the disk it was standing on. */
        extern void installer_run(void);
        installer_run();
        vga_clear();
        vga_set_cursor(0,0);
        vga_set_color(VGA_WHITE, VGA_BLACK);
    }

    else if(!strcmp(cmd,"migrate")){
        /* v0.9.6: FAT16 -> FAT32, on the same disk. */
        extern void cmd_migrate(const char *);
        cmd_migrate(arg1);
    }

    else if(!strcmp(cmd,"uninstall")){
        /* The opposite of `install`, and held to the same standard:
           a preview from a real dry run, then a typed word. */
        extern void cmd_uninstall(void);
        cmd_uninstall();
    }

    else if(!strcmp(cmd,"update")){
        cmd_update();
    }

    else if(!strcmp(cmd,"rollback")){
        cmd_rollback();
    }

    else if(!strcmp(cmd,"bpkg")){
        extern void bpkg_shell_cmd(const char*);
        /* Reconstruct full args string */
        char bpkg_args[64]="";
        if(arg1[0]){
            strncpy(bpkg_args,arg1,32);
            if(arg2[0]){
                strncat(bpkg_args," ",sizeof(bpkg_args)-1-strlen(bpkg_args));
                strncat(bpkg_args,arg2,sizeof(bpkg_args)-1-strlen(bpkg_args));
            }
        }
        bpkg_shell_cmd(bpkg_args);
    }

    else if(!strcmp(cmd,"boot")||!strcmp(cmd,"h")||!strcmp(cmd,"hplusplus")){
        extern int bpkg_is_installed(const char*);
        extern int boot_run_file(const char*);
        extern void boot_repl(void);
        int hlang = bpkg_is_installed("h-lang") || bpkg_is_installed("boot-lang");
        if(!hlang){
            vga_puts("h: H language not installed.\n");
            vga_puts("Run: bpkg install h-lang\n");
        } else if(arg1[0]){
            boot_run_file(arg1);
        } else {
            boot_repl();
        }
    }

    else if(!strcmp(cmd,"theme")){
        extern void cmd_theme(const char*);
        cmd_theme(arg1);
    }

    else if(!strcmp(cmd,"whoami"))
        vga_printf("%s [%s]\n",current_username(),current_user_admin()?"admin":"user");

    else if(!strcmp(cmd,"users")){
        extern void cmd_users(const char*);
        cmd_users(arg1);
    }

    else if(!strcmp(cmd,"passwd")){
        char target[16];
        if(arg1[0]) strncpy(target,arg1,15); else strncpy(target,current_username(),15);
        if(strcmp(target,current_username())!=0&&!current_user_admin()){vga_puts("Permission denied.\n");return;}
        char oldp[16]="",newp[16]="",newp2[16]="";
        if(!current_user_admin()){
            vga_puts("Current password: ");
            int pi=0;
            while(1){int c=keyboard_waitchar();if(c=='\n'||c=='\r')break;
                if(c=='\b'&&pi>0){pi--;oldp[pi]=0;vga_putchar('\b');}
                else if(c>=32&&c<127&&pi<15){oldp[pi++]=c;oldp[pi]=0;vga_putchar('*');}
            }vga_putchar('\n');
        }
        vga_puts("New password: ");
        {int pi=0;while(1){int c=keyboard_waitchar();if(c=='\n'||c=='\r')break;
            if(c=='\b'&&pi>0){pi--;newp[pi]=0;vga_putchar('\b');}
            else if(c>=32&&c<127&&pi<15){newp[pi++]=c;newp[pi]=0;vga_putchar('*');}
        }vga_putchar('\n');}
        vga_puts("Confirm: ");
        {int pi=0;while(1){int c=keyboard_waitchar();if(c=='\n'||c=='\r')break;
            if(c=='\b'&&pi>0){pi--;newp2[pi]=0;vga_putchar('\b');}
            else if(c>=32&&c<127&&pi<15){newp2[pi++]=c;newp2[pi]=0;vga_putchar('*');}
        }vga_putchar('\n');}
        if(strcmp(newp,newp2)!=0){vga_puts("Passwords don't match.\n");return;}
        int r=passwd_user(target,oldp,newp);
        if(r==0)vga_puts("Password changed.\n");
        else if(r==-2)vga_puts("Wrong current password.\n");
        else vga_puts("User not found.\n");
    }

    else if(!strcmp(cmd,"deluser")){
        if(!current_user_admin()){vga_puts("Admin only.\n");return;}
        if(!arg1[0]){vga_puts("Usage: deluser <username>\n");return;}
        int r=delete_user(arg1);
        if(r==0)vga_printf("Deleted: %s\n",arg1);
        else if(r==-2)vga_puts("Permission denied.\n");
        else if(r==-3)vga_puts("Cannot delete root.\n");
        else vga_puts("User not found.\n");
    }

    else if(!strcmp(cmd,"chmod")){
        if(!current_user_admin()){vga_puts("Admin only.\n");return;}
        if(!arg1[0]||!arg2[0]){vga_puts("Usage: chmod <file> <lock|unlock>\n");return;}
        if(vfs_using_disk()){
            vga_puts("chmod: file locks live in the RAM filesystem of a live session.\n"
                     "FAT16 and FAT32 have nowhere to keep them, so nothing changed.\n");
            return;
        }
        int lock=(!strcmp(arg2,"lock"))?1:0;
        int r=ramfs_chmod(arg1,lock);
        if(r==0)vga_printf("%s %s.\n",arg1,lock?"locked":"unlocked");
        else vga_puts("File not found.\n");
    }

    else if(!strcmp(cmd,"chown")){
        if(!current_user_admin()){vga_puts("Admin only.\n");return;}
        if(!arg1[0]||!arg2[0]){vga_puts("Usage: chown <file> <user>\n");return;}
        if(vfs_using_disk()){
            vga_puts("chown: owners live in the RAM filesystem of a live session.\n"
                     "FAT16 and FAT32 have nowhere to keep them, so nothing changed.\n");
            return;
        }
        int r=ramfs_chown(arg1,arg2);
        if(r==0)vga_printf("%s owned by %s.\n",arg1,arg2);
        else vga_puts("File not found.\n");
    }

    else if(!strcmp(cmd,"log")){
        extern void cmd_loginlog(void);
        cmd_loginlog();
    }
    else if(!strcmp(cmd,"set")||!strcmp(cmd,"export")){
        if(!arg1[0]){vga_puts("Usage: set <key> <value>\n");return;}
        /* If value is a pure arithmetic expression, evaluate it.
           This lets BSH loops do: set i $i+1  (after $VAR expansion
           becomes "set i 4+1" -> stored as "5") */
        if(looks_arith(arg2)){
            char res[16]; itoa(do_calc(arg2),res,10);
            env_set(arg1,res);
            vga_printf("  %s = %s\n",arg1,res);
        } else {
            env_set(arg1,arg2);
            vga_printf("  %s = %s\n",arg1,arg2);
        }
    }

    else if(!strcmp(cmd,"unset")){
        if(!arg1[0]){vga_puts("Usage: unset <key>\n");return;}
        int found=0;
        for(int i=0;i<env_count;i++){
            if(strcmp(env_keys[i],arg1)==0){
                for(int j=i;j<env_count-1;j++){
                    strncpy(env_keys[j],env_keys[j+1],15);
                    strncpy(env_vals[j],env_vals[j+1],63);
                }
                env_count--; found=1; break;
            }
        }
        found?vga_printf("Unset: %s\n",arg1):vga_printf("No var: %s\n",arg1);
    }

    else if(!strcmp(cmd,"env")){
        if(env_count==0){vga_puts("(no env vars)\n");return;}
        for(int i=0;i<env_count;i++) vga_printf("  %s=%s\n",env_keys[i],env_vals[i]);
    }

    else if(!strcmp(cmd,"alias")){
        if(!arg1[0]){
            if(alias_count==0)vga_puts("(no aliases)\n");
            for(int i=0;i<alias_count;i++) vga_printf("  %s='%s'\n",alias_keys[i],alias_vals[i]);
        } else {
            char k[16]="",v[64]="";
            char *eq=strchr(arg1,'=');
            if(eq){
                int kl=(int)(eq-arg1);if(kl>15)kl=15;
                strncpy(k,arg1,kl);k[kl]=0;
                strncpy(v,eq+1,63);
                if(arg2[0]){strncat(v," ",63);strncat(v,arg2,63);}
            } else {
                strncpy(k,arg1,15);strncpy(v,arg2,63);
            }
            if(!k[0]||!v[0]){vga_puts("Usage: alias name=value\n");return;}
            alias_set(k,v);alias_save();
            vga_printf("  %s='%s'  (saved)\n",k,v);
        }
    }

    else if(!strcmp(cmd,"unalias")){
        if(!arg1[0]){vga_puts("Usage: unalias <name>\n");return;}
        if(alias_del(arg1)==0){alias_save();vga_printf("Removed: %s\n",arg1);}
        else vga_printf("No alias: %s\n",arg1);
    }

    else if(!strcmp(cmd,"echo")){
        out_str(arg1);
        if(arg2[0]){out_char(' ');out_str(arg2);}
        out_char('\n');
    }

    else if(!strcmp(cmd,"calc")){
        if(arg1[0]){
            char expr[64]; strncpy(expr,arg1,63);
            if(arg2[0]){strncat(expr," ",63);strncat(expr,arg2,63);}
            int result=do_calc(expr);
            if(calc_left)
                vga_printf("calc: cannot read '%s' -- supports + - * / %% ^ ( )\n",calc_left);
            else
                vga_printf("%s = %d\n",expr,result);
        } else {
            vga_clear(); toast_clear();
            for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_CYAN);
            vga_puts_at(" HavenDOS Calculator  |  type expression, 'q' to quit ",
                        13,0,VGA_BLACK,VGA_CYAN);
            vga_puts_at("Supports: + - * / % ^ ( )",2,2,VGA_DARK_GREY,VGA_BLACK);
            vga_puts_at("Example:  3 + 4 * (2 - 1)",2,3,VGA_DARK_GREY,VGA_BLACK);
            vga_set_color(VGA_WHITE,VGA_BLACK);
            char cbuf[64]; int cy=5;
            while(1){
                vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
                vga_puts_at("> ",2,cy,VGA_LIGHT_GREEN,VGA_BLACK);
                vga_set_color(VGA_WHITE,VGA_BLACK);
                shell_read_line(cbuf,63,"");
                if(!cbuf[0]) continue;
                if(cbuf[0]=='q'&&!cbuf[1]) break;
                int r=do_calc(cbuf);
                vga_printf("  = %d\n",r);
                cy+=2; if(cy>=23){cy=5; vga_clear(); toast_clear();
                    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_CYAN);
                    vga_puts_at(" HavenDOS Calculator  |  type expression, 'q' to quit ",
                                13,0,VGA_BLACK,VGA_CYAN);
                }
            }
            vga_clear(); toast_clear();
        }
    }

    else if(!strcmp(cmd,"dmesg")) cmd_dmesg_pager();
    else if(!strcmp(cmd,"ps")||!strcmp(cmd,"jobs")){
        /* v0.9.6: real now. 0.9.5 replaced three invented processes with
           the truth -- one program at a time -- and 0.9.6 changed the
           truth. shell/taskswitch.c prints the task table. */
        extern void cmd_ps(void);
        cmd_ps();
    }
    else if(!strcmp(cmd,"fg")){
        extern void cmd_fg(const char *);
        cmd_fg(arg1);
    }
    else if(!strcmp(cmd,"kill")){
        extern void cmd_kill(const char *);
        cmd_kill(arg1);
    }

    else if(!strcmp(cmd,"repeat")){
        if(!arg1[0]||!arg2[0]){vga_puts("Usage: repeat <n> <command>\n");return;}
        int n=atoi(arg1);if(n<=0||n>64){vga_puts("n must be 1-64\n");return;}
        for(int ri=0;ri<n;ri++){
            char rline[128]; strncpy(rline,arg2,127);
            run_pipeline(rline);
        }
    }

    else if(!strcmp(cmd,"seq")){
        if(!arg1[0]){vga_puts("Usage: seq <from> <to>\n");return;}
        int from_n=atoi(arg1);
        int to_n=arg2[0]?atoi(arg2):from_n;
        if(!arg2[0]){to_n=from_n;from_n=1;}
        if(to_n-from_n>200){vga_puts("Range too large (max 200)\n");return;}
        int step=(to_n>=from_n)?1:-1;
        for(int si=from_n;step>0?si<=to_n:si>=to_n;si+=step){
            out_int(si);out_char('\n');
        }
    }

    else if(!strcmp(cmd,"history")){
        int start=(hist_count>MAX_HIST)?hist_count-MAX_HIST:0;
        for(int hi=start;hi<hist_count;hi++)
            vga_printf("  %3d  %s\n",hi+1,history[hi%MAX_HIST]);
    }

    else if(!strcmp(cmd,"beep")){
        uint32_t freq=arg1[0]?(uint32_t)atoi(arg1):440u;
        uint32_t dur=arg2[0]?(uint32_t)atoi(arg2):200u;
        speaker_beep(freq,dur);
        vga_printf("Beep: %uHz %ums\n",freq,dur);
    }

    else if(!strcmp(cmd,"play")) music_boot_jingle();
    else if(!strcmp(cmd,"matrix")){screensaver_run_matrix();vga_clear(); toast_clear();}
    else if(!strcmp(cmd,"stars")){screensaver_run_stars();vga_clear(); toast_clear();}
    else if(!strcmp(cmd,"pipes")){screensaver_run_pipes();vga_clear(); toast_clear();}
    else if(!strcmp(cmd,"snake")){run_snake();vga_clear(); toast_clear();}

    /* ── Package apps (bpkg packages with built-in content) ────── */
    else if(!strcmp(cmd,"fortune"))     { extern void run_package_app(const char*,const char*); run_package_app("fortune",""); }
    else if(!strcmp(cmd,"ascii-art"))   { extern void run_package_app(const char*,const char*); run_package_app("ascii-art",arg1[0]?arg1:arg2); }
    else if(!strcmp(cmd,"paint"))       { extern void run_package_app(const char*,const char*); run_package_app("paint",""); }
    else if(!strcmp(cmd,"music"))       { extern void run_package_app(const char*,const char*); run_package_app("music",""); }
    else if(!strcmp(cmd,"calendar"))    { extern void run_package_app(const char*,const char*); run_package_app("calendar",""); }
    else if(!strcmp(cmd,"quiz"))        { extern void run_package_app(const char*,const char*); run_package_app("quiz",""); }
    else if(!strcmp(cmd,"hacker"))      { extern void run_package_app(const char*,const char*); run_package_app("hacker",""); }
    else if(!strcmp(cmd,"ifconfig")) cmd_ifconfig(arg1,arg2);
    else if(!strcmp(cmd,"ping"))     cmd_ping(arg1);
    else if(!strcmp(cmd,"dns"))      cmd_dns(arg1);
    else if(!strcmp(cmd,"wget"))     cmd_wget(arg1,arg2);
    else if(!strcmp(cmd,"curl"))     cmd_curl(arg1);
    else if(!strcmp(cmd,"udpsend"))  cmd_udpsend(arg1,arg2);
    else if(!strcmp(cmd,"netstat")){
        /* v0.7.6: -s prints the new stack counters, -z resets them. */
        if(!strcmp(arg1,"-s")||!strcmp(arg1,"stats")) cmd_netstat_stats_v6();
        else if(!strcmp(arg1,"-z")||!strcmp(arg1,"reset")) cmd_netstat_reset_v6();
        else cmd_netstat();
    }
    else if(!strcmp(cmd,"resolution")||!strcmp(cmd,"res")) cmd_resolution_v6(arg1);
    else if(!strcmp(cmd,"kpoll"))    cmd_kpoll_v6(arg1);
    else if(!strcmp(cmd,"nslookup")||!strcmp(cmd,"host")) cmd_nslookup_v6(arg1);
    else if(!strcmp(cmd,"lspci"))    cmd_lspci(arg1);
    else if(!strcmp(cmd,"usb"))      cmd_usb(arg1);
    /* v0.8.9.1: `lockdown` IS a command now. It never was -- the
       subcommand is `usb lockdown` -- and both hardware-test docs told
       people to type the bare word, so the first thing anybody
       following them saw was "command not found". Fixing the docs is
       necessary and not sufficient: the bare word is what a person
       types, it is unambiguous, and there is nothing else it could
       mean. */
    else if(!strcmp(cmd,"lockdown"))  cmd_usb("lockdown");
    else if(!strcmp(cmd,"fat32"))    cmd_fat32(arg1,arg2);
    else if(!strcmp(cmd,"fat16"))    cmd_fat16(arg1,arg2);
    else if(!strcmp(cmd,"exfat"))    cmd_exfat(arg1,arg2);
    else if(!strcmp(cmd,"rsa"))      cmd_rsa(arg1,arg2);
    else if(!strcmp(cmd,"i2c"))      cmd_i2c(arg1,arg2);
    else if(!strcmp(cmd,"i2chid"))   cmd_i2chid(arg1);
    else if(!strcmp(cmd,"gfx"))      cmd_gfx(arg1,arg2);
    else if(!strcmp(cmd,"pair"))     cmd_pair(arg1);
    else if(!strcmp(cmd,"crash"))    cmd_crash(arg1);
    else if(!strcmp(cmd,"layout"))   desktop_layout_audit();
    else if(!strcmp(cmd,"selftest")) cmd_selftest_v6(arg1);
    else if(!strcmp(cmd,"tlsinfo")||!strcmp(cmd,"tls")) cmd_tlsinfo_v6();
    else if(!strcmp(cmd,"stat"))     cmd_stat(arg1);
    else if(!strcmp(cmd,"hex"))      cmd_hex(arg1,arg2);
    else if(!strcmp(cmd,"tcptest"))  cmd_tcptest(arg1,arg2);
    else if(!strcmp(cmd,"tcpsend"))  cmd_tcpsend(arg1,arg2);
    else if(!strcmp(cmd,"chat"))     cmd_bootchat(arg1);
    else if(!strcmp(cmd,"motd"))     cmd_motd_v2(arg1,arg2);
    else if(!strcmp(cmd,"df"))       cmd_df_v2();
    else if(!strcmp(cmd,"neofetch")) cmd_neofetch_v2();
    else if(!strcmp(cmd,"screensaver")) cmd_screensaver_v2(arg1[0]?arg1:arg2);
    else if(!strcmp(cmd,"source")||(!strcmp(cmd,".")&&arg1[0])) cmd_source_v2(arg1[0]?arg1:arg2);
    else if(!strcmp(cmd,"watch"))    cmd_watch_v2(arg1,arg2);
    else if(!strcmp(cmd,"script"))   cmd_script_v2(arg1);
    else if(!strcmp(cmd,"timer"))    cmd_time_v2(arg1,arg2);
    else if(!strcmp(cmd,"yes"))      cmd_yes_v2(arg1);
    else if(!strcmp(cmd,"base64"))   cmd_base64_v3(arg1,arg2);
    else if(!strcmp(cmd,"hash"))     cmd_hash_v3(arg1,arg2);
    else if(!strcmp(cmd,"todo"))     cmd_todo_v3(arg1,arg2);
    else if(!strcmp(cmd,"lock"))     cmd_lock_v3();
    else if(!strcmp(cmd,"wc")&&arg1[0]) cmd_wc_v3(arg1);
    else if(!strcmp(cmd,"zip"))      cmd_zip_v3(arg1,arg2);
    else if(!strcmp(cmd,"unzip"))    cmd_unzip_v3(arg1,arg2);
    else if(!strcmp(cmd,"date"))     cmd_date();
    else if(!strcmp(cmd,"time"))     cmd_time_rtc();
    else if(!strcmp(cmd,"adduser"))  cmd_adduser(arg1,arg2);
    else if(!strcmp(cmd,"color"))    cmd_color(arg1,arg2);
    else if(!strcmp(cmd,"cowsay"))   cmd_cowsay(arg1[0]?arg1:arg2);
    else if(!strcmp(cmd,"man"))      cmd_man(arg1[0]?arg1:arg2);
    else if(!strcmp(cmd,"edit")||!strcmp(cmd,"notepad")){
        extern void shell_launch_editor(const char *fname);
        /* v0.9.4.1: resolved against the working directory like every
           other file command. `edit notes.txt` in HOME/kyan opened an
           empty buffer for /notes.txt and saved it there -- next to
           `cat notes.txt` showing the real file in the home directory. */
        if(arg1[0]){ char fp[64]; make_path(fp,arg1); shell_launch_editor(fp); }
        else shell_launch_editor(arg1);
    }

    else if(!strcmp(cmd,"notify")){
        if(!arg1[0]){vga_puts("Usage: notify <type> <msg>  (info/success/warn/error)\n");return;}
        const char *nmsg=arg2[0]?arg2:"Notification";
        if(!strcmp(arg1,"success"))      toast_success(nmsg);
        else if(!strcmp(arg1,"warn"))    toast_warning(nmsg);
        else if(!strcmp(arg1,"error"))   toast_error(nmsg);
        else                             toast_info(nmsg);
    }

    else if(!strcmp(cmd,"reboot")){
        vga_clear(); toast_clear();
        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts_at("  Rebooting HavenDOS...  ", 28, 12, VGA_BLACK, VGA_LIGHT_CYAN);
        vga_puts_at("Please wait.",             34, 14, VGA_DARK_GREY, VGA_BLACK);
        sleep_ms(800);
        machine_reset();
    }

    else if(!strcmp(cmd,"shutdown")||!strcmp(cmd,"poweroff")){
        vga_clear(); toast_clear();
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts_at("  Shutting down HavenDOS...  ", 26, 12, VGA_BLACK, VGA_YELLOW);
        /* "It is now safe to power off." used to be printed HERE, before
           anything was attempted, so a machine that stayed on with its
           fans running told the person it was off. machine_poweroff()
           says what actually happened, after trying. */
        sleep_ms(600);
        machine_poweroff();
    }

    else if(!strcmp(cmd,"exit")||!strcmp(cmd,"quit"))
        return;

    /* v0.9.5.1: the new games and apps, by name, as `open` would. */
    else if(!strcmp(cmd,"2048")||!strcmp(cmd,"breakout")||!strcmp(cmd,"tictactoe")||
            !strcmp(cmd,"ttt")||!strcmp(cmd,"memory")||!strcmp(cmd,"stopwatch")||
            !strcmp(cmd,"convert")||!strcmp(cmd,"notes")){
        extern int desktop_open_app(const char *id);
        desktop_open_app(!strcmp(cmd,"ttt")?"tictactoe":cmd);
        vga_clear(); toast_clear();
    }
    /* v0.9.7: Retro PC. With a file, that image boots straight away;
       the path goes over first because the desktop opens apps by id. */
    else if(!strcmp(cmd,"retro")){
        extern int  desktop_open_app(const char *id);
        extern void retro_set_next(const char *path);
        if(arg1[0]){ char p[64]; make_path(p, arg1); retro_set_next(p); }
        desktop_open_app("retro");
        vga_clear(); toast_clear();
    }
    else if(!strcmp(cmd,"open")){
        extern int desktop_open_by_name(const char*);
        extern int desktop_app_count(void);
        extern const char *desktop_app_id(int);
        extern const char *desktop_app_name(int);
        if(!arg1[0]){
            vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
            vga_puts("  open <app> -- by id or by the start of its name\n\n");
            for(int i=0;i<desktop_app_count();i++){
                vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
                vga_printf("  %-11s",desktop_app_id(i));
                vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
                vga_printf("%-20s",desktop_app_name(i));
                if(i%2) vga_putchar('\n');
            }
            vga_putchar('\n');
            vga_set_color(VGA_WHITE,VGA_BLACK);
        } else {
            /* the whole rest of the line: "open text editor" */
            char want[64]; strncpy(want,arg1,63); want[63]=0;
            if(arg2[0]){ int l=(int)strlen(want); if(l<62){ want[l]=' '; strncpy(want+l+1,arg2,62-l); want[63]=0; } }
            if(desktop_open_by_name(want)<0){
                vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
                vga_printf("open: no app called '%s' -- 'open' lists them\n",want);
                vga_set_color(VGA_WHITE,VGA_BLACK);
            } else { vga_clear(); toast_clear(); }
        }
    }
    else if(!strcmp(cmd,"settings")){
        extern void settings_open(const char*);
        settings_open(arg1);
        toast_clear();
    }
    else if(!strcmp(cmd,"tree"))  cmd_tree();
    else if(!strcmp(cmd,"which")) cmd_which(arg1[0]?arg1:arg2);
    else if(!strcmp(cmd,"guide")){ extern void cmd_guide_page(const char*); cmd_guide_page(arg1); }

    else
        {
            vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
            vga_puts("  error: '");
            vga_puts(cmd);
            vga_puts("' command not found");
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_puts("  (try 'help' or 'which ");
            vga_puts(cmd);
            vga_puts("')\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
        }
}

/* ================================================================
   MAIN SHELL LOOP
   ================================================================ */
static void show_motd(void){
    theme_t *th=ui_theme();

    /* 0.9.5.1: the same bar as every app, and the fortune without the
       dashed rules around it. */
    int W=vga_cols();
    vga_color_t sf, sb; ui_sel_colors(&sf,&sb);
    for(int x=0;x<W;x++) vga_putchar_at(' ',x,0,th->bar_fg,th->bar_bg);
    vga_puts_at(" HavenDOS ",0,0,sf,sb);
    vga_puts_at("Shell",11,0,th->bar_fg,th->bar_bg);
    uint8_t h2,m2,s2; rtc_get_time(&h2,&m2,&s2);
    char tb[6];
    tb[0]='0'+h2/10;tb[1]='0'+h2%10;tb[2]=':';
    tb[3]='0'+m2/10;tb[4]='0'+m2%10;tb[5]=0;
    vga_puts_at(tb,W-6,0,th->accent,th->bar_bg);
    vga_set_cursor(0,1);
    vga_putchar('\n');

    int count=0; while(fortunes[count]) count++;
    int idx=(int)(get_ticks()%count);
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("  ");
    vga_puts(fortunes[idx]);
    vga_puts("\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

void shell_run(void){
    char line[128];
    hist_load();   /* restore history from .shell_history on FAT16 */
    /* 0.9.5.1: start from a clean screen. The motd only draws its own
       rows, and a row the login left behind stayed on screen under it. */
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_clear(); vga_flush();
    show_motd();
    vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
    vga_printf("HavenDOS Shell " HAVENDOS_VER "  [user: %s]\n",current_username());
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("  Tab=complete  Up/Dn=history  Left/Right/Home/End=edit  ^R=search\n");
    vga_puts("  .=pipe   try: neofetch  disk  usb  selftest  help\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_set_color(VGA_WHITE,VGA_BLACK);

    while(1){
        /* Render PS1 prompt */
        char prompt[64];
        ps1_render(prompt,63);

        /* v0.7.11: the desktop pins 80x25 while it is up; restore the shell's
           chosen resolution when control comes back here. */
        /* v0.9.6.1: whatever else is open -- each other app's console is
           re-laid out when it next comes forward. vga_set_mode() refuses
           (and changes nothing) from a console that is not on screen. */
        if(vga_rows()!=shell_pref_rows()||vga_cols()!=shell_pref_cols())
            vga_set_mode(shell_pref_cols(),shell_pref_rows());
        /* v0.9.6: at the prompt the shell is PAUSED behind the front --
           it waits where it is. Running a command it is RUNNING, so a
           `ping` or a `wget` carries on when you Alt+Tab away. */
        task_set_policy(TP_PAUSE);
        /* v0.9.6.3: at the prompt the shell is a terminal -- no wheel,
           no right-click; a command it runs (settings, gfx try, the
           pager) gets the whole mouse */
        um_set_mode(UM_ZONES | UM_HINTS | UM_POINTER);
        shell_read_line(line,127,prompt);
        um_set_mode(UM_ALL);
        task_set_policy(TP_RUN);
        if(!line[0]) continue;
        hist_add(line);

        /* Alias expansion FIRST — so 'q' -> 'exit' works */
        {
            const char *p=line; while(*p==' ')p++;
            char firstword[32]; int fi=0;
            while(*p&&*p!=' '&&fi<31) firstword[fi++]=*p++;
            firstword[fi]=0;
            const char *aval=alias_get(firstword);
            if(aval){
                char expanded[128];
                strncpy(expanded,aval,63); expanded[63]=0;
                while(*p==' ')p++;
                if(*p){strncat(expanded," ",127);strncat(expanded,p,63);}
                strncpy(line,expanded,127); line[127]=0;   /* v0.7.6 */
            }
        }

        /* Exit check AFTER alias expansion */
        {
            const char *p=line; while(*p==' ')p++;
            if(!strcmp(p,"exit")||!strcmp(p,"quit")) return;
        }

        /* v0.9.6: `<command> &` runs it as a job, in a task of its own
           behind the front. shell/taskswitch.c has the rules. */
        {
            int L=(int)strlen(line);
            while(L>0&&line[L-1]==' ') line[--L]=0;
            if(L>=1&&line[L-1]=='&'){
                line[--L]=0;
                while(L>0&&line[L-1]==' ') line[--L]=0;
                const char *p=line; while(*p==' ')p++;
                extern int shell_start_job(const char *);
                if(*p) shell_start_job(p);
                else vga_puts("Usage: <command> &   runs the command behind, as a job\n");
                continue;
            }
        }

        /* $VAR expansion */
        {
            char expanded[128];
            expand_vars(line,expanded,128);
            strncpy(line,expanded,127); line[127]=0;   /* v0.7.6 */
        }

        /* v0.9.5: every command starts in the user's colours, so `color`
           and Settings > Shell actually stick. A command that sets its own
           colours still does; it just no longer has the last word. */
        vga_set_color((vga_color_t)sh_fg,(vga_color_t)sh_bg);
        run_pipeline(line);
        vga_set_color((vga_color_t)sh_fg,(vga_color_t)sh_bg);
    }
}

/* v0.9.6: one line, as the prompt would run it after aliases -- for a
   job (shell/taskswitch.c), which has its own console and no prompt. */
void shell_exec_line(const char *l){
    char line[128], expanded[128];
    strncpy(line,l,127); line[127]=0;
    expand_vars(line,expanded,128);
    vga_set_color((vga_color_t)sh_fg,(vga_color_t)sh_bg);
    run_pipeline(expanded);
    vga_set_color((vga_color_t)sh_fg,(vga_color_t)sh_bg);
}

/* ================================================================
   SCRIPT RUNNER
   ================================================================ */
/* ================================================================
   SCRIPT RUNNER — BSH with if/else/endif/while/endwhile
   ================================================================ */
#define MAX_SCRIPT_LINES 64
#define SCRIPT_LINE_LEN  100
#define MAX_NEST         8

void shell_run_script(const char *filename){
    char buf[4096]; uint32_t sz=sizeof(buf)-1;
    /* v0.9.5: from the working directory, like every other file command;
       `run count.bsh` in your home folder looked in the root. */
    char fpath[64]; make_path(fpath,filename);
    int rsz=vfs_read(fpath,buf,sz);
    if(rsz<0) rsz=vfs_read(filename,buf,sz);
    if(rsz<0){
        vga_printf("run: cannot open '%s'\n",filename); return;
    }
    buf[rsz]=0;

    /* ---- split into lines, skip blanks/comments ---- */
    static char lines[MAX_SCRIPT_LINES][SCRIPT_LINE_LEN];
    static int  jmp_false[MAX_SCRIPT_LINES]; /* if/while: line to go to if cond false */
    static int  jmp_back[MAX_SCRIPT_LINES];  /* endwhile: line of matching while */
    int n=0, overflow=0;
    {
        char line[SCRIPT_LINE_LEN]; int li=0;
        for(int i=0;;i++){
            char ch=buf[i];
            if(ch=='\n'||ch=='\r'||ch==0){
                line[li]=0;
                char *p=line; while(*p==' ')p++;
                if(*p&&*p!='#'){
                    if(n<MAX_SCRIPT_LINES){
                        strncpy(lines[n],p,SCRIPT_LINE_LEN-1);
                        lines[n][SCRIPT_LINE_LEN-1]=0;
                        jmp_false[n]=-1; jmp_back[n]=-1;
                        n++;
                    } else overflow=1;   /* v0.7.6: was silently truncated */
                }
                li=0; if(!ch) break;
            } else if(li<SCRIPT_LINE_LEN-1){ line[li++]=ch; }
        }
    }

    /* ---- prescan: match if/else/endif and while/endwhile ---- */
    {
        int ifstack[MAX_NEST], elsestack[MAX_NEST], iftop=0;
        int whilestack[MAX_NEST], wtop=0;
        for(int i=0;i<n;i++){
            char *p=lines[i];
            if(!strncmp(p,"if ",3)||!strcmp(p,"if")){
                if(iftop<MAX_NEST){ ifstack[iftop]=i; elsestack[iftop]=-1; iftop++; }
            } else if(!strcmp(p,"else")){
                if(iftop>0) elsestack[iftop-1]=i;
            } else if(!strcmp(p,"endif")){
                if(iftop>0){
                    iftop--;
                    int ifi=ifstack[iftop], elsei=elsestack[iftop];
                    if(elsei>=0){
                        jmp_false[ifi]=elsei+1; /* jump into else body */
                        jmp_false[elsei]=i+1;   /* after if-body, skip past endif */
                    } else {
                        jmp_false[ifi]=i+1;     /* no else: skip straight past endif */
                    }
                }
            } else if(!strncmp(p,"while ",6)||!strcmp(p,"while")){
                if(wtop<MAX_NEST){ whilestack[wtop]=i; wtop++; }
            } else if(!strcmp(p,"endwhile")){
                if(wtop>0){
                    wtop--;
                    int wi=whilestack[wtop];
                    jmp_false[wi]=i+1;   /* exit loop */
                    jmp_back[i]=wi;      /* loop back to re-check condition */
                }
            }
        }
    }

    if(overflow){
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_printf("  ! script longer than %d lines - remainder ignored\n",
                   MAX_SCRIPT_LINES);
        vga_set_color(VGA_WHITE,VGA_BLACK);
    }
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_printf("Running script: %s (%d lines)\n",filename,n);
    vga_set_color(VGA_WHITE,VGA_BLACK);

    /* ---- interpret ---- */
    int pc=0, steps=0;
    const int MAX_STEPS=4000; /* safety cap against runaway loops */
    while(pc<n){
        if(++steps>MAX_STEPS){
            vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
            vga_puts("  ! script aborted: step limit exceeded (infinite loop?)\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            break;
        }
        char *p=lines[pc];

        /* v0.7.6: the prescan matched a bare "if"/"while" (no condition) and
           pushed it onto the nesting stack, but the interpreter only matched
           the "if " / "while " forms — so the line fell through and was run as
           a shell command while its endif still consumed a stack slot.  Treat
           a missing condition as an explicit error. */
        if(!strcmp(p,"if")||!strcmp(p,"while")){
            vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
            vga_printf("  ! malformed script: '%s' with no condition\n",p);
            vga_set_color(VGA_WHITE,VGA_BLACK);
            break;
        }
        if(!strncmp(p,"if ",3)){
            int t=eval_cond(p+3);
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_printf("  ? if %s -> %s\n",p+3,t?"true":"false");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            if(t){ pc++; }
            else {
                if(jmp_false[pc]<0){
                    vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
                    vga_puts("  ! malformed script: 'if' has no matching 'endif'\n");
                    vga_set_color(VGA_WHITE,VGA_BLACK); break;
                }
                pc=jmp_false[pc];
            }
            continue;
        }
        if(!strcmp(p,"else")){
            if(jmp_false[pc]<0){
                vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
                vga_puts("  ! malformed script: 'else' has no matching 'endif'\n");
                vga_set_color(VGA_WHITE,VGA_BLACK); break;
            }
            pc = jmp_false[pc]; /* fell through if-body; skip else-body */
            continue;
        }
        if(!strcmp(p,"endif")){
            pc++; continue;
        }
        if(!strncmp(p,"while ",6)){
            int t=eval_cond(p+6);
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_printf("  ? while %s -> %s\n",p+6,t?"true":"false");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            if(t){ pc++; }
            else {
                if(jmp_false[pc]<0){
                    vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
                    vga_puts("  ! malformed script: 'while' has no matching 'endwhile'\n");
                    vga_set_color(VGA_WHITE,VGA_BLACK); break;
                }
                pc=jmp_false[pc];
            }
            continue;
        }
        if(!strcmp(p,"endwhile")){
            if(jmp_back[pc]<0){
                vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
                vga_puts("  ! malformed script: 'endwhile' has no matching 'while'\n");
                vga_set_color(VGA_WHITE,VGA_BLACK); break;
            }
            pc = jmp_back[pc]; /* re-check while condition */
            continue;
        }

        /* exit/quit ends the script */
        {
            char *q=p; while(*q==' ')q++;
            if(!strcmp(q,"exit")||!strcmp(q,"quit")) break;
        }

        /* normal command: expand vars, echo, run */
        {
            char expanded[SCRIPT_LINE_LEN];
            expand_vars(p,expanded,SCRIPT_LINE_LEN);
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_printf("  > %s\n",expanded);
            vga_set_color(VGA_WHITE,VGA_BLACK);
            run_pipeline(expanded);
        }
        pc++;
    }

    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("Script done.\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

/* ================================================================
   CLEAR ANIMATION
   ================================================================ */
void clear_anim(void){
    toast_clear();
    int mid=12;
    for(int i=0;i<=mid;i++){
        int top=mid-i, bot=mid+i;
        for(int x=0;x<80;x++){
            if(top>=0) vga_putchar_at(' ',x,top,VGA_BLACK,VGA_BLACK);
            if(bot<25) vga_putchar_at(' ',x,bot,VGA_BLACK,VGA_BLACK);
        }
        sleep_ms(18);
    }
}

/* ================================================================
   RUN SINGLE COMMAND
   ================================================================ */
/* Run a single command from outside the shell (e.g. from desktop) */
void shell_run_cmd(const char *cmd){
    char line[128];
    strncpy(line,cmd,127); line[127]=0;
    run_pipeline(line);
}

/* ================================================================
   SHELL INIT
   ================================================================ */
void shell_init(void){
    env_set("OS","HavenDOS");
    env_set("VER",HAVENDOS_VER_NUM);
    env_set("ARCH","x86");
    env_set("PATH","/bin:/usr/bin");
    alias_set("ll","ls");
    alias_set("q","exit");
    alias_set("?","help");
    alias_set("g","guide");
    alias_load();  /* override/add user-saved aliases from ETC/ALIASES.TXT */
    shell_colors_load();
    { extern const char *cfg_prompt(void);
      const char *pp=cfg_prompt();
      if(pp&&pp[0]){ strncpy(ps1_format,pp,63); ps1_format[63]=0; } }
    /* v0.8.8.1: and the console grid the user last confirmed. Applied
       at the prompt by the re-apply a few hundred lines below, not
       here -- the desktop pins 25 rows and restores this on exit. */
    { extern void shell_grid_load(void); shell_grid_load(); }
}
