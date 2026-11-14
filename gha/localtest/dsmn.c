// dsmn: strong-scaling harness for pooled memory. N plain processes ("nodes") share one address space through dsm_shim.so
// (or, for the one-machine baseline, a single process with no shim does all the work). The TOTAL work is fixed and split
// N ways, so speedup = time(1 machine) / time(N nodes). Node 0 times the job from a start barrier to an end barrier that
// every node must reach, so the time is the slowest node's, never a sum.
//   dsmn ID N PATH MIB WORKLOAD [ARG]
// Layout (pages): [0,16) control; [16,16+TP) read-mostly table (node 0 writes it); then N equal slices, slice k owned by node k
// (matching VEIL_DSM_OWNMAP; print it with: dsmn map N MIB).
// Workloads (ARG: scale factor, default 1):
//   cpu     hashing in a 64 KiB private buffer: compute-bound
//   stream  triad a=b+s*c over the node's slice, 8 passes: memory-bandwidth-bound, private
//   gather  random 8-byte reads from the 32 MiB table written by node 0: read-shared (replicated after first touch)
//   shared  like cpu, but 1 op in ARG (default 1000) is an atomic add to one of 4 shared counters: write-shared
//   touch   first write to every page of the slice: allocation-bound
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#define PS 4096
#define TP 8192  /* 32 MiB table */
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static volatile uint64_t *ctl; static int N;
static void barrier(uint64_t round) { __atomic_fetch_add(&ctl[0], 1, __ATOMIC_SEQ_CST); while (__atomic_load_n(&ctl[0], __ATOMIC_SEQ_CST) < round * N) usleep(200); }
static inline uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return x; }
static void slice(size_t np, int n, int k, size_t *lo, size_t *hi) { size_t d = 16 + TP, per = (np - d) / n; *lo = d + k * per; *hi = *lo + per; }
static int NN, T; static double SC = 1; static volatile long bad_g;
struct job { int k, t; char *b; size_t np; const char *w; double arg; uint64_t chk; };
static void *run(void *a) { struct job *j = a; char *b = j->b; const char *w = j->w; double arg = j->arg; int k = j->k; uint64_t chk = 0; size_t lo0, hi0; slice(j->np, NN, k, &lo0, &hi0);
  size_t per = (hi0 - lo0) / T, lo = lo0 + j->t * per, hi = j->t == T - 1 ? hi0 : lo + per; uint64_t *tab = (uint64_t *)(b + 16 * PS); size_t tn = (size_t)TP * PS / 8; long div = (long)NN * T, me = (long)k * T + j->t;
  if (!strcmp(w, "cpu") || !strcmp(w, "shared")) { uint64_t *pb = (uint64_t *)(b + lo * PS), x = me + 1; long ops = (long)(4e8 * SC / div), every = !strcmp(w, "shared") ? (arg > 0 ? arg : 1000) : 0;
    volatile uint64_t *cnt = (volatile uint64_t *)(b + 2 * PS);
    for (long i = 0; i < ops; i++) { x = mix(x + i); pb[x & 8191] += x; if (every && i % every == 0) __atomic_fetch_add(&cnt[(i / every & 3) * 8], 1, __ATOMIC_SEQ_CST); } chk += x; }
  else if (!strcmp(w, "stream")) { double *a = (double *)(b + lo0 * PS); size_t m = (hi0 - lo0) * PS / 8 / 3, mp = m / T, i0 = j->t * mp, i1 = j->t == T - 1 ? m : i0 + mp; for (int r = 0; r < 8 * SC; r++) for (size_t i = i0; i < i1; i++) a[i] = a[m + i] + 3.0 * a[2 * m + i]; chk += (uint64_t)a[i1 - 1]; }
  else if (!strcmp(w, "gather")) { long ops = (long)(2e8 * SC / div); uint64_t x = me * 7919 + 1, s = 0; for (long i = 0; i < ops; i++) { x = mix(x); s += tab[x % tn]; } chk += s; }
  else if (!strcmp(w, "sweep") || !strcmp(w, "hotcold")) {  /* capacity: write the whole slice, then read it (sweep: 2 full passes; hotcold: random reads, 90% in its first tenth) */
    uint64_t *a = (uint64_t *)(b + lo * PS); size_t m = (hi - lo) * PS / 8, s = 0; double tw = now();
    for (size_t i = 0; i < m; i += 8) a[i] = mix(i + lo);
    double tr = now();
    if (!strcmp(w, "sweep")) for (int r = 0; r < 2; r++) for (size_t i = 0; i < m; i += 8) { if (a[i] != mix(i + lo)) bad_g++; s += a[i]; }
    else { uint64_t x = me + 1, hot = m / 10 / 8; long ops = (long)(4e7 * SC / div); for (long i = 0; i < ops; i++) { x = mix(x); size_t q = (x % 10 ? x % hot : x % (m / 8)) * 8; if (a[q] != mix(q + lo)) bad_g++; s += a[q]; } }
    fprintf(stderr, "DSMN part k=%d t=%d write_s=%.3f read_s=%.3f\n", k, j->t, tr - tw, now() - tr); chk += s; }
  else if (!strcmp(w, "touch")) { for (size_t p = lo; p < hi; p++) b[p * PS + 8] = 1; chk += hi - lo; }
  j->chk = chk; return 0; }
int main(int c, char **v) {
  signal(SIGPIPE, SIG_IGN);
  if (c >= 4 && !strcmp(v[1], "map")) { int n = atoi(v[2]); size_t np = atol(v[3]) * 256, lo, hi; printf("0:16-%d", 16 + TP - 1); for (int k = 0; k < n; k++) { slice(np, n, k, &lo, &hi); printf(",%d:%zu-%zu", k, lo, hi - 1); } printf("\n"); return 0; }
  if (c < 6) { fprintf(stderr, "usage: dsmn ID N PATH MIB WORKLOAD [ARG] | dsmn map N MIB\n"); return 2; }
  int id = atoi(v[1]); N = atoi(v[2]); size_t np = atol(v[4]) * 256; const char *w = v[5]; double arg = c > 6 ? atof(v[6]) : 0;
  int solo = id < 0;  /* id -1: the one-machine baseline does every node's share in one process */
  int fd = open(v[3], O_RDWR | O_CREAT, 0600); if (fd < 0 || ftruncate(fd, np * PS)) { perror("open"); return 1; }
  char *b = mmap(0, np * PS, PROT_READ | PROT_WRITE, MAP_SHARED | (solo ? MAP_POPULATE : 0), fd, 0); if (b == MAP_FAILED) { perror("mmap"); return 1; }
  ctl = (volatile uint64_t *)b; uint64_t *tab = (uint64_t *)(b + 16 * PS); size_t tn = (size_t)TP * PS / 8;
  int k0 = solo ? 0 : id, k1 = solo ? N : id + 1; if (solo) N = 1;
  if (id <= 0 && !strcmp(w, "gather")) for (size_t i = 0; i < tn; i++) tab[i] = mix(i);  /* untimed setup: node 0 writes the table */
  if (!strcmp(w, "stream")) for (int k = k0; k < k1; k++) { size_t lo, hi; slice(np, solo ? atoi(v[2]) : N, k, &lo, &hi); double *a = (double *)(b + lo * PS); size_t m = (hi - lo) * PS / 8 / 3; for (size_t i = 0; i < m; i++) { a[m + i] = i; a[2 * m + i] = 1; } }
  barrier(1); double t0 = now(); uint64_t chk = 0; NN = solo ? atoi(v[2]) : N; SC = getenv("SCALE") ? atof(getenv("SCALE")) : 1; T = getenv("THREADS") ? atoi(getenv("THREADS")) : 1; if (T < 1) T = 1;
  struct job J[256]; pthread_t th[256]; int nj = 0; for (int k = k0; k < k1; k++) for (int t = 0; t < T; t++) { J[nj] = (struct job){.k = k, .t = t, .b = b, .np = np, .w = w, .arg = arg}; nj++; }
  for (int i = 0; i < nj; i++) pthread_create(&th[i], 0, run, &J[i]); for (int i = 0; i < nj; i++) { pthread_join(th[i], 0); chk += J[i].chk; }
  double t1 = now(); barrier(2); double t2 = now();
  /* correctness: stream/touch values are checked locally; shared counters must add up exactly */
  int bad = (int)bad_g;
  if (!strcmp(w, "stream")) for (int k = k0; k < k1; k++) { size_t lo, hi; slice(np, NN, k, &lo, &hi); double *a = (double *)(b + lo * PS); size_t m = (hi - lo) * PS / 8 / 3; for (size_t i = 0; i < m; i += 97) bad += a[i] != (double)i + 3.0; }
  barrier(3); if (!strcmp(w, "shared")) { if (id <= 0) { uint64_t tot = 0; for (int q = 0; q < 4; q++) tot += ((volatile uint64_t *)(b + 2 * PS))[q * 8]; long every = arg > 0 ? arg : 1000, ops = (long)(4e8 * SC / ((long)NN * T)), want = 0; for (long k = 0; k < (long)NN * T; k++) want += (ops + every - 1) / every; bad += tot != (uint64_t)want; } }
  printf("DSMN node%d n=%d work=%s compute_s=%.3f job_s=%.3f chk=%llx bad=%d\n", id, NN, w, t1 - t0, t2 - t0, (unsigned long long)chk, bad); fflush(stdout);
  barrier(4); usleep(200000); return 0; }
