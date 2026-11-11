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
static volatile unsigned long nfault, nserve_ns;
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
      uint64_t dst = m.arg.pagefault.address & ~(ps - 1), off = dst - (uintptr_t)base; void *src = zero; static char pg[65536];
      int remote = rs >= 0 && off >= split;
      if (remote) {
        if (fcap && (ft - fh) >= fcap) {  /* evict oldest remote-range page: write back, then punch it out (next touch faults again) */
          struct timespec e0, e1; clock_gettime(CLOCK_MONOTONIC, &e0); uint64_t vo = fifo[fh++ % fcap]; char op = 'P';
          if (xfer(rs, &op, 1, 1) || xfer(rs, &vo, 8, 1) || xfer(rs, (char *)base + vo, ps, 1)) { perror("UFFD evict"); }
          fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, vo, ps); nevict++;
          clock_gettime(CLOCK_MONOTONIC, &e1); evict_ns += (e1.tv_sec - e0.tv_sec) * 1000000000L + (e1.tv_nsec - e0.tv_nsec); }
        char op = 'G'; if (xfer(rs, &op, 1, 1) || xfer(rs, &off, 8, 1) || xfer(rs, pg, ps, 0)) { perror("UFFD fetch"); } else src = pg;
        if (fcap) fifo[ft++ % fcap] = off;
      }
      struct uffdio_copy c = {.dst = dst, .src = (uintptr_t)src, .len = ps, .mode = 0};
      if (ioctl(uffd, UFFDIO_COPY, &c) < 0) { struct uffdio_range w = {c.dst, ps}; ioctl(uffd, UFFDIO_WAKE, &w); }
      clock_gettime(CLOCK_MONOTONIC, &t1); long dt = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
      nfault++; nserve_ns += dt; if (remote) { nremote++; remote_ns += dt; } else nlocal++;
    }
    if (now() - last > 5) { last = now(); fprintf(stderr, "UFFD faults=%lu local=%lu remote=%lu evict=%lu serve_ms=%.1f remote_avg_us=%.1f evict_avg_us=%.1f mapped_MiB=%zu\n", nfault, nlocal, nremote, nevict, nserve_ns / 1e6, nremote ? remote_ns / 1e3 / nremote : 0, nevict ? evict_ns / 1e3 / nevict : 0, blen >> 20); }
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
        struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_MISSING_SHMEM};
        struct uffdio_register reg = {.range = {(uintptr_t)p, len}, .mode = UFFDIO_REGISTER_MODE_MISSING};
        if (uffd < 0 || ioctl(uffd, UFFDIO_API, &api) || ioctl(uffd, UFFDIO_REGISTER, &reg)) { perror("UFFD setup"); }
        else { base = p; blen = len; memfd = dup(fd); PS = sysconf(_SC_PAGESIZE);
          const char *r = getenv("VEIL_UFFD_REMOTE"); split = (size_t)atol(getenv("VEIL_UFFD_SPLIT_MB") ? getenv("VEIL_UFFD_SPLIT_MB") : "0") << 20; rbudget = (size_t)atol(getenv("VEIL_UFFD_RESIDENT_MB") ? getenv("VEIL_UFFD_RESIDENT_MB") : "0") << 20;
          if (r && *r) { rs = rconnect(r); fprintf(stderr, "UFFD remote %s %s split=%zuMiB resident_budget=%zuMiB\n", r, rs >= 0 ? "connected" : "FAILED", split >> 20, rbudget >> 20); if (rbudget) { fcap = rbudget / PS; fifo = calloc(fcap, 8); } }
          pthread_t t; pthread_create(&t, 0, pager, 0); fprintf(stderr, "UFFD registered %s len=%zu MiB\n", tgt, len >> 20); }
      }
    }
  }
  return p;
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) __attribute__((alias("mmap")));
__attribute__((constructor)) static void hello(void) { if (getenv("VEIL_UFFD_MATCH")) fprintf(stderr, "UFFD shim loaded pid=%d\n", getpid()); }
