#!/bin/bash
set -u; W=$GITHUB_WORKSPACE; git clone -q --depth 1 https://github.com/ememos/GiantVM $W/gvm
docker run --rm -v $W/gvm:/g -w /g/Linux-DSM-4.18 ubuntu:18.04 bash -c '
  export DEBIAN_FRONTEND=noninteractive; apt-get -qq update >/dev/null; apt-get -qq install -y build-essential flex bison libelf-dev libssl-dev bc kmod >/dev/null
  make -s defconfig; ./scripts/config -e KVM -e KVM_AMD -e KVM_INTEL -e VIRTIO_PCI -e VIRTIO_NET -e VIRTIO_BLK -e BLK_DEV_INITRD -e DEVTMPFS -e DEVTMPFS_MOUNT -e KVM_DSM -e KTCP_NETWORK -d KRDMA_NETWORK -e INFINIBAND -e INFINIBAND_ADDR_TRANS -e TUN -e BRIDGE; make -s olddefconfig
  grep -E "^CONFIG_KVM(_AMD|_INTEL|_DSM)?=|KTCP" .config; make -s -j$(nproc) bzImage > /g/kbuild.log 2>&1; echo kbuild_rc=$?'
mkdir -p out; cp $W/gvm/Linux-DSM-4.18/arch/x86/boot/bzImage out/ && echo "VEIL_JSON {\"kind\":\"kbuild\",\"ok\":1}" || grep -iE "undefined|error" $W/gvm/kbuild.log | head
