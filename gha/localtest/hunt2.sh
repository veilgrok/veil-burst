#!/bin/bash
# Kill hunt that also fails when a run does not finish: each run must print xcheck and torture lines for BOTH nodes, all clean.
# usage: RUNS=N [KMIN=1.2 KMAX=2.6 HTMO=40] hunt2.sh   (env such as XENV, RA, CLEANACK passes through to run.sh)
cd "$(dirname "$0")"; n=0
for i in $(seq 1 ${RUNS:-60}); do k=$(python3 -c "import random;print(round(random.uniform(${KMIN:-1.2},${KMAX:-2.6}),2))"); n=$((n+1))
  out=$(TMO=${HTMO:-40} MIRROR=1 KILLAT=$k ./run.sh dsm 2>&1); good=$(echo "$out" | grep -a "xcheck.*bad=0$\|torture.*errors=0$" | wc -l)
  if [ "$good" -ne 4 ]; then echo "FAIL run=$n kill=$k clean_lines=$good/4"; echo "$out" | grep -a "DSMT\|FAILOVER\|TAKEOVER" | cut -c1-160; grep -a BADPAGE n0.log n1.log | head -4; for f in n0 n1 mirror home; do cp $f.log fail-$f.log; done; exit 1; fi; done; echo "PASS $n runs (all 4 result lines clean every run)"
