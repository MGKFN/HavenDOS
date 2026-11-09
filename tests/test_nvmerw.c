/* ================================================================
   tests/test_nvmerw.c -- the NVMe write path, against a controller
   ================================================================
   tests/test_nvme.c checks the command structures and the doorbell
   arithmetic, which is what can be checked without a controller. It
   cannot see the one thing that makes nvme_write() dangerous.

   A namespace reports a block size. When it is 4096 -- which every 4Kn
   drive reports, and plenty of consumer drives too -- the smallest
   thing the controller can write is 4096 bytes. A caller writing one
   512-byte sector is changing an eighth of a block, and a driver that
   writes the block without reading it first replaces the other seven
   sectors with whatever is left in the bounce buffer. Every status
   code reports success. On the disk HavenDOS is installing to, those
   seven sectors are somebody's filesystem.

   That cannot be tested by inspecting a struct. It needs a controller
   that actually executes the commands and a backing store to look at
   afterwards -- so there is one here, built in process memory, reached
   through include/mmio.h's seam. The simulated controller walks the
   submission queue the driver built, performs the reads and writes
   against a byte array, and posts completions with the phase tag the
   driver is polling for. The driver under test is the real one.

   Compiled -no-pie so that the address of a static array still fits in
   the uint32_t the driver keeps register and DMA addresses in; the same
   thing tests/test_uhci.c does, for the same reason.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ── the simulated register window and backing store ────────────────── */
#define REGWIN       8192u
#define SIM_BLOCKS   4096u          /* blocks in the fake namespace      */
#define SIM_BLKMAX   4096u          /* largest block size simulated      */

static uint8_t  g_regs[REGWIN] __attribute__((aligned(4096)));
static uint8_t  g_store[SIM_BLOCKS * SIM_BLKMAX];
static uint32_t g_blk_bytes = 512;  /* the namespace's block size        */

/* what the controller did, so the suite can assert on it */
static int      g_n_read, g_n_write;
/* Every SQE the controller takes off the queue, whether it then carries it
   out or refuses it. g_n_read/g_n_write count only the ones performed, so
   they cannot tell "the driver refused" from "the driver asked and the
   controller refused" -- and that distinction is exactly what a bounds
   check inside the driver is. */
static int      g_n_cmd;
static int      g_huge;      /* namespace too big to back with an array */
static uint64_t g_last_write_blk, g_last_read_blk;
static int      g_fail_next_write;  /* make one write command fail        */
static int      g_fail_next_read;   /* ...or one read                      */

static void sim_doorbell(uint32_t off, uint32_t val);

/* ── the MMIO seam ──────────────────────────────────────────────────── */
#define MMIO_H
static inline uint32_t mmio_read32(uint32_t base, uint32_t off){
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}
static inline void mmio_write32(uint32_t base, uint32_t off, uint32_t v){
    *(volatile uint32_t *)(uintptr_t)(base + off) = v;
    /* Doorbells start at 0x1000. Writing one is what makes a real
       controller look at the queue, so it is what makes this one. */
    if(off >= 0x1000u) sim_doorbell(off, v);
}
static inline uint16_t mmio_read16(uint32_t b, uint32_t o){
    return *(volatile uint16_t *)(uintptr_t)(b + o); }
static inline void mmio_write16(uint32_t b, uint32_t o, uint16_t v){
    *(volatile uint16_t *)(uintptr_t)(b + o) = v; }
static inline uint8_t mmio_read8(uint32_t b, uint32_t o){
    return *(volatile uint8_t *)(uintptr_t)(b + o); }
static inline void mmio_write8(uint32_t b, uint32_t o, uint8_t v){
    *(volatile uint8_t *)(uintptr_t)(b + o) = v; }
static inline uint64_t mmio_read64(uint32_t b, uint32_t o){
    return (uint64_t)mmio_read32(b,o) | ((uint64_t)mmio_read32(b,o+4) << 32); }
static inline void mmio_write64(uint32_t b, uint32_t o, uint64_t v){
    mmio_write32(b,o,(uint32_t)v); mmio_write32(b,o+4,(uint32_t)(v>>32)); }

/* ── everything else drivers/nvme.c reaches for ─────────────────────── */
#define IO_H
static inline void outb(uint16_t p, uint8_t v){ (void)p;(void)v; }
static inline void outw(uint16_t p, uint16_t v){ (void)p;(void)v; }
static inline void outl(uint16_t p, uint32_t v){ (void)p;(void)v; }
static inline uint8_t  inb(uint16_t p){ (void)p; return 0xFF; }
static inline uint16_t inw(uint16_t p){ (void)p; return 0xFFFF; }
static inline uint32_t inl(uint16_t p){ (void)p; return 0xFFFFFFFFu; }
static inline void io_wait(void){}

void sleep_ms(uint32_t ms){ (void)ms; }
void klog(int level, const char *fmt, ...){ (void)level; (void)fmt; }
void vga_puts(const char *s){ (void)s; }
void vga_printf(const char *f, ...){ (void)f; }

#include "../include/types.h"
#include "../include/pci.h"
#include "../include/highfb.h"
/* pci_find_class is only used by nvme_init(), which this suite does not
   call: the controller is brought up by hand below, because what is under
   test is the data path and not the enumeration. */
const pci_dev_t *pci_find_class(uint8_t c, uint8_t s, uint8_t i, int n){
    (void)c;(void)s;(void)i;(void)n; return 0; }
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t o){
    (void)b;(void)d;(void)f;(void)o; return 0; }
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint32_t v){
    (void)b;(void)d;(void)f;(void)o;(void)v; }
int highfb_map(uint32_t phys_hi, uint32_t phys_lo, uint32_t bytes, uint32_t *out){
    (void)phys_hi; (void)phys_lo; (void)bytes; (void)out; return 0; }
/* v0.8.5.1: NVMe maps its registers UNCACHED now, not write-through.
   pci_bar_mem64() is not stubbed -- drivers/pci_bar.c is linked. */
int highfb_map_io(uint32_t phys_hi, uint32_t phys_lo, uint32_t bytes, uint32_t *out){
    return highfb_map(phys_hi, phys_lo, bytes, out); }
void pci_enable(const pci_dev_t *d, int bus_master){ (void)d; (void)bus_master; }

#include "../drivers/nvme.c"

/* ── the controller ─────────────────────────────────────────────────── */
/* Executes one command from the I/O submission queue and posts its
   completion, the way a real controller would: read the SQE the driver
   wrote, do the work against g_store, write a CQE whose phase bit is what
   the driver is waiting for. */
/* The controller's own queue position. At file scope and reset by attach(),
   not function statics: the driver's queue state is reset for each case and
   the controller's has to be reset with it, or the phase tags disagree and
   the driver reads a completion left over from the previous case. That is
   what the first version of this did, and three sections failed for a
   reason that was entirely in the test. */
static uint16_t sim_sq_head, sim_cq_tail, sim_cq_phase;

static void sim_doorbell(uint32_t off, uint32_t val)
{
    uint16_t *const sq_head  = &sim_sq_head;
    uint16_t *const cq_tail  = &sim_cq_tail;
    uint16_t *const cq_phase = &sim_cq_phase;

    /* I/O SQ tail doorbell is at 0x1008 with DSTRD 0; CQ head doorbells
       and the admin pair are acknowledged and ignored. */
    if(off != 0x1008u) return;

    while(*sq_head != (uint16_t)val){
        nvme_sqe_t c;
        memcpy(&c, io_sq[0] + (uint32_t)*sq_head * SQE_BYTES, sizeof c);
        *sq_head = (uint16_t)((*sq_head + 1u) % QD);

        uint8_t  op    = (uint8_t)(c.cdw0 & 0xFFu);
        uint16_t cid   = (uint16_t)(c.cdw0 >> 16);
        uint64_t blk   = (uint64_t)c.cdw10 | ((uint64_t)c.cdw11 << 32);
        uint32_t nlb   = (c.cdw12 & 0xFFFFu) + 1u;      /* zero-based */
        uint8_t *dma   = (uint8_t *)(uintptr_t)(uint32_t)c.prp1;
        uint16_t status = 0;
        g_n_cmd++;

        if(g_huge){
            /* A namespace of billions of blocks cannot be backed by an
               array. The block number is recorded and nothing is copied,
               which is all that is needed to check that a 64-bit block
               number survives the trip through cdw10 and cdw11. */
            if(op == 0x01u){ g_last_write_blk = blk; g_n_write++; }
            else if(op == 0x02u){ g_last_read_blk = blk; g_n_read++; }
            else status = 0x0001;
        } else if(blk + nlb > SIM_BLOCKS){
            status = 0x0080;                     /* LBA out of range */
        } else if(op == 0x02u){                  /* Read  */
            if(g_fail_next_read){
                g_fail_next_read = 0;
                status = 0x0006;                 /* internal device error */
            } else {
                memcpy(dma, g_store + blk * g_blk_bytes, nlb * g_blk_bytes);
                g_n_read++;
            }
        } else if(op == 0x01u){                  /* Write */
            if(g_fail_next_write){
                g_fail_next_write = 0;
                status = 0x0006;                 /* internal device error */
            } else {
                memcpy(g_store + blk * g_blk_bytes, dma, nlb * g_blk_bytes);
                g_last_write_blk = blk;
                g_n_write++;
            }
        } else {
            status = 0x0001;                     /* invalid opcode */
        }

        nvme_cqe_t e;
        memset(&e, 0, sizeof e);
        e.cid     = cid;
        e.sq_head = *sq_head;
        e.sq_id   = 1;
        e.status  = (uint16_t)((status << 1) | (*cq_phase & 1u));
        memcpy(io_cq[0] + (uint32_t)*cq_tail * CQE_BYTES, &e, sizeof e);
        *cq_tail = (uint16_t)((*cq_tail + 1u) % QD);
        if(*cq_tail == 0) *cq_phase ^= 1u;
    }
}

/* Fill patterns must not repeat within the range under test. The first
   version of this used (uint8_t)(i ^ 0xA5) and friends -- every one of
   which repeats every 256 bytes, so a driver copying the WRONG 512-byte
   slice of the caller's buffer produced bytes identical to the right ones
   and the suite saw nothing. Every fill below mixes in i >> 8. */
static uint8_t pat(uint32_t i, uint32_t salt){
    return (uint8_t)(i * 7u + (i >> 8) * 131u + (i >> 16) * 29u + salt);
}

/* ── scoreboard ─────────────────────────────────────────────────────── */
static int total=0, fails=0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *w, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-56s got %ld\n", w, got); }
}

/* Bring up g_dev[0] by hand against the simulated window. */
static void attach(uint32_t block_bytes)
{
    memset(g_regs, 0, sizeof g_regs);
    memset(&g_dev[0], 0, sizeof g_dev[0]);
    memset(io_sq[0], 0, sizeof io_sq[0]);
    memset(io_cq[0], 0, sizeof io_cq[0]);
    memset(dma_buf[0], 0, sizeof dma_buf[0]);
    g_blk_bytes = block_bytes;
    g_n_read = g_n_write = 0;
    g_n_cmd = 0;
    g_huge = 0;
    g_fail_next_write = 0;
    g_fail_next_read = 0;
    sim_sq_head = 0; sim_cq_tail = 0; sim_cq_phase = 1;

    g_dev[0].base      = (uint32_t)(uintptr_t)g_regs;
    g_dev[0].dstrd     = 0;
    g_dev[0].nsid      = 1;
    g_dev[0].nsze      = SIM_BLOCKS;
    g_dev[0].lba_bytes = block_bytes;
    g_dev[0].icq_phase = 1;
    g_dev[0].ok        = 1;
    g_count = 1;
}

/* fill the backing store with a pattern that makes damage obvious */
static void fill_store(void)
{
    for(uint32_t i = 0; i < sizeof g_store; i++)
        g_store[i] = (uint8_t)((i * 31u + (i >> 11) * 7u) & 0xFF);
}
static uint8_t expect_byte(uint32_t off){
    return (uint8_t)((off * 31u + (off >> 11) * 7u) & 0xFF);
}

int main(void)
{
    static uint8_t out[8192], in[8192];
    uint32_t i;

    printf("NVMe read and write, against a controller that executes them\n");
    printf("============================================================\n");

    if((uintptr_t)g_regs > 0xFFFFFFFFull){
        printf("\n  the test's own buffers are above 4 GB, so the driver's\n"
               "  uint32_t addresses cannot reach them. Build -no-pie.\n");
        printf("\n0/0 passed\n");
        return 1;
    }

    head("a 512-byte-block namespace: the simple case");
    {
        attach(512);
        fill_store();
        for(i = 0; i < 4096; i++) out[i] = pat(i, 0x11);

        ck("write 8 sectors at LBA 100",
           nvme_write(0, 100, 8, out) == 0, 0);
        ck("the controller performed 8 writes", g_n_write == 8, g_n_write);
        ck("and no reads, because every block was filled completely",
           g_n_read == 0, g_n_read);

        memset(in, 0, sizeof in);
        ck("read them back", nvme_read(0, 100, 8, in) == 0, 0);
        ck("byte for byte", memcmp(in, out, 8*512) == 0, 0);

        ck("the sector before is untouched",
           g_store[99*512] == expect_byte(99*512), g_store[99*512]);
        ck("and the sector after",
           g_store[108*512] == expect_byte(108*512), g_store[108*512]);
    }

    head("THE HAZARD: a 4096-byte-block namespace, one sector written");
    {
        /* One 512-byte sector inside a 4096-byte block. Seven sectors of
           somebody else's data share that block. A driver that writes the
           block without reading it first destroys all seven and reports
           success. */
        attach(4096);
        fill_store();
        for(i = 0; i < 512; i++) out[i] = (uint8_t)(i ^ 0x5A);

        /* LBA 801 is sector 1 of block 100 */
        ck("write one sector at LBA 801", nvme_write(0, 801, 1, out) == 0, 0);
        ck("the controller wrote exactly one block", g_n_write == 1, g_n_write);
        ck("and READ that block first", g_n_read == 1, g_n_read);
        ck("it was block 100", g_last_write_blk == 100, (long)g_last_write_blk);

        ck("the sector written is the new data",
           memcmp(g_store + 100*4096 + 512, out, 512) == 0, 0);

        {
            int bad = 0;
            for(i = 0; i < 4096; i++){
                if(i >= 512 && i < 1024) continue;      /* the one written */
                if(g_store[100*4096 + i] != expect_byte(100*4096 + i)) bad++;
            }
            ck("THE OTHER SEVEN SECTORS OF THAT BLOCK ARE UNTOUCHED",
               bad == 0, bad);
        }
        {
            int bad = 0;
            for(i = 0; i < 4096; i++)
                if(g_store[99*4096 + i] != expect_byte(99*4096 + i)) bad++;
            ck("and the whole block before it", bad == 0, bad);
        }
    }

    head("4 KB blocks, a write that starts mid-block and runs past the end");
    {
        attach(4096);
        fill_store();
        for(i = 0; i < 4096; i++) out[i] = (uint8_t)(0xC3 ^ i);

        /* LBA 1603 is sector 3 of block 200; 8 sectors runs to sector 2 of
           block 201. Both end blocks are partial, the middle none. */
        ck("write 8 sectors at LBA 1603", nvme_write(0, 1603, 8, out) == 0, 0);
        ck("two blocks were written", g_n_write == 2, g_n_write);
        ck("and both were read first, being partial", g_n_read == 2, g_n_read);

        ck("the written range is the new data",
           memcmp(g_store + 200*4096 + 3*512, out, 8*512) == 0, 0);
        {
            int bad = 0;
            for(i = 0; i < 3u*512u; i++)
                if(g_store[200*4096 + i] != expect_byte(200*4096 + i)) bad++;
            ck("the three sectors before it survive", bad == 0, bad);
        }
        {
            int bad = 0;
            for(i = 3u*512u; i < 4096u; i++)
                if(g_store[201*4096 + i] != expect_byte(201*4096 + i)) bad++;
            ck("and the five after it", bad == 0, bad);
        }

        memset(in, 0, sizeof in);
        ck("reading it back agrees", nvme_read(0, 1603, 8, in) == 0, 0);
        ck("byte for byte", memcmp(in, out, 8*512) == 0, 0);
    }

    head("a write that exactly fills whole blocks skips the read");
    {
        attach(4096);
        fill_store();
        for(i = 0; i < 8192; i++) out[i] = (uint8_t)(i * 3u);
        /* LBA 2400 is block 300 sector 0; 16 sectors is exactly two blocks */
        ck("write 16 aligned sectors", nvme_write(0, 2400, 16, out) == 0, 0);
        ck("two blocks written", g_n_write == 2, g_n_write);
        ck("and NO read, because nothing needed preserving",
           g_n_read == 0, g_n_read);
        ck("the data is there", memcmp(g_store + 300*4096, out, 8192) == 0, 0);
    }

    head("the refusals");
    {
        attach(512);
        ck("a bad device index",    nvme_write(-1, 0, 1, out) != 0, 0);
        ck("an index past the end", nvme_write(9, 0, 1, out) != 0, 0);
        ck("a null buffer",         nvme_write(0, 0, 1, 0)   != 0, 0);
        ck("a zero count",          nvme_write(0, 0, 0, out) != 0, 0);
        {
            /* Past the end of the namespace. A real controller rejects it,
               so checking only the return value proves nothing about the
               driver -- the error could be coming from the controller. What
               is asserted is that NO COMMAND WAS ISSUED: the driver refused
               before touching the queue. */
            int r;
            g_n_read = g_n_write = 0;
            r = nvme_write(0, (uint64_t)SIM_BLOCKS * 8ull, 1, out);
            ck("past the end of the namespace is refused", r != 0, r);
            ck("and no command reached the controller",
               g_n_read == 0 && g_n_write == 0, g_n_read + g_n_write);
        }
        {
            nvme_dev_t save = g_dev[0];
            g_dev[0].ok = 0;
            ck("a controller that did not come up",
               nvme_write(0, 0, 1, out) != 0, 0);
            g_dev[0] = save;
        }
    }

    head("a command the controller fails is not reported as success");
    {
        attach(512);
        fill_store();
        for(i = 0; i < 512; i++) out[i] = 0x11;
        g_fail_next_write = 1;
        ck("nvme_write returns an error", nvme_write(0, 50, 1, out) != 0, 0);
        {
            int bad = 0;
            for(i = 0; i < 512; i++)
                if(g_store[50*512 + i] != expect_byte(50*512 + i)) bad++;
            ck("and the sector is as it was", bad == 0, bad);
        }
    }

    head("a failed read-before-write does not become a write anyway");
    {
        /* The subtlest failure in this function. A partial block is read,
           patched, and written back. If the READ fails and nobody looks,
           the patch lands in whatever the bounce buffer held from the last
           command and that is what gets written -- so one sector is
           correct and the other seven are the previous block's contents.
           The write succeeds. Every status code says so. */
        attach(4096);
        fill_store();
        /* put something recognisable in the bounce buffer first */
        ck("prime the bounce buffer", nvme_read(0, 0, 8, in) == 0, 0);
        for(i = 0; i < 512; i++) out[i] = 0x7E;

        g_fail_next_read = 1;
        ck("nvme_write reports the failure", nvme_write(0, 403, 1, out) != 0, 0);
        {
            int bad = 0;
            for(i = 0; i < 4096; i++)
                if(g_store[50*4096 + i] != expect_byte(50*4096 + i)) bad++;
            ck("and block 50 is exactly as it was", bad == 0, bad);
        }
        ck("no write was issued at all", g_n_write == 0, g_n_write);
    }

    head("reads still work, and agree with what writes put there");
    {
        attach(4096);
        fill_store();
        memset(in, 0, sizeof in);
        ck("read 8 sectors across a block boundary",
           nvme_read(0, 1606, 8, in) == 0, 0);
        {
            int bad = 0;
            for(i = 0; i < 8u*512u; i++)
                if(in[i] != expect_byte(200*4096 + 6*512 + i)) bad++;
            ck("every byte is what the store holds", bad == 0, bad);
        }
    }

    head("a namespace past 2 TB: the block number's top half must survive");
    {
        /* cdw10 carries the low 32 bits of the block number and cdw11 the
           high 32. Dropping cdw11 is invisible on any namespace small
           enough to back with an array -- which is every namespace this
           suite had until now -- and on a real 4 TB drive it means a write
           meant for the far end of the disk landing near the start of it.

           So the controller runs in g_huge mode here: it backs nothing,
           records the full 64-bit block number it was given, and that is
           the whole assertion. */
        /* Above 2^32 and comfortably inside the namespace. The first
           version of this picked 0x1234_00005678 against an nsze of
           0xFF_FFFFFFFF -- a block number LARGER than the namespace, so
           the driver refused it correctly and the suite called that a
           failure. The bounds check was doing its job; the test was
           wrong. */
        uint64_t big_blk = 0x3ull * 0x100000000ull + 0x5678ull;
        attach(512);
        g_huge = 1;
        g_dev[0].nsze = 0x100000000ull * 16ull;   /* 64 Gi blocks */
        memset(in, 0xC7, sizeof in);

        g_last_write_blk = 0;
        ck("a write far past the 32-bit boundary is accepted",
           nvme_write(0, big_blk, 1, in) == 0, 0);
        ck("and the controller saw the whole 64-bit block number",
           g_last_write_blk == big_blk,
           (long)(g_last_write_blk & 0xFFFFFFFFu));
        ck("not just its low half",
           g_last_write_blk != (big_blk & 0xFFFFFFFFull), 0);

        g_last_read_blk = 0;
        ck("a read at the same place agrees",
           nvme_read(0, big_blk, 1, out) == 0, 0);
        ck("with the same 64-bit block number",
           g_last_read_blk == big_blk, 0);

        /* and on a 4 KB-block namespace, where the sector-to-block shift
           happens before the split, so a bug can hide in either place */
        attach(4096);
        g_huge = 1;
        g_dev[0].nsze = 0x100000000ull * 16ull;
        g_last_write_blk = 0;
        ck("the same holds with 4 KB blocks",
           nvme_write(0, big_blk * 8ull, 8, in) == 0, 0);
        ck("the block number is the sector number shifted, in 64 bits",
           g_last_write_blk == big_blk, 0);
    }

    head("a write-protected namespace is refused, and says so first");
    {
        /* NSATTR bit 0. Real on encrypted, provisioned and OEM-locked
           drives. Without this the installer offers the disk, starts, and
           fails one sector at a time part-way through a partition table --
           which is the worst moment available to discover it. */
        attach(512);
        fill_store();
        g_dev[0].ro = 1;
        g_n_cmd = 0;
        memset(in, 0x33, sizeof in);

        ck("nvme_writable says no", nvme_writable(0) == 0, nvme_writable(0));
        ck("the write is refused", nvme_write(0, 10, 1, in) == -1, 0);
        ck("and no command was issued", g_n_cmd == 0, g_n_cmd);
        {
            int bad = 0;
            for(i = 0; i < 512u; i++)
                if(g_store[10*512 + i] != expect_byte(10*512 + i)) bad++;
            ck("the block is untouched", bad == 0, bad);
        }
        ck("reading it is still allowed", nvme_read(0, 10, 1, out) == 0, 0);

        g_dev[0].ro = 0;
        ck("and with the flag clear it writes again",
           nvme_writable(0) == 1 && nvme_write(0, 10, 1, in) == 0, 0);

        ck("an out-of-range index is not writable", nvme_writable(99) == 0, 0);

        /* The parse itself, against a crafted Identify Namespace page. The
           identify command travels on the admin queue, which this harness
           does not simulate, so the parse is a function of its own and is
           asked directly rather than left untested. */
        {
            static uint8_t idns[4096];
            memset(idns, 0, sizeof idns);
            ck("a namespace with NSATTR clear is writable",
               nvme_ns_write_protected(idns) == 0, 0);
            idns[99] = 0x01;
            ck("bit 0 of byte 99 means write protected",
               nvme_ns_write_protected(idns) == 1, 0);
            idns[99] = 0x02;
            ck("bit 1 does not -- it is a shared-namespace hint",
               nvme_ns_write_protected(idns) == 0, 0);
            idns[99] = 0xFF;
            ck("and bit 0 is still read when the others are set",
               nvme_ns_write_protected(idns) == 1, 0);
            idns[99] = 0x00; idns[98] = 0x01;
            ck("byte 98 is not the one", nvme_ns_write_protected(idns) == 0, 0);
            ck("a null page is not a protected namespace",
               nvme_ns_write_protected(0) == 0, 0);
        }
    }

    head("Identify Namespace, parsed: the three numbers everything uses");
    {
        /* NSZE, the block size behind FLBAS, and NSATTR. These were inline
           in the identify path, reachable only after a controller reset,
           two admin commands and a completion -- so none of them was
           tested, and a wrong block size silently reads the wrong eighth
           of every block. */
        static uint8_t idns[4096];
        uint64_t nsze; uint32_t lbab; int ro;

        memset(idns, 0, sizeof idns);
        ck("a namespace of zero blocks does not exist",
           nvme_ns_parse(idns, &nsze, &lbab, &ro) == 0, 0);

        /* NSZE = 0x0001_0000_0000 blocks, little-endian over 8 bytes */
        idns[4] = 0x01;
        idns[26] = 0;                      /* FLBAS: format 0            */
        idns[128 + 0*4 + 2] = 9;           /* LBADS 9 -> 512 bytes       */
        ck("it parses", nvme_ns_parse(idns, &nsze, &lbab, &ro) == 1, 0);
        ck("NSZE is read across all eight bytes",
           nsze == 0x100000000ull, (long)(nsze & 0xFFFFFFFFu));
        ck("512-byte blocks", lbab == 512, (long)lbab);
        ck("not write protected", ro == 0, ro);

        /* a drive whose chosen format is #3, with 4 KB blocks */
        idns[26] = 3;
        idns[128 + 3*4 + 2] = 12;          /* LBADS 12 -> 4096 bytes     */
        ck("FLBAS selects which format entry is read",
           nvme_ns_parse(idns, &nsze, &lbab, &ro) == 1 && lbab == 4096,
           (long)lbab);

        /* the index is masked to 4 bits: a drive setting the metadata bit
           in the high nibble must not select entry 19 */
        idns[26] = 0x13;                   /* 0x10 | 3                    */
        ck("the high nibble of FLBAS is not part of the index",
           nvme_ns_parse(idns, &nsze, &lbab, &ro) == 1 && lbab == 4096,
           (long)lbab);

        /* an out-of-range block size falls back to 512, because dma_buf is
           4096 bytes and a larger block would overrun it */
        idns[26] = 0;
        idns[128 + 0*4 + 2] = 16;          /* 65536 bytes -- refuse       */
        ck("a block size this kernel has no buffer for falls back to 512",
           nvme_ns_parse(idns, &nsze, &lbab, &ro) == 1 && lbab == 512,
           (long)lbab);
        idns[128 + 0*4 + 2] = 3;           /* 8 bytes -- nonsense         */
        ck("and so does an impossibly small one",
           nvme_ns_parse(idns, &nsze, &lbab, &ro) == 1 && lbab == 512,
           (long)lbab);

        idns[128 + 0*4 + 2] = 9;
        idns[99] = 0x01;
        ck("NSATTR comes through the same parse",
           nvme_ns_parse(idns, &nsze, &lbab, &ro) == 1 && ro == 1, ro);

        ck("null arguments are refused rather than dereferenced",
           nvme_ns_parse(0, &nsze, &lbab, &ro) == 0
           && nvme_ns_parse(idns, 0, &lbab, &ro) == 0
           && nvme_ns_parse(idns, &nsze, 0, &ro) == 0
           && nvme_ns_parse(idns, &nsze, &lbab, 0) == 0, 0);
        { int was = g_dev[0].ok; g_dev[0].ok = 0;
          ck("nor is a controller that never came up",
             nvme_writable(0) == 0, 0);
          g_dev[0].ok = was; }
    }

    head("a write past the end of the namespace never reaches the wire");
    {
        /* The controller would refuse it -- status 0x0080, LBA out of
           range -- so a driver without the check still "fails". The
           difference is that it fails having ISSUED a command against a
           block the drive does not have, after potentially reading one
           first. On a controller less forgiving than this one that is a
           reset, and on a drive whose namespace was just resized it is a
           write to whatever is now there.

           So the assertion is not that it fails. It is that it fails
           having sent nothing. */
        attach(512);
        fill_store();
        g_n_cmd = 0; g_n_write = 0; g_n_read = 0;
        memset(in, 0x5A, sizeof in);

        ck("a write one sector past the end is refused",
           nvme_write(0, SIM_BLOCKS, 1, in) == -1, 0);
        ck("and no command was issued", g_n_cmd == 0, g_n_cmd);

        ck("a write far past the end is refused",
           nvme_write(0, SIM_BLOCKS * 4u, 1, in) == -1, 0);
        ck("still nothing issued", g_n_cmd == 0, g_n_cmd);

        /* one that straddles the end: the first sectors are real */
        g_n_cmd = 0;
        ck("a write straddling the end is refused",
           nvme_write(0, SIM_BLOCKS - 2u, 8, in) == -1, 0);
        ck("the in-range part may be written, the rest is not",
           g_n_cmd <= 2, g_n_cmd);
        {
            int bad = 0;
            for(i = 0; i < 512u; i++)
                if(g_store[(SIM_BLOCKS - 1u) * 512u + i] != 0x5A
                   && g_store[(SIM_BLOCKS - 1u) * 512u + i]
                      != expect_byte((SIM_BLOCKS - 1u) * 512u + i)) bad++;
            ck("and nothing outside the namespace was touched", bad == 0, bad);
        }

        /* the same on 4 KB blocks, where one sector past the end is still
           inside the last block and the bounds test is on the BLOCK */
        attach(4096);
        fill_store();
        g_n_cmd = 0;
        ck("4 KB blocks: a write past the last block is refused",
           nvme_write(0, SIM_BLOCKS * 8u, 1, in) == -1, 0);
        ck("with nothing issued", g_n_cmd == 0, g_n_cmd);

        ck("a read past the end is refused too",
           nvme_read(0, SIM_BLOCKS * 8u, 1, out) == -1, 0);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
