// veil-llm: a small Llama-architecture inference engine for measuring how LLM
// inference scales when the weights are sharded across machines.
//
// Llama block: RMSNorm, GQA attention with RoPE and an fp16 KV cache, SwiGLU MLP,
// int8 weights with a per-row scale, activations quantized per 32-block (the
// same arithmetic shape as llama.cpp Q8_0). Weights are synthetic but generated
// deterministically per (tensor, layer, row), so every node builds exactly its own
// shard locally (nothing to download) and every layout computes the same model.
// Speed does not depend on weight values; correctness across layouts is checked
// by comparing generated-token checksums.
//
// Layouts:
//   pp: pipeline. node r holds a contiguous block of layers (byte balanced, the
//       LM head counts as a block). Hidden states stream node to node directly,
//       the last node sends sampled tokens back to the driver. Several
//       micro-batches can be in flight so all stages work at once.
//   tp: tensor. every node holds 1/N of every layer (heads, FFN columns, vocab
//       rows); two all-reduces per layer over a full mesh.
//
// usage:
//   vllm node  <model> <pp|tp> <N> <rank> <seqs> <ctx> <port_base> <proxy_base>
//   vllm drive <model> <pp|tp> <N> <seqs> <microbatches> <prompt> <gen> <port_base> <proxy_base>
//   vllm gen   <model> <file>                 (writes full weights for mmap mode)
//   vllm single <model> <seqs> <prompt> <gen> [file]   (one process, no network)
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <setjmp.h>
#include <signal.h>

#define MAXB 256
#define MAXN 64

typedef struct { const char *name; int d, f, L, H, Hkv, V; } cfg_t;
static cfg_t models[] = {
    {"tiny", 256, 512, 4, 8, 8, 1024},
    {"s1", 2048, 5632, 22, 32, 8, 32000},     // 1.1B class (TinyLlama shape, 8 KV heads)
    {"m3", 3072, 8192, 28, 24, 8, 32000},     // 3B class
    {"m7", 4096, 11008, 32, 32, 8, 32000},    // 7B class (Llama-2-7B shape, GQA 8)
    {"l13", 5120, 13824, 40, 40, 8, 32000},   // 13B class
    {"l34", 8192, 22016, 48, 64, 8, 32000},   // 34B class
    {"l70", 8192, 28672, 80, 64, 8, 128256},  // 70B class (Llama-3-70B shape)
};
static cfg_t C;
static int hd, kvd;

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

// ---------------- weights ----------------
typedef struct { int8_t *w; float *s; int rows, cols; } mat_t;
enum { WQ, WK, WV, WO, WG, WU, WD, EMB, HEAD, NT };

static uint64_t mix(uint64_t x) {
  x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
  return x;
}
static void gen_row(int8_t *dst, int cols, int tid, int layer, int row) {
  uint64_t s = mix(((uint64_t)tid << 56) ^ ((uint64_t)layer << 40) ^ (uint64_t)row ^ 0x9e3779b97f4a7c15ULL) | 1;
  for (int i = 0; i < cols; i += 8) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    uint64_t v = (s & 0x7f7f7f7f7f7f7f7fULL);
    memcpy(dst + i, &v, 8);
  }
  for (int i = 0; i < cols; i++) dst[i] = (int8_t)(dst[i] - 64);  // [-64,63]
}
static float row_scale(int cols) { return 1.0f / (37.0f * sqrtf((float)cols)); }

static size_t bytes_alloc;
// slice rows [r0,r1) x cols [c0,c1) of a rows_total x cols_total synthetic tensor
static mat_t mk(int tid, int layer, int cols_total, int r0, int r1, int c0, int c1) {
  mat_t m = {.rows = r1 - r0, .cols = c1 - c0};
  size_t n = (size_t)m.rows * m.cols;
  size_t al = (n + (2 << 20) - 1) / (2 << 20) * (2 << 20);
  m.w = aligned_alloc(2 << 20, al);
  madvise(m.w, al, MADV_HUGEPAGE);
  m.s = malloc(sizeof(float) * m.rows);
  int8_t *tmp = (c1 - c0 != cols_total) ? malloc(cols_total) : NULL;
#pragma omp parallel for schedule(static)
  for (int r = r0; r < r1; r++) {
    int8_t *t = tmp ? NULL : m.w + (size_t)(r - r0) * m.cols;
    int8_t loc[cols_total > 0 && tmp ? cols_total : 1];
    if (tmp) { gen_row(loc, cols_total, tid, layer, r); memcpy(m.w + (size_t)(r - r0) * m.cols, loc + c0, m.cols); }
    else gen_row(t, cols_total, tid, layer, r);
    m.s[r - r0] = row_scale(cols_total);
  }
  free(tmp);
  bytes_alloc += n + 4 * m.rows;
  return m;
}
// mmap mode: tensors laid out back to back in a file
static uint8_t *fmap; static size_t fcur;
static mat_t mk_file(int rows, int cols) {
  mat_t m = {.rows = rows, .cols = cols};
  m.w = (int8_t *)(fmap + fcur); fcur += (size_t)rows * cols;
  m.s = (float *)(fmap + fcur); fcur += 4 * (size_t)rows;
  return m;
}

// ---------------- kernels ----------------
// Activations are quantized per token in blocks of QB=128 to int8 with a float
// scale (Q8_K-like). Dot products use the sign trick (|w| as u8, x with w's sign),
// as llama.cpp does: AVX-512 VNNI vpdpbusd, or AVX2 maddubs on CPUs without it.
#define QB 128
static int8_t *qbuf; static float *qsbuf;
static void quant(const float *x, int n, int8_t *q, float *s) {
  for (int b = 0; b < n / QB; b++) {
    float m = 0;
    for (int i = 0; i < QB; i++) { float a = fabsf(x[b * QB + i]); if (a > m) m = a; }
    float inv = m > 0 ? 127.f / m : 0;
    s[b] = m / 127.f;
    for (int i = 0; i < QB; i++) q[b * QB + i] = (int8_t)lrintf(x[b * QB + i] * inv);
  }
}
#ifndef __AVX512F__
static inline float hsum8(__m256 v) {
  __m128 a = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
  a = _mm_add_ps(a, _mm_movehl_ps(a, a)); a = _mm_add_ss(a, _mm_movehdup_ps(a));
  return _mm_cvtss_f32(a);
}
#endif
// out[b*ldo + r] = W[r,:] . x[b,:]   for B tokens
static void gemm(const mat_t *W, const float *x, int ldx, int B, float *out, int ldo) {
  int K = W->cols, nb = K / QB;
  if (K % QB) { fprintf(stderr, "K=%d not a multiple of %d\n", K, QB); exit(1); }
  for (int b = 0; b < B; b++) quant(x + (size_t)b * ldx, K, qbuf + (size_t)b * K, qsbuf + (size_t)b * nb);
#pragma omp parallel for schedule(static)
  for (int r = 0; r < W->rows; r++) {
    const int8_t *w = W->w + (size_t)r * K;
    for (int b0 = 0; b0 < B; b0 += 8) {
      int bn = B - b0 < 8 ? B - b0 : 8;
#ifdef __AVX512F__
      __m512 acc[8];
      for (int j = 0; j < 8; j++) acc[j] = _mm512_setzero_ps();
      for (int k = 0; k < nb; k++) {
        _mm_prefetch((const char *)(w + k * QB + 768), _MM_HINT_T0);
        _mm_prefetch((const char *)(w + k * QB + 832), _MM_HINT_T0);
        __m512i w0 = _mm512_loadu_si512(w + k * QB), w1 = _mm512_loadu_si512(w + k * QB + 64);
        __m512i a0 = _mm512_abs_epi8(w0), a1 = _mm512_abs_epi8(w1);
        __mmask64 m0 = _mm512_movepi8_mask(w0), m1 = _mm512_movepi8_mask(w1);
        for (int j = 0; j < bn; j++) {
          const int8_t *q = qbuf + (size_t)(b0 + j) * K + k * QB;
          __m512i x0 = _mm512_loadu_si512(q), x1 = _mm512_loadu_si512(q + 64);
          x0 = _mm512_mask_sub_epi8(x0, m0, _mm512_setzero_si512(), x0);
          x1 = _mm512_mask_sub_epi8(x1, m1, _mm512_setzero_si512(), x1);
          __m512i ia = _mm512_dpbusd_epi32(_mm512_setzero_si512(), a0, x0);
          ia = _mm512_dpbusd_epi32(ia, a1, x1);
          acc[j] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(ia), _mm512_set1_ps(qsbuf[(size_t)(b0 + j) * nb + k]), acc[j]);
        }
      }
      for (int j = 0; j < bn; j++) out[(size_t)(b0 + j) * ldo + r] = _mm512_reduce_add_ps(acc[j]) * W->s[r];
#else
      __m256 acc[8];
      const __m256i ones = _mm256_set1_epi16(1);
      for (int j = 0; j < 8; j++) acc[j] = _mm256_setzero_ps();
      for (int k = 0; k < nb; k++) {
        __m256i wv[4], av[4];
        for (int c = 0; c < 4; c++) { wv[c] = _mm256_loadu_si256((const __m256i *)(w + k * QB + 32 * c)); av[c] = _mm256_sign_epi8(wv[c], wv[c]); }
        for (int j = 0; j < bn; j++) {
          const int8_t *q = qbuf + (size_t)(b0 + j) * K + k * QB;
          __m256i ia = _mm256_setzero_si256();
          for (int c = 0; c < 4; c++) {
            __m256i xs = _mm256_sign_epi8(_mm256_loadu_si256((const __m256i *)(q + 32 * c)), wv[c]);
            ia = _mm256_add_epi32(ia, _mm256_madd_epi16(_mm256_maddubs_epi16(av[c], xs), ones));
          }
          acc[j] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(ia), _mm256_set1_ps(qsbuf[(size_t)(b0 + j) * nb + k]), acc[j]);
        }
      }
      for (int j = 0; j < bn; j++) out[(size_t)(b0 + j) * ldo + r] = hsum8(acc[j]) * W->s[r];
#endif
    }
  }
}
static void rmsnorm(const float *x, float *o, int n) {
  double ss = 0;
  for (int i = 0; i < n; i++) ss += (double)x[i] * x[i];
  float r = 1.0f / sqrtf((float)(ss / n) + 1e-5f);
  for (int i = 0; i < n; i++) o[i] = x[i] * r;
}
static float *rope_c, *rope_s; static int CTX;
static void rope(float *v, int pos) {
  for (int i = 0; i < hd / 2; i++) {
    float c = rope_c[pos * (hd / 2) + i], s = rope_s[pos * (hd / 2) + i];
    float a = v[2 * i], b = v[2 * i + 1];
    v[2 * i] = a * c - b * s; v[2 * i + 1] = a * s + b * c;
  }
}
static inline float dot_h(const float *q, const uint16_t *k) {
#ifdef __AVX512F__
  __m512 acc = _mm512_setzero_ps();
  for (int i = 0; i < hd; i += 16)
    acc = _mm512_fmadd_ps(_mm512_loadu_ps(q + i), _mm512_cvtph_ps(_mm256_loadu_si256((const __m256i *)(k + i))), acc);
  return _mm512_reduce_add_ps(acc);
#else
  __m256 acc = _mm256_setzero_ps();
  for (int i = 0; i < hd; i += 8)
    acc = _mm256_fmadd_ps(_mm256_loadu_ps(q + i), _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(k + i))), acc);
  return hsum8(acc);
#endif
}
static inline void axpy_h(float *o, float w, const uint16_t *v) {
#ifdef __AVX512F__
  for (int i = 0; i < hd; i += 16)
    _mm512_storeu_ps(o + i, _mm512_fmadd_ps(_mm512_set1_ps(w), _mm512_cvtph_ps(_mm256_loadu_si256((const __m256i *)(v + i))), _mm512_loadu_ps(o + i)));
#else
  for (int i = 0; i < hd; i += 8)
    _mm256_storeu_ps(o + i, _mm256_fmadd_ps(_mm256_set1_ps(w), _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(v + i))), _mm256_loadu_ps(o + i)));
#endif
}
static inline uint16_t f2h(float f) { return _cvtss_sh(f, 0); }

// ---------------- shard ----------------
typedef struct {
  mat_t m[7];
  uint16_t *kc, *vc;  // [seq][ctx][kvl*hd]
} layer_t;
static struct {
  int l0, l1, has_emb, has_head;  // pp
  int hl, kvl, fl, v0, v1;        // local heads, kv heads, ffn cols, vocab rows
  layer_t *ly; mat_t emb, head; int nseq;
} S;

static void build(const char *mode, int N, int rank, int nseq, const char *wfile) {
  S.nseq = nseq;
  int tp = mode[0] == 't';
  S.hl = C.H, S.kvl = C.Hkv, S.fl = C.f, S.v0 = 0, S.v1 = C.V;
  S.l0 = 0, S.l1 = C.L, S.has_emb = 1, S.has_head = 1;
  if (tp) {
    S.hl = C.H / N; S.kvl = C.Hkv / N;
    S.v0 = (int)((long)C.V * rank / N); S.v1 = (int)((long)C.V * (rank + 1) / N);
  } else if (N > 1) {
    // byte-balanced contiguous split of L layer units + 1 head unit
    double lb = 2.0 * C.d * C.d + 2.0 * kvd * C.d + 3.0 * C.d * C.f, hb = (double)C.V * C.d;
    double tot = lb * C.L + hb, per = tot / N, acc = 0;
    int start[MAXN + 1], r = 0; start[0] = 0;
    for (int l = 0; l < C.L; l++) {
      if (acc + lb / 2 > per * (r + 1) && r < N - 1) start[++r] = l;
      acc += lb;
    }
    while (r < N - 1) start[++r] = C.L;  // degenerate tiny models
    start[N] = C.L;
    S.l0 = start[rank]; S.l1 = start[rank + 1];
    S.has_emb = rank == 0; S.has_head = rank == N - 1;
  }
  if (wfile) {
    int fd = open(wfile, O_RDONLY);
    struct stat st; fstat(fd, &st);
    fmap = mmap(0, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (fmap == MAP_FAILED) { perror("mmap"); exit(1); }
  }
  int qo = S.hl * hd, kvo = S.kvl * hd;
  int hq0 = tp ? rank * S.hl : 0, hk0 = tp ? rank * S.kvl : 0, f0 = 0;
  if (tp) {  // FFN columns split on QB boundaries (may be uneven by one block)
    int nbk = C.f / QB; f0 = nbk * rank / N * QB; S.fl = nbk * (rank + 1) / N * QB - f0;
  }
  S.ly = calloc(C.L, sizeof(layer_t));
  for (int l = S.l0; l < S.l1; l++) {
    layer_t *y = &S.ly[l];
    if (wfile) {
      y->m[WQ] = mk_file(C.d, C.d); y->m[WK] = mk_file(kvd, C.d); y->m[WV] = mk_file(kvd, C.d);
      y->m[WO] = mk_file(C.d, C.d); y->m[WG] = mk_file(C.f, C.d); y->m[WU] = mk_file(C.f, C.d);
      y->m[WD] = mk_file(C.d, C.f);
    } else {
      y->m[WQ] = mk(WQ, l, C.d, hq0 * hd, hq0 * hd + qo, 0, C.d);
      y->m[WK] = mk(WK, l, C.d, hk0 * hd, hk0 * hd + kvo, 0, C.d);
      y->m[WV] = mk(WV, l, C.d, hk0 * hd, hk0 * hd + kvo, 0, C.d);
      y->m[WO] = mk(WO, l, C.d, 0, C.d, hq0 * hd, hq0 * hd + qo);
      y->m[WG] = mk(WG, l, C.d, f0, f0 + S.fl, 0, C.d);
      y->m[WU] = mk(WU, l, C.d, f0, f0 + S.fl, 0, C.d);
      y->m[WD] = mk(WD, l, C.f, 0, C.d, f0, f0 + S.fl);
    }
    size_t kvn = (size_t)nseq * CTX * kvo;
    y->kc = calloc(kvn, 2); y->vc = calloc(kvn, 2);
    bytes_alloc += 4 * kvn;
  }
  if (S.has_emb) S.emb = wfile ? mk_file(C.V, C.d) : mk(EMB, 0, C.d, 0, C.V, 0, C.d);
  if (S.has_head) {
    if (wfile) { if (!S.has_emb) mk_file(C.V, C.d); S.head = mk_file(C.V, C.d); }
    else S.head = mk(HEAD, 0, C.d, S.v0, S.v1, 0, C.d);
  }
}

// ---------------- networking ----------------
// Failover (pp, VLLM_FAILOVER=1): a broken link jumps back to the connection phase
// instead of exiting. Survivors keep their shard in memory and reconnect; the driver
// replays each sequence's token history to rebuild the KV caches it lost.
static jmp_buf fail_jb; static int failover;
static void link_lost(const char *what) {
  if (failover) longjmp(fail_jb, 1);
  if (what) perror(what);
  exit(what ? 1 : 0);
}
static void wr(int fd, const void *p, size_t n) {
  while (n) { ssize_t w = write(fd, p, n); if (w <= 0) { if (errno == EINTR) continue; link_lost("write"); } p = (char *)p + w; n -= w; }
}
static void rd(int fd, void *p, size_t n) {
  while (n) { ssize_t r = read(fd, p, n); if (r <= 0) { if (r < 0 && errno == EINTR) continue; link_lost(NULL); } p = (char *)p + r; n -= r; }
}
static int listen_on(int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
  a.sin_addr.s_addr = htonl(getenv("VLLM_HOSTS") ? INADDR_ANY : INADDR_LOOPBACK);
  if (bind(s, (void *)&a, sizeof a) || listen(s, 64)) { perror("bind"); exit(1); }
  return s;
}
static void fast(int s) {
  int one = 1, big = 4 << 20;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
}
static int acc1(int ls) { int s = accept(ls, 0, 0); fast(s); return s; }
// Multi-machine mode: VLLM_HOSTS="ip0,ip1,..." gives each rank's address; links then go
// straight to hosts[dst]:port_base+dst (the driver lives on rank 0's host).
static char *hosts[MAXN]; static int nhosts;
static void parse_hosts(void) {
  char *e = getenv("VLLM_HOSTS");
  if (!e || !*e) return;
  e = strdup(e);
  for (char *t = strtok(e, ","); t && nhosts < MAXN; t = strtok(NULL, ",")) hosts[nhosts++] = t;
}
static int dial_host(const char *host, int port) {
  int tries = getenv("VLLM_DIAL_S") ? atoi(getenv("VLLM_DIAL_S")) * 100 : 3000;
  for (int i = 0; i < tries; i++) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (host) inet_pton(AF_INET, host, &a.sin_addr); else a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (void *)&a, sizeof a) == 0) { fast(s); return s; }
    close(s); usleep(10000);
  }
  fprintf(stderr, "dial %s:%d failed\n", host ? host : "127.0.0.1", port); exit(1);
}
static int dial(int port) { return dial_host(NULL, port); }
// Port plan. Node r listens on base+r. Driver listens on base+99.
// Proxy port for a link src->dst is proxy_base + src*100 + dst (dst 99 = driver).
// With proxy_base == 0 links go direct (no emulation).
static int PB, XB;
static int link_port(int src, int dst) { return XB ? XB + src * 100 + dst : PB + (dst == 99 ? 99 : dst); }
static int dial_link(int src, int dst) {
  if (nhosts) return dial_host(hosts[dst == 99 ? 0 : dst], PB + dst);
  return dial(link_port(src, dst));
}
static void say_hello(int fd, int rank) { int32_t r = rank; wr(fd, &r, 4); }
static int hear_hello(int fd) { int32_t r; rd(fd, &r, 4); return r; }

typedef struct { int32_t B, quit, mb, pad; int32_t seq[MAXB], pos[MAXB], want[MAXB]; } hdr_t;
// wire: 16-byte fixed part, then seq/pos/want for the B tokens only, then the payload,
// all in one write
static char *wbuf; static size_t wcap;
static void send_msg(int fd, hdr_t *h, const void *p1, size_t n1, const void *p2, size_t n2) {
  int B = h->quit ? 0 : h->B;
  size_t n = 16 + 12 * (size_t)B + n1 + n2;
  if (n > wcap) { wcap = n * 2; wbuf = realloc(wbuf, wcap); }
  char *q = wbuf;
  memcpy(q, h, 16); q += 16;
  memcpy(q, h->seq, 4 * B); q += 4 * B;
  memcpy(q, h->pos, 4 * B); q += 4 * B;
  memcpy(q, h->want, 4 * B); q += 4 * B;
  if (n1) { memcpy(q, p1, n1); q += n1; }
  if (n2) { memcpy(q, p2, n2); q += n2; }
  wr(fd, wbuf, n);
}
static void recv_hdr(int fd, hdr_t *h) {
  rd(fd, h, 16);
  if (h->quit) return;
  rd(fd, h->seq, 4 * h->B); rd(fd, h->pos, 4 * h->B); rd(fd, h->want, 4 * h->B);
}

// ---------------- forward ----------------
static float *X, *XN, *Q, *Kb, *Vb, *A, *T1, *T2, *T3, *LG;
static void alloc_bufs(void) {
  size_t m = (size_t)MAXB * (C.d > C.f ? C.d : C.f);
  X = malloc(4 * m); XN = malloc(4 * m); Q = malloc(4 * m); Kb = malloc(4 * m); Vb = malloc(4 * m);
  A = malloc(4 * m); T1 = malloc(4 * m); T2 = malloc(4 * m); T3 = malloc(4 * m);
  qbuf = aligned_alloc(64, m + 64); qsbuf = malloc(4 * m / QB + 64);
  CTX = CTX ? CTX : 512;
  rope_c = malloc(4 * (size_t)CTX * hd / 2); rope_s = malloc(4 * (size_t)CTX * hd / 2);
  for (int p = 0; p < CTX; p++)
    for (int i = 0; i < hd / 2; i++) {
      double th = p / pow(10000.0, 2.0 * i / hd);
      rope_c[p * hd / 2 + i] = cos(th); rope_s[p * hd / 2 + i] = sin(th);
    }
}
static int ND, RANK, *peer;  // tp mesh
static void allreduce(float *part, int B) {
  size_t n = (size_t)B * C.d;
  if (ND == 1) return;
  for (int r = 0; r < ND; r++) if (r != RANK) wr(peer[r], part, 4 * n);
  // sum in rank order so every rank gets bit-identical results
  memcpy(T3, part, 4 * n);
  float *acc = A; memset(acc, 0, 4 * n);
  for (int r = 0; r < ND; r++) {
    float *src = T3;
    if (r != RANK) { rd(peer[r], T2, 4 * n); src = T2; }
    for (size_t i = 0; i < n; i++) acc[i] += src[i];
  }
  memcpy(part, acc, 4 * n);
}
static void attention(layer_t *y, hdr_t *h, int B) {
  int qo = S.hl * hd, kvo = S.kvl * hd, grp = S.hl / S.kvl;
  for (int b = 0; b < B; b++) {
    int sq = h->seq[b], p = h->pos[b];
    for (int g = 0; g < S.kvl; g++) rope(Kb + (size_t)b * kvo + g * hd, p);
    for (int i = 0; i < kvo; i++) {
      y->kc[((size_t)sq * CTX + p) * kvo + i] = f2h(Kb[(size_t)b * kvo + i]);
      y->vc[((size_t)sq * CTX + p) * kvo + i] = f2h(Vb[(size_t)b * kvo + i]);
    }
  }
#pragma omp parallel for collapse(2) schedule(dynamic)
  for (int b = 0; b < B; b++)
    for (int hh = 0; hh < S.hl; hh++) {
      int sq = h->seq[b], p = h->pos[b], g = hh / grp;
      float *q = Q + (size_t)b * qo + hh * hd;
      rope(q, p);
      float sc[CTX], mx = -1e30f, sum = 0, isd = 1.0f / sqrtf((float)hd);
      for (int j = 0; j <= p; j++) {
        sc[j] = dot_h(q, y->kc + ((size_t)sq * CTX + j) * kvo + g * hd) * isd;
        if (sc[j] > mx) mx = sc[j];
      }
      for (int j = 0; j <= p; j++) { sc[j] = expf(sc[j] - mx); sum += sc[j]; }
      float *o = A + (size_t)b * qo + hh * hd;
      for (int i = 0; i < hd; i++) o[i] = 0;
      for (int j = 0; j <= p; j++) {
        const uint16_t *v = y->vc + ((size_t)sq * CTX + j) * kvo + g * hd;
        axpy_h(o, sc[j] / sum, v);
      }
    }
}
static void layer_fwd(int l, hdr_t *h, int B) {
  layer_t *y = &S.ly[l];
  int qo = S.hl * hd, kvo = S.kvl * hd, d = C.d;
  for (int b = 0; b < B; b++) rmsnorm(X + (size_t)b * d, XN + (size_t)b * d, d);
  gemm(&y->m[WQ], XN, d, B, Q, qo);
  gemm(&y->m[WK], XN, d, B, Kb, kvo);
  gemm(&y->m[WV], XN, d, B, Vb, kvo);
  attention(y, h, B);
  gemm(&y->m[WO], A, qo, B, T1, d);
  allreduce(T1, B);
  for (size_t i = 0; i < (size_t)B * d; i++) X[i] += T1[i];
  for (int b = 0; b < B; b++) rmsnorm(X + (size_t)b * d, XN + (size_t)b * d, d);
  gemm(&y->m[WG], XN, d, B, T1, S.fl);
  gemm(&y->m[WU], XN, d, B, T2, S.fl);
  for (size_t i = 0; i < (size_t)B * S.fl; i++) { float g = T1[i]; T1[i] = g / (1 + expf(-g)) * T2[i]; }
  gemm(&y->m[WD], T1, S.fl, B, T2, d);
  allreduce(T2, B);
  for (size_t i = 0; i < (size_t)B * d; i++) X[i] += T2[i];
}
static void embed(const int32_t *tok, int B) {
  for (int b = 0; b < B; b++) {
    const int8_t *e = S.emb.w + (size_t)tok[b] * C.d;
    for (int i = 0; i < C.d; i++) X[(size_t)b * C.d + i] = e[i] * S.emb.s[tok[b]] * 37.0f;
  }
}
// local argmax over this node's vocab rows, only for tokens that want logits
static void head(hdr_t *h, int B, float *val, int32_t *idx) {
  int nv = S.v1 - S.v0, nw = 0, wi[MAXB];
  for (int b = 0; b < B; b++) if (h->want[b]) { rmsnorm(X + (size_t)b * C.d, XN + (size_t)nw * C.d, C.d); wi[nw++] = b; }
  float *lg = malloc(4 * (size_t)nw * nv);
  if (nw) gemm(&S.head, XN, C.d, nw, lg, nv);
  for (int b = 0; b < B; b++) { val[b] = -1e30f; idx[b] = -1; }
  for (int j = 0; j < nw; j++) {
    float m = -1e30f; int mi = 0;
    for (int v = 0; v < nv; v++) if (lg[(size_t)j * nv + v] > m) { m = lg[(size_t)j * nv + v]; mi = v; }
    val[wi[j]] = m; idx[wi[j]] = mi + S.v0;
  }
  free(lg);
}

// ---------------- node ----------------
static void node_main(const char *mode, int N, int rank, int nseq) {
  int tp = mode[0] == 't';
  ND = tp ? N : 1; RANK = rank;
  double t0 = now_s();
  build(mode, N, rank, nseq, NULL);
  alloc_bufs();
  fprintf(stderr, "[node %d/%d %s] layers %d..%d emb=%d head=%d  %.2f GB resident shard, built in %.1fs\n", rank, N, mode,
          S.l0, S.l1, S.has_emb, S.has_head, bytes_alloc / 1e9, now_s() - t0);
  int ls = listen_on(PB + rank);
  static int in = -1, out = -1;
  if (!tp) {
    if (failover && setjmp(fail_jb)) {
      if (in >= 0) close(in);
      if (out >= 0) close(out);
      in = out = -1;
      fprintf(stderr, "[node %d] link lost, reconnecting (shard kept)\n", rank);
    }
    // in: from driver (rank 0) or previous node; out: next node or driver
    out = dial_link(rank, rank == N - 1 ? 99 : rank + 1);
    say_hello(out, rank);
    in = acc1(ls); hear_hello(in);
  } else {
    peer = calloc(N, sizeof(int));
    for (int r = rank + 1; r < N; r++) { peer[r] = dial_link(rank, r); say_hello(peer[r], rank); }
    int need = rank + 1;  // ranks below me + driver
    while (need--) {
      int s = acc1(ls), who = hear_hello(s);
      if (who == 99) in = s; else peer[who] = s;
    }
    out = dial_link(rank, 99); say_hello(out, rank);
  }
  static hdr_t h;
  int32_t tok[MAXB]; float val[MAXB]; int32_t idx[MAXB];
  for (;;) {
    recv_hdr(in, &h);
    if (h.quit) { if (!tp && rank != N - 1) send_msg(out, &h, 0, 0, 0, 0); break; }
    int B = h.B;
    if (S.has_emb && (tp || rank == 0)) { rd(in, tok, 4 * B); embed(tok, B); }
    else rd(in, X, 4 * (size_t)B * C.d);
    for (int l = S.l0; l < S.l1; l++) layer_fwd(l, &h, B);
    static int served;
    if (!served++) fprintf(stderr, "[node %d] serving\n", rank);
    if (S.has_head) {
      head(&h, B, val, idx);
      send_msg(out, &h, val, 4 * B, idx, 4 * B);
    } else {
      send_msg(out, &h, X, 4 * (size_t)B * C.d, 0, 0);
    }
  }
}

// ---------------- driver ----------------
static uint64_t ck = 1469598103934665603ULL;
static void ckadd(int32_t t) { ck = (ck ^ (uint64_t)t) * 1099511628211ULL; }
static int32_t prompt_tok(int s, int i) { return (int32_t)(mix((uint64_t)s * 7919 + i) % (uint64_t)(C.V - 2)) + 1; }

static void driver_main(const char *mode, int N, int nseq, int M, int P, int G) {
  int tp = mode[0] == 't';
  int ls = listen_on(PB + 99);
  static int *to, *from; to = calloc(N, sizeof(int)); from = calloc(N, sizeof(int));
  int nto = tp ? N : 1, nfrom = tp ? N : 1;
  static hdr_t h;
  int32_t *last = calloc(nseq, 4), *first = calloc(nseq, 4);
  float val[MAXB]; int32_t idx[MAXB];
  int32_t tok[MAXB];
  int32_t *hist = calloc((size_t)nseq * G, 4);
  int per = nseq / M, *steps = calloc(M, sizeof(int));
  double *sent_at = calloc(M, sizeof(double));
  static volatile int phase, done, inflight, recoveries, replay_tokens, replay_bad;
  static volatile double t0, tpre, lat_sum, rec_s, t_fail;
  static volatile long nlat;
  // receive one result batch (tp: combine argmax across ranks)
  #define RECV_RESULT() do { \
      float bv[MAXB]; int32_t bi[MAXB]; \
      for (int r = 0; r < nfrom; r++) { recv_hdr(from[r], &h); rd(from[r], val, 4 * h.B); rd(from[r], idx, 4 * h.B); \
        for (int b = 0; b < h.B; b++) if (r == 0 || val[b] > bv[b]) { bv[b] = val[b]; bi[b] = idx[b]; } } \
      for (int b = 0; b < h.B; b++) if (h.want[b]) last[h.seq[b]] = bi[b]; } while (0)
  #define SEND() do { for (int r = 0; r < nto; r++) send_msg(to[r], &h, tok, 4 * h.B, 0, 0); } while (0)
  #define CONNECT() do { \
      for (int r = 0; r < nto; r++) { to[r] = r == 0 ? dial_host(nhosts ? hosts[0] : NULL, PB) : dial_link(0, r); say_hello(to[r], 99); } \
      for (int k = 0; k < nfrom; k++) { int s_ = acc1(ls); int who = hear_hello(s_); from[tp ? who : 0] = s_; } } while (0)
  // token fed at position p of sequence s (prompt, then first sampled token, then history)
  #define TOK_AT(s, p) ((p) < P ? prompt_tok(s, p) : (p) == P ? first[s] : hist[(size_t)(s) * G + (p) - P - 1])

  if (failover && setjmp(fail_jb)) {
    t_fail = now_s();
    for (int r = 0; r < nto; r++) close(to[r]);
    for (int r = 0; r < nfrom; r++) close(from[r]);
    fprintf(stderr, "[driver] link lost in %s, reconnecting\n", phase ? "decode" : "prefill");
    recoveries++;
    CONNECT();
    if (phase == 0) goto prefill;
    // replay: rebuild every sequence's KV cache up to its current position, check the
    // re-derived last token matches what was already generated
    inflight = 0;
    for (int m = 0; m < M; m++) {
      if (steps[m] == G) continue;
      for (int b = 0; b < per; b++) {
        int s = m * per + b, upto = P + steps[m];  // positions 0..upto-1
        for (int base = 0; base < upto; base += MAXB) {
          int B = upto - base < MAXB ? upto - base : MAXB;
          memset(&h, 0, sizeof h); h.B = B; h.mb = -1;
          for (int j = 0; j < B; j++) { int p = base + j; h.seq[j] = s; h.pos[j] = p; h.want[j] = (p == upto - 1); tok[j] = TOK_AT(s, p); }
          SEND(); inflight++; replay_tokens += B;
          if (inflight >= 2 * N) { RECV_RESULT(); inflight--; }
        }
      }
    }
    while (inflight) { RECV_RESULT(); inflight--; }
    for (int m = 0; m < M; m++) {
      if (steps[m] == G) continue;
      for (int b = 0; b < per; b++) {
        int s = m * per + b, k = steps[m];
        int32_t expect = k == 0 ? first[s] : hist[(size_t)s * G + k - 1];
        if (last[s] != expect) replay_bad++;
        last[s] = expect;
      }
    }
    rec_s += now_s() - t_fail;
    goto resend;
  }
  CONNECT();

prefill:
  // prefill: each prompt as one batch of P tokens; all prompts in flight at once (pipelines in pp)
  phase = 0;
  t0 = now_s();
  inflight = 0;
  for (int s = 0; s < nseq; s++) {
    for (int base = 0; base < P; base += MAXB) {
      int B = P - base < MAXB ? P - base : MAXB;
      memset(&h, 0, sizeof h); h.B = B; h.mb = s;
      for (int b = 0; b < B; b++) { h.seq[b] = s; h.pos[b] = base + b; h.want[b] = (base + b == P - 1); tok[b] = prompt_tok(s, base + b); }
      SEND(); inflight++;
      if (inflight >= 2 * N) { RECV_RESULT(); inflight--; }
    }
  }
  while (inflight) { RECV_RESULT(); inflight--; }
  tpre = now_s() - t0;
  for (int s = 0; s < nseq; s++) { ckadd(last[s]); first[s] = last[s]; }

  // decode: M micro-batches of nseq/M sequences, each in flight independently
  phase = 1;
  t0 = now_s();
resend:
  for (int m = 0; m < M; m++) {
    if (steps[m] == G) continue;
    memset(&h, 0, sizeof h); h.B = per; h.mb = m;
    for (int b = 0; b < per; b++) { int s = m * per + b; h.seq[b] = s; h.pos[b] = P + steps[m]; h.want[b] = 1; tok[b] = last[s]; }
    sent_at[m] = now_s(); SEND();
  }
  while (done < M) {
    RECV_RESULT();
    int m = h.mb;
    if (m < 0 || m >= M) continue;
    lat_sum += now_s() - sent_at[m]; nlat++;
    for (int b = 0; b < h.B; b++) hist[(size_t)h.seq[b] * G + steps[m]] = last[h.seq[b]];
    steps[m]++;
    if (steps[m] == G) { done++; continue; }
    int B = h.B;
    for (int b = 0; b < B; b++) { h.pos[b] = P + steps[m]; tok[b] = last[h.seq[b]]; h.want[b] = 1; }
    sent_at[m] = now_s(); SEND();
  }
  double tdec = now_s() - t0;
  failover = 0;
  for (int s = 0; s < nseq; s++) for (int g = 0; g < G; g++) ckadd(hist[(size_t)s * G + g]);
  memset(&h, 0, sizeof h); h.quit = 1;
  for (int r = 0; r < nto; r++) send_msg(to[r], &h, 0, 0, 0, 0);
  printf("{\"model\":\"%s\",\"mode\":\"%s\",\"nodes\":%d,\"seqs\":%d,\"microbatches\":%d,\"prompt\":%d,\"gen\":%d,"
         "\"prefill_s\":%.4f,\"prefill_tok_s\":%.2f,\"decode_s\":%.4f,\"decode_tok_s\":%.3f,\"step_ms\":%.2f,\"checksum\":\"%016llx\","
         "\"recoveries\":%d,\"recovery_s\":%.3f,\"replay_tokens\":%d,\"replay_mismatch\":%d}\n",
         C.name, mode, N, nseq, M, P, G, (double)tpre, nseq * P / tpre, tdec, (double)nseq * G / tdec, 1000 * lat_sum / (nlat ? nlat : 1),
         (unsigned long long)ck, (int)recoveries, (double)rec_s, (int)replay_tokens, (int)replay_bad);
  fflush(stdout);
}

// ---------------- single process (no network), optional mmap weights ----------------
static void single_main(int nseq, int P, int G, const char *wfile) {
  ND = 1; RANK = 0;
  double t0 = now_s();
  build("pp", 1, 0, nseq, wfile);
  alloc_bufs();
  fprintf(stderr, "[single] %.2f GB allocated (%s), built in %.1fs\n", bytes_alloc / 1e9, wfile ? "weights mmap'd from file" : "weights in RAM", now_s() - t0);
  static hdr_t h; int32_t tok[MAXB]; float val[MAXB]; int32_t idx[MAXB];
  int32_t *last = calloc(nseq, 4);
  t0 = now_s();
  for (int s = 0; s < nseq; s++) {
    for (int base = 0; base < P; base += MAXB) {
      int B = P - base < MAXB ? P - base : MAXB;
      memset(&h, 0, sizeof h); h.B = B;
      for (int b = 0; b < B; b++) { h.seq[b] = s; h.pos[b] = base + b; h.want[b] = (base + b == P - 1); tok[b] = prompt_tok(s, base + b); }
      embed(tok, B);
      for (int l = 0; l < C.L; l++) layer_fwd(l, &h, B);
      head(&h, B, val, idx);
      for (int b = 0; b < B; b++) if (h.want[b]) last[s] = idx[b];
    }
  }
  double tpre = now_s() - t0;
  for (int s = 0; s < nseq; s++) ckadd(last[s]);
  int32_t *hist = calloc((size_t)nseq * G, 4);
  t0 = now_s();
  for (int g = 0; g < G; g++) {
    memset(&h, 0, sizeof h); h.B = nseq;
    for (int s = 0; s < nseq; s++) { h.seq[s] = s; h.pos[s] = P + g; h.want[s] = 1; tok[s] = last[s]; }
    embed(tok, nseq);
    for (int l = 0; l < C.L; l++) layer_fwd(l, &h, nseq);
    head(&h, nseq, val, idx);
    for (int s = 0; s < nseq; s++) { last[s] = idx[s]; hist[(size_t)s * G + g] = idx[s]; }
  }
  double tdec = now_s() - t0;
  for (int s = 0; s < nseq; s++) for (int g = 0; g < G; g++) ckadd(hist[(size_t)s * G + g]);
  printf("{\"model\":\"%s\",\"mode\":\"single%s\",\"nodes\":1,\"seqs\":%d,\"microbatches\":1,\"prompt\":%d,\"gen\":%d,"
         "\"prefill_s\":%.4f,\"prefill_tok_s\":%.2f,\"decode_s\":%.4f,\"decode_tok_s\":%.3f,\"step_ms\":%.2f,\"checksum\":\"%016llx\"}\n",
         C.name, wfile ? "-mmap" : "", nseq, P, G, tpre, nseq * P / tpre, tdec, (double)nseq * G / tdec, 1000 * tdec / G,
         (unsigned long long)ck);
}

static void gen_file(const char *path) {
  FILE *f = fopen(path, "wb");
  int8_t *row = malloc(C.f > C.d ? C.f : C.d);
  int rows[7] = {C.d, kvd, kvd, C.d, C.f, C.f, C.d}, cols[7] = {C.d, C.d, C.d, C.d, C.d, C.d, C.f};
  for (int l = 0; l < C.L; l++)
    for (int t = 0; t < 7; t++) {
      for (int r = 0; r < rows[t]; r++) { gen_row(row, cols[t], t, l, r); fwrite(row, 1, cols[t], f); }
      float sc = row_scale(cols[t]);
      for (int r = 0; r < rows[t]; r++) fwrite(&sc, 4, 1, f);
    }
  for (int t = EMB; t <= HEAD; t++) {
    for (int r = 0; r < C.V; r++) { gen_row(row, C.d, t, 0, r); fwrite(row, 1, C.d, f); }
    float sc = row_scale(C.d);
    for (int r = 0; r < C.V; r++) fwrite(&sc, 4, 1, f);
  }
  fclose(f);
}

int main(int argc, char **argv) {
  parse_hosts();
  signal(SIGPIPE, SIG_IGN);
  failover = getenv("VLLM_FAILOVER") && atoi(getenv("VLLM_FAILOVER"));
  if (argc < 3) { fprintf(stderr, "see header for usage\n"); return 1; }
  for (size_t i = 0; i < sizeof models / sizeof *models; i++) if (!strcmp(models[i].name, argv[2])) C = models[i];
  if (!C.name) { fprintf(stderr, "unknown model %s\n", argv[2]); return 1; }
  hd = C.d / C.H; kvd = C.Hkv * hd;
  if (!strcmp(argv[1], "info")) {
    double lb = 2.0 * C.d * C.d + 2.0 * kvd * C.d + 3.0 * C.d * C.f;
    printf("%s params %.2fB  int8 bytes %.2f GB  per layer %.1f MB  kv/token/layer %d B\n", C.name,
           (lb * C.L + 2.0 * C.V * C.d) / 1e9, (lb * C.L + 2.0 * C.V * C.d) / 1e9, lb / 1e6, 4 * kvd);
    return 0;
  }
  if (!strcmp(argv[1], "gen")) { gen_file(argv[3]); return 0; }
  if (!strcmp(argv[1], "single")) {
    CTX = atoi(argv[4]) + atoi(argv[5]) + 1;
    single_main(atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), argc > 6 ? argv[6] : NULL);
    return 0;
  }
  if (!strcmp(argv[1], "node")) {
    CTX = atoi(argv[7]); PB = atoi(argv[8]); XB = atoi(argv[9]);
    node_main(argv[3], atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
    return 0;
  }
  if (!strcmp(argv[1], "drive")) {
    PB = atoi(argv[9]); XB = atoi(argv[10]);
    driver_main(argv[3], atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), atoi(argv[8]));
    return 0;
  }
  return 1;
}
