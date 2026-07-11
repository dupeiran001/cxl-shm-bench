#!/usr/bin/env bash
# prepare.sh — bring a freshly PXE-booted AE host to an experiment-ready state.
#
# The AE hosts boot an NFS rootfs with a tmpfs overlay: NOTHING persists across
# reboots (packages, keys, /tmp, dax modes). Run this on every boot, before any
# experiment:
#
#   ssh root@<host> 'bash -s' < prepare.sh
#
# Idempotent: safe to re-run at any time.
set -euo pipefail

EXPECTED_HOSTNAME="${EXPECTED_HOSTNAME:-PXE-SC-AE}"
WORK_DIR="${WORK_DIR:-/tmp/cxl_shm_profiling}"   # /tmp is the big tmpfs; root overlay is only 4G

log()  { printf '[prepare] %s\n' "$*"; }
fail() { printf '[prepare][FATAL] %s\n' "$*" >&2; exit 1; }

# 1. Environment guard — wrong hostname means wrong PXE kernel/rootfs.
#    Check the static /etc/hostname from the rootfs image: the runtime hostname
#    can be overwritten by DHCP renewals (observed: g1 drifted to 'via').
STATIC_HN="$(cat /etc/hostname 2>/dev/null || hostname)"
[[ "$STATIC_HN" == "$EXPECTED_HOSTNAME" ]] \
  || fail "/etc/hostname '$STATIC_HN' != '$EXPECTED_HOSTNAME' — wrong PXE image? (linux-pxe: make pxe, then: ipmi <host> restart)"
if [[ "$(hostname)" != "$STATIC_HN" ]]; then
  log "runtime hostname '$(hostname)' drifted (DHCP) — resetting to $STATIC_HN"
  hostname "$STATIC_HN" || true
fi
log "host=$(hostname) kernel=$(uname -r)"

# 2. Packages (idempotent). libnuma-dev provides the static libnuma.a that
#    benchmark/Makefile requires; fio is used by interleave_dax/validate.sh.
export DEBIAN_FRONTEND=noninteractive
PKGS=(build-essential cmake git rsync curl ca-certificates python3 numactl libnuma-dev
      daxctl ndctl fio chrony openssh-server pciutils ethtool libcriterion-dev
      rdma-core ibverbs-utils perftest mstflint
      libssl-dev libglib2.0-dev libxml2-dev m4 gettext pkg-config yasm
      libx11-dev libxext-dev libxt-dev libxmu-dev libxi-dev libtiff-dev libjpeg-dev
      file gawk bison flex patch diffutils autoconf automake texinfo zlib1g-dev
      lld msr-tools)   # lld: PARSEC gcc-hooks bldconf links with -fuse-ld=lld;
                       # file/gawk/bison/flex/...: PARSEC libtool+pkg configure/build deps
log "apt-get install: ${PKGS[*]}"
apt-get update -qq
apt-get install -y -qq --no-install-recommends "${PKGS[@]}" >/dev/null

# 3. Services: sshd stays enabled; chrony for cross-host time sync (the
#    multi-host bandwidth runner rendezvouses on NTP-synced epochs).
systemctl enable --now ssh    >/dev/null 2>&1 || true
systemctl enable --now chrony >/dev/null 2>&1 || true
chronyc makestep              >/dev/null 2>&1 || true

# 3b. CPU frequency governor -> performance. The image defaults to powersave,
#     which throttles low-thread-count bandwidth (measured: 1-thread DRAM read
#     17.6 GB/s on powersave vs 22.7 GB/s on performance; paper reports 21).
for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance > "$c" 2>/dev/null || true
done
log "cpufreq governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"

# 3c. Uncore (mesh / LLC / memory-controller) frequency -> pinned to max. At
#     low thread counts the uncore drops toward its minimum, which inflates
#     memory-access LATENCY (not streaming bandwidth). Latency microbenchmarks
#     and cross-host synchronization are single-/few-threaded, so a floating
#     uncore makes them read high; pin it at the initial max.
UNC=/sys/devices/system/cpu/intel_uncore_frequency
if [ -d "$UNC" ]; then
  for d in "$UNC"/package_*_die_*; do
    [ -d "$d" ] || continue
    imax=$(cat "$d/initial_max_freq_khz" 2>/dev/null)
    [ -n "$imax" ] && { echo "$imax" > "$d/max_freq_khz" 2>/dev/null; \
                        echo "$imax" > "$d/min_freq_khz" 2>/dev/null; }
  done
  log "uncore pinned: cur=$(cat $UNC/package_00_die_00/current_freq_khz 2>/dev/null) min=$(cat $UNC/package_00_die_00/min_freq_khz 2>/dev/null) kHz"
else
  log "WARN: intel_uncore_frequency sysfs absent -- uncore not pinned"
fi
# Prefetcher state (MSR 0x1A4, bits set = prefetcher DISABLED). Recorded for
# reproducibility; left at firmware default unless PREFETCH_MSR is exported.
modprobe msr 2>/dev/null || true
if command -v rdmsr >/dev/null 2>&1; then
  [ -n "${PREFETCH_MSR:-}" ] && wrmsr -a "0x1a4" "$PREFETCH_MSR" 2>/dev/null || true
  log "prefetcher MSR 0x1a4 = $(rdmsr -p0 0x1a4 2>/dev/null || echo n/a)"
fi

# 4. DAX devices -> devdax mode (benchmarks mmap the raw /dev/daxX.Y).
#    Mode == which driver the device is bound to: device_dax (devdax) vs kmem
#    (system-ram). Fresh boots may leave regions in kmem with nothing onlined.
if compgen -G '/sys/bus/dax/devices/dax*' >/dev/null; then
  for d in /sys/bus/dax/devices/dax*; do
    dev=$(basename "$d")
    drv=$(basename "$(readlink -f "$d/driver" 2>/dev/null || echo unbound)")
    if [[ "$drv" != "device_dax" ]]; then
      log "reconfigure $dev: driver '$drv' -> devdax"
      daxctl reconfigure-device -f -m devdax "$dev" >/dev/null \
        || log "WARN: could not reconfigure $dev (driver=$drv)"
    fi
  done
else
  log "WARN: no DAX devices visible on this host"
fi

# 4b. BlueField NIC port modes. The first port (P1) uplinks to the InfiniBand
#     NDR switch and must be IB; the second port (P2) is the direct-connect
#     100G Ethernet copper and must stay ETH. LINK_TYPE lives in NIC firmware
#     (persists across boots) — a change only takes effect after a POWER CYCLE.
BF_PCI_IDS=("15b3:a2dc")   # BlueField-3 integrated ConnectX-7
NEED_POWER_CYCLE=0
for id in "${BF_PCI_IDS[@]}"; do
  for bdf in $(lspci -Dn -d "$id" 2>/dev/null | awk '$1 ~ /\.0$/ {print $1}'); do
    q=$(mstconfig -d "$bdf" q LINK_TYPE_P1 LINK_TYPE_P2 2>/dev/null) || true
    cur_p1=$(awk '/LINK_TYPE_P1/{print $2}' <<<"$q")
    cur_p2=$(awk '/LINK_TYPE_P2/{print $2}' <<<"$q")
    if [[ -z "$cur_p1" ]]; then
      log "WARN: BF3 $bdf: mstconfig query failed — skipping port-mode check"
      continue
    fi
    if [[ "$cur_p1" != "IB(1)" || "$cur_p2" != "ETH(2)" ]]; then
      log "BF3 $bdf: LINK_TYPE P1=$cur_p1 P2=$cur_p2 -> setting P1=IB(1) P2=ETH(2)"
      if mstconfig -y -d "$bdf" set LINK_TYPE_P1=1 LINK_TYPE_P2=2 >/dev/null 2>&1; then
        NEED_POWER_CYCLE=1
      else
        log "WARN: BF3 $bdf: mstconfig set failed"
      fi
    else
      log "BF3 $bdf: LINK_TYPE OK (P1=IB, P2=ETH)"
    fi
  done
done
if [[ "$NEED_POWER_CYCLE" == 1 ]]; then
  log "*** BF3 LINK_TYPE changed — POWER CYCLE REQUIRED for it to take effect (ipmi <host> restart) ***"
fi

# 5. Workspace on the big tmpfs (root overlay is too small for builds).
mkdir -p "$WORK_DIR"

# 6. Manifest — one glance tells you the host is what the paper assumes.
log "=== manifest ==="
log "cpu: $(lscpu | awk -F: '/Model name/{gsub(/^ +/,"",$2); print $2; exit}') | sockets=$(lscpu | awk -F: '/^Socket/{gsub(/ /,"",$2); print $2}')"
log "dimms populated: $(dmidecode -t 17 | grep -cE '^\s+Size: [0-9]+ GB')"
for d in /sys/bus/dax/devices/dax*; do
  [[ -e "$d" ]] || continue
  dev=$(basename "$d")
  drv=$(basename "$(readlink -f "$d/driver" 2>/dev/null || echo unbound)")
  log "dax: $dev size=$(numfmt --to=iec <"$d/size") node=$(cat "$d/target_node") driver=$drv chardev=$([[ -e /dev/$dev ]] && echo yes || echo no)"
done
log "ntp: $(chronyc tracking 2>/dev/null | awk -F': *' '/Reference ID/{print $2; ok=1} END{if(!ok) print "not syncing"}' | head -1)"
log "workdir: $WORK_DIR"
log "OK"
