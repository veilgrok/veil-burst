// LD_PRELOAD for QEMU-gvm (-shm-path): makes the guest-RAM shm mapping a veil-dsm client (MSI coherence with a
// home directory, see dsm.c). Each node runs its own QEMU-gvm with its own local /dev/shm file; this shim keeps
// those copies coherent at 4 KiB granularity via userfaultfd MISSING (fetch) + WP (upgrade/downgrade).
// env: VEIL_DSM_MATCH (substring of the shm path), VEIL_DSM_HOME=ip:port, VEIL_DSM_ID=0|1
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/userfaultfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#ifndef UFFD_FEATURE_WP_HUGETLBFS_SHMEM
#define UFFD_FEATURE_WP_HUGETLBFS_SHMEM (1 << 12)
#endif
#ifndef UFFDIO_REGISTER_MODE_WP
#define UFFDIO_REGISTER_MODE_WP ((__u64)1 << 1)
#endif
#ifndef UFFD_PAGEFAULT_FLAG_WP
#define UFFD_PAGEFAULT_FLAG_WP (1 << 1)
#endif
#ifndef UFFDIO_WRITEPROTECT
struct uffdio_writeprotect { struct uffdio_range range; __u64 mode; };
#define UFFDIO_WRITEPROTECT_MODE_WP ((__u64)1 << 0)
#define UFFDIO_WRITEPROTECT _IOWR(UFFDIO, 0x06, struct uffdio_writeprotect)
#endif
#ifndef UFFDIO_COPY_MODE_WP
#define UFFDIO_COPY_MODE_WP ((__u64)1 << 1)
#endif
#define PS 4096
enum { RD = 1, WR, ACK, DATA_S, DATA_M, GRANT_M, RECALL, INV, REPL, DATA_REP, DIFF, UPD, INVQ };
#define BM 512
#define MAXREP 8192
static uint32_t nrepl, *repidx, reppg[MAXREP]; static uint64_t *twin[MAXREP]; static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER; static volatile unsigned long ndiff_out, nupd_in;
static void addrep(uint64_t pg, const void *content) { pthread_mutex_lock(&rmu); if (!repidx[pg] && nrepl < MAXREP - 1) { uint32_t i = ++nrepl; reppg[i] = pg; twin[i] = malloc(PS); memcpy(twin[i], content, PS); repidx[pg] = i; } pthread_mutex_unlock(&rmu); }
struct msg { uint8_t t, has_data; uint16_t from; uint32_t pad; uint64_t pg; };
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int uffd = -1, memfd = -1, sock = -1, myid; static char *base; static size_t np; static uint8_t *pending;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER, smu = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned long nfault_r, nfault_w, nfault_wp, nrecv, nrecall_c, ninv_c, nzero_c, nstall; static double stall_sum, *fault_t0;
static int xfer(int fd, void *b, size_t n, int wr) { char *p = b; while (n) { ssize_t r = wr ? write(fd, p, n) : read(fd, p, n); if (r <= 0) return -1; p += r; n -= r; } return 0; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void sendm(struct msg *m, void *data) { pthread_mutex_lock(&smu); xfer(sock, m, sizeof *m, 1); if (data) xfer(sock, data, PS, 1); pthread_mutex_unlock(&smu); }
static void wake(uint64_t pg) { struct uffdio_range w = {(uintptr_t)base + pg * PS, PS}; ioctl(uffd, UFFDIO_WAKE, &w); }
static void setwp(uint64_t pg, int on) { struct uffdio_writeprotect w = {.range = {(uintptr_t)base + pg * PS, PS}, .mode = on ? UFFDIO_WRITEPROTECT_MODE_WP : 0}; ioctl(uffd, UFFDIO_WRITEPROTECT, &w); }
static void *receiver(void *a) {
  char *buf = aligned_alloc(PS, PS + BM);
  for (;;) { struct msg m; if (xfer(sock, &m, sizeof m, 0)) { fprintf(stderr, "DSM recv: home closed\n"); return 0; } uint64_t pg = m.pg; nrecv++;
    if (m.t == UPD) { xfer(sock, buf, PS + BM, 0); uint8_t *bm = (uint8_t *)buf + PS; uint64_t *nw = (uint64_t *)buf, *pw = (uint64_t *)(base + pg * PS); pthread_mutex_lock(&rmu); uint32_t i = repidx[pg];
      for (int w = 0; w < PS / 8; w++) if (bm[w >> 3] >> (w & 7) & 1) { __atomic_store_n(&pw[w], nw[w], __ATOMIC_RELAXED); if (i) twin[i][w] = nw[w]; } pthread_mutex_unlock(&rmu); nupd_in++; continue; }
    if (m.t == REPL) { char *pp = base + pg * PS; addrep(pg, pp); setwp(pg, 0); struct msg r = {.t = ACK, .has_data = 1, .pg = pg}; sendm(&r, pp); continue; }
    if (m.has_data) xfer(sock, buf, PS, 0); else if (m.t == DATA_S || m.t == DATA_M) { memset(buf, 0, PS); nzero_c++; }
    if (m.t == DATA_S || m.t == DATA_M) { struct uffdio_copy c = {.dst = (uintptr_t)base + pg * PS, .src = (uintptr_t)buf, .len = PS, .mode = m.t == DATA_S ? UFFDIO_COPY_MODE_WP : 0};
      if (ioctl(uffd, UFFDIO_COPY, &c)) setwp(pg, m.t == DATA_S); }
    else if (m.t == DATA_REP) { struct uffdio_copy c = {.dst = (uintptr_t)base + pg * PS, .src = (uintptr_t)buf, .len = PS, .mode = 0}; if (ioctl(uffd, UFFDIO_COPY, &c)) { setwp(pg, 0); memcpy(base + pg * PS, buf, PS); } addrep(pg, buf); }
    else if (m.t == GRANT_M) setwp(pg, 0);
    else if (m.t == RECALL) { setwp(pg, 1); struct msg r = {.t = ACK, .has_data = 1, .pg = pg}; sendm(&r, base + pg * PS); nrecall_c++; continue; }
    else if (m.t == INVQ) { setwp(pg, 1); fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, pg * PS, PS); ninv_c++; continue; }
    else if (m.t == INV) { setwp(pg, 1); struct msg r = {.t = ACK, .has_data = 1, .pg = pg}; sendm(&r, base + pg * PS); fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, pg * PS, PS); ninv_c++; continue; }
    pthread_mutex_lock(&mu); pending[pg] = 0; if (fault_t0[pg] > 0) { stall_sum += now() - fault_t0[pg]; nstall++; fault_t0[pg] = 0; } pthread_mutex_unlock(&mu); wake(pg); }
}
static void *faulter(void *a) {
  double last = now();
  for (;;) { struct pollfd p = {uffd, POLLIN, 0}; int r = poll(&p, 1, 1000);
    if (now() - last > 10) { last = now(); fprintf(stderr, "DSM node%d faults read=%lu write=%lu wp_upgrade=%lu replies=%lu zero=%lu recalls=%lu invs=%lu replicated=%u diffs_out=%lu upd_in=%lu stall_avg_us=%.1f\n", myid, nfault_r, nfault_w, nfault_wp, nrecv, nzero_c, nrecall_c, ninv_c, nrepl, ndiff_out, nupd_in, nstall ? stall_sum / nstall * 1e6 : 0); }
    if (r <= 0) continue; struct uffd_msg m; if (read(uffd, &m, sizeof m) != sizeof m || m.event != UFFD_EVENT_PAGEFAULT) continue;
    uint64_t pg = (m.arg.pagefault.address - (uintptr_t)base) / PS; int wp = m.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP, w = wp || (m.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE);
    if (wp) nfault_wp++; else if (w) nfault_w++; else nfault_r++;
    pthread_mutex_lock(&mu); int need = pending[pg] < (w ? WR : RD); if (need) { pending[pg] = w ? WR : RD; fault_t0[pg] = now(); } pthread_mutex_unlock(&mu);
    if (need) { struct msg q = {.t = w ? WR : RD, .from = myid, .pg = pg}; sendm(&q, 0); } }
  return 0;
}
static void *syncer(void *a) {  /* ship word-level diffs of replicated pages every VEIL_DSM_SYNC_US */
  long us = getenv("VEIL_DSM_SYNC_US") ? atol(getenv("VEIL_DSM_SYNC_US")) : 1000; char *out = aligned_alloc(PS, PS + BM);
  for (;;) { usleep(us); pthread_mutex_lock(&rmu);
    for (uint32_t i = 1; i <= nrepl; i++) { uint64_t *pw = (uint64_t *)(base + (uint64_t)reppg[i] * PS), *tw = twin[i], *ow = (uint64_t *)out; uint8_t *bm = (uint8_t *)out + PS; int any = 0;
      for (int w = 0; w < PS / 8; w++) { uint64_t v = __atomic_load_n(&pw[w], __ATOMIC_RELAXED); if (v != tw[w]) { if (!any) { memset(bm, 0, BM); any = 1; } ow[w] = v; tw[w] = v; bm[w >> 3] |= 1 << (w & 7); } }
      if (any) { struct msg r = {.t = DIFF, .has_data = 2, .pg = reppg[i]}; pthread_mutex_lock(&smu); xfer(sock, &r, sizeof r, 1); xfer(sock, out, PS + BM, 1); pthread_mutex_unlock(&smu); ndiff_out++; } }
    pthread_mutex_unlock(&rmu); } return 0; }
static void attach(void *p, size_t len, int fd, const char *tgt) {
  const char *hp = getenv("VEIL_DSM_HOME"); myid = atoi(getenv("VEIL_DSM_ID") ? getenv("VEIL_DSM_ID") : "0"); char h[64]; int port;
  if (!hp || sscanf(hp, "%63[^:]:%d", h, &port) != 2) { fprintf(stderr, "DSM: VEIL_DSM_HOME missing\n"); return; }
  sock = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)}; inet_pton(AF_INET, h, &a.sin_addr);
  for (int i = 0; connect(sock, (void *)&a, sizeof a); i++) { if (i > 300) { perror("DSM connect"); exit(1); } close(sock); sock = socket(AF_INET, SOCK_STREAM, 0); setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); sleep(1); }
  uint16_t id16 = myid; xfer(sock, &id16, 2, 1);
  base = p; np = len / PS; memfd = dup(fd); pending = calloc(np, 1); repidx = calloc(np, 4); fault_t0 = calloc(np, sizeof(double));
  fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, len);  /* start empty: every page comes from the directory */
  uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK); struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_MISSING_SHMEM | UFFD_FEATURE_WP_HUGETLBFS_SHMEM};
  struct uffdio_register reg = {.range = {(uintptr_t)p, len}, .mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP};
  if (uffd < 0 || ioctl(uffd, UFFDIO_API, &api) || ioctl(uffd, UFFDIO_REGISTER, &reg)) { perror("DSM uffd"); exit(1); }
  if (getenv("VEIL_DSM_PREFILL") && atoi(getenv("VEIL_DSM_PREFILL"))) {  /* every page starts as a local zero S copy (home marks both nodes sharers): no startup fetch storm */
    size_t ch = 2u << 20; char *z = aligned_alloc(PS, ch); memset(z, 0, ch); double t0 = now();
    for (size_t o = 0; o < len; o += ch) { struct uffdio_copy c = {.dst = (uintptr_t)p + o, .src = (uintptr_t)z, .len = len - o < ch ? len - o : ch, .mode = UFFDIO_COPY_MODE_WP}; if (ioctl(uffd, UFFDIO_COPY, &c)) { perror("DSM prefill"); break; } }
    fprintf(stderr, "DSM node%d prefilled %zu MiB as shared-zero in %.3fs\n", myid, len >> 20, now() - t0); free(z); }
  pthread_t t1, t2, t3; pthread_create(&t1, 0, receiver, 0); pthread_create(&t2, 0, faulter, 0); pthread_create(&t3, 0, syncer, 0);
  fprintf(stderr, "DSM node%d attached %s len=%zu MiB pages=%zu home=%s\n", myid, tgt, len >> 20, np, hp);
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
  void *p = real_mmap(addr, len, prot, flags, fd, off); const char *match = getenv("VEIL_DSM_MATCH");
  if (p != MAP_FAILED && match && fd >= 0 && uffd < 0 && (flags & MAP_SHARED) && len >= (64u << 20)) {
    char lp[64], tgt[256]; snprintf(lp, sizeof lp, "/proc/self/fd/%d", fd); ssize_t n = readlink(lp, tgt, sizeof tgt - 1);
    if (n > 0) { tgt[n] = 0; if (strstr(tgt, match)) attach(p, len, fd, tgt); } }
  return p;
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) __attribute__((alias("mmap")));
__attribute__((constructor)) static void hello(void) { if (getenv("VEIL_DSM_MATCH")) fprintf(stderr, "DSM shim loaded pid=%d\n", getpid()); }
