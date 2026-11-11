#!/bin/bash
# Build DSM kernel + QEMU-gvm (TCP router) + an ubuntu:18.04 L2 userland tarball holding QEMU-gvm and its libs.
set -u; W=$GITHUB_WORKSPACE; git clone -q --depth 1 https://github.com/ememos/GiantVM $W/gvm
git -C $W/gvm apply $PWD/gha/ktcp.patch && echo "ktcp.patch applied" || { echo "ktcp.patch FAILED"; exit 1; }
sed -i 's|^#define ROUTER_CONNECTION_RDMA|//#define ROUTER_CONNECTION_RDMA|; s|^//#define ROUTER_CONNECTION_TCP|#define ROUTER_CONNECTION_TCP|' $W/gvm/QEMU-gvm-vcpupin/interrupt-router.c
# ABI fix: QEMU-gvm's bundled header says KVM_CAP_X86_DSM=133, Linux-DSM-4.18 advertises 156 -> QEMU reported dsm_extension: 0
sed -i 's/#define KVM_CAP_X86_DSM 133/#define KVM_CAP_X86_DSM 156/' $W/gvm/QEMU-gvm-vcpupin/linux-headers/linux/kvm.h; grep -n KVM_CAP_X86_DSM $W/gvm/QEMU-gvm-vcpupin/linux-headers/linux/kvm.h
cp gha/pbench.c $W/gvm/
# DSM verbose debug as a runtime knob: /sys/module/kvm/parameters/dsm_verbose
grep -q dsm_verbose $W/gvm/Linux-DSM-4.18/arch/x86/kvm/dsm.c || sed -i 's|^bool kvm_dsm_dbg_verbose = 0;|bool kvm_dsm_dbg_verbose = 0;\nmodule_param_named(dsm_verbose, kvm_dsm_dbg_verbose, bool, 0644);|' $W/gvm/Linux-DSM-4.18/arch/x86/kvm/dsm.c; grep -n dsm_verbose $W/gvm/Linux-DSM-4.18/arch/x86/kvm/dsm.c
docker run --rm -v $W/gvm:/g -w /g/QEMU-gvm-vcpupin ubuntu:18.04 bash -c '
  export DEBIAN_FRONTEND=noninteractive; apt-get -qq update >/dev/null; apt-get -qq install -y build-essential python pkg-config libglib2.0-dev libpixman-1-dev zlib1g-dev libfdt-dev libnuma-dev >/dev/null 2>&1
  ./configure --target-list=x86_64-softmmu --disable-werror --enable-kvm > /g/qconf.log 2>&1 && make -s -j$(nproc) > /g/qbuild.log 2>&1; echo qemu_rc=$?; gcc -O2 -static -pthread -o /g/pbench /g/pbench.c && echo pbench_ok' &
# DSM debug: compile the dsm_debug()/dsm_debug_v() printks in (KERN_DEBUG, read via dmesg)
[ "${DSM_DEBUG:-1}" = 1 ] && sed -i 's|^ccflags-y += -Iarch/x86/kvm|ccflags-y += -Iarch/x86/kvm -DKVM_DSM_DEBUG|' $W/gvm/Linux-DSM-4.18/arch/x86/kvm/Makefile && grep -n KVM_DSM_DEBUG $W/gvm/Linux-DSM-4.18/arch/x86/kvm/Makefile
docker run --rm -v $W/gvm:/g -w /g/Linux-DSM-4.18 ubuntu:18.04 bash -c '
  export DEBIAN_FRONTEND=noninteractive; apt-get -qq update >/dev/null; apt-get -qq install -y build-essential flex bison libelf-dev libssl-dev bc kmod >/dev/null
  make -s defconfig; ./scripts/config -e KVM -e KVM_AMD -e KVM_INTEL -e VIRTIO_PCI -e VIRTIO_NET -e VIRTIO_BLK -e BLK_DEV_INITRD -e DEVTMPFS -e DEVTMPFS_MOUNT -e KVM_DSM -e KTCP_NETWORK -d KRDMA_NETWORK -e INFINIBAND -e INFINIBAND_ADDR_TRANS -e TUN -e E1000 -e STACKTRACE -e DYNAMIC_DEBUG -e PRINTK_TIME; ./scripts/config --set-val LOG_BUF_SHIFT 23; make -s olddefconfig
  make -s -j$(nproc) bzImage > /g/kbuild.log 2>&1; echo kernel_rc=$?'
wait
mkdir -p out; cp $W/gvm/Linux-DSM-4.18/arch/x86/boot/bzImage out/; cp $W/gvm/pbench out/ 2>/dev/null
Q=$W/gvm/QEMU-gvm-vcpupin; mkdir -p ctx/opt/gvm; cp $Q/x86_64-softmmu/qemu-system-x86_64 ctx/opt/gvm/; cp $Q/pc-bios/{bios-256k.bin,kvmvapic.bin,linuxboot_dma.bin,linuxboot.bin,vgabios-stdvga.bin,efi-e1000.rom,efi-virtio.rom,multiboot.bin} ctx/opt/gvm/ 2>/dev/null
printf 'FROM ubuntu:18.04\nRUN apt-get -qq update && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y libglib2.0-0 libpixman-1-0 libfdt1 libnuma1 iproute2 busybox-static >/dev/null && rm -rf /var/lib/apt/lists/*\nCOPY opt /opt\n' > ctx/Dockerfile
docker build -q -t l2root ctx && cid=$(docker create l2root) && docker export $cid | gzip -1 > out/l2root.tar.gz
ls -la out; echo "VEIL_JSON {\"kind\":\"gvm_build\",\"bz\":$([ -f out/bzImage ] && echo 1 || echo 0),\"qemu\":$([ -x ctx/opt/gvm/qemu-system-x86_64 ] && echo 1 || echo 0)}"
