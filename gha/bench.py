#!/usr/bin/env python3
"""Per-runner specs, CPU benchmark, and batch share for the Veil GHA burst.
Prints one VEIL_JSON line per phase. No secrets are read or printed."""
import hashlib, json, os, platform, subprocess, sys, time
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(__file__))
from oracle import count_primes

def emit(kind, **kw):
    kw.update(kind=kind, t=time.time(), gidx=GIDX, owner=os.environ.get("GITHUB_REPOSITORY", ""))
    print("VEIL_JSON " + json.dumps(kw), flush=True)

def sh(cmd):
    try: return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=20).stdout.strip()
    except Exception as e: return f"err {e}"

def spin(sec):
    end = time.perf_counter() + sec; b = b"veil" * 16; n = 0
    while True:
        for _ in range(2048):
            b = hashlib.sha256(b).digest()
        n += 2048
        if time.perf_counter() >= end: return n

def cp(iv): 
    t = time.perf_counter(); c = count_primes(*iv); return c, time.perf_counter() - t

GIDX = int(os.environ["GIDX"]); TOTAL = int(os.environ["TOTAL"])
phases = os.environ.get("PHASES", "cpu,batch").split(",")
ncpu = os.cpu_count()
lscpu = sh("lscpu")
model = next((l.split(":", 1)[1].strip() for l in lscpu.splitlines() if l.startswith("Model name")), "")
emit("specs", nproc=int(sh("nproc") or 0), cpu_model=model, free_m=sh("free -m"),
     image_os=os.environ.get("ImageOS"), image_version=os.environ.get("ImageVersion"),
     runner_name=os.environ.get("RUNNER_NAME"), arch=platform.machine(), kernel=platform.release())
if "cpu" in phases:
    sec = float(os.environ.get("CPU_SECONDS", "20"))
    one = spin(3.0) / 3.0
    t0 = time.perf_counter()
    with Pool(ncpu) as p: ops = p.map(spin, [sec] * ncpu)
    wall = time.perf_counter() - t0
    emit("cpu", seconds=sec, nproc=ncpu, total_ops=sum(ops), ops_per_s=sum(ops) / wall,
         single_core_ops_per_s=one, core_seconds=ncpu * wall, wall=wall, work="sha256 of 32B digest chain")
if "batch" in phases:
    CH = int(5e7); NCH = int(os.environ.get("BATCH_CHUNKS", "480"))
    mine = [(c * CH, (c + 1) * CH) for c in range(NCH) if c % TOTAL == GIDX]
    t0 = time.perf_counter(); s = time.time()
    with Pool(ncpu) as p: res = p.map(cp, mine, chunksize=1)
    wall = time.perf_counter() - t0
    emit("batch", chunks=len(mine), chunk_ids=[c for c in range(NCH) if c % TOTAL == GIDX][:3],
         primes=sum(r[0] for r in res), compute_s=sum(r[1] for r in res), wall=wall, t_start=s, t_end=time.time(),
         nch=NCH, total=TOTAL)
hold = float(os.environ.get("HOLD", "0"))
if hold > 0:
    time.sleep(hold); emit("hold", hold=hold)
