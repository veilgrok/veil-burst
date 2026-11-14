#!/usr/bin/env python3
"""veil-llm on real runners: every runner in the matrix runs this.

1. Build vllm with -march=native.
2. Rendezvous on the Veil relay KV (same API as gha/netp2p.py), exchange addresses.
3. Measure the links (TCP RTT, bulk MB/s) from every rank to its ring neighbour and to rank 0.
4. Run the same deterministic schedule on every runner. For each config, ranks < nodes
   start `vllm node`; rank 0 also runs the driver and prints one VEIL_JSON result line.
   Every config uses fresh ports, so no barrier is needed (dials retry).
Token from env, never printed. Real measured numbers only: one driver, one wall clock.
"""
import http.client, json, os, socket, subprocess, sys, threading, time, urllib.parse

G = int(os.environ["GIDX"]); NP = int(os.environ["NPEERS"]); RUN = os.environ.get("GITHUB_RUN_ID", "local")
SCHED = os.environ.get("LLM_SCHEDULE", "standard")
HERE = os.path.dirname(os.path.abspath(__file__))
u = urllib.parse.urlparse(os.environ["VEIL_RELAY_URL"])
H = {"X-Token": os.environ["VEIL_RELAY_TOKEN"], "X-Runner": f"llm{G}"}


def J(o):
    print("VEIL_JSON " + json.dumps(o), flush=True)


def req(m, path, body=None):
    for i in range(5):
        try:
            C = http.client.HTTPConnection if u.scheme == "http" else http.client.HTTPSConnection  # http: local test only
            c = C(u.netloc, timeout=30); c.request(m, path, body=body, headers=H)
            r = c.getresponse().read(); c.close(); return r
        except Exception:
            time.sleep(2 * (i + 1))
    raise RuntimeError("relay unreachable")


def kv_put(k, v): req("POST", f"/kv/llm-{RUN}-{k}", json.dumps(v).encode())


def kv_wait(prefix, n, timeout=300):
    end = time.time() + timeout
    while time.time() < end:
        d = json.loads(req("GET", f"/kv/llm-{RUN}-{prefix}") or b"{}")
        if len(d) >= n:
            return {k: json.loads(v) for k, v in d.items()}
        time.sleep(2)
    raise RuntimeError(f"rendezvous {prefix}: timed out")


# ---- 1. build
VB = os.environ.get("LLM_BIN", "/tmp/vllm")
cc = "gcc -O3 -march=native -fopenmp -o {} {}/vllm.c -lm".format(VB, HERE)
subprocess.run(cc, shell=True, check=True)
cpu = subprocess.run("lscpu | sed -n 's/Model name: *//p' | head -1", shell=True, capture_output=True, text=True).stdout.strip()
mem = int(open("/proc/meminfo").read().split()[1]) // 1024
nproc = os.cpu_count()
J({"kind": "llm_host", "g": G, "cpu": cpu, "nproc": nproc, "mem_mb": mem})

# ---- 2. rendezvous (echo server for link tests on 47101)
priv = subprocess.run("hostname -I", shell=True, capture_output=True, text=True).stdout.split()[0]
ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); EP = 47101 + (G if os.environ.get("LLM_LOCALTEST") else 0)
ls.bind(("0.0.0.0", EP)); ls.listen(16)


def echo_srv():
    while True:
        c, _ = ls.accept(); c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=echo_conn, args=(c,), daemon=True).start()


def echo_conn(c):
    # protocol: 'P'+63 bytes -> echoed; 'B'+8-byte length -> sink that many bytes, reply 'K'
    try:
        while True:
            h = c.recv(64, socket.MSG_WAITALL)
            if len(h) < 64: return
            if h[:1] == b"P": c.sendall(h)
            else:
                n = int.from_bytes(h[1:9], "little"); got = 0
                while got < n: got += len(c.recv(min(1 << 20, n - got)))
                c.sendall(b"K" * 64)
    except Exception:
        return


threading.Thread(target=echo_srv, daemon=True).start()
kv_put(f"addr-{G}", {"g": G, "ip": priv, "ep": EP})
peers = kv_wait("addr-", NP)
ips = [None] * NP; eps = [None] * NP
for v in peers.values():
    ips[v["g"]] = v["ip"]; eps[v["g"]] = v.get("ep", 47101)
J({"kind": "llm_peers", "g": G, "n": NP})


# ---- 3. link measurements
def link(ip, port):
    s = socket.create_connection((ip, port), timeout=10); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    r = []
    for _ in range(50):
        t = time.perf_counter(); s.sendall(b"P" * 64); s.recv(64, socket.MSG_WAITALL); r.append((time.perf_counter() - t) * 1e3)
    r.sort()
    n = 64 << 20; blob = b"x" * (1 << 20)
    t = time.perf_counter(); s.sendall(b"B" + n.to_bytes(8, "little") + b"\0" * 55)
    for _ in range(64): s.sendall(blob)
    s.recv(64, socket.MSG_WAITALL); dt = time.perf_counter() - t; s.close()
    return {"rtt_p50_ms": round(r[25], 3), "rtt_min_ms": round(r[0], 3), "mbps": round(n / dt / 1e6, 1)}


time.sleep(2)
for dst in sorted({(G + 1) % NP, 0} - {G}):
    try:
        J({"kind": "llm_link", "src": G, "dst": dst, **link(ips[dst], eps[dst])})
    except Exception as e:
        J({"kind": "llm_link", "src": G, "dst": dst, "ok": False, "err": type(e).__name__})

# ---- 4. schedule (identical on every runner)
T = str(nproc)
def cfgs():
    out = []
    models = [m for m in os.environ.get("LLM_MODELS", "s1,m7").split(",") if m]
    for m in models:
        out.append(dict(model=m, mode="single", nodes=1, seqs=1))
        out.append(dict(model=m, mode="single", nodes=1, seqs=8))
        for n in (2, 4, 8, 16):
            if n > NP: continue
            out.append(dict(model=m, mode="pp", nodes=n, seqs=1, mb=1))
            out.append(dict(model=m, mode="pp", nodes=n, seqs=8, mb=n if n <= 8 else 8))
            out.append(dict(model=m, mode="pp", nodes=n, seqs=16, mb=min(n, 16)))
            if n <= 8:  # 8 KV heads: tp splits at most 8 ways
                out.append(dict(model=m, mode="tp", nodes=n, seqs=1))
                out.append(dict(model=m, mode="tp", nodes=n, seqs=8))
    # capacity: models bigger than one runner's RAM (no single baseline possible)
    for m in os.environ.get("LLM_BIG", "l34,l70").split(","):
        if not m: continue
        need = {"l34": 4, "l70": 8}.get(m, 8)
        for n in (4, 8, 16):
            if n > NP or n < need: continue
            out.append(dict(model=m, mode="pp", nodes=n, seqs=1, mb=1))
            out.append(dict(model=m, mode="pp", nodes=n, seqs=n, mb=n))
            if n <= 8:
                out.append(dict(model=m, mode="tp", nodes=n, seqs=1))
    return out


P, GEN = os.environ.get("LLM_PROMPT", "64"), os.environ.get("LLM_GEN", "32")
env = dict(os.environ, OMP_NUM_THREADS=T, OMP_WAIT_POLICY="PASSIVE", VLLM_DIAL_S="900")
for i, c in enumerate(cfgs()):
    pb = 40000 + 200 * (i % 100)
    tag = dict(c, idx=i, runners=NP, threads=T)
    if c["mode"] == "single":
        if G == 0:
            r = subprocess.run(["timeout", "900", VB, "single", c["model"], str(c["seqs"]), P, GEN],
                               env=env, capture_output=True, text=True)
            line = (r.stdout.strip().splitlines() or ["{}"])[-1]
            J({"kind": "llm_result", **tag, **json.loads(line or "{}"), "rc": r.returncode})
        continue
    if G >= c["nodes"]:
        continue
    e = dict(env, VLLM_HOSTS=",".join(ips[:c["nodes"]]))
    ctx = str(int(P) + int(GEN) + 2)
    node = subprocess.Popen(["timeout", "1500", VB, "node", c["model"], c["mode"], str(c["nodes"]), str(G), str(c["seqs"]),
                             ctx, str(pb), "0"], env=e, stderr=subprocess.PIPE, text=True)
    if G == 0:
        r = subprocess.run(["timeout", "1500", VB, "drive", c["model"], c["mode"], str(c["nodes"]), str(c["seqs"]),
                            str(c.get("mb", 1)), P, GEN, str(pb), "0"], env=dict(e, OMP_NUM_THREADS="1"),
                           capture_output=True, text=True)
        line = (r.stdout.strip().splitlines() or ["{}"])[-1]
        J({"kind": "llm_result", **tag, **json.loads(line or "{}"), "rc": r.returncode, "err": r.stderr[-300:]})
    _, err = node.communicate()
    if node.returncode:
        J({"kind": "llm_node_fail", **tag, "g": G, "rc": node.returncode, "err": err[-300:]})
# ---- 5. kill gate: same pp job twice, second time rank 2's node is SIGKILLed mid-decode and
# restarted in place (fresh process, rebuilds its shard); survivors reconnect, driver replays.
# Gate: identical checksum, replay_mismatch == 0.
KM = os.environ.get("LLM_KILL_MODEL", "s1")
if NP >= 4:
    for i, kill in enumerate((False, True)):
        pb = 39000 + 200 * i
        e = dict(env, VLLM_HOSTS=",".join(ips[:4]), VLLM_FAILOVER="1")
        args = lambda: [VB, "node", KM, "pp", "4", str(G), "4", "440", str(pb), "0"]
        if G < 4:
            node = subprocess.Popen(args(), env=e, stderr=subprocess.PIPE, text=True)
            if kill and G == 2:
                for line in node.stderr:  # wait until this node is serving decode traffic
                    if "serving" in line: break
                threading.Thread(target=lambda f=node.stderr: f.read(), daemon=True).start()
                time.sleep(float(os.environ.get("LLM_KILL_AFTER", "3")))
                node.kill(); node.wait()
                J({"kind": "llm_killed", "g": G, "t": time.time()})
                node = subprocess.Popen(args(), env=e, stderr=subprocess.DEVNULL)
            else:
                threading.Thread(target=lambda f=node.stderr: f.read(), daemon=True).start()
            if G == 0:
                r = subprocess.run(["timeout", "900", VB, "drive", KM, "pp", "4", "4", "2", "32", "400", str(pb), "0"],
                                   env=dict(e, OMP_NUM_THREADS="1"), capture_output=True, text=True)
                line = (r.stdout.strip().splitlines() or ["{}"])[-1]
                J({"kind": "llm_killtest", "killed": kill, **json.loads(line or "{}"), "rc": r.returncode})
            node.wait()
J({"kind": "llm_done", "g": G})
