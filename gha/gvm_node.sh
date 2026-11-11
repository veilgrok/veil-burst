#!/bin/bash
# One GiantVM node per runner: WireGuard over UDP hole punch (runner<->runner), tap-routed L2 DSM host, QEMU-gvm L3 spanning nodes.
set -u; GI=$GIDX; W=$GITHUB_WORKSPACE; RUN=$GITHUB_RUN_ID; J() { echo "VEIL_JSON $1"; }; K() { python3 gha/kv.py "$@"; }
CPU=$(lscpu | sed -n 's/Model name: *//p' | head -1); J "{\"kind\":\"node\",\"gidx\":$GI,\"cpu\":\"$CPU\",\"vmx\":$(grep -c -w vmx /proc/cpuinfo),\"svm\":$(grep -c -w svm /proc/cpuinfo),\"region\":\"$(curl -s -m 5 -H Metadata:true 'http://169.254.169.254/metadata/instance/compute/location?api-version=2021-02-01&format=text')\"}"
# roles: gidx 0/1 = cross-runner pair (WireGuard), gidx 2 = two GiantVM nodes on one runner ("local pair")
case $GI in 0) G=0; MODE=pair;; 1) G=1; MODE=pair;; *) G=0; MODE=solo;; esac; [ "${SOLO:-0}" = 1 ] && { G=0; MODE=solo; }
if [ "${PICK:-0}" = 1 ]; then  # every runner registers; pick the best cross-runner pair: Intel+same region > Intel+Intel > any
  REG=$(curl -s -m 5 -H Metadata:true 'http://169.254.169.254/metadata/instance/compute/location?api-version=2021-02-01&format=text')
  K put gvm-$GITHUB_RUN_ID-cpu-$GI "$(grep -c -w vmx /proc/cpuinfo) $REG"
  ALL=$(K wait gvm-$GITHUB_RUN_ID-cpu- $NNODES 240) || ALL=$(K wait gvm-$GITHUB_RUN_ID-cpu- 1 5)
  SEL=$(echo "$ALL" | python3 -c "
import json,sys,itertools; d=json.load(sys.stdin); n={int(k.rsplit('-',1)[1]):v.split() for k,v in d.items()}
best=None
for a,b in itertools.combinations(sorted(n),2):
  ia,ib=int(n[a][0])>0,int(n[b][0])>0; same=n[a][1:]==n[b][1:]
  sc=4*(ia and ib)+2*same+(ia or ib)
  if best is None or sc>best[0]: best=(sc,a,b)
print(best[1],best[2] if best else '')")
  read PA PB <<< "$SEL"; J "{\"kind\":\"pick\",\"gidx\":$GI,\"pair\":\"$PA $PB\",\"all\":$(echo "$ALL" | python3 -c 'import sys,json;print(json.dumps(sys.stdin.read().strip()))')}"
  if [ "${REQINTEL:-0}" = 1 ]; then NI=$(echo "$ALL" | python3 -c "import json,sys;d=json.load(sys.stdin);print(sum(1 for k,v in d.items() if k.rsplit('-',1)[1] in '$PA $PB'.split() and int(v.split()[0])>0))"); [ "$NI" = 2 ] || { J "{\"kind\":\"no_intel_pair\",\"gidx\":$GI}"; exit 0; }; fi
  if [ "$GI" = "$PA" ]; then G=0; MODE=pair; elif [ "$GI" = "$PB" ]; then G=1; MODE=pair; else echo "not picked"; exit 0; fi
fi
J "{\"kind\":\"role\",\"gidx\":$GI,\"mode\":\"$MODE\"}"
P=$((1-G))
sudo apt-get -qq update >/dev/null; sudo apt-get -qq install -y wireguard-tools qemu-system-x86 busybox-static cpio >/dev/null 2>&1; sudo chmod 666 /dev/kvm; sudo modprobe vhost_net; sudo chmod 666 /dev/vhost-net
# --- WireGuard over hole-punched UDP 51820
sudo mkdir -p /etc/wireguard; (umask 077; wg genkey | sudo tee /etc/wireguard/k >/dev/null); PUB=$(sudo cat /etc/wireguard/k | wg pubkey)
if [ $MODE = pair ]; then
read MIP MPORT < <(python3 gha/kv.py stun 51820)
K put gvm-$RUN-wg-$G "{\"ip\":\"$MIP\",\"port\":$MPORT,\"pub\":\"$PUB\"}"
PEER=$(K wait gvm-$RUN-wg- 2 300) || { echo "peer never arrived"; exit 1; }
read PIP PPORT PPUB < <(echo "$PEER" | python3 -c "import json,sys;d=json.load(sys.stdin);p=json.loads(d['gvm-$RUN-wg-$P']);print(p['ip'],p['port'],p['pub'])")
sudo ip link add wg0 type wireguard; sudo wg set wg0 listen-port 51820 private-key /etc/wireguard/k peer $PPUB endpoint $PIP:$PPORT allowed-ips 10.88.0.$((P+1))/32,10.89.0.$((P+1))/32 persistent-keepalive 1
sudo ip addr add 10.88.0.$((G+1))/24 dev wg0; sudo ip link set wg0 up; sudo ip link set wg0 mtu ${WGMTU:-1420}; sudo ip route add 10.89.0.$((P+1))/32 dev wg0
sudo wg show wg0 | grep -E "endpoint|handshake|transfer"; sleep 4
J "{\"kind\":\"wg_ping\",\"gidx\":$G,\"out\":\"$(ping -c 20 -i 0.2 10.88.0.$((P+1)) | tail -1)\"}"
sudo ip link set wg0 mtu 8920
for df in do dont; do o=$(ping -c 10 -i 0.2 -W 2 -s 4200 -M $df 10.88.0.$((P+1)) 2>&1 | grep -E "packets transmitted|rtt|error|too long" | tr '\n' ';' | tr -d '"'); J "{\"kind\":\"wg_bigping\",\"gidx\":$G,\"wg_mtu\":8920,\"size\":4200,\"df\":\"$df\",\"out\":\"$o\"}"; done
o=$(ping -c 5 -i 0.2 -W 2 -s 1372 -M do 10.88.0.$((P+1)) 2>&1 | grep -E "packets transmitted" | tr -d '"'); J "{\"kind\":\"wg_bigping\",\"gidx\":$G,\"wg_mtu\":8920,\"size\":1372,\"df\":\"do\",\"out\":\"$o\"}"
sudo ip link set wg0 mtu ${WGMTU:-1420}
fi
sudo sysctl -qw net.ipv4.ip_forward=1; sudo iptables -I FORWARD -j ACCEPT
mktap() { sudo ip tuntap add tap$1 mode tap user $(id -un); sudo ip addr add 10.90.$1.1/24 dev tap$1; sudo ip link set tap$1 mtu ${TAPMTU:-1500} up; sudo ip route add 10.89.0.$(($1+1))/32 dev tap$1; }
# --- L2 userland (ubuntu18.04 + QEMU-gvm); L3 initrd uses 18.04's baseline-x86-64 busybox and a -march=x86-64 pbench
mkdir l2; sudo tar -xzf art/l2root.tar.gz -C l2
if [ -x art/pbench ] || [ -f art/pbench ]; then cp art/pbench pbench; chmod +x pbench; else gcc -O2 -static -pthread -march=x86-64 -mtune=generic -o pbench gha/pbench.c; fi
for th in 1 2; do t0=$(date +%s.%N); o=$(./pbench $th 10000000); t1=$(date +%s.%N); echo "L1BARE $o host_wall_s=$(python3 -c "print(round($t1-$t0,3))")"; done
mkdir -p l3/bin l3/proc l3/sys l3/dev; cp l2/bin/busybox pbench l3/bin/; for c in sh cat grep nproc poweroff mount head; do ln -sf busybox l3/bin/$c; done
printf '#!/bin/sh\nmount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev; exec >/dev/ttyS0 2>&1\necho L3 nproc=$(nproc) $(grep MemTotal /proc/meminfo)\necho PB_START 1\n/bin/pbench 1 10000000\necho PB_START $(nproc)\n/bin/pbench $(nproc) 10000000\necho PB_START 1b\n/bin/pbench 1 10000000\necho PB_DONE\npoweroff -f\n' > l3/init; chmod +x l3/init
(cd l3 && find . | cpio -o -H newc 2>/dev/null | gzip > ../l3.gz)
sudo cp art/bzImage l3.gz pbench l2/opt/gvm/
NODES="10.89.0.1 10.89.0.2"
# mk_l2 NODE LOCALCPUS BASECPUS -> l2_NODE.gz   (L3: 512 MiB, mlock'd, earlyprintk serial; watchdog dumps monitor regs, vCPU kernel stacks, DSM debug)
L3MEM=${L3MEM:-512}; L3ARGS=${L3ARGS:-}
mk_l2() { local g=$1 lc=$2 bc=$3
cat > init <<EOS
#!/bin/busybox sh
/bin/busybox mkdir -p /bb; /bin/busybox --install -s /bb
export PATH=/usr/sbin:/usr/bin:/sbin:/bin:/bb
mount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev; mount -t debugfs d /sys/kernel/debug 2>/dev/null
exec >/dev/ttyS0 2>&1 </dev/null
dmesg -n 1
ip link set lo up; ip link set eth0 mtu ${TAPMTU:-1500} up; ip addr add 10.89.0.$((g+1))/32 dev eth0; ip route add 10.90.$g.1 dev eth0; ip route add default via 10.90.$g.1
echo L2 node=$g nproc=\$(nproc) vmx=\$(grep -c -w vmx /proc/cpuinfo) svm=\$(grep -c -w svm /proc/cpuinfo) \$(grep MemTotal /proc/meminfo)
/opt/gvm/pbench 1 10000000 | sed "s/^/L2BARE /"; /opt/gvm/pbench \$(nproc) 10000000 | sed 's/^/L2BARE /'
sleep 3; cd /opt/gvm
dbg() { echo "WATCH t=\$1"; printf 'info status\ninfo cpus\ninfo registers\n' | nc -w 2 127.0.0.1 4444 2>&1 | tr -d '\r' | grep -aE "VM status|CPU #|RIP=|CS =|CR0=" | head -12 | sed 's/^/MON /'
  for p in \$(pidof qemu-system-x86_64); do for t in /proc/\$p/task/*; do echo "TASK \$(cat \$t/comm) wchan=\$(cat \$t/wchan) state=\$(grep State \$t/status | cut -f2)"; head -6 \$t/stack | sed 's/^/  STK /'; done; done | head -60
  echo "DSMDBG lines=\$(dmesg | grep -ac -iE 'dsm|ivy')"; dmesg | grep -aiE 'dsm|ivy|ktcp' | head -40 | sed 's/^/DSMDBG /'; dmesg | grep -aiE 'dsm|ivy|ktcp' | tail -15 | sed 's/^/DSMTAIL /'; }
$( [ "${GVMSKIP:-0}" = 1 ] && echo "if false; then" )
echo 1 > /sys/module/kvm/parameters/dsm_verbose 2>/dev/null; echo DSMVERBOSE=\$(cat /sys/module/kvm/parameters/dsm_verbose 2>&1)
echo GVM_START \$(date +%s)
( stdbuf -oL -eL ./qemu-system-x86_64 -L /opt/gvm -enable-kvm -cpu kvm64 -smp $((2*lc)) -m $L3MEM -realtime mlock=on -machine pc,kernel-irqchip=off -local-cpu $lc,start=$((g*lc)),iplist="$NODES" -kernel bzImage -initrd l3.gz -append "console=ttyS0 earlyprintk=serial,ttyS0,115200 nokaslr panic=-1 loglevel=8 $L3ARGS" -nographic -no-reboot -net none -monitor tcp:127.0.0.1:4444,server,nowait 2>&1 | while IFS= read -r l; do read up x </proc/uptime; echo "L3OUT @\$up \$l"; done ) &
for t in ${WATCHT:-45 150 300 450 600 750}; do sleep \$((t - \${last:-0})); last=\$t; pidof qemu-system-x86_64 >/dev/null || break; dbg \$t; echo 0 > /sys/module/kvm/parameters/dsm_verbose 2>/dev/null; grep -aE "^(L3 |PBENCH)" /tmp/l3.out 2>/dev/null; done
pidof qemu-system-x86_64 >/dev/null && { echo GVM_KILL; killall qemu-system-x86_64; sleep 2; }
echo GVM_END \$(date +%s)
$( [ "${GVMSKIP:-0}" = 1 ] && echo "fi" )
if [ $g = 0 ] && [ "${BASE1:-1}" = 1 ]; then echo BASE1_START \$(date +%s); ./qemu-system-x86_64 -L /opt/gvm -enable-kvm -cpu kvm64 -smp $bc -m $L3MEM -machine pc,kernel-irqchip=off -local-cpu $bc,start=0,iplist="10.89.0.1" -kernel bzImage -initrd l3.gz -append "console=ttyS0 panic=-1 $L3ARGS" -nographic -no-reboot -net none 2>&1 | while IFS= read -r l; do read up x </proc/uptime; echo "B1OUT @\$up \$l"; done; echo BASE1_MID \$(date +%s); ./qemu-system-x86_64 -L /opt/gvm -enable-kvm -cpu kvm64 -smp 2 -m $L3MEM -machine pc,kernel-irqchip=off -local-cpu 2,start=0,iplist="10.89.0.1" -kernel bzImage -initrd l3.gz -append "console=ttyS0 panic=-1 $L3ARGS" -nographic -no-reboot -net none 2>&1 | while IFS= read -r l; do read up x </proc/uptime; echo "B2OUT @\$up \$l"; done; echo BASE1_END \$(date +%s); fi
/bb/poweroff -f
EOS
sudo cp init l2/init; sudo chmod +x l2/init; (cd l2 && sudo find . | sudo cpio -o -H newc 2>/dev/null | gzip -1 > ../l2_$g.gz); }
runl2() { timeout ${L2T:-1000} qemu-system-x86_64 -enable-kvm -cpu host -smp $2 -m $3 -kernel art/bzImage -initrd l2_$1.gz -append "console=ttyS0 rdinit=/init panic=-1" -nographic -no-reboot \
  -netdev tap,id=n0,ifname=tap$1,script=no,downscript=no,vhost=on -device virtio-net-pci,netdev=n0,mac=52:54:00:00:00:1$1 2>&1 | tee l2_$1.log | grep -a --line-buffered -E "^(L2|L3|GVM|B1|B2|BASE|WATCH|MON|TASK|  STK|DSM)|panic" | sed -u "s/^/[n$1] /"; }
if [ $MODE = pair ]; then
  export TAPMTU=$(( ${WGMTU:-1420} > 1500 ? ${WGMTU:-1420} - 100 : 1500 )); mktap $G
  if [ $G = 0 ] && [ "${BASEFIRST:-0}" = 1 ]; then GVMSKIP=1 mk_l2 0 1 1; echo "BASEFIRST: 1-node GiantVM baselines (1 and 2 vCPU) on this runner before the pair run"; runl2 0 4 12288; fi
  mk_l2 $G 1 1; K put gvm-$RUN-ready-$G 1; K wait gvm-$RUN-ready- 2 1200 >/dev/null
  runl2 $G 4 12288
else  # solo Intel runner: two GiantVM nodes as two L2 hosts on this runner (honest label: same physical runner)
  export TAPMTU=9000; mktap 0; mktap 1; mk_l2 0 1 1; mk_l2 1 1 1
  runl2 0 2 6144 & sleep 1; runl2 1 2 6144; wait
fi
J "{\"kind\":\"gvm_done\",\"gidx\":$GI,\"mode\":\"$MODE\"}"
