/* The UHCI host controller, against a simulated controller and simulated
 * devices, with no VM involved.
 *
 * A host controller driver is mostly a memory format.  The nine words of a
 * Transfer Descriptor and a Queue Head have to be laid out exactly, and a
 * controller told to run a malformed schedule does not complain -- it does
 * nothing at all, or it moves the wrong bytes.  The classic faults all look
 * identical from the outside:
 *
 *   - the length field holds length-1, and all-ones means ZERO bytes, so
 *     reading it as a count makes every transfer look one byte short and a
 *     zero-length packet look like 2048 bytes;
 *   - the data toggle belongs to the ENDPOINT and persists across transfers,
 *     so a driver that restarts it at DATA0 each time has every second
 *     packet silently discarded by the device;
 *   - without the depth-first bit a chain runs one descriptor per frame, so
 *     everything works and everything is twenty times too slow;
 *   - a control transfer whose IN stage ends short must still run its status
 *     stage, or the device stays mid-transfer and it is the NEXT request
 *     that fails;
 *   - a machine has more than one UHCI controller, and a driver that stops
 *     at the first finds a device only if it was plugged into the right
 *     socket.
 *
 * So the simulation below is not a stub that returns success.  It runs TWO
 * controllers, each walking the frame list the driver built for it,
 * following the queue heads, decoding each TD's token, and answering as a
 * device would -- including checking the data toggle on every packet.  If
 * the driver gets the format wrong, nothing moves, exactly as on hardware.
 *
 * It also simulates an EHCI that OWNS the sockets, which is what a USB 2.0
 * controller really is: an EHCI with UHCI companions sharing its ports.  A
 * device there is invisible to this driver until the EHCI is asked for its
 * ports back, so the handoff is a tested path and not a hopeful one.
 *
 * io.h is replaced by defining IO_H, and get_ticks()/sleep_ms() are supplied
 * here as a virtual clock that also advances the controllers by one frame
 * per millisecond.  The driver is compiled unmodified.
 *
 * What this CANNOT check, so that a green run is not read as more than it
 * is: anything whose correctness is a duration.  The simulated port resets
 * instantly and the simulated device adopts its address instantly, so
 * dropping the 50 ms reset hold or the 2 ms address recovery still passes
 * here and fails on real hardware.  Those are checked in QEMU against the
 * real controller, and the delays are written with the spec's figure beside
 * them for that reason.
 */
#include <stdio.h>
#include <stdint.h>

/* ── replace the port I/O ─────────────────────────────────────────────── */
#define IO_H
static inline void io_wait(void) {}
static void     outb(uint16_t p, uint8_t v);
static void     outw(uint16_t p, uint16_t v);
static void     outl(uint16_t p, uint32_t v);
static uint8_t  inb(uint16_t p);
static uint16_t inw(uint16_t p);
static uint32_t inl(uint16_t p);

/* ── guest memory is just this process's memory ───────────────────────── */
static uint32_t rd32(uint32_t a)            { return *(volatile uint32_t *)(uintptr_t)a; }
static void     wr32(uint32_t a, uint32_t v){ *(volatile uint32_t *)(uintptr_t)a = v; }
static uint8_t *ptr8(uint32_t a)            { return (uint8_t *)(uintptr_t)a; }

/* ── PCI config space ─────────────────────────────────────────────────── */
/* 256 bytes, not 64: the EHCI's legacy-support capability lives at 0x60 and
   the BIOS handover is negotiated there. */
typedef struct { uint8_t bus, dev, fn; uint32_t cfg[64]; } fake_fn_t;

static void mk(fake_fn_t *f, uint8_t bus, uint8_t dev, uint8_t fn,
               uint16_t ven, uint16_t did, uint8_t cls, uint8_t sub,
               uint8_t progif, uint8_t hdrtype, uint8_t irq)
{
    for (unsigned i = 0; i < sizeof(*f); i++) ((uint8_t *)f)[i] = 0;
    f->bus = bus; f->dev = dev; f->fn = fn;
    f->cfg[0]  = ((uint32_t)did << 16) | ven;
    f->cfg[1]  = 0x02000000u;
    f->cfg[2]  = ((uint32_t)cls << 24) | ((uint32_t)sub << 16)
               | ((uint32_t)progif << 8);
    f->cfg[3]  = ((uint32_t)hdrtype << 16);
    f->cfg[15] = irq;
}

static fake_fn_t bus_devs[10];
static int       nbus_devs;
static uint32_t  cur_addr;
static uint32_t  legsup_written[4];      /* UHCI 0xC0, per controller */
static int       nlegsup;

/* ── two UHCI controllers and one EHCI ────────────────────────────────── */
#define NHC        2
#define UHCI_IO0   0xC040
#define UHCI_IO1   0xC060
#define PORTS_PER  2

static uint16_t hc_io(int hc) { return hc ? UHCI_IO1 : UHCI_IO0; }

/* The EHCI's register window.  A real buffer, so the driver's writes land
   where the checks can read them -- and -no-pie keeps its address inside 32
   bits, which is what the driver puts in a BAR. */
static uint8_t ehci_mmio[0x200] __attribute__((aligned(4096)));
#define EHCI_CAPLEN      0x20
#define EHCI_OP          EHCI_CAPLEN
#define EHCI_NPORTS      4
#define EHCI_EECP        0x60
#define E_OP_USBCMD      (EHCI_OP + 0x00)
#define E_OP_USBSTS      (EHCI_OP + 0x04)
#define E_OP_CONFIGFLAG  (EHCI_OP + 0x40)
#define E_OP_PORTSC      (EHCI_OP + 0x44)

static int ehci_present_in_bus;          /* is one on the simulated bus    */
static int ehci_halts = 1;               /* 0 models one that never halts  */

static uint32_t ehci_addr(uint32_t off) {
    return (uint32_t)(uintptr_t)ehci_mmio + off;
}
static void ehci_reset_regs(void)
{
    for (unsigned i = 0; i < sizeof(ehci_mmio); i++) ehci_mmio[i] = 0;
    ehci_mmio[0] = EHCI_CAPLEN;                      /* CAPLENGTH          */
    wr32(ehci_addr(0x04), EHCI_NPORTS);              /* HCSPARAMS: N_PORTS */
    wr32(ehci_addr(0x08), (uint32_t)EHCI_EECP << 8); /* HCCPARAMS: EECP    */
    wr32(ehci_addr(E_OP_USBCMD), 0x00000001);        /* running            */
    wr32(ehci_addr(E_OP_USBSTS), 0x00000000);        /* not halted         */
    wr32(ehci_addr(E_OP_CONFIGFLAG), 1);             /* EHCI owns the ports */
    for (int i = 0; i < EHCI_NPORTS; i++)
        wr32(ehci_addr(E_OP_PORTSC + i * 4), 0x00001000);  /* powered      */
}

/* Has the EHCI let go of the sockets?  Either the configure flag is clear --
   which is the state the hardware resets into -- or this port's owner bit
   points at the companion. */
static int ehci_released(int globalport)
{
    if (!ehci_present_in_bus) return 1;              /* nothing owns them  */
    if (rd32(ehci_addr(E_OP_CONFIGFLAG)) == 0) return 1;
    if (globalport < EHCI_NPORTS
        && (rd32(ehci_addr(E_OP_PORTSC + globalport * 4)) & 0x2000)) return 1;
    return 0;
}

static void build_bus(int with_uhci, int with_ehci, int nuhci)
{
    nbus_devs = 0; nlegsup = 0;
    ehci_present_in_bus = with_ehci;
    for (int i = 0; i < 4; i++) legsup_written[i] = 0xFFFFFFFFu;
    fake_fn_t *f;

    f = &bus_devs[nbus_devs++];
    mk(f, 0, 0, 0, 0x8086, 0x1237, 0x06, 0x00, 0x00, 0x00, 0xFF);
    f = &bus_devs[nbus_devs++];
    mk(f, 0, 1, 0, 0x8086, 0x7000, 0x06, 0x01, 0x00, 0x80, 0xFF);
    f = &bus_devs[nbus_devs++];
    mk(f, 0, 1, 1, 0x8086, 0x7010, 0x01, 0x01, 0x80, 0x00, 0xFF);
    f->cfg[4] = 0x0000C001;

    if (with_ehci) {
        /* What UTM builds for "USB Support: USB 2.0", and what an ICH is:
           an EHCI that owns the sockets, with UHCI companions beside it. */
        f = &bus_devs[nbus_devs++];
        mk(f, 0, 4, 0, 0x8086, 0x293A, 0x0C, 0x03, 0x20, 0x00, 11);
        f->cfg[4] = (uint32_t)(uintptr_t)ehci_mmio;      /* BAR0, memory */
        /* USB legacy support capability: BIOS owns it until asked. */
        f->cfg[EHCI_EECP / 4]       = 0x00010001u | (1u << 16);
        f->cfg[(EHCI_EECP + 4) / 4] = 0x0000FFFFu;       /* SMIs enabled */
        ehci_reset_regs();
    }
    if (with_uhci) {
        for (int i = 0; i < nuhci; i++) {
            f = &bus_devs[nbus_devs++];
            mk(f, 0, (uint8_t)(5 + i), 0, 0x8086, (uint16_t)(0x2934 + i),
               0x0C, 0x03, 0x00, 0x00, 11);
            f->cfg[8] = hc_io(i) | 1;                    /* BAR4, I/O */
        }
    }
}

static fake_fn_t *lookup(uint8_t bus, uint8_t dev, uint8_t fn)
{
    for (int i = 0; i < nbus_devs; i++)
        if (bus_devs[i].bus == bus && bus_devs[i].dev == dev
            && bus_devs[i].fn == fn) return &bus_devs[i];
    return 0;
}

/* ── the simulated UHCI register files ────────────────────────────────── */
#define R_CMD   0x00
#define R_STS   0x02
#define R_INTR  0x04
#define R_FRNUM 0x06
#define R_FRBA  0x08
#define R_SOF   0x0C
#define R_PORT  0x10

static uint16_t r_cmd[NHC], r_sts[NHC], r_intr[NHC], r_frnum[NHC];
static uint16_t r_port[NHC][PORTS_PER];
static uint8_t  r_sof[NHC];
static uint32_t r_frba[NHC];
static int      sim_hcreset_sticks;
static int      sim_frames;
static int      sim_no_framelist;

/* ── the simulated devices ────────────────────────────────────────────── */
#define EP_BULK_IN  1
#define EP_BULK_OUT 2
#define NSIM (NHC * PORTS_PER)

typedef struct {
    int     attached, lowspeed;
    int     behind_ehci;           /* invisible until the ports are handed over */
    uint8_t addr;
    uint8_t desc[18];

    uint8_t setup[8];
    int     have_setup, data_done;
    int     stall_everything;
    int     deaf;
    int     nak_left;
    int     short_by;

    uint8_t in_data[1024];
    int     in_len, in_pos;
    uint8_t out_data[1024];
    int     out_len;

    int     exp_tog[16][2];
    int     tog_errors;
    int     setup_count, in_count, out_count;
} sim_dev_t;

static sim_dev_t sim[NSIM];

static int sim_index(int hc, int port) { return hc * PORTS_PER + port; }

static void dev_reset_eps(sim_dev_t *d)
{
    for (int e = 0; e < 16; e++) { d->exp_tog[e][0] = 0; d->exp_tog[e][1] = 0; }
}

static void sim_attach(int hc, int port, int lowspeed, uint16_t ven,
                       uint16_t prod, uint8_t mps0, int behind_ehci)
{
    sim_dev_t *d = &sim[sim_index(hc, port)];
    for (unsigned i = 0; i < sizeof(*d); i++) ((uint8_t *)d)[i] = 0;
    d->attached = 1;
    d->lowspeed = lowspeed;
    d->behind_ehci = behind_ehci;
    d->addr = 0;
    uint8_t *x = d->desc;
    x[0] = 18; x[1] = 1; x[2] = 0x10; x[3] = 0x01;
    x[4] = 0x00; x[5] = 0x00; x[6] = 0x00;
    x[7] = mps0;
    x[8]  = (uint8_t)(ven  & 0xFF); x[9]  = (uint8_t)(ven  >> 8);
    x[10] = (uint8_t)(prod & 0xFF); x[11] = (uint8_t)(prod >> 8);
    x[12] = 0x00; x[13] = 0x01;
    x[14] = 1; x[15] = 2; x[16] = 3; x[17] = 1;
    dev_reset_eps(d);
}

/* A device is on this UHCI port only if it is attached AND, when it sits
   behind an EHCI, that EHCI has released the socket. */
static int port_has_device(int hc, int port)
{
    sim_dev_t *d = &sim[sim_index(hc, port)];
    if (!d->attached) return 0;
    if (d->behind_ehci && !ehci_released(sim_index(hc, port))) return 0;
    return 1;
}

/* ── TD / QH field layout ─────────────────────────────────────────────── */
#define TD_LINK 0
#define TD_CTRL 4
#define TD_TOK  8
#define TD_BUF  12
#define QH_HEAD 0
#define QH_ELEM 4

#define T_ACTIVE (1u<<23)
#define T_STALL  (1u<<22)
#define T_SPD    (1u<<29)
#define T_LS     (1u<<26)
#define T_CRCTO  (1u<<18)
#define T_NAK    (1u<<19)
#define T_ACTLEN 0x7FFu

#define P_OUT   0xE1
#define P_IN    0x69
#define P_SETUP 0x2D

static void td_finish(uint32_t td, uint32_t ctrl, int actual, uint32_t errbits)
{
    /* The controller rewrites the whole status field on every attempt, so a
       packet that NAKed and then succeeded ends up with the NAK bit clear.
       Keeping it set here would make the suite pass a driver that treats
       NAK as a failure, which is wrong on hardware that NAKs constantly. */
    ctrl &= ~(T_ACTIVE | T_ACTLEN | T_NAK);
    ctrl |= errbits;
    ctrl |= (actual > 0) ? (uint32_t)(actual - 1) : T_ACTLEN;
    wr32(td + TD_CTRL, ctrl);
}

/* Returns 0 to advance the queue, 1 to stop it, 2 to leave the TD active. */
static int exec_td(int hc, uint32_t td)
{
    uint32_t ctrl = rd32(td + TD_CTRL);
    uint32_t tok  = rd32(td + TD_TOK);
    uint32_t buf  = rd32(td + TD_BUF);
    if (!(ctrl & T_ACTIVE)) return 1;

    uint8_t  pid    = (uint8_t)(tok & 0xFF);
    uint8_t  addr   = (uint8_t)((tok >> 8) & 0x7F);
    uint8_t  ep     = (uint8_t)((tok >> 15) & 0x0F);
    int      toggle = (int)((tok >> 19) & 1);
    uint32_t ml     = (tok >> 21) & 0x7FF;
    int      maxlen = (ml == 0x7FF) ? 0 : (int)ml + 1;

    /* Only devices on THIS controller's enabled ports can answer: each
       controller drives its own sockets, and a driver that mixed them up
       would appear to work on a machine with exactly one. */
    sim_dev_t *d = 0;
    for (int p = 0; p < PORTS_PER; p++) {
        int i = sim_index(hc, p);
        if (!port_has_device(hc, p)) continue;
        if (!(r_port[hc][p] & 0x0004)) continue;        /* port not enabled */
        if (sim[i].addr != addr) continue;
        d = &sim[i];
        break;
    }
    if (!d || d->deaf) return 2;                        /* nobody answers */

    if (d->lowspeed && !(ctrl & T_LS)) {
        td_finish(td, ctrl, 0, T_CRCTO);
        return 1;
    }

    if (pid == P_SETUP) {
        if (ep != 0 || maxlen != 8) { td_finish(td, ctrl, 0, T_STALL); return 1; }
        if (toggle != 0) d->tog_errors++;               /* SETUP is DATA0 */
        for (int i = 0; i < 8; i++) d->setup[i] = ptr8(buf)[i];
        d->have_setup = 1;
        d->data_done  = 0;
        d->setup_count++;
        d->exp_tog[0][0] = 1; d->exp_tog[0][1] = 1;     /* data stage: DATA1 */
        td_finish(td, ctrl, 8, 0);
        return 0;
    }

    /* A busy device NAKs: the controller leaves the TD active and tries
       again next frame, and the toggle does not advance.  Checked before the
       toggle bookkeeping or the retry looks like a toggle error. */
    if (d->nak_left > 0) {
        d->nak_left--;
        wr32(td + TD_CTRL, ctrl | T_NAK);               /* NAK, still ACTIVE */
        return 2;
    }

    int in = (pid == P_IN);
    if (toggle != d->exp_tog[ep][in ? 1 : 0]) d->tog_errors++;
    d->exp_tog[ep][in ? 1 : 0] ^= 1;

    if (ep == 0) {
        if (!d->have_setup) { td_finish(td, ctrl, 0, T_STALL); return 1; }
        uint8_t  rtype = d->setup[0], req = d->setup[1];
        uint16_t val   = (uint16_t)(d->setup[2] | (d->setup[3] << 8));
        uint16_t wlen  = (uint16_t)(d->setup[6] | (d->setup[7] << 8));
        int host_in    = (rtype & 0x80) != 0;

        if (maxlen == 0) {                              /* the status stage */
            if (toggle != 1) d->tog_errors++;
            if (in == host_in) { td_finish(td, ctrl, 0, T_STALL); return 1; }
            /* SET_ADDRESS has no data stage, so the new address arrives in
               the SETUP packet and takes effect when the status stage
               completes -- not a moment earlier, which is why the driver
               must keep using address 0 right up to here. */
            if (req == 5 && !d->stall_everything) d->addr = (uint8_t)val;
            d->have_setup = 0;
            td_finish(td, ctrl, 0, 0);
            return 0;
        }
        if (d->stall_everything) { td_finish(td, ctrl, 0, T_STALL); return 1; }

        if (host_in && req == 6 && (val >> 8) == 1) {   /* GET_DESCRIPTOR */
            int want = wlen > 18 ? 18 : wlen;
            if (d->short_by && want - d->short_by > 0) want -= d->short_by;
            int left = want - d->data_done;
            if (left < 0) left = 0;
            int give = left > maxlen ? maxlen : left;
            for (int i = 0; i < give; i++) ptr8(buf)[i] = d->desc[d->data_done + i];
            d->data_done += give;
            d->in_count++;
            td_finish(td, ctrl, give, 0);
            if (give < maxlen && (ctrl & T_SPD)) return 1;
            return 0;
        }
        td_finish(td, ctrl, 0, T_STALL);
        return 1;
    }

    if (in) {
        int left = d->in_len - d->in_pos;
        if (left < 0) left = 0;
        int give = left > maxlen ? maxlen : left;
        for (int i = 0; i < give; i++) ptr8(buf)[i] = d->in_data[d->in_pos + i];
        d->in_pos += give;
        d->in_count++;
        td_finish(td, ctrl, give, 0);
        if (give < maxlen && (ctrl & T_SPD)) return 1;
        return 0;
    }
    for (int i = 0; i < maxlen && d->out_len < (int)sizeof(d->out_data); i++)
        d->out_data[d->out_len++] = ptr8(buf)[i];
    d->out_count++;
    td_finish(td, ctrl, maxlen, 0);
    return 0;
}

static void run_queue(int hc, uint32_t qh)
{
    for (int guard = 0; guard < 256; guard++) {
        uint32_t el = rd32(qh + QH_ELEM);
        if (el & 1) return;                       /* queue empty */
        if (el & 2) return;                       /* QH inside QH: unused */
        uint32_t td = el & ~0xFu;
        int r = exec_td(hc, td);
        if (r != 0) return;                       /* stopped, or still busy */
        uint32_t link = rd32(td + TD_LINK);
        wr32(qh + QH_ELEM, link);                 /* advance the queue */
        if (link & 1) return;                     /* chain drained */
        if (!(link & 4)) return;                  /* no depth-first */
    }
}

static void hc_frame(int hc)
{
    if (!(r_cmd[hc] & 0x0001)) { r_sts[hc] |= 0x0020; return; }
    r_sts[hc] &= (uint16_t)~0x0020;
    sim_frames++;

    if (r_frba[hc] < 0x1000) {
        /* FRBASEADD was never written.  Real hardware would fetch from
           whatever address that is and do nothing useful; say so and let the
           check report it rather than taking the process down. */
        sim_no_framelist = 1;
        r_frnum[hc] = (uint16_t)((r_frnum[hc] + 1) & 0x7FF);
        return;
    }
    uint32_t link = rd32(r_frba[hc] + (uint32_t)(r_frnum[hc] & 1023) * 4);
    for (int guard = 0; guard < 64 && !(link & 1); guard++) {
        if (!(link & 2)) break;                   /* a bare TD: not used */
        uint32_t qh = link & ~0xFu;
        run_queue(hc, qh);
        link = rd32(qh + QH_HEAD);
    }
    r_frnum[hc] = (uint16_t)((r_frnum[hc] + 1) & 0x7FF);
}

/* ── the virtual clock, which is what drives the controllers ──────────── */
static uint32_t v_us;

#define E_OP_USBCMD_  E_OP_USBCMD
#define E_OP_USBSTS_  E_OP_USBSTS

static void advance_us(uint32_t us)
{
    /* A real EHCI raises HCHalted a moment after its run bit is cleared.
       The driver waits for that before changing the port routing, so the
       wait has to be a real one here or the code path is never taken. */
    if (ehci_present_in_bus && ehci_halts
        && !(rd32(ehci_addr(E_OP_USBCMD_)) & 1))
        wr32(ehci_addr(E_OP_USBSTS_), rd32(ehci_addr(E_OP_USBSTS_)) | 0x1000);

    static uint32_t carry;
    carry += us;
    while (carry >= 1000) {
        carry -= 1000;
        for (int hc = 0; hc < NHC; hc++) hc_frame(hc);
    }
    v_us += us;
}

uint32_t get_ticks(void) { advance_us(400); return v_us / 10000; }
void     sleep_ms(uint32_t ms) { advance_us(ms * 1000); }

/* ── port I/O ─────────────────────────────────────────────────────────── */
static int which_hc(uint16_t port)
{
    for (int i = 0; i < NHC; i++)
        if (port >= hc_io(i) && port < hc_io(i) + 0x20) return i;
    return -1;
}

static void outl(uint16_t port, uint32_t val)
{
    if (port == 0xCF8) { cur_addr = val; return; }
    if (port == 0xCFC) {
        uint8_t bus = (cur_addr >> 16) & 0xFF, dev = (cur_addr >> 11) & 0x1F;
        uint8_t fn  = (cur_addr >> 8) & 7,     reg = cur_addr & 0xFC;
        fake_fn_t *f = lookup(bus, dev, fn);
        if (!f) return;
        f->cfg[reg / 4] = val;
        if (reg == 0xC0 && nlegsup < 4) legsup_written[nlegsup++] = val;
        /* The BIOS hands the EHCI over when the OS-owned bit is set: it
           clears its own ownership bit in reply.  A driver that sets the bit
           and never waits would race a real BIOS here. */
        if (reg == EHCI_EECP && (val & (1u << 24)))
            f->cfg[reg / 4] &= ~(1u << 16);
        return;
    }
    int hc = which_hc(port);
    if (hc >= 0 && port == hc_io(hc) + R_FRBA) r_frba[hc] = val;
}

static uint32_t inl(uint16_t port)
{
    if (port == 0xCFC) {
        uint8_t bus = (cur_addr >> 16) & 0xFF, dev = (cur_addr >> 11) & 0x1F;
        uint8_t fn  = (cur_addr >> 8) & 7,     reg = cur_addr & 0xFC;
        fake_fn_t *f = lookup(bus, dev, fn);
        if (!f) return 0xFFFFFFFFu;
        return f->cfg[reg / 4];
    }
    int hc = which_hc(port);
    if (hc >= 0 && port == hc_io(hc) + R_FRBA) return r_frba[hc];
    return 0xFFFFFFFFu;
}

static void hc_present_devices(int hc)
{
    for (int p = 0; p < PORTS_PER; p++) {
        r_port[hc][p] = 0x0080;
        if (port_has_device(hc, p)) {
            r_port[hc][p] |= 0x0081;
            if (sim[sim_index(hc, p)].lowspeed) r_port[hc][p] |= 0x0100;
        }
    }
}

static void outw(uint16_t port, uint16_t val)
{
    int hc = which_hc(port);
    if (hc < 0) return;
    switch ((int)port - hc_io(hc)) {
    case R_CMD:
        if (val & 0x0002) {                     /* HCRESET */
            if (!sim_hcreset_sticks) {
                r_cmd[hc] = 0; r_sts[hc] = 0x0020;
                r_frnum[hc] = 0; r_frba[hc] = 0;
                /* A host controller reset re-presents every attached device:
                   they return to address 0 and ports lose enable. */
                for (int p = 0; p < PORTS_PER; p++) {
                    int i = sim_index(hc, p);
                    if (!sim[i].attached) continue;
                    sim[i].addr = 0; sim[i].have_setup = 0;
                    dev_reset_eps(&sim[i]);
                }
                hc_present_devices(hc);
                return;
            }
            r_cmd[hc] = val;                    /* stays set: the bad path */
            return;
        }
        r_cmd[hc] = val;
        if (val & 0x0001) r_sts[hc] &= (uint16_t)~0x0020;
        return;
    case R_STS:   r_sts[hc] &= (uint16_t)~val; return;   /* write 1 to clear */
    case R_INTR:  r_intr[hc] = val; return;
    case R_FRNUM: r_frnum[hc] = val & 0x7FF; return;
    default: break;
    }
    for (int p = 0; p < PORTS_PER; p++) {
        if (port != hc_io(hc) + R_PORT + p * 2) continue;
        uint16_t cur = r_port[hc][p];
        uint16_t wc  = (uint16_t)(cur & val & 0x000A);   /* CSC|PEC cleared */
        uint16_t nv  = (uint16_t)((cur & ~0x000Au) | 0x0080);
        nv = (uint16_t)(nv & ~(uint16_t)wc);
        nv = (uint16_t)((nv & ~0x0204u) | (val & 0x0204u));
        if ((cur & 0x0200) && !(val & 0x0200)) {
            /* Reset released: the device reverts to address 0 and the
               controller raises connect-change. */
            int i = sim_index(hc, p);
            if (port_has_device(hc, p)) {
                sim[i].addr = 0; sim[i].have_setup = 0;
                dev_reset_eps(&sim[i]);
                nv |= 0x0002;                   /* CSC */
            }
        }
        if ((val & 0x0004) && port_has_device(hc, p)) nv |= 0x0004;
        if (!port_has_device(hc, p)) nv &= (uint16_t)~0x0005u;
        r_port[hc][p] = nv;
        return;
    }
}

static uint16_t inw(uint16_t port)
{
    int hc = which_hc(port);
    if (hc < 0) return 0xFFFF;
    switch ((int)port - hc_io(hc)) {
    case R_CMD:   return r_cmd[hc];
    case R_STS:   return r_sts[hc];
    case R_INTR:  return r_intr[hc];
    case R_FRNUM: return r_frnum[hc];
    default: break;
    }
    for (int p = 0; p < PORTS_PER; p++) {
        if (port != hc_io(hc) + R_PORT + p * 2) continue;
        /* Connection is answered live: a device behind an EHCI appears here
           only once the EHCI has been asked for its ports back. */
        uint16_t v = (uint16_t)(r_port[hc][p] & ~0x0101u);
        v |= 0x0080;
        if (port_has_device(hc, p)) {
            v |= 0x0001;
            if (sim[sim_index(hc, p)].lowspeed) v |= 0x0100;
        } else {
            v &= (uint16_t)~0x0004u;            /* cannot be enabled either */
        }
        return v;
    }
    if (port >= hc_io(hc) + R_PORT && port < hc_io(hc) + R_PORT + 16)
        return 0xFFFF;                          /* ports 2..7 do not exist */
    return 0xFFFF;
}

static void outb(uint16_t port, uint8_t val)
{
    int hc = which_hc(port);
    if (hc >= 0 && port == hc_io(hc) + R_SOF) r_sof[hc] = val;
}
static uint8_t inb(uint16_t port)
{
    int hc = which_hc(port);
    if (hc >= 0 && port == hc_io(hc) + R_SOF) return r_sof[hc];
    return 0xFF;
}

/* ── the driver under test, compiled unmodified ───────────────────────── */
#include "../include/types.h"
#include "../include/vga.h"

/* pci.c and uhci.c report what they find through the kernel log now, so the
   log comes along for the ride.  Its screen output is captured rather than
   drawn, and its clock is the same virtual one that drives the controllers,
   so the timestamps in a test run mean what they would on hardware. */
static char     klog_screen[32768];
static int      klog_screen_len;
void vga_puts(const char *s)
{
    while (*s && klog_screen_len < (int)sizeof(klog_screen) - 1)
        klog_screen[klog_screen_len++] = *s++;
    klog_screen[klog_screen_len] = 0;
}
void vga_set_color(vga_color_t a, vga_color_t b) { (void)a; (void)b; }
uint32_t boot_ms(void) { return v_us / 1000; }

#include "../include/klog.h"
#include "../kernel/klog.c"
#include "../include/pci.h"
#include "../include/uhci.h"
#include "../drivers/pci.c"
#include "../drivers/uhci.c"

/* Does any log line contain this? */
static int klog_mentions(const char *needle)
{
    for (int i = 0; i < klog_count(); i++) {
        const char *h = klog_text(i);
        for (int a = 0; h[a]; a++) {
            int b = 0;
            while (needle[b] && h[a + b] == needle[b]) b++;
            if (!needle[b]) return 1;
        }
    }
    return 0;
}

/* ── checks ───────────────────────────────────────────────────────────── */
static int fails = 0, total = 0;
static void ck(const char *what, int cond, const char *detail)
{
    total++;
    if (cond) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s\n          %s\n", what,
                           detail ? detail : ""); }
}
static void head(const char *t) { printf("\n%s\n", t); }

static void sim_power_on(void)
{
    for (int hc = 0; hc < NHC; hc++) {
        r_cmd[hc] = 0; r_sts[hc] = 0x0020; r_intr[hc] = 0xFFFF;
        r_frnum[hc] = 0; r_frba[hc] = 0; r_sof[hc] = 0;
    }
    sim_frames = 0; v_us = 0; sim_hcreset_sticks = 0; sim_no_framelist = 0;
    if (ehci_present_in_bus) {
        ehci_reset_regs();
        if (!ehci_halts) wr32(ehci_addr(E_OP_USBSTS), 0);
    }
    for (int hc = 0; hc < NHC; hc++) hc_present_devices(hc);
}

static void detach_all(void)
{
    for (int i = 0; i < NSIM; i++) sim[i].attached = 0;
}

static uint8_t buf[512] __attribute__((aligned(16)));

/* The device the first-contact section brought up.  uhci_probe_port() fills
   the caller's struct and does not touch the driver's table -- the table is
   uhci_scan()'s business -- so the sections below carry it themselves. */
static uhci_dev_t dev0;

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);   /* so a crash does not swallow the
                                           checks that already passed */
    klog_set_echo(0);                   /* the drivers' log lines are
                                           inspected, not printed */
    printf("UHCI host controller\n");
    printf("====================\n");

    /* ── the controller comes up ──────────────────────────────────────── */
    build_bus(1, 0, 1);
    detach_all();
    sim_attach(0, 0, 0, 0x0781, 0x5567, 8, 0);  /* a full-speed stick */
    sim_power_on();
    pci_init();
    int up = uhci_init();

    head("it finds the controller PCI enumeration made visible");
    ck("uhci_init reports a running controller", up == 1, "");
    ck("one controller on this bus", uhci_hc_count() == 1, "");
    ck("at the address in BAR4", uhci_io_base(0) == UHCI_IO0, "");
    ck("two root ports", uhci_port_count(0) == 2, "");
    ck("bus mastering is on, or it cannot fetch the schedule by DMA",
       (pci_cfg_read16(0, 5, 0, 0x04) & 0x0004) != 0,
       "a controller without bus master accepts every register write and "
       "then does nothing at all");
    ck("the BIOS legacy trap was disabled", legsup_written[0] == 0x8F00,
       "USB_LEGSUP at PCI 0xC0 must be written or the BIOS keeps "
       "intercepting the controller for PS/2 emulation");

    head("the schedule is the shape the hardware expects");
    ck("the frame list address reached the controller", r_frba[0] != 0,
       "FRBASEADD is the only thing that tells the controller where the "
       "schedule is; without it the controller runs and walks nothing");
    ck("and the controller never ran without a schedule", !sim_no_framelist, "");
    ck("the frame list address is 4096-byte aligned", (r_frba[0] & 0xFFF) == 0,
       "FRBASEADD ignores the low 12 bits: a misaligned frame list makes "
       "the controller walk the wrong memory");
    {
        int all = (r_frba[0] >= 0x1000), qbit = all;
        if (all) {
            uint32_t first = rd32(r_frba[0]);
            for (int i = 0; i < 1024; i++) {
                uint32_t e = rd32(r_frba[0] + i * 4);
                if (e != first) all = 0;
                if (!(e & 2))   qbit = 0;
            }
        }
        ck("all 1024 frames point at the same queue head", all, "");
        ck("and the entry is marked as a queue head, not a TD", qbit,
           "the Q bit tells the controller how to interpret the pointer");
    }
    ck("the controller is running", (uhci_usbcmd(0) & 0x0001) != 0, "");
    ck("configure flag set", (uhci_usbcmd(0) & 0x0040) != 0, "");
    ck("it is not halted", (uhci_usbsts(0) & 0x0020) == 0, "");
    ck("interrupts are left disabled: this driver polls", r_intr[0] == 0,
       "the house rule is that disk I/O never depends on an IRQ");
    ck("SOF timing is the 1 ms default", r_sof[0] == 0x40, "");
    {
        uint16_t f0 = uhci_frnum(0);
        sleep_ms(20);
        ck("the frame counter advances", uhci_frnum(0) != f0,
           "a controller that accepts register writes but never runs the "
           "schedule looks identical until you watch this");
    }

    /* ── first contact ────────────────────────────────────────────────── */
    head("a device is reset, addressed and read");
    {
        uhci_dev_t d;
        int r = uhci_probe_port(0, 0, &d);
        ck("the probe succeeds", r == UHCI_OK, "");
        dev0 = d;
        ck("the port ended up enabled", (uhci_portsc(0, 0) & 0x0004) != 0, "");
        ck("the device descriptor came back", d.vendor == 0x0781
           && d.product == 0x5567, "");
        ck("endpoint 0 packet size was read from the descriptor",
           d.maxpacket0 == 8, "");
        ck("it is full speed", d.lowspeed == 0, "");
        ck("an address was assigned", d.addr == 1, "");
        ck("and the device agrees it has that address", sim[0].addr == 1, "");
        ck("the data toggle was correct on every packet",
           sim[0].tog_errors == 0,
           "SETUP is DATA0, the data stage starts at DATA1 and alternates, "
           "the status stage is DATA1");
        ck("the 18-byte descriptor needed three 8-byte packets",
           sim[0].in_count >= 3,
           "an endpoint-0 packet size of 8 means the driver must split the "
           "read, not ask for 18 bytes in one TD");
    }

    head("a whole transfer runs inside one frame");
    {
        int before = sim_frames;
        uhci_dev_t d = dev0;
        int r = uhci_control(&d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)(USB_DESC_DEVICE << 8), 0, buf, 18);
        int frames = sim_frames - before;
        ck("18 bytes read back", r == 18, "");
        ck("in a single frame, not one per descriptor", frames <= 2,
           "without the Vf (depth first) bit in the link pointers the "
           "controller runs one TD per millisecond");
    }

    head("a transfer reports the bytes it moved, and no others");
    {
        uhci_dev_t d = dev0;
        int r = uhci_control(&d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)(USB_DESC_DEVICE << 8), 0, buf, 8);
        ck("an 8-byte read reports exactly 8 bytes", r == 8,
           "the SETUP and status descriptors carry bytes of their own and "
           "must not be counted into the caller's total");
    }

    head("a device that refuses a request does not wedge the bus");
    {
        uhci_dev_t d = dev0;
        int r = uhci_control(&d, USB_DIR_IN, 0x63 /* no such request */,
                             0, 0, buf, 8);
        ck("an unsupported request comes back as a stall", r == UHCI_ESTALL, "");
        int r2 = uhci_control(&d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                              (uint16_t)(USB_DESC_DEVICE << 8), 0, buf, 18);
        ck("and the very next request still works", r2 == 18,
           "a stalled chain must be unlinked from the queue head, or it "
           "sits there and every later transfer stalls behind it");
    }

    head("a short answer still completes the transfer");
    {
        sim[0].short_by = 6;
        sim[0].tog_errors = 0;
        uhci_dev_t d = dev0;
        int r = uhci_control(&d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)(USB_DESC_DEVICE << 8), 0, buf, 18);
        ck("the short length is reported, not an error", r == 12, "");
        ck("the status stage still ran", sim[0].have_setup == 0,
           "a short IN stops the queue before the status stage; it has to "
           "be run separately or the device stays mid-transfer and it is "
           "the NEXT request that fails");
        sim[0].short_by = 0;
        int r2 = uhci_control(&d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                              (uint16_t)(USB_DESC_DEVICE << 8), 0, buf, 18);
        ck("and the next request is unaffected", r2 == 18, "");
    }

    head("bulk transfers, and the toggle that persists between them");
    {
        uhci_dev_t d = dev0;
        for (int i = 0; i < 160; i++) sim[0].in_data[i] = (uint8_t)(i ^ 0x5A);
        sim[0].in_len = 160; sim[0].in_pos = 0;
        sim[0].tog_errors = 0;

        int r = uhci_bulk(&d, EP_BULK_IN, 1, buf, 64, 64);
        ck("a 64-byte bulk read moves 64 bytes", r == 64, "");
        int same = 1;
        for (int i = 0; i < 64; i++) if (buf[i] != (uint8_t)(i ^ 0x5A)) same = 0;
        ck("with the right bytes", same, "");

        r = uhci_bulk(&d, EP_BULK_IN, 1, buf, 64, 64);
        ck("a second read continues where the first stopped", r == 64, "");
        same = 1;
        for (int i = 0; i < 64; i++)
            if (buf[i] != (uint8_t)((i + 64) ^ 0x5A)) same = 0;
        ck("with the right bytes again", same, "");
        ck("the toggle carried across the two transfers",
           sim[0].tog_errors == 0,
           "the data toggle is endpoint state, not transfer state: restart "
           "it at DATA0 each time and the device discards every other "
           "packet without reporting anything");

        r = uhci_bulk(&d, EP_BULK_IN, 1, buf, 64, 64);
        ck("a read past the end returns the short remainder", r == 32, "");

        /* A device with nothing left answers with a zero-length packet, and
           a TD that moved zero bytes records its length field as all-ones.
           All-ones means ZERO, not 2047 and not 2048; reading it as a count
           is the single most common UHCI mistake and it turns an empty read
           into a claim that two kilobytes arrived. */
        r = uhci_bulk(&d, EP_BULK_IN, 1, buf, 64, 64);
        ck("an empty read reports zero bytes, not 2048", r == 0,
           "the all-ones actual-length field means zero bytes moved");

        /* Three descriptors built, two run: an ODD number left over, so a
           missing rewind actually changes the toggle rather than cancelling
           itself out. */
        for (int i = 0; i < 100; i++) sim[0].in_data[i] = (uint8_t)(i + 0x11);
        sim[0].in_len = 100; sim[0].in_pos = 0;
        sim[0].tog_errors = 0;
        uhci_bulk_reset_toggle(&d, EP_BULK_IN, 1);
        dev_reset_eps(&sim[0]);
        r = uhci_bulk(&d, EP_BULK_IN, 1, buf, 192, 64);
        ck("asking for 192 bytes of a 100-byte reply returns 100", r == 100, "");
        sim[0].in_len = 164;                  /* 64 more become available */
        r = uhci_bulk(&d, EP_BULK_IN, 1, buf, 64, 64);
        ck("the next read on that endpoint still works", r == 64, "");
        ck("because the unused descriptors gave their toggles back",
           sim[0].tog_errors == 0,
           "a chain of three descriptors that stopped after two advanced "
           "the toggle three times; the endpoint only advanced it twice");

        sim[0].out_len = 0;
        sim[0].tog_errors = 0;
        for (int i = 0; i < 100; i++) buf[i] = (uint8_t)(0xC3 ^ i);
        r = uhci_bulk(&d, EP_BULK_OUT, 0, buf, 100, 64);
        ck("a 100-byte bulk write is split into 64 + 36", r == 100
           && sim[0].out_count == 2, "");
        same = 1;
        for (int i = 0; i < 100; i++)
            if (sim[0].out_data[i] != (uint8_t)(0xC3 ^ i)) same = 0;
        ck("and the device received exactly those bytes", same, "");
        ck("the write toggles were right too", sim[0].tog_errors == 0, "");
    }

    head("a transfer longer than the descriptor pool is split, not refused");
    {
        uhci_dev_t d = dev0;
        for (int i = 0; i < 1024; i++) sim[0].in_data[i] = (uint8_t)(i * 7);
        sim[0].in_len = 1024; sim[0].in_pos = 0;
        sim[0].tog_errors = 0;
        uhci_bulk_reset_toggle(&d, EP_BULK_IN, 1);
        dev_reset_eps(&sim[0]);
        static uint8_t big[1024] __attribute__((aligned(16)));
        int r = uhci_bulk(&d, EP_BULK_IN, 1, big, 1024, 64);
        ck("all 1024 bytes arrive", r == 1024, "");
        int same = 1;
        for (int i = 0; i < 1024; i++)
            if (big[i] != (uint8_t)(i * 7)) same = 0;
        ck("intact across the split", same, "");
        ck("and the toggle survived the split", sim[0].tog_errors == 0, "");
    }

    head("a device that says 'not yet' is waited for, not failed");
    {
        uhci_dev_t d = dev0;
        for (int i = 0; i < 64; i++) sim[0].in_data[i] = (uint8_t)(i + 1);
        sim[0].in_len = 64; sim[0].in_pos = 0;
        sim[0].tog_errors = 0;
        uhci_bulk_reset_toggle(&d, EP_BULK_IN, 1);
        dev_reset_eps(&sim[0]);
        sim[0].nak_left = 4;                /* busy for four frames */
        int r = uhci_bulk(&d, EP_BULK_IN, 1, buf, 64, 64);
        ck("the transfer still completes", r == 64,
           "NAK means the device is busy, not that anything went wrong: it "
           "is the one status bit that must NOT be treated as an error");
        ck("and the toggle did not advance over the NAKs",
           sim[0].tog_errors == 0, "");
        sim[0].nak_left = 0;
    }

    head("a device that never answers times out instead of hanging");
    {
        uhci_dev_t d = dev0;
        sim[0].deaf = 1;
        int r = uhci_control(&d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)(USB_DESC_DEVICE << 8), 0, buf, 8);
        ck("the transfer gives up", r == UHCI_ETIMEOUT, "");
        sim[0].deaf = 0;
        dev_reset_eps(&sim[0]);
        sim[0].have_setup = 0;
        int r2 = uhci_control(&d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                              (uint16_t)(USB_DESC_DEVICE << 8), 0, buf, 18);
        ck("and the controller is still usable afterwards", r2 == 18,
           "the timed-out chain has to be unlinked from the queue head");
    }

    head("re-initialising starts the toggles over");
    {
        sim_power_on();
        pci_init();
        ck("the controller comes back up", uhci_init() == 1, "");
        int found = uhci_scan();
        ck("and finds the device again", found == 1, "");
        if (found == 1) {
            uhci_dev_t d = *uhci_device(0);
            for (int i = 0; i < 64; i++) sim[0].in_data[i] = (uint8_t)(i * 3);
            sim[0].in_len = 64; sim[0].in_pos = 0;
            sim[0].tog_errors = 0;
            int r = uhci_bulk(&d, EP_BULK_IN, 1, buf, 64, 64);
            ck("the first transfer after init is accepted", r == 64, "");
            ck("because the toggle started at DATA0", sim[0].tog_errors == 0,
               "a device that has just been reset and re-addressed is back "
               "at DATA0 on every endpoint; a driver that kept the toggles "
               "from before the reset has its first packet discarded");
        }
    }

    /* ── more than one controller ─────────────────────────────────────── */
    head("every controller is driven, not just the first");
    {
        /* A PIIX3 has one UHCI.  Almost nothing else does: an ICH has two to
           five, and a USB 2.0 controller is an EHCI with three UHCI
           companions sharing its sockets two ports at a time.  A driver that
           stops at the first finds a device only when it happens to be in
           the right pair of sockets, and is silently blind otherwise. */
        build_bus(1, 0, 2);
        detach_all();
        sim_attach(1, 1, 0, 0x090C, 0x1000, 64, 0);   /* the LAST socket */
        sim_power_on();
        pci_init();
        ck("init succeeds", uhci_init() == 1, "");
        ck("both controllers were started", uhci_hc_count() == 2, "");
        ck("each at its own I/O base",
           uhci_io_base(0) == UHCI_IO0 && uhci_io_base(1) == UHCI_IO1, "");
        ck("and each got its own frame list",
           r_frba[0] != 0 && r_frba[1] != 0 && r_frba[0] != r_frba[1],
           "two controllers sharing one schedule would execute each "
           "other's descriptors");
        ck("both are running",
           (uhci_usbcmd(0) & 1) && (uhci_usbcmd(1) & 1), "");

        int n = uhci_scan();
        ck("the device on the second controller is found", n == 1,
           "this is the whole point: it is plugged into a socket the first "
           "controller does not own");
        if (n == 1) {
            ck("and recorded against that controller",
               uhci_device(0)->hc == 1 && uhci_device(0)->port == 1, "");
            ck("with its own descriptor",
               uhci_device(0)->vendor == 0x090C
               && uhci_device(0)->maxpacket0 == 64, "");
            uhci_dev_t d = *uhci_device(0);
            for (int i = 0; i < 64; i++) sim[3].in_data[i] = (uint8_t)(i + 9);
            sim[3].in_len = 64; sim[3].in_pos = 0; sim[3].tog_errors = 0;
            int r = uhci_bulk(&d, EP_BULK_IN, 1, buf, 64, 64);
            ck("and transfers go to the right controller", r == 64,
               "the queue head a transfer is hung off has to belong to the "
               "controller the device is behind");
            int same = 1;
            for (int i = 0; i < 64; i++)
                if (buf[i] != (uint8_t)(i + 9)) same = 0;
            ck("with the right bytes", same, "");
        }
    }

    head("devices on different controllers get different addresses");
    {
        build_bus(1, 0, 2);
        detach_all();
        sim_attach(0, 0, 0, 0x0781, 0x5567, 8, 0);
        sim_attach(1, 0, 0, 0x090C, 0x1000, 64, 0);
        sim_power_on();
        pci_init();
        uhci_init();
        int n = uhci_scan();
        ck("both are found", n == 2, "");
        if (n == 2) {
            ck("on the controllers they are actually plugged into",
               uhci_device(0)->hc == 0 && uhci_device(1)->hc == 1, "");
            ck("addresses 1 and 2",
               uhci_device(0)->addr == 1 && uhci_device(1)->addr == 2,
               "every device answers to address 0 until it is given one, so "
               "they have to be addressed one at a time");
            ck("and each device agrees",
               sim[0].addr == 1 && sim[2].addr == 2, "");
        }
    }

    /* ── sharing sockets with an EHCI ─────────────────────────────────── */
    head("an EHCI holds the sockets until it is asked for them");
    {
        /* This is what UTM builds for "USB Support: USB 2.0", and what every
           ICH is.  The stick attaches to the EHCI at 480 Mbit and the UHCI
           companion sees an empty port -- so the controller runs perfectly
           and reports nothing attached, which looks exactly like a broken
           driver. */
        build_bus(1, 1, 2);
        detach_all();
        sim_attach(0, 0, 0, 0x0781, 0x5567, 8, 1);   /* behind the EHCI */
        ehci_halts = 1;
        sim_power_on();
        ck("before anything runs, the EHCI owns the port",
           !ehci_released(0),
           "the simulated socket only answers on UHCI once the EHCI lets go");
        ck("so the companion sees nothing there",
           !port_has_device(0, 0), "");

        pci_init();
        ck("init succeeds", uhci_init() == 1, "");
        ck("the EHCI was found and its ports taken",
           uhci_ehci_released() == 1, "");
        ck("the configure flag was cleared",
           rd32(ehci_addr(E_OP_CONFIGFLAG)) == 0,
           "EHCI spec 4.2: CONFIGFLAG 0 default-routes every port to the "
           "companion controllers, which is the state it resets into");
        {
            int owned = 1;
            for (int i = 0; i < EHCI_NPORTS; i++)
                if (!(rd32(ehci_addr(E_OP_PORTSC + i * 4)) & 0x2000)) owned = 0;
            ck("and every port's owner bit points at a companion", owned,
               "some parts honour the configure flag and some the per-port "
               "bit, so both are set");
        }
        ck("the EHCI was stopped first",
           (rd32(ehci_addr(E_OP_USBCMD)) & 1) == 0,
           "handing ports away underneath a running controller is not a "
           "state the specification describes");
        ck("the BIOS was asked for the controller first",
           (bus_devs[3].cfg[EHCI_EECP / 4] & (1u << 24)) != 0
           && !(bus_devs[3].cfg[EHCI_EECP / 4] & (1u << 16)),
           "on a real laptop the BIOS owns the EHCI and touching its "
           "registers without the handover can trap into SMM");
        ck("and its SMIs were silenced",
           (bus_devs[3].cfg[(EHCI_EECP + 4) / 4] & 0x1FFFu) == 0, "");

        ck("now the companion can see the device", port_has_device(0, 0), "");
        int n = uhci_scan();
        ck("and it enumerates", n == 1, "");
        if (n == 1)
            ck("as the stick it is",
               uhci_device(0)->vendor == 0x0781
               && uhci_device(0)->product == 0x5567,
               "");
    }

    head("an EHCI that will not stop is not waited for forever");
    {
        build_bus(1, 1, 2);
        detach_all();
        sim_attach(0, 0, 0, 0x0781, 0x5567, 8, 1);
        ehci_halts = 0;                      /* never raises HCHalted */
        sim_power_on();
        pci_init();
        ck("init still succeeds", uhci_init() == 1, "");
        ck("the ports were handed over anyway", uhci_ehci_released() == 1,
           "a controller that never reports halted must not stop the "
           "driver from using the bus");
        ck("and the device is reachable", uhci_scan() == 1, "");
        ehci_halts = 1;
    }

    head("a BAR that is not really an EHCI is left alone");
    {
        build_bus(1, 1, 2);
        detach_all();
        sim_power_on();
        ehci_mmio[0] = 0xFF;                 /* an implausible CAPLENGTH */
        pci_init();
        ck("init still succeeds", uhci_init() == 1, "");
        ck("but nothing was written to the operational registers",
           uhci_ehci_released() == 0,
           "mmio + CAPLENGTH is where the writes land; a wild CAPLENGTH "
           "would put them somewhere else entirely");
        ck("the configure flag was not touched",
           rd32(ehci_addr(E_OP_CONFIGFLAG)) == 1, "");
    }

    /* ── low speed ────────────────────────────────────────────────────── */
    head("a low-speed device gets the LS bit in every descriptor");
    {
        build_bus(1, 0, 1);
        detach_all();
        sim_attach(0, 0, 1, 0x046D, 0xC077, 8, 0);   /* a low-speed mouse */
        sim_power_on();
        pci_init();
        uhci_init();
        ck("the port reports low speed",
           (uhci_portsc(0, 0) & 0x0100) != 0, "");
        uhci_dev_t d;
        int r = uhci_probe_port(0, 0, &d);
        ck("the probe succeeds", r == UHCI_OK, "");
        ck("the driver recorded it as low speed", d.lowspeed == 1, "");
        ck("the device saw no malformed packets", sim[0].tog_errors == 0,
           "the simulated device fails any TD without the LS bit set, the "
           "way a 1.5 Mbit device gets nothing but noise");
    }

    /* ── nothing attached ─────────────────────────────────────────────── */
    head("an empty bus is not an error");
    {
        build_bus(1, 0, 1);
        detach_all();
        sim_power_on();
        pci_init();
        ck("the controller still starts", uhci_init() == 1, "");
        ck("no ports report a connection", !uhci_port_connected(0, 0)
           && !uhci_port_connected(0, 1), "");
        ck("scanning finds nothing and does not hang", uhci_scan() == 0, "");
        uhci_dev_t d;
        ck("probing an empty port says so",
           uhci_probe_port(0, 0, &d) == UHCI_ENODEV, "");
    }

    /* ── the machine the music player may actually run on ─────────────── */
    head("a machine with EHCI and no UHCI says so plainly");
    {
        build_bus(0, 1, 0);
        detach_all();
        sim_power_on();
        pci_init();
        ck("uhci_init reports no controller", uhci_init() == 0, "");
        ck("and nothing pretends to be present", uhci_present() == 0, "");
        uint8_t progif = 0;
        ck("but the EHCI controller that IS there is reported",
           uhci_other_controller(&progif) == 1 && progif == 0x20,
           "a 2013 laptop has EHCI and no UHCI at all; saying 'no USB "
           "controller' would send anyone debugging it to the wrong place");
        ck("and its ports were left alone, having no companion to take them",
           rd32(ehci_addr(E_OP_CONFIGFLAG)) == 1,
           "handing ports to companions that do not exist would strand the "
           "device on no controller at all");
        ck("the kernel log names it", klog_mentions("EHCI"),
           "a machine with no UHCI still has to say what IS there, or the "
           "log sends you looking at the driver instead of the hardware");
    }

    head("no USB hardware at all");
    {
        build_bus(0, 0, 0);
        detach_all();
        sim_power_on();
        pci_init();
        ck("init returns cleanly", uhci_init() == 0, "");
        ck("no phantom controller", uhci_other_controller(0) == 0, "");
        ck("no controllers counted", uhci_hc_count() == 0, "");
        uhci_dev_t d;
        ck("a transfer with no controller is refused, not attempted",
           uhci_control(0, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, 0, 0, buf, 8)
           == UHCI_ENODEV, "");
        ck("and so is a probe", uhci_probe_port(0, 0, &d) == UHCI_ENODEV, "");
    }

    head("a controller that will not reset is not used");
    {
        build_bus(1, 0, 1);
        detach_all();
        sim_attach(0, 0, 0, 0x0781, 0x5567, 8, 0);
        sim_power_on();
        sim_hcreset_sticks = 1;
        pci_init();
        ck("init fails rather than driving broken hardware",
           uhci_init() == 0, "");
        ck("and reports it", uhci_present() == 0, "");
        sim_hcreset_sticks = 0;
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
