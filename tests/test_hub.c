/* ════════════════════════════════════════════════════════════════════
   test_hub.c -- finding a device behind a hub, and addressing it.

   0.8.5 walked root ports only and said so: "a laptop's internal
   keyboard and trackpad are usually on root ports, which is why this was
   judged enough for now -- but it is an assumption until somebody boots
   it." The HP bore it out, with devices on root ports 2 and 9, both
   internal.

   A USB stick is not an internal device. It goes in the socket on the
   side, and on most laptops that socket is behind an internal hub -- so
   a release whose whole subject is removable storage cannot ship with
   "it may need a hub" as its acceptance risk.

   ── WHAT IS WORTH TESTING ────────────────────────────────────────────

   The hub CLASS is four control requests and a loop, and if it were
   wrong the device would simply not appear.

   What is worth testing is the ROUTE STRING, because getting it wrong
   does not look like a bug. xHCI wants the path to a device as four bits
   per tier, port numbers ONE-based, tier one in bits 0..3, and a child's
   route is its parent's with its own port shifted into the next nibble.
   An off-by-one in the shift, or zero-based ports, produces a perfectly
   valid route string pointing down a DIFFERENT SOCKET -- so the symptom
   is a device that does not answer, which is indistinguishable from a
   port with nothing in it. There is no error to read.

   The same is true of the transaction translator. A low or full speed
   device behind a high speed hub needs the hub's slot and port in the
   slot context or the controller issues full-speed traffic at high-speed
   timing and the device never answers. Silence again.

   So this suite watches what the back-end is HANDED: every attach_path()
   call is recorded with its route, its root port and its translator, and
   the assertions are about those numbers. A simulated controller that
   merely succeeded would prove nothing, which is the lesson 0.8.5.1
   paid for twice.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-54s got %ld\n", what, got); }
}

#include "../include/types.h"
#include "../include/usb.h"

/* ── the simulated bus ────────────────────────────────────────────────
 *
 * A tree of devices, each with a parent and a port. The back-end below
 * answers for all of them, and the hub class drives it through the real
 * usb/hub.c and the real usb/core.c.
 */
#define SIM_MAX 24

typedef struct {
    int      present;
    int      parent;        /* -1 for a root port                      */
    int      port;          /* zero-based on that parent               */
    int      rootport;      /* for a root device                       */
    int      is_hub;
    int      nports;        /* if a hub                                */
    uint8_t  speed;
    uint16_t vendor, product;
    uint8_t  if_class, if_subclass, if_protocol;
    int      overcurrent;
    int      reset_fails;     /* never reports ENABLE                  */
    int      reset_stuck;     /* PORT_RESET never clears               */
    int      reset_polls;     /* how many polls before it clears       */
    int      ss_descriptor;  /* answers 0x2A, stalls 0x29               */
    int      no_port_count;  /* stalls both hub descriptors             */
    int      hides_config;   /* answers the device descriptor only      */
} sim_dev_t;

static sim_dev_t SIM[SIM_MAX];
static int       g_nsim;

/* What addr the core gave each simulated device, so the hub walk can map
   a control request back onto the tree. */
static int g_addr_of[256];       /* usb address -> SIM index            */

/* Every attach_path() call, recorded. */
typedef struct {
    int      rootport;
    uint32_t route;
    uint8_t  speed;
    uint32_t tt_slot;
    uint8_t  tt_port;
} att_t;
static att_t g_att[SIM_MAX];
static int   g_natt;

static int g_port_resets;        /* SET_FEATURE(PORT_RESET) count       */
/* ── the device's own recovery time ───────────────────────────────────
   The specification gives a device 10 ms after its port reset and says
   it need not answer before then, so a device asked too early answers at
   neither address. The first version of this suite let sleep_ms() be a
   no-op, which made "the recovery time is not waited out" MISSED: there
   was nothing for the missing wait to be missing from.

   Milliseconds since the host cleared C_PORT_RESET, and a device that is
   addressed before 10 of them have passed refuses. */
static uint32_t g_ms_since_clear;
static int      g_too_early;
static int g_port_powers;        /* SET_FEATURE(PORT_POWER) count       */
static int g_clear_conn;         /* CLEAR_FEATURE(C_PORT_CONNECTION)    */
static int g_clear_reset;        /* CLEAR_FEATURE(C_PORT_RESET)         */
static int g_root_attaches;

/* Which SIM device a port of a hub holds, or -1. */
static int child_of(int hub, int port)
{
    int i;
    for(i = 0; i < g_nsim; i++)
        if(SIM[i].present && SIM[i].parent == hub && SIM[i].port == port)
            return i;
    return -1;
}

static int g_cur;    /* the SIM device currently being attached         */

/* ── the back-end ─────────────────────────────────────────────────────  */
static int hc_port_count(usb_hc_t *h){ (void)h; return 4; }

static int hc_port_connected(usb_hc_t *h, int port)
{
    int i;
    (void)h;
    for(i = 0; i < g_nsim; i++)
        if(SIM[i].present && SIM[i].parent == -1 && SIM[i].rootport == port)
            return 1;
    return 0;
}

static void fill(usb_dev_t *out, int i)
{
    out->addr         = 0;      /* the core's counter fills this below  */
    out->speed        = SIM[i].speed;
    out->maxpacket0   = 64;
    out->vendor       = SIM[i].vendor;
    out->product      = SIM[i].product;
    out->dev_class    = SIM[i].is_hub ? 0x09 : 0x00;
    out->nconfigs     = SIM[i].hides_config ? 0 : 1;
    out->slot         = (uint32_t)(i + 1);   /* xHCI-ish slot id        */
}

extern uint8_t usb_core_next_address(void);

static int hc_attach(usb_hc_t *h, int port, usb_dev_t *out)
{
    int i;
    (void)h;
    for(i = 0; i < g_nsim; i++)
        if(SIM[i].present && SIM[i].parent == -1 && SIM[i].rootport == port)
            break;
    if(i == g_nsim) return USB_ENODEV;
    g_root_attaches++;
    fill(out, i);
    out->addr = usb_core_next_address();
    g_addr_of[out->addr] = i;
    g_cur = i;
    return USB_OK;
}

static int g_no_attach_path;       /* a back-end that cannot do hubs    */

static int hc_attach_path(usb_hc_t *h, int rootport, uint32_t route,
                          uint8_t speed, uint32_t tt_slot, uint8_t tt_port,
                          usb_dev_t *out)
{
    (void)h;
    if(g_natt < SIM_MAX){
        g_att[g_natt].rootport = rootport;
        g_att[g_natt].route    = route;
        g_att[g_natt].speed    = speed;
        g_att[g_natt].tt_slot  = tt_slot;
        g_att[g_natt].tt_port  = tt_port;
        g_natt++;
    }
    if(g_cur < 0 || g_cur >= g_nsim) return USB_ENODEV;
    /* Asked before its recovery time had elapsed. A real device simply
       does not answer, so this is USB_ENODEV and not a louder error. */
    if(g_ms_since_clear < 10){ g_too_early++; return USB_ENODEV; }
    /* A back-end that fills in its OWN idea of where the device is. The
       core overwrites these afterwards for exactly this reason, and
       until this scribble existed, removing that restore changed
       nothing the suite could see. */
    out->rootport = 0xEE;
    out->tier     = 9;
    out->parent   = 7;
    out->route    = 0xBADu;
    out->port     = 0xEE;
    fill(out, g_cur);
    out->addr = usb_core_next_address();
    g_addr_of[out->addr] = g_cur;
    return USB_OK;
}

/* ── control transfers ────────────────────────────────────────────────
 * The device descriptor walk, the configuration walk, and the hub class
 * requests, answered from the tree.
 */
#define RECIP_OTHER 0x03

static int hub_status_port;   /* the port the next GET_STATUS is about  */

static int hc_control(usb_hc_t *h, const usb_dev_t *d, uint8_t rtype,
                      uint8_t req, uint16_t value, uint16_t index,
                      void *data, uint16_t len)
{
    int me = (d && d->addr < 256) ? g_addr_of[d->addr] : -1;
    uint8_t *b = (uint8_t *)data;
    (void)h;

    /* ── the configuration descriptor ──────────────────────────────── */
    if(rtype == (USB_DIR_IN|USB_TYPE_STANDARD|USB_RECIP_DEVICE) &&
       req == USB_REQ_GET_DESCRIPTOR && (value >> 8) == USB_DESC_CONFIG){
        /* 9 bytes of config + 9 of interface + 7 of endpoint */
        uint8_t cfg[25];
        int n = (len < 25) ? len : 25;
        if(me < 0) return USB_ENODEV;
        memset(cfg, 0, sizeof cfg);
        cfg[0]=9; cfg[1]=2; cfg[2]=25; cfg[3]=0; cfg[4]=1; cfg[5]=1;
        cfg[9]=9; cfg[10]=4; cfg[11]=0; cfg[12]=0; cfg[13]=1;
        cfg[14]= SIM[me].is_hub ? 0x09 : SIM[me].if_class;
        cfg[15]= SIM[me].is_hub ? 0x00 : SIM[me].if_subclass;
        cfg[16]= SIM[me].is_hub ? 0x00 : SIM[me].if_protocol;
        cfg[18]=7; cfg[19]=5; cfg[20]=0x81; cfg[21]=0x03;
        cfg[22]=1; cfg[23]=0; cfg[24]=12;
        memcpy(b, cfg, (size_t)n);
        return n;
    }
    if(rtype == (USB_DIR_OUT|USB_TYPE_STANDARD|USB_RECIP_DEVICE) &&
       req == USB_REQ_SET_CONFIGURATION)
        return 0;

    /* ── the hub descriptor ────────────────────────────────────────── */
    if(rtype == (USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_DEVICE) &&
       req == USB_REQ_GET_DESCRIPTOR){
        int want = value >> 8;
        if(me < 0 || !SIM[me].is_hub) return USB_ESTALL;
        if(SIM[me].no_port_count) return USB_ESTALL;
        /* A SuperSpeed hub answers 0x2A and STALLS 0x29. A driver that
           asks only for 0x29 sees zero ports, which looks exactly like a
           hub with nothing plugged into it. */
        if(SIM[me].ss_descriptor ? (want != 0x2A) : (want != 0x29))
            return USB_ESTALL;
        if(len < 3 || !b) return USB_EXFER;
        memset(b, 0, len);
        b[0] = 9; b[1] = (uint8_t)want; b[2] = (uint8_t)SIM[me].nports;
        /* wHubCharacteristics: TT think time 2 (bits 6:5) on a 2.0 hub,
           with the bits around it set so a reader taking the wrong ones
           shows -- power switching (1:0) = 1, compound (2) = 1. The
           first version used think time 1, which equals the power bits:
           the harness caught a reader taking bits 1:0 passing. */
        if(want == 0x29){ b[3] = (uint8_t)((2u << 5) | 0x05u); b[4] = 0x80; }
        return (len < 9) ? len : 9;
    }

    /* ── port requests ────────────────────────────────────────────── */
    if(rtype == (USB_DIR_OUT|USB_TYPE_CLASS|RECIP_OTHER)){
        int port = (int)index - 1;
        if(me < 0 || !SIM[me].is_hub) return USB_ESTALL;
        if(req == USB_REQ_SET_FEATURE){
            if(value == 8)  { g_port_powers++; return 0; }    /* POWER   */
            if(value == 4)  { g_port_resets++; hub_status_port = port;
                              return 0; }                      /* RESET  */
        }
        if(req == USB_REQ_CLEAR_FEATURE){
            if(value == 16) { g_clear_conn++;  return 0; }
            if(value == 20) { g_clear_reset++; g_ms_since_clear = 0;
                              return 0; }
        }
        return 0;
    }

    if(rtype == (USB_DIR_IN|USB_TYPE_CLASS|RECIP_OTHER) &&
       req == USB_REQ_GET_STATUS){
        int port = (int)index - 1;
        int kid;
        uint16_t st = 0, ch = 0;
        if(me < 0 || !SIM[me].is_hub) return USB_ESTALL;
        if(len < 4 || !b) return USB_EXFER;
        kid = child_of(me, port);
        if(kid >= 0){
            st |= 0x0001;                       /* CONNECTION           */
            ch |= 0x0001;
            /* A port reset takes time, and the host has to poll the
               RESET bit to find out when it is done. A hub whose bit
               never clears is a hub whose port is broken -- and an
               unbounded wait on it is indistinguishable from a reset
               that did not work, which is the bug class
               kernel/machine.c was written to get rid of. */
            if(SIM[kid].reset_stuck && g_port_resets)
                st |= 0x0010;                   /* RESET, for ever      */
            else if(SIM[kid].reset_polls > 0){
                SIM[kid].reset_polls--;
                st |= 0x0010;
            }
            if(SIM[kid].overcurrent) st |= 0x0008;
            else if(!SIM[kid].reset_fails && !(st & 0x0010))
                st |= 0x0002;                   /* ENABLE               */
            if(SIM[kid].speed == USB_SPEED_LOW)  st |= 0x0200;
            if(SIM[kid].speed == USB_SPEED_HIGH) st |= 0x0400;
            /* usb/hub.c reads a port's status and then attaches what is
               on it before touching another port, so the last status read
               names the device the next attach_path() is about. */
            g_cur = kid;
        }
        b[0]=(uint8_t)st; b[1]=(uint8_t)(st>>8);
        b[2]=(uint8_t)ch; b[3]=(uint8_t)(ch>>8);
        return 4;
    }

    return USB_EXFER;
}

static int hc_bulk(usb_hc_t *h, const usb_dev_t *d, uint8_t ep, int in,
                   void *data, int len, int mps){
    (void)h;(void)d;(void)ep;(void)in;(void)data;(void)len;(void)mps;
    return USB_ENODEV;
}
static int hc_configure(usb_hc_t *h, usb_dev_t *d){ (void)h;(void)d; return USB_OK; }

/* v0.9.2.2: what the hub driver told the controller about a hub, and
   when -- the port powers/resets counted so far, which must be none. */
static int g_hubinfo_calls, g_hubinfo_nports, g_hubinfo_ttt, g_hubinfo_mtt;
static int g_hubinfo_resets_before, g_hubinfo_powers_before, g_hubinfo_refuse;
static int hc_hub_info(usb_hc_t *h, const usb_dev_t *d, int nports, int ttt, int mtt)
{
    (void)h; (void)d;
    g_hubinfo_calls++;
    g_hubinfo_nports = nports; g_hubinfo_ttt = ttt; g_hubinfo_mtt = mtt;
    g_hubinfo_resets_before = g_port_resets;
    g_hubinfo_powers_before = g_port_powers;
    return g_hubinfo_refuse ? USB_ECMD : USB_OK;
}

static const usb_hc_ops_t OPS_WITH_PATH = {
    .name           = "xHCI",
    .port_count     = hc_port_count,
    .port_connected = hc_port_connected,
    .attach         = hc_attach,
    .attach_path    = hc_attach_path,
    .control        = hc_control,
    .bulk           = hc_bulk,
    .configure      = hc_configure,
    .hub_info       = hc_hub_info,
};
/* A back-end that cannot address a device through a hub -- which is
   UHCI's position in the kernel until drivers/uhci.c can be asked to
   reset a hub port. usb/hub.c has to SAY so rather than find nothing. */
static const usb_hc_ops_t OPS_NO_PATH = {
    .name           = "UHCI",
    .port_count     = hc_port_count,
    .port_connected = hc_port_connected,
    .attach         = hc_attach,
    .control        = hc_control,
    .bulk           = hc_bulk,
    .configure      = hc_configure,
};

static int g_sleeps;
void sleep_ms(uint32_t ms){ g_ms_since_clear += ms; g_sleeps++; }
void klog(int lvl, const char *fmt, ...){ (void)lvl; (void)fmt; }

/* The two back-end registrations usb_init() makes; neither is here. */
int uhci_hc_register(void){ return 0; }
int xhci_hc_register(void){ return 0; }
/* v0.8.8.4: usb/core.c prints WHERE an xHCI attach failed, not just
   the code, because "no answer (-4)" named four different bugs at
   once. This suite stubs the controller, so there is nothing to
   report -- and a stub that returns a string makes that visible
   rather than linking to an empty one. */
const char *xhci_attach_why(void){ return "(xHCI stubbed in this suite)"; }

/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../usb/core.c"
#include "../usb/hub.c"

/* ── building a tree ──────────────────────────────────────────────── */
static void sim_clear(void)
{
    memset(SIM, 0, sizeof SIM);
    memset(g_addr_of, 0, sizeof g_addr_of);
    memset(g_att, 0, sizeof g_att);
    g_nsim = g_natt = 0;
    g_port_resets = g_port_powers = g_clear_conn = g_clear_reset = 0;
    g_root_attaches = 0;
    g_no_attach_path = 0;
    g_ms_since_clear = 1000;        /* nothing has been reset yet */
    g_too_early = 0;
    g_cur = -1;
    g_sleeps = 0;
    g_hubinfo_calls = g_hubinfo_nports = g_hubinfo_ttt = g_hubinfo_mtt = 0;
    g_hubinfo_resets_before = g_hubinfo_powers_before = -1;
    g_hubinfo_refuse = 0;
}

static int add_root_hub(int rootport, int nports, uint8_t speed)
{
    int i = g_nsim++;
    SIM[i].present = 1; SIM[i].parent = -1; SIM[i].rootport = rootport;
    SIM[i].is_hub = 1; SIM[i].nports = nports; SIM[i].speed = speed;
    SIM[i].vendor = 0x1A40; SIM[i].product = (uint16_t)(0x0100 + i);
    return i;
}
static int add_child(int parent, int port, uint8_t speed, int is_hub,
                     int nports)
{
    int i = g_nsim++;
    SIM[i].present = 1; SIM[i].parent = parent; SIM[i].port = port;
    SIM[i].rootport = SIM[parent].rootport;
    SIM[i].speed = speed; SIM[i].is_hub = is_hub; SIM[i].nports = nports;
    SIM[i].vendor = 0x0781; SIM[i].product = (uint16_t)(0x5500 + i);
    SIM[i].if_class = 0x08; SIM[i].if_subclass = 0x06;
    SIM[i].if_protocol = 0x50;
    return i;
}

static void go(const usb_hc_ops_t *ops)
{
    /* usb_init() clears the controller table and the device table and
       then calls the two back-end registrations, both of which are
       stubbed to 0 here -- so it is the core's own reset, used rather
       than a test-only entry point added beside it. */
    usb_init();
    usb_register_hc(ops, 0, 0x1000, USB_HC_XHCI);
    usb_enumerate();
}

/* Find the device table entry for a simulated device. */
static const usb_dev_t *dev_for(int simidx)
{
    int i;
    for(i = 0; i < usb_device_count(); i++){
        const usb_dev_t *d = usb_device(i);
        if(d && d->addr < 256 && g_addr_of[d->addr] == simidx) return d;
    }
    return 0;
}

int main(void)
{
    printf("Finding a device behind a hub, and addressing it\n");
    printf("==============================================\n");

    head("a root port with a stick on it still works");
    sim_clear();
    { int i = g_nsim++;
      SIM[i].present=1; SIM[i].parent=-1; SIM[i].rootport=2;
      SIM[i].speed=USB_SPEED_HIGH; SIM[i].if_class=0x08;
      SIM[i].if_subclass=0x06; SIM[i].if_protocol=0x50;
      SIM[i].vendor=0x0781; SIM[i].product=0x5567; }
    go(&OPS_WITH_PATH);
    ck("one device", usb_device_count() == 1, usb_device_count());
    ck("it is on tier 0", usb_device(0)->tier == 0, usb_device(0)->tier);
    ck("its route string is empty", usb_device(0)->route == 0,
       (long)usb_device(0)->route);
    ck("its root port is the one it is on", usb_device(0)->rootport == 2,
       usb_device(0)->rootport);
    ck("it has no parent", usb_device(0)->parent == -1,
       usb_device(0)->parent);
    ck("no transaction translator", usb_device(0)->tt_slot == 0,
       (long)usb_device(0)->tt_slot);
    ck("attach_path was not used for it", g_natt == 0, g_natt);
    ck("the hub pass found no hubs", usb_hub_count() == 0, usb_hub_count());

    head("a hub on a root port, with a stick on its port 3");
    sim_clear();
    { int h = add_root_hub(1, 4, USB_SPEED_HIGH);
      add_child(h, 2, USB_SPEED_HIGH, 0, 0); }      /* port index 2 */
    go(&OPS_WITH_PATH);
    ck("two devices: the hub and the stick", usb_device_count() == 2,
       usb_device_count());
    ck("one hub seen", usb_hub_count() == 1, usb_hub_count());
    ck("one device found behind it", usb_hub_devices() == 1,
       usb_hub_devices());
    ck("every port was powered before being looked at",
       g_port_powers == 4, g_port_powers);
    ck("only the occupied port was reset", g_port_resets == 1,
       g_port_resets);
    ck("the reset change bit was cleared afterwards", g_clear_reset == 1,
       g_clear_reset);
    ck("the connection change bit was cleared too", g_clear_conn >= 1,
       g_clear_conn);
    ck("ONE attach_path call", g_natt == 1, g_natt);
    /* THE CHECK THIS FILE EXISTS FOR. Port index 2 is port NUMBER 3, and
       a tier-1 device's port goes in bits 0..3. A zero-based port or a
       shift of 4 instead of 0 both produce a valid route string pointing
       down a different socket, and the symptom is silence. */
    ck("the route string is the port NUMBER, one-based, in bits 0..3",
       g_att[0].route == 3u, (long)g_att[0].route);
    ck("the root port handed over is the hub's, not the hub port",
       g_att[0].rootport == 1, g_att[0].rootport);
    ck("the speed came from the HUB's port status, not the hub's own",
       g_att[0].speed == USB_SPEED_HIGH, g_att[0].speed);
    { const usb_dev_t *s = usb_device(1);
      ck("the stick is on tier 1", s->tier == 1, s->tier);
      ck("its parent is the hub", s->parent == 0, s->parent);
      ck("its parent port is recorded zero-based", s->parent_port == 2,
         s->parent_port);
      ck("its root port is the hub's", s->rootport == 1, s->rootport);
      ck("the core kept the route it built", s->route == 3u, (long)s->route);
      ck("its interface was walked, so the class is known",
         s->if_class == 0x08, s->if_class);
      ck("...and an endpoint was found", s->nep >= 1, s->nep); }

    head("port numbers are one-based: port index 0 is route 1, not 0");
    /* A route string of 0 means "on the root port". A zero-based port
       index would make the device on port 1 of a hub indistinguishable
       from one plugged straight into the machine. */
    sim_clear();
    { int h = add_root_hub(0, 2, USB_SPEED_HIGH);
      add_child(h, 0, USB_SPEED_HIGH, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("the first port of a hub is route 1", g_att[0].route == 1u,
       (long)g_att[0].route);
    ck("...which is not the same as a root-port device's 0",
       g_att[0].route != 0u, (long)g_att[0].route);

    head("two tiers: a hub behind a hub");
    sim_clear();
    { int h1 = add_root_hub(3, 4, USB_SPEED_HIGH);
      int h2 = add_child(h1, 1, USB_SPEED_HIGH, 1, 4);   /* port 2 */
      add_child(h2, 3, USB_SPEED_HIGH, 0, 0); }          /* port 4 */
    go(&OPS_WITH_PATH);
    ck("three devices", usb_device_count() == 3, usb_device_count());
    ck("two hubs seen", usb_hub_count() == 2, usb_hub_count());
    ck("two devices found behind hubs", usb_hub_devices() == 2,
       usb_hub_devices());
    { const usb_dev_t *h2 = usb_device(1);
      const usb_dev_t *st = usb_device(2);
      ck("the second hub is tier 1", h2->tier == 1, h2->tier);
      ck("...with route 2", h2->route == 2u, (long)h2->route);
      ck("the stick is tier 2", st->tier == 2, st->tier);
      /* Tier 2 goes in bits 4..7: port 4 behind port 2 is 0x42, read
         right to left. An off-by-one in the shift gives 0x24 or 0x06,
         both of which are valid route strings to somewhere else. */
      ck("...and its route is 0x42: tier 1 low, tier 2 above it",
         st->route == 0x42u, (long)st->route);
      ck("its root port is still the first hub's", st->rootport == 3,
         st->rootport);
      ck("its parent is the SECOND hub", st->parent == 1, st->parent); }

    head("three tiers, so the shift is not right by accident at two");
    sim_clear();
    { int h1 = add_root_hub(0, 8, USB_SPEED_HIGH);
      int h2 = add_child(h1, 0, USB_SPEED_HIGH, 1, 8);   /* port 1 */
      int h3 = add_child(h2, 2, USB_SPEED_HIGH, 1, 8);   /* port 3 */
      add_child(h3, 4, USB_SPEED_HIGH, 0, 0); }          /* port 5 */
    go(&OPS_WITH_PATH);
    ck("four devices", usb_device_count() == 4, usb_device_count());
    ck("the deepest device is tier 3", usb_device(3)->tier == 3,
       usb_device(3)->tier);
    ck("its route is 0x531", usb_device(3)->route == 0x531u,
       (long)usb_device(3)->route);

    head("the tier bound stops a hub that claims to be its own child");
    /* The route string has five nibbles, so five tiers, which is USB's
       own limit for the same reason. The breadth loop in usb/hub.c is
       safe against a loop in the tree only because the core refuses
       past it. */
    sim_clear();
    { int p = add_root_hub(0, 2, USB_SPEED_HIGH);
      int i;
      for(i = 0; i < 8; i++) p = add_child(p, 0, USB_SPEED_HIGH, 1, 2); }
    go(&OPS_WITH_PATH);
    ck("the walk stopped rather than running away",
       usb_device_count() <= USB_MAX_TIER, usb_device_count());
    /* Guarded, because tools/check_hub_suite.py damages this code on
       purpose and a null dereference reports as a crash rather than as a
       caught injection -- which is the difference between a harness that
       proves something and one that looks like it did. */
    { const usb_dev_t *deepest = usb_device_count() > 0
                                 ? usb_device(usb_device_count()-1) : 0;
      ck("no device claims a tier past the bound",
         deepest && deepest->tier < USB_MAX_TIER,
         deepest ? deepest->tier : -1); }
    ck("...and it still found the first few",
       usb_device_count() >= 3, usb_device_count());

    head("THE TRANSACTION TRANSLATOR, for a slow device behind a fast hub");
    /* Without it the controller issues full-speed traffic at high-speed
       timing and the device never answers -- which looks exactly like an
       empty port. There is no error to read, which is why this is
       checked on the numbers handed to the back-end. */
    sim_clear();
    { int h = add_root_hub(2, 4, USB_SPEED_HIGH);
      add_child(h, 1, USB_SPEED_FULL, 0, 0); }       /* port 2 */
    go(&OPS_WITH_PATH);
    ck("a full-speed device behind a high-speed hub gets a translator",
       g_att[0].tt_slot != 0, (long)g_att[0].tt_slot);
    ck("...and its speed is handed over as FULL",
       g_att[0].speed == USB_SPEED_FULL, g_att[0].speed);
    ck("...and it is the HUB's slot", g_att[0].tt_slot == 1u,
       (long)g_att[0].tt_slot);
    ck("...and the hub's port, one-based", g_att[0].tt_port == 2,
       g_att[0].tt_port);
    ck("the core recorded it on the device too",
       usb_device(1)->tt_slot == 1u, (long)usb_device(1)->tt_slot);

    sim_clear();
    { int h = add_root_hub(2, 4, USB_SPEED_HIGH);
      add_child(h, 1, USB_SPEED_LOW, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("a low-speed device behind one gets a translator too",
       g_att[0].tt_slot != 0, (long)g_att[0].tt_slot);
    /* And the SPEED itself, which the translator check cannot see:
       low and full both get one, so swapping the two bits in
       port_speed_of() was MISSED until this asked for the number. The
       speed decides the default pipe's packet size, so getting it wrong
       is a device that answers its first eight bytes and then stops. */
    ck("...and it is handed over as LOW, not as something else",
       g_att[0].speed == USB_SPEED_LOW, g_att[0].speed);

    sim_clear();
    { int h = add_root_hub(2, 4, USB_SPEED_HIGH);
      add_child(h, 1, USB_SPEED_HIGH, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("a HIGH-speed device behind one does NOT",
       g_att[0].tt_slot == 0, (long)g_att[0].tt_slot);
    ck("...and its speed is handed over as HIGH",
       g_att[0].speed == USB_SPEED_HIGH, g_att[0].speed);

    sim_clear();
    { int h = add_root_hub(2, 4, USB_SPEED_FULL);
      add_child(h, 1, USB_SPEED_FULL, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("a full-speed device behind a FULL-speed hub needs none either",
       g_att[0].tt_slot == 0, (long)g_att[0].tt_slot);

    head("the translator is inherited from the hub doing the conversion");
    /* It belongs to the highest-speed hub in the path, not the nearest
       one. A full-speed device two hubs down still points at the hub
       that is actually translating. */
    sim_clear();
    { int h1 = add_root_hub(1, 4, USB_SPEED_HIGH);
      int h2 = add_child(h1, 0, USB_SPEED_FULL, 1, 4);
      add_child(h2, 1, USB_SPEED_FULL, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("the full-speed hub itself got the translator",
       g_att[0].tt_slot == 1u, (long)g_att[0].tt_slot);
    ck("the device below it INHERITED the same one, not its parent's slot",
       g_att[1].tt_slot == 1u, (long)g_att[1].tt_slot);
    ck("...and the same port", g_att[1].tt_port == g_att[0].tt_port,
       g_att[1].tt_port);

    head("a back-end that cannot address through a hub says so");
    sim_clear();
    { int h = add_root_hub(0, 2, USB_SPEED_FULL);
      add_child(h, 0, USB_SPEED_FULL, 0, 0); }
    go(&OPS_NO_PATH);
    ck("the hub itself is still found", usb_device_count() == 1,
       usb_device_count());
    ck("it is counted as a hub", usb_hub_count() == 1, usb_hub_count());
    ck("but nothing behind it", usb_hub_devices() == 0, usb_hub_devices());
    ck("and attach_path was never called", g_natt == 0, g_natt);

    head("an over-current port is skipped, not reset");
    sim_clear();
    { int h = add_root_hub(0, 2, USB_SPEED_HIGH);
      int k = add_child(h, 0, USB_SPEED_HIGH, 0, 0);
      SIM[k].overcurrent = 1; }
    go(&OPS_WITH_PATH);
    ck("the device is not enumerated", usb_device_count() == 1,
       usb_device_count());
    ck("and the port was never reset", g_port_resets == 0, g_port_resets);

    head("a reset that takes a while is waited out, not given up on");
    /* The hub holds PORT_RESET set until it is done, and the host has to
       poll for it: the specification gives a hub 20 ms and a cheap one
       takes longer. A driver that reads the bit once and believes it
       sees a port with nothing on it. */
    sim_clear();
    { int h = add_root_hub(0, 2, USB_SPEED_HIGH);
      int k = add_child(h, 0, USB_SPEED_HIGH, 0, 0);
      SIM[k].reset_polls = 6; }          /* ~60 ms of reset */
    go(&OPS_WITH_PATH);
    ck("the device is found anyway", usb_device_count() == 2,
       usb_device_count());
    ck("the reset was polled rather than assumed", g_sleeps > 6, g_sleeps);
    ck("and the device was not addressed before it was ready",
       g_too_early == 0, g_too_early);

    head("the device's recovery time is waited out before it is addressed");
    /* 10 ms after the reset clears, during which the specification says
       a device need not answer -- so a device asked too early answers at
       neither address. The simulated device refuses if it is addressed
       inside that window, because with sleep_ms() a no-op there was
       nothing for a missing wait to be missing from. */
    sim_clear();
    { int h = add_root_hub(0, 2, USB_SPEED_HIGH);
      add_child(h, 0, USB_SPEED_HIGH, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("the device answered", usb_device_count() == 2, usb_device_count());
    ck("it was never addressed inside its recovery window",
       g_too_early == 0, g_too_early);

    head("a hub whose port reset never finishes is given up on");
    /* An unbounded poll here hangs the boot, and from the outside that
       is indistinguishable from a reset that did not work --
       kernel/machine.c exists because of exactly this bug class, in a
       different driver. */
    sim_clear();
    { int h = add_root_hub(0, 2, USB_SPEED_HIGH);
      int k = add_child(h, 0, USB_SPEED_HIGH, 0, 0);
      SIM[k].reset_stuck = 1; }
    go(&OPS_WITH_PATH);
    ck("the walk returned rather than spinning", usb_device_count() == 1,
       usb_device_count());
    ck("and the device was not attached", g_natt == 0, g_natt);

    head("a port whose reset never completes is given up on");
    sim_clear();
    { int h = add_root_hub(0, 2, USB_SPEED_HIGH);
      int k = add_child(h, 0, USB_SPEED_HIGH, 0, 0);
      SIM[k].reset_fails = 1; }      /* never reports ENABLE */
    go(&OPS_WITH_PATH);
    ck("the device is not enumerated", usb_device_count() == 1,
       usb_device_count());
    ck("the reset was attempted", g_port_resets == 1, g_port_resets);
    ck("and attach_path was not called on a port that is not enabled",
       g_natt == 0, g_natt);

    head("v0.9.2.2: the controller is told a hub IS a hub, before its ports");
    /* Kyan's HP, 11 November: a keyboard behind a hub lit up and typed
       nothing. The hub's slot never said it was a hub, so the low/full
       speed keyboard behind it got no split transactions. */
    sim_clear();
    { int h = add_root_hub(0, 4, USB_SPEED_HIGH);
      add_child(h, 2, USB_SPEED_LOW, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("hub_info was called once, for the one hub", g_hubinfo_calls == 1, g_hubinfo_calls);
    ck("  ... with its 4 ports and TT think time 2 (bits 6:5 only)",
       g_hubinfo_nports == 4 && g_hubinfo_ttt == 2, g_hubinfo_ttt);
    ck("  ... before any port was powered or reset",
       g_hubinfo_powers_before == 0 && g_hubinfo_resets_before == 0,
       g_hubinfo_powers_before);
    ck("  ... and the keyboard behind it is still found", usb_device_count() == 2,
       usb_device_count());
    sim_clear();
    { int h = add_root_hub(0, 4, USB_SPEED_HIGH);
      add_child(h, 2, USB_SPEED_HIGH, 0, 0); }
    g_hubinfo_refuse = 1;
    go(&OPS_WITH_PATH);
    ck("a controller that refuses it: the walk goes on (a 2.0 stick is fine)",
       g_hubinfo_calls == 1 && usb_device_count() == 2, usb_device_count());
    sim_clear();
    { int h = add_root_hub(0, 4, USB_SPEED_SUPER);
      SIM[h].ss_descriptor = 1; }
    go(&OPS_WITH_PATH);
    ck("a SuperSpeed hub has no translator: think time 0",
       g_hubinfo_calls == 1 && g_hubinfo_ttt == 0, g_hubinfo_ttt);
    sim_clear();
    { int h = add_root_hub(0, 2, USB_SPEED_HIGH);
      add_child(h, 0, USB_SPEED_HIGH, 0, 0); }
    go(&OPS_NO_PATH);
    ck("a back-end without the op (UHCI) is not asked", g_hubinfo_calls == 0,
       g_hubinfo_calls);

    head("a SuperSpeed hub answers 0x2A and stalls 0x29");
    /* Asking only for the 2.0 descriptor makes every USB 3 hub report
       zero ports, which is indistinguishable from an empty one. */
    sim_clear();
    { int h = add_root_hub(0, 4, USB_SPEED_SUPER);
      SIM[h].ss_descriptor = 1;
      add_child(h, 1, USB_SPEED_SUPER, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("the stick behind it is found", usb_device_count() == 2,
       usb_device_count());
    ck("...at the right route", g_att[0].route == 2u, (long)g_att[0].route);

    head("a hub that answers neither descriptor is reported, not walked");
    sim_clear();
    { int h = add_root_hub(0, 4, USB_SPEED_HIGH);
      SIM[h].no_port_count = 1;
      add_child(h, 1, USB_SPEED_HIGH, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("nothing behind it", usb_device_count() == 1, usb_device_count());
    ck("and no port was powered", g_port_powers == 0, g_port_powers);

    head("a hub whose configuration never parsed is still walked");
    /* 0.8.5 found that a device can answer its device descriptor and not
       its configuration. A hub that did that would otherwise hide every
       disk behind it, so the device class is checked as well as the
       interface class. */
    sim_clear();
    { int h = add_root_hub(0, 2, USB_SPEED_HIGH);
      SIM[h].hides_config = 1;
      add_child(h, 0, USB_SPEED_HIGH, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("the stick behind it is still found", usb_device_count() == 2,
       usb_device_count());

    head("an empty hub costs one pass and no attaches");
    sim_clear();
    add_root_hub(0, 7, USB_SPEED_HIGH);
    go(&OPS_WITH_PATH);
    ck("just the hub", usb_device_count() == 1, usb_device_count());
    ck("all seven ports were powered", g_port_powers == 7, g_port_powers);
    ck("none were reset", g_port_resets == 0, g_port_resets);
    ck("no device was attached", g_natt == 0, g_natt);

    head("a hub claiming more ports than exist is clamped");
    /* bNbrPorts comes off the wire from somebody else's firmware. A hub
       claiming 127 ports is a loop 127 long, and the port array it
       indexes is 15. */
    sim_clear();
    { int h = add_root_hub(0, 127, USB_SPEED_HIGH);
      add_child(h, 3, USB_SPEED_HIGH, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("at most HUB_MAX_PORTS were powered", g_port_powers == 15,
       g_port_powers);
    ck("the stick on port 4 was still found", usb_device_count() == 2,
       usb_device_count());
    ck("...at route 4", g_att[0].route == 4u, (long)g_att[0].route);

    head("usb_attach_behind refuses an index that is not a hub");
    /* usb/hub.c passes an index it got from the table, so this is a
       bound on somebody else's arithmetic -- the kind that holds until
       the caller changes. tools/check_hub_suite.py found it unchecked. */
    sim_clear();
    { int h = add_root_hub(0, 2, USB_SPEED_HIGH);
      add_child(h, 0, USB_SPEED_HIGH, 0, 0); }
    go(&OPS_WITH_PATH);
    { int n = usb_device_count();
      ck("an index past the end is refused",
         usb_attach_behind(99, 0, USB_SPEED_HIGH) == USB_EPARAM,
         usb_attach_behind(99, 0, USB_SPEED_HIGH));
      ck("a negative index is refused",
         usb_attach_behind(-1, 0, USB_SPEED_HIGH) == USB_EPARAM,
         usb_attach_behind(-1, 0, USB_SPEED_HIGH));
      ck("a port past a hub's addressable range is refused",
         usb_attach_behind(0, 99, USB_SPEED_HIGH) == USB_EPARAM,
         usb_attach_behind(0, 99, USB_SPEED_HIGH));
      ck("a negative port is refused",
         usb_attach_behind(0, -1, USB_SPEED_HIGH) == USB_EPARAM,
         usb_attach_behind(0, -1, USB_SPEED_HIGH));
      ck("and none of them added a device", usb_device_count() == n,
         usb_device_count() - n); }

    head("re-enumerating gives the same tree, not a longer one");
    sim_clear();
    { int h = add_root_hub(1, 4, USB_SPEED_HIGH);
      add_child(h, 2, USB_SPEED_HIGH, 0, 0); }
    go(&OPS_WITH_PATH);
    ck("two devices", usb_device_count() == 2, usb_device_count());
    { int first = usb_device_count();
      usb_enumerate();
      ck("two again after a rescan", usb_device_count() == first,
         usb_device_count());
      usb_enumerate();
      ck("and again", usb_device_count() == first, usb_device_count()); }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
