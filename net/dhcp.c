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
#include "pit.h"   /* v0.8.8.8: was a hand-copied extern */

extern int      udp_send(uint32_t, uint16_t, uint16_t, const void*, uint16_t);
extern void     udp_recv_clear(void);
extern uint16_t udp_recv_len(void);
extern uint16_t udp_recv_port(void);
extern uint32_t udp_recv_src_ip(void);
extern const uint8_t *udp_recv_data(void);
extern void     net_poll(void);
extern void     net_get_mac(uint8_t[6]);
extern void     udp_want_port(uint16_t);

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

/* ── v0.8.9.8: WHAT A REAL SERVER SENDS ──────────────────────────────
 * The fixed BOOTP header is 236 bytes and the magic cookie four more;
 * everything after that is options, and a server sends as many as it
 * has. This parser demanded sizeof(dhcp_pkt_t) -- 548 -- which is the
 * size of the buffer this file BUILDS, not of what anybody sends back.
 * BOOTP's own minimum is 300. So every reply a real server would send
 * was refused, as well as every one cut to 512 by udp.c on the way in.
 *
 * It also never read option 53, so an ACK was as good as an OFFER and
 * a NAK as good as either. The type is returned now and checked. */
#define DHCP_FIXED_LEN   236u
#define DHCP_MIN_REPLY   (DHCP_FIXED_LEN + 4u)
#define DHCPOFFER        2
#define DHCPACK          5
#define DHCPNAK          6

static int parse_reply(const uint8_t *buf, uint16_t len, int *type,
                       uint32_t *yip, uint32_t *mask,
                       uint32_t *gw, uint32_t *dns, uint32_t *srv)
{
    uint8_t mac[6];
    if(len < DHCP_MIN_REPLY) return 0;
    const dhcp_pkt_t *p = (const dhcp_pkt_t*)buf;
    if(ntohl(p->magic) != DHCP_MAGIC) return 0;
    if(ntohl(p->xid)   != g_xid)      return 0;
    if(p->op != 2) return 0;   /* must be BOOTREPLY */
    /* A broadcast reply reaches every client on the link; the xid makes
       a collision unlikely and the hardware address makes it certain. */
    net_get_mac(mac);
    if(memcmp(p->chaddr, mac, 6) != 0) return 0;

    /* yiaddr is your IP, stored big-endian in packet */
    uint8_t *yb = (uint8_t*)&p->yiaddr;
    *yip = (uint32_t)yb[0]|((uint32_t)yb[1]<<8)|
           ((uint32_t)yb[2]<<16)|((uint32_t)yb[3]<<24);
    *mask = 0; *gw = 0; *dns = 0; *srv = 0; *type = 0;

    /* Parse options — RFC 2132 TLV walk, bounded by what ARRIVED */
    const uint8_t *opt = buf + DHCP_MIN_REPLY;
    const uint8_t *end = buf + len;
    int iters = 0;
    while(opt < end && *opt != 255 && iters < 256) {
        iters++;
        if(*opt == 0) { opt++; continue; }   /* pad byte */
        if(opt + 1 >= end) break;
        uint8_t tag  = *opt++;
        uint8_t olen = *opt++;
        if(opt + olen > end) break;
        if(tag == 53 && olen >= 1) *type = opt[0];
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

/* Wait up to `ms` for a reply of type `want` (or a NAK). Returns the
   type that arrived, or 0. */
#define DHCP_WAIT_MS 2000
static int wait_reply(int want, uint32_t ms, uint32_t *yip, uint32_t *mask,
                      uint32_t *gw, uint32_t *dns, uint32_t *srv)
{
    static uint8_t rxbuf[1472];
    for(uint32_t t = 0; t < ms; t += 10){
        net_poll();
        if(udp_recv_len() > 0 && udp_recv_port() == DHCP_CLIENT_PORT){
            uint16_t rlen = udp_recv_len();
            int type = 0;
            if(rlen > sizeof rxbuf) rlen = sizeof rxbuf;
            memcpy(rxbuf, udp_recv_data(), rlen);
            udp_recv_clear();
            if(parse_reply(rxbuf, rlen, &type, yip, mask, gw, dns, srv)){
                if(type == DHCPNAK) return DHCPNAK;
                if(type == want && (want == DHCPNAK || *yip)) return type;
            }
        }
        sleep_ms(10);
    }
    return 0;
}

static uint32_t g_dhcp_offers, g_dhcp_acks, g_dhcp_naks, g_dhcp_sent;
uint32_t dhcp_stat_sent(void)  { return g_dhcp_sent;   }
uint32_t dhcp_stat_offers(void){ return g_dhcp_offers; }
uint32_t dhcp_stat_acks(void)  { return g_dhcp_acks;   }
uint32_t dhcp_stat_naks(void)  { return g_dhcp_naks;   }

int dhcp_request(void)
{
    static uint8_t pkt[sizeof(dhcp_pkt_t)];
    uint32_t bcast = 0xFFFFFFFF;
    int ok = 0;

    /* Temporarily set IP to 0.0.0.0 for DISCOVER broadcast */
    uint32_t save_ip   = net_get_ip();
    uint32_t save_mask = net_get_mask();
    uint32_t save_gw   = net_get_gw();
    net_set_ip(0, 0, 0);
    udp_want_port(DHCP_CLIENT_PORT);

    for(int attempt = 0; attempt < 2 && !ok; attempt++){
        uint32_t offered_ip=0, mask=0, gw=0, dns=0, srv=0;
        uint32_t yip2=0, m2=0, g2=0, d2=0, s2=0;
        uint16_t plen;
        int t;

        g_xid++;                     /* a fresh transaction each time */
        plen = build_discover(pkt);
        udp_recv_clear();
        if(udp_send(bcast, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, plen) < 0)
            continue;
        g_dhcp_sent++;
        if(wait_reply(DHCPOFFER, DHCP_WAIT_MS, &offered_ip, &mask, &gw,
                      &dns, &srv) != DHCPOFFER)
            continue;
        g_dhcp_offers++;

        plen = build_request(pkt, offered_ip, srv);
        udp_recv_clear();
        if(udp_send(bcast, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, plen) < 0)
            continue;
        t = wait_reply(DHCPACK, DHCP_WAIT_MS, &yip2, &m2, &g2, &d2, &s2);
        if(t == DHCPNAK){ g_dhcp_naks++; continue; }
        if(t != DHCPACK) continue;
        g_dhcp_acks++;

        /* Apply config. An ACK that leaves out the mask or router is
           filled from the OFFER first, and only then from the old
           configuration -- the old one belongs to another network. */
        if(!m2) m2 = mask ? mask : save_mask;
        if(!g2) g2 = gw ? gw : save_gw;
        if(!d2) d2 = dns;
        net_set_ip(yip2, m2, g2);
        net_config_source_set(NET_CFG_DHCP);
        if(d2) net_set_dns(d2);
        ok = 1;
    }

    udp_want_port(0);
    if(ok) return 1;
    /* DHCP failed — restore static config */
    net_set_ip(save_ip, save_mask, save_gw);
    return 0;
}
