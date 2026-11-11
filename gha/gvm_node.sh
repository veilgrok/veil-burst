#!/bin/bash
# One GiantVM node per runner: WireGuard over UDP hole punch (runner<->runner), tap-routed L2 DSM host, QEMU-gvm L3 spanning nodes.
set -u; G=$GIDX; P=$((1-G)); W=$GITHUB_WORKSPACE; RUN=$GITHUB_RUN_ID; J() { echo "VEIL_JSON $1"; }; K() { python3 gha/kv.py "$@"; }
CPU=$(lscpu | sed -n 's/Model name: *//p' | head -1); J "{\"kind\":\"node\",\"gidx\":$G,\"cpu\":\"$CPU\",\"vmx\":$(grep -c -w vmx /proc/cpuinfo),\"svm\":$(grep -c -w svm /proc/cpuinfo),\"region\":\"$(curl -s -m 5 -H Metadata:true 'http://169.254.169.254/metadata/instance/compute/location?api-version=2021-02-01&format=text')\"}"
sudo apt-get -qq update >/dev/null; sudo apt-get -qq install -y wireguard-tools qemu-system-x86 busybox-static cpio >/dev/null 2>&1; sudo chmod 666 /dev/kvm
# --- WireGuard over hole-punched UDP 51820
sudo mkdir -p /etc/wireguard; (umask 077; wg genkey | sudo tee /etc/wireguard/k >/dev/null); PUB=$(sudo cat /etc/wireguard/k | wg pubkey)
read MIP MPORT < <(python3 gha/kv.py stun 51820)
K put gvm-$RUN-wg-$G "{\"ip\":\"$MIP\",\"port\":$MPORT,\"pub\":\"$PUB\"}"
PEER=$(K wait gvm-$RUN-wg- 2 300) || { echo "peer never arrived"; exit 1; }
read PIP PPORT PPUB < <(echo "$PEER" | python3 -c "import json,sys;d=json.load(sys.stdin);p=json.loads(d['gvm-$RUN-wg-$P']);print(p['ip'],p['port'],p['pub'])")
sudo ip link add wg0 type wireguard; sudo wg set wg0 listen-port 51820 private-key /etc/wireguard/k peer $PPUB endpoint $PIP:$PPORT allowed-ips 10.88.0.$((P+1))/32,10.89.0.$((P+1))/32 persistent-keepalive 1
sudo ip addr add 10.88.0.$((G+1))/24 dev wg0; sudo ip link set wg0 up; sudo ip route add 10.89.0.$((P+1))/32 dev wg0
sudo wg show wg0 | grep -E "endpoint|handshake|transfer"; sleep 4
J "{\"kind\":\"wg_ping\",\"gidx\":$G,\"out\":\"$(ping -c 20 -i 0.2 10.88.0.$((P+1)) | tail -1)\"}"
# --- tap for L2, routed (no NAT)
sudo ip tuntap add tap0 mode tap user $(id -un); sudo ip addr add 10.90.$G.1/24 dev tap0; sudo ip link set tap0 up
sudo ip route add 10.89.0.$((G+1))/32 dev tap0; sudo sysctl -qw net.ipv4.ip_forward=1; sudo iptables -I FORWARD -j ACCEPT
# --- L3 guest initrd (busybox + pbench)
gcc -O2 -static -pthread -o pbench gha/pbench.c
mkdir -p l3/bin l3/proc l3/sys l3/dev; cp /bin/busybox pbench l3/bin/; for c in sh cat grep nproc poweroff mount head; do ln -sf busybox l3/bin/$c; done
printf '#!/bin/sh\nmount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev; exec >/dev/ttyS0 2>&1\necho L3 nproc=$(nproc) $(grep MemTotal /proc/meminfo)\n/bin/pbench 1 10000000\n/bin/pbench $(nproc) 10000000\npoweroff -f\n' > l3/init; chmod +x l3/init
(cd l3 && find . | cpio -o -H newc 2>/dev/null | gzip > ../l3.gz)
# --- L2 initramfs = ubuntu18.04 userland with QEMU-gvm
mkdir l2; sudo tar -xzf art/l2root.tar.gz -C l2; sudo cp art/bzImage l3.gz l2/opt/gvm/; sudo cp pbench l2/opt/gvm/
NODES="10.89.0.1 10.89.0.2"; ME=10.89.0.$((G+1))
cat > init <<EOS
#!/bin/busybox sh
/bin/busybox mkdir -p /bb; /bin/busybox --install -s /bb
export PATH=/usr/sbin:/usr/bin:/sbin:/bin:/bb
mount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev
exec >/dev/ttyS0 2>&1 </dev/null
ip link set lo up; ip link set eth0 up; ip addr add $ME/32 dev eth0; ip route add 10.90.$G.1 dev eth0; ip route add default via 10.90.$G.1
echo L2 nproc=\$(nproc) vmx=\$(grep -c -w vmx /proc/cpuinfo) svm=\$(grep -c -w svm /proc/cpuinfo) \$(grep MemTotal /proc/meminfo); ls -l /dev/kvm
/opt/gvm/pbench 1 10000000 | sed "s/^/L2BARE /"; /opt/gvm/pbench \$(nproc) 10000000 | sed 's/^/L2BARE /'
sleep 2
echo GVM_START \$(date +%s)
cd /opt/gvm; ./qemu-system-x86_64 -L /opt/gvm -enable-kvm -cpu kvm64 -smp 8 -m 3072 -local-cpu 4,start=$((G*4)),iplist="$NODES" -kernel bzImage -initrd l3.gz -append "console=ttyS0 panic=-1" -nographic -no-reboot -net none 2>&1 | sed 's/^/L3OUT /'
echo GVM_END \$? \$(date +%s)
if [ $G = 0 ]; then echo BASE1_START \$(date +%s); ./qemu-system-x86_64 -L /opt/gvm -enable-kvm -cpu kvm64 -smp 4 -m 3072 -local-cpu 4,start=0,iplist="10.89.0.1" -kernel bzImage -initrd l3.gz -append "console=ttyS0 panic=-1" -nographic -no-reboot -net none 2>&1 | sed "s/^/B1OUT /"; echo BASE1_END \$(date +%s); fi
 dmesg | grep -i -E "dsm|ktcp" | tail -8 | sed 's/^/L2DMESG /'
/bb/poweroff -f
EOS
sudo cp init l2/init; sudo chmod +x l2/init
(cd l2 && sudo find . | sudo cpio -o -H newc 2>/dev/null | gzip -1 > ../l2.gz); ls -la l2.gz
K put gvm-$RUN-ready-$G 1; K wait gvm-$RUN-ready- 2 300 >/dev/null
timeout 1000 qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 12288 -kernel art/bzImage -initrd l2.gz -append "console=ttyS0 rdinit=/init panic=-1" -nographic -no-reboot \
  -netdev tap,id=n0,ifname=tap0,script=no,downscript=no -device e1000,netdev=n0 > l2.log 2>&1; tr -d "\r" < l2.log | grep -vE "^\[ *[0-9]+\.[0-9]+\] (ACPI|pci|PCI|clocksource|e820|BIOS|x86|NODE|Zone|On node|smpboot|Memory|devtmpfs|random|NET|TCP|UDP|audit|ata|scsi|usb|serial|input|rtc|hid|tsc|Freeing|Write|Key|Run|Spectre|Unpacking|Trying|cpuidle|thermal|ehci|ohci|uhci|i8042|mousedev|libata|sd |sr |cdrom|e1000|nf_|ip_tables|Initializing|Loading|registered|PM|AMD|smp|Mount|Hierarchical|rcu|Dentry|Inode|mem auto|NR_IRQS|Console|console|APIC|IOAPIC|percpu|pcpu|Built|Policy|kvm-clock|hpet|Calibrating|pid_max|LSM|SELinux|Yama|Last level|Performance|cblist|NMI|devres|Huh|DMA|workingset|zbud|io scheduler|shpchp|Non-volatile|loop|tun|libphy|VFIO|Asymmetric|PPP|SLIP|Bridge|Segment|sched_clock|Booting|bootconsole|tsc:|check:|Linux agpgart|agpgart|intel_pstate|ledtrig|drop_monitor|Initialise|microcode|IPI|Btrfs|AppArmor|evm|ima|Unstable|clk:|platform|EDAC|sched|software IO|crash|Command line|Kernel command|DMI|Hypervisor|tracing|kvm-guest|Detected|Lock|smpboot|cpu|CPU|futex|xor|raid6|workqueue|async_tx|alg|Block|io|vgaarb|SCSI|wmi|power|Fixed|floppy|Real Time|Freeing|pnp|mce|resource|system 00|clk|ftrace|kprobe|cryptd|gre|ipip|Initialise|virtio|8021q|NET:|Segment|lp:|ppdev|parport)" | tail -80
J "{\"kind\":\"gvm_done\",\"gidx\":$G}"
