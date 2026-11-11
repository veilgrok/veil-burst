#!/bin/bash
# Build GiantVM QEMU-gvm (2.8.1.1) in ubuntu:18.04. Root cause of the configure failure: GiantVM adds -lnuma to LIBS, so every
# later link test (first one: glib size_t sanity check) fails without libnuma-dev.
set -u; W=$GITHUB_WORKSPACE; git clone -q --depth 1 https://github.com/ememos/GiantVM $W/gvm; t=$(date +%s)
docker run --rm -v $W/gvm:/g -w /g/QEMU-gvm-vcpupin ubuntu:18.04 bash -c '
  export DEBIAN_FRONTEND=noninteractive; apt-get -qq update >/dev/null; apt-get -qq install -y build-essential python pkg-config libglib2.0-dev libpixman-1-dev zlib1g-dev libfdt-dev libnuma-dev >/dev/null 2>&1
  ./configure --target-list=x86_64-softmmu --disable-werror --enable-kvm > /g/qconf.log 2>&1; echo configure_rc=$?; grep -E "KVM support|NUMA" /g/qconf.log
  make -s -j$(nproc) > /g/qbuild.log 2>&1; echo make_rc=$?; grep -iE "undefined reference|cannot find|multiple definition" /g/qbuild.log | sed "s/.*: //" | sort | uniq -c | sort -rn | head -15
  ./x86_64-softmmu/qemu-system-x86_64 --version | head -1; ./x86_64-softmmu/qemu-system-x86_64 -help | grep -A1 local-cpu | head -3'
B=$W/gvm/QEMU-gvm-vcpupin/x86_64-softmmu/qemu-system-x86_64
echo "VEIL_JSON {\"kind\":\"qbuild\",\"ok\":$([ -x $B ] && echo 1 || echo 0),\"s\":$(( $(date +%s)-t ))}"
mkdir -p out; [ -x $B ] && cp $B out/ && cp -r $W/gvm/QEMU-gvm-vcpupin/pc-bios out/ && sudo docker run --rm --device /dev/kvm -v $W/out:/o ubuntu:18.04 bash -c 'apt-get -qq update >/dev/null; apt-get -qq install -y libglib2.0-0 libpixman-1-0 libfdt1 libnuma1 >/dev/null 2>&1; /o/qemu-system-x86_64 -L /o/pc-bios -accel kvm -m 128 -nographic -display none -S -monitor none -serial none & sleep 3; kill $! && echo QEMU_GVM_KVM_START_OK'
