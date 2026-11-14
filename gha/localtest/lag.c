// lag: TCP forwarder that emulates a cross-runner link on one host (no netem in the container).
// Every chunk read on one side is written to the other side DELAY_US later (one way), and each direction is capped at
// MBPS megabytes per second (0 = uncapped). Bandwidth is kept: many chunks can be in flight, as on a real link.
//   lag LISTEN_PORT TARGET_HOST:PORT DELAY_US [MBPS]
// Accepts any number of connections (one pair of threads each); they share the cap, like one host's link. Exits never; kill it.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static long dly_us; static double mbps; static struct sockaddr_in tgt;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void sleep_until(double t) { double d = t - now(); if (d <= 0) return; struct timespec ts = {(time_t)d, (long)((d - (time_t)d) * 1e9)}; nanosleep(&ts, 0); }
#define NQ 4096
#define CH (64 << 10)
struct dir { int in, out; char *buf[NQ]; int len[NQ]; double due[NQ]; volatile unsigned h, t; pthread_mutex_t mu; pthread_cond_t cv; volatile int eof; int dirn; };
static pthread_mutex_t lmu = PTHREAD_MUTEX_INITIALIZER; static double nextfree[2];
static void *rd(void *a) { struct dir *d = a;
  for (;;) { char *b = malloc(CH); ssize_t r = read(d->in, b, CH); double t = now();
    pthread_mutex_lock(&d->mu); while (d->t - d->h >= NQ) pthread_cond_wait(&d->cv, &d->mu);
    if (r <= 0) { free(b); d->eof = 1; pthread_cond_broadcast(&d->cv); pthread_mutex_unlock(&d->mu); return 0; }
    unsigned i = d->t % NQ; d->buf[i] = b; d->len[i] = r; d->due[i] = t + dly_us / 1e6; d->t++; pthread_cond_broadcast(&d->cv); pthread_mutex_unlock(&d->mu); } }
static void *wr(void *a) { struct dir *d = a;
  for (;;) { pthread_mutex_lock(&d->mu); while (d->h == d->t && !d->eof) pthread_cond_wait(&d->cv, &d->mu);
    if (d->h == d->t && d->eof) { pthread_mutex_unlock(&d->mu); shutdown(d->out, SHUT_WR); return 0; }
    unsigned i = d->h % NQ; char *b = d->buf[i]; int n = d->len[i]; double due = d->due[i]; pthread_mutex_unlock(&d->mu);
    sleep_until(due);
    if (mbps > 0) { pthread_mutex_lock(&lmu); double t = now(), *nf = &nextfree[d->dirn]; double st = *nf > t ? *nf : t; *nf = st + n / (mbps * 1e6); double fin = *nf; pthread_mutex_unlock(&lmu); sleep_until(fin); }  /* one link per lag instance: all connections share the cap in each direction */
    char *p = b; int k = n; while (k > 0) { ssize_t w = write(d->out, p, k); if (w <= 0) { k = -1; break; } p += w; k -= w; }
    free(b); pthread_mutex_lock(&d->mu); d->h++; pthread_cond_broadcast(&d->cv); pthread_mutex_unlock(&d->mu); if (k < 0) { shutdown(d->in, SHUT_RD); } } }
static void *conn(void *a) { int c = (int)(intptr_t)a, one = 1; int s = socket(AF_INET, SOCK_STREAM, 0); setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (connect(s, (void *)&tgt, sizeof tgt)) { close(c); close(s); return 0; }
  struct dir *u = calloc(1, sizeof *u), *v = calloc(1, sizeof *v); u->in = c; u->out = s; v->in = s; v->out = c; u->dirn = 0; v->dirn = 1;
  pthread_mutex_init(&u->mu, 0); pthread_cond_init(&u->cv, 0); pthread_mutex_init(&v->mu, 0); pthread_cond_init(&v->cv, 0);
  pthread_t t[4]; pthread_create(&t[0], 0, rd, u); pthread_create(&t[1], 0, wr, u); pthread_create(&t[2], 0, rd, v); pthread_create(&t[3], 0, wr, v);
  for (int i = 0; i < 4; i++) pthread_join(t[i], 0); close(c); close(s); return 0; }
int main(int argc, char **v) {
  if (argc < 4) { fprintf(stderr, "usage: lag LISTEN_PORT HOST:PORT DELAY_US [MBPS]\n"); return 2; } signal(SIGPIPE, SIG_IGN);
  char h[64]; int port; sscanf(v[2], "%63[^:]:%d", h, &port); tgt.sin_family = AF_INET; tgt.sin_port = htons(port); inet_pton(AF_INET, h, &tgt.sin_addr);
  dly_us = atol(v[3]); mbps = argc > 4 ? atof(v[4]) : 0; int one = 1, ls = socket(AF_INET, SOCK_STREAM, 0); setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(atoi(v[1]))}; if (bind(ls, (void *)&a, sizeof a) || listen(ls, 16)) { perror("lag bind"); return 1; }
  for (;;) { int c = accept(ls, 0, 0); if (c < 0) continue; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); pthread_t t; pthread_create(&t, 0, conn, (void *)(intptr_t)c); pthread_detach(t); } }
