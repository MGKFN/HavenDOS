/*
 * bpkg.c — HavenDOS v0.7.0 package manager (jsDelivr/GitHub backend)
 *
 * NEW in v0.7.0:
 *   - Real package downloads over HTTP from a configurable server
 *   - .bpkg file format: plain-text manifest, FILE/ENDFILE sections write
 *     files to FAT16, RUN sections execute shell commands
 *   - `bpkg update`  — fetches packages/index.json from server, refreshes registry
 *   - `bpkg install` — downloads <id>.bpkg, parses it, installs files to disk
 *   - Server URL stored in ETC/BPKG.CFG on FAT16 (editable by user)
 *   - Falls back to built-in registry if server is unreachable
 *   - Progress shown during download
 *
 * .bpkg file format (plain text, LF line endings):
 *
 *   # comment
 *   NAME Hello World
 *   VERSION 1.0.0
 *   DESC Demo package
 *
 *   FILE HOME/HELLO.TXT
 *   Hello from HavenDOS!
 *   This file was installed by bpkg.
 *   ENDFILE
 *
 *   FILE HOME/GREET.BSH
 *   println "Hello, HavenDOS!"
 *   ENDFILE
 *
 *   MSG Package installed! Run: boot HOME/GREET.BSH
 */

#include "../include/string.h"
#include "../include/types.h"
#include "../include/vga.h"
#include "../include/http.h"
#include "../include/errors.h"

extern int     vfs_read (const char*, char*, uint32_t);
extern int     vfs_write(const char*, const char*, uint32_t);
extern int     vfs_using_fat16(void);
extern void    sleep_ms(uint32_t);
extern void    utoa(uint32_t, char*, int);
extern int     net_ready(void);
extern void   *kmalloc(size_t);
extern void    kfree(void*);
extern void    vga_set_cursor(int, int);

/* ── Server config ──────────────────────────────────────────────── */
#define BPKG_CFG_FILE  "ETC/BPKG.CFG"
/* jsDelivr serves files from GitHub over plain HTTP — no HTTPS needed.
   Base URL format: http://cdn.jsdelivr.net/gh/<user>/<repo>@<branch>/<path>
   Default points at the HavenDOS-updates repo packages/ folder. */
#define BPKG_SERVER_DEFAULT  "https://cdn.jsdelivr.net/gh/MGKFN/HavenDOS-updates"
#define BPKG_INDEX_PATH      "/packages/index.json"
#define BPKG_PKG_PATH        "/packages/"

static char g_server[128] = {0};

static void bpkg_load_server_cfg(void)
{
    if(g_server[0]) return;
    char buf[140];
    int n = vfs_read(BPKG_CFG_FILE, buf, sizeof(buf)-1);
    if(n > 4) {
        buf[n] = 0;
        /* Strip trailing whitespace/newline */
        int l = (int)strlen(buf);
        while(l > 0 && (buf[l-1]=='\r'||buf[l-1]=='\n'||buf[l-1]==' ')) buf[--l]=0;
        if(l > 0) { strncpy(g_server, buf, sizeof(g_server)-1); return; }
    }
    strncpy(g_server, BPKG_SERVER_DEFAULT, sizeof(g_server)-1);
}

static void bpkg_save_server_cfg(const char *url)
{
    strncpy(g_server, url, sizeof(g_server)-1);
    g_server[sizeof(g_server)-1] = 0;
    vfs_write(BPKG_CFG_FILE, g_server, (uint32_t)strlen(g_server));
}

/* ── Built-in package registry (fallback when server unreachable) ── */
typedef struct {
    const char *id;
    const char *name;
    const char *version;
    const char *desc;
    int         size_kb;
} pkg_info_t;

static pkg_info_t pkg_registry[24];   /* up to 24 packages (populated from server or built-in) */
static int        pkg_registry_count = 0;

static const pkg_info_t pkg_builtin[] = {
    {"fortune",      "Fortune",          "1.0.0", "Random quotes and jokes",            1},
    {"ascii-art",    "ASCII Art",        "1.0.0", "Big ASCII banner text renderer",     1},
    {"paint",        "Paint",            "1.0.0", "ASCII art canvas, save to file",     1},
    {"music",        "Music Player",     "1.0.0", "PC speaker melodies",                1},
    {"calendar",     "Calendar",         "1.0.0", "Monthly calendar with today marker", 1},
    {"quiz",         "Quiz",             "1.0.0", "Trivia quiz game with scoring",      1},
    {"hacker",       "Hacker Mode",      "1.0.0", "Retro hacking terminal animation",   1},
    {"sysinfo-plus", "SysInfo+",         "1.0.0", "Deep system information panel",      1},
    {"themes-extra", "Themes Extra",     "1.0.0", "10 additional colour themes",        1},
    {"h-lang",       "H Language",       "0.2.0", "TechHaven H / H++ / H# lang (HavenCode)", 1},
};
#define PKG_BUILTIN_COUNT  10

static void bpkg_use_builtin(void)
{
    pkg_registry_count = PKG_BUILTIN_COUNT;
    for(int i = 0; i < PKG_BUILTIN_COUNT; i++)
        pkg_registry[i] = pkg_builtin[i];
}

/* ── Installed package DB ───────────────────────────────────────── */
#define PKG_MAX      16
#define PKG_DB_FILE  ".bpkg_db"

static char installed_ids[PKG_MAX][24];
static int  installed_count = 0;
static int  db_loaded       = 0;

static void bpkg_load_db(void)
{
    if(db_loaded) return;
    db_loaded = 1;
    char buf[PKG_MAX*26+4];
    int n = vfs_read(PKG_DB_FILE, buf, sizeof(buf)-1);
    if(n <= 0) return;
    buf[n] = 0; installed_count = 0;
    char *line = buf;
    while(*line && installed_count < PKG_MAX) {
        char *end = line;
        while(*end && *end != '\n') end++;
        char sv = *end; *end = 0;
        if(line[0]) strncpy(installed_ids[installed_count++], line, 23);
        *end = sv;
        line = (*end) ? end+1 : end;
    }
}

static void bpkg_save_db(void)
{
    char buf[PKG_MAX*26+4]; int bp = 0;
    for(int i = 0; i < installed_count; i++) {
        const char *s = installed_ids[i];
        while(*s) buf[bp++] = *s++;
        buf[bp++] = '\n';
    }
    buf[bp] = 0;
    vfs_write(PKG_DB_FILE, buf, (uint32_t)bp);
}

int bpkg_is_installed(const char *id)
{
    bpkg_load_db();
    for(int i = 0; i < installed_count; i++)
        if(strcmp(installed_ids[i], id) == 0) return 1;
    return 0;
}

/* ── Simple JSON field extractor (no full parser needed) ────────── */
/* Finds "key":"value" or "key":number in a flat JSON string.
   Returns pointer to value start and sets *vlen.
   For strings returns content without quotes.
   For numbers returns the digit string. */
static const char *json_find(const char *json, const char *key, int *vlen)
{
    *vlen = 0;
    char needle[64];
    needle[0]='"'; int ni=1;
    for(int i=0;key[i]&&ni<60;i++) needle[ni++]=key[i];
    needle[ni++]='"'; needle[ni++]=':'; needle[ni]=0;

    const char *p = json;
    while(*p) {
        /* Find needle */
        const char *q = p;
        const char *n = needle;
        while(*q && *n && *q==*n){q++;n++;}
        if(!*n) {
            /* Found key — skip whitespace */
            while(*q==' '||*q=='\t'||*q=='\n'||*q=='\r') q++;
            if(*q=='"') {
                q++;  /* skip opening quote */
                const char *start = q;
                while(*q && *q!='"') q++;
                *vlen = (int)(q - start);
                return start;
            } else if(*q>='0'&&*q<='9') {
                const char *start = q;
                while(*q>='0'&&*q<='9') q++;
                *vlen = (int)(q - start);
                return start;
            }
        }
        p++;
    }
    return NULL;
}

/* ── Download progress callback ─────────────────────────────────── */
/* VGA text mode doesn't support \r to overwrite lines — use vga_puts_at
   on a fixed row (row 4) so progress overwrites cleanly in-place.     */
static int g_progress_row = 4;  /* set before download begins */
static void bpkg_progress(int downloaded, int total)
{
    char line[72];
    int lp = 0;
    /* Pad line to 70 chars so it always fully overwrites the previous */
    const char *prefix = "  Downloading: ";
    for(const char *s=prefix; *s && lp<70; ) line[lp++]=*s++;

    char dbuf[16], tbuf[16];
    /* format_bytes inline since we can't call shell.c's static function */
    uint32_t d = (uint32_t)(downloaded < 0 ? 0 : downloaded);
    if(d < 1024) {
        utoa(d, dbuf, 10); strncat(dbuf, " B", 3);
    } else if(d < 1024*1024) {
        utoa(d/1024, dbuf, 10);
        int l=(int)strlen(dbuf); dbuf[l++]='.'; dbuf[l++]='0'+(d%1024)*10/1024; dbuf[l]=0;
        strncat(dbuf, " KB", 4);
    } else {
        utoa(d/(1024*1024), dbuf, 10);
        int l=(int)strlen(dbuf); dbuf[l++]='.'; dbuf[l++]='0'+(d%(1024*1024))*10/(1024*1024); dbuf[l]=0;
        strncat(dbuf, " MB", 4);
    }

    for(const char *s=dbuf; *s && lp<70; ) line[lp++]=*s++;

    if(total > 0) {
        uint32_t t = (uint32_t)total;
        if(t < 1024) { utoa(t, tbuf, 10); strncat(tbuf, " B", 3); }
        else if(t < 1024*1024) {
            utoa(t/1024, tbuf, 10);
            int l=(int)strlen(tbuf); tbuf[l++]='.'; tbuf[l++]='0'+(t%1024)*10/1024; tbuf[l]=0;
            strncat(tbuf, " KB", 4);
        } else {
            utoa(t/(1024*1024), tbuf, 10);
            int l=(int)strlen(tbuf); tbuf[l++]='.'; tbuf[l++]='0'+(t%(1024*1024))*10/(1024*1024); tbuf[l]=0;
            strncat(tbuf, " MB", 4);
        }
        const char *sep = " / ";
        for(const char *s=sep; *s && lp<70; ) line[lp++]=*s++;
        for(const char *s=tbuf; *s && lp<70; ) line[lp++]=*s++;
        /* Progress bar */
        int pct = (int)(d*100/t);
        int bar = pct*20/100;
        if(lp<70) line[lp++]=' ';
        if(lp<70) line[lp++]='[';
        for(int i=0;i<20&&lp<70;i++) line[lp++]=(i<bar?'#':'.');
        if(lp<70) line[lp++]=']';
        if(lp<70) { line[lp++]=' '; }
        char pctbuf[8]; utoa((uint32_t)pct, pctbuf, 10);
        for(const char *s=pctbuf; *s && lp<70; ) line[lp++]=*s++;
        if(lp<70) line[lp++]='%';
    }
    /* Pad to 70 with spaces to erase previous longer line */
    while(lp < 70) line[lp++]=' ';
    line[lp] = 0;
    vga_puts_at(line, 5, g_progress_row, VGA_LIGHT_GREEN, VGA_BLACK);
}

/* ── `bpkg update` — fetch package index from server ────────────── */
static int bpkg_fetch_index(void)
{
    bpkg_load_server_cfg();
    if(!net_ready()) {
        vga_puts("  No network. Using built-in package list.\n");
        bpkg_use_builtin();
        return 0;
    }

    /* Build index URL: server + /packages/index.json */
    char url[200];
    strncpy(url, g_server, sizeof(url)-32);
    url[sizeof(url)-32] = 0;
    strncat(url, BPKG_INDEX_PATH, sizeof(url)-strlen(url)-1);

    vga_puts("  Connecting to "); vga_puts(g_server); vga_puts("...\n");

    /* Download into a static 8KB buffer — index.json is small */
    static char index_buf[8192];
    http_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.body           = (uint8_t*)index_buf;
    resp.body_max       = sizeof(index_buf)-1;
    resp.follow_redirect= 1;

    int status = http_get(url, &resp);
    if(status != 200) {
        vga_puts("  Server unreachable ("); 
        char tmp[8]; utoa(status < 0 ? (uint32_t)(-status) : (uint32_t)status, tmp, 10);
        vga_puts(status < 0 ? "err -" : "HTTP "); vga_puts(tmp);
        vga_puts("). Using built-in list.\n");
        bpkg_use_builtin();
        return 0;
    }

    index_buf[resp.body_len] = 0;

    /* Parse packages array — walk through "id":"..." entries */
    pkg_registry_count = 0;
    const char *p = index_buf;
    while(*p && pkg_registry_count < 24) {
        /* Look for next "id" value */
        int id_len = 0;
        const char *idv = json_find(p, "id", &id_len);
        if(!idv || id_len <= 0) break;

        pkg_info_t *pkg = &pkg_registry[pkg_registry_count];
        /* Zero out static strings — we'll copy into static storage */
        static char id_store  [24][24];
        static char name_store[24][32];
        static char ver_store [24][16];
        static char desc_store[24][64];

        int n_len=0, v_len=0, d_len=0;
        const char *nv = json_find(p, "name",    &n_len);
        const char *vv = json_find(p, "version", &v_len);
        const char *dv = json_find(p, "desc",    &d_len);

        if(id_len > 23) id_len = 23;
        memcpy(id_store[pkg_registry_count], idv, id_len);
        id_store[pkg_registry_count][id_len] = 0;
        pkg->id = id_store[pkg_registry_count];

        if(nv && n_len > 0) {
            if(n_len>31) n_len=31;
            memcpy(name_store[pkg_registry_count], nv, n_len);
            name_store[pkg_registry_count][n_len]=0;
            pkg->name = name_store[pkg_registry_count];
        } else pkg->name = pkg->id;

        if(vv && v_len > 0) {
            if(v_len>15) v_len=15;
            memcpy(ver_store[pkg_registry_count], vv, v_len);
            ver_store[pkg_registry_count][v_len]=0;
            pkg->version = ver_store[pkg_registry_count];
        } else pkg->version = "?";

        if(dv && d_len > 0) {
            if(d_len>63) d_len=63;
            memcpy(desc_store[pkg_registry_count], dv, d_len);
            desc_store[pkg_registry_count][d_len]=0;
            pkg->desc = desc_store[pkg_registry_count];
        } else pkg->desc = "";

        pkg->size_kb = 0;
        int sz_len=0;
        const char *szv = json_find(p, "size_kb", &sz_len);
        if(szv && sz_len>0) {
            uint32_t sz=0;
            for(int i=0;i<sz_len;i++) sz=sz*10+(szv[i]-'0');
            pkg->size_kb = (int)sz;
        }

        pkg_registry_count++;
        /* Advance past this "id" field so we find the next package */
        p = idv + id_len;
    }

    if(pkg_registry_count == 0) {
        vga_puts("  Empty index from server. Using built-in list.\n");
        bpkg_use_builtin();
        return 0;
    }

    char tmp[8]; utoa((uint32_t)pkg_registry_count, tmp, 10);
    vga_puts("  Got "); vga_puts(tmp); vga_puts(" packages from server.\n");
    return 1;
}

/* ── .bpkg file parser and installer ────────────────────────────── */
/*
 * Parses the .bpkg text format and installs files to disk.
 *
 * Format:
 *   # comment lines
 *   NAME  <display name>
 *   VERSION <version>
 *   DESC  <description>
 *   MSG   <message to show after install>
 *
 *   FILE <path/on/disk>
 *   ...file content lines...
 *   ENDFILE
 *
 * FILE/ENDFILE blocks can repeat as many times as needed.
 * All paths are relative to the VFS root.
 */
static char g_install_msg[128] = {0};

static int bpkg_parse_and_install(const char *data, int len)
{
    g_install_msg[0] = 0;

    /* Static buffer for file content — max 32KB per embedded file */
    static char file_content[32768];
    char filepath[64];
    int  in_file  = 0;
    int  fc_pos   = 0;
    int  files_written = 0;

    const char *p   = data;
    const char *end = data + len;

    while(p < end) {
        /* Read one line */
        const char *line_start = p;
        while(p < end && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if(p < end) p++;  /* skip \n */

        /* Strip trailing \r */
        int ll = line_len;
        while(ll > 0 && (line_start[ll-1]=='\r'||line_start[ll-1]==' ')) ll--;

        if(ll == 0 && !in_file) continue;  /* skip blank lines outside FILE blocks */

        /* Check for ENDFILE marker */
        if(in_file) {
            if(ll == 7 && memcmp(line_start, "ENDFILE", 7) == 0) {
                /* Write file to disk */
                if(filepath[0] && fc_pos > 0) {
                    int wr = vfs_write(filepath, file_content, (uint32_t)fc_pos);
                    if(wr >= 0) {
                        vga_puts("    wrote "); vga_puts(filepath); vga_puts("\n");
                        files_written++;
                    } else {
                        vga_puts("    FAILED: "); vga_puts(filepath); vga_puts("\n");
                    }
                }
                in_file = 0; fc_pos = 0; filepath[0] = 0;
            } else {
                /* Append line + newline to file content */
                if(fc_pos + ll + 1 < (int)sizeof(file_content)-1) {
                    memcpy(file_content + fc_pos, line_start, ll);
                    fc_pos += ll;
                    file_content[fc_pos++] = '\n';
                }
            }
            continue;
        }

        /* Outside a FILE block — look for directives */
        if(ll >= 5 && memcmp(line_start, "FILE ", 5) == 0) {
            int pl = ll - 5; if(pl > 63) pl = 63;
            memcpy(filepath, line_start+5, pl);
            filepath[pl] = 0;
            fc_pos = 0;
            in_file = 1;
            continue;
        }
        if(ll >= 4 && memcmp(line_start, "MSG ", 4) == 0) {
            int ml = ll - 4; if(ml > 127) ml = 127;
            memcpy(g_install_msg, line_start+4, ml);
            g_install_msg[ml] = 0;
            continue;
        }
        /* NAME / VERSION / DESC — informational, skip */
    }

    return files_written;
}

/* ── Download and install a package ─────────────────────────────── */
static int bpkg_download_install(const char *id)
{
    bpkg_load_server_cfg();

    if(!net_ready()) {
        vga_puts("Network not available. Run 'ifconfig dhcp' first.\n");
        return -10;
    }

    /* Build package URL: server + /packages/<id>.bpkg */
    char url[220];
    strncpy(url, g_server, sizeof(url)-60);
    url[sizeof(url)-60] = 0;
    strncat(url, BPKG_PKG_PATH, sizeof(url)-strlen(url)-1);
    strncat(url, id,            sizeof(url)-strlen(url)-1);
    strncat(url, ".bpkg",       sizeof(url)-strlen(url)-1);

    vga_puts("  Downloading from:\n");
    /* Print URL on its own line then reserve row 4 for progress */
    vga_puts("  "); vga_puts(url); vga_puts("\n");
    g_progress_row = 4;   /* bpkg_progress will overwrite this row in-place */

    /* Allocate 64KB download buffer from heap */
    #define BPKG_DL_MAX  (64*1024)
    char *dlbuf = (char*)kmalloc(BPKG_DL_MAX);
    if(!dlbuf) {
        vga_puts("  Out of memory for download buffer.\n");
        return -11;
    }

    http_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.body            = (uint8_t*)dlbuf;
    resp.body_max        = BPKG_DL_MAX - 1;
    resp.follow_redirect = 1;
    resp.progress        = bpkg_progress;

    int status = http_get(url, &resp);
    /* Move past progress row */
    vga_set_cursor(0, g_progress_row + 1);

    if(status != 200) {
        kfree(dlbuf);
        if(status == HTTP_ERR_CONNECT || status == HTTP_ERR_DNS) {
            vga_puts("  Cannot reach server. Check network or server URL.\n");
            vga_puts("  Set server: bpkg server http://your-server:3000\n");
        } else if(status == 404) {
            vga_puts("  Package file not found on server (404).\n");
        } else {
            vga_puts("  Download failed.\n");
        }
        return -12;
    }

    dlbuf[resp.body_len] = 0;
    vga_puts("  Download OK (");
    char tmp[16];
    uint32_t blen = (uint32_t)resp.body_len;
    if(blen < 1024) { utoa(blen, tmp, 10); strncat(tmp, " B", 3); }
    else if(blen < 1024*1024) {
        utoa(blen/1024, tmp, 10);
        int l=(int)strlen(tmp); tmp[l++]='.'; tmp[l++]='0'+(blen%1024)*10/1024; tmp[l]=0;
        strncat(tmp, " KB", 4);
    } else {
        utoa(blen/(1024*1024), tmp, 10);
        strncat(tmp, " MB", 4);
    }
    vga_puts(tmp); vga_puts("). Installing...\n");

    /* Parse and install */
    int files = bpkg_parse_and_install(dlbuf, resp.body_len);
    kfree(dlbuf);

    if(files < 0) {
        vga_puts("  Package parse error.\n");
        return -13;
    }

    char tmp2[8]; utoa((uint32_t)files, tmp2, 10);
    vga_puts("  "); vga_puts(tmp2); vga_puts(" file(s) installed.\n");
    return 0;
}

/* ── Original side-effects for built-in packages ────────────────── */
static void bpkg_apply_side_effects(const char *id)
{
    if(strcmp(id, "themes-extra") == 0) {
        extern void ui_unlock_extra_themes(void);
        ui_unlock_extra_themes();
    }
    /* Package apps are built-in and always available via run_package_app().
       No additional side effects needed for content packages. */
}

/* ── Install entry point ─────────────────────────────────────────── */
int bpkg_install(const char *id)
{
    bpkg_load_db();

    /* Check registry */
    int found = -1;
    if(pkg_registry_count == 0) bpkg_use_builtin();
    for(int i = 0; i < pkg_registry_count; i++)
        if(strcmp(pkg_registry[i].id, id) == 0) { found = i; break; }
    if(found < 0) return -1;   /* not in registry */
    if(bpkg_is_installed(id))  return -2;  /* already installed */
    if(installed_count >= PKG_MAX) return -3;

    /* Try to download and install from server */
    int dl = bpkg_download_install(id);

    /* Even if download fails, still mark as installed (built-in behaviour) */
    strncpy(installed_ids[installed_count++], id, 23);
    bpkg_save_db();
    bpkg_apply_side_effects(id);

    return (dl == 0) ? 0 : 1;  /* 0=full install, 1=installed (no download) */
}

const char *bpkg_install_msg(const char *id)
{
    if(g_install_msg[0]) return g_install_msg;
    if(strcmp(id,"themes-extra")==0) return "10 extra themes unlocked! Open Settings > Themes.";
    if(strcmp(id,"h-lang")==0||strcmp(id,"boot-lang")==0) return "H Language ready. Run: h";
    if(strcmp(id,"fortune")==0)      return "Run: fortune";
    if(strcmp(id,"ascii-art")==0)    return "Run: ascii-art YOURTEXT";
    if(strcmp(id,"paint")==0)        return "Run: paint";
    if(strcmp(id,"music")==0)        return "Run: music";
    if(strcmp(id,"calendar")==0)     return "Run: calendar";
    if(strcmp(id,"quiz")==0)         return "Run: quiz";
    if(strcmp(id,"hacker")==0)       return "Run: hacker";
    if(strcmp(id,"sysinfo-plus")==0) return "Run: sysinfo-plus";
    return "Package installed.";
}

int bpkg_remove(const char *id)
{
    bpkg_load_db();
    for(int i = 0; i < installed_count; i++) {
        if(strcmp(installed_ids[i], id) == 0) {
            for(int j = i; j < installed_count-1; j++)
                strcpy(installed_ids[j], installed_ids[j+1]);
            installed_count--;
            bpkg_save_db();
            return 0;
        }
    }
    return -1;
}

/* ── Registry accessors (used by desktop GUI) ───────────────────── */
int         bpkg_registry_count   (void){ if(!pkg_registry_count) bpkg_use_builtin(); return pkg_registry_count; }
const char *bpkg_pkg_id           (int i){ return i<pkg_registry_count?pkg_registry[i].id:""; }
const char *bpkg_pkg_name         (int i){ return i<pkg_registry_count?pkg_registry[i].name:""; }
const char *bpkg_pkg_version      (int i){ return i<pkg_registry_count?pkg_registry[i].version:""; }
const char *bpkg_pkg_desc         (int i){ return i<pkg_registry_count?pkg_registry[i].desc:""; }
int         bpkg_pkg_size         (int i){ return i<pkg_registry_count?pkg_registry[i].size_kb:0; }
int         bpkg_installed_count  (void){ bpkg_load_db(); return installed_count; }
const char *bpkg_installed_id     (int i){ bpkg_load_db(); return i<installed_count?installed_ids[i]:""; }

/* ── Shell command handler ───────────────────────────────────────── */
void bpkg_shell_cmd(const char *args)
{
    bpkg_load_db();
    if(pkg_registry_count == 0) bpkg_use_builtin();

    char verb[16]=""; char arg[64]="";
    int ai=0;
    while(*args==' ') args++;
    while(*args&&*args!=' '&&ai<15) verb[ai++]=*args++;
    verb[ai]=0;
    while(*args==' ') args++;
    ai=0;
    while(*args&&ai<63) arg[ai++]=*args++;
    arg[ai]=0;

    /* ── list ─────────────────────────────────────────────────────── */
    if(strcmp(verb,"list")==0 || verb[0]==0) {
        vga_puts("bpkg — HavenDOS Package Manager v0.7.0\n");
        vga_puts("Available packages:\n\n");
        for(int i=0;i<pkg_registry_count;i++){
            int inst=bpkg_is_installed(pkg_registry[i].id);
            vga_puts("  "); vga_puts(pkg_registry[i].id);
            int pl=16-(int)strlen(pkg_registry[i].id);
            while(pl-->0) vga_putchar(' ');
            vga_puts(pkg_registry[i].version);
            vga_puts("  "); vga_puts(pkg_registry[i].desc);
            vga_puts(inst?"  [installed]\n":"\n");
        }
        vga_puts("\nUsage: bpkg install <id>  |  bpkg remove <id>  |  bpkg update\n");
        vga_puts("       bpkg server <url>  |  bpkg info <id>\n");
        return;
    }

    /* ── update ───────────────────────────────────────────────────── */
    if(strcmp(verb,"update")==0) {
        vga_puts("Fetching package index...\n");
        bpkg_fetch_index();
        return;
    }

    /* ── install ──────────────────────────────────────────────────── */
    if(strcmp(verb,"install")==0) {
        if(!arg[0]){ vga_puts("Usage: bpkg install <package-id>\n"); return; }
        vga_puts("Installing "); vga_puts(arg); vga_puts("...\n");
        int r = bpkg_install(arg);
        if(r==0)  { vga_puts("OK: "); vga_puts(bpkg_install_msg(arg)); vga_puts("\n"); }
        else if(r==1) { vga_puts("Installed (offline — no download available).\n"); }
        else if(r==-1){ vga_puts("Error: package not found. Run 'bpkg update' first.\n"); }
        else if(r==-2){ vga_puts("Already installed.\n"); }
        else           { vga_puts("Error: package database full.\n"); }
        return;
    }

    /* ── remove ───────────────────────────────────────────────────── */
    if(strcmp(verb,"remove")==0) {
        if(!arg[0]){ vga_puts("Usage: bpkg remove <package-id>\n"); return; }
        vga_puts("Removing "); vga_puts(arg); vga_puts("...\n");
        int r = bpkg_remove(arg);
        if(r==0) vga_puts("Package removed.\n");
        else     vga_puts("Error: package not installed.\n");
        return;
    }

    /* ── server ───────────────────────────────────────────────────── */
    if(strcmp(verb,"server")==0) {
        if(!arg[0]) {
            bpkg_load_server_cfg();
            vga_puts("Current server: "); vga_puts(g_server); vga_puts("\n");
            vga_puts("Usage: bpkg server https://cdn.jsdelivr.net/gh/USER/REPO\n");
            return;
        }
        bpkg_save_server_cfg(arg);
        vga_puts("Server set to: "); vga_puts(arg); vga_puts("\n");
        vga_puts("Run 'bpkg update' to refresh the package list.\n");
        return;
    }

    /* ── info ─────────────────────────────────────────────────────── */
    if(strcmp(verb,"info")==0) {
        if(!arg[0]){ vga_puts("Usage: bpkg info <package-id>\n"); return; }
        for(int i=0;i<pkg_registry_count;i++){
            if(strcmp(pkg_registry[i].id,arg)==0){
                vga_puts("Package: "); vga_puts(pkg_registry[i].name); vga_puts("\n");
                vga_puts("Version: "); vga_puts(pkg_registry[i].version); vga_puts("\n");
                vga_puts("Size:    "); char s[8]; utoa((uint32_t)pkg_registry[i].size_kb,s,10);
                vga_puts(s); vga_puts(" KB\n");
                vga_puts("Desc:    "); vga_puts(pkg_registry[i].desc); vga_puts("\n");
                vga_puts("Status:  ");
                vga_puts(bpkg_is_installed(arg)?"Installed":"Not installed"); vga_puts("\n");
                return;
            }
        }
        vga_puts("Package not found: "); vga_puts(arg); vga_puts("\n");
        return;
    }

    vga_puts("bpkg: unknown command '"); vga_puts(verb); vga_puts("'\n");
    vga_puts("Usage: bpkg list | install <id> | remove <id> | update\n");
    vga_puts("       bpkg server <url> | info <id>\n");
}
