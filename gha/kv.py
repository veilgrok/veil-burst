#!/usr/bin/env python3
"""kv.py put KEY VALUE | kv.py wait PREFIX N  (Veil relay KV rendezvous; token from env, never printed)
   kv.py stun PORT -> prints 'ip port' mapped for local UDP PORT"""
import http.client, json, os, select, socket, struct, sys, time, urllib.parse
u = urllib.parse.urlparse(os.environ["VEIL_RELAY_URL"]); H = {"X-Token": os.environ["VEIL_RELAY_TOKEN"], "X-Runner": "kv"}
def req(m, path, body=None):
    c = http.client.HTTPSConnection(u.netloc, timeout=30); c.request(m, path, body=body, headers=H); r = c.getresponse().read(); c.close(); return r
a = sys.argv
if a[1] == "put": req("POST", "/kv/" + a[2], a[3].encode())
elif a[1] == "wait":
    end = time.time() + float(a[4] if len(a) > 4 else 600)
    while time.time() < end:
        d = json.loads(req("GET", "/kv/" + a[2]))
        if len(d) >= int(a[3]): print(json.dumps(d)); sys.exit(0)
        time.sleep(2)
    sys.exit(1)
elif a[1] == "stun":
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("0.0.0.0", int(a[2])))
    tx = os.urandom(12); s.sendto(struct.pack("!HHI", 1, 0, 0x2112A442) + tx, ("stun.l.google.com", 19302))
    if select.select([s], [], [], 3)[0]:
        d, _ = s.recvfrom(2048); i = 20
        while i < len(d):
            t, l = struct.unpack("!HH", d[i:i + 4]); v = d[i + 4:i + 4 + l]
            if t == 0x0020:
                print(socket.inet_ntoa(struct.pack("!I", struct.unpack("!I", v[4:8])[0] ^ 0x2112A442)), struct.unpack("!H", v[2:4])[0] ^ 0x2112); break
            i += 4 + l + (-l % 4)
