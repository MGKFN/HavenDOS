/* ================================================================
   tests/test_i2c_hid.c -- a trackpad, against a mock that follows
                           the specification
   ================================================================
   drivers/i2c_hid.c is the file that would make the HP's trackpad
   work, and it cannot be tested on the HP from here. So the bus is
   behind four function pointers and this suite implements a device
   that obeys the I2C-HID specification: a register file, a HID
   descriptor, a report descriptor, and input reports with their
   two-byte length header.

   ── WHAT THIS CAN AND CANNOT PROVE ──────────────────────────────────

   It proves the protocol: that discovery identifies a descriptor
   rather than guessing at one, that a descriptor which is not one is
   refused, that a report is decoded at the right bit offsets, and --
   the part that is all of the feel of a trackpad -- that absolute
   finger positions become sensible pointer movement across a lift
   and a replace.

   It cannot prove that the HP's trackpad is at one of the registers
   tried. Nothing here can. What makes that answerable is the log line
   the driver prints for every address that answered.

   ── THE CHECK THAT MATTERS MOST ─────────────────────────────────────

   "a finger lifted and put down somewhere else does not move the
   pointer". That is the whole difference between a trackpad and a
   drawing tablet, it is three lines of state, and it is the thing a
   person notices within one second of using it.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "types.h"
#include "hid_rd.h"
#include "i2c_hid.h"
#include "../usb/hid_rd.c"

/* drivers/i2c_hid.c pulls in the live driver, which needs the real
   i2c and klog. The protocol half is what is under test, so it is
   compiled with the live half's dependencies stubbed. */
#define I2C_OK 0
static int  i2c_write(int b, uint8_t a, const uint8_t *p, uint32_t l)
{ (void)b;(void)a;(void)p;(void)l; return -1; }
static int  i2c_read(int b, uint8_t a, uint8_t *p, uint32_t l)
{ (void)b;(void)a;(void)p;(void)l; return -1; }
static int  i2c_write_read(int b, uint8_t a, const uint8_t *w, uint32_t wl,
                           uint8_t *r, uint32_t rl)
{ (void)b;(void)a;(void)w;(void)wl;(void)r;(void)rl; return -1; }
static int  i2c_probe(int b, uint8_t a){ (void)b;(void)a; return -1; }
static int  i2c_bus_count(void){ return 0; }
static int  i2c_bus_ready(int b){ (void)b; return 0; }
static void klog(int lv, const char *f, ...){ (void)lv;(void)f; }
#define KLOG_INFO 0
#define KLOG_WARN 1
#define KLOG_OK   2
static void mouse_feed_i2c(int dx,int dy,int b){ (void)dx;(void)dy;(void)b; }
static void mouse_i2c_attach(void){ }
static uint32_t slept_ms; static int sleeps;
static void sleep_ms(uint32_t ms){ slept_ms += ms; sleeps++; }
#define I2C_H          /* keep the real include/i2c.h out */
#define KLOG_H
#include "../drivers/i2c_hid.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok    %s\n", what);
    else { printf("    FAIL  %s   got %ld\n", what, got); fails++; }
}

/* ════════════════════════════════════════════════════════════════
   THE MOCK DEVICE
   ════════════════════════════════════════════════════════════════ */

/* The same touchpad report descriptor tests/test_hid_rd.c judges
   against hid_parser: report 4, one contact, absolute 16-bit X and Y,
   a tip switch, a contact count and a button. */
static const uint8_t RD_TOUCH[] = {
    0x05,0x0D, 0x09,0x05, 0xA1,0x01,
      0x85,0x04,
      0x09,0x22, 0xA1,0x02,
        0x09,0x47, 0x09,0x42, 0x15,0x00, 0x25,0x01,
        0x75,0x01, 0x95,0x02, 0x81,0x02,
        0x95,0x01, 0x75,0x02, 0x81,0x03,
        0x95,0x01, 0x75,0x04, 0x09,0x51, 0x25,0x0F, 0x81,0x02,
        0x05,0x01, 0x15,0x00, 0x26,0xFF,0x0F,
        0x75,0x10, 0x95,0x01, 0x09,0x30, 0x81,0x02,
        0x09,0x31, 0x81,0x02,
      0xC0,
      0x05,0x0D, 0x09,0x54, 0x15,0x00, 0x25,0x7F,
      0x75,0x08, 0x95,0x01, 0x81,0x02,
      0x05,0x09, 0x09,0x01, 0x15,0x00, 0x25,0x01,
      0x75,0x01, 0x95,0x01, 0x81,0x02,
      0x95,0x07, 0x75,0x01, 0x81,0x03,
    0xC0
};

/* A plain relative mouse, for the branch that passes deltas through. */
static const uint8_t RD_MOUSE[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
      0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x03,
        0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
        0x95,0x01, 0x75,0x05, 0x81,0x03,
        0x05,0x01, 0x09,0x30, 0x09,0x31,
        0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x06,
      0xC0,
    0xC0
};

static struct {
    uint8_t  addr;             /* the one address that answers       */
    uint16_t desc_reg;
    uint16_t rd_reg, in_reg, out_reg, cmd_reg, data_reg;
    const uint8_t *rdesc;
    uint32_t rdesc_len;
    /* corruptions a test can ask for */
    int bad_len, bad_version, bad_reserved, bad_rdlen, bad_maxin, bad_same;
    /* the input report the device will hand over next */
    uint8_t  inrep[64];
    uint16_t inrep_len;
    /* what the host did */
    int resets, powerons, poweroffs, cmds;
    /* v0.8.9.6: a device that is busy after a reset. reset_busy is how
       many transfers it refuses once reset; busy_left counts down. A
       power-on that comes AFTER the reset is the wrong order, and
       last_cmd_order records which came first. */
    int reset_busy, busy_left;
    char order[8]; int norder;
    int reads, wrrds, probes;
} dev;

static void dev_reset(const uint8_t *rd, uint32_t rdlen)
{
    memset(&dev, 0, sizeof dev);
    dev.addr     = 0x2C;          /* a real trackpad address        */
    dev.desc_reg = 0x0020;        /* and the awkward register       */
    dev.rd_reg   = 0x0002;
    dev.in_reg   = 0x0003;
    dev.out_reg  = 0x0004;
    dev.cmd_reg  = 0x0005;
    dev.data_reg = 0x0006;
    dev.rdesc    = rd;
    dev.rdesc_len = rdlen;
}

static void put16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)(v&0xFF); p[1]=(uint8_t)(v>>8); }

static void build_hid_desc(uint8_t *d)
{
    memset(d, 0, IHID_DESC_LEN);
    put16(d + 0,  dev.bad_len ? 0x001F : IHID_DESC_LEN);
    put16(d + 2,  dev.bad_version ? 0x0200 : 0x0100);
    put16(d + 4,  dev.bad_rdlen ? 0 : (uint16_t)dev.rdesc_len);
    put16(d + 6,  dev.rd_reg);
    put16(d + 8,  dev.bad_same ? dev.rd_reg : dev.in_reg);
    put16(d + 10, dev.bad_maxin ? 1 : 32);
    put16(d + 12, dev.out_reg);
    put16(d + 14, 32);
    put16(d + 16, dev.cmd_reg);
    put16(d + 18, dev.data_reg);
    put16(d + 20, 0x04F3);        /* Elan */
    put16(d + 22, 0x3098);
    put16(d + 24, 0x0001);
    if(dev.bad_reserved) d[26] = 0x5A;
}

static int m_probe(void *c, int bus, uint8_t a)
{
    (void)c;
    dev.probes++;
    if(bus != 0) return -1;
    return a == dev.addr ? 0 : -1;
}

static int m_wrrd(void *c, int bus, uint8_t a, const uint8_t *w,
                  uint32_t wl, uint8_t *r, uint32_t rl)
{
    uint16_t reg;
    (void)c;
    dev.wrrds++;
    if(bus != 0 || a != dev.addr || wl != 2) return -1;
    if(dev.busy_left > 0){ dev.busy_left--; return -1; }   /* NAK */
    reg = (uint16_t)(w[0] | (w[1] << 8));

    if(reg == dev.desc_reg){
        uint8_t d[IHID_DESC_LEN];
        if(rl > IHID_DESC_LEN) return -1;
        build_hid_desc(d);
        memcpy(r, d, rl);
        return 0;
    }
    if(reg == dev.rd_reg){
        if(rl > dev.rdesc_len) return -1;
        memcpy(r, dev.rdesc, rl);
        return 0;
    }
    /* Any other register: a real device returns something, and what
       it returns is not an HID descriptor. 0xFF is what an unmapped
       register file reads as, which is the realistic case. */
    memset(r, 0xFF, rl);
    return 0;
}

static int m_rd(void *c, int bus, uint8_t a, uint8_t *b, uint32_t l)
{
    (void)c;
    dev.reads++;
    if(bus != 0 || a != dev.addr) return -1;
    if(dev.busy_left > 0){ dev.busy_left--; return -1; }   /* NAK */
    memset(b, 0, l);
    if(dev.inrep_len == 0){
        /* The device says "nothing to report" with a length of zero,
           which is the common case when polling. */
        return 0;
    }
    if(l < dev.inrep_len) return -1;
    memcpy(b, dev.inrep, dev.inrep_len);
    return 0;
}

static int m_wr(void *c, int bus, uint8_t a, const uint8_t *b, uint32_t l)
{
    (void)c;
    if(bus != 0 || a != dev.addr || l < 4) return -1;
    {
        uint16_t reg = (uint16_t)(b[0] | (b[1] << 8));
        if(reg != dev.cmd_reg) return -1;
        dev.cmds++;
        if(b[3] == IHID_CMD_RESET){
            dev.resets++;
            dev.busy_left = dev.reset_busy;
            if(dev.norder < 7) dev.order[dev.norder++] = 'R';
        }
        if(b[3] == IHID_CMD_SET_POWER){
            if(b[2] == IHID_POWER_ON){
                dev.powerons++;
                if(dev.norder < 7) dev.order[dev.norder++] = 'P';
            }
            else dev.poweroffs++;
        }
    }
    return 0;
}

static const ihid_ops_t OPS = { m_wr, m_rd, m_wrrd, m_probe };

/* Build an input report for the touchpad descriptor: two-byte length,
   report id 4, then the contact. */
static void touch_report(int tip, int x, int y, int button, int contacts)
{
    uint8_t *p = dev.inrep;
    memset(p, 0, sizeof dev.inrep);
    /* data layout after the id: byte0 bits 0-1 confidence+tip,
       bits 4-7 contact id; bytes 1-2 X; bytes 3-4 Y; byte5 contact
       count; byte6 bit0 button. */
    p[0] = 9; p[1] = 0;              /* length: 2 + 1 + 6 */
    p[2] = 4;                        /* report id */
    p[3] = (uint8_t)(tip ? 0x03 : 0x00);
    put16(p + 4, (uint16_t)x);
    put16(p + 6, (uint16_t)y);
    p[8] = (uint8_t)contacts;
    p[9] = (uint8_t)(button ? 1 : 0);
    p[0] = 10;                        /* 2 header + 1 id + 7 data */
    dev.inrep_len = 10;
}

static ihid_dev_t found[IHID_MAX_DEV];
static uint8_t    rdbuf[IHID_RD_MAX];

int main(void)
{
    int n, r, dx, dy, btn, have;

    printf("\n=== test_i2c_hid: a trackpad, against a mock device ===\n");

    /* ── discovery ───────────────────────────────────────────────── */
    head("discovery finds the device at a non-obvious register");
    dev_reset(RD_TOUCH, sizeof RD_TOUCH);
    n = ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
    ck("one device", n == 1, n);
    ck("at the address that answered", n == 1 && found[0].addr == 0x2C,
       n == 1 ? found[0].addr : -1);
    /* 0x20 is the second candidate, not the first: the search had to
       reject 0x01 before finding it, which is the whole mechanism. */
    ck("at register 0x20", n == 1 && found[0].desc_reg == 0x0020,
       n == 1 ? found[0].desc_reg : -1);
    ck("it read the vendor id", n == 1 && found[0].vid == 0x04F3,
       n == 1 ? found[0].vid : -1);
    ck("and the report descriptor length",
       n == 1 && found[0].report_desc_len == sizeof RD_TOUCH,
       n == 1 ? found[0].report_desc_len : -1);
    ck("it probed the whole 7-bit range", dev.probes >= 0x70, dev.probes);

    head("every candidate register in turn");
    {
        static const uint16_t regs[IHID_NCAND] = IHID_CAND_REGS;
        int i, bad = -1;
        for(i = 0; i < IHID_NCAND; i++){
            dev_reset(RD_TOUCH, sizeof RD_TOUCH);
            dev.desc_reg = regs[i];
            if(ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV) != 1
               || found[0].desc_reg != regs[i]){ bad = i; break; }
        }
        ck("a device at any of them is found", bad < 0, bad);
    }

    head("a register that is not a descriptor is not accepted");
    {
        dev_reset(RD_TOUCH, sizeof RD_TOUCH);
        dev.desc_reg = 0x0040;     /* not in the candidate list */
        n = ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
        /* Every candidate returns 0xFF bytes, which is what an
           unmapped register file reads as -- and 0xFFFF is not 30. */
        ck("nothing is found when the descriptor is elsewhere",
           n == 0, n);
    }

    head("descriptors that are refused");
    {
        struct { const char *what; int *flag; int want; } T[] = {
            { "a length that is not thirty",      &dev.bad_len,      0 },
            { "a version that is not 1.00",       &dev.bad_version,  0 },
        };
        unsigned i;
        for(i = 0; i < sizeof T / sizeof T[0]; i++){
            dev_reset(RD_TOUCH, sizeof RD_TOUCH);
            *T[i].flag = 1;
            ck(T[i].what,
               ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV) == T[i].want, 0);
        }
        /* These three have the signature and fail on their contents,
           which is a different refusal -- IHID_E_FIELDS, not
           IHID_E_SIG -- and says "this IS an HID descriptor and it is
           wrong" rather than "look elsewhere". */
        {
            uint8_t d[IHID_DESC_LEN];
            ihid_dev_t t;
            dev_reset(RD_TOUCH, sizeof RD_TOUCH);
            dev.bad_reserved = 1; build_hid_desc(d);
            ck("a nonzero reserved field",
               ihid_desc_parse(d, sizeof d, &t) == IHID_E_FIELDS, 0);
            dev_reset(RD_TOUCH, sizeof RD_TOUCH);
            dev.bad_rdlen = 1; build_hid_desc(d);
            ck("a report descriptor of zero bytes",
               ihid_desc_parse(d, sizeof d, &t) == IHID_E_FIELDS, 0);
            dev_reset(RD_TOUCH, sizeof RD_TOUCH);
            dev.bad_maxin = 1; build_hid_desc(d);
            ck("an input report shorter than its own header",
               ihid_desc_parse(d, sizeof d, &t) == IHID_E_FIELDS, 0);
            dev_reset(RD_TOUCH, sizeof RD_TOUCH);
            dev.bad_same = 1; build_hid_desc(d);
            ck("the input and descriptor registers being the same",
               ihid_desc_parse(d, sizeof d, &t) == IHID_E_FIELDS, 0);
            dev_reset(RD_TOUCH, sizeof RD_TOUCH);
            build_hid_desc(d);
            ck("...and a correct one is accepted",
               ihid_desc_parse(d, sizeof d, &t) == IHID_OK, 0);
            ck("a descriptor shorter than thirty bytes is refused",
               ihid_desc_parse(d, 29, &t) == IHID_E_PARAM, 0);
            ck("a NULL descriptor", ihid_desc_parse(0, 30, &t) == IHID_E_PARAM, 0);
        }
    }

    /* ── bringing it up ──────────────────────────────────────────── */
    head("bringing the device up");
    dev_reset(RD_TOUCH, sizeof RD_TOUCH);
    ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
    ck("power on is a command to the command register",
       ihid_set_power(&OPS, 0, &found[0], 1) == IHID_OK
       && dev.powerons == 1, dev.powerons);
    ck("reset likewise",
       ihid_reset(&OPS, 0, &found[0]) == IHID_OK && dev.resets == 1,
       dev.resets);
    r = ihid_bring_up(&OPS, 0, &found[0], rdbuf, sizeof rdbuf);
    ck("the report descriptor is fetched and parsed", r == IHID_OK, r);
    ck("it is a pointer", found[0].is_pointer, 0);
    ck("X and Y were resolved",
       found[0].rd.i_x >= 0 && found[0].rd.i_y >= 0, 0);
    ck("the tip switch too", found[0].rd.i_tip >= 0, found[0].rd.i_tip);
    ck("and the contact count", found[0].rd.i_contacts >= 0,
       found[0].rd.i_contacts);
    ck("X is absolute", found[0].rd.field[found[0].rd.i_x].relative == 0, 0);
    ck("a scratch buffer too small is refused",
       ihid_bring_up(&OPS, 0, &found[0], rdbuf, 4) == IHID_E_PARAM, 0);

    head("a device whose report descriptor has no pointer in it");
    {
        /* A keyboard: a perfectly good HID device with no X. */
        static const uint8_t RD_KBD[] = {
            0x05,0x01, 0x09,0x06, 0xA1,0x01,
              0x05,0x07, 0x19,0xE0, 0x29,0xE7,
              0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
            0xC0
        };
        dev_reset(RD_KBD, sizeof RD_KBD);
        ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
        r = ihid_bring_up(&OPS, 0, &found[0], rdbuf, sizeof rdbuf);
        /* Reported as "not a pointer" rather than as a failure: it is
           a device, it was found, and a keyboard on the I2C bus is
           exactly what the HP might also have. */
        ck("it is found and named, not silently dropped",
           r == IHID_E_NOPTR, r);
        ck("...and is_pointer is false", !found[0].is_pointer, 0);
        ck("the reason is readable",
           strstr(ihid_strerror(r), "not a pointer") != 0, 0);
    }

    /* ── reading reports ─────────────────────────────────────────── */
    head("a report descriptor that will not parse is a different refusal");
    {
        /* Pop with no Push: hrd_parse refuses it. The device, its HID
           descriptor and its registers are all fine, so the answer
           must be "the report descriptor would not parse" and not
           "there is no pointer in it" -- they are different problems
           and the log line is what gets photographed. */
        static const uint8_t RD_BAD[] = { 0xB4, 0x05,0x01, 0x09,0x30,
                                          0x75,0x08, 0x95,0x01, 0x81,0x02 };
        dev_reset(RD_BAD, sizeof RD_BAD);
        ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
        r = ihid_bring_up(&OPS, 0, &found[0], rdbuf, sizeof rdbuf);
        ck("IHID_E_DESC, not IHID_E_NOPTR", r == IHID_E_DESC, r);
        ck("the reason says so",
           strstr(ihid_strerror(r), "would not parse") != 0, 0);
    }

    head("a device with an X and no Y is not a pointer");
    {
        /* Half a pointer is not a pointer: a driver that accepted it
           would read Y from field -1. */
        static const uint8_t RD_HALF[] = {
            0x05,0x01, 0x09,0x02, 0xA1,0x01,
              0x09,0x30, 0x15,0x81, 0x25,0x7F,
              0x75,0x08, 0x95,0x01, 0x81,0x06,
            0xC0
        };
        dev_reset(RD_HALF, sizeof RD_HALF);
        ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
        r = ihid_bring_up(&OPS, 0, &found[0], rdbuf, sizeof rdbuf);
        ck("X alone is refused", r == IHID_E_NOPTR, r);
        ck("...and is_pointer is false", !found[0].is_pointer, 0);
    }

    head("reading an input report");
    dev_reset(RD_TOUCH, sizeof RD_TOUCH);
    ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
    ihid_bring_up(&OPS, 0, &found[0], rdbuf, sizeof rdbuf);
    {
        uint8_t buf[64];
        dev.inrep_len = 0;
        ck("nothing to report is zero, not an error",
           ihid_read_report(&OPS, 0, &found[0], buf, sizeof buf) == 0, 0);
        touch_report(1, 100, 200, 0, 1);
        ck("a report comes back with its own length",
           ihid_read_report(&OPS, 0, &found[0], buf, sizeof buf) == 10, 0);
        /* A length of 1 cannot be right: the header alone is two. */
        dev.inrep[0] = 1; dev.inrep[1] = 0;
        ck("a length below the header is refused",
           ihid_read_report(&OPS, 0, &found[0], buf, sizeof buf)
           == IHID_E_SHORT, 0);
        /* A length longer than the device said it would ever send. */
        dev.inrep[0] = 200; dev.inrep[1] = 0;
        ck("a length beyond max_input_len is refused",
           ihid_read_report(&OPS, 0, &found[0], buf, sizeof buf)
           == IHID_E_SHORT, 0);
    }

    /* ════════════════════════════════════════════════════════════
       THE DECODE -- absolute finger positions into pointer movement
       ════════════════════════════════════════════════════════════ */
    head("a touch, moved");
    dev_reset(RD_TOUCH, sizeof RD_TOUCH);
    ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
    ihid_bring_up(&OPS, 0, &found[0], rdbuf, sizeof rdbuf);
    {
        ihid_dev_t *d = &found[0];
        touch_report(1, 1000, 2000, 0, 1);
        r = ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("the first report of a touch decodes", r == IHID_OK, r);
        /* It must move NOTHING: there is no previous position to be a
           delta from, and inventing one jumps the pointer by wherever
           the finger happened to land. */
        ck("...and moves nothing", dx == 0 && dy == 0, dx);
        ck("...and reports nothing to do", !have, have);

        touch_report(1, 1010, 1990, 0, 1);
        r = ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("the second report moves by the difference",
           r == IHID_OK && dx == 10 && dy == -10, dx);
        ck("...and says there is something to do", have, 0);

        touch_report(1, 1010, 1990, 0, 1);
        r = ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("a report with the finger still gives zero movement",
           r == IHID_OK && dx == 0 && dy == 0, dx);
        ck("...and nothing to do", !have, have);
    }

    head("THE ONE THAT MATTERS: lift, replace elsewhere, do not teleport");
    {
        ihid_dev_t *d = &found[0];
        touch_report(1, 1000, 1000, 0, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        touch_report(1, 1100, 1000, 0, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("a drag of 100 moves 100", dx == 100, dx);

        /* Finger up. */
        touch_report(0, 1100, 1000, 0, 0);
        r = ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("the lift itself moves nothing",
           r == IHID_OK && dx == 0 && dy == 0, dx);

        /* Down again on the far side of the pad. A driver that kept
           the old position would move the pointer 2900 columns. */
        touch_report(1, 4000, 100, 0, 1);
        r = ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("replacing the finger 2900 units away moves NOTHING",
           r == IHID_OK && dx == 0 && dy == 0, dx);
        touch_report(1, 4005, 100, 0, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("...and the next movement is relative to there", dx == 5, dx);
    }

    head("the tip switch alone is enough to mean the finger is up");
    {
        ihid_dev_t *d = &found[0];
        d->touching = 0; d->last_buttons = 0;
        touch_report(1, 1000, 1000, 0, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        /* Tip CLEAR but the contact slot still reported -- which is
           what a device does while a finger hovers. The previous
           lift check cleared the contact count too, so the contact
           path caught it and the tip switch was never tested on its
           own. */
        touch_report(0, 1000, 1000, 0, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        touch_report(1, 3000, 1000, 0, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("a touch after a tip-up starts fresh", dx == 0, dx);
    }

    head("a contact count of zero means the finger is gone too");
    {
        ihid_dev_t *d = &found[0];
        /* Some devices clear the contact count and leave the tip bit
           set. Either one means no finger. */
        touch_report(1, 2000, 2000, 0, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        touch_report(1, 2000, 2000, 0, 0);     /* tip set, 0 contacts */
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        touch_report(1, 3000, 2000, 0, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("a touch after a zero contact count starts fresh", dx == 0, dx);
    }

    head("buttons");
    {
        ihid_dev_t *d = &found[0];
        d->touching = 0; d->last_buttons = 0;
        touch_report(1, 500, 500, 1, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("the button is seen", btn == 1, btn);
        /* A press with no movement still has to be reported, or a
           click on a motionless finger does nothing. */
        ck("...and a press with no movement is something to do", have, 0);
        touch_report(1, 500, 500, 1, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("a held button with no movement is not re-reported",
           !have, have);
        touch_report(1, 500, 500, 0, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("the release is", have && btn == 0, btn);
    }

    head("a report for a different report id is ignored, not misread");
    {
        ihid_dev_t *d = &found[0];
        uint8_t save = dev.inrep[2];
        touch_report(1, 700, 700, 0, 1);
        ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        /* Different COORDINATES as well as a different id. With the
           same coordinates the delta is zero either way, so the first
           version of this check passed whether or not the id was
           looked at -- and an injection that ignored the id survived
           the suite. */
        touch_report(1, 3500, 3500, 0, 1);
        dev.inrep[2] = 7;            /* not the pointer's report id */
        r = ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        /* Not an error: a touchpad sends several reports and only one
           carries the pointer. Decoding somebody else's report at the
           pointer's bit offsets is how a pointer jumps at random. */
        ck("it is accepted and does nothing", r == IHID_OK && !have, have);
        ck("...and moves nothing", dx == 0 && dy == 0, dx);
        dev.inrep[2] = save;
    }

    head("short and malformed reports");
    {
        ihid_dev_t *d = &found[0];
        touch_report(1, 900, 900, 0, 1);
        ck("a report of one byte is refused",
           ihid_decode(d, dev.inrep, 1, &dx, &dy, &btn, &have)
           == IHID_E_SHORT, 0);
        ck("a report with a header and nothing else is refused",
           ihid_decode(d, dev.inrep, 2, &dx, &dy, &btn, &have)
           == IHID_E_SHORT, 0);
        ck("a report cut off inside the coordinates is refused",
           ihid_decode(d, dev.inrep, 5, &dx, &dy, &btn, &have)
           == IHID_E_SHORT, 0);
        ck("a NULL report", ihid_decode(d, 0, 10, &dx, &dy, &btn, &have)
           == IHID_E_PARAM, 0);
    }

    /* ── the relative branch ─────────────────────────────────────── */
    head("a relative mouse on the I2C bus passes its deltas straight");
    {
        ihid_dev_t *d;
        dev_reset(RD_MOUSE, sizeof RD_MOUSE);
        ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
        r = ihid_bring_up(&OPS, 0, &found[0], rdbuf, sizeof rdbuf);
        ck("it comes up", r == IHID_OK, r);
        d = &found[0];
        ck("X is relative", d->rd.field[d->rd.i_x].relative == 1, 0);
        /* No report id in this descriptor, so the data starts right
           after the two-byte length. buttons=1, X=-3, Y=+4. */
        memset(dev.inrep, 0, sizeof dev.inrep);
        dev.inrep[0] = 5; dev.inrep[1] = 0;
        dev.inrep[2] = 0x01; dev.inrep[3] = 0xFD; dev.inrep[4] = 0x04;
        dev.inrep_len = 5;
        r = ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("the delta is the report, not a difference",
           r == IHID_OK && dx == -3 && dy == 4, dx);
        ck("the button comes through", btn == 1, btn);
        ck("there is something to do", have, 0);
        /* And no touch state is involved: the same report twice moves
           twice, which is what a mouse does. */
        r = ihid_decode(d, dev.inrep, dev.inrep_len, &dx, &dy, &btn, &have);
        ck("the same report again moves again", dx == -3, dx);
    }

    head("a device that is not a pointer cannot be decoded");
    {
        ihid_dev_t d;
        memset(&d, 0, sizeof d);
        ck("it refuses by name",
           ihid_decode(&d, dev.inrep, 10, &dx, &dy, &btn, &have)
           == IHID_E_NOPTR, 0);
    }

    head("the bus refusing everything");
    {
        static const ihid_ops_t DEAD = { 0, 0, 0, 0 };
        ihid_dev_t d;
        memset(&d, 0, sizeof d);
        ck("discovery with no transfer function",
           ihid_find(&DEAD, 0, 0, found, IHID_MAX_DEV) == 0, 0);
        ck("a command with no write function",
           ihid_set_power(&DEAD, 0, &d, 1) == IHID_E_PARAM, 0);
        ck("a read with no read function",
           ihid_read_report(&DEAD, 0, &d, rdbuf, sizeof rdbuf)
           == IHID_E_PARAM, 0);
        ck("discovery with a zero maximum",
           ihid_find(&OPS, 0, 0, found, 0) == IHID_E_PARAM, 0);
    }

    /* ══════════════════════════════════════════════════════════════
       v0.8.9.6 -- THE DEVICE NEEDS TIME AFTER A RESET
       0.8.8.11 sent SET_POWER and RESET and read the report descriptor
       in the same breath. A device still busy with the reset refuses
       that read, ihid_bring_up() returned IHID_E_IO, and the trackpad
       was never attached -- on the HP, "trackpad werkt niet". The mock
       below refuses transfers for a while after a reset, as real ones
       do; every check before this one used a device that was ready
       the instant it was told to reset.
       ══════════════════════════════════════════════════════════════ */
    head("a device busy after its reset is waited for");
    dev_reset(RD_TOUCH, sizeof RD_TOUCH);
    ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
    dev.reset_busy = 6;                 /* six refused transfers */
    {
        uint32_t took = 0;
        int r;
        slept_ms = 0; sleeps = 0;
        r = ihid_power_and_reset(&OPS, 0, &found[0], sleep_ms, 1000u, &took);
        ck("it answers in the end", r == IHID_OK, r);
        ck("  ... powered on BEFORE it was reset",
           dev.norder >= 2 && dev.order[0] == 'P' && dev.order[1] == 'R',
           dev.norder);
        ck("  ... and the wait was spent, not skipped",
           slept_ms >= 50u && took >= 50u, (long)took);
        ck("  ... and the busy period is over", dev.busy_left == 0,
           dev.busy_left);
        ck("then the report descriptor reads first time",
           ihid_bring_up(&OPS, 0, &found[0], rdbuf, sizeof rdbuf) == IHID_OK,
           0);
    }

    head("the old order -- reset, then read at once -- is what failed");
    dev_reset(RD_TOUCH, sizeof RD_TOUCH);
    ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
    dev.reset_busy = 6;
    ihid_set_power(&OPS, 0, &found[0], 1);
    ihid_reset(&OPS, 0, &found[0]);
    ck("reading the descriptor immediately is refused",
       ihid_bring_up(&OPS, 0, &found[0], rdbuf, sizeof rdbuf) == IHID_E_IO,
       0);

    head("a device that never answers its reset does not hang the boot");
    dev_reset(RD_TOUCH, sizeof RD_TOUCH);
    ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
    dev.reset_busy = 1000000;
    {
        uint32_t took = 0;
        int r;
        slept_ms = 0; sleeps = 0;
        r = ihid_power_and_reset(&OPS, 0, &found[0], sleep_ms, 1000u, &took);
        ck("it gives up with IHID_E_NORESET", r == IHID_E_NORESET, r);
        ck("  ... after the budget, not before", took >= 1000u, (long)took);
        ck("  ... and not long after it", slept_ms <= 1100u, (long)slept_ms);
        ck("  ... and says so in words",
           strcmp(ihid_strerror(IHID_E_NORESET), "unknown") != 0, 0);
    }
    {
        uint32_t took = 0;
        dev_reset(RD_TOUCH, sizeof RD_TOUCH);
        ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
        dev.reset_busy = 1000000;
        ck("without a delay function it still ends",
           ihid_power_and_reset(&OPS, 0, &found[0], 0, 200u, &took)
               == IHID_E_NORESET, (long)took);
    }

    head("a device ready at once is not made to wait for nothing");
    dev_reset(RD_TOUCH, sizeof RD_TOUCH);
    ihid_find(&OPS, 0, 0, found, IHID_MAX_DEV);
    {
        uint32_t took = 0;
        slept_ms = 0;
        ck("it answers", ihid_power_and_reset(&OPS, 0, &found[0], sleep_ms,
                                              1000u, &took) == IHID_OK, 0);
        ck("  ... on the first look after the initial settle",
           took == 50u, (long)took);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
