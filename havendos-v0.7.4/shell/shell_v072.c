/*
 * shell_v072.c — HavenDOS v0.7.4 new shell commands
 * Compiled separately and linked in. Keeps shell.c untouched.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/rtc.h"
#include "../include/virtio.h"
#include "../include/theme.h"
#include "../include/errors.h"
void exec_cmd_public(const char *cmd, const char *a1, const char *a2);

/* External config functions (from kernel/config.c) */
extern int  cfg_screensaver_timeout(void);
extern void cfg_set_screensaver_timeout(int v);
extern void cfg_save(void);



/* External helpers from shell.c */
extern void make_path(char *out, const char *in);
extern int  vfs_read(const char *path, char *buf, int maxlen);
extern int  vfs_write(const char *path, const char *buf, int len);
extern int  vfs_exists(const char *path);
extern void cfg_save(void);

/* ── MOTD ─────────────────────────────────────────────────────── */
void cmd_motd_v2(const char *arg1, const char *arg2) {
    if (arg1[0] && !strcmp(arg1, "set")) {
        if (!arg2[0]) { vga_puts("Usage: motd set <message>\n"); return; }
        vfs_write("ETC/MOTD.TXT", arg2, strlen(arg2));
        vga_puts("MOTD updated.\n");
        return;
    }
    char buf[512];
    int r = vfs_read("ETC/MOTD.TXT", buf, 511);
    if (r > 0) { buf[r] = 0; vga_puts(buf); vga_putchar('\n'); }
    else vga_puts("No MOTD set. Use: motd set <message>\n");
}

/* ── DF ───────────────────────────────────────────────────────── */
void cmd_df_v2(void) {
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("Filesystem       Size      Notes\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("------------------------------------------\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    if (virtio_blk_ready()) {
        uint32_t secs = (uint32_t)virtio_blk_sectors();
        uint32_t total_mb = secs / 2048;
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_printf("/dev/vda         %4dMB    VirtIO block\n", total_mb);
        vga_set_color(VGA_WHITE, VGA_BLACK);
    } else {
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("/dev/vda         (not detected)\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
    }
    vga_puts("ramfs            2MB       in-memory filesystem\n");
}

/* ── NEOFETCH ─────────────────────────────────────────────────── */
void cmd_neofetch_v2(void) {
    vga_clear();
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("   _   _                   ____   ___  ____\n");
    vga_puts("  | | | | __ _ _ __   ___|  _ \\ / _ \\/ ___|\n");
    vga_puts("  | |_| |/ _` | '_ \\ / _ \\ | | | | | \\___ \\\n");
    vga_puts("  |  _  | (_| | |_) |  __/ |_| | |_| |___) |\n");
    vga_puts("  |_| |_|\\__,_|_.__/ \\___|____/ \\___/|____/\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("  HavenDOS v0.7.4  -  TechHaven Studios\n\n");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("System Information\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("------------------\n");

    struct { const char *k; const char *v; } inf[] = {
        { "OS:      ", "HavenDOS v0.7.4"            },
        { "Shell:   ", "HavenDOS Shell"               },
        { "Arch:    ", "x86 32-bit Protected Mode"    },
        { "Net:     ", "RTL8139 / e1000 (QEMU SLIRP)" },
        { "Disk:    ", "VirtIO Block (UTM SE)"        },
        { "Studio:  ", "TechHaven Studios 2026"       },
    };
    int n = (int)(sizeof(inf)/sizeof(inf[0]));
    for (int i = 0; i < n; i++) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        vga_puts(inf[i].k);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        vga_puts(inf[i].v);
        vga_putchar('\n');
    }
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("Theme:   ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts(ui_theme_name(ui_current_theme()));
    vga_putchar('\n');

    vga_putchar('\n');
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("Colors: ");
    for (int c = 0; c < 16; c++)
        vga_putchar_at('#', 8+c, 23, VGA_BLACK, (vga_color_t)c);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_putchar('\n');
}

/* ── SCREENSAVER CONTROL ──────────────────────────────────────── */
void cmd_screensaver_v2(const char *arg1) {
    int t = cfg_screensaver_timeout();
    if (!arg1 || !arg1[0]) {
        if (t <= 0) vga_puts("Screensaver: disabled\n");
        else vga_printf("Screensaver timeout: %d seconds\n", t);
        return;
    }
    if (!strcmp(arg1, "off") || !strcmp(arg1, "0")) {
        cfg_set_screensaver_timeout(0);
        cfg_save();
        vga_puts("Screensaver disabled.\n");
    } else {
        int secs = atoi(arg1);
        if (secs < 5 || secs > 3600) {
            vga_puts("Timeout must be 5-3600 seconds.\n");
            return;
        }
        cfg_set_screensaver_timeout(secs);
        cfg_save();
        vga_printf("Screensaver set to %d seconds.\n", secs);
    }
}

/* ── SOURCE / . — run a BSH script ──────────────────────────── */
void cmd_source_v2(const char *arg1) {
    extern void shell_run_script(const char *filename);
    if (!arg1 || !arg1[0]) {
        vga_puts("Usage: source <script.bsh>  (or: . <script.bsh>)\n");
        return;
    }
    shell_run_script(arg1);
}

/* ── WATCH — repeat a command every N seconds ────────────────── */
extern int  keyboard_getchar(void);

void cmd_watch_v2(const char *interval_s, const char *cmd) {
    if (!interval_s[0] || !cmd[0]) {
        vga_puts("Usage: watch <seconds> <command>\n");
        vga_puts("  ESC or q to stop.\n");
        return;
    }
    int secs = atoi(interval_s);
    if (secs < 1) secs = 1;
    if (secs > 60) secs = 60;

    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_printf("Watching '%s' every %ds  (ESC/q to stop)\n", cmd, secs);
    vga_set_color(VGA_WHITE, VGA_BLACK);

    /* Parse cmd into cmd/arg1/arg2 */
    char cbuf[128]; char a1[64]=""; char a2[64]="";
    strncpy(cbuf, cmd, 127); cbuf[127] = 0;
    char *sp1 = strchr(cbuf, ' ');
    if (sp1) {
        *sp1 = 0;
        char *rest = sp1 + 1;
        char *sp2 = strchr(rest, ' ');
        if (sp2) {
            *sp2 = 0;
            strncpy(a2, sp2+1, 63);
        }
        strncpy(a1, rest, 63);
    }

    uint32_t count = 0;
    while (1) {
        count++;
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_printf("-- watch #%d --\n", (int)count);
        vga_set_color(VGA_WHITE, VGA_BLACK);

        exec_cmd_public(cbuf, a1, a2);

        /* Wait N seconds, polling keyboard */
        for (int t = 0; t < secs * 20; t++) {
            volatile uint32_t d = 200000; while(d--) __asm__ volatile("pause");
            int c = keyboard_getchar();
            if (c == 27 || c == 'q' || c == 'Q') {
                vga_puts("watch: stopped.\n");
                return;
            }
        }
    }
}

/* ── SCRIPT — record terminal session to a file ─────────────── */
extern int  vfs_write(const char *path, const char *buf, int len);
extern int  vfs_read(const char *path, char *buf, int maxlen);
extern void out_char(char c);
extern void out_str(const char *s);

/* We hook into shell output by wrapping: script appends to a buffer
   and the shell read loop writes to it. Since we can't hook at link
   time without modifying shell.c, we implement script as a mini
   interactive recorder that runs commands via exec_cmd_public. */
void cmd_script_v2(const char *filename) {
    const char *fname = (filename && filename[0]) ? filename : "SCRIPT.LOG";
    char fpath[64];
    /* Ensure it goes in home dir */
    strncpy(fpath, fname, 63); fpath[63] = 0;

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_printf("Script recording started -> %s\n", fpath);
    vga_puts("Type commands. Type 'exit' or ESC to stop recording.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);

    static char session[8192];
    int slen = 0;
    /* Write header */
    uint8_t h,m,s2; rtc_get_time(&h,&m,&s2);
    /* write header as fixed string */
    const char *hdr = "--- HavenDOS script started ---\n";
    int hl = strlen(hdr);
    if (slen + hl < 8191) { memcpy(session+slen, hdr, hl); slen += hl; }

    /* Interactive loop */
    while (1) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("[rec]$ ");
        vga_set_color(VGA_WHITE, VGA_BLACK);

        /* Read a line */
        char line[128]; int lpos = 0;
        while (1) {
            int c = -1;
            while (c == -1) c = keyboard_getchar();
            if (c == 27) goto script_done;
            if (c == '\n' || c == '\r') { vga_putchar('\n'); line[lpos] = 0; break; }
            if ((c == '\b' || c == 127) && lpos > 0) {
                lpos--; line[lpos] = 0; vga_putchar('\b');
            } else if (c >= 32 && c < 127 && lpos < 127) {
                line[lpos++] = (char)c; line[lpos] = 0; vga_putchar((char)c);
            }
        }

        if (!strcmp(line, "exit") || !strcmp(line, "quit")) break;
        if (!line[0]) continue;

        /* Log the command */
            /* log: "$ cmd\n" */
        const char *pfx = "$ ";
        int pfxl = strlen(pfx);
        int linel = strlen(line);
        if (slen + pfxl + linel + 1 < 8191) {
            memcpy(session+slen, pfx, pfxl); slen += pfxl;
            memcpy(session+slen, line, linel); slen += linel;
            session[slen++] = '\n';
        }

        /* Parse and run */
        char cbuf[128]; char a1[64]=""; char a2[64]="";
        strncpy(cbuf, line, 127);
        char *sp = strchr(cbuf, ' ');
        if (sp) { *sp=0; strncpy(a1, sp+1, 63); char*sp2=strchr(a1,' '); if(sp2){*sp2=0;strncpy(a2,sp2+1,63);} }
        exec_cmd_public(cbuf, a1, a2);
    }

script_done:
    {
            rtc_get_time(&h,&m,&s2);
        const char *ftr = "--- HavenDOS script ended ---\n";
        int fl = strlen(ftr);
        if (slen + fl < 8191) { memcpy(session+slen, ftr, fl); slen += fl; }
        session[slen] = 0;
        vfs_write(fpath, session, slen);
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_printf("Script saved: %s (%d bytes)\n", fpath, slen);
        vga_set_color(VGA_WHITE, VGA_BLACK);
    }
}

/* ── TIME — time a command's execution ───────────────────────── */
static uint32_t rtc_to_secs(void) {
    uint8_t h=0,m=0,s=0;
    rtc_get_time(&h,&m,&s);
    return (uint32_t)h*3600 + (uint32_t)m*60 + (uint32_t)s;
}

void cmd_time_v2(const char *cmd, const char *a1) {
    if (!cmd || !cmd[0]) { vga_puts("Usage: timer <command> [args]\n"); return; }
    uint32_t t0 = rtc_to_secs();
    exec_cmd_public(cmd, a1, "");
    uint32_t t1 = rtc_to_secs();
    uint32_t elapsed = (t1 >= t0) ? (t1 - t0) : (86400 - t0 + t1);
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_printf("\nreal  %ds\n", (int)elapsed);
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* ── YES — repeat a string (useful in scripts) ───────────────── */
void cmd_yes_v2(const char *str) {
    const char *s = (str && str[0]) ? str : "y";
    vga_puts("(yes loop — press ESC or q to stop)\n");
    int count = 0;
    while (count < 200) {  /* safety cap */
        vga_puts(s); vga_putchar('\n');
        count++;
        int c = keyboard_getchar();
        if (c == 27 || c == 'q' || c == 'Q') break;
    }
}
