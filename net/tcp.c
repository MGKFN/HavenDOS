/*
 * tcp.c — Multi-socket TCP for HavenDOS v0.7.13
 *
 * v0.7.13 REFACTOR — the single global connection is gone.
 * ---------------------------------------------------------------------------
 * Every version up to v0.7.12 had exactly one connection: a file-scope
 * `g_conn`.  `tcp_send(fd,...)`, `tcp_recv(fd,...)` and friends all took an fd
 * and all began with `(void)fd;`.  That was the keystone blocking BOOT Chat,
 * HTTP keep-alive, and anything that needs to talk to two hosts at once.
 *
 * This file now keeps a table of TCP_MAX_SOCK independent sockets.  The fd a
 * caller receives is a real index into that table and is honoured everywhere.
 * Added with it, because they are impossible to bolt on afterwards:
 *
 *   - tcp_listen() / tcp_accept()   — inbound connections, so HavenDOS can be
 *                                     a server and not only a client
 *   - out-of-order reassembly       — segments that arrive ahead of a gap are
 *                                     held and replayed instead of discarded
 *   - a real TIME_WAIT timer        — closed sockets linger, then are reaped,
 *                                     instead of being recycled immediately
 *   - per-socket retransmission     — each socket has its own RTO and backoff
 *
 * Everything the v0.7.5–v0.7.12 bug-fix arc established is preserved verbatim,
 * just moved from `g_conn.x` to `s->x`.  Those comments are kept where they
 * are, because each one marks a trap that was expensive to find.
 *
 * All timeouts are polling loops — no IRQ dependency anywhere (UTM SE TCG).
 */

#include "../include/net.h"
#include "../include/tcp.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/netstat.h"
#include "pit.h"   /* v0.8.8.8: was a hand-copied extern */

extern int      ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len);
extern uint16_t ip_checksum(const void *data, uint16_t len);
extern void     net_poll(void);
extern uint32_t get_ticks(void);    /* 10 ms units, PIT-polled */

/* ── Sizing ──────────────────────────────────────────────────────── */
#define TCP_RX_BUF      8192
#define TCP_TX_BUF      4096
#define TCP_MSS_MAX     1460
#define TCP_MSS         1460
#define TCP_OOO_SLOTS   4       /* held out-of-order segments per socket */

#define RTX_MAX_TRIES   5
#define RTX_RTO_TICKS   40      /* ~400 ms initial, doubled per retry */
#define TIME_WAIT_TICKS 1000    /* 10 s — then the socket is reaped */
#define ACCEPT_BACKLOG  4

typedef enum {
    TCP_CLOSED=0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECV,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT
} tcp_state_t;

typedef struct {
    uint32_t seq;
    uint16_t len;
    uint8_t  data[TCP_MSS_MAX];
} tcp_ooo_t;

typedef struct {
    int         in_use;
    tcp_state_t state;
    uint32_t    remote_ip;
    uint16_t    remote_port;
    uint16_t    local_port;
    uint32_t    seq;        /* our next send seq */
    uint32_t    ack;        /* next seq we expect from remote */

    /* receive ring */
    uint8_t     rxbuf[TCP_RX_BUF];
    uint16_t    rxhead, rxtail;   /* head=write, tail=read */

    int         fin_recv;

    /* v0.7.5: retransmission state.  The stack previously had NONE — a
       single dropped segment stalled the connection until the caller's
       timeout expired, with no recovery.  QEMU SLIRP never drops anything
       so this was invisible in testing and fatal on real hardware.
       v0.7.13: now per-socket rather than one global slot. */
    uint32_t    snd_una;
    uint8_t     rtx_data[TCP_MSS_MAX];
    uint16_t    rtx_len;            /* 0 = nothing outstanding */
    uint8_t     rtx_flags;
    uint32_t    rtx_seq;
    uint32_t    rtx_sent_tick;
    uint8_t     rtx_count;

    /* v0.7.13: out-of-order hold queue.  Before this, a segment arriving
       ahead of a gap was answered with a duplicate ACK and THROWN AWAY, so
       the peer had to resend it and everything after it.  On a lossy link
       that collapses throughput; it was invisible under SLIRP, which never
       reorders or drops. */
    tcp_ooo_t   ooo[TCP_OOO_SLOTS];
    int         ooo_used;

    /* v0.7.13: TIME_WAIT is now timed rather than instant. */
    uint32_t    tw_tick;

    /* v0.7.13: listening sockets and their accept queue. */
    int         is_listen;
    int         backlog[ACCEPT_BACKLOG];
    int         backlog_n;
    int         parent;             /* listener fd for a child, else -1 */
} tcp_sock_t;

static tcp_sock_t g_sock[TCP_MAX_SOCK];
static uint16_t   g_next_port = 49152;

static tcp_sock_t *sk(int fd)
{
    if(fd < 0 || fd >= TCP_MAX_SOCK) return 0;
    if(!g_sock[fd].in_use) return 0;
    return &g_sock[fd];
}

static int sock_alloc(void)
{
    for(int i=0;i<TCP_MAX_SOCK;i++){
        if(!g_sock[i].in_use){
            memset(&g_sock[i], 0, sizeof(g_sock[i]));
            g_sock[i].in_use = 1;
            g_sock[i].parent = -1;
            g_sock[i].state  = TCP_CLOSED;
            return i;
        }
    }
    return -1;
}

static void sock_release(tcp_sock_t *s)
{
    if(!s) return;
    memset(s, 0, sizeof(*s));
    s->parent = -1;
}

static uint16_t port_alloc(void)
{
    /* Skip any ephemeral port already in use by a live socket. */
    for(int tries=0; tries<TCP_MAX_SOCK*4; tries++){
        uint16_t p = g_next_port++;
        if(g_next_port < 49152) g_next_port = 49152;
        int clash = 0;
        for(int i=0;i<TCP_MAX_SOCK;i++)
            if(g_sock[i].in_use && g_sock[i].local_port == p){ clash = 1; break; }
        if(!clash) return p;
    }
    return g_next_port++;
}

/* ── Checksum pseudo-header ───────────────────────────────────────── */

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                              const void *seg, uint16_t seg_len)
{
    /*
     * RFC 793 TCP checksum over pseudo-header + segment.
     *
     * Accumulates bytes BIG-ENDIAN: (byte[i]<<8)|byte[i+1].
     * Returns the ones-complement result in NETWORK byte order.
     *
     * CALLERS MUST:
     *   TX: h->checksum = htons(tcp_checksum(...))
     *   RX: calc = tcp_checksum(...); saved = ntohs(saved_field); if(calc!=saved) drop
     */
    uint8_t ph[12];
    ph[0]  = src_ip        & 0xFF;  ph[1]  = (src_ip >> 8)  & 0xFF;
    ph[2]  = (src_ip >> 16) & 0xFF; ph[3]  = (src_ip >> 24) & 0xFF;
    ph[4]  = dst_ip        & 0xFF;  ph[5]  = (dst_ip >> 8)  & 0xFF;
    ph[6]  = (dst_ip >> 16) & 0xFF; ph[7]  = (dst_ip >> 24) & 0xFF;
    ph[8]  = 0;
    ph[9]  = IP_PROTO_TCP;
    ph[10] = (seg_len >> 8) & 0xFF;
    ph[11] = seg_len & 0xFF;

    uint32_t sum = 0;
    for(int i = 0; i < 12; i += 2)
        sum += (uint32_t)(((uint16_t)ph[i] << 8) | ph[i+1]);

    /* Accumulate segment BE — byte-by-byte for ARM/TCG safety */
    const uint8_t *bp = (const uint8_t *)seg;
    uint16_t rem = seg_len;
    while(rem > 1) {
        sum += (uint32_t)(((uint16_t)bp[0] << 8) | bp[1]);
        bp += 2; rem -= 2;
    }
    if(rem) sum += (uint32_t)((uint16_t)bp[0] << 8);  /* pad lone byte */

    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);   /* network byte order result */
}

/* ── Receive ring helpers ────────────────────────────────────────── */

/* BUG FIX (v0.6.8): returns actual bytes stored (may be < len if ring fills) */
static uint16_t rx_push(tcp_sock_t *s, const uint8_t *data, uint16_t len)
{
    uint16_t stored = 0;
    for(uint16_t i=0;i<len;i++){
        uint16_t next=(uint16_t)((s->rxhead+1)%TCP_RX_BUF);
        if(next==s->rxtail) break;  /* ring full — stop here */
        s->rxbuf[s->rxhead]=data[i];
        s->rxhead=next;
        stored++;
    }
    return stored;
}

static uint16_t rx_available(tcp_sock_t *s)
{
    return (uint16_t)((s->rxhead - s->rxtail + TCP_RX_BUF) % TCP_RX_BUF);
}

/* BUG FIX (v0.6.8): actual free space in ring (capacity = TCP_RX_BUF - 1) */
static uint16_t rx_free(tcp_sock_t *s)
{
    return (uint16_t)((TCP_RX_BUF - 1) - rx_available(s));
}

/* ── Build and send a TCP segment ────────────────────────────────── */

static int seg_send_at(tcp_sock_t *s, uint8_t flags,
                       const void *data, uint16_t data_len, uint32_t seq)
{
    static uint8_t buf[sizeof(tcp_hdr_t) + TCP_TX_BUF];
    if(data_len > TCP_TX_BUF) return -1;

    tcp_hdr_t *h = (tcp_hdr_t*)buf;
    h->src_port  = htons(s->local_port);
    h->dst_port  = htons(s->remote_port);
    h->seq       = htonl(seq);
    h->ack_num   = htonl(s->ack);
    h->data_off  = (5 << 4);   /* 20-byte header, no options */
    h->flags     = flags;
    /* BUG FIX (v0.6.8): advertise actual free space, not the fixed constant
       TCP_RX_BUF.  Advertising TCP_RX_BUF always caused the remote to send
       more data than the ring could hold — rx_push would silently drop bytes
       but ack advanced by the full data_len, desynchronising sequence
       numbers. */
    h->window    = htons(rx_free(s));
    h->checksum  = 0;
    h->urgent    = 0;

    if(data && data_len)
        memcpy(buf + sizeof(tcp_hdr_t), data, data_len);

    uint16_t seg_len = (uint16_t)(sizeof(tcp_hdr_t) + data_len);
    h->checksum = htons(tcp_checksum(net_get_ip(), s->remote_ip, buf, seg_len));

    NS_INC(tcp_seg_tx);
    return ip_send(s->remote_ip, IP_PROTO_TCP, buf, seg_len);
}

static int seg_send(tcp_sock_t *s, uint8_t flags, const void *data, uint16_t len)
{
    return seg_send_at(s, flags, data, len, s->seq);
}

/* ── Out-of-order queue ──────────────────────────────────────────── */

static void ooo_store(tcp_sock_t *s, uint32_t seq, const uint8_t *d, uint16_t len)
{
    if(len == 0 || len > TCP_MSS_MAX) return;
    for(int i=0;i<TCP_OOO_SLOTS;i++)
        if(s->ooo[i].len && s->ooo[i].seq == seq) return;   /* already held */
    for(int i=0;i<TCP_OOO_SLOTS;i++){
        if(!s->ooo[i].len){
            s->ooo[i].seq = seq;
            s->ooo[i].len = len;
            memcpy(s->ooo[i].data, d, len);
            s->ooo_used++;
            return;
        }
    }
    /* All slots full — drop.  The peer will retransmit. */
}

/* Replay any held segments that now sit exactly at s->ack. */
static void ooo_drain(tcp_sock_t *s)
{
    int progress = 1;
    while(progress){
        progress = 0;
        for(int i=0;i<TCP_OOO_SLOTS;i++){
            if(!s->ooo[i].len) continue;
            if(s->ooo[i].seq == s->ack){
                uint16_t stored = rx_push(s, s->ooo[i].data, s->ooo[i].len);
                s->ack += stored;
                if(stored < s->ooo[i].len) NS_INC(tcp_rx_drop);
                s->ooo[i].len = 0;
                if(s->ooo_used) s->ooo_used--;
                progress = 1;
            }
            /* Already covered by the in-order stream — discard. */
            else if((int32_t)(s->ack - (s->ooo[i].seq + s->ooo[i].len)) >= 0){
                s->ooo[i].len = 0;
                if(s->ooo_used) s->ooo_used--;
            }
        }
    }
}

/* ── Demultiplex an arriving segment onto a socket ───────────────── */
/*
 * SLIRP rewrites the source IP of every TCP packet to 10.0.2.2, which is why
 * the pre-v0.7.13 code filtered on local port alone.  That workaround has to
 * survive, but it cannot be the only rule now that several sockets are live at
 * once.  Match from most specific to least: full 4-tuple, then port pair
 * (ignoring the rewritten IP), then a unique owner of the local port.
 */
static tcp_sock_t *demux(uint32_t src_ip, uint16_t sport, uint16_t dport)
{
    tcp_sock_t *s;
    for(int i=0;i<TCP_MAX_SOCK;i++){
        s = &g_sock[i];
        if(!s->in_use || s->is_listen || s->state==TCP_CLOSED) continue;
        if(s->local_port==dport && s->remote_port==sport && s->remote_ip==src_ip)
            return s;
    }
    for(int i=0;i<TCP_MAX_SOCK;i++){
        s = &g_sock[i];
        if(!s->in_use || s->is_listen || s->state==TCP_CLOSED) continue;
        if(s->local_port==dport && s->remote_port==sport)
            return s;           /* SLIRP rewrote the IP */
    }
    {
        tcp_sock_t *only = 0; int n = 0;
        for(int i=0;i<TCP_MAX_SOCK;i++){
            s = &g_sock[i];
            if(!s->in_use || s->is_listen || s->state==TCP_CLOSED) continue;
            if(s->local_port==dport){ only = s; n++; }
        }
        if(n == 1) return only;
    }
    return 0;
}

static tcp_sock_t *find_listener(uint16_t dport)
{
    for(int i=0;i<TCP_MAX_SOCK;i++)
        if(g_sock[i].in_use && g_sock[i].is_listen &&
           g_sock[i].state == TCP_LISTEN && g_sock[i].local_port == dport)
            return &g_sock[i];
    return 0;
}

/* Send a bare RST to a segment we have no socket for. */
static void send_rst(uint32_t dst_ip, uint16_t sport, uint16_t dport,
                     uint32_t seg_seq, uint32_t seg_ack, uint8_t in_flags,
                     uint16_t data_len)
{
    uint8_t buf[sizeof(tcp_hdr_t)];
    tcp_hdr_t *h = (tcp_hdr_t*)buf;
    memset(buf, 0, sizeof(buf));
    h->src_port = htons(dport);
    h->dst_port = htons(sport);
    h->data_off = (5 << 4);
    if(in_flags & TCP_ACK){
        h->seq     = htonl(seg_ack);
        h->flags   = TCP_RST;
    } else {
        uint32_t n = seg_seq + data_len + ((in_flags & TCP_SYN) ? 1 : 0);
        h->seq     = 0;
        h->ack_num = htonl(n);
        h->flags   = TCP_RST | TCP_ACK;
    }
    h->checksum = htons(tcp_checksum(net_get_ip(), dst_ip, buf, sizeof(buf)));
    ip_send(dst_ip, IP_PROTO_TCP, buf, sizeof(buf));
}

/* ── Called from ip_handle when proto=TCP ────────────────────────── */
void tcp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len)
{
    if(len < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *h = (const tcp_hdr_t*)payload;

    uint16_t sport = ntohs(h->src_port);
    uint16_t dport = ntohs(h->dst_port);

    /* BUG FIX (v0.6.8): verify incoming TCP checksum.
       tcp_checksum() returns a network-byte-order value.  The stored field is
       also in network byte order but read as LE by x86, so ntohs() is required
       to restore the true BE value before comparing. */
    {
        uint16_t saved_field = h->checksum;         /* LE read of BE field */
        uint16_t saved_be    = ntohs(saved_field);  /* restore to BE */
        ((tcp_hdr_t *)payload)->checksum = 0;
        uint16_t calc = tcp_checksum(src_ip, net_get_ip(), payload, len);
        ((tcp_hdr_t *)payload)->checksum = saved_field;
        if(calc != saved_be){ NS_INC(tcp_bad_csum); return; }
    }
    NS_INC(tcp_seg_rx);

    uint8_t  flags   = h->flags;
    uint32_t seg_seq = ntohl(h->seq);
    uint32_t seg_ack = ntohl(h->ack_num);
    uint8_t  hdr_len = (h->data_off >> 4) * 4;

    if(hdr_len < sizeof(tcp_hdr_t) || hdr_len > len) return;

    const uint8_t *data = payload + hdr_len;
    uint16_t data_len = (uint16_t)(len - hdr_len);

    tcp_sock_t *s = demux(src_ip, sport, dport);

    /* ── No established socket: maybe a listener wants this SYN ──────── */
    if(!s){
        tcp_sock_t *l = find_listener(dport);
        if(l && (flags & TCP_SYN) && !(flags & TCP_ACK)){
            if(l->backlog_n >= ACCEPT_BACKLOG){ return; }  /* queue full */
            int cfd = sock_alloc();
            if(cfd < 0) return;
            tcp_sock_t *c = &g_sock[cfd];
            c->state       = TCP_SYN_RECV;
            c->remote_ip   = src_ip;
            c->remote_port = sport;
            c->local_port  = dport;
            c->ack         = seg_seq + 1;
            c->seq         = 0x5A5A0000u ^ (get_ticks() << 8);
            c->snd_una     = c->seq;
            c->parent      = (int)(l - g_sock);
            seg_send(c, TCP_SYN|TCP_ACK, 0, 0);
            c->seq++;
            return;
        }
        /* Nothing is listening and nothing matches — RST so the peer gives up
           immediately instead of retrying for seconds. */
        if(!(flags & TCP_RST)) send_rst(src_ip, sport, dport, seg_seq, seg_ack,
                                        flags, data_len);
        return;
    }

    /* ── SYN_RECV: waiting for the handshake's final ACK ─────────────── */
    if(s->state == TCP_SYN_RECV){
        if(flags & TCP_RST){ NS_INC(tcp_rst_rx); sock_release(s); return; }
        if((flags & TCP_ACK) && seg_ack == s->seq){
            s->state = TCP_ESTABLISHED;
            tcp_sock_t *l = (s->parent >= 0) ? &g_sock[s->parent] : 0;
            if(l && l->is_listen && l->backlog_n < ACCEPT_BACKLOG)
                l->backlog[l->backlog_n++] = (int)(s - g_sock);
        }
        return;
    }

    if(s->state == TCP_SYN_SENT){
        /* v0.7.12: RST was ignored here, so a refused connection — nothing
           listening on the port, which is the single most common connection
           failure — burned the whole 5-second poll_until() timeout before
           reporting an error, and did so on every retry.  RFC 793: in
           SYN-SENT, a RST is acceptable if it acknowledges our SYN. */
        if(flags & TCP_RST){
            if(!(flags & TCP_ACK) || seg_ack == s->seq){
                NS_INC(tcp_rst_rx);
                s->state = TCP_CLOSED;
            } else NS_INC(tcp_rst_ignored);
            return;
        }
        if((flags & (TCP_SYN|TCP_ACK)) == (TCP_SYN|TCP_ACK)){
            /* Only accept a SYN-ACK that actually acknowledges our SYN. */
            if(seg_ack != s->seq) return;
            s->ack   = seg_seq + 1;
            s->seq   = seg_ack;
            s->state = TCP_ESTABLISHED;
            seg_send(s, TCP_ACK, 0, 0);
        }
        return;
    }

    if(s->state == TCP_ESTABLISHED ||
       s->state == TCP_FIN_WAIT    ||
       s->state == TCP_CLOSE_WAIT)
    {
        /* BUG FIX (v0.6.8): validate RST sequence number before tearing down.
           RFC 5961: RST is only valid if seg_seq == RCV.NXT.  Blind RST
           injection attacks rely on the old unconditional close. */
        if(flags & TCP_RST){
            if(seg_seq == s->ack){
                NS_INC(tcp_rst_rx);
                s->state = TCP_CLOSED;
            } else NS_INC(tcp_rst_ignored);
            return;
        }

        /* v0.7.5: cumulative ACK clears the retransmission slot. */
        if(flags & TCP_ACK){
            if(s->rtx_len &&
               (int32_t)(seg_ack - (s->rtx_seq + s->rtx_len)) >= 0){
                s->rtx_len = 0;
                s->snd_una = seg_ack;
            }
        }

        /* Data payload */
        if(data_len > 0 && s->state == TCP_ESTABLISHED){
            if(seg_seq == s->ack){
                /* BUG FIX (v0.6.8): only advance ACK by bytes actually stored.
                   Old code: ack += data_len — even when rx_push dropped bytes
                   because the ring was full.  That told the remote "I have all
                   your data" for bytes we never kept, causing permanent data
                   loss and stream desync. */
                uint16_t stored = rx_push(s, data, data_len);
                if(stored < data_len) NS_INC(tcp_rx_drop);
                s->ack += stored;
                /* v0.7.13: a held segment may now be contiguous. */
                if(s->ooo_used) ooo_drain(s);
                seg_send(s, TCP_ACK, 0, 0);
            }
            else if((int32_t)(seg_seq - s->ack) > 0 &&
                    (int32_t)(seg_seq - s->ack) < TCP_RX_BUF){
                /* v0.7.13: ahead of a gap — HOLD it rather than discard, then
                   duplicate-ACK so the peer fast-retransmits the missing
                   piece.  Pre-v0.7.13 this segment was dropped outright and
                   the peer had to resend it and everything after it. */
                NS_INC(tcp_ooo);
                ooo_store(s, seg_seq, data, data_len);
                seg_send(s, TCP_ACK, 0, 0);
            }
            else {
                /* Old duplicate — re-ACK current position. */
                NS_INC(tcp_ooo);
                seg_send(s, TCP_ACK, 0, 0);
            }
        }

        /* FIN from remote.
           v0.7.5 FIX: guard on sequence.  The old code incremented ack for
           ANY segment with FIN set — a retransmitted FIN (sent because our
           ACK was lost) bumped ack a second time and desynchronised the
           connection permanently. */
        if((flags & TCP_FIN) && !s->fin_recv && seg_seq + data_len == s->ack){
            s->ack++;
            s->fin_recv = 1;
            seg_send(s, TCP_ACK, 0, 0);
            if(s->state == TCP_FIN_WAIT){
                s->state   = TCP_TIME_WAIT;
                s->tw_tick = get_ticks();
            } else {
                s->state = TCP_CLOSE_WAIT;
            }
        }
        else if((flags & TCP_FIN) && s->fin_recv){
            NS_INC(tcp_dup_fin);
            /* Duplicate FIN — our ACK was lost.  Re-ACK without touching
               state or sequence numbers, so the peer stops retransmitting. */
            seg_send(s, TCP_ACK, 0, 0);
        }

        /* ACK our FIN */
        if((flags & TCP_ACK) && s->state == TCP_FIN_WAIT){
            s->state   = TCP_TIME_WAIT;
            s->tw_tick = get_ticks();
        }
    }
}

/* ── Timers ──────────────────────────────────────────────────────────
   Called from every polling loop.  Retransmits anything whose RTO has
   expired, on every socket, and reaps expired TIME_WAIT sockets. */
void tcp_rtx_check(void)
{
    uint32_t now = get_ticks();

    for(int i=0;i<TCP_MAX_SOCK;i++){
        tcp_sock_t *s = &g_sock[i];
        if(!s->in_use) continue;

        /* v0.7.13: TIME_WAIT used to be instant — tcp_close() set the state
           and the very next connect() reused the port.  A late duplicate from
           the old connection could then be accepted by the new one.  Linger,
           then reap. */
        if(s->state == TCP_TIME_WAIT){
            if((uint32_t)(now - s->tw_tick) >= TIME_WAIT_TICKS)
                sock_release(s);
            continue;
        }

        if(!s->rtx_len) continue;
        if(s->state != TCP_ESTABLISHED){ s->rtx_len = 0; continue; }

        uint32_t rto = RTX_RTO_TICKS << s->rtx_count;   /* exponential backoff */
        if((uint32_t)(now - s->rtx_sent_tick) < rto) continue;

        if(s->rtx_count >= RTX_MAX_TRIES){
            NS_INC(tcp_rtx_giveup);
            /* Peer is gone.  Tear down rather than spin forever. */
            s->rtx_len = 0;
            s->state   = TCP_CLOSED;
            continue;
        }

        s->rtx_count++;
        NS_INC(tcp_rtx);
        s->rtx_sent_tick = now;
        seg_send_at(s, s->rtx_flags, s->rtx_data, s->rtx_len, s->rtx_seq);
    }
}

/* ── Poll with timeout (ms) ──────────────────────────────────────── */
static int poll_until(tcp_sock_t *s, tcp_state_t want, uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while(elapsed < timeout_ms){
        for(int i=0;i<32;i++) net_poll();
        tcp_rtx_check();
        if(!s->in_use)             return (want == TCP_TIME_WAIT);
        if(s->state == want)       return 1;
        if(s->state == TCP_CLOSED) return 0;
        sleep_ms(2);
        elapsed += 2;
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

int tcp_connect(uint32_t ip, uint16_t port)
{
    int fd = sock_alloc();
    if(fd < 0) return -2;                 /* socket table full */
    tcp_sock_t *s = &g_sock[fd];

    s->remote_ip   = ip;
    s->remote_port = port;
    s->local_port  = port_alloc();
    s->seq         = 0x12345678u ^ (get_ticks() << 8);
    s->snd_una     = s->seq;
    s->rtx_len     = 0;
    s->ack         = 0;
    s->state       = TCP_SYN_SENT;
    s->fin_recv    = 0;

    seg_send(s, TCP_SYN, 0, 0);
    s->seq++;

    if(!poll_until(s, TCP_ESTABLISHED, 5000)){
        sock_release(s);
        return -1;
    }
    return fd;
}

/* v0.7.13: inbound connections. */
int tcp_listen(uint16_t port)
{
    for(int i=0;i<TCP_MAX_SOCK;i++)
        if(g_sock[i].in_use && g_sock[i].local_port == port && g_sock[i].is_listen)
            return -3;                    /* already listening */
    int fd = sock_alloc();
    if(fd < 0) return -2;
    tcp_sock_t *s = &g_sock[fd];
    s->is_listen  = 1;
    s->local_port = port;
    s->state      = TCP_LISTEN;
    return fd;
}

int tcp_accept(int lfd, uint32_t timeout_ms)
{
    tcp_sock_t *l = sk(lfd);
    if(!l || !l->is_listen) return -1;

    uint32_t elapsed = 0;
    for(;;){
        if(l->backlog_n > 0){
            int cfd = l->backlog[0];
            for(int i=1;i<l->backlog_n;i++) l->backlog[i-1] = l->backlog[i];
            l->backlog_n--;
            return cfd;
        }
        if(elapsed >= timeout_ms) return -1;
        for(int i=0;i<32;i++) net_poll();
        tcp_rtx_check();
        sleep_ms(2); elapsed += 2;
    }
}

uint32_t tcp_peer_ip(int fd)   { tcp_sock_t *s = sk(fd); return s ? s->remote_ip : 0; }
uint16_t tcp_peer_port(int fd) { tcp_sock_t *s = sk(fd); return s ? s->remote_port : 0; }

/* v0.7.5 FIX: ip_send() builds into a 1500-byte frame, so a TCP segment
   larger than 1480-20 = 1460 payload bytes was rejected with -1 *after*
   tcp_send_seg had already been told to build it.  tcp_send returned the
   error but callers (tls_send_raw, http send_request) mostly ignored it, so
   large writes vanished with no diagnostic.  Now segmented at the MSS. */
int tcp_send(int fd, const void *data, uint16_t len)
{
    tcp_sock_t *s = sk(fd);
    if(!s || s->state != TCP_ESTABLISHED) return -1;
    if(len == 0) return 0;

    const uint8_t *p = (const uint8_t*)data;
    uint16_t sent = 0;
    while(sent < len){
        uint16_t chunk = (uint16_t)(len - sent);
        if(chunk > TCP_MSS) chunk = TCP_MSS;
        uint32_t this_seq = s->seq;
        if(seg_send_at(s, TCP_ACK|TCP_PSH, p + sent, chunk, this_seq) != 0) return -1;

        /* Arm retransmission for this segment. */
        memcpy(s->rtx_data, p + sent, chunk);
        s->rtx_len       = chunk;
        s->rtx_flags     = TCP_ACK|TCP_PSH;
        s->rtx_seq       = this_seq;
        s->rtx_sent_tick = get_ticks();
        s->rtx_count     = 0;

        s->seq += chunk;
        sent = (uint16_t)(sent + chunk);

        /* Wait for the ACK before pushing the next segment.  Without this
           we could have several unacknowledged segments in flight and only
           one retransmission slot. */
        {
            uint32_t waited = 0;
            while(s->rtx_len && waited < 5000 && s->state == TCP_ESTABLISHED){
                for(int i=0;i<16;i++) net_poll();
                tcp_rtx_check();
                if(!s->rtx_len) break;
                sleep_ms(2); waited += 2;
            }
            if(s->rtx_len){ s->rtx_len = 0; return -1; }  /* gave up */
        }
    }
    return 0;
}

int tcp_recv(int fd, void *buf, uint16_t maxlen, uint32_t timeout_ms)
{
    tcp_sock_t *s = sk(fd);
    if(!s || s->state == TCP_CLOSED) return -1;

    uint32_t elapsed = 0;
    while(elapsed < timeout_ms && !rx_available(s)){
        for(int i=0;i<32;i++) net_poll();
        tcp_rtx_check();
        if(!s->in_use) return -1;
        if(s->fin_recv && !rx_available(s)) return 0;   /* EOF */
        if(s->state == TCP_CLOSED) return -1;
        if(!rx_available(s)){ sleep_ms(2); elapsed+=2; }
    }

    uint16_t avail = rx_available(s);
    if(avail == 0) return 0;
    if(avail > maxlen) avail = maxlen;

    uint16_t free_before = rx_free(s);

    uint8_t *out = (uint8_t*)buf;
    for(uint16_t i=0;i<avail;i++){
        out[i] = s->rxbuf[s->rxtail];
        s->rxtail = (uint16_t)((s->rxtail+1) % TCP_RX_BUF);
    }

    /* v0.7.5 CRITICAL FIX — zero-window deadlock.
       seg_send advertises rx_free().  Once the 8 KB ring filled we correctly
       advertised a window of 0 and the peer stopped sending — but draining the
       ring here sent nothing, so the peer waited forever for a window update
       that never came.  Every transfer larger than the ring (TLS certificate
       chains, any real download) hung.  Send an explicit window update
       whenever we free a meaningful amount. */
    if(s->state == TCP_ESTABLISHED &&
       free_before < (TCP_RX_BUF/4) && rx_free(s) > free_before){
        NS_INC(tcp_win_update);
        seg_send(s, TCP_ACK, 0, 0);
    }

    return (int)avail;
}

void tcp_close(int fd)
{
    tcp_sock_t *s = sk(fd);
    if(!s) return;

    if(s->is_listen){
        /* Drop any connections queued but never accepted. */
        for(int i=0;i<s->backlog_n;i++){
            tcp_sock_t *c = &g_sock[s->backlog[i]];
            if(c->in_use && c->state == TCP_ESTABLISHED){
                c->state = TCP_FIN_WAIT;
                seg_send(c, TCP_FIN|TCP_ACK, 0, 0);
                c->seq++;
            }
        }
        sock_release(s);
        return;
    }

    if(s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT){
        s->state = TCP_FIN_WAIT;
        seg_send(s, TCP_FIN|TCP_ACK, 0, 0);
        s->seq++;
        poll_until(s, TCP_TIME_WAIT, 3000);
    }

    /* v0.7.13: leave the socket in TIME_WAIT if we got there; tcp_rtx_check()
       reaps it once the timer expires.  Anything else is released now. */
    if(s->in_use && s->state != TCP_TIME_WAIT)
        sock_release(s);
}

int tcp_connected(int fd)
{
    tcp_sock_t *s = sk(fd);
    return s && s->state == TCP_ESTABLISHED;
}

int tcp_eof(int fd)
{
    tcp_sock_t *s = sk(fd);
    if(!s) return 1;
    if(s->state == TCP_CLOSED) return 1;
    return s->fin_recv && !rx_available(s);
}

/* v0.7.13: diagnostics for `netstat` / `tlsinfo`. */
int tcp_sock_count(void)
{
    int n = 0;
    for(int i=0;i<TCP_MAX_SOCK;i++) if(g_sock[i].in_use) n++;
    return n;
}

int tcp_sock_info(int idx, uint16_t *lport, uint32_t *rip,
                  uint16_t *rport, int *state)
{
    if(idx < 0 || idx >= TCP_MAX_SOCK) return 0;
    tcp_sock_t *s = &g_sock[idx];
    if(!s->in_use) return 0;
    if(lport) *lport = s->local_port;
    if(rip)   *rip   = s->remote_ip;
    if(rport) *rport = s->remote_port;
    if(state) *state = (int)s->state;
    return 1;
}

const char *tcp_state_name(int st)
{
    switch(st){
        case TCP_CLOSED:      return "CLOSED";
        case TCP_LISTEN:      return "LISTEN";
        case TCP_SYN_SENT:    return "SYN-SENT";
        case TCP_SYN_RECV:    return "SYN-RECV";
        case TCP_ESTABLISHED: return "ESTABLISHED";
        case TCP_FIN_WAIT:    return "FIN-WAIT";
        case TCP_TIME_WAIT:   return "TIME-WAIT";
        case TCP_CLOSE_WAIT:  return "CLOSE-WAIT";
    }
    return "UNKNOWN";
}
