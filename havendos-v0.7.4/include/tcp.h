#pragma once
#include "types.h"
int  tcp_connect(uint32_t ip, uint16_t port);
int  tcp_send(int fd, const void *data, uint16_t len);
int  tcp_recv(int fd, void *buf, uint16_t maxlen, uint32_t timeout_ms);
void tcp_close(int fd);
int  tcp_connected(int fd);
void tcp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len);
