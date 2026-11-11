/* test_dhcp.c -- DHCP, end to end through the real IP and UDP layers.
 *
 * v0.8.9.8. DHCP had never worked in this tree, on any network, and
 * three separate defects each stopped it on their own:
 *
 *   1. ipv4.c's resolve_mac() ARPed for 255.255.255.255. Nobody owns
 *      the broadcast address, nobody answered, and ip_send() gave up
 *      before the DISCOVER reached the wire. A packet capture of QEMU's
 *      link showed eight ARP requests and not one UDP datagram.
 *   2. udp.c refused to send more than 512 bytes, and a DHCP message
 *      is 548. (And would have cut any reply to 512 on the way in.)
 *   3. dhcp.c's parser demanded 548 bytes of reply -- the size of the
 *      buffer it BUILDS. Real servers send what they need; BOOTP's
 *      minimum is 300.
 *
 * Nothing noticed because the fallback address is QEMU's own, so a
 * machine that never got a lease behaved exactly like one that did --
 * until it was plugged into an iPhone, whose network is 172.20.10.x.
 *
 * This suite compiles net/ipv4.c, net/udp.c and net/dhcp.c as they
 * are and puts a simulated DHCP server under eth_send(). The server's
 * replies are 300 bytes unless a check says otherwise, because that is
 * the size a real one sends and the size the old code could not read.
 * Its behaviour (what it sends, where to, when it refuses) comes from
 * RFC 2131 and from QEMU's SLIRP, which this release was checked
 * against with a packet capture: DISCOVER, OFFER, REQUEST, ACK. An
 * iPhone's server has not been captured; the unicast and noise cases
 * below are what RFC 2131 permits it to do.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../include/net.h"
#include "../include/netstat.h"

netstat_t g_netstat;

/* ── the machine the stack thinks it is on ───────────────────────── */
static uint32_t my_ip, my_mask, my_gw, my_dns;
static int      cfg_src;
static uint8_t  my_mac[6] = { 0x0A, 0xC7, 0xB5, 0x93, 0x8C, 0xB1 };

uint32_t net_get_ip(void)   { return my_ip; }
uint32_t net_get_mask(void) { return my_mask; }
uint32_t net_get_gw(void)   { return my_gw; }
void     net_set_ip(uint32_t ip, uint32_t m, uint32_t g){ my_ip=ip; my_mask=m; my_gw=g; }
void     net_set_dns(uint32_t d){ my_dns = d; }
void     net_get_mac(uint8_t m[6]){ memcpy(m, my_mac, 6); }
void     net_config_source_set(int s){ cfg_src = s; }
int      net_config_source(void){ return cfg_src; }
void     sleep_ms(uint32_t ms){ (void)ms; }
int      arp_lookup(uint32_t ip, uint8_t mac[6]){ (void)ip; (void)mac; return 0; }
static int arp_requests;
void     arp_request(uint32_t ip){ (void)ip; arp_requests++; }
void     icmp_handle(uint32_t s, const uint8_t *p, uint16_t l){ (void)s;(void)p;(void)l; }
void     tcp_handle(uint32_t s, const uint8_t *p, uint16_t l){ (void)s;(void)p;(void)l; }

void ip_handle(const uint8_t *payload, uint16_t len);

/* ── the wire ────────────────────────────────────────────────────── */
#define IP4(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))
#define SRV_IP   IP4(172,20,10,1)
#define OFFER_IP IP4(172,20,10,2)
#define NET_MASK IP4(255,255,255,240)

static struct {
    int     discovers, requests, sent_frames;
    int     last_bcast_mac;               /* dst MAC of the last frame */
    int     last_udp_payload;             /* bytes of DHCP sent        */
    /* behaviour */
    int     reply_len;                    /* total DHCP bytes to send  */
    int     unicast;                      /* reply to yiaddr, not bcast*/
    int     nak;                          /* NAK the REQUEST           */
    int     silent;                       /* never answer              */
    int     noise;                        /* mDNS after every reply    */
    int     wrong_mac;                    /* replies for another host  */
    int     ack_no_router;                /* ACK leaves out option 3   */
    int     offer_twice;                  /* re-send OFFER, no ACK     */
} sv;

/* Frames waiting to be "received" by net_poll(). */
static uint8_t  q_buf[8][1600];
static uint16_t q_len[8];
static int      q_n;

static void put32(uint8_t *p, uint32_t v){ memcpy(p, &v, 4); }

static uint16_t csum(const uint8_t *p, int n)
{
    uint32_t s = 0;
    for(int i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i+1]);
    if(n & 1) s += (uint32_t)p[n-1] << 8;
    while(s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/* An IPv4 packet carrying UDP, queued for net_poll(). */
static void queue_udp(uint32_t src, uint32_t dst, uint16_t sp, uint16_t dp,
                      const uint8_t *data, int dlen)
{
    uint8_t *p = q_buf[q_n];
    int tot = 20 + 8 + dlen;
    memset(p, 0, (size_t)tot);
    p[0] = 0x45; p[2] = (uint8_t)(tot >> 8); p[3] = (uint8_t)tot;
    p[8] = 64; p[9] = 17;
    put32(p + 12, src); put32(p + 16, dst);
    { uint16_t c = csum(p, 20); p[10] = (uint8_t)(c >> 8); p[11] = (uint8_t)c; }
    p[20] = (uint8_t)(sp >> 8); p[21] = (uint8_t)sp;
    p[22] = (uint8_t)(dp >> 8); p[23] = (uint8_t)dp;
    p[24] = (uint8_t)((8 + dlen) >> 8); p[25] = (uint8_t)(8 + dlen);
    memcpy(p + 28, data, (size_t)dlen);
    q_len[q_n++] = (uint16_t)tot;
}

static void server_reply(const uint8_t *req, int type, uint32_t xid_be)
{
    uint8_t r[1400];
    int n = sv.reply_len, o = 240;
    memset(r, 0, sizeof r);
    r[0] = 2; r[1] = 1; r[2] = 6;
    memcpy(r + 4, &xid_be, 4);
    r[10] = 0x80;                                   /* flags as sent */
    if(type != 6) put32(r + 16, OFFER_IP);          /* yiaddr */
    memcpy(r + 28, req + 28, 16);                   /* chaddr */
    if(sv.wrong_mac) r[28] ^= 0x55;
    r[236] = 0x63; r[237] = 0x82; r[238] = 0x53; r[239] = 0x63;
    r[o++] = 53; r[o++] = 1; r[o++] = (uint8_t)type;
    r[o++] = 54; r[o++] = 4; put32(r + o, SRV_IP); o += 4;
    if(type != 6){
        r[o++] = 1; r[o++] = 4; put32(r + o, NET_MASK); o += 4;
        if(!(type == 5 && sv.ack_no_router)){
            r[o++] = 3; r[o++] = 4; put32(r + o, SRV_IP); o += 4;
        }
        r[o++] = 6; r[o++] = 4; put32(r + o, SRV_IP); o += 4;
        r[o++] = 51; r[o++] = 4; r[o++] = 0; r[o++] = 1; r[o++] = 0x51; r[o++] = 0x80;
    }
    r[o++] = 255;
    if(n < o) n = o;
    queue_udp(SRV_IP, sv.unicast && type != 6 ? OFFER_IP : 0xFFFFFFFFu,
              67, 68, r, n);
    if(sv.noise){
        /* mDNS is multicast and IP drops it before UDP ever sees it;
           a NetBIOS name broadcast reaches UDP and is what would have
           overwritten the reply in the shared slot. Both, in that
           order, in the same poll as the reply. */
        static const uint8_t mdns[40] = { 0, 0, 0x84, 0 };
        static const uint8_t nbns[50] = { 0x12, 0x34, 0x01, 0x10 };
        queue_udp(IP4(172,20,10,1), IP4(224,0,0,251), 5353, 5353, mdns, 40);
        queue_udp(IP4(172,20,10,3), 0xFFFFFFFFu, 137, 137, nbns, 50);
    }
}

int eth_send(const uint8_t dst[6], uint16_t type, const void *payload, uint16_t len)
{
    const uint8_t *ip = payload;
    sv.sent_frames++;
    sv.last_bcast_mac = (dst[0] & dst[1] & dst[2] & dst[3] & dst[4] & dst[5]) == 0xFF;
    if(type != 0x0800 || len < 28 || ip[9] != 17) return 0;
    {
        const uint8_t *u = ip + 20, *b = ip + 28;
        int dlen = len - 28, mtype = 0;
        if(((u[2] << 8) | u[3]) != 67) return 0;
        sv.last_udp_payload = dlen;
        for(int o = 240; o + 2 < dlen && b[o] != 255; ){
            if(b[o] == 0){ o++; continue; }
            if(b[o] == 53) mtype = b[o + 2];
            o += 2 + b[o + 1];
        }
        if(sv.silent) return 0;
        {
            uint32_t xid; memcpy(&xid, b + 4, 4);
            if(mtype == 1){ sv.discovers++; server_reply(b, 2, xid); }
            if(mtype == 3){
                sv.requests++;
                if(sv.nak)              server_reply(b, 6, xid);
                else if(sv.offer_twice) server_reply(b, 2, xid);
                else                    server_reply(b, 5, xid);
            }
        }
    }
    return 0;
}

/* Everything queued arrives in ONE poll -- the way eth_poll() drains up
   to sixteen frames a call, which is what let a broadcast overwrite an
   OFFER before anyone read it. */
void net_poll(void)
{
    int n = q_n;
    q_n = 0;
    for(int i = 0; i < n; i++) ip_handle(q_buf[i], q_len[i]);
}

#include "../net/ipv4.c"
#include "../net/udp.c"
#include "../net/dhcp.c"

/* ── harness ─────────────────────────────────────────────────────── */
static int total, fails;
static void head(const char *s){ printf("  %s\n", s); }
static void ck(const char *what, int ok, long got)
{
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-52s got %ld\n", what, got); }
}

static void fresh(void)
{
    memset(&sv, 0, sizeof sv);
    sv.reply_len = 300;
    q_n = 0; arp_requests = 0;
    /* QEMU's fallback, which is where the machine sits before a lease */
    my_ip = IP4(10,0,2,15); my_mask = IP4(255,255,255,0);
    my_gw = IP4(10,0,2,2); my_dns = 0; cfg_src = NET_CFG_FALLBACK;
}

int main(void)
{
    printf("\n=== test_dhcp: DHCP through the real IP and UDP layers ===\n");

    head("the DISCOVER reaches the wire at all");
    fresh();
    sv.silent = 1;
    dhcp_request();
    ck("a DISCOVER was sent", sv.sent_frames > 0, sv.sent_frames);
    ck("  ... to the Ethernet broadcast address", sv.last_bcast_mac, 0);
    ck("  ... without asking ARP who owns 255.255.255.255",
       arp_requests == 0, arp_requests);
    ck("  ... carrying the whole 548-byte message",
       sv.last_udp_payload == 548, sv.last_udp_payload);

    head("a real server's 300-byte replies give the machine an address");
    fresh();
    ck("dhcp_request() succeeds", dhcp_request() == 1, 0);
    ck("the address is the one offered", my_ip == OFFER_IP, (long)my_ip);
    ck("the mask came from the server", my_mask == NET_MASK, (long)my_mask);
    ck("the gateway too", my_gw == SRV_IP, (long)my_gw);
    ck("and the DNS server", my_dns == SRV_IP, (long)my_dns);
    ck("the configuration says DHCP, not the fallback",
       cfg_src == NET_CFG_DHCP, cfg_src);
    ck("one DISCOVER and one REQUEST", sv.discovers == 1 && sv.requests == 1,
       sv.discovers * 10 + sv.requests);

    head("noise on the link does not overwrite the answer");
    fresh();
    sv.noise = 1;
    ck("broadcast noise behind every reply, same poll: still a lease",
       dhcp_request() == 1 && my_ip == OFFER_IP, (long)my_ip);
    ck("  ... and the noise was counted, not stored",
       udp_unwanted_count() > 0, (long)udp_unwanted_count());

    head("a server that ignores the broadcast flag");
    fresh();
    sv.unicast = 1;
    ck("replies sent to the offered address are still taken",
       dhcp_request() == 1 && my_ip == OFFER_IP, (long)my_ip);

    head("a NAK is a refusal, not an acknowledgement");
    fresh();
    sv.nak = 1;
    ck("dhcp_request() fails", dhcp_request() == 0, 0);
    ck("  ... and the old configuration is put back",
       my_ip == IP4(10,0,2,15) && cfg_src == NET_CFG_FALLBACK, (long)my_ip);
    ck("  ... and the refusal is counted", dhcp_stat_naks() > 0,
       (long)dhcp_stat_naks());

    head("an OFFER sent again is not an ACK");
    fresh();
    sv.offer_twice = 1;
    ck("no lease without an ACK", dhcp_request() == 0, 0);

    head("replies meant for somebody else");
    fresh();
    sv.wrong_mac = 1;
    ck("a reply for another hardware address is ignored",
       dhcp_request() == 0 && my_ip == IP4(10,0,2,15), (long)my_ip);

    head("a reply too short to be DHCP");
    fresh();
    {
        /* Built by hand: 200 bytes is shorter than the fixed header. */
        uint8_t r[200] = { 2, 1, 6 };
        my_ip = 0;
        queue_udp(SRV_IP, 0xFFFFFFFFu, 67, 68, r, sizeof r);
        net_poll();
        {
            int type = 0; uint32_t a, b, c, d, e;
            ck("is refused by the parser",
               parse_reply(udp_recv_data(), udp_recv_len(), &type,
                           &a, &b, &c, &d, &e) == 0, udp_recv_len());
        }
    }

    head("an ACK without a router keeps the OFFER's");
    fresh();
    sv.ack_no_router = 1;
    ck("the gateway is the one the OFFER named",
       dhcp_request() == 1 && my_gw == SRV_IP, (long)my_gw);

    head("after the lease, the subnet's own broadcast needs no ARP either");
    fresh();
    dhcp_request();
    arp_requests = 0;
    udp_send(IP4(172,20,10,15), 5000, 5000, "x", 1);
    ck("172.20.10.15 on a /28 goes to FF:FF:FF:FF:FF:FF",
       sv.last_bcast_mac && arp_requests == 0, arp_requests);

    head("the slot is shared again once nobody is waiting");
    fresh();
    dhcp_request();
    {
        static const uint8_t x[4] = { 1, 2, 3, 4 };
        queue_udp(SRV_IP, OFFER_IP, 9, 4444, x, 4);
        net_poll();
        ck("a datagram for any port is kept when no one holds the slot",
           udp_recv_len() == 4 && udp_recv_port() == 4444, udp_recv_len());
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
