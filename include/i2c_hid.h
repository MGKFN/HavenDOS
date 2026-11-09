#pragma once
#include "types.h"
#include "hid_rd.h"

/* ════════════════════════════════════════════════════════════════════
   include/i2c_hid.h -- a trackpad, without an ACPI interpreter

   drivers/i2c.c talks to the HP's Serial-IO controllers and its
   header states the blocker plainly:

       "A device on an I2C bus has no enumeration. There is no
        descriptor to ask for and no vendor ID to read: you must
        already know the seven-bit address. On a PC that address lives
        in ACPI, in the device's _CRS, and reaching it means an AML
        interpreter."

   Half of that is solved there: i2c_scan() asks the bus which
   addresses answer, which is a question the bus can be asked
   directly. This file solves the other half, and the way it does it
   is the point of the whole design.

   ── THE SECOND ACPI OBJECT, AND WHY IT IS NOT NEEDED EITHER ─────────

   I2C-HID needs one more number ACPI normally supplies: the register
   at which the HID descriptor lives. Windows gets it from the
   device's _DSM method. There are, in practice, about four values in
   use -- 0x01 on nearly everything, 0x20 on some Elan parts.

   Guessing would be unacceptable. **It is not a guess, because the
   descriptor validates itself.** An HID descriptor's first four
   bytes are wHIDDescLength and bcdVersion, and for every device that
   implements the specification they are 0x001E and 0x0100 -- thirty
   and version one. Reading two bytes at a register and finding that
   32-bit pattern is a positive identification, not a hope: a wrong
   register returns the top of some other register file, or NAKs, and
   the odds of it spelling 1E 00 00 01 are one in four billion before
   you even count the twelve further fields that then have to be
   internally consistent.

   So: for every address that answered, try the handful of known
   descriptor registers, and accept the one whose descriptor is a
   descriptor. That is a PROBE WITH A CHECKABLE SIGNATURE, and it
   needs no AML interpreter, no DSDT walk and no RSDP.

   ── WHAT IS STILL MISSING, STATED HONESTLY ──────────────────────────

   - NO INTERRUPT. A touchpad asserts a GPIO when it has a report and
     this kernel has no GPIO driver. This polls, which costs a
     transfer per poll and means a report can be missed if nothing
     polls for long enough. The device holds its most recent report,
     so what is lost is intermediate motion, not the finger's
     position.
   - NO MULTITOUCH GESTURES. The contact count and tip switch are
     read, and one finger moves the pointer. Two-finger scrolling is
     a thing this could do and does not yet.
   - NOT CONFIRMED ON HARDWARE. Every byte of the protocol here is
     from the specification and every path is tested against a mock
     device that implements it. Whether the HP's trackpad is at one of
     the registers tried is a question only the HP can answer, and the
     `i2chid` command prints exactly what it found at every address so
     that a photograph of one screen settles it.

   ── WHY A TOUCHPAD'S ABSOLUTE COORDINATES BECOME DELTAS ─────────────

   A touchpad reports where the finger IS. A pointer moves by how far
   the finger MOVED. Mapping absolute position straight onto the
   screen is what a drawing tablet does and it feels wrong on a
   laptop: lifting and replacing a finger would teleport the cursor.

   So the first report of a touch records the position and moves
   nothing; each later report while the tip is still down moves by the
   difference. A lift clears the record. That is three lines of state
   and it is the difference between a trackpad and a tablet.
   ════════════════════════════════════════════════════════════════════ */

#define IHID_OK          0
#define IHID_E_PARAM   (-1)
#define IHID_E_IO      (-2)   /* the bus refused the transfer         */
#define IHID_E_SIG     (-3)   /* no HID descriptor signature there    */
#define IHID_E_FIELDS  (-4)   /* a descriptor that contradicts itself */
#define IHID_E_DESC    (-5)   /* the report descriptor would not parse */
#define IHID_E_NOPTR   (-6)   /* parsed, but there is no X and Y in it */
#define IHID_E_FULL    (-7)   /* more devices than it can hold        */
#define IHID_E_SHORT   (-8)   /* a report shorter than its own header */

#define IHID_MAX_DEV      4
#define IHID_DESC_LEN     30
#define IHID_RD_MAX     1024   /* a report descriptor bigger than this
                                  is a device this does not drive      */

/* The registers worth trying, in the order real hardware uses them.
   0x01 is what nearly everything reports; 0x20 is the one that is not
   0x01 often enough to matter. */
#define IHID_CAND_REGS { 0x0001, 0x0020, 0x0000, 0x0002, 0x0005, 0x0010 }
#define IHID_NCAND 6

/* Commands, from the I2C-HID specification. */
#define IHID_CMD_RESET      0x01
#define IHID_CMD_GET_REPORT 0x02
#define IHID_CMD_SET_POWER  0x08
#define IHID_POWER_ON       0x00
#define IHID_POWER_SLEEP    0x01

/* The bus, behind function pointers -- so that a suite can implement
   a device that follows the specification and every path here is
   exercised without an HP on the desk. drivers/i2c.c's three
   functions have exactly these shapes. */
typedef struct {
    int (*wr)(void *ctx, int bus, uint8_t addr,
              const uint8_t *buf, uint32_t len);
    int (*rd)(void *ctx, int bus, uint8_t addr,
              uint8_t *buf, uint32_t len);
    int (*wrrd)(void *ctx, int bus, uint8_t addr,
                const uint8_t *w, uint32_t wl, uint8_t *r, uint32_t rl);
    int (*probe)(void *ctx, int bus, uint8_t addr);
} ihid_ops_t;

typedef struct {
    int      bus;
    uint8_t  addr;
    uint16_t desc_reg;

    /* Straight out of the 30-byte HID descriptor. */
    uint16_t hid_desc_len, bcd_version;
    uint16_t report_desc_len, report_desc_reg;
    uint16_t input_reg, max_input_len;
    uint16_t output_reg, max_output_len;
    uint16_t command_reg, data_reg;
    uint16_t vid, pid, ver;

    hrd_t    rd;              /* the parsed report descriptor         */
    int      is_pointer;      /* it has an X and a Y                  */

    /* Absolute-to-relative state. See the header note. */
    int      touching;
    int32_t  last_x, last_y;
    int      last_buttons;
} ihid_dev_t;

/* Validate and unpack the 30-byte HID descriptor. IHID_E_SIG when the
   signature is not there, IHID_E_FIELDS when it is but the fields
   contradict each other. */
int ihid_desc_parse(const uint8_t *d, uint32_t len, ihid_dev_t *out);

/* Walk one bus: every address that answers, every candidate register,
   accept what validates. Returns how many devices were filled in, or
   a negative code. Does NOT fetch report descriptors -- that needs a
   buffer and is a separate step, so that discovery can be reported
   even when a descriptor will not parse. */
int ihid_find(const ihid_ops_t *ops, void *ctx, int bus,
              ihid_dev_t *out, int max);

/* Fetch and parse the report descriptor into d->rd, and resolve
   d->is_pointer. `buf` is scratch of at least d->report_desc_len. */
int ihid_bring_up(const ihid_ops_t *ops, void *ctx, ihid_dev_t *d,
                  uint8_t *buf, uint32_t cap);

/* Power on, and reset. Separate from bring-up because a device that
   will not reset is still worth reporting as found. */
int ihid_set_power(const ihid_ops_t *ops, void *ctx,
                   const ihid_dev_t *d, int on);
int ihid_reset(const ihid_ops_t *ops, void *ctx, const ihid_dev_t *d);

/* Read one input report. Returns its length in bytes (0 when the
   device has nothing to say, which it signals with a length of zero)
   or a negative code. `buf` must hold max_input_len. */
int ihid_read_report(const ihid_ops_t *ops, void *ctx,
                     const ihid_dev_t *d, uint8_t *buf, uint32_t cap);

/* Turn one input report into pointer movement. *dx and *dy are
   deltas, *buttons is a PS/2-style mask (1 left, 2 right, 4 middle).
   Returns IHID_OK and sets *have to 1 when there is movement or a
   button change to act on, 0 when the report was for something else
   or carried no touch. */
int ihid_decode(ihid_dev_t *d, const uint8_t *report, uint32_t len,
                int *dx, int *dy, int *buttons, int *have);

const char *ihid_strerror(int code);

/* ── the live driver ────────────────────────────────────────────────── */
int  i2c_hid_init(void);          /* scan every bus; returns devices found */
int  i2c_hid_count(void);
const ihid_dev_t *i2c_hid_dev(int i);
void i2c_hid_poll(void);          /* feeds drivers/mouse.c                 */
