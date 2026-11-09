#pragma once
#include "types.h"

/* v0.7.13: the stack now keeps a table of independent sockets.  The fd a
   caller receives from tcp_connect()/tcp_accept() is a real index into that
   table and must be passed back to every other call — before v0.7.13 every
   function took an fd and ignored it. */
#define TCP_MAX_SOCK 6

int  tcp_connect(uint32_t ip, uint16_t port);   /* → fd, or <0 on error */
int  tcp_send(int fd, const void *data, uint16_t len);
int  tcp_recv(int fd, void *buf, uint16_t maxlen, uint32_t timeout_ms);
void tcp_close(int fd);
int  tcp_connected(int fd);
int  tcp_eof(int fd);          /* v0.7.5: was missing -> implicit decl in http.c */
void tcp_rtx_check(void);      /* v0.7.5: drive from any polling loop */
void tcp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len);

/* v0.7.13: server side */
int      tcp_listen(uint16_t port);                  /* → listening fd */
int      tcp_accept(int lfd, uint32_t timeout_ms);   /* → connected fd */
uint32_t tcp_peer_ip(int fd);
uint16_t tcp_peer_port(int fd);

/* v0.7.13: diagnostics */
int         tcp_sock_count(void);
int         tcp_sock_info(int idx, uint16_t *lport, uint32_t *rip,
                          uint16_t *rport, int *state);
const char *tcp_state_name(int st);
