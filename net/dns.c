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
#include "pit.h"   /* v0.8.8.8: was a hand-copied extern */

extern int      udp_send(uint32_t, uint16_t, uint16_t, const void*, uint16_t);
extern void     udp_want_port(uint16_t);
extern void     udp_recv_clear(void);
extern uint16_t udp_recv_len(void);
extern uint16_t udp_recv_port(void);
extern const uint8_t *udp_recv_data(void);
extern void     net_poll(void);

#define DNS_PORT    53
#define DNS_LOCAL   5353
#define DNS_BUF     512
#define DNS_MAX_HOST 253    /* RFC 1035 max hostname length */

static uint32_t g_dns_server = 0;
/* v0.7.17: declared here because dns_cache_flush() below clears it and
   sits above the hosts-file block that defines the rest. */
static int g_hosts_loaded = 0;
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

/* ── v0.7.11 (roadmap 0.7.8): resolver cache, CNAME, multi-A ────────────
 * The old resolver returned the FIRST A record and nothing else: no cache, so
 * every wget/curl/bpkg call paid a full round trip; no CNAME handling, so any
 * name behind a CNAME (which is most CDN-hosted hosts) failed outright even
 * though the answer section contained the address; and no notion of TTL.
 * ─────────────────────────────────────────────────────────────────────── */
#define DNS_CACHE_N     8
#define DNS_MAX_ADDRS   4

/* v0.7.12: get_ticks() counts 10 ms units (kernel/pit.c), not milliseconds.
   The cache did `expires_tick = get_ticks() + ttl_s*1000`, which is ten times
   too many ticks — every entry lived 10x its TTL, so a record with a 60 s TTL
   was served from cache for ten minutes after the address changed.  The same
   confusion made `nslookup` report the remaining lifetime 10x too small. */
#define TICKS_PER_SEC 100u

typedef struct {
    char     host[DNS_MAX_HOST+1];
    uint32_t addr[DNS_MAX_ADDRS];
    int      naddr;
    uint32_t expires_tick;    /* get_ticks() value after which this is stale */
    int      used;
} dns_cache_ent_t;

static dns_cache_ent_t g_cache[DNS_CACHE_N];
static int             g_cache_next = 0;

/* out-params from the last parse */
static uint32_t g_last_addrs[DNS_MAX_ADDRS];
static int      g_last_naddr = 0;
static uint32_t g_last_ttl   = 0;

extern uint32_t get_ticks(void);

static dns_cache_ent_t *cache_find(const char *host){
    uint32_t now = get_ticks();
    for(int i=0;i<DNS_CACHE_N;i++){
        if(!g_cache[i].used) continue;
        if(strcmp(g_cache[i].host,host)!=0) continue;
        if((int32_t)(now - g_cache[i].expires_tick) >= 0){
            g_cache[i].used = 0;          /* expired */
            return 0;
        }
        return &g_cache[i];
    }
    return 0;
}

static void cache_put(const char *host, const uint32_t *a, int n, uint32_t ttl_s){
    if(n<=0) return;
    if(ttl_s < 30)    ttl_s = 30;         /* clamp: avoid thrashing  */
    if(ttl_s > 3600)  ttl_s = 3600;       /* clamp: avoid staleness  */
    dns_cache_ent_t *e = cache_find(host);
    if(!e){
        e = &g_cache[g_cache_next];
        g_cache_next = (g_cache_next+1)%DNS_CACHE_N;
    }
    strncpy(e->host,host,DNS_MAX_HOST); e->host[DNS_MAX_HOST]=0;
    if(n>DNS_MAX_ADDRS) n=DNS_MAX_ADDRS;
    for(int i=0;i<n;i++) e->addr[i]=a[i];
    e->naddr = n;
    e->expires_tick = get_ticks() + ttl_s*TICKS_PER_SEC;
    e->used = 1;
}

void dns_cache_flush(void){
    for(int i=0;i<DNS_CACHE_N;i++) g_cache[i].used=0;
    g_hosts_loaded = 0;      /* v0.7.17: re-read hosts.txt on the next lookup */
}

int dns_cache_dump(int idx, char *host, uint32_t *addrs, int *naddr, int *secs){
    if(idx<0||idx>=DNS_CACHE_N||!g_cache[idx].used) return 0;
    strncpy(host,g_cache[idx].host,DNS_MAX_HOST); host[DNS_MAX_HOST]=0;
    for(int i=0;i<g_cache[idx].naddr;i++) addrs[i]=g_cache[idx].addr[i];
    *naddr=g_cache[idx].naddr;
    int32_t left=(int32_t)(g_cache[idx].expires_tick-get_ticks());
    *secs = left>0 ? left/(int32_t)TICKS_PER_SEC : 0;
    return 1;
}

/* Collect every A record, follow CNAMEs within the same answer section. */
int dns_resolve_all(const char *hostname, uint32_t *out, int max){
    uint32_t ip = dns_resolve(hostname);
    if(!ip) return 0;
    dns_cache_ent_t *e = cache_find(hostname);
    if(e){
        int n = e->naddr < max ? e->naddr : max;
        for(int i=0;i<n;i++) out[i]=e->addr[i];
        return n;
    }
    if(max>0){ out[0]=ip; return 1; }
    return 0;
}

/* ── Parse A record ──────────────────────────────────────────────── */
static uint32_t parse_response(const uint8_t *buf, uint16_t len, uint16_t txid)
{
    g_last_naddr = 0; g_last_ttl = 300;
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
            /* v0.7.12: when an inline label sequence TERMINATES in a
               compression pointer, `pos` already points past the name.  The
               old unconditional `if(!(buf[pos]&0xC0)) pos++` then skipped one
               more byte — and since the next field is TYPE, whose high byte is
               0x00 for every type we care about, the test passed and the whole
               record was read one byte out of alignment.  Track how the loop
               ended instead of guessing from the next byte. */
            steps = 0;
            int ended_with_ptr = 0;
            while(pos < len && buf[pos]) {
                if((buf[pos] & 0xC0) == 0xC0) { pos += 2; ended_with_ptr = 1; break; }
                pos += buf[pos] + 1;
                if(++steps > 128) return 0;
            }
            if(!ended_with_ptr && pos < len) pos++;   /* the 0x00 root label */
        }
        if(pos + 10 > len) return 0;

        uint16_t rtype = (uint16_t)(((uint16_t)buf[pos]<<8)|buf[pos+1]);
        uint32_t rttl  = ((uint32_t)buf[pos+4]<<24)|((uint32_t)buf[pos+5]<<16)
                       | ((uint32_t)buf[pos+6]<<8) |  (uint32_t)buf[pos+7];
        uint16_t rdlen = (uint16_t)(((uint16_t)buf[pos+8]<<8)|buf[pos+9]);
        pos += 10;

        /* BUG FIX: validate rdlen before using as offset */
        if(pos + rdlen > len) return 0;

        if(rtype == 1 && rdlen == 4) {
            /* A record — little-endian (HavenDOS convention).
               v0.7.11: collect up to DNS_MAX_ADDRS instead of returning the
               first one, and remember the smallest TTL in the set. */
            if(g_last_naddr < DNS_MAX_ADDRS){
                g_last_addrs[g_last_naddr++] =
                      (uint32_t)buf[pos]
                    | ((uint32_t)buf[pos+1] << 8)
                    | ((uint32_t)buf[pos+2] << 16)
                    | ((uint32_t)buf[pos+3] << 24);
            }
            if(rttl && rttl < g_last_ttl) g_last_ttl = rttl;
        }
        /* rtype 5 = CNAME: nothing to do explicitly.  The A record for the
           canonical name is normally included in the same answer section, and
           because we now keep scanning instead of bailing at the first
           non-A record, we find it.  That alone fixes CDN-hosted hosts. */
        pos += rdlen;
    }
    return g_last_naddr ? g_last_addrs[0] : 0;
}

/* ── v0.7.17: a hosts file ───────────────────────────────────────────────
 *
 * Typing a raw IP every time is miserable, and it is the first thing anyone
 * notices: "chat kyan@10.0.2.2:6700" tells you nothing about where you are
 * connecting to.  A plain-text /hosts.txt on the HavenDOS filesystem maps
 * names to addresses, exactly like every other OS:
 *
 *     # name            address
 *     bootchat          10.0.2.2
 *     laptop            192.168.2.20
 *
 * Checked BEFORE the network, so a name here never costs a DNS round trip
 * and works with the network down.  Loaded once per boot; `nslookup -flush`
 * drops it so an edit takes effect without a reboot.
 *
 * This is deliberately not mDNS.  Real .local resolution needs a multicast
 * responder and a whole second protocol; a hosts file is fifty lines and
 * solves the actual problem, which is that the user wants to type a name.
 * ───────────────────────────────────────────────────────────────────── */
#define HOSTS_MAX   16
#define HOSTS_FILE  "hosts.txt"

typedef struct { char name[64]; uint32_t ip; } host_ent_t;
static host_ent_t g_hosts[HOSTS_MAX];
static int        g_nhosts = 0;

extern int vfs_read(const char *name, char *buf, uint32_t bufsz);
extern int vfs_exists(const char *name);

static void hosts_load(void)
{
    g_hosts_loaded = 1;
    g_nhosts = 0;
    if(!vfs_exists(HOSTS_FILE)) return;

    static char buf[2048];
    int n = vfs_read(HOSTS_FILE, buf, sizeof(buf)-1);
    if(n <= 0) return;
    buf[n] = 0;

    int i = 0;
    while(buf[i] && g_nhosts < HOSTS_MAX){
        /* skip leading space, blank lines and # comments */
        while(buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') i++;
        if(!buf[i]) break;
        if(buf[i] == '#'){
            while(buf[i] && buf[i] != '\n') i++;
            continue;
        }
        /* field 1: name */
        char name[64]; int k = 0;
        while(buf[i] && buf[i] != ' ' && buf[i] != '\t' &&
              buf[i] != '\r' && buf[i] != '\n' && k < 63)
            name[k++] = buf[i++];
        name[k] = 0;
        while(buf[i] == ' ' || buf[i] == '\t') i++;
        /* field 2: dotted quad */
        char addr[24]; k = 0;
        while(buf[i] && buf[i] != ' ' && buf[i] != '\t' &&
              buf[i] != '\r' && buf[i] != '\n' && k < 23)
            addr[k++] = buf[i++];
        addr[k] = 0;
        while(buf[i] && buf[i] != '\n') i++;      /* discard the rest */

        if(name[0] && addr[0]){
            uint32_t ip = ip_from_str(addr);
            if(ip){
                strncpy(g_hosts[g_nhosts].name, name, 63);
                g_hosts[g_nhosts].name[63] = 0;
                g_hosts[g_nhosts].ip = ip;
                g_nhosts++;
            }
        }
    }
}

/* Case-insensitive, like every other hosts file. */
static uint32_t hosts_lookup(const char *name)
{
    if(!g_hosts_loaded) hosts_load();
    for(int i=0;i<g_nhosts;i++){
        const char *a = g_hosts[i].name, *b = name;
        int eq = 1;
        while(*a && *b){
            char ca = *a++, cb = *b++;
            if(ca >= 'A' && ca <= 'Z') ca += 32;
            if(cb >= 'A' && cb <= 'Z') cb += 32;
            if(ca != cb){ eq = 0; break; }
        }
        if(eq && !*a && !*b) return g_hosts[i].ip;
    }
    return 0;
}

int dns_hosts_count(void){ if(!g_hosts_loaded) hosts_load(); return g_nhosts; }
int dns_hosts_dump(int idx, char *name, uint32_t *ip)
{
    if(!g_hosts_loaded) hosts_load();
    if(idx < 0 || idx >= g_nhosts) return 0;
    strncpy(name, g_hosts[idx].name, 63); name[63] = 0;
    if(ip) *ip = g_hosts[idx].ip;
    return 1;
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

    /* v0.7.17: /hosts.txt beats the network — no round trip, works offline */
    {
        uint32_t h = hosts_lookup(hostname);
        if(h) return h;
    }

    /* v0.7.11: serve from cache when we have a live entry */
    {
        dns_cache_ent_t *e = cache_find(hostname);
        if(e && e->naddr) return e->addr[0];
    }

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

    /* v0.8.9.8: hold the receive slot for our port while waiting, so an
       mDNS announcement or a broadcast cannot overwrite the answer
       between net_poll() and the check below -- see udp_want_port(). */
    udp_want_port(DNS_LOCAL);
    for(int attempt = 0; attempt < 3; attempt++) {
        udp_recv_clear();
        udp_send(dns_ip, DNS_LOCAL, DNS_PORT, qbuf, qlen);

        for(int ms = 0; ms < 3000; ms += 5) {
            net_poll();
            if(udp_recv_len() > 0 && udp_recv_port() == DNS_LOCAL) {
                uint32_t ip = parse_response(udp_recv_data(),
                                             udp_recv_len(), txid);
                udp_recv_clear();
                if(ip){
                    cache_put(hostname,g_last_addrs,g_last_naddr,g_last_ttl);
                    udp_want_port(0);
                    return ip;
                }
            }
            sleep_ms(5);
        }
    }

    udp_want_port(0);
    kernel_log(ERR_NET_DNS_FAILED, "dns: resolution failed");
    return 0;
}
