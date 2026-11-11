// cross-vCPU data check: cpu0 writes a pattern over MB, cpu1 verifies + rewrites, cpu0 verifies again.
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
static uint32_t *buf; static size_t n; static volatile int phase;
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void pin(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s); sched_setaffinity(0,sizeof s,&s); }
static uint64_t fill(uint32_t salt){ uint64_t s=0; for(size_t i=0;i<n;i++){ buf[i]=(uint32_t)(i*2654435761u)^salt; s+=buf[i]; } return s; }
static uint64_t check(uint32_t salt, size_t *bad){ uint64_t s=0; *bad=0; for(size_t i=0;i<n;i++){ uint32_t e=(uint32_t)(i*2654435761u)^salt; s+=buf[i]; if(buf[i]!=e) (*bad)++; } return s; }
static void *other(void *a){ pin(1); while(phase!=1); size_t bad; double t=now(); uint64_t s=check(0x1234567,&bad); double t2=now();
  printf("XCHECK cpu1 verify sum=%llu bad=%zu s=%.3f\n",(unsigned long long)s,bad,t2-t); fill(0xdeadbeef); printf("XCHECK cpu1 fill s=%.3f\n",now()-t2); __sync_synchronize(); phase=2; return 0; }
int main(int c,char**v){ size_t mb=atol(v[1]); n=(mb<<20)/4; buf=malloc(n*4); pthread_t t; pthread_create(&t,0,other,0); pin(0);
  double t0=now(); uint64_t s=fill(0x1234567); printf("XCHECK cpu0 fill sum=%llu s=%.3f\n",(unsigned long long)s,now()-t0); __sync_synchronize(); phase=1;
  while(phase!=2); size_t bad; t0=now(); s=check(0xdeadbeef,&bad); printf("XCHECK cpu0 verify sum=%llu bad=%zu s=%.3f\n",(unsigned long long)s,bad,now()-t0); pthread_join(t,0); return 0; }
