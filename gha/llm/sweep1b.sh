#!/bin/bash
# Follow-ups to sweep 1: rerun a point disturbed by a concurrent test, controls that
# separate link cost from host contention, and bigger batches for pipeline throughput.
cd "$(dirname "$0")"
OUT=results/sweep1b.jsonl
run() { python3 bench.py --model s1 --cpu 0.5 "$@" >> $OUT; tail -1 $OUT | cut -c1-240; }
run --mode pp --nodes 2 --seqs 1 --mb 1 --prompt 32 --gen 32 --rtt-ms 5.85 --mbps 30.8 --tag dilated
run --mode pp --nodes 2 --seqs 1 --mb 1 --prompt 32 --gen 32 --rtt-ms 0.9 --mbps 200 --tag real-repeat
run --mode single --seqs 1 --prompt 32 --gen 32 --tag base-repeat
# zero-latency, unlimited-bandwidth links: what's left is compute split + host contention
for N in 2 4 8; do
  run --mode tp --nodes $N --seqs 1 --prompt 32 --gen 32 --rtt-ms 0 --mbps 0 --tag nolink
  run --mode pp --nodes $N --seqs 16 --mb $N --prompt 32 --gen 32 --rtt-ms 0 --mbps 0 --tag nolink
done
# throughput with enough streams to keep per-stage batches large
run --mode single --seqs 64 --prompt 4 --gen 16 --tag base
for N in 2 4 8; do
  run --mode pp --nodes $N --seqs 64 --mb $N --prompt 4 --gen 16 --rtt-ms 0.9 --mbps 200 --tag real
  run --mode pp --nodes $N --seqs 64 --mb $N --prompt 4 --gen 16 --rtt-ms 5.85 --mbps 30.8 --tag dilated
done
