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
#include <sys/time.h>
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
enum { RD = 1, WR, ACK, DATA_S, DATA_M, GRANT_M, RECALL, INV, REPL, DATA_REP, DIFF, UPD, INVQ, MPUT_, UNREP, FENCE, SWW, SWDATA, SWGRANT, SWDEMOTE, SWSYNC, DIFFB, UPDB };
#define SWB 64
#define BM 4096  /* diff payload: new words + per-word bit masks of what this node changed */
#define MAXREP 8192
static uint32_t nrepl, *repidx, reppg[MAXREP]; static uint64_t *twin[MAXREP]; static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER; static volatile unsigned long ndiff_out, nupd_in;
static void addrep(uint64_t pg, const void *content) { pthread_mutex_lock(&rmu); if (!repidx[pg] && nrepl < MAXREP - 1) { uint32_t i = ++nrepl; reppg[i] = pg; twin[i] = malloc(PS); memcpy(twin[i], content, PS); repidx[pg] = i; } pthread_mutex_unlock(&rmu); }
static unsigned long nrepdrop, nrepskip, nupd_drop, nrelflush;
static void droprep(uint64_t pg) { pthread_mutex_lock(&rmu); uint32_t i = repidx[pg]; if (i) { free(twin[i]); twin[i] = twin[nrepl]; reppg[i] = reppg[nrepl]; repidx[reppg[i]] = i; repidx[pg] = 0; nrepl--; nrepdrop++; } pthread_mutex_unlock(&rmu); }
struct msg { uint8_t t, has_data; uint16_t from; uint32_t seq; uint64_t pg; };  /* seq: per-connection, per-direction; gaps counted */
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int uffd = -1, memfd = -1, sock = -1, myid; static char *base; static size_t np; static uint8_t *pending, *st;  /* st: 0 none 1 S 2 M 3 replicated 4 single-writer read replica (WP, updated by atomic page swap) */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER, smu = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned long nfault_r, nfault_w, nfault_wp, nrecv, nrecall_c, ninv_c, nzero_c, nstall, nunsol, seqerr_c, nfailover; static double stall_sum, *fault_t0; static unsigned long nfence, nfence_to; static pthread_mutex_t fmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t fcv = PTHREAD_COND_INITIALIZER; static volatile uint64_t fdone, fgen; static volatile int need_fence, fence_grants; static uint32_t sseq, rseq;
/* writebacks sent to the home but not yet known to be mirrored: cleared by the next message from the home (it mirrors before replying) */
static uint32_t *ghost_pg; static char *ghost_data; static int nghost, maxghost = 4096; static pthread_mutex_t gmu = PTHREAD_MUTEX_INITIALIZER;
static int xfer(int fd, void *b, size_t n, int wr) { char *p = b; while (n) { ssize_t r = wr ? write(fd, p, n) : read(fd, p, n); if (r <= 0) return -1; p += r; n -= r; } return 0; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void sendm_locked(struct msg *m, void *data, size_t dl) { m->seq = sseq++; xfer(sock, m, sizeof *m, 1); if (data) xfer(sock, data, dl, 1); }
static void sendm(struct msg *m, void *data) { pthread_mutex_lock(&smu); sendm_locked(m, data, PS); pthread_mutex_unlock(&smu); }
static void wake(uint64_t pg) { struct uffdio_range w = {(uintptr_t)base + pg * PS, PS}; ioctl(uffd, UFFDIO_WAKE, &w); }
static void setwp(uint64_t pg, int on) { struct uffdio_writeprotect w = {.range = {(uintptr_t)base + pg * PS, PS}, .mode = on ? UFFDIO_WRITEPROTECT_MODE_WP : 0}; ioctl(uffd, UFFDIO_WRITEPROTECT, &w); }
static int dial(const char *hp) { char h[64]; int port; if (!hp || sscanf(hp, "%63[^:]:%d", h, &port) != 2) return -1; int s = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)}; inet_pton(AF_INET, h, &a.sin_addr); struct timeval tv = {2, 0}; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  if (connect(s, (void *)&a, sizeof a)) { close(s); return -1; } tv.tv_sec = 0; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); return s; }
/* failover: reconnect (standby first, then primary), send state dump + unmirrored writebacks + replicated pages, re-issue outstanding requests */
static int failover(void) {
  const char *h2 = getenv("VEIL_DSM_HOME2"), *h1 = getenv("VEIL_DSM_HOME"); if (!h2) return -1; double t0 = now();
  pthread_mutex_lock(&smu); close(sock); sock = -1; fprintf(stderr, "DSM node%d: home lost, failing over to %s\n", myid, h2);
  for (int i = 0; i < 240 && sock < 0; i++) { sock = dial(i & 1 ? h1 : h2); if (sock < 0) usleep(500000); }
  if (sock < 0) { pthread_mutex_unlock(&smu); fprintf(stderr, "DSM node%d: no home reachable -> giving up\n", myid); return -1; }
  sseq = rseq = 0; uint16_t id16 = myid | 0x8000; xfer(sock, &id16, 2, 1);
  uint32_t n = 0; for (size_t i = 0; i < np; i++) n += st[i] != 0; xfer(sock, &n, 4, 1);
  char *eb = malloc(5 * (size_t)n + 1), *e = eb; for (uint32_t i = 0; i < np; i++) if (st[i]) { memcpy(e, &i, 4); e[4] = st[i] == 4 ? 1 : st[i]; e += 5; } xfer(sock, eb, e - eb, 1); free(eb);
  pthread_mutex_lock(&gmu); uint32_t ng = nghost; xfer(sock, &ng, 4, 1); for (int i = 0; i < nghost; i++) { xfer(sock, &ghost_pg[i], 4, 1); xfer(sock, ghost_data + (size_t)i * PS, PS, 1); } nghost = 0; pthread_mutex_unlock(&gmu);
  uint32_t nr = 0; for (uint32_t i = 1; i <= nrepl; i++) nr += st[reppg[i]] == 3; xfer(sock, &nr, 4, 1); for (uint32_t i = 1; i <= nrepl; i++) if (st[reppg[i]] == 3) { uint32_t pg = reppg[i]; xfer(sock, &pg, 4, 1); xfer(sock, base + (size_t)pg * PS, PS, 1); }
  int nre = 0; for (size_t pg = 0; pg < np; pg++) if (pending[pg]) { struct msg q = {.t = pending[pg], .from = myid, .pg = pg}; sendm_locked(&q, 0, 0); nre++; }
  pthread_mutex_unlock(&smu); nfailover++;
  fprintf(stderr, "DSM node%d: FAILOVER done in %.3fs (states=%u unmirrored_wb=%u replicated=%u reissued=%d)\n", myid, now() - t0, n, ng, nr, nre); return 0; }
static unsigned long nswin, nswfail, nbatch_out, nbatch_in;
/* single-writer replica update: the guest sees either the whole old page or the whole new one (seqcount-safe):
   the page is read-only here (WP), so copy it, merge the writer's bits, punch it out (vCPU reads now block in uffd), install */
static void swinstall(uint64_t pg, const uint64_t *nw, const uint64_t *mk, char *ib) {
  char *pp = base + pg * PS; uint64_t *iw = (uint64_t *)ib;
  if (mk) { memcpy(ib, pp, PS); for (int w = 0; w < PS / 8; w++) if (mk[w]) iw[w] = (iw[w] & ~mk[w]) | (nw[w] & mk[w]); } else memcpy(ib, nw, PS);
  fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, pg * PS, PS);
  struct uffdio_copy c = {.dst = (uintptr_t)pp, .src = (uintptr_t)ib, .len = PS, .mode = UFFDIO_COPY_MODE_WP};
  if (ioctl(uffd, UFFDIO_COPY, &c)) { nswfail++; setwp(pg, 0); memcpy(pp, ib, PS); setwp(pg, 1); } nswin++; wake(pg); }
static void relflush(void);
static void *receiver(void *a) {
  char *buf = aligned_alloc(PS, PS + BM), *ib = aligned_alloc(PS, PS);
  for (;;) { struct msg m; if (xfer(sock, &m, sizeof m, 0)) { if (failover() == 0) continue; fprintf(stderr, "DSM recv: home closed\n"); return 0; }
    if (m.seq != rseq) seqerr_c++; rseq = m.seq + 1; uint64_t pg = m.pg; nrecv++;
    if (nghost) { pthread_mutex_lock(&gmu); nghost = 0; pthread_mutex_unlock(&gmu); }
    if (m.t == FENCE && (m.pg >> 62)) { struct msg r = {.t = ACK, .from = myid, .pg = m.pg}; pthread_mutex_lock(&smu); sendm_locked(&r, 0, 0); pthread_mutex_unlock(&smu); continue; }
    if (m.t == FENCE) { pthread_mutex_lock(&fmu); fdone = m.pg; pthread_cond_broadcast(&fcv); pthread_mutex_unlock(&fmu); continue; }
    if (m.t == UPD) { if (xfer(sock, buf, PS + BM, 0)) continue; uint64_t *mk = (uint64_t *)(buf + PS), *nw = (uint64_t *)buf, *pw = (uint64_t *)(base + pg * PS);
      if (st[pg] == 4) { swinstall(pg, nw, mk, ib); nupd_in++; continue; }
      pthread_mutex_lock(&rmu); uint32_t i = repidx[pg];
      if (i) for (int w = 0; w < PS / 8; w++) if (mk[w]) { uint64_t o = __atomic_load_n(&pw[w], __ATOMIC_RELAXED), n; do n = (o & ~mk[w]) | (nw[w] & mk[w]); while (!__atomic_compare_exchange_n(&pw[w], &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)); twin[i][w] = (twin[i][w] & ~mk[w]) | (nw[w] & mk[w]); } if (!i) { nupd_drop++; if (nupd_drop < 6) fprintf(stderr, "DSM node%d UPD for non-replicated pg=%llx st=%d\n", myid, (unsigned long long)pg, st[pg]); } pthread_mutex_unlock(&rmu); nupd_in++; continue; }
    if (m.t == UNREP) {  /* stop writing, flush pending diff, leave the replicated set; an INVQ follows */
      pthread_mutex_lock(&rmu); setwp(pg, 1); uint32_t i = repidx[pg];
      if (i) { uint64_t *pw = (uint64_t *)(base + pg * PS), *tw = twin[i], *ow = (uint64_t *)buf; uint64_t *mk = (uint64_t *)(buf + PS); int any = 0; memset(mk, 0, BM);
        for (int w = 0; w < PS / 8; w++) { uint64_t v = pw[w]; if (v != tw[w]) { ow[w] = v; mk[w] = v ^ tw[w]; tw[w] = v; any = 1; } }
        if (any) { struct msg r = {.t = DIFF, .has_data = 2, .pg = pg}; pthread_mutex_lock(&smu); sendm_locked(&r, buf, PS + BM); pthread_mutex_unlock(&smu); ndiff_out++; }
        free(twin[i]); twin[i] = twin[nrepl]; reppg[i] = reppg[nrepl]; repidx[reppg[i]] = i; repidx[pg] = 0; nrepl--; }
      st[pg] = 1; pthread_mutex_unlock(&rmu); struct msg r = {.t = ACK, .pg = pg}; sendm(&r, 0); continue; }
    if (m.t == UPDB) {  /* group swap: prepare all merged pages, punch them all (reads block), install them all */
      static char *ub, *ibs; static uint64_t pgs[SWB]; const size_t E = 8 + PS + BM; uint32_t k = (uint32_t)m.pg; if (!ub) { ub = malloc(SWB * E); ibs = aligned_alloc(PS, SWB * PS); }
      if (k > SWB || xfer(sock, ub, k * E, 0)) continue; int q = 0; nbatch_in++;
      for (uint32_t j = 0; j < k; j++) { char *e = ub + j * E; uint64_t p = *(uint64_t *)e; uint64_t *nw = (uint64_t *)(e + 8), *mk = (uint64_t *)(e + 8 + PS); nupd_in++;
        if (p >= np || st[p] != 4) { nupd_drop++; continue; } uint64_t *iw = (uint64_t *)(ibs + q * PS); memcpy(iw, base + p * PS, PS);
        for (int w = 0; w < PS / 8; w++) if (mk[w]) iw[w] = (iw[w] & ~mk[w]) | (nw[w] & mk[w]); pgs[q++] = p; }
      for (int j = 0; j < q; j++) fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, pgs[j] * PS, PS);
      for (int j = 0; j < q; j++) { struct uffdio_copy c = {.dst = (uintptr_t)base + pgs[j] * PS, .src = (uintptr_t)(ibs + j * PS), .len = PS, .mode = UFFDIO_COPY_MODE_WP};
        if (ioctl(uffd, UFFDIO_COPY, &c)) { nswfail++; setwp(pgs[j], 0); memcpy(base + pgs[j] * PS, ibs + j * PS, PS); setwp(pgs[j], 1); } nswin++; }
      for (int j = 0; j < q; j++) wake(pgs[j]); continue; }
    if (m.t == SWW) { char *pp = base + pg * PS; addrep(pg, pp); struct msg r = {.t = ACK, .has_data = 1, .pg = pg}; sendm(&r, pp); continue; }  /* we own it (M): keep writing, ship diffs */
    if (m.t == SWSYNC) { relflush(); struct msg r = {.t = ACK, .pg = pg}; sendm(&r, 0); continue; }
    if (m.t == SWDEMOTE) {  /* writer -> read replica: write-protect, flush the last diff, drop the twin */
      pthread_mutex_lock(&rmu); setwp(pg, 1); uint32_t i = repidx[pg];
      if (i) { uint64_t *pw = (uint64_t *)(base + pg * PS), *tw = twin[i], *ow = (uint64_t *)buf; uint64_t *mk = (uint64_t *)(buf + PS); int any = 0; memset(mk, 0, BM);
        for (int w = 0; w < PS / 8; w++) { uint64_t v = pw[w]; if (v != tw[w]) { ow[w] = v; mk[w] = v ^ tw[w]; tw[w] = v; any = 1; } }
        if (any) { struct msg r = {.t = DIFF, .has_data = 2, .pg = pg}; pthread_mutex_lock(&smu); sendm_locked(&r, buf, PS + BM); pthread_mutex_unlock(&smu); ndiff_out++; }
        free(twin[i]); twin[i] = twin[nrepl]; reppg[i] = reppg[nrepl]; repidx[reppg[i]] = i; repidx[pg] = 0; nrepl--; }
      st[pg] = 4; pthread_mutex_unlock(&rmu); struct msg r = {.t = ACK, .pg = pg}; sendm(&r, 0); continue; }
    if (m.t == SWDATA) { if (xfer(sock, buf, PS, 0)) continue; m.has_data = 0; struct uffdio_copy c = {.dst = (uintptr_t)base + pg * PS, .src = (uintptr_t)buf, .len = PS, .mode = UFFDIO_COPY_MODE_WP};
      if (ioctl(uffd, UFFDIO_COPY, &c)) { setwp(pg, 1); swinstall(pg, (uint64_t *)buf, 0, ib); } st[pg] = 4; }
    else if (m.t == SWGRANT) { char *pp = base + pg * PS;  /* become the single writer (our replica is current: all UPDs came first) */
      if (m.has_data) { if (xfer(sock, buf, PS, 0)) continue; m.has_data = 0; struct uffdio_copy c = {.dst = (uintptr_t)pp, .src = (uintptr_t)buf, .len = PS, .mode = 0}; if (ioctl(uffd, UFFDIO_COPY, &c)) { setwp(pg, 0); memcpy(pp, buf, PS); } }
      addrep(pg, pp); setwp(pg, 0); st[pg] = 2; }
    else if (m.t == REPL) { char *pp = base + pg * PS; addrep(pg, pp); setwp(pg, 0); st[pg] = 3; struct msg r = {.t = ACK, .has_data = 1, .pg = pg}; sendm(&r, pp); continue; }
    if ((m.t == DATA_M || m.t == GRANT_M) && fence_grants) need_fence = 1;  /* our ownership may have ack-free invalidated other copies: next interrupt waits until they are applied */
    if (m.has_data) { if (xfer(sock, buf, PS, 0)) continue; } else if (m.t == DATA_S || m.t == DATA_M) { memset(buf, 0, PS); nzero_c++; }
    if (m.t == DATA_S || m.t == DATA_M) { struct uffdio_copy c = {.dst = (uintptr_t)base + pg * PS, .src = (uintptr_t)buf, .len = PS, .mode = m.t == DATA_S ? UFFDIO_COPY_MODE_WP : 0};
      if (ioctl(uffd, UFFDIO_COPY, &c)) setwp(pg, m.t == DATA_S); st[pg] = m.t == DATA_S ? 1 : 2; if (!pending[pg]) nunsol++; }
    else if (m.t == DATA_REP) { struct uffdio_copy c = {.dst = (uintptr_t)base + pg * PS, .src = (uintptr_t)buf, .len = PS, .mode = 0}; if (ioctl(uffd, UFFDIO_COPY, &c)) { setwp(pg, 0); memcpy(base + pg * PS, buf, PS); } addrep(pg, buf); st[pg] = 3; }
    else if (m.t == GRANT_M) { setwp(pg, 0); st[pg] = 2; if (!pending[pg]) nunsol++; }
    else if (m.t == RECALL) { relflush(); if (repidx[pg]) droprep(pg); setwp(pg, 1); if (st[pg] == 2) { pthread_mutex_lock(&gmu); if (nghost < maxghost) { ghost_pg[nghost] = pg; memcpy(ghost_data + (size_t)nghost * PS, base + pg * PS, PS); nghost++; } pthread_mutex_unlock(&gmu); } st[pg] = 1; struct msg r = {.t = ACK, .has_data = 1, .pg = pg}; sendm(&r, base + pg * PS); nrecall_c++; continue; }
    else if (m.t == INVQ) { if (repidx[pg]) droprep(pg); setwp(pg, 1); st[pg] = 0; fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, pg * PS, PS); ninv_c++; continue; }
    else if (m.t == INV) { relflush(); if (repidx[pg]) droprep(pg); setwp(pg, 1); int had = st[pg] == 2; st[pg] = 0;
      if (had) { pthread_mutex_lock(&gmu); if (nghost < maxghost) { ghost_pg[nghost] = pg; memcpy(ghost_data + (size_t)nghost * PS, base + pg * PS, PS); nghost++; } pthread_mutex_unlock(&gmu); }
      struct msg r = {.t = ACK, .has_data = 1, .pg = pg}; sendm(&r, base + pg * PS); fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, pg * PS, PS); ninv_c++; continue; }
    pthread_mutex_lock(&mu); pending[pg] = 0; if (fault_t0[pg] > 0) { stall_sum += now() - fault_t0[pg]; nstall++; fault_t0[pg] = 0; } pthread_mutex_unlock(&mu); wake(pg); }
}
static void *faulter(void *a) {
  double last = now(), lastwd = now(), wd = getenv("VEIL_DSM_FAILOVER_S") ? atof(getenv("VEIL_DSM_FAILOVER_S")) : 5;
  for (;;) { struct pollfd p = {uffd, POLLIN, 0}; int r = poll(&p, 1, 500);
    if (now() - last > 10) { last = now(); fprintf(stderr, "DSM node%d faults read=%lu write=%lu wp_upgrade=%lu replies=%lu unsolicited=%lu zero=%lu recalls=%lu invs=%lu replicated=%u diffs_out=%lu upd_in=%lu seq_err=%lu failovers=%lu stall_avg_us=%.1f fences=%lu fence_timeouts=%lu rep_drop=%lu rep_skip=%lu upd_drop=%lu relflush=%lu sw_install=%lu sw_fail=%lu batches_out=%lu batches_in=%lu\n", myid, nfault_r, nfault_w, nfault_wp, nrecv, nunsol, nzero_c, nrecall_c, ninv_c, nrepl, ndiff_out, nupd_in, seqerr_c, nfailover, nstall ? stall_sum / nstall * 1e6 : 0, nfence, nfence_to, nrepdrop, nrepskip, nupd_drop, nrelflush, nswin, nswfail, nbatch_out, nbatch_in); }
    { static double lst; if (now() - lst > 10) { lst = now(); double tn = now(); int ns = 0; for (size_t i = 0; i < np; i++) if (fault_t0[i] > 0 && tn - fault_t0[i] > 2) { if (ns++ < 4) fprintf(stderr, "DSM node%d STALL pg=%zx age=%.1fs pending=%d st=%d\n", myid, i, tn - fault_t0[i], pending[i], st[i]); } if (ns) fprintf(stderr, "DSM node%d STALL total=%d\n", myid, ns); } }
    if (getenv("VEIL_DSM_HOME2") && now() - lastwd > 1) { lastwd = now(); double tn = now(); for (size_t i = 0; i < np; i++) if (fault_t0[i] > 0 && tn - fault_t0[i] > wd) { fprintf(stderr, "DSM node%d: request for page %zx unanswered %.1fs -> declaring home dead\n", myid, i, tn - fault_t0[i]); shutdown(sock, SHUT_RDWR); for (size_t j = 0; j < np; j++) if (fault_t0[j] > 0) fault_t0[j] = tn + 30; break; } }
    if (r <= 0) continue; struct uffd_msg m; if (read(uffd, &m, sizeof m) != sizeof m || m.event != UFFD_EVENT_PAGEFAULT) continue;
    uint64_t pg = (m.arg.pagefault.address - (uintptr_t)base) / PS; int wp = m.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP, w = wp || (m.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE);
    if ((st[pg] == 4 && !w) || (wp && st[pg] == 2)) { wake(pg); continue; }  /* SW replica swap / SW writer flush cut in progress: the vCPU retries */  /* single-writer replica being swapped: the install wakes it */
    if (wp) nfault_wp++; else if (w) nfault_w++; else nfault_r++;
    pthread_mutex_lock(&mu); int need = pending[pg] < (w ? WR : RD); if (need) { pending[pg] = w ? WR : RD; fault_t0[pg] = now(); } pthread_mutex_unlock(&mu);
    if (need) { struct msg q = {.t = w ? WR : RD, .from = myid, .pg = pg}; sendm(&q, 0); } }
  return 0;
}
/* word-level diffs of replicated pages: shipped every VEIL_DSM_SYNC_US and synchronously before any interrupt leaves this node */
/* single-writer pages this node writes: write-block every changed one, then diff them all (a consistent cut of the
   guest's stores), ship them as one DIFFB, then let the vCPUs write again. Caller holds rmu. */
static int swbatch(void) { static uint32_t ch[SWB]; static char *bb; const size_t E = 8 + PS + BM; int k = 0;
  for (uint32_t i = 1; i <= nrepl && k < SWB; i++) if (st[reppg[i]] == 2 && memcmp(base + (uint64_t)reppg[i] * PS, twin[i], PS)) ch[k++] = i;
  if (!k) return 0; if (!bb) bb = malloc(SWB * E);
  for (int j = 0; j < k; j++) setwp(reppg[ch[j]], 1);
  for (int j = 0; j < k; j++) { uint32_t i = ch[j]; uint64_t pg = reppg[i], *pw = (uint64_t *)(base + pg * PS), *tw = twin[i]; char *e = bb + j * E; uint64_t *ow = (uint64_t *)(e + 8), *mk = (uint64_t *)(e + 8 + PS);
    *(uint64_t *)e = pg; memset(mk, 0, BM); for (int w = 0; w < PS / 8; w++) { uint64_t v = pw[w]; if (v != tw[w]) { ow[w] = v; mk[w] = v ^ tw[w]; tw[w] = v; } } }
  struct msg r = {.t = DIFFB, .pg = (uint64_t)k}; pthread_mutex_lock(&smu); r.seq = sseq++; xfer(sock, &r, sizeof r, 1); xfer(sock, bb, k * E, 1); pthread_mutex_unlock(&smu);
  for (int j = 0; j < k; j++) setwp(reppg[ch[j]], 0);
  nbatch_out++; ndiff_out += k; return k; }
static int sync_once(char *out) { int n = 0;
    for (uint32_t i = 1; i <= nrepl; i++) { if (st[reppg[i]] == 2) continue; uint64_t *pw = (uint64_t *)(base + (uint64_t)reppg[i] * PS), *tw = twin[i], *ow = (uint64_t *)out; uint64_t *mk = (uint64_t *)(out + PS); int any = 0;
      { unsigned char v = 0; if (mincore(pw, PS, &v) || !(v & 1)) { nrepskip++; continue; } }  /* never fault while holding rmu: the fault needs the receiver, which may wait for rmu */
      for (int w = 0; w < PS / 8; w++) { uint64_t v = __atomic_load_n(&pw[w], __ATOMIC_RELAXED); if (v != tw[w]) { if (!any) { memset(mk, 0, BM); any = 1; } ow[w] = v; mk[w] = v ^ tw[w]; tw[w] = v; } }
      if (any) { struct msg r = {.t = DIFF, .has_data = 2, .pg = reppg[i]}; pthread_mutex_lock(&smu); sendm_locked(&r, out, PS + BM); pthread_mutex_unlock(&smu); ndiff_out++; n++; } }
    return n + swbatch(); }
static char *rfbuf;
static void relflush(void) {  /* release: diffs of replicated pages reach the home before this node gives away any page (RECALL/INV ack), so whoever acquires it next sees them */
  if (!nrepl) return; if (!rfbuf) rfbuf = aligned_alloc(PS, PS + BM); pthread_mutex_lock(&rmu); if (sync_once(rfbuf)) nrelflush++; pthread_mutex_unlock(&rmu); }
static void *syncer(void *a) {
  long us = getenv("VEIL_DSM_SYNC_US") ? atol(getenv("VEIL_DSM_SYNC_US")) : 1000; char *out = aligned_alloc(PS, PS + BM);
  for (;;) { usleep(us); pthread_mutex_lock(&rmu); if (sync_once(out)) need_fence = 1; pthread_mutex_unlock(&rmu); } return 0; }
static __thread char *fout;
static void fence(void) {  /* release fence: every diff written before this interrupt is at the home (and fanned out as UPD) before the interrupt is sent */
  if (!fout) fout = aligned_alloc(PS, PS + BM); pthread_mutex_lock(&rmu); int n = sync_once(fout); pthread_mutex_unlock(&rmu);
  if (!n && !need_fence) return; need_fence = 0; pthread_mutex_lock(&fmu); uint64_t g = ++fgen; pthread_mutex_unlock(&fmu);
  struct msg q = {.t = FENCE, .from = myid, .pg = g}; pthread_mutex_lock(&smu); if (sock >= 0) sendm_locked(&q, 0, 0); pthread_mutex_unlock(&smu); nfence++;
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
  pthread_mutex_lock(&fmu); while (fdone < g) if (pthread_cond_timedwait(&fcv, &fmu, &ts)) { nfence_to++; break; } pthread_mutex_unlock(&fmu); }
/* guest TSC sync across nodes: every node's QEMU writes guest IA32_TSC at vCPU reset; replace that value with a function of
   the shared wall clock so all vCPUs (on different hosts/processes) see one TSC: guest_tsc = (CLOCK_REALTIME - epoch) * tsc_khz */
static int (*real_ioctl)(int, unsigned long, ...); static unsigned long ntscfix;
#include <stdarg.h>
int ioctl(int fd, unsigned long req, ...) {
  va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap); if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
  if (req == 0x4008ae89UL && arg && getenv("VEIL_TSC_SYNC") && atoi(getenv("VEIL_TSC_SYNC"))) {  /* KVM_SET_MSRS on a vCPU fd */
    struct ent { uint32_t idx, res; uint64_t data; }; uint32_t n = *(uint32_t *)arg; struct ent *e = (struct ent *)((char *)arg + 8);
    for (uint32_t i = 0; i < n && i < 512; i++) if (e[i].idx == 0x10) {
      if (getenv("VEIL_TSC_KHZ") && atol(getenv("VEIL_TSC_KHZ")) > 0) { long want = atol(getenv("VEIL_TSC_KHZ")); int rc = real_ioctl(fd, 0xaea2UL, want);  /* KVM_SET_TSC_KHZ: same guest TSC rate on every host model */
        if (ntscfix < 8) fprintf(stderr, "DSM tsc-khz set %ld rc=%d\n", want, rc); }
      long khz = real_ioctl(fd, 0xaea3UL, 0);  /* KVM_GET_TSC_KHZ */
      if (khz > 0) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); __int128 ns = (__int128)t.tv_sec * 1000000000 + t.tv_nsec - (__int128)1780000000 * 1000000000;
        uint64_t old = e[i].data; e[i].data = (uint64_t)(ns * khz / 1000000); if (ntscfix++ < 8) fprintf(stderr, "DSM tsc-sync vcpu_fd=%d khz=%ld old=%llu new=%llu\n", fd, khz, (unsigned long long)old, (unsigned long long)e[i].data); } } }
  return real_ioctl(fd, req, arg); }
static int8_t rtr[65536];
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);
ssize_t sendmsg(int fd, const struct msghdr *mh, int fl) {
  if (!real_sendmsg) real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");
  if (base && fd >= 0 && fd < 65536) { if (!rtr[fd]) { struct sockaddr_in a; socklen_t l = sizeof a; rtr[fd] = (!getpeername(fd, (void *)&a, &l) && a.sin_family == AF_INET && ntohs(a.sin_port) >= 40000 && ntohs(a.sin_port) < 40100) ? 1 : 2; }
    if (rtr[fd] == 1 && getenv("VEIL_DSM_FENCE") && atoi(getenv("VEIL_DSM_FENCE"))) fence(); }
  return real_sendmsg(fd, mh, fl); }
static void attach(void *p, size_t len, int fd, const char *tgt) {
  const char *hp = getenv("VEIL_DSM_HOME"); myid = atoi(getenv("VEIL_DSM_ID") ? getenv("VEIL_DSM_ID") : "0");
  if (!hp) { fprintf(stderr, "DSM: VEIL_DSM_HOME missing\n"); return; }
  for (int i = 0; (sock = dial(hp)) < 0; i++) { if (i > 600) { perror("DSM connect"); exit(1); } usleep(500000); }
  uint16_t id16 = myid; xfer(sock, &id16, 2, 1);
  base = p; np = len / PS; memfd = dup(fd); pending = calloc(np, 1); st = calloc(np, 1); repidx = calloc(np, 4); fault_t0 = calloc(np, sizeof(double)); ghost_pg = calloc(maxghost, 4); ghost_data = malloc((size_t)maxghost * PS);
  fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, len);  /* start empty: every page comes from the directory */
  uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK); struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_MISSING_SHMEM | UFFD_FEATURE_WP_HUGETLBFS_SHMEM};
  struct uffdio_register reg = {.range = {(uintptr_t)p, len}, .mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP};
  if (uffd < 0 || ioctl(uffd, UFFDIO_API, &api) || ioctl(uffd, UFFDIO_REGISTER, &reg)) { perror("DSM uffd"); exit(1); }
  if (getenv("VEIL_DSM_PREFILL") && atoi(getenv("VEIL_DSM_PREFILL"))) {  /* every page starts as a local zero S copy (home marks all nodes sharers): no startup fetch storm */
    size_t ch = 2u << 20; char *z = aligned_alloc(PS, ch); memset(z, 0, ch); double t0 = now();
    for (size_t o = 0; o < len; o += ch) { struct uffdio_copy c = {.dst = (uintptr_t)p + o, .src = (uintptr_t)z, .len = len - o < ch ? len - o : ch, .mode = UFFDIO_COPY_MODE_WP}; if (ioctl(uffd, UFFDIO_COPY, &c)) { perror("DSM prefill"); break; } }
    memset(st, 1, np); fprintf(stderr, "DSM node%d prefilled %zu MiB as shared-zero in %.3fs\n", myid, len >> 20, now() - t0); free(z); }
  fence_grants = getenv("VEIL_DSM_FENCE") && atoi(getenv("VEIL_DSM_FENCE")) >= 2;
  pthread_t t1, t2, t3; pthread_create(&t1, 0, receiver, 0); pthread_create(&t2, 0, faulter, 0); pthread_create(&t3, 0, syncer, 0);
  fprintf(stderr, "DSM node%d attached %s len=%zu MiB pages=%zu home=%s standby=%s\n", myid, tgt, len >> 20, np, hp, getenv("VEIL_DSM_HOME2") ? getenv("VEIL_DSM_HOME2") : "-");
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
