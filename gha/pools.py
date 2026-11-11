#!/usr/bin/env python3
"""Pluggable overflow pools for the Upstash dispatcher.

Local tests use SimulatedPool only. Remote pools (gha, circleci, buildkite,
blacksmith, daytona, e2b, upstash_box) refuse to fire unless allow_remote=True,
so a casual import cannot burn credits or dispatch jobs. Daytona and E2B use a
control-plane pull model (exec through the provider API) unless a test passes
fake=True.
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


DEFAULT_ORDER = ("gha", "circleci", "buildkite", "blacksmith", "daytona", "e2b", "upstash_box")

# Published concurrency ceilings used as default caps. Measured GHA ubuntu
# concurrency was 5; mixed labels reached 8. CircleCI Free is 30, but we
# keep the default cap modest so a first overflow burst stays under the
# auto-flag triggers noted in the sweep report.
DEFAULT_CAPS = {
    "gha": 5,
    "circleci": 10,
    "buildkite": 10,
    "blacksmith": 8,
    "daytona": 5,
    "e2b": 10,
    "upstash_box": 1,
}


@dataclass
class WorkerHandle:
    pool: str
    name: str
    proc: subprocess.Popen | None = None
    listen: str = ""
    remote_id: str | None = None
    meta: dict[str, Any] = field(default_factory=dict)

    def poll(self) -> int | None:
        if self.proc is None:
            return None
        return self.proc.poll()

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None


class Pool(ABC):
    name: str
    cap: int
    allow_remote: bool = False

    @abstractmethod
    def spawn(
        self,
        count: int,
        *,
        controller: str,
        work_ms: int,
        name_prefix: str,
    ) -> list[WorkerHandle]:
        raise NotImplementedError

    def teardown(self, handles: list[WorkerHandle], *, timeout: float = 5.0) -> None:
        for handle in handles:
            if handle.proc is None or handle.proc.poll() is not None:
                continue
            handle.proc.terminate()
        import time

        deadline = time.time() + timeout
        for handle in handles:
            if handle.proc is None:
                continue
            remaining = max(0.05, deadline - time.time())
            try:
                handle.proc.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                handle.proc.kill()
                try:
                    handle.proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass


def allocate(n_workers: int, pools: list[Pool]) -> dict[str, int]:
    """Fill pools in order up to each cap. Leftover demand is left unassigned."""
    assigned: dict[str, int] = {p.name: 0 for p in pools}
    remaining = max(0, int(n_workers))
    for pool in pools:
        take = min(remaining, max(0, int(pool.cap)))
        assigned[pool.name] = take
        remaining -= take
        if remaining == 0:
            break
    assigned["_unassigned"] = remaining
    return assigned


def _free_udp_port() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = int(sock.getsockname()[1])
    sock.close()
    return port


class SimulatedPool(Pool):
    """Local worker.py processes. Used for tests and dry runs of every pool name."""

    def __init__(self, name: str, cap: int, worker_script: Path | None = None) -> None:
        self.name = name
        self.cap = cap
        self.allow_remote = False
        self.worker_script = Path(worker_script or Path(__file__).resolve().parent / "worker.py")

    def spawn(
        self,
        count: int,
        *,
        controller: str,
        work_ms: int,
        name_prefix: str,
    ) -> list[WorkerHandle]:
        if count <= 0:
            return []
        if count > self.cap:
            raise ValueError(f"{self.name} cap is {self.cap}, asked for {count}")
        handles: list[WorkerHandle] = []
        python = sys.executable
        for i in range(count):
            name = f"{name_prefix}-{self.name}-{i}"
            port = _free_udp_port()
            listen = f"127.0.0.1:{port}"
            env = os.environ.copy()
            env["WORKER_POOL"] = self.name
            # Do not copy or print secret files. The sim worker needs none.
            proc = subprocess.Popen(
                [
                    python,
                    str(self.worker_script),
                    "--listen",
                    listen,
                    "--controller",
                    controller,
                    "--name",
                    name,
                    "--work-ms",
                    str(work_ms),
                ],
                cwd=str(self.worker_script.parent),
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            handles.append(WorkerHandle(pool=self.name, name=name, proc=proc, listen=listen))
        return handles


class RemotePool(Pool):
    """A real CI/box pool. spawn() is a no-op unless allow_remote is set."""

    def __init__(self, name: str, cap: int, *, allow_remote: bool = False, how: str = "") -> None:
        self.name = name
        self.cap = cap
        self.allow_remote = allow_remote
        self.how = how or f"trigger the {name} probe/bridge job"

    def spawn(
        self,
        count: int,
        *,
        controller: str,
        work_ms: int,
        name_prefix: str,
    ) -> list[WorkerHandle]:
        if count <= 0:
            return []
        if not self.allow_remote:
            raise RuntimeError(
                f"{self.name} remote spawn is disabled. "
                "Local tests must use SimulatedPool. Pass allow_remote=True only after "
                "the account, token, and (for Blacksmith) the GitHub App are in place."
            )
        raise RuntimeError(
            f"{self.name} remote spawn is not enabled in this harness yet "
            f"({self.how}). Use the matching trigger script by hand after signup."
        )



# Default image and shape for the Daytona pull pool. Tier 1 is 10 vCPU / 10 GiB
# total, so five 2 vCPU / 2 GiB sandboxes fill the published compute pool
# without overrunning memory the way official medium (2/4) snapshots would.
DAYTONA_IMAGE = "daytonaio/sandbox:0.11.0"
DAYTONA_REMOTE_DIR = "/tmp/veil-equiv"
DAYTONA_KEY_FILE = Path("/home/box/agent-data/secrets/daytona-veilgrok.key")

E2B_KEY_FILE = Path("/home/box/agent-data/secrets/e2b-veilgrok.key")
E2B_REMOTE_DIR = "/tmp/veil-equiv"


def _read_key(path: Path, env_name: str) -> str:
    env = os.environ.get(env_name) or ""
    if env.strip():
        return env.strip()
    file_env = os.environ.get(env_name + "_FILE") or ""
    candidate = Path(file_env) if file_env else path
    return candidate.read_text(encoding="utf-8").strip()


class DaytonaPool(Pool):
    """API-driven pull pool for Daytona sandboxes.

    The control plane creates sandboxes and execs each task through
    Daytona's process API. Tier 1/2 sandboxes have restricted egress and
    typically cannot open the Upstash WebSocket themselves, so work is
    pulled from the box rather than pushed over a bridge.
    """

    def __init__(
        self,
        cap: int | None = None,
        *,
        allow_remote: bool = False,
        fake: bool = False,
        cpu: int = 2,
        memory: int = 2,
        disk: int = 3,
        image: str = DAYTONA_IMAGE,
        auto_stop_interval: int = 5,
        ttl_minutes: int = 20,
        key_file: Path | None = None,
        worker_script: Path | None = None,
    ) -> None:
        self.name = "daytona"
        self.cap = DEFAULT_CAPS["daytona"] if cap is None else cap
        self.allow_remote = allow_remote
        self.fake = fake
        self.cpu = cpu
        self.memory = memory
        self.disk = disk
        self.image = image
        self.auto_stop_interval = auto_stop_interval
        self.ttl_minutes = ttl_minutes
        self.key_file = Path(key_file or DAYTONA_KEY_FILE)
        self.worker_script = Path(
            worker_script or Path(__file__).resolve().parent / "daytona_pull_worker.py"
        )
        self._client = None

    def _client_or_connect(self):
        if self._client is not None:
            return self._client
        from daytona import Daytona, DaytonaConfig

        key = _read_key(self.key_file, "DAYTONA_API_KEY")
        api_url = os.environ.get("DAYTONA_API_URL") or "https://app.daytona.io/api"
        target = os.environ.get("DAYTONA_TARGET") or "us"
        os.environ["DAYTONA_API_KEY"] = key
        self._client = Daytona(DaytonaConfig(api_key=key, api_url=api_url, target=target))
        return self._client

    def _prepare_sandbox(self, sandbox) -> None:
        root = Path(__file__).resolve().parent
        sandbox.fs.create_folder(DAYTONA_REMOTE_DIR, "755")
        sandbox.fs.upload_file(str(root / "oracle.py"), f"{DAYTONA_REMOTE_DIR}/oracle.py")
        sandbox.fs.upload_file(str(root / "daytona_task.py"), f"{DAYTONA_REMOTE_DIR}/daytona_task.py")

    def spawn(
        self,
        count: int,
        *,
        controller: str,
        work_ms: int,
        name_prefix: str,
    ) -> list[WorkerHandle]:
        if count <= 0:
            return []
        if count > self.cap:
            raise ValueError(f"{self.name} cap is {self.cap}, asked for {count}")
        if not self.allow_remote:
            raise RuntimeError(
                "daytona remote spawn is disabled. Local tests must use SimulatedPool "
                "or DaytonaPool(fake=True, allow_remote=True). Pass allow_remote=True "
                "only after the account and API key are in place."
            )
        if self.fake:
            return self._spawn_workers(count, controller=controller, work_ms=work_ms, name_prefix=name_prefix, sandbox_ids=[""] * count, fake=True)
        client = self._client_or_connect()
        from daytona import CreateSandboxFromImageParams, Resources

        sandboxes = []
        try:
            for i in range(count):
                name = f"{name_prefix}-daytona-{i}"
                params = CreateSandboxFromImageParams(
                    name=name,
                    image=self.image,
                    resources=Resources(cpu=self.cpu, memory=self.memory, disk=self.disk),
                    labels={"veil": "runner-probe", "pool": "daytona"},
                    ephemeral=True,
                    auto_stop_interval=self.auto_stop_interval,
                    ttl_minutes=self.ttl_minutes,
                    target=os.environ.get("DAYTONA_TARGET") or "us",
                    language="python",
                )
                sandbox = client.create(params, timeout=180)
                self._prepare_sandbox(sandbox)
                sandboxes.append(sandbox)
            return self._spawn_workers(
                count,
                controller=controller,
                work_ms=work_ms,
                name_prefix=name_prefix,
                sandbox_ids=[sb.id for sb in sandboxes],
                fake=False,
                sandboxes=sandboxes,
            )
        except Exception:
            for sb in sandboxes:
                try:
                    client.delete(sb, timeout=60, wait=True)
                except Exception:
                    try:
                        sb.delete(timeout=60, wait=True)
                    except Exception:
                        pass
            raise

    def _spawn_workers(
        self,
        count: int,
        *,
        controller: str,
        work_ms: int,
        name_prefix: str,
        sandbox_ids: list[str],
        fake: bool,
        sandboxes: list | None = None,
    ) -> list[WorkerHandle]:
        handles: list[WorkerHandle] = []
        python = sys.executable
        env = os.environ.copy()
        env["WORKER_POOL"] = self.name
        if not fake:
            env["DAYTONA_API_KEY"] = _read_key(self.key_file, "DAYTONA_API_KEY")
            env["DAYTONA_API_URL"] = os.environ.get("DAYTONA_API_URL") or "https://app.daytona.io/api"
            env["DAYTONA_TARGET"] = os.environ.get("DAYTONA_TARGET") or "us"
        for i in range(count):
            name = f"{name_prefix}-{self.name}-{i}"
            port = _free_udp_port()
            listen = f"127.0.0.1:{port}"
            cmd = [
                python,
                str(self.worker_script),
                "--listen",
                listen,
                "--controller",
                controller,
                "--name",
                name,
                "--work-ms",
                str(work_ms),
            ]
            if fake:
                cmd.append("--fake")
            elif sandbox_ids[i]:
                cmd.extend(["--sandbox-id", sandbox_ids[i]])
            proc = subprocess.Popen(
                cmd,
                cwd=str(self.worker_script.parent),
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            meta: dict = {"fake": fake}
            if sandboxes is not None:
                meta["sandbox"] = sandboxes[i]
                meta["sandbox_id"] = sandbox_ids[i]
                meta["cpu"] = self.cpu
                meta["memory"] = self.memory
                meta["disk"] = self.disk
            elif sandbox_ids[i]:
                meta["sandbox_id"] = sandbox_ids[i]
            handles.append(WorkerHandle(pool=self.name, name=name, proc=proc, listen=listen, remote_id=sandbox_ids[i] or None, meta=meta))
        return handles

    def teardown(self, handles: list[WorkerHandle], *, timeout: float = 5.0) -> None:
        client = self._client
        for handle in handles:
            sb = handle.meta.get("sandbox")
            sid = handle.meta.get("sandbox_id") or handle.remote_id
            if self.fake:
                continue
            try:
                if sb is not None:
                    if client is not None:
                        client.delete(sb, timeout=max(30, timeout), wait=True)
                    else:
                        sb.delete(timeout=max(30, timeout), wait=True)
                elif sid and client is not None:
                    got = client.get(sid)
                    client.delete(got, timeout=max(30, timeout), wait=True)
            except Exception:
                pass
        super().teardown(handles, timeout=timeout)


class E2BPool(Pool):
    """API-driven pull pool for E2B sandboxes.

    The control plane creates sandboxes and execs each task through the
    E2B SDK. Hobby sandboxes usually have internet, so a WebSocket bridge
    to Box B can be attempted separately; the dispatcher path stays a
    pull model so it works even when outbound WS is blocked.
    """

    def __init__(
        self,
        cap: int | None = None,
        *,
        allow_remote: bool = False,
        fake: bool = False,
        cpu: int = 2,
        memory: int = 2,
        timeout_s: int = 300,
        key_file: Path | None = None,
        worker_script: Path | None = None,
    ) -> None:
        self.name = "e2b"
        self.cap = DEFAULT_CAPS["e2b"] if cap is None else cap
        self.allow_remote = allow_remote
        self.fake = fake
        self.cpu = cpu
        self.memory = memory
        self.timeout_s = timeout_s
        self.key_file = Path(key_file or E2B_KEY_FILE)
        # Reuse the Daytona pull worker with a thin E2B exec backend via env.
        self.worker_script = Path(
            worker_script or Path(__file__).resolve().parent / "e2b_pull_worker.py"
        )
        self._sandboxes: list = []

    def spawn(
        self,
        count: int,
        *,
        controller: str,
        work_ms: int,
        name_prefix: str,
    ) -> list[WorkerHandle]:
        if count <= 0:
            return []
        if count > self.cap:
            raise ValueError(f"{self.name} cap is {self.cap}, asked for {count}")
        if not self.allow_remote:
            raise RuntimeError(
                "e2b remote spawn is disabled. Local tests must use SimulatedPool "
                "or E2BPool(fake=True, allow_remote=True). Pass allow_remote=True "
                "only after the account and API key are in place."
            )
        if self.fake:
            return self._spawn_workers(count, controller=controller, work_ms=work_ms, name_prefix=name_prefix, sandbox_ids=[""] * count, fake=True)
        from e2b import Sandbox

        key = _read_key(self.key_file, "E2B_API_KEY")
        os.environ["E2B_API_KEY"] = key
        sandboxes = []
        try:
            for i in range(count):
                sb = Sandbox.create(
                    api_key=key,
                    timeout=self.timeout_s,
                    metadata={"veil": "runner-probe", "pool": "e2b", "name": f"{name_prefix}-e2b-{i}"},
                )
                self._prepare_sandbox(sb)
                sandboxes.append(sb)
            self._sandboxes.extend(sandboxes)
            return self._spawn_workers(
                count,
                controller=controller,
                work_ms=work_ms,
                name_prefix=name_prefix,
                sandbox_ids=[getattr(sb, "sandbox_id", None) or getattr(sb, "id", "") for sb in sandboxes],
                fake=False,
                sandboxes=sandboxes,
            )
        except Exception:
            for sb in sandboxes:
                try:
                    sb.kill()
                except Exception:
                    pass
            raise

    def _prepare_sandbox(self, sandbox) -> None:
        root = Path(__file__).resolve().parent
        sandbox.files.make_dir(E2B_REMOTE_DIR)
        sandbox.files.write(f"{E2B_REMOTE_DIR}/oracle.py", (root / "oracle.py").read_text(encoding="utf-8"))
        # Task helper is provider-agnostic (stdlib + oracle).
        sandbox.files.write(f"{E2B_REMOTE_DIR}/daytona_task.py", (root / "daytona_task.py").read_text(encoding="utf-8"))

    def _spawn_workers(
        self,
        count: int,
        *,
        controller: str,
        work_ms: int,
        name_prefix: str,
        sandbox_ids: list[str],
        fake: bool,
        sandboxes: list | None = None,
    ) -> list[WorkerHandle]:
        handles: list[WorkerHandle] = []
        python = sys.executable
        env = os.environ.copy()
        env["WORKER_POOL"] = self.name
        if not fake:
            env["E2B_API_KEY"] = _read_key(self.key_file, "E2B_API_KEY")
        for i in range(count):
            name = f"{name_prefix}-{self.name}-{i}"
            port = _free_udp_port()
            listen = f"127.0.0.1:{port}"
            cmd = [
                python,
                str(self.worker_script),
                "--listen",
                listen,
                "--controller",
                controller,
                "--name",
                name,
                "--work-ms",
                str(work_ms),
            ]
            if fake:
                cmd.append("--fake")
            elif sandbox_ids[i]:
                cmd.extend(["--sandbox-id", sandbox_ids[i]])
            proc = subprocess.Popen(
                cmd,
                cwd=str(self.worker_script.parent),
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            meta: dict = {"fake": fake}
            if sandboxes is not None:
                meta["sandbox"] = sandboxes[i]
                meta["sandbox_id"] = sandbox_ids[i]
                meta["cpu"] = self.cpu
                meta["memory"] = self.memory
            elif sandbox_ids[i]:
                meta["sandbox_id"] = sandbox_ids[i]
            handles.append(WorkerHandle(pool=self.name, name=name, proc=proc, listen=listen, remote_id=sandbox_ids[i] or None, meta=meta))
        return handles

    def teardown(self, handles: list[WorkerHandle], *, timeout: float = 5.0) -> None:
        for handle in handles:
            if self.fake:
                continue
            sb = handle.meta.get("sandbox")
            if sb is not None:
                try:
                    sb.kill()
                except Exception:
                    pass
        super().teardown(handles, timeout=timeout)




def default_remote_pools(*, allow_remote: bool = False) -> list[Pool]:
    return [
        RemotePool("gha", DEFAULT_CAPS["gha"], allow_remote=allow_remote, how="gh workflow run probe.yml or bridge.yml"),
        RemotePool("circleci", DEFAULT_CAPS["circleci"], allow_remote=allow_remote, how="python3 circleci_trigger.py --confirm"),
        RemotePool("buildkite", DEFAULT_CAPS["buildkite"], allow_remote=allow_remote, how="python3 buildkite_trigger.py --confirm"),
        RemotePool("blacksmith", DEFAULT_CAPS["blacksmith"], allow_remote=allow_remote, how="gh workflow run blacksmith-probe.yml after the app is installed"),
        DaytonaPool(DEFAULT_CAPS["daytona"], allow_remote=allow_remote),
        E2BPool(DEFAULT_CAPS["e2b"], allow_remote=allow_remote),
        RemotePool("upstash_box", DEFAULT_CAPS["upstash_box"], allow_remote=allow_remote, how="start the existing box worker through equiv/box_setup.mjs; do not enable keepAlive"),
    ]


def simulated_pools(
    caps: dict[str, int] | None = None,
    worker_script: Path | None = None,
) -> list[SimulatedPool]:
    caps = caps or DEFAULT_CAPS
    return [SimulatedPool(name, caps.get(name, 1), worker_script=worker_script) for name in DEFAULT_ORDER]
