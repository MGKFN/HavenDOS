/*
 * net.c -- the network subsystem's configuration and its glue.
 *
 * ── WHAT MOVED OUT OF HERE IN 0.8.7, AND WHY ────────────────────────
 *
 * This file used to BE the NIC layer. It held three switches on one
 * `static int nic_type` -- net_get_mac(), nic_send() and nic_recv() --
 * plus a fourth place that decided the probe order, inside net_init(),
 * and the numbering itself written as a comment rather than as anything
 * a compiler could check:
 *
 *         "Which driver is active: 0=none 1=rtl8139 2=e1000"
 *         static int nic_type = 0;
 *
 * 0.8.6 "Argo" closed by saying that the next bus added would arrive in
 * net/, that guarding the bus LABEL had not been enough, and that the
 * thing which CHOOSES has to be one place a grep can see. Applying that
 * to this file found the same shape the block layer had: three copies
 * of a dispatch and an order decided somewhere else.
 *
 * net/netdev.c owns all of it now -- the table, the dispatch, the bus
 * word, the registration order, the preference and the cached MAC --
 * and tools/check_nic_dispatch.py refuses a driver call outside it.
 *
 * What is left here is what was always this file's: the machine's IP
 * configuration, and two one-line accessors that mean "the active NIC"
 * rather than "a particular driver".
 */
#include "../include/net.h"
#include "../include/netdev.h"
#include "../include/string.h"
#include "../include/types.h"

extern void eth_poll(void);

#define DEFAULT_IP   ((uint32_t)(10 |(0<<8) |(2<<16)|(15<<24)))
#define DEFAULT_MASK ((uint32_t)(255|(255<<8)|(255<<16)|(0<<24)))
#define DEFAULT_GW   ((uint32_t)(10 |(0<<8) |(2<<16)|(2<<24)))

static uint32_t my_ip   = DEFAULT_IP;
static uint32_t my_mask = DEFAULT_MASK;
static uint32_t my_gw   = DEFAULT_GW;

/* ── WHERE THE ADDRESS CAME FROM, v0.8.7.2 ───────────────────────────
 * The defaults above are QEMU's: 10.0.2.15 behind a gateway at
 * 10.0.2.2. They are a sensible place to land in an emulator and they
 * are MEANINGLESS on a real network -- an iPhone hotspot hands out
 * 172.20.10.x, and a router hands out something else again.
 *
 * Until this release nothing recorded whether the configuration had
 * actually been negotiated, so `ifconfig` printed the fallback and said
 * "Status: UP" underneath it. That is a machine reporting itself
 * configured while holding an address no one on its network has heard
 * of: every lookup goes to a DNS server reached through a gateway that
 * is not there, and the only symptom is "dns: resolution failed".
 *
 * Which is exactly what the HP showed on 26 October. */
static int g_cfg_src = NET_CFG_FALLBACK;

int  net_config_source(void)      { return g_cfg_src; }
void net_config_source_set(int s) { g_cfg_src = s;    }

uint32_t net_get_ip  (void){ return my_ip;   }
uint32_t net_get_mask(void){ return my_mask; }
uint32_t net_get_gw  (void){ return my_gw;   }

/* "Is there a network" is now a question about the table rather than a
   flag latched at boot. That matters for the reason 0.8.7 exists: a USB
   adapter can be unplugged, and a latched net_up would have gone on
   reporting a network over a NIC that was no longer in the machine. */
int net_ready(void){ return netdev_active() >= 0; }

void net_set_ip(uint32_t ip, uint32_t mask, uint32_t gw){
    my_ip=ip; my_mask=mask; my_gw=gw;
}

/* ── the active NIC, not a driver ────────────────────────────────────
   net/ethernet.c calls these two and names no driver, which it never
   did -- the switches were one layer below it. They stay as functions
   because "which NIC the stack uses" is a decision with one owner and
   every caller should go through it rather than ask the table itself
   and pick. */
void net_get_mac(uint8_t mac[ETH_ALEN]){
    netdev_mac(netdev_active(), mac);
}

int nic_send(const void *data, uint16_t len){
    return netdev_send(netdev_active(), data, len);
}

uint16_t nic_recv(void *buf, uint16_t len){
    return netdev_recv(netdev_active(), buf, len);
}

void net_init(void)
{
    netdev_reset();
    netdev_scan();
    /* A USB NIC is not found here. It appears when the USB bus is
       walked, which has already happened by the time net_init() runs,
       and usb/ethernet.c registers it from there. */
}

void net_poll(void){
    if(net_ready()) eth_poll();
}

/* ── Utility ─────────────────────────────────────────────── */
uint32_t ip_from_str(const char *s){
    uint32_t a=0,b=0,c=0,d=0; int i=0;
    while(s[i]>='0'&&s[i]<='9') a=a*10+(s[i++]-'0');
    if(s[i]=='.') i++;
    while(s[i]>='0'&&s[i]<='9') b=b*10+(s[i++]-'0');
    if(s[i]=='.') i++;
    while(s[i]>='0'&&s[i]<='9') c=c*10+(s[i++]-'0');
    if(s[i]=='.') i++;
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
