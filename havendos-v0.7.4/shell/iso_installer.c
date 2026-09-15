/*
 * iso_installer.c — HavenDOS v0.7.4 ISO Install Wizard
 *
 * v0.7.4 changes:
 *  - Unified disk abstraction: probes VirtIO first, then ATA/SATA.
 *    On real PC hardware with a SATA SSD (Lenovo X140e etc.) the
 *    installer now correctly detects and installs to the ATA drive.
 *  - All version strings updated to v0.7.4.
 *  - Welcome screen no longer says "attach a VirtIO disk" on ATA systems.
 *  - Drive picker shows bus type (VirtIO / ATA) and actual disk size.
 *
 * Flow:
 *   Screen 1: Welcome
 *   Screen 2: Drive picker
 *   Screen 3b: Terms of Service
 *   Screen 3: Partition layout preview + YES confirm
 *   Screen 4: Progress bar
 *   Screen 5: Done → countdown → reboot
 */

#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"

extern int      keyboard_waitchar(void);
extern void     sleep_ms(uint32_t);
extern void     utoa(uint32_t, char*, int);

/* ── VirtIO API ─────────────────────────────────────────────────── */
extern int      virtio_scan_drives(void);
extern int      virtio_drive_count(void);
extern uint64_t virtio_drive_sectors(int);
extern uint8_t  virtio_drive_bus(int);
extern uint8_t  virtio_drive_dev(int);
extern int      virtio_drive_read(int, uint64_t, uint8_t*);
extern int      virtio_drive_write(int, uint64_t, const uint8_t*);

/* ── ATA/SATA API ───────────────────────────────────────────────── */
extern void     ata_init(void);
extern int      ata_detected(void);
extern uint32_t ata_sectors(void);
extern int      ata_lba48(void);
extern int      ata_read_sector(uint32_t, uint8_t*);
extern int      ata_write_sector(uint32_t, const uint8_t*);
extern int      ata_read_sector48(uint32_t, uint8_t*);
extern int      ata_write_sector48(uint32_t, const uint8_t*);

/* ── Install API (install.c) ────────────────────────────────────── */
extern int havendos_install_to_drive(int, void(*)(int,int,const char*));

/* ── Unified disk abstraction ───────────────────────────────────── */
/* Drive types */
#define DTYPE_VIRTIO  0
#define DTYPE_ATA     1

typedef struct {
    int      type;        /* DTYPE_* */
    int      virtio_idx;  /* valid when type==DTYPE_VIRTIO */
    uint64_t sectors;
    uint8_t  bus;
    uint8_t  dev;
} disk_entry_t;

#define MAX_DISKS 8
static disk_entry_t g_disks[MAX_DISKS];
static int          g_ndisks = 0;

/* Scan both buses and populate g_disks[].
   Returns total number of disks found. */
static int disk_scan(void) {
    g_ndisks = 0;

    /* 1 — VirtIO */
    int nv = virtio_scan_drives();
    for (int i = 0; i < nv && g_ndisks < MAX_DISKS; i++) {
        g_disks[g_ndisks].type       = DTYPE_VIRTIO;
        g_disks[g_ndisks].virtio_idx = i;
        g_disks[g_ndisks].sectors    = virtio_drive_sectors(i);
        g_disks[g_ndisks].bus        = virtio_drive_bus(i);
        g_disks[g_ndisks].dev        = virtio_drive_dev(i);
        g_ndisks++;
    }

    /* 2 — ATA / SATA */
    ata_init();
    if (ata_detected() && g_ndisks < MAX_DISKS) {
        g_disks[g_ndisks].type       = DTYPE_ATA;
        g_disks[g_ndisks].virtio_idx = -1;
        g_disks[g_ndisks].sectors    = (uint64_t)ata_sectors();
        g_disks[g_ndisks].bus        = 0;
        g_disks[g_ndisks].dev        = 0;
        g_ndisks++;
    }

    return g_ndisks;
}

/* Read/write through whichever bus owns disk index i */
int disk_read(int i, uint64_t lba, uint8_t *buf) {
    if (i < 0 || i >= g_ndisks) return 0;
    if (g_disks[i].type == DTYPE_VIRTIO)
        return virtio_drive_read(g_disks[i].virtio_idx, lba, buf);
    /* ATA */
    if (ata_lba48()) return ata_read_sector48((uint32_t)lba, buf);
    return ata_read_sector((uint32_t)lba, buf);
}

int disk_write(int i, uint64_t lba, const uint8_t *buf) {
    if (i < 0 || i >= g_ndisks) return 0;
    if (g_disks[i].type == DTYPE_VIRTIO)
        return virtio_drive_write(g_disks[i].virtio_idx, lba, buf);
    if (ata_lba48()) return ata_write_sector48((uint32_t)lba, buf);
    return ata_write_sector((uint32_t)lba, buf);
}

uint64_t disk_sectors(int i) {
    if (i < 0 || i >= g_ndisks) return 0;
    return g_disks[i].sectors;
}

/* Expose to install.c via extern symbols */
int      inst_disk_count(void)               { return g_ndisks; }
int      inst_disk_read(int i, uint64_t lba, uint8_t *b)        { return disk_read(i,lba,b); }
int      inst_disk_write(int i, uint64_t lba, const uint8_t *b) { return disk_write(i,lba,b); }
uint64_t inst_disk_sectors(int i)            { return disk_sectors(i); }

/* ── Palette ─────────────────────────────────────────────────────── */
#define HDR_BG   VGA_BLUE
#define HDR_FG   VGA_WHITE
#define BODY_BG  VGA_BLACK
#define BODY_FG  VGA_LIGHT_GREY
#define ACC      VGA_LIGHT_CYAN
#define WARN     VGA_LIGHT_RED
#define OK_COL   VGA_LIGHT_GREEN
#define SEL_BG   VGA_CYAN
#define SEL_FG   VGA_BLACK

/* ── Helpers ─────────────────────────────────────────────────────── */
static void cls(void){
    for(int y=0;y<25;y++)
        for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,BODY_FG,BODY_BG);
}

static void hdr(const char *title, int step, int total){
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,HDR_FG,HDR_BG);
    vga_puts_at(" HavenDOS v0.7.4 Installer",0,0,HDR_FG,HDR_BG);
    char sb[20]; int si=0;
    sb[si++]='S';sb[si++]='t';sb[si++]='e';sb[si++]='p';sb[si++]=' ';
    sb[si++]='0'+step; sb[si++]='/'; sb[si++]='0'+total; sb[si]=0;
    vga_puts_at(sb,72,0,VGA_LIGHT_CYAN,HDR_BG);
    for(int x=0;x<80;x++) vga_putchar_at('-',x,1,VGA_DARK_GREY,BODY_BG);
    vga_puts_at(title,2,1,ACC,BODY_BG);
}

static void footer(const char *hint){
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,HDR_FG,HDR_BG);
    vga_puts_at(hint,2,24,VGA_LIGHT_CYAN,HDR_BG);
}

static void box(int x,int y,int w,int h){
    for(int i=x;i<x+w;i++){
        vga_putchar_at('-',i,y,VGA_DARK_GREY,BODY_BG);
        vga_putchar_at('-',i,y+h-1,VGA_DARK_GREY,BODY_BG);
    }
    for(int j=y+1;j<y+h-1;j++){
        vga_putchar_at('|',x,j,VGA_DARK_GREY,BODY_BG);
        vga_putchar_at('|',x+w-1,j,VGA_DARK_GREY,BODY_BG);
    }
}

static void puts_center(const char *s, int row, vga_color_t fg, vga_color_t bg){
    int len=(int)strlen(s);
    int x=(80-len)/2; if(x<0)x=0;
    vga_puts_at(s,x,row,fg,bg);
}

/* ── Progress callback ───────────────────────────────────────────── */
static int g_prog_row=0;
static void prog_cb(int step, int total, const char *msg){
    int bar_w=60;
    int filled=(step*bar_w)/total;
    for(int x=0;x<bar_w;x++)
        vga_putchar_at(x<filled?'#':'.',10+x,g_prog_row,
                       x<filled?OK_COL:VGA_DARK_GREY,BODY_BG);
    char pct[8]; utoa((uint32_t)(step*100/total),pct,10);
    int pl=(int)strlen(pct);
    vga_puts_at("   ",71,g_prog_row,BODY_FG,BODY_BG);
    vga_puts_at(pct,71,g_prog_row,ACC,BODY_BG);
    vga_putchar_at('%',71+pl,g_prog_row,ACC,BODY_BG);
    char padmsg[72]; int ml=(int)strlen(msg);
    for(int i=0;i<71;i++) padmsg[i]=(i<ml)?msg[i]:' ';
    padmsg[71]=0;
    vga_puts_at(padmsg,4,g_prog_row+1,BODY_FG,BODY_BG);
}

/* ── Screen 1: Welcome ───────────────────────────────────────────── */
static void screen_welcome(int has_virtio, int has_ata){
    cls(); hdr("Welcome",1,5);
    footer("Press ENTER to continue  |  ESC to reboot");

    box(10,3,60,7);
    puts_center("HavenDOS v0.7.4",5,ACC,BODY_BG);
    puts_center("TechHaven Studios",6,VGA_DARK_GREY,BODY_BG);

    vga_puts_at("You have booted from the HavenDOS install ISO.",6,12,BODY_FG,BODY_BG);
    vga_puts_at("This ISO is install media - live desktop is not available.",4,13,BODY_FG,BODY_BG);
    vga_puts_at("The installer will:",6,15,BODY_FG,BODY_BG);
    vga_puts_at(". Partition and format a disk of your choice",8,16,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at(". Write the HavenDOS bootloader and kernel",8,17,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at(". Reboot into the installed system automatically",8,18,VGA_LIGHT_GREY,BODY_BG);

    /* Dynamic disk hint based on what was found */
    if(has_virtio && has_ata)
        vga_puts_at("VirtIO and ATA/SATA disks detected.",6,20,OK_COL,BODY_BG);
    else if(has_ata)
        vga_puts_at("ATA/SATA disk detected. Ready to install.",6,20,OK_COL,BODY_BG);
    else if(has_virtio)
        vga_puts_at("VirtIO disk detected. Ready to install.",6,20,OK_COL,BODY_BG);
    else
        vga_puts_at("WARNING: No disks found. Attach a disk and reboot.",6,20,WARN,BODY_BG);
}

/* ── Screen 2: Drive picker ─────────────────────────────────────── */
static int screen_drive_picker(int ndisks){
    int sel=0;

    while(1){
        cls(); hdr("Select Target Disk",2,5);
        footer("Up/Down=select  ENTER=confirm  ESC=cancel");

        vga_puts_at("Choose the disk to install HavenDOS on.",6,3,BODY_FG,BODY_BG);
        vga_puts_at("WARNING: the selected disk will be COMPLETELY WIPED.",6,4,WARN,BODY_BG);
        vga_puts_at("NOTE: Disk 0 may be your USB installer - check sizes carefully!",6,5,VGA_YELLOW,BODY_BG);

        vga_puts_at("  #  Size       Type     Bus  Dev  Notes",6,6,ACC,BODY_BG);
        for(int x=6;x<74;x++) vga_putchar_at('-',x,7,VGA_DARK_GREY,BODY_BG);

        for(int i=0;i<ndisks&&i<10;i++){
            uint64_t sects=disk_sectors(i);
            uint32_t mb=(uint32_t)(sects/2048);
            uint32_t gb_int=mb/1024, gb_frac=(mb%1024)*10/1024;

            int is_sel=(i==sel);
            vga_color_t fg=is_sel?SEL_FG:BODY_FG;
            vga_color_t bg=is_sel?SEL_BG:BODY_BG;

            for(int x=6;x<74;x++) vga_putchar_at(' ',x,8+i,fg,bg);
            vga_putchar_at(is_sel?'>':' ',6,8+i,is_sel?OK_COL:BODY_FG,bg);

            char ns[4]; utoa((uint32_t)i,ns,10);
            vga_puts_at(ns,8,8+i,fg,bg);

            /* Size */
            char szs[12];
            if(gb_int>0){
                utoa(gb_int,szs,10);
                int sl=(int)strlen(szs); szs[sl]='.'; sl++;
                utoa(gb_frac,szs+sl,10); sl++;
                szs[sl]='G'; szs[sl+1]='B'; szs[sl+2]=0;
            } else {
                utoa(mb,szs,10);
                int sl=(int)strlen(szs); szs[sl]='M'; szs[sl+1]='B'; szs[sl+2]=0;
            }
            int sl=(int)strlen(szs);
            while(sl<10){ szs[sl++]=' '; szs[sl]=0; }
            vga_puts_at(szs,12,8+i,fg,bg);

            /* Bus type label */
            if(g_disks[i].type==DTYPE_ATA)
                vga_puts_at("ATA/SATA",23,8+i,is_sel?SEL_FG:VGA_LIGHT_GREEN,bg);
            else
                vga_puts_at("VirtIO  ",23,8+i,is_sel?SEL_FG:VGA_LIGHT_CYAN,bg);

            /* Bus / dev numbers */
            char bs[4]; utoa(g_disks[i].bus,bs,10);
            vga_puts_at(bs,32,8+i,fg,bg);
            char ds[4]; utoa(g_disks[i].dev,ds,10);
            vga_puts_at(ds,37,8+i,fg,bg);

            /* Notes */
            if(mb<15) vga_puts_at("TOO SMALL (min 15MB)",42,8+i,WARN,bg);
            else if(i==0) vga_puts_at("recommended",42,8+i,OK_COL,bg);
        }

        int hint_row=8+ndisks+1;
        if(hint_row>21) hint_row=21;
        vga_puts_at("HavenDOS requires at least 15MB.",6,hint_row,VGA_DARK_GREY,BODY_BG);

        int c=keyboard_waitchar();
        if(c==27) return -1;
        if(c==0x148&&sel>0) sel--;
        if(c==0x150&&sel<ndisks-1) sel++;
        if(c=='\n'||c=='\r'){
            uint32_t mb=(uint32_t)(disk_sectors(sel)/2048);
            if(mb<15){
                vga_puts_at("Drive too small - minimum 15MB required.",6,hint_row+1,WARN,BODY_BG);
                sleep_ms(1600);
            } else {
                return sel;
            }
        }
    }
}

/* ── ToS screen ─────────────────────────────────────────────────── */
static int screen_tos(void){
    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,HDR_BG);

    hdr("Terms of Service", 2, 5);
    footer("A=Accept  D=Decline  ESC=Cancel");

    vga_puts_at("Terms of Service",32,3,VGA_LIGHT_CYAN,BODY_BG);
    for(int x=4;x<76;x++) vga_putchar_at('-',x,4,VGA_DARK_GREY,BODY_BG);

    static const char *tos_lines[]={
        "HavenDOS is developed by TechHaven Studios.",
        "By installing HavenDOS you agree to the following:",
        "",
        "1. HavenDOS is provided as-is, without warranty of any kind.",
        "   TechHaven Studios is not liable for data loss, hardware",
        "   damage, or any other issues arising from use.",
        "",
        "2. HavenDOS is free to use for personal and educational",
        "   purposes. Commercial use requires written permission.",
        "",
        "3. The source code is available on GitHub under the",
        "   TechHaven Studios open licence.",
        "",
        "4. Installation will ERASE all data on the selected drive.",
        "   Ensure you have backed up any important data.",
        "",
        "5. Network features transmit data over the internet.",
        "   Use responsibly and in accordance with local laws.",
    };
    int nlines=(int)(sizeof(tos_lines)/sizeof(tos_lines[0]));
    for(int i=0;i<nlines;i++){
        if(5+i>=22) break;
        vga_puts_at(tos_lines[i],4,5+i,
                    (tos_lines[i][0]=='1'||tos_lines[i][0]=='2'||
                     tos_lines[i][0]=='3'||tos_lines[i][0]=='4'||
                     tos_lines[i][0]=='5')?VGA_WHITE:VGA_LIGHT_GREY,BODY_BG);
    }

    for(int x=4;x<76;x++) vga_putchar_at('-',x,23,VGA_DARK_GREY,BODY_BG);
    vga_puts_at("Press A to Accept and continue, D to Decline and exit.",13,23,VGA_WHITE,BODY_BG);

    while(1){
        int k=keyboard_waitchar();
        if(k=='a'||k=='A') return 1;
        if(k=='d'||k=='D'||k==27) return 0;
    }
}

/* ── Screen 3: Layout preview + confirm ─────────────────────────── */
static int screen_confirm(int drive, uint32_t mb){
    cls(); hdr("Partition Layout Preview",3,5);
    footer("Type YES and press ENTER to confirm  |  ESC to go back");

    char mbs[12];
    utoa(mb,mbs,10);

    vga_puts_at("The following will be written to Drive",6,3,BODY_FG,BODY_BG);
    char drv[4]; utoa((uint32_t)drive,drv,10);
    vga_puts_at(drv,45,3,ACC,BODY_BG);
    vga_putchar_at(':',46,3,BODY_FG,BODY_BG);

    /* Show bus type */
    if(g_disks[drive].type==DTYPE_ATA)
        vga_puts_at("(ATA/SATA)",48,3,VGA_LIGHT_GREEN,BODY_BG);
    else
        vga_puts_at("(VirtIO)",48,3,VGA_LIGHT_CYAN,BODY_BG);

    box(10,5,60,13);
    vga_puts_at("Sector 0          MBR + GRUB bootloader",12,6,BODY_FG,BODY_BG);
    vga_puts_at("Sectors 1-2047    GRUB core image",12,7,BODY_FG,BODY_BG);
    vga_puts_at("Sector 2048+      FAT16 partition (type 0x0E)",12,8,BODY_FG,BODY_BG);
    for(int x=11;x<69;x++) vga_putchar_at('-',x,9,VGA_DARK_GREY,BODY_BG);
    vga_puts_at("  /boot/grub/grub.cfg  GRUB config",12,10,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at("  /boot/havendos.elf   HavenDOS kernel",12,11,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at("  /havendos.txt        Version marker",12,12,VGA_DARK_GREY,BODY_BG);

    vga_puts_at("Total disk size: ",6,19,BODY_FG,BODY_BG);
    vga_puts_at(mbs,23,19,ACC,BODY_BG); vga_puts_at("MB",23+(int)strlen(mbs)+1,19,BODY_FG,BODY_BG);

    vga_puts_at("ALL DATA ON THIS DISK WILL BE PERMANENTLY ERASED.",15,21,WARN,BODY_BG);
    vga_puts_at("Type YES to confirm: ",6,22,VGA_WHITE,BODY_BG);

    char conf[8]=""; int ci=0;
    vga_set_cursor(27,22);
    for(int x=27;x<34;x++) vga_putchar_at('_',x,22,VGA_DARK_GREY,BODY_BG);
    vga_set_cursor(27,22);

    while(1){
        int k=keyboard_waitchar();
        if(k==27){ return 0; }
        if(k=='\n'||k=='\r') break;
        if((k=='\b'||k==127)&&ci>0){
            ci--; conf[ci]=0;
            vga_putchar_at('_',26+ci,22,VGA_DARK_GREY,BODY_BG);
            vga_set_cursor(27+ci,22);
        } else if(ci<7){
            char uc=(k>='a'&&k<='z')?(char)(k-32):(char)k;
            if(uc>='A'&&uc<='Z'){
                conf[ci++]=uc; conf[ci]=0;
                vga_putchar_at(uc,26+ci,22,ACC,BODY_BG);
                vga_set_cursor(27+ci,22);
            }
        }
    }
    return strcmp(conf,"YES")==0;
}

/* ── Screen 4: Progress ─────────────────────────────────────────── */
static void screen_progress(int drive){
    cls(); hdr("Installing...",4,5);
    footer("Do not power off the machine.");

    vga_puts_at("Installing HavenDOS v0.7.4...",25,3,BODY_FG,BODY_BG);
    char drv[4]; utoa((uint32_t)drive,drv,10);
    vga_puts_at("Target: Drive",6,4,VGA_DARK_GREY,BODY_BG);
    vga_puts_at(drv,20,4,ACC,BODY_BG);
    if(g_disks[drive].type==DTYPE_ATA)
        vga_puts_at("(ATA/SATA)",22,4,VGA_LIGHT_GREEN,BODY_BG);
    else
        vga_puts_at("(VirtIO)",22,4,VGA_LIGHT_CYAN,BODY_BG);

    g_prog_row=7;
    prog_cb(0,8,"Preparing...");
}

/* ── Screen 5: Done ─────────────────────────────────────────────── */
static void screen_done(void){
    cls(); hdr("Installation Complete",5,5);

    box(15,4,50,9);
    puts_center("HavenDOS v0.7.4 installed successfully!",7,OK_COL,BODY_BG);
    puts_center("The system will reboot automatically.",9,BODY_FG,BODY_BG);
    puts_center("Remove the install ISO before rebooting.",11,VGA_YELLOW,BODY_BG);

    for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,HDR_FG,HDR_BG);
    vga_puts_at("Rebooting in...",32,24,HDR_FG,HDR_BG);

    for(int t=5;t>=1;t--){
        char ts[4]; utoa((uint32_t)t,ts,10);
        vga_puts_at(ts,48,24,VGA_LIGHT_CYAN,HDR_BG);
        sleep_ms(1000);
    }
    __asm__ volatile(
        "1: inb $0x64, %%al\n"
        "testb $2, %%al\n"
        "jnz 1b\n"
        "movb $0xFE, %%al\n"
        "outb %%al, $0x64\n"
        ::: "eax"
    );
    for(;;) __asm__ volatile("cli; hlt");
}

/* ── Screen: no drives found ─────────────────────────────────────── */
static void screen_no_drives(void){
    cls(); hdr("No Disks Found",0,5);

    box(12,6,56,10);
    puts_center("No disks detected (VirtIO or ATA/SATA).",9,WARN,BODY_BG);
    puts_center("HavenDOS requires a disk to install.",11,BODY_FG,BODY_BG);
    puts_center("In QEMU: attach a -drive or -cdrom disk.",13,BODY_FG,BODY_BG);
    puts_center("On real PC: ensure disk is connected and BIOS",13,BODY_FG,BODY_BG);
    puts_center("is set to AHCI or Legacy IDE mode.",14,BODY_FG,BODY_BG);

    footer("System halted. Attach a disk and reboot.");
    for(;;) __asm__ volatile("cli; hlt");
}

/* ── Screen: install error ───────────────────────────────────────── */
static void screen_error(int code){
    cls(); hdr("Install Failed",0,5);
    puts_center("Installation failed.",10,WARN,BODY_BG);
    char cs[8]; cs[0]='E'; utoa((uint32_t)(-code),cs+1,10);
    puts_center(cs,12,BODY_FG,BODY_BG);
    puts_center("Check disk is writable and large enough (min 15MB).",14,BODY_FG,BODY_BG);
    footer("Press any key to return to drive selection.");
    keyboard_waitchar();
}

/* ── Public entry point ──────────────────────────────────────────── */
void iso_install_wizard(void){
    int ndisks = disk_scan();

    int has_virtio = 0, has_ata = 0;
    for(int i=0;i<ndisks;i++){
        if(g_disks[i].type==DTYPE_VIRTIO) has_virtio=1;
        if(g_disks[i].type==DTYPE_ATA)    has_ata=1;
    }

    if(ndisks==0){ screen_no_drives(); return; }

    while(1){
        screen_welcome(has_virtio, has_ata);
        /* Show disk count hint on welcome screen */
        if(ndisks==1){
            if(g_disks[0].type==DTYPE_ATA)
                vga_puts_at("1 ATA/SATA disk found - will be auto-selected.",6,22,VGA_LIGHT_GREEN,BODY_BG);
            else
                vga_puts_at("1 VirtIO disk found - will be auto-selected.",6,22,VGA_LIGHT_GREEN,BODY_BG);
        } else {
            char nbuf[4]; utoa((uint32_t)ndisks,nbuf,10);
            vga_puts_at(nbuf,6,22,VGA_LIGHT_CYAN,BODY_BG);
            vga_puts_at(" disks found - you will choose one.",8,22,BODY_FG,BODY_BG);
        }
        int c=keyboard_waitchar();
        if(c==27){
            __asm__ volatile(
                "1: inb $0x64, %%al\n"
                "testb $2, %%al\n"
                "jnz 1b\n"
                "movb $0xFE, %%al\n"
                "outb %%al, $0x64\n"
                ::: "eax"
            );
            for(;;) __asm__ volatile("cli; hlt");
        }

        /* Always show the drive picker — never auto-select.
           Auto-select caused USB sticks to be targeted instead of SATA.
           The user must always identify their disk from the list.     */
        int drive=screen_drive_picker(ndisks);
        if(drive<0) continue;

        uint32_t mb=(uint32_t)(disk_sectors(drive)/2048);

        if(!screen_tos()) return;
        if(!screen_confirm(drive,mb)) continue;

        screen_progress(drive);
        int ret=havendos_install_to_drive(drive, prog_cb);

        if(ret!=0){
            screen_error(ret);
            continue;
        }

        screen_done();
    }
}
