# Testbed automation scripts

Operational tooling for the multi-host CXL testbed (four single-socket hosts
behind an H3 Falcon / XConn CXL memory server, PXE/NFS-root booted).

## fetch_kernel.sh — pinned kernel source for the A1 module build

```sh
scripts/fetch_kernel.sh [target-dir]     # default: <repo>/linux-pxe/linux
```

Shallow-fetches the exact mainline commit the AE hosts run
(`0138af2472df`, ~300 MB one-time), applies the shipped
`cfg/config-7.0.0-rc5-cxl`, installs the shipped
`cfg/Module.symvers-7.0.0-rc5-cxl` (full modpost symbol checking without
a kernel build), and runs `make modules_prepare` so
`interleave_dax` cross-builds against it. Called automatically by
`./setup.sh` and `interleave_dax/reproduce.sh` when no tree exists at
`KDIR`; an existing tree is never touched. Override the git source with
`KERNEL_REMOTE=` (kernel.org URL in the script header) and the target
with `KDIR=`.

## prepare.sh — per-boot host preparation

The hosts boot an NFS rootfs with a tmpfs overlay: **nothing persists across
reboots** (packages, SSH keys, DAX modes, CPU governor). Run this on every
host after every boot, before any experiment:

```bash
ssh root@<host> 'bash -s' < scripts/prepare.sh
```

It is idempotent and performs:

1. Environment guard — verifies `/etc/hostname` matches the expected PXE image
   (`PXE-SC-AE` by default; override with `EXPECTED_HOSTNAME=`) and resets a
   DHCP-drifted runtime hostname.
2. Package installation (toolchain, numactl + static libnuma, daxctl/ndctl,
   fio, chrony, RDMA userspace, mstflint, Criterion).
3. Services: sshd, chrony (multi-host benchmarks rendezvous on synced clocks).
4. CPU governor → `performance` (powersave costs ~25% single-thread bandwidth).
5. DAX devices → `devdax` mode (benchmarks mmap raw `/dev/daxX.Y`).
6. BlueField-3 port modes: enforces `LINK_TYPE_P1=IB` (uplink to the NDR
   InfiniBand switch) / `LINK_TYPE_P2=ETH` (direct-connect copper). Firmware
   changes are flagged loudly: they need a **power cycle** to take effect.
7. Prints a hardware manifest (CPU/DIMMs/dax/governor/NTP).

## cxl_remap.sh — shared-CXL fabric health & recovery

The switch-provided CXL windows do not survive host cold power cycles; the
fabric must be re-established from the H3 side. This script automates the
reliable recovery ladder.

```bash
scripts/cxl_remap.sh status                       # read-only health report
scripts/cxl_remap.sh remap [--yes] [--max-retries N]   # full recovery cycle
```

`status` reports, per host: H3 EP-port link state (`L0` good), DVSEC health
(status bits `26` + programmed base address), Poolmap.sh content verification,
and on-host devices (sockets, DIMMs, SRAT window, dax devices, local CXL
modules, BlueField/IB state).

`remap` executes: power off all hosts → reboot H3 → verify
`/eMMCRS/Poolmap.sh` maps the expected modules to every EP port (refuses to
run a mismatched map) → run the remap → power hosts on → run `prepare.sh` on
each → verify DVSEC + devices → retry the whole cycle on failure. If cycles
keep failing, it prints the manual escalation (physically replug the H3).

**`remap` powers off all four hosts.** It asks for confirmation unless
`--yes` is given.

Topology and credentials are configured at the top of the script and via
environment (`H3_ADDR`, `H3_USER`, `H3_PASS`, `HOST_PASS`, `CXL_BASE`). The
defaults describe the paper's AE testbed: modules at downstream ports
5/21/4/20 mapped linearly from `0x4080000000` (128 GiB each, 512 GiB windows)
to EP ports 8–11 (g1–g4). Note the pooled window is concatenated, **not**
interleaved — page-granularity interleaving is done host-side by the
`interleave_dax` kernel module.

Requirements on the controller machine: `ssh`, `sshpass`, and the `ipmi`
power-control tool for the host BMCs.
