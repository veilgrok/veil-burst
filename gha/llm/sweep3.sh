#!/bin/bash
# Prior art: llama.cpp RPC (layer split; and its tensor split mode) on the same s1 shape (Q8_0 GGUF).
cd "$(dirname "$0")"
OUT=${OUT:-results/sweep3.jsonl}; mkdir -p results
GG=/home/user/models/s1-q8.gguf
run() { timeout 900 python3 rpcbench.py --gguf $GG --cpu 0.5 "$@" >> $OUT; tail -1 $OUT | cut -c1-260; }
run --nodes 0 --tag base
for LINK in "0.9 200 real" "5.85 30.8 dilated"; do
  set -- $LINK
  for N in 1 2 4 8; do run --nodes $N --rtt-ms $1 --mbps $2 --tag $3; done
  for N in 2 4; do run --nodes $N --rtt-ms $1 --mbps $2 --sm tensor --tag $3; done
done
