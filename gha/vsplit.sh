#!/bin/bash
# vCPU split on the two-level path: QEMU-gvm (userspace APIC + IPI/SIPI/IO forwarding router) on STOCK runner KVM,
# guest RAM = per-node /dev/shm file kept coherent by veil-dsm (gha/bin/dsm_shim.so built vs glibc 2.27 + dsm home).
set -u; MODES=${MODES:-B S1 S2 X}; has() { [[ " $MODES " == *" $1 "* ]]; }; G=$GIDX; RUN=$GITHUB_RUN_ID; K() { python3 gha/kv.py "$@"; }; J() { echo "VEIL_JSON $1"; }
case " ${MODES:-} " in *" P "*) exec bash gha/pool.sh;; esac
case " ${MODES:-} " in *" L "*) [ -f gha/llm/vsplit-llm.sh ] && exec bash gha/llm/vsplit-llm.sh; echo "mode L needs gha/llm/vsplit-llm.sh (apply the veil-llm patch)"; exit 1;; esac
case " ${MODES:-} " in *" B2 "*|*" S3 "*|*" X2 "*|*" X3 "*|*" X4 "*|*" R "*) exec bash gha/vsplit2.sh;; esac
REG=$(curl -s -m 5 -H Metadata:true 'http://169.254.169.254/metadata/instance/compute/location?api-version=2021-02-01&format=text')
if [ "${NN:-2}" -gt 2 ]; then
  V=$(grep -qw vmx /proc/cpuinfo && echo intel || echo amd); K put vs-$RUN-reg-$G "$REG|$V"; ALL=$(K wait vs-$RUN-reg- $NN 300) || ALL=$(K wait vs-$RUN-reg- 2 5)
  read PA PB <<< "$(echo "$ALL" | python3 -c "
import json,sys; d={int(k.rsplit('-',1)[1]):v.lower() for k,v in json.load(sys.stdin).items()}; ks=sorted(d); p=None
for want in ('intel',None):  # prefer a same-region Intel pair (eptad=0 removes page-table walk writes)
  for i in ks:
    for j in ks:
      if j>i and d[i]==d[j] and p is None and (want is None or d[i].endswith('|intel')): p=(i,j)
print(*(p or ('none','none')))")"
  J "{\"kind\":\"pick\",\"gidx\":$G,\"pair\":\"$PA $PB\"}"
  [ "$PA" = none ] && { L0() { python3 gha/kv.py put vs-$RUN-log-x-$(date +%s%N) "$*" >/dev/null 2>&1; }; [ "$GIDX" = 0 ] && L0 "NOPAIR no same-region pair: $(echo $ALL | tr -d \"{}\")"; echo "no same-region pair"; exit 0; }
  if [ "$G" = "$PA" ]; then G=0; elif [ "$G" = "$PB" ]; then G=1; else echo "not picked"; exit 0; fi
fi
LQ=0; L() { echo "$*"; LQ=$((LQ+1)); (K put vs-$RUN-log-$G-$(date +%s%N) "$*" >/dev/null 2>&1 &) ; }
P=$((1-G)); L "NODE cpu=$(lscpu | sed -n "s/Model name: *//p" | head -1) region=$REG eptad=$(cat /sys/module/kvm_intel/parameters/eptad 2>/dev/null || echo na)"; J "{\"kind\":\"node\",\"gidx\":$G,\"cpu\":\"$(lscpu | sed -n 's/Model name: *//p' | head -1)\",\"region\":\"$REG\",\"eptad\":\"$(cat /sys/module/kvm_intel/parameters/eptad 2>/dev/null || echo na)\"}"
gcc -O2 -pthread -o dsm gha/dsm.c && gcc -O2 -static -o memtouch gha/memtouch.c && gcc -O2 -static -pthread -o xcheck gha/xcheck.c && gcc -O2 -static -pthread -o pbench gha/pbench.c || { echo build failed; exit 1; }
mkdir -p /l2r && tar -xzf art/l2root.tar.gz -C /l2r && mkdir -p /l2r/w && mount --bind $PWD /l2r/w && mount --rbind /dev /l2r/dev && mount -t proc p /l2r/proc || { echo chroot setup failed; exit 1; }
mkdir -p l3/bin l3/proc l3/sys l3/dev; cp /l2r/bin/busybox pbench memtouch xcheck l3/bin/; for c in sh cat grep nproc poweroff mount head tr; do ln -sf busybox l3/bin/$c; done
cat > l3/init <<'EOI'
#!/bin/sh
mount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev; exec >/dev/ttyS0 2>&1
[ "$(nproc)" = 1 ] && [ -e /sys/devices/system/cpu/cpu1/online ] && { echo CPU1_ON_START; echo 1 > /sys/devices/system/cpu/cpu1/online; echo CPU1_ON_DONE rc=$?; }
echo "L3 nproc=$(nproc) $(grep MemTotal /proc/meminfo | tr -s ' ') cs=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)"
echo PB1_START; /bin/pbench 1 10000000
echo PB2_START; /bin/pbench 2 10000000
echo MT_START; /bin/memtouch 128 3
echo XC_START; /bin/xcheck 32
echo PB_DONE
poweroff -f
EOI
chmod +x l3/init; (cd l3 && find . | cpio -o -H newc 2>/dev/null | gzip > ../l3x.gz)
# stamp + summarize: host wall time between markers on the BSP serial
stamp() { n=$1; while IFS= read -r l; do now=$(date +%s.%N); case "$l" in *PB1_START*) a=$now;; *PB2_START*) b=$now; L "PROF $n pb1_wall_s=$(python3 -c "print(round($b-$a,3))")";; *MT_START*) c=$now; L "PROF $n pb2_wall_s=$(python3 -c "print(round($c-$b,3))")";; *XC_START*) d=$now; L "PROF $n mt_wall_s=$(python3 -c "print(round($d-$c,3))")";; *PB_DONE*) L "PROF $n xc_wall_s=$(python3 -c "print(round($now-$d,3))")"; (sleep 3; pkill -9 -f opt/gvm/qemu-system; pkill -9 -f "qemu-system-x86_64 -enable-kvm") & ;; esac
  case "$l" in *L3*|*PBENCH*|*MEMTOUCH*|*XCHECK*|*"Total of"*|*"Command line"*|*rror*|*panic*|*DSM*|*"set kvmclock"*|*"RTT"*|*smp*|*SMP*|*CPU1*|*CPU1_ON*|*"x86:"*|*lockup*|*rcu*|*Stuck*|*stuck*|*NMI*|*"Run /init"*|*Freeing*) L "[$n] $(python3 -c "print(round($now-${T0:-$now},1))")s $l";; esac; done; }
GQ="/opt/gvm/qemu-system-x86_64 -L /opt/gvm -enable-kvm -machine pc,kernel-irqchip=off -smp 2 -m 512 -kernel /w/${KIMG:-gha/bin/bz-4.18.20-hz100-nohzfull} -initrd /w/l3x.gz -nographic -no-reboot -net none"
GA="console=ttyS0 panic=-1 no_timer_check noapic ${GAX:-no-kvmclock nokaslr tsc=reliable lpj=24000000}"; GAS="$GA maxcpus=1 nohz_full=1 rcu_nocbs=1"  # split: boot on CPU0, init onlines CPU1
SHIM="LD_PRELOAD=/w/gha/bin/dsm_shim.so VEIL_DSM_PREFILL=1 VEIL_DSM_MATCH=veil"
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
run_split_local() { n=$1; shm0=$2; shm1=$3; env0=$4; T0=$(date +%s.%N); rm -f /dev/shm/veil*
  [ -n "$env0" ] && { VEIL_DSM_DUMP=1 VEIL_DSM_INVQ=${INVQ:-1} VEIL_DSM_REPSILENT=${REPSILENT:-2048} VEIL_DSM_REPMIN=${REPMIN:-16} VEIL_DSM_PREFILL=1 timeout -s KILL ${ST:-600} ./dsm home 7910 131072 2> home.log & }
  sleep 0.5; timeout -s KILL ${ST:-600} chroot /l2r env $env0 VEIL_DSM_HOME=127.0.0.1:7910 VEIL_DSM_ID=1 $GQ -shm-path $shm1 -append "$GAS" -local-cpu 1,start=1,iplist="127.0.0.1 127.0.0.1" -monitor tcp:127.0.0.1:4445,server,nowait > ap_$n.log 2>&1 &
  sleep 1; (WATCHT="60 180 400" watch_mon 4444 4445 &) ; timeout -s KILL ${ST:-600} chroot /l2r env $env0 VEIL_DSM_HOME=127.0.0.1:7910 VEIL_DSM_ID=0 $GQ -shm-path $shm0 -append "$GAS" -local-cpu 1,start=0,iplist="127.0.0.1 127.0.0.1" -monitor tcp:127.0.0.1:4444,server,nowait 2>&1 | stamp $n
  wait; grep -a "DSM node1" ap_$n.log | tail -1 | sed "s/^/[$n-ap] /"; [ -n "$env0" ] && tail -3 home.log | sed "s/^/[$n-home] /"; pkill -f "dsm home" ; pkill -9 -f opt/gvm/qemu; L "CFG_END $n"; }
if [ $G = 0 ] && has B; then
  for i in 1 2; do t0=$(date +%s.%N); o=$(./pbench 1 10000000); t1=$(date +%s.%N); o2=$(./pbench 2 10000000); t2=$(date +%s.%N); L "PROF L1bare pb1_wall_s=$(python3 -c "print(round($t1-$t0,3))") pb2_wall_s=$(python3 -c "print(round($t2-$t1,3))")"; done
  # two-level stock QEMU 10 memfd, 2 vCPUs (the comparison target)
  T0=$(date +%s.%N); timeout -s KILL 180 qemu-system-x86_64 -enable-kvm -smp 2 -m 512 -cpu kvm64 -kernel ${KIMG:-gha/bin/bz-4.18.20-hz100-nohzfull} -initrd l3x.gz -append "console=ttyS0 panic=-1" -nographic -no-reboot -net none -object memory-backend-memfd,id=vram,size=512M,share=on -machine memory-backend=vram 2>&1 | stamp A2_memfd; L "CFG_END A2"
  # QEMU-gvm single process 2 vCPUs, userspace APIC (cost of the gvm VMM itself)
  T0=$(date +%s.%N); timeout -s KILL 240 chroot /l2r $GQ -append "$GA" 2>&1 | stamp G1_gvm; L "CFG_END G1"
fi
if [ $G = 0 ]; then
  has S1 && ST=300 run_split_local S1_shm veilram veilram ""
  has S2 && ST=900 run_split_local S2_dsm veilA veilB "$SHIM"
fi
has X || { [ $G = 0 ] && K put vs-$RUN-xdone-0 1; exit 0; }
# ---- cross-runner split over WireGuard: node0 = BSP + devices + dsm home, node1 = AP
mkdir -p /etc/wireguard; (umask 077; wg genkey > /etc/wireguard/k); PUB=$(wg pubkey < /etc/wireguard/k)
read MIP MPORT < <(K stun 51820); K put vs-$RUN-wg-$G "{\"ip\":\"$MIP\",\"port\":$MPORT,\"pub\":\"$PUB\"}"
PEER=$(K wait vs-$RUN-wg- 2 3600) || { echo "peer never arrived"; exit 1; }
read PIP PPORT PPUB < <(echo "$PEER" | python3 -c "import json,sys;d=json.load(sys.stdin);p=json.loads(d['vs-$RUN-wg-$P']);print(p['ip'],p['port'],p['pub'])")
ip link add wg0 type wireguard; wg set wg0 listen-port 51820 private-key /etc/wireguard/k peer $PPUB endpoint $PIP:$PPORT allowed-ips 10.88.0.$((P+1))/32 persistent-keepalive 1
ip addr add 10.88.0.$((G+1))/24 dev wg0; ip link set wg0 mtu 1420 up; sleep 4
J "{\"kind\":\"wg_ping\",\"gidx\":$G,\"out\":\"$(ping -c 20 -i 0.2 10.88.0.$((P+1)) | tail -1)\"}"
rm -f /dev/shm/veil*; T0=$(date +%s.%N); XT=${XT:-1800}
if [ $G = 0 ]; then
  VEIL_DSM_DUMP=1 VEIL_DSM_INVQ=${INVQ:-1} VEIL_DSM_REPSILENT=${REPSILENT:-2048} VEIL_DSM_REPMIN=${REPMIN:-16} VEIL_DSM_PREFILL=1 timeout -s KILL $XT ./dsm home 7910 131072 2> home.log & sleep 0.5; K put vs-$RUN-home-0 1
  (WATCHT="120 300 600 900 1200 1500" watch_mon 4444 &)
  timeout -s KILL $XT chroot /l2r env $SHIM VEIL_DSM_HOME=127.0.0.1:7910 VEIL_DSM_ID=0 $GQ -shm-path veilA -append "$GAS" -local-cpu 1,start=0,iplist="10.88.0.1 10.88.0.2" -monitor tcp:127.0.0.1:4444,server,nowait 2>&1 | stamp X_wg
  K put vs-$RUN-xdone-0 1; sleep 3; tail -3 home.log | sed 's/^/[X_wg-home] /'; pkill -f "dsm home"; wg show wg0 | grep transfer
else
  K wait vs-$RUN-home- 1 2400 >/dev/null
  timeout -s KILL $XT chroot /l2r env $SHIM VEIL_DSM_HOME=10.88.0.1:7910 VEIL_DSM_ID=1 $GQ -shm-path veilB -append "$GAS" -local-cpu 1,start=1,iplist="10.88.0.1 10.88.0.2" -monitor tcp:127.0.0.1:4445,server,nowait > ap_x.log 2>&1 &
  QP=$!; (WATCHT="120 300 600 900 1200 1500" watch_mon 4445 &); (while sleep 60; do L "[X_ap] $(grep -a "DSM node1 faults" ap_x.log | tail -1)"; done &) ; K wait vs-$RUN-xdone- 1 $XT >/dev/null; sleep 5; kill $QP 2>/dev/null; grep -aE "DSM|RTT|kvmclock|rror" ap_x.log | tail -6 | sed 's/^/[X_wg-ap] /'
fi
