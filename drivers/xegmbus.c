/* ════════════════════════════════════════════════════════════════════
   drivers/xegmbus.c -- EDID over GMBUS.  v0.9.1.

   include/xegmbus.h carries the reasoning: what GMBUS is, why every
   write here is confined to six registers, and why GMBUS1's read-back
   is logged rather than compared.

   ── THE SEAMS ───────────────────────────────────────────────────────

   GFX_RD(base, off)     a register read
   GFX_WR(base, off, v)  a register write -- used in gmbus_wr() and
                         NOWHERE ELSE in this file; the build checks it

   tests/test_edid.c replaces both with a model GMBUS controller and a
   model monitor behind it.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/xedisp.h"
#include "../include/xegmbus.h"

#ifndef GFX_RD
#define GFX_RD(base, off) \
    (*(volatile uint32_t *)(uintptr_t)((base) + (off)))
#endif
#ifndef GFX_WR
#define GFX_WR(base, off, v) \
    (*(volatile uint32_t *)(uintptr_t)((base) + (off)) = (uint32_t)(v))
#endif

/* intel_gmbus_regs.h, with __GMBUS_MMIO_BASE = PCH_DISPLAY_BASE
   (0xC0000) on a PCH platform. */
#define G0  0xC5100u     /* GMBUS0: pin, rate                          */
#define G1  0xC5104u     /* GMBUS1: command                            */
#define G2  0xC5108u     /* GMBUS2: status                             */
#define G3  0xC510Cu     /* GMBUS3: data, four bytes at a time         */
#define G4  0xC5110u     /* GMBUS4: interrupt mask (never written)     */
#define G5  0xC5120u     /* GMBUS5: two-byte index (never written)     */

#define G0_RATE_100KHZ    (0u << 8)
#define G1_SW_CLR_INT     (1u << 31)
#define G1_SW_RDY         (1u << 30)
#define G1_CYCLE_WAIT     (1u << 25)
#define G1_CYCLE_INDEX    (2u << 25)
#define G1_CYCLE_STOP     (4u << 25)
#define G1_COUNT_SHIFT    16
#define G1_INDEX_SHIFT    8
#define G1_ADDR_SHIFT     1
#define G1_READ           (1u << 0)
#define G2_INUSE          (1u << 15)
#define G2_HW_WAIT_PHASE  (1u << 14)
#define G2_HW_RDY         (1u << 11)
#define G2_SATOER         (1u << 10)   /* the target did not ACK        */
#define G2_ACTIVE         (1u << 9)

/* Polls, not time: every wait here is a bounded spin on an uncached
   register, which on the HP is about a microsecond a read. 100 kHz I2C
   moves four bytes in under half a millisecond, so this is two orders
   of magnitude of margin and still well under two seconds if the
   controller is dead. */
#define GM_SPIN 2000000u

const char *gmbus_why_name(int why)
{
    switch(why){
    case GMBUS_OK:       return "ok";
    case GMBUS_ENAK:     return "nothing answered at that address";
    case GMBUS_ETIMEOUT: return "the controller never said it was ready";
    case GMBUS_EPIN:     return "not a GMBUS pin on Alder Lake-P";
    case GMBUS_ESTUCK:   return "GMBUS0 did not keep the pin written to it";
    case GMBUS_EBUSY:    return "the bus was already busy";
    case GMBUS_ENODEV:   return "no display engine was opened";
    default:             return "unknown";
    }
}

int gmbus_pin_valid(int pin)
{
    return pin == 1 || pin == 2 || (pin >= 9 && pin <= 12);
}

static uint32_t gm_rd(const gfx_dev_t *dev, uint32_t off)
{
    return GFX_RD(dev->mmio, off);
}

/* THE ONLY WRITE. Anything outside the six GMBUS registers is refused
   and counted, and never reaches the bus. Every write is logged with
   what the register read back afterwards. */
static uint32_t gmbus_wr(const gfx_dev_t *dev, gmbus_trace_t *tr,
                         uint32_t off, uint32_t v)
{
    uint32_t back;
    if(off < GMBUS_REG_FIRST || off > GMBUS_REG_LAST || (off & 3u)){
        tr->refused++;
        return 0xFFFFFFFFu;
    }
    GFX_WR(dev->mmio, off, v);
    back = gm_rd(dev, off);
    tr->writes++;
    if(tr->nwlog < GMBUS_WLOG_MAX){
        tr->wlog[tr->nwlog].off = off;
        tr->wlog[tr->nwlog].val = v;
        tr->wlog[tr->nwlog].back = back;
        tr->nwlog++;
    }
    return back;
}

/* Wait until any bit of `want` is set in GMBUS2, or SATOER. Returns
   GMBUS_OK, GMBUS_ENAK or GMBUS_ETIMEOUT. */
static int gm_wait_for(const gfx_dev_t *dev, gmbus_trace_t *tr, uint32_t want)
{
    uint32_t n = GM_SPIN, s = 0;
    while(n--){
        s = gm_rd(dev, G2);
        if(s & G2_SATOER){ tr->status = s; return GMBUS_ENAK; }
        if(s & want){ tr->status = s; return GMBUS_OK; }
    }
    tr->status = s;
    return GMBUS_ETIMEOUT;
}

static int gm_wait_idle(const gfx_dev_t *dev, gmbus_trace_t *tr)
{
    uint32_t n = GM_SPIN, s = 0;
    while(n--){
        s = gm_rd(dev, G2);
        if(!(s & G2_ACTIVE)){ tr->status = s; return GMBUS_OK; }
    }
    tr->status = s;
    return GMBUS_ETIMEOUT;
}

/* One attempt: i915 do_gmbus_xfer() with a single index read. */
static int gm_attempt(const gfx_dev_t *dev, int pin, uint8_t addr,
                   uint8_t index, uint8_t *buf, uint32_t len,
                   gmbus_trace_t *tr)
{
    uint32_t reg0 = (uint32_t)pin | G0_RATE_100KHZ, got = 0;
    int r;

    /* READ BACK: if the pin did not take, the transfer would run on
       whatever pin was there -- possibly the panel's. */
    if(gmbus_wr(dev, tr, G0, reg0) != reg0){
        gmbus_wr(dev, tr, G0, 0);
        return GMBUS_ESTUCK;
    }

    gmbus_wr(dev, tr, G1, G1_SW_RDY | G1_CYCLE_INDEX | G1_CYCLE_WAIT |
                          (len << G1_COUNT_SHIFT) |
                          ((uint32_t)index << G1_INDEX_SHIFT) |
                          ((uint32_t)addr << G1_ADDR_SHIFT) | G1_READ);

    while(got < len){
        uint32_t v, k;
        r = gm_wait_for(dev, tr, G2_HW_RDY);
        if(r != GMBUS_OK) goto fail;
        v = gm_rd(dev, G3);
        tr->data_reads++;
        for(k = 0; k < 4 && got < len; k++){
            buf[got++] = (uint8_t)(v & 0xFFu);
            v >>= 8;
        }
    }
    r = gm_wait_for(dev, tr, G2_HW_WAIT_PHASE);
    if(r != GMBUS_OK) goto fail;

    gmbus_wr(dev, tr, G1, G1_CYCLE_STOP | G1_SW_RDY);
    r = gm_wait_idle(dev, tr);
    gmbus_wr(dev, tr, G0, 0);              /* release the pin          */
    return r;

fail:
    if(r == GMBUS_ENAK){
        /* i915 clear_err: let the bus go idle, clear the latched
           interrupt, clear the command, release the pin. */
        gm_wait_idle(dev, tr);
        gmbus_wr(dev, tr, G1, G1_SW_CLR_INT);
        gmbus_wr(dev, tr, G1, 0);
        gmbus_wr(dev, tr, G0, 0);
        return GMBUS_ENAK;
    }
    /* i915 timeout: release the pin and give up. */
    gmbus_wr(dev, tr, G0, 0);
    return GMBUS_ETIMEOUT;
}

int gmbus_index_read(const gfx_dev_t *dev, int pin, uint8_t addr,
                     uint8_t index, uint8_t *buf, uint32_t len,
                     gmbus_trace_t *tr)
{
    int r;
    tr->pin = pin; tr->addr = addr;
    if(!dev || dev->why != GFX_OK || !dev->mmio) return tr->why = GMBUS_ENODEV;
    if(!gmbus_pin_valid(pin)) return tr->why = GMBUS_EPIN;
    if(!len || len > 256u) return tr->why = GMBUS_ETIMEOUT;

    /* Somebody else's transfer in flight -- the firmware's, say. Wait
       for it rather than trampling it; if it never ends, refuse. */
    if(gm_rd(dev, G2) & G2_ACTIVE){
        if(gm_wait_idle(dev, tr) != GMBUS_OK) return tr->why = GMBUS_EBUSY;
    }

    r = gm_attempt(dev, pin, addr, index, buf, len, tr);
    /* i915 retries once when the FIRST message is NAKed: some monitors
       miss the first address after waking up. */
    if(r == GMBUS_ENAK){
        tr->retried = 1;
        r = gm_attempt(dev, pin, addr, index, buf, len, tr);
    }
    return tr->why = r;
}

int gmbus_read_edid(const gfx_dev_t *dev, int pin, uint8_t *buf,
                    uint32_t *got, gmbus_trace_t *tr)
{
    int r;
    memset(tr, 0, sizeof *tr);
    *got = 0;
    r = gmbus_index_read(dev, pin, 0x50, 0, buf, 128, tr);
    if(r != GMBUS_OK) return r;
    *got = 128;
    /* Byte 126 counts the extensions. Block 1 is the second half of
       the same 256-byte segment, so it needs no segment pointer;
       blocks 2+ would, and no 1080p monitor has them. */
    if(buf[126] >= 1){
        r = gmbus_index_read(dev, pin, 0x50, 128, buf + 128, 128, tr);
        if(r == GMBUS_OK) *got = 256;
        else tr->why = r;              /* block 0 is still good */
    }
    return GMBUS_OK;
}
