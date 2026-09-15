#pragma once
#include "types.h"

#define ETH_ALEN       6
#define ETH_TYPE_IP    0x0800
#define ETH_TYPE_ARP   0x0806

/* Byte-order */
static inline uint16_t htons(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }
static inline uint16_t ntohs(uint16_t v){ return htons(v); }
static inline uint32_t htonl(uint32_t v){ return ((v>>24)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|(v<<24)); }
static inline uint32_t ntohl(uint32_t v){ return htonl(v); }

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;
    uint8_t  dscp;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} ip_hdr_t;

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack_num;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} arp_pkt_t;
typedef arp_pkt_t arp_hdr_t;
#define ARP_REQUEST 1
#define ARP_REPLY   2


typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

/* UDP/DNS */
int  udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t len);
void udp_recv_clear(void);
uint16_t udp_recv_len(void);
uint16_t udp_recv_port(void);
const uint8_t *udp_recv_data(void);

/* ICMP */
int icmp_ping(uint32_t dest_ip, uint16_t seq);
void icmp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len);

/* ARP */
int  arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN]);
void arp_request(uint32_t target_ip);
void arp_handle(const uint8_t *payload, uint16_t len);

/* Ethernet */
int  eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
              const void *payload, uint16_t payload_len);
void eth_poll(void);
void ip_handle(const uint8_t *payload, uint16_t len);

/* net.c */
void     net_init(void);
int      net_ready(void);
uint32_t net_get_ip(void);
uint32_t net_get_mask(void);
uint32_t net_get_gw(void);
void     net_set_ip(uint32_t ip, uint32_t mask, uint32_t gw);
void     net_get_mac(uint8_t mac[ETH_ALEN]);
void     net_set_dns(uint32_t ip);
uint32_t net_get_dns(void);
void     net_poll(void);

/* ipv4.c */
int      ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len);
uint16_t ip_checksum(const void *data, uint16_t len);

/* ip helpers */



uint32_t ip_from_str(const char *s);
void     ip_to_str(uint32_t ip, char *buf);
