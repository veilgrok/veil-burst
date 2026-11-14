#!/bin/bash
# Scaling sweep for the s1 (1.1B) model, which fits one node: speedup vs one node.
# Node = 0.5 core (cgroup quota, 10 ms period), 1 thread. Links 0.9 ms RTT / 200 MB/s,
# and "dilated" links (x6.5 slower) that keep the runner's compute:network ratio.
cd "$(dirname "$0")"
OUT=${OUT:-results/sweep1.jsonl}; mkdir -p results
M=${MODEL:-s1}; P=32; G=32
run() { python3 bench.py --model $M --prompt $P --gen $G --cpu 0.5 "$@" >> $OUT; tail -1 $OUT | cut -c1-260; }
for S in 1 4 8 16; do run --mode single --seqs $S --tag base; done
for LINK in "0.9 200 real" "5.85 30.8 dilated"; do
  set -- $LINK; R=$1; BW=$2; T=$3
  for N in 2 4 8; do
    run --mode pp --nodes $N --seqs 1 --mb 1 --rtt-ms $R --mbps $BW --tag $T
    for S in 8 16; do run --mode pp --nodes $N --seqs $S --mb $N --rtt-ms $R --mbps $BW --tag $T; done
    run --mode pp --nodes $N --seqs 8 --mb 1 --rtt-ms $R --mbps $BW --tag $T
    run --mode tp --nodes $N --seqs 1 --rtt-ms $R --mbps $BW --tag $T
    run --mode tp --nodes $N --seqs 8 --rtt-ms $R --mbps $BW --tag $T
  done
done
