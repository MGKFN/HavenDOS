/*
 * ethernet.c — Ethernet TX/RX for HavenDOS v0.6.8
 * Uses nic_send/nic_recv so works with RTL8139 or e1000.
 */
#include "../include/net.h"
#include "../include/string.h"
#include "../include/types.h"

void arp_handle(const uint8_t *payload, uint16_t len);
void ip_handle (const uint8_t *payload, uint16_t len);
extern int      nic_send(const void *data, uint16_t len);
extern uint16_t nic_recv(void *buf, uint16_t len);

static uint8_t eth_rx_buf[1518];

int eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
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
    return nic_send(frame, (uint16_t)(sizeof(eth_hdr_t)+payload_len));
}

void eth_poll(void)
{
    /* Drain all pending frames in one poll call */
    for(int i = 0; i < 16; i++){
        uint16_t len = nic_recv(eth_rx_buf, sizeof(eth_rx_buf));
        if(len < sizeof(eth_hdr_t)) break;
        eth_hdr_t *hdr = (eth_hdr_t*)eth_rx_buf;
        uint16_t   type = ntohs(hdr->type);
        const uint8_t *payload = eth_rx_buf+sizeof(eth_hdr_t);
        uint16_t   plen = (uint16_t)(len-sizeof(eth_hdr_t));
        if(type==ETH_TYPE_ARP) arp_handle(payload,plen);
        else if(type==ETH_TYPE_IP) ip_handle(payload,plen);
    }
}
