/* ================================================================
   include/plist.h -- writing Apple property lists
   ================================================================
   Pairing stage 3, piece two.  Every lockdownd message is an XML
   property list, and until now the tree has written them by hand as
   string constants:

       usb/lockdown.c, LD_QUERYTYPE -- one dict, two string values,
       spelled out in C source with the DOCTYPE and the tabs typed in.

   That works for a message whose contents never change. Stage 3's
   pair request does not qualify: it carries a HostID, a SystemBUID,
   a device public key and THREE certificates, each several hundred
   base64 characters that have to be wrapped and escaped. A format
   string cannot do that, and a message that is wrong by one
   character is a message lockdownd rejects without saying why.

   ── THE SHAPE IS der_t's, ON PURPOSE ────────────────────────────────

   A buffer, a length, and an `ok` flag that never recovers. Every
   call is void and checks nothing; the caller checks plist_ok() once
   at the end. The alternative -- a return code per call -- is twenty
   ifs in the caller, nineteen of which get written and one of which
   does not. net/der.c made the same choice for the same reason and
   its header says so.

   Nesting is counted, and a document whose dicts and arrays do not
   match is refused rather than emitted. An unbalanced plist is still
   well-formed-looking text, which is the worst kind of wrong.

   ── WHAT JUDGES IT ──────────────────────────────────────────────────

   Python's `plistlib`, which is a real, independent implementation of
   this format: tests/test_plist.py-equivalent checks parse the output
   back and compare it against the values that went in. Our own
   reader agreeing with our own writer would prove nothing -- and
   there IS no reader here, which is exactly why an outside one is
   the only honest judge.
   ================================================================ */
#ifndef PLIST_H
#define PLIST_H
#include "types.h"

#define PLIST_MAX_DEPTH 16

typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t len;
    int      ok;          /* 0 once anything has failed; never recovers */
    int      depth;
    uint8_t  kind[PLIST_MAX_DEPTH];   /* 'd' dict, 'a' array            */
    int      want_key;    /* inside a dict, is a <key> expected next    */
} plist_t;

void     plist_init(plist_t *p, char *buf, uint32_t cap);
int      plist_ok(const plist_t *p);
uint32_t plist_len(const plist_t *p);

/* The XML declaration, the DOCTYPE and <plist version="1.0">, then
   its close. Between them goes exactly one value. */
void plist_begin(plist_t *p);
void plist_end(plist_t *p);

void plist_dict_begin(plist_t *p);
void plist_dict_end(plist_t *p);
void plist_array_begin(plist_t *p);
void plist_array_end(plist_t *p);

/* Inside a dict, a key must precede every value. Writing two keys in
   a row, or a value where a key belongs, fails the document. */
void plist_key(plist_t *p, const char *k);

void plist_string(plist_t *p, const char *s);
void plist_integer(plist_t *p, int32_t v);
void plist_bool(plist_t *p, int v);
/* <data>, base64, wrapped the way Apple's own writer wraps it. */
void plist_data(plist_t *p, const uint8_t *d, uint32_t n);

/* key + value, which is what almost every call site wants. */
void plist_kv_string (plist_t *p, const char *k, const char *s);
void plist_kv_integer(plist_t *p, const char *k, int32_t v);
void plist_kv_bool   (plist_t *p, const char *k, int v);
void plist_kv_data   (plist_t *p, const char *k,
                      const uint8_t *d, uint32_t n);

#endif /* PLIST_H */
