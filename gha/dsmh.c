// veil-dsm home v2: directory + backing store for N (<=8) clients of dsm_shim.so.
//  * readahead / write-ahead: a sequential fault stream gets the next K pages in the same batch (RECALLs/INVs to the
//    other holders are sent first, then all acks are collected, then all grants are corked into one TCP burst)
//  * every message carries a per-connection sequence number; gaps are counted (seq_err must stay 0)
//  * replicated pages (page tables / silent pages): word diffs merged here, forwarded as UPD; overlapping diffs from two
//    nodes within the sync window are counted as diff_conflicts
//  * optional synchronous mirror: every page the home learns (writeback, diff) is put to a standby before the home
//    acts on it; the standby takes over when a client reconnects to it (failover), rebuilding the directory from the
//    clients' state dumps (+ their not-yet-mirrored writebacks)
//   dsmh home   PORT NPAGES NC [MIRROR_HOST:MPORT]
//   dsmh mirror PORT NPAGES NC MPORT
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define PS 4096
#define BM 4096  /* per-word bit masks */
#define MAXC 8
#define MAXB 256
enum { RD = 1, WR, ACK, DATA_S, DATA_M, GRANT_M, RECALL, INV, REPL, DATA_REP, DIFF, UPD, INVQ, MPUT, UNREP, FENCE, SWW, SWDATA, SWGRANT, SWDEMOTE, SWSYNC, DIFFB, UPDB };
struct msg { uint8_t t, has_data; uint16_t from; uint32_t seq; uint64_t pg; };
static int xfer(int fd, void *b, size_t n, int wr) { char *p = b; while (n) { ssize_t r = wr ? write(fd, p, n) : read(fd, p, n); if (r <= 0) return -1; p += r; n -= r; } return 0; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static size_t np; static int NC, cs[MAXC], mfd = -1, invq, one = 1; static uint32_t sseq[MAXC], rseq[MAXC];
static char *mem, tmp[PS + BM]; static uint8_t *wrt, *sh, *rep, **lbm, *lsrc; static int8_t *owner; static uint16_t *same; static uint32_t *hot, repmin, repsilent, ramax; static double *ltime, *gt, t_start; static int8_t *gown; static double hotwin = 0.05; static long dly;
static unsigned long nreq, nrecall, ninv, nzero, nchg, nsame, nrep, nderep, ndiff, ndiffw, nconf, seqerr, nmput, nra, nwa, bytes;
static int8_t *swr; static uint16_t *ppc; static uint8_t *pwm; static float *ppt; static int swmin; static unsigned long nsw, nswsw, nswsync;
static uint64_t dq[4096]; static int ndq; static unsigned long nderep;
static int lstart[MAXC][3], lend[MAXC][3], streak[MAXC][3];
struct q { int c; struct msg m; }; static struct q pend[8192]; static int npend;
static int looks_pt(const char *pgp) { const uint64_t *e = (const uint64_t *)pgp; int n = 0;
  for (int i = 0; i < PS / 8; i++) { uint64_t v = e[i]; if (!v) continue; uint64_t fr = v & 0x000ffffffffff000ull;
    if (!(v & 1) && (v & 0x000ff00000000000ull) == 0x000ff00000000000ull && !(v & 0x7ff0000000000000ull)) { n += !!(v & 0x20); continue; }
    if (!(v & 1) || (v & 0x7ff0000000000000ull) || !fr || (fr >= np * PS && fr >= (1ull << 32))) return 0; n += !!(v & 0x20); }
  return n >= 1; }
static void fatal_client(int c) { fprintf(stderr, "DSM home: client %d LOST at t=%.1fs -> guest cannot continue (vCPUs of that node are gone); stopping cleanly\n", c, now() - t_start); exit(3); }
static void sendc(int c, int t, uint64_t pg, int hd, const void *d, size_t dl) { if (cs[c] < 0) return; if (t == DATA_S || t == DATA_M || t == GRANT_M || t == DATA_REP) { gt[pg] = now(); gown[pg] = c; } struct msg m = {.t = t, .has_data = hd, .seq = sseq[c]++, .pg = pg}; if (xfer(cs[c], &m, sizeof m, 1) || (d && xfer(cs[c], (void *)d, dl, 1))) fatal_client(c); if (d) bytes += dl; }
static int recvc(int c, struct msg *m) { if (xfer(cs[c], m, sizeof *m, 0)) return -1; if (m->seq != rseq[c]) seqerr++; rseq[c] = m->seq + 1; return 0; }
static void cork(int c, int on) { setsockopt(cs[c], IPPROTO_TCP, TCP_CORK, &on, sizeof on); }
static void mput(uint64_t pg) { if (mfd < 0) return; struct msg m = {.t = MPUT, .has_data = 1, .pg = pg}; char a; if (xfer(mfd, &m, sizeof m, 1) || xfer(mfd, mem + pg * PS, PS, 1) || xfer(mfd, &a, 1, 0)) { fprintf(stderr, "DSM home: mirror LOST (running unprotected)\n"); close(mfd); mfd = -1; return; } nmput++; }
static void dodiff(int src, uint64_t pg) {  /* tmp = page + bitmap */
  uint64_t *nw = (uint64_t *)tmp, *hm = (uint64_t *)(mem + pg * PS); uint8_t *bm = (uint8_t *)tmp + PS; uint64_t *mk = (uint64_t *)bm; double t = now();
  if (lbm[pg] && lsrc[pg] != src && t - ltime[pg] < 0.003) for (int i = 0; i < PS / 8; i++) if (((uint64_t *)lbm[pg])[i] & mk[i]) { nconf++; break; }
  if (!lbm[pg]) lbm[pg] = malloc(BM); memcpy(lbm[pg], bm, BM); lsrc[pg] = src; ltime[pg] = t;
  for (int w = 0; w < PS / 8; w++) if (mk[w]) { hm[w] = (hm[w] & ~mk[w]) | (nw[w] & mk[w]); ndiffw++; }  /* bit-level 3-way merge: only the bits this node changed */
  wrt[pg] = 1; ndiff++; mput(pg); for (int x = 0; x < NC; x++) if (x != src && cs[x] >= 0 && (!swr || !swr[pg] || (sh[pg] >> x & 1))) sendc(x, UPD, pg, 2, tmp, PS + BM);
  if (rep[pg] == 1 && !looks_pt(mem + pg * PS) && ndq < 4096) { rep[pg] = 2; dq[ndq++] = pg; } }  /* reused as data (freed page table): back to single-writer */
static unsigned long nfen;
/* single-writer replication (VEIL_DSM_SW=MIN): a page that ping-pongs >= MIN times/s between one writer node and reader nodes
   becomes write-update: the writer keeps write access and ships word diffs (every SYNC_US, before any interrupt it sends,
   before it gives away any page); readers keep a read-only copy that their shim swaps atomically on each update */

#define SWB 64
static char *bbuf; static unsigned long nbatch;
/* DIFFB: one consistent cut of all single-writer pages the writer changed (taken with those pages write-blocked); applied
   here, then forwarded to each reader as ONE UPDB holding the pages it has, which its shim swaps in as a group */
static void dodiffb(int src, uint32_t n) { const size_t E = 8 + PS + BM; if (!bbuf) bbuf = malloc(SWB * E); if (n > SWB) { fprintf(stderr, "DIFFB too big %u\n", n); fatal_client(src); }
  if (xfer(cs[src], bbuf, n * E, 0)) fatal_client(src); nbatch++;
  for (uint32_t i = 0; i < n; i++) { char *e = bbuf + i * E; uint64_t pg = *(uint64_t *)e; if (pg >= np) continue; uint64_t *nw = (uint64_t *)(e + 8), *mk = (uint64_t *)(e + 8 + PS), *hm = (uint64_t *)(mem + pg * PS);
    for (int w = 0; w < PS / 8; w++) if (mk[w]) { hm[w] = (hm[w] & ~mk[w]) | (nw[w] & mk[w]); ndiffw++; } wrt[pg] = 1; ndiff++; mput(pg); }
  for (int x = 0; x < NC; x++) { if (x == src || cs[x] < 0) continue; uint32_t k = 0; for (uint32_t i = 0; i < n; i++) { uint64_t pg = *(uint64_t *)(bbuf + i * E); if (pg < np && (sh[pg] >> x & 1)) k++; } if (!k) continue;
    struct msg m = {.t = UPDB, .has_data = 0, .seq = sseq[x]++, .pg = k}; if (xfer(cs[x], &m, sizeof m, 1)) fatal_client(x);
    for (uint32_t i = 0; i < n; i++) { char *e = bbuf + i * E; uint64_t pg = *(uint64_t *)e; if (pg < np && (sh[pg] >> x & 1)) { if (xfer(cs[x], e, E, 1)) fatal_client(x); bytes += E; } } } }
static uint32_t *pcnt; static uint8_t *pmask; static unsigned long pt_req[8][2]; static double prof_s;
static void waitack(int x, uint64_t pg) {
  if (dly && x == 1) usleep(dly);  /* VEIL_DSM_DELAY_US: emulate the cross-runner link to client 1 on one host */
  for (;;) { struct msg r; if (recvc(x, &r)) fatal_client(x);
    if (r.t == ACK && r.pg == pg) { if (r.has_data) { if (xfer(cs[x], tmp, PS, 0)) fatal_client(x); if (memcmp(tmp, mem + pg * PS, PS)) { nchg++; same[pg] = 0; } else { nsame++; if (same[pg] < 65535) same[pg]++; } memcpy(mem + pg * PS, tmp, PS); wrt[pg] = 1; bytes += PS; mput(pg); } return; }
    if (r.t == DIFF) { if (xfer(cs[x], tmp, PS + BM, 0)) fatal_client(x); if (r.pg < np) dodiff(x, r.pg); continue; }
    if (r.t == DIFFB) { dodiffb(x, (uint32_t)r.pg); continue; }
    if (r.has_data && xfer(cs[x], tmp, PS, 0)) fatal_client(x);
    if (npend < 8192) { pend[npend].c = x; pend[npend++].m = r; } } }
static int batchlen(int c, int t, uint64_t pg) {  /* sequential stream detection per client and request type */
  int s = (int)pg >= lstart[c][t] && (int)pg <= lend[c][t] + 1; streak[c][t] = s ? streak[c][t] + 1 : 0;
  int k = 0; if (ramax && streak[c][t] >= 2) { k = 1 << (streak[c][t] < 9 ? streak[c][t] - 1 : 8); if (k > (int)ramax) k = ramax; }
  if (pg + k >= np) k = np - 1 - pg; lstart[c][t] = pg; lend[c][t] = pg + k; return k; }
static void do_rd(int c, uint64_t pg) {
  uint64_t L[MAXB + 1]; int n = 0, k = batchlen(c, 1, pg); L[n++] = pg;
  double tn = now(); for (int i = 1; i <= k; i++) { uint64_t q = pg + i; if (gown[q] != c && tn - gt[q] < hotwin) break; /* another node used it just now: stop speculating */ if (!rep[q] && !(swr && swr[q]) && owner[q] != c && !(sh[q] >> c & 1)) L[n++] = q; }
  int wx[MAXB + 1]; for (int i = 0; i < n; i++) { int o = owner[L[i]]; wx[i] = -1; if (o >= 0 && o != c) { sendc(o, RECALL, L[i], 0, 0, 0); nrecall++; hot[L[i]]++; wx[i] = o; } }
  for (int i = 0; i < n; i++) if (wx[i] >= 0) { waitack(wx[i], L[i]); owner[L[i]] = -1; sh[L[i]] |= 1 << wx[i]; }
  if (n > 1) cork(c, 1);
  for (int i = 0; i < n; i++) { uint64_t p = L[i]; sendc(c, DATA_S, p, wrt[p], wrt[p] ? mem + p * PS : 0, PS); if (!wrt[p]) nzero++; sh[p] |= 1 << c; }
  if (n > 1) { cork(c, 0); nra += n - 1; } }
static void do_wr(int c, uint64_t pg) {
  if (owner[pg] == c) { sendc(c, GRANT_M, pg, 0, 0, 0); return; }
  uint64_t L[MAXB + 1]; int n = 0, k = batchlen(c, 2, pg); L[n++] = pg;
  double tn = now(); for (int i = 1; i <= k; i++) { uint64_t q = pg + i; if (gown[q] != c && tn - gt[q] < hotwin) break; if (!rep[q] && !(swr && swr[q]) && owner[q] != c) L[n++] = q; }
  int wx[MAXB + 1][MAXC]; for (int i = 0; i < n; i++) { uint64_t p = L[i]; for (int x = 0; x < NC; x++) { wx[i][x] = 0; if (x == c) continue;
      if (owner[p] == x) { sendc(x, INV, p, 0, 0, 0); ninv++; hot[p]++; wx[i][x] = 1; }
      else if (sh[p] >> x & 1) { if (invq) { sendc(x, INVQ, p, 0, 0, 0); ninv++; hot[p]++; sh[p] &= ~(1 << x); } else { sendc(x, INV, p, 0, 0, 0); ninv++; hot[p]++; wx[i][x] = 1; } } } }
  for (int i = 0; i < n; i++) for (int x = 0; x < NC; x++) if (wx[i][x]) waitack(x, L[i]);
  if (n > 1) cork(c, 1);
  for (int i = 0; i < n; i++) { uint64_t p = L[i]; if (sh[p] >> c & 1) sendc(c, GRANT_M, p, 0, 0, 0); else { sendc(c, DATA_M, p, wrt[p], wrt[p] ? mem + p * PS : 0, PS); if (!wrt[p]) nzero++; } owner[p] = c; sh[p] = 0; }
  if (n > 1) { cork(c, 0); nwa += n - 1; } }
static void handle(int c, struct msg *m) {
  if (m->t != RD && m->t != WR) return; if (dly && c == 1) usleep(dly); uint64_t pg = m->pg; if (pg >= np) { fprintf(stderr, "bad page %llx\n", (unsigned long long)pg); return; } nreq++;
  if (pcnt) { pcnt[pg]++; pmask[pg] |= (m->t == WR ? 0x10 : 0x01) << (c & 3); pt_req[c & 7][m->t == WR]++; }
  if (swr && swr[pg]) { int w = swr[pg] - 1;
    if (m->t == RD && c != w) { sendc(w, SWSYNC, pg, 0, 0, 0); waitack(w, pg); nswsync++; sendc(c, SWDATA, pg, 1, mem + pg * PS, PS); sh[pg] |= 1 << c; return; }
    if (c == w) { if (m->t == WR) sendc(c, GRANT_M, pg, 0, 0, 0); else sendc(c, SWGRANT, pg, 1, mem + pg * PS, PS); return; }
    sendc(w, SWDEMOTE, pg, 0, 0, 0); waitack(w, pg); int had = sh[pg] >> c & 1; sh[pg] = (sh[pg] | 1 << w) & ~(1 << c); swr[pg] = c + 1; owner[pg] = c; nswsw++;
    if (had) sendc(c, SWGRANT, pg, 0, 0, 0); else sendc(c, SWGRANT, pg, 1, mem + pg * PS, PS); return; }
  if (swr) { float tn = now() - t_start; if (tn - ppt[pg] > 1) { ppt[pg] = tn; ppc[pg] = 0; pwm[pg] = 0; } if (m->t == WR) pwm[pg] |= 1 << c;
    int o = owner[pg]; if ((o >= 0 && o != c) || (m->t == WR && (sh[pg] & ~(1 << c)))) { if (ppc[pg] < 65535) ppc[pg]++; }
    if (m->t == RD && o >= 0 && o != c && ppc[pg] >= swmin && (pwm[pg] & ~(1 << o)) == 0 && !rep[pg] && !looks_pt(mem + pg * PS)) {
      sendc(o, SWW, pg, 0, 0, 0); waitack(o, pg); swr[pg] = o + 1; sh[pg] = 1 << c; nsw++; sendc(c, SWDATA, pg, 1, mem + pg * PS, PS); return; } }
  if (rep[pg]) { sendc(c, DATA_REP, pg, 1, mem + pg * PS, PS); sh[pg] |= 1 << c; return; }
  int others = (owner[pg] >= 0 && owner[pg] != c) || (sh[pg] & ~(1 << c));
  if (repmin && others && same[pg] >= repmin && (looks_pt(mem + pg * PS) || (repsilent && same[pg] >= repsilent))) {
    for (int x = 0; x < NC; x++) if (x != c && (owner[pg] == x || (sh[pg] >> x & 1))) { sendc(x, REPL, pg, 0, 0, 0); waitack(x, pg); sh[pg] |= 1 << x; }  /* the former owner now holds a replica too: it must get UNREP on de-replication */
    rep[pg] = 1; nrep++; owner[pg] = -1; sh[pg] |= 1 << c; sendc(c, DATA_REP, pg, 1, mem + pg * PS, PS); return; }
  if (m->t == RD) do_rd(c, pg); else if (m->t == WR) do_wr(c, pg); }
static void derep(uint64_t pg) {  /* every holder flushes its diff and drops write access, then all copies are invalidated */
  for (int x = 0; x < NC; x++) if (cs[x] >= 0 && (sh[pg] >> x & 1)) sendc(x, UNREP, pg, 0, 0, 0);
  for (int x = 0; x < NC; x++) if (cs[x] >= 0 && (sh[pg] >> x & 1)) waitack(x, pg);
  rep[pg] = 0; same[pg] = 0; for (int x = 0; x < NC; x++) if (cs[x] >= 0 && (sh[pg] >> x & 1)) sendc(x, INVQ, pg, 0, 0, 0); sh[pg] = 0; owner[pg] = -1; mput(pg); nderep++; }
static void stats(void) {
  fprintf(stderr, "DSM home t=%.0fs requests=%lu recalls=%lu invalidations=%lu zero_grants=%lu readahead=%lu writeahead=%lu wb_changed=%lu wb_same=%lu replicated=%lu dereplicated=%lu diffs=%lu diff_words=%lu diff_conflicts=%lu seq_err=%lu mirror_puts=%lu fences=%lu sw=%lu sw_switch=%lu sw_sync=%lu sw_batches=%lu%s data_MiB=%.1f\n",
    now() - t_start, nreq, nrecall, ninv, nzero, nra, nwa, nchg, nsame, nrep, nderep, ndiff, ndiffw, nconf, seqerr, nmput, nfen, nsw, nswsw, nswsync, nbatch, mfd >= 0 ? "" : "(no mirror)", bytes / 1048576.0);
  fprintf(stderr, "DSM hot:"); for (int k = 0; k < 6; k++) { size_t bi = 0; for (size_t i = 0; i < np; i++) if (hot[i] > hot[bi]) bi = i; if (!hot[bi]) break; fprintf(stderr, " %zx:%u%s/s%u", bi, hot[bi], rep[bi] ? "R" : "", same[bi]); hot[bi] = 0; } fprintf(stderr, "\n"); memset(hot, 0, np * 4); }
static int listen_on(int port) { int ls = socket(AF_INET, SOCK_STREAM, 0); setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (bind(ls, (void *)&a, sizeof a) || listen(ls, 16)) { perror("bind"); exit(1); } return ls; }
static void accept_client(int ls) {  /* handshake: u16 id (bit15 = resume after failover: state dump follows) */
  int s = accept(ls, 0, 0); setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); uint16_t id; if (xfer(s, &id, 2, 0)) { close(s); return; } int c = id & 7;
  if (cs[c] >= 0) close(cs[c]); cs[c] = s; sseq[c] = rseq[c] = 0; for (int t = 0; t < 3; t++) lstart[c][t] = lend[c][t] = -10, streak[c][t] = 0;
  if (id & 0x8000) { uint32_t n, ng, nr; double t0 = now(); xfer(s, &n, 4, 0); struct { uint32_t pg; uint8_t st; } __attribute__((packed)) e;
    for (uint32_t i = 0; i < n; i++) { xfer(s, &e, 5, 0); if (e.pg >= np) continue; if (e.st == 1) sh[e.pg] |= 1 << c; else if (e.st == 2) { owner[e.pg] = c; sh[e.pg] = 0; } else if (e.st == 3) { rep[e.pg] = 1; sh[e.pg] |= 1 << c; } }
    xfer(s, &ng, 4, 0); for (uint32_t i = 0; i < ng; i++) { uint32_t pg; xfer(s, &pg, 4, 0); xfer(s, tmp, PS, 0); if (pg < np && owner[pg] < 0) { memcpy(mem + pg * PS, tmp, PS); wrt[pg] = 1; } }
    xfer(s, &nr, 4, 0); for (uint32_t i = 0; i < nr; i++) { uint32_t pg; xfer(s, &pg, 4, 0); xfer(s, tmp, PS, 0); if (pg < np) { memcpy(mem + pg * PS, tmp, PS); wrt[pg] = 1; } }
    fprintf(stderr, "DSM home: client %d RESUMED t=%.1fs states=%u unmirrored_writebacks=%u replicated=%u (%.3fs)\n", c, now() - t_start, n, ng, nr, now() - t0); }
  else fprintf(stderr, "DSM home client %d connected\n", c); }
static void init(size_t n, int nc) { np = n; NC = nc; mem = calloc(np, PS); wrt = calloc(np, 1); sh = calloc(np, 1); rep = calloc(np, 1); owner = malloc(np); memset(owner, -1, np); same = calloc(np, 2); hot = calloc(np, 4); lbm = calloc(np, sizeof *lbm); lsrc = calloc(np, 1); ltime = calloc(np, sizeof(double)); gt = calloc(np, sizeof(double)); gown = malloc(np); memset(gown, -1, np); if (getenv("VEIL_DSM_HOTWIN_MS")) hotwin = atof(getenv("VEIL_DSM_HOTWIN_MS")) / 1e3;
  for (int i = 0; i < MAXC; i++) cs[i] = -1; t_start = now(); invq = getenv("VEIL_DSM_INVQ") && atoi(getenv("VEIL_DSM_INVQ"));
  repmin = getenv("VEIL_DSM_REPMIN") ? atoi(getenv("VEIL_DSM_REPMIN")) : 0; repsilent = getenv("VEIL_DSM_REPSILENT") ? atoi(getenv("VEIL_DSM_REPSILENT")) : 0; dly = getenv("VEIL_DSM_DELAY_US") ? atol(getenv("VEIL_DSM_DELAY_US")) : 0; ramax = getenv("VEIL_DSM_RA") ? atoi(getenv("VEIL_DSM_RA")) : 0; if (ramax > MAXB) ramax = MAXB;
  swmin = getenv("VEIL_DSM_SW") ? atoi(getenv("VEIL_DSM_SW")) : 0; if (swmin > 0) { swr = calloc(np, 1); ppc = calloc(np, 2); pwm = calloc(np, 1); ppt = calloc(np, sizeof(float)); } }
static void prof(void) {  /* VEIL_DSM_PROF_S: per-interval request counts per client and the hottest pages (mask: low nibble = readers, high = writers, bit = client) */
  fprintf(stderr, "DSM prof t=%.1f", now() - t_start); for (int c = 0; c < NC; c++) fprintf(stderr, " c%d=rd%lu/wr%lu", c, pt_req[c][0], pt_req[c][1]); memset(pt_req, 0, sizeof pt_req); fprintf(stderr, " top:");
  for (int k = 0; k < 12; k++) { size_t bi = 0; for (size_t i = 0; i < np; i++) if (pcnt[i] > pcnt[bi]) bi = i; if (!pcnt[bi]) break; fprintf(stderr, " %zx:%u:%02x%s", bi, pcnt[bi], pmask[bi], rep[bi] ? "R" : swr && swr[bi] ? "S" : ""); pcnt[bi] = 0; }
  fprintf(stderr, "\n"); memset(pcnt, 0, np * 4); memset(pmask, 0, np); }
static int serve(int ls) {
  double tl = now(), tp = now(); int rr = 0; prof_s = getenv("VEIL_DSM_PROF_S") ? atof(getenv("VEIL_DSM_PROF_S")) : 0; if (prof_s > 0) { pcnt = calloc(np, 4); pmask = calloc(np, 1); }
  for (;;) {
    if (pcnt && now() - tp > prof_s) { tp = now(); prof(); }
    if (now() - tl > 10) { tl = now(); stats(); }
    struct q cur;
    if (npend) { cur = pend[0]; memmove(pend, pend + 1, --npend * sizeof *pend); }
    else { struct pollfd p[MAXC + 1]; int idx[MAXC + 1], k = 0, live = 0; for (int i = 0; i < NC; i++) { int c = (rr + i) % NC; if (cs[c] >= 0) { p[k].fd = cs[c]; p[k].events = POLLIN; idx[k++] = c; live++; } } p[k].fd = ls; p[k].events = POLLIN; idx[k++] = -1;
      if (!live && nreq) break; if (poll(p, k, 1000) <= 0) continue; int got = -1; for (int i = 0; i < k; i++) if (p[i].revents) { got = i; break; } if (got < 0) continue;
      if (idx[got] < 0) { accept_client(ls); continue; } int c = idx[got]; rr = c + 1; cur.c = c;
      if (recvc(c, &cur.m)) { fprintf(stderr, "DSM home: client %d closed\n", c); close(cs[c]); cs[c] = -1; int any = 0; for (int x = 0; x < NC; x++) any |= cs[x] >= 0; if (!any) break; continue; } }
    if (cur.m.t == FENCE) { uint64_t pr = (1ULL << 62) | (cur.m.pg & 0xffffffffULL) | ((uint64_t)cur.c << 40); nfen++;  /* every other node has applied all earlier UPDs before the fencing node may send its interrupt */
      for (int x = 0; x < NC; x++) if (x != cur.c && cs[x] >= 0) sendc(x, FENCE, pr, 0, 0, 0);
      for (int x = 0; x < NC; x++) if (x != cur.c && cs[x] >= 0) waitack(x, pr);
      sendc(cur.c, FENCE, cur.m.pg, 0, 0, 0); continue; }
    if (cur.m.t == DIFFB) { dodiffb(cur.c, (uint32_t)cur.m.pg); continue; }
    if (cur.m.t == DIFF) { if (xfer(cs[cur.c], tmp, PS + BM, 0)) fatal_client(cur.c); if (cur.m.pg < np) dodiff(cur.c, cur.m.pg); while (ndq) derep(dq[--ndq]); continue; }
    handle(cur.c, &cur.m); while (ndq) derep(dq[--ndq]); }
  stats(); fprintf(stderr, "DSM home done\n"); return 0; }
int main(int argc, char **v) {
  if (argc >= 5 && !strcmp(v[1], "home")) { init(atol(v[3]), atoi(v[4])); if (getenv("VEIL_DSM_PREFILL") && atoi(getenv("VEIL_DSM_PREFILL"))) memset(sh, (1 << NC) - 1, np);
    if (argc >= 6) { char h[64]; int port; sscanf(v[5], "%63[^:]:%d", h, &port); struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)}; inet_pton(AF_INET, h, &a.sin_addr);
      for (int i = 0; i < 600; i++) { mfd = socket(AF_INET, SOCK_STREAM, 0); setsockopt(mfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); if (!connect(mfd, (void *)&a, sizeof a)) break; close(mfd); mfd = -1; usleep(200000); }
      fprintf(stderr, "DSM home: mirror %s %s\n", v[5], mfd >= 0 ? "connected (synchronous page mirroring on)" : "UNREACHABLE"); }
    int ls = listen_on(atoi(v[2])); fprintf(stderr, "DSM home listening %s pages=%zu clients=%d ra=%u invq=%d\n", v[2], np, NC, ramax, invq); for (int i = 0; i < NC; i++) accept_client(ls); return serve(ls); }
  if (argc >= 6 && !strcmp(v[1], "mirror")) { init(atol(v[3]), atoi(v[4])); int ms = listen_on(atoi(v[5])), ls = listen_on(atoi(v[2]));
    fprintf(stderr, "DSM mirror: standby on %s, takeover port %s\n", v[5], v[2]); int pf = -1; unsigned long puts = 0; double lastput = 0;
    for (;;) { struct pollfd p[3] = {{ms, POLLIN, 0}, {ls, POLLIN, 0}, {pf, POLLIN, 0}}; if (poll(p, pf >= 0 ? 3 : 2, 10000) <= 0) { if (pf >= 0) fprintf(stderr, "DSM mirror: %lu puts\n", puts); continue; }
      if (p[0].revents) { pf = accept(ms, 0, 0); setsockopt(pf, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); fprintf(stderr, "DSM mirror: primary attached\n"); continue; }
      if (p[1].revents) { if (pf >= 0 && now() - lastput < 3) { int s = accept(ls, 0, 0); close(s); fprintf(stderr, "DSM mirror: refused takeover (primary alive)\n"); continue; }
        fprintf(stderr, "DSM mirror: client reconnecting -> TAKEOVER at t=%.1fs after %lu puts\n", now() - t_start, puts); if (pf >= 0) close(pf); mfd = -1; for (int i = 0; i < NC; i++) accept_client(ls); fprintf(stderr, "DSM mirror: all %d clients resumed, serving as home\n", NC); return serve(ls); }
      if (pf >= 0 && p[2].revents) { struct msg m; char a = 1; if (xfer(pf, &m, sizeof m, 0) || xfer(pf, tmp, PS, 0)) { fprintf(stderr, "DSM mirror: primary link lost t=%.1fs (%lu puts); waiting for clients\n", now() - t_start, puts); close(pf); pf = -1; continue; }
        if (m.pg < np) { memcpy(mem + m.pg * PS, tmp, PS); wrt[m.pg] = 1; } puts++; lastput = now(); xfer(pf, &a, 1, 1); } } }
  fprintf(stderr, "usage: dsmh home PORT NPAGES NC [MHOST:MPORT] | dsmh mirror PORT NPAGES NC MPORT\n"); return 2; }
