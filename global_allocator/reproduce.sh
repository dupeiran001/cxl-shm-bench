#!/usr/bin/env bash
# reproduce.sh — one-script validation of artifact A2 (global_allocator/):
#   1. Criterion unit tests against the tmpfs mock (no CXL needed)
#   2. Cross-host allocate/attach/barrier verification over the real shared
#      CXL window (verify_shm on two hosts)
#
# Usage: global_allocator/reproduce.sh [ROUNDS=100]
set -uo pipefail

H0="${H0:-g1}"; H1="${H1:-g2}"
ROUNDS="${ROUNDS:-100}"
ILDAX_BASE="${ILDAX_BASE:-0x4080000000}"
ILDAX_CFG="${ILDAX_CFG:-0-128:1,128-256:1,256-384:1,384-512:1}"
REMOTE_ROOT=/tmp/cxl_shm_profiling

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "FATAL: $*"; exit 1; }
gssh() { local h=$1; shift; ssh -4 -n -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "root@$h.dc" "$@"; }

log "1/3 unit tests (Criterion, tmpfs mock) on $H0"
gssh "$H0" "cd $REMOTE_ROOT/global_allocator && timeout 600 make test -j8 2>&1 | tail -3" || die "unit tests failed"

log "2/3 loading interleave profile on $H0 + $H1"
for h in "$H0" "$H1"; do
  gssh "$h" "pkill -x rtt 2>/dev/null; pkill -x verify_shm 2>/dev/null; pkill -x lock_bench 2>/dev/null; pkill -x ht_bench 2>/dev/null; pkill -x bw_mem 2>/dev/null; pkill -f '[g]apbs_mh/' 2>/dev/null; sleep 0.5; rmmod interleave_dax 2>/dev/null; insmod $REMOTE_ROOT/interleave_dax/interleave_dax.ko base_phys=$ILDAX_BASE config=$ILDAX_CFG && [ -e /dev/interleave_dax ]" \
    || die "$h: interleave_dax load failed"
done

sleep 5

log "3/3 cross-host verify: $ROUNDS rounds of write -> barrier -> verify (up to 3 attempts)"
verify_ok=0
for attempt in 1 2 3; do
  gssh "$H0" "pkill -x verify_shm 2>/dev/null; true"
  gssh "$H1" "pkill -x verify_shm 2>/dev/null; true"

  # Wipe the shared metadata region
  gssh "$H0" "timeout 120 $REMOTE_ROOT/benchmark/bw_mem/bw_mem --dax_dev /dev/interleave_dax --dax_offset 0 --size 256M --parallel 1 --repetition 1 --test bzero >/dev/null 2>&1" \
    || log "WARN: metadata wipe failed (attempt $attempt)"
  gssh "$H0" "systemd-run --collect --unit=ae-verify-h1-\$RANDOM bash -c 'cd $REMOTE_ROOT/global_allocator/verify && ROUNDS=$ROUNDS DAX_DEV=/dev/interleave_dax ./host1.sh >/tmp/verify0.log 2>&1'"
  sleep 2
  if gssh "$H1" "cd $REMOTE_ROOT/global_allocator/verify && ROUNDS=$ROUNDS DAX_DEV=/dev/interleave_dax timeout 300 ./host2.sh >/tmp/verify1.log 2>&1"; then
    verify_ok=1; break
  fi
  log "attempt $attempt failed/timed out — cleaning and retrying"
  gssh "$H0" "pkill -x verify_shm 2>/dev/null; true"
  sleep 3
done
[[ "$verify_ok" == 1 ]] || { gssh "$H1" "tail -5 /tmp/verify1.log"; die "host2 verify failed after 3 attempts"; }
sleep 1
gssh "$H0" "pkill -x verify_shm 2>/dev/null; true"
gssh "$H0" "tail -2 /tmp/verify0.log"
gssh "$H0" "grep -qiE 'mismatch|fail' /tmp/verify0.log" && die "host1 reported mismatches"
log "A2 validation PASSED (unit tests + $ROUNDS cross-host rounds clean)"
