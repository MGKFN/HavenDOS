#pragma once
#define ETH_ALEN 6
#include "types.h"
int      rtl8139_init(uint16_t io_base);
void     rtl8139_get_mac(uint8_t mac[6]);
int      rtl8139_send(const void *data, uint16_t len);
uint16_t rtl8139_recv(void *buf, uint16_t maxlen);
