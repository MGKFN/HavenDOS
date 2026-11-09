#pragma once
#include "types.h"
uint32_t dns_resolve(const char *host);

/* v0.7.11 (roadmap 0.7.8): resolver cache + multi-address lookup */
#define DNS_MAX_ADDRS 4
int  dns_resolve_all(const char *hostname, uint32_t *out, int max);
void dns_cache_flush(void);
int  dns_cache_dump(int idx, char *host, uint32_t *addrs, int *naddr, int *secs);

/* v0.7.17: /hosts.txt — name to address, checked before the network */
int dns_hosts_count(void);
int dns_hosts_dump(int idx, char *name, uint32_t *ip);
