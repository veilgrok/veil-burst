#!/usr/bin/env python3
"""Check lagproxy fidelity: small-message RTT and bulk throughput through one emulated link."""
import socket, subprocess, sys, threading, time
rtt_ms, mbps = float(sys.argv[1]), float(sys.argv[2])
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("127.0.0.1", 24000)); srv.listen(1)
px = subprocess.Popen(["./lagproxy", "-d", str(max(0, rtt_ms * 500 - float(sys.argv[3]) if len(sys.argv) > 3 else rtt_ms * 500)), "-b", str(mbps), "24100:24000:0:1"], stderr=subprocess.DEVNULL)
time.sleep(0.2)
def echo():
    c, _ = srv.accept(); c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    got = 0
    while True:
        d = c.recv(1 << 20)
        if not d: break
        got += len(d)
        if got < 200 * 16 + 1: c.sendall(d)          # ping phase: echo
        elif got >= 200 * 16 + 64 * (1 << 20): c.sendall(b"k")  # bulk phase done
threading.Thread(target=echo, daemon=True).start()
s = socket.create_connection(("127.0.0.1", 24100)); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
ts = []
d = socket.socket(); d.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
for i in range(200):
    t = time.perf_counter(); s.sendall(b"x" * 16); s.recv(64); ts.append(time.perf_counter() - t)
ts.sort()
print(f"RTT target {rtt_ms} ms: median {ts[100]*1e3:.3f} ms, p90 {ts[180]*1e3:.3f} ms")
blob = b"y" * (1 << 20); n = 64
t = time.perf_counter()
for i in range(n): s.sendall(blob)
s.recv(64)
dt = time.perf_counter() - t
print(f"bulk target {mbps} MB/s: {n * 1.048576 / dt:.1f} MB/s")
px.kill()
