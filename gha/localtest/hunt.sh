#!/bin/bash
# repeat the kill test until xcheck reports bad pages; keep the logs of the failing run
cd "$(dirname "$0")"; n=0
for i in $(seq 1 ${RUNS:-60}); do k=$(python3 -c "import random;print(round(random.uniform(1.2,2.6),2))"); n=$((n+1))
  r=$(TMO=40 MIRROR=1 KILLAT=$k "$@" ./run.sh dsm 2>&1 | grep -a "xcheck\|torture" | grep -v "bad=0\|errors=0")
  if [ -n "$r" ]; then echo "FAIL run=$n kill=$k $r"; grep -a BADPAGE n0.log n1.log | head -6; grep -ac BADPAGE n0.log n1.log; grep -a "FAILOVER\|RESUMED\|offered" n0.log n1.log mirror.log | cut -c1-170; for f in n0 n1 mirror home; do cp $f.log fail-$f.log; done; exit 1; fi; done; echo "PASS $n runs"
