// veil-dsm coherence core (prototype): page-granular MSI (single writer / multi reader) between processes on
// different machines, using userfaultfd MISSING+WP on a shmem memfd and a home-node directory over TCP.
// This is the memory half needed to split one guest's vCPUs across machines on the two-level path
// (each node would run its share of vCPUs against the same guest-physical memfd).
//   dsm home PORT NPAGES                      directory + backing store for 2 clients
//   dsm client ID HOST:PORT NPAGES ROUNDS     ping-pong ownership test + cross-node checksum test
#define _GNU_SOURCE
#include <arpa/inet.h>
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
#define PS 4096
enum { RD = 1, WR, ACK, DATA_S, DATA_M, GRANT_M, RECALL, INV };
struct msg { uint8_t t, has_data; uint16_t from; uint32_t pad; uint64_t pg; };
static int xfer(int fd, void *b, size_t n, int wr) { char *p = b; while (n) { ssize_t r = wr ? write(fd, p, n) : read(fd, p, n); if (r <= 0) return -1; p += r; n -= r; } return 0; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
/* ---------------- home ---------------- */
struct q { int c; struct msg m; };
static int home(int port, size_t np) {
  char *mem = calloc(np, PS); int *owner = malloc(np * sizeof(int)); uint8_t *sh = calloc(np, 1); for (size_t i = 0; i < np; i++) owner[i] = -1;
  int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (bind(ls, (void *)&a, sizeof a) || listen(ls, 4)) { perror("bind"); return 1; } fprintf(stderr, "DSM home listening %d pages=%zu\n", port, np);
  int cs[2]; for (int i = 0; i < 2; i++) { int s = accept(ls, 0, 0); setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); uint16_t id; xfer(s, &id, 2, 0); cs[id & 1] = s; fprintf(stderr, "DSM home client %d connected\n", id); }
  struct q pend[4096]; int npend = 0; unsigned long nreq = 0, nrecall = 0, ninv = 0, bytes = 0; char tmp[PS];
  /* wait for the ACK for page pg from client c; queue other requests */
  #define WAITACK(cc, pg) for (;;) { struct msg r; if (xfer(cs[cc], &r, sizeof r, 0)) return 1; if (r.t == ACK && r.pg == (pg)) { if (r.has_data) { xfer(cs[cc], mem + (pg) * PS, PS, 0); bytes += PS; } break; } if (r.has_data) xfer(cs[cc], tmp, PS, 0); pend[npend].c = cc; pend[npend++].m = r; }
  int done = 0;
  while (done < 2) {
    struct q cur; if (npend) { cur = pend[0]; memmove(pend, pend + 1, --npend * sizeof *pend); }
    else { struct pollfd p[2] = {{cs[0], POLLIN, 0}, {cs[1], POLLIN, 0}}; poll(p, 2, -1); int c = (p[0].revents & (POLLIN | POLLHUP)) ? 0 : 1; cur.c = c; if (xfer(cs[c], &cur.m, sizeof cur.m, 0)) { done++; close(cs[c]); cs[c] = -1; if (done < 2) { /* other may still run */ } if (done == 2) break; continue; } }
    int c = cur.c, o = 1 - c; uint64_t pg = cur.m.pg; nreq++;
    if (pg >= np) { fprintf(stderr, "bad page\n"); continue; }
    if (cur.m.t == RD) {
      if (owner[pg] == o) { struct msg r = {.t = RECALL, .pg = pg}; xfer(cs[o], &r, sizeof r, 1); nrecall++; WAITACK(o, pg); owner[pg] = -1; sh[pg] |= 1 << o; }
      struct msg r = {.t = DATA_S, .has_data = 1, .pg = pg}; xfer(cs[c], &r, sizeof r, 1); xfer(cs[c], mem + pg * PS, PS, 1); bytes += PS; sh[pg] |= 1 << c;
    } else if (cur.m.t == WR) {
      if (owner[pg] == c) { struct msg r = {.t = GRANT_M, .pg = pg}; xfer(cs[c], &r, sizeof r, 1); continue; } /* duplicate */
      if (owner[pg] == o || (sh[pg] >> o & 1)) { struct msg r = {.t = INV, .pg = pg}; xfer(cs[o], &r, sizeof r, 1); ninv++; WAITACK(o, pg); sh[pg] &= ~(1 << o); }
      if (sh[pg] >> c & 1) { struct msg r = {.t = GRANT_M, .pg = pg}; xfer(cs[c], &r, sizeof r, 1); }
      else { struct msg r = {.t = DATA_M, .has_data = 1, .pg = pg}; xfer(cs[c], &r, sizeof r, 1); xfer(cs[c], mem + pg * PS, PS, 1); bytes += PS; }
      owner[pg] = c; sh[pg] = 0;
    }
  }
  fprintf(stderr, "DSM home done requests=%lu recalls=%lu invalidations=%lu data_MiB=%.1f\n", nreq, nrecall, ninv, bytes / 1048576.0); return 0;
}
/* ---------------- client ---------------- */
static int uffd, memfd, sock, myid; static char *base; static size_t np; static uint8_t *pending; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER, smu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long nfault_r, nfault_w, nfault_wp, nrecv, nrecall_c, ninv_c; static double fault_t0[1 << 16], stall_sum; static unsigned long nstall;
static void sendm(struct msg *m, void *data) { pthread_mutex_lock(&smu); xfer(sock, m, sizeof *m, 1); if (data) xfer(sock, data, PS, 1); pthread_mutex_unlock(&smu); }
static void wake(uint64_t pg) { struct uffdio_range w = {(uintptr_t)base + pg * PS, PS}; ioctl(uffd, UFFDIO_WAKE, &w); }
static void setwp(uint64_t pg, int on) { struct uffdio_writeprotect w = {.range = {(uintptr_t)base + pg * PS, PS}, .mode = on ? UFFDIO_WRITEPROTECT_MODE_WP : 0}; ioctl(uffd, UFFDIO_WRITEPROTECT, &w); }
static void *receiver(void *a) {
  static char buf[PS] __attribute__((aligned(PS)));
  for (;;) { struct msg m; if (xfer(sock, &m, sizeof m, 0)) return 0; uint64_t pg = m.pg; nrecv++;
    if (m.has_data) xfer(sock, buf, PS, 0);
    if (m.t == DATA_S || m.t == DATA_M) { struct uffdio_copy c = {.dst = (uintptr_t)base + pg * PS, .src = (uintptr_t)buf, .len = PS, .mode = m.t == DATA_S ? UFFDIO_COPY_MODE_WP : 0};
      if (ioctl(uffd, UFFDIO_COPY, &c)) { setwp(pg, m.t == DATA_S); } }
    else if (m.t == GRANT_M) setwp(pg, 0);
    else if (m.t == RECALL) { setwp(pg, 1); struct msg r = {.t = ACK, .has_data = 1, .pg = pg}; sendm(&r, base + pg * PS); nrecall_c++; continue; }
    else if (m.t == INV) { setwp(pg, 1); struct msg r = {.t = ACK, .has_data = 1, .pg = pg}; sendm(&r, base + pg * PS); fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, pg * PS, PS); ninv_c++; continue; }
    pthread_mutex_lock(&mu); pending[pg] = 0; if (pg < (1 << 16) && fault_t0[pg] > 0) { stall_sum += now() - fault_t0[pg]; nstall++; fault_t0[pg] = 0; } pthread_mutex_unlock(&mu); wake(pg); }
  return 0;
}
static void *faulter(void *a) __attribute__((noreturn));
static void *faulter(void *a) {
  for (;;) { struct pollfd p = {uffd, POLLIN, 0}; if (poll(&p, 1, -1) <= 0) continue; struct uffd_msg m; if (read(uffd, &m, sizeof m) != sizeof m || m.event != UFFD_EVENT_PAGEFAULT) continue;
    uint64_t pg = (m.arg.pagefault.address - (uintptr_t)base) / PS; int wp = m.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP, w = wp || (m.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE);
    if (wp) nfault_wp++; else if (w) nfault_w++; else nfault_r++;
    pthread_mutex_lock(&mu); int need = pending[pg] < (w ? WR : RD); if (need) { pending[pg] = w ? WR : RD; if (pg < (1 << 16)) fault_t0[pg] = now(); } pthread_mutex_unlock(&mu);
    if (need) { struct msg r = {.t = w ? WR : RD, .from = myid, .pg = pg}; sendm(&r, 0); } }
}
static int client(int id, const char *hp, size_t npages, long rounds) {
  myid = id; np = npages; pending = calloc(np, 1); char h[64]; int port; sscanf(hp, "%63[^:]:%d", h, &port);
  sock = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)}; inet_pton(AF_INET, h, &a.sin_addr);
  for (int i = 0; connect(sock, (void *)&a, sizeof a); i++) { if (i > 120) { perror("connect"); return 1; } sleep(1); } uint16_t id16 = id; xfer(sock, &id16, 2, 1);
  memfd = memfd_create("veil-dsm", 0); ftruncate(memfd, np * PS); base = mmap(0, np * PS, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
  uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK); struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_MISSING_SHMEM | UFFD_FEATURE_WP_HUGETLBFS_SHMEM};
  struct uffdio_register reg = {.range = {(uintptr_t)base, np * PS}, .mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP};
  if (ioctl(uffd, UFFDIO_API, &api) || ioctl(uffd, UFFDIO_REGISTER, &reg)) { perror("uffd"); return 1; }
  pthread_t t1, t2; pthread_create(&t1, 0, receiver, 0); pthread_create(&t2, 0, faulter, 0);
  volatile uint64_t *tok = (volatile uint64_t *)base;  /* page 0: ping-pong token (true sharing, every round moves ownership) */
  double t0 = now(); long r = 0;
  while (r < rounds) { uint64_t v = __atomic_load_n(tok, __ATOMIC_ACQUIRE); if ((long)v >= 2 * rounds) break; if ((v & 1) == (uint64_t)id) { __atomic_store_n(tok, v + 1, __ATOMIC_RELEASE); r++; } }
  double t1s = now(); printf("DSM client %d pingpong rounds=%ld total_s=%.3f per_handoff_us=%.1f\n", id, r, t1s - t0, (t1s - t0) / (2.0 * r) * 1e6);
  /* checksum test: each node fills its half (pages 1..np-1 split by parity), then both read all and verify */
  size_t half0 = 1; for (size_t pg = half0; pg < np; pg++) if (pg % 2 == (size_t)id) { uint32_t *w = (uint32_t *)(base + pg * PS); for (int i = 0; i < PS / 4; i++) w[i] = (uint32_t)(pg * 2654435761u + i) ^ (id ? 0xdeadbeef : 0x1234567); }
  volatile uint64_t *bar = (volatile uint64_t *)(base + 8); __atomic_fetch_add(bar, 1, __ATOMIC_ACQ_REL);
  while (__atomic_load_n(bar, __ATOMIC_ACQUIRE) < 2) ;
  uint64_t sum = 0, bad = 0; double t2s = now();
  for (size_t pg = half0; pg < np; pg++) { uint32_t *w = (uint32_t *)(base + pg * PS); int own = pg % 2; for (int i = 0; i < PS / 4; i++) { uint32_t e = (uint32_t)(pg * 2654435761u + i) ^ (own ? 0xdeadbeef : 0x1234567); sum += w[i]; if (w[i] != e) bad++; } }
  double t3s = now(); printf("DSM client %d checksum pages=%zu sum=%llu bad_words=%llu read_all_s=%.3f\n", id, np - 1, (unsigned long long)sum, (unsigned long long)bad, t3s - t2s);
  __atomic_fetch_add(bar, 1, __ATOMIC_ACQ_REL); while (__atomic_load_n(bar, __ATOMIC_ACQUIRE) < 4) ;  /* keep serving until peer finished reading */
  printf("DSM client %d faults read=%lu write=%lu wp_upgrade=%lu replies=%lu recalls_served=%lu invs_served=%lu stall_avg_us=%.1f\n", id, nfault_r, nfault_w, nfault_wp, nrecv, nrecall_c, ninv_c, nstall ? stall_sum / nstall * 1e6 : 0);
  fflush(stdout); sleep(2); return 0;
}
int main(int c, char **v) {
  if (c >= 4 && !strcmp(v[1], "home")) return home(atoi(v[2]), atol(v[3]));
  if (c >= 6 && !strcmp(v[1], "client")) return client(atoi(v[2]), v[3], atol(v[4]), atol(v[5]));
  fprintf(stderr, "usage: dsm home PORT NPAGES | dsm client ID HOST:PORT NPAGES ROUNDS\n"); return 2;
}
