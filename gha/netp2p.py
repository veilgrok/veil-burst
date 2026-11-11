#!/usr/bin/env python3
"""Runner<->runner direct-path test: private/public TCP, ICMP, and UDP hole punch via public STUN. Rendezvous on Veil relay KV.
Token from env, never printed."""
import http.client, json, os, select, socket, struct, subprocess, sys, time, urllib.parse
G = int(os.environ["GIDX"]); N = int(os.environ["NPEERS"]); RUN = os.environ["GITHUB_RUN_ID"]
u = urllib.parse.urlparse(os.environ["VEIL_RELAY_URL"]); H = {"X-Token": os.environ["VEIL_RELAY_TOKEN"], "X-Runner": f"p2p{G}"}
def J(o): print("VEIL_JSON " + json.dumps(o), flush=True)
def req(m, path, body=None):
    c = http.client.HTTPSConnection(u.netloc, timeout=30); c.request(m, path, body=body, headers=H); r = c.getresponse().read(); c.close(); return r
def stun(sock, host, port):
    tx = os.urandom(12); sock.sendto(struct.pack("!HHI", 1, 0, 0x2112A442) + tx, (host, port)); end = time.time() + 3
    while time.time() < end:
        r, _, _ = select.select([sock], [], [], 0.5)
        if not r: continue
        d, _ = sock.recvfrom(2048)
        if d[8:20] != tx: continue
        i = 20
        while i < len(d):
            t, l = struct.unpack("!HH", d[i:i + 4]); v = d[i + 4:i + 4 + l]
            if t == 0x0020:
                p = struct.unpack("!H", v[2:4])[0] ^ 0x2112; ip = socket.inet_ntoa(struct.pack("!I", struct.unpack("!I", v[4:8])[0] ^ 0x2112A442)); return ip, p
            i += 4 + l + (-l % 4)
    return None
priv = subprocess.run("hostname -I", shell=True, capture_output=True, text=True).stdout.split()[0]
# TCP echo server
ts = socket.socket(); ts.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ts.bind(("0.0.0.0", 47001)); ts.listen(16); ts.setblocking(False)
us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); us.bind(("0.0.0.0", 47002))
m1 = stun(us, "stun.l.google.com", 19302); m2 = stun(us, "stun.cloudflare.com", 3478)
me = {"g": G, "priv": priv, "pub": m1[0] if m1 else None, "port": m1[1] if m1 else None, "port2": m2[1] if m2 else None}
J({"kind": "p2p_self", **me, "nat_endpoint_independent": bool(m1 and m2 and m1 == m2)})
req("POST", f"/kv/p2p-{RUN}-{G}", json.dumps(me).encode())
peers = {}; end = time.time() + 150
while time.time() < end and len(peers) < N - 1:
    d = json.loads(req("GET", f"/kv/p2p-{RUN}-")); peers = {int(k.rsplit("-", 1)[1]): json.loads(v) for k, v in d.items() if not k.endswith(f"-{G}")}; time.sleep(2)
J({"kind": "p2p_peers", "g": G, "n": len(peers)})
conns = []
def serve(t):
    end = time.time() + t
    while time.time() < end:
        rl = [ts, us] + conns; r, _, _ = select.select(rl, [], [], 0.05)
        for s in r:
            if s is ts:
                try: c, _ = ts.accept(); c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1); conns.append(c)
                except BlockingIOError: pass
            elif s is us:
                d, a = us.recvfrom(2048)
                if d.startswith(b"PING"): us.sendto(b"PONG" + d[4:], a)
                elif d.startswith(b"PONG"): pongs.append((d, a, time.perf_counter()))
            else:
                try: d = s.recv(4096)
                except Exception: d = b""
                if d: s.sendall(d)
                else: conns.remove(s)
pongs = []
import threading; threading.Thread(target=serve, args=(10**6,), daemon=True).start()
time.sleep(3)
# UDP hole punch: all peers simultaneously, 12 s
sent = {}; t_end = time.time() + 12; seq = 0
while time.time() < t_end:
    for g, p in peers.items():
        if p.get("pub"):
            seq += 1; sent[seq] = (g, time.perf_counter()); us.sendto(b"PING" + str(seq).encode(), (p["pub"], p["port"]))
    time.sleep(0.1)
time.sleep(1)
res = {}
for d, a, t in pongs:
    s = int(d[4:]); g, t0 = sent.get(s, (None, None))
    if g is not None: res.setdefault(g, []).append((t - t0) * 1000)
for g, p in sorted(peers.items()):
    r = sorted(res.get(g, [])); o = {"kind": "p2p_udp", "g": G, "peer": g, "pongs": len(r)}
    if r: o.update(p50_ms=r[len(r) // 2], p95_ms=r[int(len(r) * .95)], min_ms=r[0])
    J(o)
# TCP direct to private and public
for g, p in sorted(peers.items()):
    for kind in ("priv", "pub"):
        ip = p.get(kind); o = {"kind": "p2p_tcp", "g": G, "peer": g, "via": kind}
        try:
            c = socket.create_connection((ip, 47001), timeout=2); c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1); r = []
            for _ in range(30):
                t = time.perf_counter(); c.sendall(b"x" * 64); n = 0
                while n < 64: n += len(c.recv(4096))
                r.append((time.perf_counter() - t) * 1000)
            r.sort(); o.update(ok=True, p50_ms=r[15], min_ms=r[0]); c.close()
        except Exception as e: o.update(ok=False, err=type(e).__name__)
        J(o)
    pr = subprocess.run(f"ping -c 3 -W 1 {p['priv']}", shell=True, capture_output=True, text=True).stdout.strip().splitlines()[-1:]
    J({"kind": "p2p_icmp_priv", "g": G, "peer": g, "out": pr[0][:90] if pr else ""})
time.sleep(20)
