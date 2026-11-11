#!/bin/bash
# GiantVM build in era-appropriate toolchain (ubuntu:18.04 container), then boot the DSM host kernel as a nested L2 KVM host.
set -u; J() { echo "VEIL_JSON $1"; }; G=$GIDX; W=$GITHUB_WORKSPACE
git clone -q --depth 1 https://github.com/ememos/GiantVM $W/gvm
if [ "$G" = "0" ]; then
  t=$(date +%s)
  docker run --rm -v $W/gvm:/g -w /g/Linux-DSM-4.18 ubuntu:18.04 bash -c '
    export DEBIAN_FRONTEND=noninteractive; apt-get -qq update >/dev/null; apt-get -qq install -y build-essential flex bison libelf-dev libssl-dev bc kmod >/dev/null
    gcc --version | head -1; make -s defconfig; ./scripts/config -e KVM -e KVM_AMD -e KVM_INTEL -e VIRTIO_PCI -e VIRTIO_NET -e BLK_DEV_INITRD -e DEVTMPFS -e DEVTMPFS_MOUNT -e KVM_DSM -e KTCP_NETWORK -d KRDMA_NETWORK; make -s olddefconfig
    grep -E "^CONFIG_KVM(_AMD|_INTEL|_DSM)?=|KTCP|KRDMA" .config | head -8
    make -s -j$(nproc) bzImage > /g/kbuild.log 2>&1; echo rc=$?' 2>&1 | tail -12
  K=$W/gvm/Linux-DSM-4.18/arch/x86/boot/bzImage
  J "{\"kind\":\"gvm_kernel_build_1804\",\"s\":$(( $(date +%s)-t )),\"bzImage\":$([ -f $K ] && echo 1 || echo 0)}"
  grep -iE "undefined|error" $W/gvm/kbuild.log | sort | uniq -c | sort -rn | head -15
  if [ -f $K ]; then
    sudo apt-get -qq update >/dev/null; sudo apt-get -qq install -y qemu-system-x86 busybox-static cpio >/dev/null 2>&1; sudo chmod 666 /dev/kvm
    mkdir -p ir/bin ir/dev ir/proc ir/sys; cp /bin/busybox ir/bin/; for c in sh ls cat grep dmesg mount poweroff nproc free; do ln -sf busybox ir/bin/$c; done
    printf '#!/bin/sh\nmount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev\necho L2BOOT kernel=$(cat /proc/version | cut -c1-60)\necho L2KVM $(ls -l /dev/kvm 2>&1)\necho L2SVM $(grep -c -w svm /proc/cpuinfo) NPROC $(nproc)\ndmesg | grep -i -m5 -E "kvm|dsm"\npoweroff -f\n' > ir/init; chmod +x ir/init
    (cd ir && find . | cpio -o -H newc 2>/dev/null | gzip > ../ir.gz)
    t=$(date +%s); timeout 120 qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 2048 -kernel $K -initrd ir.gz -append "console=ttyS0 panic=-1" -nographic -no-reboot > l2.log 2>&1
    J "{\"kind\":\"gvm_l2_boot\",\"rc\":$?,\"s\":$(( $(date +%s)-t ))}"; grep -E "L2|kvm|dsm|Kernel panic" l2.log | head -12
  fi
else
  t=$(date +%s)
  docker run --rm -v $W/gvm:/g -w /g/QEMU-gvm-vcpupin ubuntu:18.04 bash -c '
    export DEBIAN_FRONTEND=noninteractive; apt-get -qq update >/dev/null; apt-get -qq install -y build-essential python pkg-config libglib2.0-dev libpixman-1-dev zlib1g-dev libfdt-dev 2>&1 | tail -3; pkg-config --modversion glib-2.0
    ./configure --target-list=x86_64-softmmu --disable-werror > /g/qconf.log 2>&1; echo configure_rc=$?; tail -25 /g/qconf.log; which pkg-config; pkg-config --libs glib-2.0; make -s -j$(nproc) > /g/qbuild.log 2>&1; echo rc=$?
    ls -la x86_64-softmmu/qemu-system-x86_64 2>&1; x86_64-softmmu/qemu-system-x86_64 --version 2>&1 | head -1; x86_64-softmmu/qemu-system-x86_64 -help 2>&1 | grep -iE "local-cpu|dsm|gvm|router|cluster" | head -5' 2>&1 | tail -10
  J "{\"kind\":\"gvm_qemu_build_1804\",\"s\":$(( $(date +%s)-t )),\"bin\":$([ -f $W/gvm/QEMU-gvm-vcpupin/x86_64-softmmu/qemu-system-x86_64 ] && echo 1 || echo 0)}"
  tail -3 $W/gvm/qconf.log; grep -m5 -iE " error|Error [0-9]" $W/gvm/qbuild.log
fi
