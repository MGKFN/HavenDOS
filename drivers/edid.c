/* ════════════════════════════════════════════════════════════════════
   drivers/edid.c -- parsing a monitor's EDID.  v0.9.1.

   Pure: no hardware, no allocation, no 64-bit division. include/edid.h
   has the overview. The layout is VESA E-EDID 1.4 and CTA-861; the
   offsets beside each field are the standard's.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/edid.h"

const char *edid_why_name(int why)
{
    switch(why){
    case EDID_OK:      return "ok";
    case EDID_ESHORT:  return "fewer than 128 bytes came back";
    case EDID_EHEADER: return "the header is not an EDID header";
    case EDID_ESUM:    return "the checksum is wrong";
    default:           return "unknown";
    }
}

static const uint8_t k_header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

static int sum_ok(const uint8_t *b)
{
    uint8_t s = 0; int i;
    for(i = 0; i < EDID_BLOCK; i++) s = (uint8_t)(s + b[i]);
    return s == 0;
}

/* Hz x 100, from kHz and the two totals, in 32 bits: lines per second
   first (fits for any pixel clock an EDID can express, 655 MHz), then
   frames. Truncation is under 0.01 Hz. */
static uint32_t refresh_x100(uint32_t pclk_khz, uint32_t ht, uint32_t vt)
{
    uint32_t lines;
    if(!ht || !vt) return 0;
    lines = pclk_khz * 1000u / ht;
    return lines * 100u / vt;
}

int edid_dtd(const uint8_t *d, edid_mode_t *m)
{
    uint32_t pclk = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
    memset(m, 0, sizeof *m);
    if(!pclk) return 0;                    /* a display descriptor */
    m->pclk_khz = pclk * 10u;
    m->hactive = (uint16_t)(d[2] | ((d[4] & 0xF0) << 4));
    m->hblank  = (uint16_t)(d[3] | ((d[4] & 0x0F) << 8));
    m->vactive = (uint16_t)(d[5] | ((d[7] & 0xF0) << 4));
    m->vblank  = (uint16_t)(d[6] | ((d[7] & 0x0F) << 8));
    m->hso     = (uint16_t)(d[8] | ((d[11] & 0xC0) << 2));
    m->hspw    = (uint16_t)(d[9] | ((d[11] & 0x30) << 4));
    m->vso     = (uint16_t)((d[10] >> 4) | ((d[11] & 0x0C) << 2));
    m->vspw    = (uint16_t)((d[10] & 0x0F) | ((d[11] & 0x03) << 4));
    m->interlaced = (d[17] >> 7) & 1u;
    /* Digital separate sync (bits 4:3 = 11): bit 2 is vsync polarity,
       bit 1 hsync. Anything else has no polarity to report; positive
       is what a monitor expects then. */
    if(((d[17] >> 3) & 3u) == 3u){
        m->vpos = (d[17] >> 2) & 1u;
        m->hpos = (d[17] >> 1) & 1u;
    } else {
        m->vpos = m->hpos = 1;
    }
    m->refresh_x100 = refresh_x100(m->pclk_khz,
                                   (uint32_t)m->hactive + m->hblank,
                                   (uint32_t)m->vactive + m->vblank);
    return 1;
}

/* Copy a descriptor string: 13 bytes, ended by 0x0A, padded with
   spaces. Only printable ASCII is kept -- this goes on the screen. */
static void desc_text(const uint8_t *d, char *out)
{
    int i, n = 0;
    for(i = 5; i < 18; i++){
        uint8_t c = d[i];
        if(c == 0x0A || c == 0) break;
        out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    while(n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static int same_timing(const edid_mode_t *a, const edid_mode_t *b)
{
    return a->pclk_khz == b->pclk_khz && a->hactive == b->hactive &&
           a->hblank == b->hblank && a->hso == b->hso &&
           a->hspw == b->hspw && a->vactive == b->vactive &&
           a->vblank == b->vblank && a->vso == b->vso &&
           a->vspw == b->vspw && a->interlaced == b->interlaced;
}

static void add_mode(edid_t *e, const edid_mode_t *m)
{
    int i;
    if(e->nmodes >= EDID_MAX_MODES) return;
    /* A CTA block often repeats the base block's preferred timing. */
    for(i = 0; i < e->nmodes; i++)
        if(same_timing(&e->modes[i], m)) return;
    e->modes[e->nmodes++] = *m;
}

int edid_parse(const uint8_t *buf, uint32_t len, edid_t *e)
{
    int i;
    uint32_t b, nblocks;
    uint16_t id;

    memset(e, 0, sizeof *e);
    if(!buf || len < EDID_BLOCK) return e->why = EDID_ESHORT;
    if(memcmp(buf, k_header, 8) != 0) return e->why = EDID_EHEADER;
    if(!sum_ok(buf)) return e->why = EDID_ESUM;

    /* Bytes 8-9: three 5-bit letters, big-endian, 1 = 'A'. */
    id = (uint16_t)((buf[8] << 8) | buf[9]);
    e->mfg[0] = (char)('A' - 1 + ((id >> 10) & 0x1F));
    e->mfg[1] = (char)('A' - 1 + ((id >> 5) & 0x1F));
    e->mfg[2] = (char)('A' - 1 + (id & 0x1F));
    e->mfg[3] = 0;
    e->product = (uint16_t)(buf[10] | (buf[11] << 8));
    e->serial  = (uint32_t)buf[12] | ((uint32_t)buf[13] << 8) |
                 ((uint32_t)buf[14] << 16) | ((uint32_t)buf[15] << 24);
    e->week    = buf[16];
    e->year    = (uint16_t)(1990u + buf[17]);
    e->version = buf[18];
    e->revision= buf[19];
    e->extensions = buf[126];

    /* The four descriptors at 54, 72, 90, 108. The first timing among
       them is the preferred mode, and it lands in modes[0]. */
    for(i = 0; i < 4; i++){
        const uint8_t *d = buf + 54 + i * 18;
        edid_mode_t m;
        if(edid_dtd(d, &m)){ m.block = 0; add_mode(e, &m); continue; }
        switch(d[3]){
        case 0xFC: desc_text(d, e->name); break;
        case 0xFF: desc_text(d, e->serial_text); break;
        case 0xFD:
            /* EDID 1.4 lets byte 4 add 255 to any of the four limits:
               bit 0 vmin, 1 vmax, 2 hmin, 3 hmax. A 240 Hz monitor
               needs it; a 144 Hz one does not. */
            e->have_range = 1;
            e->vmin = (uint16_t)(d[5] + ((d[4] & 0x01) ? 255u : 0u));
            e->vmax = (uint16_t)(d[6] + ((d[4] & 0x02) ? 255u : 0u));
            e->hmin_khz = (uint16_t)(d[7] + ((d[4] & 0x04) ? 255u : 0u));
            e->hmax_khz = (uint16_t)(d[8] + ((d[4] & 0x08) ? 255u : 0u));
            e->max_pclk_mhz = (uint16_t)(d[9] * 10u);
            break;
        default: break;
        }
    }

    /* Extensions: only those both announced and present. */
    nblocks = len / EDID_BLOCK;
    if(nblocks > 1u + e->extensions) nblocks = 1u + e->extensions;
    for(b = 1; b < nblocks; b++){
        const uint8_t *x = buf + b * EDID_BLOCK;
        uint8_t dtd_at;
        if(!sum_ok(x)){ e->bad_ext++; continue; }
        if(x[0] != 0x02) continue;              /* not CTA-861        */
        e->cta_blocks++;
        /* Byte 2: where the detailed timings start; 0 = none, and they
           run in 18-byte steps until a zero clock or byte 127. */
        dtd_at = x[2];
        if(dtd_at < 4 || dtd_at > 127) continue;
        while(dtd_at + 18u <= 127u){
            edid_mode_t m;
            if(!edid_dtd(x + dtd_at, &m)) break;
            m.block = (uint8_t)b;
            add_mode(e, &m);
            dtd_at = (uint8_t)(dtd_at + 18u);
        }
    }
    return e->why = EDID_OK;
}

/* ── 0.9.2: which mode `gfx on` lights ───────────────────────────────
   The first mode is the one the monitor PREFERS, and on the HP's LG it
   is 1080p144 at 346.5 MHz -- over HDMI 1.4's 340 MHz, so it needs
   HDMI 2.0 scrambling, which 0.9.2 does not do. So the rule is:

     - progressive, at or under max_khz, and at least min_w x min_h
       (the plane that is mirrored has to fit inside it);
     - of those, one within a hertz of 60 if there is one -- the
       largest, then the one listed first;
     - otherwise the largest, then the one listed first.

   -1 if nothing qualifies. Pure, like everything in this file. */
int edid_pick(const edid_t *e, uint32_t max_khz, uint32_t min_w, uint32_t min_h)
{
    int i, best = -1, best60 = 0;
    uint32_t best_area = 0;
    if(!e || e->why != EDID_OK) return -1;
    for(i = 0; i < e->nmodes; i++){
        const edid_mode_t *m = &e->modes[i];
        uint32_t area = (uint32_t)m->hactive * m->vactive;
        int at60 = m->refresh_x100 >= 5900u && m->refresh_x100 <= 6100u;
        if(m->interlaced || !m->pclk_khz || m->pclk_khz > max_khz) continue;
        if(m->hactive < min_w || m->vactive < min_h) continue;
        if(best < 0 || (at60 && !best60) ||
           (at60 == best60 && area > best_area)){
            best = i; best60 = at60; best_area = area;
        }
    }
    return best;
}
