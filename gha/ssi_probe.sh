#!/bin/bash
# SSI feasibility probe: nested virt, runner<->runner TCP over chisel/websocket relay, GiantVM build attempt. No secrets printed.
set -u; J() { echo "VEIL_JSON $1"; }
G=$GIDX; cd $GITHUB_WORKSPACE
J "{\"kind\":\"host\",\"gidx\":$G,\"cpu\":\"$(lscpu | sed -n 's/Model name: *//p' | head -1)\",\"kernel\":\"$(uname -r)\",\"nproc\":$(nproc),\"mem_mb\":$(free -m | awk '/Mem/{print $2}')}"
for m in kvm_intel kvm_amd; do [ -f /sys/module/$m/parameters/nested ] && J "{\"kind\":\"nested\",\"gidx\":$G,\"module\":\"$m\",\"nested\":\"$(cat /sys/module/$m/parameters/nested)\"}"; done
J "{\"kind\":\"cpuflags\",\"gidx\":$G,\"vmx\":$(grep -c -w vmx /proc/cpuinfo),\"svm\":$(grep -c -w svm /proc/cpuinfo),\"hypervisor\":$(grep -c -w hypervisor /proc/cpuinfo)}"
sudo apt-get -qq update >/dev/null 2>&1; sudo apt-get -qq install -y iperf3 >/dev/null 2>&1
curl -sSL https://github.com/jpillora/chisel/releases/download/v1.12.0/chisel_1.12.0_linux_amd64.gz | gunzip > chisel; chmod +x chisel
cat > echo.py <<'PY'
import socket,sys,time,json
if sys.argv[1]=="srv":
    s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(("127.0.0.1",int(sys.argv[2]))); s.listen(4)
    while True:
        c,_=s.accept(); c.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        while (d:=c.recv(65536)): c.sendall(d)
else:
    c=socket.create_connection(("127.0.0.1",int(sys.argv[2]))); c.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1); r=[]
    for size in (64,4096):
        r=[]
        for _ in range(100):
            t=time.perf_counter(); c.sendall(b"x"*size); n=0
            while n<size: n+=len(c.recv(65536))
            r.append((time.perf_counter()-t)*1000)
        r.sort(); print("VEIL_JSON "+json.dumps({"kind":"tcp_rtt","size":size,"p50_ms":r[50],"p95_ms":r[95],"min_ms":r[0]}))
PY
A="$CHISEL_AUTH"
if [ "$G" = "0" ]; then
  python3 echo.py srv 9301 & iperf3 -s -p 9302 -D
  ./chisel client --auth "$A" "$CHISEL_URL" R:9301:127.0.0.1:9301 R:9302:127.0.0.1:9302 > ch.log 2>&1 &
  sleep 150
else
  sleep 20; ./chisel client --auth "$A" "$CHISEL_URL" 127.0.0.1:9301:127.0.0.1:9301 127.0.0.1:9302:127.0.0.1:9302 > ch.log 2>&1 & sleep 6
  grep -o "Connected.*" ch.log | head -1
  timeout 60 python3 echo.py cli 9301
  timeout 40 iperf3 -c 127.0.0.1 -p 9302 -t 10 -J > up.json 2>/dev/null; python3 -c "import json;d=json.load(open('up.json'));print('VEIL_JSON '+json.dumps({'kind':'iperf','dir':'1->0','sent_Mbps':d['end']['sum_sent']['bits_per_second']/1e6,'recv_Mbps':d['end']['sum_received']['bits_per_second']/1e6}))" || echo "iperf fail"
  timeout 40 iperf3 -c 127.0.0.1 -p 9302 -t 10 -R -J > dn.json 2>/dev/null; python3 -c "import json;d=json.load(open('dn.json'));print('VEIL_JSON '+json.dumps({'kind':'iperf','dir':'0->1','recv_Mbps':d['end']['sum_received']['bits_per_second']/1e6}))" || echo "iperf -R fail"
fi
pkill chisel
# GiantVM build attempt (furthest milestone)
if [ "$G" = "0" ]; then
  t=$(date +%s); git clone -q --depth 1 https://github.com/ememos/GiantVM gvm 2>&1 | tail -2; ls gvm | head -20
  J "{\"kind\":\"gvm_clone\",\"ok\":$([ -d gvm ] && echo 1 || echo 0),\"s\":$(( $(date +%s)-t ))}"
  sudo apt-get -qq install -y flex bison libelf-dev libssl-dev bc >/dev/null 2>&1
  K=$(ls -d gvm/Linux-DSM* 2>/dev/null | head -1); echo "kernel dir: $K"
  if [ -n "$K" ]; then cd $K; t=$(date +%s); make -s defconfig >/dev/null 2>&1; scripts/config -e KVM -e KVM_INTEL -e KVM_AMD 2>/dev/null; make -s olddefconfig >/dev/null 2>&1
    timeout 1200 make -s -j$(nproc) bzImage modules > ../../kbuild.log 2>&1; rc=$?; cd $GITHUB_WORKSPACE
    J "{\"kind\":\"gvm_kernel_build\",\"rc\":$rc,\"s\":$(( $(date +%s)-t )),\"bzImage\":$([ -f $K/arch/x86/boot/bzImage ] && echo 1 || echo 0)}"
    grep -m5 -i "error" kbuild.log; fi
else
  sudo apt-get -qq install -y libglib2.0-dev libpixman-1-dev zlib1g-dev ninja-build >/dev/null 2>&1
  t=$(date +%s); git clone -q --depth 1 https://github.com/ememos/GiantVM gvm 2>&1 | tail -2
  Q=$(ls -d gvm/QEMU* 2>/dev/null | head -1); echo "qemu dir: $Q"
  if [ -n "$Q" ]; then cd $Q; ./configure --target-list=x86_64-softmmu --disable-werror > ../../qconf.log 2>&1; c=$?
    timeout 1200 make -s -j$(nproc) > ../../qbuild.log 2>&1; rc=$?; cd $GITHUB_WORKSPACE
    J "{\"kind\":\"gvm_qemu_build\",\"configure_rc\":$c,\"rc\":$rc,\"s\":$(( $(date +%s)-t )),\"bin\":$(ls $Q/x86_64-softmmu/qemu-system-x86_64 >/dev/null 2>&1 && echo 1 || echo 0)}"
    tail -3 qconf.log; grep -m5 -i "error" qbuild.log; fi
fi
