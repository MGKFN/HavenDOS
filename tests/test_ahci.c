/* ================================================================
   tests/test_ahci.c -- AHCI against a controller that obeys it
   ================================================================
   This driver writes to the disk in every machine made since about
   2010, and until it ran there was no way to see it work: AHCI is
   entirely memory-mapped, and MMIO had no test seam until
   include/mmio.h.

   So there is a controller here. It is not a mock that records calls
   and returns success. It reads the command list the driver built at
   the address the driver put in PxCLB, follows ctba to the command
   table, decodes the Host-to-Device Register FIS, walks the
   scatter-gather list, and performs the read or the write against a
   backing store -- then clears the slot's bit in PxCI, which is the
   completion the driver is actually waiting on.

   That means the things that can only be got wrong in the gaps are
   visible: a command header whose W bit is clear on a write (the
   controller then reads the disk into the buffer we meant to write
   FROM, and reports success), an LBA whose top sixteen bits never
   reach the FIS, a PRDT byte count that is off by one in the
   direction that truncates, a port whose command list is replaced
   while the controller is still running it.

   The last one the controller refuses to forgive: writing PxCLB while
   PxCMD.CR is set is "indeterminate" by specification, and here it is
   recorded and failed, because a driver that gets away with it in
   QEMU will not get away with it on an Intel PCH.

   -no-pie so that the address of a static array still fits the
   uint32_t a register holds, which is what tests/test_uhci.c and
   tests/test_nvmerw.c do, for the same reason.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define REGWIN      0x1000u
#define SIM_PORTS   6
#define SIM_SECTORS 4096u

static uint8_t  g_regs[REGWIN] __attribute__((aligned(4096)));
static uint8_t  g_store[SIM_PORTS][SIM_SECTORS * 512u];

/* What each simulated port has on it. More than "is something there":
   the cases that separate one check from the next are a port whose device
   has gone but whose signature register still reads stale, and a port with
   a device that is not a disk at all. */
#define PRES_NONE      0
#define PRES_SATA      1
#define PRES_ATAPI     2
#define PRES_STALE_SIG 3   /* DET says nothing is there, PxSIG still ATA  */
#define PRES_OTHER     4   /* DET says yes, signature is a port multiplier */

/* and what its IDENTIFY says */
#define ID_NORMAL      0
#define ID_NO_LBA48    1   /* word 83 bit 10 clear, 100..103 are garbage   */
#define ID_ZERO_CAP    2   /* identifies, reports no sectors at all        */

static int      g_present[SIM_PORTS];
static int      g_idkind[SIM_PORTS];
static uint32_t g_pi;                     /* ports-implemented bitmap      */
static int      g_prerun;                 /* ports already running, as a
                                             firmware with an AHCI driver
                                             loaded would leave them       */

/* what the controller saw, for the assertions */
static int      g_n_read, g_n_write, g_n_ident;
static uint64_t g_last_lba;
static uint16_t g_last_count;
static uint32_t g_last_bytes;
static int      g_last_was_write;
static int      g_clb_while_running;      /* the unforgivable one          */
static int      g_fail_next;              /* make one command fail         */
static int      g_short_prdbc;
static int      g_n_cmd;                  /* every command issued, good or bad */
static int      g_bad_fis;                /* a FIS a real controller refuses   */
static const char *g_bad_why;

static void sim_ci(int port, uint32_t val);

/* The engines do not stop the instant they are told to.

   PxCMD.CR and PxCMD.FR follow ST and FRE, but with hardware behind them:
   the specification allows up to 500 ms, and a driver is required to poll
   until they clear before it touches PxCLB or PxFB. A simulator that
   clears them in the same write makes that wait look unnecessary -- the
   driver gets away with not polling, and then does not get away with it on
   an Intel PCH.

   So each engine gets a countdown, set when it is told to stop and
   decremented each time PxCMD is read. Three reads, which is enough that a
   driver which does not poll at all cannot stumble through. */
#define WIND_DOWN 3
static int g_cr_left[SIM_PORTS], g_fr_left[SIM_PORTS];

/* ── the seam ──────────────────────────────────────────────────────── */
#define MMIO_H
static inline uint32_t mmio_read32(uint32_t base, uint32_t off){
    if(off >= 0x100u && off < 0x100u + SIM_PORTS*0x80u
       && (off - 0x100u) % 0x80u == 0x18u){
        int p = (int)((off - 0x100u) / 0x80u);
        uint32_t *slot = (uint32_t *)(uintptr_t)(base + off);
        if(g_cr_left[p] > 0 && --g_cr_left[p] == 0) *slot &= ~0x8000u;
        if(g_fr_left[p] > 0 && --g_fr_left[p] == 0) *slot &= ~0x4000u;
        return *slot;
    }
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}
static inline void mmio_write32(uint32_t base, uint32_t off, uint32_t v){
    uint32_t *slot = (uint32_t *)(uintptr_t)(base + off);
    /* PxCLB at 0x100 + p*0x80 + 0x00, PxCI at +0x38, PxCMD at +0x18 */
    if(off >= 0x100u && off < 0x100u + SIM_PORTS*0x80u){
        uint32_t rel  = off - 0x100u;
        int      port = (int)(rel / 0x80u);
        uint32_t reg  = rel % 0x80u;
        if(reg == 0x00u){                 /* PxCLB */
            uint32_t cmd = *(volatile uint32_t *)(uintptr_t)
                           (base + 0x100u + (uint32_t)port*0x80u + 0x18u);
            if(cmd & 0x8000u) g_clb_while_running = 1;   /* CR still set */
        }
        if(reg == 0x18u){                 /* PxCMD: CR follows ST, FR FRE */
            uint32_t was = *slot;
            uint32_t n   = v & ~0xC000u;
            /* Starting is immediate. Stopping takes time: the bit stays
               set and a countdown runs, so a driver that does not poll
               writes PxCLB with CR still set -- which is recorded. */
            if(v & 0x0001u){ n |= 0x8000u; g_cr_left[port] = 0; }
            else if(was & 0x8000u){ n |= 0x8000u; g_cr_left[port] = WIND_DOWN; }
            if(v & 0x0010u){ n |= 0x4000u; g_fr_left[port] = 0; }
            else if(was & 0x4000u){ n |= 0x4000u; g_fr_left[port] = WIND_DOWN; }
            *slot = n;
            return;
        }
        if(reg == 0x38u){ *slot = v; sim_ci(port, v); return; }
        if(reg == 0x30u){ *slot = 0; return; }           /* PxSERR: W1C   */
        if(reg == 0x10u){ *slot = 0; return; }           /* PxIS:   W1C   */
    }
    *slot = v;
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

/* ── the kernel bits the driver reaches for ────────────────────────── */
void sleep_ms(uint32_t ms){ (void)ms; }

/* The boot log. Swallowed, but counted: a driver that reports a port it
   did not bring up puts a line in the boot log that tools/check_boot_log.py
   cannot see is false, because it is a real call with real arguments. The
   count is asserted below instead. */
static int g_klogs;
void klog(int level, const char *fmt, ...){ (void)level; (void)fmt; g_klogs++; }

#include "types.h"
#include "pci.h"

static pci_dev_t g_pci;
const pci_dev_t *pci_find_class(uint8_t c, uint8_t s, uint8_t p, int nth){
    if(nth != 0) return 0;
    if(c != 0x01 || s != 0x06 || p != 0x01) return 0;
    return &g_pci;
}
const pci_dev_t *pci_find_id(uint16_t v, uint16_t d){ (void)v;(void)d; return 0; }
/* These two stay STUBBED here, unlike in test_pci / test_xhci / test_nvmerw
   which link drivers/pci_bar.c. That is deliberate and not an oversight:
   AHCI's ABAR is BAR5 and the specification makes it 32 bits wide, so the
   64-bit decode this stub does not perform cannot arise. Do not "fix" it
   by linking pci_bar.c -- that would collide with these definitions.
   (noted v0.8.5.1) */
uint32_t pci_bar_mem(const pci_dev_t *d, int n){
    (void)d; return n == 5 ? (uint32_t)(uintptr_t)g_regs : 0; }
uint16_t pci_bar_io(const pci_dev_t *d, int n){ (void)d;(void)n; return 0; }
static int g_bus_master;
void pci_enable(const pci_dev_t *d, int bm){ (void)d; g_bus_master = bm; }

#include "../drivers/ahci.c"

/* ── scoreboard ────────────────────────────────────────────────────── */
static int total=0, fails=0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *w, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-56s got %ld\n", w, got); }
}

/* ── the controller ────────────────────────────────────────────────── */
static void put16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

static void build_identify(uint8_t *id, int port)
{
    memset(id, 0, 512);
    /* model, words 27..46, byte-swapped within each word */
    { const char *m = "SIMULATED AHCI DISK 0                   ";
      char name[41]; memcpy(name, m, 41); name[20] = (char)('0' + port);
      for(int i = 0; i < 20; i++){
          id[27*2 + i*2]     = (uint8_t)name[i*2 + 1];
          id[27*2 + i*2 + 1] = (uint8_t)name[i*2];
      } }
    if(g_idkind[port] == ID_ZERO_CAP){
        /* Identifies perfectly and reports no sectors. A drive that has
           not finished spinning up, or one behind a bridge that answers
           for it. Accepting it gives a drive of zero sectors, against
           which every bounds check passes. */
        return;
    }

    put16(id + 60*2, (uint16_t)(SIM_SECTORS & 0xFFFFu));   /* LBA28 count */
    put16(id + 61*2, (uint16_t)(SIM_SECTORS >> 16));

    if(g_idkind[port] == ID_NO_LBA48){
        /* Word 83 bit 10 CLEAR. Words 100..103 are then undefined, and
           plenty of drives leave rubbish there -- which is why the bit
           has to be read rather than the words trusted. */
        put16(id + 83*2, 0x0000);
        put16(id + 100*2, 0xDEAD);
        put16(id + 101*2, 0xBEEF);
        put16(id + 102*2, 0xCAFE);
        put16(id + 103*2, 0xF00D);
        return;
    }

    put16(id + 83*2, 0x0400);                              /* LBA48 supported  */
    put16(id + 100*2, (uint16_t)(SIM_SECTORS & 0xFFFFu));
    put16(id + 101*2, (uint16_t)(SIM_SECTORS >> 16));
    put16(id + 102*2, 0);
    put16(id + 103*2, 0);
}

static void sim_ci(int port, uint32_t val)
{
    if(!(val & 1u)) return;
    uint32_t pbase = 0x100u + (uint32_t)port * 0x80u;
    uint32_t clb   = *(volatile uint32_t *)(uintptr_t)
                     ((uint32_t)(uintptr_t)g_regs + pbase + 0x00u);
    if(!clb) return;

    cmd_hdr_t *h = (cmd_hdr_t *)(uintptr_t)clb;
    cmd_tbl_t *t = (cmd_tbl_t *)(uintptr_t)h->ctba;
    uint8_t   *f = t->cfis;

    g_n_cmd++;

    /* A real controller does not execute a malformed FIS, and the fields
       below are exactly the ones a driver can leave wrong while every
       transfer still appears to work in a forgiving simulator.

         f[0]  must be 0x27, a Host-to-Device Register FIS
         f[1]  bit 7 (C) must be set, or the device treats it as a control
               update and latches no command at all
         CFL   in the command header must be 5 dwords: that is the length
               of an H2D Register FIS, and the controller fetches exactly
               that many
         f[7]  bit 6 must be set to select LBA addressing; clear means CHS,
               where the same bytes mean cylinder, head and sector      */
    if(f[0] != FIS_H2D)        { g_bad_fis = 1; g_bad_why = "FIS type not 0x27"; }
    if(!(f[1] & 0x80u))        { g_bad_fis = 1; g_bad_why = "C bit clear"; }
    if((h->flags & 0x1Fu) != 5){ g_bad_fis = 1; g_bad_why = "CFL not 5 dwords"; }
    if(!(f[7] & 0x40u))        { g_bad_fis = 1; g_bad_why = "LBA mode bit clear"; }
    if(g_bad_fis){
        *(volatile uint32_t *)(uintptr_t)
            ((uint32_t)(uintptr_t)g_regs + pbase + 0x20u) = 0x51;
        *(volatile uint32_t *)(uintptr_t)
            ((uint32_t)(uintptr_t)g_regs + pbase + 0x38u) = 0;
        return;
    }

    uint8_t  op    = f[2];
    uint64_t lba   = (uint64_t)f[4] | ((uint64_t)f[5] << 8)
                   | ((uint64_t)f[6] << 16) | ((uint64_t)f[8] << 24)
                   | ((uint64_t)f[9] << 32) | ((uint64_t)f[10] << 40);
    uint16_t cnt   = (uint16_t)f[12] | ((uint16_t)f[13] << 8);
    int      wbit  = (h->flags & 0x40u) ? 1 : 0;
    uint32_t prdtl = h->prdtl;
    uint8_t *dma   = prdtl ? (uint8_t *)(uintptr_t)t->prdt[0].dba : 0;
    uint32_t bytes = prdtl ? ((t->prdt[0].dbc & 0x3FFFFFu) + 1u) : 0;

    g_last_lba = lba; g_last_count = cnt;
    g_last_bytes = bytes; g_last_was_write = wbit;

    uint32_t tfd = 0;

    if(g_fail_next){
        g_fail_next = 0;
        tfd = 0x51;                              /* ERR | DRDY | BSY-clear */
    } else if(op == ATA_IDENTIFY || op == ATA_IDENTIFY_P){
        g_n_ident++;
        if(dma && bytes >= 512){
            uint8_t id[512];
            build_identify(id, port);
            memcpy(dma, id, 512);
            h->prdbc = 512;
        }
    } else if(op == ATA_READ_DMA){
        /* The W bit must be CLEAR on a read. */
        if(wbit) tfd = 0x51;
        else if(lba + cnt > SIM_SECTORS) tfd = 0x51;
        else if(dma){
            memcpy(dma, g_store[port] + lba * 512u, cnt * 512u);
            h->prdbc = g_short_prdbc ? 256 : cnt * 512u;
            g_n_read++;
        }
    } else if(op == ATA_WRITE_DMA){
        /* And SET on a write. A controller handed a write command with
           the W bit clear transfers the other way -- it reads the disk
           into the host buffer -- and reports success, which is how a
           write that does nothing looks exactly like a write that
           worked. */
        if(!wbit){
            if(dma) memcpy(dma, g_store[port] + lba * 512u, cnt * 512u);
            g_n_read++;
        } else if(lba + cnt > SIM_SECTORS){
            tfd = 0x51;
        } else if(dma){
            memcpy(g_store[port] + lba * 512u, dma, cnt * 512u);
            h->prdbc = cnt * 512u;
            g_n_write++;
        }
    } else {
        tfd = 0x51;
    }

    *(volatile uint32_t *)(uintptr_t)
        ((uint32_t)(uintptr_t)g_regs + pbase + 0x20u) = tfd;   /* PxTFD */
    *(volatile uint32_t *)(uintptr_t)
        ((uint32_t)(uintptr_t)g_regs + pbase + 0x38u) = 0;     /* PxCI  */
}

/* ── wiring up a machine ───────────────────────────────────────────── */
static void machine_full(uint32_t pi, const int *present, const int *idkind,
                         int prerun)
{
    memset(g_regs, 0, sizeof g_regs);
    memset(&g_pci, 0, sizeof g_pci);
    g_pci.class = 0x01; g_pci.subclass = 0x06; g_pci.progif = 0x01;
    g_pi = pi;
    g_n_read = g_n_write = g_n_ident = 0;
    g_clb_while_running = 0;
    g_fail_next = 0; g_short_prdbc = 0;
    g_bus_master = 0;

    g_n_cmd = 0; g_bad_fis = 0; g_bad_why = 0;
    g_klogs = 0;
    g_prerun = prerun;
    for(int p = 0; p < SIM_PORTS; p++){ g_cr_left[p] = 0; g_fr_left[p] = 0; }
    for(int p = 0; p < SIM_PORTS; p++){
        g_present[p] = present ? present[p] : PRES_NONE;
        g_idkind[p]  = idkind  ? idkind[p]  : ID_NORMAL;
    }

    uint32_t base = (uint32_t)(uintptr_t)g_regs;
    *(volatile uint32_t *)(uintptr_t)(base + HBA_CAP) = (SIM_PORTS - 1u);
    *(volatile uint32_t *)(uintptr_t)(base + HBA_PI)  = pi;
    *(volatile uint32_t *)(uintptr_t)(base + HBA_VS)  = 0x00010300u;

    for(int p = 0; p < SIM_PORTS; p++){
        uint32_t pb = base + 0x100u + (uint32_t)p * 0x80u;
        uint32_t ssts = 0, sig = 0;
        switch(g_present[p]){
        case PRES_SATA:      ssts = 0x0133u; sig = SIG_ATA;      break;
        case PRES_ATAPI:     ssts = 0x0133u; sig = SIG_ATAPI;    break;
        case PRES_STALE_SIG: ssts = 0x0000u; sig = SIG_ATA;      break;
        case PRES_OTHER:     ssts = 0x0133u; sig = 0x96690101u;  break;
        default:             ssts = 0;       sig = 0;            break;
        }
        *(volatile uint32_t *)(uintptr_t)(pb + PxSSTS) = ssts;
        *(volatile uint32_t *)(uintptr_t)(pb + PxSIG)  = sig;
        /* Firmware with an AHCI driver loaded leaves the ports RUNNING.
           Starting from zero made "did the driver stop the port before
           replacing its command list" unanswerable, because the engines
           were never going in the first place. */
        if(prerun)
            *(volatile uint32_t *)(uintptr_t)(pb + PxCMD) = 0xC011u;  /* CR|FR|FRE|ST */
    }
}

static void machine(uint32_t pi, const int *present){
    /* Ports already running, which is the state a real machine hands over:
       the default, so every test asks the harder question. */
    machine_full(pi, present, 0, 1);
}

static void fill_store(int port, uint32_t salt){
    for(uint32_t i = 0; i < sizeof g_store[0]; i++)
        g_store[port][i] = (uint8_t)(i*31u + (i>>9)*7u + salt);
}
static uint8_t store_byte(uint32_t off, uint32_t salt){
    return (uint8_t)(off*31u + (off>>9)*7u + salt);
}

int main(void)
{
    static uint8_t buf[512], buf2[512];
    uint32_t i;

    printf("AHCI against a controller that executes its commands\n");
    printf("====================================================\n");

    head("every implemented port with a disk, not just the first");
    {
        /* drivers/ata.c breaks out of its probe loop on the first drive
           it finds, which is why a machine with two disks has only ever
           shown one of them. */
        const int p[SIM_PORTS] = {1, 0, 1, 2, 0, 1};
        machine(0x2Du, p);                  /* ports 0,2,3,5 implemented  */
        int n = ahci_init();
        ck("four ports implemented, four devices behind them", n == 4, n);
        ck("the controller was made a bus master", g_bus_master == 1, 0);
        ck("each one was identified", g_n_ident == 4, g_n_ident);
        ck("three are disks and one is an optical drive",
           !ahci_is_atapi(0) && !ahci_is_atapi(1)
           && ahci_is_atapi(2) && !ahci_is_atapi(3), 0);
        ck("every FIS was one a real controller would execute",
           g_bad_fis == 0, 0);
        if(g_bad_fis) printf("      the controller refused it: %s\n", g_bad_why);
        ck("the model string came back byte-swapped correctly",
           ahci_model(0)[0]=='S' && ahci_model(0)[1]=='I'
           && ahci_model(0)[2]=='M', 0);
        ck("and trailing spaces are trimmed",
           ahci_model(0)[strlen(ahci_model(0))-1] != ' ', 0);
        ck("capacity came from IDENTIFY, not from a guess",
           ahci_sectors(0) == SIM_SECTORS, (long)ahci_sectors(0));
        printf("      found: %s, %llu sectors\n", ahci_model(0),
               (unsigned long long)ahci_sectors(0));
    }

    head("a port with nothing on it is skipped, not counted");
    {
        const int p[SIM_PORTS] = {0, 0, 0, 0, 0, 0};
        machine(0x3Fu, p);
        ck("no devices, no drives", ahci_init() == 0, 0);

        const int q[SIM_PORTS] = {1, 1, 1, 1, 1, 1};
        machine(0x01u, q);                  /* only port 0 implemented    */
        ck("a device on an unimplemented port is not reached",
           ahci_init() == 1, 0);
    }

    head("a port whose device has gone, and one that is not a disk");
    {
        /* Two cases that a single check cannot tell apart, and which hide
           each other if only one of them is ever tested:

             a stale signature -- DET says nothing is there, but PxSIG
             still reads 0x101 from the device that used to be. Only the
             DET check refuses it.

             a device that is not a disk -- DET and IPM both say a device
             is present and talking, and the signature says port
             multiplier. Only the signature check refuses it. */
        const int p[SIM_PORTS]  = {PRES_SATA, PRES_STALE_SIG, PRES_OTHER,
                                   PRES_NONE, PRES_NONE, PRES_NONE};
        machine(0x07u, p);
        int n = ahci_init();
        ck("only the real disk is taken", n == 1, n);
        ck("the stale-signature port is not a drive",
           ahci_sectors(1) == 0, (long)ahci_sectors(1));
        ck("nor is the port multiplier", ahci_count() == 1, ahci_count());
        ck("the log does not claim them",
           strstr(ahci_debug(), "port 1") == 0
           && strstr(ahci_debug(), "port 2") == 0, 0);
    }

    head("a drive without LBA48, and one that reports no capacity");
    {
        /* Word 83 bit 10 is the only thing that says words 100..103 mean
           anything. On a drive without it they are whatever the vendor
           left there -- 0xDEAD, 0xBEEF and friends in this fixture -- and
           a driver that reads them anyway invents a 17-exabyte disk. */
        const int p[SIM_PORTS] = {PRES_SATA, PRES_SATA, PRES_NONE,
                                  PRES_NONE, PRES_NONE, PRES_NONE};
        const int k[SIM_PORTS] = {ID_NO_LBA48, ID_ZERO_CAP, 0, 0, 0, 0};
        machine_full(0x03u, p, k, 1);
        int n = ahci_init();

        ck("the LBA28 drive is taken", n == 1, n);
        ck("its capacity comes from words 60 and 61",
           ahci_sectors(0) == SIM_SECTORS, (long)ahci_sectors(0));
        ck("not from the rubbish in 100..103",
           ahci_sectors(0) != 0xF00DCAFEBEEFDEADull
           && (ahci_sectors(0) >> 32) == 0, (long)(ahci_sectors(0) >> 32));
        ck("and the drive that reports nothing is not taken",
           ahci_count() == 1, ahci_count());
    }

    head("the command list is never replaced while the port is running");
    {
        /* By specification the controller's behaviour is indeterminate,
           and a driver that gets away with it in QEMU will not on an
           Intel PCH. The simulated controller records it. */
        const int p[SIM_PORTS] = {1, 1, 0, 0, 0, 0};
        machine(0x03u, p);
        ahci_init();
        ck("PxCLB was never written with PxCMD.CR set",
           g_clb_while_running == 0, g_clb_while_running);
    }

    head("reading a sector gives back what is on the disk");
    {
        const int p[SIM_PORTS] = {1, 0, 0, 0, 0, 0};
        machine(0x01u, p);
        ahci_init();
        fill_store(0, 11);
        memset(buf, 0, sizeof buf);
        ck("read sector 300", ahci_read_sector(0, 300, buf) == 1, 0);
        { int bad = 0;
          for(i = 0; i < 512; i++)
              if(buf[i] != store_byte(300*512 + i, 11)) bad++;
          ck("every byte matches the store", bad == 0, bad); }
        ck("the controller was asked for LBA 300",
           g_last_lba == 300, (long)g_last_lba);
        ck("one sector", g_last_count == 1, g_last_count);
        ck("512 bytes, not 511 and not 513",
           g_last_bytes == 512, (long)g_last_bytes);
        ck("with the write bit CLEAR", g_last_was_write == 0, 0);
        ck("and the FIS was well formed", g_bad_fis == 0, 0);
        if(g_bad_fis) printf("      refused: %s\n", g_bad_why);
    }

    head("THE HAZARD: a write must set the W bit in the command header");
    {
        /* With W clear the controller transfers the other way: it reads
           the disk into the buffer we meant to write from, clears PxCI,
           and reports no error. The write silently does nothing, and
           every status code says it worked. */
        const int p[SIM_PORTS] = {1, 0, 0, 0, 0, 0};
        machine(0x01u, p);
        ahci_init();
        fill_store(0, 22);
        for(i = 0; i < 512; i++) buf[i] = (uint8_t)(i*13u + 7u);

        ck("write sector 500", ahci_write_sector(0, 500, buf) == 1, 0);
        ck("the W bit was set", g_last_was_write == 1, 0);
        ck("the controller performed a write, not a read",
           g_n_write == 1, g_n_write);
        { int bad = 0;
          for(i = 0; i < 512; i++)
              if(g_store[0][500*512 + i] != (uint8_t)(i*13u + 7u)) bad++;
          ck("and the store now holds those bytes", bad == 0, bad); }

        memset(buf2, 0, sizeof buf2);
        ck("reading it back agrees", ahci_read_sector(0, 500, buf2) == 1, 0);
        ck("byte for byte", memcmp(buf, buf2, 512) == 0, 0);

        { int bad = 0;
          for(i = 0; i < 512; i++)
              if(g_store[0][499*512 + i] != store_byte(499*512 + i, 22)) bad++;
          ck("the sector before it is untouched", bad == 0, bad); }
        { int bad = 0;
          for(i = 0; i < 512; i++)
              if(g_store[0][501*512 + i] != store_byte(501*512 + i, 22)) bad++;
          ck("and the one after", bad == 0, bad); }
        ck("every FIS was well formed throughout", g_bad_fis == 0, 0);
        if(g_bad_fis) printf("      refused: %s\n", g_bad_why);
    }

    head("a 48-bit LBA arrives whole");
    {
        /* drivers/ata.c's LBA48 path takes a uint32_t and writes literal
           zeroes into LBA[47:32], so every access above 2 TB lands
           somewhere else entirely. The FIS has six LBA bytes and all six
           have to carry something. */
        const int p[SIM_PORTS] = {1, 0, 0, 0, 0, 0};
        machine(0x01u, p);
        ahci_init();
        g_drv[0].sectors = 0;                   /* no bounds check in the way */

        uint64_t big = 0x0000123456789ABCull;
        ahci_read_sector(0, big, buf);
        ck("all 48 bits reach the command FIS",
           g_last_lba == big, (long)(g_last_lba & 0xFFFFFFFFu));
        ck("including the top sixteen",
           (g_last_lba >> 32) == 0x1234ull, (long)(g_last_lba >> 32));

        ahci_write_sector(0, big, buf);
        ck("on the write path too", g_last_lba == big, 0);
    }

    head("the refusals");
    {
        const int p[SIM_PORTS] = {1, 0, 0, 2, 0, 0};
        machine(0x09u, p);
        ahci_init();
        /* Refused is not enough. The controller here answers an
           out-of-range command with an error, so a driver with no bounds
           check "fails" too -- having issued a command against a sector
           the drive does not have. On a controller less forgiving that is
           a port reset, and on a drive whose capacity was misread it is a
           write to whatever is actually there. So: nothing on the wire. */
        g_n_cmd = 0;
        ck("a read past the end is refused",
           ahci_read_sector(0, SIM_SECTORS, buf) == 0, 0);
        ck("and no command was issued", g_n_cmd == 0, g_n_cmd);
        g_n_cmd = 0;
        ck("a write past the end is refused",
           ahci_write_sector(0, SIM_SECTORS, buf) == 0, 0);
        ck("nothing issued there either", g_n_cmd == 0, g_n_cmd);
        g_n_cmd = 0;
        ck("far past the end, likewise",
           ahci_read_sector(0, SIM_SECTORS * 16u, buf) == 0
           && g_n_cmd == 0, g_n_cmd);
        ck("an out-of-range drive index is refused",
           ahci_read_sector(99, 0, buf) == 0, 0);
        ck("a negative one too", ahci_read_sector(-1, 0, buf) == 0, 0);
        ck("a null buffer is refused", ahci_read_sector(0, 0, 0) == 0, 0);
        ck("reading an optical drive as a disk is refused",
           ahci_read_sector(1, 0, buf) == 0, 0);
        ck("and writing to one", ahci_write_sector(1, 0, buf) == 0, 0);
    }

    head("a command the controller fails is not reported as success");
    {
        const int p[SIM_PORTS] = {1, 0, 0, 0, 0, 0};
        machine(0x01u, p);
        ahci_init();
        fill_store(0, 33);
        g_fail_next = 1;
        ck("a failed read returns 0", ahci_read_sector(0, 7, buf) == 0, 0);
        g_fail_next = 1;
        for(i = 0; i < 512; i++) buf[i] = 0xEE;
        ck("a failed write returns 0", ahci_write_sector(0, 7, buf) == 0, 0);
        { int bad = 0;
          for(i = 0; i < 512; i++)
              if(g_store[0][7*512 + i] != store_byte(7*512 + i, 33)) bad++;
          ck("and the sector is as it was", bad == 0, bad); }
    }

    head("a controller that never came up");
    {
        const int p[SIM_PORTS] = {1, 0, 0, 0, 0, 0};
        machine(0x01u, p);
        ahci_init();
        g_drv[0].ok = 0;
        ck("reads are refused", ahci_read_sector(0, 0, buf) == 0, 0);
        ck("writes are refused", ahci_write_sector(0, 0, buf) == 0, 0);
    }

    head("the boot log says what was found, and nothing it did not find");
    {
        const int p[SIM_PORTS] = {1, 0, 1, 0, 0, 0};
        machine(0x05u, p);
        ahci_init();
        const char *l = ahci_debug();
        ck("it mentions AHCI", strstr(l, "AHCI") != 0, 0);
        ck("it names both ports", strstr(l, "port 0") && strstr(l, "port 2"), 0);
        ck("it does not claim a port that is not there",
           strstr(l, "port 1") == 0 && strstr(l, "port 3") == 0, 0);
        /* One line for the controller and one per drive brought up. A
           driver that logged a port it then rejected would report
           hardware the machine does not have, in the one place a user
           looks to find out what it has. */
        ck("the boot log has one line per controller and one per drive",
           g_klogs == 3, g_klogs);

        machine(0x3Fu, 0);
        ahci_init();
        ck("with no disks it says so",
           strstr(ahci_debug(), "No AHCI disk found") != 0, 0);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
