/* ════════════════════════════════════════════════════════════════════
   test_msc.c -- USB mass storage, against a stick that executes its
   commands.

   "A driver is tested against a controller that executes its commands,
   not a mock that returns success." That rule is in the handoff because
   of what the kinder simulators cost: v0.8.5's HID bug was invisible
   because the simulated device always had a report ready, so an endpoint
   could never NAK -- and NAKing was the entire mechanism of the bug.

   So the device below is a three-phase state machine over an array of
   blocks. It parses the Command Block Wrapper, runs the SCSI command
   inside it against real data, and builds a Command Status Wrapper with
   the tag it was sent. It REFUSES a phase out of order, which is what
   makes a desynchronised driver visible here rather than on somebody's
   stick: if the driver loses a CSW, the next command's CBW arrives where
   a status read was expected and this device says so.

   ── WHAT IS ACTUALLY WORTH TESTING ───────────────────────────────────

   Not the happy path. Reading a sector is a CBW, 512 bytes and a CSW,
   and it works first time.

   What is worth testing is every way the status phase can go wrong,
   because a protocol with a status phase has a way to desynchronise and
   a driver one command out of step reports the PREVIOUS command's
   outcome -- which on a write is the difference between "it worked" and
   data somewhere else. The device can therefore be told to:

     - stall the data phase        (a refused command: legal, and the
                                    recovery path that matters most)
     - stall the status phase      (the specification's two-attempt rule)
     - answer with a bad signature (a data packet where a CSW belongs)
     - answer with the WRONG TAG   (the desync itself)
     - report a phase error        (the device saying the phases did not
                                    line up)
     - return a short CSW
     - refuse the first commands with UNIT ATTENTION, which nearly every
       real stick does and which a driver that believes the first answer
       reports as "not present"
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-54s got %ld\n", what, got); }
}

/* usb/msc.c's view of the world, before it is included. */
#include "../include/types.h"
#include "../include/usb.h"

#define EP_IN   0x81
#define EP_OUT  0x02

/* ── the simulated stick ────────────────────────────────────────────── */
#define SIM_MAX_BYTES  (512u * 64u)

typedef enum { PH_CBW = 0, PH_DATA_IN, PH_DATA_OUT, PH_CSW } phase_t;

static struct {
    uint8_t  media[SIM_MAX_BYTES];
    uint32_t bsize, blocks;
    int      wp;             /* MODE SENSE reports write protected   */
    int      rmb;            /* INQUIRY removable-medium bit         */
    int      devtype;        /* INQUIRY peripheral device type       */
    int      no_mode_sense;  /* device stalls MODE SENSE -- common    */
    int      maxlun;         /* GET_MAX_LUN answer, -1 to stall it    */
    int      unit_attention; /* refuse this many commands first      */
    int      no_medium;      /* a card reader with an empty slot     */

    /* injection */
    int      stall_data;     /* stall the next N data phases         */
    int      stall_csw;      /* stall the next N status phases       */
    int      bad_sig;        /* wrong CSW signature, N times         */
    int      bad_tag;        /* wrong CSW tag, N times               */
    int      phase_err;      /* report a phase error, N times        */
    int      short_csw;      /* return a short CSW, N times          */
    int      fail_cmd;       /* CSW status 1 on the next N commands  */

    /* observation */
    phase_t  phase;
    uint32_t tag;            /* the tag of the command in flight     */
    uint8_t  cdb[16];
    int      cdblen;
    uint32_t dlen;
    int      din;            /* the CBW said device-to-host          */
    uint32_t residue;
    uint8_t  status;
    uint8_t  pending[SIM_MAX_BYTES];
    uint32_t pending_len;
    /* The host-to-device buffer, separate from the device-to-host one.
       They were one field at first and sim_exec() clears the outgoing
       one as it starts, so WRITE(10) found the data it had just been
       sent already zeroed -- a bug in the SIMULATOR that showed up as
       four failing writes. Two buffers, because the two directions are
       two things. */
    uint8_t  outbuf[SIM_MAX_BYTES];
    uint32_t outlen;

    int      resets;
    int      halt_in, halt_out;
    /* A halted endpoint stays halted until the HOST clears it. The first
       version of this device only refused the status read while halted,
       which made "the driver never cleared the halt" indistinguishable
       from "it did" -- the driver's stalled-status path cleared it on the
       way past and the command's RESULT was the same either way.
       tools/check_msc_suite.py reported that injection MISSED.

       A real endpoint refuses everything. These count the refusals, so a
       test can assert the driver cleared the halt BEFORE its next
       transfer rather than that it cleared it eventually. */
    int      refused_while_halted;
    int      halt_on_phase_err;    /* devices commonly halt both       */
    int      bad_cbw;              /* a CBW the device could not use   */
    uint32_t tags_seen[64];
    int      ntags;
    int      clears_in, clears_out;
    int      out_of_order;   /* a phase arrived where another belonged */
    int      cbws;
    int      sense_key, sense_asc, sense_ascq;
    int      sense_reads;
    int      writes_done;
} S;

static uint32_t rd32le(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void wr32le(uint8_t *p, uint32_t v){
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
    p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static uint32_t rd32be(const uint8_t *p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static void wr32be(uint8_t *p, uint32_t v){
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8);  p[3]=(uint8_t)v;
}

static void sim_sense(int k, int asc, int ascq){
    S.sense_key = k; S.sense_asc = asc; S.sense_ascq = ascq;
}

/* Run the SCSI command in S.cdb. Sets S.pending/pending_len for a
   device-to-host command and S.status for the CSW. */
static void sim_exec(void)
{
    S.pending_len = 0;
    S.status = 0;
    S.residue = 0;

    if(S.fail_cmd > 0 && S.cdb[0] != 0x03){   /* never fail REQUEST SENSE */
        S.fail_cmd--;
        S.status = 1;
        sim_sense(0x03, 0x11, 0x00);          /* MEDIUM ERROR            */
        return;
    }

    /* UNIT ATTENTION, which almost every stick reports for its first
       command after being addressed. REQUEST SENSE is exempt -- it is
       how the host clears the condition. */
    if(S.unit_attention > 0 && S.cdb[0] != 0x03){
        S.unit_attention--;
        S.status = 1;
        sim_sense(0x06, 0x28, 0x00);          /* not ready to ready      */
        return;
    }

    switch(S.cdb[0]){
    case 0x00:   /* TEST UNIT READY */
        if(S.no_medium){ S.status = 1; sim_sense(0x02, 0x3A, 0x00); }
        break;

    case 0x03:   /* REQUEST SENSE */
        S.sense_reads++;
        memset(S.pending, 0, 18);
        S.pending[0] = 0x70;
        S.pending[2] = (uint8_t)S.sense_key;
        S.pending[7] = 10;
        S.pending[12] = (uint8_t)S.sense_asc;
        S.pending[13] = (uint8_t)S.sense_ascq;
        S.pending_len = 18;
        sim_sense(0, 0, 0);          /* reading it clears it */
        break;

    case 0x12: {  /* INQUIRY */
        memset(S.pending, ' ', 36);
        S.pending[0] = (uint8_t)S.devtype;
        S.pending[1] = S.rmb ? 0x80 : 0x00;
        S.pending[2] = 0x04;
        S.pending[3] = 0x02;
        S.pending[4] = 31;
        memcpy(S.pending + 8,  "Generic ", 8);
        memcpy(S.pending + 16, "Flash Disk      ", 16);
        S.pending_len = 36;
        break; }

    case 0x1A:   /* MODE SENSE (6) */
        if(S.no_mode_sense){ S.status = 1; sim_sense(0x05, 0x20, 0x00); break; }
        memset(S.pending, 0, 4);
        S.pending[0] = 3;
        S.pending[2] = S.wp ? 0x80 : 0x00;
        S.pending_len = 4;
        break;

    case 0x25:   /* READ CAPACITY (10) */
        memset(S.pending, 0, 8);
        wr32be(S.pending + 0, S.blocks - 1u);   /* LAST lba */
        wr32be(S.pending + 4, S.bsize);
        S.pending_len = 8;
        break;

    case 0x28: { /* READ (10) */
        uint32_t lba = rd32be(S.cdb + 2);
        uint32_t n   = (uint32_t)((S.cdb[7] << 8) | S.cdb[8]);
        if(lba + n > S.blocks || n * S.bsize > SIM_MAX_BYTES){
            S.status = 1; sim_sense(0x05, 0x21, 0x00); break;   /* LBA OOR */
        }
        memcpy(S.pending, S.media + lba * S.bsize, n * S.bsize);
        S.pending_len = n * S.bsize;
        break; }

    case 0x2A: { /* WRITE (10) */
        uint32_t lba = rd32be(S.cdb + 2);
        uint32_t n   = (uint32_t)((S.cdb[7] << 8) | S.cdb[8]);
        if(S.wp){ S.status = 1; sim_sense(0x07, 0x27, 0x00); break; }
        if(lba + n > S.blocks){
            S.status = 1; sim_sense(0x05, 0x21, 0x00); break;
        }
        /* the data arrived in S.outbuf during the OUT phase */
        if(S.outlen >= n * S.bsize){
            memcpy(S.media + lba * S.bsize, S.outbuf, n * S.bsize);
            S.writes_done++;
        } else {
            S.status = 2;      /* the host sent less than it promised */
        }
        S.outlen = 0;
        break; }

    default:
        S.status = 1;
        sim_sense(0x05, 0x20, 0x00);           /* INVALID COMMAND */
        break;
    }
}

/* ── the transport, as the core would present it ────────────────────── */
int usb_bulk(const usb_dev_t *d, uint8_t ep, int in, void *data, int len,
             int mps)
{
    (void)d; (void)mps;

    /* A halted endpoint refuses everything until the host clears it. */
    if(!in && S.halt_out){ S.refused_while_halted++; return USB_ESTALL; }
    if(in  && S.halt_in ){ S.refused_while_halted++; return USB_ESTALL; }

    if(!in && ep == EP_OUT && S.phase == PH_CBW){
        const uint8_t *c = (const uint8_t *)data;
        if(len != 31) { S.out_of_order++; return USB_EXFER; }
        if(rd32le(c) != 0x43425355u) { S.out_of_order++; return USB_EXFER; }
        S.cbws++;
        /* bCBWCBLength is how the device knows how much of the 16-byte
           field is a command. A device cannot guess, and one that is
           sent 0 has nothing to run -- so it refuses the wrapper, which
           is what a real device does with a CBW it cannot use. */
        if(c[14] < 1 || c[14] > 16){
            S.bad_cbw++; S.halt_out = 1; return USB_ESTALL;
        }
        /* Every tag the device has been sent, so a test can assert that
           no two commands share one. A device that merely echoes what it
           is given cannot tell a fresh tag from a reused one, and
           "the tag is not incremented" was MISSED for exactly that
           reason. */
        if(S.ntags < 64) S.tags_seen[S.ntags++] = rd32le(c + 4);
        S.tag    = rd32le(c + 4);
        S.dlen   = rd32le(c + 8);
        S.din    = (c[12] & 0x80) ? 1 : 0;
        S.cdblen = c[14];
        memcpy(S.cdb, c + 15, 16);
        S.pending_len = 0;
        S.outlen = 0;

        if(S.din || S.dlen == 0){
            sim_exec();
            S.phase = (S.din && S.dlen) ? PH_DATA_IN : PH_CSW;
        } else {
            S.phase = PH_DATA_OUT;      /* wait for the data, then run */
        }
        return 31;
    }

    if(!in && ep == EP_OUT && S.phase == PH_DATA_OUT){
        if(S.stall_data > 0){
            S.stall_data--; S.halt_out = 1;
            S.phase = PH_CSW; S.status = 1;
            sim_sense(0x05, 0x20, 0x00);
            return USB_ESTALL;
        }
        if((uint32_t)len > SIM_MAX_BYTES) return USB_EXFER;
        memcpy(S.outbuf, data, (size_t)len);
        S.outlen = (uint32_t)len;
        sim_exec();
        S.phase = PH_CSW;
        return len;
    }

    if(in && ep == EP_IN && S.phase == PH_DATA_IN){
        uint32_t n;
        if(S.stall_data > 0){
            S.stall_data--; S.halt_in = 1;
            S.phase = PH_CSW; S.status = 1;
            sim_sense(0x05, 0x20, 0x00);
            return USB_ESTALL;
        }
        n = S.pending_len;
        if(n > (uint32_t)len) n = (uint32_t)len;
        memcpy(data, S.pending, n);
        if(S.dlen > n) S.residue = S.dlen - n;
        S.phase = PH_CSW;
        return (int)n;
    }

    if(in && ep == EP_IN && S.phase == PH_CSW){
        uint8_t *o = (uint8_t *)data;
        if(S.stall_csw > 0){
            S.stall_csw--; S.halt_in = 1;
            return USB_ESTALL;
        }
        if(len < 13) return USB_EXFER;
        memset(o, 0, 13);
        wr32le(o + 0, S.bad_sig > 0 ? (S.bad_sig--, 0x41424344u)
                                    : 0x53425355u);
        wr32le(o + 4, S.bad_tag > 0 ? (S.bad_tag--, S.tag ^ 0xFFFFu) : S.tag);
        wr32le(o + 8, S.residue);
        if(S.phase_err > 0){
            S.phase_err--;
            o[12] = 2;
            /* A device reporting a phase error commonly halts both
               endpoints with it, and they stay halted until the host
               clears them. Mass Storage Reset tells the DEVICE to
               abandon the command; the halts are the host's side of the
               same thing, and one without the other leaves half the
               pipe thinking the command is still live. */
            if(S.halt_on_phase_err){ S.halt_in = 1; S.halt_out = 1; }
        } else {
            o[12] = S.status;
        }
        S.phase = PH_CBW;
        if(S.short_csw > 0){ S.short_csw--; return 12; }
        return 13;
    }

    /* Anything else is a phase arriving where another belonged, which is
       exactly what a desynchronised driver does. Recorded rather than
       tolerated: this is the failure this suite exists to see. */
    S.out_of_order++;
    return USB_EXFER;
}

int usb_control(const usb_dev_t *d, uint8_t rtype, uint8_t req,
                uint16_t value, uint16_t index, void *data, uint16_t len)
{
    (void)d; (void)value; (void)index;

    if(rtype == (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) &&
       req == 0xFF){
        S.resets++;
        S.phase = PH_CBW;
        S.pending_len = 0;
        S.outlen = 0;
        /* Deliberately does NOT clear S.halt_in/S.halt_out. The class
           reset is the device's half; the halts are the host's, and a
           driver that does only one of the two is the injection this
           models. */
        return 0;
    }
    if(rtype == (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) &&
       req == 0xFE){
        if(S.maxlun < 0) return USB_ESTALL;      /* legal: one LUN */
        if(len < 1 || !data) return USB_EXFER;
        ((uint8_t *)data)[0] = (uint8_t)S.maxlun;
        return 1;
    }
    return USB_EXFER;
}

int usb_clear_halt(const usb_dev_t *d, uint8_t ep, int in)
{
    (void)d; (void)ep;
    if(in){ S.halt_in = 0; S.clears_in++; }
    else  { S.halt_out = 0; S.clears_out++; }
    return USB_OK;
}

/* The device table the probe walks. One device, or none. */
static usb_dev_t g_tbl[2];
static int       g_ntbl;
int              usb_device_count(void){ return g_ntbl; }
const usb_dev_t *usb_device(int i){
    return (i >= 0 && i < g_ntbl) ? &g_tbl[i] : 0;
}

/* usb_find_ep() is the real core's and worth using, but the core is not
   in this binary -- it would drag in two back-ends. This is the same
   search over the same fields. */
const usb_ep_t *usb_find_ep(const usb_dev_t *d, int attr, int in)
{
    int i;
    if(!d) return 0;
    for(i = 0; i < d->nep; i++){
        if((d->ep[i].attr & 3) != attr) continue;
        if(((d->ep[i].addr & 0x80) ? 1 : 0) != (in ? 1 : 0)) continue;
        return &d->ep[i];
    }
    return 0;
}

static int g_sleeps;
void sleep_ms(uint32_t ms){ (void)ms; g_sleeps++; }
void klog(int lvl, const char *fmt, ...){ (void)lvl; (void)fmt; }

#include "../usb/msc.c"

/* ── setting the stage ──────────────────────────────────────────────── */
static void stick(uint32_t bsize, uint32_t blocks)
{
    memset(&S, 0, sizeof S);
    S.bsize = bsize; S.blocks = blocks;
    S.rmb = 1; S.devtype = 0x00; S.maxlun = 0;
    S.phase = PH_CBW;
    S.halt_on_phase_err = 1;
    /* recognisable content: every block starts with its own number */
    { uint32_t b;
      for(b = 0; b < blocks && (b + 1) * bsize <= SIM_MAX_BYTES; b++){
          memset(S.media + b * bsize, (int)(b & 0xFF), bsize);
          S.media[b * bsize] = (uint8_t)(0xA0 + (b & 0x0F));
      } }

    memset(g_tbl, 0, sizeof g_tbl);
    g_tbl[0].valid        = 1;
    g_tbl[0].addr         = 3;
    g_tbl[0].if_class     = 0x08;
    g_tbl[0].if_subclass  = 0x06;
    g_tbl[0].if_protocol  = 0x50;
    g_tbl[0].ifnum        = 0;
    g_tbl[0].nep          = 2;
    g_tbl[0].ep[0].addr   = EP_IN;  g_tbl[0].ep[0].attr = USB_EP_BULK;
    g_tbl[0].ep[0].mps    = 512;
    g_tbl[0].ep[1].addr   = EP_OUT; g_tbl[0].ep[1].attr = USB_EP_BULK;
    g_tbl[0].ep[1].mps    = 512;
    g_ntbl = 1;
    g_sleeps = 0;
}

static uint8_t buf[4096];

int main(void)
{
    printf("USB mass storage: Bulk-Only Transport and SCSI over it\n");
    printf("====================================================\n");

    head("a plain 512-byte stick comes up");
    stick(512, 32);
    ck("one device probed", msc_probe() == 1, msc_count());
    ck("its size is blocks x block size, in 512-byte sectors",
       msc_sectors(0) == 32, (long)msc_sectors(0));
    ck("the block size is reported as the DEVICE's",
       msc_blocksize(0) == 512, (long)msc_blocksize(0));
    ck("it is writable", !msc_readonly(0), 0);
    ck("INQUIRY's removable bit is carried", msc_removable(0), 0);
    ck("the vendor string is trimmed, not padded",
       !strcmp(msc_vendor(0), "Generic"), 0);
    ck("and so is the product", !strcmp(msc_product(0), "Flash Disk"), 0);
    ck("the probe reset the device before its first command",
       S.resets >= 1, S.resets);
    ck("no phase ever arrived out of order", S.out_of_order == 0,
       S.out_of_order);

    head("EVERY COMMAND CARRIES A TAG NO OTHER COMMAND USED");
    /* The tag is the only thing that can tell this command's status from
       the last one's, and tools/check_msc_suite.py showed that a driver
       which never increments it passes every result-level check: a
       device that echoes what it is given cannot tell a fresh tag from a
       reused one. So the device records what it was SENT and the
       property is checked on the wire. */
    stick(512, 32); msc_probe();
    msc_read_sector(0, 1, buf);
    msc_read_sector(0, 2, buf);
    msc_write_sector(0, 3, buf);
    ck("the device saw several commands", S.ntags >= 6, S.ntags);
    { int i, j, dup = 0;
      for(i = 0; i < S.ntags; i++)
          for(j = i + 1; j < S.ntags; j++)
              if(S.tags_seen[i] == S.tags_seen[j]) dup++;
      ck("no two of them shared a tag", dup == 0, dup); }
    { int i, zero = 0;
      for(i = 0; i < S.ntags; i++) if(S.tags_seen[i] == 0) zero++;
      /* 0 is skipped on purpose: a device that zeroes the field instead
         of echoing it would otherwise look like a match. */
      ck("and none of them was zero", zero == 0, zero); }
    ck("the command block length was always sent",
       S.bad_cbw == 0, S.bad_cbw);

    head("reading a sector gives the sector that was asked for");
    stick(512, 32); msc_probe();
    memset(buf, 0, sizeof buf);
    ck("sector 0 reads", msc_read_sector(0, 0, buf) == 1, 0);
    ck("...and it is sector 0's content", buf[0] == 0xA0, buf[0]);
    ck("sector 5 reads", msc_read_sector(0, 5, buf) == 1, 0);
    ck("...and it is sector 5's", buf[0] == 0xA5, buf[0]);
    ck("sector 31, the last one, reads", msc_read_sector(0, 31, buf) == 1, 0);
    { int before = S.cbws;
      ck("sector 32 is past the end and is refused",
         msc_read_sector(0, 32, buf) == 0, 0);
      /* Refused by the driver's own bound, not by the device saying no.
         A READ(10) for a block past the end is a stall on real hardware
         and a recovery the host did not need to pay for. */
      ck("...without sending a command to the device",
         S.cbws == before, S.cbws - before);
      ck("a huge LBA is refused too",
         msc_read_sector(0, 0xFFFFFFFFull, buf) == 0, 0);
      ck("...also without a command", S.cbws == before, S.cbws - before); }
    ck("still no phase out of order", S.out_of_order == 0, S.out_of_order);

    head("writing a sector puts it where it was addressed");
    stick(512, 32); msc_probe();
    memset(buf, 0x5A, 512);
    ck("the write reports success", msc_write_sector(0, 7, buf) == 1, 0);
    ck("the device actually wrote once", S.writes_done == 1, S.writes_done);
    ck("...and the media holds it", S.media[7*512] == 0x5A, S.media[7*512]);
    memset(buf, 0, 512);
    ck("reading it back gives it", msc_read_sector(0, 7, buf) == 1, 0);
    ck("...byte for byte", buf[0] == 0x5A && buf[511] == 0x5A, buf[0]);
    ck("sector 6 was not touched", S.media[6*512] == 0xA6, S.media[6*512]);
    ck("a write past the end is refused",
       msc_write_sector(0, 32, buf) == 0, 0);

    head("UNIT ATTENTION on the first commands, which real sticks do");
    stick(512, 32);
    S.unit_attention = 3;
    ck("it still comes up", msc_probe() == 1, msc_count());
    ck("and the sense data was read, so the condition is cleared",
       S.sense_reads >= 1, S.sense_reads);
    ck("the size is right despite the slow start",
       msc_sectors(0) == 32, (long)msc_sectors(0));
    /* The bound matters as much as the retry: a device that never becomes
       ready must not hold the boot. */
    stick(512, 32);
    S.unit_attention = 10000;
    ck("a device that never readies is refused, not waited on for ever",
       msc_probe() == 0, msc_count());

    head("an empty card-reader slot is not an error");
    stick(512, 32);
    S.no_medium = 1;
    ck("no medium: not registered", msc_probe() == 0, msc_count());
    ck("and it did not retry ten times to find that out",
       S.cbws < 8, S.cbws);

    head("A STALLED DATA PHASE IS RECOVERED AND THE CSW STILL COLLECTED");
    /* The path that matters most. A device refusing a command stalls the
       data endpoint and still has a status wrapper for you; a driver that
       treats the stall as the end leaves that wrapper in the pipe for the
       NEXT command to read as its own, and from there every answer is one
       command out of step. */
    stick(512, 32); msc_probe();
    S.stall_data = 1;
    { int refused = S.refused_while_halted;
      ck("the read reports failure", msc_read_sector(0, 3, buf) == 0, 0);
      ck("the host cleared the halt", S.clears_in >= 1, S.clears_in);
      /* BEFORE its next transfer, not eventually. A halted endpoint
         refuses everything, and a driver that leaves it halted has its
         status read refused too -- which its stalled-status path then
         recovers from, making the command's RESULT identical. Checking
         the result is why "the halt is not cleared" was MISSED. */
      ck("...before reading the status, not after being refused once",
         S.refused_while_halted == refused, S.refused_while_halted); }
    ck("THE NEXT COMMAND IS NOT ONE OUT OF STEP",
       msc_read_sector(0, 4, buf) == 1, 0);
    ck("...and it returned sector 4, not sector 3",
       buf[0] == 0xA4, buf[0]);
    ck("no phase arrived out of order through all of it",
       S.out_of_order == 0, S.out_of_order);

    head("a stalled status phase: cleared and asked once more");
    stick(512, 32); msc_probe();
    { int before = S.clears_in;
      S.stall_csw = 1;
      ck("the command still succeeds", msc_read_sector(0, 2, buf) == 1, 0);
      ck("...and it is the right sector", buf[0] == 0xA2, buf[0]);
      ck("the halt was cleared", S.clears_in > before, S.clears_in); }
    /* Twice is not retried for ever: a device that stalls everything must
       not spin. */
    stick(512, 32); msc_probe();
    S.stall_csw = 5;
    ck("a device that stalls every status read is given up on",
       msc_read_sector(0, 2, buf) == 0, 0);
    ck("...and reset rather than left halted", S.resets >= 2, S.resets);

    head("THE WRONG TAG IS NOT BELIEVED");
    /* The desync itself. A status wrapper for a different command, which
       on a write is the difference between "it worked" and data
       somewhere else. */
    stick(512, 32); msc_probe();
    { int before = S.resets;
      S.bad_tag = 1;
      ck("the command fails rather than reporting another's status",
         msc_read_sector(0, 1, buf) == 0, 0);
      ck("the device was reset", S.resets > before, S.resets);
      ck("and the next command works", msc_read_sector(0, 1, buf) == 1, 0);
      ck("...with the right data", buf[0] == 0xA1, buf[0]);
      ck("...and nothing was refused for being left halted",
         S.refused_while_halted == 0, S.refused_while_halted); }

    head("a bad signature is not a status wrapper");
    stick(512, 32); msc_probe();
    { int before = S.resets;
      S.bad_sig = 1;
      ck("the command fails", msc_read_sector(0, 1, buf) == 0, 0);
      ck("the device was reset", S.resets > before, S.resets);
      ck("and recovery works", msc_read_sector(0, 1, buf) == 1, 0); }

    head("a phase error is a reset, and the reset clears BOTH halts");
    /* The device halts both endpoints with a phase error and they stay
       halted until the host clears them: the class reset is the device's
       half and the halts are the host's. A driver that does only one of
       the two leaves half the pipe thinking the command is still live,
       and the NEXT command is the one that pays. */
    stick(512, 32); msc_probe();
    { int before = S.resets;
      S.phase_err = 1;
      ck("the command fails", msc_read_sector(0, 1, buf) == 0, 0);
      ck("the device was reset", S.resets > before, S.resets);
      ck("the OUT halt was cleared", S.halt_out == 0, S.halt_out);
      ck("the IN halt was cleared",  S.halt_in == 0,  S.halt_in);
      /* Cleared by the reset the phase error called for, not by the
         next command tripping over them. A driver that carries on from
         a phase error has its sense read refused at the command phase
         and recovers there by accident -- which leaves the halts clear
         by the time anybody looks, and is why "a phase error is not a
         reset" was MISSED. */
      ck("...by the reset, not by something being refused first",
         S.refused_while_halted == 0, S.refused_while_halted);
      /* One command, not two: with a halt left behind, this one is
         refused at its command phase and fails. */
      ck("and the very next command works",
         msc_read_sector(0, 1, buf) == 1, 0);
      ck("...with the right data", buf[0] == 0xA1, buf[0]); }

    head("a short status wrapper is not accepted as a short one");
    stick(512, 32); msc_probe();
    { int before = S.resets;
      S.short_csw = 1;
      ck("the command fails", msc_read_sector(0, 1, buf) == 0, 0);
      ck("the device was reset", S.resets > before, S.resets); }

    head("a failed command reports the device's sense, not just zero");
    stick(512, 32); msc_probe();
    S.fail_cmd = 1;
    { int before = S.sense_reads;
      ck("the read fails", msc_read_sector(0, 1, buf) == 0, 0);
      ck("and the sense was read, so the condition is cleared",
         S.sense_reads > before, S.sense_reads);
      ck("the next read works", msc_read_sector(0, 1, buf) == 1, 0); }

    head("a write-protected stick is read-only before anything is written");
    stick(512, 32);
    S.wp = 1;
    ck("it comes up", msc_probe() == 1, msc_count());
    ck("and reports read-only", msc_readonly(0), 0);
    memset(buf, 1, 512);
    { int cmds = S.cbws;
      ck("a write is refused by the driver",
         msc_write_sector(0, 1, buf) == 0, 0);
      /* No COMMAND, not merely no write. The device would refuse a
         WRITE(10) itself, so counting completed writes cannot tell the
         driver's refusal from the device's -- which is why "a write to a
         read-only device is attempted" was MISSED. */
      ck("...without sending a command at all", S.cbws == cmds,
         S.cbws - cmds); }

    head("a device that stalls MODE SENSE is not refused for it");
    /* Plenty of sticks do not implement it, and a driver that treats a
       missing optional command as a fault rejects working hardware. */
    stick(512, 32);
    S.no_mode_sense = 1;
    ck("it still comes up", msc_probe() == 1, msc_count());
    ck("and is writable, because nothing said otherwise",
       !msc_readonly(0), 0);
    ck("the stall was recovered, not left halted",
       S.halt_in == 0 && S.halt_out == 0, S.halt_in);

    head("a CD-ROM is a mass storage device and not an install target");
    /* 512-byte blocks on purpose. The first version of this used 2048,
       which is read-only for a DIFFERENT reason -- the block size -- so
       the device type was never actually what the check depended on and
       tools/check_msc_suite.py reported "a CD-ROM is offered as
       writable" MISSED. One reason per check. */
    stick(512, 16);
    S.devtype = 0x05;
    ck("it comes up", msc_probe() == 1, msc_count());
    ck("and is read-only because of its DEVICE TYPE, not its blocks",
       msc_readonly(0) && msc_blocksize(0) == 512, (long)msc_blocksize(0));

    head("block sizes other than 512");
    /* Reads work by offsetting into the device block; writes are refused
       because a torn read-modify-write on REMOVABLE media loses seven
       sectors nobody asked to write. drivers/nvme.c does the
       read-modify-write because an NVMe drive cannot be pulled out of
       the machine half way through. */
    stick(4096, 8);
    ck("a 4096-byte-block device comes up", msc_probe() == 1, msc_count());
    ck("its size is in 512-byte sectors: 8 blocks x 8",
       msc_sectors(0) == 64, (long)msc_sectors(0));
    ck("and it is read-only", msc_readonly(0), 0);
    memset(buf, 0, 512);
    ck("sector 0 reads", msc_read_sector(0, 0, buf) == 1, 0);
    ck("...and it is the first 512 bytes of block 0",
       buf[0] == 0xA0, buf[0]);
    ck("sector 8 reads", msc_read_sector(0, 8, buf) == 1, 0);
    ck("...and it is the start of block 1, not block 8",
       buf[0] == 0xA1, buf[0]);
    ck("sector 9 is the SECOND 512 bytes of block 1",
       msc_read_sector(0, 9, buf) == 1 && buf[0] == 0x01, buf[0]);
    ck("sector 63, the last, reads", msc_read_sector(0, 63, buf) == 1, 0);
    ck("sector 64 is past the end", msc_read_sector(0, 64, buf) == 0, 0);
    ck("a write is refused", msc_write_sector(0, 0, buf) == 0, 0);

    stick(2048, 16);
    ck("a 2048-byte-block device comes up", msc_probe() == 1, msc_count());
    ck("32 sectors of 512", msc_sectors(0) == 64, (long)msc_sectors(0));
    ck("sector 4 is the start of block 1",
       msc_read_sector(0, 4, buf) == 1 && buf[0] == 0xA1, buf[0]);

    head("a block size this driver cannot handle is refused, not guessed");
    stick(520, 8);                /* not a multiple of 512 */
    ck("520-byte blocks: refused", msc_probe() == 0, msc_count());
    stick(8192, 4);               /* past the bounce buffer */
    ck("8192-byte blocks: refused", msc_probe() == 0, msc_count());
    stick(512, 32); S.bsize = 0;
    ck("a device reporting zero-byte blocks: refused",
       msc_probe() == 0, msc_count());

    head("what is not a Bulk-Only SCSI device is not driven");
    stick(512, 32);
    g_tbl[0].if_protocol = 0x01;        /* CBI */
    ck("a CBI device is refused", msc_probe() == 0, msc_count());
    stick(512, 32);
    g_tbl[0].if_subclass = 0x04;        /* UFI, floppy */
    ck("a non-SCSI command set is refused", msc_probe() == 0, msc_count());
    stick(512, 32);
    g_tbl[0].if_class = 0x03;           /* HID */
    ck("a keyboard is not storage", msc_probe() == 0, msc_count());
    stick(512, 32);
    g_tbl[0].nep = 1;                   /* IN only */
    ck("one bulk endpoint is not enough", msc_probe() == 0, msc_count());
    stick(512, 32);
    g_tbl[0].ep[0].attr = USB_EP_INTR;  /* interrupt, not bulk */
    ck("an interrupt endpoint is not a bulk one",
       msc_probe() == 0, msc_count());
    stick(512, 32);
    g_tbl[0].valid = 0;
    ck("an invalid table entry is skipped", msc_probe() == 0, msc_count());
    g_ntbl = 0;
    ck("an empty device table probes to nothing", msc_probe() == 0,
       msc_count());

    head("GET_MAX_LUN: a stall means one LUN, which is not a failure");
    stick(512, 32);
    S.maxlun = -1;
    ck("a device that stalls it still comes up", msc_probe() == 1,
       msc_count());
    stick(512, 32);
    S.maxlun = 3;                 /* a four-slot card reader */
    ck("a multi-LUN device comes up", msc_probe() == 1, msc_count());
    ck("...on LUN 0, which is all 0.8.6 drives",
       msc_sectors(0) == 32, (long)msc_sectors(0));

    head("an index that is not a device");
    stick(512, 32); msc_probe();
    ck("sectors of index 1 is 0", msc_sectors(1) == 0, (long)msc_sectors(1));
    ck("index 1 is read-only",    msc_readonly(1), 0);
    ck("a negative index is read-only", msc_readonly(-1), 0);
    ck("a read of index 1 fails", msc_read_sector(1, 0, buf) == 0, 0);
    ck("a write of index 1 fails", msc_write_sector(1, 0, buf) == 0, 0);
    { int cmds = S.cbws;
      ck("a read with no buffer fails", msc_read_sector(0, 0, 0) == 0, 0);
      ck("a write with no buffer fails", msc_write_sector(0, 0, 0) == 0, 0);
      /* Again on the wire. Without the guard the command goes out, the
         data phase is skipped because the pointer is null, the status
         read collects sector data as a wrapper and the signature check
         fails -- so the RESULT is 0 either way. */
      ck("...and neither sent a command", S.cbws == cmds, S.cbws - cmds); }

    head("a rescan replaces rather than appends");
    stick(512, 32);
    ck("one device", msc_probe() == 1, msc_count());
    ck("one again, not two", msc_rescan() == 1, msc_count());
    ck("and again", msc_rescan() == 1, msc_count());

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
