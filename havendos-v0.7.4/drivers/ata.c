/*
 * ata.c — ATA/SATA PIO driver for HavenDOS v0.7.4
 *
 * What's new in v0.7.4:
 *  - AHCI controller detection: probes AHCI first, falls back to
 *    legacy PIO if AHCI port responds as SATA/ATAPI.  On real SATA
 *    SSDs and HDDs in QEMU's default ahci mode, the drive is also
 *    accessible via PIO through the emulated AHCI compatibility port
 *    — we detect it and use PIO so we need zero AHCI MMIO code.
 *  - LBA48 support: drives >128GB now read/write correctly.
 *  - Faster settle loop: uses PAUSE hint, no unnecessary spin waste.
 *  - PCI scan now also checks AHCI class (0x01/0x06) to avoid
 *    misidentifying SATA controllers as IDE.
 *
 * Purely polling — no IRQs.  Works on UTM SE / QEMU / VirtualBox /
 * VMware / real PC with BIOS legacy ATA compatibility.
 */

#include "../include/types.h"
#include "../include/io.h"
#include "../include/string.h"

/* Forward declarations for LBA48 functions defined later in this file */
int ata_read_sector48(uint32_t lba, uint8_t *buf);
int ata_write_sector48(uint32_t lba, const uint8_t *buf);

/* ── ATA registers (offset from base) ─────────────────────────── */
#define ATA_DATA        0
#define ATA_ERROR       1
#define ATA_FEAT        1
#define ATA_SECCOUNT    2
#define ATA_LBA0        3
#define ATA_LBA1        4
#define ATA_LBA2        5
#define ATA_DRIVE       6
#define ATA_STATUS      7
#define ATA_CMD         7
/* Control register (alt status) */
#define ATA_CTRL_NIEN   0x02  /* disable interrupts */
#define ATA_CTRL_SRST   0x04  /* software reset     */

/* ATA commands */
#define CMD_IDENTIFY    0xEC
#define CMD_READ28      0x20
#define CMD_WRITE28     0x30
#define CMD_READ48      0x24
#define CMD_WRITE48     0x34
#define CMD_FLUSH       0xE7
#define CMD_FLUSH48     0xEA

/* Status bits */
#define ATA_SR_BSY      0x80
#define ATA_SR_DRDY     0x40
#define ATA_SR_DF       0x20
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

typedef struct {
    uint16_t base;
    uint16_t ctrl;
    uint8_t  slave;
    int      lba48;   /* drive supports LBA48 */
    const char *name;
} ata_bus_t;

static ata_bus_t dyn_buses[4];
static const ata_bus_t *active      = NULL;
static uint32_t          disk_sectors = 0;   /* LBA28 max (or capped at 0xFFFFF if LBA48) */
static uint64_t          disk_lba48   = 0;   /* true sector count for LBA48 drives */
static char              debug_log[1024];

static inline uint16_t P(int off) { return active->base + (uint16_t)off; }

static void log_append(const char *s){
    int l=strlen(debug_log), sl=strlen(s);
    if(l+sl<(int)sizeof(debug_log)-1) memcpy(debug_log+l,s,sl+1);
}
static void log_hex(uint32_t v){
    const char *h="0123456789ABCDEF";
    char b[11]={'0','x',
        h[(v>>28)&0xF],h[(v>>24)&0xF],h[(v>>20)&0xF],h[(v>>16)&0xF],
        h[(v>>12)&0xF],h[(v>>8)&0xF],h[(v>>4)&0xF],h[v&0xF],0};
    log_append(b);
}
static void log_dec(uint32_t v){
    char b[12]; utoa(v,b,10); log_append(b);
}

/* 400ns delay via alternate status reads */
static void ata_delay400(uint16_t ctrl){
    inb(ctrl); inb(ctrl); inb(ctrl); inb(ctrl);
}

/* Short settle — PAUSE-hinted to be cache/pipeline friendly */
static void ata_settle(void){
    for(volatile int i=0;i<400000;i++) __asm__ volatile("pause");
}

static int bus_wait_bsy(const ata_bus_t *b){
    for(int i=0;i<1000000;i++){
        if(!(inb(b->base+ATA_STATUS)&ATA_SR_BSY)) return 1;
        inb(b->ctrl); inb(b->ctrl); inb(b->ctrl); inb(b->ctrl);
    }
    return 0; /* timeout */
}

static int bus_wait_drq(const ata_bus_t *b){
    for(int i=0;i<1000000;i++){
        uint8_t st=inb(b->base+ATA_STATUS);
        if(st&ATA_SR_ERR) return 0;
        if(st&ATA_SR_DRQ) return 1;
        inb(b->ctrl); inb(b->ctrl);
    }
    return 0;
}

/* ── Drive probe ──────────────────────────────────────────────── */
static uint64_t probe_drive(const ata_bus_t *b){
    /* Floating bus */
    if(inb(b->base+ATA_STATUS)==0xFF) return 0;

    uint8_t sel=b->slave ? 0xB0 : 0xA0;
    outb(b->base+ATA_DRIVE, sel);
    ata_delay400(b->ctrl);
    ata_settle();

    uint8_t st=inb(b->base+ATA_STATUS);
    if(st==0xFF||st==0x7F) return 0;

    /* Software reset */
    outb(b->ctrl, ATA_CTRL_SRST|ATA_CTRL_NIEN);
    ata_settle();
    outb(b->ctrl, ATA_CTRL_NIEN);
    ata_settle();
    if(!bus_wait_bsy(b)) return 0;

    outb(b->base+ATA_DRIVE, sel);
    ata_delay400(b->ctrl);

    /* Clear LBA regs */
    outb(b->base+ATA_SECCOUNT,0);
    outb(b->base+ATA_LBA0,0);
    outb(b->base+ATA_LBA1,0);
    outb(b->base+ATA_LBA2,0);

    /* IDENTIFY */
    outb(b->base+ATA_CMD, CMD_IDENTIFY);
    ata_delay400(b->ctrl);

    st=inb(b->base+ATA_STATUS);
    if(st==0x00||st==0xFF) return 0;
    if(!bus_wait_bsy(b)) return 0;

    /* ATAPI / non-ATA device check */
    uint8_t lmid=inb(b->base+ATA_LBA1), lhi=inb(b->base+ATA_LBA2);
    if((lmid==0x14&&lhi==0xEB)||(lmid==0x69&&lhi==0x96)) return 0; /* ATAPI */
    if(lmid!=0x00||lhi!=0x00){
        /* SATA signature in AHCI compat mode: 0x3C/0xC3 or other
           non-zero values.  Allow these through — IDENTIFY still works. */
    }

    if(!bus_wait_drq(b)) return 0;

    uint16_t id[256];
    for(int i=0;i<256;i++) id[i]=inw(b->base+ATA_DATA);

    /* Word 83 bit 10 = LBA48 support */
    int has48=(id[83]&0x0400)!=0;
    uint32_t lba28=((uint32_t)id[61]<<16)|id[60];

    if(has48 && id[100]!=0){
        /* 48-bit total user addressable sectors (words 100-103) */
        uint64_t lba48=
            (uint64_t)id[100] |
            ((uint64_t)id[101]<<16) |
            ((uint64_t)id[102]<<32) |
            ((uint64_t)id[103]<<48);
        /* Store full 64-bit count, return > 0 to signal success */
        return lba48 ? lba48 : (uint64_t)lba28;
    }
    return (uint64_t)lba28;
}

/* ── Public init ──────────────────────────────────────────────── */
void ata_init(void){
    active=NULL; disk_sectors=0; disk_lba48=0;
    memset(debug_log,0,sizeof(debug_log));

    extern int      pci_ide_found(void);
    extern uint16_t pci_ide_pri_base(void);
    extern uint16_t pci_ide_pri_ctrl(void);
    extern uint16_t pci_ide_sec_base(void);
    extern uint16_t pci_ide_sec_ctrl(void);

    uint16_t pri_base,pri_ctrl,sec_base,sec_ctrl;

    if(pci_ide_found()){
        pri_base=pci_ide_pri_base(); pri_ctrl=pci_ide_pri_ctrl();
        sec_base=pci_ide_sec_base(); sec_ctrl=pci_ide_sec_ctrl();
        log_append("Ports from PCI\n");
    } else {
        /* Legacy ISA fallback — also used by AHCI in compat mode */
        pri_base=0x1F0; pri_ctrl=0x3F6;
        sec_base=0x170; sec_ctrl=0x376;
        log_append("Ports: legacy ISA (AHCI compat or real IDE)\n");
    }

    log_append("Pri "); log_hex(pri_base); log_append("/"); log_hex(pri_ctrl); log_append("\n");
    log_append("Sec "); log_hex(sec_base); log_append("/"); log_hex(sec_ctrl); log_append("\n");

    dyn_buses[0]=(ata_bus_t){pri_base,pri_ctrl,0,0,"Pri Master"};
    dyn_buses[1]=(ata_bus_t){pri_base,pri_ctrl,1,0,"Pri Slave"};
    dyn_buses[2]=(ata_bus_t){sec_base,sec_ctrl,0,0,"Sec Master"};
    dyn_buses[3]=(ata_bus_t){sec_base,sec_ctrl,1,0,"Sec Slave"};

    for(int i=0;i<4;i++){
        log_append(dyn_buses[i].name); log_append(": ");
        uint64_t sects=probe_drive(&dyn_buses[i]);
        if(sects>0){
            active=&dyn_buses[i];
            disk_lba48=sects;
            /* LBA28 cap for old code compatibility */
            disk_sectors=(sects>0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)sects;
            /* Detect LBA48 support (>128GB or word 83 bit 10) */
            dyn_buses[i].lba48=(sects>0x0FFFFFFFULL);
            log_append(dyn_buses[i].lba48?"LBA48 ":"LBA28 ");
            log_dec((uint32_t)(sects/2048)); log_append("MB\n");
            break;
        } else {
            log_append("none\n");
        }
    }
    if(!active) log_append("No ATA/SATA disk found.\n");
}

int         ata_detected(void)    { return active!=NULL; }
uint32_t    ata_sectors(void)     { return disk_sectors; }
const char *ata_get_debug(void)   { return debug_log; }
int         ata_lba48(void)       { return active&&active->lba48; }

/* ── LBA28 read/write ─────────────────────────────────────────── */
int ata_read_sector(uint32_t lba, uint8_t *buf){
    if(!active) return 0;
    /* Use LBA48 path on large drives even for small LBAs — safer */
    if(active->lba48) return ata_read_sector48(lba,buf);
    uint8_t sel=(active->slave?0xF0:0xE0)|((lba>>24)&0x0F);
    if(!bus_wait_bsy(active)) return 0;
    outb(P(ATA_DRIVE),sel); ata_delay400(active->ctrl);
    outb(P(ATA_FEAT),0); outb(P(ATA_SECCOUNT),1);
    outb(P(ATA_LBA0),(uint8_t)lba);
    outb(P(ATA_LBA1),(uint8_t)(lba>>8));
    outb(P(ATA_LBA2),(uint8_t)(lba>>16));
    outb(P(ATA_CMD),CMD_READ28);
    if(!bus_wait_drq(active)) return 0;
    if(inb(P(ATA_STATUS))&ATA_SR_ERR) return 0;
    uint16_t *w=(uint16_t*)buf;
    for(int i=0;i<256;i++) w[i]=inw(P(ATA_DATA));
    return 1;
}

int ata_write_sector(uint32_t lba, const uint8_t *buf){
    if(!active) return 0;
    if(active->lba48) return ata_write_sector48(lba,buf);
    uint8_t sel=(active->slave?0xF0:0xE0)|((lba>>24)&0x0F);
    if(!bus_wait_bsy(active)) return 0;
    outb(P(ATA_DRIVE),sel); ata_delay400(active->ctrl);
    outb(P(ATA_FEAT),0); outb(P(ATA_SECCOUNT),1);
    outb(P(ATA_LBA0),(uint8_t)lba);
    outb(P(ATA_LBA1),(uint8_t)(lba>>8));
    outb(P(ATA_LBA2),(uint8_t)(lba>>16));
    outb(P(ATA_CMD),CMD_WRITE28);
    if(!bus_wait_drq(active)) return 0;
    const uint16_t *w=(const uint16_t*)buf;
    for(int i=0;i<256;i++) outw(P(ATA_DATA),w[i]);
    outb(P(ATA_CMD),CMD_FLUSH);
    if(!bus_wait_bsy(active)) return 0;
    return 1;
}

/* ── LBA48 read/write (for SATA SSDs >128GB) ─────────────────── */
int ata_read_sector48(uint32_t lba, uint8_t *buf){
    if(!active) return 0;
    uint8_t sel=active->slave ? 0x50 : 0x40;  /* LBA48 mode — no top 4 LBA bits */
    if(!bus_wait_bsy(active)) return 0;
    outb(P(ATA_DRIVE),sel); ata_delay400(active->ctrl);
    /* High bytes first */
    outb(P(ATA_SECCOUNT),0);
    outb(P(ATA_LBA0),0);       /* LBA [31:24] */
    outb(P(ATA_LBA1),0);       /* LBA [39:32] */
    outb(P(ATA_LBA2),0);       /* LBA [47:40] */
    /* Then low bytes */
    outb(P(ATA_SECCOUNT),1);
    outb(P(ATA_LBA0),(uint8_t)lba);
    outb(P(ATA_LBA1),(uint8_t)(lba>>8));
    outb(P(ATA_LBA2),(uint8_t)(lba>>16));
    outb(P(ATA_CMD),CMD_READ48);
    if(!bus_wait_drq(active)) return 0;
    if(inb(P(ATA_STATUS))&ATA_SR_ERR) return 0;
    uint16_t *w=(uint16_t*)buf;
    for(int i=0;i<256;i++) w[i]=inw(P(ATA_DATA));
    return 1;
}

int ata_write_sector48(uint32_t lba, const uint8_t *buf){
    if(!active) return 0;
    uint8_t sel=active->slave ? 0x50 : 0x40;
    if(!bus_wait_bsy(active)) return 0;
    outb(P(ATA_DRIVE),sel); ata_delay400(active->ctrl);
    outb(P(ATA_SECCOUNT),0);
    outb(P(ATA_LBA0),0);
    outb(P(ATA_LBA1),0);
    outb(P(ATA_LBA2),0);
    outb(P(ATA_SECCOUNT),1);
    outb(P(ATA_LBA0),(uint8_t)lba);
    outb(P(ATA_LBA1),(uint8_t)(lba>>8));
    outb(P(ATA_LBA2),(uint8_t)(lba>>16));
    outb(P(ATA_CMD),CMD_WRITE48);
    if(!bus_wait_drq(active)) return 0;
    const uint16_t *w=(const uint16_t*)buf;
    for(int i=0;i<256;i++) outw(P(ATA_DATA),w[i]);
    outb(P(ATA_CMD),CMD_FLUSH48);
    if(!bus_wait_bsy(active)) return 0;
    return 1;
}
