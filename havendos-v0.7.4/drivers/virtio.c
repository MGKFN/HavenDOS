/*
 * virtio.c  --  VirtIO Block driver for HavenDOS
 *
 * Targets the legacy VirtIO 0.9.x I/O-port interface
 * (VID:1AF4 DID:1001) confirmed on UTM SE / QEMU TCG.
 *
 * Pure polling -- IRQs are never used (they don't fire on
 * UTM SE TCG). Identical pattern to the keyboard/mouse drivers.
 *
 * API:
 *   virtio_blk_init()          -- call once from kernel_main
 *   virtio_blk_read(sec, buf)  -- read one 512-byte sector
 *   virtio_blk_write(sec, buf) -- write one 512-byte sector
 *   virtio_blk_ready()         -- 1 if disk is usable
 *   virtio_blk_sectors()       -- total sector count
 *   virtio_blk_log()           -- debug string
 */

#include "../include/types.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/virtio.h"

/* ── PCI config helpers ───────────────────────────────────── */
#define PCI_ADDR  0xCF8
#define PCI_DATA  0xCFC

static uint32_t pci_r32(uint8_t bus, uint8_t dev, uint8_t reg) {
    outl(PCI_ADDR, 0x80000000u | ((uint32_t)bus<<16)
                               | ((uint32_t)dev<<11)
                               | (reg & 0xFC));
    return inl(PCI_DATA);
}
static void pci_w32(uint8_t bus, uint8_t dev, uint8_t reg, uint32_t v) {
    outl(PCI_ADDR, 0x80000000u | ((uint32_t)bus<<16)
                               | ((uint32_t)dev<<11)
                               | (reg & 0xFC));
    outl(PCI_DATA, v);
}

/* ── VirtIO legacy register offsets (from I/O base) ──────── */
#define VREG_DEVFEAT   0x00
#define VREG_DRVFEAT   0x04
#define VREG_QPFN      0x08
#define VREG_QSIZE     0x0C
#define VREG_QSEL      0x0E
#define VREG_QNOTIFY   0x10
#define VREG_STATUS    0x12
#define VREG_ISR       0x13
#define VREG_CAPACITY  0x14   /* 64-bit capacity in sectors */

/* Device status flags */
#define VSTAT_ACK        1
#define VSTAT_DRIVER     2
#define VSTAT_DRIVER_OK  4
#define VSTAT_FAILED   128

/* Descriptor flags */
#define VDESC_NEXT   1
#define VDESC_WRITE  2        /* device writes into this buffer */

/* ── VirtQueue definitions ────────────────────────────────── */
#define QSIZE 256

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vdesc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSIZE];
} __attribute__((packed)) vavail_t;

typedef struct { uint32_t id, len; } __attribute__((packed)) vused_elem_t;
typedef struct {
    uint16_t    flags;
    uint16_t    idx;
    vused_elem_t ring[QSIZE];
} __attribute__((packed)) vused_t;

/* Queue memory: one 12KB slot per drive (max 4 drives).
   Each device gets its own descriptor table, avail ring, and
   used ring so drives never share queue state.
   Page-aligned per VirtIO legacy spec.                        */
#define MAX_DRIVES_Q 4
static uint8_t  qmem[MAX_DRIVES_Q][12288] __attribute__((aligned(4096)));

/* Currently selected queue slot (index into qmem[]) */
static int      g_qslot = 0;

#define VDESC   ((volatile vdesc_t*) (qmem[g_qslot]))
#define VAVAIL  ((volatile vavail_t*)(qmem[g_qslot] + 16*QSIZE))
#define VUSED   ((volatile vused_t*) (qmem[g_qslot] + 8192))

/* VirtIO block request header */
typedef struct {
    uint32_t type;       /* 0 = read, 1 = write */
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) blk_req_t;

/* Per-request buffers (static -- one request at a time) */
static blk_req_t         s_hdr     __attribute__((aligned(16)));
static uint8_t           s_data[512] __attribute__((aligned(16)));
static volatile uint8_t  s_status  __attribute__((aligned(16)));

/* Driver state */
static int      g_ready   = 0;
static uint16_t g_io      = 0;
static uint64_t g_sectors = 0;
static char     g_log[256];

/* ── Logging ──────────────────────────────────────────────── */
static void vlog(const char *s) {
    int l = strlen(g_log), sl = strlen(s);
    if (l + sl < (int)sizeof(g_log) - 1) {
        memcpy(g_log + l, s, sl + 1);
    }
}
static void vlog_hex16(uint16_t v) {
    const char *h = "0123456789ABCDEF";
    char b[7];
    b[0]='0'; b[1]='x';
    b[2]=h[(v>>12)&0xF]; b[3]=h[(v>>8)&0xF];
    b[4]=h[(v>>4)&0xF];  b[5]=h[v&0xF];
    b[6]=0;
    vlog(b);
}
static void vlog_dec(uint32_t v) {
    if (!v) { vlog("0"); return; }
    char b[12]; int i = 0;
    while (v) { b[i++] = '0' + v % 10; v /= 10; }
    b[i] = 0;
    /* reverse */
    for (int a=0, z=i-1; a<z; a++,z--) {
        char t = b[a]; b[a] = b[z]; b[z] = t;
    }
    vlog(b);
}

/* ── PCI: find VirtIO block device ───────────────────────── */
static uint16_t find_virtio_blk(void) {
    for (int bus = 0; bus < 8; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = pci_r32(bus, dev, 0x00);
            if ((id & 0xFFFF) != 0x1AF4) continue;
            if (((id >> 16) & 0xFFFF) != 0x1001) continue;

            /* Enable bus-master + I/O space */
            uint32_t cmd = pci_r32(bus, dev, 0x04);
            pci_w32(bus, dev, 0x04, cmd | 0x05);

            /* BAR0 must be an I/O BAR */
            uint32_t bar0 = pci_r32(bus, dev, 0x10);
            if (!(bar0 & 1)) return 0;

            vlog("VirtIO BLK bus=");
            vlog_dec(bus);
            vlog(" dev=");
            vlog_dec(dev);
            vlog(" io=");
            vlog_hex16((uint16_t)(bar0 & 0xFFFC));
            vlog("\n");

            return (uint16_t)(bar0 & 0xFFFC);
        }
    }
    return 0;
}

/* ── VirtIO device initialisation ────────────────────────── */
static int virtio_init(uint16_t io) {
    /* Reset device */
    outb(io + VREG_STATUS, 0);
    /* Acknowledge */
    outb(io + VREG_STATUS, VSTAT_ACK);
    outb(io + VREG_STATUS, VSTAT_ACK | VSTAT_DRIVER);

    /* Accept all device features */
    uint32_t devfeat = inl(io + VREG_DEVFEAT);
    outl(io + VREG_DRVFEAT, devfeat);

    /* Select queue 0 (the only block queue) */
    outw(io + VREG_QSEL, 0);
    uint16_t qsz = inw(io + VREG_QSIZE);
    if (qsz == 0) { vlog("ERR:qsz=0\n"); return 0; }
    if (qsz > QSIZE) qsz = QSIZE;

    /* Zero queue memory for current slot */
    for (int i = 0; i < 12288; i++) qmem[g_qslot][i] = 0;

    /* Write PFN (physical page number) — each slot has its own page */
    uint32_t pfn = (uint32_t)(uintptr_t)qmem[g_qslot] >> 12;
    outl(io + VREG_QPFN, pfn);

    /* Driver OK */
    outb(io + VREG_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_DRIVER_OK);
    uint8_t st = inb(io + VREG_STATUS);
    if (st & VSTAT_FAILED) { vlog("ERR:FAILED\n"); return 0; }

    /* Read disk capacity (64-bit, two 32-bit reads) */
    uint32_t cap_lo = inl(io + VREG_CAPACITY);
    uint32_t cap_hi = inl(io + VREG_CAPACITY + 4);
    g_sectors = ((uint64_t)cap_hi << 32) | cap_lo;

    vlog("qsz=");
    vlog_dec(qsz);
    vlog(" cap=");
    vlog_dec((uint32_t)(g_sectors / 2048));
    vlog("MB\n");

    return 1;
}

/* ── Submit one 3-descriptor request and poll for completion ── */
static int virtio_do(uint16_t io, int write, uint64_t sector,
                     const uint8_t *wbuf, uint8_t *rbuf) {

    /* Fill request header */
    s_hdr.type     = write ? 1 : 0;
    s_hdr.reserved = 0;
    s_hdr.sector   = sector;
    s_status       = 0xFF;   /* sentinel */

    /* Copy write data into staging buffer */
    if (write && wbuf) {
        for (int i = 0; i < 512; i++) s_data[i] = wbuf[i];
    }

    /* Descriptor 0: request header (host -> device) */
    VDESC[0].addr  = (uint64_t)(uintptr_t)&s_hdr;
    VDESC[0].len   = sizeof(blk_req_t);
    VDESC[0].flags = VDESC_NEXT;
    VDESC[0].next  = 1;

    /* Descriptor 1: data buffer */
    VDESC[1].addr  = (uint64_t)(uintptr_t)s_data;
    VDESC[1].len   = 512;
    VDESC[1].flags = (write ? 0 : VDESC_WRITE) | VDESC_NEXT;
    VDESC[1].next  = 2;

    /* Descriptor 2: status byte (device writes back) */
    VDESC[2].addr  = (uint64_t)(uintptr_t)&s_status;
    VDESC[2].len   = 1;
    VDESC[2].flags = VDESC_WRITE;
    VDESC[2].next  = 0;

    /* Snapshot used ring index */
    uint16_t used_before = VUSED->idx;

    /* Post descriptor chain head into available ring */
    uint16_t slot = VAVAIL->idx % QSIZE;
    VAVAIL->ring[slot] = 0;
    __asm__ volatile("" ::: "memory");
    VAVAIL->idx++;
    __asm__ volatile("" ::: "memory");

    /* Notify device */
    outw(io + VREG_QNOTIFY, 0);

    /* Poll used ring for completion. Host scheduling (esp. under
       UTM SE / TCG on real hardware, vs an idle dev container) can
       stall the vCPU thread long enough that a single fixed-length
       busy-wait occasionally times out even though the device WILL
       complete the request shortly after. Rather than guess a
       "big enough" iteration count, poll in rounds and re-notify
       the device between rounds — completion is also re-checked
       immediately after each re-notify in case it landed in the
       gap between the last poll and the renotify itself. */
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 20000000; i++) {
            __asm__ volatile("" ::: "memory");
            if (VUSED->idx != used_before) goto done;
        }
        /* Timed out this round — nudge the device again in case the
           original notify was missed or processed unusually slowly,
           then re-check immediately before starting the next round. */
        outw(io + VREG_QNOTIFY, 0);
        __asm__ volatile("" ::: "memory");
        if (VUSED->idx != used_before) goto done;
    }
    vlog("ERR:TIMEOUT\n");
    return 0;

done:
    if (s_status != 0) {
        vlog("ERR:status!=0\n");
        return 0;
    }

    /* Copy read data out of staging buffer */
    if (!write && rbuf) {
        for (int i = 0; i < 512; i++) rbuf[i] = s_data[i];
    }
    return 1;
}

/* ── Public API ───────────────────────────────────────────── */

int virtio_blk_init(void) {
    g_log[0]  = 0;
    g_ready   = 0;
    g_io      = 0;
    g_sectors = 0;

    uint16_t io = find_virtio_blk();
    if (!io) {
        vlog("no VirtIO BLK found\n");
        return 0;
    }
    if (!virtio_init(io)) return 0;

    g_io    = io;
    g_ready = 1;
    vlog("ready\n");
    return 1;
}

int virtio_blk_read(uint64_t sector, uint8_t *buf) {
    if (!g_ready) return 0;
    return virtio_do(g_io, 0, sector, NULL, buf);
}

int virtio_blk_write(uint64_t sector, const uint8_t *buf) {
    if (!g_ready) return 0;
    return virtio_do(g_io, 1, sector, buf, NULL);
}

int          virtio_blk_ready(void)   { return g_ready;   }
uint64_t     virtio_blk_sectors(void) { return g_sectors;  }
const char  *virtio_blk_log(void)     { return g_log;      }

/* ── Multi-drive scan API (for installer) ─────────────────── */
#define VIRTIO_MAX_DRIVES 4

typedef struct {
    uint16_t io;
    uint8_t  bus, dev;
    uint64_t sectors;
    int      ready;
    int      queue_inited;
    int      qslot;           /* index into qmem[] for this drive */
} virtio_drive_t;

static virtio_drive_t g_drives[VIRTIO_MAX_DRIVES];
static int            g_drive_count = 0;

int virtio_scan_drives(void) {
    g_drive_count = 0;

    for (int bus = 0; bus < 8 && g_drive_count < VIRTIO_MAX_DRIVES; bus++) {
        for (int dev = 0; dev < 32 && g_drive_count < VIRTIO_MAX_DRIVES; dev++) {
            uint32_t id = pci_r32(bus, dev, 0x00);
            if ((id & 0xFFFF) != 0x1AF4) continue;
            if (((id >> 16) & 0xFFFF) != 0x1001) continue;

            uint32_t cmd = pci_r32(bus, dev, 0x04);
            pci_w32(bus, dev, 0x04, cmd | 0x05);

            uint32_t bar0 = pci_r32(bus, dev, 0x10);
            if (!(bar0 & 1)) continue;

            uint16_t io = (uint16_t)(bar0 & 0xFFFC);

            virtio_drive_t *d = &g_drives[g_drive_count];
            d->io          = io;
            d->bus         = (uint8_t)bus;
            d->dev         = (uint8_t)dev;
            d->ready       = 0;
            d->sectors     = 0;
            d->qslot       = g_drive_count < MAX_DRIVES_Q ? g_drive_count : 0;
            /* Drive already initialised by virtio_blk_init gets slot 0 */
            d->queue_inited = (io == g_io) ? 1 : 0;

            /* Read capacity without disturbing queue state */
            uint32_t cap_lo = inl(io + VREG_CAPACITY);
            uint32_t cap_hi = inl(io + VREG_CAPACITY + 4);
            d->sectors = ((uint64_t)cap_hi << 32) | cap_lo;
            d->ready   = 1;

            g_drive_count++;
        }
    }
    return g_drive_count;
}

/*
 * virtio_init_drive() — fully initialise the VirtIO queue for
 * a specific drive so virtio_drive_read/write() can use it.
 * Must be called before the first I/O on any drive other than
 * drive 0 (which is already initialised by virtio_blk_init).
 * Safe to call on drive 0 — it re-initialises the shared queue.
 */
int virtio_init_drive(int idx) {
    if(idx < 0 || idx >= g_drive_count) return 0;
    virtio_drive_t *d = &g_drives[idx];
    if(!d->ready) return 0;

    /* Each drive gets its own queue slot — no shared state.
       g_qslot selects which qmem[]/VDESC/VAVAIL/VUSED slot
       virtio_init and virtio_do use for this drive.           */
    if(idx < MAX_DRIVES_Q) g_qslot = idx;
    if(!virtio_init(d->io)) return 0;
    d->queue_inited = 1;
    d->qslot = (idx < MAX_DRIVES_Q) ? idx : 0;

    /* Also update the primary driver state if this is drive 0 */
    if(d->io == g_io){
        uint32_t cap_lo = inl(d->io + VREG_CAPACITY);
        uint32_t cap_hi = inl(d->io + VREG_CAPACITY + 4);
        g_sectors = ((uint64_t)cap_hi << 32) | cap_lo;
    }
    uint32_t cap_lo = inl(d->io + VREG_CAPACITY);
    uint32_t cap_hi = inl(d->io + VREG_CAPACITY + 4);
    d->sectors = ((uint64_t)cap_hi << 32) | cap_lo;
    return 1;
}

int            virtio_drive_count(void)           { return g_drive_count; }
uint64_t       virtio_drive_sectors(int idx)      { return (idx>=0&&idx<g_drive_count)?g_drives[idx].sectors:0; }
uint16_t       virtio_drive_io(int idx)           { return (idx>=0&&idx<g_drive_count)?g_drives[idx].io:0; }
uint8_t        virtio_drive_bus(int idx)          { return (idx>=0&&idx<g_drive_count)?g_drives[idx].bus:0; }
uint8_t        virtio_drive_dev(int idx)          { return (idx>=0&&idx<g_drive_count)?g_drives[idx].dev:0; }

int virtio_drive_read(int idx, uint64_t sector, uint8_t *buf) {
    if(idx<0||idx>=g_drive_count||!g_drives[idx].ready) return 0;
    /* Auto-init queue if not done yet */
    if(!g_drives[idx].queue_inited) {
        if(!virtio_init_drive(idx)) return 0;
    }
    /* Switch to this drive's queue slot before I/O */
    g_qslot = g_drives[idx].qslot;
    return virtio_do(g_drives[idx].io, 0, sector, NULL, buf);
}

int virtio_drive_write(int idx, uint64_t sector, const uint8_t *buf) {
    if(idx<0||idx>=g_drive_count||!g_drives[idx].ready) return 0;
    /* Auto-init queue if not done yet */
    if(!g_drives[idx].queue_inited) {
        if(!virtio_init_drive(idx)) return 0;
    }
    /* Switch to this drive's queue slot before I/O */
    g_qslot = g_drives[idx].qslot;
    return virtio_do(g_drives[idx].io, 1, sector, buf, NULL);
}

