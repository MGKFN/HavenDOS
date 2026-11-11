#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/usbmux.h -- the Apple Mobile Device channel.

   usb/usbmux.c is the transport. It is the first half of the answer to
   the question the HP settled on 26 October:

     Every `usb rescan` re-prompted for trust on the phone, and the
     Personal Hotspot indicator never appeared. The phone accepted
     SET_CONFIGURATION, accepted SET_INTERFACE to the Ethernet
     interface's alternate setting, and returned a real MAC address --
     and never sent a frame, because iOS does not turn tethering on for
     a host it has no pairing record for.

     Descriptor-level cooperation is not consent.

   Pairing runs over interface 1 of the same configuration, class
   FF/FE/02, which `usb cfg` found on 25 October. That is usbmux: the
   channel usbmuxd talks, and the one lockdownd sits behind.

   ── WHAT THIS FILE IS, AND IS NOT ────────────────────────────────────

   It is the TRANSPORT only, and deliberately so. The full road to a
   paired phone is:

     0  usbmux carries packets at all       CONFIRMED on the HP, 26 Oct
     1  lockdownd answers QueryType         usb/lockdown.c, over this
     2  GetValue returns the device key     no crypto
     3  Pair is accepted                    RSA, X.509
     4  the hotspot indicator appears       --

   Stages 0 to 2 need no cryptography whatsoever, and each one is a
   yes or no the phone itself gives. That is the whole reason this is
   built bottom-up rather than all at once: every stage is a boot that
   either works or says where it stopped.

   This file owns stage 0 and the connection layer stage 1 runs over.
   The plist conversation itself is usb/lockdown.c, because stages 2
   and 3 grow a plist reader, RSA and X.509 and none of that belongs
   in a USB transport. usbmuxd splits it the same way: its device.c
   owns the mux and its TCP, and libimobiledevice owns lockdownd.

   ── THE PROTOCOL ─────────────────────────────────────────────────────

   v0.8.7.7: no longer a guess. Every field below was read out of
   usbmuxd's own src/device.c -- send_packet(), send_tcp() and
   device_tcp_input() -- which is the only implementation known to work
   against a real device. Where this driver differs from it, the
   difference is marked and argued, because "more correct than the
   thing that works" is the position that cost 0.8.7.3 a release.

   A mux packet is a header and a payload. The header is EIGHT bytes
   until both ends have agreed on version 2, and SIXTEEN after:

       uint32 protocol     0 version, 1 control, 2 setup, 6 TCP
       uint32 length       the whole packet, header included
       uint32 magic        0xFEEDFACE          version 2 only
       uint16 tx_seq       see below            version 2 only
       uint16 rx_seq       see below            version 2 only

   The magic and the sequence numbers cannot be on the packet that
   ESTABLISHES the version -- neither end knows it yet. v0.8.7.3
   hardcoded sixteen, read byte 8 as the magic, and reported "magic 2,
   expected FEEDFACE" on the HP. The 2 it complained about was the
   phone saying it speaks usbmux 2.0.

   THE SEQUENCE NUMBERS ARE NOT COUNTERS. This is the one thing in the
   framing that reads like something it is not, and v0.8.7.6 had it
   wrong in three places:

     - tx_seq advances only on a packet that CARRIES it, so the
       VERSION packet does not move it.
     - the SETUP packet RESETS the pair: tx_seq to 0 and rx_seq to
       0xFFFF, writes those values, and only then advances tx_seq.
       0xFFFF is "nothing received yet", not a count of 65535.
     - rx_seq is not a tally of packets received. It is the rx_seq
       field of the last packet the DEVICE sent, mirrored straight
       back. usbmuxd does exactly this and nothing else.

   None of that was visible before stage 1: the only sequenced packet
   this kernel had ever sent was the SETUP, and on it the wrong values
   happened to be 1 and 1 -- which is why tests/test_usbmux.c recorded
   that it could not tell the two fields apart. Stage 1 is real traffic
   and the check lives there now.

   ── THE CONNECTION LAYER ─────────────────────────────────────────────

   Protocol 6 carries a cut-down TCP: a standard twenty-byte BSD
   tcphdr, no checksum (the mux is the reliable layer), header offset
   5, and a WINDOW IN UNITS OF 256 BYTES -- `th_win = window >> 8` on
   the way out and `<< 8` on the way in. A window field that looks 256
   times too small is correct.

   The handshake is ordinary TCP: SYN out, SYN/ACK back, bare ACK out.
   Then data rides on ACK packets and each arriving payload is ACKed
   at once, which usbmuxd's own comment says the device prefers.

   ── WHAT THIS CANNOT DO, NAMED RATHER THAN DISCOVERED ────────────────

   ONE CONNECTION AT A TIME. usbmuxd keeps a collection of them; this
   keeps one, because stage 1 and stage 2 are one conversation with
   lockdownd and a table of connections with nothing to put in it is
   the fixture-that-cannot-distinguish-anything mistake in the driver
   instead of the suite.

   NO REASSEMBLY. A mux packet that arrives split across two USB
   transfers is REFUSED AND NAMED, not stitched. usbmuxd reassembles
   into a 64 KB buffer; this kernel's xHCI back-end bounces every
   transfer through 2048 bytes, so the ceiling is the controller's and
   a reassembly buffer above it would not raise it.

   THE WINDOW IS WHAT THIS CAN ACTUALLY TAKE, which is the deliberate
   difference from usbmuxd. It advertises 131072 because it has a
   262144-byte buffer behind it. MUX_BUF is 2048, so a 128 KB window
   would invite exactly the overflow the advertisement is supposed to
   prevent. MUX_WINDOW is DERIVED from MUX_BUF in usb/usbmux.c and
   nothing may state it independently -- the driver and its back-end
   each holding a separate idea of one bound is exactly the shape of
   0.8.7's transfer-size bug.
   ════════════════════════════════════════════════════════════════════ */

/* The interface this claims. Vendor-specific, so the triple is the only
   thing that identifies it. Confirmed against usbmuxd's src/usb.h,
   which spells the same three numbers INTERFACE_CLASS 255,
   INTERFACE_SUBCLASS 254, INTERFACE_PROTOCOL 2. */
#define MUX_IF_CLASS     0xFF
#define MUX_IF_SUBCLASS  0xFE
#define MUX_IF_PROTOCOL  0x02

/* What the HOST writes, per usbmuxd's send_packet(). */
#define MUX_MAGIC        0xFEEDFACEu

/* ── WHAT THE DEVICE WRITES, which is NOT the same value ──────────────
   Off the wire on the HP, 30 October 2026: an iPhone 05AC:12A8's
   SYN/ACK to lockdownd carried 0xFACEFACE where this driver expected
   0xFEEDFACE, and v0.8.7.7 refused the packet over it.

   usbmuxd never notices, because it never reads the field: src/device.c
   writes the magic in send_packet() and device_data_input() validates
   only the length. A receiver that checks a field the working
   implementation ignores is a receiver that can refuse working
   devices, and this one did.

   Kept as a named constant rather than deleted, because "the magic we
   send and the magic we get back differ" is a fact about the protocol
   worth stating once, where the next person will find it. Neither
   value is a gate -- see mux_recv(). */
#define MUX_MAGIC_DEVICE 0xFACEFACEu

#define MUX_PROTO_VERSION 0
#define MUX_PROTO_CONTROL 1
#define MUX_PROTO_SETUP   2
#define MUX_PROTO_TCP     6

/* "Nothing received from you yet", written into rx_seq by the SETUP
   packet. Not a count. */
#define MUX_RX_SEQ_NONE   0xFFFFu

/* lockdownd's port on the device. 62078 == 0xF27E, which is the literal
   libimobiledevice writes in lockdownd_client_new(). */
#define MUX_PORT_LOCKDOWN 62078

/* The TCP header protocol 6 carries: a BSD struct tcphdr, so the
   offsets are the ones every packet dump already uses. */
#define MUX_TCP_HDR    20
#define MUX_TH_SPORT    0
#define MUX_TH_DPORT    2
#define MUX_TH_SEQ      4
#define MUX_TH_ACK      8
#define MUX_TH_OFF     12   /* high nibble: header words. 5. */
#define MUX_TH_FLAGS   13
#define MUX_TH_WIN     14   /* in units of 256 bytes */
#define MUX_TH_SUM     16   /* always zero; the mux is the reliable layer */
#define MUX_TH_URP     18

#define MUX_TH_FIN   0x01
#define MUX_TH_SYN   0x02
#define MUX_TH_RST   0x04
#define MUX_TH_PSH   0x08
#define MUX_TH_ACK_F 0x10

/* Find the Apple Mobile Device interface on the bus, select the
   configuration that carries it, claim it, and record its endpoints.
   Returns USB_OK, or a negative USB_E*; every refusal is logged by
   name, because each one has a different cause.

   This does NOT send anything. Opening the channel is separate, so a
   boot can establish that the interface is claimable without poking a
   phone somebody is using. */
int usbmux_open(void);

/* Let the interface go, before the USB device table is rebuilt. */
void usbmux_close(void);

/* Is one claimed right now. */
int usbmux_present(void);

/* ── stage 0: the version exchange ────────────────────────────────────
   Send VERSION, read what comes back, and send SETUP if the device
   says version 2. Returns the device's major version, 0 if it did not
   answer, or a negative USB_E*.

   Confirmed on the HP, 26 October: the phone answers 2. */
int usbmux_handshake(void);

/* What `usbmux` prints: the claimed interface, the endpoints, and the
   RAW bytes of the last packet received, uninterpreted. */
int usbmux_dump(void);

/* Read one packet in the framing this connection settled on. Returns
   the payload length, 0 for nothing yet, or a negative USB_E*. The
   payload is at usbmux_payload(). */
int usbmux_recv_packet(uint32_t *proto_out);
const void *usbmux_payload(void);

/* ── the connection layer, v0.8.7.7 ───────────────────────────────────

   usbmux_connect() runs the three-way handshake to a port on the
   device and returns USB_OK, or a negative USB_E*. It calls
   usbmux_handshake() first, which is idempotent, so a caller does not
   have to remember the order.

   A refusal is distinguishable from a failure on purpose: a device
   that answers RST has told you the port is closed, which is a
   different fact from a device that said nothing. USB_ESTALL is not
   reused for it -- see usbmux_refused() below. */
int usbmux_connect(uint16_t dport);

/* Did the last usbmux_connect() fail because the DEVICE refused the
   port (RST), rather than because the exchange broke? A closed port
   and a broken channel send you to different places, and collapsing
   them into one error number is the mistake `cc_name()` exists to
   stop one layer down. */
int usbmux_refused(void);

/* Is a connection up. */
int usbmux_connected(void);

/* The most bytes one packet on this connection can carry, derived from
   MUX_BUF in usb/usbmux.c. A caller sizing a buffer asks rather than
   restating it: two places holding one bound independently is how
   0.8.7's transfer-size bug happened, one layer down. */
int usbmux_max_payload(void);

/* ── v0.8.9.5 ────────────────────────────────────────────────────────
   Why the last call refused, as a sentence, for a caller that is going
   to put it on a screen. Never null. The numeric code says which LAYER
   gave up; this says what the wire did, and the two are not the same
   question -- a device that closes a connection and a controller that
   corrupts a packet both reached usbmux_recv() as USB_EXFER until this
   release. */
const char *usbmux_why(void);

/* ── v0.8.9.7 ────────────────────────────────────────────────────────
   The last message the device sent in a CONTROL packet (protocol 1),
   as printable text, or 0 if it has sent none. *type_out: 3 is an
   error, 7 information (usbmuxd's meanings). These used to be taken as
   a fault that dropped the connection; they are the phone explaining
   itself, and are kept across reconnects so a message sent as one
   connection died can be read after it. */
const char *usbmux_device_said(int *type_out, uint32_t *count_out);

/* v0.8.9.7: zero-length packets sent after transfers that were an exact
   multiple of the endpoint's maximum packet size. */
uint32_t usbmux_zlps(void);

/* Send `len` bytes on the open connection. Returns USB_OK or a
   negative USB_E*. The payload rides on an ACK packet, which is what
   usbmuxd does; there is no PSH. */
int usbmux_send(const void *data, int len);

/* Read whatever has arrived on the open connection, up to `max` bytes.
   Returns the byte count, 0 if nothing came, or a negative USB_E*.
   Every payload is ACKed before this returns, because the device
   stops sending if it is not.

   A driver may not depend on the bottom half, so this POLLS. A caller
   wanting a whole reply calls it until it has one, which is what
   usb/lockdown.c does, and 0.8.8's tick is what turns that into
   something that does not spin. */
int usbmux_recv(void *buf, int max);

/* ── a request that does not fit one packet ──────────────────────────
   v0.8.9.0. usbmux_send() refuses more than usbmux_max_payload()
   bytes by name, which was right for every message through stage 2
   and wrong for stage 3's pair request: three certificates and a
   public key run to seven or eight kilobytes.

   This splits the data across as many packets as it takes. TCP is a
   byte stream and lockdownd reads a four-byte length and then that
   many bytes, so where the packet boundaries fall is not its concern.

   Between segments it takes one non-blocking look at the wire, to
   collect the ACKs the device sends and the window they carry. If a
   look turns up DATA instead, the data is stashed and acked and the
   next usbmux_recv() returns it first -- dropping it would lose bytes
   the device considers delivered.

   `sent_out`, if given, receives how many bytes made it onto the wire.
   On a failure part way through that is the useful number: the stream
   is at an offset lockdownd cannot parse from, so the connection has
   to come down, and the count says where it stopped.

   On the device's advertised window: this WARNS and sends rather than
   blocking. usb/usbmux.c's comment on the function says exactly why,
   and it is a decision rather than an omission. */
int usbmux_send_stream(const void *data, int len, int *sent_out);

/* Bytes waiting in the mid-send stash (the return), and the two
   counters worth seeing in a dump: how often the stash was needed,
   and how often a segment went out past the device's window. */
int usbmux_pending(uint32_t *hits, uint32_t *win_short);

/* v0.8.9.9. The source port the last connect used -- a fresh one each
   time, see MUX_SPORT in usb/usbmux.c -- and how many packets have been
   handed to a caller in more than one piece. */
uint16_t usbmux_last_sport(void);
uint32_t usbmux_split_reads(void);

/* Tear the connection down: RST, the way usbmuxd closes one. Leaves
   the interface claimed, so a second connection costs no handshake. */
void usbmux_disconnect(void);

/* The connection's state, for `usb lockdown` to print. Any pointer may
   be null. `win` is the window in BYTES, not the 256-byte units the
   header carries, because the header's units are the thing most
   easily misread. */
void usbmux_conn_state(uint16_t *sport, uint16_t *dport,
                       uint32_t *tx_seq, uint32_t *tx_ack,
                       uint32_t *win);
