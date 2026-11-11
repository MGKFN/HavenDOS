/*
 * udp.c — UDP for HavenDOS v0.6.8
 * Simple one-shot send + single receive slot (no socket API yet).
 */
#include "../include/net.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/netstat.h"

extern uint16_t ip_checksum(const void *data, uint16_t len);
extern int      ip_send(uint32_t dst_ip, uint8_t proto,
                        const void *payload, uint16_t payload_len);

/* ── v0.8.9.8: BIG ENOUGH FOR THE ONE DATAGRAM THAT MATTERED ─────────
 * Both buffers were 512 bytes, and udp_send() refused anything longer.
 * A DHCP message is 548 (236 fixed + 4 magic + 308 options -- RFC 2131
 * asks a client to be ready for 576 in all). So dhcp_request() built a
 * DISCOVER, udp_send() said -1, and nothing was ever sent; and had an
 * answer come, it would have been cut to 512 and parse_offer() would
 * have refused it for being shorter than 548. DHCP has not worked in
 * this tree, anywhere, ever. Nobody saw it because the fallback
 * address is QEMU's own: 10.0.2.15 behind 10.0.2.2 is exactly what
 * SLIRP would have handed out. On the iPhone's 172.20.10.x it is
 * nowhere at all, which is every ping timeout in the photo.
 *
 * 1472 is the most UDP one Ethernet frame carries (1500 - 20 - 8). */
#define UDP_MAX_PAYLOAD 1472
static uint8_t  udp_rx_buf[UDP_MAX_PAYLOAD];
static uint16_t udp_rx_len     = 0;
static uint16_t udp_rx_port    = 0;
static uint32_t udp_rx_src_ip  = 0;
static uint16_t udp_rx_src_port= 0;

int udp_send(uint32_t dest_ip, uint16_t src_port,
             uint16_t dst_port, const void *data, uint16_t len)
{
    NS_INC(udp_tx);
    static uint8_t buf[sizeof(udp_hdr_t) + UDP_MAX_PAYLOAD];
    if(len > UDP_MAX_PAYLOAD) return -1;

    udp_hdr_t *hdr = (udp_hdr_t*)buf;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    hdr->checksum = 0;   /* optional for IPv4 */

    memcpy(buf + sizeof(udp_hdr_t), data, len);
    return ip_send(dest_ip, IP_PROTO_UDP, buf,
                   (uint16_t)(sizeof(udp_hdr_t) + len));
}

/* Called from ip_handle */
/* ── v0.8.9.8: ONE SLOT, SO SAY WHICH PORT IT IS FOR ──────────────────
 * Every datagram overwrote the one before it, and eth_poll() drains up
 * to sixteen frames a call. A DHCP OFFER followed in the same poll by
 * anything else -- an iPhone announces itself over mDNS the moment a
 * link appears -- was gone before dhcp_request() looked. The same for
 * a DNS answer behind a broadcast.
 *
 * A caller that is WAITING now says for which port, and while it waits
 * nothing else is stored. Port 0 is the old behaviour: keep the latest
 * of anything. There is one waiter at a time; the shell is single-
 * threaded and nothing receives UDP from the bottom half. */
static uint16_t udp_want;
static uint32_t udp_unwanted;
void     udp_want_port(uint16_t port) { udp_want = port; udp_rx_len = 0; }
uint32_t udp_unwanted_count(void)     { return udp_unwanted; }

void udp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len)
{
    NS_INC(udp_rx);
    if(len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t *hdr = (const udp_hdr_t*)payload;

    /* BUG FIX: validate udp length field before subtracting header.
       If hdr->length < sizeof(udp_hdr_t) the subtraction wraps to a
       huge uint16_t, causing a massive memcpy out of bounds. */
    uint16_t udp_len = ntohs(hdr->length);
    if(udp_len < sizeof(udp_hdr_t)) return;   /* malformed: length < header */
    if(udp_len > len) return;                  /* malformed: claims more than arrived */

    if(udp_want && ntohs(hdr->dst_port) != udp_want){
        udp_unwanted++;
        return;
    }
    uint16_t data_len = (uint16_t)(udp_len - sizeof(udp_hdr_t));
    if(data_len > sizeof(udp_rx_buf)) data_len = sizeof(udp_rx_buf);

    memcpy(udp_rx_buf, payload + sizeof(udp_hdr_t), data_len);
    udp_rx_len      = data_len;
    udp_rx_port     = ntohs(hdr->dst_port);
    udp_rx_src_ip   = src_ip;
    udp_rx_src_port = ntohs(hdr->src_port);
}

/* Shell / app can call these to read last received datagram */
uint16_t udp_recv_len(void)         { return udp_rx_len; }
uint16_t udp_recv_port(void)        { return udp_rx_port; }
uint32_t udp_recv_src_ip(void)      { return udp_rx_src_ip; }
uint16_t udp_recv_src_port(void)    { return udp_rx_src_port; }
const uint8_t *udp_recv_data(void)  { return udp_rx_buf; }
void     udp_recv_clear(void)       { udp_rx_len = 0; }
