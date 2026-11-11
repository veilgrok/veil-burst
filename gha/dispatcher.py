#!/usr/bin/env python3
"""Pluggable UDP task dispatcher with leases, overflow pools, and drain-on-empty.

Compatible with equiv/worker.py (hello / task / result / bye), plus:
  * lease_id on each task assignment
  * drain, so workers self-exit when the queue is empty
  * reassign-on-drop, same idea as the mid-run SIGKILL path in the bridge test

Remote pools are not triggered from here. Local use is SimulatedPool only.
"""

from __future__ import annotations

import argparse
import json
import socket
import time
import uuid
from typing import Any

from pools import Pool, WorkerHandle, allocate


class Dispatcher:
    def __init__(
        self,
        listen: str,
        intervals: list[tuple[int, int]],
        expected: list[int],
        *,
        heartbeat_timeout: float = 4.0,
        lease_seconds: float = 8.0,
        verify_frac: float = 0.2,
        timeout: float = 120.0,
        ready_for: list[str] | None = None,
        ready_timeout: float = 30.0,
        status_path: str = "",
    ) -> None:
        if len(expected) != len(intervals):
            raise ValueError("expected length does not match intervals")
        host, port = listen.rsplit(":", 1)
        self.listen = (host, int(port))
        self.intervals = intervals
        self.expected = expected
        self.heartbeat_timeout = heartbeat_timeout
        self.lease_seconds = lease_seconds
        self.verify_frac = verify_frac
        self.timeout = timeout
        self.required = list(ready_for or [])
        self.ready_timeout = ready_timeout
        self.status_path = status_path

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(self.listen)
        self.sock.settimeout(0.2)
        self.bound = f"{self.sock.getsockname()[0]}:{self.sock.getsockname()[1]}"

        self.workers: dict[str, dict[str, Any]] = {}
        self.pending: list[int] = list(range(len(intervals)))
        self.inflight: dict[int, dict[str, Any]] = {}
        self.results: dict[int, dict[str, Any]] = {}
        self.verify_queue: list[int] = []
        self.verifies_done = 0
        self.mismatches = 0
        self.retries = 0
        self.events: list[dict[str, Any]] = []
        self.started = False
        self.start = 0.0
        self.draining = False
        self.closed = False

    def log_event(self, kind: str, **kw: Any) -> None:
        elapsed = (time.perf_counter() - self.start) if self.started else 0.0
        self.events.append({"t": time.time(), "elapsed": elapsed, "kind": kind, **kw})

    def send(self, addr: tuple[str, int], msg: dict[str, Any]) -> None:
        self.sock.sendto(json.dumps(msg, separators=(",", ":")).encode("utf-8"), addr)

    def write_status(self) -> None:
        if not self.status_path:
            return
        payload = {
            "started": self.started,
            "draining": self.draining,
            "workers": {
                name: {
                    "pool": w.get("pool") or "",
                    "completed": w["completed"],
                    "busy": w["busy"],
                    "alive": (time.perf_counter() - w["last"]) < self.heartbeat_timeout,
                    "last_ago": time.perf_counter() - w["last"],
                    "exited": w.get("exited", False),
                }
                for name, w in self.workers.items()
            },
            "pending": len(self.pending),
            "inflight": len(self.inflight),
            "done": len(self.results),
            "retries": self.retries,
            "mismatches": self.mismatches,
        }
        with open(self.status_path, "w", encoding="utf-8") as f:
            json.dump(payload, f)

    def _idle(self) -> list[str]:
        now = time.perf_counter()
        return [
            n
            for n, w in self.workers.items()
            if w["busy"] is None
            and not w.get("exited")
            and (now - w["last"]) < self.heartbeat_timeout
        ]

    def _new_lease(self, tid: int) -> str:
        return f"{tid}:{uuid.uuid4().hex[:8]}"

    def assign(self) -> None:
        idle = self._idle()
        while self.pending and idle:
            tid = self.pending.pop(0)
            name = idle.pop(0)
            self._send_task(name, tid, verify_of=None)
        while self.verify_queue and idle:
            tid = self.verify_queue.pop(0)
            name = idle.pop(0)
            if any(m.get("verify_of") == tid for m in self.inflight.values()):
                continue
            self._send_task(name, 1_000_000 + tid, verify_of=tid)
            self.verifies_done += 1
        # Only drain once nothing is queued or in flight. Draining idle
        # workers while a lease is still outstanding leaves no one to take
        # a reassigned task after a mid-run drop.
        if self.started and not self.pending and not self.verify_queue and not self.inflight:
            self.drain_idle()

    def _send_task(self, name: str, tid: int, verify_of: int | None) -> None:
        lo, hi = self.intervals[verify_of if verify_of is not None else tid]
        lease_id = self._new_lease(tid)
        now = time.perf_counter()
        self.workers[name]["busy"] = tid
        self.inflight[tid] = {
            "name": name,
            "sent": now,
            "lease_id": lease_id,
            "lease_until": now + self.lease_seconds,
            "lo": lo,
            "hi": hi,
            "verify_of": verify_of,
        }
        self.send(
            self.workers[name]["addr"],
            {
                "t": "task",
                "id": tid,
                "lo": lo,
                "hi": hi,
                "lease_id": lease_id,
                "lease_seconds": self.lease_seconds,
            },
        )

    def drain_idle(self) -> None:
        self.draining = True
        for name, w in self.workers.items():
            if w["busy"] is not None or w.get("exited"):
                continue
            try:
                self.send(w["addr"], {"t": "drain"})
            except OSError:
                continue
            w["exited"] = True
            self.log_event("drain", name=name)

    def reassign_stale(self) -> None:
        now = time.perf_counter()
        stale: list[int] = []
        for tid, meta in list(self.inflight.items()):
            name = meta["name"]
            w = self.workers.get(name)
            silent = w is None or w.get("exited") or (now - w["last"]) >= self.heartbeat_timeout
            overdue = now >= meta.get("lease_until", meta["sent"] + self.lease_seconds)
            if silent or overdue:
                stale.append(tid)
        for tid in stale:
            meta = self.inflight.pop(tid)
            name = meta["name"]
            if name in self.workers and self.workers[name]["busy"] == tid:
                self.workers[name]["busy"] = None
            if meta.get("verify_of") is None and tid not in self.results:
                self.pending.append(tid)
                self.retries += 1
                self.log_event("reassign", id=tid, from_worker=name, lease_id=meta.get("lease_id"))
            elif meta.get("verify_of") is not None:
                orig = meta["verify_of"]
                if orig not in self.verify_queue:
                    self.verify_queue.append(orig)
        if stale:
            self.assign()

    def maybe_verify(self, tid: int, from_name: str) -> None:
        if self.verify_frac <= 0:
            return
        if (tid % 5) != 0:
            return
        if tid in self.verify_queue or any(m.get("verify_of") == tid for m in self.inflight.values()):
            return
        if not any(n != from_name for n in self.workers):
            return
        self.verify_queue.append(tid)

    def _handle(self, msg: dict[str, Any], addr: tuple[str, int]) -> None:
        typ = msg.get("t")
        name = str(msg.get("name") or "")
        if typ == "hello" and name:
            if name not in self.workers:
                self.workers[name] = {
                    "addr": addr,
                    "last": time.perf_counter(),
                    "busy": None,
                    "completed": 0,
                    "pool": msg.get("pool") or "",
                    "exited": False,
                }
                self.log_event("hello", name=name, pool=msg.get("pool") or "")
            else:
                w = self.workers[name]
                w["addr"] = addr
                w["last"] = time.perf_counter()
                # Renew the lease for the task this worker currently holds.
                busy = w.get("busy")
                if busy in self.inflight and self.inflight[busy]["name"] == name:
                    self.inflight[busy]["lease_until"] = time.perf_counter() + self.lease_seconds
            if self.started:
                self.assign()
        elif typ == "result" and name:
            tid = int(msg["id"])
            count = int(msg["count"])
            meta = self.inflight.get(tid)
            incoming_lease = msg.get("lease_id")
            if (
                meta
                and incoming_lease
                and meta.get("lease_id")
                and incoming_lease != meta.get("lease_id")
            ):
                # Stale result from a worker whose lease was already reassigned.
                # Leave the new inflight lease in place.
                self.log_event("stale_result", id=tid, name=name)
                return
            if name in self.workers:
                self.workers[name]["last"] = time.perf_counter()
                if self.workers[name]["busy"] == tid:
                    self.workers[name]["busy"] = None
                self.workers[name]["completed"] += 1
            meta = self.inflight.pop(tid, None)
            if meta and meta.get("verify_of") is not None:
                orig = meta["verify_of"]
                if orig in self.results and self.results[orig]["count"] != count:
                    self.mismatches += 1
                    self.log_event("mismatch", id=orig, first=self.results[orig]["count"], second=count)
            elif tid < len(self.intervals) and tid not in self.results:
                self.results[tid] = {"count": count, "name": name}
                self.maybe_verify(tid, name)
            self.assign()
        elif typ == "bye" and name and name in self.workers:
            self.workers[name]["exited"] = True
            self.workers[name]["last"] = time.perf_counter()
            self.log_event("bye", name=name)

    def _maybe_start(self, ready_deadline: float) -> bool:
        if self.started:
            return True
        if self.required:
            present = [
                n
                for n in self.required
                if n in self.workers and (time.perf_counter() - self.workers[n]["last"]) < self.heartbeat_timeout
            ]
            if set(present) >= set(self.required):
                self.started = True
                self.start = time.perf_counter()
                self.log_event("start", workers=present)
                self.assign()
                return True
            return time.perf_counter() <= ready_deadline
        if self.workers:
            self.started = True
            self.start = time.perf_counter()
            self.log_event("start", workers=list(self.workers))
            self.assign()
            return True
        return time.perf_counter() <= ready_deadline

    def _finished(self) -> bool:
        return (
            self.started
            and not self.pending
            and not self.verify_queue
            and not self.inflight
            and len(self.results) == len(self.intervals)
        )

    def pump(self, *, ready_deadline: float, run_deadline: float) -> bool:
        """Process one socket event (or a timeout). Returns False when the run is done or timed out."""
        if not self._maybe_start(ready_deadline):
            return False
        if self._finished():
            self.drain_idle()
            return False
        if time.perf_counter() >= run_deadline:
            return False
        try:
            data, addr = self.sock.recvfrom(65535)
        except socket.timeout:
            self.reassign_stale()
            self.write_status()
            return True
        try:
            msg = json.loads(data.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return True
        self._handle(msg, addr)
        self.write_status()
        self.reassign_stale()
        return not self._finished()

    def run(self) -> dict[str, Any]:
        ready_deadline = time.perf_counter() + self.ready_timeout
        # The run deadline starts after start(); until then only ready_timeout applies.
        run_deadline = time.perf_counter() + self.timeout
        try:
            while True:
                if self.started:
                    run_deadline = self.start + self.timeout
                if not self.pump(ready_deadline=ready_deadline, run_deadline=run_deadline):
                    break
        finally:
            self.drain_idle()
        return self.summary()

    def summary(self) -> dict[str, Any]:
        wall = (time.perf_counter() - self.start) if self.started else None
        counts = [self.results[i]["count"] if i in self.results else None for i in range(len(self.intervals))]
        correct = counts == self.expected
        by_worker: dict[str, int] = {}
        by_pool: dict[str, int] = {}
        for row in self.results.values():
            by_worker[row["name"]] = by_worker.get(row["name"], 0) + 1
            pool = (self.workers.get(row["name"]) or {}).get("pool") or ""
            if pool:
                by_pool[pool] = by_pool.get(pool, 0) + 1
        return {
            "started": self.started,
            "wall_seconds": wall,
            "timeout": (wall is not None and wall >= self.timeout and not correct) or (not self.started),
            "tasks": len(self.intervals),
            "completed": len(self.results),
            "correct": correct,
            "mismatches": self.mismatches,
            "retries": self.retries,
            "verifies": self.verifies_done,
            "by_worker": by_worker,
            "by_pool": by_pool,
            "workers_seen": list(self.workers),
            "workers_exited": [n for n, w in self.workers.items() if w.get("exited")],
            "events": self.events,
        }

    def close(self) -> None:
        if self.closed:
            return
        try:
            self.drain_idle()
        finally:
            self.sock.close()
            self.closed = True


def spawn_allocated(
    pools: list[Pool],
    n_workers: int,
    *,
    controller: str,
    work_ms: int,
    name_prefix: str = "w",
) -> tuple[dict[str, int], list[WorkerHandle]]:
    plan = allocate(n_workers, pools)
    handles: list[WorkerHandle] = []
    for pool in pools:
        count = int(plan.get(pool.name) or 0)
        if count:
            handles.extend(
                pool.spawn(count, controller=controller, work_ms=work_ms, name_prefix=name_prefix)
            )
    return plan, handles


def teardown_all(pools: list[Pool], handles: list[WorkerHandle], *, timeout: float = 5.0) -> None:
    # Prefer letting drain stop the processes; then force anything still running.
    by_pool: dict[str, list[WorkerHandle]] = {}
    for handle in handles:
        by_pool.setdefault(handle.pool, []).append(handle)
    for pool in pools:
        pool.teardown(by_pool.get(pool.name, []), timeout=timeout)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--listen", default="127.0.0.1:0")
    p.add_argument("--intervals", required=True, help="JSON list of [lo, hi] pairs")
    p.add_argument("--expected", required=True, help="JSON list of integer counts")
    p.add_argument("--verify-frac", type=float, default=0.2)
    p.add_argument("--timeout", type=float, default=120)
    p.add_argument("--ready-for", default="", help="comma-separated worker names required before starting")
    p.add_argument("--ready-timeout", type=float, default=30)
    p.add_argument("--heartbeat-timeout", type=float, default=4.0)
    p.add_argument("--lease-seconds", type=float, default=8.0)
    p.add_argument("--status-path", default="")
    p.add_argument("--out", required=True)
    args = p.parse_args()

    intervals = [tuple(x) for x in json.loads(args.intervals)]
    expected = json.loads(args.expected)
    disp = Dispatcher(
        args.listen,
        intervals,
        expected,
        heartbeat_timeout=args.heartbeat_timeout,
        lease_seconds=args.lease_seconds,
        verify_frac=args.verify_frac,
        timeout=args.timeout,
        ready_for=[n for n in args.ready_for.split(",") if n],
        ready_timeout=args.ready_timeout,
        status_path=args.status_path,
    )
    try:
        print(f"dispatcher listening on {disp.bound}", flush=True)
        out = disp.run()
    finally:
        disp.close()
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
    print(json.dumps({k: v for k, v in out.items() if k != "events"}, sort_keys=True))
    if not out.get("correct") or not out.get("started"):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
