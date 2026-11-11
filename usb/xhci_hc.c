/*
 * usb/xhci_hc.c — the eXtensible Host Controller, behind the core.
 *
 * This is the controller every machine made since about 2012 has, and
 * the one the laptop this project is aimed at has INSTEAD of a UHCI:
 * `lspci -vv` on it reports 00:14.0 8086:51ED, class 0C/03/30, and
 * nothing else that carries USB. On that machine this file is the
 * difference between a usable computer and one that cannot be typed at,
 * and -- because its only network adapter is Wi-Fi and its only
 * removable storage is a USB socket -- it is also the gateway to 0.8.6
 * and 0.8.7.
 *
 * ── HOW IT DIFFERS FROM UHCI, WHICH IS THE WHOLE DIFFICULTY ──────────
 *
 * UHCI is a dumb pipe. The driver builds transfer descriptors in memory,
 * points the controller at them, and reads back what happened. Devices
 * are addressed by the driver putting a number in a TD.
 *
 * xHCI owns the devices. The driver asks for a slot, hands the
 * controller a description of the device, and the CONTROLLER issues
 * SET_ADDRESS and remembers the result. Endpoints do not exist until a
 * Configure Endpoint command has been accepted. Work is submitted by
 * appending to a ring and ringing a doorbell; completions come back on a
 * different ring. There are three kinds of ring and they behave
 * differently.
 *
 * ── THE FIVE THINGS THAT GO WRONG ────────────────────────────────────
 *
 * Written here rather than learned twice:
 *
 * 1. CONTEXT SIZE. HCCPARAMS1 bit 2 (CSZ) says whether a context is 32
 *    or 64 bytes. Get it wrong and every field in every context is at
 *    the wrong offset, the controller rejects commands with parameter
 *    errors, and nothing says why.
 *
 * 2. THE CYCLE BIT. A ring is a circular buffer with no head and tail
 *    pointer; ownership is carried by one bit in each TRB, which the
 *    producer flips every time it wraps. Software and the controller
 *    each keep their own copy. Forget to toggle on wrap and the
 *    controller stops dead at the wrap point, having consumed exactly
 *    one ring's worth of work.
 *
 * 3. SCRATCHPAD BUFFERS. HCSPARAMS2 can demand that the driver hand
 *    over a number of pages for the controller's own use, through entry
 *    zero of the device context base array. A controller that asked and
 *    did not get them fails to start, and the specification gives no
 *    clearer symptom than that.
 *
 * 4. CNR. After a reset the controller is not ready, and USBSTS bit 11
 *    says so. Writing any other register while CNR is set is undefined;
 *    in practice the write is dropped and the driver carries on with a
 *    configuration the hardware never received.
 *
 * 5. DCI, not endpoint number. The doorbell and the context array are
 *    indexed by Device Context Index: ep0 is 1, and endpoint N is 2N for
 *    OUT and 2N+1 for IN. Using the endpoint number rings the wrong
 *    doorbell, which is silent.
 *
 * Everything here is POLLED, as every other driver in this tree is. The
 * house rule is that no I/O depends on an interrupt, because IRQs are
 * unreliable under TCG and because the one thing that genuinely needs
 * them -- audio -- is 0.9.3 and will say so out loud.
 */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/usb.h"
#include "../include/pci.h"
#include "../include/mmio.h"
#include "../include/klog.h"
#include "../include/highfb.h"

extern void sleep_ms(uint32_t ms);

/* ── capability registers, at BAR0 + 0 ─────────────────────────────── */
/* How much of the BAR to map. Capability registers, then operational at
   +CAPLENGTH, then the runtime block at RTSOFF (1024 interrupters, 32
   bytes each) and the doorbell array at DBOFF (256 doorbells). 64 KB
   covers every layout this driver can meet; an Intel xHCI BAR is 64 KB.
   It is also comfortably inside one 4 MB window. (v0.8.5.1) */
#define XHCI_MMIO_BYTES  0x10000u

#define XHCI_HANDOFF_NONE     0   /* no USB Legacy Support capability   */
#define XHCI_HANDOFF_NOTHELD  1   /* it was there, the BIOS did not own */
#define XHCI_HANDOFF_TAKEN    2   /* the BIOS let go when asked         */
#define XHCI_HANDOFF_REFUSED  3   /* it would not, and is still driving */

/* ── extended capabilities, and taking the controller from the BIOS ──
   v0.8.5.2.

   HCCPARAMS1 bits 31:16 hold xECP: the offset, IN DWORDS from the start
   of the capability registers, of a linked list of extended
   capabilities. Each entry's low byte is its ID and the next byte is the
   offset to the following entry, again in dwords, with 0 ending the
   list.

   Capability ID 1 is USB Legacy Support, and it is the one that matters
   on any machine that boots firmware. Before the OS touches the
   controller, the BIOS owns it: an SMM handler drives the xHCI and
   presents an attached USB keyboard at ports 0x60/0x64 as though it were
   a PS/2 keyboard. That is what makes a USB keyboard work in GRUB, and
   on a laptop with no PS/2 port it is the only reason anything can be
   typed before a driver exists.

   v0.8.5.1 never performed the handoff. So on the HP the firmware kept
   emulating while this driver also drove the controller, and every
   keystroke arrived TWICE -- once through SMM at port 0x60 into
   kbd_poll(), once through this driver into usb_hid_poll(). The
   installer was unusable.

   It was invisible in QEMU, which has no SMM legacy emulation: there,
   every key arrived exactly once and the release looked finished.

   The handoff is: set the OS-owned bit, wait for the BIOS-owned bit to
   clear, then disable the firmware's SMI sources so it cannot be dragged
   back in. */
#define XECP_ID_LEGACY       1
#define XLEG_BIOS_OWNED      (1u << 16)
#define XLEG_OS_OWNED        (1u << 24)
#define XLEG_CTLSTS          0x04
/* SMI enables: USB SMI, Host System Error, OS Ownership, PCI Command, BAR */
#define XLEG_SMI_ENABLES     ((1u << 0) | (1u << 4) | (1u << 13) | \
                              (1u << 14) | (1u << 15))
/* write-1-to-clear status bits: OS Ownership Change, PCI Command, BAR */
#define XLEG_SMI_STATUS      ((1u << 29) | (1u << 30) | (1u << 31))

#define XCAP_CAPLENGTH   0x00   /* byte  */
#define XCAP_HCIVERSION  0x02   /* word  */
#define XCAP_HCSPARAMS1  0x04
#define XCAP_HCSPARAMS2  0x08
#define XCAP_HCSPARAMS3  0x0C
#define XCAP_HCCPARAMS1  0x10
#define XCAP_DBOFF       0x14
#define XCAP_RTSOFF      0x18

/* ── operational registers, at BAR0 + CAPLENGTH ────────────────────── */
#define XOP_USBCMD       0x00
#define XOP_USBSTS       0x04
#define XOP_PAGESIZE     0x08
#define XOP_DNCTRL       0x14
#define XOP_CRCR         0x18   /* 64-bit */
#define XOP_DCBAAP       0x30   /* 64-bit */
#define XOP_CONFIG       0x38
#define XOP_PORTSC(p)    (0x400u + 0x10u * (uint32_t)(p))

#define USBCMD_RS        (1u << 0)
#define USBCMD_HCRST     (1u << 1)
#define USBCMD_INTE      (1u << 2)

#define USBSTS_HCH       (1u << 0)
#define USBSTS_CNR       (1u << 11)

#define PORTSC_CCS       (1u << 0)
#define PORTSC_PED       (1u << 1)
#define PORTSC_PR        (1u << 4)
#define PORTSC_PP        (1u << 9)
#define PORTSC_SPEED_SH  10
#define PORTSC_SPEED_MSK 0x0Fu
#define PORTSC_CSC       (1u << 17)
#define PORTSC_PRC       (1u << 21)
#define PORTSC_PLS_SH    5
#define PORTSC_PLS_MSK   0xFu
#define PORTSC_PLS_U0    0u
/* The change bits are write-1-to-clear and sit in the same register as
   PED, which is write-1-to-DISABLE. A read-modify-write that preserves
   every bit it read therefore disables the port it was trying to tidy
   up. Every write below goes through portsc_write(). */
#define PORTSC_RW1CS     (PORTSC_CSC | PORTSC_PRC | (1u<<18) | (1u<<19) | \
                          (1u<<20) | (1u<<22) | (1u<<23))
#define PORTSC_RO_MASK   (PORTSC_PED | PORTSC_RW1CS)

/* ── runtime registers, at BAR0 + RTSOFF ───────────────────────────── */
#define XRT_IMAN(i)      (0x20u + 0x20u * (uint32_t)(i))
#define XRT_IMOD(i)      (0x24u + 0x20u * (uint32_t)(i))
#define XRT_ERSTSZ(i)    (0x28u + 0x20u * (uint32_t)(i))
#define XRT_ERSTBA(i)    (0x30u + 0x20u * (uint32_t)(i))
#define XRT_ERDP(i)      (0x38u + 0x20u * (uint32_t)(i))
#define ERDP_EHB         (1u << 3)

/* ── TRB types ─────────────────────────────────────────────────────── */
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_ENABLE_SLOT   9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIG_EP    12
#define TRB_EVAL_CTX     13
#define TRB_RESET_EP     14
#define TRB_STOP_EP      15
#define TRB_SET_TR_DEQ   16
#define TRB_NOOP_CMD     23
#define TRB_XFER_EVENT   32
#define TRB_CMD_COMPLETE 33
#define TRB_PORT_STATUS  34

#define TRB_C            (1u << 0)   /* cycle                           */
#define TRB_TC           (1u << 1)   /* toggle cycle, on a Link TRB     */
#define TRB_ISP          (1u << 2)   /* interrupt on short packet       */
#define TRB_CH           (1u << 4)   /* chain                           */
#define TRB_IOC          (1u << 5)   /* interrupt on completion         */
#define TRB_IDT          (1u << 6)   /* immediate data                  */
#define TRB_TYPE(t)      (((uint32_t)(t)) << 10)
#define TRB_TYPE_OF(c)   (((c) >> 10) & 0x3Fu)

#define CC_SUCCESS        1
#define CC_SHORT_PACKET  13

/* ── NAMING THE COMPLETION CODE, v0.8.7.4 ────────────────────────────
 *
 * x_control() and x_data() collapsed every completion code that was
 * not success, short-packet or stall into one USB_EXFER, and USB_EXFER
 * prints as "(3)".
 *
 * On 26 October `usb cfg` reported "the device would not describe it
 * (3)" for every configuration of every device on the HP -- the
 * phone, the webcam and the MediaTek Bluetooth alike -- and that one
 * number is at least six different faults wearing one face:
 *
 *   a device that did not answer        USB Transaction Error
 *   an endpoint in the wrong state      Context State Error
 *   a slot the controller has forgotten Slot Not Enabled
 *   a ring the controller cannot read   TRB Error
 *   a transfer longer than the buffer   Data Buffer Error
 *   a controller out of something       Resource Error
 *
 * Each has a different cause and a different fix, and "(3)" sends
 * whoever reads it looking in the wrong place. That is v0.8.6.5's
 * lesson -- `update` naming WHICH of four reasons the stick was not
 * used -- and it had not been applied to the layer underneath
 * everything.
 *
 * The RETURN values do not change. Only what the log says. */
static const char *cc_name(int cc)
{
    switch (cc) {
    case  0: return "Invalid";
    case  1: return "Success";
    case  2: return "Data Buffer Error";
    case  3: return "Babble Detected";
    case  4: return "USB Transaction Error";
    case  5: return "TRB Error";
    case  6: return "Stall Error";
    case  7: return "Resource Error";
    case  8: return "Bandwidth Error";
    case  9: return "No Slots Available";
    case 10: return "Invalid Stream Type";
    case 11: return "Slot Not Enabled";
    case 12: return "Endpoint Not Enabled";
    case 13: return "Short Packet";
    case 14: return "Ring Underrun";
    case 15: return "Ring Overrun";
    case 17: return "Parameter Error";
    case 18: return "Bandwidth Overrun";
    case 19: return "Context State Error";
    case 20: return "No Ping Response";
    case 21: return "Event Ring Full";
    case 22: return "Incompatible Device";
    case 23: return "Missed Service";
    case 24: return "Command Ring Stopped";
    case 25: return "Command Aborted";
    case 26: return "Stopped";
    case 27: return "Stopped, Length Invalid";
    case 29: return "Max Exit Latency Too Large";
    case 31: return "Isoch Buffer Overrun";
    case 32: return "Event Lost";
    case 33: return "Undefined Error";
    case 34: return "Invalid Stream ID";
    case 35: return "Secondary Bandwidth Error";
    case 36: return "Split Transaction Error";
    default: return "unknown";
    }
}

/* ── sizes ─────────────────────────────────────────────────────────── */
#define XHCI_MAX_HC       2
#define XHCI_MAX_SLOTS    8      /* devices per controller              */
#define XHCI_RING_TRBS   16      /* per transfer ring, incl. the Link   */
#define XHCI_CMD_TRBS    16
#define XHCI_EVT_TRBS    64
#define XHCI_MAX_SCRATCH 32
#define XHCI_CTX_MAX     64      /* biggest a context can be            */
#define XHCI_EP_RINGS     5      /* ep0 + four more, per slot           */

typedef struct {
    uint32_t lo, hi, status, control;
} trb_t;

typedef struct {
    uint32_t base;        /* BAR0                                       */
    uint32_t op;          /* BAR0 + CAPLENGTH                           */
    uint8_t  bios_handoff;  /* XHCI_HANDOFF_* -- who owns the controller */
    uint32_t dbg_caplen;  /* what CAPLENGTH read as -- for the failure
                             log, because "did not start" without the
                             number it rejected is a dead end (v0.8.5.1) */
    uint32_t rt;          /* BAR0 + RTSOFF                              */
    uint32_t db;          /* BAR0 + DBOFF                               */
    uint8_t  ctxsz;       /* 32 or 64                                   */
    uint8_t  maxports;
    uint8_t  maxslots;
    uint8_t  valid;
    uint32_t cmd_idx;     /* next command TRB to write                  */
    uint8_t  cmd_cycle;
    uint32_t evt_idx;
    uint8_t  evt_cycle;
    /* slot -> our index, and the endpoint ring cursors */
    uint8_t  slot_used[XHCI_MAX_SLOTS + 1];
    uint32_t ep_idx  [XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS];
    uint8_t  ep_cycle[XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS];
    /* v0.8.7.4: has this endpoint's last failure been logged. Cleared
       on the next success, so a device that recovers says so. */
    uint8_t  ep_logged[XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS];
    uint8_t  ep_dci  [XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS];

    /* ── one outstanding transfer per endpoint, v0.8.5.1 ─────────────
       For the non-blocking interrupt poll. `busy` means a TRB is live in
       that endpoint's ring; `done` means its Transfer Event has arrived
       and `status` holds the event's status word. See x_intr_poll(). */
    uint8_t  ep_busy  [XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS];
    uint8_t  ep_done  [XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS];
    uint32_t ep_status[XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS];
    uint32_t ep_want  [XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS];

    /* ── who an arriving event belongs to, v0.8.7.5 ──────────────────
       `busy` above means an ASYNCHRONOUS transfer is outstanding.
       `sync` means a synchronous caller is sitting in evt_wait_ep()
       waiting for this endpoint. An event for an endpoint with neither
       belongs to nobody -- a transfer whose caller timed out and gave
       up -- and is dropped. Handing that event to the next caller is
       the defect that turned one DHCP timeout into six failed
       descriptor reads on every device. */
    uint8_t  ep_sync  [XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS];

    /* The command ring's one filed completion. The command ring has a
       single outstanding command by construction, so one slot does. */
    uint8_t  cmd_done;
    uint32_t cmd_status;
    uint32_t cmd_param;
    uint32_t cmd_control;   /* Enable Slot's reply carries the slot id here */
} xhci_t;

static xhci_t g_x[XHCI_MAX_HC];
static int    g_nx;

/* ── the controller's memory ───────────────────────────────────────────
   Statics rather than an allocator, exactly as drivers/uhci.c does with
   its frame list: these addresses go into hardware registers as 32-bit
   physical addresses, and a static in a -no-pie binary has one the test
   harness can follow. */
static uint64_t g_dcbaa [XHCI_MAX_HC][XHCI_MAX_SLOTS + 1] __attribute__((aligned(64)));
static trb_t    g_cmdr  [XHCI_MAX_HC][XHCI_CMD_TRBS]      __attribute__((aligned(64)));
static trb_t    g_evtr  [XHCI_MAX_HC][XHCI_EVT_TRBS]      __attribute__((aligned(64)));
static uint8_t  g_erst  [XHCI_MAX_HC][16]                 __attribute__((aligned(64)));
static uint8_t  g_devctx[XHCI_MAX_HC][XHCI_MAX_SLOTS + 1][32 * XHCI_CTX_MAX]
                                                          __attribute__((aligned(64)));
static uint8_t  g_inctx [XHCI_MAX_HC][33 * XHCI_CTX_MAX]  __attribute__((aligned(64)));
static trb_t    g_epr   [XHCI_MAX_HC][XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS][XHCI_RING_TRBS]
                                                          __attribute__((aligned(64)));
static uint64_t g_spadar[XHCI_MAX_HC][XHCI_MAX_SCRATCH]   __attribute__((aligned(64)));
static uint8_t  g_spad  [XHCI_MAX_HC][XHCI_MAX_SCRATCH][4096]
                                                          __attribute__((aligned(4096)));
/* A bounce buffer, because a control transfer's data stage needs a
   physical address and a caller's buffer may be anywhere. */
/* ── 2048, and why it is not 1024 any more (v0.8.7.1) ────────────────
   Every synchronous bulk and control transfer is copied through here,
   and x_data() refuses anything longer. 1024 was chosen when the
   largest thing this kernel moved was a 512-byte sector, and it stood
   up for three releases because nothing bigger existed.

   An Ethernet frame is 1514 bytes. The Apple Ethernet driver's first
   transmit on real hardware was refused by THIS BOUND, before the
   controller was touched: `usb eth` reported tx 0/48 -- forty-eight
   frames attempted, forty-eight refused, none on the wire. */
static uint8_t  g_bounce[XHCI_MAX_HC][2048] __attribute__((aligned(64)));

/* An outstanding transfer owns its buffer until the controller completes
   it, so an asynchronous poll cannot use g_bounce -- the next transfer
   would overwrite a buffer the controller is still writing into. One
   small buffer per endpoint ring instead; interrupt reports are a
   handful of bytes. (v0.8.5.1) */
/* ── 2048, and why it is not 64 any more (v0.8.7.1) ──────────────────
   The buffer an ASYNCHRONOUS (non-blocking) IN transfer lands in. One
   per endpoint ring, because a poll's buffer is owned by the controller
   until the transfer completes and cannot be shared.

   64 was a keyboard report with room to spare, and that is all this
   path carried from v0.8.5.1 until now. x_intr_poll() refuses anything
   longer -- so the Apple Ethernet driver's receive, asking for 1516
   bytes, was refused HERE, before the controller was touched. It
   returned USB_EPARAM on every poll of the idle loop: `usb eth`
   reported rx 0/0/19440.

   Nineteen thousand failures and not one byte requested from the
   device. The suite did not catch it because tests/test_usbnet.c's
   simulated controller had no such bound -- a back-end more permissive
   than the real one, which is the same mistake as a mock that cannot
   refuse. It enforces both bounds now.

   2048 x 5 rings x 9 slots x 2 controllers is 180 KB of BSS, which is
   the price of a receive path that can carry a frame. */
#define XHCI_ABUF_SZ 2048
static uint8_t  g_abuf[XHCI_MAX_HC][XHCI_MAX_SLOTS + 1][XHCI_EP_RINGS]
                      [XHCI_ABUF_SZ] __attribute__((aligned(64)));

static uint32_t pa(const void *p) { return (uint32_t)(uintptr_t)p; }

/* ── register helpers ──────────────────────────────────────────────── */
static void portsc_write(xhci_t *x, int p, uint32_t set)
{
    /* Preserve the writable state, drop every write-1-to-clear bit and
       PED, then set what was asked. See the note on PORTSC_RO_MASK: a
       naive read-modify-write here disables the port. */
    uint32_t v = mmio_read32(x->op, XOP_PORTSC(p));
    v &= ~PORTSC_RO_MASK;
    mmio_write32(x->op, XOP_PORTSC(p), v | set);
}

static void ring_doorbell(xhci_t *x, uint32_t slot, uint32_t target)
{
    mmio_write32(x->db, slot * 4u, target);
}

/* ── contexts ──────────────────────────────────────────────────────── */
static uint8_t *slot_ctx(xhci_t *x, int hcno, int slot)
{
    return &g_devctx[hcno][slot][0];
}
static uint8_t *in_ctrl_ctx(xhci_t *x, int hcno)
{
    (void)x; return &g_inctx[hcno][0];
}
static uint8_t *in_slot_ctx(xhci_t *x, int hcno)
{
    return &g_inctx[hcno][x->ctxsz];
}
static uint8_t *in_ep_ctx(xhci_t *x, int hcno, int dci)
{
    return &g_inctx[hcno][(uint32_t)(dci + 1) * x->ctxsz];
}

static void ctx_set32(uint8_t *c, int dword, uint32_t v)
{
    c[dword*4+0] = (uint8_t)(v);       c[dword*4+1] = (uint8_t)(v >> 8);
    c[dword*4+2] = (uint8_t)(v >> 16); c[dword*4+3] = (uint8_t)(v >> 24);
}
static uint32_t ctx_get32(const uint8_t *c, int dword)
{
    return (uint32_t)c[dword*4+0] | ((uint32_t)c[dword*4+1] << 8) |
           ((uint32_t)c[dword*4+2] << 16) | ((uint32_t)c[dword*4+3] << 24);
}

/* ── rings ─────────────────────────────────────────────────────────── */

/* Lay a ring out: every TRB zeroed, and the last one a Link back to the
   start with the Toggle Cycle bit set. TC is what makes the cycle bit
   flip at the wrap, and a ring without it runs exactly once. */
static void ring_init(trb_t *r, int n)
{
    int i;
    for (i = 0; i < n; i++) { r[i].lo = r[i].hi = r[i].status = r[i].control = 0; }
    r[n-1].lo      = pa(r);
    r[n-1].hi      = 0;
    r[n-1].status  = 0;
    r[n-1].control = TRB_TYPE(TRB_LINK) | TRB_TC;
}

/* Append one TRB and advance, handling the Link TRB at the end. The Link
   gets the current cycle so the controller will follow it, and then our
   cycle flips. */
static void ring_push(trb_t *r, uint32_t *idx, uint8_t *cycle,
                      uint32_t lo, uint32_t hi, uint32_t status,
                      uint32_t control)
{
    trb_t *t = &r[*idx];
    t->lo = lo; t->hi = hi; t->status = status;
    t->control = control | (*cycle ? TRB_C : 0);
    (*idx)++;
    if (*idx == XHCI_RING_TRBS - 1) {
        trb_t *lnk = &r[XHCI_RING_TRBS - 1];
        lnk->control = TRB_TYPE(TRB_LINK) | TRB_TC | (*cycle ? TRB_C : 0);
        *idx = 0;
        *cycle ^= 1;
    }
}

/* The command ring is its own length. Same rule, different bound. */
static void cmd_push(xhci_t *x, int hcno, uint32_t lo, uint32_t hi,
                     uint32_t status, uint32_t control)
{
    trb_t *r = g_cmdr[hcno];
    trb_t *t = &r[x->cmd_idx];
    t->lo = lo; t->hi = hi; t->status = status;
    t->control = control | (x->cmd_cycle ? TRB_C : 0);
    x->cmd_idx++;
    if (x->cmd_idx == XHCI_CMD_TRBS - 1) {
        trb_t *lnk = &r[XHCI_CMD_TRBS - 1];
        lnk->control = TRB_TYPE(TRB_LINK) | TRB_TC |
                       (x->cmd_cycle ? TRB_C : 0);
        x->cmd_idx = 0;
        x->cmd_cycle ^= 1;
    }
}

/* ════════════════════════════════════════════════════════════════════
   THE EVENT RING HAS ONE CONSUMER  (v0.8.7.5)

   A controller has one event ring and many things waiting on it: every
   endpoint of every device, plus the command ring. Until 0.8.7.5 two
   functions read it. evt_wait() took the next event of the right TYPE
   and returned its completion code to whoever asked -- never checking
   which slot or endpoint the event was actually for -- and evt_drain()
   filed events by endpoint but STOPPED at the first one it could not
   attribute, leaving it to block the ring for good.

   The comment that justified evt_wait() argued cmd_run() and x_data()
   "run to completion before any poll happens". They do not. A
   synchronous transfer that TIMES OUT returns with its TRB still live
   in the ring -- which the interrupt-poll comment further down says in
   so many words, about this same driver. The controller finishes that
   TRB later, its event arrives with no waiter, and the next synchronous
   caller -- a different endpoint, a different DEVICE -- reads it as its
   own result.

   On the laptop: DHCP writes to an untrusted iPhone's bulk OUT, which
   never answers. x_data() times out and abandons the TRB, several times
   over as DHCP retries. Every descriptor read afterwards returned one
   of those abandoned transfers' completion codes -- six reads, six
   "(3)", on three devices, two of which were working perfectly.

   So: ONE consumer. evt_pump() below drains the whole ring and files
   each event where its owner will look for it. Two rules it never
   breaks:

     - nothing is discarded for being the wrong type. A command
       completion met by a transfer's wait is filed, not thrown away.
     - nothing stops the walk. An event for a torn-down endpoint, a
       port-change event, an event nobody owns: each is consumed and
       dropped. An event left on the ring stops every other consumer.

   Ownership is explicit, not guessed: ep_busy for an asynchronous
   transfer, ep_sync for a caller waiting in evt_wait_ep(). Neither set
   means the event is nobody's, and it goes in the bin. That is what
   makes a timeout survivable.
   ════════════════════════════════════════════════════════════════════ */

/* Defined with the ring bookkeeping further down; evt_pump() needs it to
   turn an event's (slot, dci) into our endpoint index. */
static int ring_slot_for_dci(xhci_t *x, int slot, int dci);

/* Drain the ring, filing everything. Non-blocking. Returns how many
   events it consumed, which is of interest only to the tests. */
static int evt_pump(xhci_t *x, int hcno)
{
    trb_t *r = g_evtr[hcno];
    int    guard = XHCI_EVT_TRBS * 2;
    int    took = 0;

    while (guard-- > 0) {
        trb_t   *e = &r[x->evt_idx];
        uint32_t c = e->control;
        uint32_t st, lo;
        int      type, slot, dci, ri;

        if (((c & TRB_C) ? 1 : 0) != x->evt_cycle) break;    /* ring empty */

        type = (int)TRB_TYPE_OF(c);
        st   = e->status;
        lo   = e->lo;

        /* Consume it FIRST, before deciding what it is. Every path below
           either files this event or drops it, and none can leave it
           sitting at the head of the ring. */
        x->evt_idx++;
        if (x->evt_idx == XHCI_EVT_TRBS) {
            x->evt_idx = 0;
            x->evt_cycle ^= 1;
        }
        /* ERDP must be written back with EHB set, or the controller
           believes the ring is still full and stops posting. */
        mmio_write32(x->rt, XRT_ERDP(0), pa(&r[x->evt_idx]) | ERDP_EHB);
        mmio_write32(x->rt, XRT_ERDP(0) + 4, 0);
        took++;

        if (type == TRB_CMD_COMPLETE) {
            x->cmd_status  = st;
            x->cmd_param   = lo;
            x->cmd_control = c;
            x->cmd_done    = 1;
            continue;
        }
        if (type != TRB_XFER_EVENT) continue;    /* a port change, say */

        slot = (int)((c >> 24) & 0xFFu);
        dci  = (int)((c >> 16) & 0x1Fu);
        if (slot < 1 || slot > x->maxslots) continue;
        ri = ring_slot_for_dci(x, slot, dci);
        if (ri < 0) continue;              /* an endpoint since torn down */

        /* Nobody is waiting for this: a transfer whose caller gave up.
           Dropping it here is the whole point of this rewrite. */
        if (!x->ep_busy[slot][ri] && !x->ep_sync[slot][ri]) continue;

        x->ep_status[slot][ri] = st;
        x->ep_done  [slot][ri] = 1;
        x->ep_busy  [slot][ri] = 0;
    }
    return took;
}

/* Wait for the command ring's completion. Returns the completion code,
   or USB_ETIMEOUT.

   Polled with a bounded spin rather than an interrupt, like every other
   driver here. The bound is deliberately generous: a device that has
   just been reset can take milliseconds to answer, and a timeout that
   is too tight reads as a dead device. */
static int evt_wait_cmd(xhci_t *x, int hcno, uint32_t *param_out,
                        uint32_t timeout_ms)
{
    uint32_t spins = timeout_ms * 200u + 1000u;

    while (spins--) {
        evt_pump(x, hcno);
        if (x->cmd_done) {
            x->cmd_done = 0;
            if (param_out) *param_out = x->cmd_param;
            return (int)((x->cmd_status >> 24) & 0xFFu);
        }
        if ((spins & 0xFFu) == 0) sleep_ms(1);
    }
    return USB_ETIMEOUT;
}

static int cmd_run(xhci_t *x, int hcno, uint32_t lo, uint32_t hi,
                   uint32_t status, uint32_t control, uint32_t *param_out)
{
    int cc;
    cmd_push(x, hcno, lo, hi, status, control);
    ring_doorbell(x, 0, 0);
    cc = evt_wait_cmd(x, hcno, param_out, 1000);
    if (cc < 0) return cc;
    if (cc != CC_SUCCESS) return USB_ECMD;
    return USB_OK;
}

/* Walk the extended capability list for one ID. Returns its byte offset
   from the capability base, or 0 if it is not there.

   The guard is a bound, not a hang-preventer, and the difference is
   worth being honest about: a next-pointer of zero ends the list and
   anything else is ADDED to the offset, so `off` only ever increases and
   `off < XHCI_MMIO_BYTES` already terminates the walk. What the guard
   buys is stopping after 64 entries rather than after 16384, on a list
   that is realistically three long. The injection harness records this
   as untested rather than pretending otherwise. */
static uint32_t xecp_find(uint32_t bar, uint32_t hcc1, uint32_t want_id)
{
    uint32_t off = ((hcc1 >> 16) & 0xFFFFu) * 4u;
    int guard = 64;

    while (off && off < XHCI_MMIO_BYTES && guard-- > 0) {
        uint32_t cap  = mmio_read32(bar, off);
        uint32_t next = ((cap >> 8) & 0xFFu) * 4u;
        if ((cap & 0xFFu) == want_id) return off;
        if (!next) return 0;
        off += next;
    }
    return 0;
}

/* Take the controller from the BIOS. Returns 1 if we own it, 0 if the
   firmware would not let go -- in which case the caller carries on
   anyway, because a controller shared with SMM still works; it just
   also delivers every keystroke a second time through port 0x60, which
   is what usb_hid_wanted() exists to let the user switch off. */
static int xhci_take_from_bios(xhci_t *x, uint32_t bar, uint32_t hcc1)
{
    uint32_t off = xecp_find(bar, hcc1, XECP_ID_LEGACY);
    uint32_t v;
    int i;

    x->bios_handoff = XHCI_HANDOFF_NONE;
    if (!off) return 1;        /* no legacy capability: nothing owns it */

    v = mmio_read32(bar, off);
    if (!(v & XLEG_BIOS_OWNED)) {
        /* Already ours, or the firmware never claimed it. Still clear the
           SMI sources below. */
        x->bios_handoff = XHCI_HANDOFF_NOTHELD;
    } else {
        mmio_write32(bar, off, v | XLEG_OS_OWNED);
        /* One second. The specification suggests the OS wait this long
           before giving up, and a firmware that has not answered by then
           is not going to. */
        for (i = 0; i < 1000; i++) {
            v = mmio_read32(bar, off);
            if (!(v & XLEG_BIOS_OWNED) && (v & XLEG_OS_OWNED)) break;
            sleep_ms(1);
        }
        v = mmio_read32(bar, off);
        x->bios_handoff = (!(v & XLEG_BIOS_OWNED) && (v & XLEG_OS_OWNED))
                        ? XHCI_HANDOFF_TAKEN : XHCI_HANDOFF_REFUSED;
    }

    /* Whether or not it let go: stop it generating SMIs, and acknowledge
       any that are latched. A firmware left able to take an SMI on an
       ownership change will keep running its emulation. */
    v = mmio_read32(bar, off + XLEG_CTLSTS);
    v &= ~XLEG_SMI_ENABLES;
    v |= XLEG_SMI_STATUS;              /* these are write-1-to-clear.
                                          Strictly redundant: v was READ
                                          from the register, so a latched
                                          bit is already 1 here and
                                          writing it back clears it. Kept
                                          because the intent belongs at
                                          the point of the write, and
                                          recorded as untested in
                                          tools/check_xhci_suite.py. */
    mmio_write32(bar, off + XLEG_CTLSTS, v);

    return x->bios_handoff != XHCI_HANDOFF_REFUSED;
}

/* ── bring-up ──────────────────────────────────────────────────────── */
/* `why` is set on every failure path. v0.8.5 logged "did not start" and
   nothing else, and the one time that happened on real hardware -- a
   64-bit BAR truncated to its low half, so every register read was
   somebody else's memory -- the log could not distinguish a wrong
   address from a dead controller. It is one static string per exit; the
   caller prints it with the address it used. */
static int xhci_start(xhci_t *x, int hcno, uint32_t bar, const char **why)
{
    uint32_t hcs1, hcs2, hcc1, caplen, i;
    uint32_t nscratch;
    const char *dummy;

    if (!why) why = &dummy;
    *why = "no reason recorded";

    x->base = bar;
    caplen  = mmio_read8(bar, XCAP_CAPLENGTH);
    x->dbg_caplen = caplen;
    if (caplen < 0x10 || caplen > 0x100) {
        /* Not an xHCI at this address. Either the device is broken or --
           far more likely -- the address is wrong. */
        *why = "CAPLENGTH out of range; wrong address or not an xHCI";
        return USB_ENODEV;
    }

    x->op = bar + caplen;
    x->rt = bar + (mmio_read32(bar, XCAP_RTSOFF) & ~0x1Fu);
    x->db = bar + (mmio_read32(bar, XCAP_DBOFF)  & ~0x03u);

    hcs1 = mmio_read32(bar, XCAP_HCSPARAMS1);
    hcs2 = mmio_read32(bar, XCAP_HCSPARAMS2);
    hcc1 = mmio_read32(bar, XCAP_HCCPARAMS1);

    x->maxslots = (uint8_t)(hcs1 & 0xFFu);
    x->maxports = (uint8_t)((hcs1 >> 24) & 0xFFu);
    x->ctxsz    = (hcc1 & (1u << 2)) ? 64 : 32;   /* CSZ -- trap 1 */

    if (x->maxslots == 0 || x->maxports == 0) {
        *why = "HCSPARAMS1 reports no ports or no slots";
        return USB_ENODEV;
    }
    if (x->maxslots > XHCI_MAX_SLOTS) x->maxslots = XHCI_MAX_SLOTS;

    /* BEFORE touching anything: take the controller from the firmware.
       Resetting a controller SMM is still driving is how v0.8.5.1 ended
       up with two keyboards. (v0.8.5.2) */
    xhci_take_from_bios(x, bar, hcc1);

    /* Stop it, then reset it, then WAIT FOR CNR -- trap 4. */
    mmio_write32(x->op, XOP_USBCMD,
                 mmio_read32(x->op, XOP_USBCMD) & ~USBCMD_RS);
    for (i = 0; i < 1000; i++) {
        if (mmio_read32(x->op, XOP_USBSTS) & USBSTS_HCH) break;
        sleep_ms(1);
    }
    mmio_write32(x->op, XOP_USBCMD, USBCMD_HCRST);
    for (i = 0; i < 1000; i++) {
        uint32_t cmd = mmio_read32(x->op, XOP_USBCMD);
        uint32_t sts = mmio_read32(x->op, XOP_USBSTS);
        if (!(cmd & USBCMD_HCRST) && !(sts & USBSTS_CNR)) break;
        sleep_ms(1);
    }
    if (mmio_read32(x->op, XOP_USBSTS) & USBSTS_CNR) {
        *why = "controller-not-ready never cleared after reset";
        return USB_ETIMEOUT;
    }

    /* Device context base array. Entry 0 is NOT a device: it is where
       the scratchpad array goes, if the controller asked for one --
       trap 3. */
    for (i = 0; i <= (uint32_t)XHCI_MAX_SLOTS; i++) g_dcbaa[hcno][i] = 0;

    nscratch = ((hcs2 >> 27) & 0x1Fu) | (((hcs2 >> 21) & 0x1Fu) << 5);
    if (nscratch > XHCI_MAX_SCRATCH) nscratch = XHCI_MAX_SCRATCH;
    if (nscratch) {
        for (i = 0; i < nscratch; i++) {
            memset(g_spad[hcno][i], 0, 4096);
            g_spadar[hcno][i] = (uint64_t)pa(g_spad[hcno][i]);
        }
        g_dcbaa[hcno][0] = (uint64_t)pa(g_spadar[hcno]);
    }

    for (i = 1; i <= (uint32_t)x->maxslots; i++) {
        memset(g_devctx[hcno][i], 0, sizeof g_devctx[hcno][i]);
        g_dcbaa[hcno][i] = (uint64_t)pa(g_devctx[hcno][i]);
        x->slot_used[i] = 0;
    }
    mmio_write32(x->op, XOP_DCBAAP,     pa(g_dcbaa[hcno]));
    mmio_write32(x->op, XOP_DCBAAP + 4, 0);

    /* Command ring. RCS (bit 0) is the cycle state we start with. */
    ring_init(g_cmdr[hcno], XHCI_CMD_TRBS);
    x->cmd_idx = 0; x->cmd_cycle = 1;
    mmio_write32(x->op, XOP_CRCR,     pa(g_cmdr[hcno]) | 1u);
    mmio_write32(x->op, XOP_CRCR + 4, 0);

    /* Event ring, and its one-entry segment table. The event ring has
       no Link TRB: the controller wraps it using ERST, and software
       follows by flipping its own cycle at the end. */
    for (i = 0; i < XHCI_EVT_TRBS; i++) {
        g_evtr[hcno][i].lo = g_evtr[hcno][i].hi = 0;
        g_evtr[hcno][i].status = g_evtr[hcno][i].control = 0;
    }
    x->evt_idx = 0; x->evt_cycle = 1;
    memset(g_erst[hcno], 0, 16);
    {
        uint32_t p = pa(g_evtr[hcno]);
        g_erst[hcno][0] = (uint8_t)p;        g_erst[hcno][1] = (uint8_t)(p >> 8);
        g_erst[hcno][2] = (uint8_t)(p >> 16);g_erst[hcno][3] = (uint8_t)(p >> 24);
        g_erst[hcno][8] = (uint8_t)XHCI_EVT_TRBS;
    }
    mmio_write32(x->rt, XRT_ERSTSZ(0), 1);
    mmio_write32(x->rt, XRT_ERDP(0),     pa(g_evtr[hcno]) | ERDP_EHB);
    mmio_write32(x->rt, XRT_ERDP(0) + 4, 0);
    mmio_write32(x->rt, XRT_ERSTBA(0),     pa(g_erst[hcno]));
    mmio_write32(x->rt, XRT_ERSTBA(0) + 4, 0);

    /* How many slots we will actually use. Set before Run, or every
       Enable Slot is refused. */
    mmio_write32(x->op, XOP_CONFIG, x->maxslots);

    /* Interrupts stay off: this driver polls, like every other one here. */
    mmio_write32(x->op, XOP_USBCMD,
                 mmio_read32(x->op, XOP_USBCMD) | USBCMD_RS);
    for (i = 0; i < 1000; i++) {
        if (!(mmio_read32(x->op, XOP_USBSTS) & USBSTS_HCH)) break;
        sleep_ms(1);
    }
    if (mmio_read32(x->op, XOP_USBSTS) & USBSTS_HCH) {
        *why = "still halted after Run/Stop was set";
        return USB_ETIMEOUT;
    }

    /* Power every port. A port with PP clear reports nothing connected
       however much is plugged into it. */
    for (i = 0; i < (uint32_t)x->maxports; i++) {
        uint32_t v = mmio_read32(x->op, XOP_PORTSC(i));
        if (!(v & PORTSC_PP)) portsc_write(x, (int)i, PORTSC_PP);
    }
    sleep_ms(20);

    x->valid = 1;
    return USB_OK;
}

/* ── ports ─────────────────────────────────────────────────────────── */
static int x_port_count(usb_hc_t *h)
{
    xhci_t *x = &g_x[h->index];
    return x->valid ? x->maxports : 0;
}

static int x_port_connected(usb_hc_t *h, int port)
{
    xhci_t *x = &g_x[h->index];
    if (!x->valid || port < 0 || port >= x->maxports) return 0;
    return (mmio_read32(x->op, XOP_PORTSC(port)) & PORTSC_CCS) ? 1 : 0;
}

/* ── A USB 3 PORT THAT IS ALREADY ENABLED MUST NOT BE RESET ──────────
   v0.8.8.4, and this is the bug behind five releases of "USB sticks
   are not recognised".

   A SuperSpeed port ENABLES ITSELF when link training completes. The
   controller does it; software is not involved. So a port that comes
   up connected, enabled and with its link in U0 is already finished,
   and writing PORTSC_PR to it knocks the link back down into Polling
   to retrain. The single PED check that used to sit at the end of
   this function then sampled the port mid-retrain, read zero, and
   returned USB_ENODEV -- "nothing on the port" -- for a device that
   was sitting there enabled.

   The HP's boot log is the proof, and it was in front of us for five
   releases:

     port  2  high-speed   webcam        enumerates
     port  9  high-speed   Bluetooth     enumerates
     port 12  SuperSpeed   something     no answer (-4)

   Every high-speed port works and the one SuperSpeed port does not.
   A USB 2 port genuinely needs the reset -- that is what enables it.
   A USB 3 one needs to be left alone.

   USB_ENODEV was also the wrong code to report: the port was neither
   empty nor absent. It is USB_ETIMEOUT now when the enable really
   does not arrive, because what happened is that something did not
   finish in time.

   TWO CHANGES, and the second matters on its own: the enable is
   WAITED FOR rather than sampled once. Even on a USB 2 port, PED
   comes up a moment after PRC, and a single read right after the
   acknowledge is a race this driver was losing on faster silicon. */
static int port_reset(xhci_t *x, int port)
{
    uint32_t i, v = mmio_read32(x->op, XOP_PORTSC(port));

    if ((v & PORTSC_PED) &&
        ((v >> PORTSC_PLS_SH) & PORTSC_PLS_MSK) == PORTSC_PLS_U0) {
        /* Already trained and enabled. Acknowledge the connect so the
           change bit does not fire again, and leave the link alone. */
        portsc_write(x, port, PORTSC_CSC);
        return USB_OK;
    }

    portsc_write(x, port, PORTSC_CSC);          /* ack the connect     */
    portsc_write(x, port, PORTSC_PR);           /* and reset it        */
    for (i = 0; i < 500; i++) {
        v = mmio_read32(x->op, XOP_PORTSC(port));
        if (v & PORTSC_PRC) break;
        sleep_ms(1);
    }
    v = mmio_read32(x->op, XOP_PORTSC(port));
    if (!(v & PORTSC_PRC)) return USB_ETIMEOUT;
    portsc_write(x, port, PORTSC_PRC);

    /* WAIT for the enable, do not sample it. A USB 2 port is only
       usable once the controller has enabled it, and that happens a
       moment after the reset-change is acknowledged. */
    for (i = 0; i < 500; i++) {
        if (mmio_read32(x->op, XOP_PORTSC(port)) & PORTSC_PED)
            return USB_OK;
        sleep_ms(1);
    }
    return USB_ETIMEOUT;
}

static uint8_t port_speed(xhci_t *x, int port)
{
    uint32_t v = mmio_read32(x->op, XOP_PORTSC(port));
    uint32_t s = (v >> PORTSC_SPEED_SH) & PORTSC_SPEED_MSK;
    switch (s) {
    case 1: return USB_SPEED_FULL;
    case 2: return USB_SPEED_LOW;
    case 3: return USB_SPEED_HIGH;
    case 4: return USB_SPEED_SUPER;
    default: return USB_SPEED_FULL;
    }
}

/* Endpoint 0's packet size is fixed by speed for everything except a
   full-speed device, which has to be asked. */
/* ── bMaxPacketSize0 IS NOT ALWAYS A BYTE COUNT, v0.8.6.7 ────────────
   At low, full and high speed, byte 7 of the device descriptor is the
   endpoint 0 packet size in bytes: 8, 16, 32 or 64.

   At SUPERSPEED it is an EXPONENT. The USB 3 specification fixes it at
   9, meaning 2^9 = 512 bytes, and a SuperSpeed device is not permitted
   to say anything else.

   Read as a byte count, 9 means a nine-byte control endpoint. The pipe
   that was working at 512 gets reprogrammed to 9, the next descriptor
   request never completes, and enumeration ends in USB_EXFER -- which
   this kernel prints as "attached, but it did not answer".

   That is what hc0 port 12 on the HP has been doing since v0.8.5.3,
   and what a USB 3 stick does in a socket that trains at SuperSpeed
   while working perfectly in one that trains at high speed. The port
   decode added in the same release is what made it findable: connected,
   enabled, powered, link U0 active, SuperSpeed -- and no answer. A link
   that is up and a device that will not talk is a protocol fault, not a
   wiring one.

   Zero is returned for a value this layer cannot make sense of, and the
   caller keeps what it had rather than programming nonsense. */
static uint16_t mps0_from_desc(uint8_t speed, uint8_t b)
{
    if (!b) return 0;
    if (speed == USB_SPEED_SUPER) {
        /* The specification does not merely encode this as an exponent,
           it FIXES it: bMaxPacketSize0 shall be 09h. So anything else
           is a device contradicting itself, and the answer is to keep
           the speed's default rather than to shift by whatever arrived
           -- 1u << 200 is undefined behaviour, not a packet size. */
        if (b != 9) return 0;
        return 512;
    }
    return (uint16_t)b;
}

static uint16_t default_mps(uint8_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:   return 8;
    case USB_SPEED_HIGH:  return 64;
    case USB_SPEED_SUPER: return 512;
    default:              return 8;   /* full speed: ask, then correct */
    }
}

/* ── endpoint rings ────────────────────────────────────────────────── */
static int ring_slot_for_dci(xhci_t *x, int slot, int dci)
{
    int i;
    for (i = 0; i < XHCI_EP_RINGS; i++)
        if (x->ep_dci[slot][i] == (uint8_t)dci) return i;
    return -1;
}

static int ring_alloc(xhci_t *x, int hcno, int slot, int dci)
{
    int i = ring_slot_for_dci(x, slot, dci);
    if (i >= 0) return i;
    for (i = 0; i < XHCI_EP_RINGS; i++) {
        if (x->ep_dci[slot][i] == 0) {
            x->ep_dci[slot][i]   = (uint8_t)dci;
            x->ep_idx[slot][i]   = 0;
            x->ep_cycle[slot][i] = 1;
            ring_init(g_epr[hcno][slot][i], XHCI_RING_TRBS);
            return i;
        }
    }
    return -1;
}

/* ── re-laying and freeing a ring, v0.8.7 ─────────────────────────────
 *
 * Until this release a slot's endpoint rings were allocated once, at
 * the single Configure Endpoint that followed enumeration, and freed
 * once, in x_detach(). Nothing configured a slot TWICE, so neither of
 * these functions had anything to do.
 *
 * SET_INTERFACE makes it twice. An alternate setting is a different set
 * of endpoints on the same interface -- that is what it IS -- and the
 * phone this release exists for has the two bulk endpoints of its
 * Ethernet interface on alt 1 and alt 2, with alt 0 carrying NONE. So
 * a configure now happens on a slot that already has rings, and three
 * things that were true by construction stop being true:
 *
 *   1. ring_alloc() returns the EXISTING ring for a DCI it already
 *      holds, without resetting the cursor. x_configure() then writes
 *      the TR Dequeue Pointer back to the ring BASE while ep_idx is
 *      still part-way along it, so the controller dequeues from a TRB
 *      the driver wrote on the previous setting and the driver enqueues
 *      somewhere the controller is not looking. x_configure()'s own
 *      comment has claimed since v0.8.5.1 that a configure "re-lays
 *      this slot's endpoint rings, so nothing queued on them survives
 *      it". It cleared ep_busy and ep_done and did not re-lay the ring.
 *
 *   2. An endpoint the new setting does NOT have keeps its ring marked
 *      in use. XHCI_EP_RINGS is five -- ep0 and four more -- so
 *      alternating between two settings whose endpoint ADDRESSES differ
 *      exhausts them in two switches and ring_alloc() starts returning
 *      -1. That is the v0.8.6.1 slot leak exactly, one level down: a
 *      resource taken on every call and given back only at detach.
 *
 *   3. The controller is never told to forget it. See x_configure().
 */
static void ring_relay(xhci_t *x, int hcno, int slot, int ri)
{
    x->ep_idx[slot][ri]   = 0;
    x->ep_cycle[slot][ri] = 1;
    x->ep_busy[slot][ri]  = 0;
    x->ep_done[slot][ri]  = 0;
    x->ep_sync[slot][ri]  = 0;
    ring_init(g_epr[hcno][slot][ri], XHCI_RING_TRBS);
}

static void ring_free(xhci_t *x, int slot, int ri)
{
    x->ep_dci[slot][ri]    = 0;
    x->ep_idx[slot][ri]    = 0;
    x->ep_cycle[slot][ri]  = 1;
    x->ep_busy[slot][ri]   = 0;
    x->ep_done[slot][ri]   = 0;
    x->ep_sync[slot][ri]   = 0;
    x->ep_status[slot][ri] = 0;
    x->ep_want[slot][ri]   = 0;
}

/* ── attach: slot, context, address ──────────────────────────────────
 *
 * One function for both entry points, because the only difference
 * between a device on a root port and a device five hubs down is three
 * fields of the slot context -- and writing that twice is how the two
 * would come to disagree.
 *
 * `rootport` is always the root port the whole path hangs off.
 * `route` is 0 for a root-port device and the xHCI route string
 *   otherwise, which is what tells the controller which socket to
 *   actually talk down.
 * `do_reset` is the difference that matters: a root port is reset HERE,
 *   and a hub port has already been reset BY THE HUB. Resetting the root
 *   port for a hub-attached device would reset the hub -- and every
 *   other device on it -- in the middle of enumerating one of them.
 * `speed` is read off the port here for a root device and handed in from
 *   the hub's port status otherwise, because a hub's own speed does not
 *   tell you the speed of what is plugged into it.
 */
/* ── WHERE AN ATTACH FAILED, NOT JUST THAT IT DID, v0.8.8.4 ──────────
   x_attach_common() has twelve return points and usb/core.c printed
   only the code:

       warn USB xHCI hc0 port 12: no answer (-4)

   -4 is USB_ENODEV, and it can come from the controller being invalid,
   from a command ring that refused ENABLE_SLOT, from ADDRESS_DEVICE,
   or from the endpoint ring not existing when the first control
   transfer went out. Those are four different bugs and the number
   tells them apart from none of them.

   The HP has been printing that line for port 12 since v0.8.5.3 and
   the answer has been asked for across five releases. xhci_start()
   already takes a `why` and says things like "CAPLENGTH out of range";
   this is the same idea one layer down.

   A static rather than an out-parameter because every caller of
   .attach goes through the usb_hc_ops table and widening that
   signature touches UHCI too, which has its own message. */
static const char *g_attach_why = "no attach attempted";

const char *xhci_attach_why(void) { return g_attach_why; }

static int x_attach_common(usb_hc_t *h, int rootport, uint32_t route,
                           int do_reset, uint8_t speed_in,
                           uint32_t tt_slot, uint8_t tt_port,
                           usb_dev_t *out)
{
    int      hcno = h->index;
    xhci_t  *x = &g_x[hcno];
    int      r, slot, ri;
    uint32_t param = 0;
    uint8_t  speed;
    uint8_t *ic, *isc, *iec;
    int      port = rootport;

    g_attach_why = "the controller is not initialised";
    if (!x->valid) return USB_ENODEV;

    if (do_reset) {
        g_attach_why = "the port never enabled after reset";
        r = port_reset(x, port);
        if (r != USB_OK) return r;
        speed = port_speed(x, port);
    } else {
        speed = speed_in;
    }

    g_attach_why = "ENABLE_SLOT was refused by the command ring";
    r = cmd_run(x, hcno, 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &param);
    if (r != USB_OK) return r;
    /* The slot id is in the top byte of the command completion's control
       field. Until 0.8.7.5 this was read back off the event ring entry
       behind the cursor, which was only ever right because evt_wait()
       stopped at the first event it took. evt_pump() drains the ring, so
       the entry behind the cursor can be anything; the filed completion
       is the one place the answer is certainly still correct. */
    slot = (int)((x->cmd_control >> 24) & 0xFFu);
    g_attach_why = "ENABLE_SLOT returned a slot id out of range";
    if (slot <= 0 || slot > x->maxslots) return USB_ECMD;
    x->slot_used[slot] = 1;

    ri = ring_alloc(x, hcno, slot, 1);        /* DCI 1 is ep0 -- trap 5 */
    g_attach_why = "no endpoint ring left for this slot";
    if (ri < 0) return USB_ENOMEM;

    /* Input context: add flags for the slot context and ep0. */
    memset(g_inctx[hcno], 0, sizeof g_inctx[hcno]);
    ic  = in_ctrl_ctx(x, hcno);
    isc = in_slot_ctx(x, hcno);
    iec = in_ep_ctx(x, hcno, 1);
    ctx_set32(ic, 1, 0x3u);                       /* A0 | A1            */

    /* Slot context: one entry (ep0), the speed, the ROUTE STRING and the
       root port.

       The route string is bits 0..19 of dword 0 and was hard zero until
       v0.8.6, which is correct for a root-port device and is why 0.8.5
       found nothing behind a hub: the controller was being told every
       device hung directly off a root port, and for one that does not,
       that is an address pointing at the wrong socket.

       Multi-TT (bit 25) is set when this device IS a high-speed hub with
       more than one translator. Not set here: the hub descriptor says
       so and this layer has not read it. A single-TT hub is the common
       case and works; a multi-TT hub behaves as single-TT, which is
       slower and not wrong. Written down rather than discovered. */
    ctx_set32(isc, 0, (1u << 27) | ((uint32_t)speed << 20) |
                      (route & 0xFFFFFu));
    ctx_set32(isc, 1, ((uint32_t)(port + 1) << 16));

    /* The transaction translator, for a low or full speed device behind
       a high speed hub. dword 2 carries the hub's slot id in bits 0..7
       and its port, one-based, in bits 8..15. Without it the controller
       issues full-speed traffic at high-speed timing and the device
       never answers -- which looks exactly like a port with nothing on
       it, so the symptom of getting this wrong is silence. */
    if (tt_slot)
        ctx_set32(isc, 2, (tt_slot & 0xFFu) |
                          (((uint32_t)tt_port & 0xFFu) << 8));

    /* Endpoint 0 context: Control, 3 errors, the ring and its cycle. */
    ctx_set32(iec, 1, (4u << 3) | (3u << 1) |
                      ((uint32_t)default_mps(speed) << 16));
    ctx_set32(iec, 2, pa(g_epr[hcno][slot][ri]) | 1u);   /* DCS = 1    */
    ctx_set32(iec, 3, 0);
    ctx_set32(iec, 4, 8u);                              /* avg TRB len */

    r = cmd_run(x, hcno, pa(g_inctx[hcno]), 0, 0,
                TRB_TYPE(TRB_ADDRESS_DEV) | ((uint32_t)slot << 24), 0);
    if (r != USB_OK) {
        g_attach_why = "ADDRESS_DEVICE was refused -- the slot context "
                       "was built but the controller would not take it";
        x->slot_used[slot] = 0;
        return r;
    }

    out->slot       = (uint32_t)slot;
    out->speed      = speed;
    out->maxpacket0 = default_mps(speed);
    /* The controller assigned the address; read it back rather than
       guessing, because it is the controller's to choose. */
    out->addr = (uint8_t)(ctx_get32(slot_ctx(x, hcno, slot), 3) & 0xFFu);

    /* Now the device descriptor, over the pipe that exists. For a
       full-speed device the first eight bytes carry the real ep0 packet
       size and the context has to be corrected. */
    {
        static uint8_t desc[18];
        memset(desc, 0, 18);
        r = usb_control(out, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                        USB_REQ_GET_DESCRIPTOR,
                        (uint16_t)(USB_DESC_DEVICE << 8), 0, desc, 8);
        if (r < 8) {
            /* THE ONE THAT MATTERS ON A SUPERSPEED PORT. The link is
               up, the slot is addressed, and the device will not
               answer the first eight bytes of its own descriptor. */
            g_attach_why = "the device did not return the first 8 bytes "
                           "of its device descriptor";
            return (r < 0) ? r : USB_EXFER;
        }
        {
        uint16_t real = mps0_from_desc(speed, desc[7]);
        if (real && real != out->maxpacket0) {
            out->maxpacket0 = real;
            memset(g_inctx[hcno], 0, sizeof g_inctx[hcno]);
            ctx_set32(in_ctrl_ctx(x, hcno), 1, 0x2u);        /* A1 only */
            iec = in_ep_ctx(x, hcno, 1);
            ctx_set32(iec, 1, (4u << 3) | (3u << 1) |
                              ((uint32_t)real << 16));
            ctx_set32(iec, 2, pa(g_epr[hcno][slot][ri]) |
                              (x->ep_cycle[slot][ri] ? 1u : 0u));
            ctx_set32(iec, 4, 8u);
            cmd_run(x, hcno, pa(g_inctx[hcno]), 0, 0,
                    TRB_TYPE(TRB_EVAL_CTX) | ((uint32_t)slot << 24), 0);
        }
        }
        memset(desc, 0, 18);
        r = usb_control(out, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                        USB_REQ_GET_DESCRIPTOR,
                        (uint16_t)(USB_DESC_DEVICE << 8), 0, desc, 18);
        if (r < 18) {
            g_attach_why = "the device returned its first 8 bytes and "
                           "then not the full 18-byte descriptor";
            return (r < 0) ? r : USB_EXFER;
        }

        out->dev_class    = desc[4];
        out->dev_subclass = desc[5];
        out->dev_protocol = desc[6];
        {   uint16_t real = mps0_from_desc(speed, desc[7]);
            if (real) out->maxpacket0 = real;
        }
        out->vendor       = (uint16_t)(desc[8]  | (desc[9]  << 8));
        out->product      = (uint16_t)(desc[10] | (desc[11] << 8));
        out->release      = (uint16_t)(desc[12] | (desc[13] << 8));
        out->nconfigs     = desc[17];
    }
    g_attach_why = "ok";
    return USB_OK;
}

/* ── a hub is a hub: the slot context says so, v0.9.2.2 ──────────────
 *
 * The slot context of a hub has four fields a function's does not:
 *
 *   dword 0 bit 26   Hub        "this device is a USB hub"
 *   dword 0 bit 25   MTT        a high-speed hub with one TT per port
 *   dword 1 31:24    Number of Ports
 *   dword 2 17:16    TT Think Time, for a high-speed hub
 *
 * Until this release none was ever written. A high-speed device behind a
 * hub did not care, which is why a USB stick behind one was fine. A LOW
 * or FULL speed device does: its slot names the hub's slot as its
 * transaction translator (x_attach_common, dword 2), and the controller
 * schedules split transactions through it only for a slot it knows is a
 * hub. Kyan's HP, 11 November: a keyboard on a hub lit up, and typed
 * nothing.
 *
 * Linux, xhci_update_hub_device(): copy the slot context out of the
 * device context, set the four fields, and give it back with a Configure
 * Endpoint whose only add flag is A0 -- no endpoint is touched. That is
 * what this does. Context Entries, route string, speed and root port
 * come from the device context unchanged, so the hub's own endpoints, if
 * it has any configured, keep theirs. */
static int x_hub_info(usb_hc_t *h, const usb_dev_t *d, int nports,
                      int think_time, int multi_tt)
{
    int      hcno = h->index;
    xhci_t  *x = &g_x[hcno];
    int      slot = (int)d->slot;
    uint8_t *isc, *dsc;
    uint32_t d0, d1, d2;

    if (!x->valid) return USB_ENODEV;
    if (slot <= 0 || slot > x->maxslots || !x->slot_used[slot]) return USB_EPARAM;
    if (nports < 1 || nports > 255) return USB_EPARAM;

    memset(g_inctx[hcno], 0, sizeof g_inctx[hcno]);
    ctx_set32(in_ctrl_ctx(x, hcno), 0, 0);       /* drop nothing          */
    ctx_set32(in_ctrl_ctx(x, hcno), 1, 1u);      /* add A0: the slot only */

    isc = in_slot_ctx(x, hcno);
    dsc = slot_ctx(x, hcno, slot);
    d0 = ctx_get32(dsc, 0) & ~((1u << 26) | (1u << 25));
    d0 |= (1u << 26);
    /* MTT only means anything for a high-speed hub. */
    if (multi_tt && d->speed == USB_SPEED_HIGH) d0 |= (1u << 25);
    d1 = (ctx_get32(dsc, 1) & 0x00FFFFFFu) | ((uint32_t)nports << 24);
    d2 = ctx_get32(dsc, 2) & ~(3u << 16);
    if (d->speed == USB_SPEED_HIGH) d2 |= ((uint32_t)think_time & 3u) << 16;
    ctx_set32(isc, 0, d0);
    ctx_set32(isc, 1, d1);
    ctx_set32(isc, 2, d2);
    ctx_set32(isc, 3, ctx_get32(dsc, 3));

    return cmd_run(x, hcno, pa(g_inctx[hcno]), 0, 0,
                   TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)slot << 24), 0);
}

/* A device on a root port: reset it, and take its speed from PORTSC. */
static int x_attach(usb_hc_t *h, int port, usb_dev_t *out)
{
    return x_attach_common(h, port, 0, 1, 0, 0, 0, out);
}

/* A device behind a hub: the hub has reset the port and read the speed.
   See usb_hc_ops_t::attach_path in include/usb.h for the contract, and
   usb/core.c for where route and tt_* come from. */
static int x_attach_path(usb_hc_t *h, int rootport, uint32_t route,
                         uint8_t speed, uint32_t tt_slot, uint8_t tt_port,
                         usb_dev_t *out)
{
    return x_attach_common(h, rootport, route, 0, speed,
                           tt_slot, tt_port, out);
}

/* ── detach: give the slot back ──────────────────────────────────────
 *
 * The counterpart of x_attach_common's Enable Slot, and it did not exist
 * until v0.8.6.1.
 *
 * TRB_DISABLE_SLOT was defined in this file and never sent. slot_used[]
 * was cleared at controller init and on a FAILED Address Device, and
 * nowhere else -- so every successful attach took a slot from the
 * controller and no path ever gave one back. usb_enumerate() clearing its
 * own table frees nothing here, because on xHCI the controller owns the
 * device.
 *
 * The HP has 8 slots and 3 devices on the bus. Boot took 1, 2 and 3; the
 * first `usb rescan` took 4, 5 and 6; the second would take 7 and 8 and
 * the third would find no slot at all. The endpoint rings went with
 * them: ep_dci[slot][] marks a ring in use and nothing cleared it, so a
 * reused slot would have found its rings already allocated and
 * ring_alloc() would have handed back a ring with somebody else's cursor
 * in it.
 *
 * So both are cleared here, and in that order: the controller is told
 * first, because a Disable Slot that fails leaves the slot live and this
 * side must not pretend otherwise. If the command is refused the local
 * state is still cleared -- a slot the controller insists on keeping is
 * a slot this kernel cannot use either way, and leaving our bookkeeping
 * set would make it unusable for ever rather than until the next init.
 */
static void x_detach(usb_hc_t *h, const usb_dev_t *d)
{
    int      hcno = h->index;
    xhci_t  *x = &g_x[hcno];
    int      slot = (int)d->slot;
    int      i;

    if (!x->valid) return;
    /* The bound is for the ARRAY, not for the command: slot_used[]
       below refuses any slot this side does not hold, and no path marks
       one past maxslots. It is here so a slot number that arrived from
       a device table cannot index off the end of the arrays. */
    if (slot <= 0 || slot > x->maxslots) return;
    if (!x->slot_used[slot]) return;

    cmd_run(x, hcno, 0, 0, 0,
            TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot << 24), 0);

    x->slot_used[slot] = 0;
    for (i = 0; i < XHCI_EP_RINGS; i++) {
        x->ep_dci[slot][i]    = 0;
        x->ep_idx[slot][i]    = 0;
        x->ep_cycle[slot][i]  = 1;
        x->ep_busy[slot][i]   = 0;
        x->ep_done[slot][i]   = 0;
        x->ep_status[slot][i] = 0;
        x->ep_want[slot][i]   = 0;
    }
    /* The DCBAA entry is deliberately LEFT ALONE. Every slot's device
       context is allocated once at controller init and the array points
       at it for the life of the controller -- see hc_start(), which
       fills every entry before writing DCBAAP. Zeroing one here would
       have made the slot unusable on a later attach, because nothing
       puts it back.

       The first version of this function cleared it, and as two 32-bit
       words rather than the one 64-bit entry the array actually holds:
       it would have written over the NEXT slot's pointer as well. */
}

/* ── transfers ─────────────────────────────────────────────────────── */
static int dci_of(uint8_t ep, int in)
{
    if ((ep & 0x0F) == 0) return 1;
    return (int)(((uint32_t)(ep & 0x0F) * 2u) + (in ? 1u : 0u));
}

/* Stop an endpoint the caller has given up on, so the controller stops
   retrying a transfer somebody has already been told failed -- and so
   the live TRB does not sit in the ring holding a buffer.

   Stop Endpoint makes the controller post a Transfer Event for the TRB
   it abandoned. By the time that arrives evt_wait_ep() has cleared
   ep_sync, so evt_pump() finds it unowned and drops it. Set TR Dequeue
   then tells the controller to resume where we will write next; without
   it the endpoint stays Stopped with its dequeue pointer parked on a TRB
   nobody will ever collect. */
static void ep_stop(xhci_t *x, int hcno, int slot, int ri)
{
    trb_t   *r = g_epr[hcno][slot][ri];
    uint32_t dci = (uint32_t)x->ep_dci[slot][ri];
    uint32_t deq;

    if (!dci) return;
    cmd_run(x, hcno, 0, 0, 0,
            TRB_TYPE(TRB_STOP_EP) | ((uint32_t)slot << 24) | (dci << 16), 0);
    deq = pa(&r[x->ep_idx[slot][ri]]) | (x->ep_cycle[slot][ri] ? 1u : 0u);
    cmd_run(x, hcno, deq, 0, 0,
            TRB_TYPE(TRB_SET_TR_DEQ) | ((uint32_t)slot << 24) | (dci << 16),
            0);
}

/* Wait for ONE endpoint's transfer to complete. The caller must have set
   ep_sync for that endpoint before ringing the doorbell, so that an event
   arriving between the two has an owner to be filed under.

   Returns the completion code, or USB_ETIMEOUT -- after which the
   endpoint has been stopped and its pending event disowned, so the next
   caller on any endpoint is unaffected. That is the difference between
   this and the function it replaced. */
static int evt_wait_ep(xhci_t *x, int hcno, int slot, int ri,
                       uint32_t *status_out, uint32_t timeout_ms)
{
    uint32_t spins = timeout_ms * 200u + 1000u;

    while (spins--) {
        evt_pump(x, hcno);
        if (x->ep_done[slot][ri]) {
            uint32_t st = x->ep_status[slot][ri];
            x->ep_done[slot][ri] = 0;
            x->ep_sync[slot][ri] = 0;
            if (status_out) *status_out = st;
            return (int)((st >> 24) & 0xFFu);
        }
        if ((spins & 0xFFu) == 0) sleep_ms(1);
    }
    x->ep_sync[slot][ri] = 0;      /* whatever arrives now is nobody's */
    ep_stop(x, hcno, slot, ri);
    return USB_ETIMEOUT;
}

static int x_control(usb_hc_t *h, const usb_dev_t *d, uint8_t rtype,
                     uint8_t req, uint16_t value, uint16_t index,
                     void *data, uint16_t len)
{
    int      hcno = h->index;
    xhci_t  *x = &g_x[hcno];
    int      slot = (int)d->slot, ri;
    trb_t   *r;
    uint32_t setup_lo, setup_hi;
    int      in = (rtype & USB_DIR_IN) ? 1 : 0;
    int      cc;
    uint32_t st = 0;

    if (!x->valid || slot <= 0 || slot > x->maxslots) return USB_ENODEV;
    if (len > sizeof g_bounce[0]) return USB_EPARAM;
    ri = ring_slot_for_dci(x, slot, 1);
    if (ri < 0) return USB_ENODEV;
    r = g_epr[hcno][slot][ri];

    if (len && !in && data) memcpy(g_bounce[hcno], data, len);
    else if (len) memset(g_bounce[hcno], 0, len);

    /* Setup stage: the eight request bytes travel IN the TRB (IDT). */
    setup_lo = (uint32_t)rtype | ((uint32_t)req << 8) |
               ((uint32_t)value << 16);
    setup_hi = (uint32_t)index | ((uint32_t)len << 16);
    ring_push(r, &x->ep_idx[slot][ri], &x->ep_cycle[slot][ri],
              setup_lo, setup_hi, 8,
              TRB_TYPE(TRB_SETUP) | TRB_IDT |
              (len ? ((uint32_t)(in ? 3u : 2u) << 16) : 0));

    if (len) {
        ring_push(r, &x->ep_idx[slot][ri], &x->ep_cycle[slot][ri],
                  pa(g_bounce[hcno]), 0, len,
                  TRB_TYPE(TRB_DATA) | TRB_ISP |
                  (in ? (1u << 16) : 0));
    }
    /* Status stage goes the opposite way to the data, and IOC on it is
       what produces the one event this transfer waits for. */
    ring_push(r, &x->ep_idx[slot][ri], &x->ep_cycle[slot][ri],
              0, 0, 0,
              TRB_TYPE(TRB_STATUS) | TRB_IOC |
              ((len && in) ? 0u : (1u << 16)));

    x->ep_sync[slot][ri] = 1;
    ring_doorbell(x, (uint32_t)slot, 1);
    cc = evt_wait_ep(x, hcno, slot, ri, &st, 1000);
    if (cc < 0) return cc;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        uint32_t residue = st & 0x00FFFFFFu;
        uint32_t got = (len > residue) ? (len - residue) : 0;
        if (len && in && data) memcpy(data, g_bounce[hcno], got ? got : len);
        return (int)(len ? (got ? got : len) : 0);
    }
    if (cc == 6) return USB_ESTALL;        /* Stall Error */
    /* Control transfers are rare -- a descriptor fetch, a
       SET_CONFIGURATION -- so this is logged every time rather than
       rate-limited. It is the line that tells somebody reading a
       screen full of "(3)" what actually went wrong. */
    klog(KLOG_WARN, "xHCI slot %u ep0: %s (cc %d), req %2x/%2x",
         (uint32_t)slot, cc_name(cc), (uint32_t)cc,
         (uint32_t)rtype, (uint32_t)req);
    return USB_EXFER;
}

static int x_data(usb_hc_t *h, const usb_dev_t *d, uint8_t ep, int in,
                  void *data, int len, int mps)
{
    int      hcno = h->index;
    xhci_t  *x = &g_x[hcno];
    int      slot = (int)d->slot, ri, dci;
    trb_t   *r;
    int      cc;
    uint32_t st = 0;

    (void)mps;
    if (!x->valid || slot <= 0 || slot > x->maxslots) return USB_ENODEV;
    if (len < 0 || (uint32_t)len > sizeof g_bounce[0]) return USB_EPARAM;

    dci = dci_of(ep, in);
    ri  = ring_slot_for_dci(x, slot, dci);
    if (ri < 0) return USB_ENODEV;     /* configure() never set it up */
    r = g_epr[hcno][slot][ri];

    if (!in && data && len) memcpy(g_bounce[hcno], data, (uint32_t)len);

    ring_push(r, &x->ep_idx[slot][ri], &x->ep_cycle[slot][ri],
              pa(g_bounce[hcno]), 0, (uint32_t)len,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    x->ep_sync[slot][ri] = 1;
    ring_doorbell(x, (uint32_t)slot, (uint32_t)dci);

    cc = evt_wait_ep(x, hcno, slot, ri, &st, 1000);
    if (cc < 0) return cc;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        if (ri >= 0 && ri < XHCI_EP_RINGS) x->ep_logged[slot][ri] = 0;
        uint32_t residue = st & 0x00FFFFFFu;
        uint32_t got = ((uint32_t)len > residue) ? ((uint32_t)len - residue)
                                                 : 0;
        if (in && data && got) memcpy(data, g_bounce[hcno], got);
        return (int)got;
    }
    if (cc == 6) return USB_ESTALL;        /* Stall Error */
    /* ONCE per endpoint until it works again. A bulk IN polled from
       the shell's idle loop fails thousands of times a minute when a
       device has gone quiet -- v0.8.7 counted 19,440 of them -- and a
       log line per failure is a screen nobody can read. The first one
       is the informative one. */
    if (ri >= 0 && ri < XHCI_EP_RINGS && !x->ep_logged[slot][ri]) {
        x->ep_logged[slot][ri] = 1;
        klog(KLOG_WARN, "xHCI slot %u ep %2x: %s (cc %d)",
             (uint32_t)slot, (uint32_t)ep, cc_name(cc), (uint32_t)cc);
    }
    return USB_EXFER;
}

/* ════════════════════════════════════════════════════════════════════
   NON-BLOCKING INTERRUPT IN  (v0.8.5.1)

   x_data() above is correct for bulk and control and WRONG for polling a
   keyboard, and v0.8.5 shipped it being used for both.

   An interrupt IN endpoint with nothing to report NAKs. An xHCI retries a
   NAK for as long as the transfer is queued and posts NO event for it --
   a NAK is not a completion and not an error. So x_data() waits out its
   full 1000 ms on every poll where no key was pressed, and then returns
   with the TRB still live in a ring of sixteen. The visible result, on a
   laptop whose only keyboard is this one: typing works at about one
   character per second and everything faster is lost.

   The shape that works is the one every real driver uses. Keep exactly
   ONE transfer outstanding per endpoint; each poll asks whether it has
   completed and, if it has, takes the data and immediately queues the
   next. Nothing waits, and a key pressed between two polls is sitting in
   a completed transfer when the next one looks.

   Two things this needs that the synchronous path does not:

     - its own buffer per endpoint. g_bounce is shared, and a transfer
       the controller has not finished with still owns its buffer.
     - transfer events filed BY ENDPOINT. The event ring is one queue for
       the whole controller, so a poll of the keyboard will meet the
       mouse's completion, and handing one to the other is silent and
       wrong. v0.8.5.1 gave this path its own reader to get that right;
       v0.8.7.5 found the synchronous reader had it wrong in the mirror
       direction and merged both into evt_pump(), which files every
       event by owner. See the block above it.
   ════════════════════════════════════════════════════════════════════ */


/* Queue one transfer on an endpoint and ring its doorbell. */
static void intr_arm(xhci_t *x, int hcno, int slot, int ri, int len)
{
    trb_t *r = g_epr[hcno][slot][ri];

    x->ep_want[slot][ri] = (uint32_t)len;
    memset(g_abuf[hcno][slot][ri], 0, (uint32_t)len);
    ring_push(r, &x->ep_idx[slot][ri], &x->ep_cycle[slot][ri],
              pa(g_abuf[hcno][slot][ri]), 0, (uint32_t)len,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ring_doorbell(x, (uint32_t)slot, (uint32_t)x->ep_dci[slot][ri]);
    x->ep_busy[slot][ri] = 1;
}

static int x_intr_poll(usb_hc_t *h, const usb_dev_t *d, uint8_t ep, int in,
                       void *data, int len, int mps)
{
    int     hcno = h->index;
    xhci_t *x = &g_x[hcno];
    int     slot = (int)d->slot;
    int     dci, ri;

    (void)mps;
    if (!x->valid || slot <= 0 || slot > x->maxslots) return USB_ENODEV;
    if (!in) return USB_EPARAM;                   /* IN endpoints only */
    if (len <= 0 || len > XHCI_ABUF_SZ) return USB_EPARAM;

    dci = dci_of(ep, in);
    ri  = ring_slot_for_dci(x, slot, dci);
    if (ri < 0) return USB_ENODEV;                /* configure() never ran */

    evt_pump(x, hcno);

    if (x->ep_done[slot][ri]) {
        uint32_t st   = x->ep_status[slot][ri];
        uint32_t cc   = (st >> 24) & 0xFFu;
        uint32_t res  = st & 0x00FFFFFFu;
        uint32_t want = x->ep_want[slot][ri];
        int      got  = 0;

        x->ep_done[slot][ri] = 0;

        if (cc == 6) return USB_ESTALL;   /* the core clears the halt, and
                                             reset_toggle() re-arms us */
        if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
            got = (want > res) ? (int)(want - res) : 0;
            if (got > len) got = len;
            if (data && got)
                memcpy(data, g_abuf[hcno][slot][ri], (uint32_t)got);
        }
        /* Re-arm immediately, whatever came back: a device that reports
           nothing is the normal case and must stay polled. */
        intr_arm(x, hcno, slot, ri, len);
        return got;
    }

    if (!x->ep_busy[slot][ri]) intr_arm(x, hcno, slot, ri, len);
    return 0;                             /* nothing yet -- not an error */
}

static int x_bulk(usb_hc_t *h, const usb_dev_t *d, uint8_t ep, int in,
                  void *data, int len, int mps)
{
    return x_data(h, d, ep, in, data, len, mps);
}
static int x_intr(usb_hc_t *h, const usb_dev_t *d, uint8_t ep, int in,
                  void *data, int len, int mps)
{
    return x_data(h, d, ep, in, data, len, mps);
}

/* The ring cursor IS the toggle on xHCI -- there is no DATA0/DATA1 for
   software to keep. Resetting it means telling the controller to reset
   the endpoint, which is what CLEAR_FEATURE(HALT) pairs with. */
static void x_reset_toggle(usb_hc_t *h, const usb_dev_t *d, uint8_t ep,
                           int in)
{
    int     hcno = h->index;
    xhci_t *x = &g_x[hcno];
    int     slot = (int)d->slot;
    int     dci = dci_of(ep, in);
    if (!x->valid || slot <= 0 || slot > x->maxslots) return;
    cmd_run(x, hcno, 0, 0, 0,
            TRB_TYPE(TRB_RESET_EP) | ((uint32_t)slot << 24) |
            ((uint32_t)dci << 16), 0);

    /* The reset discarded whatever was queued on that endpoint, so an
       outstanding asynchronous transfer is gone. Leaving ep_busy set
       would stop x_intr_poll() ever queueing another, and the device
       would go quiet for good after its first stall. (v0.8.5.1) */
    {
        int ri = ring_slot_for_dci(x, slot, dci);
        if (ri >= 0) {
            x->ep_busy[slot][ri] = 0;
            x->ep_done[slot][ri] = 0;
        }
    }
}

/* Configure Endpoint: the step with no UHCI counterpart at all, and
   without which every endpoint beyond the default pipe returns
   USB_ENODEV above.
 *
 * ── IT IS CALLED TWICE NOW, AND IT DID NOT DROP ANYTHING, v0.8.7 ────
 *
 * Until this release the core issued exactly one of these per device,
 * after SET_CONFIGURATION, so every endpoint this command had ever
 * been told about was one that was still wanted. The command carries
 * ADD flags in dword 1 of the input control context and DROP flags in
 * dword 0, and dword 0 was left at the zero the memset below put there.
 *
 * SET_INTERFACE makes it twice on one slot. An alternate setting is a
 * different set of endpoints on the same interface, and the phone this
 * release exists for carries the two bulk endpoints of its Ethernet
 * interface on alt 1 and alt 2 with alt 0 carrying NONE -- so the first
 * real use of this driver's alternate-setting support is a configure
 * that must REMOVE two endpoints and add none.
 *
 * Without the drop flags the controller keeps the endpoint contexts of
 * a setting that is no longer selected, pointing at rings this side has
 * re-laid. A device that then NAKs on the endpoint it no longer has is
 * a transfer the controller retries for ever, which is the failure
 * v0.8.5.1 spent a release on.
 *
 * And the early return below made it worse than doing nothing: a switch
 * to a setting with no endpoints produced add == 1 and returned USB_OK
 * without issuing a command at all, so the old endpoints stayed
 * configured AND the kernel reported success.
 *
 * tests/test_xhci.c's simulated controller could not see any of this,
 * for the reason its Disable Slot case records in v0.8.6.1's words --
 * it ignored the command, "which is why it could not see that the
 * driver never sent one". It honours drop flags now.
 */
static int x_configure(usb_hc_t *h, usb_dev_t *d)
{
    int      hcno = h->index;
    xhci_t  *x = &g_x[hcno];
    int      slot = (int)d->slot;
    uint32_t add  = 1u;         /* A0: the slot context is always in */
    uint32_t drop = 0u;         /* D2..D31: endpoints to forget      */
    uint32_t want = 0u;         /* the DCIs this setting asks for    */
    int      i, r, maxdci = 1;

    if (!x->valid || slot <= 0 || slot > x->maxslots) return USB_ENODEV;

    /* Which endpoints does the new setting want? Worked out before
       anything is allocated, because the drop set is its complement
       among the rings this slot already holds -- and a ring freed
       before that is known is a ring handed back to the endpoint that
       was about to be re-added. */
    for (i = 0; i < d->nep; i++) {
        const usb_ep_t *e = &d->ep[i];
        int in  = (e->addr & 0x80) ? 1 : 0;
        int dci = dci_of(e->addr, in);
        if (dci < 2 || dci > 31) continue;
        if ((e->attr & 3) == USB_EP_CONTROL) continue;
        want |= (1u << dci);
        if (dci > maxdci) maxdci = dci;
    }

    /* Every ring this slot holds for an endpoint the new setting does
       NOT want. DCI 1 is the default pipe and is never dropped: the
       control endpoint belongs to the device, not to an interface. */
    for (i = 0; i < XHCI_EP_RINGS; i++) {
        int dci = (int)x->ep_dci[slot][i];
        if (dci < 2 || dci > 31)      continue;
        if (want & (1u << dci))       continue;
        drop |= (1u << dci);
    }

    memset(g_inctx[hcno], 0, sizeof g_inctx[hcno]);

    for (i = 0; i < d->nep; i++) {
        const usb_ep_t *e = &d->ep[i];
        int in  = (e->addr & 0x80) ? 1 : 0;
        int dci = dci_of(e->addr, in);
        int ri, eptype;
        uint8_t *iec;

        if (dci < 2 || dci > 31) continue;

        /* ── v0.8.9.9: A RUNNING ENDPOINT OF ANOTHER INTERFACE ───────
           usb_claim_interface() adds an interface beside one already
           in use and marks the old one's endpoints `keep`. They are in
           `want` above, so they are not dropped; here they are not
           re-added either. Re-adding would re-lay the ring -- throwing
           away an outstanding receive -- and reset the controller's
           sequence state for a pipe whose device side was NOT reset
           (no SET_CONFIGURATION, no SET_INTERFACE reached it), so the
           next packet would be taken as a retransmission and dropped.
           An endpoint the slot does not actually hold yet is added as
           usual, keep or not. */
        if (e->keep && ring_slot_for_dci(x, slot, dci) >= 0) continue;

        ri = ring_alloc(x, hcno, slot, dci);
        if (ri < 0) continue;       /* more endpoints than rings: skip */

        /* Re-lay it, every time. ring_alloc() returns the EXISTING ring
           for a DCI this slot already holds, and the dequeue pointer
           written below is the ring BASE -- so without this the
           controller is pointed at the start of a ring the driver is
           half way along. The comment above this function has claimed
           since v0.8.5.1 that a configure re-lays the rings; clearing
           ep_busy and ep_done is not re-laying them. */
        ring_relay(x, hcno, slot, ri);

        /* EP Type: 1 isoch out, 2 bulk out, 3 interrupt out,
                    5 isoch in,  6 bulk in,  7 interrupt in. */
        switch (e->attr & 3) {
        case USB_EP_BULK: eptype = in ? 6 : 2; break;
        case USB_EP_INTR: eptype = in ? 7 : 3; break;
        case USB_EP_ISOC: eptype = in ? 5 : 1; break;
        default:          continue;
        }

        iec = in_ep_ctx(x, hcno, dci);
        ctx_set32(iec, 0, ((uint32_t)e->interval << 16));
        ctx_set32(iec, 1, ((uint32_t)eptype << 3) | (3u << 1) |
                          ((uint32_t)e->mps << 16));
        ctx_set32(iec, 2, pa(g_epr[hcno][slot][ri]) |
                          (x->ep_cycle[slot][ri] ? 1u : 0u));
        ctx_set32(iec, 3, 0);
        ctx_set32(iec, 4, (uint32_t)e->mps);

        add |= (1u << dci);
    }

    /* Nothing to add AND nothing to forget: a device with only a
       default pipe, being configured for the first time. That is the
       one case where doing nothing is right.

       It is NOT the case where a setting with no endpoints replaces one
       that had them -- that has a non-zero drop set and must go to the
       controller. Returning USB_OK there is what left two bulk
       endpoints configured on a phone that had just been switched to
       an alternate setting with none, and reported success. */
    if (add == 1u && drop == 0u) return USB_OK;

    ctx_set32(in_ctrl_ctx(x, hcno), 0, drop);
    ctx_set32(in_ctrl_ctx(x, hcno), 1, add);
    /* The slot context has to be presented again with Context Entries
       raised to the highest DCI in use, or the controller refuses the
       command with a parameter error and says nothing else.

       In USE, not merely added: after a drop-only command the highest
       is the default pipe, and maxdci is 1 because `want` is empty. */
    {
        uint8_t *isc = in_slot_ctx(x, hcno);
        uint8_t *dsc = slot_ctx(x, hcno, slot);
        ctx_set32(isc, 0, (ctx_get32(dsc, 0) & 0x07FFFFFFu) |
                          ((uint32_t)maxdci << 27));
        ctx_set32(isc, 1, ctx_get32(dsc, 1));
        ctx_set32(isc, 2, ctx_get32(dsc, 2));
    }
    r = cmd_run(x, hcno, pa(g_inctx[hcno]), 0, 0,
                TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)slot << 24), 0);

    /* Give the dropped rings back on THIS side, after the controller
       has been told -- the order x_detach() uses, and for its reason:
       a command that fails leaves the endpoint live on that side and
       this side must not pretend otherwise.

       Freed even when the command is refused, for x_detach()'s other
       reason: a ring this kernel's bookkeeping holds for an endpoint
       the controller has refused to give up is a ring unusable for
       ever rather than until the next enumeration, and XHCI_EP_RINGS
       is five. */
    if (drop) {
        for (i = 0; i < XHCI_EP_RINGS; i++) {
            int dci = (int)x->ep_dci[slot][i];
            if (dci < 2 || dci > 31)          continue;
            if (!(drop & (1u << dci)))        continue;
            ring_free(x, slot, i);
        }
    }
    return r;
}

/* Designated, not positional. v0.8.5.1 added intr_poll in the middle of
   usb_hc_ops_t, and with a positional initialiser that silently makes
   every member after it bind to the wrong function -- here it would have
   made x_configure the interrupt poll and left configure() null, which
   compiles cleanly and breaks every endpoint beyond the default pipe.
   By name, the compiler cannot get it wrong. */
static const usb_hc_ops_t XHCI_OPS = {
    .name           = "xHCI",
    .port_count     = x_port_count,
    .port_connected = x_port_connected,
    .attach         = x_attach,
    .attach_path    = x_attach_path,
    .detach         = x_detach,
    .control        = x_control,
    .bulk           = x_bulk,
    .intr           = x_intr,
    .reset_toggle   = x_reset_toggle,
    .intr_poll      = x_intr_poll,
    .configure      = x_configure,
    .hub_info       = x_hub_info,
};

/* ── discovery ─────────────────────────────────────────────────────── */
int xhci_hc_register(void)
{
    int i, n, reg = 0;

    g_nx = 0;
    n = pci_count();
    for (i = 0; i < n && g_nx < XHCI_MAX_HC; i++) {
        const pci_dev_t *p = pci_get(i);
        uint32_t bar, bar_lo, bar_hi = 0;
        const char *why = "";
        if (!p) continue;
        /* Serial bus controller / USB / xHCI. The programming interface
           is what separates this from a UHCI (0x00), an OHCI (0x10) and
           an EHCI (0x20) sitting in the same class and subclass -- and
           it is the byte `usb -vv` already reports on a machine this
           kernel cannot drive. On the HP it reads 0x30. */
        if (p->class != 0x0C || p->subclass != 0x03 || p->progif != 0x30)
            continue;

        /* ── THE BAR, BOTH HALVES (v0.8.5.1) ──────────────────────
           v0.8.5 called pci_bar_mem(), which returned the low dword of
           a 64-bit BAR. On the HP that is 0x02120000 -- the real base
           is 0x60_02120000, about 384 GiB. The driver read a
           capability register out of whatever sits at 33 MB, got a
           nonsense CAPLENGTH and refused to start, which was correct
           and useless.

           A 32-bit kernel cannot address 384 GiB with a plain pointer,
           so the registers are mapped into a window first.
           kernel/highfb.c already does exactly this for a framebuffer
           above 4 GB, via PSE-36 or PAE, and it was written with eight
           windows precisely because the framebuffer is not the only
           thing up there. This is the second tenant. */
        bar_lo = pci_bar_mem64(p, 0, &bar_hi);
        if (!bar_lo && !bar_hi) continue;

        if (bar_hi) {
            uint32_t virt = 0;
            if (!highfb_map_io(bar_hi, bar_lo, XHCI_MMIO_BYTES, &virt)) {
                klog(KLOG_WARN, "xHCI %2x:%2x.%u is at %8x%8x, above 4 GB, "
                                "and could not be mapped",
                     (uint32_t)p->bus, (uint32_t)p->dev, (uint32_t)p->fn,
                     bar_hi, bar_lo);
                continue;
            }
            klog(KLOG_INFO, "xHCI %2x:%2x.%u BAR %8x%8x mapped at %8x",
                 (uint32_t)p->bus, (uint32_t)p->dev, (uint32_t)p->fn,
                 bar_hi, bar_lo, virt);
            bar = virt;
        } else {
            bar = bar_lo;
        }

        /* Memory space and BUS MASTERING. An xHCI reads its command
           ring, writes its event ring and moves every byte of every
           transfer by DMA, so a controller that is not bus master comes
           up, accepts register writes, and then never completes a single
           command -- which looks exactly like a dead controller. */
        pci_enable(p, 1);

        memset(&g_x[g_nx], 0, sizeof g_x[g_nx]);
        if (xhci_start(&g_x[g_nx], g_nx, bar, &why) != USB_OK) {
            klog(KLOG_WARN, "xHCI %2x:%2x.%u at %8x: %s",
                 (uint32_t)p->bus, (uint32_t)p->dev, (uint32_t)p->fn,
                 bar, why);
            klog(KLOG_WARN, "  CAPLENGTH read as %2x (wanted 10..100)",
                 g_x[g_nx].dbg_caplen);
            continue;
        }
        {
            int ho = g_x[g_nx].bios_handoff;
            klog(ho == XHCI_HANDOFF_REFUSED ? KLOG_WARN : KLOG_OK,
                 "xHCI %2x:%2x.%u: %s",
                 (uint32_t)p->bus, (uint32_t)p->dev, (uint32_t)p->fn,
                 ho == XHCI_HANDOFF_TAKEN   ? "taken from the BIOS" :
                 ho == XHCI_HANDOFF_REFUSED ? "BIOS would NOT hand it over; "
                                              "keys may arrive twice" :
                 ho == XHCI_HANDOFF_NOTHELD ? "the BIOS was not holding it" :
                                              "no legacy capability");
        }
        klog(KLOG_OK, "xHCI %2x:%2x.%u at %8x, %u ports, %u slots, "
                      "%u-byte contexts",
             (uint32_t)p->bus, (uint32_t)p->dev, (uint32_t)p->fn,
             bar, (uint32_t)g_x[g_nx].maxports,
             (uint32_t)g_x[g_nx].maxslots, (uint32_t)g_x[g_nx].ctxsz);

        if (usb_register_hc(&XHCI_OPS, g_nx, bar, USB_HC_XHCI) >= 0) reg++;
        g_nx++;
    }
    return reg;
}

int xhci_present(void) { return g_nx > 0; }

/* For `usb -v`. The raw port register, so a port that is connected but
   not enabled can be told from one that is empty -- the distinction that
   matters when something is plugged in and nothing happened. */
uint32_t xhci_portsc_raw(int idx, int port)
{
    if (idx < 0 || idx >= g_nx || !g_x[idx].valid) return 0;
    if (port < 0 || port >= g_x[idx].maxports) return 0;
    return mmio_read32(g_x[idx].op, XOP_PORTSC(port));
}
