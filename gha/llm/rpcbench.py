#!/usr/bin/env python3
"""Prior-art baseline: llama.cpp RPC (layer split across rpc-server nodes) under the
same node limits (cgroup CPU quota / RAM cap) and the same emulated links as veil-llm.

  rpcbench.py --gguf s1-q8.gguf --nodes 4 --cpu 0.5 --rtt-ms 0.9 --mbps 200 [--sm layer|tensor]
nodes=0 means plain local llama-bench (no RPC) as the one-machine baseline.
The client (llama-bench) is node 0's host process and gets the same CPU quota.
"""
import argparse, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bench import cg_setup, launch  # noqa: E402

a = argparse.ArgumentParser()
a.add_argument("--llama", default=os.environ.get("LLAMA_BIN", "/home/user/llama.cpp/build/bin"))
a.add_argument("--gguf", required=True)
a.add_argument("--nodes", type=int, default=0)
a.add_argument("--cpu", type=float, default=0.5)
a.add_argument("--threads", type=int, default=1)
a.add_argument("--mem-gb", type=float, default=0)
a.add_argument("--rtt-ms", type=float, default=0.9)
a.add_argument("--mbps", type=float, default=200)
a.add_argument("--prompt", type=int, default=32)
a.add_argument("--gen", type=int, default=32)
a.add_argument("--sm", default="layer")
a.add_argument("--tag", default="")
o = a.parse_args()
base, xb = 31000, 32000
procs = []
log = open(f"/tmp/rpcbench-{o.nodes}.log", "w")
rpc = []
if o.nodes > 0:
    links = [f"{xb + r}:{base + r}:0:{r + 1}" for r in range(o.nodes)]
    procs.append(subprocess.Popen([f"{HERE}/lagproxy", "-d", str(max(0.0, o.rtt_ms * 500 - 100)), "-b", str(o.mbps)] + links,
                                  stderr=log))
    for r in range(o.nodes):
        cgs = cg_setup(f"rpc{r}", o.cpu, o.mem_gb)
        procs.append(launch([f"{o.llama}/ggml-rpc-server", "-H", "127.0.0.1", "-p", str(base + r), "-t", str(o.threads)],
                            cgs, dict(os.environ), log))
        rpc.append(f"127.0.0.1:{xb + r}")
    time.sleep(1.5)
cmd = [f"{o.llama}/llama-bench", "-m", o.gguf, "-t", str(o.threads), "-p", str(o.prompt), "-n", str(o.gen), "-r", "2",
       "-o", "jsonl", "-ngl", "99" if rpc else "0"]
if rpc:
    cmd += ["--rpc", ",".join(rpc), "-sm", o.sm]
cgs = cg_setup("rpcclient", o.cpu, o.mem_gb)
p = launch(cmd, cgs, dict(os.environ), None)
out, _ = p.communicate()
for q in procs:
    q.terminate()
res = {"engine": "llama.cpp-rpc" if rpc else "llama.cpp", "nodes": max(1, o.nodes), "sm": o.sm if rpc else "none",
       "cpu": o.cpu, "rtt_ms": o.rtt_ms if rpc else 0, "mbps": o.mbps if rpc else 0, "tag": o.tag}
for line in out.decode().splitlines():
    try:
        j = json.loads(line)
    except ValueError:
        continue
    key = "prefill_tok_s" if j.get("n_prompt", 0) > 0 else "decode_tok_s"
    res[key] = round(j["avg_ts"], 3)
if "decode_tok_s" not in res:
    log.flush()
    res["error"] = open(log.name).read()[-1500:]
print(json.dumps(res))
