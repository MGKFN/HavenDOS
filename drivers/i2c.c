/* ════════════════════════════════════════════════════════════════════
   drivers/i2c.c -- the Synopsys DesignWare I2C controller.  v0.8.8.3.

   include/i2c.h carries the reasoning: why the trackpad needs this,
   why a scan is the only way to find its address without an AML
   interpreter, and why the BAR on this machine is above 4 GB.

   ── THE CORE ────────────────────────────────────────────────────────

   Intel's Serial-IO I2C is a wrapper around the Synopsys DesignWare
   APB I2C core, which is the same part on every Intel chipset since
   Baytrail. Registers are 32 bits, little-endian, at a 4-byte pitch,
   and the ones that matter are few:

     CON        master mode, speed, restart enable, 7-bit addressing
     TAR        the target address -- WRITE ONLY WHILE DISABLED
     DATA_CMD   the FIFO.  Writing queues a byte or a read command;
                reading takes a received byte.  Bit 8 is READ, bit 9
                is STOP, bit 10 is RESTART.
     ENABLE     1 to run
     STATUS     bit 0 ACTIVITY, bit 1 TFNF, bit 2 TFE, bit 3 RFNE
     TXFLR/RXFLR how full each FIFO is
     RAW_INTR_STAT  bit 6 TX_ABRT is the one that matters
     TX_ABRT_SOURCE why it aborted -- bit 0 is "nobody acknowledged
                the address", which is how a scan learns anything
     CLR_TX_ABRT   reading it clears the abort

   ── EVERY REFUSAL IS A NAMED CODE ───────────────────────────────────

   This file has no function that returns 0 for four different
   reasons. A NAK and a timeout and a busy bus have three different
   fixes, and the whole of i2c_scan() rests on being able to tell a
   NAK from everything else -- a scan that cannot would report every
   address as present or none of them.

   ── ARITHMETIC, AND THE RULE THIS TREE KEEPS ────────────────────────

   No 64-bit division anywhere. __udivmoddi4 is not linked into this
   kernel and fs/exfat.c and net/bignum.c both hit it; the clock
   counts here are all 32-bit.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/i2c.h"
#include "../include/pci.h"
#include "../include/highfb.h"
#include "../include/klog.h"

/* ── THE SEAM: TWO HOOKS, NOT ONE POINTER ────────────────────────────
   A register read and a register write are different events to a
   device, and a simulated controller has to see both. The first
   version of this was a VGA_MEM_PTR-style macro returning a
   `volatile uint32_t *`, which works for the console because the
   console's memory IS memory -- a write to it is a write and nothing
   else happens. An I2C controller is not memory: writing IC_DATA_CMD
   puts a byte on a wire.

   With a pointer seam the suite could only observe reads; every
   write landed in the scratch word the macro handed back, the LPSS
   reset was never released, and 31 of 41 checks failed in a way that
   looked like a broken driver and was a broken test harness.

   So: two macros. Same shape as IO_H and MACHINE_ASM_H elsewhere in
   this tree. */
#ifndef I2C_RD
#define I2C_RD(base, off) \
    (*(volatile uint32_t *)(uintptr_t)((base) + (off)))
#endif
#ifndef I2C_WR
#define I2C_WR(base, off, v) \
    (*(volatile uint32_t *)(uintptr_t)((base) + (off)) = (uint32_t)(v))
#endif

/* ── DesignWare register offsets ─────────────────────────────────── */
#define IC_CON              0x00
#define IC_TAR              0x04
#define IC_DATA_CMD         0x10
#define IC_SS_SCL_HCNT      0x14
#define IC_SS_SCL_LCNT      0x18
#define IC_FS_SCL_HCNT      0x1C
#define IC_FS_SCL_LCNT      0x20
#define IC_INTR_MASK        0x30
#define IC_RAW_INTR_STAT    0x34
#define IC_RX_TL            0x38
#define IC_TX_TL            0x3C
#define IC_CLR_INTR         0x40
#define IC_CLR_TX_ABRT      0x54
#define IC_ENABLE           0x6C
#define IC_STATUS           0x70
#define IC_TXFLR            0x74
#define IC_RXFLR            0x78
#define IC_SDA_HOLD         0x7C
#define IC_TX_ABRT_SOURCE   0x80
#define IC_ENABLE_STATUS    0x9C
#define IC_COMP_PARAM_1     0xF4
#define IC_COMP_TYPE        0xFC

#define DW_COMP_TYPE        0x44570140u   /* "DW" + version; the core
                                             identifies itself here   */

/* IC_CON */
#define CON_MASTER          (1u << 0)
#define CON_SPEED_FAST      (2u << 1)
#define CON_SLAVE_DISABLE   (1u << 6)
#define CON_RESTART_EN      (1u << 5)

/* IC_DATA_CMD */
#define CMD_READ            (1u << 8)
#define CMD_STOP            (1u << 9)
#define CMD_RESTART         (1u << 10)

/* IC_STATUS */
#define ST_ACTIVITY         (1u << 0)
#define ST_TFNF             (1u << 1)   /* transmit FIFO not full      */
#define ST_TFE              (1u << 2)   /* transmit FIFO empty         */
#define ST_RFNE             (1u << 3)   /* receive FIFO not empty      */
#define ST_MST_ACTIVITY     (1u << 5)

/* IC_RAW_INTR_STAT */
#define INTR_TX_ABRT        (1u << 6)
#define INTR_STOP_DET       (1u << 9)

/* IC_TX_ABRT_SOURCE */
#define ABRT_7B_ADDR_NOACK  (1u << 0)
#define ABRT_TXDATA_NOACK   (1u << 3)
#define ABRT_ARB_LOST       (1u << 12)

/* Intel LPSS private space, which sits past the DesignWare core. The
   reset register has to be released before the core answers at all --
   on a cold boot the core reads back as zeroes without it, which
   looks exactly like a BAR mapped to nothing. */
#define LPSS_RESET          0x204
#define LPSS_RESET_RELEASE  0x7u

typedef struct {
    const pci_dev_t *pci;
    uint32_t         mmio;        /* mapped, usable address            */
    uint32_t         khz;
    uint32_t         txfifo;
    int              ready;
} i2c_bus_t;

static i2c_bus_t g_bus[I2C_MAX_BUS];
static int       g_nbus = 0;
static int       g_inited = 0;

const char *i2c_why_name(int why)
{
    switch(why){
    case I2C_OK:        return "ok";
    case I2C_ENOBUS:    return "no such I2C bus";
    case I2C_ENOTREADY: return "the controller never left reset";
    case I2C_ENAK:      return "nobody acknowledged that address";
    case I2C_ETIMEOUT:  return "the transfer did not finish";
    case I2C_EBUSY:     return "the bus was busy";
    case I2C_EARB:      return "arbitration lost to another master";
    case I2C_ETOOLONG:  return "longer than the FIFO";
    default:            return "unknown";
    }
}

static uint32_t rd(const i2c_bus_t *b, uint32_t off)
{
    return I2C_RD(b->mmio, off);
}
static void wr(const i2c_bus_t *b, uint32_t off, uint32_t v)
{
    I2C_WR(b->mmio, off, v);
}

/* A bounded spin. No sleeping: this is called from places that may
   hold a lock, and a bus transaction is microseconds. The count is
   generous enough for a 100 kHz bus stretching a clock and small
   enough that a dead controller does not hang the machine. */
#define SPIN_LIMIT 2000000u

static int wait_bit(const i2c_bus_t *b, uint32_t reg, uint32_t mask, int set)
{
    uint32_t n = SPIN_LIMIT;
    while(n--){
        uint32_t v = rd(b, reg);
        if(set  && (v & mask)) return 1;
        if(!set && !(v & mask)) return 1;
    }
    return 0;
}

/* Why the last transfer aborted, as one of this file's codes. Reading
   IC_CLR_TX_ABRT clears the condition, so this is also the thing that
   makes the controller usable again -- and it must be read even when
   the caller does not care, or the next transfer starts aborted. */
static int abort_reason(const i2c_bus_t *b)
{
    uint32_t src = rd(b, IC_TX_ABRT_SOURCE);
    (void)rd(b, IC_CLR_TX_ABRT);
    if(src & (ABRT_7B_ADDR_NOACK | ABRT_TXDATA_NOACK)) return I2C_ENAK;
    if(src & ABRT_ARB_LOST) return I2C_EARB;
    if(src) return I2C_ETIMEOUT;
    return I2C_OK;
}

static void disable(const i2c_bus_t *b)
{
    uint32_t n = 100000u;
    wr(b, IC_ENABLE, 0);
    /* IC_ENABLE_STATUS bit 0 is the one that actually says the core
       is off. Writing IC_TAR while it is still on is ignored, and a
       target address that did not take is a transfer to address
       zero -- which on a laptop is the general-call address and is
       not a thing to send by accident. */
    while(n-- && (rd(b, IC_ENABLE_STATUS) & 1u)) { }
}

static void enable(const i2c_bus_t *b)
{
    wr(b, IC_ENABLE, 1);
}

/* Point the controller at one address, with the core disabled, and
   turn it back on. Every transfer starts here. */
static int set_target(const i2c_bus_t *b, uint8_t addr)
{
    if(!b->ready) return I2C_ENOTREADY;
    /* A transfer that starts while the previous one is still on the
       wire corrupts both. */
    if(rd(b, IC_STATUS) & ST_MST_ACTIVITY){
        if(!wait_bit(b, IC_STATUS, ST_MST_ACTIVITY, 0)) return I2C_EBUSY;
    }
    disable(b);
    wr(b, IC_TAR, (uint32_t)(addr & 0x7Fu));
    (void)rd(b, IC_CLR_TX_ABRT);     /* start from a clean abort state */
    enable(b);
    return I2C_OK;
}

/* Wait for the transmit FIFO to drain and the bus to go idle, then
   report an abort if one happened. This is the end of every
   transfer and the only place a NAK is turned into I2C_ENAK. */
static int finish(const i2c_bus_t *b)
{
    if(!wait_bit(b, IC_RAW_INTR_STAT, INTR_STOP_DET, 1)){
        /* No STOP seen. Either the FIFO never drained or the device
           is holding the clock down for ever. */
        int r = abort_reason(b);
        return r == I2C_OK ? I2C_ETIMEOUT : r;
    }
    (void)rd(b, IC_CLR_INTR);
    if(rd(b, IC_RAW_INTR_STAT) & INTR_TX_ABRT){
        int r = abort_reason(b);
        return r == I2C_OK ? I2C_ETIMEOUT : r;
    }
    return I2C_OK;
}

int i2c_write(int bus, uint8_t addr, const uint8_t *buf, uint32_t len)
{
    i2c_bus_t *b;
    uint32_t i;
    int r;

    if(bus < 0 || bus >= g_nbus) return I2C_ENOBUS;
    b = &g_bus[bus];
    if(!b->ready) return I2C_ENOTREADY;
    if(len > b->txfifo) return I2C_ETOOLONG;

    r = set_target(b, addr);
    if(r != I2C_OK) return r;
    (void)rd(b, IC_CLR_INTR);

    for(i = 0; i < len; i++){
        uint32_t cmd = buf[i];
        if(i == len - 1u) cmd |= CMD_STOP;
        if(!wait_bit(b, IC_STATUS, ST_TFNF, 1)) return I2C_ETIMEOUT;
        wr(b, IC_DATA_CMD, cmd);
    }
    /* A zero-length write still has to put a STOP on the wire, or the
       bus is left held. */
    if(len == 0){
        if(!wait_bit(b, IC_STATUS, ST_TFNF, 1)) return I2C_ETIMEOUT;
        wr(b, IC_DATA_CMD, CMD_STOP);
    }
    return finish(b);
}

int i2c_read(int bus, uint8_t addr, uint8_t *buf, uint32_t len)
{
    i2c_bus_t *b;
    uint32_t i, got = 0;
    int r;

    if(bus < 0 || bus >= g_nbus) return I2C_ENOBUS;
    b = &g_bus[bus];
    if(!b->ready) return I2C_ENOTREADY;
    if(len > b->txfifo) return I2C_ETOOLONG;

    r = set_target(b, addr);
    if(r != I2C_OK) return r;
    (void)rd(b, IC_CLR_INTR);

    /* One READ command per byte wanted. The core turns each into a
       clock burst and puts the result in the receive FIFO. */
    for(i = 0; i < len; i++){
        uint32_t cmd = CMD_READ;
        if(i == len - 1u) cmd |= CMD_STOP;
        if(!wait_bit(b, IC_STATUS, ST_TFNF, 1)) return I2C_ETIMEOUT;
        wr(b, IC_DATA_CMD, cmd);
    }
    if(len == 0){
        /* THE PROBE. Address the device, no data, stop -- and the
           only thing that comes back is whether it acknowledged. */
        if(!wait_bit(b, IC_STATUS, ST_TFNF, 1)) return I2C_ETIMEOUT;
        wr(b, IC_DATA_CMD, CMD_READ | CMD_STOP);
    }

    while(got < len){
        if(rd(b, IC_RAW_INTR_STAT) & INTR_TX_ABRT){
            int why = abort_reason(b);
            return why == I2C_OK ? I2C_ETIMEOUT : why;
        }
        if(rd(b, IC_STATUS) & ST_RFNE){
            buf[got++] = (uint8_t)(rd(b, IC_DATA_CMD) & 0xFFu);
            continue;
        }
        if(!wait_bit(b, IC_STATUS, ST_RFNE, 1)){
            int why = abort_reason(b);
            return why == I2C_OK ? I2C_ETIMEOUT : why;
        }
    }
    return finish(b);
}

int i2c_write_read(int bus, uint8_t addr,
                   const uint8_t *wbuf, uint32_t wlen,
                   uint8_t *rbuf, uint32_t rlen)
{
    i2c_bus_t *b;
    uint32_t i, got = 0;
    int r;

    if(bus < 0 || bus >= g_nbus) return I2C_ENOBUS;
    b = &g_bus[bus];
    if(!b->ready) return I2C_ENOTREADY;
    if(wlen + rlen > b->txfifo) return I2C_ETOOLONG;

    r = set_target(b, addr);
    if(r != I2C_OK) return r;
    (void)rd(b, IC_CLR_INTR);

    /* The register number, then a REPEATED START, then the reads.
       Not a stop in between: a stop releases the bus and another
       master may address the same device, after which the register
       pointer is whatever they left it at. This is the whole reason
       CON_RESTART_EN is set. */
    for(i = 0; i < wlen; i++){
        if(!wait_bit(b, IC_STATUS, ST_TFNF, 1)) return I2C_ETIMEOUT;
        wr(b, IC_DATA_CMD, wbuf[i]);
    }
    for(i = 0; i < rlen; i++){
        uint32_t cmd = CMD_READ;
        if(i == 0) cmd |= CMD_RESTART;
        if(i == rlen - 1u) cmd |= CMD_STOP;
        if(!wait_bit(b, IC_STATUS, ST_TFNF, 1)) return I2C_ETIMEOUT;
        wr(b, IC_DATA_CMD, cmd);
    }

    while(got < rlen){
        if(rd(b, IC_RAW_INTR_STAT) & INTR_TX_ABRT){
            int why = abort_reason(b);
            return why == I2C_OK ? I2C_ETIMEOUT : why;
        }
        if(rd(b, IC_STATUS) & ST_RFNE){
            rbuf[got++] = (uint8_t)(rd(b, IC_DATA_CMD) & 0xFFu);
            continue;
        }
        if(!wait_bit(b, IC_STATUS, ST_RFNE, 1)){
            int why = abort_reason(b);
            return why == I2C_OK ? I2C_ETIMEOUT : why;
        }
    }
    return finish(b);
}

int i2c_probe(int bus, uint8_t addr)
{
    /* A ZERO-LENGTH READ. Nothing is written, no register is named,
       and the only information that comes back is the acknowledge.
       Writing to an unknown device could do anything; reading from
       one cannot. */
    return i2c_read(bus, addr, 0, 0);
}

int i2c_scan(int bus, uint8_t *out, int max)
{
    int n = 0;
    uint8_t a;
    if(bus < 0 || bus >= g_nbus) return 0;
    /* 0x00-0x07 and 0x78-0x7F are reserved by the I2C specification:
       general call, ten-bit addressing, and the rest. Probing them is
       not harmless -- 0x00 is the general-call address and every
       device on the bus listens to it. */
    for(a = 0x08; a <= 0x77; a++){
        if(i2c_probe(bus, a) == I2C_OK){
            if(out && n < max) out[n] = a;
            n++;
        }
    }
    return n;
}

/* ── bringing a controller up ────────────────────────────────────── */

/* Standard-mode counts for a 100 kHz bus off the LPSS 133 MHz source
   clock, and fast-mode for 400 kHz. These are the values Intel's own
   documentation gives; they are not computed, because the input clock
   is not discoverable without ACPI either and a wrong divisor is a
   bus that does not work rather than one that works slowly. */
#define SS_HCNT 0x01F4
#define SS_LCNT 0x01F4
#define FS_HCNT 0x0082
#define FS_LCNT 0x00FA

static int bring_up(i2c_bus_t *b)
{
    uint32_t t, param;

    /* Intel's wrapper holds the core in reset out of a cold boot.
       Without this every register reads back zero, which is
       indistinguishable from a BAR mapped at nothing -- and that is
       the shape of the bug that cost v0.8.5 its USB. */
    wr(b, LPSS_RESET, LPSS_RESET_RELEASE);
    for(t = 0; t < 10000u; t++) { }

    /* THE CORE IDENTIFIES ITSELF. If this is not a DesignWare core,
       the offsets above are somebody else's registers and writing
       them is worse than doing nothing. */
    t = rd(b, IC_COMP_TYPE);
    if(t != DW_COMP_TYPE) return 0;

    disable(b);

    /* How deep the FIFOs are, read off the core rather than assumed.
       Bits 16-23 are TX_BUFFER_DEPTH minus one. A transfer longer
       than this needs DMA, and i2c_write() refuses it by name
       instead of silently truncating. */
    param = rd(b, IC_COMP_PARAM_1);
    b->txfifo = ((param >> 16) & 0xFFu) + 1u;
    if(b->txfifo == 0 || b->txfifo > 256u) b->txfifo = 32u;

    wr(b, IC_CON, CON_MASTER | CON_SLAVE_DISABLE | CON_RESTART_EN
                | CON_SPEED_FAST);
    wr(b, IC_SS_SCL_HCNT, SS_HCNT);
    wr(b, IC_SS_SCL_LCNT, SS_LCNT);
    wr(b, IC_FS_SCL_HCNT, FS_HCNT);
    wr(b, IC_FS_SCL_LCNT, FS_LCNT);
    wr(b, IC_SDA_HOLD, 0x1Cu);
    wr(b, IC_TX_TL, 0);
    wr(b, IC_RX_TL, 0);
    wr(b, IC_INTR_MASK, 0);          /* polled; no interrupts here     */
    (void)rd(b, IC_CLR_INTR);
    enable(b);

    b->khz = 400u;
    b->ready = 1;
    return 1;
}

int i2c_init(void)
{
    int i;

    if(g_inited) return g_nbus;
    g_inited = 1;
    g_nbus = 0;

    for(i = 0; i < PCI_MAX_DEVS; i++){
        const pci_dev_t *d = pci_get(i);
        uint32_t lo, hi = 0, mapped;
        i2c_bus_t *b;

        if(!d) break;
        /* Class 0C, subclass 80: "serial bus controller, other". That
           is what Intel reports a Serial-IO I2C as. It is also what
           some other things report, which is why bring_up() checks
           IC_COMP_TYPE before writing a single register. */
        if(d->class != PCI_CLASS_SERIAL || d->subclass != 0x80) continue;
        if(g_nbus >= I2C_MAX_BUS) break;

        b = &g_bus[g_nbus];
        memset(b, 0, sizeof *b);
        b->pci = d;

        pci_enable(d, 0);            /* memory space on, no bus master */

        /* THE BAR IS ABOVE 4 GB ON THIS MACHINE. pci_bar_mem() would
           hand back the low half and every register read would come
           from whatever sits 33 MB up -- which is exactly how v0.8.5's
           xHCI got a nonsense CAPLENGTH. */
        /* ── A SILENT `continue` IS NOT AN ANSWER, v0.8.8.4 ───────────
           This read `if(!lo && !hi) continue;` and said nothing. On
           the HP that meant 00:1F.5 -- the SPI flash controller,
           which is also class 0C/80 -- got a message saying it was
           not a DesignWare core, while 00:15.0 and 00:15.1, THE TWO
           THAT MATTER, vanished without a word and `i2c` reported
           "No I2C controller found".

           A BAR that reads zero is a real and common state: the
           firmware assigns memory windows only to devices it expects
           an OS to drive, and on a machine booted without a vendor
           driver the Serial-IO controllers are often left
           unassigned. That is a fixable situation -- the window can
           be assigned -- and it is a completely different problem
           from "the core is not DesignWare". Saying which is the
           whole point. */
        lo = pci_bar_mem64(d, 0, &hi);
        if(!lo && !hi){
            klog(KLOG_WARN, "I2C %2x:%2x.%u: BAR0 is unassigned -- the "
                 "firmware gave it no memory window",
                 (uint32_t)d->bus, (uint32_t)d->dev, (uint32_t)d->fn);
            continue;
        }

        if(hi){
            uint32_t virt = 0;
            /* highfb_map_io() returns SUCCESS and writes the address
               through its last argument -- it does not return the
               address. Reading it the other way round gives 1 as a
               pointer, which is the kind of mistake that reads
               registers out of the interrupt vector table. */
            if(!highfb_map_io(hi, lo, 0x1000u, &virt) || !virt){
                klog(KLOG_WARN, "I2C %2x:%2x.%u: BAR %8x%8x would "
                     "not map", (uint32_t)d->bus, (uint32_t)d->dev,
                     (uint32_t)d->fn, hi, lo);
                continue;
            }
            mapped = virt;
        } else {
            mapped = lo;
        }
        b->mmio = mapped;

        if(!bring_up(b)){
            klog(KLOG_WARN, "I2C %2x:%2x.%u at %8x: not a DesignWare "
                 "core (type %8x)", (uint32_t)d->bus, (uint32_t)d->dev,
                 (uint32_t)d->fn, mapped, rd(b, IC_COMP_TYPE));
            continue;
        }

        klog(KLOG_OK, "I2C %2x:%2x.%u at %8x, %u kHz, %u-byte FIFO",
             (uint32_t)d->bus, (uint32_t)d->dev, (uint32_t)d->fn,
             mapped, b->khz, b->txfifo);
        g_nbus++;
    }

    if(g_nbus == 0)
        klog(KLOG_WARN, "I2C: no Serial-IO controller on the bus");
    return g_nbus;
}

int              i2c_bus_count(void)       { return g_nbus; }
const pci_dev_t *i2c_bus_pci(int bus)      { return (bus >= 0 && bus < g_nbus) ? g_bus[bus].pci : 0; }
uint32_t         i2c_bus_mmio(int bus)     { return (bus >= 0 && bus < g_nbus) ? g_bus[bus].mmio : 0; }
uint32_t         i2c_bus_khz(int bus)      { return (bus >= 0 && bus < g_nbus) ? g_bus[bus].khz : 0; }
int              i2c_bus_ready(int bus)    { return (bus >= 0 && bus < g_nbus) ? g_bus[bus].ready : 0; }
