// Cross-CPU coherence torture for the DSM split guest: one pinned thread per CPU.
//  A: atomic fetch_add counter            (must equal T*NA)
//  B: test-and-set spinlock guarding two plain counters on different pages (must both equal T*NB)
//  C: message passing producer(cpu0)->consumer(last cpu): 16-word payload on page P, release flag on page F, ack back
// Usage: torture NA NB NC        prints TORTURE ... errors=E (E must be 0)
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static long NA, NB, NC; static int T;
static char area[6 * 4096] __attribute__((aligned(4096)));
#define ACTR (*(volatile long *)(area + 0 * 4096))
#define LOCK (*(volatile int *)(area + 1 * 4096))
#define P1 (*(volatile long *)(area + 1 * 4096 + 64))
#define P2 (*(volatile long *)(area + 2 * 4096))
#define DATA ((volatile long *)(area + 3 * 4096))
#define FLAG (*(volatile long *)(area + 4 * 4096))
#define ACK (*(volatile long *)(area + 5 * 4096))
static volatile int go; static long mperr;
static void pin(int c) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(c, &s); sched_setaffinity(0, sizeof s, &s); }
static void *w(void *a) { int id = (int)(long)a; pin(id); while (!go) ;
  for (long i = 0; i < NA; i++) __atomic_fetch_add(&ACTR, 1, __ATOMIC_SEQ_CST);
  for (long i = 0; i < NB; i++) { while (__atomic_exchange_n(&LOCK, 1, __ATOMIC_ACQUIRE)) while (LOCK) ; long a1 = P1, a2 = P2; P1 = a1 + 1; P2 = a2 + 1; __atomic_store_n(&LOCK, 0, __ATOMIC_RELEASE); }
  if (T > 1 && id == 0) for (long k = 1; k <= NC; k++) { for (int j = 0; j < 16; j++) DATA[j] = k * 1000 + j; __atomic_store_n(&FLAG, k, __ATOMIC_RELEASE); while (__atomic_load_n(&ACK, __ATOMIC_ACQUIRE) != k) ; }
  if (T > 1 && id == T - 1) for (long k = 1; k <= NC; k++) { long f; while ((f = __atomic_load_n(&FLAG, __ATOMIC_ACQUIRE)) != k) ; for (int j = 0; j < 16; j++) if (DATA[j] != k * 1000 + j) mperr++; __atomic_store_n(&ACK, k, __ATOMIC_RELEASE); }
  return 0; }
int main(int c, char **v) { NA = atol(v[1]); NB = atol(v[2]); NC = atol(v[3]); T = sysconf(_SC_NPROCESSORS_ONLN); pthread_t t[64]; struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
  for (int i = 0; i < T; i++) pthread_create(&t[i], 0, w, (void *)(long)i); go = 1; for (int i = 0; i < T; i++) pthread_join(t[i], 0); clock_gettime(CLOCK_MONOTONIC, &b);
  long ea = ACTR - T * NA, e1 = P1 - T * NB, e2 = P2 - T * NB; long err = (ea != 0) + (e1 != 0) + (e2 != 0) + mperr;
  printf("TORTURE threads=%d atomic=%ld/%ld lock_p1=%ld lock_p2=%ld/%ld mp_msgs=%ld mp_err=%ld errors=%ld wall_s=%.3f\n", T, ACTR, T * NA, P1, P2, T * NB, T > 1 ? NC : 0, mperr, err, (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9); return err != 0; }
