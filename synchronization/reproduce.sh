#!/usr/bin/env bash
# reproduce.sh — one-script reproduction of artifact A4 (synchronization/):
#   Fig 5 (lock acquisition latency vs host count) and Fig 6 (latency vs
#   independent lock count, per algorithm), via the two benchmark sweeps
#   (decentralized locks and the centralized lock server).
#
# Run FROM the controller (dev box). Hosts must be prepared
# (scripts/prepare.sh) with the shared CXL window mapped
# (scripts/cxl_remap.sh status healthy) and the repo built at $REMOTE_ROOT.
#
# Usage:
#   synchronization/reproduce.sh            # RUNS=3 full sweeps + plots
#   RUNS=1 synchronization/reproduce.sh     # single pass
#
# Results append into result/raw/run_all*.csv (repeat runs accumulate rows;
# the plotter medians across them). First invocation preserves the previous
# CSVs in result/raw/paper_baseline/.
set -uo pipefail

RUNS="${RUNS:-3}"
REMOTE_ROOT=/tmp/cxl_shm_profiling
ILDAX_BASE=0x4080000000
ILDAX_CFG_4="0-128:1,128-256:1,256-384:1,384-512:1"
HOSTS_SSH="root@g1.dc,root@g2.dc,root@g3.dc,root@g4.dc"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RAW="$REPO_ROOT/synchronization/result/raw"
SCRIPTS="$REPO_ROOT/synchronization/scripts"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "FATAL: $*"; exit 1; }
gssh() { local h=$1; shift; ssh -4 -n -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "root@$h.dc" "$@"; }


log "preflight"
for h in g1 g2 g3 g4; do
  gssh "$h" "[ -x $REMOTE_ROOT/synchronization/lock_bench ] && [ -e /dev/dax0.0 ]" \
    || die "$h not ready (lock_bench built? CXL window mapped?)"
done
log "loading 4-module interleave profile on all hosts"
for h in g1 g2 g3 g4; do
  gssh "$h" "pkill -x rtt 2>/dev/null; pkill -x verify_shm 2>/dev/null; pkill -x lock_bench 2>/dev/null; pkill -x ht_bench 2>/dev/null; pkill -x bw_mem 2>/dev/null; pkill -f '[g]apbs_mh/' 2>/dev/null; sleep 0.5; rmmod interleave_dax 2>/dev/null; insmod $REMOTE_ROOT/interleave_dax/interleave_dax.ko base_phys=$ILDAX_BASE config=$ILDAX_CFG_4 && [ -e /dev/interleave_dax ]" \
    || die "$h: interleave_dax load failed"
done

bak="$RAW/paper_baseline"
if [[ ! -d "$bak" ]]; then
  mkdir -p "$bak"
  for f in "$RAW"/run_all.csv "$RAW"/run_all_central.csv; do
    [[ -f "$f" ]] && mv "$f" "$bak/"
  done
  log "previous raw CSVs preserved in $bak/"
fi

for run in $(seq 1 "$RUNS"); do
  log "=== sweep pass $run/$RUNS (two plans) ==="
  # Wipe shared metadata: stale barrier/segment state from killed runs
  # deadlocks the next rendezvous
  gssh g1 "timeout 120 $REMOTE_ROOT/benchmark/bw_mem/bw_mem --dax_dev /dev/interleave_dax --dax_offset 0 --size 256M --parallel 1 --repetition 1 --test bzero >/dev/null 2>&1" || log "WARN: metadata wipe failed"
  timeout 5400 env HOSTS="$HOSTS_SSH" BIN_ROOT="$REMOTE_ROOT/synchronization" "$SCRIPTS/run.sh" \
    || die "sweep pass $run failed"
done

log "=== regenerating Fig 5 / Fig 6 (validates error_count==0) ==="
(cd "$SCRIPTS" && python3 plot_lock_figs.py) || die "plotting/validation failed"
log "done — plots in synchronization/result/plots/"
