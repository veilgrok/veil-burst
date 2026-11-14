#!/bin/bash
# Pooled-memory scaling runs on K runners (MODES="P ..."): strong scaling of gha/localtest/dsmn.c over the DSM home, one
# DSM node per runner with THREADS = that runner's cores, against the same job on ONE runner with all its cores and no DSM.
# Then the correctness gates: dsmt cross-check/torture between runners 0 and 1, and a kill of the primary home (runner 2,
# mirror on runner 0) mid-run. Knobs (in MODES after P): KN=runners (4) NS=1,2,4 WL=cpu,stream,gather,shared,touch
#   SCALE=work multiplier (16) CAP=1 capacity runs (CAPMIB=6144 data (fits the default 8 GiB /dev/shm), CAPLOCAL=pages kept on the compute runner (393216 = 1.5 GiB, a quarter), COLD=ppm (100))
#   MIB=per-node shm size (2048) RA (1024) LEASE (8192) REGION (8) CLEANACK (1) SW (0) HOLD_US (0) REPMIN (0: multi-writer replicas lose concurrent atomic updates to one word, see dsmh.c) GATES=1 KILLAT (secs, 2.0)
# Every result line is "POOL ..." in the run log (via the relay KV, like vsplit2.sh). Speedup = base_s / job_s(N).
set -u; for kv in ${MODES#P}; do case $kv in KN=*|NS=*|WL=*|MIB=*|RA=*|LEASE=*|REGION=*|CLEANACK=*|SW=*|HOLD_US=*|GATES=*|KILLAT=*|SCALE=*|CAP=*|CAPMIB=*|CAPLOCAL=*|COLD=*|REPMIN=*) export "$kv";; esac; done
KN=${KN:-4}; NS=$(echo ${NS:-1,2,4} | tr , " "); WL=$(echo ${WL:-cpu,stream,gather,shared,touch} | tr , " "); MIB=${MIB:-2048}; RUN=$GITHUB_RUN_ID; ROOT=$PWD; K() { python3 $ROOT/gha/kv.py "$@"; }
# ---- groups from whatever GitHub scheduled (gha/mesh.sh): every region with >= 2 runners forms groups of up to KN, each with
# its own WireGuard mesh (runner i of group g = 10.88.g.(i+1)) and its own KV keys ($GPFX). No group anywhere: exit 1.
PFX=pl-$RUN; MINK=2; source gha/mesh.sh; REG=$MREG; CPU=$MCPU
NS="$(for n in $NS; do [ $n -le $KN ] && echo $n; done | tr '\n' ' ')"; case " $NS " in *" $KN "*) ;; *) NS="$NS $KN";; esac
L() { echo "$*"; (K put $GPFX-log-$G-$(date +%s%N) "$*" >/dev/null 2>&1 &) ; }
L "POOL node=$G group=$GRP size=$KN xreg=$XR region=$REG cpu=$CPU nproc=$(nproc) mem=$(free -g | awk '/Mem/{print $2}')G ns='$NS' knobs='${MODES#P}'"
cd gha/localtest && gcc -O2 -o dsmn dsmn.c -lpthread && gcc -O2 -o dsmt dsmt.c && gcc -O2 -o dsmh ../dsmh.c && gcc -O2 -shared -fPIC -o dsm_shim.so ../dsm_shim.c -ldl -lpthread || { L "POOL build failed"; exit 1; }
for i in $(seq 0 $((KN-1))); do [ $i = $G ] || L "POOL ping $G->$i $(ping -c 20 -i 0.2 $(MESHIP $i) | tail -1)"; done
export SCALE=${SCALE:-16}; T=$(nproc); NPG=$((MIB * 256)); E="VEIL_DSM_PREFILL=1 VEIL_DSM_RA=${RA:-1024} VEIL_DSM_REPMIN=${REPMIN:-0} VEIL_DSM_LEASE=${LEASE:-8192} VEIL_DSM_REGION=${REGION:-8} VEIL_DSM_SW=${SW:-0} VEIL_DSM_HOLD_US=${HOLD_US:-0}"
step=0
for W in $WL; do
  # one runner, all its cores, no DSM: the baseline every speedup is measured against (run twice, the faster kept)
  if [ $G = 0 ]; then for r in 1 2; do rm -f /dev/shm/veilP*; L "POOL base w=$W $(THREADS=$T ./dsmn -1 1 /dev/shm/veilP $MIB $W | sed 's/.*compute_s/compute_s/')"; done; rm -f /dev/shm/veilP*; fi
  for N in $NS; do [ $N -gt $KN ] && continue; step=$((step+1)); PORT=$((7000 + step)); MAP=$(./dsmn map $N $MIB)
    if [ $G = 0 ]; then env $E VEIL_DSM_OWNMAP=$MAP timeout -s KILL 900 ./dsmh home $PORT $NPG $N 2> home_$step.log & HP=$!; sleep 0.5; K put $GPFX-h$step-0 1 >/dev/null; fi
    if [ $G -lt $N ]; then K wait $GPFX-h$step- 1 900 >/dev/null; rm -f /dev/shm/veilP*
      R=$(env $E VEIL_DSM_OWNMAP=$MAP VEIL_DSM_CLEANACK=${CLEANACK:-1} LD_PRELOAD=$PWD/dsm_shim.so VEIL_DSM_MATCH=veilP VEIL_DSM_HOME=$(MESHIP 0):$PORT VEIL_DSM_ID=$G THREADS=$T timeout -s KILL 900 ./dsmn $G $N /dev/shm/veilP$G $MIB $W 2> node_$step.log)
      L "POOL dsm w=$W n=$N $(echo "$R" | sed 's/.*compute_s/compute_s/')"; K put $GPFX-d$step-$G 1 >/dev/null; rm -f /dev/shm/veilP*; fi
    if [ $G = 0 ]; then K wait $GPFX-d$step- $N 900 >/dev/null; sleep 1; kill -9 $HP 2>/dev/null; L "POOL home w=$W n=$N $(grep -a 'DSM home t=' home_$step.log | tail -1 | grep -o 'requests=[0-9]*\|recalls=[0-9]*\|leases=[0-9]*\|stride_pf=[0-9]*\|region_pf=[0-9]*\|data_MiB=[0-9.]*' | tr '\n' ' ')"; fi
  done; done
# ---- capacity (CAP=1, needs >= 2 runners): one compute runner keeps CAPLOCAL pages, runner 1 is the memory node holding the rest.
# Compared with the same job on one runner that has the RAM. sweep = write all then 2 read passes; hotcold = random reads, COLD ppm anywhere.
if [ "${CAP:-0}" = 1 ] && [ $KN -ge 2 ] && [ $G -lt 2 ]; then CM=${CAPMIB:-6144}; CN=$((CM * 256))
  for W in sweep hotcold; do step=$((step+1)); PORT=$((7000 + step))
    if [ $G = 0 ]; then rm -f /dev/shm/veilP*; L "POOL capbase w=$W mib=$CM $(THREADS=1 ./dsmn -1 1 /dev/shm/veilP $CM $W ${COLD:-100} 2>&1 | tr '\n' ' ' | sed 's/chk=[0-9a-f]* //')"; rm -f /dev/shm/veilP*; fi
    if [ $G = 1 ]; then env VEIL_DSM_PREFILL=0 VEIL_DSM_RA=${RA:-1024} timeout -s KILL 1800 ./dsmh home $PORT $CN 1 2> cap_home_$step.log & HP=$!; sleep 0.5; K put $GPFX-c$step-0 1 >/dev/null; K wait $GPFX-cd$step- 1 1900 >/dev/null; kill -9 $HP; L "POOL caphome w=$W $(grep -a 'DSM home t=' cap_home_$step.log | tail -1 | grep -o 'requests=[0-9]*\|region_pf=[0-9]*\|evictions=[0-9]*\|data_MiB=[0-9.]*' | tr '\n' ' ')"; fi
    if [ $G = 0 ]; then K wait $GPFX-c$step- 1 900 >/dev/null; rm -f /dev/shm/veilP*
      R=$(env VEIL_DSM_PREFILL=0 VEIL_DSM_LOCALMAX=${CAPLOCAL:-393216} VEIL_DSM_CLEANACK=1 LD_PRELOAD=$PWD/dsm_shim.so VEIL_DSM_MATCH=veilP VEIL_DSM_HOME=$(MESHIP 1):$PORT VEIL_DSM_ID=0 THREADS=1 timeout -s KILL 1800 ./dsmn 0 1 /dev/shm/veilP0 $CM $W ${COLD:-100} 2> cap_node_$step.log)
      L "POOL cap w=$W mib=$CM local_pages=${CAPLOCAL:-393216} $(echo "$R" | sed 's/chk=[0-9a-f]* //') $(grep -a 'DSMN part' cap_node_$step.log | tr '\n' ' ') $(grep -a 'faults' cap_node_$step.log | tail -1 | grep -o 'evictions=.*')"; rm -f /dev/shm/veilP*; K put $GPFX-cd$step-0 1 >/dev/null; fi
  done; fi
# ---- gates (need >= 2 runners): cross-check + torture between runners 0 and 1 with home on 0; then kill the primary home
# (on runner 2, or on runner 1 next to its node when the group has only 2 runners). A gate that isn't clean exits 1.
HR=$((KN >= 3 ? 2 : 1)); GBAD=0
if [ "${GATES:-1}" = 1 ] && [ $KN -ge 2 ] && [ $G -le $HR ]; then
  GE="$E VEIL_DSM_CLEANACK=${CLEANACK:-1} LD_PRELOAD=$PWD/dsm_shim.so VEIL_DSM_MATCH=veilT"
  if [ $G = 0 ]; then env $E timeout -s KILL 600 ./dsmh home 7600 65536 2 2> gate_home.log & HP=$!; sleep 0.5; K put $GPFX-g1h-0 1 >/dev/null; fi
  if [ $G -lt 2 ]; then K wait $GPFX-g1h- 1 600 >/dev/null; rm -f /dev/shm/veilT*; env $GE VEIL_DSM_HOME=$(MESHIP 0):7600 VEIL_DSM_ID=$G timeout -s KILL 600 ./dsmt $G /dev/shm/veilT$G 256 3000 2> gate_n.log | tee gate_o1.log | while IFS= read -r x; do L "POOL gate1 $x"; done; K put $GPFX-g1d-$G 1 >/dev/null; fi
  [ $G = 0 ] && { K wait $GPFX-g1d- 2 600 >/dev/null; kill -9 $HP; }
  # kill test: primary home on runner HR, mirror on runner 0, nodes on 0 and 1; runner HR kills its home at KILLAT s
  if [ $G = 0 ]; then env $E timeout -s KILL 600 ./dsmh mirror 7720 65536 2 7721 2> gate_mirror.log & MP=$!; sleep 0.5; K put $GPFX-g2m-0 1 >/dev/null; fi
  if [ $G = $HR ]; then ( K wait $GPFX-g2m- 1 600 >/dev/null; env $E timeout -s KILL 600 ./dsmh home 7710 65536 2 $(MESHIP 0):7721 2> gate_home2.log & HP=$!; sleep 0.5; K put $GPFX-g2h-0 1 >/dev/null
    K wait $GPFX-g2s- 2 600 >/dev/null; sleep ${KILLAT:-2.0}; kill -9 $HP; L "POOL gate2 FAULT_INJECTED primary home killed ${KILLAT:-2.0}s after start" ) & KP=$!; fi
  if [ $G -lt 2 ]; then K wait $GPFX-g2h- 1 600 >/dev/null; rm -f /dev/shm/veilT*; K put $GPFX-g2s-$G 1 >/dev/null
    env $GE VEIL_DSM_HOME=$(MESHIP $HR):7710 VEIL_DSM_HOME2=$(MESHIP 0):7720 VEIL_DSM_FAILOVER_S=8 VEIL_DSM_ID=$G timeout -s KILL 600 ./dsmt $G /dev/shm/veilT$G 256 3000 2> gate_n2.log | tee gate_o2.log | while IFS= read -r x; do L "POOL gate2 $x"; done
    grep -a "FAILOVER done" gate_n2.log | head -1 | while IFS= read -r x; do L "POOL gate2 $x"; done; K put $GPFX-g2d-$G 1 >/dev/null
    for f in gate_o1.log gate_o2.log; do [ "$(grep -ac 'xcheck.* bad=0$\|torture.* errors=0$' $f)" = 2 ] || GBAD=1; done; grep -aq "FAILOVER done" gate_n2.log || GBAD=1; fi
  [ $G = $HR ] && [ -n "${KP:-}" ] && wait $KP
  [ $G = 0 ] && { K wait $GPFX-g2d- 2 600 >/dev/null; grep -a "TAKEOVER\|unmirrored" gate_mirror.log | head -3 | while IFS= read -r x; do L "POOL gate2 $x"; done; kill -9 $MP; }
fi
[ $GBAD = 1 ] && { L "POOL GATE FAILED node=$G (see the gate1/gate2 lines)"; exit 1; }
L "POOL done node=$G"; exit 0
