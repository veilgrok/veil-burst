# Sharded LLM inference across small machines: measured scaling (cloud emulation)

Date: 2026-11-14. Engine: `gha/llm/vllm.c` (veil-llm). All numbers are measured end to end on one
wall clock by a single driver. Nothing is summed across nodes. Speedup = cluster throughput ÷
one-node throughput for the same job. Runner numbers are still pending: see "Runner request".

## Setup and how far to trust it

- **Engine.** Llama architecture: RMSNorm, GQA attention with RoPE, fp16 KV cache, SwiGLU, int8
  weights with per-row scales, and int8 activations in 128-wide blocks. Kernels: AVX-512 VNNI, plus
  an AVX2 path for runners without AVX-512. The AVX2 path gives bit-identical output.
- **Speed check against llama.cpp** (same 1.1B shape, Q8_0, this box). Decode: 10.5 vs 9.5 tok/s
  on 1 core, and 32.3 vs 32.8 tok/s on 4 cores. Prefill is 2–3x slower than llama.cpp, so prefill
  speedups below are flattering. Decode is the headline metric.
- **Weights are synthetic** (Hugging Face is blocked from this box). They use real Llama shapes,
  and each node generates exactly its own shard from (tensor, layer, row), so nothing is
  downloaded. Speed does not depend on weight values.
- **Correctness.** Every pipeline layout (1, 2, 4, 8 nodes; any micro-batching) gives a
  bit-identical token checksum to one node. Tensor layouts are not bit-identical, because partial
  sums are added in a different order (expected with any tensor split). Speed is unaffected.
- **Node** = a process in its own cgroup with a 0.5-core quota (10 ms period) and one thread, plus
  a RAM cap where stated. The box has 4 cores, so 8 nodes = the whole box.
- **Links.** The box has no tc/netem, so `lagproxy.c` stands in for it. Every inter-node byte is
  billed to the sender's egress bucket and then delayed. Measured: 1.0 ms app-to-app RTT and
  199 MB/s, against targets of 0.9 ms and 200 MB/s.
  - "real" = 0.9 ms / 200 MB/s.
  - "dilated" = 5.85 ms / 30.8 MB/s. One emulated node computes 6.5x slower than this whole
    4-core box (5.0 vs 32.3 tok/s), so slowing the link by the same factor keeps a real 4-vCPU
    runner's ratio of compute time to network time. **Dilated is the better predictor for real
    runners.**
- **Noise.** Repeating the one-node baseline gave 4.99 and 4.64 tok/s (±7%).
- **Host limit at 8 nodes.** With zero-latency links, tensor mode at 8 nodes (14.5 tok/s) is no
  faster than at 4 nodes (15.6 tok/s): 8 half-core nodes saturate this box's CPU and memory
  bandwidth. Treat the 8-node points here as lower bounds.

## 1. Model fits one node (1.1B, `s1`): speedup over one node

One-node baselines (decode tok/s): 1 stream 4.99; 8 streams 17.5; 16 streams 21.6; 64 streams 21.3.
One node is compute-bound from about 16 streams onward.

**Throughput, pipeline layout with 64 concurrent streams** (N micro-batches of 64/N streams):

| nodes | real links | efficiency | dilated links | efficiency |
|---:|---:|---:|---:|---:|
| 2 | 1.86x (39.5 tok/s) | 93% | 1.97x | 98% |
| 4 | 3.35x (71.3) | 84% | 3.71x | 93% |
| 8 | 5.77x (122.9) | 72% | 5.63x | 70% |

This clears the 80% bar at 2 and 4 nodes. With 16 streams it falls to 1.64x / 2.06x / 2.78x,
because cutting 16 streams into N micro-batches throws away the batching that one node already
gets (one node at 16 streams is 4.3x its one-stream speed).

**One stream (chat latency):**

| layout | 2 nodes | 4 nodes | 8 nodes |
|---|---:|---:|---:|
| pipeline, real | 1.10x | 0.99x | 0.93x |
| pipeline, dilated | 1.02x | 0.92x | 0.77x |
| tensor, real | 1.67x | 2.75x | 1.55x |
| tensor, zero-latency control | 1.65x | 3.12x | 2.90x (host-limited) |
| tensor, dilated | 0.94x | 0.93x | 0.82x |

Tensor layout with 8 streams: real 1.90x / 2.86x / 2.08x; dilated 1.26x / 0.93x / 0.54x.
Prefill with the pipeline layout (prompts stream through the stages): 1.8–2.0x at 2 nodes,
2.8–3.2x at 4, and 4.6–5.8x at 8.

## 2. Model bigger than one node (7B-class `m7`, 5.9 GB int8, nodes capped at 2 GB)

At 1/8 scale this stands for a ~47 GB model on 16 GB runners. Decode tok/s:

| configuration | 1 stream | 8 streams |
|---|---:|---:|
| one 2 GB node, weights mmap'd from disk | 0.27 | n/a |
| 2 nodes, 2 GB each | **does not run** (OOM-killed: a 3 GB shard won't fit in 2 GB) | |
| pipeline 4 nodes, real / dilated | 0.90 / 0.91 | 5.21 / 5.14 |
| pipeline 8 nodes, real / dilated | 0.93 / 0.88 | 5.47 / 5.53 |
| tensor 4 nodes, real / dilated | **3.02 / 1.95** | **11.03 / 5.73** |
| tensor 8 nodes, real / dilated | 2.92 / 1.75 | 10.34 / 3.36 |
| reference: one imaginary 6 GB machine with one node's CPU | 0.89 | 3.38 |
| reference: one imaginary 6 GB machine with 8 nodes' CPU (whole box) | 5.01 | 19.4 |

- **Capacity bar: met.** The model runs on 4 or 8 nodes and doesn't run on 1 or 2.
- **Pipeline equals the imaginary big machine.** Its 1-stream speed equals that of one 6 GB
  machine with a single node's CPU (0.90 vs 0.89), so the "one big machine" illusion costs ~0%.
- **Tensor beats the big machine.** At runner-ratio links, tensor mode on 4 nodes is 2.2x that big
  machine and 7x the small node streaming from disk.
- **Bigger models change the answer.** At 1.1B, tensor mode loses on runner-ratio links. At 7B, it
  wins, because per-layer compute grows with d² while the sync cost per layer stays fixed.

## 3. Prior art measured on the same setup: llama.cpp RPC (1.1B Q8_0)

Local llama.cpp on one node: 5.27 tok/s.

| | 1 node | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| RPC layer split, real | 2.70 (0.51x) | 2.59 | 2.76 | 2.89 (0.55x) |
| RPC layer split, dilated | 2.51 | 2.62 | 2.60 | 2.71 |
| RPC tensor split, real | | 3.25 (0.62x) | 1.36 (0.26x) | |
| RPC tensor split, dilated | | 3.38 | 0.28 (0.05x) | |

- **Layer split.** It gives capacity only, at about half of local speed for every node count. The
  client drives every server in a star topology with synchronous graph round trips.
- **Tensor split.** It collapses on slow links.
- **Against veil-llm on the same 8 nodes.** For one stream, Veil pipeline gets 4.65 tok/s vs 2.89
  (1.6x). Veil tensor mode on 4 nodes gets 13.7 tok/s vs 1.36 for RPC tensor (10x).
- **Caveat.** rpc-server ran with one thread under the same half-core quota, which may penalize it
  more than a real runner would.

## 4. Where efficiency breaks, and why

1. **One stream through a pipeline is serial.** Stages take turns, so a layer split adds RAM but
   no speed (≈1.0x, falling to 0.77x at 8 nodes on runner-ratio links as hops add up). Fixes:
   more streams, or speculative decoding (a small draft model proposes k tokens and the pipeline
   checks them in one pass).
2. **Micro-batching competes with batching.** One node already gets 4.3x from batching 16
   streams. The pipeline only wins clearly with about N×8 streams. So the scheduler should size
   micro-batches to the knee of one node's batching curve (8 here), not split the batch evenly.
3. **Tensor mode is latency-bound.** It needs two all-reduces per layer: 44 per token at 1.1B and
   64 at 7B. Per-token cost ≈ compute/N + 2L·(RTT/2 + (N−1)·B·d·4 bytes / egress BW). At 0.9 ms,
   1.1B on 8 nodes spends ~35 ms per token syncing against ~25 ms of compute. A real runner
   computes 6.5x faster than the emulated node, so syncing dominates and tensor mode loses below
   ~7B. It pays where per-layer compute is much larger than RTT: big models, which are exactly
   the ones that need sharding.
4. **The all-reduce bandwidth term grows with N.** In the all-to-all, each node sends N−1 copies,
   which is why 8 nodes is worse than 4 in tensor mode.
5. **Host ceiling (emulation only).** 8 half-core nodes on 4 cores share memory bandwidth. Real
   runners don't share, so 8-node points should improve there.

## 5. Failover (kill gate)

`VLLM_FAILOVER=1`, pipeline layout. SIGKILL a node mid-decode. Survivors keep their shards in RAM
and reconnect around the ring, a replacement builds the dead node's shard, and the driver replays
each sequence's token history (prompt plus everything generated) to rebuild the lost KV cache.

- **Result.** Killing rank 0, 2 or 3 mid-decode, or rank 1 during prefill, gave an identical
  checksum each time, 0 replay mismatches, and 0.02–0.17 s of recovery (tiny model, 4 nodes).
- **What it costs.** Replay is a prefill of the history, so recovery grows with context length.
  No checkpoint is needed.

## 6. How this compares with the prior art

| system | what it does | what Veil keeps or changes |
|---|---|---|
| llama.cpp RPC | client-driven star; per-graph RPC round trips | Peer-to-peer activation streaming: one message per stage boundary. Measured 1.6x faster pipeline and 10x faster tensor mode on the same setup. |
| Petals | volunteer pipeline over WAN; client-side activation cache for failover | Same replay-based failover, but replaying tokens (tiny) instead of activations. Inside one region the links are 100x faster, so tensor mode becomes viable for big models. |
| exo / distributed-llama | pipeline (exo) or tensor (distributed-llama) on home devices | Both layouts in one engine. Picking by model size and link (rules below) is measured, not assumed. |
| Megatron-LM / DeepSpeed | TP inside a node over NVLink, PP across nodes | Same hybrid rule moved to CPUs: TP only within groups whose RTT is small relative to per-layer compute, PP across groups. |
| DSM / LegoOS / CXL tiering | make remote memory look local, page by page | Pages moving at ~1 ms RTT gave 30–130x slowdowns in Veil. For LLMs the "big machine" comes from moving compute to where the weights live (a shard stays put; only activations move), which costs ~0% vs an imaginary big machine (section 2). |
| Ray / vLLM serving | many replicas, request routing | Replication can't run a model bigger than one machine. Sharding does; replication is the right call once a model fits. |

**Rules that fall out of the data:**
- Model fits one runner, many users: replicate, or use a pipeline with ≥8 streams per stage.
- Model fits one runner, one user: run it on one runner. Splitting loses on 1 ms links.
- Model doesn't fit: pipeline for capacity and throughput; tensor mode (≤4–8 per group) when the
  per-layer compute is much larger than RTT, which is ~7B and up on runners.

## 7. Roadmap

1. Real-runner numbers via the request below: link RTT/BW, both layouts at 1/2/4/8 runners,
   s1/m7 plus 34B and 70B-class capacity runs on 4 and 8 runners.
2. Hybrid TP×PP groups chosen from measured RTTs, so placement follows the network.
3. Cheaper all-reduce: fp16 or int8 on the wire, reduce-scatter + all-gather for N > 4, and
   overlapping communication with the next matmul chunk.
4. Speculative decoding over the pipeline, for one-stream latency.
5. A batching-aware scheduler: micro-batch size at one node's batching knee, plus continuous
   batching.
6. Elasticity: when a runner joins or leaves, re-partition layers (synthetic or object-store
   streamed shards) and move KV caches by replay; hot spares to cut recovery to the replay time.
7. Real weights: a GGUF shard loader, so each node reads only its byte range.

## Runner request (for the Veil bot)

Patch `0002-veil-llm-sharded-inference.patch`, on veilgrok/veil-burst `9a71b48`. It only adds new
files (`gha/llm/*`, `.github/workflows/veil-llm.yml`) and touches no DSM code, so XCHECK and
torture are unaffected. The LLM gates are checksum equality and the kill test. See README.md.
