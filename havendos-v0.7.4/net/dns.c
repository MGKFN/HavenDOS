/*
 * dns.c — DNS resolver for HavenDOS v0.6.8
 *
 * BUG FIXES vs v0.6.8:
 *   - build_query: bounds check on pos (prevents buffer overflow on long hostnames)
 *   - parse_response: pointer-compression loop guard (prevents infinite loop on
 *     malformed response with a self-referencing pointer)
 *   - parse_response: rdlen validated against remaining buffer before use
 *   - DHCP olen=0 is handled upstream; DNS itself does not use DHCP options
 */

#include "../include/net.h"
#include "../include/dns.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/errors.h"

extern int      udp_send(uint32_t, uint16_t, uint16_t, const void*, uint16_t);
extern void     udp_recv_clear(void);
extern uint16_t udp_recv_len(void);
extern uint16_t udp_recv_port(void);
extern const uint8_t *udp_recv_data(void);
extern void     net_poll(void);
extern void     sleep_ms(uint32_t);

#define DNS_PORT    53
#define DNS_LOCAL   5353
#define DNS_BUF     512
#define DNS_MAX_HOST 253    /* RFC 1035 max hostname length */

static uint32_t g_dns_server = 0;
#define DNS_DEFAULT  ((uint32_t)(8|(8<<8)|(8<<16)|(8<<24)))

void     net_set_dns(uint32_t ip){ g_dns_server = ip; }
uint32_t net_get_dns(void){ return g_dns_server ? g_dns_server : DNS_DEFAULT; }

/* ── Build DNS query ─────────────────────────────────────────────── */
static uint16_t build_query(uint8_t *buf, const char *host, uint16_t txid)
{
    buf[0]=(txid>>8)&0xFF; buf[1]=txid&0xFF;
    buf[2]=0x01; buf[3]=0x00;
    buf[4]=0;    buf[5]=1;
    buf[6]=0;    buf[7]=0;
    buf[8]=0;    buf[9]=0;
    buf[10]=0;   buf[11]=0;

    int pos = 12;
    const char *p = host;
    while(*p) {
        const char *dot = p;
        while(*dot && *dot != '.') dot++;
        int lablen = (int)(dot - p);
        if(lablen == 0 || lablen > 63) return 0;  /* invalid label */

        /* BUG FIX: bounds check — 1 (len) + lablen + 5 (root+type+class) */
        if(pos + 1 + lablen + 5 > DNS_BUF) {
            kernel_log(ERR_NET_MALFORMED, "dns: hostname too long for query buffer");
            return 0;
        }

        buf[pos++] = (uint8_t)lablen;
        for(int i = 0; i < lablen; i++) buf[pos++] = p[i];
        p = *dot ? dot + 1 : dot;
    }
    if(pos + 5 > DNS_BUF) return 0;
    buf[pos++] = 0;          /* root label */
    buf[pos++] = 0; buf[pos++] = 1;   /* QTYPE  = A */
    buf[pos++] = 0; buf[pos++] = 1;   /* QCLASS = IN */
    return (uint16_t)pos;
}

/* ── Parse A record ──────────────────────────────────────────────── */
static uint32_t parse_response(const uint8_t *buf, uint16_t len, uint16_t txid)
{
    if(len < 12) return 0;

    uint16_t rid = (uint16_t)(((uint16_t)buf[0]<<8)|buf[1]);
    if(rid != txid) return 0;
    if(!(buf[2] & 0x80)) return 0;   /* not a response */

    /* Check RCODE — non-zero means server error */
    if(buf[3] & 0x0F) return 0;

    uint16_t ancount = (uint16_t)(((uint16_t)buf[6]<<8)|buf[7]);
    if(ancount == 0) return 0;

    /* Skip question section */
    int pos = 12;
    /* Walk QNAME — with pointer-compression guard */
    int steps = 0;
    while(pos < len && buf[pos]) {
        if((buf[pos] & 0xC0) == 0xC0) {
            /* BUG FIX: pointer must point earlier in packet (RFC 1035) */
            if(pos + 1 >= len) return 0;
            int ptr = ((buf[pos] & 0x3F) << 8) | buf[pos+1];
            if(ptr >= pos) return 0;   /* forward pointer — reject */
            pos += 2;
            goto skip_qtype;
        }
        uint8_t lablen = buf[pos];
        if(lablen > 63) return 0;      /* invalid label length */
        pos += lablen + 1;
        if(++steps > 128) return 0;    /* loop guard */
    }
    if(pos >= len) return 0;
    pos++;  /* null terminator */
skip_qtype:
    if(pos + 4 > len) return 0;
    pos += 4;  /* QTYPE + QCLASS */

    /* Walk answer records */
    for(int i = 0; i < ancount && pos < len; i++) {
        /* NAME — pointer or inline labels */
        if((buf[pos] & 0xC0) == 0xC0) {
            if(pos + 1 >= len) return 0;
            pos += 2;
        } else {
            steps = 0;
            while(pos < len && buf[pos]) {
                if((buf[pos] & 0xC0) == 0xC0) { pos += 2; break; }
                pos += buf[pos] + 1;
                if(++steps > 128) return 0;
            }
            if(pos < len && !(buf[pos] & 0xC0)) pos++;  /* null terminator */
        }
        if(pos + 10 > len) return 0;

        uint16_t rtype = (uint16_t)(((uint16_t)buf[pos]<<8)|buf[pos+1]);
        uint16_t rdlen = (uint16_t)(((uint16_t)buf[pos+8]<<8)|buf[pos+9]);
        pos += 10;

        /* BUG FIX: validate rdlen before using as offset */
        if(pos + rdlen > len) return 0;

        if(rtype == 1 && rdlen == 4) {
            /* A record: return IP in little-endian (HavenDOS convention) */
            return (uint32_t)buf[pos]
                 | ((uint32_t)buf[pos+1] << 8)
                 | ((uint32_t)buf[pos+2] << 16)
                 | ((uint32_t)buf[pos+3] << 24);
        }
        pos += rdlen;
    }
    return 0;
}

/* ── Public: dns_resolve ─────────────────────────────────────────── */
uint32_t dns_resolve(const char *hostname)
{
    if(!hostname || !hostname[0]) return 0;

    /* Fast-path: looks like a dotted-quad */
    int is_ip = 1;
    for(int i = 0; hostname[i]; i++)
        if(!(hostname[i] >= '0' && hostname[i] <= '9') && hostname[i] != '.')
            { is_ip = 0; break; }
    if(is_ip) return ip_from_str(hostname);

    /* BUG FIX: reject hostnames exceeding RFC max */
    if(strlen(hostname) > DNS_MAX_HOST) {
        kernel_log(ERR_NET_MALFORMED, "dns: hostname exceeds 253 chars");
        return 0;
    }

    static uint8_t qbuf[DNS_BUF];
    static uint16_t txid = 0x4400;
    txid++;

    uint16_t qlen = build_query(qbuf, hostname, txid);
    if(qlen == 0) return 0;

    uint32_t dns_ip = net_get_dns();

    for(int attempt = 0; attempt < 3; attempt++) {
        udp_recv_clear();
        udp_send(dns_ip, DNS_LOCAL, DNS_PORT, qbuf, qlen);

        for(int ms = 0; ms < 3000; ms += 5) {
            net_poll();
            if(udp_recv_len() > 0 && udp_recv_port() == DNS_LOCAL) {
                uint32_t ip = parse_response(udp_recv_data(),
                                             udp_recv_len(), txid);
                udp_recv_clear();
                if(ip) return ip;
            }
            sleep_ms(5);
        }
    }

    kernel_log(ERR_NET_DNS_FAILED, "dns: resolution failed");
    return 0;
}
