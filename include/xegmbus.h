#pragma once
#include "types.h"
#include "xedisp.h"
#include "edid.h"

/* ════════════════════════════════════════════════════════════════════
   include/xegmbus.h -- reading a monitor's EDID over GMBUS.  v0.9.1.

   THE FIRST RELEASE IN THIS SERIES THAT WRITES -- AND WHAT IT WRITES.

   GMBUS is the display engine's own I2C controller, in the south
   display block. Six registers, 0xC5100-0xC5120 in BAR0. An HDMI
   monitor answers EDID at I2C address 0x50 on its DDC wires, which on
   the HP's DDI B are GMBUS pin 2 (0.9.0's `gfx vbt`, read off the
   machine).

   EVERY WRITE IN THIS FILE IS TO THOSE SIX REGISTERS. gmbus_wr() is
   the only place a write happens, it refuses any offset outside
   0xC5100-0xC5120 and counts the refusal, and
   tools/check_gfx_readonly.py fails the build if a write appears
   anywhere else in the file. Nothing that scans out an image -- no
   pipe, plane, transcoder, PLL or power well -- is reachable from
   here. The worst a wrong write can do is confuse the I2C controller
   until the next boot.

   The sequence is i915's (intel_gmbus.c: do_gmbus_xfer, the index
   read), including the retry on a first NAK and the three-write
   cleanup after one. The clock-gating workaround i915 applies around
   a transfer is for older PCHs (SPT, CNP) and not for this one, so it
   is not here -- that would have been a write outside the six.

   ── READ-BACK ──────────────────────────────────────────────────────

   GMBUS0 (pin and rate) is read back after every write and must hold
   what was written. GMBUS1 is a COMMAND register -- its SW_RDY bit is
   consumed by the hardware -- so its read-back is logged, not
   compared; a comparison there would fail on a controller that is
   working correctly.
   ════════════════════════════════════════════════════════════════════ */

#define GMBUS_REG_FIRST   0xC5100u
#define GMBUS_REG_LAST    0xC5120u

#define GMBUS_OK          0
#define GMBUS_ENAK        1   /* nothing answered at the address      */
#define GMBUS_ETIMEOUT    2   /* the controller never said ready      */
#define GMBUS_EPIN        3   /* not a GMBUS pin on this platform     */
#define GMBUS_ESTUCK      4   /* GMBUS0 did not hold the pin written  */
#define GMBUS_EBUSY       5   /* the bus was active before we began   */
#define GMBUS_ENODEV      6   /* no display engine to talk through    */
#define GMBUS_EREASONS    7
const char *gmbus_why_name(int why);

/* One register write, with what was read back. */
typedef struct { uint32_t off, val, back; } gmbus_wlog_t;

#define GMBUS_WLOG_MAX 48
typedef struct {
    int       why;
    int       pin;
    uint8_t   addr;
    uint32_t  status;             /* GMBUS2 when it finished or failed */
    uint32_t  writes, refused;    /* refused: outside the six registers */
    uint32_t  data_reads;         /* GMBUS3 reads                      */
    int       retried;            /* the one i915-style NAK retry      */
    int       nwlog;
    gmbus_wlog_t wlog[GMBUS_WLOG_MAX];
} gmbus_trace_t;

/* Valid GMBUS pins on ADL-P (i915 gmbus_pins_icp, reached through
   adlp_ddc_pin_map): 1 DDI A, 2 DDI B, 9-12 TC1-TC4. */
int gmbus_pin_valid(int pin);

/* Read `len` bytes (1..256) at `index` from I2C address `addr` on
   `pin`, as one index transfer. */
int gmbus_index_read(const gfx_dev_t *dev, int pin, uint8_t addr,
                     uint8_t index, uint8_t *buf, uint32_t len,
                     gmbus_trace_t *tr);

/* Block 0 and, if it announces one, block 1. *got is 0, 128 or 256. */
int gmbus_read_edid(const gfx_dev_t *dev, int pin, uint8_t *buf,
                    uint32_t *got, gmbus_trace_t *tr);

/* The `gfx edid` screen: what the transfer did (every write, with its
   read-back), what the monitor said, and the raw bytes. In
   drivers/xedisp.c, which stays read-only: this only formats. */
void gfx_report_edid(const gmbus_trace_t *tr, const edid_t *e,
                     const uint8_t *raw, uint32_t rawlen, gfx_emit_t emit);
