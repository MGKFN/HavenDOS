/* ================================================================
   drivers/ahci.c -- see include/ahci.h for how a command happens
   ================================================================ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/pci.h"
#include "../include/mmio.h"
#include "../include/ahci.h"
#include "../include/klog.h"

extern void sleep_ms(uint32_t ms);

/* ── global registers ─────────────────────────────────────────────── */
#define HBA_CAP        0x00
#define HBA_GHC        0x04
#define HBA_IS         0x08
#define HBA_PI         0x0C
#define HBA_VS         0x10
#define HBA_CAP2       0x24
#define HBA_BOHC       0x28

#define GHC_HR         0x00000001u     /* HBA reset                     */
#define GHC_IE         0x00000002u     /* interrupt enable              */
#define GHC_AE         0x80000000u     /* AHCI enable                   */

#define CAP_S64A       0x80000000u     /* 64-bit addressing supported   */
#define CAP2_BOH       0x00000001u     /* BIOS/OS handoff supported     */
#define BOHC_BOS       0x00000001u
#define BOHC_OOS       0x00000002u
#define BOHC_BB        0x00000010u

/* ── per-port registers, at 0x100 + port*0x80 ─────────────────────── */
#define PORT_BASE(p)   (0x100u + (uint32_t)(p) * 0x80u)
#define PxCLB          0x00
#define PxCLBU         0x04
#define PxFB           0x08
#define PxFBU          0x0C
#define PxIS           0x10
#define PxIE           0x14
#define PxCMD          0x18
#define PxTFD          0x20
#define PxSIG          0x24
#define PxSSTS         0x28
#define PxSCTL         0x2C
#define PxSERR         0x30
#define PxCI           0x38

#define CMD_ST         0x0001u         /* start                         */
#define CMD_SUD        0x0002u         /* spin-up device                */
#define CMD_POD        0x0004u         /* power on device               */
#define CMD_FRE        0x0010u         /* FIS receive enable            */
#define CMD_FR         0x4000u         /* FIS receive running           */
#define CMD_CR         0x8000u         /* command list running          */

#define TFD_BSY        0x80u
#define TFD_DRQ        0x08u
#define TFD_ERR        0x01u

#define SIG_ATA        0x00000101u
#define SIG_ATAPI      0xEB140101u

#define FIS_H2D        0x27u
#define ATA_IDENTIFY   0xECu
#define ATA_IDENTIFY_P 0xA1u           /* IDENTIFY PACKET DEVICE        */
#define ATA_READ_DMA   0x25u           /* READ DMA EXT                  */
#define ATA_WRITE_DMA  0x35u           /* WRITE DMA EXT                 */

/* ── the DMA structures the controller reads ──────────────────────────
   Static and aligned rather than allocated: this kernel is identity
   mapped, so a physical address is the pointer, and the alignment the
   specification demands (1 KB for the command list, 256 bytes for the
   FIS area, 128 for a command table) is easier to guarantee at compile
   time than to ask an allocator for. One set per drive, not per port,
   because a drive is what a caller addresses. */
typedef struct {
    uint16_t flags;        /* CFL in 0:4, A=5, W=6, P=7, PRDTL in 16:31 */
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
} __attribute__((packed)) cmd_hdr_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc;          /* byte count - 1 in 0:21, I in 31          */
} __attribute__((packed)) prd_t;

typedef struct {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    prd_t    prdt[8];
} __attribute__((packed)) cmd_tbl_t;

static cmd_hdr_t  g_clist[AHCI_MAX_DRIVES][32] __attribute__((aligned(1024)));
static uint8_t    g_fis  [AHCI_MAX_DRIVES][256] __attribute__((aligned(256)));
static cmd_tbl_t  g_ctbl [AHCI_MAX_DRIVES]     __attribute__((aligned(128)));
static uint8_t    g_dma  [AHCI_MAX_DRIVES][512] __attribute__((aligned(512)));
static uint8_t    g_idbuf[AHCI_MAX_DRIVES][512] __attribute__((aligned(512)));

typedef struct {
    uint32_t abar;
    int      port;
    uint64_t sectors;
    int      atapi;
    int      ok;
    char     model[41];
} ahci_drive_t;

static ahci_drive_t g_drv[AHCI_MAX_DRIVES];
static int          g_n = 0;

static char g_log[1024];
static void logs(const char *s){
    uint32_t n = 0; while(g_log[n]) n++;
    while(*s && n < sizeof g_log - 1) g_log[n++] = *s++;
    g_log[n] = 0;
}
static void logd(uint32_t v){
    char b[12]; int i = 0;
    if(!v){ logs("0"); return; }
    while(v && i < 11){ b[i++] = (char)('0' + v % 10u); v /= 10u; }
    { char o[12]; int j = 0; while(i) o[j++] = b[--i]; o[j] = 0; logs(o); }
}

static uint32_t pr(uint32_t abar, int port, uint32_t off){
    return mmio_read32(abar, PORT_BASE(port) + off);
}
static void pw(uint32_t abar, int port, uint32_t off, uint32_t v){
    mmio_write32(abar, PORT_BASE(port) + off, v);
}

/* ── stopping and starting a port ─────────────────────────────────────
   Both halves, both directions, and the waits are not optional: writing
   PxCLB while the command list is running is undefined, and the
   specification's word for it is that the controller's behaviour is
   "indeterminate". */
static int port_stop(uint32_t abar, int port)
{
    uint32_t cmd = pr(abar, port, PxCMD);
    pw(abar, port, PxCMD, cmd & ~(CMD_ST | CMD_FRE));
    for(int i = 0; i < 1000; i++){
        cmd = pr(abar, port, PxCMD);
        if(!(cmd & (CMD_CR | CMD_FR))) return 1;
        sleep_ms(1);
    }
    return 0;
}
static int port_start(uint32_t abar, int port)
{
    /* FRE before ST: the controller must be able to receive a FIS
       before it is told to fetch commands. */
    for(int i = 0; i < 1000; i++){
        if(!(pr(abar, port, PxCMD) & CMD_CR)) break;
        sleep_ms(1);
    }
    pw(abar, port, PxCMD, pr(abar, port, PxCMD) | CMD_FRE);
    pw(abar, port, PxCMD, pr(abar, port, PxCMD) | CMD_ST);
    return 1;
}

/* ── one command, polled ──────────────────────────────────────────────
   Slot 0 always. Returns 1 on success.

   write != 0 sets the W bit in the command header, which is what tells
   the controller which way the DMA goes. Getting it wrong on a write
   means the controller reads the disk into the buffer we meant to write
   from -- a silent no-op that reports success. */
static int run_cmd(int idx, uint8_t command, uint64_t lba, uint16_t count,
                   void *buf, uint32_t bytes, int write)
{
    ahci_drive_t *d = &g_drv[idx];
    cmd_hdr_t *h = &g_clist[idx][0];
    cmd_tbl_t *t = &g_ctbl[idx];
    uint8_t *f = t->cfis;

    memset(t, 0, sizeof *t);
    memset(h, 0, sizeof *h);

    f[0] = FIS_H2D;
    f[1] = 0x80;                     /* C: this is a command, not control */
    f[2] = command;
    f[3] = 0;                        /* features low                      */
    f[4] = (uint8_t)(lba      );
    f[5] = (uint8_t)(lba >>  8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40;                     /* LBA mode                          */
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10]= (uint8_t)(lba >> 40);
    f[11]= 0;                        /* features high                     */
    f[12]= (uint8_t)(count     );
    f[13]= (uint8_t)(count >> 8);

    h->flags = (uint16_t)((sizeof(uint32_t) == 4 ? 5u : 5u)   /* CFL in dwords */
                          | (write ? 0x40u : 0u));
    h->prdtl = bytes ? 1 : 0;
    h->prdbc = 0;
    h->ctba  = (uint32_t)(uintptr_t)t;
    h->ctbau = 0;

    if(bytes){
        t->prdt[0].dba  = (uint32_t)(uintptr_t)buf;
        t->prdt[0].dbau = 0;
        t->prdt[0].dbc  = (bytes - 1u) & 0x003FFFFFu;
    }

    /* Clear anything stale before issuing, or the first error we see is
       the last command's. */
    pw(d->abar, d->port, PxSERR, pr(d->abar, d->port, PxSERR));
    pw(d->abar, d->port, PxIS,   pr(d->abar, d->port, PxIS));

    for(int i = 0; i < 1000; i++){
        if(!(pr(d->abar, d->port, PxTFD) & (TFD_BSY | TFD_DRQ))) break;
        sleep_ms(1);
        if(i == 999) return 0;
    }

    pw(d->abar, d->port, PxCI, 1u);

    for(uint32_t spin = 0; spin < 2000000u; spin++){
        if(!(pr(d->abar, d->port, PxCI) & 1u)){
            uint32_t tfd = pr(d->abar, d->port, PxTFD);
            if(tfd & TFD_ERR) return 0;
            return 1;
        }
        if(pr(d->abar, d->port, PxIS) & 0x40000000u) return 0;  /* TFES */
        if((spin & 0x3FFFu) == 0x3FFFu) sleep_ms(1);
    }
    return 0;
}

/* ── IDENTIFY, and what it says ───────────────────────────────────── */
static uint16_t id16(const uint8_t *id, int word){
    /* Byte at a time. The buffer came off a device, and casting a device
       buffer to a wider type is what produced garbage on ARM/TCG in
       fs/fat16.c and net/ipv4.c. */
    return (uint16_t)id[word*2] | ((uint16_t)id[word*2 + 1] << 8);
}

static int identify(int idx)
{
    ahci_drive_t *d = &g_drv[idx];
    uint8_t *id = g_idbuf[idx];
    memset(id, 0, 512);

    if(!run_cmd(idx, d->atapi ? ATA_IDENTIFY_P : ATA_IDENTIFY,
                0, 0, id, 512, 0))
        return 0;

    /* Model, words 27..46, byte-swapped within each word. */
    for(int i = 0; i < 20; i++){
        d->model[i*2]     = (char)id[27*2 + i*2 + 1];
        d->model[i*2 + 1] = (char)id[27*2 + i*2];
    }
    d->model[40] = 0;
    for(int i = 39; i >= 0 && (d->model[i] == ' ' || !d->model[i]); i--)
        d->model[i] = 0;

    if(d->atapi){
        d->sectors = 0;              /* capacity comes from READ CAPACITY */
        return 1;
    }

    /* Word 83 bit 10 says the drive supports 48-bit addressing, and
       words 100..103 hold the count when it does. Selecting on CAPACITY
       instead -- which drivers/ata.c did -- gets it wrong in both
       directions: a small modern drive that only speaks LBA48, and a
       large old one that does not. */
    if(id16(id, 83) & 0x0400u){
        d->sectors = (uint64_t)id16(id, 100)
                   | ((uint64_t)id16(id, 101) << 16)
                   | ((uint64_t)id16(id, 102) << 32)
                   | ((uint64_t)id16(id, 103) << 48);
    } else {
        d->sectors = 0;
    }
    if(!d->sectors)
        d->sectors = (uint64_t)id16(id, 60) | ((uint64_t)id16(id, 61) << 16);
    return d->sectors != 0;
}

/* ── bringing one port up ─────────────────────────────────────────── */
static int port_init(uint32_t abar, int port)
{
    uint32_t ssts, det, ipm, sig;

    ssts = pr(abar, port, PxSSTS);
    det  = ssts & 0x0Fu;
    ipm  = (ssts >> 8) & 0x0Fu;
    if(det != 3u || ipm != 1u) return 0;      /* nothing there, or asleep */

    sig = pr(abar, port, PxSIG);
    if(sig != SIG_ATA && sig != SIG_ATAPI) return 0;
    if(g_n >= AHCI_MAX_DRIVES) return 0;

    int idx = g_n;
    ahci_drive_t *d = &g_drv[idx];
    memset(d, 0, sizeof *d);
    d->abar  = abar;
    d->port  = port;
    d->atapi = (sig == SIG_ATAPI);

    if(!port_stop(abar, port)) return 0;

    memset(g_clist[idx], 0, sizeof g_clist[idx]);
    memset(g_fis[idx],   0, sizeof g_fis[idx]);
    memset(&g_ctbl[idx], 0, sizeof g_ctbl[idx]);

    pw(abar, port, PxCLB,  (uint32_t)(uintptr_t)g_clist[idx]);
    pw(abar, port, PxCLBU, 0);
    pw(abar, port, PxFB,   (uint32_t)(uintptr_t)g_fis[idx]);
    pw(abar, port, PxFBU,  0);
    pw(abar, port, PxSERR, pr(abar, port, PxSERR));
    pw(abar, port, PxIE,   0);                 /* polled, never interrupts */

    port_start(abar, port);

    g_n++;                                     /* identify() addresses it  */
    if(!identify(idx)){
        g_n--;
        port_stop(abar, port);
        return 0;
    }
    d->ok = 1;

    logs("  port "); logd((uint32_t)port); logs(": ");
    logs(d->atapi ? "ATAPI " : "SATA ");
    logs(d->model);
    if(!d->atapi){ logs(" "); logd((uint32_t)(d->sectors / 2048u)); logs(" MB"); }
    logs("\n");
    if(d->atapi)
        klog(KLOG_OK, "AHCI port %u: ATAPI %s", (uint32_t)port, d->model);
    else
        klog(KLOG_OK, "AHCI port %u: %s, %u MB", (uint32_t)port, d->model,
             (uint32_t)(d->sectors / 2048u));
    return 1;
}

/* ── one controller ───────────────────────────────────────────────── */
static void hba_init(const pci_dev_t *p)
{
    uint32_t abar = pci_bar_mem(p, 5);
    if(!abar) return;

    pci_enable(p, 1);                          /* memory space + bus master */

    /* If the firmware owns the controller, ask for it. Skipping this on a
       machine whose BIOS has an AHCI driver loaded means two owners
       issuing commands to the same ports. */
    if(mmio_read32(abar, HBA_CAP2) & CAP2_BOH){
        mmio_write32(abar, HBA_BOHC, mmio_read32(abar, HBA_BOHC) | BOHC_OOS);
        for(int i = 0; i < 2000; i++){
            if(!(mmio_read32(abar, HBA_BOHC) & BOHC_BOS)) break;
            sleep_ms(1);
        }
    }

    mmio_write32(abar, HBA_GHC, mmio_read32(abar, HBA_GHC) | GHC_AE);
    mmio_write32(abar, HBA_GHC, mmio_read32(abar, HBA_GHC) & ~GHC_IE);

    uint32_t pi  = mmio_read32(abar, HBA_PI);
    uint32_t cap = mmio_read32(abar, HBA_CAP);
    uint32_t np  = (cap & 0x1Fu) + 1u;

    logs("AHCI at "); logd(abar); logs(", ");
    logd(np); logs(" ports, PI="); logd(pi); logs("\n");
    klog(KLOG_OK, "AHCI %2x:%2x.%u at %x, %u ports, implemented %x",
         p->bus, p->dev, p->fn, abar, np, pi);

    /* EVERY implemented port, not the first one with something on it.
       drivers/ata.c breaks out of its probe loop on the first hit, which
       is why a machine with two disks has only ever shown one. */
    for(uint32_t port = 0; port < 32u; port++){
        if(!(pi & (1u << port))) continue;
        port_init(abar, (int)port);
    }
}

int ahci_init(void)
{
    g_n = 0;
    g_log[0] = 0;

    for(int nth = 0; nth < 4; nth++){
        const pci_dev_t *p = pci_find_class(PCI_CLASS_STORAGE, 0x06, 0x01, nth);
        if(!p) break;
        hba_init(p);
    }
    if(!g_n) logs("No AHCI disk found.\n");
    return g_n;
}

int         ahci_count(void)        { return g_n; }
const char *ahci_debug(void)        { return g_log; }
uint64_t    ahci_sectors(int i)     { return (i>=0 && i<g_n) ? g_drv[i].sectors : 0; }
const char *ahci_model(int i)       { return (i>=0 && i<g_n) ? g_drv[i].model : ""; }
int         ahci_is_atapi(int i)    { return (i>=0 && i<g_n) ? g_drv[i].atapi : 0; }

int ahci_read_sector(int idx, uint64_t lba, uint8_t *buf)
{
    if(idx < 0 || idx >= g_n || !buf) return 0;
    if(!g_drv[idx].ok || g_drv[idx].atapi) return 0;
    if(g_drv[idx].sectors && lba >= g_drv[idx].sectors) return 0;
    if(!run_cmd(idx, ATA_READ_DMA, lba, 1, g_dma[idx], 512, 0)) return 0;
    memcpy(buf, g_dma[idx], 512);
    return 1;
}

int ahci_write_sector(int idx, uint64_t lba, const uint8_t *buf)
{
    if(idx < 0 || idx >= g_n || !buf) return 0;
    if(!g_drv[idx].ok || g_drv[idx].atapi) return 0;
    if(g_drv[idx].sectors && lba >= g_drv[idx].sectors) return 0;
    memcpy(g_dma[idx], buf, 512);
    return run_cmd(idx, ATA_WRITE_DMA, lba, 1, g_dma[idx], 512, 1);
}
