/*
 * config.c — HavenDOS v0.6.2 system configuration
 * Persists to .havendos_config on the VirtIO disk (FAT16).
 *
 * Key=value format, one per line:
 *   setup_done=1
 *   autologin=0
 *   autologin_user=admin
 *   verbose_boot=0      (0 = loading screen, 1 = the scrolling log)
 *   post_screen=1
 *   screensaver_timeout=0      (0 = off, which is the default)
 *   console_cols=120
 *   console_rows=67
 *   hostname=HAVENDOS
 *   account_hidden_admin=1
 *   account_hidden_bootos=1
 *   account_hidden_guest=1
 */
#include "../include/string.h"
#include "../include/types.h"

extern int  vfs_read (const char*, char*, uint32_t);
extern int  vfs_write(const char*, const char*, uint32_t);
extern void utoa(uint32_t, char*, int);
extern int  atoi(const char*);

#define CFG_FILE ".havendos_config"
#define CFG_BUF  1024

/* ── Config state ─────────────────────────────────────────────── */
static int  g_setup_done           = 0;
static int  g_autologin            = 0;
static char g_autologin_user[16]   = "";
/* v0.8.8.4: OFF BY DEFAULT, because the default is now the loading
   screen (kernel/bootsplash.c) rather than a blank one. Turning this
   on replaces the loading screen with the scrolling log; Settings has
   it as "Verbose boot log" and `verbose` on the kernel command line
   does the same for one boot.
   This does not hide anything: the ring buffer keeps every line, the
   loading screen's caption IS the latest one, and `dmesg` replays the
   lot. */
static int  g_verbose_boot         = 0;
static int  g_post_screen          = 1;
/* v0.8.8.4: OFF BY DEFAULT. It was 120, and until this release the
   desktop ignored the value anyway and fired at a hardcoded 30
   seconds -- see shell/screensaver.c. Now that the number is read, 0
   is the default, because a screensaver that hides what the machine
   is doing is not what a person wants while they are watching it
   boot, install, or generate a key. `screensaver <secs>` turns it on
   and the setting now works. */
static int  g_screensaver_timeout  = 0;
static int  g_autologin_timeout    = 5;   /* seconds before auto-login fires; 0=instant */
static char g_hostname[24]         = "HAVENDOS";

/* ── v0.8.8.1: the console grid, remembered ──────────────────────────
 * 0.8.8 let the user choose any grid and then forgot it on every
 * reboot, because shell_v076.c held the choice in two plain statics.
 * A feature you have to re-apply every single boot is one nobody uses.
 *
 * 0 means "never chosen" -- not 80x25 -- so that a machine which has
 * never been told anything keeps whatever the console came up with,
 * and a later change to that default is not silently overridden by a
 * config file written before it existed. */
static int  g_console_cols         = 0;
static int  g_console_rows         = 0;

/* Per-account hidden flags (index maps to login.c user order) */
/* 0=admin 1=bootos 2=guest */
static int  g_acct_hidden[3]       = {1, 1, 1};

static int  g_loaded = 0;

/* ── Helpers ──────────────────────────────────────────────────── */
static int cfg_startswith(const char *s, const char *prefix){
    while(*prefix) if(*s++!=*prefix++) return 0;
    return 1;
}
static int cfg_val_int(const char *line, const char *key){
    int kl=(int)strlen(key);
    return atoi(line+kl+1); /* skip "key=" */
}
static void cfg_val_str(const char *line, const char *key, char *out, int max){
    int kl=(int)strlen(key)+1; /* skip "key=" */
    int i=0;
    const char *p=line+kl;
    while(*p&&*p!='\n'&&i<max-1) out[i++]=*p++;
    out[i]=0;
}

/* ── Load ─────────────────────────────────────────────────────── */
void cfg_load(void){
    if(g_loaded) return;
    g_loaded=1;
    char buf[CFG_BUF];
    int n=vfs_read(CFG_FILE,buf,CFG_BUF-1);
    if(n<=0) return;
    buf[n]=0;
    char *line=buf;
    while(*line){
        char *end=line; while(*end&&*end!='\n')end++;
        char sv=*end; *end=0;
        if(cfg_startswith(line,"setup_done="))
            g_setup_done=cfg_val_int(line,"setup_done");
        else if(cfg_startswith(line,"autologin="))
            g_autologin=cfg_val_int(line,"autologin");
        else if(cfg_startswith(line,"autologin_user="))
            cfg_val_str(line,"autologin_user",g_autologin_user,16);
        else if(cfg_startswith(line,"verbose_boot="))
            g_verbose_boot=cfg_val_int(line,"verbose_boot");
        else if(cfg_startswith(line,"post_screen="))
            g_post_screen=cfg_val_int(line,"post_screen");
        else if(cfg_startswith(line,"screensaver_timeout="))
            g_screensaver_timeout=cfg_val_int(line,"screensaver_timeout");
        else if(cfg_startswith(line,"autologin_timeout="))
            g_autologin_timeout=cfg_val_int(line,"autologin_timeout");
        else if(cfg_startswith(line,"console_cols="))
            g_console_cols=cfg_val_int(line,"console_cols");
        else if(cfg_startswith(line,"console_rows="))
            g_console_rows=cfg_val_int(line,"console_rows");
        else if(cfg_startswith(line,"hostname="))
            cfg_val_str(line,"hostname",g_hostname,24);
        else if(cfg_startswith(line,"account_hidden_admin="))
            g_acct_hidden[0]=cfg_val_int(line,"account_hidden_admin");
        else if(cfg_startswith(line,"account_hidden_bootos="))
            g_acct_hidden[1]=cfg_val_int(line,"account_hidden_bootos");
        else if(cfg_startswith(line,"account_hidden_guest="))
            g_acct_hidden[2]=cfg_val_int(line,"account_hidden_guest");
        *end=sv;
        line=(*end)?end+1:end;
    }
}

/* ── Save ─────────────────────────────────────────────────────── */
void cfg_save(void){
    char buf[CFG_BUF]; int p=0;
    #define EMIT_KV(k,v) do{ \
        const char *_k=(k); while(*_k)buf[p++]=*_k++; \
        buf[p++]='='; \
        char _vb[12]; utoa((uint32_t)(v),_vb,10); \
        const char *_v=_vb; while(*_v)buf[p++]=*_v++; \
        buf[p++]='\n'; \
    }while(0)
    #define EMIT_KS(k,s) do{ \
        const char *_k=(k); while(*_k)buf[p++]=*_k++; \
        buf[p++]='='; \
        const char *_s=(s); while(*_s&&p<CFG_BUF-4)buf[p++]=*_s++; \
        buf[p++]='\n'; \
    }while(0)

    EMIT_KV("setup_done",           g_setup_done);
    EMIT_KV("autologin",            g_autologin);
    EMIT_KS("autologin_user",       g_autologin_user);
    EMIT_KV("verbose_boot",         g_verbose_boot);
    EMIT_KV("post_screen",          g_post_screen);
    EMIT_KV("screensaver_timeout",  g_screensaver_timeout);
    EMIT_KV("autologin_timeout",    g_autologin_timeout);
    EMIT_KV("console_cols",         g_console_cols);
    EMIT_KV("console_rows",         g_console_rows);
    EMIT_KS("hostname",             g_hostname);
    EMIT_KV("account_hidden_admin", g_acct_hidden[0]);
    EMIT_KV("account_hidden_bootos",g_acct_hidden[1]);
    EMIT_KV("account_hidden_guest", g_acct_hidden[2]);

    #undef EMIT_KV
    #undef EMIT_KS
    buf[p]=0;
    vfs_write(CFG_FILE,buf,(uint32_t)p);
}

/* ── Accessors ────────────────────────────────────────────────── */
int         cfg_setup_done(void)              { return g_setup_done; }
void        cfg_set_setup_done(int v)         { g_setup_done=v; }
int         cfg_autologin(void)               { return g_autologin; }
void        cfg_set_autologin(int v)          { g_autologin=v; }
const char *cfg_autologin_user(void)          { return g_autologin_user; }
void        cfg_set_autologin_user(const char*u){ strncpy(g_autologin_user,u,15); }
int         cfg_verbose_boot(void)            { return g_verbose_boot; }
void        cfg_set_verbose_boot(int v)       { g_verbose_boot=v; }
int         cfg_post_screen(void)             { return g_post_screen; }
void        cfg_set_post_screen(int v)        { g_post_screen=v; }
int         cfg_screensaver_timeout(void)     { return g_screensaver_timeout; }
void        cfg_set_screensaver_timeout(int v){ g_screensaver_timeout=v; }
int         cfg_autologin_timeout(void)       { return g_autologin_timeout; }
void        cfg_set_autologin_timeout(int v)  { g_autologin_timeout=(v<0)?0:v; }
int         cfg_console_cols(void)             { return g_console_cols; }
int         cfg_console_rows(void)             { return g_console_rows; }
/* Both or neither: half a grid is not a grid, and a config file with
   one of the two set would otherwise apply a column count to whatever
   row count happened to be current. */
void        cfg_set_console_grid(int c,int r){
    if(c>0 && r>0){ g_console_cols=c; g_console_rows=r; }
    else          { g_console_cols=0; g_console_rows=0; }
}
const char *cfg_hostname(void)                { return g_hostname; }
void        cfg_set_hostname(const char *h)   { strncpy(g_hostname,h,23); }
int         cfg_acct_hidden(int idx)          { return (idx>=0&&idx<3)?g_acct_hidden[idx]:0; }
void        cfg_set_acct_hidden(int idx,int v){ if(idx>=0&&idx<3)g_acct_hidden[idx]=v; }
