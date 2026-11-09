/*
 * netstat.c — v0.7.6 network counters.
 * Storage only; every increment happens at the site being measured.
 */
#include "../include/netstat.h"
#include "../include/string.h"

netstat_t g_netstat;

void netstat_reset(void){ memset(&g_netstat, 0, sizeof(g_netstat)); }
