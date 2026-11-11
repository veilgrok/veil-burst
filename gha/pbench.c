// Multithreaded prime count (dynamic chunks via atomic counter). Usage: pbench THREADS LIMIT
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
static long LIM, CH = 20000; static volatile long next = 0; static long total = 0; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int isp(long n){ if(n<2) return 0; for(long d=2; d*d<=n; d++) if(n%d==0) return 0; return 1; }
static void* w(void* a){ long c=0; for(;;){ long s=__sync_fetch_and_add(&next, CH); if(s>=LIM) break; long e=s+CH>LIM?LIM:s+CH; for(long n=s;n<e;n++) c+=isp(n);} pthread_mutex_lock(&mu); total+=c; pthread_mutex_unlock(&mu); return 0; }
int main(int argc,char**argv){ int T=atoi(argv[1]); LIM=atol(argv[2]); pthread_t t[256]; struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
 for(int i=0;i<T;i++) pthread_create(&t[i],0,w,0); for(int i=0;i<T;i++) pthread_join(t[i],0); clock_gettime(CLOCK_MONOTONIC,&b);
 printf("PBENCH threads=%d limit=%ld primes=%ld wall_s=%.3f\n",T,LIM,total,(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9); return 0; }
