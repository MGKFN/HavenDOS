#pragma once
#define ETH_ALEN 6
#include "types.h"
int      e1000_init(uint32_t mmio_base);
void     e1000_get_mac(uint8_t mac[6]);
int      e1000_send(const void *data, uint16_t len);
uint16_t e1000_recv(void *buf, uint16_t maxlen);
