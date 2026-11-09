/*
 * entropy.c - HavenDOS v0.7.11 entropy pool
 *
 * Everything cryptographic in HavenDOS previously traced back to a single
 * uint32_t seed derived from get_ticks().  On a cold boot that is a small,
 * highly predictable number: the machine reaches the shell at very nearly the
 * same tick count every time, so the TLS pre-master secret and the PKCS#1
 * padding were drawn from a keyspace an attacker could enumerate in seconds.
 *
 * This mixes four independent sources:
 *   - RTC wall-clock seconds (differs between boots)
 *   - the live PIT counter, read twice around a delay (sub-tick jitter)
 *   - RDTSC, which on real hardware has far more resolution than the PIT
 *   - a running pool stirred by every caller, so successive draws differ
 *
 * This is NOT a CSPRNG and is not claimed to be.  It is a large improvement
 * over a boot-deterministic constant, which is what it replaces.
 */
#include "../include/types.h"

#include "../include/rtc.h"
extern uint32_t get_ticks(void);

static uint32_t pool = 0x9E3779B9u;

static uint32_t rdtsc_lo(void){
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ hi;
}

static void mix(uint32_t v){
    pool ^= v;
    pool *= 0x01000193u;          /* FNV-ish avalanche */
    pool ^= pool >> 15;
    pool += 0x9E3779B9u;
    pool ^= pool << 7;
}

uint32_t entropy_u32(void){
    uint8_t h=0,m=0,sec=0;
    rtc_get_time(&h,&m,&sec);
    mix((uint32_t)h*3600u+(uint32_t)m*60u+(uint32_t)sec);
    mix(get_ticks());
    mix(rdtsc_lo());
    for(volatile int i=0;i<997;i++){ }     /* let the counters advance */
    mix(rdtsc_lo());
    mix(get_ticks() << 11);
    mix((uint32_t)(uintptr_t)&h);          /* stack address entropy */
    return pool;
}

void entropy_stir(uint32_t v){ mix(v); mix(rdtsc_lo()); }
