#!/bin/bash
# Two-level Veil DSM: stock QEMU 10 + KVM on the L1 runner; guest RAM = shmem memfd paged by the userfaultfd shim.
# gidx 1 = memory node (memsrv serves remote pages over WireGuard); gidx 0 = compute node (all vCPUs) + timed configs.
set -u; G=$GIDX; RUN=$GITHUB_RUN_ID; K() { python3 gha/kv.py "$@"; }
if [ "${NN:-2}" -gt 2 ]; then  # pick a same-region pair among NN runners (lowest gidx wins); others exit
  REG=$(curl -s -m 5 -H Metadata:true 'http://169.254.169.254/metadata/instance/compute/location?api-version=2021-02-01&format=text'); K put us-$RUN-reg-$G "$REG"
  ALL=$(K wait us-$RUN-reg- $NN 300) || ALL=$(K wait us-$RUN-reg- 2 5)
  SEL=$(echo "$ALL" | python3 -c "
import json,sys; d={int(k.rsplit('-',1)[1]):v for k,v in json.load(sys.stdin).items()}; ks=sorted(d); p=None
for i in ks:
  for j in ks:
    if j>i and d[i]==d[j] and p is None: p=(i,j)
p=p or (ks[0],ks[1]); print(p[0],p[1],d[p[0]],d[p[1]])")
  read PA PB RA_ RB_ <<< "$SEL"; echo "VEIL_JSON {\"kind\":\"pick\",\"gidx\":$G,\"pair\":\"$PA $PB\",\"regions\":\"$RA_ $RB_\"}"
  if [ "$G" = "$PA" ]; then G=0; elif [ "$G" = "$PB" ]; then G=1; else echo "not picked"; exit 0; fi
fi
P=$((1-G)); RUN=$GITHUB_RUN_ID; J() { echo "VEIL_JSON $1"; }; K() { python3 gha/kv.py "$@"; }
CPU=$(lscpu | sed -n 's/Model name: *//p' | head -1); J "{\"kind\":\"node\",\"gidx\":$G,\"cpu\":\"$CPU\",\"vmx\":$(grep -c -w vmx /proc/cpuinfo),\"region\":\"$(curl -s -m 5 -H Metadata:true 'http://169.254.169.254/metadata/instance/compute/location?api-version=2021-02-01&format=text')\"}"
gcc -O2 -shared -fPIC -o uffd_shim.so gha/uffd_shim.c -ldl -lpthread && gcc -O2 -o memsrv gha/memsrv.c && gcc -O2 -static -o memtouch gha/memtouch.c && gcc -O2 -static -pthread -o pbench gha/pbench.c || { echo "build failed"; exit 1; }
# --- WireGuard over hole-punched UDP
mkdir -p /etc/wireguard; (umask 077; wg genkey > /etc/wireguard/k); PUB=$(wg pubkey < /etc/wireguard/k)
read MIP MPORT < <(K stun 51820); K put us-$RUN-wg-$G "{\"ip\":\"$MIP\",\"port\":$MPORT,\"pub\":\"$PUB\"}"
PEER=$(K wait us-$RUN-wg- 2 300) || { echo "peer never arrived"; exit 1; }
read PIP PPORT PPUB < <(echo "$PEER" | python3 -c "import json,sys;d=json.load(sys.stdin);p=json.loads(d['us-$RUN-wg-$P']);print(p['ip'],p['port'],p['pub'])")
ip link add wg0 type wireguard; wg set wg0 listen-port 51820 private-key /etc/wireguard/k peer $PPUB endpoint $PIP:$PPORT allowed-ips 10.88.0.$((P+1))/32 persistent-keepalive 1
ip addr add 10.88.0.$((G+1))/24 dev wg0; ip link set wg0 mtu 1420 up; sleep 4
J "{\"kind\":\"wg_ping\",\"gidx\":$G,\"out\":\"$(ping -c 20 -i 0.2 10.88.0.$((P+1)) | tail -1)\"}"
if [ $G = 1 ]; then
  ./memsrv 7801 512 2> memsrv.log & SP=$!; sleep 1; K put us-$RUN-ready-1 1
  K wait us-$RUN-done- 1 2700 >/dev/null; kill $SP; sleep 1; cat memsrv.log | tail -20 | sed 's/^/[memsrv] /'; exit 0
fi
# ---- compute node: guest = same 4.18 kernel + busybox initrd as prof Phase A
cp art/pbench pbench 2>/dev/null; chmod +x pbench; MT=${MTMB:-352}
mkdir -p l3/bin l3/proc l3/sys l3/dev; cp /bin/busybox pbench memtouch l3/bin/; for c in sh cat grep nproc poweroff mount head; do ln -sf busybox l3/bin/$c; done
cat > l3/init <<EOI
#!/bin/sh
mount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev; exec >/dev/ttyS0 2>&1
echo "L3 nproc=\$(nproc) \$(grep MemTotal /proc/meminfo | tr -s ' ')"
echo PB_START 1
/bin/pbench 1 10000000
echo MT_START
/bin/memtouch $MT 3
echo PB_DONE
poweroff -f
EOI
chmod +x l3/init; (cd l3 && find . | cpio -o -H newc 2>/dev/null | gzip > ../l3.gz)
for i in 1 2; do t0=$(date +%s.%N); o=$(./pbench 1 10000000); t1=$(date +%s.%N); echo "PROF U_bare pb_wall_s=$(python3 -c "print(round($t1-$t0,3))") $o"; done
t0=$(date +%s.%N); o=$(./memtouch $MT 3); t1=$(date +%s.%N); echo "PROF U_bare mt_wall_s=$(python3 -c "print(round($t1-$t0,3))") $o"
Q="qemu-system-x86_64 -enable-kvm -smp 1 -m 512 -cpu kvm64 -kernel art/bzImage -initrd l3.gz -nographic -no-reboot -net none -object memory-backend-memfd,id=vram,size=512M,share=on -machine memory-backend=vram"
runU() { n=$1; shift; echo "CFG_START $n"; timeout 900 env "$@" $Q -append "console=ttyS0 panic=-1" 2>&1 | while IFS= read -r l; do now=$(date +%s.%N); case "$l" in *PB_START*) t0=$now;; *PBENCH*) echo "PROF $n pb_wall_s=$(python3 -c "print(round($now-$t0,3))")";; *MT_START*) t2=$now;; *MEMTOUCH*) echo "PROF $n mt_wall_s=$(python3 -c "print(round($now-$t2,3))") sum_ok=$(echo "$l" | grep -q "sum=$((MT*512))" && echo 1 || echo 0)";; esac; case "$l" in *L3*|*PBENCH*|*MEMTOUCH*|*UFFD*|qemu-system*|*rror*|*panic*|*"Out of memory"*) echo "[$n] $l";; esac; done; echo "CFG_END $n"; }
SH="LD_PRELOAD=$PWD/uffd_shim.so VEIL_UFFD_MATCH=memfd:"
K wait us-$RUN-ready- 1 300 >/dev/null || echo "memsrv never ready"
for c in ${CFGSU:-U_memfd U_uffd U_rem U_rem_evict U_memfd2 U_uffd2 U_rem2}; do case $c in
 U_memfd|U_memfd2) runU $c X=1;;
 U_uffd|U_uffd2) runU $c $SH;;
 U_rem|U_rem2) runU $c $SH VEIL_UFFD_REMOTE=10.88.0.2:7801 VEIL_UFFD_SPLIT_MB=${SPLIT:-128} VEIL_UFFD_RESIDENT_MB=0;;
 U_pipe_*|U_pev_*) IFS=_ read -r _ kind b r e <<< "$c"; RESMB=0; [ $kind = pev ] && RESMB=${e#e}; RESMB=${RESMB%%_*}; runU $c $SH VEIL_UFFD_PIPE=1 VEIL_UFFD_BATCH=${b#b} VEIL_UFFD_RA=${r#ra} VEIL_UFFD_REMOTE=10.88.0.2:7801 VEIL_UFFD_SPLIT_MB=${SPLIT:-128} VEIL_UFFD_RESIDENT_MB=$RESMB;;
 U_rem_b*) runU $c $SH VEIL_UFFD_BATCH=${c#U_rem_b} VEIL_UFFD_REMOTE=10.88.0.2:7801 VEIL_UFFD_SPLIT_MB=${SPLIT:-128} VEIL_UFFD_RESIDENT_MB=0;;
 U_rem_evict*) runU $c $SH VEIL_UFFD_REMOTE=10.88.0.2:7801 VEIL_UFFD_SPLIT_MB=${SPLIT:-128} VEIL_UFFD_RESIDENT_MB=${RES:-128};;
esac; done
K put us-$RUN-done-0 1; wg show wg0 | grep transfer
