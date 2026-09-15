/*
 * icmp.c — ICMP echo request/reply for HavenDOS v0.6.8
 *
 * BUG FIX: replaced get_ticks() with sleep_ms() loop.
 * get_ticks() relies on PIT IRQ0 which fires unreliably on UTM SE TCG —
 * the counter can stall causing ping to never time out, or advance in
 * jumps causing it to expire before the reply arrives.
 * sleep_ms() is a calibrated busy-wait and is the correct timer on TCG.
 */
#include "../include/net.h"
#include "../include/string.h"
#include "../include/types.h"

extern uint16_t ip_checksum(const void *data, uint16_t len);
extern int      ip_send(uint32_t dst_ip, uint8_t proto,
                        const void *payload, uint16_t payload_len);
extern void     net_poll(void);
extern void     sleep_ms(uint32_t ms);

static volatile uint16_t last_reply_seq = 0xFFFF;
static volatile uint32_t last_reply_ip  = 0;

#define PING_DATA_LEN 32
static const char ping_data[PING_DATA_LEN] = "HAVENDOS-PING-0.6.2-TECHHAVEN--";

int icmp_ping(uint32_t dest_ip, uint16_t seq)
{
    uint8_t buf[sizeof(icmp_hdr_t) + PING_DATA_LEN];
    icmp_hdr_t *hdr = (icmp_hdr_t*)buf;

    hdr->type     = ICMP_ECHO_REQUEST;
    hdr->code     = 0;
    hdr->checksum = 0;
    hdr->id       = htons(0xB007);
    hdr->seq      = htons(seq);
    memcpy(buf + sizeof(icmp_hdr_t), ping_data, PING_DATA_LEN);
    hdr->checksum = ip_checksum(buf, sizeof(buf));

    last_reply_seq = 0xFFFF;

    if(ip_send(dest_ip, IP_PROTO_ICMP, buf, sizeof(buf)) < 0)
        return -1;

    /* Poll for reply: 3000ms total, 5ms steps = 600 iterations.
       sleep_ms() is a calibrated busy-wait, safe on UTM SE TCG. */
    for(int i = 0; i < 600; i++){
        net_poll();
        if(last_reply_seq == seq && last_reply_ip == dest_ip)
            return i * 5;   /* approx RTT in ms */
        sleep_ms(5);
    }
    return -1;   /* timeout */
}

void icmp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len)
{
    if(len < sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t *hdr = (const icmp_hdr_t*)payload;

    if(hdr->type == ICMP_ECHO_REPLY){
        if(ntohs(hdr->id) == 0xB007){
            last_reply_seq = ntohs(hdr->seq);
            last_reply_ip  = src_ip;
        }
        return;
    }

    if(hdr->type == ICMP_ECHO_REQUEST){
        static uint8_t reply[256];
        if(len > sizeof(reply)) len = sizeof(reply);
        memcpy(reply, payload, len);
        icmp_hdr_t *r = (icmp_hdr_t*)reply;
        r->type     = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = ip_checksum(reply, len);
        ip_send(src_ip, IP_PROTO_ICMP, reply, len);
    }
}
