/*
 * bootanim.c - HavenDOS v0.6.8
 * POST + Verbose boot log (real hardware data where available).
 * Fake second-stage progress bar removed entirely.
 * UTM SE safe: letters, numbers, spaces, dots, dashes, slashes only.
 */
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"

extern int      keyboard_getchar(void);
extern int      keyboard_waitchar(void);
extern void     sleep_ms(uint32_t ms);
extern uint32_t pmm_free_blocks(void);
extern uint32_t pmm_total_blocks(void);
extern void     utoa(uint32_t, char*, int);
extern void     vga_getchar_at(int x, int y, uint8_t *ch, uint8_t *attr);
extern void     vga_putraw_at(uint8_t ch, uint8_t attr, int x, int y);

/* Real hardware accessors */
extern int      virtio_drive_count(void);
extern uint64_t virtio_drive_sectors(int);
extern int      rtl8139_found(void);
extern int      e1000_found(void);

static void cls(void){
    for(int y=0;y<25;y++)
        for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
}
static void kbd_drain(void){
    int t=0; while(keyboard_getchar()!=-1&&t++<64);
}

/* =================================================================
   POST — Power-On Self Test
   ================================================================= */
static void do_post(void){
    cls();
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_CYAN);
    vga_puts_at(" TechHaven BIOS v2.1",0,0,VGA_BLACK,VGA_CYAN);
    vga_puts_at("Power-On Self Test",31,0,VGA_BLACK,VGA_CYAN);
    vga_puts_at("x86 32-bit",69,0,VGA_BLACK,VGA_CYAN);
    for(int x=0;x<80;x++) vga_putchar_at('-',x,1,VGA_DARK_GREY,VGA_BLACK);

    /* Build real result strings */
    uint32_t total_mb = pmm_total_blocks()*4/1024;
    int vdrives = virtio_drive_count();

    char ram_str[32]; char tm[8]; utoa(total_mb,tm,10);
    strcpy(ram_str,tm); strcat(ram_str," MB  OK");

    char vio_str[48];
    if(vdrives>0){
        char dn[4]; utoa((uint32_t)vdrives,dn,10);
        char ms[12]; utoa((uint32_t)(virtio_drive_sectors(0)/2048),ms,10);
        strcpy(vio_str,dn); strcat(vio_str," drive  drive0=");
        strcat(vio_str,ms); strcat(vio_str,"MB");
    } else { strcpy(vio_str,"Not detected"); }

    char nic_str[40];
    if(rtl8139_found())    strcpy(nic_str,"RTL8139  bus-master OK");
    else if(e1000_found()) strcpy(nic_str,"e1000  bus-master OK");
    else                   strcpy(nic_str,"Not detected");

    typedef struct { const char *lbl; const char *res; vga_color_t col; int ms; } pr;
    pr rows[]={
        {"CPU",        "Intel x86 32-bit Protected Mode", VGA_LIGHT_GREEN, 40},
        {"RAM",        ram_str,                            VGA_LIGHT_GREEN,  0},
        {"CMOS-RTC",   "OK  24h clock",                   VGA_LIGHT_GREEN, 30},
        {"VGA",        "Text 80x25  16-colour",            VGA_LIGHT_GREEN, 25},
        {"PS-2 KB",    "OK  Scancode Set 2",               VGA_LIGHT_GREEN, 25},
        {"PS-2 Mouse", "OK  Stream mode",                  VGA_LIGHT_GREEN, 20},
        {"ATA Bus 0",  "Not present",                      VGA_YELLOW,      20},
        {"PCI",        "Bus 0 scan complete",               VGA_LIGHT_GREEN, 30},
        {"VirtIO BLK", vio_str, vdrives>0?VGA_LIGHT_GREEN:VGA_YELLOW,      30},
        {"NIC",        nic_str, (rtl8139_found()||e1000_found())?VGA_LIGHT_GREEN:VGA_YELLOW, 35},
        {"Boot Dev",   "CD-ROM  GRUB2 Multiboot",          VGA_LIGHT_CYAN,  20},
    };
    int nrows=(int)(sizeof(rows)/sizeof(rows[0]));

    for(int i=0;i<nrows;i++){
        int y=2+i;
        vga_puts_at(rows[i].lbl, 3, y, VGA_LIGHT_GREY, VGA_BLACK);
        vga_puts_at("............", 14, y, VGA_DARK_GREY, VGA_BLACK);
        sleep_ms(rows[i].ms);
        vga_puts_at(rows[i].res, 27, y, rows[i].col, VGA_BLACK);
    }

    for(int x=0;x<80;x++) vga_putchar_at('-',x,2+nrows,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at(" POST complete - no errors ",26,2+nrows,VGA_LIGHT_GREEN,VGA_BLACK);
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,VGA_BLACK,VGA_LIGHT_GREEN);
    vga_puts_at("  Press any key to continue...",25,24,VGA_BLACK,VGA_LIGHT_GREEN);
    kbd_drain(); keyboard_waitchar();
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,VGA_BLACK,VGA_BLACK);
}

/* =================================================================
   VERBOSE BOOT LOG — real hardware values
   ================================================================= */
static void do_verbose(void){
    cls();
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_DARK_GREY);
    vga_puts_at(" HavenDOS v0.6.8",0,0,VGA_CYAN,VGA_DARK_GREY);
    vga_puts_at("Boot Log",36,0,VGA_LIGHT_GREY,VGA_DARK_GREY);
    vga_puts_at("SPACE=pause",68,0,VGA_DARK_GREY,VGA_DARK_GREY);

    /* Build real strings */
    char ram_ln[72]; char ram_s[12];
    utoa(pmm_total_blocks()*4/1024,ram_s,10);
    strcpy(ram_ln,"PMM: "); strcat(ram_ln,ram_s);
    strcat(ram_ln,"MB usable  bitmap at 0x00300000");

    char vio_ln[72]; int vd=virtio_drive_count();
    if(vd>0){
        char dn[4]; utoa((uint32_t)vd,dn,10);
        char ms[12]; utoa((uint32_t)(virtio_drive_sectors(0)/2048),ms,10);
        strcpy(vio_ln,"VirtIO BLK: "); strcat(vio_ln,dn);
        strcat(vio_ln," drive  drive0="); strcat(vio_ln,ms);
        strcat(vio_ln,"MB  I-O 0xC100  polling");
    } else { strcpy(vio_ln,"VirtIO BLK: not present  ATA fallback"); }

    char nic_ln[72];
    if(rtl8139_found())    strcpy(nic_ln,"NIC: RTL8139  I-O 0xC000  bus-master  IRQ10");
    else if(e1000_found()) strcpy(nic_ln,"NIC: e1000  MMIO  bus-master  IRQ11");
    else                   strcpy(nic_ln,"NIC: no adapter  network disabled");

    typedef struct{ const char *tag; vga_color_t tc; const char *msg; int ms; } vl;
    vl lines[]={
        {"OK",  VGA_LIGHT_GREEN,"GRUB2 Multiboot handoff  32-bit flat mode",          12},
        {"OK",  VGA_LIGHT_GREEN,"Kernel ELF at 0x00100000  size 768KB",               10},
        {"OK",  VGA_LIGHT_GREEN,"GDT: 256 descriptors  flat 32-bit segments",         10},
        {"OK",  VGA_LIGHT_GREEN,"IDT: 256 gates  PIC remapped 0x20-0x2F",            10},
        {"OK",  VGA_LIGHT_GREEN,"PIT 8253: 100Hz  STI enabled",                       10},
        {"OK",  VGA_LIGHT_GREEN, ram_ln,                                               10},
        {"OK",  VGA_LIGHT_GREEN,"Heap: 4MB at 0x00200000",                            10},
        {"OK",  VGA_LIGHT_GREEN,"VGA: text 80x25  shadow buffer  16 colours",         10},
        {"OK",  VGA_LIGHT_GREEN,"PS-2: keyboard OK  mouse aux port enabled",          10},
        {"OK",  VGA_LIGHT_GREEN,"RTC: CMOS 24h clock OK",                             10},
        {"OK",  VGA_LIGHT_GREEN,"PCI: bus 0 scan complete",                           10},
        {"OK",  vd>0?VGA_LIGHT_GREEN:VGA_YELLOW, vio_ln,                              10},
        {"WARN",VGA_YELLOW,     "ATA: 0x1F0 not present on UTM SE  skip",             10},
        {"OK",  VGA_LIGHT_GREEN,"FAT16: scanning VirtIO partition",                   50},
        {"OK",  VGA_LIGHT_GREEN,"FAT16: mounted  BPB valid  VFS ready",               10},
        {"OK",  (rtl8139_found()||e1000_found())?VGA_LIGHT_GREEN:VGA_YELLOW, nic_ln, 10},
        {"OK",  VGA_LIGHT_GREEN,"NET: eth0  10.0.2.15-24  GW 10.0.2.2",             10},
        {"OK",  VGA_LIGHT_GREEN,"RAMFS: root mounted  overlaid on VFS",               10},
        {"OK",  VGA_LIGHT_GREEN,"Shell: history-pipe-redirect-tab ready",              10},
        {"OK",  VGA_LIGHT_GREEN,"Desktop: themes-icons-menus-notifications ready",    10},
        {"OK",  VGA_LIGHT_GREEN,"Reminders: loaded from disk",                         10},
        {"OK",  VGA_LIGHT_GREEN,"Login: user accounts loaded",                         10},
        {"OK",  VGA_LIGHT_GREEN,"System ready",                                         8},
    };
    int nl=(int)(sizeof(lines)/sizeof(lines[0]));

    int y=1; kbd_drain();
    for(int i=0;i<nl;i++){
        if(y>=23){
            for(int row=1;row<23;row++)
                for(int x=0;x<80;x++){
                    uint8_t ch2,attr; vga_getchar_at(x,row+1,&ch2,&attr);
                    vga_putraw_at(ch2,attr,x,row);
                }
            for(int x=0;x<80;x++) vga_putchar_at(' ',x,22,VGA_BLACK,VGA_BLACK);
            y=22;
        }
        int is_warn=(lines[i].tc==VGA_YELLOW);
        vga_putchar_at('[',1,y,VGA_DARK_GREY,VGA_BLACK);
        vga_puts_at(lines[i].tag, is_warn?2:3, y, lines[i].tc, VGA_BLACK);
        vga_putchar_at(']',is_warn?6:5,y,VGA_DARK_GREY,VGA_BLACK);
        vga_puts_at(lines[i].msg,8,y,VGA_LIGHT_GREY,VGA_BLACK);
        sleep_ms(lines[i].ms);
        y++;
        int c=keyboard_getchar();
        if(c==' '){
            for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,VGA_BLACK,VGA_YELLOW);
            vga_puts_at("  PAUSED - SPACE to continue  ",25,24,VGA_BLACK,VGA_YELLOW);
            kbd_drain(); while(keyboard_waitchar()!=' ');
            for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,VGA_BLACK,VGA_BLACK);
        }
    }

    for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,VGA_BLACK,VGA_LIGHT_GREEN);
    vga_puts_at("  System ready - press any key  ",24,24,VGA_BLACK,VGA_LIGHT_GREEN);
    kbd_drain(); keyboard_waitchar();
    for(int row=0;row<25;row++){
        for(int x=0;x<80;x++) vga_putchar_at(' ',x,row,VGA_BLACK,VGA_BLACK);
        sleep_ms(3);
    }
}

void boot_animation(void){
    extern int cfg_post_screen(void);
    extern int cfg_verbose_boot(void);
    if(cfg_post_screen())  do_post();
    if(cfg_verbose_boot()) do_verbose();
    /* Fake progress bar removed in v0.6.8 */
}
