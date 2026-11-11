#!/usr/bin/env python3
"""Geekbench 6 only: version/mode variants to work around upload failure. No secrets."""
import json, os, re, subprocess as sp, time, signal
V = os.environ["GBV"]; MODE = os.environ.get("GBMODE", "direct"); G = int(os.environ.get("GIDX", "0"))
def sh(c, t):
    p = sp.Popen(c, shell=True, stdout=sp.PIPE, stderr=sp.STDOUT, text=True, start_new_session=True)
    try: o, _ = p.communicate(timeout=t); return p.returncode, o
    except sp.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL); return 124, "TIMEOUT"
lscpu = sh("lscpu", 20)[1]
model = next((l.split(":", 1)[1].strip() for l in lscpu.splitlines() if l.startswith("Model name")), "")
rc, o = sh(f"mkdir -p gb && curl -sSLf https://cdn.geekbench.com/Geekbench-{V}-Linux.tar.gz | tar xz -C gb --strip-components=1", 300)
t = time.time()
if MODE == "docker":
    rc, out = sh("docker run --rm -v $PWD/gb:/gb -w /gb ubuntu:22.04 bash -c 'apt-get update -qq >/dev/null && apt-get install -y -qq ca-certificates >/dev/null; ./geekbench6 --cpu'", 1500)
else:
    rc, out = sh("cd gb && ./geekbench6 --cpu", 1500)
url = re.findall(r"https://browser\.geekbench\.com/v6/cpu/\d+", out)
sc = re.search(r"Single-Core Score\s+(\d+)", out); mc = re.search(r"Multi-Core Score\s+(\d+)", out)
print("FINAL_JSON " + json.dumps({"kind": "gb6", "gidx": G, "ver": V, "mode": MODE, "cpu_model": model, "rc": rc, "wall_s": round(time.time() - t, 1),
      "url": url[0] if url else None, "single": int(sc.group(1)) if sc else None, "multi": int(mc.group(1)) if mc else None,
      "tail": out[-300:] if not url else ""}), flush=True)
