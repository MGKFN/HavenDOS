/*
 * uhci.c — UHCI (USB 1.1) host controller driver for HavenDOS
 *
 * v0.7.20 made the USB controller FINDABLE.  Before it, every PCI scan in
 * the tree looked at function 0 only, and on a PIIX3 the controller is
 * function 2 of device 1 -- so it was not undetected, it was undetectable.
 * This file is the part that makes it WORK.
 *
 * ── what a UHCI controller actually is ────────────────────────────────
 *
 * Eleven I/O registers behind BAR4, and a schedule in main memory that the
 * controller walks by DMA once per millisecond frame.  The driver never
 * moves a byte of USB data itself; it writes descriptors into memory and
 * the controller executes them.  That is why pci_enable(d, 1) -- bus
 * mastering -- is not optional: without it the controller cannot read the
 * schedule and nothing happens at all, silently.
 *
 *   frame_list[0..1023]   one entry per 1 ms frame
 *          |              (all 1024 point at the same place: this kernel
 *          v               has no periodic or isochronous transfers)
 *     qh_control  ---->  qh_bulk  ---->  terminate
 *          |                  |
 *          v                  v
 *      TD -> TD -> TD     TD -> TD
 *
 * A transfer is a chain of Transfer Descriptors hung off a Queue Head.  The
 * controller runs each TD, writes the result back into it, and advances the
 * queue head's element pointer.  When the element pointer reads "terminate"
 * the chain is finished.  That element pointer is the authoritative answer
 * to "is it done yet" -- not the TD flags, which are only how it got there.
 *
 * ── why this is polled ───────────────────────────────────────────────
 *
 * House rule: no disk I/O in this kernel depends on an interrupt, because
 * IRQs are unreliable under UTM SE's TCG.  USB is disk I/O as far as the
 * music player is concerned, so USBINTR stays zero and the driver watches
 * the queue head in memory.  The controller still DMAs on its own clock;
 * polling only decides when we look.  Audio will be the first thing here
 * that genuinely needs an IRQ, and that is a decision to make out loud
 * rather than smuggle in now.
 *
 * ── why the descriptors are static, not kmalloc'd ─────────────────────
 *
 * The controller requires every TD and QH to be 16-byte aligned and the
 * frame list to be 4096-byte aligned, and it addresses them PHYSICALLY.
 * kmalloc() promises 4-byte alignment, so a heap TD would be rejected by
 * the hardware roughly three times in four -- and the symptom would be a
 * controller that quietly does nothing rather than an error.  There is no
 * paging in this kernel, so a BSS address is already the physical address
 * DMA needs, and __attribute__((aligned)) gives the guarantee for free.
 * The pool is fixed-size, so a transfer can never fail for want of memory
 * at an awkward moment; it is rejected up front with UHCI_ENOMEM instead.
 *
 * ── why there is more than one controller ─────────────────────────────
 *
 * A machine very rarely has exactly one UHCI controller.  A PIIX3 has one,
 * but an ICH has two to five, and a USB 2.0 controller is normally an EHCI
 * with THREE UHCI companions sharing its sockets two ports at a time:
 *
 *     00:04.0  EHCI       high-speed
 *     00:05.0  UHCI 1     ports 0-1   \
 *     00:06.0  UHCI 2     ports 2-3    > the same physical sockets
 *     00:07.0  UHCI 3     ports 4-5   /
 *
 * So the driver brings up EVERY UHCI controller it finds and scans all of
 * their ports.  Driving only the first works until a device is plugged into
 * the wrong socket, and then it is invisible with no error anywhere -- which
 * is the hardest kind of fault to go looking for.
 */

#include "../include/types.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/pci.h"
#include "../include/uhci.h"
#include "../include/klog.h"

/* Supplied by pit.c.  Declared rather than included so tests/test_uhci.c can
   substitute a virtual clock that also steps a simulated controller --
   the same trick io.h plays for the ports. */
extern uint32_t get_ticks(void);        /* 10 ms units, PIT-polled */
extern void     sleep_ms(uint32_t ms);

/* ── registers (offsets from BAR4) ────────────────────────────────────── */
#define UHCI_USBCMD      0x00
#define UHCI_USBSTS      0x02
#define UHCI_USBINTR     0x04
#define UHCI_FRNUM       0x06
#define UHCI_FRBASEADD   0x08
#define UHCI_SOFMOD      0x0C
#define UHCI_PORTSC1     0x10

/* USBCMD */
#define CMD_RS           0x0001   /* run/stop                              */
#define CMD_HCRESET      0x0002   /* host controller reset                 */
#define CMD_GRESET       0x0004   /* global reset: resets the bus too      */
#define CMD_EGSM         0x0008
#define CMD_FGR          0x0010
#define CMD_SWDBG        0x0020
#define CMD_CF           0x0040   /* configure flag: "software is here"    */
#define CMD_MAXP         0x0080   /* 64-byte packets for bandwidth reclaim */

/* USBSTS */
#define STS_USBINT       0x0001
#define STS_ERRINT       0x0002
#define STS_RD           0x0004
#define STS_HSE          0x0008   /* host system error: a failed DMA       */
#define STS_HCPE         0x0010   /* host controller process error         */
#define STS_HCH          0x0020   /* halted                                */
#define STS_ALL          0x003F

/* PORTSC */
#define PORT_CCS         0x0001   /* a device is attached                  */
#define PORT_CSC         0x0002   /* connect changed      (write 1=clear)  */
#define PORT_PE          0x0004   /* port enabled                          */
#define PORT_PEC         0x0008   /* enable changed       (write 1=clear)  */
#define PORT_LINE        0x0030
#define PORT_RD          0x0040
#define PORT_ALWAYS1     0x0080   /* reads 1 on a port that exists         */
#define PORT_LSDA        0x0100   /* low-speed device attached             */
#define PORT_RESET       0x0200
#define PORT_SUSP        0x1000
/* The two write-1-to-clear bits.  Every write to PORTSC must mask these off
   unless it means to clear them, or acking one change silently discards
   another that arrived in between. */
#define PORT_WC          (PORT_CSC | PORT_PEC)

/* PCI config: USB legacy support.  The BIOS uses the controller to emulate
   a PS/2 keyboard before the OS loads, and leaves that trapping armed.
   Writing 0x8F00 clears every R/WC status bit and disables PIRQ and SMI
   generation -- i.e. hands the controller over. */
#define UHCI_PCI_LEGSUP      0xC0
#define UHCI_PCI_LEGSUP_RWC  0x8F00

/* ── link pointer bits ────────────────────────────────────────────────── */
#define LINK_TERM        0x00000001u   /* nothing follows                  */
#define LINK_QH          0x00000002u   /* what follows is a QH, not a TD   */
#define LINK_VF          0x00000004u   /* depth first: run the chain now   */
#define LINK_MASK        0xFFFFFFF0u

/* ── TD control/status bits ───────────────────────────────────────────── */
#define TD_ACTLEN        0x000007FFu
#define TD_BITSTUFF      (1u << 17)
#define TD_CRCTO         (1u << 18)
#define TD_NAK           (1u << 19)   /* not an error: the device is busy  */
#define TD_BABBLE        (1u << 20)
#define TD_DBUF          (1u << 21)
#define TD_STALL         (1u << 22)
#define TD_ACTIVE        (1u << 23)
#define TD_IOC           (1u << 24)
#define TD_ISO           (1u << 25)
#define TD_LS            (1u << 26)
#define TD_SPD           (1u << 29)
#define TD_CERR(n)       ((uint32_t)((n) & 3) << 27)
#define TD_ERRMASK       (TD_BITSTUFF | TD_CRCTO | TD_BABBLE | TD_DBUF | TD_STALL)

/* ── packet IDs ───────────────────────────────────────────────────────── */
#define PID_OUT          0xE1
#define PID_IN           0x69
#define PID_SETUP        0x2D

/* ── the schedule ─────────────────────────────────────────────────────── */

typedef struct {
    volatile uint32_t link;
    volatile uint32_t ctrl;
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t          _pad[4];        /* hardware reads 16 bytes; 32 keeps
                                         every entry 16-byte aligned */
} uhci_td_t;

typedef struct {
    volatile uint32_t head;           /* next queue head                   */
    volatile uint32_t element;        /* the chain currently being run     */
    uint32_t          _pad[2];
} uhci_qh_t;

#define MAX_TD 128                    /* 8 KB per bulk transfer at 64 B/TD */
#define MAX_HC 6                      /* an ICH has up to five, plus slack  */

/* Every controller runs its own schedule, so the frame list and the two
   queue heads are per controller.  The descriptor pool is not: only one
   transfer is ever in flight, because every transfer here is polled to
   completion before the next is built. */
static uint32_t  frame_list[MAX_HC][1024] __attribute__((aligned(4096)));
static uhci_qh_t qh_control[MAX_HC]       __attribute__((aligned(16)));
static uhci_qh_t qh_bulk[MAX_HC]          __attribute__((aligned(16)));
static uhci_td_t td_pool[MAX_TD]          __attribute__((aligned(16)));
static uint8_t   setup_buf[8]      __attribute__((aligned(16)));

/* The descriptor uhci_probe_port() reads into.  Static rather than on the
   stack because the controller fetches it by DMA from the physical address
   in the TD: static storage is a place the controller can certainly reach,
   it cannot move under a transfer that timed out and is still being written
   back, and it is what lets the native test run this driver unmodified. */
static uint8_t   probe_desc[18]    __attribute__((aligned(16)));

/* ── driver state ─────────────────────────────────────────────────────── */
typedef struct {
    uint16_t base;                        /* BAR4, 0 when unused           */
    int      ports;
    uint8_t  irq;
} uhci_hc_t;

static uhci_hc_t g_hc[MAX_HC];
static int       g_nhc;                   /* controllers brought up        */
static uint8_t   g_other_progif;          /* an EHCI/xHCI we cannot drive  */
static int       g_other_found;

#define MAX_USB_DEV 8
static uhci_dev_t g_usbdev[MAX_USB_DEV];
static int        g_nusbdev;
static uint8_t    g_next_addr = 1;

/* Data toggle per (address, endpoint, direction).  The USB spec makes this
   state of the ENDPOINT, carried across transfers, not a property of one
   transfer: get it wrong and the device silently discards every other
   packet.  128 addresses x 16 endpoints x 2 directions = 512 bytes. */
static uint8_t toggle_bits[(128 * 16 * 2) / 8];

static int tog_index(uint8_t addr, uint8_t ep, int in) {
    return (((int)(addr & 0x7F) * 16) + (ep & 0x0F)) * 2 + (in ? 1 : 0);
}
static int tog_get(uint8_t addr, uint8_t ep, int in) {
    int i = tog_index(addr, ep, in);
    return (toggle_bits[i >> 3] >> (i & 7)) & 1;
}
static void tog_set(uint8_t addr, uint8_t ep, int in, int v) {
    int i = tog_index(addr, ep, in);
    if (v) toggle_bits[i >> 3] |=  (uint8_t)(1u << (i & 7));
    else   toggle_bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

/* ── the log ──────────────────────────────────────────────────────────
   This driver kept its own text buffer, built a nibble at a time.  It is
   gone: everything it has to say goes to the kernel log, where the boot
   screen and dmesg both read it, instead of being a private story only
   `usb -v` could tell. */

/* ── small helpers ────────────────────────────────────────────────────── */

static uhci_hc_t *hc_get(int i) {
    return (i >= 0 && i < g_nhc && g_hc[i].base) ? &g_hc[i] : 0;
}

/* The controller a device lives behind.  A null device means "whichever one
   is first", which is what probing at address 0 needs before any device has
   been placed. */
static int hc_index_of(const uhci_dev_t *d) {
    if (!d) return g_nhc ? 0 : -1;
    return (d->hc < g_nhc) ? d->hc : -1;
}

static uint16_t port_reg(const uhci_hc_t *h, int port) {
    return (uint16_t)(h->base + UHCI_PORTSC1 + port * 2);
}

/* Signed difference so the comparison stays correct when get_ticks() wraps,
   which it does roughly every 71 minutes. */
static int tick_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static uhci_td_t *td_from_link(uint32_t link) {
    if (link & (LINK_TERM | LINK_QH)) return 0;
    uint32_t a  = link & LINK_MASK;
    uint32_t lo = (uint32_t)(uintptr_t)&td_pool[0];
    uint32_t hi = lo + (uint32_t)sizeof(td_pool);
    if (a < lo || a >= hi) return 0;            /* not one of ours */
    return (uhci_td_t *)(uintptr_t)a;
}

/* Bytes a TD actually moved.  The field holds length-1, and the all-ones
   value 0x7FF means zero bytes -- not 2048.  Reading it as a plain count is
   the classic UHCI off-by-one and it shows up as every transfer appearing
   one byte short. */
static int td_actual(const uhci_td_t *t) {
    uint32_t a = t->ctrl & TD_ACTLEN;
    return (a == TD_ACTLEN) ? 0 : (int)(a + 1);
}

static void td_build(uhci_td_t *t, int ls, int toggle, uint8_t pid,
                     uint8_t addr, uint8_t ep, void *buf, int len, int spd)
{
    uint32_t c = TD_ACTIVE | TD_CERR(3);
    if (ls)  c |= TD_LS;
    if (spd) c |= TD_SPD;
    t->link   = LINK_TERM;
    t->ctrl   = c;
    t->token  = (uint32_t)pid
              | ((uint32_t)(addr & 0x7F) << 8)
              | ((uint32_t)(ep   & 0x0F) << 15)
              | ((uint32_t)(toggle & 1)  << 19)
              | ((uint32_t)(len ? ((len - 1) & 0x7FF) : 0x7FF) << 21);
    t->buffer = buf ? (uint32_t)(uintptr_t)buf : 0;
}

/* Chain n TDs depth-first so the whole transfer runs inside one frame.
   Without the Vf bit the controller takes one TD per millisecond, which
   turns an 18-byte descriptor read into a 5 ms operation and a 512-byte
   bulk read into 9 ms. */
static void td_chain(int n) {
    for (int i = 0; i < n - 1; i++)
        td_pool[i].link = (uint32_t)(uintptr_t)&td_pool[i + 1] | LINK_VF;
    td_pool[n - 1].link = LINK_TERM;
}

/* ── running a chain ──────────────────────────────────────────────────── */
/*
 * Hands td_pool[0..n-1] to a queue head and waits.  The queue head's element
 * pointer is what is watched: the controller advances it as each TD retires,
 * so TERM means the whole chain ran, and a non-TERM pointer at an inactive
 * TD means the controller stopped there -- an error, or a short packet with
 * SPD set.  Either way the transfer is over.
 *
 * Returns the index of the TD the controller stopped on, or n if the chain
 * drained, or -1 on timeout.
 */
static int run_chain(uhci_qh_t *qh, int n, uint32_t timeout_ms)
{
    uint32_t deadline = get_ticks() + (timeout_ms / 10) + 2;
    int stopped = -1;

    __asm__ volatile("" ::: "memory");       /* TDs written before armed */
    qh->element = (uint32_t)(uintptr_t)&td_pool[0];

    for (;;) {
        uint32_t el = qh->element;
        if (el & LINK_TERM) { stopped = n; break; }

        uhci_td_t *cur = td_from_link(el);
        if (!cur) { stopped = n; break; }     /* can't happen; don't hang */

        if (!(cur->ctrl & TD_ACTIVE)) {
            stopped = (int)(cur - &td_pool[0]);
            break;
        }
        if (tick_after(get_ticks(), deadline)) break;
        __asm__ volatile("pause");
    }

    qh->element = LINK_TERM;                  /* unlink, however it ended */
    __asm__ volatile("" ::: "memory");

    /* A timed-out chain may still be mid-execution.  Unlinking stops the
       controller picking up any further TD, but it can still be writing back
       the one it holds, and the pool is about to be rebuilt for the next
       transfer.  One frame of grace costs a millisecond on a path that has
       already failed, and avoids a write-back landing in a fresh TD. */
    if (stopped < 0) sleep_ms(2);
    return stopped;
}

/* Turns whatever the chain did into a UHCI_E* code.  NAK is deliberately not
   an error: it means the device was busy and the controller retried, which
   is normal traffic. */
static int chain_status(int first, int count)
{
    for (int i = first; i < first + count; i++) {
        uint32_t c = td_pool[i].ctrl;
        if (c & TD_ACTIVE)   break;           /* never reached */
        if (c & TD_STALL)    return UHCI_ESTALL;
        if (c & (TD_CRCTO | TD_BABBLE | TD_DBUF | TD_BITSTUFF)) return UHCI_EXFER;
    }
    return UHCI_OK;
}

/* ── control transfers ────────────────────────────────────────────────── */

int uhci_control(const uhci_dev_t *d, uint8_t rtype, uint8_t req,
                 uint16_t value, uint16_t index, void *data, uint16_t len)
{
    int ci = hc_index_of(d);
    if (ci < 0)             return UHCI_ENODEV;
    if (len && !data)       return UHCI_EPARAM;

    uint8_t addr = d ? d->addr : 0;
    int     ls   = d ? d->lowspeed : 0;
    int     mps  = (d && d->maxpacket0) ? d->maxpacket0 : 8;
    int     in   = (rtype & USB_DIR_IN) != 0;

    int ndata = (len + mps - 1) / mps;
    int ntd   = 1 + ndata + 1;                /* SETUP + data + STATUS */
    if (ntd > MAX_TD) return UHCI_ENOMEM;

    setup_buf[0] = rtype;
    setup_buf[1] = req;
    setup_buf[2] = (uint8_t)(value & 0xFF);
    setup_buf[3] = (uint8_t)(value >> 8);
    setup_buf[4] = (uint8_t)(index & 0xFF);
    setup_buf[5] = (uint8_t)(index >> 8);
    setup_buf[6] = (uint8_t)(len & 0xFF);
    setup_buf[7] = (uint8_t)(len >> 8);

    int n = 0;
    /* SETUP is always DATA0; the data stage starts at DATA1 and alternates;
       the status stage is always DATA1 and runs the opposite way. */
    td_build(&td_pool[n++], ls, 0, PID_SETUP, addr, 0, setup_buf, 8, 0);

    int first_data = n, toggle = 1, left = len;
    uint8_t *p = (uint8_t *)data;
    while (left > 0) {
        int chunk = left > mps ? mps : left;
        /* SPD on an IN data stage so a device that answers short stops the
           queue cleanly instead of the controller issuing further INs that
           the device NAKs until the transfer times out. */
        td_build(&td_pool[n++], ls, toggle, in ? PID_IN : PID_OUT,
                 addr, 0, p, chunk, in);
        toggle ^= 1; p += chunk; left -= chunk;
    }
    int status_td = n;
    td_build(&td_pool[n++], ls, 1, in ? PID_OUT : PID_IN, addr, 0, 0, 0, 0);
    td_chain(n);

    int stop = run_chain(&qh_control[ci], n, 1000);
    if (stop < 0) return UHCI_ETIMEOUT;

    int st = chain_status(0, n);
    if (st != UHCI_OK) return st;

    int moved = 0;
    for (int i = first_data; i < status_td; i++) {
        if (td_pool[i].ctrl & TD_ACTIVE) break;
        moved += td_actual(&td_pool[i]);
    }

    /* A short IN stops the queue before the status stage, which would leave
       the device waiting mid-transfer and stall the NEXT request instead of
       this one -- the kind of fault that gets blamed on whatever runs
       afterwards.  Run the status stage on its own. */
    if (stop <= status_td && (td_pool[status_td].ctrl & TD_ACTIVE)) {
        td_build(&td_pool[0], ls, 1, in ? PID_OUT : PID_IN, addr, 0, 0, 0, 0);
        td_chain(1);
        if (run_chain(&qh_control[ci], 1, 1000) < 0) return UHCI_ETIMEOUT;
        st = chain_status(0, 1);
        if (st != UHCI_OK) return st;
    }
    return moved;
}

/* ── bulk transfers ───────────────────────────────────────────────────── */

void uhci_bulk_reset_toggle(const uhci_dev_t *d, uint8_t ep, int in)
{
    if (d) tog_set(d->addr, ep, in, 0);
}

int uhci_bulk(const uhci_dev_t *d, uint8_t ep, int in, void *data, int len,
              int mps)
{
    if (!d)           return UHCI_EPARAM;
    int ci = hc_index_of(d);
    if (ci < 0)       return UHCI_ENODEV;
    if (len && !data) return UHCI_EPARAM;
    if (mps <= 0 || mps > 64) mps = 64;       /* full-speed bulk maximum */

    uint8_t *p = (uint8_t *)data;
    int left = len, moved = 0;

    /* The pool bounds one submission, not one request: a longer transfer is
       split here rather than refused, so callers above never have to know
       how big the pool is. */
    while (left > 0) {
        int batch = left;
        if (batch > MAX_TD * mps) batch = MAX_TD * mps;

        int n = 0;
        int done_in_batch = 0;
        while (done_in_batch < batch) {
            int chunk = batch - done_in_batch;
            if (chunk > mps) chunk = mps;
            td_build(&td_pool[n++], d->lowspeed, tog_get(d->addr, ep, in),
                     in ? PID_IN : PID_OUT, d->addr, ep, p + done_in_batch,
                     chunk, in);
            tog_set(d->addr, ep, in, !tog_get(d->addr, ep, in));
            done_in_batch += chunk;
        }
        td_chain(n);

        int stop = run_chain(&qh_bulk[ci], n, 2000);
        if (stop < 0) return moved ? moved : UHCI_ETIMEOUT;

        int st = chain_status(0, n);
        if (st != UHCI_OK) return st;

        int got = 0, ran = 0;
        for (int i = 0; i < n; i++) {
            if (td_pool[i].ctrl & TD_ACTIVE) break;
            got += td_actual(&td_pool[i]);
            ran++;
        }
        moved += got;
        p     += got;
        left  -= got;

        /* A short packet ends a bulk transfer by definition -- it is how a
           device says "that is all I have".  The TDs that never ran left the
           toggle advanced past where the endpoint actually is, so it is put
           back or every later transfer on this endpoint is discarded. */
        if (ran < n || got < done_in_batch) {
            int t = tog_get(d->addr, ep, in);
            for (int i = ran; i < n; i++) t ^= 1;
            tog_set(d->addr, ep, in, t);
            break;
        }
    }
    return moved;
}

/* ── ports ────────────────────────────────────────────────────────────── */

uint16_t uhci_portsc(int hc, int port) {
    uhci_hc_t *h = hc_get(hc);
    if (!h || port < 0 || port >= h->ports) return 0;
    return inw(port_reg(h, port));
}

int uhci_port_connected(int hc, int port) {
    return (uhci_portsc(hc, port) & PORT_CCS) ? 1 : 0;
}

int uhci_port_reset(int hc, int port)
{
    uhci_hc_t *h = hc_get(hc);
    if (!h || port < 0 || port >= h->ports) return 0;
    uint16_t reg = port_reg(h, port);
    uint16_t v   = inw(reg);
    if (!(v & PORT_CCS)) return 0;             /* nothing attached */

    /* The USB spec asks for at least 10 ms of reset; every real driver uses
       50 ms because plenty of devices need it. */
    outw(reg, (uint16_t)((v & ~PORT_WC) | PORT_RESET));
    sleep_ms(50);
    outw(reg, (uint16_t)(inw(reg) & ~(PORT_WC | PORT_RESET)));

    /* The port is not writable the instant reset drops, so enabling it is a
       retry loop rather than a single write.  Change bits are acknowledged
       as they appear -- writing a 1 back is what clears them -- because an
       unacknowledged PEC keeps the port from staying enabled. */
    for (int i = 0; i < 12; i++) {
        sleep_ms(10);
        v = inw(reg);
        if (!(v & PORT_CCS)) return 0;         /* unplugged during reset */
        if (v & PORT_PE)     return 1;
        if (v & PORT_WC) { outw(reg, (uint16_t)(v & ~PORT_RESET)); continue; }
        outw(reg, (uint16_t)((v & ~(PORT_WC | PORT_RESET)) | PORT_PE));
    }
    return (inw(reg) & PORT_PE) ? 1 : 0;
}

/* ── first contact with a device ──────────────────────────────────────── */

int uhci_probe_port(int hc, int port, uhci_dev_t *out)
{
    if (!hc_get(hc)) return UHCI_ENODEV;
    if (!out)        return UHCI_EPARAM;
    if (!uhci_port_reset(hc, port)) return UHCI_ENODEV;

    uhci_dev_t d;
    memset(&d, 0, sizeof(d));
    d.hc         = (uint8_t)hc;
    d.port       = (uint8_t)port;
    d.addr       = 0;                          /* every device starts here */
    d.lowspeed   = (uhci_portsc(hc, port) & PORT_LSDA) ? 1 : 0;
    d.maxpacket0 = 8;                          /* the only size guaranteed */

    /* The device is at address 0 and the endpoint-0 packet size is not known
       yet, so the first read asks for exactly eight bytes -- every device
       must manage that much in one packet.  Byte 7 is the real size. */
    uint8_t *desc = probe_desc;
    memset(desc, 0, 18);
    int r = uhci_control(&d, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                         USB_REQ_GET_DESCRIPTOR,
                         (uint16_t)(USB_DESC_DEVICE << 8), 0, desc, 8);
    if (r < 0) return r;
    if (r < 8) return UHCI_EXFER;
    d.maxpacket0 = desc[7] ? desc[7] : 8;

    if (g_next_addr == 0 || g_next_addr > 127) g_next_addr = 1;
    uint8_t newaddr = g_next_addr;

    r = uhci_control(&d, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                     USB_REQ_SET_ADDRESS, newaddr, 0, 0, 0);
    if (r < 0) return r;

    /* The spec allows a device 2 ms to adopt its new address; talking to it
       sooner gets no answer and looks like a dead device. */
    sleep_ms(5);
    d.addr = newaddr;
    g_next_addr++;

    /* Toggles belong to the endpoint, and this address has just been handed
       to a different device than last time it was used. */
    for (int ep = 0; ep < 16; ep++) {
        tog_set(d.addr, (uint8_t)ep, 0, 0);
        tog_set(d.addr, (uint8_t)ep, 1, 0);
    }

    r = uhci_control(&d, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                     USB_REQ_GET_DESCRIPTOR,
                     (uint16_t)(USB_DESC_DEVICE << 8), 0, desc, 18);
    if (r < 0) return r;
    if (r < 18) return UHCI_EXFER;

    d.dev_class    = desc[4];
    d.dev_subclass = desc[5];
    d.dev_protocol = desc[6];
    d.maxpacket0   = desc[7] ? desc[7] : 8;
    d.vendor       = (uint16_t)(desc[8]  | (desc[9]  << 8));
    d.product      = (uint16_t)(desc[10] | (desc[11] << 8));
    d.release      = (uint16_t)(desc[12] | (desc[13] << 8));
    d.nconfigs     = desc[17];
    d.valid        = 1;

    *out = d;
    return UHCI_OK;
}

int uhci_scan(void)
{
    g_nusbdev = 0;

    /* Every controller, every port.  On an EHCI companion set the sockets
       are split two per controller, so stopping at the first one finds a
       device only if it happened to be plugged into the right pair. */
    for (int c = 0; c < g_nhc && g_nusbdev < MAX_USB_DEV; c++) {
        uhci_hc_t *h = hc_get(c);
        if (!h) continue;
        for (int p = 0; p < h->ports && g_nusbdev < MAX_USB_DEV; p++) {
            if (!uhci_port_connected(c, p)) continue;
            uhci_dev_t d;
            int r = uhci_probe_port(c, p, &d);
            if (r == UHCI_OK) {
                g_usbdev[g_nusbdev++] = d;
                klog(KLOG_OK, "USB hc%u port %u: %4x:%4x %s, address %u",
                     (uint32_t)c, (uint32_t)p,
                     (uint32_t)d.vendor, (uint32_t)d.product,
                     d.lowspeed ? "low-speed" : "full-speed", (uint32_t)d.addr);
            } else {
                klog(KLOG_WARN, "USB hc%u port %u: device did not answer (%d)",
                     (uint32_t)c, (uint32_t)p, r);
            }
        }
    }
    return g_nusbdev;
}

int               uhci_device_count(void)  { return g_nusbdev; }
const uhci_dev_t *uhci_device(int i)       { return (i >= 0 && i < g_nusbdev) ? &g_usbdev[i] : 0; }
int               uhci_present(void)       { return g_nhc > 0; }
int               uhci_hc_count(void)      { return g_nhc; }

uint16_t uhci_io_base(int hc)    { uhci_hc_t *h = hc_get(hc); return h ? h->base  : 0; }
int      uhci_port_count(int hc) { uhci_hc_t *h = hc_get(hc); return h ? h->ports : 0; }
uint8_t  uhci_hc_irq(int hc)     { uhci_hc_t *h = hc_get(hc); return h ? h->irq   : 0xFF; }
uint16_t uhci_usbcmd(int hc)     { uhci_hc_t *h = hc_get(hc); return h ? inw((uint16_t)(h->base + UHCI_USBCMD)) : 0; }
uint16_t uhci_usbsts(int hc)     { uhci_hc_t *h = hc_get(hc); return h ? inw((uint16_t)(h->base + UHCI_USBSTS)) : 0; }
uint16_t uhci_frnum(int hc)      { uhci_hc_t *h = hc_get(hc); return h ? inw((uint16_t)(h->base + UHCI_FRNUM))  : 0; }

int uhci_other_controller(uint8_t *progif_out)
{
    if (progif_out) *progif_out = g_other_progif;
    return g_other_found;
}

/* ── taking the ports back from an EHCI companion pair ────────────────
 *
 * A USB 2.0 controller is usually not one controller.  On the ICH9 -- and on
 * what UTM builds for "USB Support: USB 2.0" -- it is an EHCI plus three UHCI
 * companions sharing the same physical ports:
 *
 *     00:04.0  EHCI       high-speed, owns the ports once an OS claims them
 *     00:05.0  UHCI 1     ports 0-1   \
 *     00:06.0  UHCI 2     ports 2-3    > the same sockets, at full speed
 *     00:07.0  UHCI 3     ports 4-5   /
 *
 * Which controller a device lands on is decided by SPEED.  Plug in a USB 2.0
 * stick and the EHCI takes it at 480 Mbit, and the UHCI companion sees an
 * empty port -- so a UHCI-only kernel finds a controller that runs perfectly
 * and reports nothing attached, which looks exactly like a broken driver.
 *
 * The EHCI specification provides for this.  Section 4.2: CONFIGFLAG set to
 * zero "default-routes all ports to the companion host controllers", and each
 * port's Port Owner bit hands that one port over individually.  Zero is the
 * value after reset -- it is the state the BIOS runs in, which is how BIOS
 * USB keyboard support works with no EHCI driver at all.  So this is not a
 * trick; it is asking for the configuration the hardware starts in, and the
 * device re-attaches to the companion at full speed.
 *
 * The cost is real and worth saying plainly: 12 Mbit instead of 480.  That is
 * about 1 MB/s, so a 4 MB song takes some seconds to read.  For this kernel
 * that is the difference between USB working and not working at all.
 */

#define EHCI_CAPLENGTH   0x00
#define EHCI_HCSPARAMS   0x04
#define EHCI_HCCPARAMS   0x08
#define EHCI_OP_USBCMD   0x00
#define EHCI_OP_USBSTS   0x04
#define EHCI_OP_CONFIGFLAG 0x40
#define EHCI_OP_PORTSC   0x44
#define EHCI_STS_HALTED  0x1000
#define EHCI_PORT_OWNER  0x2000      /* bit 13: 1 = a companion owns it */

/* No paging in this kernel, so a physical MMIO address is directly usable. */
static uint32_t mm_r32(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static void     mm_w32(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
static uint8_t  mm_r8 (uint32_t a) { return *(volatile uint8_t  *)(uintptr_t)a; }

static int      g_ehci_released;
static uint32_t g_ehci_base;

int      uhci_ehci_released(void) { return g_ehci_released; }
uint32_t uhci_ehci_base(void)     { return g_ehci_base; }

static void ehci_release_ports(void)
{
    const pci_dev_t *e = pci_find_class(PCI_CLASS_SERIAL, PCI_SUB_USB,
                                        PCI_PROGIF_EHCI, 0);
    if (!e) return;

    uint32_t mmio = pci_bar_mem(e, 0);
    if (!mmio) {
        klog(KLOG_WARN, "EHCI: present but BAR0 is not a memory range");
        return;
    }
    g_ehci_base = mmio;
    pci_enable(e, 1);

    uint8_t caplen = mm_r8(mmio + EHCI_CAPLENGTH);
    /* The capability registers are at least 12 bytes and the operational
       registers follow them.  A wild value here means the BAR is not really
       an EHCI, and writing at mmio+caplen would scribble on something else. */
    if (caplen < 12 || caplen > 0x40) {
        klog(KLOG_WARN, "EHCI: CAPLENGTH 0x%2x implausible, left alone",
             (uint32_t)caplen);
        return;
    }
    uint32_t hcs  = mm_r32(mmio + EHCI_HCSPARAMS);
    uint32_t hcc  = mm_r32(mmio + EHCI_HCCPARAMS);
    int nports = (int)(hcs & 0x0F);
    if (nports < 1 || nports > 15) nports = 0;

    /* The BIOS may still own the controller.  On a real laptop it does, and
       touching its registers while it does can trap into SMM.  The EHCI
       extended capability at HCCPARAMS[15:8] is the handover protocol:
       claim it, wait for the BIOS to let go, then silence its SMIs. */
    uint8_t eecp = (uint8_t)((hcc >> 8) & 0xFF);
    if (eecp >= 0x40) {
        uint32_t legsup = pci_cfg_read32(e->bus, e->dev, e->fn, eecp);
        if ((legsup & 0xFF) == 0x01) {                 /* USBLEGSUP */
            pci_cfg_write32(e->bus, e->dev, e->fn, eecp,
                            legsup | (1u << 24));      /* OS owned */
            int waited = 0;
            for (; waited < 100; waited++) {
                legsup = pci_cfg_read32(e->bus, e->dev, e->fn, eecp);
                if (!(legsup & (1u << 16))) break;     /* BIOS let go */
                sleep_ms(1);
            }
            /* USBLEGCTLSTS: every SMI enable off, every status bit cleared. */
            pci_cfg_write32(e->bus, e->dev, e->fn, (uint8_t)(eecp + 4),
                            0xE0000000u);
            klog(KLOG_OK, "EHCI: taken from the BIOS after %u ms", (uint32_t)waited);
        }
    }

    uint32_t op = mmio + caplen;

    /* Stop it before changing the routing.  A running EHCI with ports being
       handed away underneath it is not a state the spec describes. */
    mm_w32(op + EHCI_OP_USBCMD, 0);
    for (int i = 0; i < 50; i++) {
        if (mm_r32(op + EHCI_OP_USBSTS) & EHCI_STS_HALTED) break;
        sleep_ms(1);
    }

    /* CONFIGFLAG 0 routes every port to the companions; the Port Owner bits
       do it per port as well, because some parts honour one and not the
       other.  Both are what the hardware does at reset. */
    mm_w32(op + EHCI_OP_CONFIGFLAG, 0);
    for (int i = 0; i < nports; i++) {
        uint32_t pa = op + EHCI_OP_PORTSC + (uint32_t)i * 4;
        mm_w32(pa, mm_r32(pa) | EHCI_PORT_OWNER);
    }

    g_ehci_released = 1;
    klog(KLOG_OK, "EHCI at 0x%x: %u ports handed to the UHCI companions",
         mmio, (uint32_t)nports);

    /* The device has to be seen falling off one controller and re-appearing
       on the other.  It re-attaches at full speed, which takes a moment. */
    sleep_ms(50);
}

/* ── bringing the controller up ───────────────────────────────────────── */

static int detect_ports(uint16_t base)
{
    int n = 0;
    for (int i = 0; i < 8; i++) {
        uint16_t v = inw((uint16_t)(base + UHCI_PORTSC1 + i * 2));
        /* Bit 7 reads as 1 on a port that exists, and an absent port
           answers all-ones.  Both tests are needed: some chipsets return
           zero for a port that is not there, others return 0xFFFF. */
        if (v == 0xFFFF || !(v & PORT_ALWAYS1)) break;
        n++;
    }
    if (n < 2) {
        /* UHCI mandates two.  If probing says otherwise the register
           decode is unusual, not the controller. */
        klog(KLOG_WARN, "UHCI: port probe found %u, assuming 2", (uint32_t)n);
        n = 2;
    }
    return n;
}

/* Bring up one controller: reset it, build its schedule, start it.
   Returns 1 if it is running. */
static int hc_start(const pci_dev_t *d, int slot)
{
    uint16_t base = pci_bar_io(d, 4);
    if (!base) {
        klog(KLOG_WARN, "UHCI at %2x.%u: BAR4 is not an I/O range",
             (uint32_t)d->dev, (uint32_t)d->fn);
        return 0;
    }

    /* Bus mastering is what lets the controller fetch the schedule by DMA.
       Without it everything below succeeds and nothing ever runs. */
    pci_enable(d, 1);

    /* Take the controller off the BIOS before touching it. */
    pci_cfg_write16(d->bus, d->dev, d->fn, UHCI_PCI_LEGSUP, UHCI_PCI_LEGSUP_RWC);

    /* Stop whatever the BIOS left running, then reset. */
    outw((uint16_t)(base + UHCI_USBINTR), 0);
    outw((uint16_t)(base + UHCI_USBCMD),  0);
    outw((uint16_t)(base + UHCI_USBCMD),  CMD_HCRESET);

    int ok = 0;
    for (int i = 0; i < 50; i++) {
        sleep_ms(1);
        if (!(inw((uint16_t)(base + UHCI_USBCMD)) & CMD_HCRESET)) { ok = 1; break; }
    }
    if (!ok) {
        klog(KLOG_ERR, "UHCI at 0x%4x did not come out of reset", (uint32_t)base);
        return 0;
    }

    g_hc[slot].base  = base;
    g_hc[slot].ports = detect_ports(base);
    g_hc[slot].irq   = d->irq;

    /* The schedule.  Every frame points at this controller's control queue,
       which runs into its bulk queue; nothing here needs periodic
       transfers.  Each controller gets its own, because they run
       independently and would otherwise execute each other's descriptors. */
    for (int i = 0; i < 1024; i++)
        frame_list[slot][i] = (uint32_t)(uintptr_t)&qh_control[slot] | LINK_QH;
    qh_control[slot].head    = (uint32_t)(uintptr_t)&qh_bulk[slot] | LINK_QH;
    qh_control[slot].element = LINK_TERM;
    qh_bulk[slot].head       = LINK_TERM;
    qh_bulk[slot].element    = LINK_TERM;

    outw((uint16_t)(base + UHCI_USBINTR),    0);          /* polled, not IRQ */
    outw((uint16_t)(base + UHCI_FRNUM),      0);
    outl((uint16_t)(base + UHCI_FRBASEADD),  (uint32_t)(uintptr_t)frame_list[slot]);
    outb((uint16_t)(base + UHCI_SOFMOD),     0x40);       /* 1 ms frames */
    outw((uint16_t)(base + UHCI_USBSTS),     STS_ALL);    /* clear, w1c */
    outw((uint16_t)(base + UHCI_USBCMD),     CMD_RS | CMD_CF | CMD_MAXP);

    klog(KLOG_OK, "UHCI hc%u at 0x%4x, IRQ %u, %u ports",
         (uint32_t)slot, (uint32_t)base, (uint32_t)d->irq,
         (uint32_t)g_hc[slot].ports);
    return 1;
}

int uhci_init(void)
{
    memset(g_hc, 0, sizeof(g_hc));
    g_nhc = 0; g_nusbdev = 0; g_next_addr = 1;
    g_other_found = 0; g_other_progif = 0;
    g_ehci_released = 0; g_ehci_base = 0;
    memset(toggle_bits, 0, sizeof(toggle_bits));

    if (!pci_find_class(PCI_CLASS_SERIAL, PCI_SUB_USB, PCI_PROGIF_UHCI, 0)) {
        /* Say what IS there.  A machine newer than about 2009 has EHCI or
           xHCI and no UHCI at all, and "no USB controller" would be a lie
           that sends anyone debugging it to the wrong place. */
        for (int i = 0; i < pci_count(); i++) {
            const pci_dev_t *o = pci_get(i);
            if (o->class != PCI_CLASS_SERIAL || o->subclass != PCI_SUB_USB) continue;
            g_other_found  = 1;
            g_other_progif = o->progif;
            klog(KLOG_WARN, "USB: no UHCI; a %s controller is here (progif 0x%2x)",
                 o->progif == PCI_PROGIF_EHCI ? "EHCI" :
                 o->progif == PCI_PROGIF_OHCI ? "OHCI" :
                 o->progif == PCI_PROGIF_XHCI ? "xHCI" : "USB",
                 (uint32_t)o->progif);
            break;
        }
        return 0;
    }

    /* There is at least one UHCI here, so this kernel can use the bus.  If
       the UHCIs are companions to an EHCI, the EHCI holds the sockets and a
       USB 2.0 device would attach there, out of reach -- so ask for them
       back before bringing the companions up. */
    ehci_release_ports();

    for (int nth = 0; nth < MAX_HC; nth++) {
        const pci_dev_t *d = pci_find_class(PCI_CLASS_SERIAL, PCI_SUB_USB,
                                            PCI_PROGIF_UHCI, nth);
        if (!d) break;
        if (hc_start(d, g_nhc)) g_nhc++;
    }

    if (!g_nhc) { klog(KLOG_ERR, "UHCI: no controller could be started"); return 0; }

    /* The frame counter advancing is the proof a controller is actually
       executing its schedule, not merely accepting register writes. */
    uint16_t f0 = uhci_frnum(0);
    sleep_ms(20);
    if (uhci_frnum(0) == f0)
        klog(KLOG_ERR, "UHCI: frame counter not advancing, schedule is not running");
    else
        klog(KLOG_OK, "UHCI: %u controller(s) running", (uint32_t)g_nhc);
    return 1;
}
