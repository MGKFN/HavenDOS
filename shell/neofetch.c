/*
 * neofetch.c -- the system at a glance, from values read a moment ago.
 *
 * The version this replaces described the machine in string literals:
 *
 *     { "Net:     ", "RTL8139 / e1000 (QEMU SLIRP)" },
 *     { "Disk:    ", "VirtIO Block (UTM SE)"        },
 *
 * It printed those on every machine. On a real PC with an IDE disk and no
 * network card it said "VirtIO Block (UTM SE)" and named two NICs that were
 * not there -- which is exactly what the verbose boot log used to do before
 * 0.8 Beta replaced it, and exactly what tools/check_boot_log.py exists to
 * stop. That checker only scans the boot path, so this screen kept doing it
 * for three more releases.
 *
 * Every line below is a value some driver or subsystem has just been asked
 * for. Where the answer is "nothing", the line says so rather than naming
 * hardware that is absent.
 *
 * Layout is the usual neofetch one: logo on the left, keys on the right.
 * It is drawn into an 80x25 grid and clamped there, because every other app
 * in the tree lays out for 80x25 and a screen that assumes more is the bug
 * 13c in the handoff is about.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/version.h"

extern uint32_t    boot_ms(void);
extern uint32_t    pmm_total_blocks(void);
extern uint32_t    pmm_free_blocks(void);
extern int         vga_fb_active(void);
extern uint32_t    vga_fb_width(void);
extern uint32_t    vga_fb_height(void);
extern int         virtio_blk_ready(void);
extern uint64_t    virtio_blk_sectors(void);
extern int         ata_detected(void);
extern uint32_t    ata_sectors(void);
extern int         ata_lba48(void);
extern int         nvme_count(void);
extern uint64_t    nvme_sectors(int);
extern int         vfs_using_disk(void);
extern int         vfs_using_fat16(void);
extern int         vfs_using_fat32(void);
extern uint32_t    fat32_total_clusters(void);
extern uint32_t    fat32_cluster_bytes(void);
extern int         rtl8139_found(void);
extern int         e1000_found(void);
extern int         net_ready(void);
extern uint32_t    net_get_ip(void);
extern int         bpkg_installed_count(void);
extern const char *current_username(void);
extern const char *ui_theme_name(int n);
extern int         ui_current_theme(void);
extern int         boot_is_live(void);

/* The monogram. Twenty columns, so the key column starts at 22 and the
   longest value still lands inside eighty. */
static const char *LOGO[] = {
    "  #######  #######  ",
    "  #######  #######  ",
    "  #######  #######  ",
    "  #######  #######  ",
    "  ################  ",
    "  ################  ",
    "  #######  #######  ",
    "  #######  #######  ",
    "  #######  #######  ",
};
#define LOGO_ROWS ((int)(sizeof(LOGO)/sizeof(LOGO[0])))
#define KEY_X 22

/* ── small formatting helpers ───────────────────────────────────────
   No snprintf in this kernel, and vga_printf writes at the cursor rather
   than at a position, so values are assembled into a buffer and placed. */
static void app_s(char *d, int *n, int max, const char *s){
    while(s && *s && *n < max - 1) d[(*n)++] = *s++;
    d[*n] = 0;
}
static void app_u(char *d, int *n, int max, uint32_t v){
    char t[12]; int k = 0;
    if(!v){ if(*n < max-1) d[(*n)++] = '0'; d[*n] = 0; return; }
    while(v && k < 11){ t[k++] = (char)('0' + (v % 10)); v /= 10; }
    while(k-- && *n < max - 1) d[(*n)++] = t[k];
    d[*n] = 0;
}

/* The CPU brand string, straight out of CPUID 0x80000002..4.  Absent on a
   486 or an early Pentium, in which case the vendor from leaf 0 is what
   there is and the line says that instead of inventing a model. */
static void cpu_name(char *out, int max)
{
    uint32_t a, b, c, d;
    int n = 0;
    out[0] = 0;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u));
    if(a >= 0x80000004u){
        for(uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++){
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf));
            uint32_t r[4] = { a, b, c, d };
            for(int i = 0; i < 4; i++)
                for(int k = 0; k < 4; k++){
                    char ch = (char)((r[i] >> (8*k)) & 0xFF);
                    if(ch && n < max - 1) out[n++] = ch;
                }
        }
        out[n] = 0;
        /* The brand string is padded with leading spaces on most parts. */
        int s = 0; while(out[s] == ' ') s++;
        if(s){ int k = 0; while(out[s]) out[k++] = out[s++]; out[k] = 0; }
        if(out[0]) return;
    }
    /* No brand string: the twelve-byte vendor id is the honest fallback. */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0u));
    uint32_t v[3] = { b, d, c };
    for(int i = 0; i < 3; i++)
        for(int k = 0; k < 4; k++){
            char ch = (char)((v[i] >> (8*k)) & 0xFF);
            if(ch && n < max - 1) out[n++] = ch;
        }
    out[n] = 0;
    if(!out[0]) app_s(out, &n, max, "unknown");
}

/* Megabytes from a 512-byte sector count, by shift: a freestanding kernel
   does not link libgcc, so a 64-bit divide is a link error. */
static uint32_t mb_of_sectors(uint64_t s){ return (uint32_t)(s >> 11); }

static int g_row;
static void line(const char *key, const char *val, vga_color_t keyc)
{
    if(g_row >= vga_rows() - 1) return;
    if(key) vga_puts_at(key, KEY_X, g_row, keyc, VGA_BLACK);
    if(val) vga_puts_at(val, KEY_X + 11, g_row, VGA_WHITE, VGA_BLACK);
    g_row++;
}

void cmd_neofetch_v2(void)
{
    char buf[80];
    int  n;
    vga_color_t acc = VGA_LIGHT_CYAN;

    vga_clear();

    /* ── the monogram, drawn twice: a darker copy one cell down and right,
          then the body over it.  Two passes of the same string is the whole
          trick, and it is the difference between a flat block of hashes and
          something that reads as a mark. ── */
    for(int i = 0; i < LOGO_ROWS && i + 2 < vga_rows(); i++)
        vga_puts_at(LOGO[i], 1, i + 2, VGA_CYAN, VGA_BLACK);
    for(int i = 0; i < LOGO_ROWS && i + 1 < vga_rows(); i++)
        vga_puts_at(LOGO[i], 0, i + 1, acc, VGA_BLACK);

    /* ── user@host, and the rule under it ── */
    g_row = 1;
    n = 0;
    app_s(buf, &n, sizeof buf, current_username());
    app_s(buf, &n, sizeof buf, "@havendos");
    vga_puts_at(buf, KEY_X, g_row++, acc, VGA_BLACK);
    {
        int w = (int)strlen(buf);
        char rule[64]; int k = 0;
        while(k < w && k < 63) rule[k++] = '-';
        rule[k] = 0;
        vga_puts_at(rule, KEY_X, g_row++, VGA_DARK_GREY, VGA_BLACK);
    }

    /* ── OS: the one place the codename belongs ── */
    { extern const char *ver_display(void); line("OS", ver_display(), acc); }   /* v0.9.7 */
    line("Kernel", "x86 32-bit protected mode", acc);

    /* ── uptime ── */
    {
        uint32_t ms = boot_ms(), s = ms / 1000u;
        n = 0;
        if(s >= 3600u){ app_u(buf, &n, sizeof buf, s / 3600u); app_s(buf, &n, sizeof buf, "h "); s %= 3600u; }
        if(s >= 60u || n){ app_u(buf, &n, sizeof buf, s / 60u); app_s(buf, &n, sizeof buf, "m "); s %= 60u; }
        app_u(buf, &n, sizeof buf, s);
        app_s(buf, &n, sizeof buf, "s");
        line("Uptime", buf, acc);
    }

    /* ── CPU, from CPUID ── */
    {
        char cpu[52];
        cpu_name(cpu, sizeof cpu);
        line("CPU", cpu, acc);
    }

    /* ── memory, from the physical allocator ── */
    {
        uint32_t tot = pmm_total_blocks(), fre = pmm_free_blocks();
        uint32_t used_mb = ((tot - fre) * 4u) / 1024u;      /* 4 KB blocks */
        uint32_t tot_mb  = (tot * 4u) / 1024u;
        n = 0;
        app_u(buf, &n, sizeof buf, used_mb);
        app_s(buf, &n, sizeof buf, " / ");
        app_u(buf, &n, sizeof buf, tot_mb);
        app_s(buf, &n, sizeof buf, " MB");
        line("Memory", buf, acc);
    }

    /* ── display: the framebuffer the firmware handed over, and the grid
          the console settled on inside it ── */
    {
        n = 0;
        if(vga_fb_active()){
            app_u(buf, &n, sizeof buf, vga_fb_width());
            app_s(buf, &n, sizeof buf, "x");
            app_u(buf, &n, sizeof buf, vga_fb_height());
            app_s(buf, &n, sizeof buf, " fb, ");
        } else {
            app_s(buf, &n, sizeof buf, "EGA text, ");
        }
        app_u(buf, &n, sizeof buf, (uint32_t)vga_cols());
        app_s(buf, &n, sizeof buf, "x");
        app_u(buf, &n, sizeof buf, (uint32_t)vga_rows());
        line("Display", buf, acc);
    }

    /* ── disk and filesystem: v0.9.5 asks the block layer which disk the
          system runs from, and the volume how full it is. The old lines
          checked VirtIO, then ATA, then NVMe ("read-only", which stopped
          being true in 0.8.8), and never knew AHCI or USB existed. ── */
    {
        extern int blk_boot_dev(void);
        extern const char *blk_bus_name(int);
        extern uint64_t blk_sectors(int);
        extern int blk_count(void);
        extern const char *sys_volume_space(uint32_t*, uint32_t*);
        int b = blk_boot_dev();
        n = 0;
        if(boot_is_live())        app_s(buf, &n, sizeof buf, "live session -- no disk used");
        else if(b >= 0){
            app_s(buf, &n, sizeof buf, blk_bus_name(b));
            app_s(buf, &n, sizeof buf, " ");
            app_u(buf, &n, sizeof buf, mb_of_sectors(blk_sectors(b)));
            app_s(buf, &n, sizeof buf, " MB");
        } else if(blk_count() > 0){
            app_u(buf, &n, sizeof buf, (uint32_t)blk_count());
            app_s(buf, &n, sizeof buf, " disk(s), none is the system disk");
        } else app_s(buf, &n, sizeof buf, "none found");
        line("Disk", buf, acc);

        uint32_t tk = 0, fk = 0;
        const char *fs = sys_volume_space(&tk, &fk);
        n = 0;
        if(!fs) app_s(buf, &n, sizeof buf, boot_is_live() ? "RAM only, nothing persists" : "none mounted");
        else{
            app_s(buf, &n, sizeof buf, fs);
            app_s(buf, &n, sizeof buf, ", ");
            if(fk != 0xFFFFFFFFu){
                app_u(buf, &n, sizeof buf, (tk - fk) / 1024u);
                app_s(buf, &n, sizeof buf, " / ");
            }
            app_u(buf, &n, sizeof buf, tk / 1024u);
            app_s(buf, &n, sizeof buf, " MB");
        }
        line("Filesystem", buf, acc);
    }

    /* ── network: the card that answered, and the address it holds ── */
    {
        n = 0;
        /* v0.9.5: through the netdev layer, so a USB adapter or a tethered
           iPhone is named too, not reported as "no NIC". */
        extern int netdev_count(void);
        extern const char *netdev_bus_name(int);
        if(netdev_count() > 0) app_s(buf, &n, sizeof buf, netdev_bus_name(0));
        else                   app_s(buf, &n, sizeof buf, "no network adapter");
        if(netdev_count() > 0){
            uint32_t ip = net_get_ip();
            app_s(buf, &n, sizeof buf, net_ready() ? ", " : ", link down, ");
            app_u(buf, &n, sizeof buf,  ip        & 0xFF); app_s(buf, &n, sizeof buf, ".");
            app_u(buf, &n, sizeof buf, (ip >>  8) & 0xFF); app_s(buf, &n, sizeof buf, ".");
            app_u(buf, &n, sizeof buf, (ip >> 16) & 0xFF); app_s(buf, &n, sizeof buf, ".");
            app_u(buf, &n, sizeof buf, (ip >> 24) & 0xFF);
        }
        line("Network", buf, acc);
    }

    /* ── packages ── */
    {
        n = 0;
        app_u(buf, &n, sizeof buf, (uint32_t)bpkg_installed_count());
        app_s(buf, &n, sizeof buf, " installed");
        line("Packages", buf, acc);
    }

    line("Shell", "HavenDOS Shell", acc);
    line("Theme", ui_theme_name(ui_current_theme()), acc);

    /* ── the sixteen-colour swatch, under the keys, inside the grid ── */
    g_row++;
    if(g_row < vga_rows() - 1){
        for(int c = 0; c < 8 && KEY_X + c*2 + 1 < vga_cols(); c++){
            vga_putchar_at(' ', KEY_X + c*2,     g_row, VGA_BLACK, (vga_color_t)c);
            vga_putchar_at(' ', KEY_X + c*2 + 1, g_row, VGA_BLACK, (vga_color_t)c);
        }
        g_row++;
        for(int c = 0; c < 8 && KEY_X + c*2 + 1 < vga_cols(); c++){
            vga_putchar_at(' ', KEY_X + c*2,     g_row, VGA_BLACK, (vga_color_t)(c + 8));
            vga_putchar_at(' ', KEY_X + c*2 + 1, g_row, VGA_BLACK, (vga_color_t)(c + 8));
        }
        g_row++;
    }

    vga_set_cursor(0, g_row < vga_rows() - 1 ? g_row + 1 : vga_rows() - 1);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_flush();
}
