#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/lockdown.h -- lockdownd, stage 1.

   usb/usbmux.c carries packets and, as of v0.8.7.7, a TCP connection.
   This is what runs over it: the service on the device that decides
   whether this host is allowed to do anything, including turn
   tethering on.

   ── WHY THIS IS A SEPARATE FILE ──────────────────────────────────────

   include/usbmux.h insists it is the transport and nothing else, and
   the road ahead is why that matters:

     1  QueryType answers                  <- this file
     2  GetValue returns the device key     this file, no crypto
     3  Pair is accepted                    this file, RSA and X.509
     4  the hotspot indicator appears       --

   Stage 3 brings a plist writer, RSA key generation and an X.509
   certificate into whatever file this lives in. None of that belongs
   in a USB driver. usbmuxd and libimobiledevice split along exactly
   this line -- usbmuxd's device.c owns the mux and its TCP,
   libimobiledevice's lockdown.c owns the conversation -- and the split
   is worth copying for the same reason.

   ── THE FRAMING, FROM UPSTREAM AND NOT FROM REASONING ────────────────

   libimobiledevice's common/property_list_service.c, internal_plist_
   send() and internal_plist_receive_timeout():

       uint32 length      big-endian
       char   plist[]     XML

   THE LENGTH DOES NOT COUNT ITSELF. That is the opposite of the mux
   header one layer down, which does, and the two are four bytes apart
   in the same byte stream. Getting it wrong by four is a request
   lockdownd waits forever to finish, which presents as a device that
   does not answer -- the symptom this whole release exists to tell
   apart from the others.

   AND IT ARRIVES IN TWO WRITES. property_list_service.c calls
   service_send() for the four length bytes and then again for the
   XML, so the length and the body are separate writes on the device
   side too and arrive as separate TCP packets. A reader that expects
   one packet gets four bytes and concludes the reply is empty. So
   this accumulates, and the accumulation is the part most likely to
   be the thing that is wrong on hardware -- which is why
   lockdown_dump() prints what was collected rather than what was
   understood.

   ── STAGE 1 IS A STRING ──────────────────────────────────────────────

       host -> { "Request": "QueryType" }
       dev  -> { "Request": "QueryType",
                 "Type":    "com.apple.mobile.lockdown" }

   That string is the whole of stage 1. It needs no cryptography, it
   changes nothing on the phone, and it is the first evidence that
   anything above the transport is listening.

   THERE IS NO XML PARSER HERE and there is not going to be one for a
   string. lockdown_type() finds <key>Type</key> and reads the
   <string> after it; lockdown_error() does the same for an Error.
   Stage 2 needs a real reader for a base64 key and that is stage 2's
   problem. A scanner that is honest about being a scanner beats a
   parser written in a hurry: it cannot silently mis-read a structure
   it was never looking at.
   ════════════════════════════════════════════════════════════════════ */

/* What lockdownd calls itself when it is lockdownd. */
#define LOCKDOWN_TYPE "com.apple.mobile.lockdown"

/* ── STAGE 2: the key the pair request is built around ────────────────
   GetValue's reply differs from QueryType's in the one way that
   matters: the value comes back as <data>, not <string> -- base64,
   wrapped across lines with tabs. Stage 1's scanner reads <string>
   and would find nothing at all here.

   The decoded bytes are a PEM RSA public key:

       -----BEGIN RSA PUBLIC KEY-----
       MIIBCgKCAQEA...
       -----END RSA PUBLIC KEY-----

   so there are TWO layers of base64 between the wire and the modulus.
   Stage 2 unwraps the first and stops. Stage 3 unwraps the second,
   parses the DER, and is where the cryptography starts. */
#define LOCKDOWN_KEY_DEVICE_PUBLIC "DevicePublicKey"

/* Open the channel, handshake, and connect to lockdownd's port.
   Returns USB_OK or a negative USB_E*. Every step logs its own
   refusal by name: a channel that would not open, a device that never
   answered VERSION, and a port that sent RST are three different
   facts and the next person needs to know which one happened. */
int lockdown_connect(void);

/* Stage 1. Send QueryType and read the reply.

   Returns 1 if the device answered with the lockdown type, 0 if it
   answered something else (which lockdown_type() then holds), or a
   negative USB_E*. The raw reply is kept either way -- see
   lockdown_dump(). */
int lockdown_query_type(void);

/* Stage 2. Ask lockdownd for one value by name.

   Returns 1 if the device returned a value, 0 if it answered without
   one (lockdown_error() then usually says why), or a negative USB_E*.
   The value is at lockdown_value(), decoded from the reply's base64
   and NUL-terminated.

   The connection must already be open -- stage 1 and stage 2 are one
   conversation, and lockdownd is happy to answer several requests on
   one connection. */
int lockdown_get_value(const char *key);

/* ── Stage 3. Send a request somebody else composed. ─────────────────
   usb/pair.c builds the Pair request -- several kilobytes of
   certificates, which are no business of the lockdown framing. This
   sends it and reads the reply, and parses neither: what a pairing
   reply MEANS is a pairing question.

   Returns 1 on a reply, or a negative USB_E*. The reply is at
   lockdown_reply(), lockdown_reply_len() bytes of it.

   ld_send_plist() splits it across mux packets as needed, and tears
   the connection down if it manages only part of it -- see that
   function on why a half-sent request cannot be retried. */
int         lockdown_request(const char *xml);
const char *lockdown_reply(void);
int         lockdown_reply_len(void);

/* ── v0.8.9. Stage 3's second half: a SESSION ─────────────────────────
   Pairing makes the device remember this host. A session is what the
   host does with that afterwards -- lockdownd refuses nearly every
   interesting request outside one, including the request that turns
   tethering on, and it will not open a session for a host it does not
   already trust. So this is the step that proves a pairing was real
   rather than merely accepted.

   Returns 1 on a session, 0 if the device answered without one
   (lockdown_error() then says why -- InvalidHostID means the stored
   pair record is not the one this device remembers), or a negative
   USB_E*.

   If the reply asks for SSL -- every current iOS does --
   lockdown_enable_ssl() must be called before anything else is sent,
   because lockdownd stops reading plaintext the moment it has asked. */
int lockdown_start_session(const char *host_id, const char *buid);

/* Upgrade the connection to TLS, with the host certificate out of the
   pair record as a CLIENT certificate -- the device is the server here.

   `cc` is a `const tls_client_cert_t *` from include/tls.h, taken as
   void* so that this header does not drag the TLS layer into every
   file that reads a lockdown reply. shell/paircmd.c builds it; this
   file never sees a private key, only a callback that can sign.

   `seed` seeds net/tls.c's per-handshake PRNG. It comes from the
   caller because usb/ has no entropy source of its own.

   Returns 1 on an encrypted session, or a negative code: TLS_ERR_* from
   the handshake, or USB_E* before it. g_tls_last_stage and
   tls_stage_name() say how far it got, and `tlsinfo` prints both. */
int lockdown_enable_ssl(const void *cc, uint32_t seed);

/* End the session politely. The request travels INSIDE it, so this
   sends StopSession first and tears the TLS down afterwards. Safe to
   call with no session: it returns 0 and does nothing. */
int lockdown_stop_session(void);

const char *lockdown_session_id(void);
int lockdown_session(void);      /* a SessionID came back            */
int lockdown_ssl_wanted(void);   /* the device asked for SSL         */
int lockdown_ssl(void);          /* the handshake SUCCEEDED -- never
                                    what the device merely asked for */

/* The decoded value from the last lockdown_get_value(), or "". It is
   NUL-terminated for convenience; lockdown_value_len() is the truth,
   because a DER key has zero bytes in it and stage 3 will want the
   bytes rather than the string. */
const char *lockdown_value(void);
int lockdown_value_len(void);

/* The Type string from the last reply, or "" if there was none. */
const char *lockdown_type(void);

/* The Error string from the last reply, or "" if there was none.
   lockdownd answers a request it does not like with an Error rather
   than a refusal at the transport, so a conversation can fail with
   every byte of framing correct. */
const char *lockdown_error(void);

/* What `usb lockdown` prints: the connection, the counters, and THE
   RAW REPLY as it arrived, before this file decided what it meant.
   Same argument as `usb mux`: a layer that silently parses cannot be
   asked whether it parsed the right thing. */
int lockdown_dump(void);

/* Close the connection, telling the device first. The usbmux
   interface stays claimed, so a second connection costs no
   handshake. */
void lockdown_close(void);

/* Drop the connection WITHOUT sending anything. For `usb rescan`,
   which calls this before the bus is rewalked: the device may be the
   one that was just unplugged, and a farewell packet to it is a
   transfer that waits for a device that is not there. */
void lockdown_forget(void);
