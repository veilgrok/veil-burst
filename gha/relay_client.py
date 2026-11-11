#!/usr/bin/env python3
"""Runner-side UDP<->HTTPS relay so unmodified Veil worker.py can reach the
Veil dispatcher over an outbound-only HTTPS tunnel. Token from env; never printed."""
import http.client, json, os, socket, sys, threading, time, urllib.parse
url = urllib.parse.urlparse(os.environ["VEIL_RELAY_URL"]); tok = os.environ["VEIL_RELAY_TOKEN"]
runner = os.environ["RELAY_RUNNER"]; port = int(os.environ.get("RELAY_PORT", "7700"))
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.bind(("127.0.0.1", port))
addrs = {}; buf = []; hellos = {}; lock = threading.Condition(); stats = {"up": 0, "down": 0, "err": 0}
stop = False
def conn(): return (http.client.HTTPSConnection if url.scheme == "https" else http.client.HTTPConnection)(url.netloc, timeout=40)
def req(c, method, path, body=None):
    for attempt in range(6):
        try:
            c.request(method, path, body=body, headers={"X-Token": tok, "X-Runner": runner, "Content-Type": "application/json"})
            r = c.getresponse(); data = r.read()
            if r.status == 200: return c, data
            stats["err"] += 1
        except Exception:
            stats["err"] += 1
        try: c.close()
        except Exception: pass
        time.sleep(min(2 ** attempt * 0.2, 3)); c = conn()
    return c, b'{"pkts":[]}'
def rx():
    while not stop:
        d, a = sock.recvfrom(65535)
        try: name = json.loads(d)["name"]
        except Exception: continue
        addrs[name] = a
        with lock:
            if b'"t":"hello"' in d: hellos[name] = d.decode()
            else: buf.append({"w": name, "d": d.decode()}); lock.notify()
def up():
    c = conn(); last = 0
    while not stop:
        with lock:
            lock.wait(timeout=0.5)
            pk = buf[:]; buf.clear()
            if pk or time.time() - last > 0.5:
                pk += [{"w": n, "d": d} for n, d in hellos.items()]; hellos.clear()
        if pk:
            c, _ = req(c, "POST", "/up", json.dumps({"pkts": pk})); stats["up"] += len(pk); last = time.time()
def down():
    c = conn()
    while not stop:
        c, data = req(c, "GET", "/down?wait=15")
        for p in json.loads(data).get("pkts", []):
            a = addrs.get(p["w"])
            if a: sock.sendto(p["d"].encode(), a); stats["down"] += 1
for f in (rx, up, down): threading.Thread(target=f, daemon=True).start()
print("RELAY_READY", runner, flush=True)
while True:
    time.sleep(10); print("RELAY_STATS", json.dumps(stats), flush=True)
