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
static volatile unsigned long nfault, nserve_ns; static unsigned long nwp, nwperr, nprefetch; static int wpok; static uint32_t rbatch = 16, lbatch = 64; static char *rbuf;
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
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
    if (now() - last > 5) { last = now(); fprintf(stderr, "UFFD faults=%lu local=%lu remote=%lu evict=%lu serve_ms=%.1f remote_avg_us=%.1f evict_avg_us=%.1f prefetched=%lu wp_faults=%lu wp_err=%lu mapped_MiB=%zu\n", nfault, nlocal, nremote, nevict, nserve_ns / 1e6, nremote ? remote_ns / 1e3 / nremote : 0, nevict ? evict_ns / 1e3 / nevict : 0, nprefetch, nwp, nwperr, blen >> 20); }
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
        wpok = want_wp; if (getenv("VEIL_UFFD_BATCH")) rbatch = atoi(getenv("VEIL_UFFD_BATCH")); if (getenv("VEIL_UFFD_LBATCH")) lbatch = atoi(getenv("VEIL_UFFD_LBATCH")); if (rbatch < 1) rbatch = 1; if (lbatch < 1) lbatch = 1;
        if (uffd < 0 || ioctl(uffd, UFFDIO_REGISTER, &reg)) { perror("UFFD setup"); }
        else { base = p; blen = len; memfd = dup(fd); PS = sysconf(_SC_PAGESIZE); rbuf = aligned_alloc(PS, (size_t)rbatch * PS);
          const char *r = getenv("VEIL_UFFD_REMOTE"); split = (size_t)atol(getenv("VEIL_UFFD_SPLIT_MB") ? getenv("VEIL_UFFD_SPLIT_MB") : "0") << 20; rbudget = (size_t)atol(getenv("VEIL_UFFD_RESIDENT_MB") ? getenv("VEIL_UFFD_RESIDENT_MB") : "0") << 20;
          if (r && *r) { rs = rconnect(r); fprintf(stderr, "UFFD remote %s %s split=%zuMiB resident_budget=%zuMiB\n", r, rs >= 0 ? "connected" : "FAILED", split >> 20, rbudget >> 20); if (rbudget) { fcap = rbudget / PS; fifo = calloc(fcap, 8); } }
          pthread_t t; pthread_create(&t, 0, pager, 0); fprintf(stderr, "UFFD registered %s len=%zu MiB wp=%d rbatch=%u lbatch=%u\n", tgt, len >> 20, wpok, rbatch, lbatch); }
      }
    }
  }
  return p;
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) __attribute__((alias("mmap")));
__attribute__((constructor)) static void hello(void) { if (getenv("VEIL_UFFD_MATCH")) fprintf(stderr, "UFFD shim loaded pid=%d\n", getpid()); }
