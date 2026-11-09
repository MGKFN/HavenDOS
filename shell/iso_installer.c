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
#include "../include/instdisk.h"
#include "../include/blkdev.h"
#include "../include/machine.h"
#include "version.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/install_add.h"
#include "../include/esp.h"

extern int      keyboard_waitchar(void);
extern void     sleep_ms(uint32_t);
extern void     utoa(uint32_t, char*, int);

/* ── No driver is named here any more, v0.8.6 ───────────────────
   Fifteen extern declarations stood here -- every VirtIO and ATA entry
   point this file used to dispatch to directly. fs/blkdev.c owns the
   dispatch now, so the installer names no driver at all and
   tools/check_blk_dispatch.py keeps it that way.

   They were already dead when that check first ran and it found them,
   which is the small version of the thing it exists for: a declaration
   of a function nobody calls is an invitation to call it. */

/* ── Install API (install.c) ────────────────────────────────────── */
extern int havendos_install_to_drive(int, void(*)(int,int,const char*));

/* ── The disk table moved to fs/blkdev.c in v0.8.6 ───────────────
 *
 * It used to be here: a disk_entry_t table, a four-way disk_is_readonly(),
 * a disk_scan() that probed four buses and a disk_read/disk_write pair
 * that switched on the bus for every sector.
 *
 * When 0.8.6 came to add USB mass storage, a grep for "every place that
 * chooses a bus" found THREE such switches -- this one, fs/fat16.c's and
 * fs/fat32.c's -- holding four, four and TWO buses. fs/fat32.c could not
 * see an NVMe or an AHCI disk at all, two releases after "the installer
 * had one block layer and the filesystem had another" was declared
 * fixed. The two SCAN orders disagreed as well, so on a machine where
 * both the NVMe and the legacy ATA ports answered, this file called the
 * NVMe disk 0 and the filesystem bound itself to the ATA one.
 *
 * include/blkdev.h carries the whole argument. What is left here is the
 * installer: screens, partition planning, and the GPT questions that
 * live above a sector. The three names below are kept as one-line
 * aliases rather than renamed at 30 call sites, so this release's diff
 * is the block layer and not a rename.
 */
static int      disk_scan(void)        { return blk_scan(); }
static int      disk_read (int i, uint64_t lba, uint8_t *b)       { return blk_read(i, lba, b); }
static int      disk_write(int i, uint64_t lba, const uint8_t *b) { return blk_write(i, lba, b); }
static uint64_t disk_sectors(int i)    { return blk_sectors(i); }
static int      disk_is_readonly(int i){ return blk_readonly(i); }

/* ── What can be done with each disk ─────────────────────────────────
   Until 0.8.2 this was one question with one answer: a GPT disk was
   refused, because the only installer there was begins by zeroing the
   first 64 sectors and the last 33. There are two installers now, and
   which one applies -- or whether neither does -- depends on the disk.

   The answer is worked out ONCE, after the scan, and cached. The picker
   below redraws on every keypress, and re-reading a partition table
   sixty times a second to decide what colour to paint a row is a way to
   make a disk sound unhealthy.

   havendos_install_additive() with dry_run is what answers it: the same
   code path the install itself takes, so a row that says there is room
   is a row the install agrees with. */
#define DMODE_WIPE     0   /* no GPT: the whole-disk installer, destructive */
#define DMODE_ADD      1   /* GPT with usable free space: install beside it */
#define DMODE_PREVIEW  2   /* ...but no write path to this bus yet          */
#define DMODE_NOROOM   3   /* GPT, and nowhere to put a partition           */
#define DMODE_SMALL    4   /* too small for either                          */
#define DMODE_DONE     5   /* HavenDOS is already on it -- not an error     */

/* Sized from the registry, not from a local constant. The old table held
   8 and these three held 8; fs/blkdev.c holds 16, because a hub of sticks
   fits now. Three arrays indexed by a disk number that can reach 15 while
   they hold 8 is a stack overwrite on the screen that asks a person to
   confirm an irreversible write, so the bound is taken from the one place
   that decides it. */
static int        g_dmode[BLK_MAX_DEV];
static inst_add_t g_dplan[BLK_MAX_DEV];
static int        g_dplan_rc[BLK_MAX_DEV];

/* defined below, beside the other GPT helpers */
int inst_disk_has_gpt(int i);

static void plan_disks(void)
{
    for(int i = 0; i < blk_count(); i++){
        uint32_t mb = (uint32_t)(blk_sectors(i) / 2048);
        g_dplan_rc[i] = INSTADD_E_DISK;
        memset(&g_dplan[i], 0, sizeof g_dplan[i]);

        if(!inst_disk_has_gpt(i)){
            g_dmode[i] = (mb < 15) ? DMODE_SMALL : DMODE_WIPE;
            continue;
        }
        /* A GPT disk. Ask the additive installer what it would do. */
        g_dplan_rc[i] = havendos_install_additive(i, 1, &g_dplan[i], 0);
        if(g_dplan_rc[i] == INSTADD_E_ALREADY)
            g_dmode[i] = DMODE_DONE;
        else if(g_dplan_rc[i] == INSTADD_OK)
            g_dmode[i] = disk_is_readonly(i) ? DMODE_PREVIEW : DMODE_ADD;
        else if(g_dplan_rc[i] == INSTADD_E_READONLY)
            g_dmode[i] = DMODE_PREVIEW;
        else
            g_dmode[i] = DMODE_NOROOM;
    }
}

/* LBAs on a 512 GB disk run past 1,000,000,000 and utoa() takes a
   uint32_t, which covers up to 2 TB. Past that the number printed would be
   wrong rather than missing, so it is clamped and marked instead: a wrong
   sector number on the screen that promises which sectors get written is
   worse than no number. */
/* ── The bus label ────────────────────────────────────────────────
   fs/blkdev.c owns the words themselves now -- one string per bus, in
   that bus's ops table, which is a shape tools/check_bus_label.py can
   guard. The two functions that used to be here were the "in one place"
   of v0.8.3.1 and v0.8.5.4, and the place was a static function inside
   this file, behind a wall of VGA and keyboard code: fs/fat16.c could
   not call it and did not, and tests/test_diskname.c had to carry a COPY
   kept in step by a build check. Neither is true any more. */
const char *inst_disk_bus_name(int i){ return blk_bus_name(i); }

void inst_disk_bus_counts(int *ide, int *sata, int *nvme, int *virtio){
    blk_bus_counts(ide, sata, nvme, virtio, 0);
}
static vga_color_t bus_col(int drive){
    switch(blk_kind(drive)){
        case BLK_BUS_AHCI:
        case BLK_BUS_ATA:  return VGA_LIGHT_GREEN;
        case BLK_BUS_NVME: return VGA_YELLOW;
        /* A removable disk gets its own colour, because the one screen
           that matters here asks a person to confirm which disk is about
           to be written and "the stick" versus "the laptop's drive" is
           the distinction they actually care about. */
        case BLK_BUS_USB:  return VGA_LIGHT_MAGENTA;
        default:           return VGA_LIGHT_CYAN;
    }
}
/* "(NVMe)" into a caller buffer of at least 10 bytes. */
static void bus_paren(int drive, char *out){
    const char *n = blk_bus_name(drive); int i = 0;
    out[i++] = '(';
    while(*n) out[i++] = *n++;
    out[i++] = ')'; out[i] = 0;
}

static void lba_str(uint64_t v, char *buf)
{
    if(v > 0xFFFFFFFFull){ buf[0]='>'; buf[1]='4'; buf[2]='G'; buf[3]=0; return; }
    utoa((uint32_t)v, buf, 10);
}

/* Expose to install.c via extern symbols */
int      inst_disk_count(void)               { return blk_count(); }
/* ...and to the desktop installer, which used to run its own VirtIO-only scan
   and hand havendos_install_to_drive() a VirtIO index.  That index only ever
   matched a unified one while VirtIO was the only bus, so on an IDE machine
   the desktop app reported "No VirtIO drives found" next to a perfectly good
   disk the ISO installer could see. */
int      inst_disk_scan(void)                { return disk_scan(); }
int      inst_disk_is_ata(int i)             { return blk_kind(i) == BLK_BUS_ATA; }
int      inst_disk_is_nvme(int i)            { return blk_kind(i) == BLK_BUS_NVME; }
int      inst_disk_is_readonly(int i)        { return disk_is_readonly(i); }

/* Does this disk carry a GUID Partition Table?  Checked by reading LBA 1,
   so it works on any bus the scan found. */
#include "gpt.h"
static int gpt_rd(void *ctx, uint64_t lba, void *buf){
    int i = (int)(long)ctx;
    return disk_read(i, lba, (uint8_t*)buf) ? 0 : -1;
}
int inst_disk_has_gpt(int i){
    if(i < 0 || i >= blk_count()) return 0;
    return gpt_present(gpt_rd, (void*)(long)i);
}
int inst_disk_gpt_parts(int i, gpt_part_t *out, int max){
    if(i < 0 || i >= blk_count()) return 0;
    return gpt_scan(gpt_rd, (void*)(long)i, out, max);
}
uint8_t  inst_disk_bus(int i)                { return blk_pci_bus(i); }
uint8_t  inst_disk_dev(int i)                { return blk_pci_dev(i); }
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
    vga_puts_at(" HavenDOS " HAVENDOS_VER " Installer",0,0,HDR_FG,HDR_BG);
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
static void screen_welcome(void){
    cls(); hdr("Welcome",1,6);
    footer("Press ENTER to continue  |  ESC to reboot");

    box(10,3,60,7);
    puts_center("HavenDOS " HAVENDOS_VER,5,ACC,BODY_BG);
    puts_center("TechHaven Studios",6,VGA_DARK_GREY,BODY_BG);

    vga_puts_at("You have booted from the HavenDOS install ISO.",6,12,BODY_FG,BODY_BG);
    vga_puts_at("This ISO is install media - live desktop is not available.",4,13,BODY_FG,BODY_BG);
    vga_puts_at("The installer will:",6,15,BODY_FG,BODY_BG);
    vga_puts_at(". Partition and format a disk of your choice",8,16,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at(". Write the HavenDOS bootloader and kernel",8,17,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at(". Reboot into the installed system automatically",8,18,VGA_LIGHT_GREY,BODY_BG);

    /* v0.8.5.4 -- what was actually found, named by the one function
       that knows every bus.

       This used to test two booleans, has_virtio and has_ata, and fall
       through to "WARNING: No disks found" for anything else. On a
       laptop whose only disk is NVMe -- which is most laptops now --
       that printed a red warning directly above a green line saying a
       disk had been found and would be auto-selected. Both lines were
       generated from the same scan. */
    {
        int n = inst_disk_count();
        if(n <= 0){
            vga_puts_at("WARNING: No disks found. Attach a disk and reboot.",
                        6,20,WARN,BODY_BG);
        } else {
            char line[64];
            int p = 0, k;
            const char *w;
            /* "Found: 1 NVMe disk." / "Found: 2 disks (NVMe, SATA)." */
            w = "Found: "; for(k=0;w[k];k++) line[p++]=w[k];
            { char nb[12]; utoa((uint32_t)n,nb,10);
              for(k=0;nb[k];k++) line[p++]=nb[k]; }
            line[p++]=' ';
            if(n == 1){
                w = inst_disk_bus_name(0);
                for(k=0;w[k];k++) line[p++]=w[k];
                w = " disk."; for(k=0;w[k];k++) line[p++]=w[k];
            } else {
                w = "disks."; for(k=0;w[k];k++) line[p++]=w[k];
            }
            line[p] = 0;
            vga_puts_at(line,6,20,OK_COL,BODY_BG);
        }
    }
}

/* ── Screen 2: Drive picker ─────────────────────────────────────── */
/* ── This disk already has HavenDOS on it ───────────────────────────
   Its own screen rather than a line of red text that disappears after
   three seconds. The first version reported this case as "no free space
   on this disk -- shrink a partition in Windows first", which is true
   (the gap became the HavenDOS partition) and reads as though the disk
   has been damaged. Someone went looking for a fault in a machine that
   had just worked. */
static void screen_already_installed(int drive)
{
    char b[12];
    cls(); hdr("Already Installed", 2, 6);
    footer("Press any key to go back");

    vga_puts_at("HavenDOS is already installed on disk",6,4,BODY_FG,BODY_BG);
    utoa((uint32_t)drive,b,10);
    vga_puts_at(b,43,4,ACC,BODY_BG);
    vga_putchar_at('.',44,4,BODY_FG,BODY_BG);

    vga_puts_at("Nothing is wrong with it. The free space this installer "
                "would have used",6,6,OK_COL,BODY_BG);
    vga_puts_at("is now the HavenDOS partition, which is why there is none "
                "left.",6,7,OK_COL,BODY_BG);

    box(10,9,60,8);
    vga_puts_at("To use it:      shut down, remove the install media,",
                12,10,BODY_FG,BODY_BG);
    vga_puts_at("                and boot the disk.",12,11,BODY_FG,BODY_BG);
    vga_puts_at("To upgrade it:  boot it and run `update`.",
                12,13,BODY_FG,BODY_BG);
    vga_puts_at("To start over:  delete the HavenDOS partition from the",
                12,15,BODY_FG,BODY_BG);
    vga_puts_at("                other operating system, then come back.",
                12,16,BODY_FG,BODY_BG);

    vga_puts_at("Installing a second copy is refused on purpose: two "
                "partitions",6,19,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at("labelled HAVENDOS would make the boot menu's "
                "`search --label` pick",6,20,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at("whichever it found first.",6,21,VGA_LIGHT_GREY,BODY_BG);
    keyboard_waitchar();
}

/* ── How many rows this screen has ────────────────────────────────
   The list is drawn from row 8 and the two hint lines go under it, so ten
   rows is what fits in 25. That was never a constraint before: the disk
   table held 8, so every disk the scan could find was a disk this screen
   could draw, and `i < 10` was slack.

   fs/blkdev.c holds 16 -- a hub of sticks fits now -- and the slack is
   gone. The draw loop stopped at 10 while the DOWN key stopped at
   ndisks-1, so on a machine with eleven disks the selection could move
   onto a row that was never painted: an installer whose cursor is
   somewhere the user cannot see, on the screen before the one that asks
   them to type YES. Raising a bound in one file made a screen in another
   lie, which is the shape of bug this tree keeps paying for.

   One constant, used by the draw loop AND the key handler, so they cannot
   disagree again. */
#define PICK_MAX_ROWS 10

static int screen_drive_picker(int ndisks){
    int sel=0;
    int nshow = ndisks < PICK_MAX_ROWS ? ndisks : PICK_MAX_ROWS;

    while(1){
        cls(); hdr("Select Target Disk",2,6);
        footer("Up/Down=select  ENTER=confirm  ESC=cancel");

        vga_puts_at("Choose the disk to install HavenDOS on.",6,3,BODY_FG,BODY_BG);
        /* This used to say the selected disk would be COMPLETELY WIPED, for
           every disk, because that was the only thing the installer could
           do. It is now true of some disks and false of others, and a
           warning that is false half the time is worse than none: it is
           the Notes column, per row, that says which. */
        vga_puts_at("A disk with a partition table keeps it - HavenDOS goes "
                    "in the free space.",6,4,BODY_FG,BODY_BG);
        vga_puts_at("A disk with no partition table is WIPED. Read the Notes "
                    "column.",6,5,VGA_YELLOW,BODY_BG);

        vga_puts_at("  #  Size       Type     Bus  Dev  Notes",6,6,ACC,BODY_BG);
        for(int x=6;x<74;x++) vga_putchar_at('-',x,7,VGA_DARK_GREY,BODY_BG);

        for(int i=0;i<nshow;i++){
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

            /* Bus type label.  NVMe used to fall through to "VirtIO",
               which is not a cosmetic slip: it showed somebody's Windows
               drive under the wrong name on a screen whose next step erases
               it. */
            { char bn[10]; const char *nm = blk_bus_name(i); int k = 0;
              while(nm[k]){ bn[k] = nm[k]; k++; }
              while(k < 8) bn[k++] = ' ';
              bn[k] = 0;
              vga_puts_at(bn,23,8+i,is_sel?SEL_FG:bus_col(i),bg); }

            /* Bus / dev numbers */
            char bs[4]; utoa(blk_pci_bus(i),bs,10);
            vga_puts_at(bs,32,8+i,fg,bg);
            char ds[4]; utoa(blk_pci_dev(i),ds,10);
            vga_puts_at(ds,37,8+i,fg,bg);

            /* Notes.  The two refusals are stated HERE, on the row, rather
               than after the user has picked the disk and typed YES.  An
               installer that walks someone all the way to the confirmation
               prompt and only then says no has taught them nothing except
               that it wastes their time -- and the 0.8 Beta changelog
               already records what "discovered at step 7 of 9" cost once. */
            switch(g_dmode[i]){
            case DMODE_ADD: {
                /* The size of the partition that would be created, from the
                   cached dry run -- not the size of the gap, and not a
                   guess: the number the install itself would use. */
                char fb[12];
                utoa((uint32_t)(g_dplan[i].part_sectors/2048), fb, 10);
                vga_puts_at("keeps this disk, uses",42,8+i,OK_COL,bg);
                vga_puts_at(fb,64,8+i,ACC,bg);
                vga_puts_at("MB",64+(int)strlen(fb)+1,8+i,OK_COL,bg);
                break; }
            case DMODE_PREVIEW:
                vga_puts_at("preview only - no write path",42,8+i,
                            VGA_YELLOW,bg);
                break;
            case DMODE_DONE:
                /* Not WARN. This disk is in the state the user was trying
                   to reach, and painting it the same red as a refusal is
                   what made a working install look like a broken disk. */
                vga_puts_at("HavenDOS already installed",42,8+i,OK_COL,bg);
                break;
            case DMODE_NOROOM:
                /* Why, not just no. "Refused" taught the user nothing; the
                   reason tells them whether shrinking a partition in
                   Windows would change the answer. */
                vga_puts_at(havendos_install_add_shortstr(g_dplan_rc[i]),
                            42,8+i,WARN,bg);
                break;
            case DMODE_SMALL:
                vga_puts_at("TOO SMALL (min 15MB)",42,8+i,WARN,bg);
                break;
            default:
                vga_puts_at("no partition table - WIPES disk",42,8+i,
                            VGA_YELLOW,bg);
                break;
            }
        }

        int hint_row=8+nshow+1;
        if(hint_row>21) hint_row=21;
        /* Say it rather than silently show ten of sixteen. A list that is
           quietly truncated is how a person installs to the wrong disk. */
        if(nshow < ndisks){
            char nb[4]; utoa((uint32_t)(ndisks-nshow),nb,10);
            vga_puts_at(nb,6,hint_row,VGA_YELLOW,BODY_BG);
            vga_puts_at(" more disk(s) than fit here - unplug the rest.",
                        8,hint_row,VGA_YELLOW,BODY_BG);
            hint_row++;
            if(hint_row>21) hint_row=21;
        }
        vga_puts_at("HavenDOS requires at least 15MB, or 16MB of free space "
                    "beside another OS.",6,hint_row,VGA_DARK_GREY,BODY_BG);
        vga_puts_at("Picking a disk shows exactly what would be written "
                    "before anything is.",
                    6,hint_row+1,VGA_DARK_GREY,BODY_BG);

        int c=keyboard_waitchar();
        if(c==27) return -1;
        if(c==0x148&&sel>0) sel--;
        if(c==0x150&&sel<nshow-1) sel++;   /* nshow, not ndisks: see PICK_MAX_ROWS */
        if(c=='\n'||c=='\r'){
            switch(g_dmode[sel]){
            case DMODE_ADD:
            case DMODE_PREVIEW:
            case DMODE_WIPE:
                return sel;
            case DMODE_DONE:
                screen_already_installed(sel);
                break;
            case DMODE_NOROOM:
                vga_puts_at(havendos_install_add_strerror(g_dplan_rc[sel]),
                            6,hint_row+2,WARN,BODY_BG);
                vga_puts_at("Shrink a partition in Windows' Disk Management "
                            "to make room, then retry.",
                            6,hint_row+3,VGA_LIGHT_GREY,BODY_BG);
                sleep_ms(3000);
                break;
            default:
                vga_puts_at("Drive too small - minimum 15MB required.",
                            6,hint_row+2,WARN,BODY_BG);
                sleep_ms(1600);
                break;
            }
        }
    }
}

/* ── Screen 3a: the additive preview ────────────────────────────────
   The screen this whole release is for.

   The old confirmation screen described a fixed layout -- sector 0, the
   MBR, sectors 1..2047, the partition at 2048 -- because the layout was
   fixed. An additive install has no fixed layout: where the partition
   goes depends on where the free space is, and what gets written outside
   it is four sectors whose numbers depend on the table already there.

   So this screen does not describe the install in general terms. It
   prints the actual numbers, out of a dry run of the same function that
   will do the work, and it prints them BEFORE anything is written. If
   what appears here is wrong, nothing has happened yet. */
static int screen_additive_preview(int drive, int writable)
{
    const inst_add_t *pl = &g_dplan[drive];
    char b1[12], b2[12], b3[12];
    int row;

    cls(); hdr(writable ? "What will be written" : "What WOULD be written",
               3, 6);
    footer(writable ? "ENTER=continue  ESC=go back"
                    : "ESC=go back  (this disk cannot be written yet)");

    row = 3;
    vga_puts_at("Disk",6,row,BODY_FG,BODY_BG);
    utoa((uint32_t)drive,b1,10);
    vga_puts_at(b1,11,row,ACC,BODY_BG);
    { char bp[10]; bus_paren(drive,bp);
      vga_puts_at(bp,14,row,bus_col(drive),BODY_BG); }
    vga_puts_at("keeps its partition table and every partition on it.",
                26,row,OK_COL,BODY_BG);
    row += 2;

    /* 1 -- the new partition */
    vga_puts_at("A new partition, in free space that is already free:",
                6,row,ACC,BODY_BG); row++;
    lba_str(pl->part_first,b1); lba_str(pl->part_last,b2);
    utoa((uint32_t)(pl->part_sectors/2048),b3,10);
    vga_puts_at("sectors",8,row,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at(b1,16,row,BODY_FG,BODY_BG);
    vga_puts_at("to",17+(int)strlen(b1),row,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at(b2,20+(int)strlen(b1),row,BODY_FG,BODY_BG);
    vga_puts_at("(",21+(int)strlen(b1)+(int)strlen(b2),row,VGA_LIGHT_GREY,BODY_BG);
    vga_puts_at(b3,22+(int)strlen(b1)+(int)strlen(b2),row,ACC,BODY_BG);
    vga_puts_at("MB)",22+(int)strlen(b1)+(int)strlen(b2)+(int)strlen(b3),
                row,VGA_LIGHT_GREY,BODY_BG);
    row++;
    vga_puts_at("formatted FAT16, labelled HAVENDOS, holding the kernel",
                8,row,VGA_LIGHT_GREY,BODY_BG); row += 2;

    /* 2 -- THE FOUR SECTORS. The reason this screen exists. */
    vga_puts_at("Outside that partition, FOUR sectors are written:",
                6,row,ACC,BODY_BG); row++;
    {
        int col = 8;
        for(int i = 0; i < pl->n_outside && i < 4; i++){
            lba_str(pl->outside[i],b1);
            vga_puts_at(b1,col,row,BODY_FG,BODY_BG);
            col += (int)strlen(b1);
            if(i + 1 < pl->n_outside && i < 3){
                vga_puts_at(",",col,row,VGA_LIGHT_GREY,BODY_BG);
                col += 2;
            }
        }
    }
    row++;
    vga_puts_at("the two partition-table headers and one sector of each "
                "entry list.",8,row,VGA_LIGHT_GREY,BODY_BG); row++;
    vga_puts_at("No other sector outside the new partition is touched.",
                8,row,OK_COL,BODY_BG); row += 2;

    /* 3 -- the ESP */
    /* v0.8.8.8: it may not exist yet, in which case this is the part
       of the plan that changes the partition table a SECOND time. A
       person approving a disk change should see both partitions,
       not be told about one and find the other afterwards. */
    if(pl->esp_new_sectors){
        char nb[24];
        vga_puts_at("This disk has NO EFI System Partition, so one is "
                    "CREATED:",6,row,VGA_YELLOW,BODY_BG); row++;
        lba_str(pl->esp_new_sectors / 2048ull, nb);
        vga_puts_at(nb,8,row,ACC,BODY_BG);
        vga_puts_at("MB, FAT32, at the start of the free space -- "
                    "HavenDOS",12,row,VGA_LIGHT_GREY,BODY_BG); row++;
        vga_puts_at("goes after it. This is what lets the machine boot "
                    "without",8,row,VGA_LIGHT_GREY,BODY_BG); row++;
        vga_puts_at("another operating system's loader.",
                    8,row,VGA_LIGHT_GREY,BODY_BG); row++;
        if(pl->disk_has_linux){
            vga_puts_at("Linux is on this disk. Its partitions are not "
                        "touched.",8,row,OK_COL,BODY_BG); row++;
        }
        row++;
    }
    vga_puts_at("On the EFI System Partition, files are ADDED:",
                6,row,ACC,BODY_BG); row++;
    vga_puts_at("/EFI/HAVENDOS/grubx64.efi and grub.cfg, and "
                "/EFI/BOOT/BOOTX64.EFI",8,row,VGA_LIGHT_GREY,BODY_BG); row++;
    if(pl->esp.has_fallback){
        vga_puts_at("the BOOTX64.EFI already there is COPIED to",
                    8,row,VGA_YELLOW,BODY_BG);
        vga_puts_at(pl->esp.backup_name,50,row,ACC,BODY_BG);
        row++;
        vga_puts_at("first, and never overwritten before that copy is "
                    "verified.",8,row,VGA_LIGHT_GREY,BODY_BG);
        row++;
    }
    if(pl->esp.has_microsoft){
        vga_puts_at("Windows' own bootloader is there and is not written "
                    "to at all.",8,row,OK_COL,BODY_BG);
        row++;
        vga_puts_at("The boot menu will offer Windows as well as HavenDOS.",
                    8,row,OK_COL,BODY_BG);
        row++;
    }

    row = 21;
    if(!writable){
        vga_puts_at("THIS IS A PREVIEW ONLY. HavenDOS cannot write to this "
                    "disk yet, so",6,row,VGA_YELLOW,BODY_BG);
        vga_puts_at("nothing above will happen. Nothing has been written.",
                    6,row+1,VGA_YELLOW,BODY_BG);
        while(1){
            int k = keyboard_waitchar();
            if(k == 27 || k == '\n' || k == '\r') return 0;
        }
    }

    vga_puts_at("Nothing has been written yet. ENTER goes on to the "
                "confirmation.",6,row,VGA_LIGHT_GREY,BODY_BG);
    while(1){
        int k = keyboard_waitchar();
        if(k == 27) return 0;
        if(k == '\n' || k == '\r') return 1;
    }
}

/* ── Screen 3b: the additive confirmation ───────────────────────────── */
static int screen_additive_confirm(int drive)
{
    const inst_add_t *pl = &g_dplan[drive];
    char b1[12];

    cls(); hdr("Confirm", 3, 6);
    footer("Type YES and press ENTER to confirm  |  ESC to go back");

    vga_puts_at("HavenDOS will be installed BESIDE what is already on disk",
                6,4,BODY_FG,BODY_BG);
    utoa((uint32_t)drive,b1,10);
    vga_puts_at(b1,63,4,ACC,BODY_BG);
    vga_putchar_at('.',64,4,BODY_FG,BODY_BG);

    box(10,6,60,9);
    vga_puts_at("Nothing is deleted.",12,7,OK_COL,BODY_BG);
    vga_puts_at("No existing partition is moved, resized or reformatted.",
                12,8,OK_COL,BODY_BG);
    vga_puts_at("No file on the EFI partition is overwritten without a",
                12,9,OK_COL,BODY_BG);
    vga_puts_at("verified copy being taken first.",12,10,OK_COL,BODY_BG);
    for(int x=11;x<69;x++) vga_putchar_at('-',x,11,VGA_DARK_GREY,BODY_BG);
    vga_puts_at("What is written: one new partition in free space, four",
                12,12,BODY_FG,BODY_BG);
    vga_puts_at("sectors of the partition table, and new files on the",
                12,13,BODY_FG,BODY_BG);
    vga_puts_at("EFI partition. The previous screen listed them.",
                12,14,BODY_FG,BODY_BG);

    /* Honest, and placed where the old screen put its all-caps warning. A
       confirmation screen that promises safety and nothing else invites
       the one thing nobody should do here, which is not keep a backup. */
    vga_puts_at("This is still a write to a disk with your data on it.",
                6,17,VGA_YELLOW,BODY_BG);
    vga_puts_at("Back up anything you cannot lose before continuing.",
                6,18,VGA_YELLOW,BODY_BG);
    if(pl->esp.has_microsoft)
        vga_puts_at("Windows was found on this disk and is expected to keep "
                    "booting.",6,19,VGA_LIGHT_GREY,BODY_BG);

    vga_puts_at("Type YES to confirm: ",6,22,VGA_WHITE,BODY_BG);
    {
        char conf[8]=""; int ci=0;
        for(int x=27;x<34;x++) vga_putchar_at('_',x,22,VGA_DARK_GREY,BODY_BG);
        vga_set_cursor(27,22);
        while(1){
            int k=keyboard_waitchar();
            if(k==27) return 0;
            if(k=='\n'||k=='\r') break;
            if((k=='\b'||k==127)&&ci>0){
                ci--; conf[ci]=0;
                vga_putchar_at('_',27+ci,22,VGA_DARK_GREY,BODY_BG);
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
}

/* ── ToS screen ─────────────────────────────────────────────────── */
/* ── Screen 3: Layout preview + confirm ─────────────────────────── */
static int screen_confirm(int drive, uint32_t mb){
    cls(); hdr("Partition Layout Preview",3,6);
    footer("Type YES and press ENTER to confirm  |  ESC to go back");

    char mbs[12];
    utoa(mb,mbs,10);

    vga_puts_at("The following will be written to Drive",6,3,BODY_FG,BODY_BG);
    char drv[4]; utoa((uint32_t)drive,drv,10);
    vga_puts_at(drv,45,3,ACC,BODY_BG);
    vga_putchar_at(':',46,3,BODY_FG,BODY_BG);

    { char bp[10]; bus_paren(drive,bp);
      vga_puts_at(bp,48,3,bus_col(drive),BODY_BG); }

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
            /* the field starts at column 27, so the blank goes back where the
               character was, not one column to its left */
            vga_putchar_at('_',27+ci,22,VGA_DARK_GREY,BODY_BG);
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
    cls(); hdr("Installing...",4,6);
    footer("Do not power off the machine.");

    vga_puts_at("Installing HavenDOS " HAVENDOS_VER "...",25,3,BODY_FG,BODY_BG);
    char drv[4]; utoa((uint32_t)drive,drv,10);
    vga_puts_at("Target: Drive",6,4,VGA_DARK_GREY,BODY_BG);
    vga_puts_at(drv,20,4,ACC,BODY_BG);
    { char bp[10]; bus_paren(drive,bp);
      vga_puts_at(bp,22,4,bus_col(drive),BODY_BG); }

    g_prog_row=7;
    prog_cb(0,8,"Preparing...");
}

/* ── Screen 5: Done ─────────────────────────────────────────────── */
static void screen_done(void){
    cls(); hdr("Installation Complete",5,6);

    box(15,4,50,9);
    puts_center("HavenDOS " HAVENDOS_VER " installed successfully!",7,OK_COL,BODY_BG);
    puts_center("The system will reboot automatically.",9,BODY_FG,BODY_BG);
    puts_center("Remove the install ISO before rebooting.",11,VGA_YELLOW,BODY_BG);

    for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,HDR_FG,HDR_BG);
    vga_puts_at("Rebooting in...",32,24,HDR_FG,HDR_BG);

    for(int t=5;t>=1;t--){
        char ts[4]; utoa((uint32_t)t,ts,10);
        vga_puts_at(ts,48,24,VGA_LIGHT_CYAN,HDR_BG);
        sleep_ms(1000);
    }
    machine_reset();
}

/* ── Screen: no drives found ─────────────────────────────────────── */
static void screen_no_drives(void){
    cls(); hdr("No Disks Found",0,6);

    box(12,6,56,10);
    puts_center("No disks detected (NVMe, SATA, IDE or VirtIO).",9,WARN,BODY_BG);
    puts_center("HavenDOS requires a disk to install.",11,BODY_FG,BODY_BG);
    puts_center("In QEMU: attach a -drive or -cdrom disk.",13,BODY_FG,BODY_BG);
    puts_center("On real PC: ensure disk is connected and BIOS",13,BODY_FG,BODY_BG);
    puts_center("is set to AHCI or Legacy IDE mode.",14,BODY_FG,BODY_BG);

    footer("System halted. Attach a disk and reboot.");
    for(;;) __asm__ volatile("cli; hlt");
}

/* ── Screen: install error ───────────────────────────────────────── */
static void screen_error(int code){
    cls(); hdr("Install Failed",0,6);
    puts_center("Installation failed.",10,WARN,BODY_BG);
    char cs[8]; cs[0]='E'; utoa((uint32_t)(-code),cs+1,10);
    puts_center(cs,12,BODY_FG,BODY_BG);

    /* One line per code. This used to print "Check disk is writable and
       large enough (min 15MB)" for every failure there is, which on the
       most common one -- a disk too LARGE for FAT16 -- told the reader to
       go and check the opposite of the actual problem. */
    const char *why;
    switch(code){
        case -2:  why = "Disk is smaller than the 15MB minimum."; break;
        case -3:
        case -4:  why = "The disk refused a write. It may be read-only."; break;
        case -5:  why = "Formatting the partition failed."; break;
        case -8:  why = "This build carries no installable image."; break;
        case -13: why = "The disk reports itself as read-only."; break;
        case -14: why = "Disk has a GPT -- use the additive install.";  break;
        case -15: why = "No FAT16 geometry fits this partition size."; break;
        default:  why = "See the code above; dmesg has the detail.";    break;
    }
    puts_center(why,14,BODY_FG,BODY_BG);
    footer("Press any key to return to drive selection.");
    keyboard_waitchar();
}

/* ── Public entry point ──────────────────────────────────────────── */
void iso_install_wizard(void){
    int ndisks = disk_scan();
    /* Once, here, rather than inside the picker's redraw loop: the picker
       repaints on every keypress and this reads partition tables. */
    plan_disks();

    if(ndisks==0){ screen_no_drives(); return; }

    while(1){
        screen_welcome();
        /* Show disk count hint on welcome screen */
        if(ndisks==1){
            /* v0.8.5.4: this said "ATA/SATA" for one type and "VirtIO"
               for everything else, so an NVMe or AHCI disk was announced
               as VirtIO. The bus is named once, above, by the function
               that knows all four. */
            vga_puts_at("It will be auto-selected.",6,22,
                        VGA_LIGHT_GREEN,BODY_BG);
        } else {
            char nbuf[4]; utoa((uint32_t)ndisks,nbuf,10);
            vga_puts_at(nbuf,6,22,VGA_LIGHT_CYAN,BODY_BG);
            vga_puts_at(" disks found - you will choose one.",8,22,BODY_FG,BODY_BG);
        }
        int c=keyboard_waitchar();
        if(c==27){
            machine_reset();
        }

        /* Always show the drive picker — never auto-select.
           Auto-select caused USB sticks to be targeted instead of SATA.
           The user must always identify their disk from the list.     */
        int drive=screen_drive_picker(ndisks);
        if(drive<0) continue;

        /* Two installers, and which one runs is decided by what is on the
           disk -- never by a menu item the user might pick wrongly. A disk
           with a partition table gets the additive path; only a disk with
           no table at all reaches the one that wipes. */
        if(g_dmode[drive]==DMODE_PREVIEW){
            screen_additive_preview(drive, 0);
            continue;
        }

        if(g_dmode[drive]==DMODE_ADD){
            inst_add_t rep;
            int ret;

            if(!screen_additive_preview(drive, 1)) continue;
            if(!screen_additive_confirm(drive))    continue;

            screen_progress(drive);
            ret = havendos_install_additive(drive, 0, &rep, prog_cb);
            if(ret != INSTADD_OK){
                /* The additive codes are their own numbering and their own
                   messages; screen_error() speaks the whole-disk
                   installer's. Sharing one screen between them would mean
                   one of the two showing the wrong text for its code. */
                cls(); hdr("Installation Failed",5,6);
                footer("Press any key to return to the disk list");
                vga_puts_at("Nothing further was written.",6,4,BODY_FG,BODY_BG);
                vga_puts_at(havendos_install_add_strerror(ret),6,6,WARN,BODY_BG);
                if(ret==INSTADD_E_ESP)
                    vga_puts_at(esp_strerror(rep.esp_rc),6,8,WARN,BODY_BG);
                vga_puts_at("The partition table and every existing "
                            "partition are unchanged.",
                            6,10,VGA_LIGHT_GREY,BODY_BG);
                keyboard_waitchar();
                /* The disk has changed, so the cached plans have not. */
                plan_disks();
                continue;
            }
            plan_disks();
            screen_done();
            continue;
        }

        /* DMODE_WIPE: no partition table at all. */
        uint32_t mb=(uint32_t)(disk_sectors(drive)/2048);

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
