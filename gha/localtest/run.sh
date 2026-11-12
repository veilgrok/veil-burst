#!/bin/bash
# Local DSM harness. usage: run.sh [base|dsm] ; env MIB (256) ITERS DELAY_US SW RA INVQ CLEANACK MIRROR=1 KILLAT=secs (kill the primary home; needs MIRROR=1)
cd "$(dirname "$0")"; MIB=${MIB:-256}; NPG=$((MIB * 256)); PORT=${PORT:-7910}
gcc -O2 -o dsmt dsmt.c && gcc -O2 -o dsmh ${SRC:-..}/dsmh.c && gcc -O2 -shared -fPIC -o dsm_shim.so ${SRC:-..}/dsm_shim.c -ldl -lpthread || exit 1
rm -f /dev/shm/veilT*; pkill -9 -x dsmh 2>/dev/null; pkill -9 -x dsmt 2>/dev/null
if [ "${1:-dsm}" = base ]; then ./dsmt 1 /dev/shm/veilT $MIB $ITERS & ./dsmt 0 /dev/shm/veilT $MIB $ITERS; wait; rm -f /dev/shm/veilT*; exit; fi
E="${MSYNC:+VEIL_DSM_MSYNC=1} VEIL_DSM_SW=${SW:-0} VEIL_DSM_RA=${RA:-64} VEIL_DSM_INVQ=${INVQ:-0} VEIL_DSM_REPMIN=16 VEIL_DSM_PREFILL=1 VEIL_DSM_DELAY_US=${DELAY_US:-0}"
if [ -n "$MIRROR" ]; then env $E ./dsmh mirror $((PORT+10)) $NPG 2 $((PORT+11)) 2> mirror.log & sleep 0.3; M=127.0.0.1:$((PORT+11)); fi
env $E ./dsmh home $PORT $NPG 2 $M 2> home.log & sleep 0.3
if [ -n "$KILLAT" ]; then H2="VEIL_DSM_HOME2=127.0.0.1:$((PORT+10)) VEIL_DSM_FAILOVER_S=${FOS:-3}"; (sleep $KILLAT; pkill -9 -f "dsmh home"; echo "FAULT_INJECTED primary home killed at ${KILLAT}s") & fi
SH="$H2 VEIL_DSM_CLEANACK=${CLEANACK:-0} LD_PRELOAD=$PWD/dsm_shim.so VEIL_DSM_PREFILL=1 VEIL_DSM_MATCH=veilT VEIL_DSM_HOME=127.0.0.1:$PORT"
env $SH VEIL_DSM_ID=1 timeout ${TMO:-120} ./dsmt 1 /dev/shm/veilT1 $MIB $ITERS 2> n1.log & env $SH VEIL_DSM_ID=0 timeout ${TMO:-120} ./dsmt 0 /dev/shm/veilT0 $MIB $ITERS 2> n0.log; wait %2 2>/dev/null
sleep 0.5; pkill -9 -x dsmh; grep -ah "FAILOVER done\|TAKEOVER" n0.log n1.log mirror.log 2>/dev/null | cut -c1-150; grep -ah "DSM home t=" home.log mirror.log 2>/dev/null | tail -1 | sed "s/.*sw_batches=[0-9]* //"; rm -f /dev/shm/veilT*
