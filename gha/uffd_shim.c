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
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int uffd = -1; static void *base; static size_t blen;
static volatile unsigned long nfault, nserve_ns;
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void *pager(void *a) {
  long ps = sysconf(_SC_PAGESIZE); char *zero = aligned_alloc(ps, ps); memset(zero, 0, ps);
  double last = now();
  for (;;) {
    struct pollfd p = {uffd, POLLIN, 0}; int r = poll(&p, 1, 1000);
    if (r > 0) {
      struct uffd_msg m; if (read(uffd, &m, sizeof m) != sizeof m) continue;
      if (m.event != UFFD_EVENT_PAGEFAULT) continue;
      struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
      struct uffdio_copy c = {.dst = m.arg.pagefault.address & ~(ps - 1), .src = (uintptr_t)zero, .len = ps, .mode = 0};
      if (ioctl(uffd, UFFDIO_COPY, &c) < 0) { struct uffdio_range w = {c.dst, ps}; ioctl(uffd, UFFDIO_WAKE, &w); }
      clock_gettime(CLOCK_MONOTONIC, &t1); nfault++; nserve_ns += (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
    }
    if (now() - last > 5) { last = now(); fprintf(stderr, "UFFD faults=%lu serve_ms=%.1f mapped_MiB=%zu\n", nfault, nserve_ns / 1e6, blen >> 20); }
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
        else { base = p; blen = len; pthread_t t; pthread_create(&t, 0, pager, 0); fprintf(stderr, "UFFD registered %s len=%zu MiB\n", tgt, len >> 20); }
      }
    }
  }
  return p;
}
