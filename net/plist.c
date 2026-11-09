/* ================================================================
   net/plist.c -- see include/plist.h.
   ================================================================ */
#include "plist.h"
#include "b64.h"
#include "string.h"

#define IND_MAX 8

/* ── pl_ ON THE HELPERS, AND WHY ──────────────────────────────────────
   These were `raw`, `put`, `indent`, `text` and `pre_value`.
   net/der.c has a static `put` of its own, and a suite that includes
   both files in one translation unit -- tests/test_pair.c does,
   because a pair request is DER inside base64 inside a plist -- does
   not compile: two statics with the same name and different
   signatures.

   tools/check_statics.py exists for the data version of this problem
   and the lesson is the same one: a short generic name on a file
   static is fine until two files that each own one are read together.
   The prefix is three characters and the problem does not come back. */
static void pl_raw(plist_t *p, const char *s, uint32_t n)
{
    if(!p->ok) return;
    if(p->len + n > p->cap){ p->ok = 0; return; }
    memcpy(p->buf + p->len, s, n);
    p->len += n;
}
static void pl_put(plist_t *p, const char *s){ pl_raw(p, s, (uint32_t)strlen(s)); }

/* Apple's writer indents with tabs, one per level. Matching it is not
   required by any parser and is done anyway: the first thing anybody
   does when lockdownd rejects a message is diff it against one that
   worked. */
static void pl_indent(plist_t *p)
{
    int i, n = p->depth + 1;
    if(n > IND_MAX) n = IND_MAX;
    for(i = 0; i < n; i++) pl_put(p, "\t");
}

/* XML text. &, < and > MUST be escaped; " and ' need it only inside
   attributes and no value here is one. A control character below
   0x20 that is not tab, LF or CR cannot be represented in XML 1.0 at
   all -- not as an entity either -- so it FAILS the document rather
   than being dropped, because a silently shortened HostID is a
   pairing that never works and never says why. */
static void pl_text(plist_t *p, const char *s)
{
    if(!p->ok) return;
    for(; *s; s++){
        unsigned char c = (unsigned char)*s;
        if(c < 0x20 && c != '\t' && c != '\n' && c != '\r'){ p->ok = 0; return; }
        switch(c){
        case '&': pl_put(p, "&amp;");  break;
        case '<': pl_put(p, "&lt;");   break;
        case '>': pl_put(p, "&gt;");   break;
        default:  pl_raw(p, (const char *)&c, 1); break;
        }
    }
}

void plist_init(plist_t *p, char *buf, uint32_t cap)
{
    memset(p, 0, sizeof *p);
    p->buf = buf; p->cap = cap; p->ok = (buf && cap) ? 1 : 0;
    p->depth = -1;
}
int      plist_ok(const plist_t *p){ return p && p->ok; }
uint32_t plist_len(const plist_t *p){ return p ? p->len : 0; }

void plist_begin(plist_t *p)
{
    pl_put(p, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n");
}

void plist_end(plist_t *p)
{
    /* Every container must be closed. depth is -1 when the document
       holds exactly one complete value, which is what a plist is. */
    if(p->depth != -1){ p->ok = 0; return; }
    pl_put(p, "</plist>\n");
    if(p->ok && p->len < p->cap) p->buf[p->len] = 0;
    else p->ok = 0;                 /* no room for the terminator      */
}

/* Before any value: the indent, and the dict bookkeeping. */
static void pl_pre_value(plist_t *p)
{
    if(!p->ok) return;
    if(p->depth >= 0 && p->kind[p->depth] == 'd'){
        if(p->want_key){ p->ok = 0; return; }   /* a value where a key goes */
        p->want_key = 1;
    }
    pl_indent(p);
}

static void open_container(plist_t *p, uint8_t kind, const char *tag)
{
    if(!p->ok) return;
    if(p->depth + 1 >= PLIST_MAX_DEPTH){ p->ok = 0; return; }
    pl_pre_value(p);
    pl_put(p, tag);
    pl_put(p, "\n");
    p->depth++;
    p->kind[p->depth] = kind;
    if(kind == 'd') p->want_key = 1;
}

static void close_container(plist_t *p, uint8_t kind, const char *tag)
{
    if(!p->ok) return;
    if(p->depth < 0 || p->kind[p->depth] != kind){ p->ok = 0; return; }
    /* A dict that is waiting for a value cannot be closed: the last
       thing written was a <key> with nothing after it. */
    if(kind == 'd' && !p->want_key){ p->ok = 0; return; }
    p->depth--;
    pl_indent(p);
    pl_put(p, tag);
    pl_put(p, "\n");
    if(p->depth >= 0 && p->kind[p->depth] == 'd') p->want_key = 1;
}

void plist_dict_begin (plist_t *p){ open_container(p, 'd', "<dict>");  }
void plist_dict_end   (plist_t *p){ close_container(p, 'd', "</dict>"); }
void plist_array_begin(plist_t *p){ open_container(p, 'a', "<array>");  }
void plist_array_end  (plist_t *p){ close_container(p, 'a', "</array>"); }

void plist_key(plist_t *p, const char *k)
{
    if(!p->ok) return;
    if(p->depth < 0 || p->kind[p->depth] != 'd'){ p->ok = 0; return; }
    if(!p->want_key){ p->ok = 0; return; }      /* two keys in a row   */
    pl_indent(p);
    pl_put(p, "<key>"); pl_text(p, k); pl_put(p, "</key>\n");
    p->want_key = 0;
}

void plist_string(plist_t *p, const char *s)
{
    pl_pre_value(p);
    pl_put(p, "<string>"); pl_text(p, s); pl_put(p, "</string>\n");
}

void plist_integer(plist_t *p, int32_t v)
{
    char b[16];
    int n = 0, i;
    uint32_t u;
    pl_pre_value(p);
    pl_put(p, "<integer>");
    if(v < 0){ pl_put(p, "-"); u = (uint32_t)(-(int64_t)v); }
    else u = (uint32_t)v;
    if(!u) b[n++] = '0';
    while(u){ b[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; }
    for(i = n - 1; i >= 0; i--) pl_raw(p, &b[i], 1);
    pl_put(p, "</integer>\n");
}

void plist_bool(plist_t *p, int v)
{
    pl_pre_value(p);
    pl_put(p, v ? "<true/>\n" : "<false/>\n");
}

/* base64, wrapped and indented the way Apple's writer does it.

   The ALPHABET and the quad arithmetic are net/b64.c's -- v0.8.8.11
   consolidated the tree's four base64 implementations into one, and
   the comment that used to sit here explaining why there was a second
   encoder is gone with the second encoder. What stays here is the
   presentation: the indent before each line, the 68-character wrap,
   and the <data> element around it, none of which belong in a codec.

   One quad at a time rather than one call for the whole blob, because
   a certificate is 1600 characters and this writer streams into a
   caller's buffer with no scratch space of its own. */
void plist_data(plist_t *p, const uint8_t *d, uint32_t n)
{
    uint32_t i, col = 0;
    pl_pre_value(p);
    pl_put(p, "<data>\n");
    for(i = 0; i < n; i += 3){
        uint32_t rem = n - i;
        char q[4];
        if(b64_encode(d + i, (int)(rem > 3 ? 3 : rem), q, 4, 0) != 4){
            p->ok = 0; return;
        }
        if(col == 0) pl_indent(p);
        pl_raw(p, q, 4);
        col += 4;
        /* 68 characters a line, which is what Apple's writer emits.
           Any parser takes any wrapping; matching it keeps a diff
           against a known-good message readable. */
        if(col >= 68){ pl_put(p, "\n"); col = 0; }
    }
    if(col) pl_put(p, "\n");
    pl_indent(p);
    pl_put(p, "</data>\n");
}

void plist_kv_string(plist_t *p, const char *k, const char *s)
{ plist_key(p, k); plist_string(p, s); }
void plist_kv_integer(plist_t *p, const char *k, int32_t v)
{ plist_key(p, k); plist_integer(p, v); }
void plist_kv_bool(plist_t *p, const char *k, int v)
{ plist_key(p, k); plist_bool(p, v); }
void plist_kv_data(plist_t *p, const char *k, const uint8_t *d, uint32_t n)
{ plist_key(p, k); plist_data(p, d, n); }

/* ================================================================
   WHAT THE SUITE WAS PROVED AGAINST
   ================================================================
   93 checks passed on the first run, which on its own is the least
   trustworthy result a suite can produce. So 23 defects were injected
   into this file one at a time, each with a prediction written down
   BEFORE it ran. Nineteen were caught. Four were not, and all four
   are recorded here rather than deleted, because an injection that
   turns out not to be a defect is a finding about the format and the
   next person should not have to rediscover it:

   1. DROPPING THE > ESCAPE SURVIVES, and should. A bare `>` is legal
      in XML character data -- it only has to be escaped inside the
      sequence `]]>`. The escape stays because Apple's own writer
      emits it and a diff against a known-good message is the first
      debugging tool, but no test can convict its absence and none
      pretends to.

   2. WRAPPING BASE64 MID-QUAD SURVIVES (67 characters a line instead
      of 68, splitting a four-character group). Every base64 decoder
      skips whitespace wherever it falls, so the line length is
      cosmetic. 68 is kept to match Apple; it is not correctness.

   3. NEGATING INT32_MIN IN 32 BITS SURVIVES: `(uint32_t)(-v)` prints
      the same digits as `(uint32_t)(-(int64_t)v)`. The int64_t cast
      is there because negating INT32_MIN in int is signed overflow --
      undefined behaviour that today's compiler happens to implement
      as the answer we want. This is the one survivor that is a real
      limit of testing rather than a property of the format: a suite
      cannot catch undefined behaviour that currently works. The cast
      stays, and this note is why it must not be "simplified".

   4. INDENTING WITH SPACES SURVIVES. Whitespace between elements is
      not semantic. Tabs match Apple; nothing more.

   The nineteen that were caught include every one that matters: both
   escapes that XML requires, two different corruptions of the base64
   table, a base64 loop stepping by 4, dropped padding, <true>
   unclosed, <data> written as <string>, reversed integer digits, a
   missing minus sign, and -- separately from the parse checks -- the
   removal of the capacity test in pl_raw(), caught not by a length
   assertion but by a 0xAA canary across the rest of the buffer.

   Of the four structural refusals (two keys in a row, a value with
   no key, closing the wrong container, ending with one open), each
   was caught by exactly ONE check. That is thin by design: there is
   one way to get each wrong and one test for it. If a fifth state
   ever gets added to this writer, it needs its own.
   ================================================================ */
