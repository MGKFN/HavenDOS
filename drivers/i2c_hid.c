/* drivers/i2c_hid.c -- see include/i2c_hid.h. */
#include "../include/i2c_hid.h"
#include "../include/i2c.h"
#include "../include/string.h"
#include "../include/klog.h"

static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1] << 8)); }

int ihid_desc_parse(const uint8_t *d, uint32_t len, ihid_dev_t *out)
{
    if (!d || !out || len < IHID_DESC_LEN) return IHID_E_PARAM;

    /* THE SIGNATURE. Thirty bytes long, version 1.00 -- the two
       fields every conforming device puts first, and the reason a
       candidate register can be tried rather than looked up. */
    if (rd16(d + 0) != IHID_DESC_LEN) return IHID_E_SIG;
    if (rd16(d + 2) != 0x0100)        return IHID_E_SIG;

    out->hid_desc_len    = rd16(d + 0);
    out->bcd_version     = rd16(d + 2);
    out->report_desc_len = rd16(d + 4);
    out->report_desc_reg = rd16(d + 6);
    out->input_reg       = rd16(d + 8);
    out->max_input_len   = rd16(d + 10);
    out->output_reg      = rd16(d + 12);
    out->max_output_len  = rd16(d + 14);
    out->command_reg     = rd16(d + 16);
    out->data_reg        = rd16(d + 18);
    out->vid             = rd16(d + 20);
    out->pid             = rd16(d + 22);
    out->ver             = rd16(d + 24);
    /* Bytes 26..29 are reserved and must be zero. Checked, because a
       register file that happened to start 1E 00 00 01 is very
       unlikely to continue with four zeroes as well. */
    if (d[26] || d[27] || d[28] || d[29]) return IHID_E_FIELDS;

    /* Fields that contradict each other. A device claiming a report
       descriptor of zero bytes, or an input report shorter than the
       two-byte length header it is required to carry, is not a device
       this understands -- and the signature matching means it is
       worth saying so rather than moving on silently. */
    if (out->report_desc_len == 0 ||
        out->report_desc_len > IHID_RD_MAX)   return IHID_E_FIELDS;
    if (out->max_input_len < 2)               return IHID_E_FIELDS;
    if (out->input_reg == out->report_desc_reg) return IHID_E_FIELDS;
    return IHID_OK;
}

/* Read a register: write its two bytes little-endian, then read, with
   a repeated start and no stop between. */
static int reg_read(const ihid_ops_t *ops, void *ctx, int bus,
                    uint8_t addr, uint16_t reg, uint8_t *buf, uint32_t len)
{
    uint8_t w[2];
    w[0] = (uint8_t)(reg & 0xFF);
    w[1] = (uint8_t)(reg >> 8);
    if (!ops || !ops->wrrd) return IHID_E_PARAM;
    return ops->wrrd(ctx, bus, addr, w, 2, buf, len) == 0
           ? IHID_OK : IHID_E_IO;
}

int ihid_find(const ihid_ops_t *ops, void *ctx, int bus,
              ihid_dev_t *out, int max)
{
    static const uint16_t cand[IHID_NCAND] = IHID_CAND_REGS;
    uint8_t addrs[128];
    int n = 0, i, c, found = 0;

    if (!ops || !out || max <= 0) return IHID_E_PARAM;

    /* Every address that acknowledges. i2c_scan()'s own comment
       explains why a zero-length read is the safe probe. */
    for (i = 0x08; i <= 0x77 && n < (int)sizeof addrs; i++) {
        if (ops->probe && ops->probe(ctx, bus, (uint8_t)i) == 0)
            addrs[n++] = (uint8_t)i;
    }

    for (i = 0; i < n && found < max; i++) {
        /* `found < max` on the inner loop too. The break below means
           only one register per address can ever be accepted, so this
           is unreachable today -- and an injection that removed the
           break survived the suite, which is exactly the kind of
           change that would make it reachable. A bound that depends
           on a `break` three lines further down is not a bound. */
        for (c = 0; c < IHID_NCAND && found < max; c++) {
            uint8_t raw[IHID_DESC_LEN];
            ihid_dev_t *d = &out[found];
            int r;

            memset(raw, 0, sizeof raw);
            if (reg_read(ops, ctx, bus, addrs[i], cand[c],
                         raw, sizeof raw) != IHID_OK)
                continue;
            memset(d, 0, sizeof *d);
            r = ihid_desc_parse(raw, sizeof raw, d);
            if (r != IHID_OK) continue;
            d->bus = bus;
            d->addr = addrs[i];
            d->desc_reg = cand[c];
            found++;
            /* One device per address: the first register that
               validates is the one, and trying the rest would at best
               find the same descriptor twice. */
            break;
        }
    }
    return found;
}

int ihid_bring_up(const ihid_ops_t *ops, void *ctx, ihid_dev_t *d,
                  uint8_t *buf, uint32_t cap)
{
    int r;
    if (!ops || !d || !buf) return IHID_E_PARAM;
    if (cap < d->report_desc_len) return IHID_E_PARAM;

    r = reg_read(ops, ctx, d->bus, d->addr, d->report_desc_reg,
                 buf, d->report_desc_len);
    if (r != IHID_OK) return r;

    r = hrd_parse(buf, d->report_desc_len, &d->rd);
    if (r != HRD_OK) return IHID_E_DESC;

    d->is_pointer = (d->rd.i_x >= 0 && d->rd.i_y >= 0);
    return d->is_pointer ? IHID_OK : IHID_E_NOPTR;
}

/* A command is written to the command register: the register, then
   two bytes of opcode and report selector. */
static int command(const ihid_ops_t *ops, void *ctx, const ihid_dev_t *d,
                   uint8_t opcode, uint8_t arg)
{
    uint8_t w[4];
    if (!ops || !ops->wr || !d) return IHID_E_PARAM;
    w[0] = (uint8_t)(d->command_reg & 0xFF);
    w[1] = (uint8_t)(d->command_reg >> 8);
    w[2] = arg;            /* report id / power state */
    w[3] = opcode;
    return ops->wr(ctx, d->bus, d->addr, w, 4) == 0 ? IHID_OK : IHID_E_IO;
}

int ihid_set_power(const ihid_ops_t *ops, void *ctx,
                   const ihid_dev_t *d, int on)
{
    return command(ops, ctx, d, IHID_CMD_SET_POWER,
                   on ? IHID_POWER_ON : IHID_POWER_SLEEP);
}

int ihid_reset(const ihid_ops_t *ops, void *ctx, const ihid_dev_t *d)
{
    return command(ops, ctx, d, IHID_CMD_RESET, 0);
}

/* See include/i2c_hid.h. The first wait is longer than the rest:
   nothing comes out of a reset in under a few tens of milliseconds,
   and asking sooner only adds refused transfers to the bus. */
#define IHID_POWER_SETTLE_MS  10u
#define IHID_RESET_FIRST_MS   50u
#define IHID_RESET_STEP_MS    10u
int ihid_power_and_reset(const ihid_ops_t *ops, void *ctx, ihid_dev_t *d,
                         void (*delay_ms)(uint32_t), uint32_t budget_ms,
                         uint32_t *took_ms)
{
    uint8_t  buf[64];
    uint32_t t = 0;
    int r;

    if (took_ms) *took_ms = 0;
    if (!ops || !d) return IHID_E_PARAM;

    ihid_set_power(ops, ctx, d, 1);
    if (delay_ms) delay_ms(IHID_POWER_SETTLE_MS);

    r = ihid_reset(ops, ctx, d);
    if (r != IHID_OK) return r;

    if (delay_ms) delay_ms(IHID_RESET_FIRST_MS);
    t = IHID_RESET_FIRST_MS;
    for (;;) {
        /* Any successful read ends the wait: a zero length is the reset
           response itself, and a real report means the device is past
           it. A refused transfer is the device still busy. */
        if (ihid_read_report(ops, ctx, d, buf, sizeof buf) >= 0) {
            if (took_ms) *took_ms = t;
            return IHID_OK;
        }
        /* Counted whether or not there is a delay function, so a
           simulation without one still ends. */
        if (t >= budget_ms) {
            if (took_ms) *took_ms = t;
            return IHID_E_NORESET;
        }
        if (delay_ms) delay_ms(IHID_RESET_STEP_MS);
        t += IHID_RESET_STEP_MS;
    }
}

int ihid_read_report(const ihid_ops_t *ops, void *ctx,
                     const ihid_dev_t *d, uint8_t *buf, uint32_t cap)
{
    uint32_t want;
    uint16_t len;

    if (!ops || !ops->rd || !d || !buf) return IHID_E_PARAM;
    want = d->max_input_len;
    if (want > cap) want = cap;
    if (want < 2)   return IHID_E_PARAM;

    /* The input register is read WITHOUT writing its address first --
       the specification makes it the default read target, which is
       what lets an interrupt-driven host read a report with one
       transfer. */
    if (ops->rd(ctx, d->bus, d->addr, buf, want) != 0) return IHID_E_IO;

    len = rd16(buf);
    /* A length of zero is the device saying "nothing to report". It
       is not an error and it is the common case when polling. */
    if (len == 0) return 0;
    if (len < 2)  return IHID_E_SHORT;
    if (len > want) return IHID_E_SHORT;
    return (int)len;
}

int ihid_decode(ihid_dev_t *d, const uint8_t *report, uint32_t len,
                int *dx, int *dy, int *buttons, int *have)
{
    const uint8_t *data;
    uint32_t dlen;
    int32_t x = 0, y = 0, tip = 1, v;
    int btn = 0, k, moved = 0;

    if (!d || !report || !dx || !dy || !buttons || !have)
        return IHID_E_PARAM;
    *dx = *dy = 0; *have = 0;
    if (!d->is_pointer) return IHID_E_NOPTR;
    if (len < 2) return IHID_E_SHORT;

    /* The report begins with its own two-byte length, then the report
       ID when the descriptor declares ids. hrd_field_value()'s
       offsets are from the start of the DATA, so both come off here --
       the one place that knows the transport. */
    data = report + 2;
    dlen = len - 2;
    if (d->rd.has_report_ids) {
        if (dlen < 1) return IHID_E_SHORT;
        /* A report for a different id is not an error: a touchpad
           sends several, and only one carries the pointer. */
        if (data[0] != d->rd.pointer_report_id) return IHID_OK;
        data++; dlen--;
    }

    if (hrd_field_value(&d->rd.field[d->rd.i_x], data, dlen, &x) != HRD_OK)
        return IHID_E_SHORT;
    if (hrd_field_value(&d->rd.field[d->rd.i_y], data, dlen, &y) != HRD_OK)
        return IHID_E_SHORT;

    /* The tip switch, when there is one: a touchpad reports
       coordinates whether or not a finger is down, and acting on the
       ones with no finger is how a pointer drifts on its own. */
    if (d->rd.i_tip >= 0 &&
        hrd_field_value(&d->rd.field[d->rd.i_tip], data, dlen, &v) == HRD_OK)
        tip = (v != 0);
    /* A contact count of zero means the same thing and some devices
       report only that. */
    if (d->rd.i_contacts >= 0 &&
        hrd_field_value(&d->rd.field[d->rd.i_contacts], data, dlen, &v)
            == HRD_OK && v == 0)
        tip = 0;

    for (k = 0; k < 3; k++) {
        if (d->rd.i_button[k] < 0) continue;
        if (hrd_field_value(&d->rd.field[d->rd.i_button[k]], data, dlen, &v)
                == HRD_OK && v)
            btn |= (k == 0) ? 1 : (k == 1) ? 2 : 4;
    }

    if (d->rd.field[d->rd.i_x].relative) {
        /* A mouse, not a touchpad: the report IS the delta. */
        *dx = (int)x;
        *dy = (int)y;
        moved = (x || y);
    } else if (!tip) {
        /* Finger lifted. Forget where it was, so that replacing it
           somewhere else does not teleport the pointer by the gap. */
        d->touching = 0;
    } else if (!d->touching) {
        /* First report of a touch: remember, move nothing. */
        d->touching = 1;
        d->last_x = x;
        d->last_y = y;
    } else {
        *dx = (int)(x - d->last_x);
        *dy = (int)(y - d->last_y);
        d->last_x = x;
        d->last_y = y;
        moved = (*dx || *dy);
    }

    *buttons = btn;
    *have = moved || (btn != d->last_buttons);
    d->last_buttons = btn;
    return IHID_OK;
}

const char *ihid_strerror(int code)
{
    switch (code) {
    case IHID_OK:        return "ok";
    case IHID_E_PARAM:   return "bad argument";
    case IHID_E_IO:      return "the bus refused the transfer";
    case IHID_E_SIG:     return "no HID descriptor signature at that register";
    case IHID_E_NORESET: return "the device did not answer its reset";
    case IHID_E_FIELDS:  return "a HID descriptor that contradicts itself";
    case IHID_E_DESC:    return "the report descriptor would not parse";
    case IHID_E_NOPTR:   return "no X and Y in it -- not a pointer";
    case IHID_E_FULL:    return "more devices than it can hold";
    case IHID_E_SHORT:   return "a report shorter than its own header";
    default:             return "unknown";
    }
}

/* ════════════════════════════════════════════════════════════════════
   THE LIVE DRIVER -- drivers/i2c.c behind the function pointers
   ════════════════════════════════════════════════════════════════════ */

static int live_wr(void *c, int bus, uint8_t a, const uint8_t *b, uint32_t l)
{ (void)c; return i2c_write(bus, a, b, l) == I2C_OK ? 0 : -1; }
static int live_rd(void *c, int bus, uint8_t a, uint8_t *b, uint32_t l)
{ (void)c; return i2c_read(bus, a, b, l) == I2C_OK ? 0 : -1; }
static int live_wrrd(void *c, int bus, uint8_t a, const uint8_t *w,
                     uint32_t wl, uint8_t *r, uint32_t rl)
{ (void)c; return i2c_write_read(bus, a, w, wl, r, rl) == I2C_OK ? 0 : -1; }
static int live_probe(void *c, int bus, uint8_t a)
{ (void)c; return i2c_probe(bus, a) == I2C_OK ? 0 : -1; }

static const ihid_ops_t LIVE = { live_wr, live_rd, live_wrrd, live_probe };

static ihid_dev_t g_dev[IHID_MAX_DEV];
static int        g_ndev;
static uint8_t    g_rdbuf[IHID_RD_MAX];
static uint8_t    g_report[64];

extern void sleep_ms(uint32_t);
extern void mouse_feed_i2c(int dx, int dy, int buttons);
extern void mouse_i2c_attach(void);

int i2c_hid_init(void)
{
    int bus, nbus = i2c_bus_count();

    g_ndev = 0;
    for (bus = 0; bus < nbus && g_ndev < IHID_MAX_DEV; bus++) {
        int n, i;
        if (!i2c_bus_ready(bus)) continue;
        n = ihid_find(&LIVE, 0, bus, &g_dev[g_ndev],
                      IHID_MAX_DEV - g_ndev);
        if (n <= 0) continue;
        for (i = 0; i < n; i++) {
            ihid_dev_t *d = &g_dev[g_ndev + i];
            int r;
            klog(KLOG_INFO, "i2c-hid: bus %d addr %2x reg %4x, "
                 "%4x:%4x, %u-byte report descriptor",
                 (uint32_t)bus, (uint32_t)d->addr, (uint32_t)d->desc_reg,
                 (uint32_t)d->vid, (uint32_t)d->pid,
                 (uint32_t)d->report_desc_len);
            /* v0.8.9.6: power, reset, and WAIT -- see
               ihid_power_and_reset(). A device that never sends the
               reset response is warned about and tried anyway: some
               do not, and the descriptor read below is the real test. */
            {
                uint32_t took = 0;
                r = ihid_power_and_reset(&LIVE, 0, d, sleep_ms, 1000u, &took);
                d->reset_rc = r; d->reset_ms = took;
                if (r == IHID_OK)
                    klog(KLOG_INFO, "i2c-hid: addr %2x answered its reset "
                         "in %u ms", (uint32_t)d->addr, took);
                else
                    klog(KLOG_WARN, "i2c-hid: addr %2x -- %s (%u ms); "
                         "trying it anyway", (uint32_t)d->addr,
                         ihid_strerror(r), took);
            }
            /* And the descriptor read gets three chances: a device
               just out of reset can still refuse the first one. */
            for (int tries = 0; ; tries++) {
                r = ihid_bring_up(&LIVE, 0, d, g_rdbuf, sizeof g_rdbuf);
                if (r != IHID_E_IO || tries >= 2) break;
                sleep_ms(50);
            }
            d->up_rc = r;
            if (r != IHID_OK) {
                klog(KLOG_WARN, "i2c-hid: addr %2x -- %s",
                     (uint32_t)d->addr, ihid_strerror(r));
                continue;
            }
            klog(KLOG_OK, "i2c-hid: a pointer at %2x: %s X/Y, "
                 "%d buttons%s", (uint32_t)d->addr,
                 d->rd.field[d->rd.i_x].relative ? "relative" : "absolute",
                 (uint32_t)d->rd.nbuttons,
                 d->rd.i_contacts >= 0 ? ", multitouch" : "");
            mouse_i2c_attach();
        }
        g_ndev += n;
    }
    return g_ndev;
}

int i2c_hid_count(void) { return g_ndev; }
const ihid_dev_t *i2c_hid_dev(int i)
{
    if (i < 0 || i >= g_ndev) return 0;
    return &g_dev[i];
}

void i2c_hid_poll(void)
{
    int i;
    for (i = 0; i < g_ndev; i++) {
        ihid_dev_t *d = &g_dev[i];
        int n, dx = 0, dy = 0, btn = 0, have = 0;
        if (!d->is_pointer) continue;
        n = ihid_read_report(&LIVE, 0, d, g_report, sizeof g_report);
        if (n <= 0) continue;
        if (ihid_decode(d, g_report, (uint32_t)n, &dx, &dy, &btn, &have)
                != IHID_OK)
            continue;
        if (have) mouse_feed_i2c(dx, dy, btn);
    }
}

/* ════════════════════════════════════════════════════════════════════
   WHAT THIS WAS PROVED AGAINST

   29 defects injected one at a time, against a mock device that
   implements the I2C-HID specification. The first run caught 23 of
   them with the suite passing 66 of 66 checks. The four that mattered
   all survived for the same reason -- a fixture that could not tell
   right from wrong:

     - The wrong-report-id check: the decoy report carried the SAME
       coordinates as the real one, so the delta was zero whether or
       not the id was looked at.
     - The tip switch: every lift in the suite cleared the contact
       count as well, so the contact-count path caught it and the tip
       switch was never tested alone. A real device clears the tip and
       keeps reporting the contact while a finger hovers.
     - A report descriptor that will not parse: there was no such
       fixture, so "would not parse" and "no pointer in it" could not
       be told apart -- and they are different problems with different
       next steps, which matters because that log line is what gets
       photographed off the HP.
     - A device with an X and no Y: nothing had half a pointer, so
       requiring both was indistinguishable from requiring one.

   One injection changed the CODE rather than the suite. Removing the
   `break` after a register validates survived, and it made visible
   that the inner loop's bound depended on that break three lines
   below it. `found < max` is now on both loops: a bound that relies
   on a break somewhere else is not a bound.

   ── THE TWO SURVIVORS, BOTH REDUNDANCY ─────────────────────────────

   1. Removing that same `break` still survives, now harmlessly: only
      one register per address can hold a valid descriptor on any real
      device, so continuing the loop wastes transfers and finds
      nothing. It stays because six futile transfers per address is
      six too many on a bus this slow.

   2. Dropping the HRD_OK check on the X field read survives, because
      Y is read immediately after and fails identically -- X comes
      first in every report, so a report long enough for Y is long
      enough for X. It stays because that ordering is a property of
      the descriptor, not of this function.
   ════════════════════════════════════════════════════════════════════ */
