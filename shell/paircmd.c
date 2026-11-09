/* shell/paircmd.c -- `pair`, the command that asks an iPhone to trust
 * this computer.
 *
 * ── WHY THE LOOP IS HERE AND NOT IN net/ ─────────────────────────────
 *
 * include/rsa.h: "kpoll_sleep_ms() may not appear in drivers/, usb/,
 * net/ or fs/ at all", and tools/check_kpoll_sites.py enforces it. A
 * 2048-bit keygen is a few hundred Miller-Rabin rounds and this is
 * the only place allowed to yield between them -- so the loop, the
 * progress line and the keypress that aborts all live here.
 *
 * Two keys are needed: a root and a host. The device's own key comes
 * off the device.
 *
 * ── WHAT THE USER SEES ───────────────────────────────────────────────
 *
 * The honest version. Every step is named as it happens, because when
 * this fails on somebody's desk the useful information is which step
 * it was, and "pairing failed" is the sentence this project keeps
 * deleting.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/theme.h"
#include "../include/kpoll.h"
#include "../include/bignum.h"
#include "../include/rsa.h"
#include "../include/der_read.h"
#include "../include/lockdown.h"
#include "../include/pair.h"
#include "../include/usbmux.h"
#include "../include/tls.h"

extern void vga_printf(const char *fmt, ...);
extern int  keyboard_getchar(void);
extern uint32_t boot_ms(void);
extern uint32_t entropy_u32(void);
/* shell/usb.c. The lockdown reply, printed with vga rather than klog
   -- see the note above its definition. `pair` is where people
   actually are when a lockdown step fails, so it shows the same
   screen `usb lockdown` does instead of telling them to run it. */
extern void lockdown_show_reply(void);

/* v0.8.9: the pair record is kept on the system volume. The I/O is
   HERE and not in usb/pair.c -- see the note at the end of that file:
   a serialiser that needs a mounted volume to be exercised is a
   serialiser that does not get exercised. */
extern int vfs_write(const char *name, const char *data, uint32_t size);
extern int vfs_read (const char *name, char *buf, uint32_t bufsz);
extern int vfs_delete(const char *name);
extern int vfs_exists(const char *name);
extern int vfs_using_disk(void);

/* These do not go on the stack. An rsa_keygen_t is over 3.5 KB and a
   pair_record_t is six, and include/rsa.h says what a local would do
   to whatever is below it. */
static rsa_keygen_t  g_kg;
static pair_key_t    g_root, g_host;
static pair_record_t g_rec;
static char          g_req[16384];
static uint8_t       g_scratch[1024];
static bn_t          g_devn, g_deve;
/* v0.8.9. g_store carries the record AND the two private keys -- see
   include/pair.h on why those are one object. g_text is the serialised
   form; both are static for the same reason everything above is. */
static pair_store_t  g_store;
static char          g_text[PAIR_STORE_MAX];

/* The keygen loop: bounded work per step, polled between steps, and
   a key that gives up. Returns 1 on a key, 0 if the user aborted,
   negative on a refusal from the generator. */
static int keygen(const char *what, pair_key_t *out)
{
    uint32_t t0 = boot_ms();
    int last_pct = -1;
    int r;

    vga_printf("  generating the %s key (2048-bit)...\n", what);
    r = rsa_keygen_begin(&g_kg, 2048);
    if(r != 0){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_printf("  the generator refused to start (%d)\n", r);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return r;
    }

    for(;;){
        int pct;
        r = rsa_keygen_step(&g_kg);
        /* ── v0.8.9.4: THE RETURN CODES ARE ALL POSITIVE ─────────────
           RSA_KG_WORKING is 1, RSA_KG_DONE is 2, RSA_KG_FAILED is 3 --
           see include/rsa.h. This loop used to read `if (r < 0)` for
           failure and `if (r == 1)` for success, which is the
           USB_OK/negative-errno convention this tree uses nearly
           everywhere else, and it is not this function's.

           So it broke out of the loop on the FIRST step, after one
           bounded piece of work, and copied a key that did not exist
           yet. On Kyan's HP that printed "root key: done in 70 ms"
           -- a 2048-bit keygen is about half a second of this
           arithmetic and 577 steps -- and `pair` then refused its own
           key three functions later with "a key too small to pair
           with", which is the certificate builder catching at the end
           what the generator had never been asked.

           The failure path was broken the same way and worse: FAILED
           is 3, so `r < 0` never fired and a generator that gave up
           would have been stepped for ever.

           shell/rsacmd.c had it right all along. One file right and
           one file wrong is what tools/check_rsa_kg_loop.py is for. */
        if(r == RSA_KG_FAILED){
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            vga_printf("  the generator gave up: %s\n",
                       rsa_why_name(g_kg.why));
            vga_set_color(VGA_WHITE, VGA_BLACK);
            return -1;
        }
        if(r == RSA_KG_DONE) break;

        pct = (int)rsa_keygen_progress(&g_kg);
        if(pct != last_pct){
            last_pct = pct;
            vga_printf("\r  %s key: %d%%   (any key aborts)   ", what, pct);
        }
        /* The one place allowed to do this. */
        kpoll_sleep_ms(0);
        if(keyboard_getchar() > 0){
            vga_puts("\r  aborted.                              \n");
            return 0;
        }
    }

    /* ── AND CHECK WHAT CAME OUT ─────────────────────────────────────
       The generator says it is done; this says the key is 2048 bits.
       It costs one comparison and it is the difference between
       failing HERE, where the words "generating the root key" are
       still on the screen, and failing in pair_build_certs() with "a
       key too small to pair with" -- which is true, unhelpful, and
       three functions away from the cause. */
    if(bn_bits(&g_kg.n) != 2048){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_printf("\r  the %s key came out %u bits, not 2048.      \n",
                   what, bn_bits(&g_kg.n));
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return -1;
    }

    vga_printf("\r  %s key: done in %u ms          \n",
               what, boot_ms() - t0);
    bn_copy(&out->n, &g_kg.n);
    bn_copy(&out->e, &g_kg.e);
    bn_copy(&out->d, &g_kg.d);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
   v0.8.9: KEEPING THE RECORD, AND USING IT

   Two reasons this matters more than it sounds. The HostID IS the
   host's identity to the phone, so a fresh one is a fresh computer and
   another tap on Trust. And the two 2048-bit keys cost real time to
   make -- which is time spent reproducing something that was already
   correct.
   ════════════════════════════════════════════════════════════════════ */

static void red(void)  { vga_set_color(VGA_LIGHT_RED, VGA_BLACK); }
static void grey(void) { vga_set_color(VGA_DARK_GREY, VGA_BLACK); }
static void white(void){ vga_set_color(VGA_WHITE, VGA_BLACK); }

/* Load, and say WHY if it will not load. A record refused silently is
   a record that is quietly regenerated every boot, and the symptom of
   that is the phone asking for Trust again with nothing on screen to
   explain it. */
static int store_load(void)
{
    int n, r;

    if (!vfs_using_disk()) return 0;
    if (!vfs_exists(PAIR_STORE_FILE)) return 0;

    n = vfs_read(PAIR_STORE_FILE, g_text, sizeof g_text);
    if (n <= 0) {
        red();
        vga_printf("  %s is there but would not read (%d).\n",
                   PAIR_STORE_FILE, n);
        white();
        return 0;
    }
    r = pair_store_decode(g_text, (uint32_t)n, &g_store);
    if (r != PAIR_OK) {
        red();
        vga_printf("  the stored record is unusable: %s\n",
                   pair_strerror(r));
        grey();
        vga_puts("  `pair forget` removes it; pairing again replaces it.\n");
        white();
        return 0;
    }
    return 1;
}

static int store_save(void)
{
    uint32_t len = 0;
    int r = pair_store_encode(&g_store, g_text, sizeof g_text, &len);

    if (r != PAIR_OK) {
        red();
        vga_printf("  the record would not serialise: %s\n",
                   pair_strerror(r));
        white();
        return 0;
    }
    if (!vfs_using_disk()) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  NOT SAVED: there is no writable system volume. This\n"
                 "  is a live session -- install HavenDOS and the record\n"
                 "  survives a reboot.\n");
        white();
        return 0;
    }
    if (vfs_write(PAIR_STORE_FILE, g_text, len) < 0) {
        red();
        vga_printf("  %s would not write.\n", PAIR_STORE_FILE);
        white();
        return 0;
    }
    vga_printf("  saved to %s, %u bytes.\n", PAIR_STORE_FILE, len);
    grey();
    vga_puts("  It holds a PRIVATE KEY, unencrypted -- this kernel has\n"
             "  nothing to encrypt it with. See include/pair.h.\n");
    white();
    return 1;
}

/* ── the session ─────────────────────────────────────────────────────
   StartSession, then TLS with the host certificate as a CLIENT
   certificate, then one QueryType sent INSIDE the encrypted session as
   proof that it is really there. The last step is the one that cannot
   be faked: a handshake that completed but produced unusable keys
   looks exactly like success until something is sent through it. */
static int session_open(void)
{
    tls_client_cert_t cc;
    int r;

    vga_puts("  StartSession...\n");
    r = lockdown_start_session(g_store.rec.host_id,
                               g_store.rec.system_buid);
    if (r < 0) {
        red();
        vga_printf("  the request failed (%d).\n", r);
        white();
        return 0;
    }
    if (r == 0) {
        red();
        vga_printf("  refused: %s\n", lockdown_error());
        if (!strcmp(lockdown_error(), "InvalidHostID")) {
            grey();
            vga_puts("  This device does not know this HostID. The record\n"
                     "  is for a different device, or it was unpaired on\n"
                     "  the phone (Settings > General > Transfer or Reset).\n"
                     "  `pair` asks again from scratch.\n");
        }
        white();
        return 0;
    }
    vga_printf("  session %s\n", lockdown_session_id());

    if (!lockdown_ssl_wanted()) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  the device did NOT ask for SSL -- unusual, and the\n"
                 "  session is in the clear. Carrying on.\n");
        white();
        return 1;
    }

    /* The chain is LEAF FIRST: the host certificate, then the root
       that signed it. The opposite order is a chain the peer cannot
       build and is the mistake RFC 5246 7.4.2 exists to prevent.
       Note which key signs: the HOST key, because the host
       certificate is what certifies it. */
    memset(&cc, 0, sizeof cc);
    cc.cert[0]     = g_store.rec.host_cert;
    cc.cert_len[0] = g_store.rec.host_len;
    cc.cert[1]     = g_store.rec.root_cert;
    cc.cert_len[1] = g_store.rec.root_len;
    cc.n           = 2;
    cc.sign        = pair_tls_sign;
    cc.sign_ctx    = &g_store.host;

    vga_puts("  TLS handshake, with our host certificate...\n");
    r = lockdown_enable_ssl(&cc, entropy_u32());
    if (r < 0) {
        red();
        vga_printf("  the handshake failed (%d).\n", r);
        grey();
        vga_printf("  reached: %s\n", tls_stage_name(g_tls_last_stage));
        if (g_tls_last_alert_desc >= 0)
            vga_printf("  the device sent alert %d.\n",
                       g_tls_last_alert_desc);
        vga_puts("  `tlsinfo` has the rest of it.\n");
        white();
        return 0;
    }
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts("  the session is ENCRYPTED.\n");
    white();

    /* Proof. One request down the encrypted channel: if the keys are
       wrong this is where it shows, and it shows as a MAC failure
       rather than as a handshake that looked fine. */
    vga_puts("  QueryType inside the session...\n");
    r = lockdown_query_type();
    if (r != 1) {
        red();
        vga_printf("  nothing usable came back (%d). The handshake\n"
                   "  succeeded and the session does not work -- which is\n"
                   "  worth a photograph.\n", r);
        white();
        return 0;
    }
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_printf("  the device answered \"%s\" over TLS.\n",
               lockdown_type());
    white();
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("\n  STAGE 3 IS COMPLETE.\n");
    white();
    return 1;
}

static void pair_forget(void)
{
    if (!vfs_using_disk()) {
        grey();
        vga_puts("  no writable system volume -- nothing is stored.\n");
        white();
        return;
    }
    if (!vfs_exists(PAIR_STORE_FILE)) {
        vga_printf("  there is no %s.\n", PAIR_STORE_FILE);
        return;
    }
    if (vfs_delete(PAIR_STORE_FILE) < 0) {
        red();
        vga_printf("  %s would not delete.\n", PAIR_STORE_FILE);
        white();
        return;
    }
    vga_printf("  %s deleted.\n", PAIR_STORE_FILE);
    grey();
    vga_puts("  The phone still remembers this HostID until it is\n"
             "  unpaired there too, and a new `pair` uses a new one.\n");
    white();
}

static void pair_show(void)
{
    if (!store_load()) {
        vga_puts("  no usable stored record.\n");
        return;
    }
    vga_printf("  HostID      %s\n", g_store.rec.host_id);
    vga_printf("  SystemBUID  %s\n", g_store.rec.system_buid);
    vga_printf("  root %u, host %u, device %u bytes of certificate\n",
               g_store.rec.root_len, g_store.rec.host_len,
               g_store.rec.dev_len);
    vga_printf("  root key %u bits, host key %u bits\n",
               bn_bits(&g_store.root.n), bn_bits(&g_store.host.n));
}

void cmd_pair(const char *arg)
{
    int r, status, have_stored = 0;
    uint32_t reqlen = 0;

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("pair -- ask an iPhone to trust this computer\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);

    if(arg && !strcmp(arg, "help")){
        vga_puts("Usage: pair [session|show|forget]\n\n"
                 "  pair          pair with the phone and open a session.\n"
                 "                Reuses the stored record if there is\n"
                 "                one, so the keygen is paid for once.\n"
                 "  pair session  skip the Pair request; use the stored\n"
                 "                record to open an encrypted session.\n"
                 "  pair show     what is stored.\n"
                 "  pair forget   delete the stored record.\n\n"
                 "Plug the phone in, unlock it, and run this. It makes a\n"
                 "small certificate authority of its own and asks the phone\n"
                 "to remember it; the phone then puts a Trust dialog on\n"
                 "screen. Run `pair` again after tapping Trust.\n");
        return;
    }
    if(arg && !strcmp(arg, "forget")){ pair_forget(); return; }
    if(arg && !strcmp(arg, "show")){   pair_show();   return; }

    /* ── the conversation ────────────────────────────────────────── */
    vga_puts("  connecting to lockdownd...\n");
    r = lockdown_connect();
    if(r < 0){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_printf("  no lockdownd (%d). Is the phone plugged in and\n"
                   "  unlocked? `usb` and `usb lockdown` say more.\n", r);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    r = lockdown_query_type();
    if(r != 1){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_printf("  the device answered \"%s\" rather than lockdown (%d).\n",
                   lockdown_type(), r);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        /* v0.8.9.1: SHOW IT. "answered" is not even accurate when r is
           negative -- nothing answered at all -- and the difference
           between that and a reply we could not parse is the whole
           diagnosis. One screen, here, rather than a second command
           whose evidence also goes to a log nobody is echoing. */
        lockdown_show_reply();
        /* Carry on anyway: a device that talks at all is worth asking. */
    }

    /* ── `pair session`: the stored record, and nothing else ─────────
       Deliberately separate from the path below. Sending the Pair
       request again to a device that already trusts us is harmless but
       it is not what is being tested, and a session that fails is a
       different finding from a pairing that fails. */
    if(arg && !strcmp(arg, "session")){
        if(!store_load()){
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_puts("  nothing stored to open a session with. Run `pair`.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            return;
        }
        vga_printf("  using the stored HostID %s\n", g_store.rec.host_id);
        session_open();
        return;
    }

    vga_puts("  asking for DevicePublicKey...\n");
    r = lockdown_get_value(LOCKDOWN_KEY_DEVICE_PUBLIC);
    if(r != 1){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_printf("  it would not give one (%d). %s\n", r, lockdown_error());
        vga_set_color(VGA_WHITE, VGA_BLACK);
        lockdown_show_reply();
        return;
    }

    r = rsa_pub_from_pem(lockdown_value(),
                         (uint32_t)lockdown_value_len(),
                         g_scratch, sizeof g_scratch, &g_devn, &g_deve);
    if(r != DER_OK){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_printf("  the key would not parse: %s\n",
                   der_read_strerror(r));
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    vga_printf("  the device's key is %u bits.\n", bn_bits(&g_devn));

    /* ── our own two keys, or the ones we already have ────────────
       v0.8.9. The stored record is reused WHOLE -- keys and UUIDs --
       and not merely as a cache for the keys: a new HostID is a new
       computer to the phone, so regenerating the UUIDs while keeping
       the keys would ask for Trust again and look like the keys had
       not been kept either. */
    have_stored = store_load();
    if(have_stored){
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_printf("  reusing the stored record -- HostID %s\n",
                   g_store.rec.host_id);
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("  (no keygen: that is what saving it buys.)\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        bn_copy(&g_root.n, &g_store.root.n);
        bn_copy(&g_root.e, &g_store.root.e);
        bn_copy(&g_root.d, &g_store.root.d);
        bn_copy(&g_host.n, &g_store.host.n);
        bn_copy(&g_host.e, &g_store.host.e);
        bn_copy(&g_host.d, &g_store.host.d);
    } else {
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("  (two 2048-bit keys. Seconds in a virtual machine, a\n"
                 "   minute or two on real hardware -- `pair` prints the\n"
                 "   elapsed time for each.)\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        if(keygen("root", &g_root) != 1) return;
        if(keygen("host", &g_host) != 1) return;
    }

    /* ── the record ──────────────────────────────────────────────────
       The certificates are rebuilt even when the keys were stored,
       because the DEVICE certificate certifies THIS device's key and
       a stored one may be a different phone's. Rebuilding all three
       from the same root keeps the chain consistent, and it costs
       milliseconds -- the expensive part was the keygen. */
    vga_puts("  building the certificates...\n");
    memset(&g_rec, 0, sizeof g_rec);
    r = pair_build_certs(&g_root, &g_host, &g_devn, &g_deve, &g_rec);
    if(r != PAIR_OK){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_printf("  %s\n", pair_strerror(r));
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    vga_printf("  root %u, host %u, device %u bytes.\n",
               g_rec.root_len, g_rec.host_len, g_rec.dev_len);

    if(have_stored){
        memcpy(g_rec.host_id, g_store.rec.host_id, sizeof g_rec.host_id);
        memcpy(g_rec.system_buid, g_store.rec.system_buid,
               sizeof g_rec.system_buid);
    } else {
        pair_make_uuid(g_rec.host_id, sizeof g_rec.host_id);
        pair_make_uuid(g_rec.system_buid, sizeof g_rec.system_buid);
    }
    vga_printf("  HostID %s\n", g_rec.host_id);

    r = pair_build_request(&g_rec, g_req, sizeof g_req, &reqlen);
    if(r != PAIR_OK){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_printf("  %s\n", pair_strerror(r));
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    vga_printf("  the request is %u bytes, %u mux packets.\n",
               reqlen, (reqlen + 4 + (uint32_t)usbmux_max_payload() - 1)
                       / (uint32_t)usbmux_max_payload());

    /* ── ask ─────────────────────────────────────────────────────── */
    vga_puts("  sending it...\n");
    r = lockdown_request(g_req);
    if(r != 1){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_printf("  the request did not get a reply (%d).\n", r);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    status = pair_reply_status(lockdown_reply(),
                              (uint32_t)lockdown_reply_len());
    vga_puts("\n");
    vga_set_color(status == PAIR_R_SUCCESS ? VGA_LIGHT_GREEN
                 : status == PAIR_R_PENDING ? VGA_YELLOW
                 : VGA_LIGHT_RED, VGA_BLACK);
    vga_printf("  %s\n", pair_status_name(status));
    vga_set_color(VGA_WHITE, VGA_BLACK);

    if(status == PAIR_R_PENDING)
        vga_puts("  Tap Trust on the phone, then run `pair` again.\n");
    if(status == PAIR_R_PASSWORD)
        vga_puts("  Unlock the phone with its passcode and try again.\n");
    if(status == PAIR_R_OTHER || status == PAIR_R_NOREPLY){
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("  `lockdown dump` shows the reply verbatim.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
    }
    if(status == PAIR_R_SUCCESS){
        /* ── v0.8.9: save FIRST, then use it ─────────────────────────
           In that order on purpose. A session that fails leaves a
           record that is still valid and still worth keeping -- the
           phone has already been asked and has already agreed, and
           throwing the record away because the step AFTER it failed
           would mean another Trust tap to retry. */
        g_store.rec = g_rec;
        bn_copy(&g_store.root.n, &g_root.n);
        bn_copy(&g_store.root.e, &g_root.e);
        bn_copy(&g_store.root.d, &g_root.d);
        bn_copy(&g_store.host.n, &g_host.n);
        bn_copy(&g_store.host.e, &g_host.e);
        bn_copy(&g_store.host.d, &g_host.d);
        store_save();

        vga_puts("\n");
        session_open();
    }
}
