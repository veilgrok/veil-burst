#!/bin/bash
set -u; W=$GITHUB_WORKSPACE; git clone -q --depth 1 https://github.com/ememos/GiantVM $W/gvm
docker run --rm -v $W/gvm:/g -w /g/QEMU-gvm-vcpupin ubuntu:18.04 bash -c '
  export DEBIAN_FRONTEND=noninteractive; apt-get -qq update >/dev/null; apt-get -qq install -y build-essential python pkg-config libglib2.0-dev libpixman-1-dev zlib1g-dev libfdt-dev >/dev/null 2>&1
  echo "--- exec bits"; find . -name "*.py" -o -name "*.sh" -o -path "./scripts/*" -type f | xargs ls -l 2>/dev/null | awk "{print \$1}" | sort | uniq -c
  ./configure --target-list=x86_64-softmmu --disable-werror > /g/qconf.log 2>&1; echo configure_rc=$?
  echo "--- config.log tail"; tail -45 config.log' 2>&1 | tail -70
