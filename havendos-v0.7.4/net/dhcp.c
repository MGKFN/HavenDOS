/*
 * dhcp.c — DHCP client for HavenDOS v0.6.8
 *
 * DISCOVER → OFFER → REQUEST → ACK
 * Configures ip/mask/gw/dns via net_set_ip() and net_set_dns().
 * All polling-based — no IRQ timers, safe on UTM SE TCG.
 *
 * Returns 1 on success, 0 on timeout (falls back to static config).
 */

#include "../include/net.h"
#include "../include/dns.h"
#include "../include/string.h"
#include "../include/types.h"

extern int      udp_send(uint32_t, uint16_t, uint16_t, const void*, uint16_t);
extern void     udp_recv_clear(void);
extern uint16_t udp_recv_len(void);
extern uint16_t udp_recv_port(void);
extern uint32_t udp_recv_src_ip(void);
extern const uint8_t *udp_recv_data(void);
extern void     net_poll(void);
extern void     sleep_ms(uint32_t);
extern void     net_get_mac(uint8_t[6]);

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC       0x63825363UL   /* big-endian in packet */

/* BOOTP/DHCP message structure (fields we care about) */
typedef struct {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[308];
} __attribute__((packed)) dhcp_pkt_t;

static uint32_t g_xid = 0xDEAD0601;

static uint16_t build_discover(uint8_t *buf)
{
    dhcp_pkt_t *p = (dhcp_pkt_t*)buf;
    memset(p, 0, sizeof(*p));
    p->op    = 1;      /* BOOTREQUEST */
    p->htype = 1;      /* Ethernet */
    p->hlen  = 6;
    p->xid   = htonl(g_xid);
    p->flags = htons(0x8000);   /* broadcast */
    net_get_mac(p->chaddr);
    p->magic = htonl(DHCP_MAGIC);

    uint8_t *opt = p->options;
    *opt++=53; *opt++=1; *opt++=1;     /* DHCP Message Type: DISCOVER */
    *opt++=55; *opt++=4;               /* Parameter Request List */
    *opt++=1;                          /*   Subnet Mask */
    *opt++=3;                          /*   Router */
    *opt++=6;                          /*   DNS */
    *opt++=15;                         /*   Domain Name */
    *opt++=255;                        /* END */
    return (uint16_t)(sizeof(dhcp_pkt_t));
}

static uint16_t build_request(uint8_t *buf, uint32_t offered_ip, uint32_t server_ip)
{
    dhcp_pkt_t *p = (dhcp_pkt_t*)buf;
    memset(p, 0, sizeof(*p));
    p->op    = 1;
    p->htype = 1;
    p->hlen  = 6;
    p->xid   = htonl(g_xid);
    p->flags = htons(0x8000);
    net_get_mac(p->chaddr);
    p->magic = htonl(DHCP_MAGIC);

    uint8_t *opt = p->options;
    *opt++=53; *opt++=1; *opt++=3;         /* DHCP Message Type: REQUEST */
    *opt++=50; *opt++=4;                   /* Requested IP */
    *opt++=(offered_ip)&0xFF;
    *opt++=(offered_ip>>8)&0xFF;
    *opt++=(offered_ip>>16)&0xFF;
    *opt++=(offered_ip>>24)&0xFF;
    *opt++=54; *opt++=4;                   /* Server Identifier */
    *opt++=(server_ip)&0xFF;
    *opt++=(server_ip>>8)&0xFF;
    *opt++=(server_ip>>16)&0xFF;
    *opt++=(server_ip>>24)&0xFF;
    *opt++=255;
    return (uint16_t)sizeof(dhcp_pkt_t);
}

/* Parse options from an OFFER or ACK, extract yiaddr/mask/gw/dns */
static int parse_offer(const uint8_t *buf, uint16_t len,
                        uint32_t *yip, uint32_t *mask,
                        uint32_t *gw, uint32_t *dns, uint32_t *srv)
{
    if(len < sizeof(dhcp_pkt_t)) return 0;
    const dhcp_pkt_t *p = (const dhcp_pkt_t*)buf;

    if(ntohl(p->magic) != DHCP_MAGIC) return 0;
    if(ntohl(p->xid)   != g_xid)      return 0;
    if(p->op != 2) return 0;   /* must be BOOTREPLY */

    /* yiaddr is your IP, stored big-endian in packet */
    uint8_t *yb = (uint8_t*)&p->yiaddr;
    *yip = (uint32_t)yb[0]|((uint32_t)yb[1]<<8)|
           ((uint32_t)yb[2]<<16)|((uint32_t)yb[3]<<24);

    *mask = 0; *gw = 0; *dns = 0; *srv = 0;

    /* Parse options — RFC 2132 TLV walk */
    const uint8_t *opt = p->options;
    const uint8_t *end = buf + len;
    int iters = 0;
    while(opt < end && *opt != 255 && iters < 256) {
        iters++;
        if(*opt == 0) { opt++; continue; }   /* pad byte */
        if(opt + 1 >= end) break;
        uint8_t tag  = *opt++;
        uint8_t olen = *opt++;
        /* BUG FIX: always validate bounds; olen=0 loop guard via iters */
        if(opt + olen > end) break;
        if(olen >= 4) {
            uint32_t val = (uint32_t)opt[0] | ((uint32_t)opt[1]<<8)
                         | ((uint32_t)opt[2]<<16) | ((uint32_t)opt[3]<<24);
            if(tag == 1)  *mask = val;
            if(tag == 3)  *gw   = val;
            if(tag == 6)  *dns  = val;
            if(tag == 54) *srv  = val;
        }
        opt += olen;
    }
    return 1;
}

int dhcp_request(void)
{
    static uint8_t pkt[sizeof(dhcp_pkt_t)];
    static uint8_t rxbuf[600];

    uint32_t bcast = 0xFFFFFFFF;

    /* Temporarily set IP to 0.0.0.0 for DISCOVER broadcast */
    uint32_t save_ip   = net_get_ip();
    uint32_t save_mask = net_get_mask();
    uint32_t save_gw   = net_get_gw();
    net_set_ip(0, 0, 0);

    uint16_t plen = build_discover(pkt);

    for(int attempt=0;attempt<3;attempt++){
        udp_recv_clear();
        udp_send(bcast, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, plen);

        /* Wait up to 4 seconds for OFFER */
        uint32_t offered_ip=0, mask=0, gw=0, dns=0, srv=0;
        for(int ms=0;ms<800;ms+=10){
            net_poll();
            if(udp_recv_len()>0 && udp_recv_port()==DHCP_CLIENT_PORT){
                memcpy(rxbuf, udp_recv_data(),
                       udp_recv_len()<600?udp_recv_len():600);
                uint16_t rlen=udp_recv_len();
                udp_recv_clear();
                if(parse_offer(rxbuf,rlen,&offered_ip,&mask,&gw,&dns,&srv)
                   && offered_ip)
                    goto send_request;
            }
            sleep_ms(10);
        }
        continue;

send_request:;
        /* Send REQUEST */
        plen = build_request(pkt, offered_ip, srv);
        udp_recv_clear();
        udp_send(bcast, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, plen);

        /* Wait for ACK (type=5) */
        for(int ms=0;ms<800;ms+=10){
            net_poll();
            if(udp_recv_len()>0 && udp_recv_port()==DHCP_CLIENT_PORT){
                memcpy(rxbuf, udp_recv_data(),
                       udp_recv_len()<600?udp_recv_len():600);
                uint16_t rlen=udp_recv_len();
                udp_recv_clear();
                uint32_t yip2=0,m2=0,g2=0,d2=0,s2=0;
                if(parse_offer(rxbuf,rlen,&yip2,&m2,&g2,&d2,&s2) && yip2){
                    /* Apply config */
                    if(!m2) m2=save_mask;
                    if(!g2) g2=save_gw;
                    net_set_ip(yip2, m2, g2);
                    if(d2) net_set_dns(d2);
                    return 1;
                }
            }
            sleep_ms(10);
        }
    }

    /* DHCP failed — restore static config */
    net_set_ip(save_ip, save_mask, save_gw);
    return 0;
}
