#!/bin/bash
# One GiantVM node per runner: WireGuard over UDP hole punch (runner<->runner), tap-routed L2 DSM host, QEMU-gvm L3 spanning nodes.
set -u; GI=$GIDX; W=$GITHUB_WORKSPACE; RUN=$GITHUB_RUN_ID; J() { echo "VEIL_JSON $1"; }; K() { python3 gha/kv.py "$@"; }
CPU=$(lscpu | sed -n 's/Model name: *//p' | head -1); J "{\"kind\":\"node\",\"gidx\":$GI,\"cpu\":\"$CPU\",\"vmx\":$(grep -c -w vmx /proc/cpuinfo),\"svm\":$(grep -c -w svm /proc/cpuinfo),\"region\":\"$(curl -s -m 5 -H Metadata:true 'http://169.254.169.254/metadata/instance/compute/location?api-version=2021-02-01&format=text')\"}"
# roles: gidx 0/1 = cross-runner pair (WireGuard), gidx 2 = two GiantVM nodes on one runner ("local pair")
case $GI in 0) G=0; MODE=pair;; 1) G=1; MODE=pair;; *) G=0; MODE=solo;; esac
J "{\"kind\":\"role\",\"gidx\":$GI,\"mode\":\"$MODE\"}"
P=$((1-G))
sudo apt-get -qq update >/dev/null; sudo apt-get -qq install -y wireguard-tools qemu-system-x86 busybox-static cpio >/dev/null 2>&1; sudo chmod 666 /dev/kvm
# --- WireGuard over hole-punched UDP 51820
sudo mkdir -p /etc/wireguard; (umask 077; wg genkey | sudo tee /etc/wireguard/k >/dev/null); PUB=$(sudo cat /etc/wireguard/k | wg pubkey)
if [ $MODE = pair ]; then
read MIP MPORT < <(python3 gha/kv.py stun 51820)
K put gvm-$RUN-wg-$G "{\"ip\":\"$MIP\",\"port\":$MPORT,\"pub\":\"$PUB\"}"
PEER=$(K wait gvm-$RUN-wg- 2 300) || { echo "peer never arrived"; exit 1; }
read PIP PPORT PPUB < <(echo "$PEER" | python3 -c "import json,sys;d=json.load(sys.stdin);p=json.loads(d['gvm-$RUN-wg-$P']);print(p['ip'],p['port'],p['pub'])")
sudo ip link add wg0 type wireguard; sudo wg set wg0 listen-port 51820 private-key /etc/wireguard/k peer $PPUB endpoint $PIP:$PPORT allowed-ips 10.88.0.$((P+1))/32,10.89.0.$((P+1))/32 persistent-keepalive 1
sudo ip addr add 10.88.0.$((G+1))/24 dev wg0; sudo ip link set wg0 up; sudo ip route add 10.89.0.$((P+1))/32 dev wg0
sudo wg show wg0 | grep -E "endpoint|handshake|transfer"; sleep 4
J "{\"kind\":\"wg_ping\",\"gidx\":$G,\"out\":\"$(ping -c 20 -i 0.2 10.88.0.$((P+1)) | tail -1)\"}"
fi
sudo sysctl -qw net.ipv4.ip_forward=1; sudo iptables -I FORWARD -j ACCEPT
mktap() { sudo ip tuntap add tap$1 mode tap user $(id -un); sudo ip addr add 10.90.$1.1/24 dev tap$1; sudo ip link set tap$1 up; sudo ip route add 10.89.0.$(($1+1))/32 dev tap$1; }
# --- L2 userland (ubuntu18.04 + QEMU-gvm); L3 initrd uses 18.04's baseline-x86-64 busybox and a -march=x86-64 pbench
mkdir l2; sudo tar -xzf art/l2root.tar.gz -C l2
gcc -O2 -static -pthread -march=x86-64 -mtune=generic -o pbench gha/pbench.c
mkdir -p l3/bin l3/proc l3/sys l3/dev; cp l2/bin/busybox pbench l3/bin/; for c in sh cat grep nproc poweroff mount head; do ln -sf busybox l3/bin/$c; done
printf '#!/bin/sh\nmount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev; exec >/dev/ttyS0 2>&1\necho L3 nproc=$(nproc) $(grep MemTotal /proc/meminfo)\n/bin/pbench 1 10000000\n/bin/pbench $(nproc) 10000000\npoweroff -f\n' > l3/init; chmod +x l3/init
(cd l3 && find . | cpio -o -H newc 2>/dev/null | gzip > ../l3.gz)
sudo cp art/bzImage l3.gz pbench l2/opt/gvm/
NODES="10.89.0.1 10.89.0.2"
# mk_l2 NODE LOCALCPUS BASECPUS -> l2_NODE.gz
mk_l2() { local g=$1 lc=$2 bc=$3
cat > init <<EOS
#!/bin/busybox sh
/bin/busybox mkdir -p /bb; /bin/busybox --install -s /bb
export PATH=/usr/sbin:/usr/bin:/sbin:/bin:/bb
mount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev
exec >/dev/ttyS0 2>&1 </dev/null
ip link set lo up; ip link set eth0 up; ip addr add 10.89.0.$((g+1))/32 dev eth0; ip route add 10.90.$g.1 dev eth0; ip route add default via 10.90.$g.1
echo L2 node=$g nproc=\$(nproc) vmx=\$(grep -c -w vmx /proc/cpuinfo) \$(grep MemTotal /proc/meminfo)
/opt/gvm/pbench 1 10000000 | sed "s/^/L2BARE /"; /opt/gvm/pbench \$(nproc) 10000000 | sed 's/^/L2BARE /'
sleep 3; cd /opt/gvm
echo GVM_START \$(date +%s); ./qemu-system-x86_64 -L /opt/gvm -enable-kvm -cpu kvm64 -smp $((2*lc)) -m 2048 -local-cpu $lc,start=$((g*lc)),iplist="$NODES" -kernel bzImage -initrd l3.gz -append "console=ttyS0 panic=-1" -nographic -no-reboot -net none 2>&1 | sed 's/^/L3OUT /'
echo GVM_END \$(date +%s)
if [ $g = 0 ]; then echo BASE1_START \$(date +%s); ./qemu-system-x86_64 -L /opt/gvm -enable-kvm -cpu kvm64 -smp $bc -m 2048 -local-cpu $bc,start=0,iplist="10.89.0.1" -kernel bzImage -initrd l3.gz -append "console=ttyS0 panic=-1" -nographic -no-reboot -net none 2>&1 | sed "s/^/B1OUT /"; echo BASE1_END \$(date +%s); fi
dmesg | grep -i -E "dsm|ktcp" | tail -8 | sed 's/^/L2DMESG /'
/bb/poweroff -f
EOS
sudo cp init l2/init; sudo chmod +x l2/init; (cd l2 && sudo find . | sudo cpio -o -H newc 2>/dev/null | gzip -1 > ../l2_$g.gz); }
runl2() { timeout 900 qemu-system-x86_64 -enable-kvm -cpu host -smp $2 -m $3 -kernel art/bzImage -initrd l2_$1.gz -append "console=ttyS0 rdinit=/init panic=-1" -nographic -no-reboot \
  -netdev tap,id=n0,ifname=tap$1,script=no,downscript=no -device e1000,netdev=n0,mac=52:54:00:00:00:1$1 2>&1 | tee l2_$1.log | grep -a --line-buffered -E "^(L2|L3|GVM|B1|BASE)|dsm|DSM|panic" | sed -u "s/^/[n$1] /"; }
if [ $MODE = pair ]; then
  mktap $G; mk_l2 $G 4 4; K put gvm-$RUN-ready-$G 1; K wait gvm-$RUN-ready- 2 300 >/dev/null
  runl2 $G 4 12288
else  # solo Intel runner: two GiantVM nodes as two L2 hosts on this runner (honest label: same physical runner)
  mktap 0; mktap 1; mk_l2 0 2 2; mk_l2 1 2 2
  runl2 0 2 6144 & sleep 1; runl2 1 2 6144; wait
fi
J "{\"kind\":\"gvm_done\",\"gidx\":$GI,\"mode\":\"$MODE\"}"
