/*
 * udp.c — UDP for HavenDOS v0.6.8
 * Simple one-shot send + single receive slot (no socket API yet).
 */
#include "../include/net.h"
#include "../include/string.h"
#include "../include/types.h"

extern uint16_t ip_checksum(const void *data, uint16_t len);
extern int      ip_send(uint32_t dst_ip, uint8_t proto,
                        const void *payload, uint16_t payload_len);

/* Last received UDP datagram */
static uint8_t  udp_rx_buf[512];
static uint16_t udp_rx_len     = 0;
static uint16_t udp_rx_port    = 0;
static uint32_t udp_rx_src_ip  = 0;
static uint16_t udp_rx_src_port= 0;

int udp_send(uint32_t dest_ip, uint16_t src_port,
             uint16_t dst_port, const void *data, uint16_t len)
{
    static uint8_t buf[sizeof(udp_hdr_t) + 512];
    if(len > 512) return -1;

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
void udp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len)
{
    if(len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t *hdr = (const udp_hdr_t*)payload;

    /* BUG FIX: validate udp length field before subtracting header.
       If hdr->length < sizeof(udp_hdr_t) the subtraction wraps to a
       huge uint16_t, causing a massive memcpy out of bounds. */
    uint16_t udp_len = ntohs(hdr->length);
    if(udp_len < sizeof(udp_hdr_t)) return;   /* malformed: length < header */
    if(udp_len > len) return;                  /* malformed: claims more than arrived */

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
