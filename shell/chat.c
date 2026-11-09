/*
 * chat.c — BOOT Chat v2 client   (HavenDOS v0.7.17)
 *
 *   chat <user>@<host>[:<port>]      default port 6700
 *   chat                             reconnect to the last server
 *
 * v0.7.17: a real sidebar, and the mouse works.
 *
 * v0.7.16 shipped the v2 protocol but kept v1's single scrolling pane, so
 * servers, channels and DMs existed on the wire and were invisible on screen
 * — you navigated a tree you could not see by typing /join and hoping. The
 * web and desktop clients both had a sidebar; this one did not, and it was
 * the worst of the three to use.
 *
 * Layout now matches the other two clients:
 *
 *   +----------------+-------------------------------------+
 *   | BOOT Chat      | kyan  techhaven/#dev         ONLINE |
 *   | kyan           |-------------------------------------|
 *   | SERVERS        | messages                            |
 *   |  techhaven     |                                     |
 *   |   # general    |                                     |
 *   |   # dev      * |   <- * marks unread                 |
 *   | DIRECT         |                                     |
 *   |   @ bob        |                                     |
 *   +----------------+-------------------------------------+
 *   | > type here                                          |
 *   +------------------------------------------------------+
 *
 * NAVIGATION
 *   TAB          move focus between the sidebar and the message line
 *   UP/DOWN      move the sidebar selection (when the sidebar has focus)
 *   ENTER        open the selected channel or DM
 *   CLICK        the same, with the mouse, anywhere in the sidebar
 *   PGUP/PGDN    scroll the message pane;  END returns to the bottom
 *   ESC          leave
 *
 * The slash commands from v0.7.16 all still work — /create, /mkchan, /dm and
 * the rest — because creating things still needs a name typed in. The
 * sidebar covers getting around, which is what was actually painful.
 *
 * MOUSE
 * IRQ12 does not fire on UTM SE TCG, so the mouse is polled here exactly as
 * the desktop does: read the position every pass, take the click edge with
 * mouse_get_clicks(), and bracket each frame with ui_erase_cursor() /
 * ui_draw_cursor() so the pointer is never baked into the frame it sits on.
 *
 * Everything else is polled too — a short tcp_recv() timeout alternating
 * with a non-blocking keyboard read. No IRQ dependency anywhere.
 */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/net.h"
#include "../include/tcp.h"
#include "../include/dns.h"
#include "../include/toast.h"

extern int      keyboard_getchar(void);
extern void     sleep_ms(uint32_t ms);
extern int      kernel_poll(void);
extern void     net_poll(void);
extern uint32_t get_ticks(void);
extern int      net_ready(void);
extern void     notif_push(const char *msg);

/* mouse — same polling API the desktop uses */
extern void     mouse_get(int *x, int *y, int *btn);
extern int      mouse_is_ready(void);
extern int      mouse_get_clicks(void);
extern void     ui_draw_cursor(void);
extern void     ui_erase_cursor(void);
extern void     ui_force_cursor_redraw(void);

#define CHAT_PORT_DEFAULT 6700
#define CHAT_SCROLL       200
#define CHAT_LINE         96
#define CHAT_INPUT        160
/* v0.7.18 FIX — CHAT_LINE is the DISPLAY width of one scrollback row.  It was
   also being used to size the receive assembler, which silently threw away
   every byte past 95 in any server line.  A `MSG <srv> <chan> <user> <ts> `
   prefix costs ~40 of those, so messages were cut to roughly 55 characters,
   and a CHANLIST/DMLIST/ONLINE line with several entries lost the tail —
   channels simply vanished from the sidebar.  The wire needs its own size. */
#define CHAT_WIRE         1024
#define NAME_MAX          24
#define SIDE_W            18      /* sidebar columns, including its border */
#define REACT_SUMMARY     40      /* rendered reaction summary per message */
#define MEDIA_PER_MSG_MAX 4       /* server allows this many; match it */
#define EMAIL_MAX         64

/* v0.7.19 — protocol level this connection negotiated.  HavenDOS was a 2.0
   client: it never sent PROTO at all, so it could not see message ids, edits,
   deletions, replies, reactions, typing, read state or presence, and it could
   not sign in with an email address.  It now asks for 2.2 and falls back to
   whatever the server grants, so an older server still works. */
#define PROTO_WANT_MAJ    2
#define PROTO_WANT_MIN    3

/* sidebar model */
#define MAX_SRV      8
#define MAX_CHAN    12
#define MAX_DM      12
#define MAX_ITEMS   64

#define SB_HDR   0
#define SB_SRV   1
#define SB_CHAN  2
#define SB_DM    3

typedef struct {
    uint8_t kind;
    uint8_t unread;
    char    a[NAME_MAX];     /* server name, or DM peer */
    char    b[NAME_MAX];     /* channel name (SB_CHAN only) */
} sbitem_t;

typedef struct {
    char    text[CHAT_LINE];
    uint8_t fg;
    /* v0.7.19 (proto 2.1+): the server's id for this message, "c41" or "d17".
       Empty for a local notice.  Kept per row so MSGEDIT, MSGDEL and REACT can
       find the row they refer to without a second index. */
    char    mid[12];
    uint8_t edited;
    char    react[REACT_SUMMARY];   /* rendered "[+1 x2]" summary, or empty */
} chat_line_t;

static chat_line_t g_scroll[CHAT_SCROLL];
static int  g_count, g_head, g_view;

static char g_last_target[80];
static char g_nick[NAME_MAX];
static char g_server[NAME_MAX];
static char g_channel[NAME_MAX];
static char g_dmwith[NAME_MAX];
static int  g_authed;

/* ── v0.7.19: negotiated protocol and the 2.2 surface ─────────────── */
static int  g_pmaj = 2, g_pmin = 0;     /* what the server actually granted */
static char g_selfname[NAME_MAX];       /* stored spelling from OK LOGIN */
static int  g_needemail;                /* server says an address is owed */
static char g_typing[NAME_MAX];         /* who is typing in the current view */
static uint32_t g_typing_tick;          /* when that arrived, in 10ms ticks */
static char g_lastmid[12];              /* newest id seen in the current view */
static uint32_t g_typing_sent;          /* rate-limit our own TYPING */
static int  g_show_ids;                 /* /ids — print the wire id per row */

#define proto_at_least(maj,min) \
    (g_pmaj > (maj) || (g_pmaj == (maj) && g_pmin >= (min)))

static char g_srv[MAX_SRV][NAME_MAX];            static int g_nsrv;
static char g_chan[MAX_SRV][MAX_CHAN][NAME_MAX]; static int g_nchan[MAX_SRV];
static char g_dm[MAX_DM][NAME_MAX];              static int g_ndm;

static sbitem_t g_items[MAX_ITEMS];
static int g_nitems, g_sel, g_sbtop;
static int g_focus_side;          /* 0 = message line, 1 = sidebar */

/* ══ v0.7.15: emoji ═══════════════════════════════════════════════════════
 *
 * The wire is UTF-8 now, so a phone can send 😀 and a HavenDOS client will
 * receive four bytes for it.  VGA text mode has one byte per cell and 256
 * glyphs (code page 437), so real emoji cannot be drawn — but CP437 already
 * ships ☺ ☻ ♥ ♦ ♣ ♠ ♪ ♫ ☼ and the arrows, which covers the common ones
 * surprisingly well and looks right for a DOS-style OS.
 *
 * Incoming UTF-8 is decoded to codepoints and mapped to those glyphs.
 * Accented Latin-1 letters fold to their base ASCII letter (é -> e) rather
 * than to CP437's high range, because the 8x8 font used by the wide text
 * modes has glyphs 128-255 blank — folding renders everywhere, CP437 would
 * render only in 80x25.
 *
 * Anything with no sensible glyph becomes '?'.  That is honest: it says
 * "something was here that I cannot draw" instead of silently dropping it or
 * emitting four bytes of garbage.
 *
 * Outgoing, a HavenDOS user types :shortcodes: — the client sends the real
 * UTF-8 emoji so phones see 😀, and shows the CP437 glyph locally.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Latin-1 / Latin-Ext -> CP437.  These glyphs exist in the BIOS font and, as
   of v0.7.15, in the 8x8 font too (see tools/make_font_ext.py), so accented
   text renders the same in every text mode. */
typedef struct { uint32_t cp; uint8_t g; } cp437_map_t;
static const cp437_map_t g_cp437[] = {
    {0x00C7,0x80},{0x00FC,0x81},{0x00E9,0x82},{0x00E2,0x83},{0x00E4,0x84},
    {0x00E0,0x85},{0x00E5,0x86},{0x00E7,0x87},{0x00EA,0x88},{0x00EB,0x89},
    {0x00E8,0x8A},{0x00EF,0x8B},{0x00EE,0x8C},{0x00EC,0x8D},{0x00C4,0x8E},
    {0x00C5,0x8F},{0x00C9,0x90},{0x00F4,0x93},{0x00F6,0x94},{0x00F2,0x95},
    {0x00FB,0x96},{0x00F9,0x97},{0x00FF,0x98},{0x00D6,0x99},{0x00DC,0x9A},
    {0x00E1,0xA0},{0x00ED,0xA1},{0x00F3,0xA2},{0x00FA,0xA3},{0x00F1,0xA4},
    {0x00D1,0xA5},{0x00DF,0xE1},
};
#define CP437_N ((int)(sizeof(g_cp437)/sizeof(g_cp437[0])))

/* Emoji have no glyph and never will — one byte per cell, 256 of them.  They
   are spelled out instead: a name reads correctly in every mode, survives
   being copied into a file, and never leaves the user guessing what the
   square was.  Unnamed emoji become [emoji] rather than a wrong picture. */
typedef struct { uint32_t cp; const char *name; } emoji_name_t;
static const emoji_name_t g_emoji[] = {
    {0x1F600,"[grin]"},    {0x1F603,"[smile]"},   {0x1F604,"[smile]"},
    {0x1F601,"[grin]"},    {0x1F642,"[smile]"},   {0x1F60A,"[happy]"},
    {0x1F609,"[wink]"},    {0x263A,"[smile]"},    {0x1F602,"[laugh]"},
    {0x1F923,"[laugh]"},   {0x1F605,"[sweat]"},   {0x1F60D,"[love]"},
    {0x1F618,"[kiss]"},    {0x1F61B,"[tongue]"},  {0x1F60E,"[cool]"},
    {0x1F914,"[thinking]"},{0x1F644,"[eyeroll]"}, {0x1F62D,"[crying]"},
    {0x1F622,"[sad]"},     {0x1F61E,"[sad]"},     {0x1F620,"[angry]"},
    {0x1F621,"[angry]"},   {0x1F631,"[shock]"},   {0x1F62E,"[surprised]"},
    {0x1F634,"[sleepy]"},  {0x1F480,"[skull]"},   {0x1F608,"[devil]"},
    {0x1F921,"[clown]"},   {0x1F916,"[robot]"},   {0x1F47B,"[ghost]"},
    /* hearts + symbols */
    {0x2764,"[heart]"},    {0x2665,"[heart]"},    {0x1F496,"[heart]"},
    {0x1F495,"[hearts]"},  {0x1F497,"[heart]"},   {0x1F494,"[broken heart]"},
    {0x2666,"[diamond]"},  {0x2663,"[club]"},     {0x2660,"[spade]"},
    {0x2B50,"[star]"},     {0x1F31F,"[star]"},    {0x2728,"[sparkles]"},
    {0x1F525,"[fire]"},    {0x1F4A5,"[boom]"},    {0x26A1,"[lightning]"},
    {0x2705,"[check]"},    {0x274C,"[cross]"},    {0x2757,"[!]"},
    {0x2753,"[?]"},        {0x1F4AF,"[100]"},
    /* hands */
    {0x1F44D,"[thumbs up]"},  {0x1F44E,"[thumbs down]"},
    {0x1F44F,"[clap]"},       {0x1F64C,"[praise]"},
    {0x1F64F,"[please]"},     {0x270C,"[peace]"},
    {0x1F44B,"[wave]"},       {0x1F91D,"[handshake]"},
    {0x1F4AA,"[strong]"},
    /* things */
    {0x1F3B5,"[music]"},   {0x266A,"[music]"},    {0x1F3B6,"[music]"},
    {0x266B,"[music]"},    {0x2600,"[sun]"},      {0x1F31E,"[sun]"},
    {0x1F319,"[moon]"},    {0x2601,"[cloud]"},    {0x2614,"[rain]"},
    {0x2744,"[snow]"},     {0x1F355,"[pizza]"},   {0x1F354,"[burger]"},
    {0x2615,"[coffee]"},   {0x1F37A,"[beer]"},    {0x1F382,"[cake]"},
    {0x1F389,"[party]"},   {0x1F381,"[gift]"},    {0x1F4BB,"[laptop]"},
    {0x1F4BE,"[floppy]"},  {0x1F4F1,"[phone]"},   {0x1F527,"[wrench]"},
    {0x1F41B,"[bug]"},     {0x1F680,"[rocket]"},  {0x1F697,"[car]"},
    {0x26BD,"[ball]"},     {0x1F3AE,"[game]"},    {0x1F4D6,"[book]"},
    /* arrows */
    {0x2B06,"[up]"},       {0x2191,"[up]"},       {0x2B07,"[down]"},
    {0x2193,"[down]"},     {0x27A1,"[right]"},    {0x2192,"[right]"},
    {0x2B05,"[left]"},     {0x2190,"[left]"},
};
#define EMOJI_N ((int)(sizeof(g_emoji)/sizeof(g_emoji[0])))

/* :shortcode: -> the UTF-8 a phone should see. Ordered LONGEST FIRST: the
   scanner takes the first match at a given ':', so a short code that prefixes
   a longer one would shadow it. */
typedef struct { const char *code; const char *utf8; } shortcode_t;
static const shortcode_t g_shortcodes[] = {
    {":thumbsup:","\xF0\x9F\x91\x8D"},
    {":smile:",   "\xF0\x9F\x98\x80"},
    {":heart:",   "\xE2\x9D\xA4"},
    {":music:",   "\xF0\x9F\x8E\xB5"},
    {":party:",   "\xF0\x9F\x8E\x89"},
    {":star:",    "\xE2\xAD\x90"},
    {":fire:",    "\xF0\x9F\x94\xA5"},
    {":cool:",    "\xF0\x9F\x98\x8E"},
    {":thumb:",   "\xF0\x9F\x91\x8D"},
    {":down:",    "\xF0\x9F\x91\x8E"},
    {":wave:",    "\xF0\x9F\x91\x8B"},
    {":cry:",     "\xF0\x9F\x98\xAD"},
    {":<3:",      "\xE2\x9D\xA4"},
    {":+1:",      "\xF0\x9F\x91\x8D"},
    {":-1:",      "\xF0\x9F\x91\x8E"},
    {":)",        "\xF0\x9F\x99\x82"},
    {":(",        "\xF0\x9F\x98\x9E"},
};
#define SHORTCODE_N ((int)(sizeof(g_shortcodes)/sizeof(g_shortcodes[0])))

/* Fallback for accented letters with no CP437 glyph (Ê, Ï, Ô, Á ...). */
static char fold_latin1(uint32_t cp)
{
    if(cp>=0xC0&&cp<=0xC5) return 'A';
    if(cp==0xC7)           return 'C';
    if(cp>=0xC8&&cp<=0xCB) return 'E';
    if(cp>=0xCC&&cp<=0xCF) return 'I';
    if(cp==0xD1)           return 'N';
    if(cp>=0xD2&&cp<=0xD6) return 'O';
    if(cp>=0xD9&&cp<=0xDC) return 'U';
    if(cp==0xDD)           return 'Y';
    if(cp>=0xE0&&cp<=0xE5) return 'a';
    if(cp==0xE7)           return 'c';
    if(cp>=0xE8&&cp<=0xEB) return 'e';
    if(cp>=0xEC&&cp<=0xEF) return 'i';
    if(cp==0xF1)           return 'n';
    if(cp>=0xF2&&cp<=0xF6) return 'o';
    if(cp>=0xF9&&cp<=0xFC) return 'u';
    if(cp==0xFD||cp==0xFF) return 'y';
    return 0;
}

/* Decode one UTF-8 sequence. Returns bytes consumed, 0 if malformed. */
static int utf8_next(const char *s, uint32_t *cp)
{
    const uint8_t *u = (const uint8_t*)s;
    if(u[0] < 0x80){ *cp = u[0]; return 1; }
    if((u[0]&0xE0)==0xC0 && (u[1]&0xC0)==0x80){
        *cp = ((uint32_t)(u[0]&0x1F)<<6) | (u[1]&0x3F); return 2; }
    if((u[0]&0xF0)==0xE0 && (u[1]&0xC0)==0x80 && (u[2]&0xC0)==0x80){
        *cp = ((uint32_t)(u[0]&0x0F)<<12) | ((uint32_t)(u[1]&0x3F)<<6) | (u[2]&0x3F);
        return 3; }
    if((u[0]&0xF8)==0xF0 && (u[1]&0xC0)==0x80 && (u[2]&0xC0)==0x80 && (u[3]&0xC0)==0x80){
        *cp = ((uint32_t)(u[0]&0x07)<<18) | ((uint32_t)(u[1]&0x3F)<<12) |
              ((uint32_t)(u[2]&0x3F)<<6)  | (u[3]&0x3F);
        return 4; }
    return 0;
}

/* Rewrite a UTF-8 line in place as something the text mode can draw. */
static void utf8_to_cp437(char *buf, int bufsz)
{
    /* Spelling an emoji out makes the line LONGER than its UTF-8 source
       ("[thumbs up]" from four bytes), so the scratch buffer carries 3x
       headroom.  static, not stack: this is sized for the wire now and
       chat.c is single-threaded. */
    static char out[CHAT_WIRE*3];
    int o = 0, i = 0;
    while(buf[i] && o < (int)sizeof(out)-1){
        uint32_t cp; int n = utf8_next(buf + i, &cp);
        if(n == 0){ i++; out[o++] = '?'; continue; }
        i += n;
        if(cp < 0x80){ out[o++] = (char)cp; continue; }

        /* accented letter with a real glyph */
        int hit = 0;
        for(int m = 0; m < CP437_N; m++)
            if(g_cp437[m].cp == cp){ out[o++] = (char)g_cp437[m].g; hit = 1; break; }
        if(hit) continue;

        /* accented letter without one — fold to the base letter */
        char f = fold_latin1(cp);
        if(f){ out[o++] = f; continue; }

        /* zero-width joiners and variation selectors carry no glyph of their
           own; drop them silently or one heart becomes "[heart]?" */
        if(cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D) continue;
        if(cp >= 0x1F3FB && cp <= 0x1F3FF) continue;   /* skin-tone modifiers */

        /* emoji: spell it out */
        const char *name = 0;
        for(int e = 0; e < EMOJI_N; e++)
            if(g_emoji[e].cp == cp){ name = g_emoji[e].name; break; }
        if(!name){
            int is_emoji = (cp >= 0x1F300 && cp <= 0x1FAFF) ||
                           (cp >= 0x2600  && cp <= 0x27BF);
            name = is_emoji ? "[emoji]" : 0;
        }
        if(name){
            for(int k = 0; name[k] && o < (int)sizeof(out)-1; k++) out[o++] = name[k];
        } else {
            out[o++] = '?';
        }
    }
    out[o] = 0;
    int k = 0;
    while(out[k] && k < bufsz-1){ buf[k] = out[k]; k++; }
    buf[k] = 0;
}

/* Expand :shortcodes: to UTF-8 on the way out. */
static void expand_shortcodes(const char *in, char *out, int outsz)
{
    int o = 0, i = 0;
    while(in[i] && o < outsz-1){
        int matched = 0;
        if(in[i] == ':'){
            for(int c = 0; c < SHORTCODE_N; c++){
                int L = (int)strlen(g_shortcodes[c].code);
                if(!strncmp(in + i, g_shortcodes[c].code, (size_t)L)){
                    const char *u = g_shortcodes[c].utf8;
                    for(int k = 0; u[k] && o < outsz-1; k++) out[o++] = u[k];
                    i += L; matched = 1; break;
                }
            }
        }
        if(!matched) out[o++] = in[i++];
    }
    out[o] = 0;
}

/* ── scrollback ──────────────────────────────────────────────────── */

static void sb_reset(void){ g_count = 0; g_head = 0; g_view = 0; }

static void sb_add(const char *s, uint8_t fg)
{
    chat_line_t *l = &g_scroll[g_head];
    int i = 0;
    while(s[i] && i < CHAT_LINE-1){ l->text[i] = s[i]; i++; }
    l->text[i] = 0;
    l->fg = fg;
    /* The ring reuses rows.  Without clearing these, a local notice would
       inherit the id of the message that occupied this slot 200 lines ago and
       a later MSGEDIT would rewrite the wrong row. */
    l->mid[0] = 0; l->edited = 0; l->react[0] = 0;
    g_head = (g_head + 1) % CHAT_SCROLL;
    if(g_count < CHAT_SCROLL) g_count++;
    /* Only auto-follow when already pinned to the bottom — otherwise a busy
       channel would yank the view away while the user is reading back. */
    if(g_view > 0 && g_view < CHAT_SCROLL) g_view++;
}

/* Wrap a long line across the message pane instead of truncating it. */
static void sb_add_wrapped(const char *s, uint8_t fg)
{
    int w = vga_cols() - SIDE_W - 2;
    if(w < 16) w = 16;
    if(w > CHAT_LINE-1) w = CHAT_LINE-1;
    int len = (int)strlen(s);
    if(len <= w){ sb_add(s, fg); return; }

    char part[CHAT_LINE];
    int pos = 0;
    while(pos < len){
        int n = len - pos; if(n > w) n = w;
        if(pos + n < len){                 /* break on the last space */
            int br = n;
            while(br > 8 && s[pos+br] != ' ') br--;
            if(br > 8) n = br;
        }
        for(int i=0;i<n;i++) part[i] = s[pos+i];
        part[n] = 0;
        sb_add(part, fg);
        pos += n;
        while(pos < len && s[pos] == ' ') pos++;
    }
}

/* ══ v0.7.19: protocol 2.1+/2.2 helpers ═══════════════════════════════════
 *
 * Everything here is deliberately small and allocation-free.  The 2.2 wire
 * carries some replies as a single-line JSON object; HavenDOS does not need a
 * JSON parser to use the parts it cares about, and shipping one into a
 * freestanding 32-bit kernel to read two fields would be the wrong trade.
 * Instead a couple of targeted scanners pull named scalars out, and the
 * administrative objects (PROFILE, ROLES, AUDIT, SESSIONLIST ...) are
 * acknowledged rather than dumped as raw JSON into the scrollback.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Attach the server's message id to the row sb_add just wrote. */
static void sb_tag_last(const char *mid, int edited)
{
    int slot = (g_head - 1 + CHAT_SCROLL) % CHAT_SCROLL;
    chat_line_t *l = &g_scroll[slot];
    int i = 0;
    if(mid) while(mid[i] && i < (int)sizeof(l->mid)-1){ l->mid[i] = mid[i]; i++; }
    l->mid[i] = 0;
    l->edited = (uint8_t)(edited ? 1 : 0);
}

/* Newest row carrying this id, or -1.  Searches back from the head so an
   edited message resolves to its latest occurrence. */
static int sb_find_mid(const char *mid)
{
    if(!mid || !mid[0]) return -1;
    for(int n = 1; n <= g_count; n++){
        int slot = (g_head - n + CHAT_SCROLL*2) % CHAT_SCROLL;
        if(!strcmp(g_scroll[slot].mid, mid)) return slot;
    }
    return -1;
}

/* Rewrite a row's visible text, keeping its id and colour. */
static void sb_set_text(int slot, const char *s)
{
    chat_line_t *l = &g_scroll[slot];
    int i = 0;
    while(s[i] && i < CHAT_LINE-1){ l->text[i] = s[i]; i++; }
    l->text[i] = 0;
}

/* Read a quoted string value for `key` out of a one-line JSON object.
   Returns 1 on success.  Understands no escapes beyond \" because the server
   only ever puts names, emoji shortcodes and short status words through here. */
static int json_str(const char *src, const char *key, char *out, int outsz)
{
    char pat[32];
    int k = 0;
    pat[k++] = '"';
    for(int i = 0; key[i] && k < (int)sizeof(pat)-3; i++) pat[k++] = key[i];
    pat[k++] = '"'; pat[k++] = ':'; pat[k] = 0;

    const char *p = src;
    int plen = (int)strlen(pat);
    while(*p){
        if(!strncmp(p, pat, (size_t)plen)){
            p += plen;
            while(*p == ' ') p++;
            if(*p != '"') return 0;
            p++;
            int i = 0;
            while(*p && *p != '"' && i < outsz-1){
                if(*p == '\\' && p[1]) p++;
                out[i++] = *p++;
            }
            out[i] = 0;
            return 1;
        }
        p++;
    }
    return 0;
}

/* Read an integer value for `key`. Returns -1 when absent. */
static int json_int(const char *src, const char *key)
{
    char pat[32];
    int k = 0;
    pat[k++] = '"';
    for(int i = 0; key[i] && k < (int)sizeof(pat)-3; i++) pat[k++] = key[i];
    pat[k++] = '"'; pat[k++] = ':'; pat[k] = 0;

    const char *p = src;
    int plen = (int)strlen(pat);
    while(*p){
        if(!strncmp(p, pat, (size_t)plen)){
            p += plen;
            while(*p == ' ') p++;
            int v = 0, any = 0;
            while(*p >= '0' && *p <= '9'){ v = v*10 + (*p - '0'); p++; any = 1; }
            return any ? v : -1;
        }
        p++;
    }
    return -1;
}

/* Turn the REACT payload  [{"emoji":":+1:","n":2,"who":[...]}, ...]  into a
   compact summary the text pane can carry:  [:+1: x2] [:fire:]
   Counts of 1 are left bare; the x-count only appears when it adds something. */
static void react_summary(const char *json, char *out, int outsz)
{
    int o = 0;
    const char *p = json;
    out[0] = 0;
    while(*p && o < outsz - 12){
        /* each element begins at its own "emoji" key */
        const char *e = p;
        char emo[16];
        if(!json_str(e, "emoji", emo, sizeof(emo))) break;
        /* advance past this element's emoji so the next loop finds the next */
        const char *adv = e;
        while(*adv && strncmp(adv, "\"emoji\"", 7)) adv++;
        if(!*adv) break;
        adv += 7;
        int n = json_int(adv, "n");
        if(n < 1) n = 1;

        if(o && o < outsz-1) out[o++] = ' ';
        if(o < outsz-1) out[o++] = '[';
        for(int i = 0; emo[i] && o < outsz-6; i++) out[o++] = emo[i];
        if(n > 1 && o < outsz-5){
            out[o++] = ' '; out[o++] = 'x';
            if(n > 99) n = 99;
            if(n >= 10) out[o++] = (char)('0' + n/10);
            out[o++] = (char)('0' + n%10);
        }
        if(o < outsz-1) out[o++] = ']';
        out[o] = 0;
        p = adv;
    }
    out[o] = 0;
}

/* Describe an attachment list in text and append it to a message row.
   VGA text mode cannot show a picture, so a name and a size is the honest
   representation -- and unlike a blank cell it tells the reader what arrived.
   Appending to the row costs no extra memory per scrollback line. */
static void media_append(int slot, const char *json)
{
    if(slot < 0) return;
    chat_line_t *l = &g_scroll[slot];
    const char *p = json;
    int guard = 0;

    while(*p && guard++ < MEDIA_PER_MSG_MAX){
        char nm[40], mime[32];
        if(!json_str(p, "name", nm, sizeof(nm))) break;
        if(!json_str(p, "mime", mime, sizeof(mime))) mime[0] = 0;
        int nb = json_int(p, "bytes");

        /* Advance past this element so the next pass finds the next one. */
        const char *adv = p;
        while(*adv && strncmp(adv, "\"name\"", 6)) adv++;
        if(!*adv) break;
        adv += 6;

        int len = (int)strlen(l->text);
        if(len > CHAT_LINE - 16){ p = adv; continue; }

        strncat(l->text, "  [", CHAT_LINE - len - 1);
        /* "image/png" -> "image". The subtype adds nothing at 80 columns. */
        if(mime[0]){
            char kind[16];
            int k = 0;
            while(mime[k] && mime[k] != '/' && k < (int)sizeof(kind)-1){
                kind[k] = mime[k]; k++;
            }
            kind[k] = 0;
            strncat(l->text, kind, CHAT_LINE - strlen(l->text) - 1);
            strncat(l->text, ": ", CHAT_LINE - strlen(l->text) - 1);
        }
        strncat(l->text, nm, CHAT_LINE - strlen(l->text) - 1);
        if(nb > 0){
            char sz[16];
            int o = 0;
            sz[o++] = ' ';
            if(nb >= 1024){
                int kb = nb / 1024;
                if(kb > 9999) kb = 9999;
                if(kb >= 1000) sz[o++] = (char)('0' + kb/1000%10);
                if(kb >= 100)  sz[o++] = (char)('0' + kb/100%10);
                if(kb >= 10)   sz[o++] = (char)('0' + kb/10%10);
                sz[o++] = (char)('0' + kb%10);
                sz[o++] = 'K';
            } else {
                if(nb >= 100) sz[o++] = (char)('0' + nb/100%10);
                if(nb >= 10)  sz[o++] = (char)('0' + nb/10%10);
                sz[o++] = (char)('0' + nb%10);
                sz[o++] = 'B';
            }
            sz[o] = 0;
            strncat(l->text, sz, CHAT_LINE - strlen(l->text) - 1);
        }
        strncat(l->text, "]", CHAT_LINE - strlen(l->text) - 1);
        p = adv;
    }
}

/* Resolve a message reference the user typed into a wire id.
   Accepts a raw id ("c41", "d17") or a small ordinal counting back from the
   newest message, so "/react 1 :+1:" reacts to the last thing said.  Returns 0
   when nothing matches.  out is untouched on failure. */
static int resolve_mid(const char *tok, char *out, int outsz)
{
    if(!tok || !tok[0]) return 0;

    if((tok[0] == 'c' || tok[0] == 'd') && tok[1] >= '0' && tok[1] <= '9'){
        int i = 0;
        while(tok[i] && i < outsz-1){ out[i] = tok[i]; i++; }
        out[i] = 0;
        return sb_find_mid(out) >= 0 || 1;   /* may have scrolled out; still valid */
    }

    int want = 0;
    for(int i = 0; tok[i]; i++){
        if(tok[i] < '0' || tok[i] > '9') return 0;
        want = want*10 + (tok[i] - '0');
    }
    if(want < 1) return 0;

    /* Walk back from the head counting only rows that carry an id. */
    int seen = 0;
    for(int n = 1; n <= g_count; n++){
        int slot = (g_head - n + CHAT_SCROLL*2) % CHAT_SCROLL;
        if(!g_scroll[slot].mid[0]) continue;
        if(++seen == want){
            int i = 0;
            const char *m = g_scroll[slot].mid;
            while(m[i] && i < outsz-1){ out[i] = m[i]; i++; }
            out[i] = 0;
            return 1;
        }
    }
    return 0;
}

/* ── sidebar model ───────────────────────────────────────────────── */

static int srv_index(const char *name)
{
    for(int i=0;i<g_nsrv;i++) if(!strcmp(g_srv[i], name)) return i;
    return -1;
}

static int srv_add(const char *name)
{
    int i = srv_index(name);
    if(i >= 0) return i;
    if(g_nsrv >= MAX_SRV) return -1;
    strncpy(g_srv[g_nsrv], name, NAME_MAX-1); g_srv[g_nsrv][NAME_MAX-1] = 0;
    g_nchan[g_nsrv] = 0;
    return g_nsrv++;
}

static void dm_add(const char *user)
{
    if(!user[0]) return;
    for(int i=0;i<g_ndm;i++) if(!strcmp(g_dm[i], user)) return;
    if(g_ndm >= MAX_DM) return;
    strncpy(g_dm[g_ndm], user, NAME_MAX-1); g_dm[g_ndm][NAME_MAX-1] = 0;
    g_ndm++;
}

/* Rebuild the flat display list from the model, preserving unread flags. */
static void items_rebuild(void)
{
    sbitem_t old[MAX_ITEMS];
    int nold = g_nitems;
    for(int i=0;i<nold;i++) old[i] = g_items[i];

    g_nitems = 0;
    if(g_nsrv){
        if(g_nitems < MAX_ITEMS){
            g_items[g_nitems].kind = SB_HDR; g_items[g_nitems].unread = 0;
            strcpy(g_items[g_nitems].a, "SERVERS"); g_items[g_nitems].b[0] = 0;
            g_nitems++;
        }
        for(int s=0; s<g_nsrv && g_nitems<MAX_ITEMS; s++){
            g_items[g_nitems].kind = SB_SRV; g_items[g_nitems].unread = 0;
            strcpy(g_items[g_nitems].a, g_srv[s]); g_items[g_nitems].b[0] = 0;
            g_nitems++;
            for(int c=0; c<g_nchan[s] && g_nitems<MAX_ITEMS; c++){
                g_items[g_nitems].kind = SB_CHAN; g_items[g_nitems].unread = 0;
                strcpy(g_items[g_nitems].a, g_srv[s]);
                strcpy(g_items[g_nitems].b, g_chan[s][c]);
                g_nitems++;
            }
        }
    }
    if(g_ndm){
        if(g_nitems < MAX_ITEMS){
            g_items[g_nitems].kind = SB_HDR; g_items[g_nitems].unread = 0;
            strcpy(g_items[g_nitems].a, "DIRECT"); g_items[g_nitems].b[0] = 0;
            g_nitems++;
        }
        for(int d=0; d<g_ndm && g_nitems<MAX_ITEMS; d++){
            g_items[g_nitems].kind = SB_DM; g_items[g_nitems].unread = 0;
            strcpy(g_items[g_nitems].a, g_dm[d]); g_items[g_nitems].b[0] = 0;
            g_nitems++;
        }
    }
    /* carry unread flags across a rebuild — a CHANLIST refresh must not
       silently clear the marks that tell you where new traffic landed */
    for(int i=0;i<g_nitems;i++){
        for(int j=0;j<nold;j++){
            if(old[j].kind == g_items[i].kind &&
               !strcmp(old[j].a, g_items[i].a) &&
               !strcmp(old[j].b, g_items[i].b)){
                g_items[i].unread = old[j].unread; break;
            }
        }
    }
    if(g_sel >= g_nitems) g_sel = g_nitems ? g_nitems-1 : 0;
}

static void mark_unread(uint8_t kind, const char *a, const char *b)
{
    for(int i=0;i<g_nitems;i++){
        if(g_items[i].kind != kind) continue;
        if(strcmp(g_items[i].a, a)) continue;
        if(b && strcmp(g_items[i].b, b)) continue;
        g_items[i].unread = 1;
        return;
    }
}

static void clear_unread_here(void)
{
    for(int i=0;i<g_nitems;i++){
        if(g_dmwith[0]){
            if(g_items[i].kind == SB_DM && !strcmp(g_items[i].a, g_dmwith))
                g_items[i].unread = 0;
        } else if(g_items[i].kind == SB_CHAN &&
                  !strcmp(g_items[i].a, g_server) &&
                  !strcmp(g_items[i].b, g_channel)){
            g_items[i].unread = 0;
        }
    }
}

static int item_selectable(int i)
{
    return i >= 0 && i < g_nitems && g_items[i].kind != SB_HDR;
}

static void sel_step(int dir)
{
    if(!g_nitems) return;
    int i = g_sel;
    for(int guard=0; guard<MAX_ITEMS; guard++){
        i += dir;
        if(i < 0) i = 0;
        if(i >= g_nitems) i = g_nitems-1;
        if(item_selectable(i)){ g_sel = i; return; }
        if(i == 0 && dir < 0) return;
        if(i == g_nitems-1 && dir > 0) return;
    }
}

/* ── drawing ─────────────────────────────────────────────────────── */

/* Write a string into a fixed-width field, clipped and space-padded. */
static void put_field(const char *s, int x, int y, int w,
                      vga_color_t fg, vga_color_t bg)
{
    int i = 0;
    for(; s[i] && i < w; i++) vga_putchar_at(s[i], x+i, y, fg, bg);
    for(; i < w; i++)         vga_putchar_at(' ',  x+i, y, fg, bg);
}

static void draw_sidebar(int top, int bottom)
{
    int rows = bottom - top + 1;
    if(rows < 1) return;

    /* keep the selection on screen */
    if(g_sel < g_sbtop)            g_sbtop = g_sel;
    if(g_sel >= g_sbtop + rows)    g_sbtop = g_sel - rows + 1;
    if(g_sbtop < 0)                g_sbtop = 0;

    for(int r=0; r<rows; r++){
        int y = top + r;
        int i = g_sbtop + r;
        vga_color_t bg = VGA_BLACK, fg = VGA_LIGHT_GREY;
        char line[SIDE_W + 2];
        line[0] = 0;

        if(i < g_nitems){
            const sbitem_t *it = &g_items[i];
            int sel = (i == g_sel);
            if(it->kind == SB_HDR){
                strcpy(line, " ");
                strncat(line, it->a, sizeof(line)-strlen(line)-1);
                fg = VGA_DARK_GREY;
            } else if(it->kind == SB_SRV){
                strcpy(line, " ");
                strncat(line, it->a, sizeof(line)-strlen(line)-1);
                fg = VGA_LIGHT_CYAN;
            } else if(it->kind == SB_CHAN){
                strcpy(line, "  #");
                strncat(line, it->b, sizeof(line)-strlen(line)-1);
                int here = !g_dmwith[0] && !strcmp(it->a, g_server)
                                        && !strcmp(it->b, g_channel);
                fg = here ? VGA_WHITE : VGA_LIGHT_GREY;
            } else {
                strcpy(line, "  @");
                strncat(line, it->a, sizeof(line)-strlen(line)-1);
                int here = g_dmwith[0] && !strcmp(it->a, g_dmwith);
                fg = here ? VGA_WHITE : VGA_LIGHT_GREY;
            }
            if(sel && g_focus_side){ bg = VGA_BLUE; fg = VGA_WHITE; }
            put_field(line, 0, y, SIDE_W-1, fg, bg);
            /* unread marker in the last column of the sidebar */
            vga_putchar_at(it->unread ? '*' : ' ', SIDE_W-2, y,
                           VGA_YELLOW, bg);
        } else {
            put_field("", 0, y, SIDE_W-1, fg, bg);
        }
        vga_putchar_at('|', SIDE_W-1, y, VGA_DARK_GREY, VGA_BLACK);
    }
}

static void draw(const char *host, const char *input, int connected)
{
    int rows = vga_rows(), cols = vga_cols();
    int top = 1, bottom = rows - 3;
    int body = bottom - top + 1;
    if(body < 1) return;

    int mx = SIDE_W;                 /* first column of the message pane */
    int mw = cols - mx;              /* its width */
    if(mw < 10) return;

    vga_begin_frame();

    /* ── title bar ── */
    vga_fill_row(0, ' ', VGA_WHITE, VGA_BLUE);
    {
        /* Show WHERE you are. With servers, channels and DMs there are now
           several places a message could go, and a title that only names the
           host makes it easy to type into the wrong one. */
        char t[128];
        strcpy(t, " BOOT Chat  ");
        strncat(t, g_nick, sizeof(t)-strlen(t)-1);
        if(g_dmwith[0]){
            strncat(t, "  @", sizeof(t)-strlen(t)-1);
            strncat(t, g_dmwith, sizeof(t)-strlen(t)-1);
        } else if(g_channel[0]){
            strncat(t, "  ", sizeof(t)-strlen(t)-1);
            strncat(t, g_server, sizeof(t)-strlen(t)-1);
            strncat(t, "/#", sizeof(t)-strlen(t)-1);
            strncat(t, g_channel, sizeof(t)-strlen(t)-1);
        } else {
            strncat(t, "  (no channel)", sizeof(t)-strlen(t)-1);
        }
        (void)host;
        vga_puts_at(t, 0, 0, VGA_WHITE, VGA_BLUE);
        const char *st = connected ? "ONLINE" : "OFFLINE";
        int x = cols - (int)strlen(st) - 1;
        if(x > 0) vga_puts_at(st, x, 0,
                              connected ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLUE);
    }

    draw_sidebar(top, bottom);

    /* ── message pane — oldest visible line first ── */
    int shown = g_count < body ? g_count : body;
    int first = g_count - shown - g_view;
    if(first < 0) first = 0;
    for(int r = 0; r < body; r++){
        int y = top + r;
        for(int x = mx; x < cols; x++)
            vga_putchar_at(' ', x, y, VGA_LIGHT_GREY, VGA_BLACK);
        int idx = first + r;
        if(r < shown && idx < g_count){
            int slot = (g_head - g_count + idx + CHAT_SCROLL*2) % CHAT_SCROLL;
            chat_line_t *l = &g_scroll[slot];
            /* v0.7.19: compose the row so the id (when /ids is on) and any
               reaction summary travel with the text through one put_field,
               rather than three overlapping writes that clip differently. */
            if(l->react[0] || (g_show_ids && l->mid[0])){
                char row[CHAT_LINE + REACT_SUMMARY + 16];
                row[0] = 0;
                if(g_show_ids && l->mid[0]){
                    strncat(row, l->mid, sizeof(row)-strlen(row)-1);
                    strncat(row, " ",    sizeof(row)-strlen(row)-1);
                }
                strncat(row, l->text, sizeof(row)-strlen(row)-1);
                if(l->react[0]){
                    strncat(row, "  ",      sizeof(row)-strlen(row)-1);
                    strncat(row, l->react,  sizeof(row)-strlen(row)-1);
                }
                put_field(row, mx+1, y, mw-1, (vga_color_t)l->fg, VGA_BLACK);
            } else {
                put_field(l->text, mx+1, y, mw-1,
                          (vga_color_t)l->fg, VGA_BLACK);
            }
        }
    }

    /* ── typing indicator, on the separator row ──
       Typing is never stored and the server sends no periodic refresh, so it
       is aged out locally: without this the line would sit there for ever if
       the other end closed its socket mid-word.  3 seconds, in 10ms ticks. */
    if(g_typing[0]){
        if(get_ticks() - g_typing_tick > 300) g_typing[0] = 0;
    }

    /* ── separator + input ── */
    vga_fill_row(rows-2, '-', VGA_DARK_GREY, VGA_BLACK);
    vga_fill_row(rows-1, ' ', VGA_WHITE, VGA_BLACK);
    vga_puts_at(g_focus_side ? "  " : "> ", 0, rows-1,
                VGA_LIGHT_GREEN, VGA_BLACK);
    {
        int avail = cols - 3;
        int len = (int)strlen(input);
        const char *show = (len > avail) ? input + (len - avail) : input;
        vga_puts_at(show, 2, rows-1, VGA_WHITE, VGA_BLACK);
        int cx = 2 + (len > avail ? avail : len);
        if(cx >= cols) cx = cols-1;
        vga_set_cursor(cx, rows-1);
    }

    if(g_focus_side){
        const char *m = " TAB back to typing  ENTER open ";
        int x = cols - (int)strlen(m) - 1;
        if(x > SIDE_W) vga_puts_at(m, x, rows-2, VGA_BLACK, VGA_LIGHT_GREY);
    } else if(g_view > 0){
        const char *m = " SCROLLBACK - End to return ";
        int x = cols - (int)strlen(m) - 1;
        if(x > SIDE_W) vga_puts_at(m, x, rows-2, VGA_BLACK, VGA_LIGHT_GREY);
    } else if(g_typing[0]){
        char m[NAME_MAX + 20];
        strcpy(m, " ");
        strncat(m, g_typing, sizeof(m)-strlen(m)-1);
        strncat(m, " is typing... ", sizeof(m)-strlen(m)-1);
        int x = cols - (int)strlen(m) - 1;
        if(x > SIDE_W) vga_puts_at(m, x, rows-2, VGA_BLACK, VGA_LIGHT_GREY);
    }

    vga_end_frame();
}

/* ── protocol ────────────────────────────────────────────────────── */

static int send_line(int fd, const char *s)
{
    /* Shortcode expansion turns 2 typed bytes into up to 4 UTF-8 bytes, so
       the outgoing buffer is sized for the worst case, not the typed length. */
    char buf[CHAT_INPUT * 4 + 8];
    int n = 0;
    while(s[n] && n < (int)sizeof(buf) - 2){ buf[n] = s[n]; n++; }
    buf[n++] = '\n';
    return tcp_send(fd, buf, (uint16_t)n);
}

/* Split off the next space-delimited field. Returns a pointer past it. */
static char *field(char *p, char *out, int outsz)
{
    int i = 0;
    while(*p == ' ') p++;
    while(*p && *p != ' ' && i < outsz-1) out[i++] = *p++;
    out[i] = 0;
    /* v0.7.18 FIX — a token longer than `out` used to leave p sitting INSIDE
       it, so the tail was then parsed as the next field and every field after
       it shifted by one.  Discard the remainder of the token instead. */
    while(*p && *p != ' ') p++;
    while(*p == ' ') p++;
    return p;
}

static int in_view_channel(const char *srv, const char *chan)
{
    return !g_dmwith[0] && !strcmp(srv, g_server) && !strcmp(chan, g_channel);
}

static void handle_line(int fd, char *line)
{
    if(!line[0]) return;

    char verb[16];
    char *rest = field(line, verb, sizeof(verb));

    if(!strcmp(verb, "TOKEN")) return;
    if(!strcmp(verb, "HISTEND")) return;

    if(!strcmp(verb, "OK")){
        char what[16];
        char *d = field(rest, what, sizeof(what));
        if(!strcmp(what, "PROTO")){
            /* The server answers with the level it actually granted, never
               above what it speaks, so a 2.1-only server simply lands on 2.1
               and every shape below follows that. */
            int maj = 0, min = 0;
            const char *v = d;
            while(*v >= '0' && *v <= '9'){ maj = maj*10 + (*v - '0'); v++; }
            if(*v == '.'){ v++;
                while(*v >= '0' && *v <= '9'){ min = min*10 + (*v - '0'); v++; }
            }
            if(maj){ g_pmaj = maj; g_pmin = min; }
            char m[CHAT_LINE];
            strcpy(m, "-- protocol ");
            strncat(m, d, sizeof(m)-strlen(m)-1);
            sb_add(m, VGA_DARK_GREY);
        } else if(!strcmp(what, "LOGIN")){
            g_authed = 1;
            /* Take the account name from the server, not from what was typed:
               the users table is case-insensitive, so signing in as "haven"
               logs into the account stored as "Haven".  Comparing the typed
               spelling against the stored one is what made a person's own
               messages render as someone else's in v2. */
            strncpy(g_selfname, d, NAME_MAX-1); g_selfname[NAME_MAX-1] = 0;
            if(g_selfname[0]){
                strncpy(g_nick, g_selfname, NAME_MAX-1); g_nick[NAME_MAX-1] = 0;
            }
            char m[CHAT_LINE];
            strcpy(m, "-- signed in as ");
            strncat(m, d, sizeof(m)-strlen(m)-1);
            sb_add(m, VGA_LIGHT_GREEN);
        } else if(!strcmp(what, "JOIN")){
            char srv[NAME_MAX], chan[NAME_MAX];
            char *q = field(d, srv, sizeof(srv));
            field(q, chan, sizeof(chan));
            strncpy(g_server, srv, NAME_MAX-1);   g_server[NAME_MAX-1] = 0;
            strncpy(g_channel, chan, NAME_MAX-1); g_channel[NAME_MAX-1] = 0;
            g_dmwith[0] = 0;
            sb_reset();
            clear_unread_here();
        } else if(!strcmp(what, "CREATE") || !strcmp(what, "JOINSRV")){
            srv_add(d); items_rebuild();
            char c[96];
            strcpy(c, "CHANNELS ");
            strncat(c, d, sizeof(c)-strlen(c)-1);
            send_line(fd, c);
        } else if(!strcmp(what, "MKCHAN")){
            if(g_server[0]){
                char c[96];
                strcpy(c, "CHANNELS ");
                strncat(c, g_server, sizeof(c)-strlen(c)-1);
                send_line(fd, c);
            }
        } else {
            char m[CHAT_LINE];
            strcpy(m, "-- ok: ");
            strncat(m, what, sizeof(m)-strlen(m)-1);
            if(d[0]){ strncat(m, " ", sizeof(m)-strlen(m)-1);
                      strncat(m, d, sizeof(m)-strlen(m)-1); }
            sb_add(m, VGA_LIGHT_CYAN);
        }
        return;
    }

    if(!strcmp(verb, "ERR")){ sb_add_wrapped(rest, VGA_LIGHT_RED); return; }

    if(!strcmp(verb, "SERVERLIST")){
        char name[NAME_MAX];
        char *q = rest;
        g_nsrv = 0;
        while(*q){
            q = field(q, name, sizeof(name));
            if(name[0]) srv_add(name);
        }
        items_rebuild();
        if(!g_nsrv) sb_add("-- no servers yet; try /create <name>", VGA_LIGHT_CYAN);
        /* ask for each server's channels so the tree fills itself in */
        for(int i=0;i<g_nsrv;i++){
            char c[96];
            strcpy(c, "CHANNELS ");
            strncat(c, g_srv[i], sizeof(c)-strlen(c)-1);
            send_line(fd, c);
        }
        send_line(fd, "DMLIST");
        return;
    }

    if(!strcmp(verb, "CHANLIST")){
        char srv[NAME_MAX], chan[NAME_MAX];
        char *q = field(rest, srv, sizeof(srv));
        int si = srv_add(srv);
        if(si < 0) return;
        g_nchan[si] = 0;
        while(*q && g_nchan[si] < MAX_CHAN){
            q = field(q, chan, sizeof(chan));
            if(!chan[0]) break;
            strncpy(g_chan[si][g_nchan[si]], chan, NAME_MAX-1);
            g_chan[si][g_nchan[si]][NAME_MAX-1] = 0;
            g_nchan[si]++;
        }
        items_rebuild();
        return;
    }

    if(!strcmp(verb, "DMLIST")){
        char name[NAME_MAX];
        char *q = rest;
        g_ndm = 0;
        while(*q){
            q = field(q, name, sizeof(name));
            if(name[0]) dm_add(name);
        }
        items_rebuild();
        return;
    }

    if(!strcmp(verb, "ONLINE")){
        char m[CHAT_WIRE];
        strcpy(m, "-- online: ");
        strncat(m, rest, sizeof(m)-strlen(m)-1);
        sb_add_wrapped(m, VGA_LIGHT_CYAN);
        return;
    }

    /* 2.0   MSG <server> <channel> <user> <ts> <text>
       2.1+  MSG <server> <channel> c<id> <user> <ts> <edited> <reply> <text> */
    if(!strcmp(verb, "MSG")){
        char srv[NAME_MAX], chan[NAME_MAX], who[NAME_MAX], ts[16];
        char mid[12], ed[4], rep_to[12];
        mid[0] = 0; ed[0] = 0; rep_to[0] = 0;
        char *q = field(rest, srv, sizeof(srv));
        q = field(q, chan, sizeof(chan));
        if(proto_at_least(2,1)) q = field(q, mid, sizeof(mid));
        q = field(q, who, sizeof(who));
        q = field(q, ts, sizeof(ts));
        if(proto_at_least(2,1)){
            q = field(q, ed, sizeof(ed));
            q = field(q, rep_to, sizeof(rep_to));
        }
        if(!in_view_channel(srv, chan)){
            mark_unread(SB_CHAN, srv, chan);   /* show it in the sidebar */
            return;
        }
        char out[CHAT_WIRE];
        strcpy(out, "<");
        strncat(out, who, sizeof(out)-strlen(out)-1);
        strncat(out, "> ", sizeof(out)-strlen(out)-1);
        /* A reply shows what it answers, so a thread reads in order even
           though the pane is flat. */
        if(rep_to[0] && strcmp(rep_to, "0")){
            int rslot = sb_find_mid(rep_to);
            strncat(out, "|re ", sizeof(out)-strlen(out)-1);
            if(rslot >= 0){
                char snip[24];
                int k = 0;
                const char *t = g_scroll[rslot].text;
                while(t[k] && k < (int)sizeof(snip)-1){ snip[k] = t[k]; k++; }
                snip[k] = 0;
                strncat(out, snip, sizeof(out)-strlen(out)-1);
            } else {
                strncat(out, rep_to, sizeof(out)-strlen(out)-1);
            }
            strncat(out, "| ", sizeof(out)-strlen(out)-1);
        }
        strncat(out, q, sizeof(out)-strlen(out)-1);
        if(ed[0] == '1') strncat(out, "  (edited)", sizeof(out)-strlen(out)-1);
        sb_add_wrapped(out, strcmp(who, g_nick) == 0 ? VGA_LIGHT_GREEN : VGA_WHITE);
        if(mid[0]){
            sb_tag_last(mid, ed[0] == '1');
            strncpy(g_lastmid, mid, sizeof(g_lastmid)-1);
            g_lastmid[sizeof(g_lastmid)-1] = 0;
            /* Someone speaking ends their typing indicator immediately. */
            if(!strcmp(g_typing, who)) g_typing[0] = 0;
            /* Mark it read here, so the badge clears on the other devices. */
            if(proto_at_least(2,2)){
                char r[96];
                strcpy(r, "READ channel ");
                strncat(r, srv, sizeof(r)-strlen(r)-1);
                strncat(r, " ", sizeof(r)-strlen(r)-1);
                strncat(r, mid, sizeof(r)-strlen(r)-1);
                send_line(fd, r);
            }
        }
        return;
    }

    /* 2.0   DMMSG <from> <to> <ts> <text>
       2.1+  DMMSG d<id> <from> <to> <ts> <edited> <reply> <text> */
    if(!strcmp(verb, "DMMSG")){
        char frm[NAME_MAX], to[NAME_MAX], ts[16];
        char mid[12], ed[4], rep_to[12];
        mid[0] = 0; ed[0] = 0; rep_to[0] = 0;
        char *q = rest;
        if(proto_at_least(2,1)) q = field(q, mid, sizeof(mid));
        q = field(q, frm, sizeof(frm));
        q = field(q, to, sizeof(to));
        q = field(q, ts, sizeof(ts));
        if(proto_at_least(2,1)){
            q = field(q, ed, sizeof(ed));
            q = field(q, rep_to, sizeof(rep_to));
        }
        const char *other = strcmp(frm, g_nick) ? frm : to;
        dm_add(other);
        items_rebuild();
        if(!g_dmwith[0] || strcmp(other, g_dmwith)){
            mark_unread(SB_DM, other, 0);
            return;
        }
        char out[CHAT_WIRE];
        strcpy(out, "<");
        strncat(out, frm, sizeof(out)-strlen(out)-1);
        strncat(out, "> ", sizeof(out)-strlen(out)-1);
        strncat(out, q, sizeof(out)-strlen(out)-1);
        if(ed[0] == '1') strncat(out, "  (edited)", sizeof(out)-strlen(out)-1);
        sb_add_wrapped(out, strcmp(frm, g_nick) == 0 ? VGA_LIGHT_GREEN : VGA_WHITE);
        if(mid[0]){
            sb_tag_last(mid, ed[0] == '1');
            strncpy(g_lastmid, mid, sizeof(g_lastmid)-1);
            g_lastmid[sizeof(g_lastmid)-1] = 0;
            if(!strcmp(g_typing, frm)) g_typing[0] = 0;
            if(proto_at_least(2,2)){
                char r[96];
                strcpy(r, "READ dm ");
                strncat(r, other, sizeof(r)-strlen(r)-1);
                strncat(r, " ", sizeof(r)-strlen(r)-1);
                strncat(r, mid, sizeof(r)-strlen(r)-1);
                send_line(fd, r);
            }
        }
        return;
    }

    if(!strcmp(verb, "JOINED")){
        char who[NAME_MAX];
        field(rest, who, sizeof(who));
        char m[CHAT_LINE];
        strcpy(m, "-- "); strncat(m, who, sizeof(m)-strlen(m)-1);
        strncat(m, " joined", sizeof(m)-strlen(m)-1);
        sb_add(m, VGA_LIGHT_CYAN);
        return;
    }
    if(!strcmp(verb, "LEFT")){
        char m[CHAT_LINE];
        strcpy(m, "-- "); strncat(m, rest, sizeof(m)-strlen(m)-1);
        strncat(m, " left", sizeof(m)-strlen(m)-1);
        sb_add(m, VGA_LIGHT_CYAN);
        return;
    }

    /* ══ v0.7.19: protocol 2.1+/2.2 verbs ═════════════════════════════════ */

    /* MSGEDIT <id> <ts> <text> — rewrite the row in place. */
    if(!strcmp(verb, "MSGEDIT")){
        char mid[12], ts[16];
        char *q = field(rest, mid, sizeof(mid));
        q = field(q, ts, sizeof(ts));
        int slot = sb_find_mid(mid);
        if(slot < 0) return;             /* scrolled out of the ring */
        /* Keep the "<who> " prefix that is already on the row. */
        char out[CHAT_WIRE];
        const char *old = g_scroll[slot].text;
        int k = 0;
        if(old[0] == '<'){
            while(old[k] && old[k] != '>' && k < CHAT_LINE-1) k++;
            if(old[k] == '>') k++;
            if(old[k] == ' ') k++;
        }
        for(int i = 0; i < k; i++) out[i] = old[i];
        out[k] = 0;
        strncat(out, q, sizeof(out)-strlen(out)-1);
        strncat(out, "  (edited)", sizeof(out)-strlen(out)-1);
        sb_set_text(slot, out);
        g_scroll[slot].edited = 1;
        return;
    }

    /* MSGDEL <id> — the text goes, the row stays so the history reads right. */
    if(!strcmp(verb, "MSGDEL")){
        char mid[12];
        field(rest, mid, sizeof(mid));
        int slot = sb_find_mid(mid);
        if(slot < 0) return;
        sb_set_text(slot, "-- message deleted");
        g_scroll[slot].fg = VGA_DARK_GREY;
        g_scroll[slot].react[0] = 0;
        return;
    }

    /* REACT <id> [ {"emoji":..,"n":..}, ... ]  — full set for that message. */
    if(!strcmp(verb, "REACT")){
        char mid[12];
        char *q = field(rest, mid, sizeof(mid));
        int slot = sb_find_mid(mid);
        if(slot < 0) return;
        react_summary(q, g_scroll[slot].react, REACT_SUMMARY);
        return;
    }

    /* REACTIONS {"c41":[...],"c42":[...]} — the backfill form on join. */
    if(!strcmp(verb, "REACTIONS")){
        /* Walk each "cNN": / "dNN": key and hand its array to react_summary. */
        char *q = rest;
        while(*q){
            if(*q == '"' && (q[1] == 'c' || q[1] == 'd')){
                char mid[12];
                int i = 0;
                char *r = q + 1;
                while(*r && *r != '"' && i < (int)sizeof(mid)-1) mid[i++] = *r++;
                mid[i] = 0;
                int slot = sb_find_mid(mid);
                if(slot >= 0){
                    char *arr = r;
                    while(*arr && *arr != '[') arr++;
                    if(*arr) react_summary(arr, g_scroll[slot].react, REACT_SUMMARY);
                }
                q = r;
                continue;
            }
            q++;
        }
        return;
    }

    /* TYPING channel <server> <channel> <user>   |   TYPING dm <user> */
    if(!strcmp(verb, "TYPING") || !strcmp(verb, "TYPINGOFF")){
        int off = (verb[6] == 'O');
        char kind[12], a[NAME_MAX], b[NAME_MAX], who[NAME_MAX];
        char *q = field(rest, kind, sizeof(kind));
        if(!strcmp(kind, "channel")){
            q = field(q, a, sizeof(a));
            q = field(q, b, sizeof(b));
            field(q, who, sizeof(who));
            if(!in_view_channel(a, b)) return;
        } else {
            field(q, who, sizeof(who));
            if(!g_dmwith[0] || strcmp(who, g_dmwith)) return;
        }
        if(!strcmp(who, g_nick)) return;          /* never ourselves */
        if(off){ if(!strcmp(g_typing, who)) g_typing[0] = 0; return; }
        strncpy(g_typing, who, NAME_MAX-1); g_typing[NAME_MAX-1] = 0;
        g_typing_tick = get_ticks();
        return;
    }

    /* READ <kind> <target> <id> — mirrored from our own other devices. */
    if(!strcmp(verb, "READ")){
        char kind[12], tgt[NAME_MAX];
        char *q = field(rest, kind, sizeof(kind));
        field(q, tgt, sizeof(tgt));
        for(int i = 0; i < g_nitems; i++){
            if(!strcmp(kind, "dm")){
                if(g_items[i].kind == SB_DM && !strcmp(g_items[i].a, tgt))
                    g_items[i].unread = 0;
            } else if(g_items[i].kind == SB_CHAN && !strcmp(g_items[i].a, tgt)) {
                g_items[i].unread = 0;
            }
        }
        return;
    }

    /* MSGMEDIA <id> [ {...}, ... ] — attachments on one message (2.3). */
    if(!strcmp(verb, "MSGMEDIA")){
        char mid[12];
        char *q = field(rest, mid, sizeof(mid));
        media_append(sb_find_mid(mid), q);
        return;
    }

    /* ATTACHMENTS {"c41":[...],...} — the backfill form, one line (2.3). */
    if(!strcmp(verb, "ATTACHMENTS")){
        char *q = rest;
        while(*q){
            if(*q == '"' && (q[1] == 'c' || q[1] == 'd')){
                char mid[12];
                int i = 0;
                char *r = q + 1;
                while(*r && *r != '"' && i < (int)sizeof(mid)-1) mid[i++] = *r++;
                mid[i] = 0;
                int slot = sb_find_mid(mid);
                if(slot >= 0){
                    char *arr = r;
                    while(*arr && *arr != '[') arr++;
                    if(*arr) media_append(slot, arr);
                }
                q = r;
                continue;
            }
            q++;
        }
        return;
    }

    /* MEDIA / MEDIADONE / MEDIAINFO — HavenDOS can receive an attachment and
       say what it is, but it has no file picker and no multipart HTTP client,
       so it does not upload.  Acknowledge rather than print raw JSON. */
    if(!strcmp(verb, "MEDIA") || !strcmp(verb, "MEDIADONE") ||
       !strcmp(verb, "MEDIAINFO") || !strcmp(verb, "ATTACHED")){
        return;
    }

    /* UNREADS {...} — badge state at sign-in.  Counts are not shown per
       channel in this client; the sidebar carries a flag, so set the flag. */
    if(!strcmp(verb, "UNREADS")) return;

    /* CHANGONE <server> <channel> — the channel under us was deleted. */
    if(!strcmp(verb, "CHANGONE")){
        char srv[NAME_MAX], chan[NAME_MAX];
        char *q = field(rest, srv, sizeof(srv));
        field(q, chan, sizeof(chan));
        char m[CHAT_LINE];
        strcpy(m, "-- channel ");
        strncat(m, chan, sizeof(m)-strlen(m)-1);
        strncat(m, " was removed", sizeof(m)-strlen(m)-1);
        sb_add(m, VGA_LIGHT_RED);
        if(in_view_channel(srv, chan)) g_channel[0] = 0;
        int si = srv_index(srv);
        if(si >= 0){
            char c[96];
            strcpy(c, "CHANNELS ");
            strncat(c, srv, sizeof(c)-strlen(c)-1);
            send_line(fd, c);
        }
        return;
    }

    /* MODNOTE <action> <server> <seconds> <reason> — kicked, banned, muted. */
    if(!strcmp(verb, "MODNOTE")){
        char act[16], srv[NAME_MAX], secs[12];
        char *q = field(rest, act, sizeof(act));
        q = field(q, srv, sizeof(srv));
        q = field(q, secs, sizeof(secs));
        char m[CHAT_WIRE];
        strcpy(m, "** ");
        strncat(m, act, sizeof(m)-strlen(m)-1);
        strncat(m, " in ", sizeof(m)-strlen(m)-1);
        strncat(m, srv, sizeof(m)-strlen(m)-1);
        if(secs[0] && strcmp(secs, "0")){
            strncat(m, " for ", sizeof(m)-strlen(m)-1);
            strncat(m, secs, sizeof(m)-strlen(m)-1);
            strncat(m, "s", sizeof(m)-strlen(m)-1);
        }
        if(q[0]){
            strncat(m, " - ", sizeof(m)-strlen(m)-1);
            strncat(m, q, sizeof(m)-strlen(m)-1);
        }
        sb_add_wrapped(m, VGA_LIGHT_RED);
        toast_warning(m);
        return;
    }

    /* NEEDEMAIL 1 — the server will gate commands until an address is set. */
    if(!strcmp(verb, "NEEDEMAIL")){
        g_needemail = 1;
        sb_add("** this account still needs an email address", VGA_YELLOW);
        sb_add("** set it with:  /email <address>", VGA_YELLOW);
        return;
    }

    /* PRESENCE {"user":..,"status":..} — one line, not the whole object. */
    if(!strcmp(verb, "PRESENCE")){
        char who[NAME_MAX], st[24];
        if(json_str(rest, "user", who, sizeof(who))){
            if(!json_str(rest, "status", st, sizeof(st))) strcpy(st, "?");
            char m[CHAT_LINE];
            strcpy(m, "-- ");
            strncat(m, who, sizeof(m)-strlen(m)-1);
            strncat(m, " is ", sizeof(m)-strlen(m)-1);
            strncat(m, st, sizeof(m)-strlen(m)-1);
            sb_add(m, VGA_DARK_GREY);
        }
        return;
    }

    /* SELF {...} — confirm the stored spelling; the rest is for richer clients. */
    if(!strcmp(verb, "SELF")){
        char nm[NAME_MAX];
        if(json_str(rest, "name", nm, sizeof(nm)) && nm[0]){
            strncpy(g_selfname, nm, NAME_MAX-1); g_selfname[NAME_MAX-1] = 0;
            strncpy(g_nick, nm, NAME_MAX-1);     g_nick[NAME_MAX-1] = 0;
        }
        return;
    }

    /* The 2.2 administrative objects are single-line JSON meant for the
       desktop and admin applications.  Dumping them raw into an 80-column
       text pane is worse than saying nothing, so acknowledge and drop. */
    if(!strcmp(verb, "PROFILE")   || !strcmp(verb, "PRIVACY")  ||
       !strcmp(verb, "SESSIONLIST")|| !strcmp(verb, "BADGES")  ||
       !strcmp(verb, "PLAN")      || !strcmp(verb, "ROLES")    ||
       !strcmp(verb, "SRVINFO")   || !strcmp(verb, "AUDIT")    ||
       !strcmp(verb, "MODLOG")    || !strcmp(verb, "BLOCKLIST")){
        char m[CHAT_LINE];
        strcpy(m, "-- ");
        strncat(m, verb, sizeof(m)-strlen(m)-1);
        strncat(m, " received (use the desktop app to view it)", sizeof(m)-strlen(m)-1);
        sb_add(m, VGA_DARK_GREY);
        return;
    }

    sb_add_wrapped(line, VGA_DARK_GREY);   /* unknown verb — show, don't drop */
}

/* Open whatever the sidebar selection points at. */
static void activate(int fd, int idx)
{
    if(!item_selectable(idx)) return;
    sbitem_t *it = &g_items[idx];
    char c[96];
    if(it->kind == SB_CHAN){
        strcpy(c, "JOIN ");
        strncat(c, it->a, sizeof(c)-strlen(c)-1);
        strncat(c, " ", sizeof(c)-strlen(c)-1);
        strncat(c, it->b, sizeof(c)-strlen(c)-1);
        send_line(fd, c);
        it->unread = 0;
    } else if(it->kind == SB_DM){
        strncpy(g_dmwith, it->a, NAME_MAX-1); g_dmwith[NAME_MAX-1] = 0;
        g_channel[0] = 0;
        sb_reset();
        it->unread = 0;
        strcpy(c, "DMHIST ");
        strncat(c, g_dmwith, sizeof(c)-strlen(c)-1);
        send_line(fd, c);
    } else if(it->kind == SB_SRV){
        strcpy(c, "CHANNELS ");
        strncat(c, it->a, sizeof(c)-strlen(c)-1);
        send_line(fd, c);
    }
}

/* ── target parsing:  user@host[:port] ───────────────────────────── */

static int parse_target(const char *arg, char *user, char *host, uint16_t *port)
{
    int i = 0;
    while(arg[i] && arg[i] != '@' && i < NAME_MAX-1){ user[i] = arg[i]; i++; }
    user[i] = 0;
    if(arg[i] != '@' || !user[0]) return 0;

    const char *h = arg + i + 1;
    int j = 0;
    while(h[j] && h[j] != ':' && j < 63){ host[j] = h[j]; j++; }
    host[j] = 0;
    if(!host[0]) return 0;

    *port = CHAT_PORT_DEFAULT;
    if(h[j] == ':'){
        uint32_t p = 0; const char *q = h + j + 1;
        if(!(*q >= '0' && *q <= '9')) return 0;
        while(*q >= '0' && *q <= '9'){ p = p*10 + (uint32_t)(*q - '0'); q++; }
        if(p == 0 || p > 65535) return 0;
        *port = (uint16_t)p;
    }
    return 1;
}

/* Read a line at the current cursor. echo=0 masks it for passwords. */
static int read_field(char *out, int outsz, int echo)
{
    int n = 0; out[0] = 0;
    for(;;){
        int k = keyboard_getchar();
        /* net_poll() drains the receive path; kernel_poll() runs the
           PERIODIC work, which is a different thing and the one that
           keeps DHCP retrying and notices a stick being plugged in.
           This loop had the first and not the second, so a person
           sitting on a login prompt stopped both. */
        if(k < 0){
            net_poll();
            kernel_poll();  /* kpoll-site-ok: waiting for a person to type, which is an idle point and may last minutes */
            sleep_ms(5);
            continue;
        }
        if(k == 0x1B) return 0;                       /* ESC = cancel */
        if(k > 0xFF) continue;
        if(k == '\b'){
            if(n > 0){ out[--n] = 0; vga_puts("\b \b"); }
            continue;
        }
        if(k == '\n' || k == '\r'){ vga_putchar('\n'); return n > 0; }
        if(k >= 32 && k < 127 && n < outsz-1){
            out[n++] = (char)k; out[n] = 0;
            vga_putchar(echo ? (char)k : '*');
        }
    }
}

/* ── the client ──────────────────────────────────────────────────── */

void cmd_bootchat(const char *arg1)
{
    char user[NAME_MAX], host[64];
    uint16_t port = CHAT_PORT_DEFAULT;

    if(!net_ready()){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("  Network is not up. Try: ifconfig dhcp\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    const char *target = (arg1 && arg1[0]) ? arg1 : g_last_target;
    if(!target || !target[0]){
        vga_puts("  Usage: chat <user>@<host>[:<port>]   (default port 6700)\n");
        vga_puts("  In chat: TAB for the sidebar, /help for commands\n");
        return;
    }
    if(!parse_target(target, user, host, &port)){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("  Bad target. Expected <user>@<host>[:<port>]\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    strncpy(g_last_target, target, sizeof(g_last_target)-1);
    g_last_target[sizeof(g_last_target)-1] = 0;
    strncpy(g_nick, user, NAME_MAX-1); g_nick[NAME_MAX-1] = 0;

    char pass[64];
    vga_puts("  Password for "); vga_puts(user); vga_puts(": ");
    if(!read_field(pass, sizeof(pass), 0)){ vga_puts("  Cancelled.\n"); return; }
    vga_puts("  [L]ogin or [R]egister a new account? ");
    int mode = 0;
    for(;;){
        int k = keyboard_getchar();
        if(k < 0){
            kernel_poll();  /* kpoll-site-ok: waiting for a person to answer a prompt, which is an idle point and may last minutes */
            sleep_ms(5);
            continue;
        }
        if(k == 'l' || k == 'L'){ mode = 0; vga_puts("login\n");    break; }
        if(k == 'r' || k == 'R'){ mode = 1; vga_puts("register\n"); break; }
        if(k == 0x1B){ vga_puts("cancelled\n"); return; }
    }

    /* v0.7.19: protocol 2.2 registration takes an address as well, and the
       server gates a legacy account until one is set.  Ask once, here, rather
       than letting the first command fail with a message about email. */
    char email[EMAIL_MAX];
    email[0] = 0;
    if(mode){
        vga_puts("  Email address: ");
        if(!read_field(email, sizeof(email), 1)){ vga_puts("  Cancelled.\n"); return; }
        vga_puts("\n");
    }

    vga_puts("  Resolving "); vga_puts(host); vga_puts(" ...\n");
    uint32_t ip = dns_resolve(host);
    if(!ip){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        /* dns_resolve() returns 0 both for "lookup failed" and for the
           address 0.0.0.0, so say what was tried rather than blaming DNS. */
        vga_puts("  Could not reach that address.\n");
        vga_puts("  From a QEMU guest the host machine is 10.0.2.2\n");
        vga_puts("  Names from /hosts.txt also work - see 'man chat'\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    vga_puts("  Connecting ...\n");
    int fd = tcp_connect(ip, port);
    if(fd < 0){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_puts(fd == -2 ? "  No free sockets.\n" : "  Connect failed.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    sb_reset();
    g_authed = 0; g_server[0] = 0; g_channel[0] = 0; g_dmwith[0] = 0;
    g_nsrv = 0; g_ndm = 0; g_nitems = 0; g_sel = 0; g_sbtop = 0;
    g_focus_side = 0;
    items_rebuild();
    sb_add("-- connected, authenticating", VGA_LIGHT_CYAN);

    /* Ask for the newest protocol first.  The server answers OK PROTO with the
       level it granted and applies it to everything that follows on this
       connection, so the auth line below is already interpreted at 2.2.  An
       older server that does not know PROTO answers ERR and leaves us at 2.0,
       which is exactly what this client spoke before. */
    g_pmaj = 2; g_pmin = 0;
    g_selfname[0] = 0; g_needemail = 0; g_typing[0] = 0; g_lastmid[0] = 0;
    {
        char pv[24];
        strcpy(pv, "PROTO ");
        pv[6] = (char)('0' + PROTO_WANT_MAJ); pv[7] = '.';
        pv[8] = (char)('0' + PROTO_WANT_MIN); pv[9] = 0;
        send_line(fd, pv);
    }
    {
        char auth[256];
        strcpy(auth, mode ? "REGISTER " : "LOGIN ");
        strncat(auth, user, sizeof(auth)-strlen(auth)-1);
        /* REGISTER is <user> <email> <password> at 2.2 and <user> <password>
           below it, so send the shape the granted level actually expects. */
        if(mode && email[0]){
            strncat(auth, " ", sizeof(auth)-strlen(auth)-1);
            strncat(auth, email, sizeof(auth)-strlen(auth)-1);
        }
        strncat(auth, " ",  sizeof(auth)-strlen(auth)-1);
        strncat(auth, pass, sizeof(auth)-strlen(auth)-1);
        send_line(fd, auth);
        memset(pass, 0, sizeof(pass));   /* don't leave it on the stack */
        memset(email, 0, sizeof(email));
    }

    char input[CHAT_INPUT]; int ilen = 0; input[0] = 0;
    static char rxline[CHAT_WIRE]; int rlen = 0;
    int running = 1, connected = 1;
    uint32_t last_draw = 0;

    vga_clear();
    ui_force_cursor_redraw();
    mouse_get_clicks();               /* flush any stale click edge */

    while(running){
        uint8_t rb[512];
        int n = tcp_recv(fd, rb, sizeof(rb), 15);
        if(n < 0 || (n == 0 && tcp_eof(fd))){
            connected = 0;
            sb_add("-- disconnected by server", VGA_LIGHT_RED);
            draw(host, input, connected);
            break;
        }
        for(int i = 0; i < n; i++){
            char c = (char)rb[i];
            if(c == '\n'){
                rxline[rlen] = 0;
                if(!strcmp(rxline, "PING")) send_line(fd, "PONG");
                else { utf8_to_cp437(rxline, CHAT_WIRE); handle_line(fd, rxline); }
                rlen = 0;
            } else if(c != '\r' && rlen < CHAT_WIRE-1){
                rxline[rlen++] = c;
            }
        }

        /* ── mouse: click anywhere in the sidebar to open that row ── */
        if(mouse_is_ready() && mouse_get_clicks()){
            int px, py, pb;
            mouse_get(&px, &py, &pb);
            if(px < SIDE_W-1 && py >= 1 && py <= vga_rows()-3){
                int idx = g_sbtop + (py - 1);
                if(item_selectable(idx)){
                    g_sel = idx;
                    activate(fd, idx);
                    g_focus_side = 0;     /* carry on typing after a click */
                }
            } else if(py == vga_rows()-1){
                g_focus_side = 0;         /* click the input line to type */
            }
        }

        /* ── keyboard ── */
        int k;
        while((k = keyboard_getchar()) >= 0){
            if(k == 0x1B){ running = 0; break; }
            if(k == '\t'){ g_focus_side = !g_focus_side; continue; }

            if(k > 0xFF){
                int ext = k & 0xFF;
                if(g_focus_side){
                    if(ext == 0x48) sel_step(-1);            /* up   */
                    else if(ext == 0x50) sel_step(+1);       /* down */
                    else if(ext == 0x49) for(int z=0;z<5;z++) sel_step(-1);
                    else if(ext == 0x51) for(int z=0;z<5;z++) sel_step(+1);
                } else {
                    if(ext == 0x49){ if(g_view < g_count) g_view += 5; }
                    else if(ext == 0x51){ g_view -= 5; if(g_view < 0) g_view = 0; }
                    else if(ext == 0x4F){ g_view = 0; }
                }
                continue;
            }

            if(g_focus_side){
                if(k == '\n' || k == '\r'){ activate(fd, g_sel); g_focus_side = 0; }
                continue;                 /* the sidebar eats plain keys */
            }

            if(k == '\b'){ if(ilen > 0) input[--ilen] = 0; continue; }
            if(k == '\n' || k == '\r'){
                if(!ilen) continue;
                if(!strcmp(input, "/quit")){ running = 0; break; }
                else if(!strcmp(input, "/help")){
                    sb_add("-- TAB sidebar, arrows move, ENTER opens", VGA_DARK_GREY);
                    sb_add("-- click the sidebar with the mouse too", VGA_DARK_GREY);
                    sb_add("-- /create <s> /joinsrv <s> /mkchan <c>", VGA_DARK_GREY);
                    sb_add("-- /dm <user> /who /quit", VGA_DARK_GREY);
                    if(proto_at_least(2,1)){
                        sb_add("-- /reply <n> <t> /edit <n> <t> /del <n>", VGA_DARK_GREY);
                        sb_add("-- n counts back from the newest; /ids shows ids", VGA_DARK_GREY);
                    }
                    if(proto_at_least(2,2)){
                        sb_add("-- /react <n> <:emoji:> /unreact <n> <:emoji:>", VGA_DARK_GREY);
                        sb_add("-- /email <addr> /verify <tok> /resend", VGA_DARK_GREY);
                        sb_add("-- /me /sessions /proto", VGA_DARK_GREY);
                    }
                }
                else if(!strcmp(input, "/servers")) send_line(fd, "SERVERS");
                else if(!strcmp(input, "/dms"))     send_line(fd, "DMLIST");
                else if(!strcmp(input, "/who"))     send_line(fd, "WHO");
                else if(!strncmp(input, "/create ", 8)){
                    char c[96]; strcpy(c, "CREATE ");
                    strncat(c, input+8, sizeof(c)-strlen(c)-1); send_line(fd, c);
                }
                else if(!strncmp(input, "/joinsrv ", 9)){
                    char c[96]; strcpy(c, "JOINSRV ");
                    strncat(c, input+9, sizeof(c)-strlen(c)-1); send_line(fd, c);
                }
                else if(!strncmp(input, "/channels", 9)){
                    char c[96]; strcpy(c, "CHANNELS ");
                    const char *a = input[9] == ' ' ? input+10 : g_server;
                    strncat(c, a, sizeof(c)-strlen(c)-1); send_line(fd, c);
                }
                else if(!strncmp(input, "/mkchan ", 8)){
                    if(!g_server[0]) sb_add("-- join a server first", VGA_LIGHT_RED);
                    else {
                        char c[96]; strcpy(c, "MKCHAN ");
                        strncat(c, g_server, sizeof(c)-strlen(c)-1);
                        strncat(c, " ", sizeof(c)-strlen(c)-1);
                        strncat(c, input+8, sizeof(c)-strlen(c)-1);
                        send_line(fd, c);
                    }
                }
                else if(!strncmp(input, "/join ", 6)){
                    char c[96]; strcpy(c, "JOIN ");
                    strncat(c, input+6, sizeof(c)-strlen(c)-1); send_line(fd, c);
                }
                else if(!strncmp(input, "/j ", 3)){
                    if(!g_server[0]) sb_add("-- join a server first", VGA_LIGHT_RED);
                    else {
                        char c[96]; strcpy(c, "JOIN ");
                        strncat(c, g_server, sizeof(c)-strlen(c)-1);
                        strncat(c, " ", sizeof(c)-strlen(c)-1);
                        strncat(c, input+3, sizeof(c)-strlen(c)-1);
                        send_line(fd, c);
                    }
                }
                else if(!strncmp(input, "/dm ", 4)){
                    char who[NAME_MAX];
                    field((char*)input+4, who, sizeof(who));
                    strncpy(g_dmwith, who, NAME_MAX-1); g_dmwith[NAME_MAX-1] = 0;
                    g_channel[0] = 0;
                    dm_add(g_dmwith); items_rebuild(); clear_unread_here();
                    sb_reset();
                    char c[96]; strcpy(c, "DMHIST ");
                    strncat(c, g_dmwith, sizeof(c)-strlen(c)-1); send_line(fd, c);
                }
                /* ══ v0.7.19: protocol 2.1+/2.2 commands ═══════════════════
                   A message is referenced either by its wire id (c41) or by
                   how far back it is (1 = newest).  /ids shows the ids in the
                   pane when you want to point at one exactly. */
                else if(!strncmp(input, "/reply ", 7) || !strncmp(input, "/r ", 3)){
                    int off = input[2] == ' ' ? 3 : 7;
                    char ref[16], mid[12];
                    char *tail = field((char*)input+off, ref, sizeof(ref));
                    if(!proto_at_least(2,1))
                        sb_add("-- replies need protocol 2.1; server gave 2.0", VGA_LIGHT_RED);
                    else if(!resolve_mid(ref, mid, sizeof(mid)))
                        sb_add("-- no such message; try /reply 1 <text>", VGA_LIGHT_RED);
                    else if(!tail[0])
                        sb_add("-- /reply <n|id> <text>", VGA_LIGHT_RED);
                    else {
                        char wire[CHAT_INPUT * 4];
                        expand_shortcodes(tail, wire, sizeof(wire));
                        char c[CHAT_INPUT * 4 + 32];
                        strcpy(c, "REPLY ");
                        strncat(c, mid, sizeof(c)-strlen(c)-1);
                        strncat(c, " ", sizeof(c)-strlen(c)-1);
                        strncat(c, wire, sizeof(c)-strlen(c)-1);
                        send_line(fd, c);
                    }
                }
                else if(!strncmp(input, "/edit ", 6)){
                    char ref[16], mid[12];
                    char *tail = field((char*)input+6, ref, sizeof(ref));
                    if(!proto_at_least(2,1))
                        sb_add("-- editing needs protocol 2.1", VGA_LIGHT_RED);
                    else if(!resolve_mid(ref, mid, sizeof(mid)))
                        sb_add("-- no such message; try /edit 1 <text>", VGA_LIGHT_RED);
                    else if(!tail[0])
                        sb_add("-- /edit <n|id> <text>", VGA_LIGHT_RED);
                    else {
                        char wire[CHAT_INPUT * 4];
                        expand_shortcodes(tail, wire, sizeof(wire));
                        char c[CHAT_INPUT * 4 + 32];
                        strcpy(c, "EDIT ");
                        strncat(c, mid, sizeof(c)-strlen(c)-1);
                        strncat(c, " ", sizeof(c)-strlen(c)-1);
                        strncat(c, wire, sizeof(c)-strlen(c)-1);
                        send_line(fd, c);
                    }
                }
                else if(!strncmp(input, "/del ", 5)){
                    char ref[16], mid[12];
                    field((char*)input+5, ref, sizeof(ref));
                    if(!proto_at_least(2,1))
                        sb_add("-- deleting needs protocol 2.1", VGA_LIGHT_RED);
                    else if(!resolve_mid(ref, mid, sizeof(mid)))
                        sb_add("-- no such message; try /del 1", VGA_LIGHT_RED);
                    else {
                        char c[32]; strcpy(c, "DEL ");
                        strncat(c, mid, sizeof(c)-strlen(c)-1);
                        send_line(fd, c);
                    }
                }
                else if(!strncmp(input, "/react ", 7) || !strncmp(input, "/unreact ", 9)){
                    int un = input[1] == 'u';
                    char ref[16], mid[12], emo[24];
                    char *tail = field((char*)input + (un ? 9 : 7), ref, sizeof(ref));
                    field(tail, emo, sizeof(emo));
                    if(!proto_at_least(2,2))
                        sb_add("-- reactions need protocol 2.2", VGA_LIGHT_RED);
                    else if(!resolve_mid(ref, mid, sizeof(mid)))
                        sb_add("-- no such message; try /react 1 :+1:", VGA_LIGHT_RED);
                    else if(!emo[0])
                        sb_add("-- /react <n|id> <:emoji:>", VGA_LIGHT_RED);
                    else {
                        char c[64];
                        strcpy(c, un ? "UNREACT " : "REACT ");
                        strncat(c, mid, sizeof(c)-strlen(c)-1);
                        strncat(c, " ", sizeof(c)-strlen(c)-1);
                        strncat(c, emo, sizeof(c)-strlen(c)-1);
                        send_line(fd, c);
                    }
                }
                else if(!strncmp(input, "/email ", 7)){
                    if(!proto_at_least(2,2))
                        sb_add("-- this server has no email support", VGA_LIGHT_RED);
                    else {
                        char c[96]; strcpy(c, "SETEMAIL ");
                        strncat(c, input+7, sizeof(c)-strlen(c)-1);
                        send_line(fd, c);
                        g_needemail = 0;
                    }
                }
                else if(!strncmp(input, "/verify ", 8)){
                    char c[96]; strcpy(c, "VERIFY ");
                    strncat(c, input+8, sizeof(c)-strlen(c)-1); send_line(fd, c);
                }
                else if(!strcmp(input, "/resend"))   send_line(fd, "RESEND");
                else if(!strcmp(input, "/me"))       send_line(fd, "ME");
                else if(!strcmp(input, "/sessions")) send_line(fd, "SESSIONS");
                else if(!strcmp(input, "/ids")){
                    g_show_ids = !g_show_ids;
                    sb_add(g_show_ids ? "-- message ids shown"
                                      : "-- message ids hidden", VGA_DARK_GREY);
                }
                else if(!strcmp(input, "/proto")){
                    char m[CHAT_LINE];
                    strcpy(m, "-- protocol ");
                    m[12] = (char)('0' + g_pmaj); m[13] = '.';
                    m[14] = (char)('0' + g_pmin); m[15] = 0;
                    sb_add(m, VGA_DARK_GREY);
                }
                else if(input[0] == '/'){
                    sb_add("-- unknown command; /help lists them", VGA_DARK_GREY);
                }
                else {
                    if(!g_authed){ sb_add("-- not signed in yet", VGA_LIGHT_RED); }
                    else if(!g_dmwith[0] && !g_channel[0]){
                        sb_add("-- pick a channel in the sidebar (TAB)", VGA_LIGHT_RED);
                    } else {
                        char wire[CHAT_INPUT * 4];
                        expand_shortcodes(input, wire, sizeof(wire));
                        char out[CHAT_INPUT * 4 + 32];
                        if(g_dmwith[0]){
                            strcpy(out, "DM ");
                            strncat(out, g_dmwith, sizeof(out)-strlen(out)-1);
                            strncat(out, " ", sizeof(out)-strlen(out)-1);
                        } else {
                            strcpy(out, "SAY ");
                        }
                        strncat(out, wire, sizeof(out)-strlen(out)-1);
                        if(send_line(fd, out) < 0){
                            connected = 0;
                            sb_add("-- send failed", VGA_LIGHT_RED);
                            running = 0;
                        }
                    }
                }
                ilen = 0; input[0] = 0; g_view = 0;
                continue;
            }
            if(k >= 32 && k < 127 && ilen < CHAT_INPUT-1){
                input[ilen++] = (char)k; input[ilen] = 0;
                /* v0.7.19: tell the room we are typing, at most once every
                   two seconds and never for a slash command — announcing
                   "typing" while composing /quit would be a lie, and an
                   unthrottled indicator would send a packet per keystroke. */
                if(proto_at_least(2,2) && g_authed && input[0] != '/' &&
                   (g_dmwith[0] || g_channel[0]) &&
                   (uint32_t)(get_ticks() - g_typing_sent) > 200){
                    g_typing_sent = get_ticks();
                    char t[64];
                    if(g_dmwith[0]){
                        strcpy(t, "TYPING dm ");
                        strncat(t, g_dmwith, sizeof(t)-strlen(t)-1);
                    } else {
                        strcpy(t, "TYPING channel");
                    }
                    send_line(fd, t);
                }
            }
        }

        /* Redraw at ~20 Hz rather than per keystroke — a full frame is a
           screen of VGA writes and doing that per key makes typing feel
           heavy on TCG.  The pointer is lifted off the frame first and put
           back after, so it is never baked into the cells underneath it. */
        uint32_t now = get_ticks();
        if((uint32_t)(now - last_draw) >= 5){
            ui_erase_cursor();
            draw(host, input, connected);
            if(mouse_is_ready()) ui_draw_cursor();
            last_draw = now;
        }
        net_poll();
    }

    if(connected) send_line(fd, "QUIT");
    tcp_close(fd);

    ui_erase_cursor();
    vga_clear();
    vga_flush();
    vga_set_cursor(0, 0);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("  Left BOOT Chat.\n");
    notif_push("BOOT Chat session ended");
    toast_info("Disconnected from BOOT Chat");
}
