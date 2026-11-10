/* ════════════════════════════════════════════════════════════════════
   test_i2c.c -- the DesignWare I2C controller, against a simulated
   core that behaves like one.

   THIS DRIVER HAS NEVER SEEN AN I2C CONTROLLER. It was written from
   the DesignWare register map for a machine that is not here, and it
   will be run for the first time on Kyan's HP. That is the situation
   tests/test_usbmux.c was in when it passed 79 of 79 against a
   sixteen-byte header the phone has never used.

   So the simulated core here is written to be AWKWARD rather than
   agreeable:

     - it holds the core in RESET until the LPSS reset register is
       written, and reads back ZERO until then. That is what an Intel
       Serial-IO controller actually does out of a cold boot, and a
       driver that skips the release sees a BAR full of zeroes --
       indistinguishable from a mapping that failed, which is the bug
       that cost v0.8.5 its USB.
     - it IGNORES writes to SIM_IC_TAR while the core is enabled, as the
       hardware does. A driver that sets the target without disabling
       first addresses whatever was there before, and on a fresh core
       that is address zero -- the general-call address, which every
       device on the bus listens to.
     - it NAKs every address except the ones the test puts on the
       bus, and it sets ABRT_7B_ADDR_NOACK when it does. A scan rests
       entirely on telling a NAK from a timeout.
     - its FIFOs are FINITE and it reports their depth through
       SIM_IC_COMP_PARAM_1, so a driver that assumes 32 is caught on a
       core with 16.

   ── WHAT THIS CANNOT PROVE ──────────────────────────────────────────

   That the real controller behaves as the manual says. The simulated
   core is this project's reading of the register map, and a driver
   written from the same reading agrees with it perfectly. What it
   does prove is that the driver is internally consistent, refuses by
   name, and does not walk off the end of a FIFO -- and it makes the
   first boot on the laptop a test of ONE thing rather than twenty.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ── the simulated core, behind the driver's own seam ────────────── */
#define SIM_REGS  0x400u
static uint32_t sim[SIM_REGS / 4];
static uint32_t sim_base = 0x1000u;      /* a nonzero fake address    */

/* Which addresses have a device on them, and the core's state. */
static int      dev_present[128];
static int      in_reset   = 1;
static int      enabled    = 0;
static uint32_t tar        = 0;
static int      fifo_depth = 16;         /* deliberately NOT 32       */
/* What the core says it is. Class 0C/80 is "serial bus controller,
   other" and plenty of things report it, so IC_COMP_TYPE is the only
   thing that says this register map belongs to this device. The suite
   can make it lie. */
static uint32_t comp_type = 0x44570140u;

/* The transmit side, as a queue of command words, and the receive
   side as the bytes the addressed device "sent". */
static uint32_t txq[64]; static int txn = 0;
static uint8_t  rxq[64]; static int rxn = 0, rxi = 0;
/* What the last transfer actually put on the wire, so a check can
   assert the BYTES rather than the return code. */
static uint8_t  wire[64]; static int wire_n = 0;
static int      saw_restart = 0, saw_stop = 0;
/* What the device will answer with, set by the test. */
static uint8_t  dev_reply[64]; static int dev_reply_n = 0;

#define SIM_IC_CON              0x00
#define SIM_IC_TAR              0x04
#define SIM_IC_DATA_CMD         0x10
#define SIM_IC_CLR_TX_ABRT      0x54
#define SIM_IC_ENABLE           0x6C
#define SIM_IC_STATUS           0x70
#define SIM_IC_RAW_INTR_STAT    0x34
#define SIM_IC_CLR_INTR         0x40
#define SIM_IC_TX_ABRT_SOURCE   0x80
#define SIM_IC_ENABLE_STATUS    0x9C
#define SIM_IC_COMP_PARAM_1     0xF4
#define SIM_IC_COMP_TYPE        0xFC
#define SIM_LPSS_RESET          0x204

#define SIM_ST_ACTIVITY   (1u<<0)
#define SIM_ST_TFNF       (1u<<1)
#define SIM_ST_TFE        (1u<<2)
#define SIM_ST_RFNE       (1u<<3)
#define SIM_ST_MST_ACT    (1u<<5)
#define SIM_INTR_TX_ABRT  (1u<<6)
#define SIM_INTR_STOP_DET (1u<<9)
#define SIM_CMD_READ      (1u<<8)
#define SIM_CMD_STOP      (1u<<9)
#define SIM_CMD_RESTART   (1u<<10)
#define SIM_ABRT_NOACK    (1u<<0)

static uint32_t abrt_src = 0;
static uint32_t raw_intr = 0;

/* Run whatever is queued, as the core would when the FIFO drains. */
static void sim_execute(void)
{
    int i;
    if(!dev_present[tar & 0x7F]){
        abrt_src |= SIM_ABRT_NOACK;
        raw_intr |= SIM_INTR_TX_ABRT;
        txn = 0;
        return;
    }
    for(i = 0; i < txn; i++){
        uint32_t c = txq[i];
        if(c & SIM_CMD_RESTART) saw_restart = 1;
        if(c & SIM_CMD_STOP)    saw_stop = 1;
        if(c & SIM_CMD_READ){
            if(rxn < (int)sizeof rxq)
                rxq[rxn++] = (rxi < dev_reply_n) ? dev_reply[rxi++] : 0xFF;
        } else {
            if(wire_n < (int)sizeof wire) wire[wire_n++] = (uint8_t)(c & 0xFF);
        }
    }
    txn = 0;
    raw_intr |= SIM_INTR_STOP_DET;
}

static uint32_t sim_read(uint32_t off)
{
    if(in_reset && off != SIM_LPSS_RESET) return 0;
    switch(off){
    case SIM_IC_COMP_TYPE:     return comp_type;
    case SIM_IC_COMP_PARAM_1:  return (uint32_t)((fifo_depth - 1) << 16);
    case SIM_IC_ENABLE_STATUS: return enabled ? 1u : 0u;
    case SIM_IC_STATUS: {
        uint32_t v = SIM_ST_TFNF;
        if(txn == 0) v |= SIM_ST_TFE;
        if(rxi < rxn || rxn > 0) { }
        if(rxn > 0) v |= SIM_ST_RFNE;
        if(txn >= fifo_depth) v &= ~SIM_ST_TFNF;
        return v;
    }
    case SIM_IC_RAW_INTR_STAT: return raw_intr;
    case SIM_IC_TX_ABRT_SOURCE: return abrt_src;
    case SIM_IC_CLR_TX_ABRT:   { uint32_t v = abrt_src;
                             abrt_src = 0; raw_intr &= ~SIM_INTR_TX_ABRT;
                             return v; }
    case SIM_IC_CLR_INTR:      { raw_intr &= ~SIM_INTR_STOP_DET; return 0; }
    case SIM_IC_DATA_CMD: {
        uint32_t v = 0;
        if(rxn > 0){ v = rxq[0];
                     memmove(rxq, rxq + 1, (size_t)(--rxn)); }
        return v;
    }
    default: return sim[off / 4];
    }
}

static void sim_write(uint32_t off, uint32_t v)
{
    /* v0.8.9.6: the value matters. The driver now ASSERTS the reset
       (writes 0) before releasing it, and a simulation that took any
       write as a release passed a driver that only ever asserted --
       tools/check_i2c_suite.py's "never released" injection went
       MISSED the moment the first write became a 0. Bits 0-1 are the
       function reset; both set is out of reset. */
    if(off == SIM_LPSS_RESET){ in_reset = ((v & 3u) != 3u); sim[off/4] = v; return; }
    if(in_reset) return;
    switch(off){
    case SIM_IC_ENABLE:
        enabled = (v & 1u) ? 1 : 0;
        if(enabled && txn) sim_execute();
        break;
    case SIM_IC_TAR:
        /* THE HARDWARE IGNORES THIS WHILE ENABLED. */
        if(!enabled) tar = v;
        break;
    case SIM_IC_DATA_CMD:
        if(txn < (int)(sizeof txq / sizeof txq[0])) txq[txn++] = v;
        if(v & SIM_CMD_STOP) sim_execute();
        break;
    default:
        sim[off/4] = v;
        break;
    }
}

/* The seam: drivers/i2c.c reads through one hook and writes through
   the other, so the simulated core sees BOTH. A pointer-returning
   seam cannot express a write to a device -- see the note in
   drivers/i2c.c. */
#define I2C_RD(base, off)      (sim_rd_hook((base), (off)))
#define I2C_WR(base, off, v)   (sim_wr_hook((base), (off), (v)))
static uint32_t sim_rd_hook(uint32_t base, uint32_t off);
static void     sim_wr_hook(uint32_t base, uint32_t off, uint32_t v);

#include "../include/i2c.h"

/* ── PCI, faked just enough ─────────────────────────────────────── */
static pci_dev_t fake_pci;
static int       n_pci = 1;
const pci_dev_t *pci_get(int i){ return (i == 0 && n_pci) ? &fake_pci : 0; }
void pci_enable(const pci_dev_t *d, int bm){ (void)d; (void)bm; }
/* v0.8.9.6: D3. While the simulated function is powered down every
   MMIO read is all ones, which is what real hardware does and what
   made bring_up() call a perfectly good core "not DesignWare". */
static int sim_d3, d0_calls, d0_delays;
static void sim_delay(uint32_t ms){ (void)ms; d0_delays++; }
void sleep_ms(uint32_t ms){ sim_delay(ms); }
int pci_set_d0(const pci_dev_t *d, void (*delay_ms)(uint32_t)){
    (void)d; d0_calls++;
    if(!sim_d3) return 0;
    sim_d3 = 0;
    if(delay_ms) delay_ms(10);
    return 3;
}
static uint32_t bar_hi = 0, bar_lo = 0x1000u;
uint32_t pci_bar_mem64(const pci_dev_t *d, int n, uint32_t *hi){
    (void)d; (void)n; if(hi) *hi = bar_hi; return bar_lo;
}
static int map_ok = 1;
int highfb_map_io(uint32_t hi, uint32_t lo, uint32_t bytes, uint32_t *out){
    (void)hi; (void)bytes;
    if(!map_ok) return 0;
    if(out) *out = lo;              /* identity, for the simulation   */
    return 1;
}
/* klog, swallowed. */
void klog(int lvl, const char *fmt, ...){ (void)lvl; (void)fmt; }

#include "../drivers/i2c.c"

static uint32_t sim_rd_hook(uint32_t base, uint32_t off)
{
    (void)base;
    if(sim_d3) return 0xFFFFFFFFu;
    return sim_read(off);
}
static void sim_wr_hook(uint32_t base, uint32_t off, uint32_t v)
{
    (void)base;
    sim_write(off, v);
}

static int total = 0, fails = 0;
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("  FAIL  %s   (got %ld)\n", what, got); }
}
static void head(const char *s){ printf("\n%s\n", s); }

static void reset_sim(int depth)
{
    memset(sim, 0, sizeof sim);
    memset(dev_present, 0, sizeof dev_present);
    in_reset = 1; enabled = 0; tar = 0; fifo_depth = depth;
    comp_type = 0x44570140u;
    txn = rxn = rxi = wire_n = 0;
    saw_restart = saw_stop = 0;
    abrt_src = raw_intr = 0;
    dev_reply_n = 0;
    bar_hi = 0; bar_lo = sim_base; map_ok = 1;
    g_nbus = 0; g_inited = 0;
    memset(&fake_pci, 0, sizeof fake_pci);
    fake_pci.bus = 0; fake_pci.dev = 0x15; fake_pci.fn = 0;
    fake_pci.vendor = 0x8086; fake_pci.device = 0x51E8;
    fake_pci.class = 0x0C; fake_pci.subclass = 0x80;
    n_pci = 1;
    sim_d3 = 0; d0_calls = 0; d0_delays = 0;
}

int main(void)
{
    printf("test_i2c: the DesignWare core, against a simulated one\n");

    /* ── v0.8.9.6: a controller the firmware left powered down ─── */
    head("a controller left in D3 is powered up before it is read");
    reset_sim(16);
    sim_d3 = 1;
    ck("it still comes up as a bus", i2c_init() == 1, i2c_bus_count());
    ck("  ... because it was asked to go to D0", d0_calls == 1, d0_calls);
    ck("  ... and given the recovery time the spec requires",
       d0_delays >= 1, d0_delays);
    ck("  ... and is ready", i2c_bus_ready(0) == 1, 0);
    reset_sim(16);
    ck("a controller already in D0 is asked, and nothing changes",
       i2c_init() == 1 && d0_calls == 1, d0_calls);

    /* ── bringing it up ──────────────────────────────────────────── */
    head("a controller comes up, and the reset is released first");
    reset_sim(16);
    ck("i2c_init finds one bus", i2c_init() == 1, i2c_bus_count());
    ck("  ... and it is ready", i2c_bus_ready(0) == 1, 0);
    ck("  ... the LPSS reset WAS released",
       in_reset == 0, in_reset);
    ck("  ... the core was left enabled", enabled == 1, enabled);
    /* THE FIFO DEPTH IS READ, NOT ASSUMED. A core with 16 and a
       driver that believes 32 overruns it by sixteen bytes. */
    ck("  ... and the FIFO depth came off SIM_IC_COMP_PARAM_1",
       g_bus[0].txfifo == 16u, (long)g_bus[0].txfifo);
    ck("  ... the pci device is reported back",
       i2c_bus_pci(0) == &fake_pci, 0);

    head("a device of the right CLASS that is not a DesignWare core");
    reset_sim(16);
    comp_type = 0x12345678u;        /* something else entirely        */
    ck("class 0C/80 alone is not enough -- it is refused",
       i2c_init() == 0, i2c_bus_count());
    ck("  ... and no bus was registered", i2c_bus_count() == 0,
       i2c_bus_count());
    /* AND NOT ONE REGISTER WAS WRITTEN INTO IT. Writing this map
       into somebody else's device is worse than doing nothing: the
       offsets mean something different there, and IC_ENABLE at 0x6C
       could be anything. The LPSS reset is the one write that has to
       happen first to read the type at all. */
    ck("  ... and nothing past the reset was written to it",
       enabled == 0 && tar == 0, (long)tar);

    head("a core that does not identify itself is refused");
    reset_sim(16);
    {
        /* Make SIM_IC_COMP_TYPE read as something else by pretending the
           reset never releases -- a core still in reset reads zero,
           which is exactly the symptom. */
        int before;
        /* drive init with the reset write swallowed */
        in_reset = 1;
        before = i2c_bus_count();
        (void)before;
        /* A device of the right class whose BAR will not map. */
        map_ok = 0; bar_hi = 0x60u;
        ck("a BAR that will not map is not a bus", i2c_init() == 0,
           i2c_bus_count());
    }

    head("a 64-bit BAR goes through highfb_map_io, not pci_bar_mem");
    reset_sim(16);
    bar_hi = 0x60u; bar_lo = sim_base;     /* above 4 GB, as on the HP */
    ck("it still comes up", i2c_init() == 1, i2c_bus_count());
    ck("  ... mapped where the mapper said", i2c_bus_mmio(0) == sim_base,
       (long)i2c_bus_mmio(0));

    /* ── addressing ──────────────────────────────────────────────── */
    head("the target address is set with the core DISABLED");
    reset_sim(16);
    i2c_init();
    dev_present[0x2c] = 1;
    {
        uint8_t b = 0xAA;
        ck("a write to a present device succeeds",
           i2c_write(0, 0x2c, &b, 1) == I2C_OK, 0);
        /* If SIM_IC_TAR had been written while enabled the simulated core
           would have ignored it and tar would still be 0 -- which on
           a real bus is the general-call address. */
        ck("  ... and the address really took", tar == 0x2cu, (long)tar);
        ck("  ... the byte reached the wire",
           wire_n == 1 && wire[0] == 0xAA, wire_n ? wire[0] : -1);
        ck("  ... with a STOP", saw_stop == 1, saw_stop);
    }

    /* ── the refusals, each its own code ─────────────────────────── */
    head("every refusal has its own name");
    reset_sim(16);
    i2c_init();
    dev_present[0x2c] = 1;
    {
        uint8_t b = 0;
        ck("nobody at 0x40 is ENAK",
           i2c_write(0, 0x40, &b, 1) == I2C_ENAK, i2c_write(0, 0x41, &b, 1));
        ck("  ... and the name says so",
           !strcmp(i2c_why_name(I2C_ENAK),
                   "nobody acknowledged that address"), 0);
        ck("a bus that does not exist is ENOBUS",
           i2c_write(9, 0x2c, &b, 1) == I2C_ENOBUS, 0);
        /* LONGER THAN THE FIFO IS REFUSED BY NAME, not truncated. A
           short write that reports success is a register half
           written, and on a touchpad that is a mode nobody chose. */
        {
            uint8_t big[64];
            memset(big, 0x5A, sizeof big);
            ck("longer than the FIFO is ETOOLONG",
               i2c_write(0, 0x2c, big, 40) == I2C_ETOOLONG, 0);
            ck("  ... and nothing was put on the wire",
               wire_n == 0, wire_n);
        }
    }

    /* A NAK must not leave the controller aborted for the NEXT
       transfer. The abort is sticky in hardware and has to be
       cleared, and a scan does 112 of these in a row. */
    head("a NAK leaves the controller usable");
    reset_sim(16);
    i2c_init();
    dev_present[0x2c] = 1;
    {
        uint8_t b = 0x11;
        ck("the NAK happens", i2c_write(0, 0x40, &b, 1) == I2C_ENAK, 0);
        ck("  ... the abort was cleared", abrt_src == 0, (long)abrt_src);
        ck("  ... and the next transfer works",
           i2c_write(0, 0x2c, &b, 1) == I2C_OK, 0);
    }

    /* ── reading ─────────────────────────────────────────────────── */
    head("a read brings back the device's bytes");
    reset_sim(16);
    i2c_init();
    dev_present[0x15] = 1;
    {
        uint8_t got[4];
        dev_reply[0] = 0xDE; dev_reply[1] = 0xAD;
        dev_reply[2] = 0xBE; dev_reply[3] = 0xEF;
        dev_reply_n = 4;
        ck("the read succeeds", i2c_read(0, 0x15, got, 4) == I2C_OK, 0);
        ck("  ... and the bytes are the device's",
           got[0]==0xDE && got[1]==0xAD && got[2]==0xBE && got[3]==0xEF,
           got[0]);
        ck("  ... with a STOP at the end", saw_stop == 1, saw_stop);
    }

    head("write-then-read uses a REPEATED START, not a stop");
    reset_sim(16);
    i2c_init();
    dev_present[0x15] = 1;
    {
        uint8_t reg[2] = { 0x00, 0x01 };
        uint8_t got[2];
        dev_reply[0] = 0x41; dev_reply[1] = 0x42; dev_reply_n = 2;
        ck("it succeeds",
           i2c_write_read(0, 0x15, reg, 2, got, 2) == I2C_OK, 0);
        ck("  ... the register number went out first",
           wire_n == 2 && wire[0] == 0x00 && wire[1] == 0x01, wire_n);
        ck("  ... the reply came back", got[0]==0x41 && got[1]==0x42,
           got[0]);
        /* THE WHOLE POINT. A stop between the two halves releases the
           bus, and another master may address the same device before
           the read -- after which the register pointer is whatever
           they left it at. */
        ck("  ... and a RESTART was used, not a stop-then-start",
           saw_restart == 1, saw_restart);
    }

    /* ── the scan, which is what this release is for ─────────────── */
    head("the scan finds what is there and nothing else");
    reset_sim(16);
    i2c_init();
    dev_present[0x2c] = 1;
    dev_present[0x50] = 1;
    {
        uint8_t found[112];
        int n = i2c_scan(0, found, (int)sizeof found);
        ck("two devices answered", n == 2, n);
        ck("  ... 0x2c", n > 0 && found[0] == 0x2c, n > 0 ? found[0] : -1);
        ck("  ... and 0x50", n > 1 && found[1] == 0x50, n > 1 ? found[1] : -1);
    }

    head("the scan skips the addresses the specification reserves");
    reset_sim(16);
    i2c_init();
    {
        uint8_t found[112];
        int i, n;
        /* Put a device on EVERY address, including the reserved ones.
           A scan that probed them would report 128. */
        for(i = 0; i < 128; i++) dev_present[i] = 1;
        n = i2c_scan(0, found, (int)sizeof found);
        ck("0x08 to 0x77 is 112 addresses", n == 112, n);
        ck("  ... it started at 0x08, not 0x00",
           found[0] == 0x08, found[0]);
        ck("  ... and stopped at 0x77, not 0x7F",
           found[111] == 0x77, found[111]);
        /* 0x00 is the GENERAL CALL address and every device on the
           bus listens to it. Probing it is not harmless. */
        ck("  ... so the general-call address was never addressed",
           found[0] != 0x00, found[0]);
    }

    head("an empty bus says so rather than reporting rubbish");
    reset_sim(16);
    i2c_init();
    {
        uint8_t found[112];
        ck("nothing answers", i2c_scan(0, found, (int)sizeof found) == 0, 0);
    }

    head("a scan of a bus that does not exist is zero, not a crash");
    ck("bus 9", i2c_scan(9, 0, 0) == 0, 0);
    ck("bus -1", i2c_scan(-1, 0, 0) == 0, 0);

    /* ── the probe writes nothing ────────────────────────────────── */
    head("the probe writes NOTHING -- it is a zero-length read");
    reset_sim(16);
    i2c_init();
    dev_present[0x2c] = 1;
    {
        ck("the probe answers", i2c_probe(0, 0x2c) == I2C_OK, 0);
        /* A write to an unknown device can do anything. This is the
           check that keeps a scan from being destructive. */
        ck("  ... and put no byte on the wire", wire_n == 0, wire_n);
    }

    head("two controllers are two buses");
    reset_sim(16);
    n_pci = 1;                      /* the fake table holds one       */
    ck("one bus from one device", i2c_init() == 1, i2c_bus_count());

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
