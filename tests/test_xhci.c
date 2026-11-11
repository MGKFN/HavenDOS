/* xHCI against a controller that executes its commands.
 *
 * This is the driver the laptop depends on: that machine has no UHCI, so
 * `lspci` on it shows 00:14.0 class 0C/03/30 and nothing else carrying
 * USB. Input, removable storage and the only route to a network all sit
 * behind it. It is also the most intricate driver in the tree -- three
 * kinds of ring, a cycle bit each, contexts whose size depends on a
 * capability bit, and a controller that owns the device addresses.
 *
 * So the simulated controller below is not a mock that returns success.
 * It reads the command ring, follows Link TRBs, matches cycle bits,
 * copies input contexts into device contexts the way real hardware does,
 * executes control transfers against a simulated device with real
 * descriptors, and posts events onto the event ring. A driver that gets
 * the cycle bit wrong stops dead here exactly as it would on metal, and
 * a driver that reads a context field at the wrong offset -- the 32 vs
 * 64 byte trap -- gets rubbish, because this controller lays its
 * contexts out at the size it advertised.
 *
 * tests/test_ahci.c and tests/test_nvmerw.c are the template. The seam
 * is include/mmio.h, which exists for exactly this.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-58s got %ld\n", what, got); }
}

/* ════════════════════════════════════════════════════════════════════
   The simulated controller's register space
   ════════════════════════════════════════════════════════════════════ */
#define SIM_REGS   0x2000u
#define SIM_CAPLEN 0x40u
#define SIM_RTSOFF 0x800u
#define SIM_DBOFF  0x1000u
#define SIM_PORTS  2
#define SIM_SLOTS  8

static uint8_t g_regs[SIM_REGS] __attribute__((aligned(4096)));
#define RBASE ((uint32_t)(uintptr_t)g_regs)
#define ROP   (RBASE + SIM_CAPLEN)

static int g_ctxsz   = 32;     /* what HCCPARAMS1.CSZ advertises        */
static int g_nscratch = 2;     /* what HCSPARAMS2 demands               */
static int g_reset_count;
static int g_cnr_spins;        /* how long CNR stays set after a reset  */
static int g_cnr_stuck;        /* ...or forever, which must be refused  */
static int g_run_set;
static int g_config_slots;
static int g_doorbells;
static int g_cmds_executed;
static int g_scratch_seen;     /* DCBAA[0] was non-zero when we looked  */

/* the controller's own ring cursors */
static uint32_t g_cmd_deq;     /* address of the next command TRB       */
static int      g_cmd_cyc = 1;
static uint32_t g_evt_enq;     /* address of the next event TRB         */
static uint32_t g_evt_base, g_evt_end;
static int      g_evt_cyc = 1;

static uint8_t  g_slot_taken[SIM_SLOTS + 1];
/* v0.8.6.1: Disable Slot bookkeeping. g_enable_failed counts the times
   the controller had no free slot left -- which is what the HP started
   doing on its third `usb rescan`. */
static int      g_disables, g_disable_unowned, g_enable_failed;

static int slots_in_use(void)
{
    int s, n = 0;
    for(s = 1; s <= SIM_SLOTS; s++) if(g_slot_taken[s]) n++;
    return n;
}
static uint32_t g_slot_ctx[SIM_SLOTS + 1];   /* device context address  */
static uint32_t g_ep_deq[SIM_SLOTS + 1][32]; /* per-DCI ring cursor     */
static int      g_ep_cyc[SIM_SLOTS + 1][32];
static int      g_ep_set[SIM_SLOTS + 1][32];

/* Things real hardware notices and a forgiving simulator does not. Each
   of these was added because tools/check_xhci_suite.py injected the
   matching bug and this file did not care. */
static int g_erdp_without_ehb;   /* an ERDP write with no handler-busy  */
static int g_erstba_before_erdp; /* the ring was armed with no dequeue  */
static int g_erdp_written;
static int g_bad_ctx_entries;    /* Configure Endpoint with too few     */

/* ── v0.8.7: the DROP flags, which this simulator used to ignore ──────
   Dword 0 of the input control context is D31..D2, the endpoints the
   command is telling the controller to FORGET. This case ignored it
   entirely -- apply_input_ctx() read only the add flags -- which is why
   the suite could not see that x_configure() never dropped anything.

   That is the Disable Slot situation of v0.8.6.1 verbatim, and this
   file records the lesson in its own words there: the simulator
   "ignored this command, which is why it could not see that the driver
   never sent one ... Freeing it here is what makes the leak visible."
   Four releases later, the same hole one level down.

   Honouring it here is what makes a STALE ENDPOINT visible: an
   endpoint context the controller still holds for an alternate setting
   that is no longer selected. */
static int g_drops;              /* endpoints dropped, total            */
static int g_drop_unset;         /* ...of which were not configured     */
static int g_drop_reserved;      /* D0 or D1 set: the spec reserves them */

/* How many endpoints beyond the default pipe the controller believes
   this slot has. The number the suite checks after a setting change. */
static int sim_eps_configured(int slot)
{
    int dci, n = 0;
    if(slot < 1 || slot > SIM_SLOTS) return -1;
    for(dci = 2; dci < 32; dci++) if(g_ep_set[slot][dci]) n++;
    return n;
}
static int g_addr_without_a0;    /* Address Device with no slot context */
static int g_fail_next_cmd;      /* force one command to be refused     */
static int g_fail_cmd_type;      /* ...or the next one of a given type  */
static int g_ep0_mps_wrong;      /* host talked at the wrong ep0 size   */

/* ── the simulated DEVICE on port 0: a boot-protocol keyboard ──────── */
static const uint8_t DEV_DESC[18] = {
    18, 1, 0x00,0x02, 0,0,0, 8,          /* ep0 max 8 -> forces EVAL_CTX */
    0x3A,0x1D, 0x34,0x12, 0x00,0x01, 0,0,0, 1
};

/* ── v0.8.6.7: A SUPERSPEED DEVICE ───────────────────────────────────
   At SuperSpeed, bMaxPacketSize0 is an EXPONENT: the specification
   fixes it at 9, meaning 2^9 = 512 bytes. Read as a byte count it means
   a nine-byte control endpoint, which is what hc0 port 12 on the HP has
   been getting since v0.8.5.3 -- and what any USB 3 stick gets in a
   socket that trains at SuperSpeed.

   g_ss serves this descriptor and makes the port report speed id 4.
   The ep0 guard further down then expects 512, not 9, so a host that
   programs the raw byte is STALLED exactly as a real controller
   stalls it. */
static const uint8_t DEV_DESC_SS[18] = {
    18, 1, 0x00,0x03, 0,0,0, 9,          /* 9 == 2^9 == 512 bytes      */
    0x51,0x09, 0x62,0x16, 0x00,0x01, 0,0,0, 1
};
static int g_ss;                      /* the port trains at SuperSpeed */
/* Overrides DEV_DESC_SS[7] when non-zero, so a SuperSpeed device can be
   made to say something the specification does not allow. */
static int g_ss_mps_byte;
static const uint8_t CFG_DESC[] = {
    9, 2, 34,0, 1, 1, 0, 0xA0, 50,        /* config, wTotalLength 34    */
    9, 4, 0, 0, 1, 3, 1, 1, 0,            /* interface: HID/boot/kbd    */
    9, 0x21, 0x11,0x01, 0, 1, 0x22, 63,0, /* HID descriptor             */
    7, 5, 0x81, 3, 8,0, 10                /* ep 0x81, interrupt in, 8   */
};
/* ── v0.8.6.4: a device with a SECOND configuration ───────────────────
   An iPhone enumerated on the HP as class 06, Still Image -- PTP, which
   is its default. USB Ethernet tethering is in another configuration,
   and until this release nothing here could ask for one: the
   GET_DESCRIPTOR below ignored wValue's low byte, which IS the
   descriptor index, so every index returned configuration 0 and a test
   could not tell a working index from a hardcoded one.

   This blob stands in for the interesting one: three interfaces, one of
   them an alternate setting, and one vendor-specific 255/253/1 -- which
   is what Apple's tethering interface actually is. */
static const uint8_t CFG_DESC2[] = {
    9, 2, 43,0, 2, 2, 0, 0xA0, 50,        /* config 2, wTotalLength 43  */
    9, 4, 0, 0, 0, 6, 1, 1, 0,            /* if 0: still image (PTP)    */
    9, 4, 1, 0, 0, 0xFF, 0xFD, 0x01, 0,   /* if 1: APPLE USB ETHERNET   */
    9, 4, 1, 1, 0, 0xFF, 0xFD, 0x01, 0,   /* if 1 alt 1: same, alt      */
    7, 5, 0x82, 2, 64,0, 0                /* a bulk in on the last one  */
};
/* ── v0.8.7: THE SHAPE THE iPHONE ACTUALLY HAS ────────────────────────
   `usb cfg` on the HP, 25 October, configuration index 3:

     interface 0        class 06/01/01  3 endpoints
     interface 1        class FF/FE/02  2 endpoints   usbmux
     interface 2        class FF/FD/01  0 endpoints   <- Apple Ethernet
     interface 2 alt 1  class FF/FD/01  2 endpoints   <- Apple Ethernet
     interface 2 alt 2  class FF/FD/01  2 endpoints   <- Apple Ethernet

   Two things in that are the whole reason alternate settings are new
   work rather than a flag:

     - ALT 0 OF THE ETHERNET INTERFACE HAS NO ENDPOINTS. An interface
       that exists to be switched away from, which makes the walk's old
       "only alt 0 is taken" exactly wrong here rather than incomplete.

     - ALT 1 AND ALT 2 USE DIFFERENT ENDPOINT ADDRESSES. That is what
       makes a ring leak observable: switching between them allocates a
       ring per address, and XHCI_EP_RINGS is five.

   The addresses here are 0x83/0x04 on alt 1 and 0x85/0x06 on alt 2 --
   DCIs 7/8 and 11/12 -- so neither setting's rings can be mistaken for
   the other's, and a drop that does not happen shows up as an endpoint
   context the controller still holds. */
static const uint8_t CFG_DESC3[] = {
    9, 2, 126,0, 3, 3, 0, 0xA0, 50,       /* config 3, wTotalLength 126 */
    9, 4, 0, 0, 1, 6, 1, 1, 0,            /* if 0: still image (PTP)    */
    7, 5, 0x81, 2, 64,0, 0,               /*   ep 0x81 bulk in          */
    9, 4, 1, 0, 2, 0xFF, 0xFE, 0x02, 0,   /* if 1: usbmux               */
    7, 5, 0x82, 2, 64,0, 0,               /*   ep 0x82 bulk in          */
    7, 5, 0x03, 2, 64,0, 0,               /*   ep 0x03 bulk out         */
    9, 4, 2, 0, 0, 0xFF, 0xFD, 0x01, 0,   /* if 2 alt 0: NO ENDPOINTS   */
    9, 4, 2, 1, 2, 0xFF, 0xFD, 0x01, 0,   /* if 2 alt 1: ethernet       */
    7, 5, 0x83, 2, 0x00,0x02, 0,          /*   ep 0x83 bulk in  512     */
    7, 5, 0x04, 2, 0x00,0x02, 0,          /*   ep 0x04 bulk out 512     */
    9, 4, 2, 2, 2, 0xFF, 0xFD, 0x01, 0,   /* if 2 alt 2: ethernet       */
    7, 5, 0x85, 2, 64,0, 0,               /*   ep 0x85 bulk in  (DCI 11)*/
    7, 5, 0x06, 2, 64,0, 0,               /*   ep 0x06 bulk out (DCI 12)*/
    /* ── alt 3 SHARES ep 0x83 with alt 1, and that is the whole point.
       Switching between two settings whose endpoint addresses all
       differ FREES every ring, and ring_free() zeroes the cursor -- so
       the ring that comes back is clean whether or not the configure
       re-laid it. The harness caught that: removing ring_relay() was
       MISSED, because ring_free() was covering for it. Two guards
       covering each other, which is v0.8.6.7's pair of bugs again.

       A setting that KEEPS an endpoint is the only case where re-laying
       is observable, so there is one in the fixture. */
    9, 4, 2, 3, 2, 0xFF, 0xFD, 0x01, 0,   /* if 2 alt 3: ethernet       */
    7, 5, 0x83, 2, 0x00,0x02, 0,          /*   ep 0x83 AGAIN    (DCI 7) */
    7, 5, 0x06, 2, 64,0, 0                /*   ep 0x06 bulk out (DCI 12)*/
};

/* A device that offers FOUR configurations and boots into index 0,
   which is what the phone does. Off by default, so every test written
   before v0.8.7 still meets a device with one. */
static int g_multicfg;

static int g_cfg_index_seen = -1;     /* what index the host asked for */

/* ── SET_INTERFACE, which this device had never been asked for ────────
   The request the walk's old refusal meant could never be sent. */
static int g_set_if_alt  = -1;    /* wValue of the last SET_INTERFACE  */
static int g_set_if_num  = -1;    /* its wIndex                        */
static int g_set_ifs;             /* how many have been sent           */
static int g_set_if_stall;        /* make the device refuse the next    */
static int g_set_if_recip_bad;    /* ...sent to the DEVICE, not the if  */

/* The survey's observer. It RECORDS; the assertions then read what it
   recorded, so what is checked is what the walk REPORTED and not what
   the blob happens to contain. A test that re-read the blob would pass
   with a walk that never called back at all. */
#define SEEN_MAX 8
static int     g_seen_n;
static uint8_t g_seen_num[SEEN_MAX], g_seen_alt[SEEN_MAX];
static uint8_t g_seen_cls[SEEN_MAX], g_seen_sub[SEEN_MAX];
static uint8_t g_seen_pro[SEEN_MAX], g_seen_nep[SEEN_MAX];
static void survey_cb(void *ctx, uint8_t num, uint8_t alt, uint8_t nep,
                      uint8_t cls, uint8_t sub, uint8_t pro)
{
    (void)ctx;
    if(g_seen_n >= SEEN_MAX){ g_seen_n++; return; }
    g_seen_num[g_seen_n] = num; g_seen_alt[g_seen_n] = alt;
    g_seen_cls[g_seen_n] = cls; g_seen_sub[g_seen_n] = sub;
    g_seen_pro[g_seen_n] = pro; g_seen_nep[g_seen_n] = nep;
    g_seen_n++;
}

static uint8_t g_report[8] = { 0,0, 0x04,0,0,0,0,0 };   /* 'a' held     */

/* A scratch buffer for the bulk transfers the alternate-setting tests
   make. Named for what it is: somewhere for bytes to land, never read. */
static uint8_t g_cfgbuf_probe[64];

/* ── v0.8.5.1: the device is allowed to have NOTHING TO SAY ──────────
   Until this release the simulated device completed every Normal TRB
   the instant the doorbell rang -- it always had a report ready. A real
   interrupt IN endpoint with no key pressed NAKs, the controller retries
   the same TRB for as long as it is queued, and NO event is posted.
   That difference is the whole of the bug v0.8.5 shipped: the driver
   waited out a 1000 ms timeout on every poll and left the TRB live.
   A simulator that always answers cannot show it. */
static int g_nak[SIM_SLOTS + 1][32];
static int g_ep_stall[SIM_SLOTS + 1][32];
static int g_clear_halt_seen;

/* v0.8.7.5: an endpoint that has gone silent. No event is EVER posted
   and the TRB stays live, which is what an xHCI retrying an unanswered
   transfer does -- and what an untrusted iPhone's bulk endpoints do.
   g_nak above models the same thing for a polled interrupt endpoint;
   this one is for the synchronous paths, where the caller times out and
   must leave nothing behind for the next one. */
static int g_ep_dead[SIM_SLOTS + 1][32];
static int g_stops;      /* Stop Endpoint commands executed    */
static int g_deqsets;    /* Set TR Dequeue commands executed   */

/* v0.8.7.5: a transfer event that lands BEHIND a command completion.
   On hardware this is ordinary -- a polled keyboard finishes while an
   Enable Slot is in flight -- and it is the one arrangement that tells
   apart reading a command's reply from the event ring cursor and reading
   it from the completion itself. Set it and the next Enable Slot is
   followed by a stray transfer event naming slot 0, which is not a slot
   any device can be given. */
static int g_trail_evt;

/* ── v0.8.5.2: a firmware that owns the controller ───────────────────
   Every machine that boots firmware starts with the BIOS driving the
   xHCI: an SMM handler reads the USB keyboard and presents it at ports
   0x60/0x64 as a PS/2 keyboard, which is how anything can be typed in
   GRUB. The xHCI specification's USB Legacy Support capability is how an
   OS takes it over, and v0.8.5.1 never used it -- so on real hardware
   the firmware kept emulating while the driver also drove the
   controller, and every keystroke arrived TWICE.

   QEMU has no SMM legacy emulation, so none of this existed in the
   simulator and the release looked finished. It does now. */
#define SIM_XECP   0x500u          /* where the capability list starts */
#define SIM_LEGOFF (SIM_XECP + 0x10u)  /* ...and where LEGACY actually is */
static int g_bios_owns;            /* firmware claims the controller  */
static int g_bios_refuses;         /* ...and will not let go of it    */
static int g_no_legacy_cap;        /* no capability at all            */
static int      g_legsup_writes;       /* how often USBLEGSUP was written */
static int      g_legsup_before_reset; /* ...and was it before HCRST?     */
static uint32_t g_legctl_last;         /* what was left in USBLEGCTLSTS   */
/* Real firmware does not drop its ownership bit on the same cycle the OS
   asks. It takes a moment, and a driver that does not wait sees a BIOS
   that still owns the controller. Three reads is enough to tell a driver
   that waits from one that does not. */
static int      g_bios_delay;
static int     g_set_config_value = -1;
static int     g_set_config_count;     /* v0.8.9.9: how many were sent  */
static uint32_t g_last_add;            /* v0.8.9.9: last Configure's A  */
static int     g_set_protocol_seen;

/* ── register helpers for the simulator itself ─────────────────────── */
static uint32_t rd(uint32_t off){ uint32_t v; memcpy(&v, g_regs+off, 4); return v; }
static void     wr(uint32_t off, uint32_t v){ memcpy(g_regs+off, &v, 4); }
static uint32_t m32(uint32_t a){ uint32_t v; memcpy(&v,(void*)(uintptr_t)a,4); return v; }
static void     mw32(uint32_t a, uint32_t v){ memcpy((void*)(uintptr_t)a,&v,4); }

static void post_event(uint32_t lo, uint32_t hi, uint32_t status,
                       uint32_t control)
{
    mw32(g_evt_enq + 0, lo);
    mw32(g_evt_enq + 4, hi);
    mw32(g_evt_enq + 8, status);
    mw32(g_evt_enq + 12, control | (g_evt_cyc ? 1u : 0u));
    g_evt_enq += 16;
    if(g_evt_enq >= g_evt_end){ g_evt_enq = g_evt_base; g_evt_cyc ^= 1; }
}

#define TY(c) (((c) >> 10) & 0x3Fu)

/* Copy an input context's slot and endpoint contexts into the device
   context, the way Address Device and Configure Endpoint do. Laid out at
   g_ctxsz, so a driver that assumed the other size reads the wrong
   dwords -- which is the point. */
static void apply_input_ctx(int slot, uint32_t in, int addressing)
{
    uint32_t add = m32(in + 4);            /* add-flags dword 1         */
    uint32_t dev = g_slot_ctx[slot];
    int dci;

    if(!addressing) g_last_add = add;

    if(!dev) return;
    if(add & 1u){                           /* A0: slot context          */
        memcpy((void*)(uintptr_t)dev,
               (void*)(uintptr_t)(in + (uint32_t)g_ctxsz), (size_t)g_ctxsz);
    }
    for(dci = 1; dci < 32; dci++){
        if(!(add & (1u << dci))) continue;
        memcpy((void*)(uintptr_t)(dev + (uint32_t)dci * (uint32_t)g_ctxsz),
               (void*)(uintptr_t)(in + (uint32_t)(dci + 1) * (uint32_t)g_ctxsz),
               (size_t)g_ctxsz);
        /* remember where that endpoint's ring is, and its cycle state */
        {
            uint32_t epc = in + (uint32_t)(dci + 1) * (uint32_t)g_ctxsz;
            uint32_t deq = m32(epc + 8);
            g_ep_deq[slot][dci] = deq & ~0xFu;
            g_ep_cyc[slot][dci] = (int)(deq & 1u);
            g_ep_set[slot][dci] = 1;
        }
    }
    if(addressing){
        /* The controller assigns the address and writes it into dword 3
           of the slot context. */
        uint32_t d3 = m32(dev + 12);
        mw32(dev + 12, (d3 & 0xFFFFFF00u) | (uint32_t)slot);
    }
}

/* ── control transfer against the simulated device ─────────────────── */
static int device_control(uint8_t rtype, uint8_t req, uint16_t value,
                          uint16_t index, uint32_t buf, uint16_t len,
                          int *out_len)
{
    /* v0.8.7: wIndex is READ now. SET_INTERFACE carries the interface
       number in it, and a host that put the alternate setting there
       instead would look, to every other request, exactly like one that
       got it right. */
    *out_len = 0;
    if((rtype & 0x80u) && req == 6){                 /* GET_DESCRIPTOR  */
        const uint8_t *src = 0; int n = 0;
        if((value >> 8) == 1){
            src = g_ss ? DEV_DESC_SS : DEV_DESC; n = 18;
            if(g_ss && g_ss_mps_byte){
                static uint8_t mod[18];
                memcpy(mod, DEV_DESC_SS, 18);
                mod[7] = (uint8_t)g_ss_mps_byte;
                src = mod;
            }
            /* bNumConfigurations is the LAST byte. usb_select_config()
               refuses an index past it, so a device that offers more
               than one has to say so. */
            if(g_multicfg){
                static uint8_t mcd[18];
                memcpy(mcd, src, 18);
                mcd[17] = 3;
                src = mcd;
            }
        }
        else if((value >> 8) == 2){
            /* wValue's LOW byte is the descriptor index. Honoured now,
               so a survey of index 1 can be told from a hardcoded 0. */
            g_cfg_index_seen = (int)(value & 0xFFu);
            if(g_cfg_index_seen == 0){ src = CFG_DESC;  n = (int)sizeof CFG_DESC; }
            else if(g_cfg_index_seen == 1){ src = CFG_DESC2; n = (int)sizeof CFG_DESC2; }
            else if(g_multicfg && g_cfg_index_seen == 2){
                src = CFG_DESC3; n = (int)sizeof CFG_DESC3; }
            else return 6;                            /* no such config */
        }
        else return 6;                                /* stall          */
        if(n > (int)len) n = (int)len;
        if(buf) memcpy((void*)(uintptr_t)buf, src, (size_t)n);
        *out_len = n;
        return 1;
    }
    if(!(rtype & 0x80u) && req == 9){                /* SET_CONFIGURATION */
        g_set_config_value = (int)value;
        g_set_config_count++;
        return 1;
    }
    /* SET_INTERFACE. The recipient must be the INTERFACE -- bits 4:0 of
       bmRequestType == 1. A host that sends it to the DEVICE gets a
       stall from real firmware, and that stall looks exactly like a
       device with no alternate settings, so the simulated device
       records the mistake rather than quietly accepting it. */
    if(!(rtype & 0x80u) && req == 11){
        if((rtype & 0x1Fu) != 0x01u){ g_set_if_recip_bad++; return 6; }
        if(g_set_if_stall){ g_set_if_stall = 0; return 6; }
        g_set_if_alt = (int)value;
        g_set_if_num = (int)index;
        g_set_ifs++;
        return 1;
    }
    if(!(rtype & 0x80u) && (rtype & 0x60u) == 0x20u && req == 0x0B){
        g_set_protocol_seen = 1;                     /* HID SET_PROTOCOL */
        return 1;
    }
    /* CLEAR_FEATURE(ENDPOINT_HALT). A device that stalls this one makes
       usb_clear_halt() return early WITHOUT resetting the endpoint,
       which is why the stall test looked like it proved something it
       did not. Accepting it is what a device does. (v0.8.5.1) */
    if(!(rtype & 0x80u) && (rtype & 0x1Fu) == 0x02u && req == 1 &&
       value == 0){
        g_clear_halt_seen++;
        return 1;
    }
    return 6;                                        /* anything else stalls */
}

/* ── execute one endpoint ring ─────────────────────────────────────── */
static void run_ep_ring(int slot, int dci)
{
    int guard = 0;
    uint8_t  rtype = 0, req = 0;
    uint16_t value = 0, index = 0, wlen = 0;
    uint32_t data_buf = 0; uint32_t data_len = 0;
    int have_setup = 0;

    if(!g_ep_set[slot][dci]) return;

    while(guard++ < 64){
        uint32_t t = g_ep_deq[slot][dci];
        uint32_t lo = m32(t), hi = m32(t+4), st = m32(t+8), c = m32(t+12);
        if((int)(c & 1u) != g_ep_cyc[slot][dci]) break;    /* not ours yet */

        if(TY(c) == 6){                                    /* Link        */
            g_ep_deq[slot][dci] = lo & ~0xFu;
            if(c & 2u) g_ep_cyc[slot][dci] ^= 1;            /* toggle      */
            continue;
        }
        if(g_ep_dead[slot][dci]) break;     /* silent: no event, TRB lives */
        g_ep_deq[slot][dci] = t + 16;

        if(TY(c) == 2){                                    /* Setup       */
            rtype = (uint8_t)(lo & 0xFFu);
            req   = (uint8_t)((lo >> 8) & 0xFFu);
            value = (uint16_t)(lo >> 16);
            index = (uint16_t)(hi & 0xFFFFu);
            wlen  = (uint16_t)(hi >> 16);
            have_setup = 1;
            (void)st;
        } else if(TY(c) == 3){                             /* Data        */
            data_buf = lo; data_len = st & 0x1FFFFu;
        } else if(TY(c) == 4){                             /* Status      */
            int got = 0, cc;
            /* A device is reached in packets of the size the HOST put in
               the endpoint context. The first descriptor read asks for
               exactly eight bytes and fits either way, which is why it
               must be allowed -- it is how the real size is learned. A
               LONGER transfer cannot be carried to an 8-byte endpoint by
               a host that believes it is 64, and a real device does not
               answer it.

               Added because tools/check_xhci_suite.py deleted the ep0
               packet-size correction and this file did not care. */
            if(dci == 1 && g_slot_ctx[slot] && wlen > 8){
                uint32_t epc = g_slot_ctx[slot] + (uint32_t)g_ctxsz;
                uint32_t mps = m32(epc + 4) >> 16;
                /* The size the host must have programmed. At
                   SuperSpeed that is 512, NOT the 9 in the descriptor
                   -- a host that copies the byte straight through
                   programs a nine-byte endpoint and gets stalled, which
                   is the whole point of this arm. */
                uint32_t want = g_ss ? 512u : (uint32_t)DEV_DESC[7];
                if(mps != 0 && mps != want){
                    g_ep0_mps_wrong = 1;
                    post_event(t, 0, (6u << 24),         /* Stall */
                               (32u << 10) | ((uint32_t)slot << 24) |
                               ((uint32_t)dci << 16));
                    have_setup = 0; data_buf = 0; data_len = 0;
                    continue;
                }
            }
            if(!have_setup){ cc = 5; }
            else cc = device_control(rtype, req, value, index,
                                     data_buf, wlen, &got);
            {
                uint32_t residue = (data_len > (uint32_t)got)
                                 ? (data_len - (uint32_t)got) : 0;
                post_event(t, 0,
                           (residue & 0xFFFFFFu) | ((uint32_t)cc << 24),
                           (32u << 10) | ((uint32_t)slot << 24) |
                           ((uint32_t)dci << 16));
            }
            have_setup = 0; data_buf = 0; data_len = 0;
        } else if(TY(c) == 1){                             /* Normal      */
            uint32_t n = st & 0x1FFFFu;
            if(g_nak[slot][dci]){
                /* A NAK is not a completion. The controller leaves the
                   TRB where it is and tries again later, so the dequeue
                   pointer goes BACK and nothing else on this ring runs. */
                g_ep_deq[slot][dci] = t;
                break;
            }
            if(g_ep_stall[slot][dci]){
                post_event(t, 0, (n & 0xFFFFFFu) | (6u << 24),
                           (32u << 10) | ((uint32_t)slot << 24) |
                           ((uint32_t)dci << 16));
                continue;
            }
            uint32_t give = (n < sizeof g_report) ? n : sizeof g_report;
            if(lo) memcpy((void*)(uintptr_t)lo, g_report, give);
            post_event(t, 0,
                       ((n - give) & 0xFFFFFFu) | (1u << 24),
                       (32u << 10) | ((uint32_t)slot << 24) |
                       ((uint32_t)dci << 16));
        }
    }
}

/* Choose what the firmware is doing, AFTER sim_reset() has laid the
   registers down -- it re-lays HCCPARAMS1's xECP field and the capability
   itself, because sim_reset() read these flags when it ran. */
static void sim_bios(int owns, int refuses, int nocap)
{
    g_bios_owns = owns; g_bios_refuses = refuses; g_no_legacy_cap = nocap;
    wr(0x10, (rd(0x10) & 0xFFFFu) |
             (nocap ? 0u : ((SIM_XECP / 4u) << 16)));
    if(!nocap){
        /* Same two-capability layout sim_reset() lays: a Supported
           Protocol entry first, legacy support SECOND. One copy of this
           in two places is how the first version of this helper quietly
           disagreed with the simulator it was configuring. */
        wr(SIM_XECP, 2u | (4u << 8));
        wr(SIM_LEGOFF, 1u | (owns ? (1u << 16) : 0u));
        wr(SIM_LEGOFF + 4u, 0x0000E011u | 0xE0000000u);
    }
    g_legsup_writes = g_legsup_before_reset = 0;
    g_legctl_last = 0xFFFFFFFFu;
    g_bios_delay = 3;
}

/* The device finally has something to report, and the controller
   completes the transfer that has been sitting queued on its ring. No
   doorbell is rung -- that is exactly the point: a completion can arrive
   between two polls, with nothing the driver did to cause it. */
static void sim_ep_report(int slot, int dci, const uint8_t *r, int n)
{
    int was = g_nak[slot][dci];
    if(r) memcpy(g_report, r, (size_t)n);
    g_nak[slot][dci] = 0;
    run_ep_ring(slot, dci);
    g_nak[slot][dci] = was;
}

/* ── execute the command ring ──────────────────────────────────────── */
static void run_cmd_ring(void)
{
    int guard = 0;
    while(guard++ < 64){
        uint32_t t = g_cmd_deq;
        uint32_t lo = m32(t), c = m32(t+12);
        if((int)(c & 1u) != g_cmd_cyc) break;

        if(TY(c) == 6){                                /* Link            */
            g_cmd_deq = lo & ~0xFu;
            if(c & 2u) g_cmd_cyc ^= 1;
            continue;
        }
        g_cmd_deq = t + 16;
        g_cmds_executed++;

        if(g_fail_next_cmd || (g_fail_cmd_type &&
                               (int)TY(c) == g_fail_cmd_type)){
            g_fail_next_cmd = 0;
            g_fail_cmd_type = 0;
            /* A refused command still names its slot, as hardware does --
               the completion code is what says it failed. */
            post_event(t, 0, (5u << 24),            /* TRB Error */
                       (33u << 10) | (c & 0xFF000000u));
            continue;
        }

        switch(TY(c)){
        case 9: {                                      /* Enable Slot     */
            int s;
            for(s = 1; s <= SIM_SLOTS; s++) if(!g_slot_taken[s]) break;
            if(s > SIM_SLOTS){ g_enable_failed++;
                               post_event(t,0,(17u<<24),(33u<<10)); break; }
            g_slot_taken[s] = 1;
            {   /* DCBAA[s] is where the device context lives */
                uint32_t dcbaa = rd(SIM_CAPLEN + 0x30);
                g_slot_ctx[s] = m32(dcbaa + (uint32_t)s * 8u);
                if(m32(dcbaa) != 0) g_scratch_seen = 1;
            }
            post_event(t, 0, (1u << 24), (33u << 10) | ((uint32_t)s << 24));
            if(g_trail_evt){
                g_trail_evt = 0;
                post_event(0, 0, (1u << 24), (32u << 10) | (0u << 24) |
                                             (1u << 16));
            }
            break;
        }
        case 11: {                                     /* Address Device  */
            int s = (int)((c >> 24) & 0xFFu);
            uint32_t in = lo & ~0xFu;
            if(!(m32(in + 4) & 1u)){
                /* No A0: the slot context was not presented. Real
                   hardware refuses this with a parameter error rather
                   than addressing a device it was not described. */
                g_addr_without_a0 = 1;
                post_event(t, 0, (17u << 24),
                           (33u << 10) | ((uint32_t)s << 24));
                break;
            }
            apply_input_ctx(s, in, 1);
            post_event(t, 0, (1u << 24), (33u << 10) | ((uint32_t)s << 24));
            break;
        }
        case 12: {                                     /* Configure EP    */
            int s = (int)((c >> 24) & 0xFFu);
            uint32_t in = lo & ~0xFu;
            uint32_t drop = m32(in + 0);               /* D31..D2        */
            uint32_t add = m32(in + 4);
            int hi_dci = 0, k;
            uint32_t entries;
            for(k = 1; k < 32; k++) if(add & (1u << k)) hi_dci = k;
            /* Context Entries lives in bits 31:27 of slot-context dword
               0, and the command is refused if it does not reach the
               highest endpoint being added. */
            entries = m32(in + (uint32_t)g_ctxsz) >> 27;
            if((int)entries < hi_dci){
                g_bad_ctx_entries = 1;
                post_event(t, 0, (17u << 24),
                           (33u << 10) | ((uint32_t)s << 24));
                break;
            }
            /* Drops are applied BEFORE adds, which is the order the
               specification gives and the order that matters: a command
               that drops DCI 3 and adds DCI 3 is how an endpoint's
               packet size is changed, and applying the add first would
               then throw it away. */
            if(drop & 3u) g_drop_reserved++;     /* D0/D1 are reserved  */
            for(k = 2; k < 32; k++){
                if(!(drop & (1u << k))) continue;
                g_drops++;
                if(!g_ep_set[s][k]) g_drop_unset++;
                g_ep_set[s][k]  = 0;
                g_ep_deq[s][k]  = 0;
                g_ep_cyc[s][k]  = 0;
                memset((void*)(uintptr_t)(g_slot_ctx[s] +
                                          (uint32_t)k * (uint32_t)g_ctxsz),
                       0, (size_t)g_ctxsz);
            }
            apply_input_ctx(s, in, 0);
            g_config_slots++;
            post_event(t, 0, (1u << 24), (33u << 10) | ((uint32_t)s << 24));
            break;
        }
        case 13:                                       /* Evaluate Ctx    */
            { int s = (int)((c >> 24) & 0xFFu);
              apply_input_ctx(s, lo & ~0xFu, 0);
              post_event(t, 0, (1u << 24), (33u << 10) | ((uint32_t)s << 24)); }
            break;
        case 10: {                                     /* Disable Slot    */
            /* v0.8.6.1. The simulator ignored this command, which is
               why it could not see that the driver never sent one: a
               slot was taken for ever and the only symptom was
               Enable Slot eventually refusing -- after more
               enumerations than any test performed.

               Freeing it here is what makes the leak visible. */
            int s = (int)((c >> 24) & 0xFFu);
            if(s < 1 || s > SIM_SLOTS){
                post_event(t, 0, (17u << 24), (33u << 10));
                break;
            }
            if(!g_slot_taken[s]) g_disable_unowned++;
            g_slot_taken[s] = 0;
            g_disables++;
            post_event(t, 0, (1u << 24),
                       (33u << 10) | ((uint32_t)s << 24));
            break;
        }
        case 15: {                                     /* Stop Endpoint   */
            int s2  = (int)((c >> 24) & 0xFFu);
            int dci = (int)((c >> 16) & 0x1Fu);
            /* The controller gives up on the TRB it was retrying and
               posts a Transfer Event for it: CC 26, Stopped. By the time
               that arrives the caller has long since timed out, so it
               belongs to nobody -- which is precisely the event the
               driver must drop instead of handing to the next caller. */
            if(s2 >= 1 && s2 <= SIM_SLOTS && dci < 32 && g_ep_set[s2][dci])
                post_event(g_ep_deq[s2][dci], 0, (26u << 24),
                           (32u << 10) | ((uint32_t)s2 << 24) |
                           ((uint32_t)dci << 16));
            g_stops++;
            post_event(t, 0, (1u << 24), (33u << 10) | (c & 0xFF000000u));
            break;
        }
        case 16: {                                     /* Set TR Dequeue  */
            int s2  = (int)((c >> 24) & 0xFFu);
            int dci = (int)((c >> 16) & 0x1Fu);
            if(s2 >= 1 && s2 <= SIM_SLOTS && dci < 32){
                g_ep_deq[s2][dci] = lo & ~0xFu;
                g_ep_cyc[s2][dci] = (int)(lo & 1u);
            }
            g_deqsets++;
            post_event(t, 0, (1u << 24), (33u << 10) | (c & 0xFF000000u));
            break;
        }
        case 14:                                       /* Reset Endpoint  */
        case 23:                                       /* No Op           */
        default:
            post_event(t, 0, (1u << 24), (33u << 10) | (c & 0xFF000000u));
            break;
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
   The seam
   ════════════════════════════════════════════════════════════════════ */
#define MMIO_H
/* v0.8.5.1 -- the mapped window has an EXTENT, and this models it.

   When the BAR is above 4 GB the driver maps only `bytes` of it, and an
   access past that lands outside the window. On hardware that is another
   device's memory, a fault, or nothing at all -- never the register the
   driver meant. The highfb_map() stub records how much was asked for, and
   every access beyond it is counted here.

   This exists because the harness planted "only one page of the BAR is
   mapped" and the suite reported it CAUGHT when it was not: the stub
   handed back a working base address no matter how little was requested,
   so a 4 KB window reached the doorbells at 0x1000 perfectly happily.
   The simulator was kinder than hardware, which is the same lesson 0.8.5
   learned ten times over. */
static int g_outside_window;
/* Tentative definitions: the highfb_map() stub that sets these lives
   further down with the other stubs, and C permits a static object to be
   declared more than once at file scope. */
static uint32_t g_map_bytes;
static int      g_map_calls;
static inline void window_check(uint32_t rel)
{
    if(g_map_calls > 0 && g_map_bytes && rel >= g_map_bytes)
        g_outside_window++;
}

static inline uint32_t mmio_read32(uint32_t base, uint32_t off)
{
    uint32_t a = base + off;
    uint32_t rel = a - RBASE;
    window_check(rel);
    if(rel < SIM_REGS){
        /* USBSTS: CNR clears after g_cnr_spins reads, so a driver that
           does not wait for it is reading a controller that is not
           ready -- trap 4. */
        if(rel == SIM_CAPLEN + 0x04u){
            uint32_t v = rd(rel);
            if(v & (1u<<11)){ if(g_cnr_spins > 0 && --g_cnr_spins == 0)
                                  wr(rel, v & ~(1u<<11)); }
            return rd(rel);
        }
        /* USBLEGSUP: the firmware drops its ownership bit a few reads
           after being asked, not instantly. A driver that sets the bit
           and does not wait sees a BIOS that still owns the controller
           -- which is what hardware does and what the first version of
           this simulator did not. (v0.8.5.2) */
        if(rel == SIM_LEGOFF){
            uint32_t v = rd(rel);
            if((v & (1u << 24)) && (v & (1u << 16)) && !g_bios_refuses){
                if(g_bios_delay > 0 && --g_bios_delay == 0)
                    wr(rel, v & ~(1u << 16));
            }
            return rd(rel);
        }
        return rd(rel);
    }
    return m32(a);
}
static inline void mmio_write32(uint32_t base, uint32_t off, uint32_t v)
{
    uint32_t a = base + off;
    uint32_t rel = a - RBASE;
    window_check(rel);
    if(rel >= SIM_REGS){ mw32(a, v); return; }

    /* the doorbells */
    if(rel >= SIM_DBOFF && rel < SIM_DBOFF + 4u*(SIM_SLOTS+1)){
        uint32_t slot = (rel - SIM_DBOFF) / 4u;
        g_doorbells++;
        wr(rel, v);
        if(slot == 0) run_cmd_ring();
        else          run_ep_ring((int)slot, (int)(v & 0xFFu));
        return;
    }
    /* ── USBLEGSUP: the BIOS handshake (v0.8.5.2) ────────────────────
       The OS asks for the controller by setting bit 24. A cooperative
       firmware answers by dropping bit 16. One that refuses keeps it,
       and keeps running its keyboard emulation -- which is a real
       machine, not a hypothetical. */
    if(rel == SIM_LEGOFF){
        uint32_t cur = rd(rel);
        g_legsup_writes++;
        if(g_reset_count == 0) g_legsup_before_reset = 1;
        if(v & (1u << 24)) cur |= (1u << 24);
        wr(rel, cur);
        return;
    }
    if(rel == SIM_LEGOFF + 4u){        /* USBLEGCTLSTS: 31:29 are W1C */
        /* Bits 31:29 are write-1-to-CLEAR: they keep their value unless
           the driver writes a 1 at them. The first version of this
           cleared them on every write no matter what was written, which
           made "the latched SMIs are never acknowledged" impossible to
           detect -- the simulator was doing the driver's job for it.
           (v0.8.5.2) */
        uint32_t old_st = rd(rel) & 0xE0000000u;
        uint32_t now    = (v & ~0xE0000000u) | (old_st & ~(v & 0xE0000000u));
        wr(rel, now);
        g_legctl_last = now;
        return;
    }
    if(rel == SIM_CAPLEN + 0x00u){                 /* USBCMD             */
        if(v & 2u){                                 /* HCRST             */
            g_reset_count++;
            g_cnr_spins = g_cnr_stuck ? 0 : 3;
            wr(SIM_CAPLEN + 0x04u, (1u<<0) | (1u<<11));   /* HCH | CNR   */
            wr(rel, v & ~2u);                       /* reset self-clears */
            return;
        }
        if(v & 1u){                                 /* RS                */
            g_run_set = 1;
            wr(SIM_CAPLEN + 0x04u, rd(SIM_CAPLEN+0x04u) & ~1u);  /* ~HCH */
        }
        wr(rel, v);
        return;
    }
    if(rel == SIM_CAPLEN + 0x18u){                 /* CRCR low           */
        g_cmd_deq = v & ~0x3Fu;
        g_cmd_cyc = (int)(v & 1u);
        wr(rel, v); return;
    }
    if(rel == SIM_CAPLEN + 0x38u){ g_config_slots = 0; wr(rel, v); return; }
    if(rel == SIM_RTSOFF + 0x38u){                 /* ERDP low           */
        if(!(v & 8u)) g_erdp_without_ehb = 1;
        g_erdp_written = 1;
        wr(rel, v); return;
    }
    if(rel == SIM_RTSOFF + 0x30u){                 /* ERSTBA low         */
        if(!g_erdp_written) g_erstba_before_erdp = 1;
        uint32_t erst = v & ~0x3Fu;
        g_evt_base = m32(erst);
        g_evt_end  = g_evt_base + 16u * m32(erst + 8);
        g_evt_enq  = g_evt_base;
        g_evt_cyc  = 1;
        wr(rel, v); return;
    }
    /* PORTSC: PR sets PRC and PED, write-1-to-clear on the change bits */
    if(rel >= SIM_CAPLEN + 0x400u &&
       rel <  SIM_CAPLEN + 0x400u + 0x10u*SIM_PORTS &&
       (rel - SIM_CAPLEN - 0x400u) % 0x10u == 0){
        uint32_t cur = rd(rel);
        uint32_t keep = cur & ~(v & ((1u<<17)|(1u<<21)|(1u<<18)|(1u<<19)|
                                     (1u<<20)|(1u<<22)|(1u<<23)));
        if(v & (1u<<1)) keep &= ~(1u<<1);   /* PED: write 1 DISABLES it */
        if(v & (1u<<4)){ keep |= (1u<<21) | (1u<<1); }     /* PRC | PED  */
        if(v & (1u<<9))  keep |= (1u<<9);                  /* PP         */
        wr(rel, keep);
        return;
    }
    wr(rel, v);
}
static inline uint16_t mmio_read16(uint32_t b, uint32_t o){
    uint32_t v = mmio_read32(b, o & ~3u);
    return (uint16_t)(v >> (8u * (o & 3u)));
}
static inline void mmio_write16(uint32_t b, uint32_t o, uint16_t v){
    (void)b;(void)o;(void)v;
}
static inline uint8_t mmio_read8(uint32_t b, uint32_t o){
    uint32_t v = mmio_read32(b, o & ~3u);
    return (uint8_t)(v >> (8u * (o & 3u)));
}
static inline void mmio_write8(uint32_t b, uint32_t o, uint8_t v){
    (void)b;(void)o;(void)v;
}
static inline uint64_t mmio_read64(uint32_t b, uint32_t o){
    return (uint64_t)mmio_read32(b,o) | ((uint64_t)mmio_read32(b,o+4) << 32);
}
static inline void mmio_write64(uint32_t b, uint32_t o, uint64_t v){
    mmio_write32(b, o, (uint32_t)v);
    mmio_write32(b, o+4, (uint32_t)(v >> 32));
}

/* ════════════════════════════════════════════════════════════════════
   Stubs for everything the driver touches that is not the controller
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/pci.h"

static pci_dev_t g_pci;
static int       g_pci_enabled_busmaster;

int              pci_count(void){ return 1; }
const pci_dev_t *pci_get(int i){ return i == 0 ? &g_pci : 0; }

/* pci_bar_mem(), pci_bar_mem64() and pci_bar_is_64() are NOT stubbed:
   drivers/pci_bar.c is linked in, so this suite runs the real decoder
   over real BAR words set in sim_reset(). v0.8.5 stubbed pci_bar_mem()
   to hand back a working address, which is exactly why the suite could
   not have found the truncation that stopped the HP from booting USB.
   (v0.8.5.1) */

/* kernel/highfb.c is not in this binary. The stub records what it was
   asked for and can be made to fail, so both branches are tested. */
static uint32_t g_map_hi, g_map_lo;
static int      g_map_fail, g_map_io_calls;
static uint32_t g_map_give;
int highfb_map(uint32_t hi, uint32_t lo, uint32_t bytes, uint32_t *out){
    g_map_hi = hi; g_map_lo = lo; g_map_bytes = bytes; g_map_calls++;
    if(g_map_fail || !out) return 0;
    *out = g_map_give; return 1;
}
int highfb_map_io(uint32_t hi, uint32_t lo, uint32_t bytes, uint32_t *out){
    g_map_io_calls++;
    return highfb_map(hi, lo, bytes, out);
}
void pci_enable(const pci_dev_t *d, int bm){ (void)d; if(bm) g_pci_enabled_busmaster = 1; }

void sleep_ms(uint32_t ms){ (void)ms; }
void klog(int lvl, const char *fmt, ...){ (void)lvl; (void)fmt; }

/* the UHCI back-end is not in this binary */
int uhci_hc_register(void){ return 0; }

/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../usb/core.c"
/* v0.8.6: usb_enumerate() ends with the hub pass, so the hub class is in
   this binary too. The simulated controller reports no hub, so the pass
   finds nothing and the root-port walk these 150 checks cover is
   unchanged -- which is the point of linking it rather than stubbing
   usb_hub_scan() out: a stub would have let the hub pass break the
   enumeration without any suite noticing. tests/test_hub.c drives it. */
#include "../usb/hub.c"
#include "../usb/xhci_hc.c"

/* ════════════════════════════════════════════════════════════════════ */
static void sim_reset(int ctxsz, int nscratch, int connect_port0)
{
    memset(g_regs, 0, sizeof g_regs);
    memset(g_slot_taken, 0, sizeof g_slot_taken);
    g_disables = g_disable_unowned = g_enable_failed = 0;
    memset(g_slot_ctx, 0, sizeof g_slot_ctx);
    memset(g_ep_set, 0, sizeof g_ep_set);
    g_ctxsz = ctxsz; g_nscratch = nscratch;
    g_reset_count = g_run_set = g_doorbells = 0;
    g_cmds_executed = g_config_slots = g_scratch_seen = 0;
    g_set_config_value = -1; g_set_protocol_seen = 0;
    g_cnr_stuck = 0;
    g_erdp_without_ehb = g_erstba_before_erdp = g_erdp_written = 0;
    g_bad_ctx_entries = g_addr_without_a0 = 0;
    g_drops = g_drop_unset = g_drop_reserved = 0;
    g_multicfg = 0;
    g_set_if_alt = g_set_if_num = -1;
    g_set_ifs = g_set_if_stall = g_set_if_recip_bad = 0;
    g_fail_next_cmd = g_ep0_mps_wrong = g_fail_cmd_type = 0;
    g_pci_enabled_busmaster = 0;

    memset(&g_pci, 0, sizeof g_pci);
    g_pci.class = 0x0C; g_pci.subclass = 0x03; g_pci.progif = 0x30;
    g_pci.vendor = 0x8086; g_pci.device = 0x51ED;
    /* A plain 32-bit memory BAR by default: type bits 0, so bar[1] is
       not consulted. The 64-bit cases set these themselves. */
    g_pci.bar[0] = RBASE; g_pci.bar[1] = 0;
    g_map_hi = g_map_lo = g_map_bytes = 0;
    g_map_calls = g_map_io_calls = g_map_fail = 0;
    g_outside_window = 0;
    /* A real machine boots with the firmware owning the controller, so
       that is the default here. Every test above now runs through the
       handoff, which is the only way it gets exercised by accident the
       way hardware exercises it. (v0.8.5.2) */
    g_bios_owns = 1;
    g_bios_refuses = g_no_legacy_cap = 0;
    g_bios_delay = 3;
    g_legsup_writes = g_legsup_before_reset = 0;
    g_legctl_last = 0xFFFFFFFFu;
    memset(g_nak, 0, sizeof g_nak);
    memset(g_ep_stall, 0, sizeof g_ep_stall);
    g_clear_halt_seen = 0;
    g_map_give = RBASE;

    wr(0x00, SIM_CAPLEN | (0x0110u << 16));            /* CAPLENGTH|VER */
    wr(0x04, (uint32_t)SIM_SLOTS | (1u << 8) |
             ((uint32_t)SIM_PORTS << 24));             /* HCSPARAMS1    */
    wr(0x08, ((uint32_t)(nscratch & 0x1F) << 27));     /* HCSPARAMS2    */
    /* HCCPARAMS1: CSZ, and xECP in bits 31:16 -- an offset in DWORDS. */
    wr(0x10, ((ctxsz == 64) ? (1u << 2) : 0u) |
             (g_no_legacy_cap ? 0u : ((SIM_XECP / 4u) << 16)));
    if(!g_no_legacy_cap){
        /* TWO capabilities, with legacy support SECOND. A real
           controller's list begins with Supported Protocol entries, so a
           walk that only ever reads the first one -- or that forgets the
           next-pointer is counted in DWORDS -- finds nothing here and
           silently never performs the handoff. */
        wr(SIM_XECP, 2u | (4u << 8));           /* ID 2, next = 4 dwords */
        wr(SIM_LEGOFF, 1u | (g_bios_owns ? (1u << 16) : 0u));
        wr(SIM_LEGOFF + 4u, 0x0000E011u | 0xE0000000u);  /* enables + latched */
    }
    wr(0x14, SIM_DBOFF);
    wr(0x18, SIM_RTSOFF);
    wr(SIM_CAPLEN + 0x04, 1u);                         /* USBSTS: HCH   */

    if(connect_port0)
        wr(SIM_CAPLEN + 0x400u,
           1u | ((g_ss ? 4u : 3u) << 10));  /* CCS, SuperSpeed or high */
}

int main(void)
{
    head("bring-up, with 32-byte contexts and two scratchpad pages");
    sim_reset(32, 2, 1);
    {
        int n = xhci_hc_register();
        ck("the controller registers", n == 1, n);
        ck("PCI bus mastering was turned on", g_pci_enabled_busmaster, 0);
        ck("it was reset", g_reset_count == 1, g_reset_count);
        ck("and the run bit was set afterwards", g_run_set == 1, g_run_set);
        ck("CONFIG was written before run", rd(SIM_CAPLEN+0x38) == SIM_SLOTS,
           (long)rd(SIM_CAPLEN+0x38));
        ck("DCBAAP points somewhere", rd(SIM_CAPLEN+0x30) != 0, 0);
        ck("the command ring was given RCS=1",
           (rd(SIM_CAPLEN+0x18) & 1u) == 1u, (long)rd(SIM_CAPLEN+0x18));
        ck("ERSTSZ is one segment", rd(SIM_RTSOFF+0x28) == 1,
           (long)rd(SIM_RTSOFF+0x28));
        ck("ERDP was primed with EHB",
           (rd(SIM_RTSOFF+0x38) & 8u) != 0, (long)rd(SIM_RTSOFF+0x38));
        ck("the port was powered", (rd(SIM_CAPLEN+0x400) & (1u<<9)) != 0,
           (long)rd(SIM_CAPLEN+0x400));
    }

    head("the scratchpad buffers the controller demanded");
    {
        uint32_t dcbaa = rd(SIM_CAPLEN + 0x30);
        ck("DCBAA entry 0 holds a scratchpad array", m32(dcbaa) != 0,
           (long)m32(dcbaa));
        ck("and it points at a page, not at a device context",
           m32(dcbaa) != m32(dcbaa + 8), 0);
    }

    head("enumeration end to end, through the core");
    {
        int n = usb_enumerate();
        const usb_dev_t *d = usb_device(0);
        ck("one device was found", n == 1, n);
        ck("the device is valid", d && d->valid, 0);
        ck("vendor came off the descriptor", d && d->vendor == 0x1D3A,
           d ? d->vendor : -1);
        ck("product too", d && d->product == 0x1234, d ? d->product : -1);
        ck("the controller named itself",
           usb_hc(0) && !strcmp(usb_hc_name(usb_hc(0)), "xHCI"), 0);
        ck("speed came off PORTSC", d && d->speed == USB_SPEED_HIGH,
           d ? d->speed : -1);
        ck("the slot id was taken from the event, not guessed",
           d && d->slot >= 1 && d->slot <= SIM_SLOTS, d ? (long)d->slot : -1);
        ck("endpoint 0 packet size was corrected to the descriptor's",
           d && d->maxpacket0 == 8, d ? d->maxpacket0 : -1);
    }

    head("the configuration walk");
    {
        const usb_dev_t *d = usb_device(0);
        ck("SET_CONFIGURATION reached the device",
           g_set_config_value == 1, g_set_config_value);
        ck("the chosen configuration was recorded", d && d->config == 1,
           d ? d->config : -1);
        ck("interface class is HID", d && d->if_class == 3,
           d ? d->if_class : -1);
        ck("boot subclass", d && d->if_subclass == 1, d ? d->if_subclass : -1);
        ck("keyboard protocol", d && d->if_protocol == 1,
           d ? d->if_protocol : -1);
        ck("one endpoint", d && d->nep == 1, d ? d->nep : -1);
        ck("it is interrupt IN",
           d && d->nep && (d->ep[0].attr & 3) == USB_EP_INTR
             && (d->ep[0].addr & 0x80), 0);
        ck("with the packet size off the descriptor",
           d && d->nep && d->ep[0].mps == 8, d && d->nep ? d->ep[0].mps : -1);
    }

    head("Configure Endpoint -- the step UHCI has no counterpart for");
    {
        const usb_dev_t *d = usb_device(0);
        ck("the command was issued", g_config_slots >= 1, g_config_slots);
        ck("the controller now knows that endpoint's ring",
           d && g_ep_set[d->slot][3], 0);     /* ep1 IN -> DCI 3 */
    }

    head("an interrupt transfer on an endpoint that only exists "
         "because of it");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t rep[8];
        int r;
        memset(rep, 0, sizeof rep);
        r = usb_interrupt(d, 0x81, 1, rep, 8, 8);
        ck("it transferred eight bytes", r == 8, r);
        ck("and they are the report the device had",
           memcmp(rep, g_report, 8) == 0, 0);
    }

    head("the cycle bit, across a transfer-ring AND an event-ring wrap");
    {
        const usb_dev_t *d = usb_device(0);
        int i, ok = 1, r = 0, done = 0;
        /* 80 transfers: more than twice the transfer ring's usable
           entries, and more than the 64-entry EVENT ring, so both cycle
           bits have to toggle. Before this loop was long enough, the
           harness proved the event-ring toggle could be deleted without
           the suite noticing. */
        for(i = 0; i < 80; i++){
            uint8_t rep[8];
            r = usb_interrupt(d, 0x81, 1, rep, 8, 8);
            if(r != 8){ ok = 0; break; }
            done++;
        }
        ck("all 80 transfers completed", ok && done == 80, done);
        ck("which is past the transfer ring",
           80 > (XHCI_RING_TRBS - 1), 0);
        ck("and past the event ring", 80 > XHCI_EVT_TRBS, 0);
    }

    /* ════════════════════════════════════════════════════════════════
       v0.8.5.1 -- POLLING A KEYBOARD.

       usb_interrupt() above works because the simulated device always had
       a report ready. A real keyboard with no key pressed NAKs, and an
       xHCI retries a NAK forever rather than completing the transfer. So
       v0.8.5's HID poll waited out a full second on every poll and left a
       live TRB in a sixteen-entry ring each time. On the laptop this
       release is for -- whose only keyboard is this one -- that is a
       keyboard that accepts about one character per second.

       g_nak models it. These tests are the acceptance check for the
       non-blocking path that replaces it.
       ════════════════════════════════════════════════════════════════ */
    head("polling an endpoint with nothing to report");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t rep[8];
        int slot = d ? (int)d->slot : 0;
        int i, before, r = -1, zeros = 0;

        g_nak[slot][3] = 1;                 /* ep1 IN -> DCI 3 */
        before = g_doorbells;
        for(i = 0; i < 40; i++){
            memset(rep, 0xEE, sizeof rep);
            r = usb_intr_poll(d, 0x81, 1, rep, 8, 8);
            if(r == 0) zeros++;
        }
        ck("forty polls all answered 'nothing yet'", zeros == 40, zeros);
        ck("...which is not an error", r == 0, r);
        ck("ONE transfer was queued, not forty",
           g_doorbells - before == 1, g_doorbells - before);
        ck("...so the ring did not fill",
           g_x[0].ep_busy[slot][ring_slot_for_dci(&g_x[0], slot, 3)] == 1, 0);
    }

    head("...and the keypress that arrives between two polls");
    {
        const usb_dev_t *d = usb_device(0);
        const uint8_t keys[8] = { 0,0, 0x0B,0,0,0,0,0 };   /* 'h' held */
        uint8_t rep[8];
        int slot = d ? (int)d->slot : 0;
        int before, r;

        /* The controller completes the queued transfer on its own. */
        sim_ep_report(slot, 3, keys, 8);

        before = g_doorbells;
        memset(rep, 0, sizeof rep);
        r = usb_intr_poll(d, 0x81, 1, rep, 8, 8);
        ck("the poll returns the eight bytes", r == 8, r);
        ck("and they are what the device reported",
           memcmp(rep, keys, 8) == 0, 0);
        ck("the next transfer was queued straight away",
           g_doorbells - before == 1, g_doorbells - before);
        ck("...so a second keypress is already being waited for",
           g_x[0].ep_busy[slot][ring_slot_for_dci(&g_x[0], slot, 3)] == 1, 0);
    }

    head("a hundred polls and ten keypresses, across every ring wrap");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t rep[8];
        int slot = d ? (int)d->slot : 0;
        int i, got = 0, bad = 0;

        for(i = 0; i < 100; i++){
            int r;
            if(i % 10 == 0){
                uint8_t k[8] = { 0,0, 0x04,0,0,0,0,0 };
                k[2] = (uint8_t)(0x04 + (i / 10));
                sim_ep_report(slot, 3, k, 8);
            }
            r = usb_intr_poll(d, 0x81, 1, rep, 8, 8);
            if(r > 0) got++;
            else if(r < 0) bad++;
        }
        ck("every report was collected", got == 10, got);
        ck("and no poll failed", bad == 0, bad);
        ck("which is past the transfer ring", 100 > (XHCI_RING_TRBS - 1), 0);
        ck("and past the event ring", 100 > XHCI_EVT_TRBS, 0);
    }

    head("a poll leaves an event it does not own where it found it");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t rep[8];
        uint32_t before = g_x[0].evt_idx;
        int r;
        /* A command completion, planted directly. The event ring is one
           queue for the whole controller, so a poll WILL meet events that
           belong to somebody else -- and evt_wait() discards events of
           the wrong type, which here would lose a command's result. */
        post_event(0, 0, (1u << 24), (33u << 10));
        r = usb_intr_poll(d, 0x81, 1, rep, 8, 8);
        ck("the poll still reports nothing", r == 0, r);
        /* v0.8.7.5: the poll now CONSUMES it -- leaving it on the ring is
           what blocked every later consumer -- and files it where a
           command waiter will find it. The ring moves; the result does
           not go missing. */
        ck("the ring advanced past it",
           g_x[0].evt_idx != before, (long)g_x[0].evt_idx);
        ck("and the command's result was filed, not discarded",
           g_x[0].cmd_done == 1, g_x[0].cmd_done);
        ck("a command waiter still finds it",
           evt_wait_cmd(&g_x[0], 0, 0, 10) == 1, 0);
    }

    head("...and the same for a SYNCHRONOUS transfer's completion");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t rep[8];
        int slot = d ? (int)d->slot : 0;
        uint32_t before = g_x[0].evt_idx;
        int r;
        /* A transfer event for endpoint 0, which has a ring but never an
           asynchronous transfer outstanding on it -- so it belongs to a
           synchronous caller. Deciding ownership by "does this endpoint
           have a ring" rather than "is a transfer of MINE outstanding"
           steals it, and x_data() then waits for an event that has
           already been thrown away. */
        post_event(0, 0, (1u << 24),
                   (32u << 10) | ((uint32_t)slot << 24) | (1u << 16));
        g_x[0].ep_sync[slot][ring_slot_for_dci(&g_x[0], slot, 1)] = 1;
        r = usb_intr_poll(d, 0x81, 1, rep, 8, 8);
        ck("the poll reports nothing", r == 0, r);
        ck("the ring advanced past it",
           g_x[0].evt_idx != before, (long)g_x[0].evt_idx);
        ck("and ep0's waiter finds its result filed",
           evt_wait_ep(&g_x[0], 0, slot,
                       ring_slot_for_dci(&g_x[0], slot, 1), 0, 10) == 1, 0);
    }

    /* ─────────────────────────────────────────────────────────────────
       v0.8.7.5: AN ABANDONED TRANSFER'S EVENT IS READ AS THE NEXT
       TRANSFER'S RESULT.

       The two tests above prove evt_drain() leaves a foreign event where
       it found it. Both then consume it by hand "so the tests after this
       one start clean" -- and that hand cleanup is precisely what the
       real system never does.

       evt_wait() returns the next event of the right TYPE. It never
       checks which slot or endpoint the event belongs to. The comment
       above evt_drain() argues this is safe because cmd_run() and
       x_data() "run to completion before any poll happens". A transfer
       that TIMES OUT does not: it returns with its TRB still live in the
       ring -- which the interrupt-poll comment forty lines up states in
       so many words, about this same driver. When the controller finally
       finishes that TRB, its event arrives with no waiter, and the next
       synchronous caller reads it as its own result.
       ───────────────────────────────────────────────────────────────── */
    head("an abandoned transfer's event is not read as the next one's result");
    {
        const usb_dev_t *d = usb_device(0);
        int slot = d ? (int)d->slot : 0;
        uint8_t dd[18];
        int r;
        /* A transaction error on ep0 arriving with nobody waiting: a
           control transfer that timed out and was abandoned, which the
           controller has now finished with. */
        post_event(0, 0, (4u << 24),
                   (32u << 10) | ((uint32_t)slot << 24) | (1u << 16));
        memset(dd, 0, sizeof dd);
        r = usb_control(d, 0x80, 6, (uint16_t)(1u << 8), 0, dd, 18);
        ck("the descriptor read reports its OWN result", r == 18, r);
        ck("and the descriptor really arrived", dd[1] == 1, dd[1]);
    }

    head("...and one abandoned transfer does not desync the ring for good");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t dd[18];
        int a, b;
        memset(dd, 0, sizeof dd);
        a = usb_control(d, 0x80, 6, (uint16_t)(1u << 8), 0, dd, 18);
        memset(dd, 0, sizeof dd);
        b = usb_control(d, 0x80, 6, (uint16_t)(1u << 8), 0, dd, 18);
        ck("two reads in a row both succeed", a == 18 && b == 18,
           (long)((a != 18) ? a : b));
    }
    head("an event for an endpoint we do not know does not block the ring");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t dd[18];
        int r;
        /* Slot 7 has no rings here: nothing was ever attached there. The
           old evt_drain() RETURNED on an event it could not attribute,
           leaving it at the head of the ring -- where it stopped every
           consumer on the controller, permanently. */
        post_event(0, 0, (1u << 24),
                   (32u << 10) | (7u << 24) | (1u << 16));
        memset(dd, 0, sizeof dd);
        r = usb_control(d, 0x80, 6, (uint16_t)(1u << 8), 0, dd, 18);
        ck("the next descriptor read still works", r == 18, r);
    }

    head("a port-change event does not block the ring either");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t dd[18];
        int r;
        /* Unplug anything and the controller posts one of these. It is
           nobody's transfer and nobody's command, and both old readers
           got it wrong: evt_wait() threw it away, evt_drain() stopped
           dead on it. */
        post_event(0, 0, 0, (34u << 10));
        memset(dd, 0, sizeof dd);
        r = usb_control(d, 0x80, 6, (uint16_t)(1u << 8), 0, dd, 18);
        ck("the next descriptor read still works", r == 18, r);
    }

    head("an unowned transfer event is dropped, not filed for the next caller");
    {
        const usb_dev_t *d = usb_device(0);
        int slot = d ? (int)d->slot : 0;
        int ri = ring_slot_for_dci(&g_x[0], slot, 1);
        int took;
        /* ep0 of a real device with no waiter and no asynchronous
           transfer outstanding: a transfer whose caller timed out. Filing
           this is what made six descriptor reads return one abandoned
           DHCP write's completion code. */
        post_event(0, 0, (4u << 24),
                   (32u << 10) | ((uint32_t)slot << 24) | (1u << 16));
        took = evt_pump(&g_x[0], 0);
        ck("it was consumed", took == 1, took);
        ck("but not filed against the endpoint",
           g_x[0].ep_done[slot][ri] == 0, g_x[0].ep_done[slot][ri]);
    }

    head("a transfer that times out leaves nothing behind for the next one");
    {
        const usb_dev_t *d = usb_device(0);
        int slot = d ? (int)d->slot : 0;
        int ri = ring_slot_for_dci(&g_x[0], slot, 1);
        int stops = g_stops, deqs = g_deqsets;
        uint8_t dd[18];
        int r;

        /* The laptop, in miniature: a device that answers nothing. */
        g_ep_dead[slot][1] = 1;
        memset(dd, 0, sizeof dd);
        r = usb_control(d, 0x80, 6, (uint16_t)(1u << 8), 0, dd, 18);
        ck("the caller is told it timed out", r == USB_ETIMEOUT, r);
        ck("no waiter is left claiming the endpoint",
           g_x[0].ep_sync[slot][ri] == 0, g_x[0].ep_sync[slot][ri]);
        ck("the endpoint was stopped, not abandoned",
           g_stops > stops, (long)(g_stops - stops));
        ck("and told where to resume",
           g_deqsets > deqs, (long)(g_deqsets - deqs));

        /* The device comes back. Under v0.8.7.4 this read returned the
           timed-out transfer's completion code instead of its own. */
        g_ep_dead[slot][1] = 0;
        memset(dd, 0, sizeof dd);
        r = usb_control(d, 0x80, 6, (uint16_t)(1u << 8), 0, dd, 18);
        ck("the NEXT read reports its own result", r == 18, r);
        ck("and the descriptor really arrived", dd[1] == 1, dd[1]);
    }
    head("a stalled endpoint is reported, and polling resumes after a reset");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t rep[8];
        int slot = d ? (int)d->slot : 0;
        int ri = ring_slot_for_dci(&g_x[0], slot, 3);
        int before, r;

        g_ep_stall[slot][3] = 1;
        sim_ep_report(slot, 3, 0, 0);
        r = usb_intr_poll(d, 0x81, 1, rep, 8, 8);
        ck("the stall is reported", r == USB_ESTALL, r);
        ck("and nothing was re-queued onto a halted endpoint",
           g_x[0].ep_busy[slot][ri] == 0, g_x[0].ep_busy[slot][ri]);

        g_ep_stall[slot][3] = 0;
        usb_clear_halt(d, 0x81, 1);
        before = g_doorbells;
        r = usb_intr_poll(d, 0x81, 1, rep, 8, 8);
        ck("polling resumes afterwards", r == 0, r);
        ck("...by queueing a fresh transfer",
           g_doorbells - before == 1, g_doorbells - before);
        g_nak[slot][3] = 0;
    }

    head("resetting an endpoint with a transfer still outstanding");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t rep[8];
        int slot = d ? (int)d->slot : 0;
        int ri = ring_slot_for_dci(&g_x[0], slot, 3);
        int before, r;

        /* The case the stall test above does NOT cover: there, the stall
           arrived as a completion, so the endpoint was already marked
           idle. Here the transfer is still live when the endpoint is
           reset -- and the reset threw it away, so if nothing clears the
           flag the driver waits forever for a completion that cannot
           come, and the device is silent from then on. */
        g_nak[slot][3] = 1;
        r = usb_intr_poll(d, 0x81, 1, rep, 8, 8);
        ck("a transfer is outstanding", r == 0 &&
           g_x[0].ep_busy[slot][ri] == 1, g_x[0].ep_busy[slot][ri]);

        ck("the device accepts CLEAR_FEATURE",
           usb_clear_halt(d, 0x81, 1) == USB_OK, 0);
        ck("...so the endpoint really was reset", g_clear_halt_seen > 0,
           g_clear_halt_seen);
        ck("the reset marked the endpoint idle again",
           g_x[0].ep_busy[slot][ri] == 0, g_x[0].ep_busy[slot][ri]);

        before = g_doorbells;
        r = usb_intr_poll(d, 0x81, 1, rep, 8, 8);
        ck("so the next poll queues a fresh transfer",
           g_doorbells - before == 1, g_doorbells - before);
        ck("...and reports nothing yet", r == 0, r);
        g_nak[slot][3] = 0;
    }

    head("the command ring wraps too");
    {
        const usb_dev_t *d = usb_device(0);
        uint8_t rep[8];
        int i, before = g_cmds_executed, r;
        /* Each reset_toggle is a Reset Endpoint command. Twenty of them
           is past the 16-entry command ring, so its cycle bit has to
           toggle as well -- a separate ring with a separate cursor, and
           a separate chance to get it wrong. */
        for(i = 0; i < 20; i++) usb_reset_toggle(d, 0x81, 1);
        ck("the controller executed all twenty",
           g_cmds_executed - before >= 20, g_cmds_executed - before);
        r = usb_interrupt(d, 0x81, 1, rep, 8, 8);
        ck("and transfers still work afterwards", r == 8, r);
    }

    /* ── the survey, on the wire ──────────────────────────────────── */
    head("a survey asks the device for the index it was given");
    {
        /* The parser tests below drive usb_walk_config() with blobs in
           hand. This drives usb_survey_config() through the CONTROLLER,
           against a device that answers -- because the defect this
           whole command exists to undo is a hardcoded descriptor index,
           and only the wire can show which index was asked for. */
        const usb_dev_t *cd = usb_device(0);
        usb_dev_t *d = (usb_dev_t *)cd;

        if(!d){
            ck("a device to survey", 0, 0);
        } else {
            uint8_t cfg_before = d->config;
            uint8_t cls_before = d->if_class;

            g_cfg_index_seen = -1;
            g_seen_n = 0;
            ck("index 0 surveys",
               usb_survey_config(d, 0, survey_cb, 0) == USB_OK, 0);
            ck("...and the device was asked for index 0",
               g_cfg_index_seen == 0, g_cfg_index_seen);
            ck("...reporting the keyboard interface",
               g_seen_n == 1 && g_seen_cls[0] == 3, g_seen_n);

            g_cfg_index_seen = -1;
            g_seen_n = 0;
            ck("index 1 surveys",
               usb_survey_config(d, 1, survey_cb, 0) == USB_OK, 0);
            ck("...AND THE DEVICE WAS ASKED FOR INDEX 1",
               g_cfg_index_seen == 1, g_cfg_index_seen);
            ck("...reporting the OTHER configuration's three interfaces",
               g_seen_n == 3, g_seen_n);
            ck("...including the vendor-specific one",
               g_seen_cls[1] == 0xFF && g_seen_sub[1] == 0xFD &&
               g_seen_pro[1] == 0x01, g_seen_cls[1]);

            ck("an index the device does not have is refused, not faked",
               usb_survey_config(d, 7, survey_cb, 0) < 0, 0);

            ck("and after all of that the live device is unchanged",
               d->config == cfg_before && d->if_class == cls_before,
               d->config);
            ck("...and no SET_CONFIGURATION was sent by the survey",
               g_set_config_value == (int)cfg_before, g_set_config_value);
        }
    }

    /* ── alternate settings, on the wire ──────────────────────────── */
    head("a configuration that is not the first, and an ALTERNATE SETTING");
    {
        /* Everything above this point met a device with ONE
           configuration whose one interface had its endpoints on alt 0.
           The phone v0.8.7 exists for has four configurations, boots
           into index 0 which is PTP, and keeps tethering on interface 2
           of index 3 -- whose ALT 0 HAS NO ENDPOINTS AT ALL.

           So this drives usb_select_config() and usb_set_interface()
           through the CONTROLLER, against a device that answers, and
           checks the three things only the wire can show: which index
           was asked for, what SET_INTERFACE carried and who it was
           addressed to, and -- the one that matters -- WHICH ENDPOINTS
           THE CONTROLLER IS LEFT HOLDING. */
        usb_dev_t *d;
        int slot, nrings, i;

        sim_reset(32, 2, 1);
        g_multicfg = 1;              /* four configurations, as a phone */
        usb_init();
        d = (usb_dev_t *)usb_device(0);

        if(!d){
            ck("a device to configure", 0, 0);
        } else {
        slot = (int)d->slot;

        ck("it boots into index 0, which is not the interesting one",
           d->cfgidx == 0 && d->if_class == 3, d->if_class);

        head("  usb_select_config refuses an index the device lacks");
        {
            uint8_t cfg = d->config, cls = d->if_class;
            ck("index 3 is past bNumConfigurations and is refused",
               usb_select_config(d, 3, -1) == USB_EPARAM, 0);
            ck("...without asking the device", g_cfg_index_seen != 3,
               g_cfg_index_seen);
            ck("...and the live device is untouched",
               d->config == cfg && d->if_class == cls, d->if_class);
        }

        head("  selecting index 2, interface 2 -- the one with no endpoints");
        {
            int cmds = g_config_slots;
            g_cfg_index_seen = -1;
            ck("it is selected", usb_select_config(d, 2, 2) == USB_OK, 0);
            ck("the device was asked for index 2", g_cfg_index_seen == 2,
               g_cfg_index_seen);
            ck("SET_CONFIGURATION carried bConfigurationValue 3",
               g_set_config_value == 3, g_set_config_value);
            ck("...which is not the index, and must not be",
               d->config == 3 && d->cfgidx == 2, d->config);
            ck("interface 2 was adopted", d->ifnum == 2, d->ifnum);
            ck("at alternate setting 0", d->altsetting == 0, d->altsetting);
            ck("the Apple vendor triple is on it",
               d->if_class == 0xFF && d->if_subclass == 0xFD &&
               d->if_protocol == 0x01, d->if_class);
            ck("AND IT HAS NO ENDPOINTS -- which is the point",
               d->nep == 0, d->nep);
            /* The first version of this check expected NO command,
               reasoning that a setting with no endpoints has nothing to
               add. It fails, and the failure is the right answer: the
               BOOT configuration gave this device an interrupt IN at
               0x81, DCI 3, and changing configuration has to take it
               away. Before v0.8.7 nothing did -- a device switched from
               one configuration to another kept the first one's
               endpoints configured on the controller, with its rings
               re-laid underneath them. */
            ck("a Configure Endpoint WAS issued -- to drop the boot "
               "configuration's endpoint", g_config_slots == cmds + 1,
               g_config_slots - cmds);
            ck("...and DCI 3, the keyboard's, is what it dropped",
               !g_ep_set[slot][3], 0);
            ck("so the controller holds no endpoint beyond ep0",
               sim_eps_configured(slot) == 0, sim_eps_configured(slot));
        }

        head("  SET_INTERFACE to alt 1, where the bulk endpoints are");
        {
            int cmds = g_config_slots;
            ck("it switches", usb_set_interface(d, 2, 1) == USB_OK, 0);
            ck("exactly one SET_INTERFACE was sent", g_set_ifs == 1,
               g_set_ifs);
            ck("carrying alternate setting 1 in wValue",
               g_set_if_alt == 1, g_set_if_alt);
            ck("and interface 2 in wIndex -- not the other way round",
               g_set_if_num == 2, g_set_if_num);
            ck("ADDRESSED TO THE INTERFACE, not the device",
               g_set_if_recip_bad == 0, g_set_if_recip_bad);
            ck("the table records the setting in use",
               d->altsetting == 1, d->altsetting);
            ck("two endpoints now", d->nep == 2, d->nep);
            ck("...0x83 bulk in", d->ep[0].addr == 0x83 &&
               (d->ep[0].attr & 3) == USB_EP_BULK, d->ep[0].addr);
            ck("...0x04 bulk out", d->ep[1].addr == 0x04 &&
               (d->ep[1].attr & 3) == USB_EP_BULK, d->ep[1].addr);
            ck("...both 512 bytes", d->ep[0].mps == 512 &&
               d->ep[1].mps == 512, d->ep[0].mps);
            ck("a Configure Endpoint WAS issued this time",
               g_config_slots == cmds + 1, g_config_slots - cmds);
            ck("and the controller holds exactly those two",
               sim_eps_configured(slot) == 2, sim_eps_configured(slot));
            ck("...at DCI 7 and DCI 8",
               g_ep_set[slot][7] && g_ep_set[slot][8], 0);
            ck("a bulk transfer on the new endpoint works",
               usb_bulk(d, 0x83, 1, g_cfgbuf_probe, 8, 512) >= 0, 0);
        }

        head("  THE DROP: alt 2 uses DIFFERENT endpoint addresses");
        {
            int cmds = g_config_slots, drops = g_drops;
            ck("it switches", usb_set_interface(d, 2, 2) == USB_OK, 0);
            ck("two endpoints again", d->nep == 2, d->nep);
            ck("...but 0x85 and 0x06 this time",
               d->ep[0].addr == 0x85 && d->ep[1].addr == 0x06,
               d->ep[0].addr);
            ck("one Configure Endpoint", g_config_slots == cmds + 1,
               g_config_slots - cmds);
            ck("IT CARRIED DROP FLAGS -- two of them",
               g_drops == drops + 2, g_drops - drops);
            ck("...for endpoints the controller really held",
               g_drop_unset == 0, g_drop_unset);
            ck("...and never for D0 or D1, which the spec reserves",
               g_drop_reserved == 0, g_drop_reserved);
            ck("THE OLD ENDPOINTS ARE GONE from the controller",
               !g_ep_set[slot][7] && !g_ep_set[slot][8], 0);
            ck("the new ones are there", g_ep_set[slot][11] &&
               g_ep_set[slot][12], 0);
            ck("so it holds TWO, not four",
               sim_eps_configured(slot) == 2, sim_eps_configured(slot));
        }

        head("  back to alt 0: a drop with nothing to add");
        {
            int cmds = g_config_slots, drops = g_drops;
            ck("it switches", usb_set_interface(d, 2, 0) == USB_OK, 0);
            ck("no endpoints", d->nep == 0, d->nep);
            ck("A COMMAND WAS STILL ISSUED -- returning OK without one "
               "is what left two endpoints live",
               g_config_slots == cmds + 1, g_config_slots - cmds);
            ck("dropping both", g_drops == drops + 2, g_drops - drops);
            ck("and the controller holds none",
               sim_eps_configured(slot) == 0, sim_eps_configured(slot));
        }

        head("  THE RING LEAK: switching back and forth, five times each");
        {
            /* XHCI_EP_RINGS is five -- ep0 and four more. Alt 1 and alt 2
               use four DIFFERENT endpoint addresses between them, so a
               ring allocated per address and freed only at detach runs
               out on the SECOND switch. That is the v0.8.6.1 slot leak
               one level down, and it is reachable for the first time in
               this release because nothing before it configured a slot
               twice. */
            int k, ok = 1, worst = 0;
            for(k = 0; k < 5; k++){
                if(usb_set_interface(d, 2, 1) != USB_OK){ ok = 0; break; }
                if(d->nep != 2 || d->ep[0].addr != 0x83){ ok = 0; break; }
                nrings = 0;
                for(i = 0; i < XHCI_EP_RINGS; i++)
                    if(g_x[0].ep_dci[slot][i]) nrings++;
                if(nrings > worst) worst = nrings;
                if(usb_set_interface(d, 2, 2) != USB_OK){ ok = 0; break; }
                if(d->nep != 2 || d->ep[0].addr != 0x85){ ok = 0; break; }
                nrings = 0;
                for(i = 0; i < XHCI_EP_RINGS; i++)
                    if(g_x[0].ep_dci[slot][i]) nrings++;
                if(nrings > worst) worst = nrings;
            }
            ck("ten switches all succeed", ok, k);
            ck("and the slot never holds more than ep0 and two endpoints",
               worst == 3, worst);
            ck("the controller still holds exactly two",
               sim_eps_configured(slot) == 2, sim_eps_configured(slot));
            ck("...and they are alt 2's",
               g_ep_set[slot][11] && g_ep_set[slot][12], 0);
        }

        head("  a KEPT ring is RE-LAID, not resumed part-way along");
        {
            /* ring_alloc() returns the ring a DCI already has, and
               x_configure() writes the TR Dequeue Pointer back to its
               BASE. Without resetting the cursor the driver enqueues
               where it left off and the controller dequeues from the
               start -- so it processes a TRB the driver wrote on the
               previous setting. x_configure()'s own comment has claimed
               since v0.8.5.1 that a configure re-lays the rings.

               ALT 3 IS WHY THIS WORKS AS A TEST. It keeps endpoint
               0x83, so DCI 7's ring is not dropped and not freed. The
               first version of this check switched between alt 1 and
               alt 2, whose addresses all differ -- every ring was
               freed, ring_free() zeroed the cursor, and the ring came
               back clean whether or not the configure re-laid it. The
               harness reported removing ring_relay() as MISSED, which
               is how that was found. */
            int ri;
            uint32_t before;
            usb_set_interface(d, 2, 1);
            usb_bulk(d, 0x83, 1, g_cfgbuf_probe, 8, 512);
            usb_bulk(d, 0x83, 1, g_cfgbuf_probe, 8, 512);
            ri = -1;
            for(i = 0; i < XHCI_EP_RINGS; i++)
                if(g_x[0].ep_dci[slot][i] == 7) ri = i;
            before = (ri >= 0) ? g_x[0].ep_idx[slot][ri] : 0;
            ck("the IN endpoint's ring has advanced", ri >= 0 && before > 0,
               (long)before);

            ck("alt 3 keeps that endpoint and adds another",
               usb_set_interface(d, 2, 3) == USB_OK, 0);
            ck("...two endpoints, 0x83 among them",
               d->nep == 2 && d->ep[0].addr == 0x83, d->ep[0].addr);
            ri = -1;
            for(i = 0; i < XHCI_EP_RINGS; i++)
                if(g_x[0].ep_dci[slot][i] == 7) ri = i;
            ck("DCI 7 still has a ring -- it was NOT dropped", ri >= 0,
               ri);
            ck("AND ITS CURSOR IS BACK AT THE BASE",
               ri >= 0 && g_x[0].ep_idx[slot][ri] == 0,
               ri >= 0 ? (long)g_x[0].ep_idx[slot][ri] : -1);
            ck("...with the cycle bit back to 1",
               ri >= 0 && g_x[0].ep_cycle[slot][ri] == 1,
               ri >= 0 ? (long)g_x[0].ep_cycle[slot][ri] : -1);
            ck("the controller agrees the ring moved back to its base",
               ri >= 0 && g_ep_deq[slot][7] ==
                          (uint32_t)(uintptr_t)g_epr[0][slot][ri], 0);
            ck("0x04 was dropped, being alt 1's alone",
               !g_ep_set[slot][8], 0);
            ck("and a transfer on the kept endpoint still works",
               usb_bulk(d, 0x83, 1, g_cfgbuf_probe, 8, 512) >= 0, 0);
        }

        head("  a setting the device does not offer changes nothing");
        {
            uint8_t alt = d->altsetting, nep = d->nep;
            uint8_t a0 = d->ep[0].addr;
            int ifs = g_set_ifs;
            ck("alt 9 is refused", usb_set_interface(d, 2, 9) < 0, 0);
            ck("...by the WALK, so no SET_INTERFACE was sent",
               g_set_ifs == ifs, g_set_ifs - ifs);
            ck("...and the endpoints are untouched",
               d->altsetting == alt && d->nep == nep &&
               d->ep[0].addr == a0, d->nep);
            ck("an interface the configuration lacks is refused too",
               usb_set_interface(d, 7, 0) < 0, 0);
            ck("...with the device still on its setting",
               d->altsetting == alt && d->nep == nep, d->altsetting);
        }

        head("  A DEVICE THAT REFUSES SET_INTERFACE leaves the table alone");
        {
            /* The walk has to run BEFORE the request is sent, because
               the new setting's endpoints are in the descriptor. It
               writes the interface and the whole endpoint array. If the
               request is then refused and that had been written to the
               live device, the kernel would describe a setting the
               device never left -- and the next bulk transfer would go
               to an endpoint address the device does not have. */
            uint8_t alt = d->altsetting, nep = d->nep;
            uint8_t a0 = d->ep[0].addr, a1 = d->ep[1].addr;
            int cmds = g_config_slots;
            g_set_if_stall = 1;
            ck("the refusal is reported", usb_set_interface(d, 2, 2) < 0, 0);
            ck("the setting in the table is the OLD one",
               d->altsetting == alt, d->altsetting);
            ck("so are the endpoints",
               d->nep == nep && d->ep[0].addr == a0 &&
               d->ep[1].addr == a1, d->ep[0].addr);
            ck("and the controller was not reconfigured",
               g_config_slots == cmds, g_config_slots - cmds);
            ck("a transfer on the endpoint it still has works",
               usb_bulk(d, a0, 1, g_cfgbuf_probe, 8, 512) >= 0, 0);
        }

        head("  a controller that refuses the reconfigure says so");
        {
            /* The device has switched -- it accepted SET_INTERFACE --
               and the controller has not. There is no state both sides
               can be described by, so the table reports NO USABLE
               ENDPOINTS, which is true, rather than a list the
               controller was never told about. */
            uint8_t alt;
            g_fail_cmd_type = 12;            /* Configure Endpoint */
            ck("the failure is reported", usb_set_interface(d, 2, 2) < 0, 0);
            alt = d->altsetting;
            ck("the device's own setting is recorded, because it DID switch",
               alt == 2, alt);
            ck("but no endpoints are offered to a driver", d->nep == 0,
               d->nep);
            g_fail_cmd_type = 0;
            ck("and a later switch recovers",
               usb_set_interface(d, 2, 1) == USB_OK, 0);
            ck("...with its endpoints back", d->nep == 2, d->nep);
        }

        head("  an unconfigured device has no interface to switch");
        {
            /* THE WIRE, not the outcome. The first version of this used
               a zeroed device, and a zeroed device fails the transfer
               anyway -- so USB_ENODEV came back whether the guard was
               there or not, and the harness reported removing it as
               MISSED.

               This copy is a device that CAN answer, with config zeroed
               -- so without the guard the request really is sent, to a
               device the kernel's own table says is not configured. The
               check is that no SET_INTERFACE left the host. */
            usb_dev_t unconf = *d;
            int ifs = g_set_ifs, idx = g_cfg_index_seen;
            unconf.config = 0;
            ck("it is refused", usb_set_interface(&unconf, 2, 1) == USB_ENODEV,
               0);
            ck("...and NO SET_INTERFACE was sent", g_set_ifs == ifs,
               g_set_ifs - ifs);
            ck("...and the descriptor was not even fetched",
               g_cfg_index_seen == idx, g_cfg_index_seen);

            {
                usb_dev_t bare;
                memset(&bare, 0, sizeof bare);
                bare.hc = d->hc;
                ck("a device offering no configurations cannot select one",
                   usb_select_config(&bare, 0, -1) == USB_ENODEV, 0);
            }
        }
        }
    }

    /* ── v0.8.9.9: TWO DRIVERS ON ONE PHONE ───────────────────────────
       Kyan's HP, 11 November: the phone came up as a NIC at boot, then
       `pair` opened usbmux, which selected configuration index 3 again
       for interface 1. That reset interface 2 to alt 0 and dropped the
       Ethernet pair at the controller -- and `ifconfig dhcp` reported
       0 asked, because the DISCOVER could not leave. In this fixture
       the phone's both-interfaces configuration is index 2. */
    head("two interfaces on one phone: Ethernet first, then usbmux");
    {
        usb_dev_t *d;
        int slot, cmds, sets, sifs;
        sim_reset(32, 2, 1);
        g_multicfg = 1;
        usb_init();
        d = (usb_dev_t *)usb_device(0);
        if(!d){ ck("a phone", 0, 0); } else {
            slot = (int)d->slot;
            ck("Ethernet is claimed: index 2, interface 2, alt 1",
               usb_claim_interface(d, 2, 2, 1) == USB_OK, 0);
            ck("its bulk in is 0x83",
               usb_find_ep_if(d, 2, USB_EP_BULK, 1) &&
               usb_find_ep_if(d, 2, USB_EP_BULK, 1)->addr == 0x83, 0);
            ck("the controller holds DCIs 7 and 8",
               g_ep_set[slot][7] && g_ep_set[slot][8], 0);

            cmds = g_config_slots; sets = g_set_config_count;
            sifs = g_set_ifs;
            g_last_add = 0;
            ck("usbmux is claimed beside it: interface 1, alt 0",
               usb_claim_interface(d, 2, 1, 0) == USB_OK, 0);
            ck("SET_CONFIGURATION was NOT sent again -- that is the bug",
               g_set_config_count == sets, g_set_config_count - sets);
            ck("no SET_INTERFACE for alt 0",
               g_set_ifs == sifs, g_set_ifs - sifs);
            ck("one Configure Endpoint, to add the mux pair",
               g_config_slots == cmds + 1, g_config_slots - cmds);
            ck("it ADDED DCIs 5 and 6, the mux's",
               (g_last_add & (1u << 5)) && (g_last_add & (1u << 6)),
               (long)g_last_add);
            ck("and did NOT re-add 7 and 8 -- a running pipe is left alone",
               !(g_last_add & ((1u << 7) | (1u << 8))), (long)g_last_add);
            ck("the Ethernet pair is STILL configured",
               g_ep_set[slot][7] && g_ep_set[slot][8], 0);
            ck("and so is the mux pair",
               g_ep_set[slot][5] && g_ep_set[slot][6], 0);
            ck("four endpoints in the table", d->nep == 4, d->nep);
            ck("asked by interface, each driver finds its own",
               usb_find_ep_if(d, 1, USB_EP_BULK, 1) &&
               usb_find_ep_if(d, 1, USB_EP_BULK, 1)->addr == 0x82 &&
               usb_find_ep_if(d, 2, USB_EP_BULK, 0) &&
               usb_find_ep_if(d, 2, USB_EP_BULK, 0)->addr == 0x04, 0);
            ck("no endpoint is left marked keep",
               !d->ep[0].keep && !d->ep[1].keep && !d->ep[2].keep &&
               !d->ep[3].keep, 0);
            ck("claiming the mux AGAIN changes nothing it should not",
               usb_claim_interface(d, 2, 1, 0) == USB_OK &&
               d->nep == 4 && g_ep_set[slot][7] && g_ep_set[slot][5], d->nep);
        }
    }

    head("two interfaces on one phone: usbmux first, then Ethernet");
    {
        usb_dev_t *d;
        int slot, sets, sifs;
        sim_reset(32, 2, 1);
        g_multicfg = 1;
        usb_init();
        d = (usb_dev_t *)usb_device(0);
        if(!d){ ck("a phone", 0, 0); } else {
            slot = (int)d->slot;
            ck("usbmux is claimed", usb_claim_interface(d, 2, 1, 0) == USB_OK, 0);
            ck("the mux pair is configured",
               g_ep_set[slot][5] && g_ep_set[slot][6], 0);
            sets = g_set_config_count; sifs = g_set_ifs;
            g_last_add = 0;
            ck("Ethernet is claimed beside it",
               usb_claim_interface(d, 2, 2, 1) == USB_OK, 0);
            ck("without SET_CONFIGURATION",
               g_set_config_count == sets, g_set_config_count - sets);
            ck("with exactly one SET_INTERFACE, alt 1 on interface 2",
               g_set_ifs == sifs + 1 && g_set_if_alt == 1 &&
               g_set_if_num == 2, g_set_ifs - sifs);
            ck("the mux pair was not re-added",
               !(g_last_add & ((1u << 5) | (1u << 6))), (long)g_last_add);
            ck("all four are live",
               g_ep_set[slot][5] && g_ep_set[slot][6] &&
               g_ep_set[slot][7] && g_ep_set[slot][8], 0);
            ck("a DIFFERENT configuration still resets everything",
               usb_claim_interface(d, 1, 0, 0) == USB_OK &&
               g_set_config_count == sets + 1, g_set_config_count - sets);
            ck("and leaves no phone endpoint behind",
               !g_ep_set[slot][5] && !g_ep_set[slot][7], 0);
        }
    }

    head("what the controller would have refused");
    {
        ck("every ERDP write carried the handler-busy bit",
           !g_erdp_without_ehb, g_erdp_without_ehb);
        ck("the dequeue pointer was armed before the segment table",
           !g_erstba_before_erdp, g_erstba_before_erdp);
        ck("Address Device presented the slot context",
           !g_addr_without_a0, g_addr_without_a0);
        ck("Configure Endpoint raised Context Entries far enough",
           !g_bad_ctx_entries, g_bad_ctx_entries);
        ck("the host talked to endpoint 0 at the size the device asked for",
           !g_ep0_mps_wrong, g_ep0_mps_wrong);
    }

    head("a command the controller refuses must not produce a device");
    {
        int n;
        sim_reset(32, 2, 1);
        g_fail_next_cmd = 1;        /* the first command is Enable Slot */
        n = usb_init();
        ck("no device was invented out of a refused command", n == 0, n);
    }

    head("a REFUSED Configure Endpoint is not reported as a configured "
         "device");
    {
        const usb_dev_t *d;
        sim_reset(32, 2, 1);
        g_fail_cmd_type = 12;       /* Configure Endpoint */
        usb_init();
        d = usb_device(0);
        /* The device still enumerates -- it answered its descriptors --
           but it is NOT configured, and `usb -vv` must not claim it is.
           A driver that reads a non-success completion code as success
           leaves config set and the endpoint unusable. */
        ck("the device is still listed", d && d->valid, 0);
        ck("but it is not marked configured", d && d->config == 0,
           d ? d->config : -1);
        ck("the command really was refused", g_config_slots == 0,
           g_config_slots);
    }

    head("64-byte contexts -- the trap that moves every field");
    {
        int n;
        sim_reset(64, 0, 1);
        n = usb_init();
        ck("it registers", usb_hc_count() == 1, usb_hc_count());
        ck("and read the context size off HCCPARAMS1",
           g_x[0].ctxsz == 64, g_x[0].ctxsz);
        ck("enumeration still works at the other context size", n == 1, n);
        {
            const usb_dev_t *d = usb_device(0);
            ck("and the device still parses", d && d->if_class == 3,
               d ? d->if_class : -1);
        }
    }

    /* ── SuperSpeed, v0.8.6.7 ─────────────────────────────────────── */
    head("A SUPERSPEED DEVICE ENUMERATES");
    {
        /* hc0 port 12 on the HP: connected, enabled, powered, link U0
           active, SuperSpeed -- and "attached, but it did not answer",
           every boot since v0.8.5.3. A link that is up and a device
           that will not talk is a protocol fault, and this is it.

           bMaxPacketSize0 is an EXPONENT at SuperSpeed. Copied through
           as a byte count it programs a nine-byte control endpoint, the
           pipe that was working at 512 stops working, and enumeration
           ends in USB_EXFER. */
        int n;
        g_ss = 1;
        sim_reset(32, 0, 1);
        n = usb_init();
        ck("the device is found", n == 1, n);
        {
            const usb_dev_t *d = usb_device(0);
            ck("...at SuperSpeed", d && d->speed == USB_SPEED_SUPER,
               d ? d->speed : -1);
            ck("...with a 512-byte control endpoint, not 9",
               d && d->maxpacket0 == 512, d ? d->maxpacket0 : -1);
            ck("...and its descriptor was read in full",
               d && d->vendor == 0x0951 && d->product == 0x1662,
               d ? d->vendor : 0);
            ck("...so the 18-byte request completed, which a nine-byte "
               "endpoint cannot carry",
               d && d->nconfigs == 1, d ? d->nconfigs : -1);
        }
        ck("the controller never saw a wrong ep0 packet size",
           g_ep0_mps_wrong == 0, g_ep0_mps_wrong);

        head("a SuperSpeed device that says nothing keeps 512");
        {
            /* bMaxPacketSize0 of ZERO is malformed. The speed's default
               is the right answer and it is 512 -- which is also the
               only state in which the uint8_t this field used to be is
               visible, because the correction path does not fire. */
            const usb_dev_t *d;
            g_ss_mps_byte = 1;         /* 1 is not 9: not allowed      */
            sim_reset(32, 0, 1);
            n = usb_init();
            ck("it still enumerates", n == 1, n);
            d = usb_device(0);
            ck("...and keeps the speed's 512, not the byte",
               d && d->maxpacket0 == 512, d ? d->maxpacket0 : -1);
            ck("...and no wrong size reached the controller",
               g_ep0_mps_wrong == 0, g_ep0_mps_wrong);
        }

        head("...and one that says something absurd does too");
        {
            const usb_dev_t *d;
            g_ss_mps_byte = 12;        /* 1u << 12 == 4096             */
            sim_reset(32, 0, 1);
            n = usb_init();
            ck("it still enumerates", n == 1, n);
            d = usb_device(0);
            ck("...with 512, not 4096",
               d && d->maxpacket0 == 512, d ? d->maxpacket0 : -1);
            ck("...and 4096 was never programmed",
               g_ep0_mps_wrong == 0, g_ep0_mps_wrong);
            g_ss_mps_byte = 0;
        }
        g_ss = 0;
    }

    head("...and a high-speed one still does, with 64");
    {
        int n;
        sim_reset(32, 0, 1);
        n = usb_init();
        ck("the device is found", n == 1, n);
        {
            const usb_dev_t *d = usb_device(0);
            ck("...at high speed", d && d->speed == USB_SPEED_HIGH,
               d ? d->speed : -1);
            /* DEV_DESC says 8, which is what the correction path exists
               to notice: the context starts at the speed default and is
               re-programmed to what the device actually said. */
            ck("...with the packet size the DEVICE named, not the "
               "speed's default",
               d && d->maxpacket0 == 8, d ? d->maxpacket0 : -1);
        }
        ck("...and no wrong size was ever programmed",
           g_ep0_mps_wrong == 0, g_ep0_mps_wrong);
    }

    head("a controller with no scratchpad requirement");
    {
        uint32_t dcbaa;
        sim_reset(32, 0, 1);
        usb_init();
        dcbaa = rd(SIM_CAPLEN + 0x30);
        ck("DCBAA entry 0 is left alone when none were asked for",
           m32(dcbaa) == 0, (long)m32(dcbaa));
    }

    head("an empty port is not enumerated");
    {
        int n;
        sim_reset(32, 2, 0);
        n = usb_init();
        ck("nothing found", n == 0, n);
        ck("and no slot was enabled", g_cmds_executed == 0, g_cmds_executed);
    }

    head("a controller that never clears CNR is refused, not used");
    {
        int n;
        sim_reset(32, 2, 1);
        g_cnr_stuck = 1;                 /* CNR never clears */
        wr(SIM_CAPLEN + 0x04, 1u | (1u << 11));
        n = usb_init();
        /* The driver must notice. Either it refuses to register, or it
           registers and finds nothing -- what it must NOT do is carry on
           writing configuration into a controller that is not ready. */
        ck("it did not claim a working controller",
           n == 0 && usb_hc_count() == 0, usb_hc_count());
    }

    /* ── surveying configurations, v0.8.6.4 ───────────────────────── */
    head("every configuration a device offers, selecting none of them");
    {
        usb_dev_t d;
        memset(&d, 0, sizeof d);

        /* A device that IS configured, so the "changes nothing" check
           below has something to lose. */
        ck("the live device parses first",
           usb_parse_config(&d, CFG_DESC, sizeof CFG_DESC, -1) == USB_OK, 0);
        ck("...and is configured", d.config == 1, d.config);
        ck("...on an interface with endpoints", d.nep == 1, d.nep);

        {
            uint8_t  cfg_before  = d.config;
            uint8_t  ifn_before  = d.ifnum;
            uint8_t  cls_before  = d.if_class;
            uint8_t  nep_before  = d.nep;
            uint8_t  epa_before  = d.ep[0].addr;

            g_seen_n = 0;
            ck("a survey of the other configuration walks it",
               usb_walk_config(&d, CFG_DESC2, sizeof CFG_DESC2,
                               USB_IF_NONE, survey_cb, 0) == USB_OK, 0);

            /* ── THE CHECK THIS EXISTS FOR ────────────────────────────
               A survey runs against a device that is working. If it
               cleared d->config the kernel would believe the device
               unconfigured while the device itself carried on
               configured, and the two disagreeing is worse than not
               looking at all. */
            ck("AND THE LIVE DEVICE IS UNTOUCHED: config",
               d.config == cfg_before, d.config);
            ck("...interface number",  d.ifnum == ifn_before, d.ifnum);
            ck("...interface class",   d.if_class == cls_before, d.if_class);
            ck("...endpoint count",    d.nep == nep_before, d.nep);
            ck("...and the endpoint itself",
               d.ep[0].addr == epa_before, d.ep[0].addr);
        }

        ck("it reported every interface, alternates included",
           g_seen_n == 3, g_seen_n);
        ck("the first is the still-image one",
           g_seen_cls[0] == 0x06 && g_seen_num[0] == 0 && g_seen_alt[0] == 0,
           g_seen_cls[0]);
        ck("the second is vendor-specific 255/253/1",
           g_seen_cls[1] == 0xFF && g_seen_sub[1] == 0xFD &&
           g_seen_pro[1] == 0x01, g_seen_cls[1]);
        ck("...on interface 1, alt 0",
           g_seen_num[1] == 1 && g_seen_alt[1] == 0, g_seen_alt[1]);
        ck("and the ALTERNATE setting is reported too, not skipped",
           g_seen_num[2] == 1 && g_seen_alt[2] == 1, g_seen_alt[2]);

        head("a survey with no observer is still a legal walk");
        ck("no callback, no fault",
           usb_walk_config(&d, CFG_DESC2, sizeof CFG_DESC2,
                           USB_IF_NONE, 0, 0) == USB_OK, 0);

        head("a survey still refuses a malformed blob");
        {
            uint8_t b[64];
            memcpy(b, CFG_DESC2, sizeof CFG_DESC2);
            b[1] = 7;
            ck("not a configuration descriptor",
               usb_walk_config(&d, b, sizeof CFG_DESC2, USB_IF_NONE,
                               survey_cb, 0) == USB_EXFER, 0);
            memcpy(b, CFG_DESC2, sizeof CFG_DESC2);
            b[9] = 0;                       /* a bLength of zero        */
            ck("a zero-length descriptor, which would never advance",
               usb_walk_config(&d, b, sizeof CFG_DESC2, USB_IF_NONE,
                               survey_cb, 0) == USB_EXFER, 0);
            memcpy(b, CFG_DESC2, sizeof CFG_DESC2);
            b[5] = 0;
            ck("bConfigurationValue 0 -- unconfigured",
               usb_walk_config(&d, b, sizeof CFG_DESC2, USB_IF_NONE,
                               survey_cb, 0) == USB_EXFER, 0);
        }

        head("a device that claims more than it sends");
        {
            /* wTotalLength is what the device SAYS the blob is, and a
               device that overstates it is a device contradicting
               itself -- which is the condition this parser is pure in
               order to survive.

               It matters because of the controller, not the device: one
               that reports no residual hands back the length that was
               ASKED for, so the tail of the buffer is whatever was
               there before. The walk would read a zero bLength past the
               last real descriptor and refuse the entire configuration.

               This blob is CFG_DESC2 with its wTotalLength inflated and
               the buffer zero-padded to match, which is exactly the
               shape that produced it. wTotalLength bounds the walk now,
               so the padding is never reached. */
            uint8_t b[64];
            memset(b, 0, sizeof b);
            memcpy(b, CFG_DESC2, sizeof CFG_DESC2);
            g_seen_n = 0;
            ck("the overstated blob still walks",
               usb_walk_config(&d, b, 56, USB_IF_NONE, survey_cb, 0)
                   == USB_OK, 0);
            ck("...and reports exactly the interfaces that are there",
               g_seen_n == 3, g_seen_n);

            /* And the other direction, which has always worked: a blob
               SHORTER than wTotalLength promises is an ordinary short
               read, and the walk stops at the last descriptor that
               fits rather than reading past it. */
            memset(b, 0, sizeof b);
            memcpy(b, CFG_DESC2, sizeof CFG_DESC2);
            g_seen_n = 0;
            ck("a short read stops cleanly",
               usb_walk_config(&d, b, 28, USB_IF_NONE, survey_cb, 0)
                   == USB_OK, 0);
            ck("...having reported only what arrived",
               g_seen_n == 2, g_seen_n);
        }

        head("a configuration with no interfaces is empty, not broken");
        {
            /* usb_parse_config() refuses this, because a device with no
               interface is one it cannot drive. A SURVEY must not: "it
               offers a configuration with nothing in it" is an answer,
               and refusing it would read as "the device would not
               describe it", which is a different thing. */
            static const uint8_t EMPTY_CFG[] = { 9, 2, 9,0, 0, 3, 0, 0xA0, 50 };
            usb_dev_t e;
            memset(&e, 0, sizeof e);
            g_seen_n = 0;
            ck("the survey accepts it",
               usb_walk_config(&e, EMPTY_CFG, sizeof EMPTY_CFG,
                               USB_IF_NONE, survey_cb, 0) == USB_OK, 0);
            ck("...and reports no interfaces", g_seen_n == 0, g_seen_n);
            ck("while the ordinary parse REFUSES it",
               usb_parse_config(&e, EMPTY_CFG, sizeof EMPTY_CFG, -1)
                   == USB_EXFER, 0);
        }
    }

    /* ── the descriptor walk, on its own ──────────────────────────── */
    head("the configuration descriptor parser, fed malformed blobs");
    {
        usb_dev_t d;
        uint8_t b[64];

        ck("the real one parses",
           usb_parse_config(&d, CFG_DESC, sizeof CFG_DESC, -1) == USB_OK, 0);

        memset(b, 0, sizeof b);
        ck("an empty blob is refused",
           usb_parse_config(&d, b, 0, -1) == USB_EXFER, 0);

        memcpy(b, CFG_DESC, sizeof CFG_DESC);
        b[1] = 7;
        ck("a blob whose first descriptor is not a config is refused",
           usb_parse_config(&d, b, sizeof CFG_DESC, -1) == USB_EXFER, 0);

        memcpy(b, CFG_DESC, sizeof CFG_DESC);
        b[5] = 0;
        ck("bConfigurationValue 0 is refused -- it means unconfigured",
           usb_parse_config(&d, b, sizeof CFG_DESC, -1) == USB_EXFER, 0);

        memcpy(b, CFG_DESC, sizeof CFG_DESC);
        b[9] = 0;                              /* interface bLength = 0 */
        ck("A DESCRIPTOR OF LENGTH ZERO IS REFUSED, NOT LOOPED ON",
           usb_parse_config(&d, b, sizeof CFG_DESC, -1) == USB_EXFER, 0);

        memcpy(b, CFG_DESC, sizeof CFG_DESC);
        b[9] = 1;
        ck("and so is one too short to hold its own type byte",
           usb_parse_config(&d, b, sizeof CFG_DESC, -1) == USB_EXFER, 0);

        memcpy(b, CFG_DESC, sizeof CFG_DESC);
        b[31] = 0; b[32] = 0;                  /* wMaxPacketSize = 0    */
        ck("an endpoint that can carry nothing is refused",
           usb_parse_config(&d, b, sizeof CFG_DESC, -1) == USB_EXFER, 0);

        memcpy(b, CFG_DESC, sizeof CFG_DESC);
        ck("a short read stops at the last whole descriptor",
           usb_parse_config(&d, b, 20, -1) == USB_OK, 0);
        ck("and reports the endpoints it actually saw", d.nep == 0, d.nep);

        memcpy(b, CFG_DESC, sizeof CFG_DESC);
        b[12] = 1;                             /* bAlternateSetting = 1 */
        ck("an alternate setting is not adopted by accident",
           usb_parse_config(&d, b, sizeof CFG_DESC, -1) == USB_EXFER, 0);
    }

    head("finding an endpoint by kind and direction");
    {
        usb_dev_t d;
        usb_parse_config(&d, CFG_DESC, sizeof CFG_DESC, -1);
        ck("the interrupt IN endpoint is found",
           usb_find_ep(&d, USB_EP_INTR, 1) != 0, 0);
        ck("an interrupt OUT is not invented",
           usb_find_ep(&d, USB_EP_INTR, 0) == 0, 0);
        ck("nor a bulk one", usb_find_ep(&d, USB_EP_BULK, 1) == 0, 0);
    }

    /* ════════════════════════════════════════════════════════════════
       v0.8.5.1 -- THE BAR THE HP ACTUALLY HAS.

       `lspci -vv` on that laptop reports, for 00:14.0:

           BAR0 mem 0x02120000
           BAR1 mem 0x00000060

       which is ONE 64-bit BAR at 0x60_02120000 -- about 384 GiB -- not
       two BARs. v0.8.5 took the low half, read a capability register out
       of whatever sits at 33 MB, got a nonsense CAPLENGTH and refused to
       start. That was the correct response to a wrong address and it
       told nobody which address it had used.

       The numbers below are his, not invented: 0x02120004 is that BAR
       with its type bits (10b, 64-bit) and bar[1] is 0x60.
       ════════════════════════════════════════════════════════════════ */
    head("a 64-bit BAR above 4 GB -- the HP's own numbers");
    sim_reset(32, 2, 1);
    g_pci.bar[0] = 0x02120004u;      /* type 10b: 64-bit memory BAR */
    g_pci.bar[1] = 0x00000060u;      /* the high dword, not a BAR   */
    g_map_give   = RBASE;            /* the window lands on the sim  */
    {
        uint32_t hi = 0, lo = pci_bar_mem64(&g_pci, 0, &hi);
        int n;
        ck("the decoder reports it as 64-bit", pci_bar_is_64(&g_pci, 0) == 1, 0);
        ck("...with the low half masked", lo == 0x02120000u, (long)lo);
        ck("...and the high half recovered", hi == 0x60u, (long)hi);
        ck("pci_bar_mem() REFUSES it rather than truncating",
           pci_bar_mem(&g_pci, 0) == 0, (long)pci_bar_mem(&g_pci, 0));

        n = xhci_hc_register();
        ck("the controller registers anyway", n == 1, n);
        ck("because the window was mapped", g_map_calls == 1, g_map_calls);
        ck("...through the UNCACHED entry point, not the framebuffer one",
           g_map_io_calls == 1, g_map_io_calls);
        ck("...with the real 40-bit address", g_map_hi == 0x60u &&
           g_map_lo == 0x02120000u, (long)g_map_lo);
        ck("...and the whole register file, not one page",
           g_map_bytes >= 0x10000u, (long)g_map_bytes);
        ck("...so nothing was touched outside the window",
           g_outside_window == 0, g_outside_window);
        ck("it was reset through that window", g_reset_count == 1, g_reset_count);
        ck("and the run bit was set", g_run_set == 1, g_run_set);
    }

    head("...and when the window cannot be had, it says so and stops");
    sim_reset(32, 2, 1);
    g_pci.bar[0] = 0x02120004u;
    g_pci.bar[1] = 0x00000060u;
    g_map_fail = 1;
    {
        int before = usb_hc_count();
        int n = xhci_hc_register();
        ck("no controller registers", n == 0, n);
        ck("the mapping was attempted", g_map_calls == 1, g_map_calls);
        ck("the controller was NOT reset at the truncated address",
           g_reset_count == 0, g_reset_count);
        ck("and the core's controller table did not grow",
           usb_hc_count() == before, usb_hc_count() - before);
    }

    head("a 32-bit BAR is untouched by any of this");
    sim_reset(32, 2, 1);
    {
        int n;
        ck("not reported as 64-bit", pci_bar_is_64(&g_pci, 0) == 0, 0);
        n = xhci_hc_register();
        ck("it registers", n == 1, n);
        ck("and no window was mapped", g_map_calls == 0, g_map_calls);
    }

    head("a 64-bit BAR whose high half is zero needs no window");
    sim_reset(32, 2, 1);
    g_pci.bar[0] = RBASE | 0x4u;     /* 64-bit type, but entirely low  */
    g_pci.bar[1] = 0u;
    {
        int n;
        ck("still reported as 64-bit", pci_bar_is_64(&g_pci, 0) == 1, 0);
        ck("pci_bar_mem() allows it, because it fits",
           pci_bar_mem(&g_pci, 0) == (RBASE & ~0xFu),
           (long)pci_bar_mem(&g_pci, 0));
        n = xhci_hc_register();
        ck("it registers", n == 1, n);
        ck("and no window was wasted on it", g_map_calls == 0, g_map_calls);
    }

    head("a BAR whose low half is exactly zero is still a BAR");
    sim_reset(32, 2, 1);
    /* 0x60_00000000 is a legal base and its bar[0] is 0 once masked.
       Testing the low half alone to decide "is there a BAR here" throws
       such a controller away, and the machine simply has no USB with no
       message to say so. */
    g_pci.bar[0] = 0x00000004u;
    g_pci.bar[1] = 0x00000060u;
    {
        uint32_t hi = 0, lo = pci_bar_mem64(&g_pci, 0, &hi);
        int n;
        ck("the low half really is zero", lo == 0, (long)lo);
        ck("...and the high half is not", hi == 0x60u, (long)hi);
        n = xhci_hc_register();
        ck("the controller is not discarded", n == 1, n);
        ck("the window was mapped at the right place",
           g_map_hi == 0x60u && g_map_lo == 0u, (long)g_map_lo);
    }

    head("a 32-bit BAR0 beside a second, unrelated BAR1");
    sim_reset(32, 2, 1);
    /* bar[0] is 32-bit, so bar[1] is a BAR in its own right and has
       nothing to do with it. Deciding "is this above 4 GB" by looking at
       bar[1] rather than at bar[0]'s type bits maps a perfectly
       reachable controller into a window built from an unrelated
       number. */
    g_pci.bar[0] = RBASE;            /* type 00b: 32-bit */
    g_pci.bar[1] = 0x00000060u;      /* somebody else's BAR */
    {
        int n = xhci_hc_register();
        ck("it registers", n == 1, n);
        ck("and NO window was mapped", g_map_calls == 0, g_map_calls);
        ck("pci_bar_mem() hands back the address directly",
           pci_bar_mem(&g_pci, 0) == (RBASE & ~0xFu),
           (long)pci_bar_mem(&g_pci, 0));
    }

    head("a 64-bit BAR in the last slot has nowhere to keep its high half");
    {
        pci_dev_t t;
        uint32_t hi = 0xDEADu;
        memset(&t, 0, sizeof t);
        t.bar[5] = 0x02120004u;
        ck("bar[5] cannot be 64-bit, so it is refused",
           pci_bar_mem64(&t, 5, &hi) == 0, 0);
        ck("...and the high half is cleared, not left as the caller had it",
           hi == 0, (long)hi);
    }

    head("an I/O BAR is never mistaken for memory");
    {
        pci_dev_t t;
        uint32_t hi = 0;
        memset(&t, 0, sizeof t);
        t.bar[0] = 0x0000CF01u;      /* I/O at 0xCF00 */
        ck("pci_bar_mem64 declines it", pci_bar_mem64(&t, 0, &hi) == 0, 0);
        ck("pci_bar_is_64 declines it", pci_bar_is_64(&t, 0) == 0, 0);
        ck("pci_bar_io reads it", pci_bar_io(&t, 0) == 0xCF00u,
           (long)pci_bar_io(&t, 0));
    }

    /* ════════════════════════════════════════════════════════════════
       v0.8.5.2 -- TAKING THE CONTROLLER FROM THE FIRMWARE.

       Before an OS touches an xHCI, the BIOS owns it: an SMM handler
       drives the controller and presents an attached USB keyboard at
       ports 0x60/0x64 as a PS/2 keyboard. That is why a USB keyboard
       works in GRUB, and on a laptop with no PS/2 port it is the only
       reason anything can be typed before a driver exists.

       v0.8.5.1 never performed the handoff. The firmware kept emulating
       while the driver also drove the controller, so every keystroke
       arrived TWICE -- once at port 0x60 into kbd_poll(), once through
       usb_hid_poll() -- and the installer could not be driven on the one
       machine this work is for.

       It was invisible in QEMU, which has no SMM legacy emulation.
       ════════════════════════════════════════════════════════════════ */
    head("taking the controller from a firmware that owns it");
    sim_reset(32, 2, 1);
    sim_bios(1, 0, 0);
    {
        uint32_t leg;
        int n = usb_init();
        ck("the controller registers", n == 1, n);
        ck("USBLEGSUP was written", g_legsup_writes >= 1, g_legsup_writes);
        ck("BEFORE the controller was reset", g_legsup_before_reset == 1, 0);
        leg = rd(SIM_LEGOFF);
        ck("the BIOS no longer owns it", (leg & (1u << 16)) == 0, (long)leg);
        ck("and we do", (leg & (1u << 24)) != 0, (long)leg);
        ck("the driver recorded the handoff",
           g_x[0].bios_handoff == XHCI_HANDOFF_TAKEN, g_x[0].bios_handoff);
        ck("every SMI source the firmware had was disabled",
           (g_legctl_last & 0x0000E011u) == 0, (long)g_legctl_last);
        ck("...and the SMIs it had already latched were acknowledged",
           (rd(SIM_LEGOFF + 4u) & 0xE0000000u) == 0,
           (long)rd(SIM_LEGOFF + 4u));
        ck("the driver waited rather than assuming",
           g_bios_delay == 0, g_bios_delay);
    }

    head("...and a firmware that will not let go");
    sim_reset(32, 2, 1);
    sim_bios(1, 1, 0);
    {
        int n = usb_init();
        ck("the controller still comes up", n == 1, n);
        ck("but the refusal is recorded",
           g_x[0].bios_handoff == XHCI_HANDOFF_REFUSED, g_x[0].bios_handoff);
        ck("the BIOS-owned bit is still set",
           (rd(SIM_LEGOFF) & (1u << 16)) != 0, (long)rd(SIM_LEGOFF));
        ck("and its SMIs were disabled anyway",
           (g_legctl_last & 0x0000E011u) == 0, (long)g_legctl_last);
    }

    head("a controller the firmware was not holding");
    sim_reset(32, 2, 1);
    sim_bios(0, 0, 0);
    {
        int n = usb_init();
        ck("it comes up", n == 1, n);
        ck("and says so rather than claiming a handoff",
           g_x[0].bios_handoff == XHCI_HANDOFF_NOTHELD, g_x[0].bios_handoff);
    }

    head("a controller with no legacy capability at all");
    sim_reset(32, 2, 1);
    sim_bios(0, 0, 1);
    {
        int n = usb_init();
        ck("it comes up", n == 1, n);
        ck("nothing was written to a capability that is not there",
           g_legsup_writes == 0, g_legsup_writes);
        ck("and the driver says there was none",
           g_x[0].bios_handoff == XHCI_HANDOFF_NONE, g_x[0].bios_handoff);
    }

    head("a capability list full of entries that are not ours");
    sim_reset(32, 2, 1);
    {
        int n;
        /* A next-pointer of zero dwords points at itself. Firmware builds
           this list, so a walk that trusts it is a hang before a single
           line of boot log has been printed. */
        sim_bios(1, 0, 0);
        wr(SIM_XECP, 0x00000099u);        /* id 0x99, next pointer 0 */
        n = usb_init();
        ck("it still comes up", n == 1, n);
        ck("and found no legacy capability",
           g_x[0].bios_handoff == XHCI_HANDOFF_NONE, g_x[0].bios_handoff);
    }

    /* ════════════════════════════════════════════════════════════════
       A SLOT IS GIVEN BACK -- v0.8.6.1
       ════════════════════════════════════════════════════════════════

       Found on the HP, not here. TRB_DISABLE_SLOT was defined in
       usb/xhci_hc.c and never sent, and slot_used[] was cleared only at
       controller init and on a FAILED Address Device. usb_enumerate()
       clearing its own table frees nothing, because on xHCI the
       controller owns the device.

       The laptop has 8 slots and 3 devices. Boot took slots 1, 2 and 3;
       the first `usb rescan` took 4, 5 and 6 -- the addresses are in the
       photograph. The second would take 7 and 8, the third would lose a
       device and the fourth would find none.

       Every existing check in this file enumerated ONCE, which is
       exactly why 150 of them passed over a leak. So this enumerates
       more times than there are slots. */
    head("a slot is handed back, so rescanning does not run out");
    sim_reset(32, 2, 1);
    {
        int first, i, lost = 0;

        first = usb_init();
        ck("the first walk finds the device", first == 1, first);
        ck("...and it took a slot", slots_in_use() == 1, slots_in_use());

        /* Four times the number of slots. With the leak this stops
           finding anything after the eighth. */
        for(i = 0; i < SIM_SLOTS * 4; i++)
            if(usb_enumerate() != first) lost++;

        ck("THIRTY-TWO RESCANS LATER IT STILL FINDS THE DEVICE",
           lost == 0, lost);
        ck("the controller never ran out of slots",
           g_enable_failed == 0, g_enable_failed);
        ck("exactly one slot is in use, not thirty-three",
           slots_in_use() == 1, slots_in_use());
        ck("a Disable Slot was sent for every rescan",
           g_disables == SIM_SLOTS * 4, g_disables);
        ck("...and never for a slot the controller did not hold",
           g_disable_unowned == 0, g_disable_unowned);
    }

    head("the endpoint rings are released with the slot");
    /* ep_dci[slot][] marks a ring in use and nothing cleared it either.
       A reused slot would have found its rings already allocated, and
       ring_alloc() would have handed back a ring with the previous
       device's cursor in it -- a transfer posted at whatever offset the
       last one left behind. */
    sim_reset(32, 2, 1);
    {
        int i, slot;
        const usb_dev_t *d0;
        usb_init();
        /* Guarded, because this block runs with the leak present when
           tools/check_xhci_suite.py damages the driver -- and a null
           dereference reports as a CRASH, which tells you less than a
           failure does and stops every check after it from running. */
        d0 = usb_device_count() > 0 ? usb_device(0) : 0;
        ck("the device enumerated at all", d0 != 0, usb_device_count());
        slot = d0 ? (int)d0->slot : 0;
        ck("the device has a slot", slot > 0, slot);
        if(!slot) slot = 1;            /* keep the indices in range */
        ck("its default pipe has a ring", g_x[0].ep_dci[slot][0] == 1,
           g_x[0].ep_dci[slot][0]);
        {
            /* The cursor after ONE walk, whatever it is: a handful of
               control transfers' worth of TRBs. The number is not the
               point and guessing it is how this check first failed --
               what matters is that it does not GROW, because a ring
               that was never released keeps the previous device's
               cursor and the next transfer is posted wherever the last
               one left off. */
            uint32_t after_one = g_x[0].ep_idx[slot][0];
            int s2;
            ck("one walk advanced the cursor", after_one > 0, after_one);
            for(i = 0; i < 3; i++) usb_enumerate();
            ck("it still enumerates", usb_device_count() == 1,
               usb_device_count());
            s2 = usb_device_count() > 0 ? (int)usb_device(0)->slot : slot;
            if(s2 <= 0 || s2 > SIM_SLOTS) s2 = slot;
            ck("and its ring is fresh, not three walks' worth deep",
               g_x[0].ep_idx[s2][0] == after_one, g_x[0].ep_idx[s2][0]);
            ck("...on a ring the driver re-allocated for it",
               g_x[0].ep_dci[s2][0] == 1, g_x[0].ep_dci[s2][0]);
        }
    }

    head("a device that never got a slot is not disabled");
    /* x_detach() must refuse a slot this side does not hold, or a
       failed attach turns into a Disable Slot for somebody else's. */
    sim_reset(32, 2, 1);
    {
        usb_dev_t fake;
        memset(&fake, 0, sizeof fake);
        fake.valid = 1; fake.hc = 0; fake.slot = 0;
        usb_init();
        { int before = g_disables;
          XHCI_OPS.detach(&g_hc[0], &fake);
          ck("slot 0 is refused", g_disables == before, g_disables);
          fake.slot = SIM_SLOTS + 4;
          XHCI_OPS.detach(&g_hc[0], &fake);
          ck("a slot past the end is refused", g_disables == before,
             g_disables);
          fake.slot = SIM_SLOTS;        /* in range, but not ours */
          XHCI_OPS.detach(&g_hc[0], &fake);
          ck("an unheld slot is refused", g_disables == before,
             g_disables); }
    }

    head("a command's reply is read from the command, not the ring cursor");
    /* Enable Slot's reply carries the slot id, and until 0.8.7.5 the
       driver read it off the event ring entry BEHIND the cursor. That was
       correct only because the old evt_wait() stopped at the first event
       it took. evt_pump() drains the whole ring, so anything that lands
       behind the completion ends up under that cursor instead -- and a
       polled keyboard finishing while a command is in flight does exactly
       that. g_trail_evt models it: one stray transfer event naming slot
       0, which is not a slot any device can be given. */
    sim_reset(32, 2, 1);
    {
        int n;
        usb_init();
        g_trail_evt = 1;
        n = usb_enumerate();
        ck("the device still got a slot", n == 1, n);
        ck("and it is a real slot",
           usb_device(0) && usb_device(0)->slot >= 1,
           usb_device(0) ? usb_device(0)->slot : -1);
    }

    /* ── v0.9.2.2: a hub is told it is a hub ─────────────────────────
       Kyan's HP, 11 November: a keyboard behind a USB hub lit up and
       typed nothing. No hub's slot context ever had its Hub flag, port
       count or TT think time, so the controller had no reason to run
       split transactions for the low/full speed keyboard behind it. */
    head("a hub's slot context says it is a hub (v0.9.2.2)");
    {
        usb_dev_t *d;
        int slot, cfgs, r;
        uint32_t sc, d0b, d1b, d2b, d0, d1, d2;
        sim_reset(32, 2, 1);
        usb_init();
        d = (usb_dev_t *)usb_device(0);
        if(!d){
            ck("a device to mark", 0, 0);
        } else {
            slot = (int)d->slot;
            sc = g_slot_ctx[slot];
            d0b = m32(sc); d1b = m32(sc + 4); d2b = m32(sc + 8);
            ck("before: no Hub flag, no port count",
               !(d0b & (1u << 26)) && !(d1b >> 24), (long)d0b);
            cfgs = g_config_slots;
            d->speed = USB_SPEED_HIGH;          /* as a USB 2.0 hub is */
            r = usb_hub_info(d, 4, 2, 1);
            d0 = m32(sc); d1 = m32(sc + 4); d2 = m32(sc + 8);
            ck("usb_hub_info is accepted", r == USB_OK, r);
            ck("  ... as ONE Configure Endpoint", g_config_slots == cfgs + 1,
               g_config_slots - cfgs);
            ck("  ... whose only add flag is A0: no endpoint touched",
               g_last_add == 1u, (long)g_last_add);
            ck("  ... Hub flag (dword 0 bit 26) and Multi-TT (bit 25) set",
               (d0 & (1u << 26)) && (d0 & (1u << 25)), (long)d0);
            ck("  ... route, speed and Context Entries unchanged",
               (d0 & ~((1u << 26) | (1u << 25))) == d0b, (long)d0);
            ck("  ... Number of Ports 4 (dword 1 31:24), root port kept",
               (d1 >> 24) == 4u && (d1 & 0x00FFFFFFu) == (d1b & 0x00FFFFFFu), (long)d1);
            ck("  ... TT think time 2 (dword 2 17:16)",
               ((d2 >> 16) & 3u) == 2u && (d2 & ~(3u << 16)) == (d2b & ~(3u << 16)),
               (long)d2);
            d->speed = USB_SPEED_FULL;
            r = usb_hub_info(d, 4, 2, 1);
            d0 = m32(sc); d2 = m32(sc + 8);
            ck("a FULL-speed hub: Hub flag, but no MTT and no think time",
               r == USB_OK && (d0 & (1u << 26)) && !(d0 & (1u << 25)) &&
               !((d2 >> 16) & 3u), (long)d0);
            ck("no port count is refused", usb_hub_info(d, 0, 0, 0) == USB_EPARAM, 0);
        }
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
