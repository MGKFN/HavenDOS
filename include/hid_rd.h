#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/hid_rd.h -- the HID report descriptor, parsed

   usb/hid.c drives a keyboard and a mouse in BOOT PROTOCOL, and its
   header is honest about what that costs:

       "a device whose subclass is 0 -- which is legal, and which some
        keyboards are -- is not driven by this file. The report-
        descriptor parser that would fix that is real work and is not
        in this release."

   This is that parser, and it is here now for a second reason that
   matters more: **I2C-HID HAS NO BOOT PROTOCOL.** A trackpad on the
   HP's Serial-IO bus describes itself and only itself. Without a
   descriptor parser there is no way to know which bits of its report
   are X, so there is no trackpad, however well drivers/i2c.c talks to
   it.

   ── WHAT IT PRODUCES ────────────────────────────────────────────────

   A flat list of FIELDS: usage page, usage, bit offset within the
   report, bit width, signedness bounds, and whether the value is
   relative. That is all a pointer driver needs, and it is the form in
   which the question "where is X in this report" has an answer.

   It then resolves the handful of fields a pointing device is made
   of -- X, Y, wheel, up to five buttons, and the contact count a
   touchpad carries -- so that a caller does not walk the list hunting
   for usage 0x30 itself.

   ── WHAT IT DELIBERATELY DOES NOT DO ────────────────────────────────

   - No collection semantics beyond counting depth. A Physical
     Collection inside an Application Collection changes nothing about
     where the bits are, and the drivers here do not care which
     collection a field came from.
   - No units, no exponents. A touchpad's X in millimetres and the
     same X in counts occupy the same bits.
   - No Output or Feature reports. They are parsed far enough to keep
     the Input bit offsets right -- each report type has its own
     running offset, and conflating them is how every field after the
     first Feature item ends up at the wrong place -- and then
     dropped.
   - An ARRAY becomes one field PER SLOT, each carrying the usage
     range it selects from and flagged `array`. A keyboard's six-key
     rollover is six fields, which is six hrd_field_value() calls.
     The first version made it one field spanning all six slots and
     that was simply unreadable: hrd_field_value() tops out at 32
     bits, so the one thing a caller would want from it could not be
     done. usb/hid.c's keyboard path still uses boot protocol where
     it can.

   ── WHAT IT REFUSES ─────────────────────────────────────────────────

   A descriptor is a stream from a device, which means it is
   untrusted input. Every refusal is a structure this cannot
   represent, and none of them are "mostly parse it anyway":

     - an item whose data runs past the end of the descriptor
     - Report Size or Report Count of zero, or a product that would
       put a field past the end of the largest report HID allows
     - more fields than HRD_MAX_FIELDS
     - Pop without Push, or Push nested deeper than HRD_MAX_PUSH
     - End Collection without a matching Collection
     - a long item (prefix 0xFE), which is legal and which nothing
       real uses: it is refused rather than half-skipped
   ════════════════════════════════════════════════════════════════════ */

#define HRD_MAX_FIELDS   48
#define HRD_MAX_PUSH      4
#define HRD_MAX_DEPTH    16
/* A HID report cannot exceed 65535 bytes; nothing here needs more
   than one, and a field claiming to live past this is a descriptor
   that has gone wrong rather than a device with a big report. */
#define HRD_MAX_BITS   (8 * 1024)

/* Usage pages and usages, the ones this resolves. */
#define HRD_PAGE_DESKTOP   0x01
#define HRD_PAGE_BUTTON    0x09
#define HRD_PAGE_DIGITIZER 0x0D
#define HRD_USAGE_X        0x30
#define HRD_USAGE_Y        0x31
#define HRD_USAGE_WHEEL    0x38
#define HRD_USAGE_CONTACTS 0x54   /* digitizer: Contact Count          */
#define HRD_USAGE_TIP      0x42   /* digitizer: Tip Switch             */

#define HRD_OK          0
#define HRD_E_PARAM   (-1)
#define HRD_E_TRUNC   (-2)   /* an item runs past the end             */
#define HRD_E_ITEM    (-3)   /* an item this parser will not accept   */
#define HRD_E_FULL    (-4)   /* more fields than it can hold          */
#define HRD_E_NEST    (-5)   /* push/pop or collection mismatch       */
#define HRD_E_RANGE   (-6)   /* a field outside any sane report       */
#define HRD_E_EMPTY   (-7)   /* nothing usable in it                  */

typedef struct {
    uint16_t usage_page;
    uint16_t usage;
    uint16_t usage_max;      /* for an array field; == usage otherwise */
    uint32_t bit_offset;     /* from the start of the report's DATA,
                                i.e. after the report ID byte          */
    uint16_t bit_size;
    uint8_t  report_id;      /* 0 when the descriptor declares none    */
    uint8_t  relative;       /* 1 if the value is a delta              */
    uint8_t  array;          /* 1 if this is one slot of an ARRAY: the
                                value is a usage in usage..usage_max,
                                not a quantity                         */
    int32_t  logical_min;
    int32_t  logical_max;
} hrd_field_t;

typedef struct {
    hrd_field_t field[HRD_MAX_FIELDS];
    int      nfields;
    int      has_report_ids;

    /* Resolved, as indices into field[] -- or -1. The FIRST match
       wins, because a touchpad that also reports a mouse collection
       lists the one it wants a driver to use first. */
    int      i_x, i_y, i_wheel, i_contacts, i_tip;
    int      i_button[5];
    int      nbuttons;

    /* The input report length in BITS for the report id the pointer
       fields live in, so a caller can tell a short report from a
       report for a different id. */
    uint32_t input_bits;
    uint8_t  pointer_report_id;
} hrd_t;

int hrd_parse(const uint8_t *desc, uint32_t len, hrd_t *out);

/* Pull one field out of a report, sign-extended according to its
   logical minimum. `report` EXCLUDES the report-ID byte when the
   descriptor declares ids -- the caller strips it, because only the
   caller knows whether the transport delivered one. Returns HRD_OK or
   HRD_E_RANGE if the field is not inside `len` bytes. */
int hrd_field_value(const hrd_field_t *f, const uint8_t *report,
                    uint32_t len, int32_t *out);

const char *hrd_strerror(int code);
