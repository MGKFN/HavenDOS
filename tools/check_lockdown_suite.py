#!/usr/bin/env python3
"""check_lockdown_suite.py -- prove tests/test_lockdown.c can fail.

usb/lockdown.c is stage 1 of pairing, and it speaks a protocol read out
of libimobiledevice rather than off a wire. This tree has been here
twice:

  - usb/ethernet.c passed 110 checks and could not move a byte on real
    hardware, because its simulated controller accepted a transfer of
    any length while the real one refused anything over 64.
  - tests/test_usbmux.c passed 79 of 79 against a sixteen-byte mux
    header the phone has never used, because the device model and the
    driver were written by the same person on the same afternoon from
    the same guess.

So this harness is not here to show the framing is right. It is here to
pin it down tightly enough that one boot on the HP refutes it -- and in
particular to show that the two things most likely to be wrong are
checked rather than assumed:

  THE LENGTH PREFIX DOES NOT COUNT ITSELF, which is the opposite of the
  mux header four bytes below it in the same byte stream.

  THE REPLY ARRIVES IN TWO WRITES, because property_list_service.c
  sends the length and the body separately -- so a reader written for
  one packet gets four bytes and concludes the reply is empty.

Run:  python3 tools/check_lockdown_suite.py

NEVER in a background process. This patches the tree and restores it in
a `finally`; a container that kills it mid-run leaves an injected defect
in the source looking like real code. That has happened once, to
drivers/nvme.c, and `make test` is the only reason it did not ship.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "usb/lockdown.c"
TEST = "tests/test_lockdown.c"
CC = ["gcc", "-std=gnu99", "-no-pie", "-Iinclude",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    # ══════════════════════════════════════════════════════════════════
    # v0.8.9.3 -- THE REPLY BUDGET, which was counted in the wrong unit
    #
    # The defect that shipped was LD_POLL_TRIES: a count of polls, not a
    # duration, described in a comment as "a ten-second budget". Twenty
    # thousand empty bulk-INs finish in under ten milliseconds on the
    # HP, so GetValue DevicePublicKey always timed out and its reply
    # arrived in time to be read as the answer to the NEXT request.
    #
    # That exact defect cannot be injected here, and the reason is the
    # finding: in this fixture one poll costs one tick, so a poll count
    # and a tick count are the same number. A suite cannot reproduce a
    # machine where a poll costs half a microsecond. What it CAN check
    # is the behaviour the fix specifies, which is what these do.
    # ══════════════════════════════════════════════════════════════════
    ("a request that times out leaves the channel open",
     "lockdownd answers every request. If this side stops waiting, the "
     "answer still arrives -- into a stream whose next reader is the "
     "next request, which then gets the previous one's reply. For "
     "ever, and with nothing reporting an error",
     """        ld_desync("a request went unanswered");
""",
     """"""),

    ("a reply that stopped part way leaves the channel open",
     "the same desynchronisation, reached by a different door: the "
     "rest of that reply is still coming",
     """        ld_desync("a reply that stopped part way");
""",
     """"""),

    ("the budget is not reset by progress",
     "a reply in two packets with a slow device between them spends "
     "one budget across the whole exchange and gives up during the "
     "second wait, holding the length prefix with the body still on "
     "its way",
     """        t0 = get_ticks();       /* progress resets the budget */""",
     """        ;"""),

    ("the first reply of a connection gets the short budget",
     "the device may still be waking the service up, and the first "
     "exchange is the one with nothing cached behind it",
     """    budget = g_ld.recvd ? LD_REPLY_TICKS : LD_FIRST_TICKS;""",
     """    budget = LD_REPLY_TICKS;"""),

    ("the dump gives up when the connection is gone",
     "ld_desync() drops the channel on purpose, and the reply fragment "
     "it was holding is the most useful thing on the screen at that "
     "moment. Returning early throws away exactly what the teardown "
     "exists to let somebody look at",
     """        if (!g_ld.have_reply) return 0;""",
     """        return 0;"""),


    # ── THE LENGTH PREFIX, which is the whole of the framing ──────────
    ("the length prefix counts itself",
     "the mux header four bytes below this one DOES count itself, and "
     "the two are in the same byte stream. Four too many leaves "
     "lockdownd waiting for a request that never finishes -- so the "
     "symptom is a device that does not answer, which is the one "
     "symptom this release exists to tell apart from the others",
     """    ld_wr32(out, (uint32_t)len);""",
     """    ld_wr32(out, (uint32_t)(len + 4));"""),

    ("the length prefix is four too few",
     "the other direction: lockdownd reads a plist four bytes short "
     "and cannot parse it",
     """    ld_wr32(out, (uint32_t)len);""",
     """    ld_wr32(out, (uint32_t)(len - 4));"""),

    ("the length prefix is written little-endian",
     "it is big-endian -- htobe32 in internal_plist_send(). A 250-byte "
     "request declares 1627389952",
     """static void ld_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}""",
     """static void ld_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}"""),

    ("the reply's length prefix is read little-endian",
     "the same field in the other direction. A 250-byte reply is read "
     "as needing 1627389952 bytes",
     """static uint32_t ld_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}""",
     """static uint32_t ld_be32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] <<  8) |  (uint32_t)p[0];
}"""),

    ("the request is sent without its length prefix",
     "lockdownd reads the first four bytes of the XML as a length and "
     "waits for 1012762419 more. v0.8.9: this injection had gone STALE "
     "-- it still named usbmux_send(), which v0.8.8.10 replaced with "
     "usbmux_send_stream(), so it had silently stopped being applied. "
     "The harness reports a stale injection rather than counting it, "
     "which is the only reason it was noticed",
     """    r = usbmux_send_stream(out, len + 4, &sent);""",
     """    r = usbmux_send_stream(out + 4, len, &sent);"""),

    ("the reply's declared length is taken to include the prefix",
     "four bytes short: the plist is handed on with its closing tag "
     "cut off, and a scanner looking for a string near the start finds "
     "it anyway -- which is the worst case, because it reports success",
     """            need = 4 + (int)ld_be32(g_ld.reply);""",
     """            need = (int)ld_be32(g_ld.reply);"""),

    # ── THE ACCUMULATION, which is the other half ─────────────────────
    ("the reply is read in one packet and whatever came is believed",
     "property_list_service.c sends the length and the body as two "
     "separate writes, so a reader written for one packet gets FOUR "
     "BYTES and concludes the reply is empty. The whole loop exists "
     "for this and it is the part no single-packet fixture could show",
     """        if (need >= 0 && have >= need) break;""",
     """        break;"""),

    ("the accumulation overwrites itself instead of appending",
     "every packet lands at offset zero, so a reply in three packets "
     "is the last one with the first two's bytes behind it",
     """        r = g_ld.ssl_on
            ? tls_recv(LD_TLS_HANDLE, g_ld.reply + have,
                       (uint16_t)(LD_BUF - have), 500)
            : usbmux_recv(g_ld.reply + have, LD_BUF - have);""",
     """        r = g_ld.ssl_on
            ? tls_recv(LD_TLS_HANDLE, g_ld.reply, (uint16_t)LD_BUF, 500)
            : usbmux_recv(g_ld.reply, LD_BUF);"""),

    ("the byte count does not advance with what arrived",
     "the loop never reaches the declared length and the reply never "
     "completes",
     """        have += r;
        g_ld.packets++;""",
     """        g_ld.packets++;"""),

    ("a reply bigger than the buffer is accepted and truncated",
     "a truncated plist that still contains the string being looked "
     "for is the worst outcome available: it reports success on a "
     "reply nobody read",
     """            if (need < 4 || need > LD_BUF) {""",
     """            if (0) {"""),

    ("an incomplete reply is reported as a whole one",
     "the device stopped part way and the plist is cut off",
     """    if (have < need) {""",
     """    if (0) {"""),

    ("a reply with no length prefix at all is parsed",
     "fewer than four bytes arrived, so the length was never read and "
     "`need` is still -1",
     """    if (need < 0) {""",
     """    if (0) {"""),

    # ── THE EVIDENCE ──────────────────────────────────────────────────
    ("what arrived is discarded when the reply fails",
     "the reply most worth looking at is the one that did not parse, "
     "and `usb lockdown` prints it. Recording only on success throws "
     "away exactly the evidence -- the same argument as `usb mux`'s "
     "raw packet line, one layer up",
     """        if (r < 0) {
            /* Keep what arrived. A reply that broke half way through
               is evidence; discarding it on the way out is how the
               only informative screen in a release gets thrown away. */
            g_ld.reply_len  = have;
            g_ld.have_reply = have > 0;
            return r;
        }""",
     """        if (r < 0) return r;"""),

    ("the state is not cleared between queries",
     "a second query whose reply has no Type reports the FIRST one's, "
     "and the screen says lockdownd answered when it did not",
     """    g_ld.reply_len  = 0;
    g_ld.have_reply = 0;
    g_ld.packets    = 0;
    g_ld.type[0]    = 0;
    g_ld.error[0]   = 0;""",
     """    ;"""),

    # ── THE SCANNER ───────────────────────────────────────────────────
    ("the scanner takes the first string in the document",
     "a QueryType reply carries Request/QueryType BEFORE Type, so "
     "this reports the type as \"QueryType\" -- a real lockdownd "
     "string, which makes the result look like a plausible answer "
     "rather than a bug",
     """    s = ld_find(g_ld.reply + at, g_ld.reply_len - at, "<string>");
    if (s < 0) return 0;
    s += at + 8;""",
     """    s = ld_find(g_ld.reply, g_ld.reply_len, "<string>");
    if (s < 0) return 0;
    s += 8;"""),

    ("the scanner does not skip the opening tag",
     "the Type comes back as \"<string>com.apple.mobile.lockdown\"",
     """    s += at + 8;""",
     """    s += at;"""),

    ("the scanner reads past the closing tag",
     "everything from the Type to the end of the document",
     """    e = ld_find(g_ld.reply + s, g_ld.reply_len - s, "</string>");
    if (e < 0) return 0;""",
     """    e = g_ld.reply_len - s;
    if (e < 0) return 0;"""),

    ("the scanner is not length-bounded",
     "a plist is text today, but that is an assumption about somebody "
     "else's service -- and a reply with a zero in it would be "
     "searched only as far as the zero",
     """    for (i = 0; i + n <= haylen; i++) {""",
     """    for (i = 0; hay[i]; i++) {"""),

    ("the key is not anchored to a key tag",
     "\"Type\" appears inside \"QueryType\", so searching for the bare "
     "word finds the Request's value first. Widened to ld_key_string()'s "
     "copy in v0.8.7.9: ld_key_data() grew an identical three lines and "
     "the injection matched both",
     """    out[0] = 0;
    if (!g_ld.have_reply) return 0;

    /* "<key>" + key + "</key>" */
    if ((int)strlen(key) + 12 >= (int)sizeof pat) return 0;
    strcpy(pat, "<key>");
    strcat(pat, key);
    strcat(pat, "</key>");""",
     """    out[0] = 0;
    if (!g_ld.have_reply) return 0;

    if ((int)strlen(key) + 12 >= (int)sizeof pat) return 0;
    strcpy(pat, key);"""),

    ("the Type is not compared, only found",
     "a device answering any type at all would be reported as "
     "lockdownd, and stage 2 would be sent to whatever it is",
     """    if (!strcmp(g_ld.type, LOCKDOWN_TYPE)) {""",
     """    if (1) {"""),

    ("an Error reply is reported as a transport failure",
     "every byte of framing worked and lockdownd refused the request "
     "itself. Collapsing that into the transport's error number sends "
     "the next person to re-check a framing that was perfect",
     """    if (g_ld.error[0]) {
        klog(KLOG_WARN, "lockdown: the device answered with an Error");
        return 0;
    }""",
     """    if (g_ld.error[0]) {
        klog(KLOG_WARN, "lockdown: the device answered with an Error");
        return USB_EXFER;
    }"""),

    # ── REDUNDANT, BY DESIGN, and recorded rather than deleted ───────
    #
    # "a reply with no Type is reported as a success" -- removing the
    # `!g_ld.type[0]` guard in lockdown_query_type(). Reported MISSED,
    # and correctly: with it gone the empty type falls through to the
    # strcmp against LOCKDOWN_TYPE below, which does not match, and 0
    # is returned either way.
    #
    # What the guard buys is the MESSAGE. "a reply with no Type in it"
    # sends somebody to the raw line in `usb lockdown`; "answered type
    # , expected com.apple.mobile.lockdown" sends them looking for a
    # device that answered something. Same return value, different
    # half hour.
    #
    # The guard stays. A log line is not nothing -- v0.8.7 lost a
    # release to a format string in the one diagnostic it depended
    # on -- but it is not behaviour either, and this suite checks
    # behaviour. Recorded so nobody writes the injection again and
    # concludes there is a hole.

    # ── THE REQUEST ───────────────────────────────────────────────────
    ("the request does not ask for QueryType",
     "lockdownd answers an unknown request with an Error",
     """    "\\t<string>QueryType</string>\\n\"""",
     """    "\\t<string>QueryTyp</string>\\n\""""),

    ("the QueryType request carries no Request key",
     "a plist lockdownd cannot act on. Anchored to LD_QUERYTYPE's own "
     "indentation: the GetValue builder grew the same line at a "
     "different indent and the injection matched both",
     """    "\\t<key>Request</key>\\n"
    "\\t<string>QueryType</string>\\n\"""",
     """    "\\t<key>Requst</key>\\n"
    "\\t<string>QueryType</string>\\n\""""),

    ("the QueryType request does not say who is asking",
     "the Label is what the phone shows the user in the trust prompt. "
     "On a release whose whole purpose is finding out whether the "
     "phone trusts this host, the prompt naming the host is the point",
     """    "\\t<key>Label</key>\\n"
    "\\t<string>HavenDOS</string>\\n\"""",
     """    "\\t<key>Label</key>\\n"
    "\\t<string></string>\\n\""""),

    # ── CONNECTING ────────────────────────────────────────────────────
    ("a QueryType is attempted with no connection",
     "a send into a closed channel, and a poll for a reply that cannot "
     "come. Anchored past the guard: lockdown_get_value() grew the "
     "identical three lines and has its own injection",
     """    if (!g_ld.connected || !usbmux_connected()) {
        klog(KLOG_WARN, "lockdown: not connected");
        return USB_ENODEV;
    }

    r = ld_send_plist(LD_QUERYTYPE);""",
     """    r = ld_send_plist(LD_QUERYTYPE);"""),

    ("connect goes to the wrong port",
     "lockdownd listens on 62078 and nothing else does",
     """    r = usbmux_connect(MUX_PORT_LOCKDOWN);""",
     """    r = usbmux_connect(MUX_PORT_LOCKDOWN + 1);"""),

    ("connect does not claim the interface first",
     "a connection over a channel nobody opened",
     """    if (!usbmux_present()) {
        r = usbmux_open();
        if (r < 0) {
            klog(KLOG_WARN, "lockdown: no Apple Mobile Device interface "
                 "to talk over (%d)", (uint32_t)r);
            return r;
        }
    }""",
     """    ;"""),

    ("connect reopens a connection that is already up",
     "the conversation in progress is torn down and restarted under "
     "whatever was using it",
     """    if (g_ld.connected && usbmux_connected()) return USB_OK;""",
     """    ;"""),

    ("a refused connection is reported as a success",
     "the first version of this injection only disabled a log line "
     "and was correctly MISSED -- an injection that does not change "
     "behaviour is not a defect. This one takes the failure branch "
     "out, so the flag is set over a connection the device refused "
     "and a query is attempted into it",
     """    r = usbmux_connect(MUX_PORT_LOCKDOWN);
    if (r < 0) {""",
     """    r = usbmux_connect(MUX_PORT_LOCKDOWN);
    if (0) {"""),

    ("forgetting sends a reset after all",
     "`usb rescan` calls it before usb_enumerate() rewalks the bus, "
     "and the device it would address may be the one that was just "
     "unplugged -- which is usually why the rescan is happening",
     """void lockdown_forget(void)
{""",
     """void lockdown_forget(void)
{
    if (g_ld.connected) usbmux_disconnect();"""),

    ("closing does not tell the device",
     "the device keeps the connection, and the next SYN on the same "
     "ports is a duplicate of one it thinks is open",
     """    if (g_ld.connected && usbmux_connected()) usbmux_disconnect();""",
     """    ;"""),

    # ── STAGE 2: GetValue, and the base64 under it ───────────────────

    ("the request asks for the wrong thing",
     "GetValue is the request lockdownd answers with a value; "
     "anything else gets an Error",
     """        "\\t<string>GetValue</string>\\n\"""",
     """        "\\t<string>GetValues</string>\\n\""""),

    ("the key is not put in the request",
     "every GetValue would ask for the same empty key",
     """    strcat(req, key);""",
     """    strcat(req, "");"""),

    ("a value that is <data> is looked for as a <string>",
     "DevicePublicKey comes back as base64 in a <data> element. The "
     "stage-1 scanner reads <string> and finds nothing at all",
     """    s = ld_find(g_ld.reply + at, g_ld.reply_len - at, "<data>");
    if (s < 0) return -1;
    s += at + 6;""",
     """    s = ld_find(g_ld.reply + at, g_ld.reply_len - at, "<string>");
    if (s < 0) return -1;
    s += at + 8;"""),

    ("the <data> scanner does not skip its opening tag",
     "the value comes back with \"<data>\" on the front of it, which "
     "is not base64 and is refused -- so the symptom is a key that "
     "will not decode rather than one that is six characters long",
     """    s += at + 6;""",
     """    s += at;"""),

    ("the <data> scanner reads past the closing tag",
     "everything from the value to the end of the document, which is "
     "not base64 either",
     """    e = ld_find(g_ld.reply + s, g_ld.reply_len - s, "</data>");
    if (e < 0) return -1;""",
     """    e = g_ld.reply_len - s;
    if (e < 0) return -1;"""),

    ("the value is not anchored to its key",
     "the FIRST <data> in the document rather than the one after "
     "<key>Value</key>",
     """    at = ld_find(g_ld.reply, g_ld.reply_len, pat);
    if (at < 0) return -1;

    s = ld_find(g_ld.reply + at, g_ld.reply_len - at, "<data>");""",
     """    at = 0;

    s = ld_find(g_ld.reply + at, g_ld.reply_len - at, "<data>");"""),

    # ── THE DECODER. The whole reason stage 2 needs one of its own ───
    ("the decoded value is not terminated",
     "lockdown_value() returns it as a string for the dump to print",
     """    g_ld.val_len = n;
    g_ld.val[n]  = 0;""",
     """    g_ld.val_len = n;"""),

    ("a stale value survives a request that returned none",
     "a screen saying stage 2 worked on a boot where it did not -- "
     "and stage 3 would build a pair record around the old key",
     """    g_ld.val_len    = 0;
    g_ld.val[0]     = 0;""",
     """    ;"""),

    ("an Error reply still reports a value",
     "lockdownd refused the request and the driver answers with "
     "whatever the scanner found",
     """    if (g_ld.error[0]) {
        klog(KLOG_WARN, "lockdown: GetValue %s refused -- %s", key,
             g_ld.error);
        return 0;
    }""",
     """    ;"""),

    ("a key too long for the request buffer is interpolated anyway",
     "the one place a caller's string reaches a fixed buffer in this "
     "file",
     """    if (strlen(key) > 64) return USB_EPARAM;""",
     """    ;"""),

    ("GetValue is attempted with no connection",
     "a send into a closed channel",
     """    if (!g_ld.connected || !usbmux_connected()) {
        klog(KLOG_WARN, "lockdown: not connected");
        return USB_ENODEV;
    }
    if (!key || !*key) return USB_EPARAM;""",
     """    if (!key || !*key) return USB_EPARAM;"""),

    ("a <string> value is not read at all",
     "some keys answer with a string -- ProductVersion is one -- and "
     "a driver that handled only <data> would be wrong for half of "
     "GetValue",
     """        if (ld_key_string("Value", (char *)g_ld.val, LD_VAL)) {""",
     """        if (0) {"""),

    # ══════════════════════════════════════════════════════════════════
    # v0.8.9: THE SESSION
    #
    # StartSession, then TLS. The defects that matter here are not about
    # cryptography -- tests/test_tlscc.c owns that, against openssl --
    # they are about WHICH PATH a request takes and WHEN. A request that
    # goes out in the clear on an encrypted session is answered by
    # nothing and reports no error.
    # ══════════════════════════════════════════════════════════════════
    ("the session is called encrypted because the device ASKED for it",
     "ssl_on would then be set from intent rather than from success, "
     "and the next request would be encrypted into a device that is "
     "not decrypting. Everything above that line travels in the clear "
     "and everything below it does not",
     """    g_ld.ssl_on = 1;
    klog(KLOG_OK, "lockdown: the session is encrypted""",
     """    g_ld.ssl_on = g_ld.ssl_wanted;
    klog(KLOG_OK, "lockdown: the session is encrypted"""),

    ("the session is marked encrypted BEFORE the handshake",
     "the ClientHello itself would then be encrypted with keys that do "
     "not exist yet",
     """    g_ld.ssl_broken = 0;

    /* NO HOSTNAME.""",
     """    g_ld.ssl_broken = 0;
    g_ld.ssl_on = 1;

    /* NO HOSTNAME."""),

    ("SSL is turned on with no session",
     "lockdownd asks for SSL in the StartSession reply, so there is no "
     "legitimate order in which the handshake comes first",
     """    if (!g_ld.session) {
        klog(KLOG_WARN, "lockdown: SSL without a session -- "
             "StartSession first");
        return USB_EPARAM;
    }""",
     """    ;"""),

    # ── the boolean scanner ───────────────────────────────────────────
    ("the boolean scanner takes the first <true/> after the key",
     "ld_key_string() cannot read a boolean -- a plist boolean is an "
     "empty element with no <string> near it -- so this is a second "
     "scanner, and the thing it must not do is run past its own key "
     "into the NEXT one's value. A reply whose EnableSessionSSL is a "
     "string and whose next key is <true/> would report SSL requested",
     """    if (i + 7 <= g_ld.reply_len &&
        memcmp(g_ld.reply + i, "<true/>", 7) == 0) { *out = 1; return 1; }""",
     """    if (ld_find(g_ld.reply + i, g_ld.reply_len - i, "<true/>") >= 0)
        { *out = 1; return 1; }"""),

    ("<false/> is read as true",
     "a device that explicitly declined SSL would be sent an encrypted "
     "request",
     """        memcmp(g_ld.reply + i, "<false/>", 8) == 0) { *out = 0; return 1; }""",
     """        memcmp(g_ld.reply + i, "<false/>", 8) == 0) { *out = 1; return 1; }"""),

    # ── StartSession ──────────────────────────────────────────────────
    ("an Error in the StartSession reply is not looked for",
     "InvalidHostID means the stored pair record is not the one this "
     "device remembers, and it is the ONLY thing that distinguishes a "
     "lost record from a pairing that was refused",
     """    ld_key_string("Error", g_ld.error, (int)sizeof g_ld.error);
    if (g_ld.error[0]) {
        klog(KLOG_WARN, "lockdown: StartSession refused -- %s",
             g_ld.error);
        return 0;
    }

    if (!ld_key_string(""",
     """    if (!ld_key_string("""),

    ("a session is recorded without a SessionID",
     "StopSession would then be sent with an empty SessionID",
     """    if (!ld_key_string("SessionID", g_ld.session_id,
                       (int)sizeof g_ld.session_id)) {
        klog(KLOG_WARN, "lockdown: a StartSession reply with no "
             "SessionID in it");
        return 0;
    }
    g_ld.session = 1;""",
     """    ld_key_string("SessionID", g_ld.session_id,
                  (int)sizeof g_ld.session_id);
    g_ld.session = 1;"""),

    ("a HostID too long for the request buffer is interpolated anyway",
     "it comes off a FILE now. The same shape of defect v0.8.8.11 "
     "found in two base64 helpers that could not refuse",
     """    if (strlen(host_id) > 64 || strlen(buid) > 64) return USB_EPARAM;""",
     """    ;"""),

    ("the previous session's state survives a new StartSession",
     "a failed StartSession after a successful one would leave the old "
     "SessionID and the old SSL answer in place",
     """    g_ld.session_id[0] = 0;
    g_ld.session = 0;
    g_ld.ssl_wanted = 0;

    strcpy(req,""",
     """    strcpy(req,"""),

    # ── StopSession and the teardown order ────────────────────────────
    ("the TLS session is torn down BEFORE StopSession is sent",
     "StopSession travels INSIDE the session. Closing first sends it "
     "in the clear to a device that is still decrypting",
     """    r = ld_send_plist(req);
    if (r >= 0) r = ld_recv_plist();

    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }""",
     """    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }
    r = ld_send_plist(req);
    if (r >= 0) r = ld_recv_plist();"""),

    ("close() disconnects the mux before closing the TLS session",
     "tls_close() sends a close_notify, and it has to travel down a "
     "channel that is still open",
     """    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }
    g_ld.session = 0;
    g_ld.session_id[0] = 0;

    /* Polite: tell the device the connection is going.""",
     """    if (g_ld.connected && usbmux_connected()) usbmux_disconnect();
    g_ld.connected = 0;
    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }
    g_ld.session = 0;
    g_ld.session_id[0] = 0;

    /* Polite: tell the device the connection is going."""),

    ("forget() closes the TLS session while still holding the channel",
     "the whole point of forget() is that the device may have been "
     "unplugged. A close_notify to it is a transfer that burns the "
     "poll budget waiting for a device that is not there",
     """    g_ld.connected = 0;
    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }
    g_ld.session = 0;
    g_ld.session_id[0] = 0;
    g_ld.ssl_broken = 0;
}""",
     """    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }
    g_ld.connected = 0;
    g_ld.session = 0;
    g_ld.session_id[0] = 0;
    g_ld.ssl_broken = 0;
}"""),

    ("forget() leaves the TLS slot claimed",
     "net/tls.c has three slots keyed by handle. A slot never released "
     "means the next handshake on this handle reuses a session whose "
     "keys belong to a device that has been unplugged",
     """    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }
    g_ld.session = 0;
    g_ld.session_id[0] = 0;
    g_ld.ssl_broken = 0;
}""",
     """    g_ld.ssl_on = 0;
    g_ld.session = 0;
    g_ld.session_id[0] = 0;
    g_ld.ssl_broken = 0;
}"""),

    # ── the routing, which is what the suite's XOR exists for ─────────
    ("requests go out in the clear even once the session is encrypted",
     "the framing goes INSIDE the TLS record layer, not beside it. "
     "lockdownd answers a plaintext request on an encrypted session "
     "with nothing at all",
     """    if (g_ld.ssl_on) {
        r = tls_send(LD_TLS_HANDLE, out, (uint16_t)(len + 4));""",
     """    if (0) {
        r = tls_send(LD_TLS_HANDLE, out, (uint16_t)(len + 4));"""),

    ("replies are read from the mux even once the session is encrypted",
     "the other direction: the reply arrives encrypted and is parsed "
     "as XML",
     """        r = g_ld.ssl_on
            ? tls_recv(LD_TLS_HANDLE, g_ld.reply + have,
                       (uint16_t)(LD_BUF - have), 500)
            : usbmux_recv(g_ld.reply + have, LD_BUF - have);""",
     """        r = usbmux_recv(g_ld.reply + have, LD_BUF - have);"""),

    ("an encrypted send that fails is reported as success",
     "the request never reached lockdownd and the caller goes on to "
     "read a reply that will never come",
     """        if (r < 0) {
            klog(KLOG_WARN, "lockdown: the encrypted request would not "
                 "send (%d)", (uint32_t)r);
            return USB_EXFER;
        }""",
     """        ;"""),

    # ── the transport ─────────────────────────────────────────────────
    ("a TLS record sent half way does not tear the session down",
     "half a record leaves the peer reading the next one at the wrong "
     "offset. It decrypts to rubbish and fails a MAC check several "
     "messages later, so the symptom is nowhere near the cause",
     """        if (sent > 0) {
            klog(KLOG_WARN, "lockdown: %d of %d TLS bytes went out -- "
                 "the record stream is misaligned and cannot be reused",
                 (uint32_t)sent, (uint32_t)len);
            g_ld.ssl_broken = 1;
            g_ld.ssl_on     = 0;
            g_ld.connected  = 0;
            usbmux_disconnect();
        }
        return -1;""",
     """        return -1;"""),

    ("the broken-stream latch is not consulted",
     "a later send would carry on down a stream that is permanently "
     "misaligned",
     """    if (g_ld.ssl_broken) return -1;""",
     """    ;"""),

    ("the transport reports a timeout as a dead connection",
     "usbmux_recv() returns 0 when nothing has arrived yet. net/tls.c's "
     "read loop asks again and counts the time itself, so a transport "
     "that returned -1 here would turn a slow device into a broken one",
     """    /* Nothing yet. NOT an error: net/tls.c's read loop asks again and
       counts the time itself, and a transport that reported a timeout
       as a dead connection would turn a slow device into a broken
       one. */
    return 0;""",
     """    return -1;"""),

    ("the transport sends down a channel that is already gone",
     "tls_close() attempts a close_notify after lockdown_forget() has "
     "let go of the connection, and this guard is what makes that safe",
     """    if (!g_ld.connected || !usbmux_connected()) return -1;
    if (g_ld.ssl_broken) return -1;""",
     """    if (g_ld.ssl_broken) return -1;"""),

    ("the transport reports a failed send as a success",
     "net/tls.c reads the SIGN of the return and nothing else. A "
     "failure reported as success means the record never went out and "
     "the peer is waiting for it",
     """        return -1;
    }
    g_ld.tls_tx++;""",
     """        return 0;
    }
    g_ld.tls_tx++;"""),

    # ── NOT REACHABLE, and recorded rather than written a second time ─
    #
    # "the transport claims a byte count it did not send" --
    # ld_tr_send() returning len+1 instead of len. UNDETECTABLE, and
    # the reason is worth keeping: net/tcp.c's tcp_send() returns 0 for
    # a complete write, so net/tls.c cannot compare the result against
    # the length it asked for without rejecting every successful send.
    # The contract is therefore the SIGN only, and a wrong count is not
    # a defect because the count means nothing.
    #
    # That was not written down anywhere until this injection asked for
    # it. include/tls.h says it now, and ld_tr_send() returns 0 so both
    # transports mean the same thing. An injection that cannot express
    # a reachable defect must not be counted as caught -- v0.8.6.3's
    # rule -- so it is recorded here instead.
    # ── NOT REACHABLE, and recorded rather than written a second time ─
    #
    # "a request longer than one packet is sent anyway" -- ld_send_plist()
    # refuses more than usbmux_max_payload(). The only request this file
    # sends is the QueryType plist, around 250 bytes, so nothing can
    # reach the bound.
    #
    # It becomes reachable at STAGE 3: the pair request carries an X.509
    # certificate and a public key and will be several kilobytes, which
    # is more than one packet and more than LD_BUF. Splitting a request
    # across packets is correct, is not written, and is refused by name
    # rather than half-sent -- so the guard is doing its job today by
    # saying no.
    #
    # Add the injection with the code that splits, in the release that
    # needs it. Recorded, not deleted, following v0.8.6.3's precedent:
    # an injection deleted without a word is one somebody writes again.
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """const char *lockdown_type(void)  { return g_ld.type; }""",
    """/* the Type string from the last reply, or "" */
const char *lockdown_type(void)  { return g_ld.type; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "lockdown", SRC, TEST, CC, BUGS, HARMLESS, timeout=60,
        preamble="Proving tests/test_lockdown.c can fail.\n"
                 "The framing here is read off libimobiledevice, not "
                 "off a wire -- and the two\nprevious times that was "
                 "true, the suite passed and the hardware did not."))
