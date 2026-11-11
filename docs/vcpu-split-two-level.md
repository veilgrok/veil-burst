# Splitting one guest's vCPUs across runners on the two-level path (design note)

Status: memory spanning is in place (uffd shim, memsrv, pipelined fetch). For coherence there is a first
prototype (gha/dsm.c). The VMM piece for splitting vCPUs is NOT built yet.

## Why two-level
GiantVM needs its own L1 host kernel. On GHA we cannot replace the runner kernel, so GiantVM ends up three
levels deep: runner -> L2 4.18-gvm -> L3 guest.
- Measured cost: 11-22x on Intel (prof7). Most of it is nested MMIO exits from the HPET clocksource plus
  timer injections.
- Stock QEMU 10 + KVM on the runner is 1.01-1.04x of bare metal.
So DSM has to live in userspace on L1. Stock KVM backs the guest RAM with a shmem memfd, and userfaultfd
(MISSING plus WP) gives us page-granular control.

## Pieces
1. **Memory** (done): the uffd shim plus memsrv. Remote-backed ranges, pipelined readahead, zero-page
   elision, and WP-safe eviction.
2. **Coherence** (prototype, gha/dsm.c): MSI per 4 KiB page with a home-node directory.
   - Reads install the page with UFFDIO_COPY_MODE_WP (shared).
   - A write triggers a WP fault and an upgrade request. The home invalidates the other copy: the holder
     write-protects the page, writes it back, then punches the hole.
   - Recall (M->S) write-protects the page and sends the data back.
   - Faults are handled on a fault thread; a receiver thread installs pages and acks. That keeps
     recalls deadlock-free while the faulting side is blocked.
3. **vCPU split** (new VMM piece, not built): one QEMU per node, each opening the same guest-physical
   layout over its own memfd registered with the dsm client. Node k runs vCPUs S_k.
   - Run the other node's vCPUs as "parked" vCPUs that never enter KVM_RUN.
   - Route cross-node IPIs/INIT/SIPI: intercept LAPIC ICR writes (split irqchip, or a userspace hook on
     the KVM_EXIT for x2APIC ICR MSR writes) and forward them to the owning node, which injects with
     KVM_SIGNAL_MSI / KVM_INTERRUPT.
   - Make one node the device owner (serial, virtio, PIT/HPET). The other node forwards PIO/MMIO exits
     for those devices over the link (FragVisor-style delegation).
   - Clock: give both nodes kvm-clock with the same TSC offset, from a master clock sync over the link.
   - vCPU state migration (optional, later): KVM_GET/SET_REGS/SREGS/MSRS/LAPIC, for rebalancing.
4. **Fault tolerance**: the home directory plus a write-back log are the replication point. This is
   where Veil's RAID-like redundancy plugs in: mirror the home to a second memsrv.

## Expected cost model (from measurements)
- Same-region WG RTT is about 1 ms.
- One ownership handoff costs about one RTT plus about 50 us of userspace. dsm ping-pong measures the
  real number cross-runner.
- Workloads with heavy true sharing (spinlocks, shared counters) will be RTT-bound. Throughput-bound
  remote first-touch is about 190 MB/s through WireGuard on one core.
- Parallel jobs with mostly private pages (pbench threads) should scale.
