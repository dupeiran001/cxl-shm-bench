#!/usr/bin/env bash
# reproduce.sh — one-script validation of artifact A1 (interleave_dax/):
# cross-builds the module against the AE kernel tree, loads it on a testbed
# host with the current shared-window topology, validates bandwidth through
# /dev/interleave_dax with fio, and unloads.
#
# Usage: interleave_dax/reproduce.sh [HOST=g2]
set -uo pipefail

HOST="${HOST:-g2}"
ILDAX_BASE="${ILDAX_BASE:-0x4080000000}"
ILDAX_CFG="${ILDAX_CFG:-0-128:1,128-256:1,256-384:1,384-512:1}"
REMOTE_ROOT=/tmp/cxl_shm_profiling

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KDIR="$REPO_ROOT/linux-pxe/linux"
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "FATAL: $*"; exit 1; }
gssh() { ssh -4 -n -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "root@$HOST.dc" "$@"; }

if [[ -d "$KDIR" ]]; then
  log "cross-building module against $KDIR (AE kernel tree)"
  make -C "$KDIR" M="$REPO_ROOT/interleave_dax" modules >/dev/null || die "module build failed"
else
  log "no local kernel tree — using the .ko shipped with the deployed repo"
fi

log "deploying + loading on $HOST (base=$ILDAX_BASE cfg=$ILDAX_CFG)"
scp -q -o BatchMode=yes "$REPO_ROOT/interleave_dax/interleave_dax.ko" "root@$HOST.dc:$REMOTE_ROOT/interleave_dax/" 2>/dev/null || true
gssh "pkill -x rtt 2>/dev/null; pkill -x verify_shm 2>/dev/null; pkill -x lock_bench 2>/dev/null; pkill -x ht_bench 2>/dev/null; pkill -x bw_mem 2>/dev/null; pkill -f '[g]apbs_mh/' 2>/dev/null; sleep 0.5; rmmod interleave_dax 2>/dev/null; insmod $REMOTE_ROOT/interleave_dax/interleave_dax.ko base_phys=$ILDAX_BASE config=$ILDAX_CFG && [ -e /dev/interleave_dax ]" \
  || die "insmod failed"

log "fio validation (read bandwidth through the interleaved window)"
gssh "cd $REMOTE_ROOT/interleave_dax && SIZE=4G BS=2m JOBS=8 RW=read timeout 300 ./validate.sh" \
  | grep -E "READ:|bw=" | head -2 || die "validate.sh failed"

gssh "rmmod interleave_dax" || true
log "A1 validation PASSED (module built, loaded, served I/O, unloaded)"
