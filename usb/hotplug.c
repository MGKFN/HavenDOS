/* ════════════════════════════════════════════════════════════════════
   usb/hotplug.c -- a device plugged in after boot, noticed, v0.8.8

   "No USB hot-plug" has been an open item since 0.8.5 and the reason
   given was always the same one: nothing in this kernel ran
   periodically, so a port change was never looked at.  `usb rescan`
   exists because of it, and usb/hub.c says so in its own header.

   0.8.8 gives the kernel a tick (include/kpoll.h), so this is the
   client that uses it.  Two things live here.

   ── ONE COPY OF THE RESCAN SEQUENCE ─────────────────────────────────

   usb_rescan_all() is the body that was inside `usb rescan` in
   shell/usb.c.  It moved because this file needs it too, and two
   copies of a four-call sequence whose ORDER is the whole point would
   drift -- which is precisely what tools/check_diskname_sync.py was
   written to refuse one layer down, and what v0.8.6 did when it moved
   the disk table out of shell/iso_installer.c so a suite could LINK it
   rather than copy it.

   The order is the order the layers stack, and each step is there
   because of a bug:

     usbnet_release()   a NIC holds an INDEX into the core's device
                        table, and usb_enumerate() rebuilds that table.
                        v0.8.7.
     lockdown_forget()  the connection's transport is about to be torn
                        down; one that was not told would report itself
                        up.  v0.8.7.7.
     usbmux_close()     likewise, one layer below it.
     usb_enumerate()    the bus itself.  Issues Disable Slot for every
                        device first -- v0.8.6.1, found on the HP after
                        the third rescan lost a device.
     usb_hid_init()     v0.9.6.6. Keyboards and mice hold POINTERS into
                        the table usb_enumerate() just rebuilt, and this
                        sequence never re-bound them -- though hid.c's
                        own note said it did. On the HP: a mouse unplugged
                        and plugged back in never moved again, and any
                        rescan (a stick, the iPhone) could leave a HID
                        entry polling whatever device took its place.
     msc_rescan()       each mass-storage device holds a COPY of its
                        usb_dev_t, so a stale one addresses whatever
                        moved into that slot.
     blk_scan()         a disk that has gone away must not stay in the
                        installer's picker.
     usbnet_probe()     and the NIC comes back, with a fresh index.

   ── HOW A CHANGE IS DETECTED, AND WHAT IS NOT DETECTED ──────────────

   By polling the ROOT PORTS, through the port_connected() op both
   back-ends already supply.  On xHCI that is one MMIO read of PORTSC
   and a test of CCS; on UHCI one PORTSC read and a test of the same
   bit.  Pure reads, no write-1-to-clear, nothing new in either driver
   and nothing new in usb_hc_ops_t.

   THAT WAS A DELIBERATE CHOICE OVER THE CHANGE BITS.  xHCI's PORTSC
   has CSC, which latches a connect change and is cleared by writing
   1 -- it would catch an unplug-and-replug inside one interval, which
   a connected-bitmap comparison cannot.  It is not used, for the
   reason v0.8.7.8 cost a release: a guard that has never run against
   a real packet is not a guard.  A write-1-to-clear bit touched from
   a periodic poll is a write to a live controller register on a
   timer, on hardware nobody here has read the specification of, with
   the existing enumeration path also acking CSC during a reset --
   two writers of one latch.  A bitmap of connected ports is a read.
   When somebody has a port-change trace off real silicon, the bits
   are the better answer; until then they are a guess that writes.

   NOT DETECTED, and recorded rather than claimed:

     - A DEVICE BEHIND A HUB.  A hub reports port changes on its
       interrupt IN endpoint, which means a control transfer or an
       interrupt poll per hub per interval, and the HP cannot test any
       of it: two boots on 25 October had the stick on root port 0
       directly and `usb -vv` showed no hub anywhere -- this laptop's
       side sockets appear to be wired straight to the xHCI root
       ports.  0.8.7 shipped a NIC layer nothing could exercise and
       the next release was spent finding out what was wrong with it.
       Not again, in the same release, on a path with a control
       transfer in it.
     - AN UNPLUG AND REPLUG INSIDE ONE INTERVAL -- on UHCI. The bitmap
       is the same before and after. On xHCI, v0.9.7.3 sees it: see
       "THE REPLUG" below.

   ── THE REPLUG, v0.9.7.3 ────────────────────────────────────────────
   Kyan: "usb devices plugging back in out still an pain point". A
   person pulls a mouse and pushes it back in about a second; the
   bitmap read a second apart says "connected" both times and nothing
   happens, and the device -- which has been reset by its own unplug --
   answers nothing, because its slot was set up for the device that
   was there before.

   The change bit is still not CLEARED from here, for the reason above.
   It is READ (the optional port_changed op): port_reset() acknowledges
   it on every connected port whenever the bus is walked, so a port that
   was connected at the last walk, is connected now, and shows the bit
   set again, was disconnected and reconnected in between. That is a
   change, and the bus is walked; the walk acknowledges it again, so one
   replug is one rescan. A port that is EMPTY is ignored: nothing
   acknowledges an empty port, and its bit can stay set forever.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/kpoll.h"
#include "../include/usb.h"
#include "../include/klog.h"
#include "../include/types.h"

/* The layers above the core.  Declared rather than included: this file
   is the one place the rescan ORDER lives, and pulling in four headers
   to say so would invite somebody to call them in a different order
   somewhere else. */
extern int  msc_rescan(void);
extern int  blk_scan(void);
extern void usbnet_release(void);
extern int  usbnet_probe(void);
extern void lockdown_forget(void);
extern void usbmux_close(void);
extern void net_dhcp_retry_reset(void);
extern int  usb_hid_init(void);       /* usb/hid.c, v0.9.6.6 */
extern int  usb_hid_wanted(void);     /* kernel/main.c: `nousbhid` */

/* What `usb rescan` and the poll client both report. */
static usb_rescan_t g_last;

const usb_rescan_t *usb_rescan_result(void){ return &g_last; }

void usb_rescan_all(void)
{
    usbnet_release();
    lockdown_forget();
    usbmux_close();
    g_last.devices = usb_enumerate();
    if(usb_hid_wanted()) usb_hid_init();
    g_last.disks   = msc_rescan();
    g_last.blkdevs = blk_scan();
    g_last.nics    = usbnet_probe();
    g_last.hubs    = usb_hub_count();
    g_last.behind  = usb_hub_devices();
}

/* ── the port bitmap ─────────────────────────────────────────────────
 * One word per controller, one bit per root port.  USB_MAX_HC
 * controllers and 32 ports each; a controller reporting more than 32
 * root ports has its extra ports ignored, which is said out loud
 * because silently reading 32 of 40 ports is the kind of truncation
 * that looks like a dead socket. */
#define HP_MAX_PORTS 32
static uint32_t g_seen[USB_MAX_HC];
static uint8_t  g_have[USB_MAX_HC];
static uint32_t g_changes;          /* port changes observed            */
static uint32_t g_rescans;          /* rescans the poll client ran      */
static uint32_t g_truncated;        /* controllers with >32 root ports  */

uint32_t usb_hotplug_changes(void){ return g_changes; }
uint32_t usb_hotplug_rescans(void){ return g_rescans; }

/* ── v0.8.9.6: A STORM IS NOT A SERIES OF PLUG EVENTS ───────────────
 *
 * Every change rescans the WHOLE bus: every device is released and
 * enumerated again, the mouse and the keyboard included, and on the HP
 * that is hundreds of milliseconds of port resets during which nothing
 * on USB answers. Right for one plug event. Ruinous for a port that
 * flaps -- a device that drops and comes back every second, an iPhone
 * switching configurations mid-pair -- because then the bus is torn
 * down every second, the pointer freezes in time with it, and the log
 * fills with a dozen lines a pass. Choppy, and a full log, and
 * eventually `0 devices after hot-plug`, are one symptom.
 *
 * So the first change is still acted on at once, as before -- a person
 * plugging in a stick should not wait. But a change arriving when
 * HP_STORM_COUNT rescans already ran inside HP_STORM_WINDOW is a storm, and the client holds off: ten
 * seconds, then twenty, forty, eighty, for as long as it continues.
 * The hold is said once in the log, with the way out (`usb rescan`
 * still works whenever a person asks). A minute of quiet ends it.
 *
 * Changes seen during a hold are not lost: g_seen is not updated, so
 * the first pass after the hold sees the difference and rescans once. */
#define HP_STORM_COUNT   3
#define HP_STORM_WINDOW  500u           /* ticks: 5 s -- a flap is ~1 s */
#define HP_HOLD_FIRST    1000u          /* 10 s                         */
#define HP_HOLD_MAX      8000u          /* 80 s                         */
#define HP_QUIET_RESET   6000u          /* 60 s without a rescan        */
static uint32_t g_rescan_at[HP_STORM_COUNT];   /* ring of recent rescans */
static int      g_rescan_n;
static uint32_t g_hold_until;
static uint32_t g_hold_len;
static uint32_t g_holds;                /* storms detected, for the tests */
static int      g_hold_said;

/* ── and a rescan that finds nothing gets one more try ───────────────
 * `xHCI slot 2 ep0: USB Transaction Error (cc 4)` followed by
 * `usb: 0 devices ... after hot-plug` on a bus with things plainly
 * plugged into it. A device caught mid-reset by the walk answers
 * nothing, and when the walk was triggered by that same device
 * flapping, everything is caught. One retry, a second later, when the
 * ports say something is connected and the table says nothing is. Not
 * more than one: a bus that is genuinely broken should be reported,
 * not hammered. */
static int      g_retry_owed;
static uint32_t g_retries;

/* ── v0.9.7.6: AND A WALK THAT FINDS SOME, BUT NOT ALL ───────────────
 * Kyan, 23 Nov, on 0.9.7.5: "only one USB works after the unplugs,
 * plugging back in works".
 *
 * The retry above only ever fired when the walk found NOTHING. A walk
 * that found the stick and lost the keyboard -- the keyboard caught by
 * the reset and slower to come back than the walk was to give up on it
 * -- found one device, which is not zero, so nothing tried again and
 * the keyboard stayed dead until the next plug event walked the bus.
 *
 * Now it is per port: after any walk, an OCCUPIED root port with no
 * device in the table owes a retry. Two of them, one second and then
 * two seconds later, and then a sentence naming the port. A plug event
 * gives a fresh pair; a retry does not, so a device that is genuinely
 * dead costs two extra walks per plug event and is not hammered. */
#define HP_RETRIES 2
static const uint32_t g_retry_gap[HP_RETRIES] = { 100u, 200u };   /* ticks */
static int      g_retry_left;
static uint32_t g_retry_at;

uint32_t usb_hotplug_holds(void)  { return g_holds; }
uint32_t usb_hotplug_retries(void){ return g_retries; }

static int storm_now(uint32_t now)
{
    int i, recent = 0;
    for(i = 0; i < g_rescan_n && i < HP_STORM_COUNT; i++)
        if(now - g_rescan_at[i] < HP_STORM_WINDOW) recent++;
    return recent >= HP_STORM_COUNT;
}

static void note_rescan(uint32_t now)
{
    /* Quiet for long enough: the next storm starts from the shortest
       hold again. */
    if(g_rescan_n > 0){
        uint32_t last = g_rescan_at[(g_rescan_n - 1) % HP_STORM_COUNT];
        if(now - last >= HP_QUIET_RESET) g_hold_len = 0;
    }
    g_rescan_at[g_rescan_n % HP_STORM_COUNT] = now;
    g_rescan_n++;
}

/* Is a device that enumerated last time on this root port? A port whose
   device did NOT answer (the HP's port 12 says "no answer (-4)" every
   boot) can drop and reconnect as a consequence of being reset, which
   sets its change bit again; acting on that would walk the bus every
   second, for ever. Only a port that had a working device counts. */
static int port_had_device(int hc, int port)
{
    int n = usb_device_count();
    for(int i = 0; i < n; i++){
        const usb_dev_t *d = usb_device(i);
        if(d && d->hc == hc && (d->parent < 0 ? d->port == port : d->rootport == port)) return 1;
    }
    return 0;
}

/* connected ports whose change bit is set: see THE REPLUG above */
static uint32_t replugged_of(const usb_hc_t *h, int hc, uint32_t connected)
{
    uint32_t bits = 0;
    int n = h->ops->port_count ? h->ops->port_count((usb_hc_t *)h) : 0;
    if(n > HP_MAX_PORTS) n = HP_MAX_PORTS;
    if(!h->ops->port_changed) return 0;
    for(int p = 0; p < n; p++)
        if((connected >> p) & 1u)
            if(h->ops->port_changed((usb_hc_t *)h, p) && port_had_device(hc, p))
                bits |= (uint32_t)1u << p;
    return bits;
}

static uint32_t ports_of(const usb_hc_t *h, int *n_out)
{
    uint32_t bits = 0;
    int n = h->ops->port_count ? h->ops->port_count((usb_hc_t *)h) : 0;
    if(n > HP_MAX_PORTS){ g_truncated++; n = HP_MAX_PORTS; }
    if(n_out) *n_out = n;
    if(!h->ops->port_connected) return 0;
    for(int p = 0; p < n; p++)
        if(h->ops->port_connected((usb_hc_t *)h, p))
            bits |= (uint32_t)1u << p;
    return bits;
}

/* Take a reading WITHOUT acting on it.  Called once at registration so
   the first poll does not read every occupied port as a brand-new
   arrival and rescan a bus that has just been enumerated. */
void usb_hotplug_baseline(void)
{
    int nhc = usb_hc_count();
    for(int c = 0; c < nhc && c < USB_MAX_HC; c++){
        const usb_hc_t *h = usb_hc(c);
        if(!h || !h->ops) continue;
        /* v0.9.7.4: clear the change bits the walk itself set (a USB 2
           device can drop and come back while its port is reset), so
           only a change after this point reads as a replug. On the HP
           0.9.7.3 took those for replugs and walked again and again. */
        if(h->ops->port_ack_change){
            int n = h->ops->port_count ? h->ops->port_count((usb_hc_t *)h) : 0;
            for(int p = 0; p < n && p < HP_MAX_PORTS; p++)
                h->ops->port_ack_change((usb_hc_t *)h, p);
        }
        g_seen[c] = ports_of(h, 0);
        g_have[c] = 1;
    }
}

static void usb_hotplug_work(void)
{
    int nhc = usb_hc_count();
    int changed = 0;

    for(int c = 0; c < nhc && c < USB_MAX_HC; c++){
        const usb_hc_t *h = usb_hc(c);
        if(!h || !h->ops) continue;
        uint32_t now = ports_of(h, 0);
        if(!g_have[c]){ g_seen[c] = now; g_have[c] = 1; continue; }
        if(now == g_seen[c]){
            uint32_t back = replugged_of(h, c, now & g_seen[c]);
            if(!back) continue;
            for(int p = 0; p < HP_MAX_PORTS; p++){
                if(!(back & ((uint32_t)1u << p))) continue;
                g_changes++;
                klog(KLOG_INFO, "usb: %s port %u: device unplugged and plugged back in",
                     usb_hc_name(h), (uint32_t)p);
            }
            changed = 1;
            continue;
        }

        /* Report the direction, per port, before rescanning -- a bus
           walk prints a dozen lines and the one fact worth having is
           which socket moved. */
        uint32_t diff = now ^ g_seen[c];
        for(int p = 0; p < HP_MAX_PORTS; p++){
            if(!(diff & ((uint32_t)1u << p))) continue;
            g_changes++;
            klog(KLOG_INFO, "usb: %s port %u: device %s",
                 usb_hc_name(h), (uint32_t)p,
                 (now & ((uint32_t)1u << p)) ? "arrived" : "removed");
        }
        /* ── g_seen IS NOT UPDATED HERE, ON PURPOSE ────────────────
           It was, and the redundancy made both writes untestable.
           usb_hotplug_baseline() runs after the rescan and records the
           settled reading, so this assignment and that call each
           masked the other: delete either one alone and the behaviour
           is identical, which is why tools/check_hotplug_suite.py's
           injections into BOTH were MISSED.  That is v0.8.6.7's
           failing-only-in-pairs shape, and four of test_netdev's seven
           holes were the same thing.

           The post-rescan reading is the one that has to win anyway:
           enumeration resets ports, so a value read here -- in the
           middle of a change -- can disagree with the settled one, and
           recording the mid-change value is what would make one plug
           event rescan twice. */
        changed = 1;
    }

    {
        uint32_t now = kpoll_ticks();

        if(!changed){
            if(!g_retry_owed) return;
            if((int32_t)(now - g_retry_at) < 0) return;
            g_retry_owed = 0;
            g_retries++;
            klog(KLOG_WARN, "usb: a port is occupied and nothing on it "
                            "answered -- walking the bus again (%d of %d)",
                 HP_RETRIES - g_retry_left, HP_RETRIES);
        } else {
            if((int32_t)(now - g_hold_until) < 0) return;  /* held */
            if(storm_now(now)){
                g_hold_len = g_hold_len ? g_hold_len * 2u : HP_HOLD_FIRST;
                if(g_hold_len > HP_HOLD_MAX) g_hold_len = HP_HOLD_MAX;
                g_hold_until = now + g_hold_len;
                g_holds++;
                g_hold_said = 0;
            }
            if((int32_t)(now - g_hold_until) < 0){
                if(!g_hold_said){
                    g_hold_said = 1;
                    klog(KLOG_WARN, "usb: ports keep changing -- rescans "
                         "held off for %u s (`usb rescan` forces one)",
                         g_hold_len / 100u);
                }
                return;
            }
        }
        note_rescan(now);
    }

    g_rescans++;
    usb_rescan_all();
    klog(KLOG_OK, "usb: %u device%s, %u disk%s, %u NIC%s after hot-plug",
         (uint32_t)g_last.devices, g_last.devices == 1 ? "" : "s",
         (uint32_t)g_last.disks,   g_last.disks   == 1 ? "" : "s",
         (uint32_t)g_last.nics,    g_last.nics    == 1 ? "" : "s");

    /* The bitmap is re-read after the rescan, not trusted from before
       it: enumeration resets ports and the reading taken mid-change
       can disagree with the settled one.  Without this, one plug event
       could rescan twice. */
    usb_hotplug_baseline();

    /* An occupied root port with nothing in the table: owe a retry,
       while the plug event's pair lasts -- see v0.9.7.6 above. */
    if(changed) g_retry_left = HP_RETRIES;
    {
        int missing = 0;
        for(int c = 0; c < nhc && c < USB_MAX_HC; c++){
            if(!g_have[c]) continue;
            for(int p = 0; p < HP_MAX_PORTS; p++)
                if(((g_seen[c] >> p) & 1u) && !port_had_device(c, p)){
                    missing = 1;
                    if(!g_retry_left)
                        klog(KLOG_WARN, "usb: %s port %u: something is plugged in "
                             "and does not answer -- unplug it and plug it back in",
                             usb_hc_name(usb_hc(c)), (uint32_t)p);
                }
        }
        g_retry_owed = 0;
        if(missing && g_retry_left > 0){
            g_retry_at = kpoll_ticks() + g_retry_gap[HP_RETRIES - g_retry_left];
            g_retry_left--;
            g_retry_owed = 1;
        }
    }

    /* A NIC that has just arrived needs an address, and the compile-
       time fallback is QEMU's.  v0.8.7.2 added this to `usb rescan`;
       here it is the same reasoning with nobody having typed
       anything. */
    if(g_last.nics > 0) net_dhcp_retry_reset();
}

/* ── registration ────────────────────────────────────────────────────
 * One second.  The cost per poll is one MMIO read per root port --
 * the HP's xHCI has twelve -- and a second is far below the time it
 * takes a person to plug something in and look at the screen. */
void usb_kpoll_register(void)
{
    /* A fresh client starts with no storm behind it. */
    g_rescan_n = 0; g_hold_until = 0; g_hold_len = 0; g_hold_said = 0;
    g_retry_owed = 0; g_retry_left = 0;
    usb_hotplug_baseline();
    kpoll_register("usb ports", 1000, usb_hotplug_work);
}

uint32_t usb_hotplug_truncated(void){ return g_truncated; }
