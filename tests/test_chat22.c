/* Compile the REAL shell/chat.c natively and feed it real BootChat wire lines.
 *
 * v0.7.19 teaches the HavenDOS client protocol 2.2/2.3. Before it, chat.c
 * never sent PROTO at all, so it was a 2.0 client: no message ids, no edits,
 * no replies, no reactions, no typing, no read state, no attachments, and no
 * signing in with an email address.
 *
 * The shapes below are not invented. They are what the server's own
 * fmt_msg() and fmt_dm() emit:
 *
 *   2.0   MSG   <srv> <chan> <user> <ts> <text>
 *   2.1+  MSG   <srv> <chan> c<id> <user> <ts> <edited> <reply|0> <text>
 *   2.0   DMMSG <from> <to> <ts> <text>
 *   2.1+  DMMSG d<id> <from> <to> <ts> <edited> <reply|0> <text>
 *
 * Getting one field position wrong here shows up as a message attributed to
 * the wrong person or a timestamp printed as the body, which is exactly the
 * class of bug that is invisible in a build log and obvious to a user. So the
 * parse is checked against the real strings rather than inferred from a
 * successful compile.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* The OS headers come first so every stub below matches the real prototype
   exactly -- vga takes vga_color_t, not int, and tcp_recv's timeout is a
   uint32_t. Guessing these produced eight conflicting-type errors. */
#include "../include/types.h"
#include "../include/vga.h"
#include "../include/tcp.h"
#include "../include/net.h"
#include "../include/dns.h"
#include "../include/toast.h"

/* ── fake VGA: the parse does not draw, but chat.c links against it ── */
static int g_fake_cols = 80, g_fake_rows = 25;
int  vga_cols(void){ return g_fake_cols; }
int  vga_rows(void){ return g_fake_rows; }
void vga_putchar_at(char c, int x, int y, vga_color_t fg, vga_color_t bg)
{ (void)c;(void)x;(void)y;(void)fg;(void)bg; }
void vga_puts_at(const char *s, int x, int y, vga_color_t fg, vga_color_t bg)
{ (void)s;(void)x;(void)y;(void)fg;(void)bg; }
void vga_puts(const char *s){ (void)s; }
void vga_putchar(char c){ (void)c; }
void vga_clear(void){}
void vga_set_color(vga_color_t fg, vga_color_t bg){ (void)fg;(void)bg; }
void vga_set_cursor(int x, int y){ (void)x;(void)y; }
void vga_fill_row(int y, char c, vga_color_t fg, vga_color_t bg)
{ (void)y;(void)c;(void)fg;(void)bg; }
void vga_begin_frame(void){}
void vga_end_frame(void){}
void vga_flush(void){}
void ui_draw_cursor(void){}
void ui_erase_cursor(void){}
void ui_force_cursor_redraw(void){}

/* ── fake network: nothing is sent, but what WOULD be sent is recorded,
      because "does the client mark a message read" is a real question ── */
#define SENTMAX 64
#define SENTLEN 256
static char g_sent[SENTMAX][SENTLEN];
static int  g_nsent;
static void sent_reset(void){ g_nsent = 0; }
static int sent_has(const char *needle)
{
    for(int i=0;i<g_nsent;i++) if(strstr(g_sent[i], needle)) return 1;
    return 0;
}
int tcp_send(int fd, const void *buf, uint16_t n)
{
    (void)fd;
    if(g_nsent < SENTMAX){
        int m = n < SENTLEN-1 ? n : SENTLEN-1;
        memcpy(g_sent[g_nsent], buf, (size_t)m);
        g_sent[g_nsent][m] = 0;
        for(int i=0;i<m;i++) if(g_sent[g_nsent][i]=='\n') g_sent[g_nsent][i]=0;
        g_nsent++;
    }
    return n;
}
int  tcp_connect(uint32_t ip, uint16_t port){ (void)ip;(void)port; return 1; }
int  tcp_recv(int fd, void *b, uint16_t n, uint32_t t)
{ (void)fd;(void)b;(void)n;(void)t; return 0; }
int  tcp_eof(int fd){ (void)fd; return 0; }
void tcp_close(int fd){ (void)fd; }
uint32_t dns_resolve(const char *h){ (void)h; return 0x0A000202u; }
int  net_ready(void){ return 1; }
void net_poll(void){}
/* v0.8.8.4: shell/chat.c's input waits run the periodic work now, so
   a person sitting on a login prompt no longer stops the clock, the
   DHCP retries and hotplug. There is none of that in a native suite,
   so this is the stub -- the same arrangement as net_poll() above. */
int  kernel_poll(void){ return 0; }

/* ── fake input / timing / notifications ── */
static uint32_t g_ticks;
uint32_t get_ticks(void){ return g_ticks; }
void sleep_ms(uint32_t ms){ (void)ms; }
int  keyboard_getchar(void){ return -1; }
int  mouse_is_ready(void){ return 0; }
void mouse_get(int *x,int *y,int *b){ (void)x;(void)y;(void)b; }
int  mouse_get_clicks(void){ return 0; }
void toast_info(const char *m){ (void)m; }
static int g_warned;
void toast_warning(const char *m){ (void)m; g_warned++; }
void notif_push(const char *m){ (void)m; }

#include "../shell/chat.c"

/* ── checks ───────────────────────────────────────────────────────── */
static int fails = 0, total = 0;
static void ck(const char *what, int cond, const char *detail)
{
    total++;
    if(cond) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s\n          %s\n", what,
                           detail ? detail : ""); }
}
static void head(const char *t){ printf("\n%s\n", t); }

/* Feed one server line through the parser, as the receive loop would. */
static void feed(const char *line)
{
    char buf[CHAT_WIRE];
    int i = 0;
    while(line[i] && i < CHAT_WIRE-1){ buf[i] = line[i]; i++; }
    buf[i] = 0;
    handle_line(1, buf);
}

/* Newest scrollback row. */
static const char *last_text(void)
{
    if(!g_count) return "";
    return g_scroll[(g_head - 1 + CHAT_SCROLL) % CHAT_SCROLL].text;
}
static const char *row_text(const char *mid)
{
    int s = sb_find_mid(mid);
    return s < 0 ? "" : g_scroll[s].text;
}

static void reset_view(const char *srv, const char *chan)
{
    sb_reset();
    g_nsrv = 0; g_ndm = 0; g_nitems = 0;
    g_dmwith[0] = 0;
    strncpy(g_server, srv, NAME_MAX-1);  g_server[NAME_MAX-1] = 0;
    strncpy(g_channel, chan, NAME_MAX-1); g_channel[NAME_MAX-1] = 0;
    srv_add(srv);
    items_rebuild();
    sent_reset();
}

int main(void)
{
    printf("HavenDOS BOOT Chat client: protocol 2.2/2.3 parse\n");
    printf("=================================================\n");

    strcpy(g_nick, "kyan");

    /* ── negotiation ─────────────────────────────────────────────── */
    head("protocol negotiation");
    g_pmaj = 2; g_pmin = 0;
    reset_view("haven", "general");
    feed("OK PROTO 2.3");
    ck("OK PROTO 2.3 is recorded", g_pmaj == 2 && g_pmin == 3, last_text());
    ck("proto_at_least(2,3) then holds", proto_at_least(2,3) != 0, "");

    g_pmaj = 2; g_pmin = 0;
    feed("OK PROTO 2.1");
    ck("a server that grants only 2.1 is honoured",
       g_pmaj == 2 && g_pmin == 1, "");
    ck("2.2 features are then off", !proto_at_least(2,2), "");

    /* ── the v2 bug this client had: case-folded account names ───── */
    head("the account name comes from the server, not from what was typed");
    strcpy(g_nick, "haven");            /* what the user typed */
    feed("OK LOGIN Haven");             /* what the table stores */
    ck("g_nick becomes the stored spelling",
       strcmp(g_nick, "Haven") == 0, g_nick);
    strcpy(g_nick, "kyan");

    /* ── 2.0 shapes still parse ──────────────────────────────────── */
    head("a 2.0 server is still understood");
    g_pmaj = 2; g_pmin = 0;
    reset_view("haven", "general");
    feed("MSG haven general mgkfn 1790000000 hello there");
    ck("2.0 MSG: speaker and body are right",
       strcmp(last_text(), "<mgkfn> hello there") == 0, last_text());

    g_dmwith[0] = 0;
    strncpy(g_dmwith, "mgkfn", NAME_MAX-1);
    g_channel[0] = 0;
    feed("DMMSG mgkfn kyan 1790000001 a private word");
    ck("2.0 DMMSG: from/to/ts consumed, body intact",
       strcmp(last_text(), "<mgkfn> a private word") == 0, last_text());

    /* ── 2.1+ shapes ─────────────────────────────────────────────── */
    head("2.1+ MSG carries an id, an edited flag and a reply id");
    g_pmaj = 2; g_pmin = 3;
    reset_view("haven", "general");
    feed("MSG haven general c41 mgkfn 1790000000 0 0 first message");
    ck("the three extra fields are consumed, not printed",
       strcmp(last_text(), "<mgkfn> first message") == 0, last_text());
    ck("the row is tagged with its wire id",
       sb_find_mid("c41") >= 0, "c41 not found");
    ck("viewing it marks it read for the other devices",
       sent_has("READ channel haven c41"), "no READ sent");

    feed("MSG haven general c42 mgkfn 1790000009 1 0 an edited one");
    ck("the edited flag shows as (edited)",
       strstr(last_text(), "(edited)") != NULL, last_text());

    feed("MSG haven general c43 kyan 1790000010 0 c41 answering that");
    ck("a reply quotes what it answers",
       strstr(last_text(), "|re ") != NULL, last_text());
    ck("...and still shows its own body",
       strstr(last_text(), "answering that") != NULL, last_text());

    head("2.1+ DMMSG puts the id FIRST, which is a different position");
    g_channel[0] = 0;
    strncpy(g_dmwith, "mgkfn", NAME_MAX-1); g_dmwith[NAME_MAX-1] = 0;
    sb_reset();
    feed("DMMSG d17 mgkfn kyan 1790000002 0 0 dm with an id");
    ck("the body is not shifted by the extra fields",
       strcmp(last_text(), "<mgkfn> dm with an id") == 0, last_text());
    ck("the dm row is tagged d17", sb_find_mid("d17") >= 0, "");

    /* ── edit and delete in place ────────────────────────────────── */
    head("MSGEDIT and MSGDEL find the row and rewrite it");
    reset_view("haven", "general");
    feed("MSG haven general c50 mgkfn 1790000020 0 0 orignal typo");
    feed("MSGEDIT c50 1790000030 original, fixed");
    ck("the speaker prefix survives the edit",
       strncmp(row_text("c50"), "<mgkfn> ", 8) == 0, row_text("c50"));
    ck("the new text replaced the old",
       strstr(row_text("c50"), "original, fixed") != NULL, row_text("c50"));
    ck("the old text is gone",
       strstr(row_text("c50"), "orignal typo") == NULL, row_text("c50"));
    ck("and it is marked edited",
       strstr(row_text("c50"), "(edited)") != NULL, row_text("c50"));

    feed("MSGDEL c50");
    ck("a deleted message says so rather than vanishing",
       strstr(row_text("c50"), "deleted") != NULL, row_text("c50"));

    feed("MSGEDIT c999 1790000031 nothing to edit");
    ck("an edit for a message not in the ring is ignored, not crashed",
       1, "");

    /* ── reactions ───────────────────────────────────────────────── */
    head("reactions render as a compact summary");
    reset_view("haven", "general");
    feed("MSG haven general c60 mgkfn 1790000040 0 0 react to me");
    feed("REACT c60 [{\"emoji\":\":+1:\",\"n\":2,\"who\":[\"kyan\",\"mgkfn\"]}]");
    {
        int s = sb_find_mid("c60");
        ck("a count above one shows the count",
           s >= 0 && strstr(g_scroll[s].react, ":+1:") != NULL
                  && strstr(g_scroll[s].react, "x2") != NULL,
           s >= 0 ? g_scroll[s].react : "(no row)");
    }
    feed("REACT c60 [{\"emoji\":\":fire:\",\"n\":1}]");
    {
        int s = sb_find_mid("c60");
        ck("a single reaction is left bare",
           s >= 0 && strstr(g_scroll[s].react, ":fire:") != NULL
                  && strstr(g_scroll[s].react, "x1") == NULL,
           s >= 0 ? g_scroll[s].react : "(no row)");
    }
    feed("REACTIONS {\"c60\":[{\"emoji\":\":heart:\",\"n\":3}]}");
    {
        int s = sb_find_mid("c60");
        ck("the backfill form reaches the right row",
           s >= 0 && strstr(g_scroll[s].react, ":heart:") != NULL,
           s >= 0 ? g_scroll[s].react : "(no row)");
    }

    /* ── attachments (2.3) ───────────────────────────────────────── */
    head("attachments become text, because a cell cannot hold a picture");
    reset_view("haven", "general");
    feed("MSG haven general c70 mgkfn 1790000050 0 0 have a look");
    feed("MSGMEDIA c70 [{\"id\":\"ab\",\"mime\":\"image/png\","
         "\"name\":\"shot.png\",\"bytes\":2800}]");
    ck("the file name appears on the row",
       strstr(row_text("c70"), "shot.png") != NULL, row_text("c70"));
    ck("so does the kind, without the subtype",
       strstr(row_text("c70"), "image:") != NULL, row_text("c70"));
    ck("and a readable size",
       strstr(row_text("c70"), "2K") != NULL, row_text("c70"));
    ck("the original caption is still there",
       strstr(row_text("c70"), "have a look") != NULL, row_text("c70"));

    feed("MSG haven general c71 mgkfn 1790000051 0 0 small one");
    feed("MSGMEDIA c71 [{\"id\":\"cd\",\"mime\":\"text/plain\","
         "\"name\":\"note.txt\",\"bytes\":40}]");
    ck("a sub-kilobyte size is shown in bytes",
       strstr(row_text("c71"), "40B") != NULL, row_text("c71"));

    feed("ATTACHMENTS {\"c70\":[{\"id\":\"ef\",\"mime\":\"audio/mpeg\","
         "\"name\":\"clip.mp3\",\"bytes\":9000}]}");
    ck("the backfill form also lands on the right row",
       strstr(row_text("c70"), "clip.mp3") != NULL, row_text("c70"));

    /* ── typing ──────────────────────────────────────────────────── */
    head("typing indicator");
    reset_view("haven", "general");
    g_ticks = 1000;
    feed("TYPING channel haven general mgkfn");
    ck("someone typing here is recorded",
       strcmp(g_typing, "mgkfn") == 0, g_typing);
    feed("TYPINGOFF channel haven general mgkfn");
    ck("and cleared when they stop", g_typing[0] == 0, g_typing);

    feed("TYPING channel haven general mgkfn");
    feed("TYPING channel other general someone");
    ck("typing in a channel we are not viewing is ignored",
       strcmp(g_typing, "mgkfn") == 0, g_typing);

    g_typing[0] = 0;
    feed("TYPING channel haven general kyan");
    ck("our own typing is never shown back to us", g_typing[0] == 0, g_typing);

    g_typing[0] = 0;
    feed("TYPING channel haven general mgkfn");
    feed("MSG haven general c80 mgkfn 1790000060 0 0 done typing");
    ck("sending a message ends that person's indicator at once",
       g_typing[0] == 0, g_typing);

    /* ── read state mirrored from another device ─────────────────── */
    head("read state arriving from our own other devices");
    reset_view("haven", "general");
    /* Build the sidebar the way the server does, so the channel being marked
       unread is one that actually exists in the tree. */
    feed("CHANLIST haven general random");
    mark_unread(SB_CHAN, "haven", "random");
    {
        int any = 0;
        for(int i=0;i<g_nitems;i++) if(g_items[i].unread) any = 1;
        ck("a channel can be marked unread", any, "");
    }
    feed("READ channel haven c99");
    {
        int any = 0;
        for(int i=0;i<g_nitems;i++)
            if(g_items[i].kind == SB_CHAN && g_items[i].unread) any = 1;
        ck("a mirrored READ clears the badge", !any, "");
    }

    /* ── message references ──────────────────────────────────────── */
    head("referring to a message by ordinal or by id");
    reset_view("haven", "general");
    feed("MSG haven general c101 mgkfn 1790000070 0 0 oldest");
    feed("MSG haven general c102 mgkfn 1790000071 0 0 middle");
    feed("MSG haven general c103 mgkfn 1790000072 0 0 newest");
    {
        char mid[12];
        ck("1 is the newest message",
           resolve_mid("1", mid, sizeof(mid)) && !strcmp(mid, "c103"), mid);
        ck("3 counts back past the others",
           resolve_mid("3", mid, sizeof(mid)) && !strcmp(mid, "c101"), mid);
        ck("a raw id passes through",
           resolve_mid("c102", mid, sizeof(mid)) && !strcmp(mid, "c102"), mid);
        ck("an ordinal past the end fails rather than guessing",
           !resolve_mid("99", mid, sizeof(mid)), mid);
        ck("nonsense fails", !resolve_mid("zz", mid, sizeof(mid)), mid);
    }

    /* ── a local notice must not inherit a recycled row's id ─────── */
    head("the scrollback ring does not leak message ids");
    reset_view("haven", "general");
    feed("MSG haven general c200 mgkfn 1790000080 0 0 real message");
    sb_add("-- a local notice", VGA_DARK_GREY);
    {
        int slot = (g_head - 1 + CHAT_SCROLL) % CHAT_SCROLL;
        ck("a notice added after a message carries no id",
           g_scroll[slot].mid[0] == 0, g_scroll[slot].mid);
    }
    feed("MSGEDIT c200 1790000081 edited the real one");
    ck("an edit still finds the real message, not the notice",
       strstr(row_text("c200"), "edited the real one") != NULL,
       row_text("c200"));

    /* ── JSON scalars ────────────────────────────────────────────── */
    head("the small JSON readers");
    {
        char out[32];
        ck("a string value is read",
           json_str("{\"user\":\"Kyan\",\"status\":\"online\"}", "user",
                    out, sizeof(out)) && !strcmp(out, "Kyan"), out);
        ck("a later key is found too",
           json_str("{\"user\":\"Kyan\",\"status\":\"online\"}", "status",
                    out, sizeof(out)) && !strcmp(out, "online"), out);
        ck("a missing key fails rather than returning rubbish",
           !json_str("{\"user\":\"Kyan\"}", "bio", out, sizeof(out)), out);
        ck("an integer value is read",
           json_int("{\"n\":42}", "n") == 42, "");
        ck("a missing integer is -1",
           json_int("{\"n\":42}", "m") == -1, "");
        ck("a long value is truncated, not overflowed",
           json_str("{\"user\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
                    "user", out, 8) && strlen(out) == 7, out);
    }

    /* ── presence and the administrative objects ─────────────────── */
    head("2.2 objects are summarised, never dumped as raw JSON");
    reset_view("haven", "general");
    feed("PRESENCE {\"user\":\"mgkfn\",\"status\":\"online\"}");
    ck("presence reads as a sentence",
       strstr(last_text(), "mgkfn") && strstr(last_text(), "online")
       && !strstr(last_text(), "{"), last_text());

    feed("SELF {\"id\":3,\"name\":\"Kyan\"}");
    ck("SELF sets the account name and prints nothing",
       strcmp(g_nick, "Kyan") == 0, g_nick);
    strcpy(g_nick, "kyan");

    feed("SESSIONLIST {\"sessions\":[{\"id\":1,\"device\":\"Desktop\"}]}");
    ck("a session list is acknowledged, not printed as JSON",
       strstr(last_text(), "{") == NULL, last_text());

    feed("ROLES {\"roles\":[{\"name\":\"Owner\"}]}");
    ck("so is a role list", strstr(last_text(), "{") == NULL, last_text());

    /* ── moderation notices ──────────────────────────────────────── */
    head("moderation notices reach the user");
    reset_view("haven", "general");
    g_warned = 0;
    feed("MODNOTE timeout haven 600 being rude");
    ck("the action, server and reason are shown",
       strstr(last_text(), "timeout") && strstr(last_text(), "haven")
       && strstr(last_text(), "being rude"), last_text());
    ck("it also raises a toast, because it may arrive off-screen",
       g_warned == 1, "");

    reset_view("haven", "general");
    feed("CHANGONE haven general");
    ck("a removed channel is announced",
       strstr(last_text(), "removed") != NULL, last_text());
    ck("and we are no longer in it", g_channel[0] == 0, g_channel);

    feed("NEEDEMAIL 1");
    ck("the email requirement is surfaced with the command to fix it",
       g_needemail == 1, "");

    /* ── unknown verbs ───────────────────────────────────────────── */
    head("an unknown verb is shown, not dropped");
    reset_view("haven", "general");
    feed("WHATEVER something new");
    ck("it appears in the pane",
       strstr(last_text(), "WHATEVER") != NULL, last_text());

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
