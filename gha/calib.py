#!/usr/bin/env python3
"""Per-runner calibration: time the shard chunk under full load (W parallel procs). No secrets."""
import json, os, sys, time
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import count_primes
def one(c):
    t = time.perf_counter(); count_primes(c * 10**7, (c + 1) * 10**7); return time.perf_counter() - t
if __name__ == "__main__":
    W = int(os.environ.get("WPR", "8")); cs = [2399 - i for i in range(W * 2)]
    with Pool(W) as p: p.map(one, cs[:W]); ts = p.map(one, cs)
    lscpu = os.popen("lscpu").read()
    model = next((l.split(":", 1)[1].strip() for l in lscpu.splitlines() if l.startswith("Model name")), "")
    print("VEIL_JSON " + json.dumps({"kind": "calib", "gidx": int(os.environ["GIDX"]), "cpu_model": model, "nproc": os.cpu_count(),
          "workers": W, "chunk_s_loaded": sum(ts) / len(ts), "chunks_per_s": W / (sum(ts) / len(ts)), "t": time.time()}), flush=True)
