/*
 * net.c — Network subsystem init + glue for HavenDOS v0.6.8
 * Supports RTL8139 (port I/O) and Intel e1000 (MMIO).
 * Tries e1000 first (UTM SE default), falls back to RTL8139.
 */
#include "../include/net.h"
#include "../include/rtl8139.h"
#include "../include/e1000.h"
#include "../include/pci.h"
#include "../include/string.h"
#include "../include/types.h"

extern void eth_poll(void);

#define DEFAULT_IP   ((uint32_t)(10 |(0<<8) |(2<<16)|(15<<24)))
#define DEFAULT_MASK ((uint32_t)(255|(255<<8)|(255<<16)|(0<<24)))
#define DEFAULT_GW   ((uint32_t)(10 |(0<<8) |(2<<16)|(2<<24)))

static uint32_t my_ip   = DEFAULT_IP;
static uint32_t my_mask = DEFAULT_MASK;
static uint32_t my_gw   = DEFAULT_GW;
static int      net_up  = 0;

/* Which driver is active: 0=none 1=rtl8139 2=e1000 */
static int nic_type = 0;

uint32_t net_get_ip  (void){ return my_ip;   }
uint32_t net_get_mask(void){ return my_mask; }
uint32_t net_get_gw  (void){ return my_gw;   }
int      net_ready   (void){ return net_up;  }

void net_set_ip(uint32_t ip, uint32_t mask, uint32_t gw){
    my_ip=ip; my_mask=mask; my_gw=gw;
}

void net_get_mac(uint8_t mac[ETH_ALEN]){
    if(nic_type==1)      rtl8139_get_mac(mac);
    else if(nic_type==2) e1000_get_mac(mac);
    else for(int i=0;i<ETH_ALEN;i++) mac[i]=0;
}

/* Called by rtl8139.c / e1000.c send path via ethernet.c */
int nic_send(const void *data, uint16_t len){
    if(nic_type==1) return rtl8139_send(data,len);
    if(nic_type==2) return e1000_send(data,len);
    return -1;
}
uint16_t nic_recv(void *buf, uint16_t len){
    if(nic_type==1) return rtl8139_recv(buf,len);
    if(nic_type==2) return e1000_recv(buf,len);
    return 0;
}

void net_init(void)
{
    uint16_t rtl_io    = 0;
    uint32_t e1000_mem = 0;
    pci_scan_net2(&rtl_io, &e1000_mem);

    /* Prefer e1000 (UTM SE default) */
    if(e1000_mem){
        if(e1000_init(e1000_mem)){ nic_type=2; net_up=1; return; }
    }
    if(rtl_io){
        if(rtl8139_init(rtl_io)){ nic_type=1; net_up=1; return; }
    }
    /* No NIC found */
}

void net_poll(void){
    if(net_up) eth_poll();
}

/* ── Utility ─────────────────────────────────────────────── */
uint32_t ip_from_str(const char *s){
    uint32_t a=0,b=0,c=0,d=0; int i=0;
    while(s[i]>='0'&&s[i]<='9') a=a*10+(s[i++]-'0'); if(s[i]=='.') i++;
    while(s[i]>='0'&&s[i]<='9') b=b*10+(s[i++]-'0'); if(s[i]=='.') i++;
    while(s[i]>='0'&&s[i]<='9') c=c*10+(s[i++]-'0'); if(s[i]=='.') i++;
    while(s[i]>='0'&&s[i]<='9') d=d*10+(s[i++]-'0');
    return (uint32_t)(a|(b<<8)|(c<<16)|(d<<24));
}
void ip_to_str(uint32_t ip, char *buf){
    uint8_t *b=(uint8_t*)&ip; int pos=0; char tmp[4];
    for(int i=0;i<4;i++){
        utoa(b[i],tmp,10);
        for(int j=0;tmp[j];j++) buf[pos++]=tmp[j];
        if(i<3) buf[pos++]='.';
    } buf[pos]=0;
}
void mac_to_str(const uint8_t mac[ETH_ALEN], char *buf){
    const char *h="0123456789ABCDEF"; int pos=0;
    for(int i=0;i<ETH_ALEN;i++){
        buf[pos++]=h[mac[i]>>4]; buf[pos++]=h[mac[i]&0xF];
        if(i<ETH_ALEN-1) buf[pos++]=':';
    } buf[pos]=0;
}
