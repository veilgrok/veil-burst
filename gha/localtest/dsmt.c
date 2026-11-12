// Local DSM harness (no KVM needed): two plain processes stand in for the two nodes' vCPUs. Each maps its own
// /dev/shm file through dsm_shim.so (or, as the baseline, both map the SAME file with no shim) and runs
// first-touch, cross-check and contended-atomics phases. It prints seconds per phase and error counts.
//   dsmt ID PATH MIB [TORTURE_ITERS]
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#define PS 4096
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static volatile uint64_t *ctl;  /* page 0: [0] phase barrier count, [1] turn */
static void barrier(uint64_t want) { __atomic_fetch_add(&ctl[0], 1, __ATOMIC_SEQ_CST); while (__atomic_load_n(&ctl[0], __ATOMIC_SEQ_CST) < want) usleep(2000); }
static uint32_t pat(size_t pg, int i, int own) { return (uint32_t)(pg * 2654435761u + i) ^ (own ? 0xdeadbeef : 0x1234567); }
int main(int c, char **v) {
  signal(SIGPIPE, SIG_IGN);  /* as QEMU does: a dead home must show up as a failed write, not kill the node */
  if (c < 4) return 2; int id = atoi(v[1]); size_t mib = atol(v[3]), np = mib * 256; long iters = c > 4 ? atol(v[4]) : 20000;
  int fd = open(v[2], O_RDWR | O_CREAT, 0600); if (fd < 0 || ftruncate(fd, np * PS)) { perror("open"); return 1; }
  char *b = mmap(0, np * PS, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); if (b == MAP_FAILED) { perror("mmap"); return 1; }
  ctl = (volatile uint64_t *)b; size_t q = (np - 16) / 4, lo = 16 + (id ? 2 * q : 0), hi = lo + 2 * q, olo = 16 + (id ? 0 : 2 * q);
  barrier(2);
  /* 1. first touch, one node at a time (node 0 then node 1): one store per page in the first half of the own range */
  while (__atomic_load_n(&ctl[1], __ATOMIC_SEQ_CST) != (uint64_t)id) usleep(2000);
  double t0 = now(); for (size_t pg = lo; pg < lo + q; pg++) b[pg * PS + 8] = 1; double t1 = now();
  printf("DSMT node%d memtouch mib=%zu secs=%.3f mib_per_s=%.1f\n", id, q / 256, t1 - t0, q / 256.0 / (t1 - t0)); fflush(stdout);
  __atomic_store_n(&ctl[1], id + 1, __ATOMIC_SEQ_CST); barrier(4);
  /* 2. cross-check: fill the whole own range (both at once), then read and verify the other node's range */
  t0 = now(); for (size_t pg = lo; pg < hi; pg++) { uint32_t *w = (uint32_t *)(b + pg * PS); for (int i = 0; i < PS / 4; i++) w[i] = pat(pg, i, id); } t1 = now();
  barrier(6); double t2 = now(); uint64_t bad = 0; int nbp = 0;
  for (size_t pg = olo; pg < olo + 2 * q; pg++) { uint32_t *w = (uint32_t *)(b + pg * PS); uint64_t b0 = bad; for (int i = 0; i < PS / 4; i++) bad += w[i] != pat(pg, i, !id); if (bad != b0 && nbp++ < 40) fprintf(stderr, "DSMT node%d BADPAGE pg=%zx badwords=%llu w0=%x w2=%x want0=%x\n", id, pg, (unsigned long long)(bad - b0), w[0], w[2], pat(pg, 0, !id)); } double t3 = now();
  printf("DSMT node%d xcheck fill_s=%.3f read_s=%.3f bad=%llu\n", id, t1 - t0, t3 - t2, (unsigned long long)bad); fflush(stdout);
  barrier(8);
  /* 3. torture: both nodes add to counters on the same two pages, and rewrite their own range pages in between */
  volatile uint64_t *ca = (volatile uint64_t *)(b + 2 * PS), *cb = (volatile uint64_t *)(b + 3 * PS + 64 * id);
  t0 = now(); for (long i = 0; i < iters; i++) { __atomic_fetch_add(ca, 2, __ATOMIC_SEQ_CST); __atomic_fetch_add(cb, 2, __ATOMIC_SEQ_CST);  /* even values: never mistaken for page-table entries by the home's heuristic */ b[(lo + i % q) * PS + 100]++; } t1 = now();
  barrier(10); uint64_t a = __atomic_load_n(ca, __ATOMIC_SEQ_CST), o0 = *(volatile uint64_t *)(b + 3 * PS), o1 = *(volatile uint64_t *)(b + 3 * PS + 64);
  int err = (a != 4 * (uint64_t)iters) + (o0 != 2 * (uint64_t)iters) + (o1 != 2 * (uint64_t)iters);
  printf("DSMT node%d torture iters=%ld secs=%.3f shared=%llu errors=%d\n", id, iters, t1 - t0, (unsigned long long)a, err); fflush(stdout);
  barrier(12); usleep(300000); return 0; }
