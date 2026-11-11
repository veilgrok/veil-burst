// memtouch MB PASSES: allocate MB, write every page, then PASSES read+write sweeps; prints wall time.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
int main(int c,char**v){ size_t n=(size_t)atol(v[1])<<20; int P=atoi(v[2]); volatile char *b=malloc(n); double t0=now(); for(size_t i=0;i<n;i+=4096) b[i]=1; double t1=now(); long s=0;
 for(int k=0;k<P;k++) for(size_t i=0;i<n;i+=4096){ s+=b[i]; b[i]=(char)k; } double t2=now(); printf("MEMTOUCH mb=%zu passes=%d first_touch_s=%.3f sweeps_s=%.3f sum=%ld\n",n>>20,P,t1-t0,t2-t1,s); return 0; }
