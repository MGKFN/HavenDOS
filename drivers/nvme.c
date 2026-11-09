/* ================================================================
   drivers/nvme.c -- NVM Express, read-only
   ================================================================
   See include/nvme.h for why there is no write path here.
   ================================================================ */

#include "types.h"
#include "io.h"
#include "../include/mmio.h"
#include "pci.h"
#include "nvme.h"
#include "klog.h"
#include "highfb.h"

extern void  sleep_ms(uint32_t);
/* include/string.h rather than two prototypes with uint32_t lengths. Those
   match size_t in the 32-bit kernel and not in a native test, so this file
   could not share a translation unit with anything that includes string.h
   -- which tests/test_nvmerw.c has to, to run the driver against a
   simulated controller. Same change, same reason, as in fs/gpt.c and
   fs/gpt_write.c. */
#include "string.h"

/* ---- Controller registers (NVMe base spec, figure "Register Definition") */
#define REG_CAP     0x00    /* 64: capabilities                            */
#define REG_VS      0x08    /* 32: version                                 */
#define REG_INTMS   0x0C
#define REG_INTMC   0x10
#define REG_CC      0x14    /* 32: controller configuration                */
#define REG_CSTS    0x1C    /* 32: controller status                       */
#define REG_AQA     0x24    /* 32: admin queue attributes                  */
#define REG_ASQ     0x28    /* 64: admin submission queue base             */
#define REG_ACQ     0x30    /* 64: admin completion queue base             */

#define CC_EN       (1u<<0)
#define CSTS_RDY    (1u<<0)
#define CSTS_CFS    (1u<<1)     /* controller fatal status */

/* Queue depths.  Small on purpose: this driver issues one command at a
   time and waits for it, so depth buys nothing and the queues have to come
   out of static, page-aligned storage. */
#define QD          8
#define SQE_BYTES   64
#define CQE_BYTES   16

typedef struct {
    uint32_t cdw0;  uint32_t nsid;  uint32_t rsvd2, rsvd3;
    uint64_t mptr;
    uint64_t prp1,  prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed)) nvme_sqe_t;

typedef struct {
    uint32_t dw0, dw1;
    uint16_t sq_head, sq_id;
    uint16_t cid;
    uint16_t status;        /* bit 0 is the phase tag */
} __attribute__((packed)) nvme_cqe_t;

/* Page-aligned, because every one of these addresses is handed to a
   controller that does DMA with it.  Identity-mapped paging (kernel/highfb.c)
   keeps physical == virtual for everything below 4 GB, so taking the address
   of a static is still a physical address. */
static uint8_t  admin_sq[NVME_MAX_DEVS][QD*SQE_BYTES] __attribute__((aligned(4096)));
static uint8_t  admin_cq[NVME_MAX_DEVS][QD*CQE_BYTES] __attribute__((aligned(4096)));
static uint8_t  io_sq   [NVME_MAX_DEVS][QD*SQE_BYTES] __attribute__((aligned(4096)));
static uint8_t  io_cq   [NVME_MAX_DEVS][QD*CQE_BYTES] __attribute__((aligned(4096)));
static uint8_t  dma_buf [NVME_MAX_DEVS][4096]         __attribute__((aligned(4096)));

typedef struct {
    uint32_t  base;             /* BAR0, as a usable (possibly mapped) address */
    int       bar_high;         /* BAR0 was above 4 GB and had to be paged in  */
    uint32_t  dstrd;            /* doorbell stride, from CAP                   */
    uint32_t  nsid;
    uint64_t  nsze;             /* namespace size, in logical blocks           */
    uint32_t  lba_bytes;
    uint16_t  asq_tail, acq_head, acq_phase;
    uint16_t  isq_tail, icq_head, icq_phase;
    uint16_t  cid;
    char      model[41];
    int       ok;
    int       ro;               /* the namespace says it is write protected */
} nvme_dev_t;

static nvme_dev_t g_dev[NVME_MAX_DEVS];
static int        g_count = 0;

/* ---- MMIO helpers, now from include/mmio.h ----
   These were four local inlines dereferencing volatile pointers. They are
   the same four functions, moved behind a guard a test can replace -- see
   the header for why. nvme.c is the first adopter on purpose: there is a
   passing suite over this driver, so if the shared versions are wrong, the
   existing 95 checks say so before AHCI is built on them. */
static inline uint32_t rd32(uint32_t base, uint32_t off){
    return mmio_read32(base, off);
}
static inline void wr32(uint32_t base, uint32_t off, uint32_t v){
    mmio_write32(base, off, v);
}
static inline uint64_t rd64(uint32_t base, uint32_t off){
    return mmio_read64(base, off);
}
static inline void wr64(uint32_t base, uint32_t off, uint64_t v){
    mmio_write64(base, off, v);
}

/* Doorbells start at 0x1000 and are (4 << CAP.DSTRD) bytes apart. */
static inline void ring_sq(nvme_dev_t *d, int qid, uint16_t tail){
    wr32(d->base, 0x1000u + (2u*(uint32_t)qid) * (4u << d->dstrd), tail);
}
static inline void ring_cq(nvme_dev_t *d, int qid, uint16_t head){
    wr32(d->base, 0x1000u + (2u*(uint32_t)qid + 1u) * (4u << d->dstrd), head);
}

/* Submit one command and wait for it.  Returns the status field with the
   phase bit masked off: 0 is success, anything else is the controller
   refusing and is reported rather than retried. */
/* Identify Namespace, parsed.

   NSZE at bytes 0..7, FLBAS at 26, the LBA format table at 128, NSATTR at
   99. All of it was inline in nvme_identify_ns(), reachable only after a
   controller reset, two admin commands and a completion -- so none of it
   was tested, and the three numbers it produces are the ones every read
   and write is computed from. A wrong block size silently reads the wrong
   eighth of every block.

   Returns 0 for a namespace that does not exist (NSZE zero), which is a
   legal answer for an unallocated namespace id.

   The LBA format index is masked to 4 bits and the table has 16 entries,
   so the read at 128 + flbas*4 + 2 cannot leave the 4096-byte page: the
   highest it reaches is byte 190. */
int nvme_ns_parse(const uint8_t *idns, uint64_t *nsze_out,
                  uint32_t *lba_bytes_out, int *ro_out)
{
    uint64_t nsze = 0;
    uint8_t  flbas;
    uint32_t lbads;
    int      i;

    if(!idns || !nsze_out || !lba_bytes_out || !ro_out) return 0;

    for(i = 0; i < 8; i++) nsze |= (uint64_t)idns[i] << (8*i);
    if(!nsze) return 0;

    flbas = idns[26] & 0x0Fu;
    lbads = idns[128 + (uint32_t)flbas*4u + 2u];
    /* 9 is 512 and 12 is 4096. Anything else is a drive reporting
       something this kernel has no bounce buffer for, and 512 is the safe
       reading -- dma_buf is 4096 bytes, so a larger block would overrun
       it on the first read. */
    if(lbads < 9 || lbads > 12) lbads = 9;

    *nsze_out      = nsze;
    *lba_bytes_out = 1u << lbads;
    *ro_out        = nvme_ns_write_protected(idns);
    return 1;
}

/* NSATTR, Identify Namespace byte 99. Bit 0 set means the namespace is
   write protected -- a real state on encrypted, provisioned and some
   OEM-locked drives, and one worth knowing BEFORE the installer offers the
   disk as a target. Every write would otherwise fail one sector at a time,
   part-way through a partition table.

   A function of its own, and not three bytes inline where it is used,
   because the identify path needs the whole admin queue simulated before a
   test can reach it. This takes the 4 KB structure and answers the
   question, so tests/test_nvmerw.c can hand it a crafted one. */
int nvme_ns_write_protected(const uint8_t *idns)
{
    if(!idns) return 0;
    return (idns[99] & 0x01u) ? 1 : 0;
}

static int submit_wait(nvme_dev_t *d, int qid, nvme_sqe_t *cmd, uint32_t *dw0)
{
    uint8_t  *sq   = qid ? io_sq[d - g_dev] : admin_sq[d - g_dev];
    uint8_t  *cq   = qid ? io_cq[d - g_dev] : admin_cq[d - g_dev];
    uint16_t *tail = qid ? &d->isq_tail : &d->asq_tail;
    uint16_t *head = qid ? &d->icq_head : &d->acq_head;
    uint16_t *ph   = qid ? &d->icq_phase : &d->acq_phase;

    cmd->cdw0 = (cmd->cdw0 & 0xFFFFu) | ((uint32_t)(++d->cid) << 16);
    memcpy(sq + (uint32_t)(*tail) * SQE_BYTES, cmd, SQE_BYTES);
    *tail = (uint16_t)((*tail + 1u) % QD);
    ring_sq(d, qid, *tail);

    /* Polled, like the USB driver and for the same reason: an interrupt
       cannot be relied on under UTM SE's interpreter.  The phase tag flips
       each time the controller wraps the queue, which is how a new entry is
       told from a stale one without zeroing anything. */
    for(uint32_t spin = 0; spin < 2000000u; spin++){
        volatile nvme_cqe_t *e =
            (volatile nvme_cqe_t*)(cq + (uint32_t)(*head) * CQE_BYTES);
        if((e->status & 1u) == *ph){
            uint16_t st = (uint16_t)(e->status >> 1);
            if(dw0) *dw0 = e->dw0;
            *head = (uint16_t)((*head + 1u) % QD);
            if(*head == 0) *ph ^= 1u;
            ring_cq(d, qid, *head);
            return (int)st;
        }
        if(rd32(d->base, REG_CSTS) & CSTS_CFS) return -2;   /* fatal */
        if((spin & 0x3FFFu) == 0x3FFFu) sleep_ms(1);
    }
    return -1;                                              /* timed out */
}

static int ctrl_wait_ready(nvme_dev_t *d, int want)
{
    for(uint32_t i = 0; i < 20000u; i++){
        uint32_t st = rd32(d->base, REG_CSTS);
        if(st & CSTS_CFS) return 0;
        if(((st & CSTS_RDY) ? 1 : 0) == want) return 1;
        sleep_ms(1);
    }
    return 0;
}

static int nvme_bring_up(const pci_dev_t *p, int slot)
{
    nvme_dev_t *d = &g_dev[slot];
    memset(d, 0, sizeof *d);

    /* BAR0 is a 64-bit memory BAR.  On a machine with enough RAM to push
       MMIO above 4 GB -- the same machines whose framebuffer ends up there
       -- the high half is non-zero and a 32-bit kernel cannot form the
       address without help.  Page it in rather than silently truncating,
       which would be a wild write to someone else's memory.

       v0.8.5.1: this decode used to be written out here, inline, and it
       was the only correct copy in the tree. pci_bar_mem() truncated, and
       usb/xhci_hc.c trusted it and refused to start on real hardware. The
       decode now lives in drivers/pci.c and this is a caller of it --
       one decoder, like the block layer should have been in 0.8.3.1. */
    uint32_t hi = 0;
    uint32_t lo = pci_bar_mem64(p, 0, &hi);

    if(hi){
        uint32_t v;
        if(!highfb_map_io(hi, lo, 0x2000u, &v)){
            klog(KLOG_WARN, "NVMe %2x:%2x.%u BAR0 at %x%4x is above 4 GB and "
                            "could not be mapped",
                 p->bus, p->dev, p->fn, hi, lo);
            return 0;
        }
        d->base = v;
        d->bar_high = 1;
    } else {
        if(!lo) return 0;
        d->base = lo;
    }

    pci_enable(p, 1);                       /* memory space + bus master */

    uint64_t cap = rd64(d->base, REG_CAP);
    d->dstrd = (uint32_t)((cap >> 32) & 0x0Fu);

    /* A controller that reports a minimum page size larger than 4 KB cannot
       be driven by these queues; say so rather than programming it wrong. */
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0x0Fu);
    if(mpsmin != 0){
        klog(KLOG_WARN, "NVMe: controller wants a %u KB page, not supported",
             4u << mpsmin);
        return 0;
    }

    wr32(d->base, REG_CC, 0);                       /* EN = 0 */
    if(!ctrl_wait_ready(d, 0)){
        klog(KLOG_WARN, "NVMe: controller never went not-ready");
        return 0;
    }

    memset(admin_sq[slot], 0, sizeof admin_sq[slot]);
    memset(admin_cq[slot], 0, sizeof admin_cq[slot]);
    wr32(d->base, REG_AQA, ((QD-1u) << 16) | (QD-1u));
    wr64(d->base, REG_ASQ, (uint64_t)(uint32_t)admin_sq[slot]);
    wr64(d->base, REG_ACQ, (uint64_t)(uint32_t)admin_cq[slot]);
    d->acq_phase = 1; d->icq_phase = 1;

    /* IOSQES=6 (64 B), IOCQES=4 (16 B), MPS=0 (4 KB), CSS=0 (NVM), EN=1 */
    wr32(d->base, REG_CC, (6u<<16) | (4u<<20) | (0u<<7) | (0u<<4) | CC_EN);
    if(!ctrl_wait_ready(d, 1)){
        klog(KLOG_ERR, "NVMe: controller never became ready");
        return 0;
    }

    /* Identify Controller, for the model string. */
    nvme_sqe_t c; memset(&c, 0, sizeof c);
    c.cdw0  = 0x06u;                      /* Identify */
    c.prp1  = (uint64_t)(uint32_t)dma_buf[slot];
    c.cdw10 = 1u;                         /* CNS 1 = controller */
    if(submit_wait(d, 0, &c, 0) != 0){
        klog(KLOG_WARN, "NVMe: Identify Controller refused");
        return 0;
    }
    for(int i = 0; i < 40; i++) d->model[i] = (char)dma_buf[slot][24+i];
    d->model[40] = 0;
    for(int i = 39; i >= 0 && (d->model[i]==' '||!d->model[i]); i--) d->model[i]=0;

    /* Namespace 1.  A drive with none is a drive with nothing to read. */
    memset(&c, 0, sizeof c);
    c.cdw0  = 0x06u;
    c.nsid  = 1u;
    c.prp1  = (uint64_t)(uint32_t)dma_buf[slot];
    c.cdw10 = 0u;                         /* CNS 0 = namespace */
    if(submit_wait(d, 0, &c, 0) != 0) return 0;

    /* The whole namespace page, parsed in one place that a test can reach
       without the admin queue this command travelled on. */
    if(!nvme_ns_parse(dma_buf[slot], &d->nsze, &d->lba_bytes, &d->ro))
        return 0;
    d->nsid = 1u;

    /* One I/O queue pair.  The completion queue must exist first: the
       submission queue's create command names it. */
    memset(io_sq[slot], 0, sizeof io_sq[slot]);
    memset(io_cq[slot], 0, sizeof io_cq[slot]);
    memset(&c, 0, sizeof c);
    c.cdw0  = 0x05u;                      /* Create I/O Completion Queue */
    c.prp1  = (uint64_t)(uint32_t)io_cq[slot];
    c.cdw10 = ((QD-1u) << 16) | 1u;       /* size-1, qid 1 */
    c.cdw11 = 1u;                         /* physically contiguous, no IRQ  */
    if(submit_wait(d, 0, &c, 0) != 0){
        klog(KLOG_WARN, "NVMe: could not create an I/O completion queue");
        return 0;
    }
    memset(&c, 0, sizeof c);
    c.cdw0  = 0x01u;                      /* Create I/O Submission Queue */
    c.prp1  = (uint64_t)(uint32_t)io_sq[slot];
    c.cdw10 = ((QD-1u) << 16) | 1u;
    c.cdw11 = (1u << 16) | 1u;            /* CQ 1, physically contiguous */
    if(submit_wait(d, 0, &c, 0) != 0){
        klog(KLOG_WARN, "NVMe: could not create an I/O submission queue");
        return 0;
    }

    d->ok = 1;
    return 1;
}

int nvme_init(void)
{
    g_count = 0;
    for(int nth = 0; nth < NVME_MAX_DEVS; nth++){
        /* class 01 subclass 08 progif 02 is NVM Express over PCIe. */
        const pci_dev_t *p = pci_find_class(PCI_CLASS_STORAGE, 0x08, 0x02, nth);
        if(!p) break;
        if(nvme_bring_up(p, g_count)){
            nvme_dev_t *d = &g_dev[g_count];
            klog(KLOG_OK, "NVMe %2x:%2x.%u %s, %u MB, %u B blocks%s",
                 p->bus, p->dev, p->fn, d->model,
                 (uint32_t)((d->nsze * d->lba_bytes) >> 20),
                 d->lba_bytes,
                 d->bar_high ? " (BAR paged in from above 4 GB)" : "");
            g_count++;
        }
    }
    if(!g_count){
        const pci_dev_t *p = pci_find_class(PCI_CLASS_STORAGE, 0x08, 0x02, 0);
        if(p) klog(KLOG_WARN, "NVMe: controller present but not usable");
    }
    return g_count;
}

int      nvme_count(void){ return g_count; }
uint64_t nvme_sectors(int i){
    if(i<0||i>=g_count) return 0;
    /* Reported in 512-byte sectors, which is what the rest of the kernel
       counts in, regardless of the drive's own block size. */
    return g_dev[i].nsze * (g_dev[i].lba_bytes / 512u);
}
uint32_t nvme_sector_size(int i){ return (i<0||i>=g_count)?0:g_dev[i].lba_bytes; }
const char *nvme_model(int i){ return (i<0||i>=g_count)?"":g_dev[i].model; }
int nvme_bar_was_high(int i){ return (i<0||i>=g_count)?0:g_dev[i].bar_high; }

/* Read `count` 512-byte sectors.  Split into one drive-block at a time
   through the 4 KB bounce buffer: simple, and correct for a drive whose
   blocks are 4 KB as well as one whose blocks are 512 B. */
int nvme_read(int idx, uint64_t lba, uint32_t count, void *buf)
{
    if(idx < 0 || idx >= g_count || !buf || !count) return -1;
    nvme_dev_t *d = &g_dev[idx];
    if(!d->ok) return -1;

    /* A 64-bit divide would pull in libgcc's __udivmoddi4, which a
       freestanding kernel does not link.  It is not needed: the drive's
       block size is a power of two, so sectors-per-block is too, and the
       divide and modulo are a shift and a mask.  The shift is done one bit
       at a time (at most three iterations, 512 B to 4 KB) because a
       variable-count 64-bit shift can also become a libgcc call. */
    uint32_t per = d->lba_bytes / 512u;         /* 512B sectors per block */
    if(!per) return -1;
    uint32_t per_shift = 0;
    while((1u << per_shift) < per) per_shift++;
    uint8_t *out = (uint8_t*)buf;

    for(uint32_t done = 0; done < count; ){
        uint64_t abs   = lba + done;
        uint64_t block = abs;
        for(uint32_t k = 0; k < per_shift; k++) block >>= 1;
        uint32_t skip  = (uint32_t)abs & (per - 1u);
        if(block >= d->nsze) return -1;

        nvme_sqe_t c; memset(&c, 0, sizeof c);
        c.cdw0  = 0x02u;                        /* Read */
        c.nsid  = d->nsid;
        c.prp1  = (uint64_t)(uint32_t)dma_buf[idx];
        c.cdw10 = (uint32_t)block;
        c.cdw11 = (uint32_t)(block >> 32);
        c.cdw12 = 0;                            /* NLB is zero-based: 1 block */
        int st = submit_wait(d, 1, &c, 0);
        if(st != 0) return -1;

        uint32_t have = per - skip;
        if(have > count - done) have = count - done;
        memcpy(out + done*512u, dma_buf[idx] + skip*512u, have*512u);
        done += have;
    }
    return 0;
}

/* Write `count` 512-byte sectors.
   ------------------------------------------------------------------
   The same block arithmetic as nvme_read above, and one thing more that
   is the entire hazard of this function:

   A namespace reports a block size. On most consumer drives it is 512
   and this is simple. On plenty of others -- and on every drive formatted
   with 4Kn, which is increasingly the default -- it is 4096, and then the
   smallest thing the controller can write is 4096 bytes. A caller asking
   to write one 512-byte sector is asking to change an eighth of a block.

   Writing that block without reading it first replaces the other seven
   sectors with whatever happens to be in the bounce buffer. Every status
   code says success. On the disk being installed to, those seven sectors
   belong to somebody's filesystem.

   So: any block this function does not completely fill is READ first,
   patched, and written back. A block it does fill is written straight
   out, because reading it would be a round trip for bytes about to be
   replaced -- and that is the only case where skipping the read is
   correct. */
/* Can this namespace be written at all?  Separate from nvme_write()
   failing, because the installer has to know before it offers a disk as a
   target rather than after it has started. */
int nvme_writable(int idx)
{
    if(idx < 0 || idx >= g_count) return 0;
    return g_dev[idx].ok && !g_dev[idx].ro;
}

int nvme_write(int idx, uint64_t lba, uint32_t count, const void *buf)
{
    if(idx < 0 || idx >= g_count || !buf || !count) return -1;
    nvme_dev_t *d = &g_dev[idx];
    if(!d->ok) return -1;
    if(d->ro)  return -1;      /* the namespace said so at identify time */

    /* No 64-bit divide: it would pull in libgcc's __udivmoddi4, which a
       freestanding kernel does not link. Block sizes are powers of two,
       so the divide is a shift, done one bit at a time because a
       variable-count 64-bit shift can also become a libgcc call. */
    uint32_t per = d->lba_bytes / 512u;
    if(!per) return -1;
    uint32_t per_shift = 0;
    while((1u << per_shift) < per) per_shift++;
    const uint8_t *in = (const uint8_t *)buf;

    for(uint32_t done = 0; done < count; ){
        uint64_t abs   = lba + done;
        uint64_t block = abs;
        for(uint32_t k = 0; k < per_shift; k++) block >>= 1;
        uint32_t skip  = (uint32_t)abs & (per - 1u);
        if(block >= d->nsze) return -1;

        uint32_t have = per - skip;
        if(have > count - done) have = count - done;

        /* Partial block: read it before changing part of it. */
        if(skip != 0 || have != per){
            nvme_sqe_t r; memset(&r, 0, sizeof r);
            r.cdw0  = 0x02u;                    /* Read                  */
            r.nsid  = d->nsid;
            r.prp1  = (uint64_t)(uint32_t)dma_buf[idx];
            r.cdw10 = (uint32_t)block;
            r.cdw11 = (uint32_t)(block >> 32);
            r.cdw12 = 0;                        /* NLB zero-based: 1 blk */
            if(submit_wait(d, 1, &r, 0) != 0) return -1;
        }

        memcpy(dma_buf[idx] + skip*512u, in + done*512u, have*512u);

        nvme_sqe_t c; memset(&c, 0, sizeof c);
        c.cdw0  = 0x01u;                        /* Write                 */
        c.nsid  = d->nsid;
        c.prp1  = (uint64_t)(uint32_t)dma_buf[idx];
        c.cdw10 = (uint32_t)block;
        c.cdw11 = (uint32_t)(block >> 32);
        c.cdw12 = 0;                            /* NLB zero-based: 1 blk */
        if(submit_wait(d, 1, &c, 0) != 0) return -1;

        done += have;
    }
    return 0;
}
