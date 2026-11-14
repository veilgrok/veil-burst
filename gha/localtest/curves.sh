#!/bin/bash
# Scaling curves on this host: base (one process, one core) twice, then N=1..NMAX pooled nodes (one core each) over lag links.
cd "$(dirname "$0")"; export MIB=${MIB:-512}; NMAX=${NMAX:-4}
for w in ${WL:-cpu stream gather shared touch}; do
  for r in 1 2; do ./scale.sh base $NMAX $w | sed "s/^/BASE /"; done
  for n in $(seq 1 $NMAX); do LAG_US=${LAG_US:-450} LAG_MBPS=${LAG_MBPS:-200} ./scale.sh dsm $n $w | sed "s/^/DSM$n /"; grep -ah "STALL total\|BAD" n_s*.log | head -2; done; done
