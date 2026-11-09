/*
 * usb.c — what the USB controllers and bus are actually doing
 *
 * `lspci` answers "is a controller there".  This answers the next three
 * questions, which are the ones you need when a USB stick does not show up:
 * is the controller RUNNING, is anything plugged in, and did the device
 * answer when we spoke to it.
 *
 * It also names a controller this kernel cannot drive, and says when the
 * ports were taken back from an EHCI.  A machine newer than about 2009 has
 * EHCI or xHCI, and a USB 2.0 controller is normally an EHCI with UHCI
 * companions sharing its sockets -- on either of those, a bare "no USB"
 * would be a lie that sends you looking at the driver instead of the
 * hardware.
 *
 *   usb        controllers, ports in use, and the devices that answered
 *   usb -v     the raw registers as well
 *   usb -vv    and a line for every port, including the empty ones
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/pci.h"
#include "../include/uhci.h"
#include "../include/usb.h"
#include "../include/ui.h"
/* v0.8.9.1: this file called lockdown_reply() through an IMPLICIT
   DECLARATION, which the compiler takes as returning int -- on this
   ABI a pointer survives that and the next person to change a
   signature finds out the hard way. include/fat16.h's note records the
   same mistake costing a release. Include the header instead. */
#include "../include/lockdown.h"

extern void utoa(uint32_t v, char *buf, int base);

/* ── paging, v0.8.5.1 ────────────────────────────────────────────────
   `usb -vv` on a laptop with a hub-less xHCI, two ports and a keyboard
   is already longer than 25 rows, and on the HP it had to be
   photographed to be read -- which is precisely why shell/lspci.c was
   paged in v0.8.5. It uses the same shared pager in shell/ui.c, sized
   from vga_rows().

   Every print in this file goes through up() rather than up(), so
   that one place counts lines. The alternative -- a page check after
   each of the forty-odd prints below -- needs an early return from
   inside three nested loops, and the version of that which gets written
   in a hurry returns from the inner loop only and pages forever. A stop
   FLAG instead: once the user presses Q, up() keeps being called and
   prints nothing, and the command finishes normally. */
static int g_stop;

/* Defined below; used by the lockdown steps above it, and by
   shell/paircmd.c, which hits the same failures. */
void lockdown_show_reply(void);

static void up(const char *s)
{
    if (g_stop) return;
    vga_puts(s);
    for (const char *q = s; *q; q++)
        if (*q == '\n' && !ui_page_line()) { g_stop = 1; return; }
}

static void put_hex(uint32_t v, int digits)
{
    const char *hex = "0123456789ABCDEF";
    char buf[9];
    int i = 0;
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        buf[i++] = hex[(v >> shift) & 0xF];
    buf[i] = 0;
    up(buf);
}

static void put_dec(uint32_t v)
{
    char b[12];
    utoa(v, b, 10);
    up(b);
}

/* The device class byte.  A USB flash drive reports class 0 here and
   declares itself mass storage in its INTERFACE descriptor, which this
   kernel does not read yet -- so say that, rather than print "00" and leave
   it looking like the device failed to identify itself. */
static const char *class_name(uint8_t c)
{
    switch (c) {
    case 0x00: return "in the interface descriptor";
    case 0x01: return "audio";
    case 0x02: return "communications";
    case 0x03: return "human interface";
    case 0x07: return "printer";
    case 0x08: return "mass storage";
    case 0x09: return "hub";
    case 0x0E: return "video";
    case 0xE0: return "wireless";
    case 0xFF: return "vendor specific";
    default:   return "unknown";
    }
}

/* v0.8.5. Rewritten onto usb/core.c: this command used to ask
   drivers/uhci.c directly, which meant it could only ever describe one
   kind of controller and had to be edited again for the next one. It now
   asks the core, so every controller the kernel can drive is listed by
   whatever usb_hc_name() calls it, and nothing here knows what kinds
   there are. */

extern uint32_t xhci_portsc_raw(int idx, int port);

/* ── printing one device, at a depth ──────────────────────────────────
 *
 * v0.8.6. Until this release every device was on a root port, so one
 * fixed indent said everything there was to say about where it was.
 * usb/hub.c walks hubs now, and a stick two tiers down under a monitor's
 * built-in hub is not the same thing as one in the side socket --
 * `usb -vv` is the screen somebody reads to find out WHY their stick did
 * not appear, so the shape of the tree has to be on it.
 *
 * Indented by tier, with the route string on the line, because the route
 * is the thing that is wrong when a device behind a hub does not answer
 * and there is no error anywhere to say so.
 */
static void pad(int depth)
{
    int i;
    for (i = 0; i < 8 + depth * 4; i++) up(" ");
}

static int i_of(const usb_dev_t *d)
{
    int i;
    for (i = 0; i < usb_device_count(); i++)
        if (usb_device(i) == d) return i;
    return -1;
}

static void print_dev(const usb_dev_t *d, int depth, int verbose)
{
    int i;

    pad(depth);
    up("addr ");  put_dec(d->addr);
    up("  ");     put_hex(d->vendor, 4);
    up(":");      put_hex(d->product, 4);
    up("  rev "); put_hex(d->release, 4);
    up("  ");
    up(d->speed == USB_SPEED_LOW   ? "low-speed"   :
       d->speed == USB_SPEED_HIGH  ? "high-speed"  :
       d->speed == USB_SPEED_SUPER ? "super-speed" :
                                     "full-speed");
    up("\n");

    /* Where it is, for anything not on a root port. The route string is
       what the controller uses to reach it, so a device that does not
       answer and a route that looks wrong are the same screen. */
    if (d->tier) {
        pad(depth);
        up("behind the hub at addr "); put_dec(d->parent >= 0
                                               ? usb_device(d->parent)->addr
                                               : 0);
        up(" port "); put_dec((uint32_t)d->parent_port + 1);
        up("  tier "); put_dec(d->tier);
        up("  route "); put_hex(d->route, 5);
        up("\n");
        if (d->tt_slot) {
            pad(depth);
            up("through a transaction translator on port ");
            put_dec(d->tt_port);
            up("\n");
        }
    }

    pad(depth);
    up("class ");
    put_hex(d->dev_class, 2);    up("/");
    put_hex(d->dev_subclass, 2); up("/");
    put_hex(d->dev_protocol, 2);
    up("  ");
    up(class_name(d->if_class ? d->if_class : d->dev_class));
    up("\n");
    pad(depth);
    up("endpoint 0 packet size ");
    put_dec(d->maxpacket0);
    up("  configurations ");
    put_dec(d->nconfigs);
    up("\n");

    if (d->config) {
        pad(depth);
        up("interface ");  put_dec(d->ifnum);
        up("  class ");    put_hex(d->if_class, 2);
        up("/");           put_hex(d->if_subclass, 2);
        up("/");           put_hex(d->if_protocol, 2);
        up("  ");          put_dec((uint32_t)d->nep);
        up(d->nep == 1 ? " endpoint\n" : " endpoints\n");
        if (verbose) {
            for (i = 0; i < d->nep; i++) {
                const usb_ep_t *e = &d->ep[i];
                pad(depth);
                up("  ep 0x"); put_hex(e->addr, 2);
                up("  ");
                up((e->attr & 3) == USB_EP_BULK ? "bulk " :
                   (e->attr & 3) == USB_EP_INTR ? "intr " :
                   (e->attr & 3) == USB_EP_ISOC ? "isoc " :
                                                  "ctrl ");
                up((e->addr & 0x80) ? "in " : "out");
                up("  max "); put_dec(e->mps);
                up("  interval "); put_dec(e->interval);
                up("\n");
            }
        }
    } else {
        pad(depth);
        up("no configuration was accepted.\n");
    }
}

/* Everything attached to device `parent`, and everything under that.
   Bounded by USB_MAX_TIER, which is the core's bound too -- a tree that
   claimed to be deeper than the route string can express would be a hub
   reporting itself as its own child. */
static void print_children(int parent, int depth, int verbose)
{
    int i;
    if (parent < 0 || depth > USB_MAX_TIER) return;
    for (i = 0; i < usb_device_count(); i++) {
        const usb_dev_t *e = usb_device(i);
        if (!e || !e->valid || e->parent != (int8_t)parent) continue;
        print_dev(e, depth, verbose);
        print_children(i, depth + 1, verbose);
    }
}

static void no_controller(void)
{
    uint8_t progif = 0;

    /* uhci_other_controller() answers "is there USB hardware this kernel
       does not drive". Since v0.8.5 it drives xHCI too, so reaching here
       with an xHCI present means that one was found and REFUSED TO START
       -- a completely different thing from an unsupported controller, and
       worth saying so rather than repeating the old sentence. */
    if (uhci_other_controller(&progif)) {
        up("No USB controller this kernel could start.\n\n");
        up("There IS a USB controller, programming interface 0x");
        put_hex(progif, 2);
        up(progif == 0x20 ? " (EHCI)\n" :
                 progif == 0x10 ? " (OHCI)\n" :
                 progif == 0x30 ? " (xHCI)\n" : "\n");
        up("\n");
        if (progif == 0x30) {
            up("xHCI is driven from v0.8.5, so this one was found and\n");
            up("did not come up. 'dmesg' has the reason it gave.\n");
        } else {
            up("UHCI and xHCI are driven. EHCI and OHCI are not yet.\n");
            up("The transfer and enumeration layers above the\n");
            up("controller are the same either way.\n");
        }
    } else {
        up("No USB controller found.\n");
        up("Run 'lspci' to see what is on the bus.\n");
    }
}


/* ── `usb cfg <addr>`, v0.8.6.4 ──────────────────────────────────────
   Every configuration a device offers, and every interface in each.

   WHY: an iPhone enumerated on the HP as class 06 -- Still Image, which
   is PTP, and which is the phone's DEFAULT configuration. USB Ethernet
   tethering lives in a different one. usb/core.c has always asked for
   descriptor index 0 because every device this kernel had met offered
   exactly one configuration, and `usb -vv` can therefore only ever show
   the one in use.

   This SELECTS NOTHING. It issues no SET_CONFIGURATION and does not
   modify the device table: surveying a phone that is working must not
   stop it working. What it changes is what can be seen. */
static int g_cfg_ifaces;

static void cfg_iface(void *ctx, uint8_t num, uint8_t alt, uint8_t nep,
                      uint8_t cls, uint8_t sub, uint8_t proto)
{
    (void)ctx;
    g_cfg_ifaces++;
    up("      interface "); put_dec(num);
    if (alt) { up(" alt "); put_dec(alt); }
    up("  class ");  put_hex(cls, 2);
    up("/");         put_hex(sub, 2);
    up("/");         put_hex(proto, 2);
    up("  ");        put_dec(nep);
    up(nep == 1 ? " endpoint  " : " endpoints  ");
    up(class_name(cls));
    /* The one this whole command exists to find. Apple's tethering
       interface is vendor-specific 255/253/1 -- there is no class code
       for it, so nothing else on screen would mark it out. */
    if (cls == 0xFF && sub == 0xFD && proto == 0x01)
        up("   <-- APPLE USB ETHERNET");
    up("\n");
}

/* ── `usb mux`, v0.8.7.3 ────────────────────────────────────────────────
 *
 * The Apple Mobile Device channel, and the first thing in this tree
 * that talks to the half of the phone which decides whether tethering
 * is allowed at all.
 *
 * 26 October settled why v0.8.7's Ethernet driver never carried a
 * frame: every rescan re-prompted for trust and the hotspot indicator
 * never appeared, because iOS does not turn tethering on for a host it
 * has no pairing record for. The descriptors cooperate; the device
 * does not.
 *
 * This sends the VERSION packet and prints whatever comes back,
 * unparsed. If the device answers, the channel works and pairing is a
 * long road with a known start. If it does not, this is where the road
 * ends and the money goes on an adapter instead.
 */
static void do_mux(void)
{
    extern int usbmux_open(void);
    extern int usbmux_handshake(void);
    extern int usbmux_dump(void);
    extern int usbmux_present(void);
    int r;

    if (!usbmux_present()) {
        up("Opening the Apple Mobile Device interface...\n");
        r = usbmux_open();
        if (r < 0) {
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            up("No Apple Mobile Device interface on the bus.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            up("An iPhone must be plugged in and unlocked.  If it is,\n");
            up("'usb cfg' will show a class FF/FE/02 interface on it;\n");
            up("if that is missing, this kernel cannot reach lockdown.\n");
            return;
        }
    }

    up("Sending VERSION...\n");
    r = usbmux_handshake();
    up("\n");
    usbmux_dump();
    up("\n");

    if (r > 0) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        up("The device answered.  The channel works.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("Next is lockdownd QueryType, then the pair request.\n");
    } else if (r == 0) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        up("No answer.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("The interface is claimed and the packet went out, so the\n");
        up("framing above is the thing to doubt -- the last packet\n");
        up("line is raw, before this driver decided anything.\n");
    } else {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        up("The exchange failed.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("'dmesg | grep usbmux' names which step refused.\n");
    }
}

/* ── `usb lockdown`, v0.8.7.7 -- STAGE 1 ────────────────────────────────
 *
 * The one boot that matters for this release.
 *
 * `usb mux` proved the transport on 26 October: the phone answered a
 * VERSION packet with twenty bytes, the first thing it has sent back
 * over any channel in any release. That closed stage 0. This is stage
 * 1: open a TCP connection to lockdownd on port 62078 and ask it what
 * it is.
 *
 * Three outcomes, and they are not the same fact:
 *
 *   - the channel will not open                   nothing above works
 *   - the port answers RST                        lockdownd is not there
 *   - QueryType returns com.apple.mobile.lockdown STAGE 1 IS DONE
 *
 * The third one needs no cryptography and changes nothing on the
 * phone. Stage 2 is one more request on the same connection; stage 3
 * is where RSA and X.509 arrive and is not started until 1 and 2
 * answer.
 *
 * As with `usb mux`, the raw reply is printed before this screen says
 * anything about what it means. The framing is read off
 * libimobiledevice and a layer that silently parses cannot be asked
 * whether it parsed the right thing.
 */
static void do_lockdown(void)
{
    extern int  lockdown_connect(void);
    extern int  lockdown_query_type(void);
    extern int  lockdown_dump(void);
    extern const char *lockdown_type(void);
    extern const char *lockdown_error(void);
    extern int  lockdown_get_value(const char *key);
    extern int  lockdown_value_len(void);
    extern int  usbmux_refused(void);
    extern int  usbmux_present(void);
    int r;

    up("Opening a connection to lockdownd on port 62078...\n");
    r = lockdown_connect();
    if (r < 0) {
        up("\n");
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        /* ── NO INTERFACE IS A DIFFERENT SCREEN, v0.8.7.7 ───────────
           Found by booting it: harness/lockdown_boot.py runs this on a
           machine with no iPhone, and the first version printed the
           generic failure -- which sends the reader to doubt the TCP
           framing when the framing was never reached.

           If the channel was never claimed, the cause is one step
           earlier and `usb mux` already has the right words for it.
           Three causes behind one screen is the same mistake
           x_control() made with one error number, in the one place
           where it costs a trip to the laptop. */
        if (!usbmux_present()) {
            up("No Apple Mobile Device interface on the bus.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            up("An iPhone must be plugged in and unlocked.  If it is,\n");
            up("'usb cfg' will show a class FF/FE/02 interface on it;\n");
            up("if that is missing, this kernel cannot reach lockdown.\n");
        } else if (usbmux_refused()) {
            up("The device REFUSED the port.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            up("That is an answer: the mux channel works and nothing\n");
            up("is listening on 62078.  An iPhone always listens, so\n");
            up("this says the connection reached something else.\n");
        } else {
            up("The connection could not be made.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            up("'usb mux' first -- if the device answers a VERSION the\n");
            up("transport is fine and the TCP framing is the suspect.\n");
            up("'dmesg | grep -e usbmux -e lockdown' names the step.\n");
        }
        return;
    }

    up("Sending QueryType...\n\n");
    r = lockdown_query_type();
    lockdown_dump();
    if (r != 1) lockdown_show_reply();
    up("\n");

    if (r == 1) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        up("LOCKDOWND ANSWERED.  Stage 1 is done.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);

        /* ── STAGE 2, ON THE SAME CONNECTION ─────────────────────────
           lockdownd answers several requests on one connection and
           stages 1 and 2 are one conversation, so there is nothing to
           be gained by making the user run a second command -- and
           something to lose: a second connection is a second SYN and
           another chance for the thing being tested to be the
           transport rather than the request. */
        up("\nStage 2: GetValue DevicePublicKey...\n\n");
        r = lockdown_get_value("DevicePublicKey");
        lockdown_dump();
        if (r != 1) lockdown_show_reply();
        up("\n");

        if (r == 1) {
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            up("GOT THE DEVICE KEY.  Stage 2 is done.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            put_dec((uint32_t)lockdown_value_len());
            up(" bytes, and the first line above says whether it is\n");
            up("a PEM RSA public key.  Stage 3 is the pair request,\n");
            up("and that is where RSA and X.509 arrive.\n");
        } else if (r == 0) {
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            up("It answered, but gave no value.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            if (lockdown_error()[0]) {
                up("The reply carried an Error, so the framing was\n");
                up("right and lockdownd refused the request.  If it\n");
                up("names pairing or trust, stage 2 needs stage 3.\n");
            } else {
                up("No Value in it.  What came is printed above.\n");
            }
        } else {
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            up("Stage 2 failed where stage 1 worked.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            up("Same connection and the same framing, so the reply\n");
            up("is the difference -- GetValue returns <data>, which\n");
            up("is base64 and wrapped across lines.\n");
            up("What arrived, if anything, is printed above.\n");
        }
    } else if (r == 0) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        up("It answered, but not with the lockdown type.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        if (lockdown_error()[0]) {
            up("The reply carried an Error, which means the framing\n");
            up("was right and lockdownd refused the request itself.\n");
        } else if (lockdown_type()[0]) {
            up("The reply carried a Type this kernel did not expect.\n");
            up("What arrived is printed above.\n");
        } else {
            up("No Type in it.  What arrived is printed above --\n");
            up("the hex says what it is when the text does not, and\n");
            up("the prefix line says whether the framing agrees.\n");
        }
    } else {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        up("The exchange failed.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("The connection opened, so the transport and the TCP\n");
        up("handshake both worked.  What is left is the plist\n");
        up("framing: 'dmesg | grep lockdown' names which part.\n");
    }
}

/* ── `usb eth`, v0.8.7 ────────────────────────────────────────────────
 *
 * The state of every Apple USB Ethernet NIC, and -- the reason this
 * command exists -- THE RAW FIRST BYTES of the next transfer that
 * arrives on it.
 *
 * usb/ethernet.c's framing is read off the published Linux driver: two
 * bytes prepended to a received frame, a fixed 1514-byte padded
 * transmit. No Apple document says so and this kernel has never
 * exchanged a byte with the interface. A receive path that silently
 * strips two bytes cannot be asked whether there were two bytes to
 * strip, so the driver keeps the first transfer as it arrived and this
 * prints it.
 *
 * v0.8.6.6 is why this is a named argument rather than a flag: `usb`
 * used to fall through to the plain bus listing for an argument it did
 * not know, so `usb cfg` on an older kernel printed a normal screen and
 * looked exactly like success.
 */
static void do_eth(void)
{
    extern int usbnet_dump(void);
    extern int usbnet_count(void);

    if (usbnet_count() == 0) {
        up("No USB network adapter.\n");
        up("An iPhone must be unlocked and trusted, with Personal\n");
        up("Hotspot ON.  As of v0.8.8 a device plugged into a root\n");
        up("port is noticed within a second on its own; one behind a\n");
        up("hub still needs 'usb rescan'.\n");
        return;
    }

    up("USB network adapters:\n");
    usbnet_dump();
    up("\n");
    up("The last line of each is the FIRST TRANSFER as it arrived,\n");
    up("before the driver stripped anything.  An Ethernet frame\n");
    up("starts with a six-byte destination, so on a broadcast it\n");
    up("reads FF FF FF FF FF FF -- and whatever sits in front of\n");
    up("that is the framing this driver is guessing at.\n");
}

static void do_cfg(const char *arg)
{
    int bad_desc = 0;
    int n = usb_device_count(), i, want = -1, found = 0;

    if (arg && *arg){
        want = 0;
        for (; *arg; arg++){
            if (*arg < '0' || *arg > '9'){ want = -1; break; }
            want = want * 10 + (*arg - '0');
        }
    }

    for (i = 0; i < n; i++){
        const usb_dev_t *cd = usb_device(i);
        usb_dev_t *d;
        int ci;

        if (!cd || !cd->valid) continue;
        if (want >= 0 && (int)cd->addr != want) continue;
        found++;

        /* usb_survey_config() takes a non-const device because the
           control transfer needs it; it does not modify one. */
        d = (usb_dev_t *)cd;

        up("addr "); put_dec(d->addr);
        up("  ");    put_hex(d->vendor, 4);
        up(":");     put_hex(d->product, 4);
        up("  ");    put_dec(d->nconfigs);
        up(d->nconfigs == 1 ? " configuration" : " configurations");
        up(", in use "); put_dec(d->config);
        up("\n");

        for (ci = 0; ci < (int)d->nconfigs; ci++){
            int r;
            up("  configuration index "); put_dec((uint32_t)ci);
            up(":\n");
            g_cfg_ifaces = 0;
            r = usb_survey_config(d, (uint8_t)ci, cfg_iface, 0);
            if (r < 0){
                up("      the device would not describe it (");
                put_dec((uint32_t)(-r));
                up(")\n");
                /* v0.8.7.4: -3 is USB_EXFER, which the xHCI driver
                   returns for EVERY completion code that is not
                   success, short-packet or stall -- at least six
                   different faults wearing one number. The driver
                   names the real one in the log now. */
                if (r == -3) bad_desc++;
            } else if (g_cfg_ifaces == 0){
                up("      no interfaces in it\n");
            }
        }
    }

    if (!found){
        up(want >= 0 ? "no device at that address\n"
                     : "no devices on the bus\n");
        up("'usb rescan' walks the bus again.\n");
    }
    if (bad_desc) {
        up("\n");
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        put_dec((uint32_t)bad_desc);
        up(bad_desc == 1 ? " descriptor read failed.\n"
                         : " descriptor reads failed.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("(3) is every controller fault but a stall wearing one\n");
        up("number.  'dmesg | grep xHCI' names the real one -- a\n");
        up("transaction error is a device not answering, a context\n");
        up("state error is an endpoint left in the wrong state.\n");
    }
}


/* ── decoding PORTSC, v0.8.6.7 ───────────────────────────────────────
   `usb -vv` printed a 32-bit hex word and, for a port that did not
   enumerate, "attached, but it did not answer." Those two lines
   together are an unreadable number and a shrug, and they are the whole
   evidence available when a stick is in the socket and nothing happens.

   The bits are not secret. A port that is connected but NOT ENABLED
   never finished training, which is a different fault from one that is
   enabled and did not answer a descriptor request -- and a port that is
   not POWERED is a third thing again. Printing them in words is the
   difference between "idk why" and a diagnosis.

   xHCI PORTSC, section 5.4.8:
     bit 0   CCS  current connect status
     bit 1   PED  port enabled/disabled
     bit 3   OCA  over-current active
     bit 4   PR   port reset
     bits 5-8    PLS  port link state
     bit 9   PP   port power
     bits 10-13  port speed (an index into the speed ID table)

   The speed IDs below are the xHCI DEFAULT ones. A controller MAY
   publish its own table through a Supported Protocol capability, so
   these are labelled as what they usually are rather than asserted --
   getting that wrong on screen is better than silently wrong, and the
   raw word is printed beside it either way. */
static void put_pls(uint32_t pls)
{
    static const char *N[] = {
        "U0 active", "U1", "U2", "U3 suspended",
        "disabled", "RxDetect", "inactive", "polling",
        "recovery", "hot reset", "compliance", "test",
        "?", "?", "?", "resume"
    };
    up(N[pls & 0xF]);
}

static void put_portsc_words(uint32_t v, int is_xhci)
{
    if (!is_xhci) return;       /* UHCI's bits are a different register */

    up("        ");
    up((v & 1u) ? "connected" : "not connected");
    up((v & 2u) ? ", enabled" : ", NOT ENABLED");
    up((v & (1u << 9)) ? ", powered" : ", NOT POWERED");
    if (v & (1u << 3)) up(", OVER-CURRENT");
    if (v & (1u << 4)) up(", in reset");
    up(", link ");
    put_pls((v >> 5) & 0xFu);
    {
        uint32_t sp = (v >> 10) & 0xFu;
        up(", ");
        switch (sp) {
            case 1: up("full-speed");   break;
            case 2: up("low-speed");    break;
            case 3: up("high-speed");   break;
            case 4: up("SuperSpeed");   break;
            case 5: up("SuperSpeed+");  break;
            default: up("speed id ");   put_dec(sp); break;
        }
    }
    up("\n");
}

/* ── WHAT ACTUALLY ARRIVED, ON THE SCREEN ─────────────────────────────
   v0.8.9.1. `usb lockdown` has always said "the raw line above is what
   arrived" and there has never been a raw line above it, because
   lockdown_dump() is written entirely in klog() and kernel/main.c
   calls klog_set_echo(0) before the shell starts. Every byte of
   evidence this command produces goes to the ring buffer and the
   screen gets the verdict with a blank space where the reason should
   be.

   That is not a small thing: this is the command somebody runs on a
   machine with no debugger, photographs, and sends to somebody else.
   A diagnostic whose output is invisible is a diagnostic that sends
   two people looking in the wrong place.

   So this prints the reply with vga, not klog. Both halves, because
   they answer different questions:

     - the TEXT says whether it is a plist at all and which keys are
       in it. A lockdownd reply has <key>Type</key> in the first two
       hundred bytes.
     - the HEX of the first 16 bytes says what it is when it is NOT
       text. "bplist00" is a binary plist; a 4-byte length prefix that
       disagrees with the byte count is the framing; all zeroes is a
       read that returned a buffer nobody filled.

   Non-printable bytes become '.', so a binary reply is still
   photographable rather than scrolling the console sideways. */
void lockdown_show_reply(void)
{
    const char *p = lockdown_reply();
    int n = lockdown_reply_len();
    int i, show;

    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    up("  what arrived: ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    put_dec((uint32_t)(n < 0 ? 0 : n));
    up(" bytes");

    if (!p || n <= 0) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up(" -- NOTHING. The device never answered.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    /* The 4-byte big-endian length prefix, and whether it agrees with
       what turned up. These disagreeing IS the framing bug the old
       message guessed at, and it is one subtraction to know. */
    if (n >= 4) {
        uint32_t said = ((uint32_t)(uint8_t)p[0] << 24) |
                        ((uint32_t)(uint8_t)p[1] << 16) |
                        ((uint32_t)(uint8_t)p[2] << 8)  |
                         (uint32_t)(uint8_t)p[3];
        up(", the prefix says ");
        put_dec(said);
        if (said + 4 != (uint32_t)n) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            up(" -- THESE DISAGREE");
            vga_set_color(VGA_WHITE, VGA_BLACK);
        }
    }
    up("\n");

    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    up("  first 16 bytes: ");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    show = n < 16 ? n : 16;
    for (i = 0; i < show; i++) {
        static const char HX[] = "0123456789abcdef";
        char b[4];
        b[0] = HX[((uint8_t)p[i] >> 4) & 0xF];
        b[1] = HX[(uint8_t)p[i] & 0xF];
        b[2] = ' ';
        b[3] = 0;
        up(b);
    }
    up("\n");

    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    up("  as text:\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    show = n < 320 ? n : 320;
    for (i = 0; i < show; i++) {
        char c = p[i];
        char b[2];
        if (c == '\n') { up("\n"); continue; }
        b[0] = (c >= 32 && c < 127) ? c : '.';
        b[1] = 0;
        up(b);
    }
    if (n > show) up("\n  ...");
    up("\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

void cmd_usb(const char *arg1)
{
    /* -v is the registers and the init log; -vv also lists every empty
       port.  They are separate because a companion set has six ports and a
       line each pushes the init log -- the part you actually came for --
       off the top of an 80x25 screen. */
    int vv      = arg1 && !strcmp(arg1, "-vv");
    int verbose = vv || (arg1 && !strcmp(arg1, "-v"));
    int nhc     = usb_hc_count();
    int c, p, i, empty = 0;

    g_stop = 0;
    ui_page_reset();

    if (nhc == 0) { no_controller(); return; }

    /* ── `usb rescan`, v0.8.6 ──────────────────────────────────────
       Nothing in this kernel runs periodically, so a hub's change
       endpoint is never polled and a device plugged in after boot is
       not noticed on its own. Stated in usb/hub.c and given a command
       here rather than left for somebody to discover.

       The order is the order the layers stack: walk the bus, re-probe
       the mass-storage devices on it -- each holds a COPY of its
       usb_dev_t, so a stale one addresses whatever moved into that
       slot -- and then rebuild the block layer's table, because a disk
       that has gone away must not stay in the installer's picker. */
    if (arg1 && !strcmp(arg1, "cfg")) { do_cfg(0); return; }
    if (arg1 && arg1[0] >= '0' && arg1[0] <= '9') { do_cfg(arg1); return; }

    /* ── AN ARGUMENT THIS COMMAND DOES NOT KNOW, v0.8.6.6 ──────────
       It used to fall through to the plain bus listing in silence, so
       `usb cfg` on a kernel too old to have it printed a perfectly
       normal screen and looked like it had worked. That cost a round
       trip on 25 October: the listing was read as the survey's output
       and the survey had never run.

       A command that quietly does something else when it does not
       understand you is worse than one that refuses. */
    if (arg1 && *arg1 && !verbose &&
        strcmp(arg1, "rescan") != 0 && strcmp(arg1, "eth") != 0 &&
        strcmp(arg1, "mux") != 0 && strcmp(arg1, "lockdown") != 0) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        up("usb: no such option: ");
        up(arg1);
        up("\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("  usb              the controllers and what is on them\n");
        up("  usb -v | -vv     more of it, and every empty port\n");
        up("  usb rescan       walk the bus again\n");
        up("  usb cfg [addr]   every configuration a device offers\n");
        up("  usb eth          USB network adapters, and raw framing\n");
        up("  usb mux          the Apple Mobile Device channel\n");
        up("  usb lockdown     ask lockdownd what it is (stage 1)\n");
        return;
    }

    if (arg1 && !strcmp(arg1, "eth")) { do_eth(); return; }
    if (arg1 && !strcmp(arg1, "mux")) { do_mux(); return; }
    if (arg1 && !strcmp(arg1, "lockdown")) { do_lockdown(); return; }

    if (arg1 && !strcmp(arg1, "rescan")) {
        int nd, ns, nb, nu;
        up("Walking the bus again...\n");
        /* ── v0.8.8: ONE COPY OF THE SEQUENCE, IN usb/hotplug.c ─────
           The seven calls that were written out here are the same
           seven the periodic root-port check needs, and the ORDER is
           the whole content -- each step is there because of a
           specific bug, and they are named where the function lives.
           Two copies of that would drift, which is the mistake
           tools/check_diskname_sync.py refuses one layer down and the
           reason v0.8.6 moved the disk table out of
           shell/iso_installer.c.  tools/check_kpoll_guard.py refuses
           a second copy of it here. */
        usb_rescan_all();
        {
            const usb_rescan_t *r = usb_rescan_result();
            nd = r->devices; ns = r->disks; nb = r->blkdevs; nu = r->nics;
        }
        put_dec((uint32_t)nd);
        up(nd == 1 ? " device" : " devices");
        if (usb_hub_count()) {
            up(", ");
            put_dec((uint32_t)usb_hub_devices());
            up(" behind ");
            put_dec((uint32_t)usb_hub_count());
            up(usb_hub_count() == 1 ? " hub" : " hubs");
        }
        up(".  ");
        put_dec((uint32_t)ns);
        up(ns == 1 ? " USB disk.  " : " USB disks.  ");
        put_dec((uint32_t)nb);
        up(nb == 1 ? " disk in all.  " : " disks in all.  ");
        put_dec((uint32_t)nu);
        up(nu == 1 ? " USB NIC.\n" : " USB NICs.\n");

        /* ── v0.8.7.2: A NIC THAT ARRIVES NEEDS AN ADDRESS ────────────
           DHCP ran once, at boot, and only if a NIC was already there.
           Nothing ran on a timer, so the ordinary way a USB NIC
           appeared was a person plugging it in and typing `usb rescan`
           -- after boot, after the one DHCP attempt.

           v0.8.8 makes that the belt rather than the braces:
           net/net_tick.c retries with backoff on its own, and
           usb/hotplug.c resets that sequence when a device arrives.
           This stays because `usb rescan` is still the answer for a
           device behind a hub, and because asking immediately is
           better than waiting for the next backoff step.

           Until this release the adapter then came up with the
           COMPILE-TIME FALLBACK still in place: 10.0.2.15 behind a
           gateway at 10.0.2.2, which is QEMU's network and nobody
           else's. `ifconfig` said UP, `ping` and `wget` went to a
           gateway that was not there, and the only thing on screen was
           "dns: resolution failed".

           That was the whole of it on the HP, 26 October. */
        if (nu > 0) {
            extern int  dhcp_request(void);
            extern int  net_config_source(void);
            extern void net_get_mac(uint8_t[6]);
            if (net_config_source() == 0) {       /* NET_CFG_FALLBACK */
                up("Asking the new adapter's network for an address...\n");
                if (dhcp_request() && net_config_source() != 0) {
                    up("Got one. 'ifconfig' to see it.\n");
                } else {
                    up("No answer. Still on the built-in fallback\n");
                    up("address, which will not route anywhere -- set\n");
                    up("one by hand with 'ifconfig set <ip> <gw>'.\n");
                }
            }
        }
        up("'usb -vv' lists the bus, 'disk' the disks, 'usb eth' the\n");
        up("network adapters.\n");
        return;
    }

    /* ── the controllers, named by the core ── */
    for (c = 0; c < nhc; c++) {
        const usb_hc_t *h = usb_hc(c);
        int np = 0;
        if (!h) continue;
        np = h->ops->port_count ? h->ops->port_count((usb_hc_t *)h) : 0;

        up("hc"); put_dec((uint32_t)c);
        up("  "); up(usb_hc_name(h));
        up("  at 0x"); put_hex(h->base, 8);
        up("  "); put_dec((uint32_t)np);
        up(np == 1 ? " port" : " ports");
        up("\n");

        if (verbose && h->kind == USB_HC_UHCI) {
            up("      IRQ ");    put_dec(uhci_hc_irq(h->index));
            up("  CMD 0x");      put_hex(uhci_usbcmd(h->index), 4);
            up(" STS 0x");       put_hex(uhci_usbsts(h->index), 4);
            up(" FRNUM ");       put_dec(uhci_frnum(h->index));
            up("\n");
        }
    }

    if (uhci_ehci_released()) {
        up("EHCI at 0x"); put_hex(uhci_ehci_base(), 8);
        up(" - its ports were handed to the UHCI\n");
        up("companions, so devices run at full speed, 12 Mbit.\n");
    }
    up("\n");

    /* ── ports, through the core, so an xHCI port reads the same way ── */
    for (c = 0; c < nhc; c++) {
        const usb_hc_t *h = usb_hc(c);
        int np;
        if (!h) continue;
        np = h->ops->port_count ? h->ops->port_count((usb_hc_t *)h) : 0;

        for (p = 0; p < np; p++) {
            int conn = h->ops->port_connected
                     ? h->ops->port_connected((usb_hc_t *)h, p) : 0;
            const usb_dev_t *d = 0;

            if (!conn) {
                empty++;
                if (vv) {
                    up("hc"); put_dec((uint32_t)c);
                    up(" port "); put_dec((uint32_t)p);
                    up("  empty\n");
                }
                continue;
            }

            up("hc"); put_dec((uint32_t)c);
            up(" port "); put_dec((uint32_t)p);
            {
                uint32_t psc = (h->kind == USB_HC_UHCI)
                             ? (uint32_t)uhci_portsc(h->index, p)
                             : xhci_portsc_raw(h->index, p);
                if (verbose) {
                    up("  PORTSC 0x");
                    put_hex(psc, 8);
                }
                up("\n");
                /* In words, always -- not only under -vv. A port with
                   something on it that did not come up is the case
                   somebody is staring at, and the hex alone has never
                   helped anyone. */
                put_portsc_words(psc, h->kind != USB_HC_UHCI);
            }

            for (i = 0; i < usb_device_count(); i++) {
                const usb_dev_t *e = usb_device(i);
                /* tier 0: the device ON the port. Everything behind a
                   hub carries the same rootport in `port`, so without
                   this the first hub-attached device would be printed
                   as though it were plugged into the machine. */
                if (e && e->hc == (uint8_t)c && e->tier == 0 &&
                    e->port == (uint8_t)p) {
                    d = e; break;
                }
            }
            if (!d) {
                uint32_t psc = (h->kind == USB_HC_UHCI)
                             ? (uint32_t)uhci_portsc(h->index, p)
                             : xhci_portsc_raw(h->index, p);
                up("        attached, but it did not answer.\n");
                /* Which of the three it is, said rather than implied. */
                if (h->kind != USB_HC_UHCI && !(psc & (1u << 9)))
                    up("        the port is NOT POWERED -- nothing can "
                       "answer from it.\n");
                else if (h->kind != USB_HC_UHCI && !(psc & 2u))
                    up("        it never finished training, so no "
                       "request was ever sent.\n");
                else
                    up("        the link is up and the device did not "
                       "reply to enumeration.\n");
                up("        'dmesg' shows what the scan saw.\n");
                continue;
            }

            print_dev(d, 0, verbose);
            print_children(i_of(d), 1, verbose);
        }
    }

    if (!vv && empty) {
        put_dec((uint32_t)empty);
        up(empty == 1 ? " other port is empty.\n" : " ports are empty.\n");
    }

    if (usb_device_count() == 0) {
        up("\nNo devices answered.\n");
        if (uhci_ehci_released()) {
            up("The EHCI's ports were handed over, so anything plugged\n");
            up("in should appear here. If it does not, that device\n");
            up("cannot run at full speed and needs an EHCI driver.\n");
        } else {
            up("In QEMU a USB device needs asking for:\n");
            up("  -device qemu-xhci -device usb-kbd\n");
        }
    } else {
        up("\nThe bus talks to ");
        put_dec((uint32_t)usb_device_count());
        up(usb_device_count() == 1 ? " device" : " devices");
        if (usb_hub_count()) {
            up(", ");
            put_dec((uint32_t)usb_hub_devices());
            up(" of them behind ");
            put_dec((uint32_t)usb_hub_count());
            up(usb_hub_count() == 1 ? " hub" : " hubs");
        }
        up(".\n");
        /* v0.8.6: this used to end "Reading files from one needs the
           mass-storage layer, which is the next release." It is this
           release, so the line says what to type instead. */
        { extern int msc_count(void);
          int n = msc_count();
          if (n) {
              put_dec((uint32_t)n);
              up(n == 1 ? " of them is a disk: 'disk' lists it,\n"
                        : " of them are disks: 'disk' lists them,\n");
              up("and a FAT16 or FAT32 volume on one can be mounted.\n");
          } else {
              up("None of them is a disk. A stick plugged in after\n");
              up("boot needs 'usb rescan' -- nothing here runs on a\n");
              up("timer, so a device is noticed when it is asked for.\n");
          } }
    }

    /* v0.8.6.4: `usb -vv` shows the configuration a device is USING,
       which for a phone is the one it boots into and not the one with
       Ethernet in it. Said here so the next person does not conclude
       from this screen that there is nothing else on offer. */
    if (verbose) {
        up("\n'usb cfg' lists EVERY configuration a device offers --\n");
        up("what is above is only the one in use.\n");
        up("What happened during init is in the kernel log: 'dmesg'.\n");
    }
}
