#!/bin/bash
# Capacity sweep: m7 (5.9 GB int8) on nodes capped at 2 GB RAM each (1/8 scale of a
# ~47 GB model on 16 GB runners). One node cannot hold it.
cd "$(dirname "$0")"
OUT=${OUT:-results/sweep2.jsonl}; mkdir -p results
M=m7; P=32; G=16; MEM=2
run() { python3 bench.py --model $M --prompt $P "$@" >> $OUT; tail -1 $OUT | cut -c1-260; }
WF=/home/user/models/m7.vllm
[ -f $WF ] || ./vllm gen $M $WF
# references: an imaginary big machine (no RAM cap) with one node's CPU, and with 8 nodes' CPU
for S in 1 8; do run --mode single --seqs $S --gen $G --cpu 0.5 --tag bigmachine-1node-cpu; done
for S in 1 8; do run --mode single --seqs $S --gen $G --cpu 0 --threads 4 --tag bigmachine-8node-cpu; done
# one real small node: 2 GB cap, weights streamed from disk through mmap
sync; echo 3 > /proc/sys/vm/drop_caches
run --mode single --seqs 1 --gen 4 --cpu 0.5 --mem-gb $MEM --wfile $WF --tag smallnode-mmap
for LINK in "0.9 200 real" "5.85 30.8 dilated"; do
  set -- $LINK; R=$1; BW=$2; T=$3
  run --mode pp --nodes 2 --seqs 1 --gen $G --cpu 0.5 --mem-gb $MEM --rtt-ms $R --mbps $BW --tag $T || true
  for N in 4 8; do
    run --mode pp --nodes $N --seqs 1 --mb 1 --gen $G --cpu 0.5 --mem-gb $MEM --rtt-ms $R --mbps $BW --tag $T
    run --mode pp --nodes $N --seqs 8 --mb $N --gen $G --cpu 0.5 --mem-gb $MEM --rtt-ms $R --mbps $BW --tag $T
    run --mode tp --nodes $N --seqs 1 --gen $G --cpu 0.5 --mem-gb $MEM --rtt-ms $R --mbps $BW --tag $T
    run --mode tp --nodes $N --seqs 8 --gen $G --cpu 0.5 --mem-gb $MEM --rtt-ms $R --mbps $BW --tag $T
  done
done
