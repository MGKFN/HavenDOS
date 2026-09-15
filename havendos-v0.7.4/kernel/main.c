#include "../include/io.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/gfx.h"
#include "../include/virtio.h"

void idt_init(void);
void irq_enable_basic(void);
void irq_enable_mouse(void);
void pit_init(uint32_t hz);
void pmm_init(uint32_t mem_size);
void heap_init(void);
void keyboard_init(void);
void mouse_init(void);
void ramfs_init(void);
void shell_init(void);
void desktop_run(void);
void login_screen(void);
void boot_animation(void);
void shell_run(void);
void mode13_desktop_run(void);
void vesa_desktop_run(void);
void vesa_set_framebuffer(uint32_t,uint32_t,uint32_t,uint32_t);
extern uint32_t get_ticks(void);

gfx_mode_t current_gfx = GFX_TEXT;

#define MB1_LOADER_MAGIC  0x2BADB002u
#define MB2_LOADER_MAGIC  0x36d76289u

/* ── Per-user home directory setup ───────────────────────────────
   Called from login.c after each successful login.
   Creates HOME/<username>/ and populates it with starter files
   the first time that user logs in.  Subsequent logins are no-ops. */
void setup_user_home(const char *username)
{
    extern int  ramfs_write(const char*,const char*,uint32_t);
    extern int  ramfs_mkdir(const char*);
    extern int  vfs_exists(const char*);

    /* Build HOME/<username> path */
    char homedir[48];
    strncpy(homedir, "HOME/", 5);
    strncat(homedir, username, sizeof(homedir)-6);
    homedir[sizeof(homedir)-1] = 0;

    /* Already exists — user has logged in before, leave files alone */
    if(vfs_exists(homedir)) return;

    ramfs_mkdir(homedir);

    /* NOTES.TXT — editable notes file */
    char notes_path[64];
    strncpy(notes_path, homedir, sizeof(notes_path)-16);
    strncat(notes_path, "/NOTES.TXT", sizeof(notes_path)-strlen(notes_path)-1);

    char notes[256];
    strncpy(notes, "HavenDOS Notes - ", sizeof(notes)-1);
    strncat(notes, username, sizeof(notes)-strlen(notes)-1);
    strncat(notes, "\r\n--------------\r\nEdit this file with: edit ~/NOTES.TXT\r\n", sizeof(notes)-strlen(notes)-1);
    ramfs_write(notes_path, notes, (uint32_t)strlen(notes));

    /* HELLO.BOOT — starter BOOT language script */
    char boot_path[64];
    strncpy(boot_path, homedir, sizeof(boot_path)-16);
    strncat(boot_path, "/HELLO.BOOT", sizeof(boot_path)-strlen(boot_path)-1);

    char boot_src[256];
    strncpy(boot_src, "rem Hello from BOOT Language\r\nvar name = \"", sizeof(boot_src)-1);
    strncat(boot_src, username, sizeof(boot_src)-strlen(boot_src)-1);
    strncat(boot_src, "\"\r\nprintln \"Hello, \" + name + \"!\"\r\nwait 1000\r\n", sizeof(boot_src)-strlen(boot_src)-1);
    ramfs_write(boot_path, boot_src, (uint32_t)strlen(boot_src));
}

typedef struct {
    uint32_t flags,mem_lower,mem_upper,boot_device,cmdline,
             mods_count,mods_addr,syms[4],mmap_length,mmap_addr,
             drives_length,drives_addr,config_table,boot_loader,
             apm_table,vbe_control,vbe_mode_info,vbe_mode,
             vbe_seg,vbe_off,vbe_len,
             fb_addr_lo,fb_addr_hi,fb_pitch,fb_width,fb_height,
             fb_bpp,fb_type;
} __attribute__((packed)) mb1_info_t;

typedef struct{uint32_t type,size;}__attribute__((packed))mb2_tag_t;
typedef struct{uint32_t type,size;char str[1];}__attribute__((packed))mb2_cmd_t;

typedef enum{MODE_DESKTOP=0,MODE_SHELL,MODE_SAFE,MODE_LOWRES}boot_mode_t;
static boot_mode_t boot_mode=MODE_DESKTOP;
static uint32_t    ram_bytes=32u*1024u*1024u;
static int         mouse_enabled=1; /* default ON; mode=safe disables it */
static int         force_install=0;  /* set by install=1 on cmdline */
static int         gfx_flip_x=0, gfx_flip_y=0;

static void busy_wait(volatile uint32_t n){while(n--)__asm__ volatile("nop");}

static void apply_cmdline(const char *s){
    if(!s||!s[0])return;
    if(strstartwith(s,"mode=shell"))       boot_mode=MODE_SHELL;
    else if(strstartwith(s,"mode=safe"))   boot_mode=MODE_SAFE;
    else if(strstartwith(s,"mode=lowres")) boot_mode=MODE_LOWRES;
    else if(strstartwith(s,"mode=desktop"))boot_mode=MODE_DESKTOP;
    if(strstartwith(s,"mouse=on"))         mouse_enabled=1;
    else if(strstartwith(s,"mouse=off"))   mouse_enabled=0;
    if(strstartwith(s,"install=1"))        force_install=1;
    /* Graphics mode */
    if(strstartwith(s,"gfx=mode13"))       current_gfx=GFX_MODE13;
    else if(strstartwith(s,"gfx=vesa640")) current_gfx=GFX_VESA640;
    else if(strstartwith(s,"gfx=vesa800")) current_gfx=GFX_VESA800;
    else if(strstartwith(s,"gfx=text"))    current_gfx=GFX_TEXT;
    if(strstartwith(s,"flip=xy")){gfx_flip_x=1;gfx_flip_y=1;}
    else if(strstartwith(s,"flip=x"))gfx_flip_x=1;
    else if(strstartwith(s,"flip=y"))gfx_flip_y=1;
    /* also check after space */
    const char *p=s; while(*p&&*p!=' ')p++; while(*p==' ')p++;
    if(*p) apply_cmdline(p);
}

static void parse_mb1(uint32_t addr){
    if(addr<0x500||addr>0x8000000)return;
    mb1_info_t *mb=(mb1_info_t*)addr;
    if(mb->flags&1){
        /* mem_upper is KB above 1MB — allow up to 4GB (0xFFFFF KB) */
        uint32_t kb=mb->mem_upper+1024;
        if(kb>1024) ram_bytes=(uint64_t)kb*1024 > 0xFFFFFFFFULL
                              ? 0xFFFFFFFF : (uint32_t)((uint64_t)kb*1024);
    }
    /* Use memory map if available — more accurate */
    if((mb->flags&(1<<6))&&mb->mmap_addr&&mb->mmap_length){
        uint32_t highest=0;
        uint8_t *p=(uint8_t*)mb->mmap_addr;
        uint8_t *end=p+mb->mmap_length;
        while(p<end){
            uint32_t entry_sz=*(uint32_t*)p;
            uint32_t type=*(uint32_t*)(p+16);
            uint64_t base=*(uint64_t*)(p+4);
            uint64_t len =*(uint64_t*)(p+12);
            if(type==1&&base<0x100000000ULL){ /* type 1 = usable RAM */
                uint64_t top=base+len;
                if(top>0x100000000ULL) top=0x100000000ULL;
                if((uint32_t)top>highest) highest=(uint32_t)top;
            }
            p+=entry_sz+4;
        }
        if(highest>1024*1024) ram_bytes=highest;
    }
    if((mb->flags&4)&&mb->cmdline&&mb->cmdline<0x8000000)
        apply_cmdline((const char*)mb->cmdline);
    /* Check for framebuffer (bit 12) */
    if((mb->flags&(1<<12))&&mb->fb_addr_lo&&mb->fb_width){
        vesa_set_framebuffer(mb->fb_addr_lo,mb->fb_width,mb->fb_height,mb->fb_pitch);
    }
}

static void parse_mb2(uint32_t addr){
    if(!addr||(addr&7)||addr>0x8000000)return;
    uint32_t total=*(uint32_t*)addr; if(total<8||total>65536)return;
    mb2_tag_t *tag=(mb2_tag_t*)(addr+8);
    while((uint32_t)tag<addr+total&&tag->type!=0){
        if(tag->type==1&&tag->size>8) apply_cmdline(((mb2_cmd_t*)tag)->str);
        if(tag->type==6) ram_bytes=64u*1024u*1024u;
        uint32_t n=(tag->size+7)&~7u; if(!n)break;
        tag=(mb2_tag_t*)((uint8_t*)tag+n);
    }
}

static void create_sysfiles(void){
    extern int ramfs_write(const char*,const char*,uint32_t);
    extern int ramfs_mkdir(const char*);
    extern int ramfs_chmod(const char*,int);

    /* ── Root files ─────────────────────────────────────── */
    const char *readme =
        "HavenDOS v0.7.4\r\n"
        "TechHaven Studios\r\n"
        "Type 'help' in the shell for a command list.\r\n"
        "Type 'guide' for the built-in user guide.\r\n";
    ramfs_write("README.TXT", readme, strlen(readme));

    const char *changelog =
        "v0.7.4 - Anti-flicker VGA shadow buffer, SATA/LBA48 ATA, faster PS/2 mouse (200sps), PC optimisations\r\n"
        "v0.6.3 - Final stabilization: TCP checksum fix, heap magic, DNS/FAT loop guards\r\n"
        "v0.6.2 - Security: IPv4 checksum verify, UDP length fix, TCP ACK/window, heap double-free\r\n"
        "v0.6.1 - Major: ISO install-only boot, arrow-key installer, shell history, autologin timeout\r\n"
        "v0.5.9.19 - BOOT language IDE + boot shell cmd + 10 extra themes via bpkg\r\n"
        "v0.5.9.18 - Bug fixes: verbose boot, reminder alerts, notif count, theme preview\r\n"
        "v0.5.9.17 - bpkg package manager + per-theme wallpapers + notification center\r\n"
        "v0.5.9.16 - Desktop polish: live clock, reminders, calculator, real POST screen\r\n"
        "v0.5.9.15 - Win11-style login (theme bg, user tiles, live clock)\r\n"
        "v0.5.9.14 - ISO-based update command + rollback\r\n"
        "v0.5.9.13 - FAT16 disk subsystem (installable HavenDOS)\r\n"
        "v0.5.9.11 - VirtIO block driver (UTM SE disk I/O)\r\n"
        "v0.5.9.10 - File Manager mouse + start menu hover\r\n"
        "v0.5.9.6 - Desktop: per-theme bg, pipes screensaver, dashboard, 2GB RAM\r\n"
        "v0.5.9.4 - Shell: PS1 prompt, fortune, banner, tab-complete overhaul\r\n"
        "v0.5.8   - Polish: RTC date on taskbar, sysinfo, shutdown\r\n"
        "v0.5.6   - Networking: RTL8139, ARP, ICMP ping, UDP send\r\n"
        "v0.5.3   - Shell: touch, wc, head, tail, alias, tab-complete\r\n";
    ramfs_write("CHANGELOG.TXT", changelog, strlen(changelog));

    const char *welcome =
        "Welcome to HavenDOS!\r\n"
        "Press ESC for the Start Menu.\r\n"
        "Open a terminal from the Start Menu to use the shell.\r\n";
    ramfs_write("WELCOME.TXT", welcome, strlen(welcome));

    /* ── ETC — system configuration ─────────────────────── */
    ramfs_mkdir("ETC");

    const char *motd =
        "HavenDOS v0.7.4 - TechHaven Studios\r\n"
        "Type 'help' for commands. Type 'guide' for the user guide.\r\n"
        "v0.7.4: VGA flicker fix, SATA+LBA48 disk support, 200sps mouse, all v0.7.3 commands inherited\r\n";
    ramfs_write("ETC/MOTD.TXT", motd, strlen(motd));

    const char *hosts =
        "127.0.0.1  localhost\r\n"
        "10.0.2.2   gateway\r\n"
        "10.0.2.15  bootos\r\n";
    ramfs_write("ETC/HOSTS.TXT", hosts, strlen(hosts));

    const char *sysconf =
        "OS=HavenDOS\r\n"
        "VERSION=0.6.1\r\n"
        "ARCH=x86-32\r\n"
        "BOOT=GRUB2-Multiboot1\r\n"
        "FS=RAMFS\r\n"
        "DISPLAY=VGA-80x25\r\n";
    ramfs_write("ETC/SYSCONF.TXT", sysconf, strlen(sysconf));

    /* SECURITY FIX: do not store plaintext credentials in any file.
       Passwords are stored as djb2 hashes in login.c (login_init).
       This file exists only as a user reference; no actual secrets here. */
    const char *passwd =
        "# HavenDOS user database\r\n"
        "# Passwords are stored as hashes in the login subsystem.\r\n"
        "# Use 'passwd' in the shell to change your password.\r\n"
        "admin::1\r\n"
        "bootos::0\r\n"
        "guest::0\r\n";
    ramfs_write("ETC/PASSWD.TXT", passwd, strlen(passwd));
    ramfs_chmod("ETC/PASSWD.TXT", 1);  /* admin only */

    /* ── BIN — built-in command reference ───────────────── */
    ramfs_mkdir("BIN");

    const char *cmdlist =
        "Shell built-in commands (v0.7.4):\r\n"
        "  Files:   ls tree cat write touch mkdir cd pwd del cp mv find grep wc\r\n"
        "  Shell:   echo set env alias history run which calc repeat seq\r\n"
        "  System:  sysinfo mem uptime disk dmesg ps ver date time\r\n"
        "  Users:   whoami users passwd adduser deluser chmod chown log\r\n"
        "  Network: ifconfig ping netstat udpsend\r\n"
        "  Fun:     matrix stars pipes snake guide man help theme fortune\r\n"
        "Type 'man <cmd>' for details. Type 'guide' for the full guide.\r\n";
    ramfs_write("BIN/COMMANDS.TXT", cmdlist, strlen(cmdlist));

    /* ── HOME — per-user home directories ───────────────── */
    /* Root HOME dir. Each user gets HOME/<username>/ created at first login.
       setup_user_home() is called from login.c after a successful login. */
    ramfs_mkdir("HOME");

    /* ── VAR — runtime logs ──────────────────────────────── */
    ramfs_mkdir("VAR");
    { static const char sl[] = "HavenDOS v0.7.4 boot OK\r\n";
      ramfs_write("VAR/SYSLOG.TXT", sl, strlen(sl)); }
    ramfs_chmod("VAR/SYSLOG.TXT", 1);  /* admin only */

    /* Hidden audit log */
    ramfs_write(".loginlog","",0);
    ramfs_chmod(".loginlog",1);
}

static void boot_screen(const char *lbl, const char *gfxlbl, uint32_t magic){
    vga_clear();
    vga_puts_at("  ____   ___   ___ _____    ___  ____  ", 20,4,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at(" | __ ) / _ \\ / _ \\_   _|  / _ \\/ ___| ",20,5,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at(" |  _ \\| | | | | | || |   | | | \\___ \\ ",20,6,VGA_CYAN,VGA_BLACK);
    vga_puts_at(" | |_) | |_| | |_| || |   | |_| |___) |",20,7,VGA_CYAN,VGA_BLACK);
    vga_puts_at(" |____/ \\___/ \\___/ |_|    \\___/|____/ ", 20,8,VGA_BLUE,VGA_BLACK);
    vga_puts_at("v0.7.4",               34,10,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Mode: ",   28,12,VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts_at(lbl,        34,12,VGA_LIGHT_GREEN,VGA_BLACK);
    vga_puts_at("Graphics: ",28,13,VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts_at(gfxlbl,     38,13,VGA_YELLOW,     VGA_BLACK);
    char mb[12]; utoa(magic,mb,16);
    vga_puts_at("Magic: 0x",28,14,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at(mb,        37,14,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("[",19,16,VGA_WHITE,VGA_BLACK);
    vga_puts_at("]",60,16,VGA_WHITE,VGA_BLACK);
    for(int i=0;i<41;i++){
        vga_putchar_at('=',20+i,16,VGA_LIGHT_GREEN,VGA_BLACK);
        busy_wait(300000);
    }
    vga_puts_at("Starting...",20,18,VGA_LIGHT_GREEN,VGA_BLACK);
    busy_wait(800000);
}

/* Kernel location — used by installer to copy the running ELF to disk */
uint32_t g_kernel_load_addr = 0x00100000;
uint32_t g_kernel_size      = 0; /* set at runtime from linker symbol */

void kernel_main(uint32_t magic, uint32_t mb_info){
    /* Calculate kernel size from linker symbol */
    extern uint32_t _kernel_end;
    g_kernel_size = (uint32_t)&_kernel_end - g_kernel_load_addr;
    vga_init();

    /* Show a minimal "starting" banner while hardware inits silently */
    vga_puts_at("HavenDOS - Starting...",0,0,VGA_WHITE,VGA_BLUE);

    idt_init();
    pit_init(100);
    irq_enable_basic();

    if(magic==MB1_LOADER_MAGIC)       parse_mb1(mb_info);
    else if(magic==MB2_LOADER_MAGIC)  parse_mb2(mb_info);
    else{ parse_mb1(mb_info); parse_mb2(mb_info); }

    pmm_init(ram_bytes);
    heap_init();
    keyboard_init();
    if(mouse_enabled&&boot_mode!=MODE_SAFE){ mouse_init(); irq_enable_mouse(); }
    ramfs_init();
    /* Disk init — PCI scan, then VirtIO (UTM SE), then ATA fallback */
    extern void pci_init(void);
    extern void ata_init(void);
    extern int  fat12_init(void);
    extern int  fat16_init(void);
    extern void vfs_init(void);
    extern int  virtio_blk_init(void);
    pci_init();
    virtio_blk_init();   /* VirtIO block — primary disk on UTM SE (QEMU TCG) */
    ata_init();          /* ATA PIO fallback — no-ops on UTM SE (0x1F0 = 0xFF) */
    fat16_init();        /* FAT16 on VirtIO partition — real persistent disk */
    fat12_init();        /* FAT12 on ATA — fallback for other targets */
    vfs_init();
    extern void ui_load_theme(void);
    extern void cfg_load(void);
    extern void run_setup_wizard(void);
    cfg_load();          /* load config BEFORE boot_animation so verbose/POST flags are respected */
    ui_load_theme();

    /* boot_animation runs POST and verbose boot log based on saved config */
    boot_animation();
    extern void net_init(void);
    net_init();
    /* DHCP — auto-configure IP/mask/gw/dns. Falls back to static
       10.0.2.15/24 gw 10.0.2.2 if DHCP server doesn't respond.      */
    if(net_ready()){
        extern int dhcp_request(void);
        dhcp_request();   /* ~4s timeout max; non-blocking on failure */
    }
    create_sysfiles();
    shell_init();

    extern int  vfs_exists(const char*);
    extern void login_welcome(const char*, int);
    extern void reminders_load(void);

    /* ── Disk probe + boot decision ────────────────────────────────
       v0.7.4: bus-agnostic disk detection.
       Probes VirtIO then ATA/SATA.  Three outcomes:

       A) Disk found AND has a valid HavenDOS FAT16 partition
          → boot straight to desktop (normal case after install).

       B) Disk(s) found but unformatted / blank
          → go to installer (new install).

       C) No disk at all
          → go to installer which shows the "no disk" error screen.

       If force_install flag is set (GRUB menu "Reinstall" entry)
       always go to installer regardless.

       Disk picker: if multiple disks found and none is already
       formatted, show a simple list and let the user pick.
       If exactly one disk found, auto-select it silently.
    ─────────────────────────────────────────────────────────────── */
    extern int  vfs_using_fat16(void);
    extern void iso_install_wizard(void);

    if(!vfs_using_fat16() || force_install){
        /* fat16_init() (called earlier in vfs_init) already tried
           both buses.  If it failed, we're running from ISO with no
           formatted HavenDOS partition — go to installer.            */
        iso_install_wizard();
        vga_clear();
        for(int y=0;y<25;y++) for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLUE);
        vga_puts_at("Installation cancelled or failed.",23,11,VGA_LIGHT_GREY,VGA_BLUE);
        vga_puts_at("Remove the ISO and reboot to try again.",21,13,VGA_DARK_GREY,VGA_BLUE);
        for(;;) __asm__ volatile("hlt");
    }

    reminders_load();

    /* First-boot after fresh install: run post-install setup wizard */
    int first_boot = !cfg_setup_done();

    vga_clear();

    /* Apply flip settings for Mode13h */
    mode13_set_flip(gfx_flip_x, gfx_flip_y);

    /* Helper: run desktop in chosen graphics mode with fallback */
    #define RUN_DESKTOP() do { \
        if(current_gfx==GFX_MODE13){ \
            if(mode13_try_enter()) mode13_desktop_run(); \
            else { vga_init(); vga_puts_at("Mode13h failed - text fallback",0,0,VGA_YELLOW,VGA_BLACK); busy_wait(2000000); desktop_run(); } \
        } else if(current_gfx==GFX_VESA640||current_gfx==GFX_VESA800){ \
            vesa_desktop_run(); \
        } else { \
            desktop_run(); \
        } \
    } while(0)

    switch(boot_mode){
        case MODE_DESKTOP:
            if(first_boot) run_setup_wizard();
            login_screen();
            RUN_DESKTOP();
            break;
        case MODE_SHELL:
            vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
            vga_puts("HavenDOS v0.7.4 Shell\nType 'exit' for desktop.\n\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            shell_run();
            login_screen();
            RUN_DESKTOP();
            break;
        case MODE_SAFE:
            vga_set_color(VGA_YELLOW,VGA_BLACK);
            vga_puts("Safe Mode\n\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            shell_run();
            break;
        case MODE_LOWRES:
            if(mode13_try_enter()) mode13_desktop_run();
            else { vga_init(); desktop_run(); }
            break;
    }

    vga_puts("\nSession ended. Rebooting...\n");
    busy_wait(20000000);
    outb(0x64,0xFE);
    for(;;)__asm__ volatile("cli;hlt");
}
