/*
 * shell_v073.c — HavenDOS v0.7.4 new shell commands
 * base64, hash (SHA-256 simplified), todo, lock, wc standalone, zip (RLE)
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/rtc.h"
#include "../include/virtio.h"
#include "../include/theme.h"
#include "../include/config.h"
#include "../include/errors.h"

extern void make_path(char *out, const char *in);
extern int  vfs_read(const char *path, char *buf, int maxlen);
extern int  vfs_write(const char *path, const char *buf, int len);
extern int  vfs_exists(const char *path);
extern int  vfs_delete(const char *path);
extern void exec_cmd_public(const char *cmd, const char *a1, const char *a2);
extern int  keyboard_getchar(void);
extern int  keyboard_waitchar(void);
extern void sleep_ms(uint32_t ms);
extern int  current_user_admin(void);
extern const char *current_username(void);
extern int  cfg_screensaver_timeout(void);
extern void cfg_set_screensaver_timeout(int v);
extern void cfg_save(void);

/* ── BASE64 ──────────────────────────────────────────────────── */
static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode_buf(const uint8_t *in, int inlen, char *out) {
    int oi = 0;
    for (int i = 0; i < inlen; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i+1 < inlen) v |= (uint32_t)in[i+1] << 8;
        if (i+2 < inlen) v |= (uint32_t)in[i+2];
        out[oi++] = b64_chars[(v>>18)&0x3F];
        out[oi++] = b64_chars[(v>>12)&0x3F];
        out[oi++] = (i+1<inlen) ? b64_chars[(v>>6)&0x3F] : '=';
        out[oi++] = (i+2<inlen) ? b64_chars[(v   )&0x3F] : '=';
    }
    out[oi] = 0;
}

static int b64_decode_buf(const char *in, uint8_t *out) {
    static const int8_t tbl[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };
    int oi = 0;
    for (int i = 0; in[i] && in[i]!='=' && in[i+1]; i+=4) {
        int a=tbl[(uint8_t)in[i]],b=tbl[(uint8_t)in[i+1]];
        int c=(in[i+2]&&in[i+2]!='=')?tbl[(uint8_t)in[i+2]]:-1;
        int d=(in[i+3]&&in[i+3]!='=')?tbl[(uint8_t)in[i+3]]:-1;
        if (a<0||b<0) break;
        out[oi++] = (uint8_t)((a<<2)|(b>>4));
        if (c>=0) out[oi++] = (uint8_t)((b<<4)|(c>>2));
        if (d>=0) out[oi++] = (uint8_t)((c<<6)|d);
    }
    return oi;
}

void cmd_base64_v3(const char *flag, const char *arg) {
    int decode = (!strcmp(flag,"d")||!strcmp(flag,"-d")||!strcmp(flag,"decode"));
    const char *data = decode ? flag : arg;
    if (!strcmp(flag,"-d")||!strcmp(flag,"d")||!strcmp(flag,"decode")) data = arg;
    else data = flag; /* encode mode: flag IS the data */

    if (!data||!data[0]) {
        vga_puts("Usage: base64 <text>          (encode)\n");
        vga_puts("       base64 -d <b64string>  (decode)\n");
        return;
    }

    if (!decode) {
        /* Encode */
        static char out[512];
        b64_encode_buf((const uint8_t*)data, strlen(data), out);
        vga_puts(out); vga_putchar('\n');
    } else {
        /* Decode */
        static uint8_t out[512];
        int n = b64_decode_buf(data, out);
        out[n] = 0;
        vga_puts((char*)out); vga_putchar('\n');
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_printf("(%d bytes decoded)\n", n);
        vga_set_color(VGA_WHITE, VGA_BLACK);
    }
}

/* ── HASH — simple djb2 + display as hex (full SHA-256 not practical here) */
void cmd_hash_v3(const char *arg1, const char *arg2) {
    const char *data = NULL;
    int from_file = 0;
    if (!strcmp(arg1,"-f")||!strcmp(arg1,"file")) {
        from_file = 1; data = arg2;
    } else {
        data = arg1;
    }
    if (!data||!data[0]) {
        vga_puts("Usage: hash <string>\n");
        vga_puts("       hash -f <file>\n");
        return;
    }

    static char fbuf[512];
    const char *src = data;
    int slen;
    if (from_file) {
        char fp[64]; make_path(fp, data);
        int r = vfs_read(fp, fbuf, 511);
        if (r < 0) { vga_printf("hash: %s: not found\n", fp); return; }
        fbuf[r] = 0; src = fbuf; slen = r;
    } else {
        slen = strlen(data);
    }

    /* djb2 hash — simple but deterministic */
    uint32_t h1 = 5381, h2 = 0x811c9dc5;
    for (int i = 0; i < slen; i++) {
        uint8_t c = (uint8_t)src[i];
        h1 = ((h1 << 5) + h1) ^ c;
        h2 = (h2 ^ c) * 0x01000193;
    }
    /* Produce a 64-bit "fingerprint" from two hashes */
    uint32_t h3 = h1 ^ (h2 << 13);
    uint32_t h4 = h2 ^ (h1 >> 7);

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    /* Print as hex string */
    char hex[33];
    for (int i = 0; i < 4; i++) {
        uint32_t v = (i==0)?h1:(i==1)?h2:(i==2)?h3:h4;
        for (int j = 7; j >= 0; j--) {
            hex[i*8+j] = "0123456789abcdef"[v&0xF]; v>>=4;
        }
    }
    hex[32]=0;
    vga_puts(hex); vga_putchar('\n');
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_printf("(djb2x2 fingerprint of %d bytes)\n", slen);
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* ── TODO — persistent task list ────────────────────────────── */
#define TODO_FILE "ETC/TODO.TXT"
#define TODO_MAX  32

static void todo_load(char tasks[][80], int *count) {
    static char buf[2600];
    *count = 0;
    int r = vfs_read(TODO_FILE, buf, 2599);
    if (r <= 0) return;
    buf[r] = 0;
    char *p = buf;
    while (*p && *count < TODO_MAX) {
        char *end = p;
        while (*end && *end != '\n') end++;
        int len = (int)(end - p);
        if (len > 0 && len < 79) {
            memcpy(tasks[*count], p, len);
            tasks[*count][len] = 0;
            (*count)++;
        }
        p = end + (*end ? 1 : 0);
    }
}

static void todo_save(char tasks[][80], int count) {
    static char buf[2600];
    int pos = 0;
    for (int i = 0; i < count && pos < 2590; i++) {
        int l = strlen(tasks[i]);
        memcpy(buf+pos, tasks[i], l);
        pos += l;
        buf[pos++] = '\n';
    }
    buf[pos] = 0;
    vfs_write(TODO_FILE, buf, pos);
}

void cmd_todo_v3(const char *subcmd, const char *arg) {
    static char tasks[TODO_MAX][80];
    int count = 0;
    todo_load(tasks, &count);

    if (!subcmd[0] || !strcmp(subcmd,"list") || !strcmp(subcmd,"ls")) {
        /* List todos */
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        vga_puts("TODO list:\n");
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("----------\n");
        if (count == 0) {
            vga_set_color(VGA_DARK_GREY, VGA_BLACK);
            vga_puts("(empty — add with: todo add <task>)\n");
        } else {
            for (int i = 0; i < count; i++) {
                char prefix[8]; prefix[0]='['; prefix[1]='0'+i+1; prefix[2]=']'; prefix[3]=' '; prefix[4]=0;
                vga_set_color(VGA_YELLOW, VGA_BLACK);
                vga_puts(prefix);
                vga_set_color(VGA_WHITE, VGA_BLACK);
                vga_puts(tasks[i]); vga_putchar('\n');
            }
        }
        vga_set_color(VGA_WHITE, VGA_BLACK);
    } else if (!strcmp(subcmd,"add") || !strcmp(subcmd,"a")) {
        if (!arg[0]) { vga_puts("Usage: todo add <task>\n"); return; }
        if (count >= TODO_MAX) { vga_puts("todo: list full (max 32)\n"); return; }
        strncpy(tasks[count++], arg, 79);
        todo_save(tasks, count);
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_printf("Added [%d]: %s\n", count, arg);
        vga_set_color(VGA_WHITE, VGA_BLACK);
    } else if (!strcmp(subcmd,"done") || !strcmp(subcmd,"del") || !strcmp(subcmd,"rm")) {
        if (!arg[0]) { vga_puts("Usage: todo done <number>\n"); return; }
        int n = atoi(arg) - 1;
        if (n < 0 || n >= count) { vga_printf("todo: invalid item %d\n", n+1); return; }
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_printf("Done: [%d] %s\n", n+1, tasks[n]);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        for (int i = n; i < count-1; i++) strcpy(tasks[i], tasks[i+1]);
        count--;
        todo_save(tasks, count);
    } else if (!strcmp(subcmd,"clear") || !strcmp(subcmd,"clr")) {
        vfs_delete(TODO_FILE);
        vga_puts("TODO list cleared.\n");
    } else {
        vga_puts("Usage: todo [list|add <task>|done <n>|clear]\n");
    }
}

/* ── LOCK — lock the screen ─────────────────────────────────── */
extern void screensaver_run_matrix(void);

void cmd_lock_v3(void) {
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("Screen locked. Press any key and enter password to unlock.\n");
    sleep_ms(500);
    vga_clear();
    /* Show lock screen */
    for (int y = 0; y < 25; y++) {
        for (int x = 0; x < 80; x++) {
            vga_putchar_at(' ', x, y, VGA_BLACK, VGA_BLACK);
        }
    }
    vga_puts_at("HAVENDOS - SCREEN LOCKED", 28, 11, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_at(current_username(), 37, 12, VGA_WHITE, VGA_BLACK);
    vga_puts_at("Press any key...", 32, 14, VGA_DARK_GREY, VGA_BLACK);
    keyboard_waitchar();
    /* Password prompt */
    vga_puts_at("Password: ", 35, 16, VGA_WHITE, VGA_BLACK);
    vga_set_cursor(45, 16);
    char pw[32]; int pi = 0;
    while (1) {
        int c = -1;
        while (c == -1) c = keyboard_getchar();
        if (c == '\n' || c == '\r') { pw[pi]=0; break; }
        if ((c=='\b'||c==127)&&pi>0) { pi--; vga_putchar('\b'); }
        else if (c>=32&&c<127&&pi<31) { pw[pi++]=(char)c; vga_putchar('*'); }
    }
    /* Verify via exec_cmd_public — "lock_verify <user> <pw>" */
    /* Simple approach: use the existing login verify helper from shell */
    extern int shell_verify_password(const char *user, const char *pw);
    int ok = shell_verify_password(current_username(), pw);
    if (ok) {
        vga_clear();
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("Unlocked.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
    } else {
        vga_clear();
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("Wrong password. Locked for 3s.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        sleep_ms(3000);
        cmd_lock_v3();
    }
}

/* ── WC standalone ────────────────────────────────────────────── */
void cmd_wc_v3(const char *path) {
    if (!path||!path[0]) { vga_puts("Usage: wc <file>\n"); return; }
    char fp[64]; make_path(fp, path);
    static char buf[4096];
    int r = vfs_read(fp, buf, 4095);
    if (r < 0) { vga_printf("wc: %s: not found\n", fp); return; }
    buf[r] = 0;
    int lines=0, words=0, chars=r;
    int in_word=0;
    for (int i=0;i<r;i++){
        if (buf[i]=='\n') lines++;
        if (buf[i]==' '||buf[i]=='\n'||buf[i]=='\t') in_word=0;
        else if (!in_word) { words++; in_word=1; }
    }
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_printf("  %6d lines  %6d words  %6d chars  %s\n", lines, words, chars, path);
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* ── ZIP (RLE compression) ────────────────────────────────────── */
void cmd_zip_v3(const char *src, const char *dst_arg) {
    if (!src||!src[0]) {
        vga_puts("Usage: zip <source_file> [dest.z]\n");
        vga_puts("       unzip <file.z> [dest]\n");
        return;
    }
    char sfp[64], dfp[64];
    make_path(sfp, src);
    if (dst_arg&&dst_arg[0]) make_path(dfp, dst_arg);
    else { strncpy(dfp, sfp, 60); strcat(dfp, ".z"); }

    static char inbuf[4096], outbuf[8192];
    int r = vfs_read(sfp, inbuf, 4095);
    if (r < 0) { vga_printf("zip: %s: not found\n", sfp); return; }

    /* Simple RLE: [count][byte] pairs */
    int oi = 0;
    outbuf[oi++] = 'Z'; outbuf[oi++] = 'R'; /* magic */
    for (int i = 0; i < r && oi < 8188; ) {
        uint8_t b = (uint8_t)inbuf[i];
        int cnt = 1;
        while (i+cnt < r && cnt < 255 && (uint8_t)inbuf[i+cnt]==b) cnt++;
        outbuf[oi++] = (char)cnt;
        outbuf[oi++] = (char)b;
        i += cnt;
    }
    vfs_write(dfp, outbuf, oi);
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_printf("zip: %s -> %s  (%d -> %d bytes)\n", sfp, dfp, r, oi);
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

void cmd_unzip_v3(const char *src, const char *dst_arg) {
    if (!src||!src[0]) { vga_puts("Usage: unzip <file.z> [dest]\n"); return; }
    char sfp[64], dfp[64];
    make_path(sfp, src);
    if (dst_arg&&dst_arg[0]) make_path(dfp, dst_arg);
    else {
        strncpy(dfp, sfp, 60);
        /* Remove .z extension */
        int l = strlen(dfp);
        if (l > 2 && dfp[l-2]=='.' && dfp[l-1]=='z') dfp[l-2]=0;
        else strcat(dfp, ".out");
    }

    static char inbuf[8192], outbuf[4096];
    int r = vfs_read(sfp, inbuf, 8191);
    if (r < 0) { vga_printf("unzip: %s: not found\n", sfp); return; }
    if (r < 2 || inbuf[0]!='Z' || inbuf[1]!='R') {
        vga_puts("unzip: not a zip file (missing ZR magic)\n"); return;
    }
    int oi = 0;
    for (int i = 2; i+1 < r && oi < 4090; i+=2) {
        uint8_t cnt = (uint8_t)inbuf[i];
        uint8_t b   = (uint8_t)inbuf[i+1];
        for (int j=0;j<cnt&&oi<4090;j++) outbuf[oi++]=(char)b;
    }
    vfs_write(dfp, outbuf, oi);
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_printf("unzip: %s -> %s  (%d -> %d bytes)\n", sfp, dfp, r, oi);
    vga_set_color(VGA_WHITE, VGA_BLACK);
}
