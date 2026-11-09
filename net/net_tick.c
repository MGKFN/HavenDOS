/* ════════════════════════════════════════════════════════════════════
   net/net_tick.c -- the network's periodic work, v0.8.8 "Helios"

   Two kpoll clients.  The mechanism, the refusal codes and the rules
   that keep work out of the IRQ are in include/kpoll.h; what is here
   is what the network actually wants doing on a timer, and -- just as
   important -- what it turned out it cannot.

   ── 1. DHCP RETRY.  This one is real. ───────────────────────────────

   v0.8.7.2 added a DHCP request at boot, and v0.8.7 added a second one
   to `usb rescan`.  Both are patches around not having a timer:

     - at boot, only if a NIC was ALREADY registered.  A USB NIC is
       registered when the bus is walked, and on the HP that is after
       net_init(), so the ordinary tethering case missed it.
     - on rescan, only because a person typed the command.

   Until one of those fired, the machine held 10.0.2.15 behind a
   gateway at 10.0.2.2 -- QEMU's network and nobody else's -- with
   `ifconfig` reporting "Status: UP" over the top of it.  The only
   symptom was `dns: resolution failed`.  That was the whole of it on
   the HP on 26 October.

   ── 2. CARRIER.  This one is real for EXACTLY ONE NIC. ──────────────

   The scope for this release said the tick would close carrier
   polling.  Checking before building found that the answer depends
   entirely on the driver, and the two kinds of NIC in this tree are
   opposite:

     usb/ethernet.c   un_link() issues IPHETH_REQ_CARRIER and reports
                      the device's answer.  REAL.
     drivers/e1000.c  no link_up op at all.
     drivers/rtl8139.c       likewise.

   netdev_link_up() returns 1 for a NIC with no op -- "this NIC cannot
   say", documented in include/netdev.h -- so a carrier poll of a PCI
   NIC reads a constant.  A client that watched that would be a fixture
   that cannot distinguish anything, which is the shape 0.8.7's
   harnesses found four times, except in the kernel rather than in a
   test.

   So this client watches for a TRANSITION and only reports one, and it
   can only ever see one on a NIC whose driver answers.  That is stated
   here rather than left for somebody to infer from an event that never
   arrives, and `net` prints which kind each NIC is.  Reading the link
   state out of the e1000 and RTL8139 status registers is a driver
   change, not a timer change, and it is recorded as open.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/kpoll.h"
#include "../include/net.h"
#include "../include/netdev.h"
#include "../include/klog.h"
#include "../include/types.h"

extern int  dhcp_request(void);
extern int  keyboard_haschar(void);

/* ── DHCP retry ──────────────────────────────────────────────────────
 *
 * BACKOFF, AND WHY IT IS NOT A FIXED INTERVAL.  dhcp_request() blocks
 * for up to about four seconds, and the place this runs from is the
 * keyboard's idle loop -- so an attempt that is made while somebody is
 * sitting at the prompt is four seconds in which the shell does not
 * answer.  A fixed ten-second retry on a machine with no DHCP server
 * would mean the shell was unresponsive about 40% of the time, for
 * ever, and the user would read that as the OS being broken rather
 * than as the network being absent.
 *
 * So: a small number of attempts, spaced further and further apart,
 * and then silence until something changes.  `usb rescan` and
 * `ifconfig dhcp` both reset the sequence, because both mean "the
 * situation is different now".
 *
 * And it does not start an attempt when a keystroke is already
 * waiting.  That does not make the freeze shorter; it makes it land
 * when nobody is typing, which is the only thing that can be done
 * about it without taking dhcp_request() apart into a state machine.
 * Doing that is the right answer and it is a bigger change than this
 * release should carry. */
#define DHCP_MAX_TRIES 5
static const uint32_t DHCP_BACKOFF_TICKS[DHCP_MAX_TRIES] = {
    1000, 3000, 6000, 12000, 30000      /* 10s, 30s, 60s, 2min, 5min */
};
static int      g_dhcp_tries;
static uint32_t g_dhcp_next;            /* kpoll tick to try at       */
static int      g_dhcp_done;
static uint32_t g_dhcp_skipped_key;     /* attempts deferred for a key */

void net_dhcp_retry_reset(void)
{
    g_dhcp_tries = 0;
    g_dhcp_done  = 0;
    g_dhcp_next  = kpoll_ticks() + DHCP_BACKOFF_TICKS[0];
}

/* Exposed so `net` can print it rather than the user wondering why
   nothing is happening.  Returns -1 when the sequence has finished. */
int net_dhcp_tries(void){ return g_dhcp_done ? -1 : g_dhcp_tries; }
uint32_t net_dhcp_deferred(void){ return g_dhcp_skipped_key; }

static void net_dhcp_work(void)
{
    /* Nothing to do: the address was negotiated, or set by hand. */
    if(net_config_source() != NET_CFG_FALLBACK){ g_dhcp_done = 1; return; }
    if(g_dhcp_done) return;
    if(netdev_active() < 0) return;          /* no NIC to ask through  */
    if((int32_t)(kpoll_ticks() - g_dhcp_next) < 0) return;

    /* A NIC that can say it has no carrier is a NIC not worth asking.
       One that cannot say returns 1, so this costs nothing on a PCI
       NIC -- see the header. */
    if(!netdev_link_up(netdev_active())) return;

    if(keyboard_haschar()){ g_dhcp_skipped_key++; return; }

    klog(KLOG_INFO, "dhcp: no address yet, asking again (try %u of %u)",
         (uint32_t)(g_dhcp_tries + 1), (uint32_t)DHCP_MAX_TRIES);

    dhcp_request();

    if(net_config_source() != NET_CFG_FALLBACK){
        g_dhcp_done = 1;
        klog(KLOG_OK, "dhcp: address negotiated on try %u",
             (uint32_t)(g_dhcp_tries + 1));
        return;
    }

    g_dhcp_tries++;
    if(g_dhcp_tries >= DHCP_MAX_TRIES){
        g_dhcp_done = 1;
        /* Said once, not every interval.  A line printed on every
           healthy pass buries the one that matters -- 0.8.7.8's
           spurious-warning lesson, and `dmesg | grep` is how every
           hardware question in this tree has been answered. */
        klog(KLOG_WARN, "dhcp: no answer after %u tries, keeping the "
                        "built-in address (ifconfig set <ip> <gw>)",
             (uint32_t)DHCP_MAX_TRIES);
        return;
    }
    g_dhcp_next = kpoll_ticks() + DHCP_BACKOFF_TICKS[g_dhcp_tries];
}

/* ── carrier ─────────────────────────────────────────────────────────
 * A TRANSITION, reported once.  The state is kept per NIC index, and
 * it is reset when the NIC count changes, because netdev_unregister()
 * COMPACTS the table -- so index 1 after an unplug is a different NIC
 * from index 1 before it, and remembering its carrier would be the
 * v0.8.7 pinned-index bug in a new place. */
static uint8_t g_carrier[NETDEV_MAX];
static uint8_t g_carrier_known[NETDEV_MAX];
static int     g_carrier_ncount = -1;
static uint32_t g_carrier_events;

uint32_t net_carrier_events(void){ return g_carrier_events; }

static void net_carrier_work(void)
{
    int n = netdev_count();
    if(n != g_carrier_ncount){
        for(int i = 0; i < NETDEV_MAX; i++) g_carrier_known[i] = 0;
        g_carrier_ncount = n;
    }
    for(int i = 0; i < n && i < NETDEV_MAX; i++){
        int up = netdev_link_up(i) ? 1 : 0;
        if(!g_carrier_known[i]){
            g_carrier_known[i] = 1;
            g_carrier[i] = (uint8_t)up;
            continue;                 /* the first reading is not an event */
        }
        if(g_carrier[i] == (uint8_t)up) continue;
        g_carrier[i] = (uint8_t)up;
        g_carrier_events++;
        klog(up ? KLOG_OK : KLOG_WARN, "net: %s carrier %s",
             netdev_bus_name(i), up ? "up" : "down");
        /* A link that comes back is a reason to ask for an address
           again: an iPhone hotspot switched off and on hands out a
           different subnet, and the one we hold came from the old
           one. */
        if(up) net_dhcp_retry_reset();
    }
}

/* ── registration ────────────────────────────────────────────────────
 * Called from kernel/main.c once the NIC layer is up.  The intervals
 * are deliberately coarse: the DHCP client's own backoff decides when
 * it really acts, so its poll interval only has to be fine enough not
 * to add noticeable lag to that, and the carrier client's control
 * transfer is rate-limited to once a second inside un_link() anyway. */
void net_kpoll_register(void)
{
    net_dhcp_retry_reset();
    kpoll_register("net dhcp",    2000, net_dhcp_work);
    kpoll_register("net carrier", 1000, net_carrier_work);
}
