#!/bin/bash
# vCPU split v2 on the two-level path. Guest RAM per node = /dev/shm file kept coherent by dsm_shim.so + gha/dsmh.c
# (N-client home with readahead/write-ahead, per-connection sequence numbers, page-table/silent-page replication with
# de-replication, optional synchronous mirror + client failover). K runners meshed with WireGuard.
#  MODES (one): B2  baselines (bare, stock QEMU 10 memfd 2/4 vCPU)      S3  same-runner split (2 QEMU-gvm, localhost)
#               X2  2 runners x 1 vCPU (home on node0)                 X4  2 runners x 2 vCPU
#               X3  3 runners x 1 vCPU                                 R   4 runners: 2 x 1 vCPU + primary home + mirror;
#                                                                          the primary-home runner is killed at KILLAT
set -u; MODE=${MODES%% *}; G=$GIDX; RUN=$GITHUB_RUN_ID; K() { python3 gha/kv.py "$@"; }; J() { echo "VEIL_JSON $1"; }
case $MODE in X2) KN=2 SMP=2 NCOMP=2;; X4) KN=2 SMP=4 NCOMP=2;; X3) KN=3 SMP=3 NCOMP=3;; R) KN=4 SMP=2 NCOMP=2;; *) KN=1 SMP=2 NCOMP=2;; esac
REG=$(curl -s -m 5 -H Metadata:true 'http://169.254.169.254/metadata/instance/compute/location?api-version=2021-02-01&format=text'); V=$(grep -qw vmx /proc/cpuinfo && echo intel || echo amd)
if [ "$KN" -gt 1 ]; then
  K put vs-$RUN-reg-$G "$REG|$V"; ALL=$(K wait vs-$RUN-reg- $NN $([ $KN -gt 2 ] && echo 900 || echo 300)) || ALL=$(K wait vs-$RUN-reg- $KN 5)
  SEL=$(echo "$ALL" | MODE=$MODE KN=$KN python3 -c "
import json,sys,os; kn=int(os.environ['KN']); d={int(k.rsplit('-',1)[1]):v.lower() for k,v in json.load(sys.stdin).items()}; g={}
for i in sorted(d): g.setdefault(d[i],[]).append(i)
c=[v for k,v in g.items() if len(v)>=kn and k.endswith('|intel')] or [v for v in g.values() if len(v)>=kn]
r=c[0][:kn] if c else None
if not r and kn==4 and os.environ.get('MODE')=='R':
  pr=[v for k,v in g.items() if len(v)>=2]
  if pr and len(d)>=4: a=pr[0][:2]; r=a+[i for i in sorted(d) if i not in a][:2]
print(' '.join(map(str,r)) if r else 'none')")
  S0=$(K wait vs-$RUN-sel- 1 1 2>/dev/null | python3 -c "import json,sys;print(list(json.load(sys.stdin).values())[0])" 2>/dev/null); if [ -n "$S0" ]; then SEL=$S0; else K put vs-$RUN-sel-0 "$SEL" >/dev/null; fi
  J "{\"kind\":\"pick\",\"gidx\":$G,\"sel\":\"$SEL\"}"
  if [ "$SEL" = none ]; then [ "$G" = 0 ] && K put vs-$RUN-log-x-$(date +%s%N) "NOPAIR no $KN same-region runners: $(echo $ALL | tr -d '{}')" >/dev/null; echo "no group"; exit 0; fi
  P=-1; i=0; for s in $SEL; do [ "$s" = "$G" ] && P=$i; i=$((i+1)); done; [ $P -lt 0 ] && { echo "not picked"; exit 0; }; G=$P
fi
L() { echo "$*"; (K put vs-$RUN-log-$G-$(date +%s%N) "$*" >/dev/null 2>&1 &) ; }
L "NODE mode=$MODE cpu=$(lscpu | sed -n 's/Model name: *//p' | head -1) region=$REG"
gcc -O2 -o dsmh gha/dsmh.c && gcc -O2 -static -o memtouch gha/memtouch.c && gcc -O2 -static -pthread -o xcheck gha/xcheck.c && gcc -O2 -static -pthread -o pbench gha/pbench.c && gcc -O2 -static -pthread -o torture gha/torture.c || { L "build failed"; exit 1; }
mkdir -p /l2r && tar -xzf art/l2root.tar.gz -C /l2r && mkdir -p /l2r/w && mount --bind $PWD /l2r/w && mount --rbind /dev /l2r/dev && mount -t proc p /l2r/proc || { echo chroot setup failed; exit 1; }
mkdir -p l3/bin l3/proc l3/sys l3/dev; cp /l2r/bin/busybox pbench memtouch xcheck torture l3/bin/; for c in sh cat grep nproc poweroff mount head tr; do ln -sf busybox l3/bin/$c; done
cat > l3/init <<'EOI'
#!/bin/sh
mount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev; exec >/dev/ttyS0 2>&1
echo CPU_ON_START; for c in /sys/devices/system/cpu/cpu[1-9]*; do [ -e $c/online ] && echo 1 > $c/online; done; echo CPU_ON_DONE nproc=$(nproc)
echo "L3 nproc=$(nproc) $(grep MemTotal /proc/meminfo | tr -s ' ') cs=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)"
echo PB1_START; /bin/pbench 1 10000000
echo PB2_START; /bin/pbench 2 10000000
[ "$(nproc)" -ge 4 ] && { echo PB4_START; /bin/pbench 4 10000000; }
echo MT_START; /bin/memtouch 128 3
echo XC_START; /bin/xcheck 32
echo TT_START; /bin/torture 3000 1500 500
echo PB_DONE
poweroff -f
EOI
chmod +x l3/init; (cd l3 && find . | cpio -o -H newc 2>/dev/null | gzip > ../l3x.gz)
KILLAT=${KILLAT:-PB2_START}
stamp() { n=$1; pm=""; pt=0; while IFS= read -r l; do now=$(date +%s.%N)
  case "$l" in *_START*|*PB_DONE*) [ -n "$pm" ] && L "PROF $n ${pm}_wall_s=$(python3 -c "print(round($now-$pt,3))")"; pm=$(echo "$l" | grep -o '[A-Z0-9]*_START' | head -1 | sed 's/_START//'); pt=$now
      case "$l" in *PB_DONE*) pm=""; (sleep 3; pkill -9 -f opt/gvm/qemu-system; pkill -9 -f "qemu-system-x86_64 -enable-kvm") & ;; esac;; esac
  case "$l" in *"$KILLAT"*) [ "$MODE" = R ] && { K put vs-$RUN-kill-0 1 >/dev/null; L "[$n] KILL requested at $KILLAT"; };; esac
  case "$l" in *L3*|*PBENCH*|*MEMTOUCH*|*XCHECK*|*TORTURE*|*CPU_ON*|*"Command line"*|*rror*|*panic*|*DSM*|*lockup*|*rcu_sched*|*Stuck*|*"Run /init"*) L "[$n] $(python3 -c "print(round($now-${T0:-$now},1))")s $l";; esac; done; }
watch_mon() { for t in ${WATCHT:-120 300 600 900 1200}; do sleep $((t - ${pt:-0})); pt=$t; for p in "$@"; do python3 - $p <<'PY' 2>/dev/null | tr -d '\r' | grep -aE "CPU #" | sed "s/^/MON t=$t :$p /" | while IFS= read -r x; do L "$x"; done
import socket,sys,time
s=socket.create_connection(("127.0.0.1",int(sys.argv[1])),timeout=3); time.sleep(0.3); s.sendall(b"info cpus\n"); time.sleep(0.7); s.settimeout(1); d=b""
try:
  while True:
    x=s.recv(65536)
    if not x: break
    d+=x
except Exception: pass
print(d.decode(errors="replace"))
PY
  done; tail -2 home.log 2>/dev/null | sed "s/^/HOME t=$t /" | while IFS= read -r x; do L "$x"; done; done; }
KI=gha/bin/bz-4.18.20-hz100-nohzfull
GA="console=ttyS0 panic=-1 no_timer_check noapic no-kvmclock nokaslr tsc=reliable lpj=24000000 maxcpus=1 nohz_full=1-$((SMP-1)) rcu_nocbs=1-$((SMP-1))"
GQ="/opt/gvm/qemu-system-x86_64 -L /opt/gvm -enable-kvm -machine pc,kernel-irqchip=off -smp $SMP -m 512 -kernel /w/$KI -initrd /w/l3x.gz -nographic -no-reboot -net none"
E="VEIL_DSM_RA=${RA:-64} VEIL_DSM_INVQ=${INVQ:-0} VEIL_DSM_REPSILENT=2048 VEIL_DSM_REPMIN=16 VEIL_DSM_PREFILL=1"
SH="LD_PRELOAD=/w/gha/bin/dsm_shim.so VEIL_DSM_PREFILL=1 VEIL_DSM_MATCH=veil"
if [ "$MODE" = B2 ]; then
  for i in 1 2; do t0=$(date +%s.%N); ./pbench 1 10000000 >/dev/null; t1=$(date +%s.%N); ./pbench 2 10000000 >/dev/null; t2=$(date +%s.%N); ./pbench 4 10000000 >/dev/null; t3=$(date +%s.%N); L "PROF L1bare pb1_wall_s=$(python3 -c "print(round($t1-$t0,3))") pb2_wall_s=$(python3 -c "print(round($t2-$t1,3))") pb4_wall_s=$(python3 -c "print(round($t3-$t2,3))")"; done
  for s in 2 4; do T0=$(date +%s.%N); timeout -s KILL 240 qemu-system-x86_64 -enable-kvm -smp $s -m 512 -cpu kvm64 -kernel $KI -initrd l3x.gz -append "console=ttyS0 panic=-1 nokaslr" -nographic -no-reboot -net none -object memory-backend-memfd,id=vram,size=512M,share=on -machine memory-backend=vram 2>&1 | stamp A${s}_memfd; done
  exit 0; fi
if [ "$MODE" = S3 ]; then rm -f /dev/shm/veil*; T0=$(date +%s.%N)
  env $E timeout -s KILL 900 ./dsmh home 7910 131072 2 2> home.log &
  sleep 0.5; timeout -s KILL 900 chroot /l2r env $SH VEIL_DSM_HOME=127.0.0.1:7910 VEIL_DSM_ID=1 $GQ -shm-path veilB -append "$GA" -local-cpu 1,start=1,iplist="127.0.0.1 127.0.0.1" > ap.log 2>&1 &
  sleep 1; timeout -s KILL 900 chroot /l2r env $SH VEIL_DSM_HOME=127.0.0.1:7910 VEIL_DSM_ID=0 $GQ -shm-path veilA -append "$GA" -local-cpu 1,start=0,iplist="127.0.0.1 127.0.0.1" 2>&1 | stamp S3_dsm
  sleep 2; grep -a "DSM node1 faults" ap.log | tail -1 | sed 's/^/[S3-ap] /' | while IFS= read -r x; do L "$x"; done; grep -a "DSM home t=" home.log | tail -1 | sed 's/^/[S3-home] /' | while IFS= read -r x; do L "$x"; done; pkill -9 -f dsmh; exit 0; fi
# ---- WireGuard mesh over KN runners: node i = 10.88.0.(i+1)
mkdir -p /etc/wireguard; (umask 077; wg genkey > /etc/wireguard/k); PUB=$(wg pubkey < /etc/wireguard/k)
read MIP MPORT < <(K stun 51820); K put vs-$RUN-wg-$G "{\"ip\":\"$MIP\",\"port\":$MPORT,\"pub\":\"$PUB\"}"
PEERS=$(K wait vs-$RUN-wg- $KN 3600) || { L "peers never arrived"; exit 1; }
ip link add wg0 type wireguard; wg set wg0 listen-port 51820 private-key /etc/wireguard/k; ip addr add 10.88.0.$((G+1))/24 dev wg0; ip link set wg0 mtu 1420 up
echo "$PEERS" | python3 -c "
import json,sys; d=json.load(sys.stdin)
for k,v in d.items():
  i=int(k.rsplit('-',1)[1]); p=json.loads(v)
  if i!=$G: print(i,p['ip'],p['port'],p['pub'])" | while read i ip port pub; do wg set wg0 peer $pub endpoint $ip:$port allowed-ips 10.88.0.$((i+1))/32 persistent-keepalive 1; done
sleep 4; for i in $(seq 0 $((KN-1))); do [ $i = $G ] || J "{\"kind\":\"wg_ping\",\"from\":$G,\"to\":$i,\"out\":\"$(ping -c 20 -i 0.2 10.88.0.$((i+1)) | tail -1)\"}"; done
IPL=""; for i in $(seq 0 $((NCOMP-1))); do IPL="$IPL 10.88.0.$((i+1))"; done; IPL=${IPL# }
if [ "$MODE" = R ]; then HOMEN=2; H2="VEIL_DSM_HOME2=10.88.0.4:7910 VEIL_DSM_FAILOVER_S=4"; else HOMEN=0; H2=""; fi
HIP=10.88.0.$((HOMEN+1)); XT=${XT:-900}; rm -f /dev/shm/veil*; T0=$(date +%s.%N)
homelog() { f=$1; tag=$2; (while sleep 20; do tail -2 $f | while IFS= read -r x; do L "[$tag] $x"; done; done &) ; }
if [ "$MODE" = R ] && [ $G = 3 ]; then env $E timeout -s KILL $XT ./dsmh mirror 7910 131072 $NCOMP 7911 2> mirror.log & homelog mirror.log mirror; K put vs-$RUN-mirror-0 1 >/dev/null
  K wait vs-$RUN-xdone- 1 $XT >/dev/null; sleep 3; grep -aE "TAKEOVER|RESUMED|resumed|lost" mirror.log | while IFS= read -r x; do L "[mirror] $x"; done; tail -2 mirror.log | while IFS= read -r x; do L "[mirror-final] $x"; done; exit 0; fi
if [ $G = $HOMEN ]; then
  MARG=""; [ "$MODE" = R ] && { K wait vs-$RUN-mirror- 1 600 >/dev/null; MARG=10.88.0.4:7911; }
  env $E timeout -s KILL $XT ./dsmh home 7910 131072 $NCOMP $MARG 2> home.log & HPID=$!; homelog home.log home; sleep 0.5; K put vs-$RUN-home-0 1 >/dev/null
  if [ "$MODE" = R ]; then K wait vs-$RUN-kill- 1 $XT >/dev/null; L "FAULT_INJECTION killing primary home runner state: $(grep -a 'DSM home t=' home.log | tail -1 | cut -c1-200)"; kill -9 $HPID; pkill -9 -f dsmh; ip link del wg0; L "FAULT_INJECTED primary home process killed + its network removed"; exit 0; fi
fi
if [ $G -lt $NCOMP ]; then K wait vs-$RUN-home- 1 600 >/dev/null; LC=$((SMP/NCOMP)); ST=$((G*LC))
  if [ $G = 0 ]; then (WATCHT="150 300 600" watch_mon 4444 &); timeout -s KILL $XT chroot /l2r env $SH VEIL_DSM_HOME=$HIP:7910 $H2 VEIL_DSM_ID=0 $GQ -shm-path veilA -append "$GA" -local-cpu $LC,start=0,iplist="$IPL" -monitor tcp:127.0.0.1:4444,server,nowait 2>&1 | stamp $MODE
    K put vs-$RUN-xdone-0 1 >/dev/null; sleep 4; [ -f home.log ] && grep -a "DSM home t=" home.log | tail -1 | sed 's/^/[home-final] /' | while IFS= read -r x; do L "$x"; done
  else timeout -s KILL $XT chroot /l2r env $SH VEIL_DSM_HOME=$HIP:7910 $H2 VEIL_DSM_ID=$G $GQ -shm-path veilB -append "$GA" -local-cpu $LC,start=$ST,iplist="$IPL" -monitor tcp:127.0.0.1:4445,server,nowait > ap.log 2>&1 & QP=$!; (WATCHT="150 300 600" watch_mon 4445 &)
    (while sleep 30; do grep -a "DSM node$G" ap.log | tail -3 | while IFS= read -r x; do L "[ap$G] $x"; done; done &); K wait vs-$RUN-xdone- 1 $XT >/dev/null; sleep 3; kill -9 $QP 2>/dev/null
    grep -aE "DSM node$G|FAILOVER|lost" ap.log | tail -4 | while IFS= read -r x; do L "[ap$G-final] $x"; done; fi
elif [ $G = $HOMEN ]; then K wait vs-$RUN-xdone- 1 $XT >/dev/null; fi
exit 0
