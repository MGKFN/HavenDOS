/* ════════════════════════════════════════════════════════════════════
   usb/msc.c -- USB mass storage: Bulk-Only Transport, and SCSI over it.

   This is 0.8.6 "Argo". RM-1003 puts it here because 0.8.5 answered the
   hardware question it existed to ask -- the xHCI works on the HP, HID
   enumerates, a whole command types at speed -- so the bus that input,
   removable storage and the only available network adapter all sit
   behind is real now rather than simulated.

   ── WHAT BULK-ONLY TRANSPORT IS ──────────────────────────────────────

   A stick has two bulk endpoints and no state machine worth the name.
   Every operation is three phases:

     1. a 31-byte Command Block Wrapper out the bulk OUT endpoint,
        carrying a SCSI command inside it
     2. the data, in whichever direction the wrapper said
     3. a 13-byte Command Status Wrapper in from the bulk IN endpoint

   That is the whole protocol. What makes it worth 600 lines is phase 3:
   the status wrapper is where a device tells you it refused, and the
   ways it can fail to tell you that are the entire difficulty.

   ── WHY THE RECOVERY PATHS ARE THE SUBSTANCE ─────────────────────────

   A SCSI command that a device cannot do STALLS the data endpoint and
   then expects you to clear the halt and come back for the status. A
   driver that treats a stall as a transfer error loses the CSW, the next
   command reads the PREVIOUS command's status, and from there every
   answer is one command out of step -- which on a write means data
   landing where the status of something else said it would.

   That is not hypothetical: it is the shape of the bug 0.8.5.1 shipped
   (a poll that left a live TRB behind) and the shape of v0.8.5.3's
   (a status register read before it had caught up). A protocol with a
   status phase has a way to desynchronise, and the only defence is to
   treat the tag as load-bearing. So:

     - every CBW carries an incrementing tag, and a CSW whose tag does
       not match the command just sent is not believed
     - a stalled data phase is recovered and the CSW still collected
     - a stalled status phase is recovered and the CSW read once more
     - a phase error, a bad signature or a mismatched tag resets the
       device through the class's own Mass Storage Reset and gives up on
       that command rather than guessing

   ── WHAT IS NOT DONE, STATED HERE AND IN THE CHANGELOG ───────────────

   - **Writes to a device whose block size is not 512 are refused.** A
     4096-byte-block stick would need read-modify-write, which is what
     drivers/nvme.c does -- but an NVMe drive cannot be pulled out of
     the machine half way through, and a stick can. A torn 4096-byte
     block on removable media loses seven sectors nobody asked to write.
     Reads work at any block size up to 4096. The disk reports read-only
     and the installer already has a row for that: "preview only - no
     write path".
   - **One LUN.** GET_MAX_LUN is asked and the answer is logged, but only
     LUN 0 is registered. A card reader with four slots presents four
     LUNs; this release sees the first. Stated rather than discovered.
   - **No hot-plug.** Nothing in this kernel runs periodically, so a
     stick plugged in after boot appears on `usb rescan` / `disk rescan`
     and not on its own.
   ════════════════════════════════════════════════════════════════════ */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/usb.h"
#include "../include/klog.h"

extern void sleep_ms(uint32_t ms);

/* ── the class ───────────────────────────────────────────────────────
   Numbers from the USB Mass Storage Class specifications. Theirs, not
   ours, and nothing above this file has any use for them. */
#define MSC_CLASS            0x08
#define MSC_SUB_SCSI         0x06   /* SCSI transparent command set     */
#define MSC_SUB_SFF8070      0x05   /* SFF-8070i: SCSI with extras      */
#define MSC_SUB_RBC          0x01
#define MSC_PROTO_BOT        0x50   /* Bulk-Only Transport              */

#define MSC_REQ_MAX_LUN      0xFE
#define MSC_REQ_RESET        0xFF

#define CBW_SIG              0x43425355u   /* "USBC" little-endian      */
#define CSW_SIG              0x53425355u   /* "USBS"                    */
#define CBW_LEN              31
#define CSW_LEN              13

#define CSW_PASS             0
#define CSW_FAIL             1
#define CSW_PHASE            2

/* ── SCSI ─────────────────────────────────────────────────────────── */
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_MODE_SENSE6     0x1A
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A

#define MSC_MAX_DEV          4
#define MSC_BLOCK_MAX        4096   /* the bounce buffer, and the limit */

typedef struct {
    int       valid;
    usb_dev_t dev;          /* a COPY -- see the note on msc_probe()    */
    uint8_t   ep_in,  ep_out;
    uint16_t  mps_in, mps_out;
    uint8_t   lun;
    uint8_t   maxlun;       /* what the device reported, for the log    */
    uint8_t   readonly;
    uint8_t   removable;
    uint32_t  blocks;       /* in the device's own block size           */
    uint32_t  bsize;        /* bytes per block, 512..4096              */
    uint32_t  per;          /* 512-byte sectors per device block        */
    uint32_t  per_shift;    /* log2(per); no 64-bit divide in a kernel  */
    uint64_t  sectors;      /* blocks * per, what the block layer sees  */
    uint32_t  tag;
    char      vendor[9];
    char      product[17];
} msc_dev_t;

static msc_dev_t g_msc[MSC_MAX_DEV];
static int       g_nmsc;
static int       g_probed;

/* One bounce buffer, used inside a call and never held. Static because a
   4 KB automatic is most of a kernel stack frame, and this is reached
   from the filesystem which is already several frames deep. */
static uint8_t g_blk[MSC_BLOCK_MAX];
static uint8_t g_cbw[CBW_LEN];
static uint8_t g_csw[CSW_LEN];

/* ── little helpers ──────────────────────────────────────────────── */
static void st32le(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t ld32le(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* SCSI is big-endian on the wire, which is the opposite of everything
   else in this file. Two functions so the two never get confused -- a
   READ CAPACITY parsed little-endian reports a 512 GB stick as 2 TB and
   the mistake only shows up past the first few sectors. */
static uint32_t ld32be(const uint8_t *p){
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static void st32be(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* ── the class's own reset ───────────────────────────────────────────
 *
 * Mass Storage Reset, then clear the halt on BOTH endpoints, in that
 * order, which the specification requires: the reset tells the device to
 * abandon the current command and ready itself for the next CBW, and the
 * halts are the host side of the same thing. Doing one without the other
 * leaves half the pipe thinking the command is still live.
 *
 * This is the heavy recovery and it is used only where the light one
 * cannot apply -- a phase error, a bad signature, a tag that does not
 * match. Those three all mean the same thing: the two ends disagree
 * about which command this is, and nothing short of starting over is
 * honest. */
static int msc_reset(msc_dev_t *m)
{
    int r = usb_control(&m->dev,
                        USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                        MSC_REQ_RESET, 0, m->dev.ifnum, 0, 0);
    /* A device that refuses the reset is a device to stop talking to,
       but the halts are cleared anyway: they are the host controller's
       state, and leaving an endpoint halted makes every later attempt
       fail for a reason that has nothing to do with the device. */
    usb_clear_halt(&m->dev, m->ep_in,  1);
    usb_clear_halt(&m->dev, m->ep_out, 0);
    sleep_ms(2);
    return r;
}

/* ── one BOT command ─────────────────────────────────────────────────
 *
 * cmd/cmdlen is the SCSI command; data/dlen the data phase, with `in`
 * saying which way it goes. Returns:
 *
 *    >= 0  the device's CSW status (0 pass, 1 fail, 2 phase error)
 *    <  0  a USB_E* value: the transport itself did not work
 *
 * Those two are kept apart deliberately. "The device says no" and "the
 * bus did not carry it" lead to different recovery, and a driver that
 * returns -1 for both cannot tell a stick that is not ready from a stick
 * that has been unplugged.
 */
static int bot_cmd(msc_dev_t *m, const uint8_t *cmd, int cmdlen,
                   void *data, int dlen, int in, uint32_t *residue)
{
    int r, got;
    uint32_t tag;

    if (cmdlen < 1 || cmdlen > 16) return USB_EPARAM;

    /* The tag. Incremented BEFORE the command and never reused, because
       it is the only thing that can tell this command's status from the
       last one's -- and 0 is skipped so a device that zeroes the field
       instead of echoing it cannot accidentally look right. */
    if (++m->tag == 0) m->tag = 1;
    tag = m->tag;

    memset(g_cbw, 0, CBW_LEN);
    st32le(g_cbw + 0,  CBW_SIG);
    st32le(g_cbw + 4,  tag);
    st32le(g_cbw + 8,  (uint32_t)(dlen < 0 ? 0 : dlen));
    g_cbw[12] = in ? 0x80 : 0x00;
    g_cbw[13] = m->lun & 0x0F;
    g_cbw[14] = (uint8_t)cmdlen;
    memcpy(g_cbw + 15, cmd, (uint32_t)cmdlen);

    /* ── phase 1: the command ─────────────────────────────────────── */
    r = usb_bulk(&m->dev, m->ep_out, 0, g_cbw, CBW_LEN, m->mps_out);
    if (r < 0) {
        /* A stalled command phase means the device is not in a state to
           accept one at all, which is what the class reset is for. */
        if (r == USB_ESTALL) { msc_reset(m); return USB_ESTALL; }
        return r;
    }
    if (r != CBW_LEN) return USB_EXFER;

    /* ── phase 2: the data ────────────────────────────────────────── */
    if (data && dlen > 0) {
        r = usb_bulk(&m->dev, in ? m->ep_in : m->ep_out, in, data, dlen,
                     in ? m->mps_in : m->mps_out);
        if (r == USB_ESTALL) {
            /* THE PATH THAT MATTERS. A device refusing a command stalls
               the data endpoint and then still has a CSW for you. Clear
               the halt and carry on to phase 3; treating this as the end
               is what leaves the status wrapper in the pipe for the next
               command to read as its own. */
            usb_clear_halt(&m->dev, in ? m->ep_in : m->ep_out, in);
        } else if (r < 0) {
            msc_reset(m);
            return r;
        }
        /* A short data phase is NOT an error here. The device reports
           what it actually transferred in dCSWDataResidue and phase 3 is
           where that is read; refusing short reads in this layer would
           reject a legal INQUIRY from any device with a shorter
           descriptor than the 36 bytes asked for. */
    }

    /* ── phase 3: the status ──────────────────────────────────────── */
    memset(g_csw, 0, CSW_LEN);
    got = usb_bulk(&m->dev, m->ep_in, 1, g_csw, CSW_LEN, m->mps_in);
    if (got == USB_ESTALL) {
        /* The specification's own two-attempt rule: a stalled status
           phase is cleared and the CSW requested once more. Exactly
           once -- a loop here would spin on a device that stalls
           everything. */
        usb_clear_halt(&m->dev, m->ep_in, 1);
        memset(g_csw, 0, CSW_LEN);
        got = usb_bulk(&m->dev, m->ep_in, 1, g_csw, CSW_LEN, m->mps_in);
    }
    if (got < 0)        { msc_reset(m); return got; }
    if (got != CSW_LEN) { msc_reset(m); return USB_EXFER; }

    if (ld32le(g_csw + 0) != CSW_SIG) {
        /* Not our protocol any more. Could be a data packet that arrived
           where a status wrapper should be, which means the two ends
           disagree about the phase. */
        msc_reset(m);
        return USB_EXFER;
    }
    if (ld32le(g_csw + 4) != tag) {
        /* The desynchronisation this whole function is arranged to
           catch: a status wrapper for a DIFFERENT command. Believing it
           would mean reporting one command's outcome for another, and on
           a write that is the difference between "it worked" and data
           somewhere else. */
        klog(KLOG_WARN, "USB MSC: CSW tag mismatch (sent %u got %u) -- "
                        "resetting", tag, ld32le(g_csw + 4));
        msc_reset(m);
        return USB_EXFER;
    }

    if (residue) *residue = ld32le(g_csw + 8);

    if (g_csw[12] == CSW_PHASE) {
        /* The device is telling us the phases did not line up. There is
           no recovering the command and the class says to reset. */
        msc_reset(m);
        return CSW_PHASE;
    }
    return (int)g_csw[12];
}

/* ── SCSI, over that ─────────────────────────────────────────────── */

/* REQUEST SENSE, purely so a refusal can be reported with a reason. The
   three bytes that matter are the sense key, the ASC and the ASCQ, and
   the one combination worth acting on is 0x06/0x28/0x00 -- UNIT
   ATTENTION, "not ready to ready change": a stick saying "I have only
   just woken up, ask me again". */
static int scsi_request_sense(msc_dev_t *m, uint8_t *key, uint8_t *asc,
                              uint8_t *ascq)
{
    uint8_t cmd[6], sense[18];
    int r;
    memset(cmd, 0, sizeof cmd);
    memset(sense, 0, sizeof sense);
    cmd[0] = SCSI_REQUEST_SENSE;
    cmd[4] = 18;
    r = bot_cmd(m, cmd, 6, sense, 18, 1, 0);
    if (r < 0) return r;
    if (key)  *key  = sense[2] & 0x0F;
    if (asc)  *asc  = sense[12];
    if (ascq) *ascq = sense[13];
    return r;
}

/* TEST UNIT READY, retried while the device says it is waking up.
 *
 * A stick almost always refuses the FIRST command after being addressed,
 * with UNIT ATTENTION, and a driver that takes the first answer as final
 * reports a perfectly good stick as not present. Bounded at ten tries
 * over about a second, which is enough for every device tested and short
 * enough that a dead one does not hold up the boot. */
static int scsi_wait_ready(msc_dev_t *m)
{
    uint8_t cmd[6];
    int i, r;

    for (i = 0; i < 10; i++) {
        memset(cmd, 0, sizeof cmd);
        cmd[0] = SCSI_TEST_UNIT_READY;
        r = bot_cmd(m, cmd, 6, 0, 0, 0, 0);
        if (r == CSW_PASS) return USB_OK;
        if (r < 0) return r;

        /* It said no. Find out whether it is a no or a not-yet; the
           sense data has to be read anyway or the device keeps
           reporting the same condition for every later command. */
        { uint8_t key = 0, asc = 0, ascq = 0;
          scsi_request_sense(m, &key, &asc, &ascq);
          if (key == 0x02 && asc == 0x3A) {
              /* NOT READY / medium not present: a card reader with no
                 card in it. Not an error and not worth retrying -- it
                 is a slot, and it is empty. */
              return USB_ENODEV;
          }
        }
        sleep_ms(100);
    }
    return USB_ETIMEOUT;
}

static int scsi_inquiry(msc_dev_t *m)
{
    uint8_t cmd[6], d[36];
    int r, i;

    memset(cmd, 0, sizeof cmd);
    memset(d, 0, sizeof d);
    cmd[0] = SCSI_INQUIRY;
    cmd[4] = 36;
    r = bot_cmd(m, cmd, 6, d, 36, 1, 0);
    if (r < 0)        return r;
    if (r != CSW_PASS) return USB_EXFER;

    /* byte 1 bit 7 is RMB, removable medium. A stick sets it; a USB
       hard disk in an enclosure usually does not, and the distinction
       is worth keeping: the block layer registers both after the fixed
       disks, but a person reading the installer's picker is told which
       is which. */
    m->removable = (d[1] & 0x80) ? 1 : 0;

    for (i = 0; i < 8; i++)  m->vendor[i]  = (char)d[8 + i];
    m->vendor[8] = 0;
    for (i = 0; i < 16; i++) m->product[i] = (char)d[16 + i];
    m->product[16] = 0;
    /* Trailing spaces, which SCSI pads with and a screen should not. */
    for (i = 7;  i >= 0 && m->vendor[i]  == ' '; i--) m->vendor[i]  = 0;
    for (i = 15; i >= 0 && m->product[i] == ' '; i--) m->product[i] = 0;

    /* byte 0 low 5 bits: peripheral device type. 0x00 is a direct-access
       block device and 0x0E a simplified one; 0x05 is a CD-ROM, which is
       a legal mass-storage device and not something to install onto. */
    { uint8_t type = d[0] & 0x1F;
      if (type == 0x05) m->readonly = 1; }
    return USB_OK;
}

/* READ CAPACITY (10): the last LBA and the block size, both big-endian.
 *
 * "Last LBA", not "count" -- a device with one block reports 0. Getting
 * that wrong by one is a driver that reads one sector past the end of
 * every disk, which on a stick is a stall and on a disk image is silent
 * nonsense. */
static int scsi_read_capacity(msc_dev_t *m)
{
    uint8_t cmd[10], d[8];
    int r;

    memset(cmd, 0, sizeof cmd);
    memset(d, 0, sizeof d);
    cmd[0] = SCSI_READ_CAPACITY10;
    r = bot_cmd(m, cmd, 10, d, 8, 1, 0);
    if (r < 0)         return r;
    if (r != CSW_PASS) return USB_EXFER;

    { uint32_t last = ld32be(d + 0);
      uint32_t bs   = ld32be(d + 4);

      if (bs == 0 || bs > MSC_BLOCK_MAX) {
          klog(KLOG_WARN, "USB MSC: %u-byte blocks, which this driver "
                          "does not handle", bs);
          return USB_EPARAM;
      }
      if (bs % 512u) {
          klog(KLOG_WARN, "USB MSC: %u-byte blocks, not a multiple of "
                          "512", bs);
          return USB_EPARAM;
      }
      m->blocks = last + 1u;           /* LAST lba, so one more */
      m->bsize  = bs;
      m->per    = bs / 512u;
      m->per_shift = 0;
      while ((1u << m->per_shift) < m->per) m->per_shift++;
      m->sectors = (uint64_t)m->blocks * (uint64_t)m->per;

      /* Writes need read-modify-write on anything but 512, and this is
         removable media: a torn 4096-byte block loses seven sectors
         nobody asked to write. drivers/nvme.c does the read-modify-write
         because an NVMe drive cannot be pulled out of the machine
         half-way through. A stick can. */
      if (bs != 512u) m->readonly = 1;
    }
    return USB_OK;
}

/* MODE SENSE (6), for the write-protect bit alone.
 *
 * Byte 2 of the four-byte header, bit 7. A refused MODE SENSE is NOT an
 * error -- plenty of sticks do not implement it -- so a failure here
 * leaves the flag as it was rather than refusing the device. A stick
 * whose tab is across and whose MODE SENSE is missing reports writable
 * and fails the write, which is the device's answer and not a thing this
 * driver can know in advance. */
static void scsi_check_wp(msc_dev_t *m)
{
    uint8_t cmd[6], d[4];
    int r;
    memset(cmd, 0, sizeof cmd);
    memset(d, 0, sizeof d);
    cmd[0] = SCSI_MODE_SENSE6;
    cmd[2] = 0x3F;          /* all pages */
    cmd[4] = 4;             /* the header is all we want */
    r = bot_cmd(m, cmd, 6, d, 4, 1, 0);
    if (r == CSW_PASS && (d[2] & 0x80)) {
        m->readonly = 1;
        klog(KLOG_WARN, "USB MSC: write protected");
    }
}

/* ── reading and writing one device block ─────────────────────────── */

static int scsi_rw10(msc_dev_t *m, uint32_t lba, void *buf, int write)
{
    uint8_t cmd[10];
    int r;

    memset(cmd, 0, sizeof cmd);
    cmd[0] = write ? SCSI_WRITE10 : SCSI_READ10;
    st32be(cmd + 2, lba);
    cmd[7] = 0;
    cmd[8] = 1;                      /* one block */
    r = bot_cmd(m, cmd, 10, buf, (int)m->bsize, write ? 0 : 1, 0);
    if (r < 0)         return r;
    if (r != CSW_PASS) {
        /* Read the sense so the device is not left reporting the same
           condition for every command after this one, and say what it
           was: "the write failed" is not an answer somebody can act on.
           Medium errors and write-protect faults look identical to a
           caller that only sees a zero. */
        uint8_t key = 0, asc = 0, ascq = 0;
        scsi_request_sense(m, &key, &asc, &ascq);
        klog(KLOG_WARN, "USB MSC: %s lba %u failed, sense %1x/%2x/%2x",
             write ? "write" : "read", lba,
             (uint32_t)key, (uint32_t)asc, (uint32_t)ascq);
        return USB_EXFER;
    }
    return USB_OK;
}

/* ── probing ──────────────────────────────────────────────────────────
 *
 * Walks the core's device table for a Bulk-Only mass storage interface.
 *
 * A COPY of the usb_dev_t is taken, not a pointer: usb_enumerate() sets
 * its count back to zero and rebuilds the table, so a pointer into it
 * survives exactly until the next rescan and then addresses whatever
 * moved into that slot. The copy's cost is that this must be re-run
 * after any enumeration -- which is the contract, and msc_rescan() is
 * what the shell calls. */
int msc_probe(void)
{
    int i, n;

    g_nmsc  = 0;
    g_probed = 1;
    memset(g_msc, 0, sizeof g_msc);

    n = usb_device_count();
    for (i = 0; i < n && g_nmsc < MSC_MAX_DEV; i++) {
        const usb_dev_t *d = usb_device(i);
        const usb_ep_t  *ei, *eo;
        msc_dev_t       *m;

        if (!d || !d->valid) continue;
        if (d->if_class != MSC_CLASS) continue;

        if (d->if_protocol != MSC_PROTO_BOT) {
            /* CBI and the control/bulk variants are pre-2001 and need a
               different transport entirely. Named rather than skipped in
               silence: somebody with such a device should be told it was
               seen and refused, not left to wonder. */
            klog(KLOG_WARN, "USB MSC %4x:%4x: protocol %2x is not "
                            "Bulk-Only, not driven",
                 (uint32_t)d->vendor, (uint32_t)d->product,
                 (uint32_t)d->if_protocol);
            continue;
        }
        if (d->if_subclass != MSC_SUB_SCSI &&
            d->if_subclass != MSC_SUB_SFF8070 &&
            d->if_subclass != MSC_SUB_RBC) {
            klog(KLOG_WARN, "USB MSC %4x:%4x: subclass %2x is not a SCSI "
                            "command set, not driven",
                 (uint32_t)d->vendor, (uint32_t)d->product,
                 (uint32_t)d->if_subclass);
            continue;
        }

        /* Both bulk endpoints, or nothing. usb_find_ep() returning 0 is
           the early refusal include/usb.h describes: better here than
           part-way through a transfer on a device that was never going
           to work. */
        ei = usb_find_ep(d, USB_EP_BULK, 1);
        eo = usb_find_ep(d, USB_EP_BULK, 0);
        if (!ei || !eo) {
            klog(KLOG_WARN, "USB MSC %4x:%4x: %s bulk endpoint missing",
                 (uint32_t)d->vendor, (uint32_t)d->product,
                 ei ? "OUT" : "IN");
            continue;
        }

        m = &g_msc[g_nmsc];
        memset(m, 0, sizeof *m);
        m->dev     = *d;                 /* the copy */
        m->ep_in   = ei->addr;
        m->ep_out  = eo->addr;
        m->mps_in  = ei->mps ? ei->mps : 64;
        m->mps_out = eo->mps ? eo->mps : 64;
        m->lun     = 0;
        m->bsize   = 512;
        m->per     = 1;

        /* GET_MAX_LUN. A device may STALL it, which means "one LUN" and
           is not a failure -- so the stall is swallowed and only a real
           answer is used. Logged when it is more than one, because this
           release drives LUN 0 only and a card reader with four slots
           should say so somewhere other than a comment. */
        { uint8_t ml = 0;
          int r = usb_control(&m->dev,
                              USB_DIR_IN | USB_TYPE_CLASS |
                              USB_RECIP_INTERFACE,
                              MSC_REQ_MAX_LUN, 0, m->dev.ifnum, &ml, 1);
          m->maxlun = (r == 1) ? ml : 0;
          if (m->maxlun)
              klog(KLOG_WARN, "USB MSC %4x:%4x: %u LUNs, only LUN 0 is "
                              "driven in 0.8.6",
                   (uint32_t)d->vendor, (uint32_t)d->product,
                   (uint32_t)(m->maxlun + 1));
        }

        /* A reset before the first command. A stick that was addressed
           by the firmware and left mid-command answers the first CBW
           with the tail of somebody else's, and the tag check would
           catch it -- after costing a reset anyway. Cheaper to start
           from a known state. */
        msc_reset(m);

        if (scsi_wait_ready(m) != USB_OK) {
            klog(KLOG_WARN, "USB MSC %4x:%4x: never became ready",
                 (uint32_t)d->vendor, (uint32_t)d->product);
            continue;
        }
        if (scsi_inquiry(m) != USB_OK) {
            klog(KLOG_WARN, "USB MSC %4x:%4x: INQUIRY failed",
                 (uint32_t)d->vendor, (uint32_t)d->product);
            continue;
        }
        if (scsi_read_capacity(m) != USB_OK) {
            klog(KLOG_WARN, "USB MSC %4x:%4x: READ CAPACITY failed",
                 (uint32_t)d->vendor, (uint32_t)d->product);
            continue;
        }
        if (!m->readonly) scsi_check_wp(m);

        m->valid = 1;
        g_nmsc++;

        /* The capacity in MB rather than sectors, because the number a
           person recognises their own stick by is the one on the
           packaging. */
        klog(KLOG_OK, "USB MSC %s %s: %u MB, %u-byte blocks%s%s",
             m->vendor[0] ? m->vendor : "?",
             m->product[0] ? m->product : "?",
             (uint32_t)(m->sectors / 2048u), m->bsize,
             m->removable ? ", removable" : "",
             m->readonly ? ", READ ONLY" : "");
    }
    return g_nmsc;
}

int msc_rescan(void){ return msc_probe(); }

/* Lazily probe, so a caller that asks before the boot sequence reached
   msc_probe() gets an answer rather than zero. fs/blkdev.c's scan is
   such a caller: fat16_init() runs it, and on some paths that is before
   anything else has asked. */
int msc_count(void)
{
    if (!g_probed) msc_probe();
    return g_nmsc;
}

uint64_t msc_sectors(int i){
    return (i >= 0 && i < g_nmsc) ? g_msc[i].sectors : 0;
}
int msc_readonly(int i){
    if (i < 0 || i >= g_nmsc) return 1;
    return g_msc[i].readonly ? 1 : 0;
}
int msc_removable(int i){
    return (i >= 0 && i < g_nmsc) ? g_msc[i].removable : 0;
}
uint32_t msc_blocksize(int i){
    return (i >= 0 && i < g_nmsc) ? g_msc[i].bsize : 0;
}
const char *msc_vendor(int i){
    return (i >= 0 && i < g_nmsc) ? g_msc[i].vendor : "";
}
const char *msc_product(int i){
    return (i >= 0 && i < g_nmsc) ? g_msc[i].product : "";
}

/* ── the block-layer face ─────────────────────────────────────────────
 *
 * 512-byte sectors in, device blocks out. fs/blkdev.c speaks 512 because
 * every other bus in this tree does and because a filesystem that has to
 * ask its disk what a sector is would be a filesystem with a fifth bus
 * switch in it.
 *
 * For a 512-byte-block device -- which is every USB stick anybody has --
 * this is a straight pass-through. For 1024, 2048 or 4096 the sector is
 * read out of the middle of a device block through the bounce buffer.
 * Writes to those are refused in scsi_read_capacity() and the refusal is
 * in the block layer below every caller, so this function never sees
 * one.
 *
 * The shift is computed once at probe time and applied one bit at a
 * time, because a variable-count 64-bit shift becomes a libgcc call that
 * a freestanding kernel does not link -- the same trap drivers/nvme.c
 * documents. */
int msc_read_sector(int i, uint64_t lba, uint8_t *buf)
{
    msc_dev_t *m;
    if (i < 0 || i >= g_nmsc || !buf) return 0;
    m = &g_msc[i];
    if (!m->valid) return 0;
    if (lba >= m->sectors) return 0;

    if (m->per == 1)
        return scsi_rw10(m, (uint32_t)lba, buf, 0) == USB_OK;

    { uint32_t blk = (uint32_t)(lba >> m->per_shift);
      uint32_t off = (uint32_t)(lba & (uint64_t)(m->per - 1u)) * 512u;
      if (scsi_rw10(m, blk, g_blk, 0) != USB_OK) return 0;
      memcpy(buf, g_blk + off, 512);
      return 1; }
}

int msc_write_sector(int i, uint64_t lba, const uint8_t *buf)
{
    msc_dev_t *m;
    if (i < 0 || i >= g_nmsc || !buf) return 0;
    m = &g_msc[i];
    if (!m->valid || m->readonly) return 0;
    if (lba >= m->sectors) return 0;
    /* per != 1 is read-only, refused at probe time and again by
       fs/blkdev.c, so this is the 512-byte case by construction. The
       check is here anyway: "by construction" is what the three bus
       switches this release deleted were also true by. */
    if (m->per != 1) return 0;

    { uint8_t tmp[512];
      memcpy(tmp, buf, 512);      /* scsi_rw10 takes a writable pointer */
      return scsi_rw10(m, (uint32_t)lba, tmp, 1) == USB_OK; }
}
