#!/usr/bin/env python3
"""Run one veil-llm cluster configuration on this box and print the driver's JSON line.

Each emulated node is a separate process in its own cgroup (CPU quota and RAM cap),
and every inter-node byte goes through lagproxy (per-node egress bandwidth + one-way delay).

  bench.py --model s1 --mode pp --nodes 4 --seqs 8 --mb 4 --prompt 64 --gen 32 \
           --cpu 0.5 --mem-gb 2 --rtt-ms 0.9 --mbps 200
"""
import argparse, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
CG = "/sys/fs/cgroup"


def cg_setup(name, cpu, mem_gb):
    paths = []
    if cpu and os.path.isdir(f"{CG}/cpu"):
        p = f"{CG}/cpu/{name}"
        os.makedirs(p, exist_ok=True)
        period = int(os.environ.get("CFS_PERIOD_US", "10000"))
        open(f"{p}/cpu.cfs_period_us", "w").write(str(period))
        open(f"{p}/cpu.cfs_quota_us", "w").write(str(int(period * cpu)))
        paths.append(p)
    if mem_gb and os.path.isdir(f"{CG}/memory"):
        p = f"{CG}/memory/{name}"
        os.makedirs(p, exist_ok=True)
        try:
            open(f"{p}/memory.memsw.limit_in_bytes", "w").write("-1")
        except OSError:
            pass
        open(f"{p}/memory.limit_in_bytes", "w").write(str(int(mem_gb * 2**30)))
        paths.append(p)
    return paths


def launch(cmd, cgs, env, log):
    pre = "".join(f"echo $$ > {p}/cgroup.procs; " for p in cgs)
    return subprocess.Popen(["sh", "-c", pre + "exec " + " ".join(cmd)], env=env, stdout=subprocess.PIPE if log is None else log,
                            stderr=log or subprocess.DEVNULL)


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--model", default="s1")
    a.add_argument("--mode", default="pp", choices=["pp", "tp", "single"])
    a.add_argument("--nodes", type=int, default=1)
    a.add_argument("--seqs", type=int, default=1)
    a.add_argument("--mb", type=int, default=1, help="micro-batches in flight (pp)")
    a.add_argument("--prompt", type=int, default=64)
    a.add_argument("--gen", type=int, default=32)
    a.add_argument("--cpu", type=float, default=0.5, help="cores per node (cgroup quota); 0 = no limit")
    a.add_argument("--threads", type=int, default=1)
    a.add_argument("--mem-gb", type=float, default=0, help="RAM cap per node; 0 = none")
    a.add_argument("--rtt-ms", type=float, default=0.9)
    a.add_argument("--mbps", type=float, default=200)
    a.add_argument("--wfile", default="", help="single mode: mmap weights from this file")
    a.add_argument("--base", type=int, default=21000)
    a.add_argument("--tag", default="")
    a.add_argument("--kill-rank", type=int, default=-1, help="kill test: SIGKILL this pp rank mid-decode")
    a.add_argument("--kill-after", type=float, default=0, help="seconds after the driver starts")
    a.add_argument("--spare-delay", type=float, default=0, help="seconds before the replacement node starts")
    o = a.parse_args()
    VL = os.environ.get("VLLM_BIN", f"{HERE}/vllm")
    env = dict(os.environ, OMP_NUM_THREADS=str(o.threads), OMP_WAIT_POLICY="PASSIVE")
    if o.kill_rank >= 0:
        env["VLLM_FAILOVER"] = "1"
    logf = open(f"/tmp/vllm-{o.mode}-{o.nodes}.log", "w")
    if o.mode == "single":
        cgs = cg_setup("vllm0", o.cpu, o.mem_gb)
        cmd = [VL, "single", o.model, str(o.seqs), str(o.prompt), str(o.gen)] + ([o.wfile] if o.wfile else [])
        p = launch(cmd, cgs, env, None)
        out, _ = p.communicate()
        res = json.loads(out.decode().strip().splitlines()[-1])
    else:
        N = o.nodes
        pb, xb = o.base, o.base + 1000
        procs = []
        if N > 1:
            links = []
            for s in range(N):
                for d in list(range(N)) + [99]:
                    if s != d:
                        dport = pb + (99 if d == 99 else d)
                        links.append(f"{xb + s * 100 + d}:{dport}:{s}:{0 if d == 99 else d}")
            procs.append(subprocess.Popen([f"{HERE}/lagproxy", "-d", str(max(0.0, o.rtt_ms * 500 - 100)), "-b", str(o.mbps)] + links,
                                          stderr=logf))
        ctx = o.prompt + o.gen + 2
        node_cmd = lambda r: [VL, "node", o.model, o.mode, str(N), str(r), str(o.seqs), str(ctx), str(pb), str(xb if N > 1 else 0)]
        nodes = {}
        for r in range(N):
            cgs = cg_setup(f"vllm{r}", o.cpu, o.mem_gb)
            nodes[r] = launch(node_cmd(r), cgs, env, logf)
            procs.append(nodes[r])
        if o.kill_rank >= 0:
            import threading
            def killer():
                time.sleep(o.kill_after)
                nodes[o.kill_rank].kill()
                logf.write(f"[bench] killed rank {o.kill_rank} at +{o.kill_after}s\n"); logf.flush()
                time.sleep(o.spare_delay)
                spare = launch(node_cmd(o.kill_rank), cg_setup(f"vllm{o.kill_rank}s", o.cpu, o.mem_gb), env, logf)
                procs.append(spare)
            threading.Thread(target=killer, daemon=True).start()
        drv = subprocess.Popen([VL, "drive", o.model, o.mode, str(N), str(o.seqs), str(o.mb), str(o.prompt),
                                str(o.gen), str(pb), str(xb if N > 1 else 0)], env=dict(env, OMP_NUM_THREADS="1"),
                               stdout=subprocess.PIPE, stderr=logf)
        out, _ = drv.communicate()
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        lines = out.decode().strip().splitlines()
        if not lines:
            logf.flush()
            print(json.dumps({"error": "driver produced no result", "log": open(logf.name).read()[-2000:]}))
            sys.exit(1)
        res = json.loads(lines[-1])
    if o.kill_rank >= 0:
        res.update(kill_rank=o.kill_rank, kill_after=o.kill_after)
    res.update(cpu=o.cpu, threads=o.threads, mem_gb=o.mem_gb, rtt_ms=o.rtt_ms if o.nodes > 1 else 0,
               mbps=o.mbps if o.nodes > 1 else 0, tag=o.tag)
    print(json.dumps(res))


if __name__ == "__main__":
    main()
