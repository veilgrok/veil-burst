#!/usr/bin/env python3
"""Prime-counting UDP worker for the labeled equivalent workload."""

from __future__ import annotations

import argparse
import os
import hashlib
import json
import socket
import time

from oracle import count_primes


def work_loop(payload: bytes, work_ms: int) -> None:
    if work_ms <= 0:
        return
    deadline = time.perf_counter() + (work_ms / 1000.0)
    block = payload
    n = 0
    while time.perf_counter() < deadline:
        block = hashlib.sha256(block).digest()
        n += 1
        if n % 64 == 0 and time.perf_counter() >= deadline:
            break


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--listen", default="127.0.0.1:27700")
    p.add_argument("--controller", default="127.0.0.1:7700")
    p.add_argument("--name", required=True)
    p.add_argument("--work-ms", type=int, default=120)
    args = p.parse_args()

    listen_host, listen_port = args.listen.rsplit(":", 1)
    ctrl_host, ctrl_port = args.controller.rsplit(":", 1)
    dest = (ctrl_host, int(ctrl_port))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((listen_host, int(listen_port)))
    sock.settimeout(0.5)

    def send(msg: dict) -> None:
        sock.sendto(json.dumps(msg, separators=(",", ":")).encode("utf-8"), dest)

    last_hello = 0.0
    while True:
        now = time.perf_counter()
        if now - last_hello >= 0.4:
            hello = {"t": "hello", "name": args.name, "ts": time.time()}
            pool = os.environ.get("WORKER_POOL") or ""
            if pool:
                hello["pool"] = pool
            if args.name:
                # Renew any in-flight lease by identifying this worker.
                hello["lease"] = True
            send(hello)
            last_hello = now
        try:
            data, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue
        try:
            msg = json.loads(data.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            continue
        typ = msg.get("t")
        if typ in ("bye", "drain"):
            # drain = queue is empty; the worker exits itself rather than idling.
            send({"t": "bye", "name": args.name, "reason": typ})
            return
        if typ == "task":
            lo = int(msg["lo"])
            hi = int(msg["hi"])
            tid = int(msg["id"])
            lease_id = msg.get("lease_id")
            count = count_primes(lo, hi)
            work_loop(f"{args.name}:{tid}:{lo}:{hi}:{count}".encode(), args.work_ms)
            send({"t": "result", "id": tid, "name": args.name, "count": count, "lease_id": lease_id})
            last_hello = 0.0


if __name__ == "__main__":
    main()
