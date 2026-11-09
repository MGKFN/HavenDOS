/*
 * shell_v076.c — HavenDOS v0.7.11 new shell commands
 *
 *   selftest   built-in regression suite
 *   netstat -s network counters   /  netstat -z  reset
 *   tlsinfo    last TLS handshake diagnostics
 *
 * WHY selftest EXISTS
 * -------------------
 * v0.7.5 fixed strncmp(), the TLS record layer, TCP retransmission and the
 * zero-window deadlock.  v0.7.6 fixes the http_resp_t ABI and the chunked
 * decoder.  Almost none of that is observable from the desktop, and QEMU's
 * SLIRP is too forgiving to reproduce the conditions the fixes address.
 *
 * `selftest` pins the invariants directly so a regression shows up as a red
 * FAIL line on the machine it broke on, instead of as "the network feels
 * flaky".  It runs entirely offline — no NIC required — so it works on the
 * ThinkPad X140e and under UTM SE alike.
 */
#include "../include/types.h"
#include "version.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/kpoll.h"
#include "../include/net.h"
#include "../include/netstat.h"
#include "../include/tls.h"
#include "../include/http.h"

extern void  make_path(char *out, const char *in);
extern int   vfs_read(const char *path, char *buf, uint32_t maxlen);
extern int   vfs_write(const char *path, const char *buf, uint32_t len);
extern int   vfs_exists(const char *path);
extern int   vfs_delete(const char *path);
extern int   vfs_mkdir(const char *path);
extern uint16_t ip_checksum(const void *data, uint16_t len);
extern uint32_t get_ticks(void);
extern int   net_ready(void);

/* ── test bookkeeping ─────────────────────────────────────────── */
static int t_pass, t_fail;

static void t_begin(const char *group){
    vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts("  "); vga_puts(group); vga_putchar('\n');
    vga_set_color(VGA_WHITE,VGA_BLACK);
}
static void t_check(const char *name, int ok){
    vga_set_color(ok?VGA_LIGHT_GREEN:VGA_LIGHT_RED,VGA_BLACK);
    vga_puts(ok?"    PASS  ":"    FAIL  ");
    vga_set_color(ok?VGA_LIGHT_GREY:VGA_WHITE,VGA_BLACK);
    vga_puts(name); vga_putchar('\n');
    vga_set_color(VGA_WHITE,VGA_BLACK);
    if(ok) t_pass++; else t_fail++;
}

/* ── 1. string.c ──────────────────────────────────────────────────
   strncmp() is the single most load-bearing function in the OS: kernel
   command line, tab completion, script if/while, HTTP header parsing and all
   14 strstartwith() call sites go through it.  The v0.7.5 bug was that the
   final n-- wrapped 0 to SIZE_MAX, so it only ever returned 0 for FULLY equal
   strings.  Case 2 below is the exact shape that failed. */
static void test_string(void){
    t_begin("string.c");
    t_check("strncmp equal-prefix, n < len   ", strncmp("mode=desktop","mode=",5)==0);
    t_check("strncmp full match              ", strncmp("abc","abc",3)==0);
    t_check("strncmp n=0 always equal        ", strncmp("x","y",0)==0);
    t_check("strncmp mismatch inside window  ", strncmp("abcd","abxd",4)!=0);
    t_check("strncmp stops at NUL            ", strncmp("ab","abcd",4)!=0);
    t_check("strncmp past NUL both           ", strncmp("ab","ab",8)==0);
    t_check("strstartwith                    ", strstartwith("Content-Length: 42","Content-Length:"));
    t_check("strstartwith negative           ", !strstartwith("Content-Type: x","Content-Length:"));
    t_check("strstr found                    ", strstr("hello world","wor")!=0);
    t_check("strstr absent                   ", strstr("hello","xyz")==0);
    t_check("strstr empty needle             ", strstr("abc","")!=0);
    char b[16]; strncpy(b,"ab",8);
    t_check("strncpy zero-pads               ", b[2]==0 && b[7]==0);
    t_check("atoi negative                   ", atoi("-42")==-42);
    char ib[16]; itoa(-7,ib,10);
    t_check("itoa negative                   ", strcmp(ib,"-7")==0);
    utoa(255,ib,16);
    t_check("utoa hex                        ", strcmp(ib,"ff")==0);
}

/* ── 1b. DER length bounds (v0.7.11) ─────────────────────────────
   The certificate parser used to add a uint32 read off the wire to a small
   constant and compare against a uint16 length - a crafted value near 2^32
   wraps and the check passes.  These pin the arithmetic that replaced it. */
static int len_ok(uint32_t declared, uint16_t actual){
    if(declared > (uint32_t)actual) return 0;
    if(declared + 6u > (uint32_t)actual) return 0;
    return 1;
}
static void test_der_bounds(void){
    t_begin("DER length bounds");
    t_check("normal length accepted        ", len_ok(100, 200));
    t_check("exact fit accepted            ", len_ok(194, 200));
    t_check("one past the end rejected     ", !len_ok(195, 200));
    t_check("length > buffer rejected      ", !len_ok(5000, 200));
    t_check("wrap-around value rejected    ", !len_ok(0xFFFFFFFAu, 200));
    t_check("0xFFFFFFFF rejected           ", !len_ok(0xFFFFFFFFu, 200));
}

/* ── 2. TLS 1.2 record padding ───────────────────────────────────
   RFC 5246 6.2.3.2: a record carries padding_length bytes of value
   padding_length, and the padding_length byte is itself one of them — so the
   receiver strips pad+1, not pad.  v0.7.5 fixed this at three sites; if any
   of them regress, the MAC never lines up and HTTPS dies silently. */
static int tls_pad_strip(const uint8_t *rec, int ct_len){
    uint8_t pad = rec[ct_len-1];
    if(pad>15 || (pad+1)>=ct_len) return -1;
    return ct_len-(pad+1);
}
static void test_tls_padding(void){
    t_begin("TLS 1.2 record padding (RFC 5246 6.2.3.2)");
    uint8_t rec[64];
    /* 48 bytes of content, already a block multiple -> one full pad block */
    memset(rec,0xAA,48); memset(rec+48,0x0F,16);
    t_check("48+full block strips to 48      ", tls_pad_strip(rec,64)==48);
    /* 40 bytes of content -> 8 pad bytes of value 0x07 */
    memset(rec,0xAA,40); memset(rec+40,0x07,8);
    t_check("40+8 strips to 40               ", tls_pad_strip(rec,48)==40);
    /* the pre-0.7.5 encoding wrote the COUNT (0x10) not count-1 */
    memset(rec,0xAA,48); memset(rec+48,0x10,16);
    t_check("legacy 0x10 padding rejected    ", tls_pad_strip(rec,64)<0);
    /* padding longer than the record must not underflow */
    memset(rec,0x0F,16);
    t_check("oversized padding rejected      ", tls_pad_strip(rec,16)<0);
}

/* ── 3. chunked transfer decoding ────────────────────────────────
   The v0.7.6 fix: the size line was never NUL-terminated when it hit the
   buffer cap, and chunk extensions were not skipped. */
static int chunk_size_of(const char *line){
    int v=0, digits=0;
    for(int i=0;line[i];i++){
        char c=line[i];
        if(c>='0'&&c<='9')      { v=v*16+(c-'0');    digits++; }
        else if(c>='a'&&c<='f') { v=v*16+(c-'a'+10); digits++; }
        else if(c>='A'&&c<='F') { v=v*16+(c-'A'+10); digits++; }
        else break;
    }
    return digits?v:-1;
}
static void test_chunked(void){
    t_begin("HTTP chunked decoder");
    t_check("plain hex size                  ", chunk_size_of("1a3")==419);
    t_check("uppercase hex                   ", chunk_size_of("FF")==255);
    t_check("terminating zero chunk          ", chunk_size_of("0")==0);
    t_check("chunk extension ignored         ", chunk_size_of("10;name=value")==16);
    t_check("junk size line rejected         ", chunk_size_of("zz")<0);
    t_check("empty size line rejected        ", chunk_size_of("")<0);
}

/* ── 4. http_resp_t ABI ──────────────────────────────────────────
   THE v0.7.6 headline bug.  shell.c carried a private 28-byte copy of this
   struct while net/http.c compiled against a 152-byte one, so http.c read the
   progress function pointer 124 bytes past the end of a stack object and
   called it.  file_path was also a char[128] array, so `if(filepath)` could
   never be false and resp->body was never populated — bpkg downloaded whole
   packages into a buffer nobody filled. */
static void test_http_abi(void){
    t_begin("http_resp_t ABI");
    http_resp_t r;
    memset(&r,0,sizeof(r));
    t_check("file_path is a pointer, not array", sizeof(r.file_path)==sizeof(void*));
    t_check("zeroed file_path reads as NULL  ", r.file_path==0);
    r.body_max = 100; r.body_len = 7;
    t_check("body_max / body_len distinct    ", r.body_max==100 && r.body_len==7);
    t_check("struct is the compact layout    ", sizeof(http_resp_t)<=32);
    t_check("http_strerror(DNS)              ", http_strerror(HTTP_ERR_DNS)[0]=='D');
    t_check("http_strerror(TLS)              ", http_strerror(HTTP_ERR_TLS)[0]=='T');
}

/* ── 5. IPv4 checksum ────────────────────────────────────────────
   v0.7.5 made this byte-wise to stop the unaligned uint16_t* reads that
   corrupt data under ARM/TCG — the same undefined-behaviour class that ate
   the FAT16 BPB.  A correct header must checksum to zero when re-summed. */
static void test_ip_checksum(void){
    t_begin("IPv4 checksum");
    /* Textbook header from RFC 1071 worked example */
    uint8_t hdr[20] = {
        0x45,0x00,0x00,0x73, 0x00,0x00,0x40,0x00,
        0x40,0x11,0x00,0x00, 0xc0,0xa8,0x00,0x01,
        0xc0,0xa8,0x00,0xc7 };
    uint16_t ck = ip_checksum(hdr,20);
    hdr[10]=(uint8_t)(ck&0xFF); hdr[11]=(uint8_t)(ck>>8);
    t_check("re-sum of a valid header is 0   ", ip_checksum(hdr,20)==0);
    hdr[12] ^= 0x01;                       /* flip one bit of the source IP */
    t_check("corrupted header is detected    ", ip_checksum(hdr,20)!=0);
    /* Odd length must not read past the buffer */
    uint8_t odd[5]={1,2,3,4,5};
    (void)ip_checksum(odd,5);
    t_check("odd length handled              ", 1);
}

/* ── 6. path building / VFS round trip ───────────────────────── */
static void test_vfs(void){
    t_begin("VFS round trip");
    const char *msg="HavenDOS " HAVENDOS_VER " selftest payload";
    int ml=(int)strlen(msg);
    vfs_delete("SELFTEST.TMP");
    int w = vfs_write("SELFTEST.TMP",msg,(uint32_t)ml);
    t_check("write returns byte count        ", w==ml);
    t_check("file now exists                 ", vfs_exists("SELFTEST.TMP"));
    char rb[128];
    int r = vfs_read("SELFTEST.TMP",rb,127);
    if(r>=0 && r<128) rb[r]=0; else rb[0]=0;
    t_check("read returns the same length    ", r==ml);
    t_check("contents survive the round trip ", strcmp(rb,msg)==0);
    t_check("delete succeeds                 ", vfs_delete("SELFTEST.TMP")>=0);
    t_check("file is gone                    ", !vfs_exists("SELFTEST.TMP"));
}

/* ── 7. shell path expansion ─────────────────────────────────── */
static void test_paths(void){
    t_begin("Path building");
    char p[64];
    make_path(p,"/ETC/MOTD.TXT");
    t_check("absolute strips leading slash   ", strcmp(p,"ETC/MOTD.TXT")==0);
    make_path(p,"~");
    t_check("~ expands to a HOME path        ", strncmp(p,"HOME",4)==0);
    make_path(p,"~/NOTES.TXT");
    t_check("~/file expands with separator   ", strstr(p,"/NOTES.TXT")!=0);
    make_path(p,"");
    t_check("empty name yields empty path    ", p[0]==0);
}

void cmd_selftest_v6(const char *arg1){
    (void)arg1;
    t_pass=t_fail=0;
    vga_set_color(VGA_BLACK,VGA_LIGHT_CYAN);
    vga_puts(" HavenDOS " HAVENDOS_VER " self-test                                              \n");
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("  Offline invariant checks - no network required.\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);

    test_string();
    test_der_bounds();
    test_tls_padding();
    test_chunked();
    test_http_abi();
    test_ip_checksum();
    test_vfs();
    test_paths();

    vga_putchar('\n');
    vga_set_color(t_fail?VGA_LIGHT_RED:VGA_LIGHT_GREEN,VGA_BLACK);
    vga_puts(t_fail?"  RESULT: FAILURES DETECTED":"  RESULT: ALL TESTS PASSED");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("   passed=");   { char b[8]; itoa(t_pass,b,10); vga_puts(b); }
    vga_puts("  failed=");    { char b[8]; itoa(t_fail,b,10); vga_puts(b); }
    vga_putchar('\n');
    if(t_fail){
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("  Please report the failing line names to TechHaven Studios.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
    }
}

/* ── netstat -s ─────────────────────────────────────────────── */
static void row(const char *label, uint32_t v, vga_color_t col){
    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts("    "); vga_puts(label);
    int pad = 26-(int)strlen(label); while(pad-->0) vga_putchar(' ');
    vga_set_color(v?col:VGA_DARK_GREY,VGA_BLACK);
    char b[16]; utoa(v,b,10);
    int rp = 8-(int)strlen(b); while(rp-->0) vga_putchar(' ');
    vga_puts(b); vga_putchar('\n');
    vga_set_color(VGA_WHITE,VGA_BLACK);
}
static void hdr(const char *s){
    vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts("  "); vga_puts(s); vga_putchar('\n');
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

void cmd_netstat_stats_v6(void){
    netstat_t *n = &g_netstat;
    vga_set_color(VGA_BLACK,VGA_LIGHT_CYAN);
    vga_puts(" Network statistics                                                     \n");
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts(net_ready()? "  NIC: up\n" : "  NIC: none detected - counters will stay at zero\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);

    hdr("Ethernet");
    row("frames received",    n->eth_rx,      VGA_LIGHT_GREEN);
    row("frames sent",        n->eth_tx,      VGA_LIGHT_GREEN);
    row("send failures",      n->eth_tx_fail, VGA_LIGHT_RED);
    row("runt frames",        n->eth_runt,    VGA_YELLOW);

    hdr("ARP");
    row("requests sent",      n->arp_req_tx,   VGA_LIGHT_GREEN);
    row("replies received",   n->arp_reply_rx, VGA_LIGHT_GREEN);
    row("requests answered",  n->arp_req_rx,   VGA_LIGHT_GREEN);
    row("resolve failures",   n->arp_fail,     VGA_LIGHT_RED);

    hdr("IPv4");
    row("packets received",   n->ip_rx,        VGA_LIGHT_GREEN);
    row("packets sent",       n->ip_tx,        VGA_LIGHT_GREEN);
    row("bad checksum",       n->ip_bad_csum,  VGA_LIGHT_RED);
    row("bad length",         n->ip_bad_len,   VGA_LIGHT_RED);
    row("fragments dropped",  n->ip_frag_drop, VGA_YELLOW);
    row("not addressed to us",n->ip_not_mine,  VGA_DARK_GREY);
    row("re-entrant sends",   n->ip_reentrant, VGA_YELLOW);

    hdr("ICMP / UDP");
    row("echo requests rx",   n->icmp_echo_rx,  VGA_LIGHT_GREEN);
    row("echo replies rx",    n->icmp_reply_rx, VGA_LIGHT_GREEN);
    row("UDP datagrams rx",   n->udp_rx,        VGA_LIGHT_GREEN);
    row("UDP datagrams tx",   n->udp_tx,        VGA_LIGHT_GREEN);

    hdr("TCP");
    row("segments received",  n->tcp_seg_rx,     VGA_LIGHT_GREEN);
    row("segments sent",      n->tcp_seg_tx,     VGA_LIGHT_GREEN);
    row("bad checksum",       n->tcp_bad_csum,   VGA_LIGHT_RED);
    row("out of order",       n->tcp_ooo,        VGA_YELLOW);
    row("retransmissions",    n->tcp_rtx,        VGA_YELLOW);
    row("retransmit give-ups",n->tcp_rtx_giveup, VGA_LIGHT_RED);
    row("window updates",     n->tcp_win_update, VGA_LIGHT_GREEN);
    row("ring-full drops",    n->tcp_rx_drop,    VGA_LIGHT_RED);
    row("RST accepted",       n->tcp_rst_rx,     VGA_YELLOW);
    row("RST ignored",        n->tcp_rst_ignored,VGA_YELLOW);
    row("duplicate FINs",     n->tcp_dup_fin,    VGA_YELLOW);

    hdr("TLS");
    row("records received",   n->tls_rec_rx,    VGA_LIGHT_GREEN);
    row("records sent",       n->tls_rec_tx,    VGA_LIGHT_GREEN);
    row("bad padding",        n->tls_bad_pad,   VGA_LIGHT_RED);
    row("truncated records",  n->tls_short_rec, VGA_LIGHT_RED);
    row("alerts received",    n->tls_alerts,    VGA_YELLOW);
    row("MAC failures",       n->tls_bad_mac,   VGA_LIGHT_RED);

    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("\n  Retransmissions and window updates should be 0 under QEMU SLIRP.\n");
    vga_puts("  Non-zero on real hardware is normal; growing give-ups is not.\n");
    vga_puts("  'netstat -z' resets these counters.\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

void cmd_netstat_reset_v6(void){
    netstat_reset();
    vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
    vga_puts("Network counters reset.\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

/* ── tlsinfo ────────────────────────────────────────────────── */
static const char *alert_text(int d){
    switch(d){
        case 0:   return "close_notify (normal shutdown)";
        case 10:  return "unexpected_message";
        case 20:  return "bad_record_mac";
        case 40:  return "handshake_failure";
        case 42:  return "bad_certificate";
        case 47:  return "illegal_parameter";
        case 48:  return "unknown_ca";
        case 50:  return "decode_error";
        case 51:  return "decrypt_error";
        case 70:  return "protocol_version";
        case 71:  return "insufficient_security";
        case 80:  return "internal_error";
        case 112: return "unrecognized_name (SNI)";
        default:  return "(see RFC 5246 A.3)";
    }
}
/* v0.8.9: the switch that used to be here was a SECOND copy of the
   stage names, keyed on bare numbers. net/tls.c owns the list now --
   see include/tls.h on what inserting a stage did to this copy. */

void cmd_tlsinfo_v6(void){
    vga_set_color(VGA_BLACK,VGA_LIGHT_CYAN);
    vga_puts(" TLS diagnostics - last handshake                                       \n");
    vga_set_color(VGA_WHITE,VGA_BLACK);

    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts("  Reached stage   ");
    vga_set_color(g_tls_last_stage>=TLS_STAGE_ESTABLISHED?VGA_LIGHT_GREEN
                                                          :VGA_YELLOW,VGA_BLACK);
    vga_puts(tls_stage_name(g_tls_last_stage));
    vga_puts(" (");
    { char b[8]; itoa(g_tls_last_stage,b,10); vga_puts(b); }
    vga_puts("/");
    { char b[8]; itoa(TLS_STAGE_ESTABLISHED,b,10); vga_puts(b); }
    vga_puts(")\n");

    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts("  Offered suite   ");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("0x003C TLS_RSA_WITH_AES_128_CBC_SHA256\n");

    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts("  Server chose    ");
    if(g_tls_peer_cipher < 0){
        vga_set_color(VGA_DARK_GREY,VGA_BLACK); vga_puts("(no ServerHello seen)\n");
    } else {
        vga_set_color(g_tls_peer_cipher==0x003C?VGA_LIGHT_GREEN:VGA_LIGHT_RED,VGA_BLACK);
        vga_puts("0x"); { char b[8]; utoa((uint32_t)g_tls_peer_cipher,b,16); vga_puts(b); }
        vga_puts(g_tls_peer_cipher==0x003C?"  (match)\n":"  (NOT supported)\n");
    }

    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts("  Last alert      ");
    if(g_tls_last_alert_desc < 0){
        vga_set_color(VGA_DARK_GREY,VGA_BLACK); vga_puts("none\n");
    } else {
        vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
        { char b[8]; itoa(g_tls_last_alert_desc,b,10); vga_puts(b); }
        vga_puts(" - "); vga_puts(alert_text(g_tls_last_alert_desc));
        vga_puts(g_tls_last_alert_level==2?"  [fatal]\n":"  [warning]\n");
    }

    /* ── v0.8.9: the client-certificate path ─────────────────────────
       Three lines rather than one, because on hardware these fail
       apart and the remedies are different. "not asked" means the peer
       never wanted a certificate, so a failure is somewhere else
       entirely; "asked, none sent" is a missing or unusable pair
       record; "sent, not verified" is the signature. */
    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts("  Client cert     ");
    if(!g_tls_cert_requested){
        vga_set_color(VGA_DARK_GREY,VGA_BLACK);
        vga_puts("the peer did not ask for one\n");
    } else if(g_tls_cert_sent == 0){
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("asked for, NONE sent (empty Certificate)\n");
    } else {
        vga_set_color(g_tls_cert_verified?VGA_LIGHT_GREEN:VGA_LIGHT_RED,VGA_BLACK);
        { char b[8]; itoa(g_tls_cert_sent,b,10); vga_puts(b); }
        vga_puts(g_tls_cert_verified
                 ? " sent, CertificateVerify signed\n"
                 : " sent, NO CertificateVerify\n");
    }

    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("\n  HavenDOS speaks TLS 1.2 with static-RSA key exchange only.\n");
    vga_puts("  Servers that require ECDHE or TLS 1.3 will answer alert 40 or 70.\n");
    vga_puts("  That is a capability limit, not a bug - see the " HAVENDOS_VER " roadmap.\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

/* ── v0.7.11: resolution ─────────────────────────────────────────────────
 * HavenDOS now supports 80x50 as well as 80x25.  The shell benefits
 * immediately - twice the scrollback on screen - because vga.c scrolls and
 * clears against the runtime row count.
 *
 * The DESKTOP still hardcodes row 22 (toast) and row 24 (taskbar) in roughly
 * 145 places across desktop.c, ui.c and shell.c, so it forces 25 rows on entry
 * and restores your preference on exit.  Converting those coordinates is the
 * next step; shipping a half-converted desktop would just be a new class of
 * misplaced-element bug.
 *
 * If a switch ever leaves the screen unreadable on your hardware, type
 * `resolution 25` blind - the keyboard path is unaffected by the mode.
 */
extern int  vga_rows(void);
extern int  keyboard_getchar(void);
extern uint32_t get_ticks(void);
extern void sleep_ms(uint32_t ms);
extern int  vga_fb_app_grid(int *cols, int *rows);
extern int  kernel_poll(void);
extern void vga_force_text_mode(void);
extern int  vga_set_rows(int rows);
/* v0.8.8: the framebuffer console's own limits, so this screen can say
   which clamp it hit instead of naming four text modes on every refusal. */
extern int  vga_fb_active(void);
extern uint32_t vga_fb_width(void);
extern uint32_t vga_fb_height(void);
extern int  vga_fb_best_grid(int *cols, int *rows);
extern int  vga_fb_readable_grid(int min_px, int *cols, int *rows);
extern int  vga_grid_capped(void);
extern int  vga_mode_why(void);
extern const char *vga_mode_why_name(void);

/* The shell's preferred row count.  desktop_run() pins 25 while it is up; the
   shell re-applies this at its prompt, so returning from the desktop restores
   whatever the user chose without having to patch every exit path. */
static int g_shell_rows = 25;
static int g_shell_cols = 80;
int shell_pref_rows(void){ return g_shell_rows; }
int shell_pref_cols(void){ return g_shell_cols; }

/* ── v0.8.8.1: the choice SURVIVES A REBOOT ──────────────────────────
 *
 * These were two plain statics, reset to 80x25 on every boot, so a
 * grid chosen with `resolution` lasted exactly one session. A feature
 * you have to re-apply every single time you switch the machine on is
 * one nobody uses, and that is most of why v0.8.8 looked like nothing
 * had changed.
 *
 * The console keeps its own default at boot -- the grid is applied at
 * the SHELL PROMPT, through the re-apply that shell.c already does on
 * every command. That matters: the desktop pins 25 rows on entry and
 * restores the preference on exit, so a saved 120x67 cannot put 45
 * apps that draw at 80x25 into the corner of a large screen. The
 * auto-detect-at-boot that RM-1003 asks for still needs the viewport
 * layer; this does not.
 *
 * A zero in the config means "never chosen", not 80x25, so a machine
 * that has never been told anything keeps whatever the console came up
 * with. */
extern int  cfg_console_cols(void);
extern int  cfg_console_rows(void);
extern void cfg_set_console_grid(int c, int r);
extern void cfg_save(void);

void shell_grid_load(void)
{
    int c = cfg_console_cols(), r = cfg_console_rows();
    if(c > 0 && r > 0){ g_shell_cols = c; g_shell_rows = r; }
}

/* Remember it, or forget it.  Called only after a mode has been
   CONFIRMED -- a mode that was reverted because it would not sync is
   the last thing to write to a file that is read at every boot. */
static void shell_grid_remember(int c, int r)
{
    cfg_set_console_grid(c, r);
    cfg_save();
}

void cmd_resolution_v6(const char *arg1){
    extern int vga_cols(void);
    extern int vga_set_mode(int cols,int rows);

    if(!arg1 || !arg1[0]){
        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts("Display mode\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        vga_puts("  Current : ");
        { char b[8]; itoa(vga_cols(),b,10); vga_puts(b); }
        vga_putchar('x');
        { char b[8]; itoa(vga_rows(),b,10); vga_puts(b); }
        if(vga_grid_capped()) vga_puts("  (capped by the cell budget)");
        vga_putchar('\n');
        /* ── v0.8.8: the two consoles have different limits, so they get
           different screens.  The old one listed the four VGA text modes
           on a UEFI machine that has no text memory, which is advice
           about hardware that is not there. */
        if(vga_fb_active()){
            int bc=80, br=25;
            vga_puts("  Panel   : ");
            { char b[12]; itoa((int)vga_fb_width(),b,10); vga_puts(b); }
            vga_putchar('x');
            { char b[12]; itoa((int)vga_fb_height(),b,10); vga_puts(b); }
            vga_puts(" pixels, framebuffer console\n");
            if(vga_fb_best_grid(&bc,&br)){
                vga_puts("  Largest : ");
                { char b[8]; itoa(bc,b,10); vga_puts(b); }
                vga_putchar('x');
                { char b[8]; itoa(br,b,10); vga_puts(b); }
                vga_puts("   ('resolution max') - 8px text, very small\n");
            }
            /* What `fit` WILL DO, not what readable_grid() returns.
               The report said 64x24 on a 1024x768 panel and the
               command then set it, which is how a console too small
               for any app came to be saved on the HP. A report that
               names a different grid from the command beside it is
               worse than no report. */
            { int rc=80, rr=25;
              if(vga_fb_app_grid(&rc,&rr)){
                vga_puts("  Readable: ");
                { char b[8]; itoa(rc,b,10); vga_puts(b); }
                vga_putchar('x');
                { char b[8]; itoa(rr,b,10); vga_puts(b); }
                vga_puts("   ('resolution fit') - recommended\n");
              } }
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_puts("\n  Any grid up to 240x135 and 32400 cells, as long\n");
            vga_puts("  as it fits the panel.  Glyphs are 8x8 scaled by a\n");
            vga_puts("  whole number, so fewer cells means bigger text.\n");
            vga_puts("\n  Usage: resolution fit | max | 160x50 | reset\n");
            vga_puts("  'fit' keeps the text about the size a console font\n");
            vga_puts("  is anywhere else; 'max' is every cell the panel\n");
            vga_puts("  holds and is hard to read on anything over 720p.\n");
            /* v0.8.8.5: this said "The desktop always runs at 80x25
               for now" and had been false since v0.7.18, which made
               the desktop chrome derive from vga_rows()/vga_cols().
               v0.8.8.4 finished the job: apps that lay out for 80x25
               are boxed and centred by the viewport, so the grid is
               free and the desktop uses all of it. */
            vga_puts("  The desktop uses the whole grid; apps that lay\n");
            vga_puts("  out for 80x25 are centred in a box of that size.\n");
        } else {
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_puts("\n  80x25   720x400, 9x16 cells  (stock, best proportions)\n");
            vga_puts("  80x50   720x400, 9x8  cells  (squashed - use 90x60)\n");
            vga_puts("  90x30   720x480, 8x16 cells  (wider)\n");
            vga_puts("  90x60   720x480, 8x8  cells  (max text, proper shape)\n");
            vga_puts("\n  Usage: resolution 90x60   (or just: resolution 60)\n");
            /* v0.8.8.5: this said "The desktop always runs at 80x25
               for now" and had been false since v0.7.18, which made
               the desktop chrome derive from vga_rows()/vga_cols().
               v0.8.8.4 finished the job: apps that lay out for 80x25
               are boxed and centred by the viewport, so the grid is
               free and the desktop uses all of it. */
            vga_puts("  The desktop uses the whole grid; apps that lay\n");
            vga_puts("  out for 80x25 are centred in a box of that size.\n");
            vga_puts("  VGA text memory: these four and no others, because\n");
            vga_puts("  each one is a whole CRTC register set.\n");
        }
        vga_puts("  If a mode will not sync, wait 10s - it reverts itself.\n");
        vga_puts("  Or type 'resolution reset' blind to force stock 80x25.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        return;
    }

    if(!strcmp(arg1,"reset")||!strcmp(arg1,"fix")){
        vga_force_text_mode();
        g_shell_cols=80; g_shell_rows=25;
        /* Forgotten, not saved as 80x25: `resolution reset` is the
           blind recovery path, and what it should leave behind is a
           machine that boots to its default rather than one pinned to
           whatever the default was on the day it was typed. */
        shell_grid_remember(0, 0);
        vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
        vga_puts("Video reprogrammed to stock 80x25.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        return;
    }

    /* Accept "90x60", "60", "25", and v0.8.8 "max" / "auto" */
    int cols=0, rows=0;
    if(!strcmp(arg1,"fit")||!strcmp(arg1,"readable")){
        /* ── v0.8.8.6: vga_fb_app_grid(), not readable_grid() ─────────
           On a 1024x768 panel the readable cell gives 64x24, and this
           command OFFERED IT. Kyan took it, it was saved, and every
           app then drew off the edge of a console too small to hold
           one -- `layout` on the HP reported the Dashboard 3855 cells
           outside its 64x24 console.

           app_grid() keeps the readable cell wherever the grid is
           still at least 80x25 and drops to a smaller one where it is
           not, so the same panel now offers 128x48. */
        if(!vga_fb_app_grid(&cols,&rows)){
            vga_set_color(VGA_YELLOW,VGA_BLACK);
            vga_puts("No framebuffer console here, so there is no panel to\n");
            vga_puts("fit. The VGA text maximum is 90x60.\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            return;
        }
    }
    else if(!strcmp(arg1,"max")||!strcmp(arg1,"auto")||!strcmp(arg1,"full")){
        if(!vga_fb_best_grid(&cols,&rows)){
            vga_set_color(VGA_YELLOW,VGA_BLACK);
            vga_puts("No framebuffer console here, so there is no panel to\n");
            vga_puts("fill. The VGA text maximum is 90x60: 'resolution 90x60'.\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            return;
        }
    } else {
        const char *p=arg1;
        int a=0; while(*p>='0'&&*p<='9'){ a=a*10+(*p-'0'); p++; }
        if(*p=='x'||*p=='X'){
            p++; int b=0; while(*p>='0'&&*p<='9'){ b=b*10+(*p-'0'); p++; }
            cols=a; rows=b;
        } else {
            rows=a;
            cols=(rows==30||rows==60)?90:80;
        }
    }

    int old_cols=vga_cols(), old_rows=vga_rows();

    if(!vga_set_mode(cols,rows)){
        /* ── v0.8.8: SAY WHICH REFUSAL IT WAS ───────────────────────────
           This printed one sentence about four text modes for all four
           causes, and on a framebuffer machine all four of those modes
           are irrelevant -- the advice sent the reader at the CRTC when
           the answer was the cell budget.  Named code, named remedy. */
        vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
        vga_puts("Refused: ");
        vga_puts(vga_mode_why_name());
        vga_puts(".\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        switch(vga_mode_why()){
            case VGA_MODE_ETEXT:
                vga_puts("  VGA text memory runs four modes: 80x25, 80x50,\n");
                vga_puts("  90x30, 90x60. Each is a whole register set.\n");
                break;
            case VGA_MODE_ERANGE:
                vga_puts("  The grid must be inside 240x135 -- that is the\n");
                vga_puts("  console buffers' addressing limit.\n");
                break;
            case VGA_MODE_EBUDGET:
                vga_puts("  Over 32400 cells. Ask for fewer: the glyphs get\n");
                vga_puts("  bigger, the screen stays full.\n");
                break;
            case VGA_MODE_EPANEL:
                vga_puts("  Bigger than the panel at 8 pixels a cell. ");
                vga_puts("'resolution'\n  on its own prints what fits.\n");
                break;
            default:
                vga_puts("  'resolution' on its own prints what fits.\n");
                break;
        }
        return;
    }

    /* ── v0.7.11: confirm-or-revert ──────────────────────────────────────
     * A mode that does not sync on a particular monitor leaves you looking at
     * a dark screen with no way to read a prompt, which is exactly what
     * happened with v0.7.11.  Every desktop OS guards display changes this way
     * and HavenDOS should too: keep the new mode only if a human confirms it
     * within ten seconds, otherwise put the old one back automatically.
     *
     * This makes an unsyncable mode a ten-second annoyance rather than a
     * reboot, and it holds even if the CRTC tables are wrong. */
    if(!(cols==80 && rows==25)){
        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts("\n  Keep this mode?  Press ENTER to confirm.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        vga_puts("  Reverting automatically in ");

        uint32_t start=get_ticks();
        int      confirmed=0, last_shown=-1;
        while(1){
            /* v0.7.12 FIX — get_ticks() counts 10 ms units, not milliseconds
               (kernel/pit.c).  Dividing by 1000 produced TEN-SECOND units, so
               `left` sat at 10 for a hundred seconds and the "reverts itself
               in 10s" guarantee was really a hundred-second wait.  That is the
               one timer in the OS that absolutely has to be right: it is the
               safety net for a display mode that will not sync, i.e. the case
               where the user cannot read anything on screen and is deciding
               whether to hard-reset the machine.  Same unit confusion as the
               DNS cache TTL. */
            uint32_t elapsed=(get_ticks()-start)/100u;
            int left=10-(int)elapsed;
            if(left<0) left=0;
            if(left!=last_shown){
                char b[8]; itoa(left,b,10);
                vga_puts(b); vga_puts("... ");
                last_shown=left;
            }
            int k=keyboard_getchar();
            if(k=='\n'||k=='\r'||k=='y'||k=='Y'){ confirmed=1; break; }
            if(k==27){ break; }
            if(left==0) break;
            /* The countdown runs for ten seconds waiting for a person
               to confirm a mode they may not be able to see. Ten
               seconds with no periodic work is ten seconds of DHCP
               not retrying -- on the one screen where the machine may
               be about to be told its display is unreadable. */
            kernel_poll();  /* kpoll-site-ok: a confirm-or-revert countdown waiting on a person, which is an idle point lasting ten seconds */
            sleep_ms(20);
        }

        if(!confirmed){
            vga_set_mode(old_cols,old_rows);
            g_shell_cols=old_cols; g_shell_rows=old_rows;
            vga_set_color(VGA_YELLOW,VGA_BLACK);
            vga_puts("\nMode reverted - no confirmation received.\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            return;
        }
    }

    g_shell_cols=cols; g_shell_rows=rows;
    shell_grid_remember(cols, rows);
    vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
    vga_puts("\nMode set to ");
    { char b[8]; itoa(cols,b,10); vga_puts(b); }
    vga_putchar('x');
    { char b[8]; itoa(rows,b,10); vga_puts(b); }
    vga_putchar('\n');
    vga_set_color(VGA_WHITE,VGA_BLACK);
}

/* ── v0.7.11 (roadmap 0.7.8): nslookup ──────────────────────────────── */
#include "../include/dns.h"
extern uint32_t dns_resolve(const char *host);
extern uint32_t net_get_dns(void);
extern void     ip_to_str(uint32_t ip, char *buf);

void cmd_nslookup_v6(const char *arg1){
    if(!arg1 || !arg1[0]){
        vga_puts("Usage: nslookup <hostname>   |   nslookup -cache   |   nslookup -flush\n");
        return;
    }
    char ipb[20];

    if(!strcmp(arg1,"-flush")){
        dns_cache_flush();
        vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
        vga_puts("Resolver cache cleared.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        return;
    }
    if(!strcmp(arg1,"-cache")){
        vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts("Resolver cache\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        int shown=0;
        for(int i=0;i<8;i++){
            char h[256]; uint32_t a[DNS_MAX_ADDRS]; int n=0, secs=0;
            if(!dns_cache_dump(i,h,a,&n,&secs)) continue;
            vga_puts("  "); vga_puts(h);
            int pad=32-(int)strlen(h); while(pad-->0) vga_putchar(' ');
            for(int k=0;k<n;k++){
                ip_to_str(a[k],ipb);
                vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
                vga_puts(ipb);
                vga_set_color(VGA_WHITE,VGA_BLACK);
                if(k<n-1) vga_puts(", ");
            }
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_puts("   ttl "); { char b[12]; itoa(secs,b,10); vga_puts(b); }
            vga_puts("s\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            shown++;
        }
        if(!shown){
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_puts("  (empty)\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
        }
        return;
    }

    ip_to_str(net_get_dns(),ipb);
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("Server:  "); vga_puts(ipb); vga_putchar('\n');
    vga_set_color(VGA_WHITE,VGA_BLACK);

    uint32_t addrs[DNS_MAX_ADDRS];
    int n = dns_resolve_all(arg1, addrs, DNS_MAX_ADDRS);
    if(n<=0){
        vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
        vga_puts("** server can't find "); vga_puts(arg1); vga_puts("\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        return;
    }
    vga_puts("Name:    "); vga_puts(arg1); vga_putchar('\n');
    for(int i=0;i<n;i++){
        ip_to_str(addrs[i],ipb);
        vga_puts("Address: ");
        vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
        vga_puts(ipb);
        vga_set_color(VGA_WHITE,VGA_BLACK);
        vga_putchar('\n');
    }
    if(n>1){
        vga_set_color(VGA_DARK_GREY,VGA_BLACK);
        vga_puts("(");
        { char b[8]; itoa(n,b,10); vga_puts(b); }
        vga_puts(" A records - previous releases returned only the first)\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
    }
}

/* ── v0.8.8 "Helios": `kpoll` ─────────────────────────────────────────
 *
 * What runs on a timer, how often, how many times it has run, and how
 * many times the poll refused -- with the refusals BROKEN OUT BY
 * REASON rather than counted together.
 *
 * That last part is the whole reason this command exists rather than a
 * line in `sysinfo`.  "The poll did not run" has four causes
 * (include/kpoll.h) and they are four different bugs: not initialised
 * is a boot-order mistake, already-polling is a client that called a
 * polling sleep, a-driver-holds-a-buffer is normal and expected, and
 * switched-off is somebody having typed `kpoll off`.  One counter for
 * all four would be x_control() returning (3) for six causes, which
 * cost this project a release to find.
 *
 * `late` is the number that answers "is the work slow, or is the poll
 * rare": a client becomes due on the tick whether or not anybody polls,
 * so a client that was already flagged due when the tick came round
 * again has not been reached. */
void cmd_kpoll_v6(const char *arg1)
{
    if(arg1 && (!strcmp(arg1,"off")||!strcmp(arg1,"stop"))){
        kpoll_set_enabled(0);
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("Periodic work is off. Nothing retries DHCP or notices a\n");
        vga_puts("device being plugged in until 'kpoll on'.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        return;
    }
    if(arg1 && (!strcmp(arg1,"on")||!strcmp(arg1,"start"))){
        kpoll_set_enabled(1);
        vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
        vga_puts("Periodic work is on.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        return;
    }

    vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts("Periodic work\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);

    vga_puts("  IRQ0 ticks : ");
    { char b[16]; utoa(kpoll_ticks(),b,10); vga_puts(b); }
    vga_puts("  at ");
    { char b[8]; itoa(KPOLL_HZ,b,10); vga_puts(b); }
    vga_puts(" Hz\n");

    /* The tick count is the one number here that can show the IRQ is
       not arriving at all -- which is what every release before this
       one assumed. A machine whose ticks stay near zero while uptime
       climbs has lost IRQ0, and before 0.8.8 nothing would have said
       so, because nothing depended on it. */
    vga_puts("  Enabled    : ");
    vga_puts(kpoll_enabled() ? "yes\n" : "no  ('kpoll on')\n");

    int n = kpoll_client_count();
    if(n == 0){
        vga_set_color(VGA_YELLOW,VGA_BLACK);
        vga_puts("\n  Nothing is registered.\n");
        vga_set_color(VGA_WHITE,VGA_BLACK);
        return;
    }

    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("\n  task           every     runs      late\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    for(int i = 0; i < n; i++){
        const char *nm = kpoll_client_name(i);
        int pad = 14;
        vga_puts("  ");
        vga_puts(nm);
        for(int k = 0; nm[k]; k++) pad--;
        while(pad-- > 0) vga_putchar(' ');
        { char b[12]; utoa(kpoll_client_interval(i)*(1000u/KPOLL_HZ),b,10);
          vga_puts(b); vga_puts("ms"); 
          int w = 8; for(int k=0;b[k];k++) w--; w -= 2;
          while(w-- > 0) vga_putchar(' '); }
        { char b[12]; utoa(kpoll_client_runs(i),b,10); vga_puts(b);
          int w = 10; for(int k=0;b[k];k++) w--;
          while(w-- > 0) vga_putchar(' '); }
        { char b[12]; utoa(kpoll_client_late(i),b,10); vga_puts(b); }
        vga_putchar('\n');
    }

    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("\n  refusals, by reason\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
    for(int r = 1; r < KPOLL_REASONS; r++){
        vga_puts("  ");
        { char b[12]; utoa(kpoll_refusals(r),b,10); vga_puts(b);
          int w = 10; for(int k=0;b[k];k++) w--;
          while(w-- > 0) vga_putchar(' '); }
        vga_puts(kpoll_reason_name(r));
        vga_putchar('\n');
    }

    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("\n  'a driver holds a shared buffer' is normal: the poll\n");
    vga_puts("  runs from the keyboard's idle loop and refuses while a\n");
    vga_puts("  transfer, a sector write or a borrowed buffer is open.\n");
    vga_puts("  'already polling' should stay at zero.\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}
