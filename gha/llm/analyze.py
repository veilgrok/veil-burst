#!/usr/bin/env python3
"""Turn sweep jsonl files into speedup tables (markdown). Speedup = cluster wall-clock
throughput / one-node wall-clock throughput for the same job (same seqs, prompt, gen)."""
import json, sys
from collections import defaultdict

rows = []
for f in sys.argv[1:]:
    for line in open(f):
        line = line.strip()
        if line.startswith("{"):
            rows.append(json.loads(line))

base = {}
for r in rows:
    if r.get("mode") == "single" and r.get("tag") == "base":
        base[(r["model"], r["seqs"])] = r

print("| link | layout | nodes | streams | micro-batches | decode tok/s | speedup | efficiency | step ms | prefill tok/s | prefill speedup | output = 1-node |")
print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|")
for r in rows:
    if r.get("mode") not in ("pp", "tp"):
        continue
    b = base.get((r["model"], r["seqs"]))
    if not b:
        continue
    sp = r["decode_tok_s"] / b["decode_tok_s"]
    psp = r["prefill_tok_s"] / b["prefill_tok_s"]
    same = "yes" if r["checksum"] == b["checksum"] else "no (fp sum order)"
    print(f"| {r['tag']} | {r['mode']} | {r['nodes']} | {r['seqs']} | {r['microbatches']} | {r['decode_tok_s']:.2f} | "
          f"{sp:.2f}x | {100 * sp / r['nodes']:.0f}% | {r['step_ms']:.0f} | {r['prefill_tok_s']:.1f} | {psp:.2f}x | {same} |")
print()
print("one-node baselines:")
for (m, s), b in sorted(base.items()):
    print(f"- {m}, {s} streams: decode {b['decode_tok_s']:.2f} tok/s, prefill {b['prefill_tok_s']:.1f} tok/s, step {b['step_ms']:.0f} ms")
