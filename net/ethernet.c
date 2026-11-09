/*
 * ethernet.c — Ethernet TX/RX for HavenDOS v0.6.8
 * Uses nic_send/nic_recv so works with RTL8139 or e1000.
 */
#include "../include/kpoll.h"
#include "../include/net.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/netstat.h"

void arp_handle(const uint8_t *payload, uint16_t len);
void ip_handle (const uint8_t *payload, uint16_t len);
extern int      nic_send(const void *data, uint16_t len);
extern uint16_t nic_recv(void *buf, uint16_t len);

static uint8_t eth_rx_buf[1518];

/* ── v0.8.8: bracketed for kernel_poll(), as a wrapper ───────────────
 * `frame` below is a static inside this function and eth_rx_buf above
 * it is a static in this file; the net poll clients send DHCP and read
 * the carrier, so a poll reached from inside a send would reuse both.
 * Wrapper, not an edited body -- tools/check_kpoll_guard.py. */
static int eth_send_inner(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
             const void *payload, uint16_t payload_len)
{
    static uint8_t frame[1518];
    if(payload_len + sizeof(eth_hdr_t) > sizeof(frame)) return -1;

    eth_hdr_t *hdr = (eth_hdr_t*)frame;
    uint8_t my_mac[ETH_ALEN];
    net_get_mac(my_mac);
    for(int i=0;i<ETH_ALEN;i++) hdr->dst[i]=dst[i];
    for(int i=0;i<ETH_ALEN;i++) hdr->src[i]=my_mac[i];
    hdr->type = htons(ethertype);
    memcpy(frame+sizeof(eth_hdr_t), payload, payload_len);
    int r = nic_send(frame, (uint16_t)(sizeof(eth_hdr_t)+payload_len));
    if(r < 0) NS_INC(eth_tx_fail); else NS_INC(eth_tx);
    return r;
}

int eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
             const void *payload, uint16_t payload_len)
{
    int r;
    kpoll_enter();
    r = eth_send_inner(dst, ethertype, payload, payload_len);
    kpoll_leave();
    return r;
}

/* eth_poll() is bracketed too, and it is the one that would bite
   soonest: it owns eth_rx_buf and hands it to arp_handle() and
   ip_handle(), which reply -- so a frame being dispatched is a send
   waiting to happen, with the receive buffer still live. */
static void eth_poll_inner(void);

void eth_poll(void)
{
    kpoll_enter();
    eth_poll_inner();
    kpoll_leave();
}

static void eth_poll_inner(void)
{
    /* Drain all pending frames in one poll call */
    for(int i = 0; i < 16; i++){
        uint16_t len = nic_recv(eth_rx_buf, sizeof(eth_rx_buf));
        if(len == 0) break;
        /* v0.7.12: was `break`, which abandoned the rest of the RX queue for
           this poll because of one runt frame.  Under load that turns a single
           malformed frame into dropped ACKs and stalled transfers. */
        if(len < sizeof(eth_hdr_t)){ NS_INC(eth_runt); continue; }
        NS_INC(eth_rx);
        eth_hdr_t *hdr = (eth_hdr_t*)eth_rx_buf;
        uint16_t   type = ntohs(hdr->type);
        const uint8_t *payload = eth_rx_buf+sizeof(eth_hdr_t);
        uint16_t   plen = (uint16_t)(len-sizeof(eth_hdr_t));
        if(type==ETH_TYPE_ARP) arp_handle(payload,plen);
        else if(type==ETH_TYPE_IP) ip_handle(payload,plen);
    }
}
