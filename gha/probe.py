#!/usr/bin/env python3
"""Runner -> relay RTT and bandwidth probe (HTTPS keep-alive). Token from env; never printed."""
import http.client, json, os, time, urllib.parse
u = urllib.parse.urlparse(os.environ["VEIL_RELAY_URL"]); H = {"X-Token": os.environ["VEIL_RELAY_TOKEN"], "X-Runner": "probe"}
c = http.client.HTTPSConnection(u.netloc, timeout=60)
def req(m, path, body=None):
    c.request(m, path, body=body, headers=H); r = c.getresponse(); return r.read()
req("GET", "/health"); rtt = []
for _ in range(50):
    t = time.perf_counter(); req("GET", "/health"); rtt.append(time.perf_counter() - t)
rtt.sort(); out = {"kind": "probe", "gidx": int(os.environ["GIDX"]), "rtt_ms_p50": rtt[25] * 1000, "rtt_ms_p95": rtt[47] * 1000, "rtt_ms_min": rtt[0] * 1000}
for n in [4096, 1 << 20, 16 << 20]:
    t = time.perf_counter(); b = req("GET", f"/blob?n={n}"); dt = time.perf_counter() - t
    out[f"down_{n}_s"] = dt; out[f"down_{n}_MBps"] = len(b) / dt / 1e6
    t = time.perf_counter(); req("POST", "/blob", b"x" * n); dt = time.perf_counter() - t
    out[f"up_{n}_s"] = dt; out[f"up_{n}_MBps"] = n / dt / 1e6
out["t"] = time.time(); print("VEIL_JSON " + json.dumps(out), flush=True)
