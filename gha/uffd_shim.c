// LD_PRELOAD shim for stock QEMU: registers guest RAM (a shared memfd/file mapping whose path contains
// $VEIL_UFFD_MATCH) with userfaultfd MISSING mode and serves faults from a pager thread.
// This is the two-level alternative to GiantVM: DSM ownership lives in userspace on L1 with stock KVM.
// Pager policy here = single-owner local fast path (zero-fill / local copy on first touch), counting
// faults and service time; a remote fetch would replace the zero-fill in serve().
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <linux/falloc.h>
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int uffd = -1, memfd = -1, rs = -1; static void *base; static size_t blen, split, rbudget; static long PS;
static unsigned long nremote, nevict, nlocal, remote_ns, evict_ns; static uint64_t *fifo; static size_t fh, ft, fcap;
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
static int xfer(int fd, void *b, size_t n, int wr){ char *p=b; while(n){ ssize_t r = wr? write(fd,p,n): read(fd,p,n); if(r<=0) return -1; p+=r; n-=r;} return 0; }
static int rconnect(const char *hp){ char h[64]; int port; if(sscanf(hp,"%63[^:]:%d",h,&port)!=2) return -1; int s=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(s,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
  struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)}; inet_pton(AF_INET,h,&a.sin_addr); for(int i=0;i<60;i++){ if(!connect(s,(void*)&a,sizeof a)) return s; sleep(1);} return -1; }
static volatile unsigned long nfault, nserve_ns; static unsigned long nwp, nwperr, nprefetch; static int wpok; static uint32_t rbatch = 16, lbatch = 512; static char *rbuf;
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

/* ---- pipelined remote path (VEIL_UFFD_PIPE=1): pager only sends requests; a receiver thread installs chunks as they stream back.
   chunk = rbatch pages of the remote range; readahead keeps up to `ra` following chunks in flight; resident budget counted in chunks. */
static int pipe_on, ra = 8; static uint8_t *cst; static double *cfault; static size_t nchunks, *cfifo, cfh, cft, cfcap, nres; static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long ndata, nzero, ndemand, nhit_inflight, nhit_done, nreq, nstall; static double stall_sum;
static uint64_t coff(size_t c) { return split + (uint64_t)c * rbatch * PS; }
static uint32_t clen(size_t c) { uint64_t o = coff(c), e = o + (uint64_t)rbatch * PS; if (e > blen) e = blen; return (e - o) / PS; }
static void send_req(size_t c) { char q[13]; uint64_t o = coff(c); uint32_t n = clen(c); q[0] = 'Q'; memcpy(q + 1, &o, 8); memcpy(q + 9, &n, 4); if (xfer(rs, q, 13, 1)) perror("UFFD req"); nreq++; }
static void *recv_thr(void *a) {
  char *buf = aligned_alloc(PS, (size_t)rbatch * PS);
  static char zero4k[4096] __attribute__((aligned(4096)));
  for (;;) { uint64_t o, mask; uint32_t n; if (xfer(rs, &o, 8, 0) || xfer(rs, &n, 4, 0) || xfer(rs, &mask, 8, 0)) { perror("UFFD recv"); return 0; }
    int nd = __builtin_popcountll(mask); if (nd && xfer(rs, buf, (size_t)nd * PS, 0)) { perror("UFFD recv"); return 0; } ndata += nd; nzero += n - nd;
    size_t bi = 0; for (uint32_t i = 0; i < n;) { int d = mask >> i & 1; uint32_t j = i; while (j < n && (int)(mask >> j & 1) == d) j++;  /* run [i,j) of same kind */
      uint64_t dst = (uintptr_t)base + o + (uint64_t)i * PS, len = (uint64_t)(j - i) * PS;
      if (d) { struct uffdio_copy c = {.dst = dst, .src = (uintptr_t)buf + bi * PS, .len = len, .mode = UFFDIO_COPY_MODE_DONTWAKE}; if (ioctl(uffd, UFFDIO_COPY, &c)) for (uint32_t k = 0; k < j - i; k++) { struct uffdio_copy e = {.dst = dst + k * PS, .src = (uintptr_t)buf + (bi + k) * PS, .len = PS, .mode = UFFDIO_COPY_MODE_DONTWAKE}; ioctl(uffd, UFFDIO_COPY, &e); } bi += j - i; }
      else { struct uffdio_zeropage z = {.range = {dst, len}, .mode = UFFDIO_ZEROPAGE_MODE_DONTWAKE}; if (ioctl(uffd, UFFDIO_ZEROPAGE, &z)) for (uint32_t k = 0; k < j - i; k++) { struct uffdio_copy e = {.dst = dst + k * PS, .src = (uintptr_t)zero4k, .len = PS, .mode = UFFDIO_COPY_MODE_DONTWAKE}; ioctl(uffd, UFFDIO_COPY, &e); } }
      i = j; }
    { struct uffdio_range w = {(uintptr_t)base + o, (uint64_t)n * PS}; ioctl(uffd, UFFDIO_WAKE, &w); }
    size_t ci = (o - split) / ((uint64_t)rbatch * PS);
    pthread_mutex_lock(&smu); cst[ci] = 2; if (cfault[ci] > 0) { stall_sum += now() - cfault[ci]; nstall++; cfault[ci] = 0; } if (cfcap) cfifo[cft++ % (cfcap + 64)] = ci; pthread_mutex_unlock(&smu); nprefetch += n; }
}
static void pipe_evict_one(void) {  /* pager thread only: WP chunk, write it back, punch it */
  pthread_mutex_lock(&smu); if (cft == cfh) { pthread_mutex_unlock(&smu); return; } size_t c = cfifo[cfh++ % (cfcap + 64)]; pthread_mutex_unlock(&smu);
  struct timespec e0, e1; clock_gettime(CLOCK_MONOTONIC, &e0); uint64_t o = coff(c); uint32_t n = clen(c);
  if (wpok) { struct uffdio_writeprotect w = {.range = {(uintptr_t)base + o, (uint64_t)n * PS}, .mode = UFFDIO_WRITEPROTECT_MODE_WP}; if (ioctl(uffd, UFFDIO_WRITEPROTECT, &w)) nwperr++; }
  char q[13]; q[0] = 'W'; memcpy(q + 1, &o, 8); memcpy(q + 9, &n, 4); if (xfer(rs, q, 13, 1) || xfer(rs, (char *)base + o, (size_t)n * PS, 1)) perror("UFFD evict");
  fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, o, (off_t)n * PS); pthread_mutex_lock(&smu); cst[c] = 0; nres--; pthread_mutex_unlock(&smu); nevict += n;
  clock_gettime(CLOCK_MONOTONIC, &e1); evict_ns += (e1.tv_sec - e0.tv_sec) * 1000000000L + (e1.tv_nsec - e0.tv_nsec); }
static void pipe_fault(uint64_t off, uint64_t dst) {
  size_t c = (off - split) / ((uint64_t)rbatch * PS), want[64]; int nw = 0;
  pthread_mutex_lock(&smu);
  if (cst[c] == 0) { cst[c] = 1; cfault[c] = now(); ndemand++; want[nw++] = c; nres++; }
  else if (cst[c] == 1) { if (cfault[c] == 0) cfault[c] = now(); nhit_inflight++; }
  else { nhit_done++; struct uffdio_range w = {dst, PS}; ioctl(uffd, UFFDIO_WAKE, &w); }
  int rae = cfcap && (size_t)ra > cfcap / 32 ? (int)(cfcap / 32) : ra; for (int k = 1; k <= rae && nw < 64; k++) { size_t d = c + k; if (d >= nchunks) break; if (cst[d] == 0 && (!cfcap || nres + 1 < cfcap)) { cst[d] = 1; want[nw++] = d; nres++; } }
  pthread_mutex_unlock(&smu);
  while (cfcap && nres > cfcap) pipe_evict_one();
  for (int i = 0; i < nw; i++) send_req(want[i]);
  if (cfcap) while (nres + rae > cfcap && cft > cfh) pipe_evict_one();
}
static void *pager(void *a) {
  long ps = sysconf(_SC_PAGESIZE); char *zero = aligned_alloc(ps, ps); memset(zero, 0, ps);
  double last = now();
  for (;;) {
    struct pollfd p = {uffd, POLLIN, 0}; int r = poll(&p, 1, 200);
    if (r > 0) {
      struct uffd_msg m; if (read(uffd, &m, sizeof m) != sizeof m) continue;
      if (m.event != UFFD_EVENT_PAGEFAULT) continue;
      struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
      uint64_t dst = m.arg.pagefault.address & ~(ps - 1), off = dst - (uintptr_t)base;
      if (m.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) {  /* write hit a page being evicted: eviction already finished (single pager thread), so just unprotect + wake */
        struct uffdio_writeprotect w = {.range = {dst, ps}, .mode = 0}; ioctl(uffd, UFFDIO_WRITEPROTECT, &w); struct uffdio_range wr = {dst, ps}; ioctl(uffd, UFFDIO_WAKE, &wr); nwp++; continue; }
      int remote = rs >= 0 && off >= split;
      if (remote && pipe_on) { pipe_fault(off, dst); nfault++; nremote++; continue; }
      if (remote) {
        uint64_t b0 = off & ~((uint64_t)rbatch * ps - 1); uint32_t n = rbatch; if (b0 < split) b0 = split > off ? off : split; if (b0 + (uint64_t)n * ps > blen) n = (blen - b0) / ps;
        if (b0 > off || off >= b0 + (uint64_t)n * ps) { b0 = off; n = 1; }
        while (fcap && (ft - fh) + n > fcap && ft > fh) {  /* evict oldest: write-protect, copy+send, punch (WP stops a racing write from being lost) */
          struct timespec e0, e1; clock_gettime(CLOCK_MONOTONIC, &e0); uint64_t vo = fifo[fh++ % fcap]; char op = 'P';
          if (wpok) { struct uffdio_writeprotect w = {.range = {(uintptr_t)base + vo, ps}, .mode = UFFDIO_WRITEPROTECT_MODE_WP}; if (ioctl(uffd, UFFDIO_WRITEPROTECT, &w)) { nwperr++; } }
          if (xfer(rs, &op, 1, 1) || xfer(rs, &vo, 8, 1) || xfer(rs, (char *)base + vo, ps, 1)) { perror("UFFD evict"); }
          fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, vo, ps); nevict++;
          clock_gettime(CLOCK_MONOTONIC, &e1); evict_ns += (e1.tv_sec - e0.tv_sec) * 1000000000L + (e1.tv_nsec - e0.tv_nsec); }
        char op = 'R'; if (xfer(rs, &op, 1, 1) || xfer(rs, &b0, 8, 1) || xfer(rs, &n, 4, 1) || xfer(rs, rbuf, (size_t)n * ps, 0)) { perror("UFFD fetch"); n = 0; }
        for (uint32_t i = 0; i < n; i++) { uint64_t o = b0 + (uint64_t)i * ps; struct uffdio_copy c = {.dst = (uintptr_t)base + o, .src = (uintptr_t)rbuf + (size_t)i * ps, .len = ps, .mode = o == off ? 0 : UFFDIO_COPY_MODE_DONTWAKE};
          if (ioctl(uffd, UFFDIO_COPY, &c) == 0) { if (fcap) fifo[ft++ % fcap] = o; if (o != off) nprefetch++; } }
        struct uffdio_range w = {dst, ps}; ioctl(uffd, UFFDIO_WAKE, &w);
      } else {
        uint64_t lim = rs >= 0 ? split : blen, b0 = off & ~((uint64_t)lbatch * ps - 1), len = (uint64_t)lbatch * ps; if (b0 + len > lim) { b0 = off; len = ps; }
        struct uffdio_zeropage z = {.range = {(uintptr_t)base + b0, len}, .mode = 0};
        if (ioctl(uffd, UFFDIO_ZEROPAGE, &z)) { struct uffdio_copy c = {.dst = dst, .src = (uintptr_t)zero, .len = ps, .mode = 0}; if (ioctl(uffd, UFFDIO_COPY, &c) < 0) { struct uffdio_range w = {dst, ps}; ioctl(uffd, UFFDIO_WAKE, &w); } }
      }
      clock_gettime(CLOCK_MONOTONIC, &t1); long dt = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
      nfault++; nserve_ns += dt; if (remote) { nremote++; remote_ns += dt; } else nlocal++;
    }
    if (now() - last > 5) { last = now(); fprintf(stderr, "UFFD faults=%lu local=%lu remote=%lu evict=%lu serve_ms=%.1f remote_avg_us=%.1f evict_avg_us=%.1f prefetched=%lu wp_faults=%lu wp_err=%lu mapped_MiB=%zu\n", nfault, nlocal, nremote, nevict, nserve_ns / 1e6, nremote ? remote_ns / 1e3 / nremote : 0, nevict ? evict_ns / 1e3 / nevict : 0, nprefetch, nwp, nwperr, blen >> 20); if (pipe_on) fprintf(stderr, "UFFD pipe data_pages=%lu zero_pages=%lu demand=%lu hit_inflight=%lu hit_done=%lu req=%lu stall_n=%lu stall_avg_us=%.1f resident_chunks=%zu\n", ndata, nzero, ndemand, nhit_inflight, nhit_done, nreq, nstall, nstall ? stall_sum * 1e6 / nstall : 0, nres); }
  }
  return 0;
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
  void *p = real_mmap(addr, len, prot, flags, fd, off);
  const char *match = getenv("VEIL_UFFD_MATCH");
  if (p != MAP_FAILED && match && fd >= 0 && uffd < 0 && (flags & MAP_SHARED) && len >= (64u << 20)) {
    char lp[64], tgt[256]; snprintf(lp, sizeof lp, "/proc/self/fd/%d", fd); ssize_t n = readlink(lp, tgt, sizeof tgt - 1);
    if (n > 0) { tgt[n] = 0;
      if (strstr(tgt, match)) {
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
        int want_wp = getenv("VEIL_UFFD_RESIDENT_MB") && atol(getenv("VEIL_UFFD_RESIDENT_MB")) > 0;
        struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_MISSING_SHMEM | (want_wp ? UFFD_FEATURE_WP_HUGETLBFS_SHMEM : 0)};
        struct uffdio_register reg = {.range = {(uintptr_t)p, len}, .mode = UFFDIO_REGISTER_MODE_MISSING | (want_wp ? UFFDIO_REGISTER_MODE_WP : 0)};
        if (uffd >= 0 && ioctl(uffd, UFFDIO_API, &api) && want_wp) { close(uffd); uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK); api.features = UFFD_FEATURE_MISSING_SHMEM; reg.mode = UFFDIO_REGISTER_MODE_MISSING; want_wp = 0; ioctl(uffd, UFFDIO_API, &api); }
        wpok = want_wp; if (getenv("VEIL_UFFD_BATCH")) rbatch = atoi(getenv("VEIL_UFFD_BATCH")); if (getenv("VEIL_UFFD_LBATCH")) lbatch = atoi(getenv("VEIL_UFFD_LBATCH")); if (rbatch < 1) rbatch = 1; if (rbatch > 64) rbatch = 64; if (lbatch < 1) lbatch = 1;
        if (uffd < 0 || ioctl(uffd, UFFDIO_REGISTER, &reg)) { perror("UFFD setup"); }
        else { base = p; blen = len; memfd = dup(fd); PS = sysconf(_SC_PAGESIZE); rbuf = aligned_alloc(PS, (size_t)rbatch * PS);
          const char *r = getenv("VEIL_UFFD_REMOTE"); split = (size_t)atol(getenv("VEIL_UFFD_SPLIT_MB") ? getenv("VEIL_UFFD_SPLIT_MB") : "0") << 20; rbudget = (size_t)atol(getenv("VEIL_UFFD_RESIDENT_MB") ? getenv("VEIL_UFFD_RESIDENT_MB") : "0") << 20;
          if (r && *r) { rs = rconnect(r); fprintf(stderr, "UFFD remote %s %s split=%zuMiB resident_budget=%zuMiB\n", r, rs >= 0 ? "connected" : "FAILED", split >> 20, rbudget >> 20); if (rbudget) { fcap = rbudget / PS; fifo = calloc(fcap, 8); } }
          if (rs >= 0 && getenv("VEIL_UFFD_PIPE") && atoi(getenv("VEIL_UFFD_PIPE"))) { pipe_on = 1; if (getenv("VEIL_UFFD_RA")) ra = atoi(getenv("VEIL_UFFD_RA")); nchunks = (blen - split + (size_t)rbatch * PS - 1) / ((size_t)rbatch * PS); cst = calloc(nchunks, 1); cfault = calloc(nchunks, sizeof(double));
            if (rbudget) { cfcap = rbudget / ((size_t)rbatch * PS); cfifo = calloc(cfcap + 64, sizeof(size_t)); } pthread_t rt; pthread_create(&rt, 0, recv_thr, 0); fprintf(stderr, "UFFD pipe on chunks=%zu ra=%d cap_chunks=%zu\n", nchunks, ra, cfcap); }
          pthread_t t; pthread_create(&t, 0, pager, 0); fprintf(stderr, "UFFD registered %s len=%zu MiB wp=%d rbatch=%u lbatch=%u\n", tgt, len >> 20, wpok, rbatch, lbatch); }
      }
    }
  }
  return p;
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) __attribute__((alias("mmap")));
__attribute__((constructor)) static void hello(void) { if (getenv("VEIL_UFFD_MATCH")) fprintf(stderr, "UFFD shim loaded pid=%d\n", getpid()); }

__attribute__((destructor)) static void bye(void) { if (uffd < 0) return; fprintf(stderr, "UFFD FINAL faults=%lu local=%lu remote=%lu evict=%lu serve_ms=%.1f remote_avg_us=%.1f evict_avg_us=%.1f prefetched=%lu wp_faults=%lu\n", nfault, nlocal, nremote, nevict, nserve_ns / 1e6, nremote && !pipe_on ? remote_ns / 1e3 / nremote : 0, nevict ? evict_ns / 1e3 / nevict : 0, nprefetch, nwp);
  if (pipe_on) fprintf(stderr, "UFFD FINAL pipe data_pages=%lu zero_pages=%lu demand=%lu hit_inflight=%lu hit_done=%lu req=%lu stall_n=%lu stall_avg_us=%.1f\n", ndata, nzero, ndemand, nhit_inflight, nhit_done, nreq, nstall, nstall ? stall_sum * 1e6 / nstall : 0); }
