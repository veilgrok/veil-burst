#!/usr/bin/env python3
"""Local stand-in for the Veil relay KV (POST /kv/<key>, GET /kv/<prefix> -> {key: value}), for testing llm_runner.py."""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
KV = {}
class Hd(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); KV[self.path[4:]] = self.rfile.read(n).decode()
        self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
    def do_GET(self):
        pre = self.path[4:]; b = json.dumps({k: v for k, v in KV.items() if k.startswith(pre)}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
ThreadingHTTPServer(("127.0.0.1", 47900), Hd).serve_forever()
