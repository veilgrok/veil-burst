#!/bin/bash
# Strong-scaling run of dsmn on one host. usage: scale.sh base|dsm N WORKLOAD [ARG]
# base: one process on one core does the whole job (the one-machine baseline). dsm: N node processes, one core each
# (core k mod nproc), the home on node 0's core; nodes 1..N-1 reach the home through their own ./lag link
# (LAG_US one way, LAG_MBPS per direction). env: MIB (512) RA (1024) XENV (extra env for home and nodes) OWN=0 disables the owner map
cd "$(dirname "$0")"; MIB=${MIB:-512}; NPG=$((MIB * 256)); PORT=${PORT:-8010}; MODE=$1; N=$2; W=$3; ARG=${4:-}; NC=$(nproc)
gcc -O2 -o dsmn dsmn.c -lpthread && gcc -O2 -o dsmh_s ${SRC:-..}/dsmh.c && gcc -O2 -shared -fPIC -o shim_s.so ${SRC:-..}/dsm_shim.c -ldl -lpthread && gcc -O2 -o lag lag.c -lpthread || exit 1
pkill -9 -x dsmn 2>/dev/null; pkill -9 -x dsmh_s 2>/dev/null; rm -f /dev/shm/veilS*
if [ "$MODE" = base ]; then taskset -c 0 ./dsmn -1 $N /dev/shm/veilS $MIB $W $ARG; rm -f /dev/shm/veilS*; exit; fi
MAP=$(./dsmn map $N $MIB); [ "${OWN:-1}" = 0 ] && MAP=""
E="VEIL_DSM_PREFILL=1 VEIL_DSM_RA=${RA:-1024} VEIL_DSM_REPMIN=16 ${MAP:+VEIL_DSM_OWNMAP=$MAP} ${XENV:-}"
LP=""; for k in $(seq 1 $((N-1))); do ./lag $((PORT+100+k)) 127.0.0.1:$PORT ${LAG_US:-0} ${LAG_MBPS:-0} > /dev/null 2>&1 & LP="$LP $!"; done
env $E taskset -c 0 ./dsmh_s home $PORT $NPG $N 2> home_s.log & HP=$!; sleep 0.3
PIDS=""; for k in $(seq $((N-1)) -1 0); do HPORT=$PORT; [ $k -gt 0 ] && HPORT=$((PORT+100+k))
  env $E LD_PRELOAD=$PWD/shim_s.so VEIL_DSM_MATCH=veilS VEIL_DSM_HOME=127.0.0.1:$HPORT VEIL_DSM_ID=$k timeout ${TMO:-300} taskset -c $((k % NC)) ./dsmn $k $N /dev/shm/veilS$k $MIB $W $ARG 2> n_s$k.log & PIDS="$PIDS $!"; done
wait $PIDS; kill -9 $HP $LP 2>/dev/null; wait 2>/dev/null; grep -a "DSM home t=" home_s.log | tail -1 | grep -o "requests=[0-9]*\|recalls=[0-9]*\|data_MiB=[0-9.]*" | tr '\n' ' '; echo; rm -f /dev/shm/veilS*
