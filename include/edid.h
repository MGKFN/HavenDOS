#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/edid.h -- what a monitor says about itself.  v0.9.1.

   EDID is 128-byte blocks a monitor answers with at I2C address 0x50
   on its DDC wires. Block 0 is the base: who made it, its name, and up
   to four 18-byte descriptors, the first of which is the PREFERRED
   mode -- the one 0.9.2 will program into pipe B. An HDMI monitor
   usually adds a CTA-861 extension (block 1) carrying more detailed
   timings, which is where a high-refresh mode like 144 Hz lives.

   drivers/edid.c is PURE: bytes in, a struct out. It never touches
   hardware; drivers/xegmbus.c is what fetches the bytes. Every field
   below is checked against VESA's published timings in tests/test_edid.c.
   ════════════════════════════════════════════════════════════════════ */

#define EDID_BLOCK      128
#define EDID_MAX_MODES  12

#define EDID_OK         0
#define EDID_ESHORT     1   /* fewer than 128 bytes                  */
#define EDID_EHEADER    2   /* not 00 FF FF FF FF FF FF 00           */
#define EDID_ESUM       3   /* the block's bytes do not sum to 0     */

const char *edid_why_name(int why);

typedef struct {
    uint32_t pclk_khz;
    uint16_t hactive, hblank, hso, hspw;   /* sync offset, pulse width */
    uint16_t vactive, vblank, vso, vspw;
    uint8_t  interlaced, hpos, vpos;       /* sync polarity, 1 = +    */
    uint8_t  block;                        /* which block it came from */
    uint32_t refresh_x100;                 /* Hz * 100                 */
} edid_mode_t;

typedef struct {
    int      why;
    char     mfg[4];            /* PNP ID, e.g. "GSM" for LG           */
    uint16_t product;
    uint32_t serial;
    uint8_t  week, version, revision;
    uint16_t year;
    uint8_t  extensions;        /* byte 126                            */
    char     name[14];          /* descriptor 0xFC, trimmed            */
    char     serial_text[14];   /* descriptor 0xFF                     */
    uint8_t  have_range;        /* descriptor 0xFD                     */
    uint16_t vmin, vmax;        /* Hz                                  */
    uint16_t hmin_khz, hmax_khz;
    uint16_t max_pclk_mhz;
    uint8_t  cta_blocks;        /* CTA-861 extensions parsed           */
    uint8_t  bad_ext;           /* extensions that failed the checksum */
    int      nmodes;            /* modes[0] is the preferred one       */
    edid_mode_t modes[EDID_MAX_MODES];
} edid_t;

/* Parse `len` bytes: block 0, then as many extension blocks as are
   present in the buffer AND announced in byte 126. Returns EDID_OK or
   the reason block 0 was refused. A bad extension does not refuse the
   base block; it is counted in bad_ext. */
int  edid_parse(const uint8_t *buf, uint32_t len, edid_t *out);

/* One 18-byte detailed timing descriptor. Returns 1 if it is a timing
   (pixel clock nonzero), 0 if it is a text/range descriptor. */
int  edid_dtd(const uint8_t *d, edid_mode_t *m);
