// lagproxy: userspace link emulator (stand-in for tc/netem, which this box lacks).
// One process emulates every link of a cluster. Each direction of every TCP
// connection is billed to the SENDING node's egress token bucket (one NIC per
// node, shared by all its connections), then delayed by a fixed one-way latency.
//
// usage: lagproxy -d <one_way_us> -b <MB/s per node egress, 0=unlimited>
//                 lport:dport:src_node:dst_node [...]
// A client connecting to 127.0.0.1:lport is treated as node src_node, and is
// forwarded to 127.0.0.1:dport which is node dst_node.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXL 128
#define MAXC 512
#define CHUNK 65536
#define QMAX (8 << 20)

static double now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

typedef struct pkt { struct pkt *next; double due; int len, off; char data[]; } pkt;
typedef struct { int in, out, node; pkt *head, *tail; long qbytes; int eof, closed; } dir_t;
typedef struct { int used; dir_t d[2]; } conn_t;  // d[0]: client->server, d[1]: server->client

static int lfd[MAXL], ldport[MAXL], lsrc[MAXL], ldst[MAXL], nl;
static conn_t conns[MAXC];
static double bucket[256];  // per-node egress: time the NIC is next free
static double delay_us = 450, bw = 200;  // bw in bytes/us == MB/s

static int mk_listen(int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (void *)&a, sizeof a) || listen(s, 64)) { perror("bind"); exit(1); }
  fcntl(s, F_SETFL, O_NONBLOCK);
  return s;
}

static void setfast(int s) {
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  fcntl(s, F_SETFL, O_NONBLOCK);
}

static void conn_close(conn_t *c) {
  for (int k = 0; k < 2; k++) {
    pkt *p = c->d[k].head;
    while (p) { pkt *n = p->next; free(p); p = n; }
  }
  close(c->d[0].in); close(c->d[0].out);
  c->used = 0;
}

int main(int argc, char **argv) {
  int i = 1;
  for (; i < argc && argv[i][0] == '-'; i += 2) {
    if (argv[i][1] == 'd') delay_us = atof(argv[i + 1]);
    else if (argv[i][1] == 'b') bw = atof(argv[i + 1]);
  }
  for (; i < argc; i++) {
    int lp, dp, s, d;
    if (sscanf(argv[i], "%d:%d:%d:%d", &lp, &dp, &s, &d) != 4) { fprintf(stderr, "bad %s\n", argv[i]); return 1; }
    lfd[nl] = mk_listen(lp); ldport[nl] = dp; lsrc[nl] = s; ldst[nl] = d; nl++;
  }
  prctl(PR_SET_TIMERSLACK, 1);
  fprintf(stderr, "lagproxy: %d links, one-way %.0f us, %.0f MB/s per node egress\n", nl, delay_us, bw);
  static struct pollfd pf[MAXL + 2 * MAXC];
  static int pmap[MAXL + 2 * MAXC];
  char *buf = malloc(CHUNK);
  for (;;) {
    int n = 0;
    for (int l = 0; l < nl; l++) { pf[n].fd = lfd[l]; pf[n].events = POLLIN; pmap[n] = -1 - l; n++; }
    double t = now_us(), next = 1e18;
    for (int c = 0; c < MAXC; c++) {
      if (!conns[c].used) continue;
      for (int k = 0; k < 2; k++) {
        dir_t *d = &conns[c].d[k];
        if (!d->eof && d->qbytes < QMAX) { pf[n].fd = d->in; pf[n].events = POLLIN; pmap[n] = c * 2 + k; n++; }
        if (d->head) {
          if (d->head->due <= t) { pf[n].fd = d->out; pf[n].events = POLLOUT; pmap[n] = c * 2 + k; n++; }
          else if (d->head->due < next) next = d->head->due;
        }
      }
    }
    int to_us = next > 1e17 ? 100000 : (int)(next - t);
    if (to_us < 0) to_us = 0;
    struct timespec ts = {to_us / 1000000, (to_us % 1000000) * 1000};
    if (ppoll(pf, n, &ts, NULL) < 0 && errno != EINTR) { perror("ppoll"); return 1; }
    t = now_us();
    for (int j = 0; j < n; j++) {
      if (!pf[j].revents) continue;
      if (pmap[j] < 0) {  // accept
        int l = -1 - pmap[j];
        int a = accept(lfd[l], 0, 0);
        if (a < 0) continue;
        // the target may still be loading its shard: retry (startup only)
        int b = -1;
        for (int tries = 0; tries < 6000; tries++) {
          b = socket(AF_INET, SOCK_STREAM, 0);
          struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(ldport[l])};
          sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
          if (connect(b, (void *)&sa, sizeof sa) == 0) break;
          close(b); b = -1; usleep(10000);
        }
        if (b < 0) { close(a); continue; }
        setfast(a); setfast(b);
        int c = 0;
        while (c < MAXC && conns[c].used) c++;
        if (c == MAXC) { close(a); close(b); continue; }
        memset(&conns[c], 0, sizeof conns[c]);
        conns[c].used = 1;
        conns[c].d[0] = (dir_t){.in = a, .out = b, .node = lsrc[l]};
        conns[c].d[1] = (dir_t){.in = b, .out = a, .node = ldst[l]};
        continue;
      }
      conn_t *c = &conns[pmap[j] / 2];
      if (!c->used) continue;
      dir_t *d = &c->d[pmap[j] % 2];
      if (pf[j].events & POLLIN) {
        ssize_t r = read(d->in, buf, CHUNK);
        if (r <= 0) {
          if (r < 0 && errno == EAGAIN) continue;
          d->eof = 1;
          if (!d->head) { conn_close(c); }
          continue;
        }
        // serialization on the sender's NIC, then propagation delay
        double start = bucket[d->node] > t ? bucket[d->node] : t;
        double done = bw > 0 ? start + r / bw : t;
        bucket[d->node] = done;
        pkt *p = malloc(sizeof(pkt) + r);
        p->next = 0; p->due = done + delay_us; p->len = r; p->off = 0;
        memcpy(p->data, buf, r);
        if (d->tail) d->tail->next = p; else d->head = p;
        d->tail = p; d->qbytes += r;
      } else if (pf[j].events & POLLOUT) {
        while (d->head && d->head->due <= t) {
          pkt *p = d->head;
          ssize_t w = write(d->out, p->data + p->off, p->len - p->off);
          if (w < 0) { if (errno == EAGAIN) break; conn_close(c); goto nextfd; }
          p->off += w;
          if (p->off < p->len) break;
          d->qbytes -= p->len;
          d->head = p->next;
          if (!d->head) d->tail = 0;
          free(p);
        }
        if (d->eof && !d->head) conn_close(c);
      }
    nextfd:;
    }
  }
}
