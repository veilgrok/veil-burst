#!/bin/bash
# 1-node GiantVM overhead profile: L3 guest under L2 (GiantVM 4.18 DSM kernel) with A/B toggles.
# Per config: host-wall pbench 1t (L2 /proc/uptime), L2 KVM debugfs exit counters over exactly the pbench window, L3 clocksource + timer IRQ delta.
set -u; J() { echo "VEIL_JSON $1"; }
CPU=$(lscpu | sed -n 's/Model name: *//p' | head -1); J "{\"kind\":\"node\",\"gidx\":$GIDX,\"cpu\":\"$CPU\",\"vmx\":$(grep -c -w vmx /proc/cpuinfo)}"
sudo apt-get -qq update >/dev/null; sudo apt-get -qq install -y qemu-system-x86 busybox-static cpio >/dev/null 2>&1; sudo chmod 666 /dev/kvm
mkdir l2; sudo tar -xzf art/l2root.tar.gz -C l2
cp art/pbench pbench 2>/dev/null || gcc -O2 -static -pthread -march=x86-64 -mtune=generic -o pbench gha/pbench.c; chmod +x pbench
for th in 1; do t0=$(date +%s.%N); o=$(./pbench $th 10000000); t1=$(date +%s.%N); echo "L1BARE $o host_wall_s=$(python3 -c "print(round($t1-$t0,3))")"; done
# stock QEMU inside the L2 rootfs (Ubuntu 18.04 qemu 2.11) for the stock-vs-gvm A/B
sudo cp /etc/resolv.conf l2/etc/resolv.conf; sudo mount -t proc p l2/proc
sudo chroot l2 bash -c 'apt-get -qq update >/dev/null 2>&1; DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends qemu-system-x86 >/dev/null 2>&1; ls -la /usr/bin/qemu-system-x86_64 && /usr/bin/qemu-system-x86_64 --version | head -1' | sed 's/^/STOCKQEMU /'
sudo umount l2/proc
mkdir -p l3/bin l3/proc l3/sys l3/dev; cp l2/bin/busybox pbench l3/bin/; for c in sh cat grep nproc poweroff mount head; do ln -sf busybox l3/bin/$c; done
cat > l3/init <<'EOF'
#!/bin/sh
mount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev; exec >/dev/ttyS0 2>&1
echo "L3 nproc=$(nproc) cs=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource) avail=$(cat /sys/devices/system/clocksource/clocksource0/available_clocksource | tr ' ' ,)"
echo "IRQ0 $(grep -E '^ *(0|LOC):' /proc/interrupts | tr -s ' ' | tr '\n' ';')"
echo PB_START 1
/bin/pbench 1 10000000
echo "IRQ1 $(grep -E '^ *(0|LOC):' /proc/interrupts | tr -s ' ' | tr '\n' ';')"
echo PB_DONE
poweroff -f
EOF
chmod +x l3/init; (cd l3 && find . | cpio -o -H newc 2>/dev/null | gzip > ../l3.gz)
sudo cp art/bzImage l3.gz pbench l2/opt/gvm/
CFGS=${CFGS:-"G_off S_on S_off G_on G_on_na G_off_mlock S_on_host"}
cat > init <<EOS
#!/bin/busybox sh
/bin/busybox mkdir -p /bb; /bin/busybox --install -s /bb
export PATH=/usr/sbin:/usr/bin:/sbin:/bin:/bb
mount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev; mount -t debugfs d /sys/kernel/debug 2>/dev/null; mount -t tracefs t /sys/kernel/debug/tracing 2>/dev/null
exec >/dev/ttyS0 2>&1 </dev/null
dmesg -n 1
echo L2 nproc=\$(nproc) \$(grep MemTotal /proc/meminfo) dsm_verbose=\$(cat /sys/module/kvm/parameters/dsm_verbose 2>&1) ept=\$(cat /sys/module/kvm_intel/parameters/ept 2>&1)
/opt/gvm/pbench 1 10000000 | sed "s/^/L2BARE /"
cd /opt/gvm
snap() { for f in /sys/kernel/debug/kvm/*; do [ -f "\$f" ] && echo "\${f##*/} \$(cat \$f)"; done; }
ktr() { T=/sys/kernel/debug/tracing; [ -d \$T/events/kvm/kvm_exit ] || { echo "TRACE none"; return; }; echo > \$T/trace; echo 1 > \$T/events/kvm/kvm_exit/enable; sleep 2; echo 0 > \$T/events/kvm/kvm_exit/enable; grep -o 'reason [A-Z_]*' \$T/trace | sort | uniq -c | sort -rn | head -8 | tr '\n' ';' | sed 's/^/TRACE2s /'; echo; }
run() { n=\$1; shift; echo "CFG_START \$n"; timeout 300 "\$@" -nographic -no-reboot -net none 2>&1 | while IFS= read -r l; do read up x </proc/uptime; case "\$l" in *PB_START*) t0=\$up; snap > /tmp/s0; (sleep 3; ktr) & ;; *PBENCH*) snap > /tmp/s1; echo "PROF \$n wall_s=\$(awk "BEGIN{print \$up-\$t0}") \$(awk 'NR==FNR{a[\$1]=\$2;next}{d=\$2-a[\$1]; if(d>0) printf "%s=%d ",\$1,d}' /tmp/s0 /tmp/s1)";; esac; echo "[\$n] @\$up \$l"; done; echo "CFG_END \$n"; }
K=bzImage; I=l3.gz; NA="console=ttyS0 panic=-1 noapic no_timer_check"; DF="console=ttyS0 panic=-1"
GVM="./qemu-system-x86_64 -L /opt/gvm -enable-kvm -smp 1 -m 512 -local-cpu 1,start=0,iplist=10.89.0.1 -kernel \$K -initrd \$I"
STK="/usr/bin/qemu-system-x86_64 -enable-kvm -smp 1 -m 512 -kernel \$K -initrd \$I"
for c in $CFGS; do case \$c in
 G_off) run G_off \$GVM -cpu kvm64 -machine pc,kernel-irqchip=off -append "\$NA";;
 G_off_mlock) run G_off_mlock \$GVM -cpu kvm64 -realtime mlock=on -machine pc,kernel-irqchip=off -append "\$NA";;
 G_on) run G_on \$GVM -cpu kvm64 -append "\$DF";;
 G_on_na) run G_on_na \$GVM -cpu kvm64 -append "\$NA";;
 S_on) [ -x /usr/bin/qemu-system-x86_64 ] && run S_on \$STK -cpu kvm64 -append "\$DF";;
 S_off) [ -x /usr/bin/qemu-system-x86_64 ] && run S_off \$STK -cpu kvm64 -machine pc,kernel-irqchip=off -append "\$NA";;
 S_on_host) [ -x /usr/bin/qemu-system-x86_64 ] && run S_on_host \$STK -cpu host -append "\$DF";;
 G_on_host) run G_on_host \$GVM -cpu host -append "\$DF";;
 G_on_invtsc) run G_on_invtsc \$GVM -cpu kvm64,+invtsc -append "\$DF";;
esac; done
/bb/poweroff -f
EOS
sudo cp init l2/init; sudo chmod +x l2/init; (cd l2 && sudo find . | sudo cpio -o -H newc 2>/dev/null | gzip -1 > ../l2.gz)
timeout ${L2T:-2400} qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 4096 -kernel art/bzImage -initrd l2.gz -append "console=ttyS0 rdinit=/init panic=-1" -nographic -no-reboot -net none 2>&1 | grep -a --line-buffered -E "^(L2|PROF|TRACE|CFG_|STOCK|\[[A-Za-z_]+\] @[0-9.]+ (L3 |IRQ|PBENCH|PB_|.*(clocksource|tsc|error|panic|Kernel)))"
J "{\"kind\":\"prof_done\",\"gidx\":$GIDX}"
