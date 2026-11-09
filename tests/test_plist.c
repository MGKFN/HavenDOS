/* ================================================================
   tests/test_plist.c -- property lists, judged by Python's plistlib
   ================================================================
   Pairing stage 3, piece two. net/plist.c writes the XML property
   lists every lockdownd message is made of, and the failure mode
   worth fearing is not a crash: it is a document that LOOKS right in
   a terminal, is accepted by nothing, and makes lockdownd hang up
   without saying why.

   So this suite does not read its own output. tests/plist_judge.py
   hands every document to Python's plistlib, which is an independent
   implementation of this format, and asks it two separate questions:
   does this parse at all, and did the value come back as the value
   that went in. Those get different exit codes because they are
   different findings.

   The checks that matter most are the ones nobody writes by hand:

     - a string carrying & and < , which is every certificate
       subject line with a comma in it, and the first thing a
       hand-rolled writer gets wrong
     - base64 of all 256 byte values, since a public key is binary
       and a table with one wrong character is a key off by one bit
     - the 1/2/3-byte padding cases, each of which is a different
       branch
     - a buffer one byte too small, which must refuse rather than
       write past the end -- checked with a 0xAA canary across the
       whole rest of the buffer, not by trusting the length field

   And the refusals are checked as hard as the successes, because a
   writer that emits an unbalanced document silently is worse than
   one that fails: the unbalanced document gets sent.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "types.h"
#include "../include/b64.h"
#include "../net/b64.c"
#include "plist.h"
#include "../net/plist.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }

static void ck(const char *what, int ok){
    total++;
    if(ok){ printf("    ok    %s\n", what); }
    else   { printf("    FAIL  %s\n", what); fails++; }
}

#define PBUF 65536
static char   g_buf[PBUF];
static char   g_path[] = "/tmp/hd_plist.plist";

static void save(const plist_t *p){
    FILE *f = fopen(g_path, "wb");
    if(!f){ printf("    cannot open %s\n", g_path); return; }
    fwrite(p->buf, 1, p->len, f);
    fclose(f);
}

/* Hand the document to plistlib and ask it one question.
   0 = true, 1 = false, 2 = did not parse, 3 = bad test.

   The expression goes to a FILE, not onto the command line. Passing
   it as a shell argument worked for everything except the one check
   that mattered most -- a string containing both a double and a
   single quote, which no amount of single-quoting survives. The
   plist was correct and the test could not be run, which is the
   worst possible way for a suite to be wrong. */
static int ask(const char *expr){
    char cmd[512];
    FILE *f = fopen("/tmp/hd_plist.expr", "wb");
    int rc;
    if(!f) return -1;
    fputs(expr, f);
    fclose(f);
    snprintf(cmd, sizeof cmd,
             "python3 -I tests/plist_judge.py %s /tmp/hd_plist.expr "
             "2>/tmp/hd_plist.err", g_path);
    rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void judge(const char *what, const plist_t *p, const char *expr){
    int rc;
    if(!plist_ok(p)){ ck(what, 0); printf("          writer refused the document\n"); return; }
    save(p);
    rc = ask(expr);
    ck(what, rc == 0);
    if(rc != 0){
        printf("          judge exit %d%s\n", rc,
               rc == 2 ? " (plistlib could not parse it at all)" : "");
        system("sed 's/^/          | /' /tmp/hd_plist.err | head -6");
    }
}

/* A document that must NOT be produced. The writer's `ok` flag is the
   whole assertion: nothing is written out and plistlib is not
   consulted, because there is deliberately nothing to consult. */
static void refuses(const char *what, const plist_t *p){
    ck(what, !plist_ok(p));
}

/* ── canary ──────────────────────────────────────────────────────
   plist_init is told a capacity smaller than the buffer, and every
   byte past that capacity is 0xAA. A writer that honours cap cannot
   touch them; one that trusts its own length arithmetic might. */
static char g_canary[PBUF];
static void canary_init(uint32_t cap, plist_t *p){
    memset(g_canary, 0xAA, sizeof g_canary);
    plist_init(p, g_canary, cap);
}
static int canary_clean(uint32_t cap){
    uint32_t i;
    for(i = cap; i < sizeof g_canary; i++)
        if((unsigned char)g_canary[i] != 0xAAu) return 0;
    return 1;
}

int main(void){
    plist_t p;

    printf("\n=== test_plist: property lists, judged by plistlib ===\n");

    if(system("python3 -c 'import plistlib' >/dev/null 2>&1") != 0){
        printf("\n  plistlib unavailable -- SKIP (nothing here can be judged)\n");
        return 77;
    }

    /* ── the smallest real document ──────────────────────────── */
    head("a dict with one string");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p);
    plist_dict_begin(&p);
    plist_kv_string(&p, "Request", "QueryType");
    plist_dict_end(&p);
    plist_end(&p);
    ck("the writer is happy", plist_ok(&p));
    ck("it is NUL terminated", p.len < PBUF && p.buf[p.len] == 0);
    ck("it ends with a newline", p.len > 1 && p.buf[p.len-1] == '\n');
    judge("plistlib parses it", &p, "len(p) == 1");
    judge("the value survives", &p, "p[\"Request\"] == \"QueryType\"");
    judge("it is a dict, not a one-element list", &p, "type(p) is dict");

    /* The real QueryType message usb/lockdown.c sends today, so this
       suite covers the thing already in the field, not only the thing
       being built. */
    head("the QueryType message as lockdown.c sends it");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p);
    plist_dict_begin(&p);
    plist_kv_string(&p, "Label",   "HavenDOS");
    plist_kv_string(&p, "Request", "QueryType");
    plist_dict_end(&p);
    plist_end(&p);
    judge("both keys present", &p, "sorted(p) == [\"Label\", \"Request\"]");
    judge("Label is ours",     &p, "p[\"Label\"] == \"HavenDOS\"");
    judge("Request is right",  &p, "p[\"Request\"] == \"QueryType\"");

    /* ── escaping ────────────────────────────────────────────────
       An X.509 subject is "C=US, O=Apple Inc. & co" often enough, and
       a HostID is a UUID, but the device sends back error strings
       with angle brackets in them. Any of these unescaped is a
       document that does not parse. */
    head("XML escaping");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p);
    plist_dict_begin(&p);
    plist_kv_string(&p, "amp",   "a & b");
    plist_kv_string(&p, "lt",    "a < b");
    plist_kv_string(&p, "gt",    "a > b");
    plist_kv_string(&p, "all",   "<&>&<");
    plist_kv_string(&p, "quote", "he said \"no\" and 'no'");
    plist_kv_string(&p, "entityish", "&amp; is not an ampersand");
    plist_dict_end(&p);
    plist_end(&p);
    judge("it still parses with & and < in it", &p, "len(p) == 6");
    judge("ampersand round-trips",  &p, "p[\"amp\"] == \"a & b\"");
    judge("less-than round-trips",  &p, "p[\"lt\"]  == \"a < b\"");
    judge("greater-than round-trips", &p, "p[\"gt\"] == \"a > b\"");
    judge("a run of them round-trips", &p, "p[\"all\"] == \"<&>&<\"");
    judge("quotes are left alone",  &p,
          "p[\"quote\"] == \"he said \\\"no\\\" and 'no'\"");
    judge("a literal &amp; is not double-decoded", &p,
          "p[\"entityish\"] == \"&amp; is not an ampersand\"");

    head("escaping in KEYS, not just values");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p);
    plist_dict_begin(&p);
    plist_kv_string(&p, "a & b < c", "v");
    plist_dict_end(&p);
    plist_end(&p);
    judge("the key round-trips", &p, "list(p) == [\"a & b < c\"]");

    head("an empty string is a string, not a missing key");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p);
    plist_dict_begin(&p);
    plist_kv_string(&p, "empty", "");
    plist_dict_end(&p);
    plist_end(&p);
    judge("present and empty", &p, "p[\"empty\"] == \"\" and len(p) == 1");

    /* ── integers ───────────────────────────────────────────────── */
    head("integers");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p);
    plist_dict_begin(&p);
    plist_kv_integer(&p, "zero",  0);
    plist_kv_integer(&p, "one",   1);
    plist_kv_integer(&p, "nine",  9);
    plist_kv_integer(&p, "ten",   10);
    plist_kv_integer(&p, "neg1",  -1);
    plist_kv_integer(&p, "big",   2147483647);
    plist_kv_integer(&p, "small", -2147483647 - 1);
    plist_kv_integer(&p, "port",  62078);         /* lockdownd's port */
    plist_dict_end(&p);
    plist_end(&p);
    judge("zero is 0 and not empty",  &p, "p[\"zero\"] == 0");
    judge("one",                      &p, "p[\"one\"] == 1");
    judge("a single digit",           &p, "p[\"nine\"] == 9");
    judge("two digits, right order",  &p, "p[\"ten\"] == 10");
    judge("negative",                 &p, "p[\"neg1\"] == -1");
    judge("INT32_MAX",                &p, "p[\"big\"] == 2147483647");
    judge("INT32_MIN (the negate-overflow case)", &p,
          "p[\"small\"] == -2147483648");
    judge("62078 reads as a number, not a string", &p,
          "p[\"port\"] == 62078 and type(p[\"port\"]) is int");

    /* ── booleans ──────────────────────────────────────────────── */
    head("booleans");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p);
    plist_dict_begin(&p);
    plist_kv_bool(&p, "t", 1);
    plist_kv_bool(&p, "f", 0);
    plist_kv_bool(&p, "truthy", 42);
    plist_dict_end(&p);
    plist_end(&p);
    judge("true is True",   &p, "p[\"t\"] is True");
    judge("false is False", &p, "p[\"f\"] is False");
    judge("they are bools, not the strings \"true\"/\"false\"", &p,
          "type(p[\"t\"]) is bool and type(p[\"f\"]) is bool");
    judge("any nonzero is true", &p, "p[\"truthy\"] is True");

    /* ── data / base64 ──────────────────────────────────────────
       A public key, a certificate and a challenge are all <data>, and
       a base64 table with one character out of place is a key that is
       wrong in a way no amount of staring at the XML reveals. */
    head("base64 <data>");
    {
        uint8_t all256[256], three[3] = {0xDE,0xAD,0xBE}, one[1] = {0xFF};
        uint8_t two[2] = {0x00,0x01};
        uint8_t big[1200];
        int i;
        for(i = 0; i < 256; i++)  all256[i] = (uint8_t)i;
        for(i = 0; i < 1200; i++) big[i] = (uint8_t)((i * 31 + 7) & 0xFF);

        plist_init(&p, g_buf, PBUF);
        plist_begin(&p);
        plist_dict_begin(&p);
        plist_kv_data(&p, "empty", all256, 0);
        plist_kv_data(&p, "one",   one, 1);
        plist_kv_data(&p, "two",   two, 2);
        plist_kv_data(&p, "three", three, 3);
        plist_kv_data(&p, "all",   all256, 256);
        plist_kv_data(&p, "big",   big, 1200);
        plist_dict_end(&p);
        plist_end(&p);
        judge("empty data is b'' and not absent", &p,
              "p[\"empty\"] == b\"\" and type(p[\"empty\"]) is bytes");
        judge("1 byte (two pad characters)", &p, "p[\"one\"] == b\"\\xff\"");
        judge("2 bytes (one pad character)",  &p, "p[\"two\"] == b\"\\x00\\x01\"");
        judge("3 bytes (no padding)", &p, "p[\"three\"] == b\"\\xde\\xad\\xbe\"");
        judge("ALL 256 byte values, byte-exact", &p,
              "p[\"all\"] == bytes(range(256))");
        judge("1200 bytes across many wrapped lines", &p,
              "len(p[\"big\"]) == 1200 and "
              "all(p[\"big\"][i] == ((i*31+7) & 255) for i in range(1200))");
        judge("the wrapping did not merge the two blobs", &p,
              "len(p[\"all\"]) == 256 and len(p[\"big\"]) == 1200");
    }

    head("a 256-byte blob is a 2048-bit modulus");
    {
        uint8_t mod[256];
        int i;
        for(i = 0; i < 256; i++) mod[i] = (uint8_t)(255 - i);
        mod[0] = 0xC0;                   /* a real modulus' top bit is set */
        plist_init(&p, g_buf, PBUF);
        plist_begin(&p);
        plist_dict_begin(&p);
        plist_kv_data(&p, "DevicePublicKey", mod, 256);
        plist_dict_end(&p);
        plist_end(&p);
        judge("the high byte is not lost", &p,
              "p[\"DevicePublicKey\"][0] == 0xC0");
        judge("the low byte is not lost", &p,
              "p[\"DevicePublicKey\"][255] == 0");
        judge("all 256 bytes, in order", &p,
              "len(p[\"DevicePublicKey\"]) == 256 and "
              "all(p[\"DevicePublicKey\"][i] == (255-i) for i in range(1,256))");
    }

    /* ── structure ─────────────────────────────────────────────── */
    head("nesting");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p);
    plist_dict_begin(&p);
      plist_key(&p, "outer");
      plist_dict_begin(&p);
        plist_kv_string(&p, "inner", "yes");
        plist_key(&p, "deeper");
        plist_dict_begin(&p);
          plist_kv_integer(&p, "n", 3);
        plist_dict_end(&p);
      plist_dict_end(&p);
      plist_kv_string(&p, "after", "still here");
    plist_dict_end(&p);
    plist_end(&p);
    judge("three levels deep", &p, "p[\"outer\"][\"deeper\"][\"n\"] == 3");
    judge("the middle level kept its own key", &p,
          "p[\"outer\"][\"inner\"] == \"yes\"");
    judge("the sibling after a nested dict is not swallowed", &p,
          "p[\"after\"] == \"still here\"");

    head("arrays");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p);
    plist_dict_begin(&p);
      plist_key(&p, "list");
      plist_array_begin(&p);
        plist_string(&p, "a");
        plist_string(&p, "b");
        plist_integer(&p, 7);
        plist_bool(&p, 0);
      plist_array_end(&p);
      plist_key(&p, "empty");
      plist_array_begin(&p);
      plist_array_end(&p);
      plist_key(&p, "dicts");
      plist_array_begin(&p);
        plist_dict_begin(&p);
          plist_kv_string(&p, "k", "v1");
        plist_dict_end(&p);
        plist_dict_begin(&p);
          plist_kv_string(&p, "k", "v2");
        plist_dict_end(&p);
      plist_array_end(&p);
    plist_dict_end(&p);
    plist_end(&p);
    judge("a mixed array, in order", &p,
          "p[\"list\"] == [\"a\", \"b\", 7, False]");
    judge("an empty array is [] and not absent", &p,
          "p[\"empty\"] == [] and type(p[\"empty\"]) is list");
    judge("an array of dicts", &p,
          "[d[\"k\"] for d in p[\"dicts\"]] == [\"v1\", \"v2\"]");

    head("an array at the top level (a plist need not be a dict)");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p);
    plist_array_begin(&p);
      plist_string(&p, "one");
      plist_integer(&p, 2);
    plist_array_end(&p);
    plist_end(&p);
    judge("it parses as a list", &p, "p == [\"one\", 2]");

    /* ── the thing this is all for ──────────────────────────────
       The stage-3 pair request. Three certificates, a public key and
       two UUIDs: the message ld_send_plist() cannot send today
       because it is several KB, and the message a format string
       cannot build. */
    head("a stage-3 Pair request, full size");
    {
        uint8_t rootcert[1180], hostcert[1120], devpub[270];
        int i;
        for(i = 0; i < 1180; i++) rootcert[i] = (uint8_t)((i * 7 + 1) & 0xFF);
        for(i = 0; i < 1120; i++) hostcert[i] = (uint8_t)((i * 11 + 3) & 0xFF);
        for(i = 0; i < 270;  i++) devpub[i]   = (uint8_t)((i * 13 + 5) & 0xFF);

        plist_init(&p, g_buf, PBUF);
        plist_begin(&p);
        plist_dict_begin(&p);
          plist_kv_string(&p, "Label",   "HavenDOS");
          plist_kv_string(&p, "Request", "Pair");
          plist_key(&p, "PairRecord");
          plist_dict_begin(&p);
            plist_kv_data(&p, "DeviceCertificate",     devpub,   270);
            plist_kv_data(&p, "HostCertificate",       hostcert, 1120);
            plist_kv_data(&p, "RootCertificate",       rootcert, 1180);
            plist_kv_string(&p, "HostID",
                            "C1A2B3C4-D5E6-47F8-9A0B-1C2D3E4F5A6B");
            plist_kv_string(&p, "SystemBUID",
                            "7F8E9D0C-1B2A-4958-8674-5F4E3D2C1B0A");
            plist_kv_data(&p, "HostPrivateKey", hostcert, 16);
          plist_dict_end(&p);
          plist_key(&p, "PairingOptions");
          plist_dict_begin(&p);
            plist_kv_bool(&p, "ExtendedPairingErrors", 1);
          plist_dict_end(&p);
          plist_kv_string(&p, "ProtocolVersion", "2");
        plist_dict_end(&p);
        plist_end(&p);

        ck("the document is multi-kilobyte (which is the point)",
           plist_ok(&p) && p.len > 4000);
        judge("it parses",            &p, "p[\"Request\"] == \"Pair\"");
        judge("the record is nested", &p, "type(p[\"PairRecord\"]) is dict");
        judge("the root certificate is byte-exact", &p,
              "len(p[\"PairRecord\"][\"RootCertificate\"]) == 1180 and "
              "all(p[\"PairRecord\"][\"RootCertificate\"][i] == ((i*7+1)&255) "
              "for i in range(1180))");
        judge("the host certificate is byte-exact", &p,
              "len(p[\"PairRecord\"][\"HostCertificate\"]) == 1120 and "
              "all(p[\"PairRecord\"][\"HostCertificate\"][i] == ((i*11+3)&255) "
              "for i in range(1120))");
        judge("the device key is byte-exact", &p,
              "len(p[\"PairRecord\"][\"DeviceCertificate\"]) == 270 and "
              "all(p[\"PairRecord\"][\"DeviceCertificate\"][i] == ((i*13+5)&255) "
              "for i in range(270))");
        judge("three separate blobs, none truncated or merged", &p,
              "len(set([len(p[\"PairRecord\"][k]) for k in "
              "[\"RootCertificate\",\"HostCertificate\",\"DeviceCertificate\"]])) == 3");
        judge("the UUIDs survive intact", &p,
              "p[\"PairRecord\"][\"HostID\"] == "
              "\"C1A2B3C4-D5E6-47F8-9A0B-1C2D3E4F5A6B\"");
        judge("a bool beside the blobs", &p,
              "p[\"PairingOptions\"][\"ExtendedPairingErrors\"] is True");
        judge("every key is accounted for", &p,
              "sorted(p) == [\"Label\",\"PairRecord\",\"PairingOptions\",\"ProtocolVersion\",\"Request\"]");
        judge("the record's keys too", &p,
              "len(p[\"PairRecord\"]) == 6");
    }

    /* ── refusals ───────────────────────────────────────────────
       Each of these produces text that would look plausible in a
       terminal. The writer has to refuse them, because the
       alternative is sending them. */
    head("documents the writer must refuse");

    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_key(&p, "a"); plist_key(&p, "b");
    refuses("two keys in a row", &p);

    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_string(&p, "orphan");
    refuses("a value in a dict with no key", &p);

    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_kv_string(&p, "k", "v");
    plist_array_end(&p);
    refuses("closing a dict as an array", &p);

    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_array_begin(&p);
    plist_string(&p, "x");
    plist_dict_end(&p);
    refuses("closing an array as a dict", &p);

    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_key(&p, "dangling");
    plist_dict_end(&p);
    refuses("closing a dict with a key and no value", &p);

    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_kv_string(&p, "k", "v");
    plist_end(&p);
    refuses("ending the document with a dict still open", &p);

    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_kv_string(&p, "k", "v");
    plist_dict_end(&p); plist_dict_end(&p);
    refuses("closing one container too many", &p);

    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_array_begin(&p);
    plist_key(&p, "no keys in arrays");
    refuses("a key inside an array", &p);

    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_kv_string(&p, "ctrl", "a\x01b");
    refuses("a control character XML cannot represent", &p);

    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_key(&p, "k\x1F" "ey"); plist_string(&p, "v");
    refuses("a control character in a KEY", &p);

    head("a control character XML CAN represent is allowed through");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_kv_string(&p, "tabbed", "a\tb");
    plist_kv_string(&p, "lines",  "a\nb");
    plist_dict_end(&p); plist_end(&p);
    judge("tab survives", &p, "p[\"tabbed\"] == \"a\\tb\"");
    judge("a newline inside a string survives", &p, "p[\"lines\"] == \"a\\nb\"");

    head("too deep");
    {
        int i;
        plist_init(&p, g_buf, PBUF);
        plist_begin(&p);
        for(i = 0; i < PLIST_MAX_DEPTH + 2; i++){
            plist_array_begin(&p);
        }
        refuses("nesting past PLIST_MAX_DEPTH", &p);
    }
    {
        int i;
        plist_init(&p, g_buf, PBUF);
        plist_begin(&p);
        for(i = 0; i < PLIST_MAX_DEPTH; i++) plist_array_begin(&p);
        plist_string(&p, "bottom");
        for(i = 0; i < PLIST_MAX_DEPTH; i++) plist_array_end(&p);
        plist_end(&p);
        ck("exactly PLIST_MAX_DEPTH levels is allowed", plist_ok(&p));
        judge("and parses", &p, "len(p) == 1");
    }

    /* ── the buffer ────────────────────────────────────────────── */
    head("a buffer that is too small");
    canary_init(100, &p);
    plist_begin(&p);                 /* the preamble alone is ~130 bytes */
    plist_dict_begin(&p);
    plist_kv_string(&p, "k", "v");
    plist_dict_end(&p);
    plist_end(&p);
    ck("it refuses rather than truncating", !plist_ok(&p));
    ck("and wrote nothing past the capacity it was given", canary_clean(100));

    canary_init(1, &p);
    plist_begin(&p); plist_end(&p);
    ck("a one-byte buffer refuses", !plist_ok(&p));
    ck("a one-byte buffer is not overrun", canary_clean(1));

    canary_init(0, &p);
    plist_begin(&p);
    plist_dict_begin(&p);
    plist_kv_data(&p, "k", (const uint8_t *)"abc", 3);
    plist_dict_end(&p);
    plist_end(&p);
    ck("a zero-capacity buffer refuses", !plist_ok(&p));
    ck("a zero-capacity buffer is never written to", canary_clean(0));

    plist_init(&p, 0, 0);
    plist_begin(&p);
    ck("a NULL buffer refuses instead of faulting", !plist_ok(&p));

    head("exact fit: cap == len, with no room for the terminator");
    {
        uint32_t need;
        plist_init(&p, g_buf, PBUF);
        plist_begin(&p); plist_dict_begin(&p);
        plist_kv_string(&p, "k", "v");
        plist_dict_end(&p); plist_end(&p);
        ck("the reference document is fine", plist_ok(&p));
        need = plist_len(&p);

        /* Exactly the bytes, no room for the NUL: must refuse, because
           a caller that passes buf to strlen() would run off the end. */
        canary_init(need, &p);
        plist_begin(&p); plist_dict_begin(&p);
        plist_kv_string(&p, "k", "v");
        plist_dict_end(&p); plist_end(&p);
        ck("cap == len refuses (no room to terminate)", !plist_ok(&p));
        ck("and does not write the terminator past the end",
           canary_clean(need));

        canary_init(need + 1, &p);
        plist_begin(&p); plist_dict_begin(&p);
        plist_kv_string(&p, "k", "v");
        plist_dict_end(&p); plist_end(&p);
        ck("cap == len+1 succeeds", plist_ok(&p));
        ck("one byte of headroom is all it takes", canary_clean(need + 1));
        ck("and the terminator is in it", g_canary[need] == 0);
    }

    head("the flag never recovers");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_key(&p, "a"); plist_key(&p, "b");       /* breaks it here */
    plist_string(&p, "v");
    plist_dict_end(&p); plist_end(&p);
    ck("a document broken in the middle stays broken to the end",
       !plist_ok(&p));
    {
        uint32_t after = plist_len(&p);
        plist_string(&p, "more");
        ck("and nothing further is appended to it", plist_len(&p) == after);
    }

    /* ── the preamble, which lockdownd is picky about ───────────── */
    head("the preamble");
    plist_init(&p, g_buf, PBUF);
    plist_begin(&p); plist_dict_begin(&p);
    plist_kv_string(&p, "k", "v");
    plist_dict_end(&p); plist_end(&p);
    ck("starts with the XML declaration",
       strncmp(p.buf, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>", 38) == 0);
    ck("carries Apple's DOCTYPE",
       strstr(p.buf, "-//Apple//DTD PLIST 1.0//EN") != 0);
    ck("declares plist version 1.0",
       strstr(p.buf, "<plist version=\"1.0\">") != 0);
    ck("closes the plist element", strstr(p.buf, "</plist>") != 0);
    ck("no stray NULs in the middle", strlen(p.buf) == p.len);

    /* run_suite.py reads exactly this line and nothing else. */
    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
