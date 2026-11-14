# veil-llm: one LLM sharded across runners

`vllm.c` is a self-contained Llama-architecture inference engine (int8, AVX-512 VNNI or AVX2) with
two sharding layouts:

- `pp` (pipeline): each node owns a contiguous, byte-balanced block of layers. Activations stream
  node to node, and several micro-batches can be in flight.
- `tp` (tensor): each node owns 1/N of every layer, with two all-reduces per layer.

Weights are synthetic, at real Llama shapes (`tiny`, `s1` 1.1B, `m3`, `m7` 7B, `l13`, `l34`,
`l70` 70B). Each node generates only its own shard, so there is nothing to download. Results and
analysis are in `RESULTS.md`.

## Runner request (Veil bot)

Base: veilgrok/veil-burst `9a71b48`. Apply `0002-veil-llm-sharded-inference.patch` (`git am`).
It adds new files only: `gha/llm/*` and `.github/workflows/veil-llm.yml`. No existing file
changes, so DSM behaviour, XCHECK and torture are untouched.

Runs, in order of value:

1. **Main run, 8 runners.** Dispatch `veil-llm.yml` with `relay_url=<Veil relay>`,
   `n=[0,1,2,3,4,5,6,7]`, `npeers=8`, `models=s1,m7`, `big=l34,l70`, `tag=llm-main`.
   Per runner: build, rendezvous over the relay KV (the existing `VEIL_RELAY_TOKEN` secret, only
   for exchanging private IPs), then measure links. Then, on one schedule shared by all runners:
   - one-runner baselines for s1 and m7;
   - pp and tp at 2, 4 and 8 runners with 1, 8 and 16 streams;
   - l34 (33.7 GB, needs ≥3 runners) at 4 and 8 runners, and l70 (70.5 GB) at 8 runners;
   - the kill gate.
2. **Scale-out, if 16 runners can be scheduled at once.** Same inputs with `n=[0..15]` and
   `npeers=16`. It adds 16-runner points.

Topology: all runners in one matrix job, so they share a region. Runner-to-runner traffic is
direct TCP to each runner's private IP: ports 40000–60000 for engine links, 47101 for the link
test. If direct TCP doesn't work between runners, the first `llm_link` lines will show
`"ok": false` and nothing else will run. That is a finding in itself; please post it.

Env knobs (workflow inputs): `models`, `big`, `prompt` (default 64), `gen` (default 32).
Optional runner env: `LLM_KILL_AFTER` (s, default 3), `LLM_KILL_MODEL` (default s1).

## Output to post back

Every result is one `VEIL_JSON` line in rank 0's log:

- `llm_host` (CPU, cores, RAM) and `llm_link` (RTT p50/min, MB/s): one per runner.
- `llm_result`: one per configuration, with `decode_tok_s`, `prefill_tok_s`, `step_ms` and
  `checksum`. Speedup = decode_tok_s ÷ the one-runner baseline with the same model and stream
  count. For l34 and l70 there is no one-runner baseline, because they don't fit one runner.
- `llm_killtest`: two lines, without and with a SIGKILL of rank 2 mid-decode.
  **Gate:** both checksums are equal, and `replay_mismatch == 0` and `recoveries == 1` on the
  killed run.
- `llm_node_fail`: any node crash, such as an OOM for a model that doesn't fit.

Correctness gate for every pp line: its checksum equals the one-runner checksum for the same
model and stream count.

## Reproducing the cloud emulation (one 4-core box)

```
gcc -O3 -march=native -fopenmp -o vllm vllm.c -lm
gcc -O2 -o lagproxy lagproxy.c
python3 linktest.py 0.9 200 100       # emulator fidelity (RTT, MB/s)
./sweep1.sh; ./sweep1b.sh; ./sweep2.sh # needs root for cgroups (v1 cpu + memory)
python3 analyze.py results/sweep1.jsonl
```

`sweep3.sh` runs llama.cpp RPC on the same setup. It needs a llama.cpp build with
`-DGGML_RPC=ON`, plus a GGUF from `mkgguf.py` quantized with `llama-quantize ... Q8_0`. To dry-run
the runner script on one box, start `mockrelay.py`, then launch 4 copies of `llm_runner.py`. Give
each copy `GIDX=0..3`, `NPEERS=4`, `VEIL_RELAY_URL=http://127.0.0.1:47900`, `LLM_LOCALTEST=1`,
and `LLM_BIN=/tmp/vllm-r$GIDX`.
