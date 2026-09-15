#pragma once
#include "types.h"
#define TLS_ERR_HANDSHAKE -1
#define TLS_ERR_ALERT     -2
#define TLS_ERR_CERT      -3
#define TLS_HANDSHAKE      1
#define TLS_ALERT          2
int  tls_connect(int tcp_fd, const char *hostname, uint32_t seed);
int  tls_send(int fd, const uint8_t *data, uint16_t len);
int  tls_recv(int fd, uint8_t *buf, uint16_t maxlen, uint32_t timeout_ms);
void tls_close(int fd);
