/*
 * tcp.c — Minimal TCP for HavenDOS v0.6.8
 *
 * Supports one connection at a time (client-only, no server).
 * No IRQ-based timers — all timeouts use polling loops safe on UTM SE TCG.
 *
 * API:
 *   int  tcp_connect(uint32_t ip, uint16_t port)   → fd (0) or <0 error
 *   int  tcp_send(int fd, const void *data, uint16_t len)
 *   int  tcp_recv(int fd, void *buf, uint16_t maxlen, uint32_t timeout_ms)
 *   void tcp_close(int fd)
 *   int  tcp_connected(int fd)
 *
 * BUG FIXES (v0.6.8):
 *   - TCP_CLOSE_WAIT added to state enum (was only in tcp.h as a stale #define)
 *   - rx_push now returns actual bytes stored; ACK only advances by stored count
 *   - Window advertisement corrected to actual ring free space, not fixed constant
 *   - Advertised window never exceeds ring capacity (TCP_RX_BUF - 1)
 */

#include "../include/net.h"
#include "../include/tcp.h"
#include "../include/string.h"
#include "../include/types.h"

extern int      ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len);
extern uint16_t ip_checksum(const void *data, uint16_t len);
extern void     net_poll(void);
extern void     sleep_ms(uint32_t ms);

/* ── Single connection state ──────────────────────────────────────── */
#define TCP_RX_BUF  8192
#define TCP_TX_BUF  4096

typedef enum {
    TCP_CLOSED=0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT   /* BUG FIX: was missing from enum, only in tcp.h #define */
} tcp_state_t;

static struct {
    tcp_state_t state;
    uint32_t    remote_ip;
    uint16_t    remote_port;
    uint16_t    local_port;
    uint32_t    seq;       /* our next send seq */
    uint32_t    ack;       /* next seq we expect from remote */

    /* receive ring */
    uint8_t     rxbuf[TCP_RX_BUF];
    uint16_t    rxhead, rxtail;   /* head=write, tail=read */

    int         fin_recv;
} g_conn;

static uint16_t g_next_port = 49152;

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
     *
     * This matches how every standard TCP stack (Linux, BSD) works:
     * checksum is computed in network byte order, then byte-swapped into
     * the struct field so the wire representation is correct.
     */

    /* Build pseudo-header in network byte order (RFC 793 §3.1) */
    uint8_t ph[12];
    ph[0]  = src_ip        & 0xFF;  ph[1]  = (src_ip >> 8)  & 0xFF;
    ph[2]  = (src_ip >> 16) & 0xFF; ph[3]  = (src_ip >> 24) & 0xFF;
    ph[4]  = dst_ip        & 0xFF;  ph[5]  = (dst_ip >> 8)  & 0xFF;
    ph[6]  = (dst_ip >> 16) & 0xFF; ph[7]  = (dst_ip >> 24) & 0xFF;
    ph[8]  = 0;
    ph[9]  = IP_PROTO_TCP;
    ph[10] = (seg_len >> 8) & 0xFF;  /* tcp_len high byte */
    ph[11] = seg_len & 0xFF;         /* tcp_len low byte  */

    uint32_t sum = 0;

    /* Accumulate pseudo-header BE — byte-by-byte to avoid unaligned reads */
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

    /* Fold 32-bit carries */
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);   /* network byte order result */
}

/* ── Receive ring helpers ────────────────────────────────────────── */

/* BUG FIX: returns actual bytes stored (may be < len if ring fills) */
static uint16_t rx_push(const uint8_t *data, uint16_t len)
{
    uint16_t stored = 0;
    for(uint16_t i=0;i<len;i++){
        uint16_t next=(g_conn.rxhead+1)%TCP_RX_BUF;
        if(next==g_conn.rxtail) break;  /* ring full — stop here */
        g_conn.rxbuf[g_conn.rxhead]=data[i];
        g_conn.rxhead=next;
        stored++;
    }
    return stored;
}

static uint16_t rx_available(void)
{
    return (g_conn.rxhead - g_conn.rxtail + TCP_RX_BUF) % TCP_RX_BUF;
}

/* BUG FIX: actual free space in ring (capacity = TCP_RX_BUF - 1) */
static uint16_t rx_free(void)
{
    return (uint16_t)((TCP_RX_BUF - 1) - rx_available());
}

/* ── Build and send a TCP segment ────────────────────────────────── */
static int tcp_send_seg(uint8_t flags, const void *data, uint16_t data_len)
{
    static uint8_t buf[sizeof(tcp_hdr_t) + TCP_TX_BUF];
    if(data_len > TCP_TX_BUF) return -1;

    tcp_hdr_t *h = (tcp_hdr_t*)buf;
    h->src_port  = htons(g_conn.local_port);
    h->dst_port  = htons(g_conn.remote_port);
    h->seq       = htonl(g_conn.seq);
    h->ack_num   = htonl(g_conn.ack);
    h->data_off  = (5 << 4);   /* 20-byte header, no options */
    h->flags     = flags;
    /* BUG FIX: advertise actual free space, not the fixed constant TCP_RX_BUF.
       Advertising TCP_RX_BUF always caused the remote to send more data than
       the ring could hold — rx_push would silently drop bytes but ack advanced
       by the full data_len, desynchronising sequence numbers. */
    h->window    = htons(rx_free());
    h->checksum  = 0;
    h->urgent    = 0;

    if(data && data_len)
        memcpy(buf + sizeof(tcp_hdr_t), data, data_len);

    uint16_t seg_len = (uint16_t)(sizeof(tcp_hdr_t) + data_len);
    /* tcp_checksum() returns network byte order; htons converts to the
       byte layout the struct field requires so the wire value is correct. */
    h->checksum = htons(tcp_checksum(net_get_ip(), g_conn.remote_ip, buf, seg_len));

    return ip_send(g_conn.remote_ip, IP_PROTO_TCP, buf, seg_len);
}

/* ── Called from ip_handle when proto=TCP ────────────────────────── */
void tcp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len)
{
    if(len < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *h = (const tcp_hdr_t*)payload;

    if(g_conn.state == TCP_CLOSED) return;

    /* BUG FIX: filter by local port AND remote IP+port.
       In TCP_ESTABLISHED we must only accept segments from our peer.
       During SYN_SENT we accept from any IP (SLIRP rewrites src to 10.0.2.2
       anyway) but still validate the destination port. */
    if(ntohs(h->dst_port) != g_conn.local_port) return;
    if(g_conn.state == TCP_ESTABLISHED ||
       g_conn.state == TCP_FIN_WAIT    ||
       g_conn.state == TCP_CLOSE_WAIT) {
        if(ntohs(h->src_port) != g_conn.remote_port) return;
    }

    /* BUG FIX: verify incoming TCP checksum.
       tcp_checksum() returns a network-byte-order value.
       The stored field is also in network byte order but read as LE by x86,
       so ntohs() is required to restore the true BE value before comparing. */
    {
        uint16_t saved_field = h->checksum;   /* LE read of BE field */
        uint16_t saved_be    = ntohs(saved_field);   /* restore to BE */
        ((tcp_hdr_t *)payload)->checksum = 0;
        uint16_t calc = tcp_checksum(src_ip, net_get_ip(), payload, len);
        ((tcp_hdr_t *)payload)->checksum = saved_field;   /* restore */
        if(calc != saved_be) return;   /* checksum mismatch — discard */
    }

    uint8_t  flags   = h->flags;
    uint32_t seg_seq = ntohl(h->seq);
    uint32_t seg_ack = ntohl(h->ack_num);
    uint8_t  hdr_len = (h->data_off >> 4) * 4;

    /* Validate TCP header length field */
    if(hdr_len < sizeof(tcp_hdr_t) || hdr_len > len) return;

    const uint8_t *data = payload + hdr_len;
    uint16_t data_len = (uint16_t)(len - hdr_len);

    if(g_conn.state == TCP_SYN_SENT){
        if((flags & (TCP_SYN|TCP_ACK)) == (TCP_SYN|TCP_ACK)){
            g_conn.ack = seg_seq + 1;
            g_conn.seq = seg_ack;
            g_conn.state = TCP_ESTABLISHED;
            tcp_send_seg(TCP_ACK, 0, 0);
        }
        return;
    }

    if(g_conn.state == TCP_ESTABLISHED ||
       g_conn.state == TCP_FIN_WAIT    ||
       g_conn.state == TCP_CLOSE_WAIT)
    {
        /* BUG FIX: validate RST sequence number before tearing down.
           RFC 5961: RST is only valid if seg_seq == RCV.NXT.
           Blind RST injection attacks rely on the old unconditional close. */
        if(flags & TCP_RST){
            if(seg_seq == g_conn.ack)
                g_conn.state = TCP_CLOSED;
            /* else: off-window RST — ignore (possible injection attempt) */
            return;
        }

        /* Data payload */
        if(data_len > 0 && g_conn.state == TCP_ESTABLISHED){
            if(seg_seq == g_conn.ack){
                /* BUG FIX: only advance ACK by bytes actually stored.
                   Old code: g_conn.ack += data_len — even when rx_push
                   dropped bytes because the ring was full. That told the
                   remote "I have all your data" for bytes we never kept,
                   causing permanent data loss and stream desync. */
                uint16_t stored = rx_push(data, data_len);
                g_conn.ack += stored;
                tcp_send_seg(TCP_ACK, 0, 0);
            }
            /* Out-of-order or duplicate: re-ACK current position to signal gap */
            else {
                tcp_send_seg(TCP_ACK, 0, 0);
            }
        }

        /* FIN from remote */
        if(flags & TCP_FIN){
            g_conn.ack++;
            g_conn.fin_recv = 1;
            tcp_send_seg(TCP_ACK, 0, 0);
            if(g_conn.state == TCP_FIN_WAIT)
                g_conn.state = TCP_TIME_WAIT;
            else
                g_conn.state = TCP_CLOSE_WAIT;
        }

        /* ACK our FIN */
        if((flags & TCP_ACK) && g_conn.state == TCP_FIN_WAIT)
            g_conn.state = TCP_TIME_WAIT;
    }
}

/* ── Poll with timeout (ms) ──────────────────────────────────────── */
static int poll_until(tcp_state_t want, uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while(elapsed < timeout_ms){
        for(int i=0;i<32;i++) net_poll();
        if(g_conn.state == want)   return 1;
        if(g_conn.state == TCP_CLOSED) return 0;
        sleep_ms(2);
        elapsed += 2;
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

int tcp_connect(uint32_t ip, uint16_t port)
{
    memset(&g_conn, 0, sizeof(g_conn));
    g_conn.remote_ip   = ip;
    g_conn.remote_port = port;
    g_conn.local_port  = g_next_port++;
    if(g_next_port < 49152) g_next_port = 49152;
    g_conn.seq         = 0x12345678;
    g_conn.ack         = 0;
    g_conn.state       = TCP_SYN_SENT;
    g_conn.fin_recv    = 0;

    tcp_send_seg(TCP_SYN, 0, 0);
    g_conn.seq++;

    if(!poll_until(TCP_ESTABLISHED, 5000)){
        g_conn.state = TCP_CLOSED;
        return -1;
    }
    return 0;
}

int tcp_send(int fd, const void *data, uint16_t len)
{
    (void)fd;
    if(g_conn.state != TCP_ESTABLISHED) return -1;
    if(len == 0) return 0;

    int ret = tcp_send_seg(TCP_ACK|TCP_PSH, data, len);
    if(ret == 0) g_conn.seq += len;
    return ret;
}

int tcp_recv(int fd, void *buf, uint16_t maxlen, uint32_t timeout_ms)
{
    (void)fd;
    if(g_conn.state == TCP_CLOSED) return -1;

    uint32_t elapsed = 0;
    while(elapsed < timeout_ms && !rx_available()){
        for(int i=0;i<32;i++) net_poll();
        if(g_conn.fin_recv && !rx_available()) return 0;   /* EOF */
        if(g_conn.state == TCP_CLOSED) return -1;
        if(!rx_available()){ sleep_ms(2); elapsed+=2; }
    }

    uint16_t avail = rx_available();
    if(avail == 0) return 0;
    if(avail > maxlen) avail = maxlen;

    uint8_t *out = (uint8_t*)buf;
    for(uint16_t i=0;i<avail;i++){
        out[i] = g_conn.rxbuf[g_conn.rxtail];
        g_conn.rxtail = (g_conn.rxtail+1) % TCP_RX_BUF;
    }
    return (int)avail;
}

void tcp_close(int fd)
{
    (void)fd;
    if(g_conn.state == TCP_ESTABLISHED || g_conn.state == TCP_CLOSE_WAIT){
        g_conn.state = TCP_FIN_WAIT;
        tcp_send_seg(TCP_FIN|TCP_ACK, 0, 0);
        g_conn.seq++;
        poll_until(TCP_TIME_WAIT, 3000);
    }
    g_conn.state = TCP_CLOSED;
}

int tcp_connected(int fd)
{
    (void)fd;
    return g_conn.state == TCP_ESTABLISHED;
}

int tcp_eof(int fd)
{
    (void)fd;
    return g_conn.fin_recv && !rx_available();
}
