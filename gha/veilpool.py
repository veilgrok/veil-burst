#!/usr/bin/env python3
"""veilpool: plan, run and manage pooled-memory jobs (one big machine made of many small runners).

  veilpool plan    --ram 64 --cpus 16 [--kind vm|process|llm] [--sharing private|read|write|stream] [--json]
  veilpool up      NAME --ram 8 --cpus 4 [--sharing ...] [--local]   start a job (local: emulated on this host)
  veilpool ps                                                         list jobs started from this host
  veilpool logs    NAME                                               show a job's node and home logs
  veilpool down    NAME                                               stop a job and free its memory
  veilpool request NAME --ram 64 --cpus 16 [--out DIR]                write a runner request for the Veil bot

The planner picks, from the job's RAM and CPU needs and how its memory is shared:
  * compute runners (one DSM node each, all their cores), the home placement, and extra memory-only runners;
  * the owner map (each compute runner owns the memory its threads use), the local page limit, prefetch knobs;
  * a predicted speedup over one runner from e = 1/(1 + f*t) with the measured costs below, and where it will break.
Runner defaults are GitHub-hosted standard runners: 4 cores, 16 GB (about 12 GB usable), about 0.9 ms apart, about
200 MB/s over WireGuard. Every prediction is labelled as a prediction; measured numbers come from pool.sh runs.
"""
import argparse, json, math, os, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__)); STATE = os.path.expanduser("~/.veilpool")
RUNNER = {"cpus": 4, "ram_gb": 16, "usable_gb": 12, "rtt_ms": 0.9, "link_mbps": 200}
# Per-sharing-class stall rate f (round trips per second per node), calibrated from the 2-node emulated dsmn runs
# (0.9 ms, 200 MB/s): e = speedup/2, f = (1/e - 1)/t. cpu 1.95x, stream 1.85x, gather 1.58x (32 MiB table, ~4 s run),
# shared 0.75x (1 atomic in 1000 ops). Runner measurements from pool.sh replace these as they come in.
CLASS = {
    "private": dict(desc="threads touch mostly their own memory (CPU-bound, per-thread buffers)", f_per_s=28.0),
    "stream": dict(desc="each thread streams over its own slice (memory-bandwidth-bound)", f_per_s=90.0),
    "read": dict(desc="all threads read a shared read-mostly set (tables, weights, code)", f_per_s=295.0),
    "write": dict(desc="threads write shared data (locks, counters, shared queues)", f_per_s=1850.0),
}

def plan(ram_gb, cpus, kind="vm", sharing="private", runner=RUNNER, shared_gb=0.0):
    r = dict(runner); per_node_cores = r["cpus"]; usable = r["usable_gb"]
    compute = max(1, math.ceil(cpus / per_node_cores))
    local_cap = compute * usable * 0.8  # 20% of each compute runner kept for the OS, the shim and the page cache
    memory_runners = 0 if ram_gb <= local_cap else math.ceil((ram_gb - local_cap) / (usable * 0.9))
    home = "runner 0 (with node 0)" if memory_runners == 0 else "memory runner m0 (the backing store), mirror on m1 or node 0"
    t = r["rtt_ms"] / 1000.0; f = CLASS[sharing]["f_per_s"]
    eff_remote = 1.0 / (1.0 + f * t * 1.0)  # each remote event stalls one core for one round trip
    if sharing == "read" and shared_gb: eff_remote = min(eff_remote, 1.0)  # one-time pull: amortised, see notes
    speedup = compute * eff_remote if compute > 1 else 1.0
    local_pages = int(usable * 0.8 * (1 << 30) / 4096)
    knobs = dict(VEIL_DSM_OWNMAP="per-node slices (guest NUMA node k = runner k)", VEIL_DSM_LEASE=8192, VEIL_DSM_RA=1024,
                 VEIL_DSM_REGION=8, VEIL_DSM_CLEANACK=1)
    if memory_runners: knobs["VEIL_DSM_LOCALMAX"] = local_pages; knobs["VEIL_DSM_PREFILL"] = 0
    if sharing == "write": knobs["VEIL_DSM_HOLD_US"] = 2000
    notes = []
    if memory_runners:
        hot = local_cap
        notes.append(f"capacity job: {ram_gb} GB on {compute} compute runner(s) holding about {hot:.0f} GB locally; the rest lives on "
                     f"{memory_runners} memory runner(s). Usable when the hot set fits in {hot:.0f} GB; a full sweep of RAM is "
                     f"link-bound at about {ram_gb * 1024 / r['link_mbps']:.0f} s per pass.")
        if ram_gb > usable * 0.9:
            notes.append("today the backing store is one home process, so it must fit on one memory runner "
                         f"(about {usable * 0.9:.0f} GB); striping it over several memory runners is on the roadmap.")
    if sharing == "write":
        notes.append("write-shared data costs a round trip per handoff: expect it to be slower than one runner unless the guest "
                     "keeps per-CPU copies (per-CPU counters, sharded locks). Measured: 1 atomic in 1000 ops ran 0.72-0.77x on 2 nodes.")
    if sharing == "read":
        notes.append("read-shared data is pulled once per node (2 MiB regions after 8 misses), then read locally; "
                     "measured 1.58x on 2 nodes for a 32 MiB table, rising with longer runs.")
    if kind == "llm":
        notes.append("LLM: weights are read-shared; shard layers across runners (pipeline) for capacity and use batching for "
                     "throughput; tensor sharding only inside a runner (two all-reduces per layer cost ~2 RTT each).")
    if kind == "process" and compute > 1:
        notes.append("one process across runners: its threads are pinned per runner, memory placed by first touch and owner map.")
    return dict(job=dict(ram_gb=ram_gb, cpus=cpus, kind=kind, sharing=sharing), runner=r,
                topology=dict(compute_runners=compute, cores_per_runner=per_node_cores, memory_runners=memory_runners, home=home,
                              total_runners=compute + memory_runners),
                knobs=knobs, predicted=dict(speedup_vs_one_runner=round(speedup, 2), efficiency=round(speedup / compute, 2) if compute > 1 else 1.0,
                                            basis="prediction from e=1/(1+f*t) with f calibrated on emulated 2-node runs, not a runner measurement"),
                notes=notes)

def show(p):
    t = p["topology"]; print(f"job: {p['job']['ram_gb']} GB, {p['job']['cpus']} cpus, {p['job']['kind']}, sharing={p['job']['sharing']} ({CLASS[p['job']['sharing']]['desc']})")
    print(f"runners: {t['compute_runners']} compute x {t['cores_per_runner']} cores + {t['memory_runners']} memory-only = {t['total_runners']}; home on {t['home']}")
    print(f"predicted: {p['predicted']['speedup_vs_one_runner']}x over one runner (efficiency {p['predicted']['efficiency']}) -- {p['predicted']['basis']}")
    print("knobs: " + " ".join(f"{k}={v}" for k, v in p["knobs"].items()))
    for n in p["notes"]: print("note: " + n)

def jobdir(name): return os.path.join(STATE, name)

def up(a):
    p = plan(a.ram, a.cpus, a.kind, a.sharing); d = jobdir(a.name); os.makedirs(d, exist_ok=True)
    json.dump(p, open(os.path.join(d, "plan.json"), "w"), indent=1); show(p)
    if not a.local:
        print("\nremote runs go through the Veil bot: use `veilpool request` and post the file it writes."); return
    n = min(p["topology"]["compute_runners"], os.cpu_count() or 1); mib = int(min(a.ram * 1024, 1024))
    env = dict(os.environ, MIB=str(mib), LAG_US=str(int(RUNNER["rtt_ms"] * 500)), LAG_MBPS=str(RUNNER["link_mbps"]), SCALE=str(a.scale))
    if p["topology"]["memory_runners"]: env.update(PREFILL="0", OWN="0", CAPREMOTE="1", XENV=f"VEIL_DSM_LOCALMAX={mib * 64}")
    wl = {"private": "cpu", "stream": "stream", "read": "gather", "write": "shared"}[a.sharing] if not p["topology"]["memory_runners"] else "hotcold"
    log = open(os.path.join(d, "run.log"), "w")
    pr = subprocess.Popen(["bash", os.path.join(HERE, "localtest", "scale.sh"), "dsm", str(max(1, n)), wl], stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    json.dump(dict(pid=pr.pid, started=time.time(), nodes=n, workload=wl, emulated=True), open(os.path.join(d, "state.json"), "w"))
    print(f"\nstarted {a.name}: {n} emulated node(s) on this host, workload {wl}, pid {pr.pid}; `veilpool logs {a.name}`")

def ps(a):
    if not os.path.isdir(STATE): print("no jobs"); return
    for name in sorted(os.listdir(STATE)):
        s = os.path.join(STATE, name, "state.json"); st = json.load(open(s)) if os.path.exists(s) else {}
        alive = False
        if st.get("pid"):
            try: os.kill(st["pid"], 0); alive = True
            except OSError: pass
        print(f"{name:20s} {'running' if alive else 'stopped':8s} nodes={st.get('nodes', '-')} workload={st.get('workload', '-')}")

def logs(a):
    d = jobdir(a.name); f = os.path.join(d, "run.log"); print(open(f).read() if os.path.exists(f) else "no log")
    for g in sorted(os.listdir(os.path.join(HERE, "localtest"))):
        if g.startswith(("n_s", "home_s")) and g.endswith(".log"):
            lines = open(os.path.join(HERE, "localtest", g), errors="replace").read().splitlines()[-2:]
            for l in lines: print(f"[{g}] {l[:200]}")

def down(a):
    s = os.path.join(jobdir(a.name), "state.json")
    if os.path.exists(s):
        st = json.load(open(s))
        try: os.killpg(st["pid"], signal.SIGKILL)
        except OSError: pass
    for pat in ("dsmn", "dsmh_s", "lag"): subprocess.call(["pkill", "-9", "-x", pat], stderr=subprocess.DEVNULL)
    for f in os.listdir("/dev/shm"):
        if f.startswith("veilS"): os.remove(os.path.join("/dev/shm", f))
    print(f"stopped {a.name}")

def request(a):
    p = plan(a.ram, a.cpus, a.kind, a.sharing); t = p["topology"]; kn = t["total_runners"]
    wl = {"private": "cpu", "stream": "stream", "read": "gather", "write": "shared"}[a.sharing]
    modes = f"P KN={max(kn, 3)} NS=1,{t['compute_runners']} WL={wl} SCALE=16 GATES=1" + (" CAP=1" if t["memory_runners"] else "")
    out = os.path.join(a.out, f"REQUEST-{a.name}.md"); os.makedirs(a.out, exist_ok=True)
    with open(out, "w") as f:
        f.write(f"# Run request: {a.name}\n\nBase: veilgrok/veil-burst 9a71b48 + 0002-pooled-memory-series.patch (see README.md).\n\n")
        f.write(f"Workflow `veil-vsplit.yml`, `n` = `{json.dumps(list(range(max(kn, 3) + 2)))}`, `modes` = `{modes}`.\n\n")
        f.write("Plan (prediction, not a measurement):\n\n```\n" + json.dumps(p, indent=1) + "\n```\n\nPass: every POOL gate line bad=0 / errors=0, kill run shows the takeover.\n")
    print(out)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter); sp = ap.add_subparsers(dest="cmd", required=True)
    def common(x): x.add_argument("--ram", type=float, required=True); x.add_argument("--cpus", type=int, required=True)
    x = sp.add_parser("plan"); common(x); x.add_argument("--kind", default="vm", choices=["vm", "process", "llm"]); x.add_argument("--sharing", default="private", choices=list(CLASS)); x.add_argument("--json", action="store_true")
    x = sp.add_parser("up"); x.add_argument("name"); common(x); x.add_argument("--kind", default="process", choices=["vm", "process", "llm"]); x.add_argument("--sharing", default="private", choices=list(CLASS)); x.add_argument("--local", action="store_true"); x.add_argument("--scale", type=float, default=1)
    sp.add_parser("ps"); x = sp.add_parser("logs"); x.add_argument("name"); x = sp.add_parser("down"); x.add_argument("name")
    x = sp.add_parser("request"); x.add_argument("name"); common(x); x.add_argument("--kind", default="vm", choices=["vm", "process", "llm"]); x.add_argument("--sharing", default="private", choices=list(CLASS)); x.add_argument("--out", default="/mnt/project-files/veil-burst")
    a = ap.parse_args()
    if a.cmd == "plan":
        p = plan(a.ram, a.cpus, a.kind, a.sharing); print(json.dumps(p, indent=1)) if a.json else show(p)
    else: dict(up=up, ps=ps, logs=logs, down=down, request=request)[a.cmd](a)

if __name__ == "__main__": main()
