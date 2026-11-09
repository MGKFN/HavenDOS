/*
 * ipv4.c — IPv4 send/receive for HavenDOS v0.6.8
 */
#include "../include/net.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/netstat.h"
#include "pit.h"   /* v0.8.8.8: was a hand-copied extern */

static uint16_t ip_id = 0x1234;

int  eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
              const void *payload, uint16_t payload_len);
int  arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN]);
void arp_request(uint32_t target_ip);
void icmp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len);
void udp_handle (uint32_t src_ip, const uint8_t *payload, uint16_t len);
void tcp_handle (uint32_t src_ip, const uint8_t *payload, uint16_t len);
extern void     net_poll(void);

/* ── Checksum ────────────────────────────────────────────────── */
/* v0.7.5: byte-wise accumulation.  The old version cast the buffer to
   uint16_t* and dereferenced it, which is an unaligned access whenever the
   IP header does not start on an even boundary — the same undefined-behaviour
   class that was corrupting the FAT16 BPB.  tcp_checksum() already avoided
   this; ip_checksum() did not. */
uint16_t ip_checksum(const void *data, uint16_t len)
{
    const uint8_t *p = (const uint8_t*)data;
    uint32_t sum = 0;
    while(len > 1){ sum += (uint32_t)(p[0] | ((uint32_t)p[1] << 8)); p += 2; len -= 2; }
    if(len) sum += (uint32_t)p[0];
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ── Resolve MAC — with retries and sleep ───────────────────── */
/* v0.7.5 FIX — two problems here.
 *
 * 1. RUNAWAY BLOCKING.  15 attempts x 40000 net_poll() iterations, with a
 *    sleep every 4096, meant a failed ARP could wedge the machine for
 *    minutes with no way out.  Now 4 attempts x ~250 ms.
 *
 * 2. RE-ENTRANCY.  net_poll() -> eth_poll() -> ip_handle() -> tcp_handle()
 *    -> tcp_send_seg() -> ip_send() re-enters ip_send while the OUTER call
 *    still has its packet half-built in the shared static frame[].  The
 *    outer eth_send() then transmitted the inner packet's bytes under the
 *    outer packet's length.  A depth guard now makes nested sends fail
 *    cleanly instead of corrupting the wire. */
int g_ip_send_depth = 0;

static int resolve_mac(uint32_t dst_ip, uint8_t mac_out[ETH_ALEN])
{
    uint32_t my_ip = net_get_ip();
    uint32_t mask  = net_get_mask();
    uint32_t gw    = net_get_gw();
    uint32_t next_hop = ((dst_ip & mask) == (my_ip & mask)) ? dst_ip : gw;

    if(arp_lookup(next_hop, mac_out)) return 1;

    /* Never poll from inside a nested send — that is what corrupts frame[]. */
    if(g_ip_send_depth > 1) return 0;

    for(int attempt = 0; attempt < 4; attempt++){
        NS_INC(arp_req_tx);
        arp_request(next_hop);
        for(int i = 0; i < 250; i++){
            net_poll();
            if(arp_lookup(next_hop, mac_out)) return 1;
            sleep_ms(1);
        }
    }
    NS_INC(arp_fail);
    return 0;
}

/* ── IP send ─────────────────────────────────────────────────── */
int ip_send(uint32_t dst_ip, uint8_t proto,
            const void *payload, uint16_t payload_len)
{
    static uint8_t frame[1500];
    uint16_t total = (uint16_t)(sizeof(ip_hdr_t) + payload_len);
    if(total > sizeof(frame)) return -1;

    /* Refuse re-entrant sends rather than scribbling over frame[]. */
    if(g_ip_send_depth){ NS_INC(ip_reentrant); return -3; }
    g_ip_send_depth++;

    ip_hdr_t *hdr = (ip_hdr_t*)frame;
    hdr->ver_ihl    = 0x45;
    hdr->dscp       = 0;
    hdr->total_len  = htons(total);
    hdr->id         = htons(ip_id++);
    hdr->flags_frag = htons(0x4000);   /* DF bit set — we don't support reassembly */
    hdr->ttl        = 64;
    hdr->proto      = proto;
    hdr->checksum   = 0;
    hdr->src        = net_get_ip();
    hdr->dst        = dst_ip;
    hdr->checksum   = ip_checksum(hdr, sizeof(ip_hdr_t));

    memcpy(frame + sizeof(ip_hdr_t), payload, payload_len);

    uint8_t dst_mac[ETH_ALEN];
    if(!resolve_mac(dst_ip, dst_mac)){ g_ip_send_depth--; return -2; }

    int r = eth_send(dst_mac, ETH_TYPE_IP, frame, total);
    if(r >= 0) NS_INC(ip_tx);
    g_ip_send_depth--;
    return r;
}

/* ── IP receive ──────────────────────────────────────────────── */
void ip_handle(const uint8_t *payload, uint16_t len)
{
    if(len < sizeof(ip_hdr_t)) return;
    const ip_hdr_t *hdr = (const ip_hdr_t*)payload;

    /* BUG FIX: verify IPv4 version */
    if((hdr->ver_ihl & 0xF0) != 0x40) return;

    uint8_t ihl = (hdr->ver_ihl & 0x0F) * 4;
    if(ihl < 20 || ihl > len) return;

    /* BUG FIX: verify IPv4 header checksum — drop corrupted packets */
    if(ip_checksum(hdr, ihl) != 0){ NS_INC(ip_bad_csum); return; }
    NS_INC(ip_rx);

    /* BUG FIX: validate total_len field.
       total_len must be: >= ihl (so upper-layer length >= 0)
       and <= the actual bytes we received (no claiming extra data). */
    uint16_t total_len = ntohs(hdr->total_len);
    if(total_len < ihl){ NS_INC(ip_bad_len); return; }
    if(total_len > len){ NS_INC(ip_bad_len); return; }

    /* BUG FIX: reject fragmented packets — we have no reassembly.
       MF bit set (0x2000) or non-zero fragment offset means a fragment. */
    uint16_t flags_frag = ntohs(hdr->flags_frag);
    if((flags_frag & 0x2000) || (flags_frag & 0x1FFF)){ NS_INC(ip_frag_drop); return; }

    uint32_t my_ip = net_get_ip();
    if(hdr->dst != my_ip && hdr->dst != 0xFFFFFFFFu){ NS_INC(ip_not_mine); return; }

    const uint8_t *upper = payload + ihl;
    uint16_t       ulen  = (uint16_t)(total_len - ihl);
    uint32_t       src_ip = hdr->src;

    if(hdr->proto == IP_PROTO_ICMP) icmp_handle(src_ip, upper, ulen);
    else if(hdr->proto == IP_PROTO_UDP)  udp_handle(src_ip, upper, ulen);
    else if(hdr->proto == IP_PROTO_TCP)  tcp_handle(src_ip, upper, ulen);
}
