/*
 * config.c — HavenDOS v0.6.2 system configuration
 * Persists to .havendos_config on the VirtIO disk (FAT16).
 *
 * Key=value format, one per line:
 *   setup_done=1
 *   autologin=0
 *   autologin_user=admin
 *   verbose_boot=1
 *   post_screen=1
 *   screensaver_timeout=120
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
static int  g_verbose_boot         = 1;
static int  g_post_screen          = 1;
static int  g_screensaver_timeout  = 120;
static int  g_autologin_timeout    = 5;   /* seconds before auto-login fires; 0=instant */
static char g_hostname[24]         = "HAVENDOS";

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
const char *cfg_hostname(void)                { return g_hostname; }
void        cfg_set_hostname(const char *h)   { strncpy(g_hostname,h,23); }
int         cfg_acct_hidden(int idx)          { return (idx>=0&&idx<3)?g_acct_hidden[idx]:0; }
void        cfg_set_acct_hidden(int idx,int v){ if(idx>=0&&idx<3)g_acct_hidden[idx]=v; }
