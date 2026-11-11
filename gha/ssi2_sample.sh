#!/bin/bash
# CPU / nested-virt sampling + direct p2p net test + (if kernel artifact) GiantVM DSM kernel as nested hypervisor guest.
set -u; G=$GIDX; W=$GITHUB_WORKSPACE; J() { echo "VEIL_JSON $1"; }
CPU=$(lscpu | sed -n 's/Model name: *//p' | head -1); NI=$(cat /sys/module/kvm_intel/parameters/nested 2>/dev/null); NA=$(cat /sys/module/kvm_amd/parameters/nested 2>/dev/null)
J "{\"kind\":\"cpu\",\"gidx\":$G,\"cpu\":\"$CPU\",\"vmx\":$(grep -c -w vmx /proc/cpuinfo),\"svm\":$(grep -c -w svm /proc/cpuinfo),\"nested_intel\":\"$NI\",\"nested_amd\":\"$NA\",\"region_hint\":\"$(curl -s -m 5 -H Metadata:true 'http://169.254.169.254/metadata/instance/compute/location?api-version=2021-02-01&format=text' || true)\"}"
timeout 240 python3 gha/netp2p.py > p2p.log 2>&1 &
sudo apt-get -qq update >/dev/null; sudo apt-get -qq install -y qemu-system-x86 busybox-static cpio >/dev/null 2>&1; sudo chmod 666 /dev/kvm
if [ -f art/bzImage ]; then
  gcc -static -O2 -o kvmtest gha/kvmtest.c
  mkdir -p ir/bin ir/dev ir/proc ir/sys; cp /bin/busybox kvmtest ir/bin/; for c in sh ls cat grep dmesg mount poweroff nproc free head; do ln -sf busybox ir/bin/$c; done
  printf '#!/bin/sh\nmount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev\necho L2BOOT $(head -c 40 /proc/version)\necho L2FLAGS vmx=$(grep -c -w vmx /proc/cpuinfo) svm=$(grep -c -w svm /proc/cpuinfo) nproc=$(nproc) memkb=$(grep MemTotal /proc/meminfo)\n/bin/kvmtest\ndmesg | grep -i -E "kvm|dsm|vmx|svm" | head -8\npoweroff -f\n' > ir/init; chmod +x ir/init
  (cd ir && find . | cpio -o -H newc 2>/dev/null | gzip > ../ir.gz)
  timeout 120 qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4096 -kernel art/bzImage -initrd ir.gz -append "console=ttyS0 panic=-1" -nographic -no-reboot > l2.log 2>&1
  grep -E "L2|KVMTEST|kvm|dsm|vmx|svm|panic" l2.log | head -14
fi
wait; cat p2p.log
