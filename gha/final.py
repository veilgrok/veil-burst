#!/usr/bin/env python3
"""Veil final pass: per-runner specs + Geekbench 6/7 + SGEMM + STREAM + mem latency +
llama.cpp + torch train step + /dev/kvm microVM starts. Prints FINAL_JSON lines.
Uses no secrets. Downloads only public artifacts."""
import json, os, re, shutil, subprocess as sp, sys, threading, time, glob, statistics as st
G = int(os.environ.get("GIDX", "0")); T0 = float(os.environ.get("T0", "0") or 0); QUICK = os.environ.get("QUICK") == "1"
HERE = os.path.dirname(os.path.abspath(__file__)); W = os.path.expanduser("~/fp"); os.makedirs(W, exist_ok=True); os.chdir(W)
NP = os.cpu_count()
def J(kind, **kw):
    kw.update(kind=kind, gidx=G, t=time.time()); print("FINAL_JSON " + json.dumps(kw), flush=True)
def sh(c, t=600, env=None):
    # Own process group so a timeout kills every child (avoids pipe-held hangs).
    import signal
    p = sp.Popen(c, shell=True, stdout=sp.PIPE, stderr=sp.STDOUT, text=True, env=env, start_new_session=True)
    try:
        out, _ = p.communicate(timeout=t); return p.returncode, out
    except sp.TimeoutExpired:
        try: os.killpg(p.pid, signal.SIGKILL)
        except Exception: pass
        try: out, _ = p.communicate(timeout=10)
        except Exception: out = ""
        return 124, (out or "") + "\nTIMEOUT"
def wait_until(off, name):
    tgt = T0 + off; now = time.time()
    if T0 and now < tgt: time.sleep(tgt - now)
    J("phase_start", phase=name, late_s=round(time.time() - tgt, 1) if T0 else None)

# ---- specs
lscpu = sh("lscpu")[1]
J("specs", nproc=int(sh("nproc")[1].strip()), cpu_model=next((l.split(":",1)[1].strip() for l in lscpu.splitlines() if l.startswith("Model name")), ""),
  threads_per_core=next((l.split(":",1)[1].strip() for l in lscpu.splitlines() if l.startswith("Thread(s) per core")), ""),
  free_m=sh("free -m")[1], df_h=sh("df -h / /mnt 2>/dev/null")[1], image=os.environ.get("ImageOS"), image_version=os.environ.get("ImageVersion"),
  kvm=os.path.exists("/dev/kvm"), kvm_ls=sh("ls -l /dev/kvm")[1].strip(), flags_avx512=("avx512f" in lscpu))

# ---- setup (parallel downloads)
URLS = {
 "gb6": "https://cdn.geekbench.com/Geekbench-6.7.0-Linux.tar.gz",
 "gb7": "https://cdn.geekbench.com/Geekbench-7.1.0-Linux.tar.gz",
 "llama": "https://github.com/ggml-org/llama.cpp/releases/download/b12282/llama-b12282-bin-ubuntu-x64.tar.gz",
 "fc": "https://github.com/firecracker-microvm/firecracker/releases/download/v1.18.0/firecracker-v1.18.0-x86_64.tgz",
 "vmlinux": "https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.15/x86_64/vmlinux-6.1.155",
 "stream": "https://www.cs.virginia.edu/stream/FTP/Code/stream.c",
 "m05": "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf",
 "m3": "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf",
 "m8": "https://huggingface.co/bartowski/Meta-Llama-3.1-8B-Instruct-GGUF/resolve/main/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf",
}
MODELS = ["m05"] if QUICK else ["m05", "m3", "m8"]
setup = {}
def dl(k):
    t = time.time(); rc, out = sh(f"curl -sSLf --retry 3 -o {k}.dl '{URLS[k]}'", 900)
    setup[k] = {"rc": rc, "s": round(time.time() - t, 1), "bytes": os.path.getsize(f"{k}.dl") if os.path.exists(f"{k}.dl") else 0}
def pip():
    t = time.time(); rc, out = sh("python3 -m venv ~/fp/venv && ~/fp/venv/bin/pip -q install numpy && ~/fp/venv/bin/pip -q install torch --index-url https://download.pytorch.org/whl/cpu", 900)
    setup["pip"] = {"rc": rc, "s": round(time.time() - t, 1), "err": out[-300:] if rc else ""}
def tmb():
    t = time.time(); rc, out = sh(f"gcc -O2 {HERE}/memlat.c -o memlat", 120)
    setup["memlat_build"] = {"rc": rc, "s": round(time.time() - t, 1)}
th = [threading.Thread(target=dl, args=(k,)) for k in ["gb6", "gb7", "llama", "fc", "vmlinux", "stream"] + MODELS] + [threading.Thread(target=pip), threading.Thread(target=tmb)]
t_setup = time.time(); [x.start() for x in th]; [x.join() for x in th]
sh("mkdir -p gb6 gb7 llama fc && tar xzf gb6.dl -C gb6 --strip-components=1 && tar xzf gb7.dl -C gb7 --strip-components=1 && tar xzf llama.dl -C llama && tar xzf fc.dl -C fc", 300)
J("setup", wall_s=round(time.time() - t_setup, 1), items=setup, df_after=sh("df -h /")[1])
PY = os.path.expanduser("~/fp/venv/bin/python")

# ---- Geekbench (barrier T0; GB7 at T0+9min)
def gb(ver, d):
    b = glob.glob(f"{d}/geekbench{ver}")
    if not b: return J(f"gb{ver}", ok=False, err="binary missing", files=os.listdir(d)[:20] if os.path.isdir(d) else [])
    t = time.time(); rc, out = sh(f"cd {d} && ./geekbench{ver} --cpu", 1200); dt = time.time() - t
    if rc != 0 and "internal code" in out and os.environ.get("GB_RETRY") == "1":
        J(f"gb{ver}_retry", first_tail=out[-200:])
        e = dict(os.environ, SSL_CERT_FILE="/etc/ssl/certs/ca-certificates.crt", SSL_CERT_DIR="/etc/ssl/certs", CURL_CA_BUNDLE="/etc/ssl/certs/ca-certificates.crt")
        t = time.time(); rc, out = sh(f"cd {d} && ./geekbench{ver} --cpu", 1200, env=e); dt = time.time() - t
    url = re.findall(r"https://browser\.geekbench\.com/v\d/cpu/\d+", out)
    sc = re.search(r"Single-Core Score\s+(\d+)", out); mc = re.search(r"Multi-Core Score\s+(\d+)", out)
    J(f"gb{ver}", ok=rc == 0, rc=rc, wall_s=round(dt, 1), url=url[0] if url else None, single=int(sc.group(1)) if sc else None,
      multi=int(mc.group(1)) if mc else None, tail=out[-600:] if (rc or not url) else "")
wait_until(0, "gb6"); gb(6, "gb6")
wait_until(0 if QUICK else 600, "gb7"); gb(7, "gb7")

# ---- SGEMM / vector math / ML (T0+20min)
wait_until(0 if QUICK else 1200, "compute")
open("compute.py", "w").write(r'''
import json, os, time, numpy as np
out = {"numpy": np.__version__}
N = 4096; a = np.random.rand(N, N).astype(np.float32); b = np.random.rand(N, N).astype(np.float32); a @ b
ts = []
for _ in range(5):
    t = time.perf_counter(); a @ b; ts.append(time.perf_counter() - t)
out["np_sgemm_gflops"] = 2 * N**3 / min(ts) / 1e9
N2 = 4096; a64 = a.astype(np.float64); b64 = b.astype(np.float64); a64 @ b64
t = time.perf_counter(); a64 @ b64; out["np_dgemm_gflops"] = 2 * N2**3 / (time.perf_counter() - t) / 1e9
try:
    import torch
    torch.set_num_threads(os.cpu_count()); out["torch"] = torch.__version__; out["torch_threads"] = torch.get_num_threads()
    A = torch.rand(N, N); B = torch.rand(N, N); A @ B; ts = []
    for _ in range(5):
        t = time.perf_counter(); A @ B; ts.append(time.perf_counter() - t)
    out["torch_sgemm_gflops"] = 2 * N**3 / min(ts) / 1e9
    x = torch.rand(50_000_000); y = torch.exp(x) * torch.sin(x) + x * 0.5; ts = []
    for _ in range(3):
        t = time.perf_counter(); y = torch.exp(x) * torch.sin(x) + x * 0.5; ts.append(time.perf_counter() - t)
    out["torch_vec_gelem_s"] = 50_000_000 / min(ts) / 1e9
    torch.set_num_threads(1); t = time.perf_counter(); y = torch.exp(x) * torch.sin(x) + x * 0.5; out["torch_vec_1t_gelem_s"] = 50_000_000 / (time.perf_counter() - t) / 1e9
    torch.set_num_threads(os.cpu_count())
    import torch.nn as nn
    torch.manual_seed(0)
    layer = nn.TransformerEncoderLayer(d_model=512, nhead=8, dim_feedforward=2048, batch_first=True)
    m = nn.Sequential(nn.TransformerEncoder(layer, num_layers=6), nn.Linear(512, 512))
    opt = torch.optim.AdamW(m.parameters(), 1e-4); X = torch.randn(16, 256, 512); Y = torch.randn(16, 256, 512)
    for i in range(2):
        opt.zero_grad(); l = ((m(X) - Y) ** 2).mean(); l.backward(); opt.step()
    t = time.perf_counter(); steps = 6
    for i in range(steps):
        opt.zero_grad(); l = ((m(X) - Y) ** 2).mean(); l.backward(); opt.step()
    dt = time.perf_counter() - t
    out["train"] = {"model": "6L d512 TransformerEncoder (~19M params)", "params": sum(p.numel() for p in m.parameters()),
                    "batch": 16, "seq": 256, "steps_s": steps / dt, "tokens_s": steps * 16 * 256 / dt}
except Exception as e:
    out["torch_err"] = repr(e)[:300]
print("COMPUTE " + json.dumps(out))
''')
rc, out = sh(f"{PY} compute.py", 900)
m = re.search(r"COMPUTE (\{.*\})", out); J("compute", ok=bool(m), **(json.loads(m.group(1)) if m else {"err": out[-500:]}))

# ---- STREAM + memory latency
rc, out = sh("gcc -O3 -march=native -fopenmp -mcmodel=medium -DSTREAM_ARRAY_SIZE=100000000 -DNTIMES=10 -x c stream.dl -o stream_bin", 120)
res = {}
for thr in [NP, 1]:
    rc, out = sh(f"OMP_NUM_THREADS={thr} ./stream_bin", 300)
    res[f"t{thr}"] = {k: float(v) for k, v in re.findall(r"^(Copy|Scale|Add|Triad):\s+([\d.]+)", out, re.M)}
J("stream", unit="MB/s best rate", array_elems=100000000, **res)
rc, out = sh("./memlat", 300)
lat = re.findall(r"^LAT (\d+) ([\d.]+)", out, re.M)
J("memlat", ok=bool(lat), method="pointer chase, random 64B lines, 2e7 loads", rows=[{"kb": int(a), "ns_single": float(b)} for a, b in lat])

# ---- llama.cpp
lb = glob.glob("llama/**/llama-bench", recursive=True)
for k in MODELS:
    if not lb: J("llama", model=k, ok=False, err="llama-bench missing"); break
    d = os.path.dirname(lb[0])
    rc, out = sh(f"LD_LIBRARY_PATH={d} {lb[0]} -m {k}.dl -p 512 -n 128 -t {NP} -r 2 -o json", 900)
    try:
        js = json.loads(out[out.index("["):out.rindex("]") + 1])
        J("llama", model=k, ok=True, rows=[{"n_prompt": r.get("n_prompt"), "n_gen": r.get("n_gen"), "avg_ts": r.get("avg_ts"), "model_size": r.get("model_size"), "model_type": r.get("model_type")} for r in js])
    except Exception as e:
        J("llama", model=k, ok=False, rc=rc, err=out[-400:])
if G == 0 and lb:  # loopback RPC overhead check
    d = os.path.dirname(lb[0]); rpc = glob.glob(f"{d}/rpc-server")
    if rpc:
        p = sp.Popen(f"LD_LIBRARY_PATH={d} {rpc[0]} -H 127.0.0.1 -p 50052", shell=True, stdout=sp.DEVNULL, stderr=sp.DEVNULL); time.sleep(3)
        rc, out = sh(f"LD_LIBRARY_PATH={d} {lb[0]} -m m05.dl -p 512 -n 128 -t {NP} -r 2 --rpc 127.0.0.1:50052 -o json", 600); p.kill()
        try:
            js = json.loads(out[out.index("["):out.rindex("]") + 1]); J("llama_rpc_loopback", ok=True, rows=[{"n_prompt": r.get("n_prompt"), "n_gen": r.get("n_gen"), "avg_ts": r.get("avg_ts"), "backends": r.get("backends")} for r in js])
        except Exception: J("llama_rpc_loopback", ok=False, rc=rc, err=out[-400:])
    else: J("llama_rpc_loopback", ok=False, err="rpc-server not in release tarball")

# ---- /dev/kvm microVMs (Firecracker) or container fallback
def pctl(a, p):
    a = sorted(a); k = (len(a) - 1) * p; f = int(k); c = min(f + 1, len(a) - 1); return a[f] + (a[c] - a[f]) * (k - f)
if os.path.exists("/dev/kvm"):
    sh("sudo chmod 666 /dev/kvm")
    open("init.c", "w").write("#include <sys/reboot.h>\n#include <unistd.h>\nint main(){sync();reboot(RB_AUTOBOOT);return 0;}\n")
    rc, out = sh("gcc -static -O2 init.c -o init && mkdir -p ird && cp init ird/init && (cd ird && find . | cpio -o -H newc 2>/dev/null) > initrd.cpio", 120)
    fcb = (glob.glob("fc/**/firecracker-v*-x86_64", recursive=True) or [""])[0]
    def boot(i):
        cfg = {"boot-source": {"kernel_image_path": os.path.abspath("vmlinux.dl"), "initrd_path": os.path.abspath("initrd.cpio"),
               "boot_args": "console=ttyS0 reboot=k panic=1 pci=off quiet i8042.noaux i8042.nomux i8042.dumbkbd"},
               "drives": [], "machine-config": {"vcpu_count": 1, "mem_size_mib": 128}}
        fn = f"cfg{i}.json"; json.dump(cfg, open(fn, "w"))
        t = time.perf_counter(); r = sp.run([fcb, "--no-api", "--config-file", fn, "--level", "Error"], stdout=sp.DEVNULL, stderr=sp.DEVNULL, timeout=60)
        return time.perf_counter() - t, r.returncode
    if fcb and rc == 0:
        seq = [boot(i) for i in range(20)]
        ok = [d for d, c in seq if c == 0]
        res = {"seq_n": 20, "seq_ok": len(ok), "seq_rcs": sorted(set(c for _, c in seq))}
        if ok: res.update(cold_p50_ms=pctl(ok, .5) * 1000, cold_p95_ms=pctl(ok, .95) * 1000)
        for n in [16, 64]:
            out_l = []; t = time.perf_counter()
            ths = [threading.Thread(target=lambda i=i: out_l.append(boot(1000 + n * 10 + i))) for i in range(n)]
            [x.start() for x in ths]; [x.join() for x in ths]; wall = time.perf_counter() - t
            okp = [d for d, c in out_l if c == 0]
            res[f"par{n}"] = {"ok": len(okp), "wall_s": wall, "vms_per_s": len(okp) / wall, "p50_ms": pctl(okp, .5) * 1000 if okp else None, "p95_ms": pctl(okp, .95) * 1000 if okp else None}
        J("microvm", engine="firecracker v1.18.0", kernel="vmlinux-6.1.155 (firecracker-ci)", vm="1 vCPU/128 MiB, initrd static init -> reboot", **res)
    else:
        J("microvm", ok=False, err=("firecracker missing" if not fcb else out[-300:]))
else:
    J("microvm", ok=False, err="/dev/kvm not present")
# container start (always, for comparison)
sh("docker pull -q alpine:3.20", 120); ts = []
for i in range(10):
    t = time.perf_counter(); r = sh("docker run --rm alpine:3.20 true", 60); ts.append(time.perf_counter() - t)
J("container", engine="docker runc", n=10, p50_ms=pctl(ts, .5) * 1000, p95_ms=pctl(ts, .95) * 1000)
J("done", df_end=sh("df -h /")[1])
