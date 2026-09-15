/*
 * arp.c — ARP for HavenDOS v0.6.8
 * Static table, 16 entries. Handles requests + replies.
 */
#include "../include/net.h"
#include "../include/string.h"
#include "../include/types.h"

#define ARP_TABLE_SIZE 16
static const uint8_t BCAST_MAC[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ARP cache */
static struct {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    int      valid;
} arp_table[ARP_TABLE_SIZE];

static int arp_count = 0;

/* Forward decl */
int eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
             const void *payload, uint16_t payload_len);

static void arp_cache_add(uint32_t ip, const uint8_t mac[ETH_ALEN])
{
    /* Update if exists */
    for(int i=0;i<ARP_TABLE_SIZE;i++){
        if(arp_table[i].valid && arp_table[i].ip == ip){
            for(int j=0;j<ETH_ALEN;j++) arp_table[i].mac[j]=mac[j];
            return;
        }
    }
    /* Add new entry */
    int slot = arp_count % ARP_TABLE_SIZE;
    arp_table[slot].ip    = ip;
    arp_table[slot].valid = 1;
    for(int j=0;j<ETH_ALEN;j++) arp_table[slot].mac[j]=mac[j];
    arp_count++;
}

int arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN])
{
    for(int i=0;i<ARP_TABLE_SIZE;i++){
        if(arp_table[i].valid && arp_table[i].ip == ip){
            for(int j=0;j<ETH_ALEN;j++) mac_out[j]=arp_table[i].mac[j];
            return 1;
        }
    }
    return 0;
}

void arp_request(uint32_t target_ip)
{
    arp_pkt_t pkt;
    uint8_t   my_mac[ETH_ALEN];
    net_get_mac(my_mac);

    pkt.htype = htons(0x0001);
    pkt.ptype = htons(ETH_TYPE_IP);
    pkt.hlen  = ETH_ALEN;
    pkt.plen  = 4;
    pkt.oper  = htons(ARP_REQUEST);

    for(int i=0;i<ETH_ALEN;i++) pkt.sha[i] = my_mac[i];
    pkt.spa = net_get_ip();
    for(int i=0;i<ETH_ALEN;i++) pkt.tha[i] = 0;
    pkt.tpa = target_ip;

    eth_send(BCAST_MAC, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

void arp_handle(const uint8_t *payload, uint16_t len)
{
    if(len < (uint16_t)sizeof(arp_pkt_t)) return;
    const arp_pkt_t *pkt = (const arp_pkt_t*)payload;

    if(ntohs(pkt->htype) != 0x0001) return;
    if(ntohs(pkt->ptype) != ETH_TYPE_IP) return;

    /* BUG FIX: only learn valid sender addresses.
       Reject spa=0 (invalid) and spa=our own IP (spoofing attempt). */
    if(pkt->spa != 0 && pkt->spa != net_get_ip())
        arp_cache_add(pkt->spa, pkt->sha);

    /* If it's a request for our IP, reply */
    if(ntohs(pkt->oper) == ARP_REQUEST && pkt->tpa == net_get_ip()){
        arp_pkt_t reply;
        uint8_t my_mac[ETH_ALEN];
        net_get_mac(my_mac);

        reply.htype = htons(0x0001);
        reply.ptype = htons(ETH_TYPE_IP);
        reply.hlen  = ETH_ALEN;
        reply.plen  = 4;
        reply.oper  = htons(ARP_REPLY);
        for(int i=0;i<ETH_ALEN;i++) reply.sha[i] = my_mac[i];
        reply.spa = net_get_ip();
        for(int i=0;i<ETH_ALEN;i++) reply.tha[i] = pkt->sha[i];
        reply.tpa = pkt->spa;

        eth_send(pkt->sha, ETH_TYPE_ARP, &reply, sizeof(reply));
    }
}
