#pragma once
#include "types.h"

/* ── v0.7.6: network statistics ──────────────────────────────────────────
 * v0.7.5 rewrote large parts of the TCP/IP stack — retransmission, window
 * updates, MSS segmentation, checksum verification — and every one of those
 * paths is invisible in QEMU because SLIRP never drops, reorders or corrupts
 * anything.  They only exercise on real hardware (ThinkPad X140e, real LAN).
 *
 * These counters exist so a failure on real hardware produces a number rather
 * than a guess.  `netstat -s` prints them; `netstat -z` zeroes them.
 * ─────────────────────────────────────────────────────────────────────── */
typedef struct {
    /* link layer */
    uint32_t eth_rx, eth_tx, eth_tx_fail, eth_runt;
    /* ARP */
    uint32_t arp_req_tx, arp_reply_rx, arp_req_rx, arp_fail;
    /* IPv4 */
    uint32_t ip_rx, ip_tx, ip_bad_csum, ip_bad_len, ip_frag_drop,
             ip_not_mine, ip_reentrant;
    /* ICMP / UDP */
    uint32_t icmp_echo_rx, icmp_reply_rx, udp_rx, udp_tx;
    /* TCP */
    uint32_t tcp_seg_rx, tcp_seg_tx, tcp_bad_csum, tcp_ooo, tcp_rtx,
             tcp_rtx_giveup, tcp_win_update, tcp_rst_rx, tcp_rst_ignored,
             tcp_dup_fin, tcp_rx_drop;
    /* TLS */
    uint32_t tls_rec_rx, tls_rec_tx, tls_bad_pad, tls_short_rec, tls_alerts,
             tls_bad_mac;   /* v0.7.7 */
} netstat_t;

extern netstat_t g_netstat;

#define NS_INC(field) (g_netstat.field++)

void netstat_reset(void);
